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

// The transient composite atlas is always expressed in the current shadow
// camera's linear-depth space. Persistent static depth is rebased into that
// space when it is copied into the transient atlas, so current-frame dynamic
// casters require no additional adjustment here.
float CLodVirtualShadowDepthToCachedPageSpace(
    float currentLinearDepth,
    uint physicalPageIndex,
    uint shadowCameraBufferIndex)
{
    (void)physicalPageIndex;
    (void)shadowCameraBufferIndex;
    return currentLinearDepth;
}

uint CLodVirtualShadowRebaseCachedDepthToCurrentPageSpace(
    uint cachedDepthBits,
    CLodVirtualShadowClipmapInfo clipmapInfo,
    float cachedViewTranslationZ,
    float currentViewTranslationZ)
{
    if (cachedDepthBits >= kCLodVirtualShadowClearedDepth)
    {
        return cachedDepthBits;
    }

    // depth = -(dot(worldPosition, viewZ.xyz) + viewZ.w), hence the inverse
    // of the current-to-cached conversion is:
    //   currentDepth = cachedDepth + cachedViewZ.w - currentViewZ.w
    const float cachedLinearDepth =
        CLodVirtualShadowDecodeDepth(cachedDepthBits, clipmapInfo);
    const float currentLinearDepth = cachedLinearDepth +
        cachedViewTranslationZ - currentViewTranslationZ;
    return CLodVirtualShadowEncodeDepth(currentLinearDepth, clipmapInfo);
}

#endif
