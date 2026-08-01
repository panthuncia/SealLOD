#ifndef CLOD_VIRTUAL_SHADOW_DEPTH_HLSLI
#define CLOD_VIRTUAL_SHADOW_DEPTH_HLSLI

// The physical atlas is R32_UINT so every raster path can use InterlockedMin.
// Reserve the top two values for the cleared and missing states and encode all
// finite depths as an unsigned clip-relative fixed-point value. 0xFFFFFF00 is
// the largest exactly representable float below 2^32, avoiding an overflowing
// float-to-uint conversion at the far plane while retaining almost the entire
// 32-bit integer range.
static const uint kCLodVirtualShadowFixedDepthMax = 0xFFFFFF00u;
static const uint kCLodVirtualShadowClearedDepth = 0xFFFFFFFEu;
static const uint kCLodVirtualShadowMissingDepth = 0xFFFFFFFFu;
static const float kCLodVirtualShadowFixedDepthScale = 4294967040.0f;
static const float kCLodVirtualShadowFixedDepthInvScale =
    1.0f / kCLodVirtualShadowFixedDepthScale;

uint CLodVirtualShadowEncodeDepth(
    float linearDepth,
    CLodVirtualShadowClipmapInfo clipmapInfo)
{
    const float normalizedDepth = saturate(
        (linearDepth - clipmapInfo.depthNear) /
        max(clipmapInfo.depthRange, 1.0e-6f));
    return min(
        (uint)round(normalizedDepth * kCLodVirtualShadowFixedDepthScale),
        kCLodVirtualShadowFixedDepthMax);
}

float CLodVirtualShadowDecodeDepth(
    uint encodedDepth,
    CLodVirtualShadowClipmapInfo clipmapInfo)
{
    return clipmapInfo.depthNear +
        ((float)encodedDepth * kCLodVirtualShadowFixedDepthInvScale) *
            clipmapInfo.depthRange;
}

// Persistent static pages retain the light-view Z origin from the frame in
// which they were rendered. Dynamic casters are rasterized with the current
// shadow camera, so their linear depth must be translated into that cached
// page space before it can be compared with the static depth.
//
// For the directional shadow view orientation:
//   depth = -(dot(worldPosition, viewZ.xyz) + viewZ.w)
// therefore:
//   cachedDepth = currentDepth + currentViewZ.w - cachedViewZ.w
float CLodVirtualShadowDepthToCachedPageSpace(
    float currentLinearDepth,
    uint physicalPageIndex,
    uint shadowCameraBufferIndex)
{
    StructuredBuffer<Camera> shadowCameras =
        ResourceDescriptorHeap[ResourceDescriptorIndex(
            Builtin::CameraBuffer)];
    StructuredBuffer<float4> directionalPageViewInfo =
        ResourceDescriptorHeap[ResourceDescriptorIndex(
            Builtin::Shadows::CLodDirectionalPageViewInfo)];

    const float currentViewTranslationZ =
        shadowCameras[shadowCameraBufferIndex].view[3].z;
    const float cachedViewTranslationZ =
        directionalPageViewInfo[physicalPageIndex].z;
    return currentLinearDepth +
        currentViewTranslationZ -
        cachedViewTranslationZ;
}

#endif
