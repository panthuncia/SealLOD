#ifndef __SHADOWS_HLSLI__
#define __SHADOWS_HLSLI__

#include "include/cbuffers.hlsli"
#include "include/clodVirtualShadowClipmap.hlsli"
#include "include/clodVirtualShadowDepth.hlsli"
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
    float2 cachedBasisCorrectionTexels;
    uint2 sampledPageLocalTexel;
    float depthDeltaTexels;
    uint comparisonClipmapIndex;
    float comparisonDepthDeltaTexels;
    uint comparisonFlags;
    float2 comparisonGridOffsetTexels;
    float continuousClipLevel;
    float visualFootprintWorld;
    float actualTexelWorldSize;
    float cameraFootprintWorld;
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
    float continuousClipLevel;
    float visualFootprintWorld;
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
    debugInfo.cachedBasisCorrectionTexels = 0.0f.xx;
    debugInfo.sampledPageLocalTexel = uint2(0u, 0u);
    debugInfo.depthDeltaTexels = 0.0f;
    debugInfo.comparisonClipmapIndex = 0xFFFFFFFFu;
    debugInfo.comparisonDepthDeltaTexels = 0.0f;
    debugInfo.comparisonFlags = 0u;
    debugInfo.comparisonGridOffsetTexels = 0.0f.xx;
    debugInfo.continuousClipLevel = 0.0f;
    debugInfo.visualFootprintWorld = 0.0f;
    debugInfo.actualTexelWorldSize = 0.0f;
    debugInfo.cameraFootprintWorld = 0.0f;
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
    result.continuousClipLevel = 0.0f;
    result.visualFootprintWorld = 0.0f;
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

float3 CLodVirtualShadowDebugCachedBasisColor(
    CLodVirtualShadowDebugInfo debugInfo)
{
    if (debugInfo.sampledClipmapIndex == 0xFFFFFFFFu)
    {
        return float3(1.0f, 0.0f, 1.0f);
    }

    // Neutral gray is no correction. R/G encode signed X/Y displacement,
    // while blue exposes the total magnitude. A one-texel displacement reaches
    // the end of the signed color range.
    const float2 signedCorrection =
        clamp(debugInfo.cachedBasisCorrectionTexels, -1.0f.xx, 1.0f.xx);
    return float3(
        signedCorrection * 0.5f + 0.5f,
        saturate(length(debugInfo.cachedBasisCorrectionTexels)));
}

float3 CLodVirtualShadowDebugPageLocalTexelColor(
    CLodVirtualShadowDebugInfo debugInfo)
{
    if (debugInfo.sampledClipmapIndex == 0xFFFFFFFFu)
    {
        return float3(1.0f, 0.0f, 1.0f);
    }

    const float pageSize =
        max((float)kCLodVirtualShadowPhysicalPageSize, 1.0f);
    const float2 pageUv =
        ((float2)debugInfo.sampledPageLocalTexel + 0.5f) / pageSize;
    const uint2 edgeDistance = min(
        debugInfo.sampledPageLocalTexel,
        (kCLodVirtualShadowPhysicalPageSize - 1u) -
            debugInfo.sampledPageLocalTexel);
    const float edge = any(edgeDistance < 2u) ? 1.0f : 0.0f;
    return float3(pageUv, edge);
}

float3 CLodVirtualShadowDebugDepthMarginColor(
    CLodVirtualShadowDebugInfo debugInfo)
{
    if (debugInfo.sampledClipmapIndex == 0xFFFFFFFFu ||
        (debugInfo.flags &
            (kCLodVirtualShadowDebugFlagFiniteDepthLit |
             kCLodVirtualShadowDebugFlagFiniteDepthShadowed)) == 0u)
    {
        return float3(1.0f, 0.0f, 1.0f);
    }

    const float normalizedMargin =
        clamp(debugInfo.depthDeltaTexels * 0.25f, -1.0f, 1.0f);
    const float3 nearSurface = float3(0.85f, 0.85f, 0.85f);
    return normalizedMargin >= 0.0f
        ? lerp(nearSurface, float3(1.0f, 0.05f, 0.0f), normalizedMargin)
        : lerp(nearSurface, float3(0.0f, 0.25f, 1.0f), -normalizedMargin);
}

float3 CLodVirtualShadowDebugClipComparisonColor(
    CLodVirtualShadowDebugInfo debugInfo)
{
    // Yellow means the adjacent coarser level could not provide a finite
    // comparison sample. Magenta is an actual lit/shadowed disagreement.
    if ((debugInfo.comparisonFlags & 0x1u) == 0u)
    {
        return float3(1.0f, 0.75f, 0.0f);
    }
    if ((debugInfo.comparisonFlags & 0x2u) != 0u)
    {
        const float severity = saturate(
            abs(debugInfo.depthDeltaTexels -
                debugInfo.comparisonDepthDeltaTexels) * 0.25f);
        return lerp(
            float3(0.65f, 0.0f, 0.65f),
            float3(1.0f, 0.0f, 1.0f),
            severity);
    }
    if ((debugInfo.comparisonFlags & 0x4u) != 0u)
    {
        return float3(0.05f, 0.8f, 0.15f);
    }
    return float3(0.05f, 0.12f, 0.45f);
}

float3 CLodVirtualShadowDebugClipGridOffsetColor(
    CLodVirtualShadowDebugInfo debugInfo)
{
    if (debugInfo.comparisonClipmapIndex == 0xFFFFFFFFu)
    {
        return float3(1.0f, 0.75f, 0.0f);
    }

    // Signed adjacent-level texel-center displacement expressed in texels of
    // the sampled (finer) level. This makes the expected half-fine-texel phase
    // of a boundary-aligned 2x grid immediately visible.
    const float2 signedOffset =
        clamp(debugInfo.comparisonGridOffsetTexels, -1.0f.xx, 1.0f.xx);
    return float3(
        signedOffset * 0.5f + 0.5f,
        saturate(length(debugInfo.comparisonGridOffsetTexels)));
}

float3 CLodVirtualShadowDebugTraceFootprintColor(
    CLodVirtualShadowDebugInfo debugInfo)
{
    const float actualToVisual =
        debugInfo.actualTexelWorldSize /
        max(debugInfo.visualFootprintWorld, 1.0e-6f);
    const float cameraToVisual =
        debugInfo.cameraFootprintWorld /
        max(debugInfo.visualFootprintWorld, 1.0e-6f);
    return float3(
        frac(debugInfo.continuousClipLevel),
        saturate((actualToVisual - 1.0f) * 0.5f),
        saturate(cameraToVisual));
}

float CLodVirtualShadowCameraWorldUnitsPerPixel(
    float3 positionWorldSpace,
    Camera camera,
    uint screenHeight)
{
    const float4 positionView =
        mul(float4(positionWorldSpace, 1.0f), camera.view);
    const float viewDepth =
        max(-positionView.z, max(camera.zNear, 1.0e-3f));
    return 2.0f * viewDepth *
        abs(camera.projectionInverse[1][1]) /
        max((float)screenHeight, 1.0f);
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

float2 CLodVirtualShadowQuantizedTexelCenterLightBasis(
    float2 uv,
    CLodVirtualShadowClipmapInfo clipmapInfo,
    CLodVirtualShadowCompactShadowCameraInfo lightCamera)
{
    const float resolution =
        max((float)clipmapInfo.virtualResolution, 1.0f);
    const float2 centerUv =
        (floor(uv * resolution) + 0.5f) / resolution;
    const float2 centerNdc =
        float2(centerUv.x * 2.0f - 1.0f,
            1.0f - centerUv.y * 2.0f);
    const float2 viewPosition = centerNdc /
        float2(
            lightCamera.projection[0][0],
            lightCamera.projection[1][1]);
    return viewPosition - lightCamera.view[3].xy;
}

uint2 CLodVirtualShadowCachedPageAtlasPixel(
    float2 currentUv,
    uint physicalPageIndex,
    CLodVirtualShadowClipmapInfo clipmapInfo,
    CLodVirtualShadowCompactShadowCameraInfo currentLightCamera,
    float4 cachedPageViewRow,
    out float2 basisCorrectionTexels,
    out uint2 pageLocalTexel)
{
    // Page selection is performed in the current toroidal clipmap, but a
    // persistent page's texels were rasterized in its cached view basis.
    // Translate the current UV into that cached basis before taking the
    // page-local texel coordinate. Work in signed virtual texels so an exact
    // whole-page camera scroll (or a small correction across a page edge)
    // wraps to the same physical-page texel.
    const float2 viewTranslationDelta =
        cachedPageViewRow.xy - currentLightCamera.view[3].xy;
    const float4 projectedTranslationDelta =
        mul(float4(viewTranslationDelta, 0.0f, 0.0f),
            currentLightCamera.projection);
    const float2 cachedUv = currentUv +
        float2(
            projectedTranslationDelta.x * 0.5f,
            -projectedTranslationDelta.y * 0.5f);
    basisCorrectionTexels =
        (cachedUv - currentUv) * (float)clipmapInfo.virtualResolution;

    const int2 cachedVirtualTexel =
        (int2)floor(cachedUv * (float)clipmapInfo.virtualResolution);
    const int pageSize = (int)kCLodVirtualShadowPhysicalPageSize;
    const int2 localTexel =
        ((cachedVirtualTexel % pageSize) + pageSize) % pageSize;
    pageLocalTexel = (uint2)localTexel;
    return CLodVirtualShadowPhysicalAtlasPixel(
        physicalPageIndex,
        pageLocalTexel,
        clipmapInfo);
}

#if defined(CLOD_VSM_ADAPTIVE_RECEIVER_SCREEN_TRACE)
bool CLodVirtualShadowIsValidCameraDepth(float depth)
{
    return depth > 0.0f &&
        asuint(depth) != 0x7F7FFFFFu;
}

struct CLodVirtualShadowReceiverOffsetResult
{
    float distance;
    uint escaped;
    uint valid;
};

struct CLodVirtualShadowReceiverTraceContext
{
    float cameraDepthSafety;
    float firstTraceDistance;
    float traceLogRatio;
    uint sampleCount;
    uint valid;
};

CLodVirtualShadowReceiverTraceContext CLodVirtualShadowPrepareReceiverTrace(
    float2 pixelCoords,
    float receiverViewDepth,
    float worldUnitsPerPixel,
    Texture2D<float> cameraLinearDepth,
    uint2 screenSize,
    uint requestedSampleCount,
    float maxTraceDistanceWorld,
    float uncertaintyScale,
    float depthSafetyScale)
{
    CLodVirtualShadowReceiverTraceContext context =
        (CLodVirtualShadowReceiverTraceContext)0;
    if (screenSize.x == 0u || screenSize.y == 0u ||
        pixelCoords.x < 0.0f || pixelCoords.y < 0.0f ||
        pixelCoords.x >= (float)screenSize.x ||
        pixelCoords.y >= (float)screenSize.y ||
        receiverViewDepth <= 0.0f)
    {
        return context;
    }

    const uint2 receiverPixel = min(uint2(pixelCoords), screenSize - 1u);
    const float cameraReceiverDepth =
        cameraLinearDepth.Load(int3(receiverPixel, 0));
    if (!CLodVirtualShadowIsValidCameraDepth(cameraReceiverDepth))
    {
        return context;
    }

    const float receiverAgreementTolerance = max(
        worldUnitsPerPixel * 2.0f,
        receiverViewDepth * 1.0e-5f);
    if (abs(cameraReceiverDepth - receiverViewDepth) >
        receiverAgreementTolerance)
    {
        return context;
    }

    const float traceLimit = max(maxTraceDistanceWorld, 0.0f);
    if (traceLimit <= 1.0e-5f)
    {
        return context;
    }

    const float clampedUncertaintyScale = max(uncertaintyScale, 0.0f);
    const float cameraRepresentationUncertainty = max(
        worldUnitsPerPixel * clampedUncertaintyScale,
        1.0e-4f);
    context.cameraDepthSafety = max(
        worldUnitsPerPixel *
            (clampedUncertaintyScale + max(depthSafetyScale, 0.0f)),
        1.0e-4f);
    context.firstTraceDistance = min(
        traceLimit,
        max(cameraRepresentationUncertainty * 0.25f, 1.0e-4f));
    context.traceLogRatio = log(max(
        traceLimit / max(context.firstTraceDistance, 1.0e-6f),
        1.0f));
    context.sampleCount = clamp(requestedSampleCount, 1u, 32u);
    context.valid = 1u;
    return context;
}

bool CLodVirtualShadowReceiverTraceEscapedAt(
    float3 sampleWorldSpace,
    Camera mainCamera,
    Texture2D<float> cameraLinearDepth,
    uint2 screenSize,
    float cameraDepthSafety,
    out bool sampleValid)
{
    sampleValid = false;
    const float4 sampleClip = mul(
        float4(sampleWorldSpace, 1.0f),
        mainCamera.viewProjection);
    if (sampleClip.w <= 1.0e-6f)
    {
        return false;
    }

    float2 sampleUv = sampleClip.xy / sampleClip.w;
    sampleUv = sampleUv * 0.5f + 0.5f;
    sampleUv.y = 1.0f - sampleUv.y;
    if (any(sampleUv < 0.0f) || any(sampleUv >= 1.0f))
    {
        return false;
    }

    const uint2 samplePixel = min(
        uint2(sampleUv * float2(screenSize)),
        screenSize - 1u);
    const float sceneDepth = cameraLinearDepth.Load(int3(samplePixel, 0));
    if (!CLodVirtualShadowIsValidCameraDepth(sceneDepth))
    {
        return false;
    }

    // This camera projection uses clip.w = -view.z. Reuse the value already
    // produced by the view-projection transform instead of multiplying the
    // sample by the view matrix a second time.
    const float rayViewDepth = sampleClip.w;
    sampleValid = true;
    return rayViewDepth + cameraDepthSafety < sceneDepth;
}

CLodVirtualShadowReceiverOffsetResult CLodVirtualShadowAdaptiveReceiverOffset(
    float3 receiverWorldSpace,
    float3 traceDirection,
    Camera mainCamera,
    Texture2D<float> cameraLinearDepth,
    uint2 screenSize,
    CLodVirtualShadowReceiverTraceContext traceContext)
{
    CLodVirtualShadowReceiverOffsetResult result;
    result.distance = 0.0f;
    result.escaped = 0u;
    result.valid = 0u;
    if (traceContext.valid == 0u)
    {
        return result;
    }
    float lastFailedDistance = 0.0f;
    bool traceRemainedValid = true;

    [loop]
    for (uint sampleIndex = 0u;
         sampleIndex < traceContext.sampleCount;
         ++sampleIndex)
    {
        // Geometric spacing covers very different world scales while retaining
        // samples close enough to the receiver to find a tight ray origin.
        const float sampleAlpha = traceContext.sampleCount > 1u
            ? (float)sampleIndex /
                (float)(traceContext.sampleCount - 1u)
            : 1.0f;
        const float sampleDistance =
            traceContext.firstTraceDistance *
            exp(sampleAlpha * traceContext.traceLogRatio);
        const float3 sampleWorldSpace =
            receiverWorldSpace + traceDirection * sampleDistance;
        bool sampleValid;
        const bool cameraEscaped = CLodVirtualShadowReceiverTraceEscapedAt(
            sampleWorldSpace,
            mainCamera,
            cameraLinearDepth,
            screenSize,
            traceContext.cameraDepthSafety,
            sampleValid);
        if (!sampleValid)
        {
            traceRemainedValid = false;
            break;
        }

        if (!cameraEscaped)
        {
            lastFailedDistance = sampleDistance;
            continue;
        }

        float lowerDistance = lastFailedDistance;
        float upperDistance = sampleDistance;
        [unroll]
        for (uint refinementIndex = 0u; refinementIndex < 3u; ++refinementIndex)
        {
            const float candidateDistance =
                0.5f * (lowerDistance + upperDistance);
            const float3 candidateWorldSpace =
                receiverWorldSpace +
                traceDirection * candidateDistance;
            bool candidateValid;
            const bool candidateCameraEscaped =
                CLodVirtualShadowReceiverTraceEscapedAt(
                    candidateWorldSpace,
                    mainCamera,
                    cameraLinearDepth,
                    screenSize,
                    traceContext.cameraDepthSafety,
                    candidateValid);
            if (candidateValid && candidateCameraEscaped)
            {
                upperDistance = candidateDistance;
            }
            else
            {
                lowerDistance = candidateDistance;
            }
        }

        result.distance = upperDistance;
        result.escaped = 1u;
        result.valid = 1u;
        return result;
    }

    // Failure remains explicit. A valid sample is not proof that the ray left
    // the receiver, and using the last sample as an offset can skip real nearby
    // blockers without fixing the underlying representation error.
    result.valid = traceRemainedValid ? 1u : 0u;
    return result;
}
#endif

struct CLodVirtualShadowProjectedPageCache
{
    uint2 virtualPageCoords;
    uint candidateIndex;
    uint pageEntry;
    uint physicalPageIndex;
    float4 cachedPageViewRow;
    uint valid;
};

CLodVirtualShadowLookupResult CLodVirtualShadowLookupDirectionalOcclusionProjected(
    float3 samplePosWorldSpace,
    uint preferredClipmapIndex,
    float2 preferredUv,
    float preferredLinearLightDepth,
    CLodVirtualShadowClipmapInfo preferredClipmapInfo,
    CLodVirtualShadowCompactShadowCameraInfo preferredLightCamera,
    float visualFootprintWorld,
    float2 ditherWorld,
    uint trustedClipmapIndex,
    uint trustedPhysicalPageIndex,
    uint activeClipmapCount,
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos,
    StructuredBuffer<CLodVirtualShadowCompactShadowCameraInfo> compactShadowCameraBuffer,
    StructuredBuffer<float4> directionalPageViewInfo,
    Texture2DArray<uint> pageTable,
    Texture2D<uint> physicalPages,
    inout CLodVirtualShadowProjectedPageCache projectedPageCache,
    out CLodVirtualShadowDebugInfo debugInfo)
{
    CLodVirtualShadowLookupResult result = CLodVirtualShadowInitLookupResult();
    debugInfo = CLodVirtualShadowInitDebugInfo(preferredClipmapIndex);
    (void)preferredLinearLightDepth;
    result.visualFootprintWorld = visualFootprintWorld;
    debugInfo.visualFootprintWorld = visualFootprintWorld;

    [loop]
    for (uint attempt = 0u; attempt < 3u; ++attempt)
    {
        const uint candidateIndex = preferredClipmapIndex + attempt;
        if (candidateIndex >= activeClipmapCount)
            break;

        CLodVirtualShadowClipmapInfo clipmapInfo = preferredClipmapInfo;
        if (attempt != 0u)
            clipmapInfo = clipmapInfos[candidateIndex];
        if (!CLodVirtualShadowClipmapIsValid(clipmapInfo))
            continue;

        CLodVirtualShadowCompactShadowCameraInfo lightCamera = preferredLightCamera;
        if (attempt != 0u)
            lightCamera = compactShadowCameraBuffer[candidateIndex];
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
        const bool reuseProjectedPage =
            projectedPageCache.valid != 0u &&
            projectedPageCache.candidateIndex == candidateIndex &&
            all(projectedPageCache.virtualPageCoords == virtualPageCoords);
        uint pageEntry;
        if (reuseProjectedPage)
        {
            pageEntry = projectedPageCache.pageEntry;
        }
        else
        {
            const uint2 wrappedPageCoords =
                CLodVirtualShadowWrappedPageCoords(
                    virtualPageCoords,
                    clipmapInfo);
            pageEntry = pageTable.Load(
                int4(wrappedPageCoords, clipmapInfo.pageTableLayer, 0));
        }

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
        if (!CLodVirtualShadowPageEntryHasSampleableContent(pageEntry))
        {
            if (attempt == 0u)
                debugInfo.flags |= kCLodVirtualShadowDebugFlagSampledPageUnwritten;
            continue;
        }
        const uint physicalPageIndex = reuseProjectedPage
            ? projectedPageCache.physicalPageIndex
            : pageEntry & kCLodVirtualShadowPhysicalPageIndexMask;
        const bool receiverPageAlreadyValidated =
            candidateIndex == trustedClipmapIndex &&
            physicalPageIndex == trustedPhysicalPageIndex;
        debugInfo.sampledClipmapIndex = candidateIndex;
        debugInfo.sampledPageEntry = pageEntry;
        debugInfo.sampledPhysicalPageIndex = physicalPageIndex;
        debugInfo.actualTexelWorldSize = clipmapInfo.texelWorldSize;
        if ((pageEntry & kCLodVirtualShadowRerenderedThisFrameMask) != 0u)
            debugInfo.flags |= kCLodVirtualShadowDebugFlagSampledRerenderedThisFrame;

        row_major matrix cachedPageView = lightCamera.view;
        const float4 cachedPageViewRow = reuseProjectedPage
            ? projectedPageCache.cachedPageViewRow
            : directionalPageViewInfo[physicalPageIndex];
        if (!reuseProjectedPage && !receiverPageAlreadyValidated)
        {
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
        }
        if (!reuseProjectedPage)
        {
            projectedPageCache.virtualPageCoords = virtualPageCoords;
            projectedPageCache.candidateIndex = candidateIndex;
            projectedPageCache.pageEntry = pageEntry;
            projectedPageCache.physicalPageIndex = physicalPageIndex;
            projectedPageCache.cachedPageViewRow = cachedPageViewRow;
            projectedPageCache.valid = 1u;
        }
        // Cached X/Y is consumed below to locate the persistent page, but the
        // transient composite depth was rebased to the current view when this
        // page was initialized.
        cachedPageView[3][2] = cachedPageViewRow.z;
        cachedPageView[3][3] = 1.0f;
        const float4 samplePosCurrentLightView =
            mul(float4(samplePosWorldSpace, 1.0f), lightCamera.view);
        const float linearLightDepth =
            -samplePosCurrentLightView.z;
        if (linearLightDepth <= 0.0f)
            continue;

        float2 basisCorrectionTexels;
        uint2 pageLocalTexel;
        const uint2 atlasPixel = CLodVirtualShadowCachedPageAtlasPixel(
            uv.xy,
            physicalPageIndex,
            clipmapInfo,
            lightCamera,
            cachedPageViewRow,
            basisCorrectionTexels,
            pageLocalTexel);
        debugInfo.cachedBasisCorrectionTexels = basisCorrectionTexels;
        debugInfo.sampledPageLocalTexel = pageLocalTexel;
        const uint storedDepthBits = physicalPages.Load(int3(atlasPixel, 0));

        result.clipmapInfo = clipmapInfo;
        debugInfo.actualTexelWorldSize = clipmapInfo.texelWorldSize;
        result.valid = 1u;
        result.sampledLinearDepth = linearLightDepth;
        result.sampledClipmapIndex = candidateIndex;
        result.sampledPhysicalPageIndex = physicalPageIndex;
        if (storedDepthBits == kCLodVirtualShadowClearedDepth)
        {
            // This is the texel actually selected by lookup, even when the
            // preferred clipmap fell back to a coarser one. Restricting this
            // diagnostic to attempt zero made clear coarse-page texels appear
            // healthy green.
            debugInfo.flags |=
                kCLodVirtualShadowDebugFlagSampledTexelCleared;
            result.depthAvailable = 0u;
            result.occlusion = 0.0f;
            result.closestDepth = clipmapInfo.depthNear + clipmapInfo.depthRange;
            return result;
        }
        if (storedDepthBits == kCLodVirtualShadowMissingDepth)
        {
            if (attempt == 0u)
                debugInfo.flags |= kCLodVirtualShadowDebugFlagSampledDepthMissing;
            result.valid = 0u;
            continue;
        }

        const float closestDepth = CLodVirtualShadowDecodeDepth(
            storedDepthBits,
            clipmapInfo);
        const float depthDelta = linearLightDepth - closestDepth;
        result.depthAvailable = 1u;
        result.closestDepth = closestDepth;
        result.occlusion = smoothstep(
            0.0f,
            max(visualFootprintWorld, 1.0e-6f) * 0.5f,
            depthDelta);
        debugInfo.depthDeltaTexels =
            depthDelta / max(clipmapInfo.texelWorldSize, 1.0e-6f);
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
    uint activeClipmapCount,
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos,
    StructuredBuffer<CLodVirtualShadowMainCameraInfo> compactMainCameraBuffer,
    StructuredBuffer<CLodVirtualShadowCompactShadowCameraInfo> compactShadowCameraBuffer,
    StructuredBuffer<float4> directionalPageViewInfo,
    Texture2DArray<uint> pageTable,
    Texture2D<uint> physicalPages,
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

    const float continuousClipLevel =
        CLodVirtualShadowContinuousClipmapLevel(
            samplePosWorldSpace,
            mainCamera.positionWorldSpace.xyz,
            clipmapInfos[0].texelWorldSize,
            clipmapInfos[0].directionalLodBias,
            activeClipmapCount);
    const float visualFootprintWorld =
        CLodVirtualShadowContinuousTexelWorldSize(
            samplePosWorldSpace,
            mainCamera.positionWorldSpace.xyz,
            clipmapInfos[0].texelWorldSize,
            clipmapInfos[0].directionalLodBias,
            activeClipmapCount);
    result.continuousClipLevel = continuousClipLevel;
    result.visualFootprintWorld = visualFootprintWorld;
    result.clipmapInfo = clipmapInfos[preferredClipmapIndex];
    debugInfo.continuousClipLevel = continuousClipLevel;
    debugInfo.visualFootprintWorld = visualFootprintWorld;

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
        if (!CLodVirtualShadowPageEntryHasSampleableContent(pageEntry))
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
        debugInfo.actualTexelWorldSize = clipmapInfo.texelWorldSize;
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
        // Cached X/Y is consumed below to locate the persistent page, but the
        // transient composite depth was rebased to the current view when this
        // page was initialized.
        cachedPageView[3][2] = cachedPageViewRow.z;
        cachedPageView[3][3] = 1.0f;
        const float4 samplePosCurrentLightView =
            mul(float4(samplePosWorldSpace, 1.0f), lightCamera.view);
        const float linearLightDepth =
            -samplePosCurrentLightView.z +
            CLodVirtualShadowReceiverPlaneDepthBias(normal, lightCamera, ditherWorld);
        if (linearLightDepth <= 0.0f)
            continue;

        float2 basisCorrectionTexels;
        uint2 pageLocalTexel;
        const uint2 atlasPixel = CLodVirtualShadowCachedPageAtlasPixel(
            uv.xy,
            physicalPageIndex,
            clipmapInfo,
            lightCamera,
            cachedPageViewRow,
            basisCorrectionTexels,
            pageLocalTexel);
        debugInfo.cachedBasisCorrectionTexels = basisCorrectionTexels;
        debugInfo.sampledPageLocalTexel = pageLocalTexel;

        const uint storedDepthBits = physicalPages.Load(int3(atlasPixel, 0));
        if (storedDepthBits == kCLodVirtualShadowClearedDepth)
        {
            // Report the state of the page that was actually sampled. Coarse
            // fallback samples are just as authoritative as attempt zero.
            debugInfo.flags |=
                kCLodVirtualShadowDebugFlagSampledTexelCleared;
            result.clipmapInfo = clipmapInfo;
            result.valid = 1u;
            result.depthAvailable = 0u;
            result.occlusion = 0.0f;
            result.closestDepth = clipmapInfo.depthNear + clipmapInfo.depthRange;
            result.sampledClipmapIndex = candidateIndex;
            result.sampledPhysicalPageIndex = physicalPageIndex;
            return result;
        }
        if (storedDepthBits == kCLodVirtualShadowMissingDepth)
        {
            if (attempt == 0u)
                debugInfo.flags |= kCLodVirtualShadowDebugFlagSampledDepthMissing;
            continue;
        }

        const float closestDepth = CLodVirtualShadowDecodeDepth(
            storedDepthBits,
            clipmapInfo);
        result.clipmapInfo = clipmapInfo;
        debugInfo.actualTexelWorldSize = clipmapInfo.texelWorldSize;
        result.valid = 1u;
        result.depthAvailable = 1u;
        result.closestDepth = closestDepth;
        result.sampledClipmapIndex = candidateIndex;
        result.sampledPhysicalPageIndex = physicalPageIndex;
        const float depthDelta = linearLightDepth - closestDepth;
        result.occlusion = smoothstep(
            0.0f,
            visualFootprintWorld * 0.5f,
            depthDelta);
        debugInfo.depthDeltaTexels =
            depthDelta / max(clipmapInfo.texelWorldSize, 1.0e-6f);
        debugInfo.flags |= result.occlusion > 0.01f
            ? kCLodVirtualShadowDebugFlagFiniteDepthShadowed
            : kCLodVirtualShadowDebugFlagFiniteDepthLit;
        return result;
    }

    return result;
}

float calculateDirectionalVSMShadowDetailed(float2 pixelCoords, float3 fragPosWorldSpace, float3 fragPosViewSpace, float3 normal, LightInfo light, uint numDirectionalClipmaps, float4 cascadeSplits, StructuredBuffer<unsigned int> cascadeCameraIndexBuffer, StructuredBuffer<Camera> cameraBuffer, out CLodVirtualShadowDebugInfo debugInfo) {
    (void)cascadeCameraIndexBuffer;
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
        activeClipmapCount,
        clipmapInfos,
        compactMainCameraBuffer,
        compactShadowCameraBuffer,
        directionalPageViewInfo,
        pageTable,
        physicalPages,
        float2(0.0f, 0.0f),
        debugInfo);
    const float hardShadow = receiverLookup.occlusion;
    const Camera mainCamera =
        cameraBuffer[perFrameBuffer.mainCameraIndex];
    const float cameraFootprintWorld =
        CLodVirtualShadowCameraWorldUnitsPerPixel(
            fragPosWorldSpace,
            mainCamera,
            perFrameBuffer.screenResY);
    const float visualFootprintWorld =
        max(receiverLookup.visualFootprintWorld, 1.0e-5f);
    debugInfo.cameraFootprintWorld = cameraFootprintWorld;

    if ((perFrameBuffer.outputType == OUTPUT_VSM_CLIP_COMPARISON ||
         perFrameBuffer.outputType == OUTPUT_VSM_CLIP_GRID_OFFSET) &&
        receiverLookup.valid != 0u &&
        receiverLookup.depthAvailable != 0u &&
        debugInfo.sampledClipmapIndex != 0xFFFFFFFFu)
    {
        const uint comparisonClipmapIndex =
            debugInfo.sampledClipmapIndex + 1u;
        debugInfo.comparisonClipmapIndex = comparisonClipmapIndex;
        if (comparisonClipmapIndex < activeClipmapCount)
        {
            const CLodVirtualShadowCompactShadowCameraInfo
                comparisonLightCamera =
                    compactShadowCameraBuffer[comparisonClipmapIndex];
            float2 comparisonUv;
            float comparisonLinearDepth;
            CLodVirtualShadowProjectWorldToUvDepth(
                fragPosWorldSpace,
                comparisonLightCamera,
                comparisonUv,
                comparisonLinearDepth);
            const uint baseClipmapIndex =
                debugInfo.sampledClipmapIndex;
            const CLodVirtualShadowCompactShadowCameraInfo baseLightCamera =
                compactShadowCameraBuffer[baseClipmapIndex];
            float2 baseUv;
            float baseLinearDepth;
            CLodVirtualShadowProjectWorldToUvDepth(
                fragPosWorldSpace,
                baseLightCamera,
                baseUv,
                baseLinearDepth);
            (void)baseLinearDepth;
            const float2 baseTexelCenterLightBasis =
                CLodVirtualShadowQuantizedTexelCenterLightBasis(
                    baseUv,
                    clipmapInfos[baseClipmapIndex],
                    baseLightCamera);
            const float2 comparisonTexelCenterLightBasis =
                CLodVirtualShadowQuantizedTexelCenterLightBasis(
                    comparisonUv,
                    clipmapInfos[comparisonClipmapIndex],
                    comparisonLightCamera);
            debugInfo.comparisonGridOffsetTexels =
                (comparisonTexelCenterLightBasis -
                    baseTexelCenterLightBasis) /
                max(
                    clipmapInfos[baseClipmapIndex].texelWorldSize,
                    1.0e-6f);

            CLodVirtualShadowProjectedPageCache comparisonPageCache =
                (CLodVirtualShadowProjectedPageCache)0;
            CLodVirtualShadowDebugInfo comparisonDebugInfo;
            const CLodVirtualShadowLookupResult comparisonLookup =
                CLodVirtualShadowLookupDirectionalOcclusionProjected(
                    fragPosWorldSpace,
                    comparisonClipmapIndex,
                    comparisonUv,
                    comparisonLinearDepth,
                    clipmapInfos[comparisonClipmapIndex],
                    comparisonLightCamera,
                    receiverLookup.visualFootprintWorld,
                    float2(0.0f, 0.0f),
                    0xFFFFFFFFu,
                    0xFFFFFFFFu,
                    comparisonClipmapIndex + 1u,
                    clipmapInfos,
                    compactShadowCameraBuffer,
                    directionalPageViewInfo,
                    pageTable,
                    physicalPages,
                    comparisonPageCache,
                    comparisonDebugInfo);
            if (comparisonLookup.valid != 0u &&
                comparisonLookup.depthAvailable != 0u)
            {
                const bool baseShadowed =
                    receiverLookup.occlusion > 0.01f;
                const bool comparisonShadowed =
                    comparisonLookup.occlusion > 0.01f;
                debugInfo.comparisonFlags =
                    0x1u |
                    (baseShadowed != comparisonShadowed ? 0x2u : 0u) |
                    (baseShadowed ? 0x4u : 0u) |
                    (comparisonShadowed ? 0x8u : 0u);
                debugInfo.comparisonDepthDeltaTexels =
                    comparisonDebugInfo.depthDeltaTexels;
            }
        }
    }

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

    const uint receiverClipmapIndex =
        debugInfo.sampledClipmapIndex != 0xFFFFFFFFu ?
        debugInfo.sampledClipmapIndex :
        debugInfo.preferredClipmapIndex;
    const CLodVirtualShadowCompactShadowCameraInfo receiverLightCamera =
        compactShadowCameraBuffer[receiverClipmapIndex];
#if defined(CLOD_VSM_ADAPTIVE_RECEIVER_SCREEN_TRACE)
    Texture2D<float> cameraLinearDepth =
        ResourceDescriptorHeap[ResourceDescriptorIndex(
            Builtin::PrimaryCamera::LinearDepthMap)];
    CLodVirtualShadowReceiverTraceContext receiverTraceContext =
        (CLodVirtualShadowReceiverTraceContext)0;
    if (perFrameBuffer.shadowVirtualReceiverTraceEnabled != 0u)
    {
        receiverTraceContext = CLodVirtualShadowPrepareReceiverTrace(
            pixelCoords,
            -fragPosViewSpace.z,
            cameraFootprintWorld,
            cameraLinearDepth,
            uint2(perFrameBuffer.screenResX, perFrameBuffer.screenResY),
            perFrameBuffer.shadowVirtualReceiverTraceSampleCount,
            perFrameBuffer.shadowVirtualReceiverTraceMaxDistanceWorld,
            perFrameBuffer.shadowVirtualReceiverTraceUncertaintyScale,
            perFrameBuffer.shadowVirtualReceiverTraceDepthSafetyScale);
    }
#endif

    const float3 baseFragToLight = normalize(lightToFrag);
    float3 tangent;
    float3 bitangent;
    CLodVirtualShadowBuildOrthonormalBasis(baseFragToLight, tangent, bitangent);

    float2 rayStartUv;
    float rayStartLinearDepth;
    CLodVirtualShadowProjectWorldToUvDepth(fragPosWorldSpace, receiverLightCamera, rayStartUv, rayStartLinearDepth);

    const float2 blueNoiseBase = blueNoiseTex.Load(int3(pixelCoordsInt % blueNoiseSize, 0)).xy;
    const float2 rotation = frac(blueNoiseBase + CLodVirtualShadowSmrtRotation(pixelCoordsInt, perFrameBuffer.frameIndex));
    const float lightDiskTan = tan(coneAngleRadians);
    const float invSampleIntervals =
        1.0f / (float)max(samplesPerRay > 1u ? samplesPerRay - 1u : 1u, 1u);
    float visibleRayCount = 0.0f;
    float validRayCount = 0.0f;
    [loop]
    for (uint rayIndex = 0u; rayIndex < rayCount; ++rayIndex)
    {
        const int2 rayBnOffset = int2(
            CLodVirtualShadowR2Sequence(rayIndex + 1u) *
            float2(blueNoiseSize));
        const float2 rayBn = blueNoiseTex.Load(int3((pixelCoordsInt + rayBnOffset) % blueNoiseSize, 0)).xy;
        float2 xi = CLodVirtualShadowHammersley2D(rayIndex, rayCount);
        xi = frac(xi + rotation + rayBn);
        const float diskRadius = sqrt(xi.x);
        const float diskAngle = kCLodVirtualShadowTwoPi * xi.y;
        const float2 diskSample = diskRadius * float2(cos(diskAngle), sin(diskAngle));

        const float rayJitter = frac(rotation.x + (float)rayIndex * 0.618033988749895f);
        const float3 rayDirection = normalize(
            baseFragToLight +
            tangent * (diskSample.x * lightDiskTan) +
            bitangent * (diskSample.y * lightDiskTan));
        float rayNear = max(cameraFootprintWorld * 0.25f, 1.0e-4f);
#if defined(CLOD_VSM_ADAPTIVE_RECEIVER_SCREEN_TRACE)
        if (perFrameBuffer.shadowVirtualReceiverTraceEnabled != 0u)
        {
            const CLodVirtualShadowReceiverOffsetResult receiverOffset =
                CLodVirtualShadowAdaptiveReceiverOffset(
                    fragPosWorldSpace,
                    rayDirection,
                    mainCamera,
                    cameraLinearDepth,
                    uint2(
                        perFrameBuffer.screenResX,
                        perFrameBuffer.screenResY),
                    receiverTraceContext);
            if (receiverOffset.escaped != 0u)
            {
                // Bound physical ray-origin displacement in screen-space
                // terms. A grazing screen trace may require an arbitrarily
                // long world-space distance to exceed the depth margin; using
                // that full distance skips real blockers and causes peter-pan.
                const float maximumReceiverOffset =
                    cameraFootprintWorld * max(
                        perFrameBuffer.shadowVirtualReceiverTraceUncertaintyScale +
                            perFrameBuffer.shadowVirtualReceiverTraceDepthSafetyScale,
                        0.25f);
                rayNear = max(
                    min(receiverOffset.distance, maximumReceiverOffset),
                    1.0e-4f);
            }
            else if (receiverOffset.valid != 0u)
            {
                // Failure to exceed a camera-depth safety margin is not proof
                // of occlusion: the pixel-sized margin grows in world units
                // with distance. Only a direction below the receiver's
                // shading horizon is definitively self-occluded. Otherwise
                // retain the small baseline origin and let VSM evidence decide.
                if (dot(normalize(normal), rayDirection) <= 0.0f)
                {
                    validRayCount += 1.0f;
                    continue;
                }
            }
            else
            {
                // Off-screen or unavailable camera depth is unknown. Keep the
                // baseline origin rather than converting missing evidence into
                // either shadow or a large receiver offset.
            }
        }
#endif
        // Receiver suppression does not consume the configured blocker-search
        // span. Every ray starts at its own proven-safe point and receives the
        // full SMRT trace distance beyond it.
        const float rayFar = rayNear + maxTraceDistance;
        const float rayLogRatio = log(max(rayFar / rayNear, 1.0f));
        const float3 rayEndWorldSpace =
            fragPosWorldSpace + rayDirection * rayFar;
        float2 rayEndUv;
        float rayEndLinearDepth;
        CLodVirtualShadowProjectWorldToUvDepth(rayEndWorldSpace, receiverLightCamera, rayEndUv, rayEndLinearDepth);

        bool rayHit = false;
        bool rayHadValidSample = false;
        bool rayHadUnknownSample = false;
        bool rayObservedFreeSpace = false;
        bool rayEndpointBehindSurface = false;
        bool previousFiniteDepthValid = false;
        float previousDepthDelta = 0.0f;
        float previousDepthTolerance = 0.0f;
        float previousSampleDistance = 0.0f;
        CLodVirtualShadowProjectedPageCache projectedPageCache =
            (CLodVirtualShadowProjectedPageCache)0;
        [loop]
        for (uint sampleIndex = 0u; sampleIndex < samplesPerRay; ++sampleIndex)
        {
            // Trace from the lightward endpoint back toward the receiver. A
            // receiver in hard shadow starts behind the blocker, so marching
            // receiver-to-light and treating that state as a hit preserves the
            // hard-shadow silhouette instead of testing whether an angled ray
            // actually crosses the blocker. Keep both endpoints exact; jitter
            // only interior strata.
            float sampleLogAlpha = samplesPerRay > 1u
                ? 1.0f - (float)sampleIndex * invSampleIntervals
                : 1.0f;
            if (sampleIndex > 0u && sampleIndex + 1u < samplesPerRay)
            {
                sampleLogAlpha = saturate(
                    sampleLogAlpha +
                    (rayJitter - 0.5f) * invSampleIntervals * 0.5f);
            }
            const float sampleDistance =
                rayNear * exp(sampleLogAlpha * rayLogRatio);
            const float rayAlpha = saturate(sampleDistance / rayFar);
            const float3 samplePosWorldSpace = lerp(fragPosWorldSpace, rayEndWorldSpace, rayAlpha);
            const float2 sampleUv = lerp(rayStartUv, rayEndUv, rayAlpha);
            const float sampleLinearDepth = lerp(rayStartLinearDepth, rayEndLinearDepth, rayAlpha);
            CLodVirtualShadowDebugInfo unusedDebugInfo;
            const CLodVirtualShadowLookupResult raySample = CLodVirtualShadowLookupDirectionalOcclusionProjected(
                samplePosWorldSpace,
                receiverClipmapIndex,
                sampleUv,
                sampleLinearDepth,
                receiverLookup.clipmapInfo,
                receiverLightCamera,
                visualFootprintWorld,
                float2(0.0f, 0.0f),
                receiverLookup.sampledClipmapIndex,
                receiverLookup.sampledPhysicalPageIndex,
                activeClipmapCount,
                clipmapInfos,
                compactShadowCameraBuffer,
                directionalPageViewInfo,
                pageTable,
                physicalPages,
                projectedPageCache,
                unusedDebugInfo);

            if (raySample.valid == 0u)
            {
                // An unallocated or otherwise invalid page is unknown, so it
                // cannot bridge a depth crossing between two known regions.
                rayHadUnknownSample = true;
                previousFiniteDepthValid = false;
                continue;
            }

            // A valid cleared texel is positive evidence that this portion of
            // the ray is free. It makes the ray eligible for the visibility
            // estimator even though there is no finite surface to compare.
            rayHadValidSample = true;
            if (raySample.depthAvailable == 0u)
            {
                rayObservedFreeSpace = true;
                previousFiniteDepthValid = false;
                continue;
            }

            const float closestDepth = raySample.closestDepth;
            const float refDepth = raySample.sampledLinearDepth;
            const float depthDelta = refDepth - closestDepth;
            // Use the continuous receiver footprint rather than the actual
            // fallback clip texel. The latter doubles at clip boundaries and
            // may jump several levels on fallback. Depth tolerance must also
            // remain independent of trace length; coupling it to tFar caused
            // longer traces to widen the dead zone and miss nearby blockers.
            const float depthTolerance = max(
                visualFootprintWorld * 0.15f,
                abs(refDepth) * 1.0e-4f);

            if (sampleIndex == 0u && depthDelta > depthTolerance)
            {
                rayEndpointBehindSurface = true;
            }

            if (depthDelta < -depthTolerance)
            {
                rayObservedFreeSpace = true;
            }

            if (previousFiniteDepthValid &&
                previousDepthDelta < -previousDepthTolerance &&
                depthDelta > depthTolerance)
            {
                // Moving light-to-receiver, a real intersection changes the
                // signed separation from in front of a surface to behind it.
                // Reject large discontinuous jumps: those are normally the
                // ray entering a blocker silhouette after it has already
                // passed the blocker's depth, which is a miss rather than a
                // surface crossing.
                const float stepDistance = max(
                    previousSampleDistance - sampleDistance,
                    1.0e-4f);
                const float maximumContinuousDeltaChange =
                    stepDistance * 4.0f;
                if (abs(depthDelta - previousDepthDelta) <=
                    maximumContinuousDeltaChange)
                {
                    rayHit = true;
                    break;
                }
            }

            previousFiniteDepthValid = true;
            previousDepthDelta = depthDelta;
            previousDepthTolerance = depthTolerance;
            previousSampleDistance = sampleDistance;
        }

        // Retain conservative occlusion only when the exact lightward endpoint
        // is definitively behind a finite surface. Samples which merely remain
        // inside the tolerance band are unresolved, not proof of a blocker.
        if (!rayHit && rayEndpointBehindSurface && !rayObservedFreeSpace)
        {
            rayHit = true;
        }

        if (rayHadValidSample && (rayHit || !rayHadUnknownSample))
        {
            validRayCount += 1.0f;
            visibleRayCount += rayHit ? 0.0f : 1.0f;
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
