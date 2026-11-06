// Reference path tracer. One state object: PathTracePass.h lists its shaders and hit groups.
//
// Hit group index = ray contribution (0 radiance, 1 shadow) + instance contribution (0 general, 2 terrain, from
// rayTracingSceneBuild.hlsl), with a multiplier of 1 because records are laid out in that order.
//
// Recursion depth is 1. A closest hit evaluates the material, samples the BSDF and one light, and returns the next ray and an
// unoccluded light contribution. Ray generation traces that shadow ray itself, so shading never holds traversal state.
//
// Alpha tests use one fixed mip per texture, which opacity micromaps are baked against: see clodRayTracingAlpha.hlsli.
#define CLOD_RT_FIXED_ALPHA_MIP 1
#include "include/pathTracingHit.hlsli"
#include "include/clodRayTracingHits.hlsli"
#include "include/pathTracingBsdf.hlsli"
#include "include/pathTracingLights.hlsli"

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

void PathTraceWritePrimaryMiss(uint2 pixel, float3 direction, Camera camera)
{
    const float3 currentView = normalize(mul(float4(direction, 0.0f), camera.view).xyz);
    const float4 currentClip = mul(float4(currentView, 1.0f), camera.unjitteredProjection);
    const float3 previousView = normalize(mul(float4(direction, 0.0f), camera.prevView).xyz);
    const float4 previousClip = mul(float4(previousView, 1.0f), camera.prevUnjitteredProjection);
    RWTexture2D<float2> motion = ResourceDescriptorHeap[CLOD_RT_PT_MOTION_UAV];
    motion[pixel] = currentClip.xy / max(abs(currentClip.w), 1e-6f) - previousClip.xy / max(abs(previousClip.w), 1e-6f);
    RWTexture2D<float> projectedDepth = ResourceDescriptorHeap[CLOD_RT_PT_PROJECTED_DEPTH_UAV];
    projectedDepth[pixel] = 0.0f;
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
    const RayDesc primary = CLodRtCameraRayAt(camera, float2(pixel) + subpixel, float2(DispatchRaysDimensions().xy));

    RayDesc ray = primary;
    float3 radiance = 0.0f.xxx;
    float3 throughput = 1.0f.xxx;
    float coneWidth = 0.0f;
    const float pixelSpread = CLodRtPixelConeSpread(camera, float(perFrame.screenResY));
    float coneSpread = pixelSpread;
    const uint maxBounces = CLOD_RT_PT_MAX_BOUNCES;
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
        if (payload.shadowTMax > 0.0f) {
            RayDesc shadowRay;
            shadowRay.Origin = payload.nextOrigin;
            shadowRay.Direction = PathTraceUnpackDirection(payload.shadowDirection);
            shadowRay.TMin = 0.0f;
            shadowRay.TMax = payload.shadowTMax;
            PathTraceShadowPayload shadowPayload = (PathTraceShadowPayload)0;
            shadowPayload.occluded = 1.0f;
            shadowPayload.alphaConeSpread = pixelSpread;
            const uint shadowOmmFlags = (CLOD_RT_PT_FLAGS & CLOD_RT_PT_FLAG_OMM_2_STATE_SECONDARY) != 0u ? RAY_FLAG_FORCE_OMM_2_STATE : 0u;
            TraceRay(scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | shadowOmmFlags, 0xFFu, 1u, 1u, 1u, shadowRay, shadowPayload);
            if (shadowPayload.occluded == 0.0f) {
                contribution += payload.unoccluded;
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

        if (payload.hitT < 0.0f) {
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

    if (any(isnan(radiance)) || any(isinf(radiance))) {
        radiance = 0.0f.xxx;
    }

    RWTexture2D<float4> output = ResourceDescriptorHeap[CLOD_RT_SEC_OUTPUT_UAV];
    if ((flags & CLOD_RT_PT_FLAG_ACCUMULATE) != 0u) {
        // Alpha counts samples, so a moved camera restarts every pixel without any state on the CPU.
        RWTexture2D<float4> accumulation = ResourceDescriptorHeap[CLOD_RT_PT_ACCUMULATION_UAV];
        const bool still = (flags & CLOD_RT_PT_FLAG_RESTART_ACCUMULATION) == 0u
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
    motion[pixel] = hit.motionVector;

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
