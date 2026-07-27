#ifndef CLOD_VIRTUAL_SHADOW_DEPTH_HLSLI
#define CLOD_VIRTUAL_SHADOW_DEPTH_HLSLI

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
