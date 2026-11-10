// The physical lens's per-frame focus (SARP's docs/lens_simulation.md, "Focus and autofocus"). The state lives in
// Builtin::RayTracing::LensFocus (lensDataLayout.h, "Per-frame focus"), which the camera rays read; the lens data's focus nodes
// are interpolated for it, so a focus change is not a lens build.
//
// LensFocusUpdateCS (LensFocusPass, before the path tracer): one group of 64. Its first thread steps the focus: manual focus takes the CPU's shift as is. Autofocus
// latches the last measured target when it is more than a dead band from the latched one, and moves toward it as a critically
// damped spring, snapped onto it once within 1% of the dead band, so a settled focus stays bit for bit. It flags a restart for
// accumulation when the focus moved, and computes the frame's sensor frames and 1/K. Then every thread interpolates pupil nodes to
// the new focus (LENS_FOCUS_ROW_PUPIL), so camera rays read two radius nodes, as with one focus.
//   0 focus UAV, 1 mode (LENS_FOCUS_MODE_*), 2 asfloat(manual shift mm), 3 asfloat(dt s), 4 asfloat(spring rate 1/s),
//   5 asfloat(dead band mm), 6 lens identity (a change re-initializes at the manual focus), 7 asfloat(restart epsilon mm).
//
// LensAutofocusCS (LensAutofocusPass, after the path tracer): one group. The primary hits' view depth over the autofocus region
// (32 x 32 samples), sky and blocked pixels left out; the mode of a log-depth histogram (the 3-bin window with most samples)
// and the mean log depth inside it; then its shift through the lens's focus table, for the next frame's update.
//   0 focus UAV, 1 projected depth SRV, 2 asfloat(point NDC x), 3 asfloat(point NDC y), 4 asfloat(region size, fraction of the
//   frame height).
#include "include/cbuffers.hlsli"
#include "include/lens/lensCommon.hlsli"

groupshared float gs_lensFocusShift;

void LensFocusStep(LensHeader header)
{
    RWStructuredBuffer<float4> state = ResourceDescriptorHeap[UintRootConstant0];
    StructuredBuffer<float4> data = LensData();
    const uint mode = UintRootConstant1;
    const float manualShift = clamp(asfloat(UintRootConstant2), header.shiftMin, header.shiftMax);
    const float dt = asfloat(UintRootConstant3);
    const float rate = max(asfloat(UintRootConstant4), 1e-3f);
    const float deadBand = asfloat(UintRootConstant5);
    const uint identity = UintRootConstant6;
    const float epsilon = asfloat(UintRootConstant7);

    uint4 flags = asuint(state[LENS_FOCUS_ROW_STATE]);
    float4 current = state[LENS_FOCUS_ROW_CURRENT];
    float4 spring = state[LENS_FOCUS_ROW_SPRING];
    const bool reset = flags.x != LENS_FOCUS_MAGIC || flags.w != identity;
    if (reset) {
        current = float4(manualShift, 0.0f, 0.0f, manualShift);
        spring = 0.0f.xxxx;
        flags = uint4(LENS_FOCUS_MAGIC, 0u, 0u, identity);
    }
    const float previousShift = clamp(current.x, header.shiftMin, header.shiftMax);
    float shift = previousShift;
    float target = current.w;
    float velocity = spring.x;
    if (mode != LENS_FOCUS_MODE_AUTO) {
        shift = manualShift;
        target = manualShift;
        velocity = 0.0f;
    }
    else {
        // A measurement more than the dead band from the latched target replaces it; within it, the focus holds still.
        if (spring.z > 0.0f && abs(spring.w - target) > deadBand) target = spring.w;
        target = clamp(target, header.shiftMin, header.shiftMax);
        // x(t) = target + (y0 + (v0 + w y0) t) e^(-w t), exact for any dt.
        const float y = shift - target;
        const float a = velocity + rate * y;
        const float e = exp(-rate * dt);
        float yNext = (y + a * dt) * e;
        float vNext = (velocity - rate * a * dt) * e;
        if (abs(yNext) < 0.01f * deadBand && abs(vNext) < 0.01f * deadBand * rate) {
            yNext = 0.0f;
            vNext = 0.0f;
        }
        shift = clamp(target + yNext, header.shiftMin, header.shiftMax);
        velocity = vNext;
    }
    const bool moved = reset || abs(shift - previousShift) > epsilon;
    flags.y = moved ? 1u : 0u;
    flags.z += moved ? 1u : 0u;
    const LensFocus focus = LensFocusFromShift(header, shift);
    const LensFocus previous = LensFocusFromShift(header, reset ? shift : previousShift);
    state[LENS_FOCUS_ROW_PREVIOUS] = float4(previous.shift, previous.sensorZ, float(previous.node) + previous.t, 0.0f);
    state[LENS_FOCUS_ROW_CURRENT] = float4(focus.shift, focus.sensorZ, float(focus.node) + focus.t, target);
    state[LENS_FOCUS_ROW_SPRING] = float4(velocity, 0.0f, spring.z, spring.w);
    state[LENS_FOCUS_ROW_STATE] = asfloat(flags);

    // The frame's sensor frames and 1/K, once for every camera ray (LensLoadFrame).
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const Camera camera = cameras[perFrame.mainCameraIndex];
    const LensSensorFrame frame = LensSensorHalfExtent(data, header, focus, camera.unjitteredProjection);
    const LensSensorFrame previousFrame = LensSensorHalfExtent(data, header, previous, camera.prevUnjitteredProjection);
    state[LENS_FOCUS_ROW_FRAME] = float4(frame.halfExtent, frame.fieldScale, LensInverseNormalization(data, header, focus));
    state[LENS_FOCUS_ROW_PREVIOUS_FRAME] = float4(previousFrame.halfExtent, previousFrame.fieldScale, asfloat(header.revision));
    gs_lensFocusShift = focus.shift;
}

[numthreads(64, 1, 1)]
void LensFocusUpdateCS(uint thread : SV_GroupIndex)
{
    StructuredBuffer<float4> data = LensData();
    const LensHeader header = LensLoadHeader(data);
    if (!header.valid) return;
    if (thread == 0u) LensFocusStep(header);
    GroupMemoryBarrierWithGroupSync();
    // The pupil nodes at the focus: each radius node's two focus nodes, linearly (zero where either has no light, as
    // LensPupilRegionAt's bilinear lookup rejects).
    RWStructuredBuffer<float4> state = ResourceDescriptorHeap[UintRootConstant0];
    const LensFocus focus = LensFocusFromShift(header, gs_lensFocusShift);
    const uint nodes = min(header.pupilNodeCount, LENS_FOCUS_MAX_PUPIL_NODES);
    const uint farNode = LensFocusNextNode(header, focus);
    for (uint node = thread; node < nodes; node += 64u) {
        const uint a = header.pupilNodeOffset + (focus.node * header.pupilNodeCount + node) * LENS_PUPIL_NODE_ROWS;
        const uint b = header.pupilNodeOffset + (farNode * header.pupilNodeCount + node) * LENS_PUPIL_NODE_ROWS;
        const uint target = LENS_FOCUS_ROW_PUPIL + node * LENS_PUPIL_NODE_ROWS;
        const bool lit = data[a].y > 0.0f && data[b].y > 0.0f;
        [unroll] for (uint r = 0u; r < LENS_PUPIL_NODE_ROWS; ++r) {
            state[target + r] = lit ? lerp(data[a + r], data[b + r], focus.t) : 0.0f.xxxx;
        }
    }
}

#define LENS_AF_GRID 32u
#define LENS_AF_THREADS 256u
#define LENS_AF_BINS 64u
#define LENS_AF_SAMPLES_PER_THREAD (LENS_AF_GRID * LENS_AF_GRID / LENS_AF_THREADS)
#define LENS_AF_LOG2_MIN -4.321928f // 0.05 m
#define LENS_AF_LOG2_MAX 13.287712f // 10 km

groupshared uint gs_lensAfCounts[LENS_AF_BINS];
groupshared float gs_lensAfSum[LENS_AF_THREADS];
groupshared uint gs_lensAfUsed[LENS_AF_THREADS];
groupshared uint gs_lensAfWindow;

[numthreads(LENS_AF_THREADS, 1, 1)]
void LensAutofocusCS(uint thread : SV_GroupIndex)
{
    RWStructuredBuffer<float4> state = ResourceDescriptorHeap[UintRootConstant0];
    Texture2D<float> depth = ResourceDescriptorHeap[UintRootConstant1];
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const Camera camera = cameras[perFrame.mainCameraIndex];
    StructuredBuffer<float4> data = LensData();
    const LensHeader header = LensLoadHeader(data);

    const float2 afPoint = float2(asfloat(UintRootConstant2), asfloat(UintRootConstant3));
    const float region = asfloat(UintRootConstant4);
    uint width, height;
    depth.GetDimensions(width, height);
    const float2 centre = float2((afPoint.x * 0.5f + 0.5f) * width, (0.5f - afPoint.y * 0.5f) * height);
    const float halfSize = max(0.5f * region * height, 0.5f);

    if (thread < LENS_AF_BINS) gs_lensAfCounts[thread] = 0u;
    GroupMemoryBarrierWithGroupSync();

    // Each thread's samples: view depth (m) from the reverse-Z projected depth the path tracer wrote, log2.
    // LENS_AF_SAMPLES_PER_THREAD each.
    float logDepth[LENS_AF_SAMPLES_PER_THREAD];
    uint valid = 0u;
    [unroll] for (uint k = 0u; k < LENS_AF_SAMPLES_PER_THREAD; ++k) {
        const uint index = thread * LENS_AF_SAMPLES_PER_THREAD + k;
        const float2 cell = (float2(index % LENS_AF_GRID, index / LENS_AF_GRID) + 0.5f) / float(LENS_AF_GRID) * 2.0f - 1.0f;
        const int2 pixel = clamp(int2(centre + cell * halfSize), int2(0, 0), int2(width - 1u, height - 1u));
        const float projected = depth[pixel];
        const float view = camera.unjitteredProjection[3][2] / (projected + camera.unjitteredProjection[2][2]);
        logDepth[k] = projected > 0.0f && view > 0.0f && isfinite(view) ? log2(view) : -1e30f;
        if (logDepth[k] > -1e29f) {
            const uint bin = uint(clamp((logDepth[k] - LENS_AF_LOG2_MIN) / (LENS_AF_LOG2_MAX - LENS_AF_LOG2_MIN) * LENS_AF_BINS, 0.0f, LENS_AF_BINS - 1.0f));
            InterlockedAdd(gs_lensAfCounts[bin], 1u);
            ++valid;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (thread == 0u) {
        uint best = 0u, bestCount = 0u;
        for (uint b = 0u; b < LENS_AF_BINS; ++b) {
            const uint count = gs_lensAfCounts[b] + (b > 0u ? gs_lensAfCounts[b - 1u] : 0u) + (b + 1u < LENS_AF_BINS ? gs_lensAfCounts[b + 1u] : 0u);
            if (count > bestCount) {
                bestCount = count;
                best = b;
            }
        }
        gs_lensAfWindow = best;
    }
    GroupMemoryBarrierWithGroupSync();
    const uint window = gs_lensAfWindow;
    float sum = 0.0f;
    uint used = 0u;
    [unroll] for (uint m = 0u; m < LENS_AF_SAMPLES_PER_THREAD; ++m) {
        if (logDepth[m] < -1e29f) continue;
        const uint bin = uint(clamp((logDepth[m] - LENS_AF_LOG2_MIN) / (LENS_AF_LOG2_MAX - LENS_AF_LOG2_MIN) * LENS_AF_BINS, 0.0f, LENS_AF_BINS - 1.0f));
        if (bin + 1u >= window && bin <= window + 1u) {
            sum += logDepth[m];
            ++used;
        }
    }
    gs_lensAfSum[thread] = sum;
    gs_lensAfUsed[thread] = used | (valid << 16u);
    GroupMemoryBarrierWithGroupSync();
    if (thread != 0u) return;
    float total = 0.0f;
    uint totalUsed = 0u, totalValid = 0u;
    for (uint t = 0u; t < LENS_AF_THREADS; ++t) {
        total += gs_lensAfSum[t];
        totalUsed += gs_lensAfUsed[t] & 0xFFFFu;
        totalValid += gs_lensAfUsed[t] >> 16u;
    }
    const float samples = float(LENS_AF_GRID * LENS_AF_GRID);
    float4 spring = state[LENS_FOCUS_ROW_SPRING];
    // At least 5% of the region has a surface: otherwise (sky, or every camera sample blocked) there is no measurement.
    if (!header.valid || totalUsed < uint(0.05f * samples) || header.focusTableCount < 2u) {
        spring.z = 0.0f;
        state[LENS_FOCUS_ROW_SPRING] = spring;
        state[LENS_FOCUS_ROW_MEASURE] = float4(totalValid / samples, 0.0f, float(totalUsed), 0.0f);
        return;
    }
    const float distanceMm = exp2(total / float(totalUsed)) * 1000.0f;
    // The focus table is uniform in 1 / distance from infinity to the near limit; nearer than it clamps.
    const float x = clamp(header.nearFocusMm / distanceMm, 0.0f, 1.0f) * float(header.focusTableCount - 1u);
    const uint i = min(uint(x), header.focusTableCount - 2u);
    const float f = x - float(i);
    const float s0 = data[header.focusTableOffset + i / 4u][i % 4u];
    const float s1 = data[header.focusTableOffset + (i + 1u) / 4u][(i + 1u) % 4u];
    spring.z = distanceMm;
    spring.w = lerp(s0, s1, f);
    state[LENS_FOCUS_ROW_SPRING] = spring;
    state[LENS_FOCUS_ROW_MEASURE] = float4(totalValid / samples, 3.0f * (LENS_AF_LOG2_MAX - LENS_AF_LOG2_MIN) / LENS_AF_BINS, float(totalUsed), 0.0f);
}
