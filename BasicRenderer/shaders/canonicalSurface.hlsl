#include "include/clodResolveCommon.hlsli"
#include "include/debugPayload.hlsli"
#include "include/canonicalSurfaceDebug.hlsli"

void WriteCanonicalSurfaceSample(uint2 pixel, float2 motionVector, MaterialInputs material)
{
    RWTexture2D<float4> baseColorOpacity = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::BaseColorOpacity)];
    RWTexture2D<float4> normalRoughness = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::NormalRoughness)];
    RWTexture2D<float4> specularAo = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::SpecularAo)];
    RWTexture2D<float4> emissiveTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::Emissive)];
    RWTexture2D<float2> motionVectorTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::Motion)];
    RWTexture2D<float4> payload0Texture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::Payload0)];
    RWTexture2D<float4> payload1Texture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::Payload1)];
    RWTexture2D<uint2> surfaceIdentity = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::Identity)];
    RWStructuredBuffer<SARPSurfaceRecordV1> surfaceRecords = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::Records)];

    const OpenPBRMaterialInfo openPBR = LoadOpenPBRMaterialInfo(material.openPBRMaterialDataIndex);
    const float baseWeight = saturate(openPBR.baseWeight);
    const float specularWeight = saturate(openPBR.specularWeight);
    const float3 specularColor = saturate(openPBR.specularColor);
    const float3 weightedBaseColor = saturate(material.albedo * baseWeight);
    const float weightedIor = OpenPBRApplySpecularWeightToIor(openPBR.specularIor, specularWeight);
    const float3 dielectricF0 = saturate(specularColor * OpenPBRIorToF0(weightedIor));
    const float3 metalF0 = saturate(weightedBaseColor * specularColor);
    const float metalness = saturate(material.metallic);
    baseColorOpacity[pixel] = float4(weightedBaseColor * (1.0f - metalness), saturate(material.opacity));
    normalRoughness[pixel] = float4(normalize(material.normalWS), saturate(material.roughness));
    specularAo[pixel] = float4(lerp(dielectricF0, metalF0, metalness), saturate(material.ambientOcclusion));
    emissiveTexture[pixel] = float4(material.emissive, 0.0f);
    payload0Texture[pixel] = float4(material.coatColor, material.coatWeight);
    payload1Texture[pixel] = float4(material.fuzzColor, material.fuzzRoughness);
    motionVectorTexture[pixel] = motionVector;

    uint width;
    uint height;
    surfaceIdentity.GetDimensions(width, height);
    const uint recordIndex = pixel.y * width + pixel.x;
    const uint hasCoat = material.coatWeight > 0.0f ? (1u << 9u) : 0u;
    const uint hasFuzz = material.fuzzWeight > 0.0f ? (1u << 10u) : 0u;
    const uint hasGlint = material.glintEnabled != 0u ? (1u << 12u) : 0u;
    const uint metallicWorkflow = material.metallic > 0.0f ? (1u << 8u) : 0u;
    const uint flags = material.surfaceFlags | hasCoat | hasFuzz | hasGlint | metallicWorkflow;
    const uint payloadProfile = hasGlint != 0u ? 3u : ((hasCoat | hasFuzz) != 0u ? 1u : 0u);
    surfaceIdentity[pixel] = uint2(
        recordIndex,
        (material.semanticFamily & 0xffu) | ((payloadProfile & 0xffu) << 8u) | ((flags & 0xffffu) << 16u));

    SARPSurfaceRecordV1 record = (SARPSurfaceRecordV1)0;
    record.sourceObjectId = material.sourceObjectId;
    record.sourceMaterialId = material.sourceMaterialId;
    record.materialTableIndex = material.materialTableIndex;
    record.semanticFamilyAndPayload = (material.semanticFamily & 0xffffu) | ((payloadProfile & 0xffffu) << 16u);
    record.flags = flags;
    record.diagnosticReason = material.diagnosticReason;
    surfaceRecords[recordIndex] = record;
}

void EvaluateCanonicalSurfaceOptimized(uint2 pixel, uint64_t vis)
{
#if defined(VISUTIL_COLOR_ONLY_GBUFFER_EVAL)
    ClodGBufferColorSample sample;
    if (!ResolveClodGBufferColorSampleFromVisKey(vis, pixel, sample))
    {
        return;
    }

    WriteCanonicalSurfaceSample(pixel, sample.motionVector, sample.materialInputs);
    return;
#else
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    uint outputType = perFrame.outputType;

    if (outputType == OUTPUT_COLOR)
    {
        ClodGBufferColorSample sample;
        if (!ResolveClodGBufferColorSampleFromVisKey(vis, pixel, sample))
        {
            return;
        }

        WriteCanonicalSurfaceSample(pixel, sample.motionVector, sample.materialInputs);
        return;
    }

    ClodGBufferDebugSample sample;
    if (!ResolveClodGBufferDebugSampleFromVisKey(vis, pixel, sample))
    {
        return;
    }

    WriteCanonicalSurfaceSample(pixel, sample.motionVector, sample.materialInputs);
    WriteCanonicalSurfaceDebugPixel(pixel, vis, outputType, sample);
#endif
}

void EvaluateCanonicalSurfaceOptimized(uint2 pixel)
{
    Texture2D<uint64_t> visibilityTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::VisibilityTexture)];
    EvaluateCanonicalSurfaceOptimized(pixel, visibilityTexture[pixel]);
}

[numthreads(8, 8, 1)]
void PerViewPrimaryDepthCopyCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint screenW = UintRootConstant2;
    uint screenH = UintRootConstant3;

    if (dispatchThreadId.x >= screenW || dispatchThreadId.y >= screenH)
    {
        return;
    }

    uint2 pixel = dispatchThreadId.xy;
    Texture2D<uint64_t> visibilityTexture = ResourceDescriptorHeap[UintRootConstant0];
    uint64_t vis = visibilityTexture[pixel];

    float depth;
    if (vis == 0xFFFFFFFFFFFFFFFF)
    {
        depth = asfloat(0x7F7FFFFF);
    }
    else
    {
        uint clusterIndex;
        uint meshletTriangleIndex;
        UnpackVisKey(vis, depth, clusterIndex, meshletTriangleIndex);
    }

    RWTexture2D<float> linearDepthTexture = ResourceDescriptorHeap[UintRootConstant1];
    linearDepthTexture[pixel] = depth;

    uint projectedDepthUAVIndex = UintRootConstant4;
    if (projectedDepthUAVIndex != 0xFFFFFFFFu)
    {
        float projectedDepth;
        if (vis == 0xFFFFFFFFFFFFFFFF)
        {
            projectedDepth = 0.0f;
        }
        else
        {
            float M22 = asfloat(UintRootConstant5);
            float M32 = asfloat(UintRootConstant6);
            projectedDepth = -M22 + M32 / depth;
        }
        RWTexture2D<float> projectedDepthTexture = ResourceDescriptorHeap[projectedDepthUAVIndex];
        projectedDepthTexture[pixel] = projectedDepth;
        if (UintRootConstant7 != 0xFFFFFFFFu)
        {
            RWTexture2D<float> canonicalDeviceDepth = ResourceDescriptorHeap[UintRootConstant7];
            canonicalDeviceDepth[pixel] = projectedDepth;
        }
    }
}

[numthreads(8, 8, 1)]
void PrimaryDepthCopyCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];

    uint screenW = perFrameBuffer.screenResX;
    uint screenH = perFrameBuffer.screenResY;

    if (dispatchThreadId.x >= screenW || dispatchThreadId.y >= screenH)
    {
        return;
    }

    uint2 pixel = dispatchThreadId.xy;
    Texture2D<uint64_t> visibilityTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::VisibilityTexture)];

    uint64_t vis = visibilityTexture[pixel];
    float depth;
    if (vis == 0xFFFFFFFFFFFFFFFF)
    {
        depth = asfloat(0x7F7FFFFF);
    }
    else
    {
        uint clusterIndex;
        uint meshletTriangleIndex;
        UnpackVisKey(vis, depth, clusterIndex, meshletTriangleIndex);
    }

    RWTexture2D<float> linearDepthTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::LinearDepthMap)];
    linearDepthTexture[pixel] = depth;
}
