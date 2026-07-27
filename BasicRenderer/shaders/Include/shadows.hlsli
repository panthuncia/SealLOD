#ifndef __SHADOWS_HLSLI__
#define __SHADOWS_HLSLI__

#include "include/cbuffers.hlsli"
#include "include/clodVirtualShadowClipmap.hlsli"
#include "include/structs.hlsli"
#include "include/utilities.hlsli"

static const uint kCLodVirtualShadowDebugFlagPreferredAllocated = 0x1u;
static const uint kCLodVirtualShadowDebugFlagPreferredDirty = 0x2u;
static const uint kCLodVirtualShadowDebugFlagSampledDepthMissing = 0x4u;
static const uint kCLodVirtualShadowDebugFlagSampledPageUnwritten = 0x8u;
static const uint kCLodVirtualShadowDebugFlagSampledTexelCleared = 0x10u;
static const uint kCLodVirtualShadowDebugFlagSampledRerenderedThisFrame = 0x20u;
static const uint kCLodVirtualShadowDebugFlagCachedPageTagMismatch = 0x40u;
static const uint kCLodVirtualShadowDebugFlagPhysicalOwnerMismatch = 0x80u;
static const uint kCLodVirtualShadowDebugFlagSyntheticEmptyCompletion = 0x100u;
static const uint kCLodVirtualShadowDebugFlagFiniteDepthLit = 0x200u;
static const uint kCLodVirtualShadowDebugFlagFiniteDepthShadowed = 0x400u;
static const float kCLodVirtualShadowPi = 3.14159265359f;
static const float kCLodVirtualShadowTwoPi = 6.28318530718f;
static const float kCLodVirtualShadowDegreesToRadians = kCLodVirtualShadowPi / 180.0f;
static const float kCLodVirtualShadowGoldenRatioConjugate = 0.618033988749895f;

struct CLodVirtualShadowDebugInfo
{
    uint preferredClipmapIndex;
    uint sampledClipmapIndex;
    uint preferredPageEntry;
    uint sampledPageEntry;
    uint sampledPhysicalPageIndex;
    uint flags;
};

struct CLodVirtualShadowLookupResult
{
    uint valid;
    uint depthAvailable;
    float occlusion;
    float closestDepth;
    float sampledLinearDepth;
    uint sampledClipmapIndex;
    uint sampledPhysicalPageIndex;
    CLodVirtualShadowClipmapInfo clipmapInfo;
};

CLodVirtualShadowDebugInfo CLodVirtualShadowInitDebugInfo(uint preferredClipmapIndex)
{
    CLodVirtualShadowDebugInfo debugInfo;
    debugInfo.preferredClipmapIndex = preferredClipmapIndex;
    debugInfo.sampledClipmapIndex = 0xFFFFFFFFu;
    debugInfo.preferredPageEntry = 0u;
    debugInfo.sampledPageEntry = 0u;
    debugInfo.sampledPhysicalPageIndex = 0xFFFFFFFFu;
    debugInfo.flags = 0u;
    return debugInfo;
}

CLodVirtualShadowLookupResult CLodVirtualShadowInitLookupResult()
{
    CLodVirtualShadowLookupResult result;
    result.valid = 0u;
    result.depthAvailable = 0u;
    result.occlusion = 0.0f;
    result.closestDepth = 0.0f;
    result.sampledLinearDepth = 0.0f;
    result.sampledClipmapIndex = 0xFFFFFFFFu;
    result.sampledPhysicalPageIndex = 0xFFFFFFFFu;
    result.clipmapInfo = (CLodVirtualShadowClipmapInfo)0;
    return result;
}

uint CLodVirtualShadowSmrtRayCountDirectional(uint packedCounts)
{
    return packedCounts & 0xFFFFu;
}

uint CLodVirtualShadowSmrtSamplesPerRayDirectional(uint packedCounts)
{
    return packedCounts >> 16u;
}

float CLodVirtualShadowRadiansFromDegrees(float degrees)
{
    return degrees * kCLodVirtualShadowDegreesToRadians;
}

float CLodVirtualShadowRadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return (float)bits * 2.3283064365386963e-10f;
}

float2 CLodVirtualShadowHammersley2D(uint sampleIndex, uint sampleCount)
{
    const float safeSampleCount = max((float)sampleCount, 1.0f);
    return float2(((float)sampleIndex + 0.5f) / safeSampleCount, CLodVirtualShadowRadicalInverseVdC(sampleIndex));
}

uint CLodVirtualShadowHash(uint2 pixelCoords, uint frameIndex)
{
    uint hash = pixelCoords.x * 0x1F123BB5u + pixelCoords.y * 0x05491333u + frameIndex * 0x9E3779B9u + 0x68BC21EBu;
    hash ^= hash >> 16u;
    hash *= 0x7FEB352Du;
    hash ^= hash >> 15u;
    hash *= 0x846CA68Bu;
    hash ^= hash >> 16u;
    return hash;
}

float2 CLodVirtualShadowSmrtRotation(uint2 pixelCoords, uint frameIndex)
{
    const uint hash0 = CLodVirtualShadowHash(pixelCoords, frameIndex);
    const uint hash1 = hash0 * 1664525u + 1013904223u;
    return float2(
        (float)(hash0 & 0x00FFFFFFu) / 16777216.0f,
        (float)(hash1 & 0x00FFFFFFu) / 16777216.0f);
}

void CLodVirtualShadowBuildOrthonormalBasis(float3 direction, out float3 tangent, out float3 bitangent)
{
    const float3 helper = abs(direction.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
    tangent = normalize(cross(helper, direction));
    bitangent = cross(direction, tangent);
}

// R2 quasi-random sequence for low-discrepancy sampling.
// Based on the plastic constant g ≈ 1.32471795724.
static const float kCLodVirtualShadowR2Alpha1 = 0.7548776662466927f; // 1/g
static const float kCLodVirtualShadowR2Alpha2 = 0.5698402909980532f; // 1/g^2

float2 CLodVirtualShadowR2Sequence(uint n)
{
    return frac(float2(0.5f + (float)n * kCLodVirtualShadowR2Alpha1,
                       0.5f + (float)n * kCLodVirtualShadowR2Alpha2));
}

float3 CLodVirtualShadowHueToRgb(float hue)
{
    const float3 offsets = float3(0.0f, 2.0f / 3.0f, 1.0f / 3.0f);
    return saturate(abs(frac(hue + offsets) * 6.0f - 3.0f) - 1.0f);
}

float3 CLodVirtualShadowDebugClipmapColor(uint clipmapIndex)
{
    if (clipmapIndex == 0xFFFFFFFFu)
    {
        return float3(1.0f, 0.0f, 1.0f);
    }

    const float hue = frac(0.08f + (float)clipmapIndex * kCLodVirtualShadowGoldenRatioConjugate);
    const float3 rgb = CLodVirtualShadowHueToRgb(hue);
    return lerp(float3(0.18f, 0.22f, 0.28f), float3(0.95f, 0.92f, 0.85f), rgb);
}

float3 CLodVirtualShadowDebugPageStateColor(CLodVirtualShadowDebugInfo debugInfo)
{
    if ((debugInfo.flags &
            kCLodVirtualShadowDebugFlagPhysicalOwnerMismatch) != 0u)
    {
        return float3(1.0f, 0.0f, 0.6f);
    }

    if ((debugInfo.flags &
            kCLodVirtualShadowDebugFlagCachedPageTagMismatch) != 0u)
    {
        return float3(1.0f, 0.25f, 0.0f);
    }

    if ((debugInfo.flags & kCLodVirtualShadowDebugFlagSampledDepthMissing) != 0u)
    {
        return float3(1.0f, 0.0f, 1.0f);
    }

    if ((debugInfo.flags & kCLodVirtualShadowDebugFlagSampledPageUnwritten) != 0u)
    {
        return float3(0.0f, 0.85f, 0.95f);
    }

    // Blue identifies a page whose validity came from the raster pipeline
    // completing an exact admitted job without any depth writes. This is
    // expected only for page-job raster and distinguishes intentional empty
    // pages from normally rendered green pages.
    if ((debugInfo.flags &
            kCLodVirtualShadowDebugFlagSyntheticEmptyCompletion) != 0u)
    {
        return float3(0.05f, 0.30f, 1.0f);
    }

    if ((debugInfo.flags & kCLodVirtualShadowDebugFlagSampledTexelCleared) != 0u)
    {
        return float3(1.0f, 1.0f, 1.0f);
    }

    if ((debugInfo.flags & kCLodVirtualShadowDebugFlagPreferredAllocated) == 0u)
    {
        return float3(0.05f, 0.10f, 0.55f);
    }

    if ((debugInfo.flags & kCLodVirtualShadowDebugFlagPreferredDirty) != 0u)
    {
        return float3(1.0f, 0.95f, 0.10f);
    }

    if (debugInfo.sampledClipmapIndex == 0xFFFFFFFFu)
    {
        return float3(0.65f, 0.0f, 0.0f);
    }

    // A valid page and a useful shadow sample are not the same state. Keep
    // green for samples that actually occlude this receiver, and use violet
    // when a finite depth value was accepted but compares as fully lit. This
    // makes cached-depth-origin or incomplete-coverage failures visible
    // instead of reporting both cases as healthy green pages.
    if ((debugInfo.flags & kCLodVirtualShadowDebugFlagFiniteDepthShadowed) != 0u)
    {
        return float3(0.10f, 0.85f, 0.20f);
    }
    if ((debugInfo.flags & kCLodVirtualShadowDebugFlagFiniteDepthLit) != 0u)
    {
        return float3(0.62f, 0.18f, 0.90f);
    }

    return float3(0.35f, 0.35f, 0.35f);
}

float3 CLodVirtualShadowDebugRerenderedThisFrameColor(CLodVirtualShadowDebugInfo debugInfo)
{
    if ((debugInfo.flags & kCLodVirtualShadowDebugFlagSampledDepthMissing) != 0u)
    {
        return float3(1.0f, 0.0f, 1.0f);
    }

    if ((debugInfo.flags & kCLodVirtualShadowDebugFlagSampledPageUnwritten) != 0u)
    {
        return float3(0.0f, 0.85f, 0.95f);
    }

    if ((debugInfo.flags & kCLodVirtualShadowDebugFlagSampledTexelCleared) != 0u)
    {
        return float3(1.0f, 1.0f, 1.0f);
    }

    if ((debugInfo.flags & kCLodVirtualShadowDebugFlagPreferredAllocated) == 0u)
    {
        return float3(0.05f, 0.10f, 0.55f);
    }

    if (debugInfo.sampledClipmapIndex == 0xFFFFFFFFu)
    {
        return float3(0.65f, 0.0f, 0.0f);
    }

    if ((debugInfo.flags & kCLodVirtualShadowDebugFlagSampledRerenderedThisFrame) != 0u)
    {
        return float3(1.0f, 0.45f, 0.05f);
    }

    return float3(0.08f, 0.08f, 0.08f);
}

float2 CLodVirtualShadowDirectionalNormalOffsetUv(
    float3 fragPosWorldSpace,
    float3 normal,
    float3 lightToFrag,
    CLodVirtualShadowClipmapInfo clipmapInfo,
    CLodVirtualShadowCompactShadowCameraInfo lightCamera)
{
    const float3 surfaceNormal = -normalize(normal);
    const float3 normalizedLightToFrag = normalize(lightToFrag);
    const float normalDotLight = saturate(dot(surfaceNormal, normalizedLightToFrag));
    const float angleScale = 1.0f - normalDotLight;

    const float normalOffsetWorld = clipmapInfo.texelWorldSize * (15.0f + 0.1f * angleScale);
    const float3 offsetWorldPosition = fragPosWorldSpace - surfaceNormal * normalOffsetWorld;

    const float4 offsetLightSpace = mul(float4(offsetWorldPosition, 1.0f), lightCamera.viewProjection);
    const float safeOffsetW = max(abs(offsetLightSpace.w), 1.0e-6f);
    float2 offsetUv = offsetLightSpace.xy / safeOffsetW;
    offsetUv = offsetUv * 0.5f + 0.5f;
    offsetUv.y = 1.0f - offsetUv.y;

    const float4 baseLightSpace = mul(float4(fragPosWorldSpace, 1.0f), lightCamera.viewProjection);
    const float safeBaseW = max(abs(baseLightSpace.w), 1.0e-6f);
    float2 baseUv = baseLightSpace.xy / safeBaseW;
    baseUv = baseUv * 0.5f + 0.5f;
    baseUv.y = 1.0f - baseUv.y;

    return offsetUv - baseUv;
}

float CLodVirtualShadowReceiverPlaneDepthBias(
    float3 normal,
    CLodVirtualShadowCompactShadowCameraInfo lightCamera,
    float2 ditherWorld)
{
    const float3 normalLightSpace = normalize(mul(float4(normalize(normal), 0.0f), lightCamera.view).xyz);
    const float safeNormalZ = abs(normalLightSpace.z) > 1.0e-4f ? normalLightSpace.z : 0.0f;
    if (safeNormalZ == 0.0f)
    {
        return 0.0f;
    }

    // Positive UV Y maps to negative light-space Y because the shadow UV is flipped.
    const float2 planeSlope = clamp(normalLightSpace.xy / safeNormalZ, -8.0f, 8.0f);
    const float2 ditherLightSpace = float2(ditherWorld.x, -ditherWorld.y);
    return dot(ditherLightSpace, planeSlope);
}

void CLodVirtualShadowProjectWorldToUvDepth(
    float3 samplePosWorldSpace,
    CLodVirtualShadowCompactShadowCameraInfo lightCamera,
    out float2 uv,
    out float linearLightDepth)
{
    const float4 samplePosLightSpace = mul(float4(samplePosWorldSpace, 1.0f), lightCamera.viewProjection);
    const float safeW = max(abs(samplePosLightSpace.w), 1.0e-6f);
    uv = samplePosLightSpace.xy / safeW;
    uv = uv * 0.5f + 0.5f;
    uv.y = 1.0f - uv.y;

    const float4 samplePosLightView = mul(float4(samplePosWorldSpace, 1.0f), lightCamera.view);
    linearLightDepth = -samplePosLightView.z;
}

CLodVirtualShadowLookupResult CLodVirtualShadowLookupDirectionalOcclusionProjected(
    float3 samplePosWorldSpace,
    float3 normal,
    uint preferredClipmapIndex,
    float2 preferredUv,
    float preferredLinearLightDepth,
    float2 ditherWorld,
    uint activeClipmapCount,
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos,
    StructuredBuffer<CLodVirtualShadowCompactShadowCameraInfo> compactShadowCameraBuffer,
    StructuredBuffer<float4> directionalPageViewInfo,
    Texture2DArray<uint> pageTable,
    Texture2D<uint> physicalPages,
    out CLodVirtualShadowDebugInfo debugInfo)
{
    CLodVirtualShadowLookupResult result = CLodVirtualShadowInitLookupResult();
    debugInfo = CLodVirtualShadowInitDebugInfo(preferredClipmapIndex);
    (void)preferredLinearLightDepth;

    [loop]
    for (uint attempt = 0u; attempt < 3u; ++attempt)
    {
        const uint candidateIndex = preferredClipmapIndex + attempt;
        if (candidateIndex >= activeClipmapCount)
            break;

        const CLodVirtualShadowClipmapInfo clipmapInfo = clipmapInfos[candidateIndex];
        if (!CLodVirtualShadowClipmapIsValid(clipmapInfo))
            continue;

        const CLodVirtualShadowCompactShadowCameraInfo lightCamera = compactShadowCameraBuffer[candidateIndex];
        float2 uv = preferredUv;
        if (attempt != 0u)
        {
            float unusedLinearDepth;
            CLodVirtualShadowProjectWorldToUvDepth(samplePosWorldSpace, lightCamera, uv, unusedLinearDepth);
        }

        const float2 uvDitherFromWorld = ditherWorld / max(clipmapInfo.texelWorldSize * (float)clipmapInfo.virtualResolution, 1.0e-6f);
        uv += uvDitherFromWorld;

        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
            continue;

        const uint2 virtualPageCoords = CLodVirtualShadowVirtualPageCoordsFromUv(uv.xy, clipmapInfo);
        const uint2 wrappedPageCoords = CLodVirtualShadowWrappedPageCoords(virtualPageCoords, clipmapInfo);
        const uint pageEntry = pageTable.Load(int4(wrappedPageCoords, clipmapInfo.pageTableLayer, 0));

        if (attempt == 0u)
        {
            debugInfo.preferredPageEntry = pageEntry;
            if ((pageEntry & kCLodVirtualShadowAllocatedMask) != 0u)
                debugInfo.flags |= kCLodVirtualShadowDebugFlagPreferredAllocated;
            if ((pageEntry & kCLodVirtualShadowDirtyMask) != 0u)
                debugInfo.flags |= kCLodVirtualShadowDebugFlagPreferredDirty;
        }

        if ((pageEntry & kCLodVirtualShadowAllocatedMask) == 0u)
            continue;
        if ((pageEntry & kCLodVirtualShadowContentValidMask) == 0u)
        {
            if (attempt == 0u)
                debugInfo.flags |= kCLodVirtualShadowDebugFlagSampledPageUnwritten;
            continue;
        }
        const uint physicalPageIndex = pageEntry & kCLodVirtualShadowPhysicalPageIndexMask;
        StructuredBuffer<uint4> pageMetadata =
            ResourceDescriptorHeap[ResourceDescriptorIndex(
                Builtin::Shadows::CLodPageMetadata)];
        const uint4 physicalMeta = pageMetadata[physicalPageIndex];
        const uint expectedVirtualAddress =
            wrappedPageCoords.y * clipmapInfo.pageTableResolution +
            wrappedPageCoords.x;
        if ((physicalMeta.z &
                kCLodVirtualShadowPhysicalPageResidentFlag) == 0u ||
            physicalMeta.x != expectedVirtualAddress ||
            physicalMeta.w != clipmapInfo.pageTableLayer)
        {
            if (attempt == 0u)
                debugInfo.flags |=
                    kCLodVirtualShadowDebugFlagPhysicalOwnerMismatch;
            continue;
        }
        if ((physicalMeta.y &
                kCLodVirtualShadowClearEpochPendingMask) != 0u)
        {
            debugInfo.flags |=
                kCLodVirtualShadowDebugFlagSyntheticEmptyCompletion;
        }
        debugInfo.sampledClipmapIndex = candidateIndex;
        debugInfo.sampledPageEntry = pageEntry;
        debugInfo.sampledPhysicalPageIndex = physicalPageIndex;
        if ((pageEntry & kCLodVirtualShadowRerenderedThisFrameMask) != 0u)
            debugInfo.flags |= kCLodVirtualShadowDebugFlagSampledRerenderedThisFrame;

        row_major matrix cachedPageView = lightCamera.view;
        const float4 cachedPageViewRow =
            directionalPageViewInfo[physicalPageIndex];
        const uint expectedPageTag = CLodVirtualShadowPackAbsolutePageTag(
            virtualPageCoords,
            clipmapInfo.unwrappedPageOffsetX,
            clipmapInfo.unwrappedPageOffsetY);
        if (asuint(cachedPageViewRow.w) != expectedPageTag)
        {
            if (attempt == 0u)
                debugInfo.flags |=
                    kCLodVirtualShadowDebugFlagCachedPageTagMismatch;
            continue;
        }
        // Only the cached depth origin is required here.  Projected page
        // selection already used the current clipmap transform, and X/Y view
        // translation cannot contribute to view-space Z.
        cachedPageView[3][2] = cachedPageViewRow.z;
        cachedPageView[3][3] = 1.0f;
        const float4 samplePosCachedPageLightView = mul(float4(samplePosWorldSpace, 1.0f), cachedPageView);
        const float linearLightDepth =
            -samplePosCachedPageLightView.z +
            CLodVirtualShadowReceiverPlaneDepthBias(normal, lightCamera, ditherWorld);
        if (linearLightDepth <= 0.0f)
            continue;

        const uint2 virtualTexelCoords = CLodVirtualShadowVirtualTexelCoordsFromUv(uv.xy, clipmapInfo);
        const uint2 atlasPixel = CLodVirtualShadowPhysicalAtlasPixel(physicalPageIndex, virtualTexelCoords, clipmapInfo);
        const uint storedDepthBits = physicalPages.Load(int3(atlasPixel, 0));

        result.clipmapInfo = clipmapInfo;
        result.valid = 1u;
        result.sampledLinearDepth = linearLightDepth;
        result.sampledClipmapIndex = candidateIndex;
        result.sampledPhysicalPageIndex = physicalPageIndex;
        if (storedDepthBits == 0x7F7FFFFFu)
        {
            // This is the texel actually selected by lookup, even when the
            // preferred clipmap fell back to a coarser one. Restricting this
            // diagnostic to attempt zero made clear coarse-page texels appear
            // healthy green.
            debugInfo.flags |=
                kCLodVirtualShadowDebugFlagSampledTexelCleared;
            result.depthAvailable = 0u;
            result.occlusion = 0.0f;
            result.closestDepth = asfloat(storedDepthBits);
            return result;
        }
        if (storedDepthBits == 0xFFFFFFFFu)
        {
            if (attempt == 0u)
                debugInfo.flags |= kCLodVirtualShadowDebugFlagSampledDepthMissing;
            result.valid = 0u;
            continue;
        }

        const float closestDepth = asfloat(storedDepthBits);
        const float depthDelta = linearLightDepth - closestDepth;
        result.depthAvailable = 1u;
        result.closestDepth = closestDepth;
        result.occlusion = smoothstep(0.0f, clipmapInfo.texelWorldSize * 0.5f, depthDelta);
        debugInfo.flags |= result.occlusion > 0.01f
            ? kCLodVirtualShadowDebugFlagFiniteDepthShadowed
            : kCLodVirtualShadowDebugFlagFiniteDepthLit;
        return result;
    }

    return result;
}

float calculatePointShadow(float3 fragPosWorldSpace, float3 normal, LightInfo light, StructuredBuffer<unsigned int> pointShadowCameraIndexBuffer, StructuredBuffer<Camera> cameraBuffer) {
    float3 lightToFrag = fragPosWorldSpace.xyz - light.posWorldSpace.xyz;
    lightToFrag.z = -lightToFrag.z;
    float3 worldDir = normalize(lightToFrag);

    TextureCube<float> shadowMap = ResourceDescriptorHeap[light.shadowMapIndex];
    SamplerState shadowSampler = SamplerDescriptorHeap[light.shadowSamplerIndex];
    float depthSample = shadowMap.SampleLevel(shadowSampler, worldDir, 0);
    //depthSample = unprojectDepth(depthSample, light.nearPlane, light.farPlane);
    if (depthSample == 1.0) {
        return 0.0;
    }
    
    int faceIndex = 0;
    float maxDir = max(max(abs(worldDir.x), abs(worldDir.y)), abs(worldDir.z));

    if (worldDir.x == maxDir) {
        faceIndex = 0; // +X
    }
    else if (worldDir.x == -maxDir) {
        faceIndex = 1; // -X
    }
    else if (worldDir.y == maxDir) {
        faceIndex = 2; // +Y
    }
    else if (worldDir.y == -maxDir) {
        faceIndex = 3; // -Y
    }
    else if (worldDir.z == maxDir) {
        faceIndex = 4; // +Z
    }
    else if (worldDir.z == -maxDir) {
        faceIndex = 5; // -Z
    }
    
    uint cameraIndex = pointShadowCameraIndexBuffer[light.shadowViewInfoIndex * 6 + faceIndex];
    Camera lightCamera = cameraBuffer[cameraIndex];
    float closestDepth = unprojectDepth(depthSample, light.nearPlane, light.farPlane);

    
    float4 fragPosLightProjection = mul(float4(fragPosWorldSpace.xyz, 1.0), lightCamera.viewProjection);
    //float dist = length(lightToFrag);
    float lightSpaceDepth = fragPosLightProjection.z;
    
    float shadow = 0.0;
    float bias = max(0.0005, 0.02 * (1.0 - dot(normal, worldDir.xyz)));
    shadow = lightSpaceDepth - bias > closestDepth ? 1.0 : 0.0;
    return shadow;
}

int calculateShadowCascadeIndex(float depth, uint numCascadeSplits, float4 cascadeSplits) {
    for (int i = 0; i < numCascadeSplits; i++) {
        if (depth < cascadeSplits[i]) {
            return i;
        }
    }
    return numCascadeSplits - 1;
}

float calculateDirectionalVSMShadowDetailed(float2 pixelCoords, float3 fragPosWorldSpace, float3 fragPosViewSpace, float3 normal, LightInfo light, uint numDirectionalClipmaps, float4 cascadeSplits, StructuredBuffer<unsigned int> cascadeCameraIndexBuffer, StructuredBuffer<Camera> cameraBuffer, out CLodVirtualShadowDebugInfo debugInfo);

float calculateDirectionalVSMShadowDetailed(float3 fragPosWorldSpace, float3 fragPosViewSpace, float3 normal, LightInfo light, uint numDirectionalClipmaps, float4 cascadeSplits, StructuredBuffer<unsigned int> cascadeCameraIndexBuffer, StructuredBuffer<Camera> cameraBuffer, out CLodVirtualShadowDebugInfo debugInfo) {
    return calculateDirectionalVSMShadowDetailed(
        float2(0.0f, 0.0f),
        fragPosWorldSpace,
        fragPosViewSpace,
        normal,
        light,
        numDirectionalClipmaps,
        cascadeSplits,
        cascadeCameraIndexBuffer,
        cameraBuffer,
        debugInfo);
}

CLodVirtualShadowLookupResult CLodVirtualShadowLookupDirectionalOcclusion(
    float3 samplePosWorldSpace,
    float3 normal,
    float3 lightToFrag,
    uint activeClipmapCount,
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos,
    StructuredBuffer<CLodVirtualShadowMainCameraInfo> compactMainCameraBuffer,
    StructuredBuffer<CLodVirtualShadowCompactShadowCameraInfo> compactShadowCameraBuffer,
    StructuredBuffer<float4> directionalPageViewInfo,
    Texture2DArray<uint> pageTable,
    Texture2D<uint> physicalPages,
    uint applyReceiverBias,
    float2 ditherWorld,
    out CLodVirtualShadowDebugInfo debugInfo)
{
    CLodVirtualShadowLookupResult result = CLodVirtualShadowInitLookupResult();
    if (activeClipmapCount == 0u)
    {
        debugInfo = CLodVirtualShadowInitDebugInfo(0u);
        return result;
    }

    const CLodVirtualShadowMainCameraInfo mainCamera = compactMainCameraBuffer[0];
    const uint preferredClipmapIndex = CLodVirtualShadowSelectClipmapIndex(
        samplePosWorldSpace,
        mainCamera.positionWorldSpace.xyz,
        clipmapInfos[0].texelWorldSize,
        clipmapInfos[0].directionalLodBias,
        activeClipmapCount);
    debugInfo = CLodVirtualShadowInitDebugInfo(preferredClipmapIndex);

    result.clipmapInfo = clipmapInfos[preferredClipmapIndex];

    // Try preferred level + up to 2 coarser fallback levels
    [loop]
    for (uint attempt = 0u; attempt < 3u; ++attempt)
    {
        const uint candidateIndex = preferredClipmapIndex + attempt;
        if (candidateIndex >= activeClipmapCount)
            break;

        const CLodVirtualShadowClipmapInfo clipmapInfo = clipmapInfos[candidateIndex];
        if (!CLodVirtualShadowClipmapIsValid(clipmapInfo))
            continue;

        const CLodVirtualShadowCompactShadowCameraInfo lightCamera = compactShadowCameraBuffer[candidateIndex];
        const float4 samplePosLightSpace = mul(float4(samplePosWorldSpace, 1.0f), lightCamera.viewProjection);
        const float safeW = max(abs(samplePosLightSpace.w), 1.0e-6f);
        float3 uv = samplePosLightSpace.xyz / safeW;
        uv.xy = uv.xy * 0.5f + 0.5f;
        uv.y = 1.0f - uv.y;

        if (attempt == 0u && applyReceiverBias != 0u)
        {
            const float2 biasedUv = uv.xy + CLodVirtualShadowDirectionalNormalOffsetUv(
                samplePosWorldSpace,
                normal,
                lightToFrag,
                clipmapInfo,
                lightCamera);
            if (biasedUv.x < 0.0f || biasedUv.x > 1.0f || biasedUv.y < 0.0f || biasedUv.y > 1.0f)
                continue;
            uv.xy = biasedUv;
        }

        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f || uv.z < 0.0f || uv.z > 1.0f)
            continue;

        const float2 uvDitherFromWorld = ditherWorld / max(clipmapInfo.texelWorldSize * (float)clipmapInfo.virtualResolution, 1.0e-6f);
        uv.xy += uvDitherFromWorld;

        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
            continue;

        const uint2 virtualPageCoords = CLodVirtualShadowVirtualPageCoordsFromUv(uv.xy, clipmapInfo);
        const uint2 wrappedPageCoords = CLodVirtualShadowWrappedPageCoords(virtualPageCoords, clipmapInfo);
        const uint pageEntry = pageTable.Load(int4(wrappedPageCoords, clipmapInfo.pageTableLayer, 0));

        if (attempt == 0u)
        {
            debugInfo.preferredPageEntry = pageEntry;
            if ((pageEntry & kCLodVirtualShadowAllocatedMask) != 0u)
                debugInfo.flags |= kCLodVirtualShadowDebugFlagPreferredAllocated;
            if ((pageEntry & kCLodVirtualShadowDirtyMask) != 0u)
                debugInfo.flags |= kCLodVirtualShadowDebugFlagPreferredDirty;
        }

        if ((pageEntry & kCLodVirtualShadowAllocatedMask) == 0u)
            continue;
        if ((pageEntry & kCLodVirtualShadowContentValidMask) == 0u)
        {
            if (attempt == 0u)
                debugInfo.flags |= kCLodVirtualShadowDebugFlagSampledPageUnwritten;
            continue;
        }
        const uint physicalPageIndex = pageEntry & kCLodVirtualShadowPhysicalPageIndexMask;
        StructuredBuffer<uint4> pageMetadata =
            ResourceDescriptorHeap[ResourceDescriptorIndex(
                Builtin::Shadows::CLodPageMetadata)];
        const uint4 physicalMeta = pageMetadata[physicalPageIndex];
        const uint expectedVirtualAddress =
            wrappedPageCoords.y * clipmapInfo.pageTableResolution +
            wrappedPageCoords.x;
        if ((physicalMeta.z &
                kCLodVirtualShadowPhysicalPageResidentFlag) == 0u ||
            physicalMeta.x != expectedVirtualAddress ||
            physicalMeta.w != clipmapInfo.pageTableLayer)
        {
            if (attempt == 0u)
                debugInfo.flags |=
                    kCLodVirtualShadowDebugFlagPhysicalOwnerMismatch;
            continue;
        }
        if ((physicalMeta.y &
                kCLodVirtualShadowClearEpochPendingMask) != 0u)
        {
            debugInfo.flags |=
                kCLodVirtualShadowDebugFlagSyntheticEmptyCompletion;
        }
        debugInfo.sampledClipmapIndex = candidateIndex;
        debugInfo.sampledPageEntry = pageEntry;
        debugInfo.sampledPhysicalPageIndex = physicalPageIndex;
        if ((pageEntry & kCLodVirtualShadowRerenderedThisFrameMask) != 0u)
            debugInfo.flags |= kCLodVirtualShadowDebugFlagSampledRerenderedThisFrame;

        row_major matrix cachedPageView = lightCamera.view;
        const float4 cachedPageViewRow =
            directionalPageViewInfo[physicalPageIndex];
        const uint expectedPageTag = CLodVirtualShadowPackAbsolutePageTag(
            virtualPageCoords,
            clipmapInfo.unwrappedPageOffsetX,
            clipmapInfo.unwrappedPageOffsetY);
        if (asuint(cachedPageViewRow.w) != expectedPageTag)
        {
            if (attempt == 0u)
                debugInfo.flags |=
                    kCLodVirtualShadowDebugFlagCachedPageTagMismatch;
            continue;
        }
        // Only the cached depth origin is required here.  Projected page
        // selection already used the current clipmap transform, and X/Y view
        // translation cannot contribute to view-space Z.
        cachedPageView[3][2] = cachedPageViewRow.z;
        cachedPageView[3][3] = 1.0f;
        const float4 samplePosCachedPageLightView = mul(float4(samplePosWorldSpace, 1.0f), cachedPageView);
        const float linearLightDepth =
            -samplePosCachedPageLightView.z +
            CLodVirtualShadowReceiverPlaneDepthBias(normal, lightCamera, ditherWorld);
        if (linearLightDepth <= 0.0f)
            continue;

        const uint2 virtualTexelCoords = CLodVirtualShadowVirtualTexelCoordsFromUv(uv.xy, clipmapInfo);
        const uint2 atlasPixel = CLodVirtualShadowPhysicalAtlasPixel(physicalPageIndex, virtualTexelCoords, clipmapInfo);

        const uint storedDepthBits = physicalPages.Load(int3(atlasPixel, 0));
        if (storedDepthBits == 0x7F7FFFFFu)
        {
            // Report the state of the page that was actually sampled. Coarse
            // fallback samples are just as authoritative as attempt zero.
            debugInfo.flags |=
                kCLodVirtualShadowDebugFlagSampledTexelCleared;
            result.clipmapInfo = clipmapInfo;
            result.valid = 1u;
            result.depthAvailable = 0u;
            result.occlusion = 0.0f;
            result.closestDepth = asfloat(storedDepthBits);
            result.sampledClipmapIndex = candidateIndex;
            result.sampledPhysicalPageIndex = physicalPageIndex;
            return result;
        }
        if (storedDepthBits == 0xFFFFFFFFu)
        {
            if (attempt == 0u)
                debugInfo.flags |= kCLodVirtualShadowDebugFlagSampledDepthMissing;
            continue;
        }

        const float closestDepth = asfloat(storedDepthBits);
        result.clipmapInfo = clipmapInfo;
        result.valid = 1u;
        result.depthAvailable = 1u;
        result.closestDepth = closestDepth;
        result.sampledClipmapIndex = candidateIndex;
        result.sampledPhysicalPageIndex = physicalPageIndex;
        const float depthDelta = linearLightDepth - closestDepth;
        result.occlusion = smoothstep(0.0f, clipmapInfo.texelWorldSize * 0.5f, depthDelta);
        debugInfo.flags |= result.occlusion > 0.01f
            ? kCLodVirtualShadowDebugFlagFiniteDepthShadowed
            : kCLodVirtualShadowDebugFlagFiniteDepthLit;
        return result;
    }

    return result;
}

float calculateDirectionalVSMShadowDetailed(float2 pixelCoords, float3 fragPosWorldSpace, float3 fragPosViewSpace, float3 normal, LightInfo light, uint numDirectionalClipmaps, float4 cascadeSplits, StructuredBuffer<unsigned int> cascadeCameraIndexBuffer, StructuredBuffer<Camera> cameraBuffer, out CLodVirtualShadowDebugInfo debugInfo) {
    (void)fragPosViewSpace;
    (void)cascadeCameraIndexBuffer;
    (void)cameraBuffer;
    (void)cascadeSplits;

    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[0];
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Shadows::CLodClipmapInfo)];
    StructuredBuffer<CLodVirtualShadowMainCameraInfo> compactMainCameraBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Shadows::CLodCompactMainCamera)];
    StructuredBuffer<CLodVirtualShadowCompactShadowCameraInfo> compactShadowCameraBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Shadows::CLodCompactShadowCameras)];
    StructuredBuffer<float4> directionalPageViewInfo = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Shadows::CLodDirectionalPageViewInfo)];
    Texture2DArray<uint> pageTable = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Shadows::CLodPageTable)];
    Texture2D<uint> physicalPages = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Shadows::CLodPhysicalPages)];

    const uint activeClipmapCount = min(numDirectionalClipmaps, kCLodVirtualShadowClipmapCount);
    const float3 lightToFrag = -light.dirWorldSpace.xyz;
    const CLodVirtualShadowLookupResult receiverLookup = CLodVirtualShadowLookupDirectionalOcclusion(
        fragPosWorldSpace,
        normal,
        lightToFrag,
        activeClipmapCount,
        clipmapInfos,
        compactMainCameraBuffer,
        compactShadowCameraBuffer,
        directionalPageViewInfo,
        pageTable,
        physicalPages,
        1u,
        float2(0.0f, 0.0f),
        debugInfo);
    const float hardShadow = receiverLookup.occlusion;

    const uint packedCounts = perFrameBuffer.shadowVirtualSmrtDirectionalCountsPacked;
    const uint rayCount = CLodVirtualShadowSmrtRayCountDirectional(packedCounts);
    const uint samplesPerRay = CLodVirtualShadowSmrtSamplesPerRayDirectional(packedCounts);
    if (rayCount == 0u || samplesPerRay == 0u || light.shadowSourceAngleDegrees <= 0.0f || receiverLookup.valid == 0u)
    {
        return hardShadow;
    }

    Texture2D<float4> blueNoiseTex = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Noise::BlueNoise2D)];
    uint2 blueNoiseSize;
    blueNoiseTex.GetDimensions(blueNoiseSize.x, blueNoiseSize.y);
    const uint2 pixelCoordsInt = uint2(pixelCoords);

    const float clampedRayAngleDegrees = min(
        light.shadowSourceAngleDegrees,
        perFrameBuffer.shadowVirtualSmrtMaxRayAngleFromLightDegrees);
    const float coneAngleRadians = CLodVirtualShadowRadiansFromDegrees(clampedRayAngleDegrees);
    if (coneAngleRadians <= 0.0f)
    {
        return hardShadow;
    }

    const float maxTraceDistance =
        perFrameBuffer.shadowVirtualSmrtMaxTraceDistanceWorld *
        perFrameBuffer.shadowVirtualSmrtRayLengthScaleDirectional;
    if (maxTraceDistance <= 1.0e-5f)
    {
        return hardShadow;
    }

    const float tNear = max(receiverLookup.clipmapInfo.texelWorldSize * 4.0f, 1.0e-4f);
    const float tFar = maxTraceDistance;
    const float logRatio = log(max(tFar / tNear, 1.0f));

    const float3 baseFragToLight = normalize(lightToFrag);
    float3 tangent;
    float3 bitangent;
    CLodVirtualShadowBuildOrthonormalBasis(baseFragToLight, tangent, bitangent);

    const uint receiverClipmapIndex =
        debugInfo.sampledClipmapIndex != 0xFFFFFFFFu ?
        debugInfo.sampledClipmapIndex :
        debugInfo.preferredClipmapIndex;
    const CLodVirtualShadowClipmapInfo receiverClipmapInfo = clipmapInfos[receiverClipmapIndex];
    const CLodVirtualShadowCompactShadowCameraInfo receiverLightCamera = compactShadowCameraBuffer[receiverClipmapIndex];
    float2 rayStartUv;
    float rayStartLinearDepth;
    CLodVirtualShadowProjectWorldToUvDepth(fragPosWorldSpace, receiverLightCamera, rayStartUv, rayStartLinearDepth);

    const float2 blueNoiseBase = blueNoiseTex.Load(int3(pixelCoordsInt % blueNoiseSize, 0)).xy;
    const float2 rotation = frac(blueNoiseBase + CLodVirtualShadowSmrtRotation(pixelCoordsInt, perFrameBuffer.frameIndex));
    const float lightDiskTan = tan(coneAngleRadians);
    const float invSamplesPerRay = 1.0f / max((float)samplesPerRay, 1.0f);
    const float texelDitherScale = 0.5f;

    float visibleRayCount = 0.0f;
    float validRayCount = 0.0f;
    bool allRaysBlockedSoFar = true;
    [loop]
    for (uint rayIndex = 0u; rayIndex < rayCount; ++rayIndex)
    {
        const int2 rayBnOffset = int2(CLodVirtualShadowR2Sequence(rayIndex + receiverClipmapIndex * 17u + 1u) * float2(blueNoiseSize));
        const float2 rayBn = blueNoiseTex.Load(int3((pixelCoordsInt + rayBnOffset) % blueNoiseSize, 0)).xy;
        float2 xi = CLodVirtualShadowHammersley2D(rayIndex, rayCount);
        xi = frac(xi + rotation + rayBn);
        const float diskRadius = sqrt(xi.x);
        const float diskAngle = kCLodVirtualShadowTwoPi * xi.y;
        const float2 diskSample = diskRadius * float2(cos(diskAngle), sin(diskAngle));

        const float rayJitter = frac(rotation.x + (float)rayIndex * 0.618033988749895f);
        const int2 ditherBnOffset = int2(CLodVirtualShadowR2Sequence(rayCount + rayIndex + receiverClipmapIndex * 29u + 3u) * float2(blueNoiseSize));
        const float2 rayDitherBn = blueNoiseTex.Load(int3((pixelCoordsInt + ditherBnOffset) % blueNoiseSize, 0)).xy;
        const float2 rayDitherWorld = texelDitherScale * (rayDitherBn - 0.5f) * receiverClipmapInfo.texelWorldSize;
        const float3 rayEndWorldSpace = fragPosWorldSpace + baseFragToLight * tFar +
            tangent * (diskSample.x * lightDiskTan * tFar) +
            bitangent * (diskSample.y * lightDiskTan * tFar);
        float2 rayEndUv;
        float rayEndLinearDepth;
        CLodVirtualShadowProjectWorldToUvDepth(rayEndWorldSpace, receiverLightCamera, rayEndUv, rayEndLinearDepth);

        bool rayHit = false;
        bool rayHadValidSample = false;
        bool depthHistoryValid = false;
        float depthHistory = 0.0f;
        float depthSlope = 0.0f;
        float depthHistoryDistance = 0.0f;
        float prevSampledLinearDepth = 0.0f;
        uint prevSampledClipmapIndex = 0xFFFFFFFFu;
        uint prevSampledPhysicalPageIndex = 0xFFFFFFFFu;
        bool mustResetHistory = false;
        [loop]
        for (uint sampleIndex = 0u; sampleIndex < samplesPerRay; ++sampleIndex)
        {
            const float t = ((float)sampleIndex + 0.5f + (rayJitter - 0.5f)) * invSamplesPerRay;
            const float sampleDistance = tNear * exp(saturate(t) * logRatio);
            const float rayAlpha = saturate(sampleDistance / tFar);
            const float3 samplePosWorldSpace = lerp(fragPosWorldSpace, rayEndWorldSpace, rayAlpha);
            const float2 sampleUv = lerp(rayStartUv, rayEndUv, rayAlpha);
            const float sampleLinearDepth = lerp(rayStartLinearDepth, rayEndLinearDepth, rayAlpha);
            CLodVirtualShadowDebugInfo unusedDebugInfo;
            const CLodVirtualShadowLookupResult raySample = CLodVirtualShadowLookupDirectionalOcclusionProjected(
                samplePosWorldSpace,
                normal,
                receiverClipmapIndex,
                sampleUv,
                sampleLinearDepth,
                rayDitherWorld,
                activeClipmapCount,
                clipmapInfos,
                compactShadowCameraBuffer,
                directionalPageViewInfo,
                pageTable,
                physicalPages,
                unusedDebugInfo);

            if (raySample.valid == 0u || raySample.depthAvailable == 0u)
            {
                mustResetHistory = true;
                continue;
            }

            rayHadValidSample = true;

            const bool pageChanged =
                mustResetHistory ||
                raySample.sampledClipmapIndex != prevSampledClipmapIndex ||
                raySample.sampledPhysicalPageIndex != prevSampledPhysicalPageIndex;

            prevSampledClipmapIndex = raySample.sampledClipmapIndex;
            prevSampledPhysicalPageIndex = raySample.sampledPhysicalPageIndex;
            mustResetHistory = false;

            if (pageChanged)
            {
                depthHistoryValid = false;
                depthSlope = 0.0f;
            }

            const float closestDepth = raySample.closestDepth;
            const float refDepth = raySample.sampledLinearDepth;

            if (!depthHistoryValid)
            {
                // First valid sample in this page-local frame: simple depth test.
                depthHistory = closestDepth;
                depthHistoryDistance = sampleDistance;
                depthHistoryValid = true;
                prevSampledLinearDepth = refDepth;

                const float depthTolerance = max(
                    raySample.clipmapInfo.texelWorldSize * 0.15f,
                    abs(refDepth) * 1.0e-4f);
                if (refDepth > closestDepth + depthTolerance)
                {
                    rayHit = true;
                    break;
                }

                continue;
            }

            const float stepDist = max(sampleDistance - depthHistoryDistance, 1.0e-4f);

            // Is the shadow map showing a surface far behind the ray?
            // This happens when the texel shows the ground past the occluder.
            const float behindTolerance = max(
                abs(refDepth - prevSampledLinearDepth) * 1.05f,
                raySample.clipmapInfo.texelWorldSize * 0.1f);
            const bool bBehind = (closestDepth - refDepth) > behindTolerance;

            float depthForComparison;
            if (bBehind)
            {
                // Shadow map doesn't show the tracked surface: extrapolate
                depthForComparison = depthHistory + depthSlope * stepDist;
            }
            else
            {
                // Shadow map shows a surface near the ray: use it directly
                depthForComparison = closestDepth;
                if (abs(closestDepth - depthHistory) > 1.0e-6f)
                {
                    depthSlope = clamp(
                        (closestDepth - depthHistory) / stepDist,
                        -4.0f, 4.0f);
                }
                depthHistory = closestDepth;
                depthHistoryDistance = sampleDistance;
            }

            // For receiver-to-light march: hit when ray is behind
            // the tracked/extrapolated surface
            const float hitTolerance = max(
                raySample.clipmapInfo.texelWorldSize * 0.15f,
                abs(refDepth) * 1.0e-4f);
            if (refDepth - depthForComparison > hitTolerance)
            {
                rayHit = true;
                break;
            }

            prevSampledLinearDepth = refDepth;
        }

        if (rayHadValidSample)
        {
            validRayCount += 1.0f;
            visibleRayCount += rayHit ? 0.0f : 1.0f;
            if (!rayHit)
                allRaysBlockedSoFar = false;
        }

        // Wave early-out: all lanes fully lit after center ray
        if (rayIndex == 0u && rayHadValidSample && WaveActiveAllTrue(!rayHit))
        {
            break;
        }
        // Wave early-out: all lanes in full umbra
        if (rayIndex >= 3u && (rayIndex & 3u) == 3u &&
            WaveActiveAllTrue(allRaysBlockedSoFar && validRayCount > 0.0f))
        {
            break;
        }
    }

    if (validRayCount <= 0.0f)
    {
        return hardShadow;
    }

    float shadow = 1.0f - (visibleRayCount / validRayCount);

    // Umbra-safe dither to break banding
    const float ditherNoise = (rotation.x - 0.5f) * 0.06f;
    if (shadow > 0.015f && shadow < 0.985f)
    {
        shadow = saturate(shadow + ditherNoise);
    }

    return shadow;
}

float calculateDirectionalVSMShadow(float2 pixelCoords, float3 fragPosWorldSpace, float3 fragPosViewSpace, float3 normal, LightInfo light, uint numDirectionalClipmaps, float4 cascadeSplits, StructuredBuffer<unsigned int> cascadeCameraIndexBuffer, StructuredBuffer<Camera> cameraBuffer) {
    CLodVirtualShadowDebugInfo debugInfo;
    return calculateDirectionalVSMShadowDetailed(pixelCoords, fragPosWorldSpace, fragPosViewSpace, normal, light, numDirectionalClipmaps, cascadeSplits, cascadeCameraIndexBuffer, cameraBuffer, debugInfo);
}


float calculateCascadedShadow(float2 pixelCoords, float3 fragPosWorldSpace, float3 fragPosViewSpace, float3 normal, LightInfo light, uint numDirectionalClipmaps, float4 cascadeSplits, StructuredBuffer<unsigned int> cascadeCameraIndexBuffer, StructuredBuffer<Camera> cameraBuffer) {
    return calculateDirectionalVSMShadow(pixelCoords, fragPosWorldSpace, fragPosViewSpace, normal, light, numDirectionalClipmaps, cascadeSplits, cascadeCameraIndexBuffer, cameraBuffer);
}

float calculateSpotShadow(float3 fragPosWorldSpace, float3 normal, LightInfo light, matrix lightMatrix, float near, float far) {
    float4 fragPosLightProjection = mul(float4(fragPosWorldSpace, 1.0), lightMatrix);
    float3 uv = fragPosLightProjection.xyz / fragPosLightProjection.w;
    uv.xy = uv.xy * 0.5 + 0.5; // Map to [0, 1] // In OpenGL this would include z, DirectX doesn't need it
    uv.y = 1.0 - uv.y;
        
    Texture2D<float> shadowMap = ResourceDescriptorHeap[light.shadowMapIndex];
    SamplerState shadowSampler = SamplerDescriptorHeap[light.shadowSamplerIndex];
    float closestDepth = unprojectDepth(shadowMap.SampleLevel(shadowSampler, uv.xy, 0).r, near, far);
    float currentDepth = fragPosLightProjection.z;
    
    // Scale bias with difference between light direction and normal
    float bias = max(0.0005, 0.01 * (1.0 - dot(normal, light.dirWorldSpace.xyz)));
    
    float shadow = currentDepth - bias > closestDepth ? 1.0 : 0.0;
    return shadow;
}

#endif //__SHADOWS_HLSLI__
