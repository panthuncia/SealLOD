// Reference path tracer. One state object: PathTracePass.h lists its shaders and hit groups.
//
// Hit group index = ray contribution (0 radiance, 1 shadow) + instance contribution (0 general, 2 terrain, from
// rayTracingSceneBuild.hlsl), with a multiplier of 1 because records are laid out in that order.
//
// Recursion depth is 1. A closest hit evaluates the material, samples the BSDF and one light, and returns the next ray and an
// unoccluded light contribution. Ray generation traces that shadow ray itself, inline, so shading never holds traversal state.
//
// Alpha tests use one fixed mip per texture, which opacity micromaps are baked against: see clodRayTracingAlpha.hlsli.
#define CLOD_RT_FIXED_ALPHA_MIP 1
#include "include/pathTracingHit.hlsli"
#include "include/clodRayTracingHits.hlsli"
#include "include/pathTracingBsdf.hlsli"
#include "include/pathTracingLights.hlsli"
#include "include/pathTracingWatch.hlsli"
#include "include/lens/lensCamera.hlsli"

// Cost breakdown only (PATH_TRACE_EXPERIMENT, pathTracingHit.hlsli): 3 skips every alpha test, 5 those of hierarchy roots'
// instances (merged geometry, whose templates have a material palette), treating the candidate as opaque.
bool PathTraceSkipAlphaTest(uint instanceID)
{
    if (PATH_TRACE_EXPERIMENT == 3) return true;
    if (PATH_TRACE_EXPERIMENT == 5) return CLodTemplateHasPalette(LoadMeshTemplateForDrawRecord(LoadInstanceDrawRecord(CLodRtInstanceDraw(instanceID))));
    return false;
}

// Payload access qualifiers aren't used, and the library compiles with -disable-payload-qualifiers: the engine's shader
// preprocessor can't parse them, and without a clean parse it can't prune descriptor names, which have a limit of 64.
struct PathTracePayload
{
    float3 emitted; // Surface emission, or sky on a miss. Before throughput.
    uint rngState;
    float3 weight; // BSDF sample weight over its PDF, for what comes after this hit.
    uint rngIncrement;
    float3 nextOrigin;
    float hitT; // Negative when the path ends here.
    uint nextDirection;
    uint shadowDirection;
    float shadowTMax; // Zero when there is no shadow ray to trace.
    float3 unoccluded; // Light contribution if the shadow ray misses. Before throughput.
    float coneWidth;
    float coneSpread; // Grows with each rough bounce, for the footprint of material samples.
    float alphaConeSpread; // Unused while alpha tests take a fixed mip (CLOD_RT_FIXED_ALPHA_MIP), kept for the cone-mip test.
    uint bounce;
};

#define PATH_TRACE_SKY_WITHOUT_SUN_LUMINANCE 10.0f // The brightest pixel away from the sun on the test sky is 3.2.

struct PathTraceShadowPayload
{
    float occluded;
    float alphaConeSpread;
};

// 16:16 octahedral. Directions take a third of the payload otherwise, and payload size costs registers on every trace.
uint PathTracePackDirection(float3 direction)
{
    direction /= abs(direction.x) + abs(direction.y) + abs(direction.z);
    float2 octahedral = direction.z >= 0.0f ? direction.xy : (1.0f - abs(direction.yx)) * select(direction.xy >= 0.0f, 1.0f.xx, -1.0f.xx);
    const uint2 encoded = uint2(saturate(octahedral * 0.5f + 0.5f) * 65535.0f + 0.5f);
    return encoded.x | (encoded.y << 16u);
}

float3 PathTraceUnpackDirection(uint packed)
{
    const float2 octahedral = float2(packed & 0xFFFFu, packed >> 16u) / 65535.0f * 2.0f - 1.0f;
    float3 direction = float3(octahedral, 1.0f - abs(octahedral.x) - abs(octahedral.y));
    const float fold = saturate(-direction.z);
    direction.xy += select(direction.xy >= 0.0f, -fold.xx, fold.xx);
    return normalize(direction);
}

float PathTraceLuminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

// Camera rays through the lens drawn per pixel and frame, of which one is traced (PathTraceLensCameraRay): for Ray
// Reconstruction, and when accumulating. Each candidate costs a full lens transport and its share of the scene's rays
// (about 2.8 ms at 2560x1600 through the double Gauss): see SARP's docs/lens_simulation.md.
#ifndef PATH_TRACE_LENS_CANDIDATES
#define PATH_TRACE_LENS_CANDIDATES 1
#endif
#ifndef PATH_TRACE_LENS_ACCUMULATE_CANDIDATES
#define PATH_TRACE_LENS_ACCUMULATE_CANDIDATES 1
#endif

// A primary ray through the physical lens, resampled from candidateCount candidates by their weights' luminance (resampled
// importance sampling with the camera weight as the target). The traced ray's weight is
// w_s * (sum of luminances / N) / luminance(w_s): an unbiased estimate of the pixel's lens response for any N, whose spread
// is that of the N-sample average rather than of one sample. The exact pupil sampler (lensCamera.hlsli) leaves about 2-4%
// of candidates blocked, so one is enough for Ray Reconstruction: its halo around bright edges, which came from blocked
// candidates flickering, is +1% on the test chart (+8% with 14% blocked). False only when every candidate is blocked.
bool PathTraceLensCameraRay(Camera camera, float2 pixelPosition, float2 resolution, uint candidateCount, inout PathRng rng, out RayDesc ray,
    out float3 weight)
{
    ray = (RayDesc)0;
    weight = 0.0f.xxx;
    float total = 0.0f;
    float selectedTarget = 0.0f;
    [loop]
    for (uint candidate = 0u; candidate < candidateCount; ++candidate) {
        const float2 pupilSample = PathRngNext2(rng);
        const float wavelengthSample = PathRngNext1(rng);
        const float selection = PathRngNext1(rng);
        RayDesc candidateRay;
        float3 candidateWeight;
        if (!LensCameraRay(camera, pixelPosition, resolution, pupilSample, wavelengthSample, candidateRay, candidateWeight)) continue;
        // Luminance is Y, which no wavelength's response makes negative.
        const float target = PathTraceLuminance(candidateWeight);
        if (!(target > 0.0f)) continue;
        total += target;
        if (selection * total < target) {
            ray = candidateRay;
            weight = candidateWeight;
            selectedTarget = target;
        }
    }
    if (!(selectedTarget > 0.0f)) return false;
    weight *= total / (float(candidateCount) * selectedTarget);
    return true;
}

// The physical lens's motion: both view-space directions through its chief-ray mapping, as pinhole motion projects both
// positions through the unjittered matrices, each at its own frame's focus (focus breathing changes the mapping a little). A
// defocused point gets its in-focus position's motion.
float2 PathTraceLensMotion(Camera camera, float3 currentView, float3 previousView)
{
    StructuredBuffer<float4> data = LensData();
    const LensHeader header = LensLoadHeader(data);
    if (!header.valid) return 0.0f.xx;
    const LensFrame frame = LensLoadFrame(data, header, false, camera.unjitteredProjection);
    const LensFrame previousFrame = LensLoadFrame(data, header, true, camera.prevUnjitteredProjection);
    const float2 current = LensProjectViewDirection(data, header, frame.focus, frame.sensor, currentView);
    const float2 previous = LensProjectViewDirection(data, header, previousFrame.focus, previousFrame.sensor, previousView);
    return current - previous;
}

void PathTraceWritePrimaryMiss(uint2 pixel, float3 direction, Camera camera)
{
    const float3 currentView = normalize(mul(float4(direction, 0.0f), camera.view).xyz);
    const float3 previousView = normalize(mul(float4(direction, 0.0f), camera.prevView).xyz);
    RWTexture2D<float2> motion = ResourceDescriptorHeap[CLOD_RT_PT_MOTION_UAV];
    if ((CLOD_RT_PT_FLAGS & CLOD_RT_PT_FLAG_LENS) != 0u) {
        motion[pixel] = PathTraceLensMotion(camera, currentView, previousView);
    }
    else {
        const float4 currentClip = mul(float4(currentView, 1.0f), camera.unjitteredProjection);
        const float4 previousClip = mul(float4(previousView, 1.0f), camera.prevUnjitteredProjection);
        motion[pixel] = currentClip.xy / max(abs(currentClip.w), 1e-6f) - previousClip.xy / max(abs(previousClip.w), 1e-6f);
    }
    RWTexture2D<float> projectedDepth = ResourceDescriptorHeap[CLOD_RT_PT_PROJECTED_DEPTH_UAV];
    projectedDepth[pixel] = 0.0f;
    // Every guide a surface hit writes, so none keeps a surface from an earlier frame. Specular albedo is cleared by no pass:
    // left alone, sky pixels kept the albedo of whatever geometry last covered them, and Ray Reconstruction drew those stale
    // edges into the sky after the camera moved. Base colour and normal are cleared with the visibility buffer, but only
    // while that pass is in the graph.
    RWTexture2D<float4> baseColor = ResourceDescriptorHeap[CLOD_RT_PT_BASE_COLOR_UAV];
    baseColor[pixel] = 0.0f.xxxx;
    RWTexture2D<float4> normalRoughness = ResourceDescriptorHeap[CLOD_RT_PT_NORMAL_ROUGHNESS_UAV];
    normalRoughness[pixel] = 0.0f.xxxx;
    if (CLOD_RT_PT_SPECULAR_ALBEDO_UAV != CLOD_RT_PT_NO_DESCRIPTOR) {
        RWTexture2D<float4> specularAlbedo = ResourceDescriptorHeap[CLOD_RT_PT_SPECULAR_ALBEDO_UAV];
        specularAlbedo[pixel] = 0.0f.xxxx;
    }
    if (CLOD_RT_PT_SPECULAR_HIT_UAV != CLOD_RT_PT_NO_DESCRIPTOR) {
        RWTexture2D<float> specularHit = ResourceDescriptorHeap[CLOD_RT_PT_SPECULAR_HIT_UAV];
        specularHit[pixel] = camera.zFar;
    }
}

[shader("raygeneration")]
void PathTraceRayGen()
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const Camera camera = cameras[perFrame.mainCameraIndex];
    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[CLOD_RT_SEC_TLAS_SRV];

    const uint flags = CLOD_RT_PT_FLAGS;
    const uint2 pixel = DispatchRaysIndex().xy;
    PathRng rng = PathRngCreate(pixel, CLOD_RT_SEC_FRAME, 0u);

    // Nothing else jitters when accumulating without an upscaler, and every sample must land somewhere new in the pixel.
    const float2 subpixel = (flags & CLOD_RT_PT_FLAG_ACCUMULATE) != 0u ? PathRngNext2(rng) : 0.5f.xx;
    RayDesc primary;
    float3 primaryWeight = 1.0f.xxx;
    bool lensBlocked = false;
    bool guidesOnly = false;
    if ((flags & CLOD_RT_PT_FLAG_LENS) != 0u) {
        // Through the physical lens (lensCamera.hlsli: its surfaces, or its polynomial). A pixel whose candidates the lens all
        // stops carries nothing, as it would on film: the pixel's average over samples is what vignetting looks like.
        const uint candidates = (flags & CLOD_RT_PT_FLAG_ACCUMULATE) != 0u ? PATH_TRACE_LENS_ACCUMULATE_CANDIDATES : PATH_TRACE_LENS_CANDIDATES;
        lensBlocked = !PathTraceLensCameraRay(camera, float2(pixel) + subpixel, float2(DispatchRaysDimensions().xy), candidates, rng, primary,
            primaryWeight);
        if (lensBlocked) {
            primary = CLodRtCameraRayAt(camera, float2(pixel) + subpixel, float2(DispatchRaysDimensions().xy));
            primaryWeight = 0.0f.xxx;
            // Ray Reconstruction reads the primary hit's guides. A blocked pixel that wrote the miss guides (projected depth 0)
            // told it the pixel was sky, at random a third of the pixels each frame, and its output came out 30% dark (0.71 of
            // the accumulated image on the lens chart). The pinhole ray's surface, at zero radiance, says it is dark here.
            // Accumulation has no guides to feed, so it writes the miss guides and traces nothing.
            guidesOnly = (flags & CLOD_RT_PT_FLAG_ACCUMULATE) == 0u;
            if (!guidesOnly) {
                PathTraceWritePrimaryMiss(pixel, primary.Direction, camera);
            }
        }
    }
    else {
        primary = CLodRtCameraRayAt(camera, float2(pixel) + subpixel, float2(DispatchRaysDimensions().xy));
    }

    RayDesc ray = primary;
    float3 radiance = 0.0f.xxx;
    float3 throughput = primaryWeight;
    float coneWidth = 0.0f;
    const float pixelSpread = CLodRtPixelConeSpread(camera, float(perFrame.screenResY));
    float coneSpread = pixelSpread;
    const uint maxBounces = lensBlocked ? (guidesOnly ? 1u : 0u) : CLOD_RT_PT_MAX_BOUNCES;
    // Pixel watch: the bounce that added most, and how the primary hit went.
    const bool watch = PathWatchEnabled();
    float watchTopLuminance = -1.0f;
    uint watchTopBounce = 0u;
    uint4 watchTopIds = 0u.xxxx;
    float3 watchTopEmitted = 0.0f.xxx;
    float3 watchTopDirect = 0.0f.xxx;
    float watchTopThroughput = 0.0f;
    bool watchPrimaryTerrain = false;
    uint4 watchPrimaryIds = 0u.xxxx;
    float watchPrimaryT = 0.0f;
    uint watchPrimaryFlags = 0u;
    float watchPrimaryThroughput = 0.0f;
    bool watchHitSomething = false;
    for (uint bounce = 0u; bounce < maxBounces; ++bounce) {
        PathTracePayload payload = (PathTracePayload)0;
        payload.rngState = rng.state;
        payload.rngIncrement = rng.increment;
        payload.coneWidth = coneWidth;
        payload.coneSpread = coneSpread;
        payload.alphaConeSpread = pixelSpread;
        payload.bounce = bounce;

        const uint twoStateFlag = bounce == 0u ? CLOD_RT_PT_FLAG_OMM_2_STATE_PRIMARY : CLOD_RT_PT_FLAG_OMM_2_STATE_SECONDARY;
        const uint ommFlags = (CLOD_RT_PT_FLAGS & twoStateFlag) != 0u ? RAY_FLAG_FORCE_OMM_2_STATE : 0u;
        dx::HitObject hit = dx::HitObject::TraceRay(scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES | ommFlags, 0xFFu, 0u, 1u, 0u, ray, payload);
        const bool reorder = (flags & (bounce == 0u ? CLOD_RT_PT_FLAG_REORDER_PRIMARY : CLOD_RT_PT_FLAG_REORDER_SECONDARY)) != 0u;
        if (reorder) {
            // After the hit group, rays heading the same way are likely to touch the same memory next.
            const uint octant = (ray.Direction.x < 0.0f ? 1u : 0u) | (ray.Direction.y < 0.0f ? 2u : 0u) | (ray.Direction.z < 0.0f ? 4u : 0u);
            dx::MaybeReorderThread(hit, octant, 3u);
        }
        dx::HitObject::Invoke(hit, payload);

        rng.state = payload.rngState;
        rng.increment = payload.rngIncrement;

        float3 contribution = payload.emitted;
        bool watchShadowVisible = false;
        if (payload.shadowTMax > 0.0f && PATH_TRACE_EXPERIMENT != 2 && !guidesOnly) {
            RayDesc shadowRay;
            shadowRay.Origin = payload.nextOrigin;
            shadowRay.Direction = PathTraceUnpackDirection(payload.shadowDirection);
            shadowRay.TMin = 0.0f;
            shadowRay.TMax = payload.shadowTMax;
            const uint shadowOmmFlags = (CLOD_RT_PT_FLAGS & CLOD_RT_PT_FLAG_OMM_2_STATE_SECONDARY) != 0u ? RAY_FLAG_FORCE_OMM_2_STATE : 0u;
            // Inline, with alpha candidates tested here: through TraceRay, each one called the shadow any-hit through the shader
            // table, which measured 0.6 ms slower of 14.8 at the saved camera. The shadow hit groups and miss shader stay in the
            // table only so the terrain instances' hit group offset keeps its meaning.
            // 3: RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS | RAYQUERY_FLAG_ALLOW_CLUSTERED_GEOMETRY.
            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 3> shadowQuery;
            shadowQuery.TraceRayInline(scene, shadowOmmFlags, 0xFFu, shadowRay);
            while (shadowQuery.Proceed()) {
                if (shadowQuery.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
                    StructuredBuffer<CLodRtClasInfo> clasInfo = ResourceDescriptorHeap[CLOD_RT_SEC_CLAS_INFO_SRV];
                    if (PathTraceSkipAlphaTest(shadowQuery.CandidateInstanceID()) || !CLodRtCandidateIsTransparent(clasInfo, shadowQuery.CandidateInstanceID(), shadowQuery.CandidateInstanceIndex(),
                            shadowQuery.CandidateClusterID(), shadowQuery.CandidatePrimitiveIndex(), shadowQuery.CandidateTriangleBarycentrics(),
                            shadowRay.Direction, shadowQuery.CandidateTriangleRayT(), pixelSpread)) {
                        shadowQuery.CommitNonOpaqueTriangleHit();
                    }
                }
            }
            if (shadowQuery.CommittedStatus() == COMMITTED_NOTHING) {
                contribution += payload.unoccluded;
                watchShadowVisible = true;
            }
        }
        if (watch && hit.IsHit()) {
            const uint4 ids = uint4(hit.GetInstanceID(), hit.GetInstanceIndex(), 0xFFFFFFFFu, hit.GetPrimitiveIndex());
            if (bounce == 0u) {
                watchHitSomething = true;
                // Hit groups: 0 general radiance, 2 terrain radiance (the table order in PathTracePass.h).
                watchPrimaryTerrain = hit.GetShaderTableIndex() == 2u;
                watchPrimaryIds = ids;
                watchPrimaryT = hit.GetRayTCurrent();
                watchPrimaryFlags = (payload.hitT < 0.0f ? 1u : 0u) | (payload.shadowTMax > 0.0f ? 2u : 0u) | (watchShadowVisible ? 4u : 0u);
                watchPrimaryThroughput = PathTraceLuminance(payload.weight);
            }
            const float added = PathTraceLuminance(contribution * throughput);
            if (added > watchTopLuminance) {
                watchTopLuminance = added;
                watchTopBounce = bounce;
                watchTopIds = ids;
                watchTopEmitted = payload.emitted * throughput;
                watchTopDirect = watchShadowVisible ? payload.unoccluded * throughput : 0.0f.xxx;
                watchTopThroughput = PathTraceLuminance(throughput);
            }
        }
        contribution *= throughput;
        // Without a denoiser, one sample of a rare bright path would stay on screen for hundreds of frames.
        if ((flags & CLOD_RT_PT_FLAG_FIREFLY_CLAMP) != 0u && bounce > 0u) {
            const float luminance = PathTraceLuminance(contribution);
            if (luminance > 100.0f) {
                contribution *= 100.0f / luminance;
            }
        }
        radiance += contribution;

        if (payload.hitT < 0.0f || PATH_TRACE_EXPERIMENT == 4) {
            break;
        }
        throughput *= payload.weight;
        if ((flags & CLOD_RT_PT_FLAG_ROULETTE) != 0u && bounce > 1u) {
            const float survival = min(0.95f, PathTraceLuminance(throughput));
            if (PathRngNext1(rng) >= survival) {
                break;
            }
            throughput /= survival;
        }
        ray.Origin = payload.nextOrigin;
        ray.Direction = PathTraceUnpackDirection(payload.nextDirection);
        ray.TMin = 0.0f;
        ray.TMax = 1e20f;
        coneWidth = payload.coneWidth;
        coneSpread = payload.coneSpread;
    }

    if (guidesOnly && CLOD_RT_PT_SPECULAR_HIT_UAV != CLOD_RT_PT_NO_DESCRIPTOR) {
        // No bounce was traced to say how far the reflection is.
        RWTexture2D<float> specularHit = ResourceDescriptorHeap[CLOD_RT_PT_SPECULAR_HIT_UAV];
        specularHit[pixel] = camera.zFar;
    }
    if (any(isnan(radiance)) || any(isinf(radiance))) {
        radiance = 0.0f.xxx;
        if (watch) PathWatchCount(CLOD_RT_WATCH_NON_FINITE);
    }
    if (watch && watchHitSomething) {
        const float luminance = PathTraceLuminance(radiance);
        if (luminance > 1.0f) {
            PathWatchMaxLuminance(luminance); // Only bright pixels contend for the word.
        }
        if (luminance > asfloat(CLOD_RT_PT_WATCH_BRIGHT)) {
            PathWatchRecord(CLOD_RT_WATCH_BRIGHT_PIXEL, watchTopBounce, 0u, pixel, watchTopIds, asuint(watchTopThroughput),
                asuint(radiance), asuint(watchTopEmitted), asuint(watchTopDirect));
        }
        if (watchPrimaryTerrain && luminance < 1e-3f) {
            RWTexture2D<float4> baseColor = ResourceDescriptorHeap[CLOD_RT_PT_BASE_COLOR_UAV];
            PathWatchRecord(CLOD_RT_WATCH_DARK_TERRAIN_PIXEL, 0u, watchPrimaryFlags, pixel, watchPrimaryIds, 0u,
                asuint(radiance), asuint(baseColor[pixel].rgb), asuint(float3(watchPrimaryT, watchPrimaryThroughput, watchTopLuminance)));
        }
    }

    RWTexture2D<float4> output = ResourceDescriptorHeap[CLOD_RT_SEC_OUTPUT_UAV];
    if ((flags & CLOD_RT_PT_FLAG_ACCUMULATE) != 0u) {
        // Alpha counts samples, so a moved camera restarts every pixel without any state on the CPU.
        RWTexture2D<float4> accumulation = ResourceDescriptorHeap[CLOD_RT_PT_ACCUMULATION_UAV];
        // The lens's focus moves without a rebuild (LensFocusPass, autofocus): it says when it moved.
        const bool still = (flags & CLOD_RT_PT_FLAG_RESTART_ACCUMULATION) == 0u
            && ((flags & CLOD_RT_PT_FLAG_LENS) == 0u || !LensFocusRestart())
            && all(camera.view[0] == camera.prevView[0]) && all(camera.view[1] == camera.prevView[1])
            && all(camera.view[2] == camera.prevView[2]) && all(camera.view[3] == camera.prevView[3]);
        const float4 history = still ? accumulation[pixel] : 0.0f.xxxx;
        const float samples = history.a + 1.0f;
        const float3 average = lerp(history.rgb, radiance, 1.0f / samples);
        accumulation[pixel] = float4(average, samples);
        output[pixel] = float4(average, 1.0f);
    }
    else {
        output[pixel] = float4(radiance, 1.0f);
    }
}

[shader("miss")]
void PathTraceRadianceMiss(inout PathTracePayload payload)
{
    payload.hitT = -1.0f;
    payload.emitted = (CLOD_RT_PT_FLAGS & CLOD_RT_PT_FLAG_SUN_ONLY) != 0u ? 0.0f.xxx : PathSampleSky(WorldRayDirection());
    // The environment image has its own sun, up to 65504 and 59% of its energy, and the directional light already stands for it.
    // A BSDF ray finding it would count the sun twice, as one-in-thousands samples.
    if (payload.bounce > 0u) {
        const float skyLuminance = PathTraceLuminance(payload.emitted);
        if (skyLuminance > PATH_TRACE_SKY_WITHOUT_SUN_LUMINANCE) {
            payload.emitted *= PATH_TRACE_SKY_WITHOUT_SUN_LUMINANCE / skyLuminance;
        }
    }
    if (payload.bounce == 0u) {
        ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
        StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
        PathTraceWritePrimaryMiss(DispatchRaysIndex().xy, WorldRayDirection(), cameras[perFrame.mainCameraIndex]);
    }
    else if (payload.bounce == 1u && CLOD_RT_PT_SPECULAR_HIT_UAV != CLOD_RT_PT_NO_DESCRIPTOR) {
        ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
        StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
        RWTexture2D<float> specularHit = ResourceDescriptorHeap[CLOD_RT_PT_SPECULAR_HIT_UAV];
        specularHit[DispatchRaysIndex().xy] = cameras[perFrame.mainCameraIndex].zFar;
    }
}

[shader("miss")]
void PathTraceShadowMiss(inout PathTraceShadowPayload payload)
{
    payload.occluded = 0.0f;
}

void PathTraceWritePrimarySurface(uint2 pixel, CLodPathHitSurface hit, FragmentInfo fragment, float3 worldPosition)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const Camera camera = cameras[perFrame.mainCameraIndex];

    RWTexture2D<float4> baseColor = ResourceDescriptorHeap[CLOD_RT_PT_BASE_COLOR_UAV];
    baseColor[pixel] = float4(hit.materialInputs.albedo * (1.0f - saturate(hit.materialInputs.metallic)), 1.0f);
    RWTexture2D<float4> normalRoughness = ResourceDescriptorHeap[CLOD_RT_PT_NORMAL_ROUGHNESS_UAV];
    const bool flipNormal = !hit.frontFace && (hit.materialFlags & MATERIAL_DOUBLE_SIDED) != 0u;
    const float3 materialNormal = normalize(hit.materialInputs.normalWS);
    normalRoughness[pixel] = float4(flipNormal ? -materialNormal : materialNormal, saturate(hit.materialInputs.roughness));
    RWTexture2D<float2> motion = ResourceDescriptorHeap[CLOD_RT_PT_MOTION_UAV];
    if ((CLOD_RT_PT_FLAGS & CLOD_RT_PT_FLAG_LENS) != 0u) {
        // The previous view-space direction from pinhole motion (which carries object motion), then both through the lens.
        const float3 currentView = mul(float4(worldPosition, 1.0f), camera.view).xyz;
        const float4 currentClip = mul(float4(currentView, 1.0f), camera.unjitteredProjection);
        const float2 previousNdc = currentClip.xy / max(abs(currentClip.w), 1e-6f) - hit.motionVector;
        const float3 previousView = float3(previousNdc.x / camera.prevUnjitteredProjection[0][0], previousNdc.y / camera.prevUnjitteredProjection[1][1], -1.0f);
        motion[pixel] = PathTraceLensMotion(camera, currentView, previousView);
    }
    else {
        motion[pixel] = hit.motionVector;
    }

    // The same reverse-Z value raster's depth copy writes, from view depth.
    const float viewDepth = -mul(float4(worldPosition, 1.0f), camera.view).z;
    RWTexture2D<float> projectedDepth = ResourceDescriptorHeap[CLOD_RT_PT_PROJECTED_DEPTH_UAV];
    projectedDepth[pixel] = -camera.unjitteredProjection[2][2] + camera.unjitteredProjection[3][2] / max(viewDepth, 1e-4f);

    if (CLOD_RT_PT_SPECULAR_ALBEDO_UAV != CLOD_RT_PT_NO_DESCRIPTOR) {
        RWTexture2D<float4> specularAlbedo = ResourceDescriptorHeap[CLOD_RT_PT_SPECULAR_ALBEDO_UAV];
        const float3 f0 = lerp(fragment.dielectricSpecularF0, fragment.metalSpecularF0, saturate(hit.materialInputs.metallic));
        specularAlbedo[pixel] = float4(saturate(mx_ggx_dir_albedo_analytic(saturate(fragment.NdotV), fragment.specularAlpha, f0, 1.0f.xxx)), 1.0f);
    }
}

void PathTraceShadeHit(inout PathTracePayload payload, BuiltInTriangleIntersectionAttributes attributes, bool isTerrain)
{
    const float t = RayTCurrent();
    const float3 rayDirection = WorldRayDirection();
    const float3 position = WorldRayOrigin() + rayDirection * t;
    const float coneWidth = payload.coneWidth + payload.coneSpread * t;
    const bool isPrimary = payload.bounce == 0u;

    CLodPathHitSurface hit;
    if (!CLodResolvePathHit(InstanceID(), InstanceIndex(), ClusterID(), PrimitiveIndex(), attributes.barycentrics, HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE,
            position, rayDirection, coneWidth, isPrimary, isTerrain, hit)) {
        payload.hitT = -1.0f;
        if (PathWatchEnabled()) {
            StructuredBuffer<CLodRtClasInfo> clasInfo = ResourceDescriptorHeap[CLOD_RT_SEC_CLAS_INFO_SRV];
            const CLodRtClasInfo info = clasInfo[ClusterID()];
            PathWatchRecord(CLOD_RT_WATCH_RESOLVE_FAILED, payload.bounce, g_clodPathHitFailure | (isTerrain ? CLOD_RT_WATCH_TERRAIN : 0u),
                DispatchRaysIndex().xy, uint4(InstanceID(), InstanceIndex(), ClusterID(), PrimitiveIndex()), g_clodPathHitFailureDetail,
                uint3(info.slabDescriptor, info.pageOffsetUnits, info.localMeshlet), uint3(info.groupLocalIndex, asuint(t), 0u), asuint(position));
        }
        return;
    }
    FragmentInfo fragment;
    CLodPathHitFillFragment(hit, position, rayDirection, DispatchRaysIndex().xy, fragment);

    if (isPrimary) {
        PathTraceWritePrimarySurface(DispatchRaysIndex().xy, hit, fragment, position);
    }
    else if (payload.bounce == 1u && CLOD_RT_PT_SPECULAR_HIT_UAV != CLOD_RT_PT_NO_DESCRIPTOR) {
        RWTexture2D<float> specularHit = ResourceDescriptorHeap[CLOD_RT_PT_SPECULAR_HIT_UAV];
        specularHit[DispatchRaysIndex().xy] = t;
    }

    PathRng rng;
    rng.state = payload.rngState;
    rng.increment = payload.rngIncrement;

    const float3 viewDirection = -rayDirection;
    const PathBsdfContext context = PathBsdfMakeContext(fragment, hit.geometricNormalWS, viewDirection);

    if ((CLOD_RT_PT_FLAGS & CLOD_RT_PT_FLAG_SUN_ONLY) == 0u) {
        const OpenPBRBaseLayerState baseState = MakeOpenPBRBaseLayerState(fragment.albedo, fragment.diffuseColor, fragment.baseDiffuseRoughness,
            fragment.specularAlpha, fragment.weightedSpecularIor, fragment.dielectricSpecularF0, fragment.dielectricSpecularWeight,
            fragment.metalAverageFresnel, fragment.metalSpecularF0, fragment.metalSpecularWeight);
        const OpenPBRCoatLayerState coatState = MakeOpenPBRCoatLayerState(baseState, fragment.coatColor, fragment.coatWeight, fragment.coatIor,
            fragment.coatRoughness, fragment.coatDarkening);
        payload.emitted = EvaluateOpenPBREmissive(fragment.emissive, coatState, fragment.fuzzWeight, fragment.fuzzRoughness, fragment.NdotV);
    }

    // Spawn on the side the ray arrived from, far enough to clear float error at this distance from the origin.
    const float3 facingNormal = dot(hit.geometricNormalWS, viewDirection) >= 0.0f ? hit.geometricNormalWS : -hit.geometricNormalWS;
    const float magnitude = max(abs(position.x), max(abs(position.y), abs(position.z)));
    payload.nextOrigin = position + facingNormal * max(0.02f, magnitude * 3e-7f);

    PathLightSample light = PathSampleOneLight(position, fragment.normalWS, rng);
    if ((CLOD_RT_PT_FLAGS & CLOD_RT_PT_FLAG_SUN_ONLY) != 0u && light.distance < 1e29f) {
        light.valid = false;
    }
    if (light.valid) {
        const float3 unoccluded = PathBsdfEval(context, light.direction) * light.radiance / light.pdf;
        if (any(unoccluded > 0.0f)) {
            payload.unoccluded = unoccluded;
            payload.shadowDirection = PathTracePackDirection(light.direction);
            payload.shadowTMax = light.distance;
        }
    }

    float3 nextDirection;
    float pdf;
    float3 weight;
    if (PathBsdfSample(context, rng, nextDirection, pdf, weight)) {
        payload.hitT = t;
        payload.weight = weight;
        payload.nextDirection = PathTracePackDirection(nextDirection);
        payload.coneWidth = coneWidth;
        // The lobe's own spread on top of the ray's. A rough enough lobe has no use for a sharp texture sample.
        payload.coneSpread += fragment.specularAlpha;
    }
    else {
        payload.hitT = -1.0f;
    }

    payload.rngState = rng.state;
    payload.rngIncrement = rng.increment;

    if (PathWatchEnabled() && isPrimary && payload.shadowTMax <= 0.0f && payload.hitT < 0.0f) {
        // misc: 1 the light sample was valid, 2 no lobe has weight, 4 a lobe weight isn't finite, 8 the light's BSDF value
        // isn't finite, 16 the normal isn't finite, 32 the light's radiance or pdf isn't finite.
        const float3 lightValue = light.valid ? PathBsdfEval(context, light.direction) : 0.0f.xxx;
        const uint why = (light.valid ? 1u : 0u) | (all(context.lobeProbabilities == 0.0f) ? 2u : 0u)
            | (!all(isfinite(context.lobeProbabilities)) ? 4u : 0u) | (!PathWatchFinite3(lightValue) ? 8u : 0u)
            | (!PathWatchFinite3(context.normal) ? 16u : 0u) | (!all(isfinite(float4(light.radiance, light.pdf))) ? 32u : 0u);
        PathWatchRecord(CLOD_RT_WATCH_PRIMARY_ENDED, 0u, isTerrain ? CLOD_RT_WATCH_TERRAIN : 0u, DispatchRaysIndex().xy,
            uint4(InstanceID(), InstanceIndex(), ClusterID(), PrimitiveIndex()), why,
            asuint(float3(context.normDotView, context.specularAlpha, t)),
            asuint(float3(light.valid ? dot(context.normal, light.direction) : -2.0f, light.valid ? dot(context.geometricNormal, light.direction) : -2.0f,
                PathTraceLuminance(lightValue))),
            asuint(context.lobeProbabilities.xyz));
    }

    if (PathWatchEnabled()) {
        const MaterialInputs m = hit.materialInputs;
        uint bad = 0u;
        if (!PathWatchFinite3(m.albedo)) bad |= CLOD_RT_WATCH_ALBEDO_NON_FINITE;
        else if (any(m.albedo > 1.001f) || any(m.albedo < 0.0f)) bad |= CLOD_RT_WATCH_ALBEDO_RANGE;
        if (!PathWatchFinite3(m.emissive)) bad |= CLOD_RT_WATCH_EMISSIVE_NON_FINITE;
        else if (PathTraceLuminance(m.emissive) > CLOD_RT_WATCH_EMISSIVE_LIMIT) bad |= CLOD_RT_WATCH_EMISSIVE_HUGE;
        if (!all(isfinite(float4(m.roughness, m.metallic, m.coatWeight, m.fuzzWeight)))) bad |= CLOD_RT_WATCH_SCALAR_NON_FINITE;
        if (!PathWatchFinite3(m.normalWS) || dot(m.normalWS, m.normalWS) < 0.25f) bad |= CLOD_RT_WATCH_NORMAL_BAD;
        if (payload.hitT >= 0.0f) {
            if (!PathWatchFinite3(payload.weight)) bad |= CLOD_RT_WATCH_WEIGHT_NON_FINITE;
            else if (PathTraceLuminance(payload.weight) > CLOD_RT_WATCH_WEIGHT_LIMIT) bad |= CLOD_RT_WATCH_WEIGHT_HUGE;
        }
        if (payload.shadowTMax > 0.0f) {
            if (!PathWatchFinite3(payload.unoccluded)) bad |= CLOD_RT_WATCH_DIRECT_NON_FINITE;
            else if (PathTraceLuminance(payload.unoccluded) > asfloat(CLOD_RT_PT_WATCH_BRIGHT)) bad |= CLOD_RT_WATCH_DIRECT_HUGE;
        }
        const uint4 ids = uint4(InstanceID(), InstanceIndex(), ClusterID(), PrimitiveIndex());
        if (bad != 0u) {
            PathWatchRecord(CLOD_RT_WATCH_BAD_MATERIAL, payload.bounce, bad | (isTerrain ? CLOD_RT_WATCH_TERRAIN : 0u), DispatchRaysIndex().xy, ids,
                m.openPBRMaterialDataIndex, asuint(m.albedo), asuint(m.emissive),
                asuint(float3(m.roughness, m.metallic, PathTraceLuminance(payload.hitT >= 0.0f ? payload.weight : payload.unoccluded))));
        }
        if (isPrimary && isTerrain && PathTraceLuminance(m.albedo) < 0.005f) {
            PathWatchRecord(CLOD_RT_WATCH_DARK_TERRAIN_ALBEDO, 0u, m.terrainRvtFallbackReason & 0xFFFFu, DispatchRaysIndex().xy, ids,
                m.terrainRvtDebugFlags, asuint(m.albedo), uint3(m.terrainRvtRequestedMip, m.terrainRvtResidentMip, m.terrainRvtPageTableIndex),
                uint3(m.terrainRvtPhysicalPageIndex, m.terrainRvtAtlasPoolIndex, asuint(t)));
        }
    }
}

[shader("closesthit")]
void PathTraceGeneralClosestHit(inout PathTracePayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    PathTraceShadeHit(payload, attributes, false);
}

[shader("closesthit")]
void PathTraceTerrainClosestHit(inout PathTracePayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    PathTraceShadeHit(payload, attributes, true);
}

// Instances without alpha-tested materials are forced opaque when the TLAS is built, so these run only for alpha.
bool PathTraceHitIsTransparent(BuiltInTriangleIntersectionAttributes attributes, float coneSpread)
{
    if (PathTraceSkipAlphaTest(InstanceID())) return false;
    StructuredBuffer<CLodRtClasInfo> clasInfo = ResourceDescriptorHeap[CLOD_RT_SEC_CLAS_INFO_SRV];
    return CLodRtCandidateIsTransparent(clasInfo, InstanceID(), InstanceIndex(), ClusterID(), PrimitiveIndex(), attributes.barycentrics, WorldRayDirection(), RayTCurrent(), coneSpread);
}

[shader("anyhit")]
void PathTraceRadianceAnyHit(inout PathTracePayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    g_clodPathHitIsPrimary = payload.bounce == 0u;
    if (PathTraceHitIsTransparent(attributes, payload.alphaConeSpread)) {
        IgnoreHit();
    }
}

[shader("anyhit")]
void PathTraceShadowAnyHit(inout PathTraceShadowPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    if (PathTraceHitIsTransparent(attributes, payload.alphaConeSpread)) {
        IgnoreHit();
    }
}
