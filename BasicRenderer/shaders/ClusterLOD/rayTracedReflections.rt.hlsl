#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/utilities.hlsli"
#include "PerPassRootConstants/clodRayTracingSetupRootConstants.h"

struct CLodRtPayload
{
    float3 color;
    float hitT;
    uint hit;
};

float3 CLodRtReconstructWorldPosition(uint2 pixel, uint2 dimensions, float depth, Camera camera)
{
    const float2 uv = (float2(pixel) + 0.5f) / max(float2(dimensions), 1.0f);
    const float linearZ = unprojectDepth(depth, camera.zNear, camera.zFar);
    const float2 ndc = uv * 2.0f - 1.0f;
    const float4 clipPos = float4(ndc, 1.0f, 1.0f);
    const float4 viewPosH = mul(clipPos, camera.projectionInverse);
    const float3 positionVS = viewPosH.xyz * linearZ;
    return mul(float4(positionVS, 1.0f), camera.viewInverse).xyz;
}

bool CLodRtProjectWorldToPixel(float3 positionWS, Camera camera, uint2 dimensions, out uint2 pixel)
{
    const float4 clip = mul(float4(positionWS, 1.0f), camera.viewProjection);
    if (clip.w <= 0.0f) {
        pixel = uint2(0u, 0u);
        return false;
    }

    const float2 ndc = clip.xy / clip.w;
    const float2 uv = ndc * 0.5f + 0.5f;
    if (any(uv < float2(0.0f, 0.0f)) || any(uv > float2(1.0f, 1.0f))) {
        pixel = uint2(0u, 0u);
        return false;
    }

    pixel = min(uint2(uv * float2(dimensions)), dimensions - 1u);
    return true;
}

[shader("raygeneration")]
void CLodRtReflectionsRayGen()
{
    RaytracingAccelerationStructure scene =
        ResourceDescriptorHeap[CLOD_RT_TLAS_DESCRIPTOR_INDEX];
    RWTexture2D<float4> reflectionOutput =
        ResourceDescriptorHeap[CLOD_RT_REFLECTION_OUTPUT_DESCRIPTOR_INDEX];
    Texture2D<float4> hdrColor =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Color::HDRColorTarget)];
    Texture2D<float> depthTexture =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::DepthTexture)];
    Texture2D<float4> normalsTexture =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::NormalRoughness)];
    ConstantBuffer<PerFrameBuffer> perFrameBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<Camera> cameras =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];

    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    const Camera camera = cameras[perFrameBuffer.mainCameraIndex];

    const float depth = depthTexture[pixel];
    const float4 normalSample = normalsTexture[pixel];
    const float perceptualRoughness = normalSample.w;
    const float3 normalWS = normalize(normalSample.xyz);
    const bool reflectivePixel = depth > 0.0f
        && depth < 1.0f
        && all(isfinite(normalWS))
        && dot(normalWS, normalWS) > 0.25f
        && perceptualRoughness < 0.85f;

    if (!reflectivePixel) {
        reflectionOutput[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    const float3 positionWS = CLodRtReconstructWorldPosition(pixel, dimensions, depth, camera);
    const float3 viewDirWS = normalize(camera.positionWorldSpace.xyz - positionWS);
    const float3 reflectionDirWS = normalize(reflect(-viewDirWS, normalWS));

    RayDesc ray;
    ray.Origin = positionWS + normalWS * 0.02f;
    ray.Direction = reflectionDirWS;
    ray.TMin = 0.001f;
    ray.TMax = 100000.0f;

    CLodRtPayload payload;
    payload.color = float3(0.02f, 0.02f, 0.04f);
    payload.hitT = 0.0f;
    payload.hit = 0u;

    TraceRay(scene, RAY_FLAG_NONE, 0xFFu, 0u, 1u, 0u, ray, payload);

    if (payload.hit == 0u) {
        reflectionOutput[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    const float3 hitPositionWS = ray.Origin + ray.Direction * payload.hitT;
    uint2 reprojectedPixel = uint2(0u, 0u);
    float3 reflectedColor = payload.color;
    if (CLodRtProjectWorldToPixel(hitPositionWS, camera, dimensions, reprojectedPixel)) {
        reflectedColor = hdrColor[reprojectedPixel].rgb;
    }

    const float roughnessFade = saturate((0.85f - perceptualRoughness) / 0.85f);
    reflectionOutput[pixel] = float4(reflectedColor, roughnessFade);
}

[shader("miss")]
void CLodRtReflectionsMiss(inout CLodRtPayload payload)
{
    payload.color = float3(0.0f, 0.0f, 0.0f);
    payload.hitT = 0.0f;
    payload.hit = 0u;
}

[shader("closesthit")]
void CLodRtReflectionsClosestHit(inout CLodRtPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    const float3 bary = float3(1.0f - attributes.barycentrics.x - attributes.barycentrics.y, attributes.barycentrics.x, attributes.barycentrics.y);
    payload.color = lerp(float3(0.0f, 0.85f, 1.0f), float3(1.0f, 0.1f, 0.75f), saturate(bary.y + bary.z));
    payload.hitT = RayTCurrent();
    payload.hit = 1u;
}
