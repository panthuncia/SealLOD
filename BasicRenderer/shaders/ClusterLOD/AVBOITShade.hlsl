#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/lighting.hlsli"
#include "include/clodResolveCommon.hlsli"
#include "include/utilities.hlsli"
#include "include/visibilityPacking.hlsli"
#include "include/AVBOITCommon.hlsli"
#include "PerPassRootConstants/clodRasterizationRootConstants.h"

struct AVBOITShadeOutput
{
    float4 accumulation : SV_Target0;
    float4 normalization : SV_Target1;
    float4 shadingExtinction : SV_Target2;
};

AVBOITShadeOutput MakeAVBOITShadeOutput(float4 accumulation, float4 normalization, float4 shadingExtinction)
{
    AVBOITShadeOutput output;
    output.accumulation = accumulation;
    output.normalization = normalization;
    output.shadingExtinction = shadingExtinction;
    return output;
}

[shader("pixel")]
#if defined(CLOD_AVBOIT_FORWARD_TRANSPARENT)
AVBOITShadeOutput AVBOITShadePSMain(VisBufferPSInput input, bool isFrontFace : SV_IsFrontFace)
#else
AVBOITShadeOutput AVBOITShadePSMain(VisBufferPSInput input, bool isFrontFace : SV_IsFrontFace, uint primID : SV_PrimitiveID)
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
    if (viewRasterInfo.opaqueVisibilitySRVDescriptorIndex == 0xFFFFFFFFu ||
        config.shadingTransmittanceSRVDescriptorIndex == 0xFFFFFFFFu ||
        config.sliceCount == 0u)
    {
        return MakeAVBOITShadeOutput(0.0f.xxxx, 0.0f.xxxx, 0.0f.xxxx);
    }

    const uint2 pixel = input.position.xy;
    if (pixel.x < viewRasterInfo.scissorMinX ||
        pixel.y < viewRasterInfo.scissorMinY ||
        pixel.x >= viewRasterInfo.scissorMaxX ||
        pixel.y >= viewRasterInfo.scissorMaxY)
    {
        return MakeAVBOITShadeOutput(0.0f.xxxx, 0.0f.xxxx, 0.0f.xxxx);
    }

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
            return MakeAVBOITShadeOutput(0.0f.xxxx, 0.0f.xxxx, 0.0f.xxxx);
        }
    }

    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const Camera mainCamera = cameras[perFrameBuffer.mainCameraIndex];

    FragmentInfo fragmentInfo = (FragmentInfo)0;
    float linearDepth;
    {
        ClodShadingSample sample;
#if defined(CLOD_AVBOIT_FORWARD_TRANSPARENT)
        if (!LoadAVBOITShadingSample(input, pixel, !isFrontFace, sample))
#else
        if (!LoadAVBOITShadingSample(input, pixel, !isFrontFace, primID, sample))
#endif
        {
            return MakeAVBOITShadeOutput(0.0f.xxxx, 0.0f.xxxx, 0.0f.xxxx);
        }

        linearDepth = sample.linearDepth;
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
    }

    const float alpha = saturate(fragmentInfo.alpha);
    if (alpha <= 0.0f)
    {
        return MakeAVBOITShadeOutput(0.0f.xxxx, 0.0f.xxxx, 0.0f.xxxx);
    }

    const float sliceCoordinate = ComputeDepthSliceCoordinate(config, linearDepth);
    Texture2DArray<float4> integratedTransmittanceTexture = ResourceDescriptorHeap[config.shadingTransmittanceSRVDescriptorIndex];
    const float3 transmittance = SampleIntegratedTransmittanceAtSliceCoordinate(
        integratedTransmittanceTexture,
        config,
        perFrameBuffer,
        pixel,
        sliceCoordinate);
    if (max(transmittance.r, max(transmittance.g, transmittance.b)) <= max(config.zeroTransmittanceThreshold, 1.0e-4f))
    {
        return MakeAVBOITShadeOutput(0.0f.xxxx, 0.0f.xxxx, 0.0f.xxxx);
    }

    const float3 lighting = lightFragmentColor(
        fragmentInfo,
        mainCamera,
        perFrameBuffer.activeEnvironmentIndex,
        ResourceDescriptorIndex(Builtin::Environment::InfoBuffer));

    const float3 weight = alpha * transmittance;
    const float scalarOpticalDepth = -log(max(1.0f - alpha, 1.0e-4f));
    const float3 shadingOpticalDepth = UsesChromaticTransmittance(fragmentInfo.albedo)
        ? ComputeChromaticExtinctionOpticalDepth(scalarOpticalDepth, fragmentInfo.albedo)
        : scalarOpticalDepth.xxx;
    return MakeAVBOITShadeOutput(
        float4(lighting * weight, 0.0f),
        float4(weight, 0.0f),
        float4(shadingOpticalDepth * transmittance, 0.0f));
}
