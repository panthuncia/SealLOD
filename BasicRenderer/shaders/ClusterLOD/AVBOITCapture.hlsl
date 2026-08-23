#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/utilities.hlsli"
#include "include/visibilityPacking.hlsli"
#include "include/clodResolveCommon.hlsli"
#include "include/AVBOITCommon.hlsli"
#include "PerPassRootConstants/clodRasterizationRootConstants.h"

[shader("pixel")]
#if defined(CLOD_AVBOIT_FORWARD_TRANSPARENT)
void AVBOITCapturePSMain(VisBufferPSInput input, bool isFrontFace : SV_IsFrontFace)
#else
void AVBOITCapturePSMain(VisBufferPSInput input, bool isFrontFace : SV_IsFrontFace, uint primID : SV_PrimitiveID)
#endif
{
    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuffer = ResourceDescriptorHeap[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodAVBOITConfig> configBuffer = ResourceDescriptorHeap[CLOD_RASTER_AVBOIT_VBOIT_CONFIG_DESCRIPTOR_INDEX];

#if defined(CLOD_AVBOIT_FORWARD_TRANSPARENT)
    const ClodViewRasterInfo viewRasterInfo = viewRasterInfoBuffer[input.viewID];
#else
    const ClodViewRasterInfo viewRasterInfo = viewRasterInfoBuffer[input.viewID];
#endif
    const CLodAVBOITConfig config = configBuffer[0];
#if defined(CLOD_AVBOIT_VBOIT_OCCUPANCY_ONLY)
    if (viewRasterInfo.opaqueVisibilitySRVDescriptorIndex == 0xFFFFFFFFu ||
        config.virtualSliceCount == 0u ||
        config.occupancyUAVDescriptorIndex == 0xFFFFFFFFu ||
        config.occupancySliceMaskUAVDescriptorIndex == 0xFFFFFFFFu)
#else
    if (viewRasterInfo.opaqueVisibilitySRVDescriptorIndex == 0xFFFFFFFFu ||
        config.sliceCount == 0u ||
        config.occupancyUAVDescriptorIndex == 0xFFFFFFFFu ||
        config.scalarExtinctionUAVDescriptorIndex == 0xFFFFFFFFu ||
        config.chromaticExtinctionUAVDescriptorIndex == 0xFFFFFFFFu)
#endif
    {
        return;
    }

    const uint2 rasterPixel = input.position.xy;
    if (rasterPixel.x < viewRasterInfo.scissorMinX ||
        rasterPixel.y < viewRasterInfo.scissorMinY ||
        rasterPixel.x >= viewRasterInfo.scissorMaxX ||
        rasterPixel.y >= viewRasterInfo.scissorMaxY)
    {
        return;
    }

#if defined(CLOD_AVBOIT_LOW_RES_RASTER)
    const uint2 lowPixel = rasterPixel;
    const uint2 pixel = min(
        rasterPixel * CLOD_AVBOIT_VBOIT_DEFAULT_DOWNSAMPLE_FACTOR +
            CLOD_AVBOIT_VBOIT_DEFAULT_DOWNSAMPLE_FACTOR / 2u,
        uint2(perFrameBuffer.screenResX - 1u, perFrameBuffer.screenResY - 1u));
#else
    const uint2 pixel = rasterPixel;
#endif

#if defined(PSO_ALPHA_TEST)
#if defined(CLOD_AVBOIT_FORWARD_TRANSPARENT)
    TestAlpha(input.texcoord, input.materialDataIndex);
#else
    TestAlpha(input.texcoord, input.materialDataIndex);
#endif
#endif

    Texture2D<uint64_t> opaqueVisibility =
        ResourceDescriptorHeap[NonUniformResourceIndex(viewRasterInfo.opaqueVisibilitySRVDescriptorIndex)];
    const uint64_t opaqueVisKey = opaqueVisibility[pixel];
    if (opaqueVisKey != 0xFFFFFFFFFFFFFFFFu)
    {
        float opaqueDepth;
        uint opaqueClusterIndex;
        uint opaquePrimID;
        UnpackVisKey(opaqueVisKey, opaqueDepth, opaqueClusterIndex, opaquePrimID);
        if (input.linearDepth >= opaqueDepth)
        {
            return;
        }
    }

    ClodShadingSample sample;
#if defined(CLOD_AVBOIT_FORWARD_TRANSPARENT)
    if (!LoadAVBOITShadingSample(input, pixel, !isFrontFace, sample))
#else
    if (!LoadAVBOITShadingSample(input, pixel, !isFrontFace, primID, sample))
#endif
    {
        return;
    }

    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const Camera mainCamera = cameras[perFrameBuffer.mainCameraIndex];

    FragmentInfo fragmentInfo = (FragmentInfo)0;
    fragmentInfo.pixelCoords = float2(pixel);
    fragmentInfo.fragPosWorldSpace = sample.positionWS;
    fragmentInfo.fragPosViewSpace = sample.positionVS;

    const float3 viewWS = normalize(mainCamera.positionWorldSpace.xyz - sample.positionWS);
    FillFragmentInfoDirect(
        fragmentInfo,
        sample.materialInputs,
        viewWS,
        float2(pixel),
        false,
        true,
        isFrontFace,
        sample.materialFlags);

    const float alpha = saturate(fragmentInfo.alpha);
    if (alpha <= 0.0f)
    {
        return;
    }

#if !defined(CLOD_AVBOIT_LOW_RES_RASTER)
    const uint2 lowPixel = pixel / CLOD_AVBOIT_VBOIT_DEFAULT_DOWNSAMPLE_FACTOR;
#endif
    if (lowPixel.x >= config.lowResolutionWidth || lowPixel.y >= config.lowResolutionHeight)
    {
        return;
    }

#if defined(CLOD_AVBOIT_VBOIT_OCCUPANCY_ONLY)
    const float virtualSliceCoordinate = ComputeBaseDepthSliceCoordinate(config, sample.linearDepth);
    const uint virtualSliceIndex0 = min((uint)floor(virtualSliceCoordinate), config.virtualSliceCount - 1u);
    const uint virtualSliceIndex1 = min(virtualSliceIndex0 + 1u, config.virtualSliceCount - 1u);
    RWTexture2D<float> occupancyTexture = ResourceDescriptorHeap[config.occupancyUAVDescriptorIndex];
    RWTexture2D<uint> occupancySliceMaskTexture = ResourceDescriptorHeap[config.occupancySliceMaskUAVDescriptorIndex];
    occupancyTexture[lowPixel] = 1.0f;

    const uint sliceBit0 = 1u << virtualSliceIndex0;
    const uint sliceBit1 = 1u << virtualSliceIndex1;
    const uint occupancySliceMask = virtualSliceIndex0 == virtualSliceIndex1 ? sliceBit0 : (sliceBit0 | sliceBit1);
    uint ignoredOccupancyMask;
    InterlockedOr(occupancySliceMaskTexture[lowPixel], occupancySliceMask, ignoredOccupancyMask);
    return;
#endif

    const float sliceCoordinate = ComputeDepthSliceCoordinate(config, sample.linearDepth);
    const uint sliceIndex0 = min((uint)floor(sliceCoordinate), config.sliceCount - 1u);
    const uint sliceIndex1 = min(sliceIndex0 + 1u, config.sliceCount - 1u);

#if defined(CLOD_AVBOIT_LOW_RES_RASTER)
    const float lowResolutionPixelCoverage = 1.0f;
#else
    const float lowResolutionPixelCoverage = rcp((float)(
        CLOD_AVBOIT_VBOIT_DEFAULT_DOWNSAMPLE_FACTOR *
        CLOD_AVBOIT_VBOIT_DEFAULT_DOWNSAMPLE_FACTOR));
#endif
    const float opticalDepth = -log(max(1.0f - alpha, 1.0e-4f)) * lowResolutionPixelCoverage;
    const float sliceWeight1 = saturate(sliceCoordinate - (float)sliceIndex0);
    const float sliceWeight0 = 1.0f - sliceWeight1;

    const bool useChromaticExtinction = UsesChromaticTransmittance(fragmentInfo.albedo);
    const float opticalDepth0 = opticalDepth * sliceWeight0;
    const float opticalDepth1 = opticalDepth * sliceWeight1;

    if (!useChromaticExtinction)
    {
        uint quantizedExtinction0 = (uint)round(opticalDepth0 * CLOD_AVBOIT_VBOIT_EXTINCTION_QUANTIZATION_SCALE);
        uint quantizedExtinction1 = (uint)round(opticalDepth1 * CLOD_AVBOIT_VBOIT_EXTINCTION_QUANTIZATION_SCALE);
        if (quantizedExtinction0 == 0u && quantizedExtinction1 == 0u)
        {
            if (sliceWeight0 >= sliceWeight1)
            {
                quantizedExtinction0 = 1u;
            }
            else
            {
                quantizedExtinction1 = 1u;
            }
        }

        RWTexture2DArray<uint> scalarExtinctionTexture = ResourceDescriptorHeap[config.scalarExtinctionUAVDescriptorIndex];
        uint ignored;
        if (quantizedExtinction0 != 0u)
        {
            InterlockedAdd(scalarExtinctionTexture[uint3(lowPixel, sliceIndex0)], quantizedExtinction0, ignored);
        }
        if (quantizedExtinction1 != 0u)
        {
            InterlockedAdd(scalarExtinctionTexture[uint3(lowPixel, sliceIndex1)], quantizedExtinction1, ignored);
        }
        return;
    }

    const float3 chromaticOpticalDepth0 = ComputeChromaticExtinctionOpticalDepth(opticalDepth0, fragmentInfo.albedo);
    const float3 chromaticOpticalDepth1 = ComputeChromaticExtinctionOpticalDepth(opticalDepth1, fragmentInfo.albedo);
    uint4 quantizedChromaticExtinction0 = uint4(
        (uint)round(chromaticOpticalDepth0.r * CLOD_AVBOIT_VBOIT_EXTINCTION_QUANTIZATION_SCALE),
        (uint)round(chromaticOpticalDepth0.g * CLOD_AVBOIT_VBOIT_EXTINCTION_QUANTIZATION_SCALE),
        (uint)round(chromaticOpticalDepth0.b * CLOD_AVBOIT_VBOIT_EXTINCTION_QUANTIZATION_SCALE),
        0u);
    uint4 quantizedChromaticExtinction1 = uint4(
        (uint)round(chromaticOpticalDepth1.r * CLOD_AVBOIT_VBOIT_EXTINCTION_QUANTIZATION_SCALE),
        (uint)round(chromaticOpticalDepth1.g * CLOD_AVBOIT_VBOIT_EXTINCTION_QUANTIZATION_SCALE),
        (uint)round(chromaticOpticalDepth1.b * CLOD_AVBOIT_VBOIT_EXTINCTION_QUANTIZATION_SCALE),
        0u);
    if ((quantizedChromaticExtinction0.x | quantizedChromaticExtinction0.y | quantizedChromaticExtinction0.z | quantizedChromaticExtinction1.x | quantizedChromaticExtinction1.y | quantizedChromaticExtinction1.z) == 0u)
    {
        if (sliceWeight0 >= sliceWeight1)
        {
            quantizedChromaticExtinction0 = uint4(1u, 1u, 1u, 0u);
        }
        else
        {
            quantizedChromaticExtinction1 = uint4(1u, 1u, 1u, 0u);
        }
    }

    RWTexture2DArray<uint> chromaticExtinctionTexture = ResourceDescriptorHeap[config.chromaticExtinctionUAVDescriptorIndex];
    uint ignored;
    if ((quantizedChromaticExtinction0.x | quantizedChromaticExtinction0.y | quantizedChromaticExtinction0.z) != 0u)
    {
        InterlockedAdd(
            chromaticExtinctionTexture[uint3(lowPixel, GetChromaticExtinctionSliceIndex(config, sliceIndex0, 0u))],
            quantizedChromaticExtinction0.x,
            ignored);
        InterlockedAdd(
            chromaticExtinctionTexture[uint3(lowPixel, GetChromaticExtinctionSliceIndex(config, sliceIndex0, 1u))],
            quantizedChromaticExtinction0.y,
            ignored);
        InterlockedAdd(
            chromaticExtinctionTexture[uint3(lowPixel, GetChromaticExtinctionSliceIndex(config, sliceIndex0, 2u))],
            quantizedChromaticExtinction0.z,
            ignored);
    }
    if ((quantizedChromaticExtinction1.x | quantizedChromaticExtinction1.y | quantizedChromaticExtinction1.z) != 0u)
    {
        InterlockedAdd(
            chromaticExtinctionTexture[uint3(lowPixel, GetChromaticExtinctionSliceIndex(config, sliceIndex1, 0u))],
            quantizedChromaticExtinction1.x,
            ignored);
        InterlockedAdd(
            chromaticExtinctionTexture[uint3(lowPixel, GetChromaticExtinctionSliceIndex(config, sliceIndex1, 1u))],
            quantizedChromaticExtinction1.y,
            ignored);
        InterlockedAdd(
            chromaticExtinctionTexture[uint3(lowPixel, GetChromaticExtinctionSliceIndex(config, sliceIndex1, 2u))],
            quantizedChromaticExtinction1.z,
            ignored);
    }
}
