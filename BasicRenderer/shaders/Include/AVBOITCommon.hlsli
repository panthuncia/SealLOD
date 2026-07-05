#ifndef CLOD_AVBOIT_VBOIT_COMMON_HLSLI
#define CLOD_AVBOIT_VBOIT_COMMON_HLSLI

float GetAVBOITLuma(float3 value)
{
    return dot(value, float3(0.2126f, 0.7152f, 0.0722f));
}

bool UsesChromaticTransmittance(float3 tint)
{
    const float maxChannel = max(tint.r, max(tint.g, tint.b));
    const float minChannel = min(tint.r, min(tint.g, tint.b));
    return maxChannel - minChannel > 1.0e-3f;
}

float3 ComputeChromaticExtinctionOpticalDepth(float scalarOpticalDepth, float3 tint)
{
    const float3 clampedTint = saturate(tint);
    const float3 channelAbsorption = -log(max(clampedTint, 1.0e-3f.xxx));
    const float averageAbsorption = max(
        (channelAbsorption.r + channelAbsorption.g + channelAbsorption.b) / 3.0f,
        1.0e-4f);
    return scalarOpticalDepth * (channelAbsorption / averageAbsorption.xxx);
}

uint GetChromaticExtinctionSliceIndex(CLodAVBOITConfig config, uint sliceIndex, uint channelIndex)
{
    return sliceIndex + channelIndex * config.sliceCount;
}

float ComputeDepthWarpLUTSampleCoordinate(CLodAVBOITConfig config, float baseSliceCoordinate)
{
    if (config.virtualSliceCount <= 1u)
    {
        return 0.0f;
    }

    return saturate(baseSliceCoordinate / (float)(config.virtualSliceCount - 1u)) *
        (float)(CLOD_AVBOIT_VBOIT_DEPTH_WARP_LUT_RESOLUTION - 1u);
}

float ComputeBaseSliceCoordinateFromDepthWarpLUTSampleCoordinate(CLodAVBOITConfig config, float lutSampleCoordinate)
{
    if (config.virtualSliceCount <= 1u)
    {
        return 0.0f;
    }

    return saturate(lutSampleCoordinate / (float)(CLOD_AVBOIT_VBOIT_DEPTH_WARP_LUT_RESOLUTION - 1u)) *
        (float)(config.virtualSliceCount - 1u);
}

float SampleDepthWarpLUT(
    StructuredBuffer<CLodAVBOITDepthWarpLUTEntry> depthWarpLUT,
    float lutSampleCoordinate)
{
    const uint lutIndex0 = min(
        (uint)floor(lutSampleCoordinate),
        CLOD_AVBOIT_VBOIT_DEPTH_WARP_LUT_RESOLUTION - 1u);
    const uint lutIndex1 = min(
        lutIndex0 + 1u,
        CLOD_AVBOIT_VBOIT_DEPTH_WARP_LUT_RESOLUTION - 1u);
    const CLodAVBOITDepthWarpLUTEntry depthWarpEntry0 = depthWarpLUT[lutIndex0];
    if (lutIndex0 == lutIndex1 ||
        (depthWarpEntry0.flags & CLOD_AVBOIT_VBOIT_DEPTH_WARP_FLAG_FILTER_TO_NEXT) == 0u)
    {
        return depthWarpEntry0.warpedSliceCoordinate;
    }

    const CLodAVBOITDepthWarpLUTEntry depthWarpEntry1 = depthWarpLUT[lutIndex1];
    return lerp(
        depthWarpEntry0.warpedSliceCoordinate,
        depthWarpEntry1.warpedSliceCoordinate,
        saturate(lutSampleCoordinate - (float)lutIndex0));
}

float ComputeBaseDepthSliceCoordinate(CLodAVBOITConfig config, float depth)
{
    const float depthRange = max(config.viewFarDepth - config.viewNearDepth, 1.0e-5f);
    const float normalizedDepth = saturate((depth - config.viewNearDepth) / depthRange);
    const float depthExponent = max(config.depthDistributionExponent, 1.0e-4f);
    const float distributedDepth = pow(normalizedDepth, depthExponent);
    const uint domainSliceCount = max(config.virtualSliceCount, max(config.sliceCount, 1u));
    return distributedDepth * (float)(domainSliceCount - 1u);
}

float ComputeDepthSliceCoordinate(CLodAVBOITConfig config, float depth)
{
    const float baseSliceCoordinate = ComputeBaseDepthSliceCoordinate(config, depth);
    if (config.depthWarpLUTSRVDescriptorIndex == 0xFFFFFFFFu || config.virtualSliceCount == 0u)
    {
        return baseSliceCoordinate;
    }

    StructuredBuffer<CLodAVBOITDepthWarpLUTEntry> depthWarpLUT =
        ResourceDescriptorHeap[config.depthWarpLUTSRVDescriptorIndex];
    return SampleDepthWarpLUT(depthWarpLUT, ComputeDepthWarpLUTSampleCoordinate(config, baseSliceCoordinate));
}

float ComputeLookupSliceCoordinate(CLodAVBOITConfig config, float sliceCoordinate)
{
    return clamp(
        sliceCoordinate - config.lookupDepthBiasInSlices,
        0.0f,
        (float)(max(config.sliceCount, 1u) - 1u));
}

float3 SampleIntegratedTransmittanceAtSliceCoordinate(
    Texture2DArray<float4> integratedTransmittanceTexture,
    CLodAVBOITConfig config,
    PerFrameBuffer perFrameBuffer,
    uint2 pixel,
    float sliceCoordinate)
{
    if (config.sliceCount == 0u ||
        config.lowResolutionWidth == 0u ||
        config.lowResolutionHeight == 0u ||
        perFrameBuffer.screenResX == 0u ||
        perFrameBuffer.screenResY == 0u)
    {
        return 1.0f.xxx;
    }

    const float2 uv = (float2(pixel) + 0.5f) /
        float2((float)perFrameBuffer.screenResX, (float)perFrameBuffer.screenResY);
    const float lookupSliceCoordinate = ComputeLookupSliceCoordinate(config, sliceCoordinate);
    const uint sliceIndex = min((uint)floor(lookupSliceCoordinate), config.sliceCount - 1u);
    const float sliceLerpFactor = saturate(lookupSliceCoordinate - (float)sliceIndex);
    const float3 previousTransmittance = sliceIndex == 0u
        ? 1.0f.xxx
        : integratedTransmittanceTexture.SampleLevel(g_linearClamp, float3(uv, (float)(sliceIndex - 1u)), 0.0f).rgb;
    const float3 currentTransmittance = integratedTransmittanceTexture.SampleLevel(
        g_linearClamp,
        float3(uv, (float)sliceIndex),
        0.0f).rgb;
    return lerp(previousTransmittance, currentTransmittance, sliceLerpFactor);
}

#if defined(CLOD_AVBOIT_FORWARD_TRANSPARENT)
bool LoadAVBOITShadingSample(VisBufferPSInput input, uint2 pixel, bool isBackface, out ClodShadingSample sample)
{
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    StructuredBuffer<MaterialInfo> materialDataBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];

    sample = (ClodShadingSample)0;

    const MaterialInfo materialInfo = materialDataBuffer[input.materialDataIndex];
    const uint materialFlags = materialInfo.materialFlags;
    const float3 worldPosition = input.positionWorldSpace;
    const float3 worldNormal = normalize(input.normalWorldSpace);
    const float3 dpdx = ((materialFlags & (MATERIAL_NORMAL_MAP | MATERIAL_PARALLAX)) != 0u) ? ddx(worldPosition) : 0.0f.xxx;
    const float3 dpdy = ((materialFlags & (MATERIAL_NORMAL_MAP | MATERIAL_PARALLAX)) != 0u) ? ddy(worldPosition) : 0.0f.xxx;
    MaterialUvCache uvCache;
    MaterialUvBindings uvBindings;
    BuildForwardTransparentMaterialUvData(input, materialInfo, materialFlags, uvCache, uvBindings);

    MaterialInputs materialInputs;
    SampleMaterialFromUvCacheRuntime(
        uvCache,
        uvBindings,
        worldNormal,
        worldPosition,
        input.color,
        materialInfo,
        materialFlags,
        dpdx,
        dpdy,
        materialInputs);

    const Camera mainCamera = cameras[input.viewID];
    sample.linearDepth = input.linearDepth;
    sample.positionWS = worldPosition;
    sample.positionVS = mul(float4(worldPosition, 1.0f), mainCamera.view).xyz;
    sample.materialFlags = materialFlags;
    sample.materialInputs = materialInputs;
    return true;
}
#else
bool LoadAVBOITShadingSample(VisBufferPSInput input, uint2 pixel, bool isBackface, uint primID, out ClodShadingSample sample)
{
    return ResolveClodShadingSampleFromVisKeyWithFace(
        PackVisKey(input.linearDepth, input.visibleClusterIndex, primID),
        pixel,
        isBackface,
        sample);
}
#endif

#endif // CLOD_AVBOIT_VBOIT_COMMON_HLSLI
