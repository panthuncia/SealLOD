#ifndef __UTILITY_HLSL__
#define __UTILITY_HLSL__
#include "include/structs.hlsli"
#include "include/cbuffers.hlsli"
#include "include/vertex.hlsli"
#include "include/materialFlags.hlsli"
#include "include/parallax.hlsli"
#include "include/gammaCorrection.hlsli"
#include "include/constants.hlsli"
#include "include/dynamicSwizzle.hlsli"
#include "include/outputTypes.hlsli"
#include "include/waveIntrinsicsHelpers.hlsli"

struct OpenPBRSurfaceSample
{
    uint openPBRMaterialDataIndex;
    float3 baseColor;
    float baseMetalness;
    float specularRoughness;
    float3 coatColor;
    float coatWeight;
    float coatRoughness;
    float3 fuzzColor;
    float fuzzWeight;
    float fuzzRoughness;
    float opacity;
    float3 emissive;
};

OpenPBRMaterialInfo LoadOpenPBRMaterialInfo(uint openPBRMaterialDataIndex)
{
    StructuredBuffer<OpenPBRMaterialInfo> openPBRMaterialBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialOpenPBRDataBuffer)];
    return openPBRMaterialBuffer[openPBRMaterialDataIndex];
}

OpenPBRMaterialInfo LoadOpenPBRMaterialInfo(MaterialInfo materialInfo)
{
    return LoadOpenPBRMaterialInfo(materialInfo.openPBRMaterialDataIndex);
}

OpenPBRMaterialInfo LoadOpenPBRMaterialInfo(MaterialEvalInfo materialInfo)
{
    return LoadOpenPBRMaterialInfo(materialInfo.openPBRMaterialDataIndex);
}

TextureStreamingGPUInfo LoadTextureStreamingInfo(uint streamingTextureID)
{
    StructuredBuffer<TextureStreamingGPUInfo> textureStreamingMetadataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Material::TextureStreamingMetadataBuffer)];
    return textureStreamingMetadataBuffer[streamingTextureID];
}

static const uint kTextureStreamingFlagEnabled = 1u << 1;

float3 DecodeMaterialNormalSample(float4 textureNormal, uint3 channels, uint materialFlags)
{
    float3 tangentSpaceNormal;
    if (channels.z >= 4u)
    {
        float2 xy = float2(
            DynamicSwizzle(textureNormal, channels.x),
            DynamicSwizzle(textureNormal, channels.y)) * 2.0f - 1.0f;
        tangentSpaceNormal = float3(xy, sqrt(saturate(1.0f - dot(xy, xy))));
    }
    else
    {
        tangentSpaceNormal = DynamicSwizzle(textureNormal, channels) * 2.0f - 1.0f;
    }

    tangentSpaceNormal = normalize(tangentSpaceNormal);
    if ((materialFlags & MATERIAL_NEGATE_NORMALS) != 0u) tangentSpaceNormal = -tangentSpaceNormal;
    if ((materialFlags & MATERIAL_INVERT_NORMAL_GREEN) != 0u) tangentSpaceNormal.g = -tangentSpaceNormal.g;
    return tangentSpaceNormal;
}

bool TextureStreamingHasFullMip0Dimensions(TextureStreamingGPUInfo streamingInfo)
{
    return streamingInfo.fullWidth != 0u && streamingInfo.fullHeight != 0u;
}

float2 ResolveTextureStreamingTexelScale(
    TextureStreamingGPUInfo streamingInfo,
    uint residentWidth,
    uint residentHeight)
{
    const uint width = TextureStreamingHasFullMip0Dimensions(streamingInfo)
        ? streamingInfo.fullWidth
        : residentWidth;
    const uint height = TextureStreamingHasFullMip0Dimensions(streamingInfo)
        ? streamingInfo.fullHeight
        : residentHeight;
    return float2((float)max(width, 1u), (float)max(height, 1u));
}

uint ComputeTextureStreamingRequestedTopMip(TextureStreamingGPUInfo streamingInfo, float2 texelDdx, float2 texelDdy)
{
    const float maxFootprintSq = max(dot(texelDdx, texelDdx), dot(texelDdy, texelDdy));
    const float lod = max(0.0f, 0.5f * log2(max(maxFootprintSq, 1e-8f)));
    const uint localRequestedTopMip = (uint)lod;
    const uint requestedTopMipBase = TextureStreamingHasFullMip0Dimensions(streamingInfo)
        ? 0u
        : streamingInfo.residentTopMip;
    return min(streamingInfo.totalMipCount - 1u, requestedTopMipBase + localRequestedTopMip);
}

uint ReduceWaveGroupRequestedTopMipMin(uint4 groupMask, uint requestedTopMip)
{
    uint minRequestedTopMip = 0xffffffffu;

    [unroll]
    for (uint wordIndex = 0u; wordIndex < 4u; ++wordIndex)
    {
        uint wordMask = groupMask[wordIndex];
        while (wordMask != 0u)
        {
            const uint bitIndex = firstbitlow(wordMask);
            const uint laneIndex = wordIndex * 32u + bitIndex;
            minRequestedTopMip = min(minRequestedTopMip, WaveReadLaneAt(requestedTopMip, laneIndex));
            wordMask &= ~(1u << bitIndex);
        }
    }

    return minRequestedTopMip;
}

void RecordTextureStreamingFeedback(
    TextureStreamingGPUInfo streamingInfo,
    uint streamingTextureID,
    float2 texelDdx,
    float2 texelDdy)
{
    if (streamingTextureID == 0u) {
        return;
    }

    if ((streamingInfo.flags & kTextureStreamingFlagEnabled) == 0u || streamingInfo.totalMipCount <= 1u) {
        return;
    }

    RWStructuredBuffer<uint> textureStreamingFeedbackBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Material::TextureStreamingFeedbackBuffer)];

    const uint requestedTopMip = ComputeTextureStreamingRequestedTopMip(streamingInfo, texelDdx, texelDdy);
    const uint4 groupMask = WaveMatch(streamingTextureID);
    const uint leaderLane = WaveFirstLaneFromMask(groupMask);
    if (WaveGetLaneIndex() != leaderLane) {
        return;
    }

    const uint groupRequestedTopMip = ReduceWaveGroupRequestedTopMipMin(groupMask, requestedTopMip);

    uint previousRequestedTopMip;
    InterlockedMin(textureStreamingFeedbackBuffer[streamingTextureID], groupRequestedTopMip, previousRequestedTopMip);
}

OpenPBRSurfaceSample ResolveCanonicalOpenPBRSurface(
    MaterialInfo materialInfo,
    OpenPBRMaterialInfo openPBRMaterialInfo,
    float3 sampledBaseColor,
    float sampledMetalness,
    float sampledSpecularRoughness,
    float sampledOpacity,
    float3 sampledEmissive)
{
    const float3 canonicalEmissive = openPBRMaterialInfo.emissionColor * openPBRMaterialInfo.emissionLuminance;

    OpenPBRSurfaceSample surface = (OpenPBRSurfaceSample)0;
    surface.openPBRMaterialDataIndex = materialInfo.openPBRMaterialDataIndex;
    surface.baseColor = sampledBaseColor;
    surface.baseMetalness = sampledMetalness;
    surface.specularRoughness = sampledSpecularRoughness;
    surface.coatColor = saturate(openPBRMaterialInfo.coatColor);
    surface.coatWeight = saturate(openPBRMaterialInfo.coatWeight);
    surface.coatRoughness = saturate(openPBRMaterialInfo.coatRoughness);
    surface.fuzzColor = saturate(openPBRMaterialInfo.fuzzColor);
    surface.fuzzWeight = saturate(openPBRMaterialInfo.fuzzWeight);
    surface.fuzzRoughness = saturate(openPBRMaterialInfo.fuzzRoughness);
    surface.opacity = saturate(sampledOpacity * openPBRMaterialInfo.geometryOpacity);
    surface.emissive = dot(sampledEmissive, sampledEmissive) > 0.0f ? sampledEmissive : canonicalEmissive;
    return surface;
}

OpenPBRSurfaceSample ResolveCanonicalOpenPBRSurface(
    MaterialInfo materialInfo,
    float3 sampledBaseColor,
    float sampledMetalness,
    float sampledSpecularRoughness,
    float sampledOpacity,
    float3 sampledEmissive)
{
    return ResolveCanonicalOpenPBRSurface(
        materialInfo,
        LoadOpenPBRMaterialInfo(materialInfo),
        sampledBaseColor,
        sampledMetalness,
        sampledSpecularRoughness,
        sampledOpacity,
        sampledEmissive);
}

OpenPBRSurfaceSample ResolveCanonicalOpenPBRSurface(
    MaterialEvalInfo materialInfo,
    OpenPBRMaterialInfo openPBRMaterialInfo,
    float3 sampledBaseColor,
    float sampledMetalness,
    float sampledSpecularRoughness,
    float sampledOpacity,
    float3 sampledEmissive)
{
    const float3 canonicalEmissive = openPBRMaterialInfo.emissionColor * openPBRMaterialInfo.emissionLuminance;

    OpenPBRSurfaceSample surface = (OpenPBRSurfaceSample)0;
    surface.openPBRMaterialDataIndex = materialInfo.openPBRMaterialDataIndex;
    surface.baseColor = sampledBaseColor;
    surface.baseMetalness = sampledMetalness;
    surface.specularRoughness = sampledSpecularRoughness;
    surface.coatColor = saturate(openPBRMaterialInfo.coatColor);
    surface.coatWeight = saturate(openPBRMaterialInfo.coatWeight);
    surface.coatRoughness = saturate(openPBRMaterialInfo.coatRoughness);
    surface.fuzzColor = saturate(openPBRMaterialInfo.fuzzColor);
    surface.fuzzWeight = saturate(openPBRMaterialInfo.fuzzWeight);
    surface.fuzzRoughness = saturate(openPBRMaterialInfo.fuzzRoughness);
    surface.opacity = saturate(sampledOpacity * openPBRMaterialInfo.geometryOpacity);
    surface.emissive = dot(sampledEmissive, sampledEmissive) > 0.0f ? sampledEmissive : canonicalEmissive;
    return surface;
}

OpenPBRSurfaceSample ResolveCanonicalOpenPBRSurface(
    MaterialEvalInfo materialInfo,
    float3 sampledBaseColor,
    float sampledMetalness,
    float sampledSpecularRoughness,
    float sampledOpacity,
    float3 sampledEmissive)
{
    return ResolveCanonicalOpenPBRSurface(
        materialInfo,
        LoadOpenPBRMaterialInfo(materialInfo),
        sampledBaseColor,
        sampledMetalness,
        sampledSpecularRoughness,
        sampledOpacity,
        sampledEmissive);
}

void PopulateLegacyMaterialInputsFromOpenPBRSurface(
    OpenPBRSurfaceSample surface,
    float3 normalWS,
    float ambientOcclusion,
    out MaterialInputs materialInputs)
{
    materialInputs.albedo = surface.baseColor;
    materialInputs.normalWS = normalWS;
    materialInputs.emissive = surface.emissive;
    materialInputs.coatColor = surface.coatColor;
    materialInputs.metallic = surface.baseMetalness;
    materialInputs.roughness = surface.specularRoughness;
    materialInputs.coatWeight = surface.coatWeight;
    materialInputs.coatRoughness = surface.coatRoughness;
    materialInputs.fuzzColor = surface.fuzzColor;
    materialInputs.fuzzWeight = surface.fuzzWeight;
    materialInputs.fuzzRoughness = surface.fuzzRoughness;
    materialInputs.opacity = surface.opacity;
    materialInputs.ambientOcclusion = ambientOcclusion;
    materialInputs.openPBRMaterialDataIndex = surface.openPBRMaterialDataIndex;
}

float4 lightUints(uint meshletIndex, float3 normal, float3 viewDir)
{
    float ambientIntensity = 0.3;
    float3 lightColor = float3(1, 1, 1);
    float3 lightDir = -normalize(float3(1, -1, 1));

    float3 diffuseColor = float3(
            float(meshletIndex & 1),
            float(meshletIndex & 3) / 4,
            float(meshletIndex & 7) / 8);
   float shininess = 16.0;

    float cosAngle = saturate(dot(normal, lightDir));
    float3 halfAngle = normalize(lightDir + viewDir);

    float blinnTerm = saturate(dot(normal, halfAngle));
    blinnTerm = cosAngle != 0.0 ? blinnTerm : 0.0;
    blinnTerm = pow(blinnTerm, shininess);

    float3 finalColor = (cosAngle + blinnTerm + ambientIntensity) * diffuseColor;

    return float4(finalColor, 1);
}

#define FLT_MAX 3.402823466e+38f
float3 SignedOctEncode(float3 n)
{
    float3 OutN;

    n /= (abs(n.x) + abs(n.y) + abs(n.z));

    OutN.y = n.y * 0.5 + 0.5;
    OutN.x = n.x * 0.5 + OutN.y;
    OutN.y = n.x * -0.5 + OutN.y;

    OutN.z = saturate(n.z * FLT_MAX);
    return OutN;
}

float3 SignedOctDecode(float3 n)
{
    float3 OutN;

    OutN.x = (n.x - n.y);
    OutN.y = (n.x + n.y) - 1.0;
    OutN.z = n.z * 2.0 - 1.0;
    OutN.z = OutN.z * (1.0 - abs(OutN.x) - abs(OutN.y));
 
    OutN = normalize(OutN);
    return OutN;
}

float3 computeDiffuseColor(const float3 baseColor, float metallic){
    return baseColor.rgb * (1.0 - metallic);
}

//http://www.thetenthplanet.de/archives/1180
float3x3 cotangent_frame(float3 N, float3 p, float2 uv)
{
    // get edge vectors of the pixel triangle 
    float3 dp1 = ddx(p);
    float3 dp2 = ddy(p);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);
    // solve the linear system 
    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    // construct a scale-invariant frame 
    float invmax = rsqrt(max(dot(T, T), dot(B, B)));
    return float3x3(T * invmax, B * invmax, N);
}

// Build cotangent frame using explicit derivatives (no ddx/ddy)
float3x3 cotangent_frame_from_derivs(
    float3 N,
    float3 dpdx, float3 dpdy,
    float2 dUVdx, float2 dUVdy)
{
    float3 dp2perp = cross(dpdy, N);
    float3 dp1perp = cross(N, dpdx);

    float3 T = dp2perp * dUVdx.x + dp1perp * dUVdy.x;
    float3 B = dp2perp * dUVdx.y + dp1perp * dUVdy.y;

    float invmax = rsqrt(max(dot(T, T), dot(B, B)));
    return float3x3(T * invmax, B * invmax, N);
}

void TestAlpha(in float2 texcoords, in uint materialDataIndex)
{
    StructuredBuffer<MaterialInfo> materialDataBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    MaterialInfo materialInfo = materialDataBuffer[materialDataIndex];
    uint materialFlags = materialInfo.materialFlags;
    float2 dTexcoordsDx = ddx(texcoords);
    float2 dTexcoordsDy = ddy(texcoords);
        
    float4 baseColor = materialInfo.baseColorFactor;

    if (materialFlags & MATERIAL_BASE_COLOR_TEXTURE)
    {
        Texture2D<float4> baseColorTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.baseColorTextureIndex)];
        SamplerState baseColorSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.baseColorSamplerIndex)];
        float4 sampledColor = baseColorTexture.SampleGrad(baseColorSamplerState, texcoords, dTexcoordsDx, dTexcoordsDy);
#if defined(PSO_ALPHA_TEST) || defined (PSO_BLEND)
        if (baseColor.a * sampledColor.a < materialInfo.alphaCutoff){
            discard;
        }
#endif // PSO_ALPHA_TEST || PSO_BLEND
    }
    
    if (materialFlags & MATERIAL_OPACITY_TEXTURE)
    {
        Texture2D<float4> opacityTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.opacityTextureIndex)];
        SamplerState opacitySamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.opacitySamplerIndex)];
        float4 opacitySample = opacityTexture.SampleGrad(opacitySamplerState, texcoords, dTexcoordsDx, dTexcoordsDy);
        float opacity = opacitySample.a;
        baseColor.a *= opacity;
        if (baseColor.a < materialInfo.alphaCutoff)
        {
            discard;
        }
    }
}

void SampleMaterialCorePrecompiled(
    in float2 uv,
    in float2 dUVdx,
    in float2 dUVdy,
    in float3 normalWSBase,
    in float3 posWS,
    in MaterialInfo materialInfo,
    in uint materialFlags,
    out MaterialInputs ret);

void SampleMaterialCore(
    in float2 uv,
    in float3 normalWSBase,
    in float3 posWS,
    in MaterialInfo materialInfo,
    in uint materialFlags,
    out MaterialInputs ret)
{
    SampleMaterialCorePrecompiled(uv, ddx(uv), ddy(uv), normalWSBase, posWS, materialInfo, materialFlags, ret);
}

float4 Sample2DGrad(Texture2D<float4> tex, SamplerState samp, float2 uv, float2 dUVdx, float2 dUVdy)
{
    return tex.SampleGrad(samp, uv, dUVdx, dUVdy);
}
float Sample2DGrad(Texture2D<float> tex, SamplerState samp, float2 uv, float2 dUVdx, float2 dUVdy)
{
    return tex.SampleGrad(samp, uv, dUVdx, dUVdy);
}

uint ComputeTextureMipCountFromDimensions(uint width, uint height)
{
    const uint maxDimension = max(width, height);
    return maxDimension > 0u ? (1u + firstbithigh(maxDimension)) : 1u;
}

float ComputeMaterialSelectedMipLod(float2 texelDdx, float2 texelDdy)
{
    const float maxFootprintSq = max(dot(texelDdx, texelDdx), dot(texelDdy, texelDdy));
    return max(0.0f, 0.5f * log2(max(maxFootprintSq, 1.0e-8f)));
}

bool ShouldTrackMaterialSelectedMipDebug()
{
    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    return perFrameBuffer.outputType == OUTPUT_MATERIAL_SELECTED_MIP;
}

void InitializeMaterialSelectedMipDebug(inout MaterialInputs materialInputs)
{
    materialInputs.selectedMaterialMipLevel = MATERIAL_DEBUG_INVALID_MIP_LEVEL;
    materialInputs.selectedMaterialMipMaxLevel = 0u;
    materialInputs.parallaxApplied = 0u;
    materialInputs.terrainRvtDebugFlags = 0u;
    materialInputs.terrainRvtRequestedMip = MATERIAL_DEBUG_INVALID_MIP_LEVEL;
    materialInputs.terrainRvtResidentMip = MATERIAL_DEBUG_INVALID_MIP_LEVEL;
    materialInputs.terrainRvtPageTableIndex = 0xffffffffu;
    materialInputs.terrainRvtPhysicalPageIndex = 0xffffffffu;
    materialInputs.terrainRvtAtlasPoolIndex = 0xffffffffu;
    materialInputs.terrainRvtOwnerPageTableIndex = 0xffffffffu;
    materialInputs.terrainRvtFallbackReason = 0u;
    materialInputs.terrainRvtPageCoord = 0xffffffffu.xx;
    materialInputs.terrainRvtPageUv = 0.0f.xx;
    materialInputs.terrainRvtAtlasUv = 0.0f.xxx;
    materialInputs.terrainRvtPhysicalTileUv = 0.0f.xx;
    materialInputs.terrainRvtSampleAlbedo = 0.0f.xxx;
    materialInputs.terrainRvtSampleAlbedoPoint = 0.0f.xxx;
    materialInputs.terrainRvtSampleNormal = 0.0f.xxx;
    materialInputs.terrainRvtSampleMaterial = 0.0f.xxx;
    materialInputs.terrainRvtHeightScale = 0.0f;
}

void AccumulateMaterialSelectedMipDebug(inout MaterialInputs materialInputs, uint selectedMipLevel, uint selectedMipMaxLevel)
{
    if (materialInputs.selectedMaterialMipLevel == MATERIAL_DEBUG_INVALID_MIP_LEVEL ||
        selectedMipLevel < materialInputs.selectedMaterialMipLevel)
    {
        materialInputs.selectedMaterialMipLevel = selectedMipLevel;
        materialInputs.selectedMaterialMipMaxLevel = selectedMipMaxLevel;
        return;
    }

    if (selectedMipLevel == materialInputs.selectedMaterialMipLevel)
    {
        materialInputs.selectedMaterialMipMaxLevel = max(materialInputs.selectedMaterialMipMaxLevel, selectedMipMaxLevel);
    }
}

void RecordMaterialSelectedMipDebug(
    inout MaterialInputs materialInputs,
    TextureStreamingGPUInfo streamingInfo,
    bool hasStreamingInfo,
    uint width,
    uint height,
    float2 dUVdx,
    float2 dUVdy)
{
    uint residentTopMip = 0u;
    uint totalMipCount = ComputeTextureMipCountFromDimensions(width, height);
    float2 texelScale = float2((float)width, (float)height);
    if (hasStreamingInfo)
    {
        residentTopMip = streamingInfo.residentTopMip;
        totalMipCount = max(streamingInfo.totalMipCount, 1u);
        texelScale = ResolveTextureStreamingTexelScale(streamingInfo, width, height);
    }

    const uint selectedMipBase = (hasStreamingInfo && TextureStreamingHasFullMip0Dimensions(streamingInfo))
        ? 0u
        : residentTopMip;
    const uint selectedMipLevel = min(
        totalMipCount - 1u,
        selectedMipBase + (uint)ComputeMaterialSelectedMipLod(dUVdx * texelScale, dUVdy * texelScale));
    AccumulateMaterialSelectedMipDebug(materialInputs, selectedMipLevel, totalMipCount - 1u);
}

float4 SampleResidentMaterialTexture2DGrad(
    Texture2D<float4> tex,
    SamplerState samp,
    float2 uv,
    float2 dUVdx,
    float2 dUVdy,
    inout MaterialInputs materialInputs)
{
    if (ShouldTrackMaterialSelectedMipDebug())
    {
        uint width;
        uint height;
        tex.GetDimensions(width, height);
        RecordMaterialSelectedMipDebug(materialInputs, (TextureStreamingGPUInfo)0, false, width, height, dUVdx, dUVdy);
    }

    return Sample2DGrad(tex, samp, uv, dUVdx, dUVdy);
}

float4 SampleStreamingMaterialTexture2DGrad(
    Texture2D<float4> tex,
    SamplerState samp,
    uint streamingTextureID,
    float2 uv,
    float2 dUVdx,
    float2 dUVdy,
    inout MaterialInputs materialInputs)
{
    uint width;
    uint height;
    tex.GetDimensions(width, height);

    const bool hasStreamingInfo = streamingTextureID != 0u;
    TextureStreamingGPUInfo streamingInfo = (TextureStreamingGPUInfo)0;
    if (hasStreamingInfo) {
        streamingInfo = LoadTextureStreamingInfo(streamingTextureID);
    }

    if (hasStreamingInfo) {
        const float2 texelScale = ResolveTextureStreamingTexelScale(streamingInfo, width, height);
        RecordTextureStreamingFeedback(streamingInfo, streamingTextureID, dUVdx * texelScale, dUVdy * texelScale);
    }

    if (ShouldTrackMaterialSelectedMipDebug())
    {
        RecordMaterialSelectedMipDebug(materialInputs, streamingInfo, hasStreamingInfo, width, height, dUVdx, dUVdy);
    }

    return Sample2DGrad(tex, samp, uv, dUVdx, dUVdy);
}

float4 SampleMaterialTexture2DGrad(
    Texture2D<float4> tex,
    SamplerState samp,
    uint streamingTextureID,
    float2 uv,
    float2 dUVdx,
    float2 dUVdy,
    inout MaterialInputs materialInputs)
{
#if defined(PSO_TEXTURE_STREAMING)
    return SampleStreamingMaterialTexture2DGrad(tex, samp, streamingTextureID, uv, dUVdx, dUVdy, materialInputs);
#else
    return SampleResidentMaterialTexture2DGrad(tex, samp, uv, dUVdx, dUVdy, materialInputs);
#endif
}

float4 SampleMaterialTexture2DGrad(Texture2D<float4> tex, SamplerState samp, uint streamingTextureID, float2 uv, float2 dUVdx, float2 dUVdy)
{
    MaterialInputs unusedMaterialInputs = (MaterialInputs)0;
    InitializeMaterialSelectedMipDebug(unusedMaterialInputs);
    return SampleMaterialTexture2DGrad(tex, samp, streamingTextureID, uv, dUVdx, dUVdy, unusedMaterialInputs);
}

float SampleResidentMaterialTexture2DGrad(
    Texture2D<float> tex,
    SamplerState samp,
    float2 uv,
    float2 dUVdx,
    float2 dUVdy,
    inout MaterialInputs materialInputs)
{
    if (ShouldTrackMaterialSelectedMipDebug())
    {
        uint width;
        uint height;
        tex.GetDimensions(width, height);
        RecordMaterialSelectedMipDebug(materialInputs, (TextureStreamingGPUInfo)0, false, width, height, dUVdx, dUVdy);
    }

    return Sample2DGrad(tex, samp, uv, dUVdx, dUVdy);
}

float SampleStreamingMaterialTexture2DGrad(
    Texture2D<float> tex,
    SamplerState samp,
    uint streamingTextureID,
    float2 uv,
    float2 dUVdx,
    float2 dUVdy,
    inout MaterialInputs materialInputs)
{
    uint width;
    uint height;
    tex.GetDimensions(width, height);

    const bool hasStreamingInfo = streamingTextureID != 0u;
    TextureStreamingGPUInfo streamingInfo = (TextureStreamingGPUInfo)0;
    if (hasStreamingInfo) {
        streamingInfo = LoadTextureStreamingInfo(streamingTextureID);
    }

    if (hasStreamingInfo) {
        const float2 texelScale = ResolveTextureStreamingTexelScale(streamingInfo, width, height);
        RecordTextureStreamingFeedback(streamingInfo, streamingTextureID, dUVdx * texelScale, dUVdy * texelScale);
    }

    if (ShouldTrackMaterialSelectedMipDebug())
    {
        RecordMaterialSelectedMipDebug(materialInputs, streamingInfo, hasStreamingInfo, width, height, dUVdx, dUVdy);
    }

    return Sample2DGrad(tex, samp, uv, dUVdx, dUVdy);
}

float SampleMaterialTexture2DGrad(
    Texture2D<float> tex,
    SamplerState samp,
    uint streamingTextureID,
    float2 uv,
    float2 dUVdx,
    float2 dUVdy,
    inout MaterialInputs materialInputs)
{
#if defined(PSO_TEXTURE_STREAMING)
    return SampleStreamingMaterialTexture2DGrad(tex, samp, streamingTextureID, uv, dUVdx, dUVdy, materialInputs);
#else
    return SampleResidentMaterialTexture2DGrad(tex, samp, uv, dUVdx, dUVdy, materialInputs);
#endif
}

float SampleMaterialTexture2DGrad(Texture2D<float> tex, SamplerState samp, uint streamingTextureID, float2 uv, float2 dUVdx, float2 dUVdy)
{
    MaterialInputs unusedMaterialInputs = (MaterialInputs)0;
    InitializeMaterialSelectedMipDebug(unusedMaterialInputs);
    return SampleMaterialTexture2DGrad(tex, samp, streamingTextureID, uv, dUVdx, dUVdy, unusedMaterialInputs);
}

#define MATERIAL_MAX_UNIQUE_UV_SETS 8u
#define MATERIAL_INVALID_UV_CACHE_INDEX 0xffffffffu
#define OPENPBR_TEXTURE_SLOT_COUNT 6u

enum OpenPBRTextureSlot
{
    OPENPBR_TEXTURE_SLOT_COAT_COLOR = 0,
    OPENPBR_TEXTURE_SLOT_COAT_WEIGHT = 1,
    OPENPBR_TEXTURE_SLOT_COAT_ROUGHNESS = 2,
    OPENPBR_TEXTURE_SLOT_FUZZ_COLOR = 3,
    OPENPBR_TEXTURE_SLOT_FUZZ_WEIGHT = 4,
    OPENPBR_TEXTURE_SLOT_FUZZ_ROUGHNESS = 5
};

enum MaterialTextureSlot
{
    MATERIAL_TEXTURE_SLOT_BASE_COLOR = 0,
    MATERIAL_TEXTURE_SLOT_OPACITY = 1,
    MATERIAL_TEXTURE_SLOT_METALLIC = 2,
    MATERIAL_TEXTURE_SLOT_ROUGHNESS = 3,
    MATERIAL_TEXTURE_SLOT_NORMAL = 4,
    MATERIAL_TEXTURE_SLOT_AO = 5,
    MATERIAL_TEXTURE_SLOT_EMISSIVE = 6,
    MATERIAL_TEXTURE_SLOT_HEIGHT = 7,
    MATERIAL_TEXTURE_SLOT_COUNT = 8
};

struct MaterialUvSample
{
    uint uvSetIndex;
    float2 uv;
    float2 dUVdx;
    float2 dUVdy;
};

struct MaterialUvCache
{
    uint count;
    MaterialUvSample samples[MATERIAL_MAX_UNIQUE_UV_SETS];
};

struct MaterialUvBindings
{
    uint cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_COUNT];
    uint openPBRCacheIndexBySlot[OPENPBR_TEXTURE_SLOT_COUNT];
    uint tbnCacheIndex;
    uint heightCacheIndex;
    bool hasTbnSource;
    bool hasHeightSource;
};

void InitializeMaterialUvBindings(out MaterialUvBindings bindings)
{
    bindings = (MaterialUvBindings)0;

    [unroll]
    for (uint slot = 0u; slot < MATERIAL_TEXTURE_SLOT_COUNT; ++slot)
    {
        bindings.cacheIndexBySlot[slot] = MATERIAL_INVALID_UV_CACHE_INDEX;
    }

    [unroll]
    for (uint slot = 0u; slot < OPENPBR_TEXTURE_SLOT_COUNT; ++slot)
    {
        bindings.openPBRCacheIndexBySlot[slot] = MATERIAL_INVALID_UV_CACHE_INDEX;
    }

    bindings.tbnCacheIndex = MATERIAL_INVALID_UV_CACHE_INDEX;
    bindings.heightCacheIndex = MATERIAL_INVALID_UV_CACHE_INDEX;
    bindings.hasTbnSource = false;
    bindings.hasHeightSource = false;
}

MaterialUvSample MakeDefaultMaterialUvSample()
{
    MaterialUvSample sample = (MaterialUvSample)0;
    sample.uvSetIndex = 0u;
    return sample;
}

MaterialUvCache BuildSingleUvCache(float2 uv, float2 dUVdx, float2 dUVdy)
{
    MaterialUvCache cache = (MaterialUvCache)0;
    cache.count = 1u;
    cache.samples[0].uvSetIndex = 0u;
    cache.samples[0].uv = uv;
    cache.samples[0].dUVdx = dUVdx;
    cache.samples[0].dUVdy = dUVdy;
    return cache;
}

static const uint OPENPBR_INVALID_TEXTURE_INDEX = 0xffffffffu;

bool HasOpenPBRTexture(uint textureIndex, uint samplerIndex)
{
    return textureIndex != OPENPBR_INVALID_TEXTURE_INDEX && samplerIndex != OPENPBR_INVALID_TEXTURE_INDEX;
}

MaterialUvSample ResolveOpenPBRTextureUv(
    in MaterialUvCache uvCache,
    uint cacheIndex,
    uint uvSetIndex,
    bool hasParallaxResolvedUv,
    uint parallaxUvSetIndex,
    float2 parallaxUv,
    float2 parallaxDUdx,
    float2 parallaxDUdy)
{
    if (hasParallaxResolvedUv && uvSetIndex == parallaxUvSetIndex)
    {
        MaterialUvSample sample = (MaterialUvSample)0;
        sample.uvSetIndex = uvSetIndex;
        sample.uv = parallaxUv;
        sample.dUVdx = parallaxDUdx;
        sample.dUVdy = parallaxDUdy;
        return sample;
    }

    if (cacheIndex == MATERIAL_INVALID_UV_CACHE_INDEX || cacheIndex >= uvCache.count)
    {
        return MakeDefaultMaterialUvSample();
    }

    return uvCache.samples[cacheIndex];
}

float4 SampleOpenPBRTexture(uint textureIndex, uint samplerIndex, uint streamingTextureID, MaterialUvSample uvSample, inout MaterialInputs materialInputs)
{
    Texture2D<float4> textureHandle = ResourceDescriptorHeap[NonUniformResourceIndex(textureIndex)];
    SamplerState samplerHandle = SamplerDescriptorHeap[NonUniformResourceIndex(samplerIndex)];
    return SampleMaterialTexture2DGrad(textureHandle, samplerHandle, streamingTextureID, uvSample.uv, uvSample.dUVdx, uvSample.dUVdy, materialInputs);
}

float3 SampleOpenPBRColorTexture(
    uint textureIndex,
    uint samplerIndex,
    uint streamingTextureID,
    uint4 channels,
    MaterialUvSample uvSample,
    inout MaterialInputs materialInputs)
{
    if (!HasOpenPBRTexture(textureIndex, samplerIndex))
    {
        return float3(1.0f, 1.0f, 1.0f);
    }

    const float4 sampleValue = SampleOpenPBRTexture(textureIndex, samplerIndex, streamingTextureID, uvSample, materialInputs);
    return float3(
        DynamicSwizzle(sampleValue, channels.x),
        DynamicSwizzle(sampleValue, channels.y),
        DynamicSwizzle(sampleValue, channels.z));
}

float SampleOpenPBRScalarTexture(
    uint textureIndex,
    uint samplerIndex,
    uint streamingTextureID,
    uint channel,
    MaterialUvSample uvSample,
    inout MaterialInputs materialInputs)
{
    if (!HasOpenPBRTexture(textureIndex, samplerIndex))
    {
        return 1.0f;
    }

    const float4 sampleValue = SampleOpenPBRTexture(textureIndex, samplerIndex, streamingTextureID, uvSample, materialInputs);
    return DynamicSwizzle(sampleValue, channel);
}

void ApplyOpenPBRTextureSampling(
    in MaterialUvCache uvCache,
    in MaterialUvBindings uvBindings,
    bool hasParallaxResolvedUv,
    uint parallaxUvSetIndex,
    float2 parallaxUv,
    float2 parallaxDUdx,
    float2 parallaxDUdy,
    in OpenPBRMaterialInfo openPBRMaterialInfo,
    inout OpenPBRSurfaceSample surface,
    inout MaterialInputs materialInputs)
{
    const uint cacheIndices[OPENPBR_TEXTURE_SLOT_COUNT] = {
        uvBindings.openPBRCacheIndexBySlot[0],
        uvBindings.openPBRCacheIndexBySlot[1],
        uvBindings.openPBRCacheIndexBySlot[2],
        uvBindings.openPBRCacheIndexBySlot[3],
        uvBindings.openPBRCacheIndexBySlot[4],
        uvBindings.openPBRCacheIndexBySlot[5]
    };

    const MaterialUvSample coatColorUv = ResolveOpenPBRTextureUv(
        uvCache,
        cacheIndices[0],
        openPBRMaterialInfo.coatColorUvSetIndex,
        hasParallaxResolvedUv,
        parallaxUvSetIndex,
        parallaxUv,
        parallaxDUdx,
        parallaxDUdy);
    const MaterialUvSample coatWeightUv = ResolveOpenPBRTextureUv(
        uvCache,
        cacheIndices[1],
        openPBRMaterialInfo.coatWeightUvSetIndex,
        hasParallaxResolvedUv,
        parallaxUvSetIndex,
        parallaxUv,
        parallaxDUdx,
        parallaxDUdy);
    const MaterialUvSample coatRoughnessUv = ResolveOpenPBRTextureUv(
        uvCache,
        cacheIndices[2],
        openPBRMaterialInfo.coatRoughnessUvSetIndex,
        hasParallaxResolvedUv,
        parallaxUvSetIndex,
        parallaxUv,
        parallaxDUdx,
        parallaxDUdy);
    const MaterialUvSample fuzzColorUv = ResolveOpenPBRTextureUv(
        uvCache,
        cacheIndices[3],
        openPBRMaterialInfo.fuzzColorUvSetIndex,
        hasParallaxResolvedUv,
        parallaxUvSetIndex,
        parallaxUv,
        parallaxDUdx,
        parallaxDUdy);
    const MaterialUvSample fuzzWeightUv = ResolveOpenPBRTextureUv(
        uvCache,
        cacheIndices[4],
        openPBRMaterialInfo.fuzzWeightUvSetIndex,
        hasParallaxResolvedUv,
        parallaxUvSetIndex,
        parallaxUv,
        parallaxDUdx,
        parallaxDUdy);
    const MaterialUvSample fuzzRoughnessUv = ResolveOpenPBRTextureUv(
        uvCache,
        cacheIndices[5],
        openPBRMaterialInfo.fuzzRoughnessUvSetIndex,
        hasParallaxResolvedUv,
        parallaxUvSetIndex,
        parallaxUv,
        parallaxDUdx,
        parallaxDUdy);

    surface.coatColor *= SampleOpenPBRColorTexture(
        openPBRMaterialInfo.coatColorTextureIndex,
        openPBRMaterialInfo.coatColorSamplerIndex,
        openPBRMaterialInfo.coatColorStreamingTextureID,
        openPBRMaterialInfo.coatColorChannels,
        coatColorUv,
        materialInputs);
    surface.coatWeight *= SampleOpenPBRScalarTexture(
        openPBRMaterialInfo.coatWeightTextureIndex,
        openPBRMaterialInfo.coatWeightSamplerIndex,
        openPBRMaterialInfo.coatWeightStreamingTextureID,
        openPBRMaterialInfo.coatWeightChannel,
        coatWeightUv,
        materialInputs);
    surface.coatRoughness *= SampleOpenPBRScalarTexture(
        openPBRMaterialInfo.coatRoughnessTextureIndex,
        openPBRMaterialInfo.coatRoughnessSamplerIndex,
        openPBRMaterialInfo.coatRoughnessStreamingTextureID,
        openPBRMaterialInfo.coatRoughnessChannel,
        coatRoughnessUv,
        materialInputs);

    surface.fuzzColor *= SampleOpenPBRColorTexture(
        openPBRMaterialInfo.fuzzColorTextureIndex,
        openPBRMaterialInfo.fuzzColorSamplerIndex,
        openPBRMaterialInfo.fuzzColorStreamingTextureID,
        openPBRMaterialInfo.fuzzColorChannels,
        fuzzColorUv,
        materialInputs);
    surface.fuzzWeight *= SampleOpenPBRScalarTexture(
        openPBRMaterialInfo.fuzzWeightTextureIndex,
        openPBRMaterialInfo.fuzzWeightSamplerIndex,
        openPBRMaterialInfo.fuzzWeightStreamingTextureID,
        openPBRMaterialInfo.fuzzWeightChannel,
        fuzzWeightUv,
        materialInputs);
    surface.fuzzRoughness *= SampleOpenPBRScalarTexture(
        openPBRMaterialInfo.fuzzRoughnessTextureIndex,
        openPBRMaterialInfo.fuzzRoughnessSamplerIndex,
        openPBRMaterialInfo.fuzzRoughnessStreamingTextureID,
        openPBRMaterialInfo.fuzzRoughnessChannel,
        fuzzRoughnessUv,
        materialInputs);

    surface.coatColor = saturate(surface.coatColor);
    surface.coatWeight = saturate(surface.coatWeight);
    surface.coatRoughness = saturate(surface.coatRoughness);
    surface.fuzzColor = saturate(surface.fuzzColor);
    surface.fuzzWeight = saturate(surface.fuzzWeight);
    surface.fuzzRoughness = saturate(surface.fuzzRoughness);
}

void ApplyOpenPBRTextureSamplingSpecialized(
    in MaterialUvCache uvCache,
    in MaterialUvBindings uvBindings,
    bool hasParallaxResolvedUv,
    uint parallaxUvSetIndex,
    float2 parallaxUv,
    float2 parallaxDUdx,
    float2 parallaxDUdy,
    in OpenPBRMaterialInfo openPBRMaterialInfo,
    inout OpenPBRSurfaceSample surface,
    inout MaterialInputs materialInputs)
{
#if defined(PSO_OPENPBR_COAT_COLOR_TEXTURE)
    const MaterialUvSample coatColorUv = ResolveOpenPBRTextureUv(
        uvCache,
        uvBindings.openPBRCacheIndexBySlot[0],
        openPBRMaterialInfo.coatColorUvSetIndex,
        hasParallaxResolvedUv,
        parallaxUvSetIndex,
        parallaxUv,
        parallaxDUdx,
        parallaxDUdy);
    surface.coatColor *= SampleOpenPBRColorTexture(
        openPBRMaterialInfo.coatColorTextureIndex,
        openPBRMaterialInfo.coatColorSamplerIndex,
        openPBRMaterialInfo.coatColorStreamingTextureID,
        openPBRMaterialInfo.coatColorChannels,
        coatColorUv,
        materialInputs);
#endif

#if defined(PSO_OPENPBR_COAT_WEIGHT_TEXTURE)
    const MaterialUvSample coatWeightUv = ResolveOpenPBRTextureUv(
        uvCache,
        uvBindings.openPBRCacheIndexBySlot[1],
        openPBRMaterialInfo.coatWeightUvSetIndex,
        hasParallaxResolvedUv,
        parallaxUvSetIndex,
        parallaxUv,
        parallaxDUdx,
        parallaxDUdy);
    surface.coatWeight *= SampleOpenPBRScalarTexture(
        openPBRMaterialInfo.coatWeightTextureIndex,
        openPBRMaterialInfo.coatWeightSamplerIndex,
        openPBRMaterialInfo.coatWeightStreamingTextureID,
        openPBRMaterialInfo.coatWeightChannel,
        coatWeightUv,
        materialInputs);
#endif

#if defined(PSO_OPENPBR_COAT_ROUGHNESS_TEXTURE)
    const MaterialUvSample coatRoughnessUv = ResolveOpenPBRTextureUv(
        uvCache,
        uvBindings.openPBRCacheIndexBySlot[2],
        openPBRMaterialInfo.coatRoughnessUvSetIndex,
        hasParallaxResolvedUv,
        parallaxUvSetIndex,
        parallaxUv,
        parallaxDUdx,
        parallaxDUdy);
    surface.coatRoughness *= SampleOpenPBRScalarTexture(
        openPBRMaterialInfo.coatRoughnessTextureIndex,
        openPBRMaterialInfo.coatRoughnessSamplerIndex,
        openPBRMaterialInfo.coatRoughnessStreamingTextureID,
        openPBRMaterialInfo.coatRoughnessChannel,
        coatRoughnessUv,
        materialInputs);
#endif

#if defined(PSO_OPENPBR_FUZZ_COLOR_TEXTURE)
    const MaterialUvSample fuzzColorUv = ResolveOpenPBRTextureUv(
        uvCache,
        uvBindings.openPBRCacheIndexBySlot[3],
        openPBRMaterialInfo.fuzzColorUvSetIndex,
        hasParallaxResolvedUv,
        parallaxUvSetIndex,
        parallaxUv,
        parallaxDUdx,
        parallaxDUdy);
    surface.fuzzColor *= SampleOpenPBRColorTexture(
        openPBRMaterialInfo.fuzzColorTextureIndex,
        openPBRMaterialInfo.fuzzColorSamplerIndex,
        openPBRMaterialInfo.fuzzColorStreamingTextureID,
        openPBRMaterialInfo.fuzzColorChannels,
        fuzzColorUv,
        materialInputs);
#endif

#if defined(PSO_OPENPBR_FUZZ_WEIGHT_TEXTURE)
    const MaterialUvSample fuzzWeightUv = ResolveOpenPBRTextureUv(
        uvCache,
        uvBindings.openPBRCacheIndexBySlot[4],
        openPBRMaterialInfo.fuzzWeightUvSetIndex,
        hasParallaxResolvedUv,
        parallaxUvSetIndex,
        parallaxUv,
        parallaxDUdx,
        parallaxDUdy);
    surface.fuzzWeight *= SampleOpenPBRScalarTexture(
        openPBRMaterialInfo.fuzzWeightTextureIndex,
        openPBRMaterialInfo.fuzzWeightSamplerIndex,
        openPBRMaterialInfo.fuzzWeightStreamingTextureID,
        openPBRMaterialInfo.fuzzWeightChannel,
        fuzzWeightUv,
        materialInputs);
#endif

#if defined(PSO_OPENPBR_FUZZ_ROUGHNESS_TEXTURE)
    const MaterialUvSample fuzzRoughnessUv = ResolveOpenPBRTextureUv(
        uvCache,
        uvBindings.openPBRCacheIndexBySlot[5],
        openPBRMaterialInfo.fuzzRoughnessUvSetIndex,
        hasParallaxResolvedUv,
        parallaxUvSetIndex,
        parallaxUv,
        parallaxDUdx,
        parallaxDUdy);
    surface.fuzzRoughness *= SampleOpenPBRScalarTexture(
        openPBRMaterialInfo.fuzzRoughnessTextureIndex,
        openPBRMaterialInfo.fuzzRoughnessSamplerIndex,
        openPBRMaterialInfo.fuzzRoughnessStreamingTextureID,
        openPBRMaterialInfo.fuzzRoughnessChannel,
        fuzzRoughnessUv,
        materialInputs);
#endif

    surface.coatColor = saturate(surface.coatColor);
    surface.coatWeight = saturate(surface.coatWeight);
    surface.coatRoughness = saturate(surface.coatRoughness);
    surface.fuzzColor = saturate(surface.fuzzColor);
    surface.fuzzWeight = saturate(surface.fuzzWeight);
    surface.fuzzRoughness = saturate(surface.fuzzRoughness);
}

bool MaterialSlotEnabled(MaterialInfo materialInfo, uint materialFlags, MaterialTextureSlot slot)
{
    switch (slot)
    {
    case MATERIAL_TEXTURE_SLOT_BASE_COLOR:
        return (materialFlags & MATERIAL_BASE_COLOR_TEXTURE) != 0u;
    case MATERIAL_TEXTURE_SLOT_OPACITY:
        return (materialFlags & MATERIAL_OPACITY_TEXTURE) != 0u;
    case MATERIAL_TEXTURE_SLOT_METALLIC:
        return (materialFlags & MATERIAL_METALLIC_TEXTURE) != 0u;
    case MATERIAL_TEXTURE_SLOT_ROUGHNESS:
        return (materialFlags & MATERIAL_ROUGHNESS_TEXTURE) != 0u;
    case MATERIAL_TEXTURE_SLOT_NORMAL:
        return (materialFlags & MATERIAL_NORMAL_MAP) != 0u;
    case MATERIAL_TEXTURE_SLOT_AO:
        return (materialFlags & MATERIAL_AO_TEXTURE) != 0u;
    case MATERIAL_TEXTURE_SLOT_EMISSIVE:
        return (materialFlags & MATERIAL_EMISSIVE_TEXTURE) != 0u;
    case MATERIAL_TEXTURE_SLOT_HEIGHT:
        return (materialFlags & MATERIAL_PARALLAX) != 0u &&
            (((materialFlags & MATERIAL_HEIGHT_FROM_BASE_ALPHA) == 0u) || ((materialFlags & MATERIAL_BASE_COLOR_TEXTURE) != 0u));
    default:
        return false;
    }
}

bool MaterialSlotEnabled(MaterialEvalInfo materialInfo, uint materialFlags, MaterialTextureSlot slot)
{
    return MaterialSlotEnabled((MaterialInfo)0, materialFlags, slot);
}

uint MaterialSlotUvSetIndex(MaterialInfo materialInfo, MaterialTextureSlot slot)
{
    switch (slot)
    {
    case MATERIAL_TEXTURE_SLOT_BASE_COLOR:
        return materialInfo.baseColorUvSetIndex;
    case MATERIAL_TEXTURE_SLOT_OPACITY:
        return materialInfo.opacityUvSetIndex;
    case MATERIAL_TEXTURE_SLOT_METALLIC:
        return materialInfo.metallicUvSetIndex;
    case MATERIAL_TEXTURE_SLOT_ROUGHNESS:
        return materialInfo.roughnessUvSetIndex;
    case MATERIAL_TEXTURE_SLOT_NORMAL:
        return materialInfo.normalUvSetIndex;
    case MATERIAL_TEXTURE_SLOT_AO:
        return materialInfo.aoUvSetIndex;
    case MATERIAL_TEXTURE_SLOT_EMISSIVE:
        return materialInfo.emissiveUvSetIndex;
    case MATERIAL_TEXTURE_SLOT_HEIGHT:
        return (materialInfo.materialFlags & MATERIAL_HEIGHT_FROM_BASE_ALPHA) != 0u
            ? materialInfo.baseColorUvSetIndex
            : materialInfo.heightUvSetIndex;
    default:
        return 0u;
    }
}

uint MaterialSlotUvSetIndex(MaterialEvalInfo materialInfo, MaterialTextureSlot slot)
{
    switch (slot)
    {
    case MATERIAL_TEXTURE_SLOT_BASE_COLOR:
        return materialInfo.baseColorUvSetIndex;
    case MATERIAL_TEXTURE_SLOT_OPACITY:
        return materialInfo.opacityUvSetIndex;
    case MATERIAL_TEXTURE_SLOT_METALLIC:
        return materialInfo.metallicUvSetIndex;
    case MATERIAL_TEXTURE_SLOT_ROUGHNESS:
        return materialInfo.roughnessUvSetIndex;
    case MATERIAL_TEXTURE_SLOT_NORMAL:
        return materialInfo.normalUvSetIndex;
    case MATERIAL_TEXTURE_SLOT_AO:
        return materialInfo.aoUvSetIndex;
    case MATERIAL_TEXTURE_SLOT_EMISSIVE:
        return materialInfo.emissiveUvSetIndex;
    case MATERIAL_TEXTURE_SLOT_HEIGHT:
        return (materialInfo.materialFlags & MATERIAL_HEIGHT_FROM_BASE_ALPHA) != 0u
            ? materialInfo.baseColorUvSetIndex
            : materialInfo.heightUvSetIndex;
    default:
        return 0u;
    }
}

uint MaterialSlotStreamingTextureID(MaterialInfo materialInfo, MaterialTextureSlot slot)
{
    switch (slot)
    {
    case MATERIAL_TEXTURE_SLOT_BASE_COLOR:
        return materialInfo.baseColorStreamingTextureID;
    case MATERIAL_TEXTURE_SLOT_OPACITY:
        return materialInfo.opacityStreamingTextureID;
    case MATERIAL_TEXTURE_SLOT_METALLIC:
        return materialInfo.metallicStreamingTextureID;
    case MATERIAL_TEXTURE_SLOT_ROUGHNESS:
        return materialInfo.roughnessStreamingTextureID;
    case MATERIAL_TEXTURE_SLOT_NORMAL:
        return materialInfo.normalStreamingTextureID;
    case MATERIAL_TEXTURE_SLOT_AO:
        return materialInfo.aoStreamingTextureID;
    case MATERIAL_TEXTURE_SLOT_EMISSIVE:
        return materialInfo.emissiveStreamingTextureID;
    case MATERIAL_TEXTURE_SLOT_HEIGHT:
        return (materialInfo.materialFlags & MATERIAL_HEIGHT_FROM_BASE_ALPHA) != 0u
            ? materialInfo.baseColorStreamingTextureID
            : materialInfo.heightStreamingTextureID;
    default:
        return 0u;
    }
}

uint MaterialSlotStreamingTextureID(MaterialEvalInfo materialInfo, MaterialTextureSlot slot)
{
    switch (slot)
    {
    case MATERIAL_TEXTURE_SLOT_BASE_COLOR:
        return materialInfo.baseColorStreamingTextureID;
    case MATERIAL_TEXTURE_SLOT_OPACITY:
        return materialInfo.opacityStreamingTextureID;
    case MATERIAL_TEXTURE_SLOT_METALLIC:
        return materialInfo.metallicStreamingTextureID;
    case MATERIAL_TEXTURE_SLOT_ROUGHNESS:
        return materialInfo.roughnessStreamingTextureID;
    case MATERIAL_TEXTURE_SLOT_NORMAL:
        return materialInfo.normalStreamingTextureID;
    case MATERIAL_TEXTURE_SLOT_AO:
        return materialInfo.aoStreamingTextureID;
    case MATERIAL_TEXTURE_SLOT_EMISSIVE:
        return materialInfo.emissiveStreamingTextureID;
    case MATERIAL_TEXTURE_SLOT_HEIGHT:
        return (materialInfo.materialFlags & MATERIAL_HEIGHT_FROM_BASE_ALPHA) != 0u
            ? materialInfo.baseColorStreamingTextureID
            : materialInfo.heightStreamingTextureID;
    default:
        return 0u;
    }
}

uint OpenPBRSlotStreamingTextureID(OpenPBRMaterialInfo openPBRMaterialInfo, OpenPBRTextureSlot slot)
{
    switch (slot)
    {
    case OPENPBR_TEXTURE_SLOT_COAT_COLOR:
        return openPBRMaterialInfo.coatColorStreamingTextureID;
    case OPENPBR_TEXTURE_SLOT_COAT_WEIGHT:
        return openPBRMaterialInfo.coatWeightStreamingTextureID;
    case OPENPBR_TEXTURE_SLOT_COAT_ROUGHNESS:
        return openPBRMaterialInfo.coatRoughnessStreamingTextureID;
    case OPENPBR_TEXTURE_SLOT_FUZZ_COLOR:
        return openPBRMaterialInfo.fuzzColorStreamingTextureID;
    case OPENPBR_TEXTURE_SLOT_FUZZ_WEIGHT:
        return openPBRMaterialInfo.fuzzWeightStreamingTextureID;
    case OPENPBR_TEXTURE_SLOT_FUZZ_ROUGHNESS:
        return openPBRMaterialInfo.fuzzRoughnessStreamingTextureID;
    default:
        return 0u;
    }
}

uint FindMaterialUvCacheIndex(in MaterialUvCache cache, uint uvSetIndex)
{
    [unroll]
    for (uint i = 0u; i < MATERIAL_MAX_UNIQUE_UV_SETS; ++i)
    {
        if (i >= cache.count)
        {
            break;
        }
        if (cache.samples[i].uvSetIndex == uvSetIndex)
        {
            return i;
        }
    }

    return MATERIAL_INVALID_UV_CACHE_INDEX;
}

MaterialUvBindings BuildMaterialUvBindings(MaterialInfo materialInfo, uint materialFlags, in MaterialUvCache cache)
{
    MaterialUvBindings bindings;
    InitializeMaterialUvBindings(bindings);

    uint uv0CacheIndex = FindMaterialUvCacheIndex(cache, 0u);

    [unroll]
    for (uint slot = 0u; slot < MATERIAL_TEXTURE_SLOT_COUNT; ++slot)
    {
        MaterialTextureSlot textureSlot = (MaterialTextureSlot)slot;
        if (!MaterialSlotEnabled(materialInfo, materialFlags, textureSlot))
        {
            continue;
        }

        uint cacheIndex = FindMaterialUvCacheIndex(cache, MaterialSlotUvSetIndex(materialInfo, textureSlot));
        if (cacheIndex == MATERIAL_INVALID_UV_CACHE_INDEX)
        {
            cacheIndex = uv0CacheIndex;
        }

        bindings.cacheIndexBySlot[slot] = cacheIndex;
    }

    const OpenPBRMaterialInfo openPBRMaterialInfo = LoadOpenPBRMaterialInfo(materialInfo);
    const uint uvSetIndices[OPENPBR_TEXTURE_SLOT_COUNT] = {
        openPBRMaterialInfo.coatColorUvSetIndex,
        openPBRMaterialInfo.coatWeightUvSetIndex,
        openPBRMaterialInfo.coatRoughnessUvSetIndex,
        openPBRMaterialInfo.fuzzColorUvSetIndex,
        openPBRMaterialInfo.fuzzWeightUvSetIndex,
        openPBRMaterialInfo.fuzzRoughnessUvSetIndex
    };
    const uint textureIndices[OPENPBR_TEXTURE_SLOT_COUNT] = {
        openPBRMaterialInfo.coatColorTextureIndex,
        openPBRMaterialInfo.coatWeightTextureIndex,
        openPBRMaterialInfo.coatRoughnessTextureIndex,
        openPBRMaterialInfo.fuzzColorTextureIndex,
        openPBRMaterialInfo.fuzzWeightTextureIndex,
        openPBRMaterialInfo.fuzzRoughnessTextureIndex
    };
    const uint samplerIndices[OPENPBR_TEXTURE_SLOT_COUNT] = {
        openPBRMaterialInfo.coatColorSamplerIndex,
        openPBRMaterialInfo.coatWeightSamplerIndex,
        openPBRMaterialInfo.coatRoughnessSamplerIndex,
        openPBRMaterialInfo.fuzzColorSamplerIndex,
        openPBRMaterialInfo.fuzzWeightSamplerIndex,
        openPBRMaterialInfo.fuzzRoughnessSamplerIndex
    };

    [unroll]
    for (uint slot = 0u; slot < OPENPBR_TEXTURE_SLOT_COUNT; ++slot)
    {
        if (!HasOpenPBRTexture(textureIndices[slot], samplerIndices[slot]))
        {
            continue;
        }

        uint cacheIndex = FindMaterialUvCacheIndex(cache, uvSetIndices[slot]);
        if (cacheIndex == MATERIAL_INVALID_UV_CACHE_INDEX)
        {
            cacheIndex = uv0CacheIndex;
        }

        bindings.openPBRCacheIndexBySlot[slot] = cacheIndex;
    }

    if ((materialFlags & MATERIAL_NORMAL_MAP) != 0u)
    {
        bindings.tbnCacheIndex = bindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_NORMAL];
        bindings.hasTbnSource = bindings.tbnCacheIndex != MATERIAL_INVALID_UV_CACHE_INDEX;
    }
    else if ((materialFlags & MATERIAL_PARALLAX) != 0u)
    {
        bindings.tbnCacheIndex = bindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_HEIGHT];
        bindings.hasTbnSource = bindings.tbnCacheIndex != MATERIAL_INVALID_UV_CACHE_INDEX;
    }

    if ((materialFlags & MATERIAL_PARALLAX) != 0u)
    {
        bindings.heightCacheIndex = bindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_HEIGHT];
        bindings.hasHeightSource = bindings.heightCacheIndex != MATERIAL_INVALID_UV_CACHE_INDEX;
    }

    return bindings;
}

MaterialUvSample GetBoundUvSample(in MaterialUvCache cache, in MaterialUvBindings bindings, MaterialTextureSlot slot)
{
    uint cacheIndex = bindings.cacheIndexBySlot[slot];
    if (cacheIndex == MATERIAL_INVALID_UV_CACHE_INDEX || cacheIndex >= cache.count)
    {
        return MakeDefaultMaterialUvSample();
    }

    return cache.samples[cacheIndex];
}

MaterialUvSample ResolveMaterialUvSample(
    in MaterialUvCache cache,
    in MaterialUvBindings bindings,
    MaterialTextureSlot slot)
{
    return GetBoundUvSample(cache, bindings, slot);
}

MaterialUvSample ResolveMaterialUvSample(
    in MaterialUvCache cache,
    in MaterialUvBindings bindings,
    MaterialTextureSlot slot,
    bool hasParallaxResolvedUv,
    float2 parallaxUv,
    float2 parallaxDUdx,
    float2 parallaxDUdy)
{
    MaterialUvSample sample = GetBoundUvSample(cache, bindings, slot);
    if (hasParallaxResolvedUv && bindings.cacheIndexBySlot[slot] == bindings.heightCacheIndex)
    {
        sample.uv = parallaxUv;
        sample.dUVdx = parallaxDUdx;
        sample.dUVdy = parallaxDUdy;
    }

    return sample;
}

float3x3 BuildMaterialTBN(
    in MaterialUvCache cache,
    in MaterialUvBindings bindings,
    in float3 normalWSBase,
    in float3 dpdx,
    in float3 dpdy)
{
    MaterialUvSample tbnUv = cache.samples[bindings.tbnCacheIndex];
    return cotangent_frame_from_derivs(normalWSBase.xyz, dpdx, dpdy, tbnUv.dUVdx, tbnUv.dUVdy);
}

float3x3 BuildMaterialTBNFromVertexTangent(
    in MaterialUvCache cache,
    in MaterialUvBindings bindings,
    in float3 normalWSBase,
    in float3 dpdx,
    in float3 dpdy,
    in float4 tangentWS)
{
    if (abs(tangentWS.w) > 0.5f && dot(tangentWS.xyz, tangentWS.xyz) > 1.0e-8f)
    {
        const float3 N = normalize(normalWSBase);
        const float3 T = normalize(tangentWS.xyz - N * dot(N, tangentWS.xyz));
        const float3 B = normalize(cross(T, N)) * (tangentWS.w < 0.0f ? -1.0f : 1.0f);
        return float3x3(T, B, N);
    }

    return BuildMaterialTBN(cache, bindings, normalWSBase, dpdx, dpdy);
}

void AppendOpenPBRForwardUvSamples(
    inout MaterialUvCache cache,
    in VisBufferPSInput input,
    in OpenPBRMaterialInfo openPBRMaterialInfo);

#if defined(CLOD_AVBOIT_FORWARD_TRANSPARENT)
void BuildForwardTransparentMaterialUvData(
    in VisBufferPSInput input,
    in MaterialInfo materialInfo,
    uint materialFlags,
    out MaterialUvCache cache,
    out MaterialUvBindings bindings)
{
    cache = (MaterialUvCache)0;
    InitializeMaterialUvBindings(bindings);

    uint cacheIndexByUvSet[MATERIAL_MAX_UNIQUE_UV_SETS];

    [unroll]
    for (uint uvSetIndex = 0u; uvSetIndex < MATERIAL_MAX_UNIQUE_UV_SETS; ++uvSetIndex)
    {
        cacheIndexByUvSet[uvSetIndex] = MATERIAL_INVALID_UV_CACHE_INDEX;
    }

    const float4 uvSet01 = input.uvSet01;
    const float4 uvSet23 = input.uvSet23;
    const float4 uvSet45 = input.uvSet45;
    const float4 uvSet67 = input.uvSet67;
    const float4 uvSet01Dx = ddx(uvSet01);
    const float4 uvSet01Dy = ddy(uvSet01);
    const float4 uvSet23Dx = ddx(uvSet23);
    const float4 uvSet23Dy = ddy(uvSet23);
    const float4 uvSet45Dx = ddx(uvSet45);
    const float4 uvSet45Dy = ddy(uvSet45);
    const float4 uvSet67Dx = ddx(uvSet67);
    const float4 uvSet67Dy = ddy(uvSet67);

    [unroll]
    for (uint slot = 0u; slot < MATERIAL_TEXTURE_SLOT_COUNT; ++slot)
    {
        const MaterialTextureSlot textureSlot = (MaterialTextureSlot)slot;
        if (!MaterialSlotEnabled(materialInfo, materialFlags, textureSlot))
        {
            continue;
        }

        const uint uvSetIndex = MaterialSlotUvSetIndex(materialInfo, textureSlot);
        if (uvSetIndex >= MATERIAL_MAX_UNIQUE_UV_SETS || cacheIndexByUvSet[uvSetIndex] != MATERIAL_INVALID_UV_CACHE_INDEX)
        {
            continue;
        }

        const uint cacheIndex = cache.count;
        MaterialUvSample sample = (MaterialUvSample)0;
        sample.uvSetIndex = uvSetIndex;

        switch (uvSetIndex)
        {
        case 0u:
            sample.uv = uvSet01.xy;
            sample.dUVdx = uvSet01Dx.xy;
            sample.dUVdy = uvSet01Dy.xy;
            break;
        case 1u:
            sample.uv = uvSet01.zw;
            sample.dUVdx = uvSet01Dx.zw;
            sample.dUVdy = uvSet01Dy.zw;
            break;
        case 2u:
            sample.uv = uvSet23.xy;
            sample.dUVdx = uvSet23Dx.xy;
            sample.dUVdy = uvSet23Dy.xy;
            break;
        case 3u:
            sample.uv = uvSet23.zw;
            sample.dUVdx = uvSet23Dx.zw;
            sample.dUVdy = uvSet23Dy.zw;
            break;
        case 4u:
            sample.uv = uvSet45.xy;
            sample.dUVdx = uvSet45Dx.xy;
            sample.dUVdy = uvSet45Dy.xy;
            break;
        case 5u:
            sample.uv = uvSet45.zw;
            sample.dUVdx = uvSet45Dx.zw;
            sample.dUVdy = uvSet45Dy.zw;
            break;
        case 6u:
            sample.uv = uvSet67.xy;
            sample.dUVdx = uvSet67Dx.xy;
            sample.dUVdy = uvSet67Dy.xy;
            break;
        default:
            sample.uv = uvSet67.zw;
            sample.dUVdx = uvSet67Dx.zw;
            sample.dUVdy = uvSet67Dy.zw;
            break;
        }

        cache.samples[cacheIndex] = sample;
        cacheIndexByUvSet[uvSetIndex] = cacheIndex;
        cache.count = cacheIndex + 1u;
    }

    AppendOpenPBRForwardUvSamples(cache, input, LoadOpenPBRMaterialInfo(materialInfo));

    [unroll]
    for (uint cacheIndex = 0u; cacheIndex < MATERIAL_MAX_UNIQUE_UV_SETS; ++cacheIndex)
    {
        if (cacheIndex >= cache.count)
        {
            break;
        }

        const uint uvSetIndex = cache.samples[cacheIndex].uvSetIndex;
        if (uvSetIndex < MATERIAL_MAX_UNIQUE_UV_SETS)
        {
            cacheIndexByUvSet[uvSetIndex] = cacheIndex;
        }
    }

    const uint uv0CacheIndex = cacheIndexByUvSet[0u];

    const OpenPBRMaterialInfo openPBRMaterialInfo = LoadOpenPBRMaterialInfo(materialInfo);
    const uint openPBRUvSetIndices[OPENPBR_TEXTURE_SLOT_COUNT] = {
        openPBRMaterialInfo.coatColorUvSetIndex,
        openPBRMaterialInfo.coatWeightUvSetIndex,
        openPBRMaterialInfo.coatRoughnessUvSetIndex,
        openPBRMaterialInfo.fuzzColorUvSetIndex,
        openPBRMaterialInfo.fuzzWeightUvSetIndex,
        openPBRMaterialInfo.fuzzRoughnessUvSetIndex
    };
    const uint openPBRTextureIndices[OPENPBR_TEXTURE_SLOT_COUNT] = {
        openPBRMaterialInfo.coatColorTextureIndex,
        openPBRMaterialInfo.coatWeightTextureIndex,
        openPBRMaterialInfo.coatRoughnessTextureIndex,
        openPBRMaterialInfo.fuzzColorTextureIndex,
        openPBRMaterialInfo.fuzzWeightTextureIndex,
        openPBRMaterialInfo.fuzzRoughnessTextureIndex
    };
    const uint openPBRSamplerIndices[OPENPBR_TEXTURE_SLOT_COUNT] = {
        openPBRMaterialInfo.coatColorSamplerIndex,
        openPBRMaterialInfo.coatWeightSamplerIndex,
        openPBRMaterialInfo.coatRoughnessSamplerIndex,
        openPBRMaterialInfo.fuzzColorSamplerIndex,
        openPBRMaterialInfo.fuzzWeightSamplerIndex,
        openPBRMaterialInfo.fuzzRoughnessSamplerIndex
    };

    [unroll]
    for (uint slot = 0u; slot < MATERIAL_TEXTURE_SLOT_COUNT; ++slot)
    {
        const MaterialTextureSlot textureSlot = (MaterialTextureSlot)slot;
        if (!MaterialSlotEnabled(materialInfo, materialFlags, textureSlot))
        {
            continue;
        }

        uint cacheIndex = MATERIAL_INVALID_UV_CACHE_INDEX;
        const uint uvSetIndex = MaterialSlotUvSetIndex(materialInfo, textureSlot);
        if (uvSetIndex < MATERIAL_MAX_UNIQUE_UV_SETS)
        {
            cacheIndex = cacheIndexByUvSet[uvSetIndex];
        }
        if (cacheIndex == MATERIAL_INVALID_UV_CACHE_INDEX)
        {
            cacheIndex = uv0CacheIndex;
        }

        bindings.cacheIndexBySlot[slot] = cacheIndex;
    }

    [unroll]
    for (uint slot = 0u; slot < OPENPBR_TEXTURE_SLOT_COUNT; ++slot)
    {
        if (!HasOpenPBRTexture(openPBRTextureIndices[slot], openPBRSamplerIndices[slot]))
        {
            continue;
        }

        uint cacheIndex = MATERIAL_INVALID_UV_CACHE_INDEX;
        const uint uvSetIndex = openPBRUvSetIndices[slot];
        if (uvSetIndex < MATERIAL_MAX_UNIQUE_UV_SETS)
        {
            cacheIndex = cacheIndexByUvSet[uvSetIndex];
        }
        if (cacheIndex == MATERIAL_INVALID_UV_CACHE_INDEX)
        {
            cacheIndex = uv0CacheIndex;
        }

        bindings.openPBRCacheIndexBySlot[slot] = cacheIndex;
    }

    if ((materialFlags & MATERIAL_NORMAL_MAP) != 0u)
    {
        bindings.tbnCacheIndex = bindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_NORMAL];
        bindings.hasTbnSource = bindings.tbnCacheIndex != MATERIAL_INVALID_UV_CACHE_INDEX;
    }
    else if ((materialFlags & MATERIAL_PARALLAX) != 0u)
    {
        bindings.tbnCacheIndex = bindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_HEIGHT];
        bindings.hasTbnSource = bindings.tbnCacheIndex != MATERIAL_INVALID_UV_CACHE_INDEX;
    }

    if ((materialFlags & MATERIAL_PARALLAX) != 0u)
    {
        bindings.heightCacheIndex = bindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_HEIGHT];
        bindings.hasHeightSource = bindings.heightCacheIndex != MATERIAL_INVALID_UV_CACHE_INDEX;
    }
}

float2 GetForwardTransparentUvSet(in VisBufferPSInput input, uint uvSetIndex)
{
    switch (uvSetIndex)
    {
    case 0u:
        return input.uvSet01.xy;
    case 1u:
        return input.uvSet01.zw;
    case 2u:
        return input.uvSet23.xy;
    case 3u:
        return input.uvSet23.zw;
    case 4u:
        return input.uvSet45.xy;
    case 5u:
        return input.uvSet45.zw;
    case 6u:
        return input.uvSet67.xy;
    case 7u:
        return input.uvSet67.zw;
    default:
        return float2(0.0f, 0.0f);
    }
}

void AppendForwardMaterialUvSample(inout MaterialUvCache cache, uint uvSetIndex, in VisBufferPSInput input)
{
    if (cache.count >= MATERIAL_MAX_UNIQUE_UV_SETS)
    {
        return;
    }

    const float2 uv = GetForwardTransparentUvSet(input, uvSetIndex);

    MaterialUvSample sample = (MaterialUvSample)0;
    sample.uvSetIndex = uvSetIndex;
    sample.uv = uv;
    sample.dUVdx = ddx(uv);
    sample.dUVdy = ddy(uv);
    cache.samples[cache.count] = sample;
    cache.count++;
}

void AppendOpenPBRForwardUvSamples(
    inout MaterialUvCache cache,
    in VisBufferPSInput input,
    in OpenPBRMaterialInfo openPBRMaterialInfo)
{
    const uint uvSetIndices[6] = {
        openPBRMaterialInfo.coatColorUvSetIndex,
        openPBRMaterialInfo.coatWeightUvSetIndex,
        openPBRMaterialInfo.coatRoughnessUvSetIndex,
        openPBRMaterialInfo.fuzzColorUvSetIndex,
        openPBRMaterialInfo.fuzzWeightUvSetIndex,
        openPBRMaterialInfo.fuzzRoughnessUvSetIndex
    };

    const uint textureIndices[6] = {
        openPBRMaterialInfo.coatColorTextureIndex,
        openPBRMaterialInfo.coatWeightTextureIndex,
        openPBRMaterialInfo.coatRoughnessTextureIndex,
        openPBRMaterialInfo.fuzzColorTextureIndex,
        openPBRMaterialInfo.fuzzWeightTextureIndex,
        openPBRMaterialInfo.fuzzRoughnessTextureIndex
    };

    const uint samplerIndices[6] = {
        openPBRMaterialInfo.coatColorSamplerIndex,
        openPBRMaterialInfo.coatWeightSamplerIndex,
        openPBRMaterialInfo.coatRoughnessSamplerIndex,
        openPBRMaterialInfo.fuzzColorSamplerIndex,
        openPBRMaterialInfo.fuzzWeightSamplerIndex,
        openPBRMaterialInfo.fuzzRoughnessSamplerIndex
    };

    [unroll]
    for (uint i = 0u; i < 6u; ++i)
    {
        if (!HasOpenPBRTexture(textureIndices[i], samplerIndices[i]))
        {
            continue;
        }

        const uint uvSetIndex = uvSetIndices[i];
        if (FindMaterialUvCacheIndex(cache, uvSetIndex) != MATERIAL_INVALID_UV_CACHE_INDEX)
        {
            continue;
        }

        AppendForwardMaterialUvSample(cache, uvSetIndex, input);
    }
}

MaterialUvCache BuildMaterialUvCacheFromForwardInput(
    in VisBufferPSInput input,
    in MaterialInfo materialInfo,
    uint materialFlags)
{
    MaterialUvCache cache = (MaterialUvCache)0;

    [unroll]
    for (uint slot = 0u; slot < MATERIAL_TEXTURE_SLOT_COUNT; ++slot)
    {
        const MaterialTextureSlot textureSlot = (MaterialTextureSlot)slot;
        if (!MaterialSlotEnabled(materialInfo, materialFlags, textureSlot))
        {
            continue;
        }

        const uint uvSetIndex = MaterialSlotUvSetIndex(materialInfo, textureSlot);
        if (FindMaterialUvCacheIndex(cache, uvSetIndex) != MATERIAL_INVALID_UV_CACHE_INDEX)
        {
            continue;
        }

        AppendForwardMaterialUvSample(cache, uvSetIndex, input);
    }

    AppendOpenPBRForwardUvSamples(cache, input, LoadOpenPBRMaterialInfo(materialInfo));

    return cache;
}
#endif

void SampleMaterialFromUvCache(
    in MaterialUvCache uvCache,
    in MaterialUvBindings uvBindings,
    in float3 normalWSBase,
    in float3 posWS,
    in float3 vertexColorMultiplier,
    in MaterialInfo materialInfo,
    in uint materialFlags,
    in float3 dpdx,
    in float3 dpdy,
    out MaterialInputs ret)
{
    InitializeMaterialSelectedMipDebug(ret);

    MaterialUvSample baseColorUv = GetBoundUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_BASE_COLOR);
    MaterialUvSample opacityUv = GetBoundUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_OPACITY);
    MaterialUvSample metallicUv = GetBoundUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_METALLIC);
    MaterialUvSample roughnessUv = GetBoundUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_ROUGHNESS);
    MaterialUvSample normalUv = GetBoundUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_NORMAL);
    MaterialUvSample aoUv = GetBoundUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_AO);
    MaterialUvSample emissiveUv = GetBoundUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_EMISSIVE);

    float3x3 TBN = (float3x3)0.0f;
    if (uvBindings.hasTbnSource)
    {
        MaterialUvSample tbnUv = uvCache.samples[uvBindings.tbnCacheIndex];
        TBN = cotangent_frame_from_derivs(normalWSBase.xyz, dpdx, dpdy, tbnUv.dUVdx, tbnUv.dUVdy);
    }

    float2 parallaxUv = float2(0.0f, 0.0f);
    float2 parallaxDUdx = float2(0.0f, 0.0f);
    float2 parallaxDUdy = float2(0.0f, 0.0f);
    bool hasParallaxResolvedUv = false;

#if defined(PSO_PARALLAX)
    if (uvBindings.hasHeightSource && uvBindings.hasTbnSource)
    {
        MaterialUvSample heightUv = uvCache.samples[uvBindings.heightCacheIndex];
        ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[0];
        StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
        Camera mainCamera = cameras[perFrameBuffer.mainCameraIndex];

        float3 viewDir = normalize(mainCamera.positionWorldSpace.xyz - posWS.xyz);

        if (perFrameBuffer.parallaxOcclusionMappingEnabled != 0u)
        {
            float3 uvh;
            if ((materialFlags & MATERIAL_HEIGHT_FROM_BASE_ALPHA) != 0u)
            {
                Texture2D<float4> parallaxTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.baseColorTextureIndex)];
                SamplerState parallaxSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.baseColorSamplerIndex)];
                uvh = getParallaxOcclusionMappingCoordsAndHeight(
                    parallaxTexture,
                    parallaxSamplerState,
                    3u,
                    TBN,
                    heightUv.uv,
                    viewDir,
                    materialInfo.heightMapScale,
                    16u,
                    heightUv.dUVdx,
                    heightUv.dUVdy);
            }
            else
            {
                Texture2D<float> parallaxTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.heightMapIndex)];
                SamplerState parallaxSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.heightSamplerIndex)];
                uvh = getParallaxOcclusionMappingCoordsAndHeight(
                    parallaxTexture,
                    parallaxSamplerState,
                    TBN,
                    heightUv.uv,
                    viewDir,
                    materialInfo.heightMapScale,
                    16u,
                    heightUv.dUVdx,
                    heightUv.dUVdy);
            }

            parallaxUv = uvh.xy;
            parallaxDUdx = heightUv.dUVdx;
            parallaxDUdy = heightUv.dUVdy;
            hasParallaxResolvedUv = true;
            ret.parallaxApplied = 1u;
        }
    }
#endif

    float4 baseColor = materialInfo.baseColorFactor;

#if defined(PSO_BASE_COLOR_TEXTURE)
    Texture2D<float4> baseColorTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.baseColorTextureIndex)];
    SamplerState baseColorSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.baseColorSamplerIndex)];
    float2 baseColorSampleUv = baseColorUv.uv;
    float2 baseColorDUdx = baseColorUv.dUVdx;
    float2 baseColorDUdy = baseColorUv.dUVdy;
    if (hasParallaxResolvedUv && uvBindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_BASE_COLOR] == uvBindings.heightCacheIndex)
    {
        baseColorSampleUv = parallaxUv;
        baseColorDUdx = parallaxDUdx;
        baseColorDUdy = parallaxDUdy;
    }
    float4 sampledColor = SampleMaterialTexture2DGrad(baseColorTexture, baseColorSamplerState, materialInfo.baseColorStreamingTextureID, baseColorSampleUv, baseColorDUdx, baseColorDUdy, ret);
    baseColor *= sampledColor;
#endif

#if defined(PSO_OPACITY_TEXTURE)
    Texture2D<float4> opacityTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.opacityTextureIndex)];
    SamplerState opacitySamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.opacitySamplerIndex)];
    float2 opacitySampleUv = opacityUv.uv;
    float2 opacityDUdx = opacityUv.dUVdx;
    float2 opacityDUdy = opacityUv.dUVdy;
    if (hasParallaxResolvedUv && uvBindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_OPACITY] == uvBindings.heightCacheIndex)
    {
        opacitySampleUv = parallaxUv;
        opacityDUdx = parallaxDUdx;
        opacityDUdy = parallaxDUdy;
    }
    float4 opacitySample = SampleMaterialTexture2DGrad(opacityTexture, opacitySamplerState, materialInfo.opacityStreamingTextureID, opacitySampleUv, opacityDUdx, opacityDUdy, ret);
    baseColor.a *= opacitySample.a;
#endif

    float metallic = materialInfo.metallicFactor;
    float roughness = materialInfo.roughnessFactor;

#if defined(PSO_METALLIC_TEXTURE)
    Texture2D<float4> metallicTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.metallicTextureIndex)];
    SamplerState metallicSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.metallicSamplerIndex)];

    float2 metallicSampleUv = metallicUv.uv;
    float2 metallicDUdx = metallicUv.dUVdx;
    float2 metallicDUdy = metallicUv.dUVdy;
    if (hasParallaxResolvedUv && uvBindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_METALLIC] == uvBindings.heightCacheIndex)
    {
        metallicSampleUv = parallaxUv;
        metallicDUdx = parallaxDUdx;
        metallicDUdy = parallaxDUdy;
    }

    float4 metallicSample = SampleMaterialTexture2DGrad(metallicTexture, metallicSamplerState, materialInfo.metallicStreamingTextureID, metallicSampleUv, metallicDUdx, metallicDUdy, ret);
    metallic = DynamicSwizzle(metallicSample, materialInfo.metallicChannel) * materialInfo.metallicFactor;
#endif

#if defined(PSO_ROUGHNESS_TEXTURE)
    Texture2D<float4> roughnessTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.roughnessTextureIndex)];
    SamplerState roughnessSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.roughnessSamplerIndex)];

    float2 roughnessSampleUv = roughnessUv.uv;
    float2 roughnessDUdx = roughnessUv.dUVdx;
    float2 roughnessDUdy = roughnessUv.dUVdy;
    if (hasParallaxResolvedUv && uvBindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_ROUGHNESS] == uvBindings.heightCacheIndex)
    {
        roughnessSampleUv = parallaxUv;
        roughnessDUdx = parallaxDUdx;
        roughnessDUdy = parallaxDUdy;
    }

    float4 roughnessSample = SampleMaterialTexture2DGrad(roughnessTexture, roughnessSamplerState, materialInfo.roughnessStreamingTextureID, roughnessSampleUv, roughnessDUdx, roughnessDUdy, ret);
    roughness = DynamicSwizzle(roughnessSample, materialInfo.roughnessChannel) * materialInfo.roughnessFactor;
#endif

    float3 normalWS = normalWSBase;

#if defined(PSO_NORMAL_MAP)
    if (uvBindings.hasTbnSource)
    {
        Texture2D<float4> normalTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.normalTextureIndex)];
        SamplerState normalSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.normalSamplerIndex)];
        float2 normalSampleUv = normalUv.uv;
        float2 normalDUdx = normalUv.dUVdx;
        float2 normalDUdy = normalUv.dUVdy;
        if (hasParallaxResolvedUv && uvBindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_NORMAL] == uvBindings.heightCacheIndex)
        {
            normalSampleUv = parallaxUv;
            normalDUdx = parallaxDUdx;
            normalDUdy = parallaxDUdy;
        }

        float4 textureNormal = SampleMaterialTexture2DGrad(normalTexture, normalSamplerState, materialInfo.normalStreamingTextureID, normalSampleUv, normalDUdx, normalDUdy, ret);
        float3 tangentSpaceNormal = DecodeMaterialNormalSample(textureNormal, materialInfo.normalChannels, materialFlags);

        normalWS = normalize(mul(tangentSpaceNormal, TBN));
    }
#endif

    float ao = 1.0f;
#if defined(PSO_AO_TEXTURE)
    Texture2D<float4> aoTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.aoMapIndex)];
    SamplerState aoSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.aoSamplerIndex)];
    float2 aoSampleUv = aoUv.uv;
    float2 aoDUdx = aoUv.dUVdx;
    float2 aoDUdy = aoUv.dUVdy;
    if (hasParallaxResolvedUv && uvBindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_AO] == uvBindings.heightCacheIndex)
    {
        aoSampleUv = parallaxUv;
        aoDUdx = parallaxDUdx;
        aoDUdy = parallaxDUdy;
    }
    float4 aoSample = SampleMaterialTexture2DGrad(aoTexture, aoSamplerState, materialInfo.aoStreamingTextureID, aoSampleUv, aoDUdx, aoDUdy, ret);
    ao = DynamicSwizzle(aoSample, materialInfo.aoChannel);
#endif

    float3 emissive = materialInfo.emissiveFactor.rgb;
#if defined(PSO_EMISSIVE_TEXTURE)
    Texture2D<float4> emissiveTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.emissiveTextureIndex)];
    SamplerState emissiveSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.emissiveSamplerIndex)];
    float2 emissiveSampleUv = emissiveUv.uv;
    float2 emissiveDUdx = emissiveUv.dUVdx;
    float2 emissiveDUdy = emissiveUv.dUVdy;
    if (hasParallaxResolvedUv && uvBindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_EMISSIVE] == uvBindings.heightCacheIndex)
    {
        emissiveSampleUv = parallaxUv;
        emissiveDUdx = parallaxDUdx;
        emissiveDUdy = parallaxDUdy;
    }
    float4 emissiveSample = SampleMaterialTexture2DGrad(emissiveTexture, emissiveSamplerState, materialInfo.emissiveStreamingTextureID, emissiveSampleUv, emissiveDUdx, emissiveDUdy, ret);
    emissive = float3(
        DynamicSwizzle(emissiveSample, materialInfo.emissiveChannels.x),
        DynamicSwizzle(emissiveSample, materialInfo.emissiveChannels.y),
        DynamicSwizzle(emissiveSample, materialInfo.emissiveChannels.z)) * materialInfo.emissiveFactor.rgb;
#endif

    const OpenPBRMaterialInfo openPBRMaterialInfo = LoadOpenPBRMaterialInfo(materialInfo);
    OpenPBRSurfaceSample openPBRSurface = ResolveCanonicalOpenPBRSurface(
        materialInfo,
        openPBRMaterialInfo,
        baseColor.rgb * vertexColorMultiplier,
        metallic,
        roughness,
        baseColor.a,
        emissive);
    ApplyOpenPBRTextureSamplingSpecialized(
        uvCache,
        uvBindings,
        hasParallaxResolvedUv,
        materialInfo.heightUvSetIndex,
        parallaxUv,
        parallaxDUdx,
        parallaxDUdy,
        openPBRMaterialInfo,
        openPBRSurface,
        ret);
    PopulateLegacyMaterialInputsFromOpenPBRSurface(openPBRSurface, normalWS, ao, ret);
}

void SampleMaterialEvalFromUvCache(
    in MaterialUvCache uvCache,
    in MaterialUvBindings uvBindings,
    in float3 normalWSBase,
    in float3 posWS,
    in float3 vertexColorMultiplier,
    in MaterialEvalInfo materialInfo,
    in uint materialFlags,
    in float3 dpdx,
    in float3 dpdy,
    out MaterialInputs ret)
{
    InitializeMaterialSelectedMipDebug(ret);

    float2 parallaxUv = float2(0.0f, 0.0f);
    float2 parallaxDUdx = float2(0.0f, 0.0f);
    float2 parallaxDUdy = float2(0.0f, 0.0f);
    bool hasParallaxResolvedUv = false;

#if defined(PSO_PARALLAX)
    if (uvBindings.hasHeightSource && uvBindings.hasTbnSource)
    {
        const float3x3 parallaxTBN = BuildMaterialTBN(uvCache, uvBindings, normalWSBase, dpdx, dpdy);
        const MaterialUvSample heightUv = GetBoundUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_HEIGHT);
        ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[0];
        StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
        Camera mainCamera = cameras[perFrameBuffer.mainCameraIndex];

        float3 viewDir = normalize(mainCamera.positionWorldSpace.xyz - posWS.xyz);

        if (perFrameBuffer.parallaxOcclusionMappingEnabled != 0u)
        {
            float3 uvh;
            if ((materialFlags & MATERIAL_HEIGHT_FROM_BASE_ALPHA) != 0u)
            {
                Texture2D<float4> parallaxTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.baseColorTextureIndex)];
                SamplerState parallaxSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.baseColorSamplerIndex)];
                uvh = getParallaxOcclusionMappingCoordsAndHeight(
                    parallaxTexture,
                    parallaxSamplerState,
                    3u,
                    parallaxTBN,
                    heightUv.uv,
                    viewDir,
                    materialInfo.heightMapScale,
                    16u,
                    heightUv.dUVdx,
                    heightUv.dUVdy);
            }
            else
            {
                Texture2D<float> parallaxTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.heightMapIndex)];
                SamplerState parallaxSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.heightSamplerIndex)];
                uvh = getParallaxOcclusionMappingCoordsAndHeight(
                    parallaxTexture,
                    parallaxSamplerState,
                    parallaxTBN,
                    heightUv.uv,
                    viewDir,
                    materialInfo.heightMapScale,
                    16u,
                    heightUv.dUVdx,
                    heightUv.dUVdy);
            }

            parallaxUv = uvh.xy;
            parallaxDUdx = heightUv.dUVdx;
            parallaxDUdy = heightUv.dUVdy;
            hasParallaxResolvedUv = true;
            ret.parallaxApplied = 1u;
        }
    }
#endif

    float4 baseColor = materialInfo.baseColorFactor;

#if defined(PSO_BASE_COLOR_TEXTURE)
    Texture2D<float4> baseColorTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.baseColorTextureIndex)];
    SamplerState baseColorSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.baseColorSamplerIndex)];
#if defined(PSO_PARALLAX)
    const MaterialUvSample baseColorUv = ResolveMaterialUvSample(
        uvCache,
        uvBindings,
        MATERIAL_TEXTURE_SLOT_BASE_COLOR,
        hasParallaxResolvedUv,
        parallaxUv,
        parallaxDUdx,
        parallaxDUdy);
#else
    const MaterialUvSample baseColorUv = ResolveMaterialUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_BASE_COLOR);
#endif
    float4 sampledColor = SampleMaterialTexture2DGrad(baseColorTexture, baseColorSamplerState, materialInfo.baseColorStreamingTextureID, baseColorUv.uv, baseColorUv.dUVdx, baseColorUv.dUVdy, ret);
    baseColor *= sampledColor;
#endif

#if defined(PSO_OPACITY_TEXTURE)
    Texture2D<float4> opacityTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.opacityTextureIndex)];
    SamplerState opacitySamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.opacitySamplerIndex)];
#if defined(PSO_PARALLAX)
    const MaterialUvSample opacityUv = ResolveMaterialUvSample(
        uvCache,
        uvBindings,
        MATERIAL_TEXTURE_SLOT_OPACITY,
        hasParallaxResolvedUv,
        parallaxUv,
        parallaxDUdx,
        parallaxDUdy);
#else
    const MaterialUvSample opacityUv = ResolveMaterialUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_OPACITY);
#endif
    float4 opacitySample = SampleMaterialTexture2DGrad(opacityTexture, opacitySamplerState, materialInfo.opacityStreamingTextureID, opacityUv.uv, opacityUv.dUVdx, opacityUv.dUVdy, ret);
    baseColor.a *= opacitySample.a;
#endif

    float metallic = materialInfo.metallicFactor;
    float roughness = materialInfo.roughnessFactor;

#if defined(PSO_METALLIC_TEXTURE)
    Texture2D<float4> metallicTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.metallicTextureIndex)];
    SamplerState metallicSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.metallicSamplerIndex)];
#if defined(PSO_PARALLAX)
    const MaterialUvSample metallicUv = ResolveMaterialUvSample(
        uvCache,
        uvBindings,
        MATERIAL_TEXTURE_SLOT_METALLIC,
        hasParallaxResolvedUv,
        parallaxUv,
        parallaxDUdx,
        parallaxDUdy);
#else
    const MaterialUvSample metallicUv = ResolveMaterialUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_METALLIC);
#endif
    float4 metallicSample = SampleMaterialTexture2DGrad(metallicTexture, metallicSamplerState, materialInfo.metallicStreamingTextureID, metallicUv.uv, metallicUv.dUVdx, metallicUv.dUVdy, ret);
    metallic = DynamicSwizzle(metallicSample, materialInfo.metallicChannel) * materialInfo.metallicFactor;
#endif

#if defined(PSO_ROUGHNESS_TEXTURE)
    Texture2D<float4> roughnessTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.roughnessTextureIndex)];
    SamplerState roughnessSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.roughnessSamplerIndex)];
#if defined(PSO_PARALLAX)
    const MaterialUvSample roughnessUv = ResolveMaterialUvSample(
        uvCache,
        uvBindings,
        MATERIAL_TEXTURE_SLOT_ROUGHNESS,
        hasParallaxResolvedUv,
        parallaxUv,
        parallaxDUdx,
        parallaxDUdy);
#else
    const MaterialUvSample roughnessUv = ResolveMaterialUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_ROUGHNESS);
#endif
    float4 roughnessSample = SampleMaterialTexture2DGrad(roughnessTexture, roughnessSamplerState, materialInfo.roughnessStreamingTextureID, roughnessUv.uv, roughnessUv.dUVdx, roughnessUv.dUVdy, ret);
    roughness = DynamicSwizzle(roughnessSample, materialInfo.roughnessChannel) * materialInfo.roughnessFactor;
#endif

    float3 normalWS = normalWSBase;

#if defined(PSO_NORMAL_MAP)
    if (uvBindings.hasTbnSource)
    {
        const float3x3 normalTBN = BuildMaterialTBN(uvCache, uvBindings, normalWSBase, dpdx, dpdy);
        Texture2D<float4> normalTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.normalTextureIndex)];
        SamplerState normalSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.normalSamplerIndex)];
#if defined(PSO_PARALLAX)
        const MaterialUvSample normalUv = ResolveMaterialUvSample(
            uvCache,
            uvBindings,
            MATERIAL_TEXTURE_SLOT_NORMAL,
            hasParallaxResolvedUv,
            parallaxUv,
            parallaxDUdx,
            parallaxDUdy);
#else
        const MaterialUvSample normalUv = ResolveMaterialUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_NORMAL);
#endif
        float4 textureNormal = SampleMaterialTexture2DGrad(normalTexture, normalSamplerState, materialInfo.normalStreamingTextureID, normalUv.uv, normalUv.dUVdx, normalUv.dUVdy, ret);
        float3 tangentSpaceNormal = DecodeMaterialNormalSample(textureNormal, materialInfo.normalChannels, materialFlags);

        normalWS = normalize(mul(tangentSpaceNormal, normalTBN));
    }
#endif

    float ao = 1.0f;
#if defined(PSO_AO_TEXTURE)
    Texture2D<float4> aoTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.aoMapIndex)];
    SamplerState aoSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.aoSamplerIndex)];
#if defined(PSO_PARALLAX)
    const MaterialUvSample aoUv = ResolveMaterialUvSample(
        uvCache,
        uvBindings,
        MATERIAL_TEXTURE_SLOT_AO,
        hasParallaxResolvedUv,
        parallaxUv,
        parallaxDUdx,
        parallaxDUdy);
#else
    const MaterialUvSample aoUv = ResolveMaterialUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_AO);
#endif
    float4 aoSample = SampleMaterialTexture2DGrad(aoTexture, aoSamplerState, materialInfo.aoStreamingTextureID, aoUv.uv, aoUv.dUVdx, aoUv.dUVdy, ret);
    ao = DynamicSwizzle(aoSample, materialInfo.aoChannel);
#endif

    float3 emissive = materialInfo.emissiveFactor.rgb;
#if defined(PSO_EMISSIVE_TEXTURE)
    Texture2D<float4> emissiveTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.emissiveTextureIndex)];
    SamplerState emissiveSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.emissiveSamplerIndex)];
#if defined(PSO_PARALLAX)
    const MaterialUvSample emissiveUv = ResolveMaterialUvSample(
        uvCache,
        uvBindings,
        MATERIAL_TEXTURE_SLOT_EMISSIVE,
        hasParallaxResolvedUv,
        parallaxUv,
        parallaxDUdx,
        parallaxDUdy);
#else
    const MaterialUvSample emissiveUv = ResolveMaterialUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_EMISSIVE);
#endif
    float4 emissiveSample = SampleMaterialTexture2DGrad(emissiveTexture, emissiveSamplerState, materialInfo.emissiveStreamingTextureID, emissiveUv.uv, emissiveUv.dUVdx, emissiveUv.dUVdy, ret);
    emissive = float3(
        DynamicSwizzle(emissiveSample, materialInfo.emissiveChannels.x),
        DynamicSwizzle(emissiveSample, materialInfo.emissiveChannels.y),
        DynamicSwizzle(emissiveSample, materialInfo.emissiveChannels.z)) * materialInfo.emissiveFactor.rgb;
#endif

    const OpenPBRMaterialInfo openPBRMaterialInfo = LoadOpenPBRMaterialInfo(materialInfo.openPBRMaterialDataIndex);
    OpenPBRSurfaceSample openPBRSurface = ResolveCanonicalOpenPBRSurface(
        materialInfo,
        openPBRMaterialInfo,
        baseColor.rgb * vertexColorMultiplier,
        metallic,
        roughness,
        baseColor.a,
        emissive);
    ApplyOpenPBRTextureSampling(
        uvCache,
        uvBindings,
        hasParallaxResolvedUv,
        materialInfo.heightUvSetIndex,
        parallaxUv,
        parallaxDUdx,
        parallaxDUdy,
        openPBRMaterialInfo,
        openPBRSurface,
        ret);
    PopulateLegacyMaterialInputsFromOpenPBRSurface(openPBRSurface, normalWS, ao, ret);
}

void SampleMaterialFromUvCacheWithVertexTangent(
    in MaterialUvCache uvCache,
    in MaterialUvBindings uvBindings,
    in float3 normalWSBase,
    in float4 tangentWS,
    in float3 posWS,
    in float3 vertexColorMultiplier,
    in MaterialInfo materialInfo,
    in uint materialFlags,
    in float3 dpdx,
    in float3 dpdy,
    out MaterialInputs ret)
{
    SampleMaterialFromUvCache(uvCache, uvBindings, normalWSBase, posWS, vertexColorMultiplier, materialInfo, materialFlags, dpdx, dpdy, ret);

#if defined(PSO_NORMAL_MAP)
    if (uvBindings.hasTbnSource && abs(tangentWS.w) > 0.5f && (materialFlags & MATERIAL_NORMAL_MAP) != 0u)
    {
        const float3x3 normalTBN = BuildMaterialTBNFromVertexTangent(uvCache, uvBindings, normalWSBase, dpdx, dpdy, tangentWS);
        Texture2D<float4> normalTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.normalTextureIndex)];
        SamplerState normalSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.normalSamplerIndex)];
        const MaterialUvSample normalUv = ResolveMaterialUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_NORMAL);
        float4 textureNormal = SampleMaterialTexture2DGrad(normalTexture, normalSamplerState, materialInfo.normalStreamingTextureID, normalUv.uv, normalUv.dUVdx, normalUv.dUVdy);
        float3 tangentSpaceNormal = DecodeMaterialNormalSample(textureNormal, materialInfo.normalChannels, materialFlags);
        ret.normalWS = normalize(mul(tangentSpaceNormal, normalTBN));
    }
#endif
}

void SampleMaterialFromUvCacheRuntime(
    in MaterialUvCache uvCache,
    in MaterialUvBindings uvBindings,
    in float3 normalWSBase,
    in float3 posWS,
    in float3 vertexColorMultiplier,
    in MaterialInfo materialInfo,
    in uint materialFlags,
    in float3 dpdx,
    in float3 dpdy,
    out MaterialInputs ret)
{
    InitializeMaterialSelectedMipDebug(ret);

    float3x3 TBN = (float3x3)0.0f;
    if (uvBindings.hasTbnSource)
    {
        MaterialUvSample tbnUv = uvCache.samples[uvBindings.tbnCacheIndex];
        TBN = cotangent_frame_from_derivs(normalWSBase.xyz, dpdx, dpdy, tbnUv.dUVdx, tbnUv.dUVdy);
    }

    float2 parallaxUv = float2(0.0f, 0.0f);
    float2 parallaxDUdx = float2(0.0f, 0.0f);
    float2 parallaxDUdy = float2(0.0f, 0.0f);
    bool hasParallaxResolvedUv = false;

    if ((materialFlags & MATERIAL_PARALLAX) != 0u && uvBindings.hasHeightSource && uvBindings.hasTbnSource)
    {
        MaterialUvSample heightUv = uvCache.samples[uvBindings.heightCacheIndex];
        ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[0];
        StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
        Camera mainCamera = cameras[perFrameBuffer.mainCameraIndex];

        float3 viewDir = normalize(mainCamera.positionWorldSpace.xyz - posWS);

        if (perFrameBuffer.parallaxOcclusionMappingEnabled != 0u)
        {
            float3 uvh;
            if ((materialFlags & MATERIAL_HEIGHT_FROM_BASE_ALPHA) != 0u)
            {
                Texture2D<float4> parallaxTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.baseColorTextureIndex)];
                SamplerState parallaxSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.baseColorSamplerIndex)];
                uvh = getParallaxOcclusionMappingCoordsAndHeight(
                    parallaxTexture,
                    parallaxSamplerState,
                    3u,
                    TBN,
                    heightUv.uv,
                    viewDir,
                    materialInfo.heightMapScale,
                    16u,
                    heightUv.dUVdx,
                    heightUv.dUVdy);
            }
            else
            {
                Texture2D<float> parallaxTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.heightMapIndex)];
                SamplerState parallaxSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.heightSamplerIndex)];
                uvh = getParallaxOcclusionMappingCoordsAndHeight(
                    parallaxTexture,
                    parallaxSamplerState,
                    TBN,
                    heightUv.uv,
                    viewDir,
                    materialInfo.heightMapScale,
                    16u,
                    heightUv.dUVdx,
                    heightUv.dUVdy);
            }

            parallaxUv = uvh.xy;
            parallaxDUdx = heightUv.dUVdx;
            parallaxDUdy = heightUv.dUVdy;
            hasParallaxResolvedUv = true;
            ret.parallaxApplied = 1u;
        }
    }

    float4 baseColor = materialInfo.baseColorFactor;

    if ((materialFlags & MATERIAL_BASE_COLOR_TEXTURE) != 0u)
    {
        const MaterialUvSample baseColorUv = GetBoundUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_BASE_COLOR);
        Texture2D<float4> baseColorTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.baseColorTextureIndex)];
        SamplerState baseColorSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.baseColorSamplerIndex)];
        float2 baseColorSampleUv = baseColorUv.uv;
        float2 baseColorDUdx = baseColorUv.dUVdx;
        float2 baseColorDUdy = baseColorUv.dUVdy;
        if (hasParallaxResolvedUv && uvBindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_BASE_COLOR] == uvBindings.heightCacheIndex)
        {
            baseColorSampleUv = parallaxUv;
            baseColorDUdx = parallaxDUdx;
            baseColorDUdy = parallaxDUdy;
        }
        float4 sampledColor = SampleMaterialTexture2DGrad(baseColorTexture, baseColorSamplerState, materialInfo.baseColorStreamingTextureID, baseColorSampleUv, baseColorDUdx, baseColorDUdy, ret);
        baseColor *= sampledColor;
    }

    if ((materialFlags & MATERIAL_OPACITY_TEXTURE) != 0u)
    {
        const MaterialUvSample opacityUv = GetBoundUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_OPACITY);
        Texture2D<float4> opacityTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.opacityTextureIndex)];
        SamplerState opacitySamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.opacitySamplerIndex)];
        float2 opacitySampleUv = opacityUv.uv;
        float2 opacityDUdx = opacityUv.dUVdx;
        float2 opacityDUdy = opacityUv.dUVdy;
        if (hasParallaxResolvedUv && uvBindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_OPACITY] == uvBindings.heightCacheIndex)
        {
            opacitySampleUv = parallaxUv;
            opacityDUdx = parallaxDUdx;
            opacityDUdy = parallaxDUdy;
        }
        float4 opacitySample = SampleMaterialTexture2DGrad(opacityTexture, opacitySamplerState, materialInfo.opacityStreamingTextureID, opacitySampleUv, opacityDUdx, opacityDUdy, ret);
        baseColor.a *= opacitySample.a;
    }

    float metallic = materialInfo.metallicFactor;
    float roughness = materialInfo.roughnessFactor;
    if ((materialFlags & MATERIAL_METALLIC_TEXTURE) != 0u)
    {
        const MaterialUvSample metallicUv = GetBoundUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_METALLIC);
        Texture2D<float4> metallicTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.metallicTextureIndex)];
        SamplerState metallicSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.metallicSamplerIndex)];

        float2 metallicSampleUv = metallicUv.uv;
        float2 metallicDUdx = metallicUv.dUVdx;
        float2 metallicDUdy = metallicUv.dUVdy;
        if (hasParallaxResolvedUv && uvBindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_METALLIC] == uvBindings.heightCacheIndex)
        {
            metallicSampleUv = parallaxUv;
            metallicDUdx = parallaxDUdx;
            metallicDUdy = parallaxDUdy;
        }

        float4 metallicSample = SampleMaterialTexture2DGrad(metallicTexture, metallicSamplerState, materialInfo.metallicStreamingTextureID, metallicSampleUv, metallicDUdx, metallicDUdy, ret);
        metallic = DynamicSwizzle(metallicSample, materialInfo.metallicChannel) * materialInfo.metallicFactor;
    }

    if ((materialFlags & MATERIAL_ROUGHNESS_TEXTURE) != 0u)
    {
        const MaterialUvSample roughnessUv = GetBoundUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_ROUGHNESS);
        Texture2D<float4> roughnessTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.roughnessTextureIndex)];
        SamplerState roughnessSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.roughnessSamplerIndex)];

        float2 roughnessSampleUv = roughnessUv.uv;
        float2 roughnessDUdx = roughnessUv.dUVdx;
        float2 roughnessDUdy = roughnessUv.dUVdy;
        if (hasParallaxResolvedUv && uvBindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_ROUGHNESS] == uvBindings.heightCacheIndex)
        {
            roughnessSampleUv = parallaxUv;
            roughnessDUdx = parallaxDUdx;
            roughnessDUdy = parallaxDUdy;
        }

        float4 roughnessSample = SampleMaterialTexture2DGrad(roughnessTexture, roughnessSamplerState, materialInfo.roughnessStreamingTextureID, roughnessSampleUv, roughnessDUdx, roughnessDUdy, ret);
        roughness = DynamicSwizzle(roughnessSample, materialInfo.roughnessChannel) * materialInfo.roughnessFactor;
    }

    float3 normalWS = normalWSBase;
    if ((materialFlags & MATERIAL_NORMAL_MAP) != 0u && uvBindings.hasTbnSource)
    {
        const MaterialUvSample normalUv = GetBoundUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_NORMAL);
        Texture2D<float4> normalTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.normalTextureIndex)];
        SamplerState normalSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.normalSamplerIndex)];
        float2 normalSampleUv = normalUv.uv;
        float2 normalDUdx = normalUv.dUVdx;
        float2 normalDUdy = normalUv.dUVdy;
        if (hasParallaxResolvedUv && uvBindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_NORMAL] == uvBindings.heightCacheIndex)
        {
            normalSampleUv = parallaxUv;
            normalDUdx = parallaxDUdx;
            normalDUdy = parallaxDUdy;
        }

        float4 textureNormal = SampleMaterialTexture2DGrad(normalTexture, normalSamplerState, materialInfo.normalStreamingTextureID, normalSampleUv, normalDUdx, normalDUdy, ret);
        float3 tangentSpaceNormal = DecodeMaterialNormalSample(textureNormal, materialInfo.normalChannels, materialFlags);

        normalWS = normalize(mul(tangentSpaceNormal, TBN));
    }

    float ao = 1.0f;
    if ((materialFlags & MATERIAL_AO_TEXTURE) != 0u)
    {
        const MaterialUvSample aoUv = GetBoundUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_AO);
        Texture2D<float4> aoTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.aoMapIndex)];
        SamplerState aoSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.aoSamplerIndex)];
        float2 aoSampleUv = aoUv.uv;
        float2 aoDUdx = aoUv.dUVdx;
        float2 aoDUdy = aoUv.dUVdy;
        if (hasParallaxResolvedUv && uvBindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_AO] == uvBindings.heightCacheIndex)
        {
            aoSampleUv = parallaxUv;
            aoDUdx = parallaxDUdx;
            aoDUdy = parallaxDUdy;
        }
        float4 aoSample = SampleMaterialTexture2DGrad(aoTexture, aoSamplerState, materialInfo.aoStreamingTextureID, aoSampleUv, aoDUdx, aoDUdy, ret);
        ao = DynamicSwizzle(aoSample, materialInfo.aoChannel);
    }

    float3 emissive = materialInfo.emissiveFactor.rgb;
    if ((materialFlags & MATERIAL_EMISSIVE_TEXTURE) != 0u)
    {
        const MaterialUvSample emissiveUv = GetBoundUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_EMISSIVE);
        Texture2D<float4> emissiveTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.emissiveTextureIndex)];
        SamplerState emissiveSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.emissiveSamplerIndex)];
        float2 emissiveSampleUv = emissiveUv.uv;
        float2 emissiveDUdx = emissiveUv.dUVdx;
        float2 emissiveDUdy = emissiveUv.dUVdy;
        if (hasParallaxResolvedUv && uvBindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_EMISSIVE] == uvBindings.heightCacheIndex)
        {
            emissiveSampleUv = parallaxUv;
            emissiveDUdx = parallaxDUdx;
            emissiveDUdy = parallaxDUdy;
        }
        float4 emissiveSample = SampleMaterialTexture2DGrad(emissiveTexture, emissiveSamplerState, materialInfo.emissiveStreamingTextureID, emissiveSampleUv, emissiveDUdx, emissiveDUdy, ret);
        emissive = float3(
            DynamicSwizzle(emissiveSample, materialInfo.emissiveChannels.x),
            DynamicSwizzle(emissiveSample, materialInfo.emissiveChannels.y),
            DynamicSwizzle(emissiveSample, materialInfo.emissiveChannels.z)) * materialInfo.emissiveFactor.rgb;
    }

    const OpenPBRMaterialInfo openPBRMaterialInfo = LoadOpenPBRMaterialInfo(materialInfo);
    OpenPBRSurfaceSample openPBRSurface = ResolveCanonicalOpenPBRSurface(
        materialInfo,
        openPBRMaterialInfo,
        baseColor.rgb * vertexColorMultiplier,
        metallic,
        roughness,
        baseColor.a,
        emissive);
    ApplyOpenPBRTextureSampling(
        uvCache,
        uvBindings,
        hasParallaxResolvedUv,
        materialInfo.heightUvSetIndex,
        parallaxUv,
        parallaxDUdx,
        parallaxDUdy,
        openPBRMaterialInfo,
        openPBRSurface,
        ret);
    PopulateLegacyMaterialInputsFromOpenPBRSurface(openPBRSurface, normalWS, ao, ret);
}

void SampleMaterialCorePrecompiled(
    in float2 uv,
    in float2 dUVdx,
    in float2 dUVdy,
    in float3 normalWSBase,
    in float3 posWS,
    in MaterialInfo materialInfo,
    in uint materialFlags,
    out MaterialInputs ret)
{
    MaterialUvCache uvCache = BuildSingleUvCache(uv, dUVdx, dUVdy);
    MaterialUvBindings uvBindings = BuildMaterialUvBindings(materialInfo, materialFlags, uvCache);
    SampleMaterialFromUvCache(uvCache, uvBindings, normalWSBase, posWS, float3(1.0f, 1.0f, 1.0f), materialInfo, materialFlags, ddx(posWS), ddy(posWS), ret);
    return;
#if 0
    float2 localUV = uv;
    float2 localDUdx = dUVdx;
    float2 localDUdy = dUVdy;

#if defined(PSO_PARALLAX)
    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[0];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    Camera mainCamera = cameras[perFrameBuffer.mainCameraIndex];

    float3 viewDir = normalize(mainCamera.positionWorldSpace.xyz - posWS.xyz);

    Texture2D<float> parallaxTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.heightMapIndex)];
    SamplerState parallaxSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.heightSamplerIndex)];

    // IMPORTANT: inside getParallaxOcclusionMappingCoordsAndHeight, use SampleGrad too.
    float3 uvh = getParallaxOcclusionMappingCoordsAndHeight(
        parallaxTexture,
        parallaxSamplerState,
        TBN,
        localUV,
        viewDir,
        materialInfo.heightMapScale,
        16u,
        localDUdx,
        localDUdy
    );

    localUV = uvh.xy;

    // If you *don't* implement derivative correction for parallax, keeping the original gradients
    // is the common approximation (it's usually fine).
#endif

    // Base color
    float4 baseColor = materialInfo.baseColorFactor;

#if defined(PSO_BASE_COLOR_TEXTURE)
    Texture2D<float4> baseColorTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.baseColorTextureIndex)];
    SamplerState baseColorSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.baseColorSamplerIndex)];
    float4 sampledColor = SampleMaterialTexture2DGrad(baseColorTexture, baseColorSamplerState, materialInfo.baseColorStreamingTextureID, localUV, localDUdx, localDUdy);
    baseColor *= sampledColor;
#endif

#if defined(PSO_OPACITY_TEXTURE)
    Texture2D<float4> opacityTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.opacityTextureIndex)];
    SamplerState opacitySamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.opacitySamplerIndex)];
    float4 opacitySample = SampleMaterialTexture2DGrad(opacityTexture, opacitySamplerState, materialInfo.opacityStreamingTextureID, localUV, localDUdx, localDUdy);
    baseColor.a *= opacitySample.a;
#endif

    // Metallic / roughness
    float metallic = materialInfo.metallicFactor;
    float roughness = materialInfo.roughnessFactor;

#if defined(PSO_METALLIC_TEXTURE)
    Texture2D<float4> metallicTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.metallicTextureIndex)];
    SamplerState metallicSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.metallicSamplerIndex)];
    float4 metallicSample  = SampleMaterialTexture2DGrad(metallicTexture,  metallicSamplerState, materialInfo.metallicStreamingTextureID, localUV, localDUdx, localDUdy);
    metallic  = DynamicSwizzle(metallicSample,  materialInfo.metallicChannel)  * materialInfo.metallicFactor;
#endif

#if defined(PSO_ROUGHNESS_TEXTURE)
    Texture2D<float4> roughnessTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.roughnessTextureIndex)];
    SamplerState roughnessSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.roughnessSamplerIndex)];
    float4 roughnessSample = SampleMaterialTexture2DGrad(roughnessTexture, roughnessSamplerState, materialInfo.roughnessStreamingTextureID, localUV, localDUdx, localDUdy);
    roughness = DynamicSwizzle(roughnessSample, materialInfo.roughnessChannel) * materialInfo.roughnessFactor;
#endif

    // Normal map
    float3 normalWS = normalWSBase;

#if defined(PSO_NORMAL_MAP)
    Texture2D<float4> normalTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.normalTextureIndex)];
    SamplerState normalSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.normalSamplerIndex)];

    float4 textureNormal = SampleMaterialTexture2DGrad(normalTexture, normalSamplerState, materialInfo.normalStreamingTextureID, localUV, localDUdx, localDUdy);
    float3 tangentSpaceNormal = DecodeMaterialNormalSample(textureNormal, materialInfo.normalChannels, materialFlags);

    normalWS = normalize(mul(tangentSpaceNormal, TBN));
#endif

    // AO
    float ao = 1.0;
#if defined(PSO_AO_TEXTURE)
    Texture2D<float4> aoTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.aoMapIndex)];
    SamplerState aoSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.aoSamplerIndex)];
    float4 aoSample = SampleMaterialTexture2DGrad(aoTexture, aoSamplerState, materialInfo.aoStreamingTextureID, localUV, localDUdx, localDUdy);
    ao = DynamicSwizzle(aoSample, materialInfo.aoChannel);
#endif

    // Emissive
    float3 emissive = materialInfo.emissiveFactor.rgb;
#if defined(PSO_EMISSIVE_TEXTURE)
    Texture2D<float4> emissiveTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.emissiveTextureIndex)];
    SamplerState emissiveSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.emissiveSamplerIndex)];
    emissive = SampleMaterialTexture2DGrad(emissiveTexture, emissiveSamplerState, materialInfo.emissiveStreamingTextureID, localUV, localDUdx, localDUdy).rgb * materialInfo.emissiveFactor.rgb;
#endif

    OpenPBRSurfaceSample openPBRSurface = ResolveCanonicalOpenPBRSurface(
        materialInfo,
        baseColor.rgb,
        metallic,
        roughness,
        baseColor.a,
        emissive);
    ApplyOpenPBRTextureSampling(
        BuildSingleUvCache(localUV, localDUdx, localDUdy),
        false,
        0u,
        float2(0.0f, 0.0f),
        float2(0.0f, 0.0f),
        float2(0.0f, 0.0f),
        LoadOpenPBRMaterialInfo(materialInfo),
        openPBRSurface);
    PopulateLegacyMaterialInputsFromOpenPBRSurface(openPBRSurface, normalWS, ao, ret);
#endif
}

void SampleMaterial(
    in float2 uv,
    in float3 normalWSBase,
    in float3 posWS,
    in uint materialDataIndex,
    out MaterialInputs ret)
{
    StructuredBuffer<MaterialInfo> materialDataBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    MaterialInfo materialInfo = materialDataBuffer[materialDataIndex];
    uint materialFlags = materialInfo.materialFlags;
    SampleMaterialCore(uv, normalWSBase, posWS, materialInfo, materialFlags, ret);

    // For PS version, alpha test / discard here
#if defined(PSO_ALPHA_TEST) || defined(PSO_BLEND)
    if (ret.opacity < materialInfo.alphaCutoff)
    {
        discard;
    }
#endif
}

void SampleMaterialPrecompiled(
    in float2 uv,
    in float3 normalWSBase,
    in float3 posWS,
    in uint materialDataIndex,
    out MaterialInputs ret)
{
    StructuredBuffer<MaterialInfo> materialDataBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    MaterialInfo materialInfo = materialDataBuffer[materialDataIndex];
    uint materialFlags = materialInfo.materialFlags;
    SampleMaterialCorePrecompiled(uv, ddx(uv), ddy(uv), normalWSBase, posWS, materialInfo, materialFlags, ret);

    // For PS version, alpha test / discard here
#if defined(PSO_ALPHA_TEST) || defined(PSO_BLEND)
    if (ret.opacity < materialInfo.alphaCutoff)
    {
        discard;
    }
#endif
}

void GetMaterialInfoForFragment(in const PSInput input, out MaterialInputs ret)
{
    StructuredBuffer<PerMeshBuffer> perMeshBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    uint meshBufferIndexLocal = GetRootPerMeshBufferIndex();
    PerMeshBuffer meshBuffer = perMeshBuffer[meshBufferIndexLocal];
    StructuredBuffer<MaterialInfo> materialDataBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    MaterialInfo materialInfo = materialDataBuffer[meshBuffer.materialDataIndex];
    uint materialFlags = materialInfo.materialFlags;
    MaterialUvCache uvCache = BuildSingleUvCache(input.texcoord, ddx(input.texcoord), ddy(input.texcoord));
    MaterialUvBindings uvBindings = BuildMaterialUvBindings(materialInfo, materialFlags, uvCache);
    SampleMaterialFromUvCacheWithVertexTangent(uvCache, uvBindings, input.normalWorldSpace, input.tangentWorldSpace, input.positionWorldSpace.xyz, input.color, materialInfo, materialFlags, ddx(input.positionWorldSpace.xyz), ddy(input.positionWorldSpace.xyz), ret);
}

void GetMaterialInfoForFragmentPrecompiled(in const PSInput input, out MaterialInputs ret)
{
    StructuredBuffer<PerMeshBuffer> perMeshBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    uint meshBufferIndexLocal = GetRootPerMeshBufferIndex();
    PerMeshBuffer meshBuffer = perMeshBuffer[meshBufferIndexLocal];
    StructuredBuffer<MaterialInfo> materialDataBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    MaterialInfo materialInfo = materialDataBuffer[meshBuffer.materialDataIndex];
    uint materialFlags = materialInfo.materialFlags;
    MaterialUvCache uvCache = BuildSingleUvCache(input.texcoord, ddx(input.texcoord), ddy(input.texcoord));
    MaterialUvBindings uvBindings = BuildMaterialUvBindings(materialInfo, materialFlags, uvCache);
    SampleMaterialFromUvCacheWithVertexTangent(
        uvCache,
        uvBindings,
        input.normalWorldSpace,
        input.tangentWorldSpace,
        input.positionWorldSpace.xyz,
        input.color,
        materialInfo,
        materialFlags,
        ddx(input.positionWorldSpace.xyz),
        ddy(input.positionWorldSpace.xyz),
        ret);
}

void SampleMaterialFromUvCacheRuntimeWithVertexTangent(
    in MaterialUvCache uvCache,
    in MaterialUvBindings uvBindings,
    in float3 normalWSBase,
    in float4 tangentWS,
    in float3 posWS,
    in float3 vertexColorMultiplier,
    in MaterialInfo materialInfo,
    in uint materialFlags,
    in float3 dpdx,
    in float3 dpdy,
    out MaterialInputs ret)
{
    SampleMaterialFromUvCacheRuntime(uvCache, uvBindings, normalWSBase, posWS, vertexColorMultiplier, materialInfo, materialFlags, dpdx, dpdy, ret);

    if (uvBindings.hasTbnSource && abs(tangentWS.w) > 0.5f && (materialFlags & MATERIAL_NORMAL_MAP) != 0u)
    {
        const float3x3 normalTBN = BuildMaterialTBNFromVertexTangent(uvCache, uvBindings, normalWSBase, dpdx, dpdy, tangentWS);
        Texture2D<float4> normalTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.normalTextureIndex)];
        SamplerState normalSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.normalSamplerIndex)];
        const MaterialUvSample normalUv = ResolveMaterialUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_NORMAL);
        float4 textureNormal = SampleMaterialTexture2DGrad(normalTexture, normalSamplerState, materialInfo.normalStreamingTextureID, normalUv.uv, normalUv.dUVdx, normalUv.dUVdy);
        float3 tangentSpaceNormal = DecodeMaterialNormalSample(textureNormal, materialInfo.normalChannels, materialFlags);
        ret.normalWS = normalize(mul(tangentSpaceNormal, normalTBN));
    }
}

void SampleMaterialCS(
    in float2 uv,
    in float3 normalWSBase,
    in float3 posWS,
    in uint materialDataIndex,
    in float3 dpdx,
    in float3 dpdy,
    in float2 dUVdx,
    in float2 dUVdy,
    out MaterialInputs ret)
{
    StructuredBuffer<MaterialInfo> materialDataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    MaterialInfo materialInfo = materialDataBuffer[materialDataIndex];
    uint materialFlags = materialInfo.materialFlags;
    MaterialUvCache uvCache = BuildSingleUvCache(uv, dUVdx, dUVdy);
    MaterialUvBindings uvBindings = BuildMaterialUvBindings(materialInfo, materialFlags, uvCache);
    SampleMaterialFromUvCache(uvCache, uvBindings, normalWSBase, posWS, float3(1.0f, 1.0f, 1.0f), materialInfo, materialFlags, dpdx, dpdy, ret);
}

float PerceptualRoughnessToRoughness(float perceptualRoughness)
{
    return perceptualRoughness * perceptualRoughness;
}

float3 computeF0(const float4 baseColor, float metallic, float reflectance)
{
    return baseColor.rgb * metallic + (reflectance * (1.0 - metallic));
}

float computeDielectricF0(float reflectance)
{
    return 0.16 * reflectance * reflectance;
}

float OpenPBRIorToF0(float ior)
{
    const float safeIor = max(ior, 1.0f);
    const float f = (safeIor - 1.0f) / (safeIor + 1.0f);
    return f * f;
}

float OpenPBRIorFromF0(float f0)
{
    const float safeF0 = min(saturate(f0), 0.9999f);
    const float sqrtF0 = sqrt(safeF0);
    return (1.0f + sqrtF0) / max(1.0f - sqrtF0, 1.0e-4f);
}

float OpenPBRApplySpecularWeightToIor(float ior, float specularWeight)
{
    const float unscaledF0 = OpenPBRIorToF0(ior);
    const float scaledF0 = min(unscaledF0 * saturate(specularWeight), 0.9999f);
    return OpenPBRIorFromF0(scaledF0);
}

float3 OpenPBRComputeMetalSchlickBFactor(float3 f0, float3 f82Tint)
{
    const float cosThetaMax = 1.0f / 7.0f;
    const float oneMinusCosThetaMax = 1.0f - cosThetaMax;
    const float oneMinusCosThetaMaxToTheFifth = pow(oneMinusCosThetaMax, 5.0f);
    const float oneMinusCosThetaMaxToTheSixth = pow(oneMinusCosThetaMax, 6.0f);
    const float3 whiteMinusF0 = 1.0f.xxx - saturate(f0);
    const float3 whiteMinusTint = 1.0f.xxx - saturate(f82Tint);
    const float3 numerator = (saturate(f0) + whiteMinusF0 * oneMinusCosThetaMaxToTheFifth) * whiteMinusTint;
    const float denominator = cosThetaMax * oneMinusCosThetaMaxToTheSixth;
    return numerator / max(denominator, 1.0e-6f);
}

float3 OpenPBRMetalAverageFresnelWithF82Tint(float3 f0, float3 f82Tint)
{
    const float3 safeF0 = saturate(f0);
    const float3 whiteMinusF0 = 1.0f.xxx - safeF0;
    const float3 b = OpenPBRComputeMetalSchlickBFactor(safeF0, f82Tint);
    return saturate(safeF0 + whiteMinusF0 * (1.0f / 21.0f) - b * (1.0f / 126.0f));
}

void PopulateFragmentInfoFromOpenPBR(
    OpenPBRSurfaceSample surface,
    inout FragmentInfo ret)
{
    OpenPBRMaterialInfo openPBRMaterialInfo = LoadOpenPBRMaterialInfo(surface.openPBRMaterialDataIndex);
    const float baseWeight = saturate(openPBRMaterialInfo.baseWeight);
    const float specularWeight = saturate(openPBRMaterialInfo.specularWeight);
    const float3 specularColor = saturate(openPBRMaterialInfo.specularColor);
    const float3 weightedBaseColor = saturate(surface.baseColor * baseWeight);
    const float weightedSpecularIor = OpenPBRApplySpecularWeightToIor(openPBRMaterialInfo.specularIor, specularWeight);
    const float dielectricF0Scalar = OpenPBRIorToF0(weightedSpecularIor);
    const float3 dielectricF0 = saturate(specularColor * dielectricF0Scalar);
    const float dielectricF0Max = max(max(dielectricF0.x, dielectricF0.y), dielectricF0.z);
    const float coatPerceptualRoughness = clamp(surface.coatRoughness, MIN_PERCEPTUAL_ROUGHNESS, 1.0f);
    const float coatF0Scalar = OpenPBRIorToF0(openPBRMaterialInfo.coatIor);
    const float dielectricSpecularWeight = saturate(1.0f - surface.baseMetalness);
    const float metalSpecularWeight = saturate(surface.baseMetalness * specularWeight);
    const float3 metalF0 = saturate(weightedBaseColor * specularColor);
    const float3 metalAverageFresnel = OpenPBRMetalAverageFresnelWithF82Tint(weightedBaseColor, specularColor);
    const float specularAlpha = ret.roughness;

    ret.openPBRMaterialDataIndex = surface.openPBRMaterialDataIndex;
    ret.albedo = weightedBaseColor;
    ret.emissive = surface.emissive;
    ret.metallic = surface.baseMetalness;
    ret.coatWeight = saturate(surface.coatWeight);
    ret.coatColor = saturate(surface.coatColor);
    ret.coatPerceptualRoughness = coatPerceptualRoughness;
    ret.coatRoughness = PerceptualRoughnessToRoughness(coatPerceptualRoughness);
    ret.coatF0 = saturate(ret.coatColor * coatF0Scalar);
    ret.coatIor = openPBRMaterialInfo.coatIor;
    ret.coatDarkening = saturate(openPBRMaterialInfo.coatDarkening);
    ret.fuzzWeight = saturate(surface.fuzzWeight);
    ret.fuzzColor = saturate(surface.fuzzColor);
    ret.fuzzRoughness = saturate(surface.fuzzRoughness);
    ret.baseDiffuseRoughness = saturate(openPBRMaterialInfo.baseDiffuseRoughness);
    ret.specularAlpha = specularAlpha;
    ret.weightedSpecularIor = weightedSpecularIor;
    ret.dielectricSpecularWeight = dielectricSpecularWeight;
    ret.dielectricSpecularF0 = dielectricF0;
    ret.metalSpecularWeight = metalSpecularWeight;
    ret.metalSpecularF0 = metalF0;
    ret.metalAverageFresnel = metalAverageFresnel;
    ret.diffuseColor = computeDiffuseColor(weightedBaseColor, surface.baseMetalness);
    ret.reflectance = sqrt(saturate(dielectricF0Max / 0.16f));
    ret.dielectricF0 = dielectricF0Max * (1.0f - surface.baseMetalness);
    ret.F0 = saturate(dielectricSpecularWeight * dielectricF0 + metalSpecularWeight * metalF0);
}

void GetFragmentInfoScreenSpace(in uint2 pixelCoordinates, in float3 viewWS, in float3 fragPosViewSpace, in float3 fragPosWorldSpace, in bool enableGTAO, out FragmentInfo ret) {
    ret.pixelCoords = pixelCoordinates;
    ret.fragPosViewSpace = fragPosViewSpace;
    ret.fragPosWorldSpace = fragPosWorldSpace;
    ret.selectedMaterialMipLevel = MATERIAL_DEBUG_INVALID_MIP_LEVEL;
    ret.selectedMaterialMipMaxLevel = 0u;
    ret.parallaxApplied = 0u;
    
    // Gather textures
    Texture2D<float4> normalsTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::GBuffer::Normals)];
    
    // Load values
    float4 normalSample = normalsTexture[pixelCoordinates];
    ret.normalWS = normalSample.xyz;
    //ret.normalWS = SignedOctDecode(encodedNormal.yzw);
    
    Texture2D<float4> albedoTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::GBuffer::Albedo)];
    float4 baseColorSample = albedoTexture[pixelCoordinates];
    Texture2D<float4> coatTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::GBuffer::Coat)];
    float4 coatSample = coatTexture[pixelCoordinates];
    Texture2D<float4> emissiveTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::GBuffer::Emissive)];
    float4 emissive = emissiveTexture[pixelCoordinates];
    Texture2D<float4> fuzzTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::GBuffer::Fuzz)];
    float4 fuzzSample = fuzzTexture[pixelCoordinates];
    
    if (enableGTAO)
    {
        Texture2D<uint> aoTexture = ResourceDescriptorHeap[OptionalResourceDescriptorIndex(Builtin::GTAO::OutputAOTerm)];
        ret.diffuseAmbientOcclusion = min(baseColorSample.w, float(aoTexture[pixelCoordinates].x) / 255.0);
    }
    else
    {
        ret.diffuseAmbientOcclusion = baseColorSample.w; // AO stored in alpha channel
    }
    
    Texture2D<float4> metallicRoughnessTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::GBuffer::MetallicRoughness)];
    float4 metallicRoughness = metallicRoughnessTexture[pixelCoordinates];
    
    float perceptualRoughness = metallicRoughness.y;
    ret.perceptualRoughnessUnclamped = perceptualRoughness;
    // Clamp the roughness to a minimum value to avoid divisions by 0 during lighting
    ret.perceptualRoughness = clamp(perceptualRoughness, MIN_PERCEPTUAL_ROUGHNESS, 1.0);
    // Remaps the roughness to a perceptually linear roughness (roughness^2)
    ret.roughness = PerceptualRoughnessToRoughness(ret.perceptualRoughness);
    ret.roughnessUnclamped = PerceptualRoughnessToRoughness(ret.perceptualRoughnessUnclamped);
    
    ret.viewWS = viewWS;
    //ret.NdotV = dot(ret.normalWS, viewWS);
    ret.NdotV = dot(ret.normalWS, ret.viewWS);
    ret.normalWS = normalize(ret.normalWS + max(0, -ret.NdotV + MIN_N_DOT_V) * ret.viewWS);
    ret.NdotV = max(MIN_N_DOT_V, ret.NdotV);
    ret.reflectedWS = reflect(-ret.viewWS, ret.normalWS);
    
    //ret.DFG = prefilteredDFG(ret.perceptualRoughness, ret.NdotV);

    ret.alpha = 1.0; // Opaque objects

    OpenPBRSurfaceSample surface = (OpenPBRSurfaceSample)0;
    surface.openPBRMaterialDataIndex = (uint)(normalSample.w + 0.5f);
    surface.baseColor = baseColorSample.xyz;
    surface.baseMetalness = metallicRoughness.x;
    surface.specularRoughness = metallicRoughness.y;
    surface.coatColor = coatSample.xyz;
    surface.coatWeight = coatSample.w;
    surface.coatRoughness = metallicRoughness.z;
    surface.fuzzColor = fuzzSample.xyz;
    surface.fuzzWeight = metallicRoughness.w;
    surface.fuzzRoughness = fuzzSample.w;
    surface.opacity = 1.0f;
    surface.emissive = emissive.xyz;
    PopulateFragmentInfoFromOpenPBR(surface, ret);
}

void FillFragmentInfoDirect(inout FragmentInfo ret, in MaterialInputs materialInfo, in float3 viewWS, in float2 pixelCoords, in bool enableGTAO, in bool transparent, in bool isFrontFace, in uint materialFlags)
{
    ret.materialFlags = materialFlags;
    ret.selectedMaterialMipLevel = materialInfo.selectedMaterialMipLevel;
    ret.selectedMaterialMipMaxLevel = materialInfo.selectedMaterialMipMaxLevel;
    ret.parallaxApplied = materialInfo.parallaxApplied;
    float perceptualRoughness = materialInfo.roughness;
    ret.perceptualRoughnessUnclamped = perceptualRoughness;
    // Clamp the roughness to a minimum value to avoid divisions by 0 during lighting
    ret.perceptualRoughness = clamp(perceptualRoughness, MIN_PERCEPTUAL_ROUGHNESS, 1.0);
    // Remaps the roughness to a perceptually linear roughness (roughness^2)
    ret.roughness = PerceptualRoughnessToRoughness(ret.perceptualRoughness);
    ret.roughnessUnclamped = PerceptualRoughnessToRoughness(ret.perceptualRoughnessUnclamped);

    ret.normalWS = materialInfo.normalWS;
    if (!isFrontFace && ((materialFlags & MATERIAL_DOUBLE_SIDED) != 0u)) {
        ret.normalWS = -ret.normalWS;
    }
    
    ret.viewWS = viewWS;
    //ret.NdotV = dot(ret.normalWS, viewWS);
    ret.NdotV = dot(ret.normalWS, ret.viewWS);
    ret.normalWS = normalize(ret.normalWS + max(0, -ret.NdotV + MIN_N_DOT_V) * ret.viewWS);
    ret.NdotV = max(MIN_N_DOT_V, ret.NdotV);
    
    ret.reflectedWS = reflect(-ret.viewWS, ret.normalWS);
    
    //ret.DFG = prefilteredDFG(ret.perceptualRoughness, ret.NdotV);

    OpenPBRSurfaceSample surface = (OpenPBRSurfaceSample)0;
    surface.openPBRMaterialDataIndex = materialInfo.openPBRMaterialDataIndex;
    surface.baseColor = materialInfo.albedo;
    surface.baseMetalness = materialInfo.metallic;
    surface.specularRoughness = materialInfo.roughness;
    surface.coatColor = materialInfo.coatColor;
    surface.coatWeight = materialInfo.coatWeight;
    surface.coatRoughness = materialInfo.coatRoughness;
    surface.fuzzColor = materialInfo.fuzzColor;
    surface.fuzzWeight = materialInfo.fuzzWeight;
    surface.fuzzRoughness = materialInfo.fuzzRoughness;
    surface.opacity = materialInfo.opacity;
    surface.emissive = materialInfo.emissive;
    PopulateFragmentInfoFromOpenPBR(surface, ret);

    if (transparent)
    {
        ret.alpha = materialInfo.opacity;
        ret.diffuseAmbientOcclusion = materialInfo.ambientOcclusion; // Screen-space AO not applied to transparent objects
    }
    else
    {
        ret.alpha = 1.0; // Opaque objects
        if (enableGTAO)
        {
            Texture2D<uint> aoTexture = ResourceDescriptorHeap[OptionalResourceDescriptorIndex(Builtin::GTAO::OutputAOTerm)];
            ret.diffuseAmbientOcclusion = min(materialInfo.ambientOcclusion, float(aoTexture[pixelCoords].x) / 255.0);
        }
        else
        {
            ret.diffuseAmbientOcclusion = materialInfo.ambientOcclusion;
        }
    }
    
}

void GetFragmentInfoDirectPrecompiled(in PSInput input, in float3 viewWS, bool enableGTAO, bool transparent, bool isFrontFace, out FragmentInfo ret)
{
    ret.pixelCoords = input.position.xy;
    ret.fragPosViewSpace = input.positionViewSpace.xyz;
    ret.fragPosWorldSpace = input.positionWorldSpace.xyz;
    
    MaterialInputs materialInfo;
    GetMaterialInfoForFragmentPrecompiled(input, materialInfo);
    
    StructuredBuffer<PerMeshBuffer> perMeshBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    PerMeshBuffer meshBuffer = perMeshBuffer[GetRootPerMeshBufferIndex()];
    StructuredBuffer<MaterialInfo> materialDataBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    MaterialInfo materialData = materialDataBuffer[meshBuffer.materialDataIndex];
    FillFragmentInfoDirect(ret, materialInfo, viewWS, input.position.xy, enableGTAO, transparent, isFrontFace, materialData.materialFlags);
}

void GetFragmentInfoDirect(in PSInput input, in float3 viewWS, bool enableGTAO, bool transparent, bool isFrontFace, out FragmentInfo ret)
{
    ret.pixelCoords = input.position.xy;
    ret.fragPosViewSpace = input.positionViewSpace.xyz;
    ret.fragPosWorldSpace = input.positionWorldSpace.xyz;
    
    MaterialInputs materialInfo;
    GetMaterialInfoForFragment(input, materialInfo);

    StructuredBuffer<PerMeshBuffer> perMeshBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    PerMeshBuffer meshBuffer = perMeshBuffer[GetRootPerMeshBufferIndex()];
    StructuredBuffer<MaterialInfo> materialDataBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    MaterialInfo materialData = materialDataBuffer[meshBuffer.materialDataIndex];
    FillFragmentInfoDirect(ret, materialInfo, viewWS, input.position.xy, enableGTAO, transparent, isFrontFace, materialData.materialFlags);
}

float unprojectDepth(float depth, float near, float far)
{
    return near * far / (far - depth * (far - near));
}

#endif // __UTILITY_HLSL__
