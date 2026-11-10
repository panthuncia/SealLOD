// Ghost flares, stage 1 (GhostFlareManager; SARP's docs/lens_simulation.md "Ghost flares"): this frame's sources and the
// sensor frame, in one group. The sources are the sun (the first directional light, as the path tracer takes it) and the
// LENS_FLARE_RC_MAX_LIGHTS brightest point and spot lights at the camera, among those within the field the ghost data
// covers (a little wider than the frame's: a source just outside the frame still makes ghosts). Each source's visibility
// is the fraction of LENS_FLARE_VISIBILITY_RAYS fixed points on it (the sun's disc, a light's shadow radius) that the
// camera sees: by ray query against the path tracer's BVH (LENS_FLARE_RAY_QUERY), else by the depth buffer, where a point
// outside the frame counts as seen.
#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#ifdef LENS_FLARE_RAY_QUERY
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/materialFlags.hlsli"
#include "include/clodStructs.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/visibleClusterPacking.hlsli"
#include "include/visibilityPacking.hlsli"
#include "PerPassRootConstants/clodRayTracingSetupRootConstants.h"
#include "include/clodRayTracingHits.hlsli"
#endif
#include "include/lens/lensGhost.hlsli"

#define LENS_FLARE_SUN_HALF_ANGLE_DEGREES 0.27f // as pathTracingLights.hlsli's PATH_LIGHT_DEFAULT_SUN_CONE_DEGREES
#define LENS_FLARE_MAX_LOCAL (LENS_FLARE_MAX_SOURCES - 1u)
#define LENS_FLARE_SHADOW_DISTANCE 100000.0f

groupshared float gs_score[LENS_FLARE_SOURCES_GROUP * LENS_FLARE_MAX_LOCAL];
groupshared uint gs_light[LENS_FLARE_SOURCES_GROUP * LENS_FLARE_MAX_LOCAL];
groupshared uint gs_selected[LENS_FLARE_MAX_LOCAL];

struct FlareSource
{
    bool valid;
    bool isPoint;
    float3 lensVector; // travel direction (parallel) or position (point), lens space
    float3 irradiance;
    float field;
    float2 azimuth;
    float3 worldToSource; // unit, from the camera
    float worldDistance; // 0: at infinity
    float sampleRadius; // tangent of the sun's half-angle, or a light's radius in world units
    uint lightIndex;
};

float FlareLuminance(float3 rgb) { return dot(rgb, float3(0.2126f, 0.7152f, 0.0722f)); }

// Lens space is view space in millimetres; the field remap (lensCommon.hlsli) is undone: a view-space lateral offset is k
// times the lens's.
bool FlareToLens(Camera camera, float fieldScale, float maxField, inout FlareSource source)
{
    float3 travel;
    if (source.isPoint) {
        const float3 viewPosition = mul(float4(camera.positionWorldSpace.xyz + source.worldToSource * source.worldDistance, 1.0f), camera.view).xyz;
        float3 lensPosition = viewPosition * 1000.0f;
        lensPosition.xy /= fieldScale;
        source.lensVector = lensPosition;
        travel = normalize(-lensPosition);
    }
    else {
        const float3 viewDirection = mul(float4(source.worldToSource, 0.0f), camera.view).xyz;
        travel = normalize(float3(-viewDirection.xy / fieldScale, -viewDirection.z));
        source.lensVector = travel;
    }
    if (!(travel.z > 0.0f)) return false;
    source.field = acos(saturate(travel.z));
    if (!(source.field <= maxField)) return false;
    const float lateral = length(travel.xy);
    source.azimuth = lateral > 1e-7f ? travel.xy / lateral : float2(1.0f, 0.0f);
    return true;
}

// A fixed Vogel disc: the same points every frame, so a still source's visibility doesn't flicker.
float2 FlareDiscPoint(uint index)
{
    const float r = sqrt((float(index) + 0.5f) / float(LENS_FLARE_VISIBILITY_RAYS));
    const float angle = float(index) * 2.39996323f;
    return r * float2(cos(angle), sin(angle));
}

float FlareVisibility(Camera camera, FlareSource source, uint2 depthSize)
{
    float3 tangent = normalize(abs(source.worldToSource.y) < 0.99f ? cross(source.worldToSource, float3(0.0f, 1.0f, 0.0f))
        : cross(source.worldToSource, float3(1.0f, 0.0f, 0.0f)));
    const float3 bitangent = cross(source.worldToSource, tangent);
    float seen = 0.0f;
    [loop]
    for (uint index = 0u; index < LENS_FLARE_VISIBILITY_RAYS; ++index) {
        const float2 disc = FlareDiscPoint(index) * source.sampleRadius;
        float3 direction;
        float distance;
        if (source.worldDistance > 0.0f) {
            const float3 target = source.worldToSource * source.worldDistance + tangent * disc.x + bitangent * disc.y;
            distance = length(target);
            direction = target / max(distance, 1e-6f);
        }
        else {
            direction = normalize(source.worldToSource + tangent * disc.x + bitangent * disc.y);
            distance = 0.0f;
        }
#ifdef LENS_FLARE_RAY_QUERY
        if (CLOD_RT_SEC_TLAS_SRV == 0xFFFFFFFFu) {
            seen += 1.0f; // no BVH yet
            continue;
        }
        RayDesc ray;
        ray.Origin = camera.positionWorldSpace.xyz;
        ray.Direction = direction;
        ray.TMin = camera.zNear;
        ray.TMax = distance > 0.0f ? max(distance * 0.999f, camera.zNear * 2.0f) : LENS_FLARE_SHADOW_DISTANCE;
        RaytracingAccelerationStructure scene = ResourceDescriptorHeap[CLOD_RT_SEC_TLAS_SRV];
        StructuredBuffer<CLodRtClasInfo> clasInfo = ResourceDescriptorHeap[CLOD_RT_SEC_CLAS_INFO_SRV];
        const CLodRtHit hit = CLodRtTrace(scene, clasInfo, ray, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFFu, 1e-3f);
        seen += hit.hit ? 0.0f : 1.0f;
#else
        // Reverse Z: the sky is 0, nearer is larger. A point outside the frame (or behind) counts as seen.
        const float4 clip = distance > 0.0f ? mul(float4(camera.positionWorldSpace.xyz + direction * distance, 1.0f), camera.viewProjection)
                                            : mul(float4(direction, 0.0f), camera.viewProjection);
        if (!(clip.w > 0.0f)) {
            seen += 1.0f;
            continue;
        }
        const float3 ndc = clip.xyz / clip.w;
        const float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
        if (any(uv < 0.0f) || any(uv >= 1.0f) || depthSize.x == 0u) {
            seen += 1.0f;
            continue;
        }
        Texture2D<float> depth = ResourceDescriptorHeap[LENS_FLARE_RC_DEPTH_SRV];
        const float scene = depth[min(uint2(uv * float2(depthSize)), depthSize - 1u)];
        const float pointDepth = distance > 0.0f ? saturate(ndc.z) : 0.0f;
        seen += scene <= pointDepth * 1.0001f + 1e-7f ? 1.0f : 0.0f;
#endif
    }
    return seen / float(LENS_FLARE_VISIBILITY_RAYS);
}

FlareSource FlareLightSource(Camera camera, LightInfo light, uint lightIndex)
{
    FlareSource source = (FlareSource)0;
    source.lightIndex = lightIndex;
    if (light.type == 2u) {
        source.worldToSource = normalize(-light.dirWorldSpace.xyz);
        source.irradiance = light.color.xyz * light.color.w;
        const float halfAngle = light.shadowSourceAngleDegrees > 0.0f ? light.shadowSourceAngleDegrees : LENS_FLARE_SUN_HALF_ANGLE_DEGREES;
        source.sampleRadius = tan(radians(halfAngle));
        source.valid = any(source.irradiance > 0.0f);
        return source;
    }
    const float3 toLight = light.posWorldSpace.xyz - camera.positionWorldSpace.xyz;
    const float distance = length(toLight);
    if (!(distance > camera.zNear) || distance > light.maxRange) return source;
    source.isPoint = true;
    source.worldDistance = distance;
    source.worldToSource = toLight / distance;
    const float attenuation = 1.0f / (light.attenuation.x + light.attenuation.y * distance + light.attenuation.z * distance * distance + 0.0001f);
    float spot = 1.0f;
    if (light.type == 1u) {
        const float cosine = dot(normalize(light.dirWorldSpace.xyz), -source.worldToSource);
        spot = cosine > light.outerConeAngle ? (cosine < light.innerConeAngle ? smoothstep(light.outerConeAngle, light.innerConeAngle, cosine) : 1.0f) : 0.0f;
    }
    source.irradiance = light.color.xyz * light.color.w * attenuation * spot;
    source.sampleRadius = max(light.shadowSourceRadius, 0.0f);
    source.valid = any(source.irradiance > 0.0f);
    return source;
}

void FlareWriteSource(RWStructuredBuffer<float4> output, uint slot, FlareSource source, float visibility, float intensity)
{
    const uint row = LENS_FLARE_ROW_SOURCES + slot * LENS_FLARE_SOURCE_ROWS;
    const bool valid = source.valid && visibility > 0.0f;
    output[row] = float4(source.lensVector, source.isPoint ? LENS_FLARE_SOURCE_POINT : LENS_FLARE_SOURCE_PARALLEL);
    output[row + 1u] = float4(source.irradiance * (visibility * intensity), visibility);
    output[row + 2u] = float4(asfloat(valid ? 1u : 0u), source.azimuth.x, source.azimuth.y, source.field);
}

[numthreads(LENS_FLARE_SOURCES_GROUP, 1, 1)]
void LensFlareSourcesCS(uint3 thread : SV_GroupThreadID)
{
    const uint tid = thread.x;
    RWStructuredBuffer<float4> output = ResourceDescriptorHeap[LENS_FLARE_RC_SOURCES_UAV];
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const Camera camera = cameras[perFrame.mainCameraIndex];
    StructuredBuffer<float4> data = LensData();
    const LensHeader header = LensLoadHeader(data);
    StructuredBuffer<float4> ghosts = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::LensFlare::Ghosts)];
    const LensGhostHeader ghostHeader = LensGhostLoadHeader(ghosts);
    const uint lensRevision = asuint(data[LENS_ROW_HEADER5]).y;
    // Ghost data for another lens revision (the ghosts are still being built for a new lens) traces nothing.
    const bool active = header.valid && ghostHeader.valid && ghostHeader.lensRevision == lensRevision;
    const uint maxLocal = min(LENS_FLARE_RC_MAX_LIGHTS, LENS_FLARE_MAX_LOCAL);
    const float intensity = asfloat(LENS_FLARE_RC_INTENSITY);
    const bool selfTest = LENS_FLARE_RC_SELF_TEST != 0u;
    const bool testSource = LENS_FLARE_RC_TEST_SOURCE != 0u;

    // The frame's focus (LensFocusPass's, per frame): the ghosts land on the focused sensor plane, through the same sensor
    // frame and normalization as the camera's rays.
    LensSensorFrame frame;
    frame.halfExtent = 1.0f.xx;
    frame.fieldScale = 1.0f;
    LensFocus focus = (LensFocus)0;
    if (header.valid) {
        focus = LensLoadFocus(header, false);
        frame = LensSensorHalfExtent(data, header, focus, camera.unjitteredProjection);
    }

    if (tid == 0u) {
        const bool monochromatic = !header.valid || (header.flags & LENS_FLAG_MONOCHROMATIC) != 0u;
        // The CPU passes 1 for a monochromatic lens; the layout of the passes after this one follows its count.
        const uint wavelengths = clamp(LENS_FLARE_RC_WAVELENGTHS, 1u, LENS_FLARE_MAX_WAVELENGTHS);
        output[LENS_FLARE_ROW_FRAME0] = float4(frame.halfExtent, header.valid ? focus.sensorZ : 0.0f,
            header.valid ? LensInverseNormalization(data, header, focus) : 0.0f);
        output[LENS_FLARE_ROW_FRAME1] = asfloat(uint4(active ? 1u + maxLocal : 0u, wavelengths, LENS_FLARE_RC_GRID, selfTest ? LENS_FLARE_FLAG_SELF_TEST : 0u));
        // One flare-target pixel's sensor area: no cell divides its energy by less.
        const float2 pixel = 2.0f * frame.halfExtent / float2(max(LENS_FLARE_RC_TARGET_WIDTH, 1u), max(LENS_FLARE_RC_TARGET_HEIGHT, 1u));
        output[LENS_FLARE_ROW_FRAME2] = float4(frame.fieldScale, intensity, pixel.x * pixel.y, 0.0f);
        output[LENS_FLARE_ROW_FRAME3] = asfloat(uint4(LENS_FLARE_RC_SELF_TEST, lensRevision, ghostHeader.lensRevision, LENS_FLARE_RC_GHOSTS_SLOTS));
        // Wavelengths spread evenly over 380-780 nm, weighted by the lens's CIE fit (negative lobes dropped) and normalized so
        // that together they make white.
        float3 sum = 0.0f.xxx;
        float3 weights[LENS_FLARE_MAX_WAVELENGTHS];
        float lambdas[LENS_FLARE_MAX_WAVELENGTHS];
        [unroll]
        for (uint w = 0u; w < LENS_FLARE_MAX_WAVELENGTHS; ++w) {
            lambdas[w] = monochromatic ? (header.valid ? header.designWavelength : 587.5618f)
                                       : LENS_LAMBDA_MIN + (float(w) + 0.5f) * (LENS_LAMBDA_MAX - LENS_LAMBDA_MIN) / float(wavelengths);
            weights[w] = w < wavelengths ? (monochromatic ? 1.0f.xxx : max(LensRgbWeight(lambdas[w]), 0.0f.xxx)) : 0.0f.xxx;
            sum += weights[w];
        }
        [unroll]
        for (uint w2 = 0u; w2 < LENS_FLARE_MAX_WAVELENGTHS; ++w2) {
            output[LENS_FLARE_ROW_WAVELENGTHS + w2] = float4(weights[w2] / max(sum, 1e-6f.xxx), lambdas[w2]);
        }
    }
    if (!active) {
        if (tid < LENS_FLARE_MAX_SOURCES) output[LENS_FLARE_ROW_SOURCES + tid * LENS_FLARE_SOURCE_ROWS + 2u] = 0.0f.xxxx;
        return;
    }

    StructuredBuffer<unsigned int> activeLights = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::ActiveLightIndices)];
    StructuredBuffer<LightInfo> lights = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::InfoBuffer)];
    const uint lightCount = perFrame.numLights;

    // Each thread's brightest local lights in the field, then thread 0 merges them.
    float best[LENS_FLARE_MAX_LOCAL];
    uint bestLight[LENS_FLARE_MAX_LOCAL];
    [unroll]
    for (uint k = 0u; k < LENS_FLARE_MAX_LOCAL; ++k) {
        best[k] = 0.0f;
        bestLight[k] = 0xFFFFFFFFu;
    }
    if (!selfTest && !testSource) {
        for (uint slot = tid; slot < lightCount; slot += LENS_FLARE_SOURCES_GROUP) {
            const uint index = activeLights[slot];
            const LightInfo light = lights[index];
            if (light.type == 2u) continue;
            FlareSource source = FlareLightSource(camera, light, index);
            if (!source.valid || !FlareToLens(camera, frame.fieldScale, ghostHeader.maxField, source)) continue;
            float score = FlareLuminance(source.irradiance);
            uint candidate = index;
            [unroll]
            for (uint k2 = 0u; k2 < LENS_FLARE_MAX_LOCAL; ++k2) {
                if (k2 < maxLocal && score > best[k2]) {
                    const float s = best[k2];
                    const uint l = bestLight[k2];
                    best[k2] = score;
                    bestLight[k2] = candidate;
                    score = s;
                    candidate = l;
                }
            }
        }
    }
    [unroll]
    for (uint k3 = 0u; k3 < LENS_FLARE_MAX_LOCAL; ++k3) {
        gs_score[tid * LENS_FLARE_MAX_LOCAL + k3] = best[k3];
        gs_light[tid * LENS_FLARE_MAX_LOCAL + k3] = bestLight[k3];
    }
    GroupMemoryBarrierWithGroupSync();
    if (tid == 0u) {
        for (uint pick = 0u; pick < maxLocal; ++pick) {
            float top = 0.0f;
            uint topEntry = 0xFFFFFFFFu;
            for (uint entry = 0u; entry < LENS_FLARE_SOURCES_GROUP * LENS_FLARE_MAX_LOCAL; ++entry) {
                if (gs_light[entry] != 0xFFFFFFFFu && gs_score[entry] > top) {
                    top = gs_score[entry];
                    topEntry = entry;
                }
            }
            gs_selected[pick] = topEntry == 0xFFFFFFFFu ? 0xFFFFFFFFu : gs_light[topEntry];
            if (topEntry != 0xFFFFFFFFu) gs_light[topEntry] = 0xFFFFFFFFu;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // Thread s fills slot s: 0 the sun (or the test source), 1.. the local lights.
    if (tid > maxLocal) return;
    FlareSource source = (FlareSource)0;
    float visibility = 0.0f;
    uint2 depthSize = 0u.xx;
#ifndef LENS_FLARE_RAY_QUERY
    if (LENS_FLARE_RC_DEPTH_SRV != 0xFFFFFFFFu) {
        Texture2D<float> depth = ResourceDescriptorHeap[LENS_FLARE_RC_DEPTH_SRV];
        depth.GetDimensions(depthSize.x, depthSize.y);
    }
#endif
    if (tid == 0u && selfTest) {
        // The self-test's source: parallel, at 0.6 of the field and azimuth 0.5 rad in lens space, unit irradiance, seen.
        const float field = 0.6f * ghostHeader.maxField;
        source.valid = true;
        source.lensVector = float3(sin(field) * cos(0.5f), sin(field) * sin(0.5f), cos(field));
        source.irradiance = 1.0f.xxx;
        source.field = field;
        source.azimuth = float2(cos(0.5f), sin(0.5f));
        FlareWriteSource(output, 0u, source, 1.0f, 1.0f);
        return;
    }
    if (tid == 0u && testSource) {
        // The pipe's test source (lens.flare.set test_source), in view space: it lets a scene's emissive disc stand in for a
        // light, or a light be placed anywhere.
        const float3 view = normalize(float3(asfloat(LENS_FLARE_RC_TEST_X), asfloat(LENS_FLARE_RC_TEST_Y), asfloat(LENS_FLARE_RC_TEST_Z)));
        source.worldToSource = normalize(mul(float4(view, 0.0f), camera.viewInverse).xyz);
        source.worldDistance = asfloat(LENS_FLARE_RC_TEST_DISTANCE);
        source.isPoint = source.worldDistance > 0.0f;
        source.irradiance = float3(asfloat(LENS_FLARE_RC_TEST_R), asfloat(LENS_FLARE_RC_TEST_G), asfloat(LENS_FLARE_RC_TEST_B));
        source.sampleRadius = 0.0f;
        source.valid = any(source.irradiance > 0.0f);
    }
    else if (tid == 0u) {
        for (uint slot = 0u; slot < lightCount; ++slot) {
            const uint index = activeLights[slot];
            if (lights[index].type == 2u) {
                source = FlareLightSource(camera, lights[index], index);
                break;
            }
        }
    }
    else {
        const uint index = gs_selected[tid - 1u];
        if (index != 0xFFFFFFFFu) source = FlareLightSource(camera, lights[index], index);
    }
    if (source.valid) source.valid = FlareToLens(camera, frame.fieldScale, ghostHeader.maxField, source);
    if (source.valid) visibility = FlareVisibility(camera, source, depthSize);
    FlareWriteSource(output, tid, source, visibility, intensity);
}
