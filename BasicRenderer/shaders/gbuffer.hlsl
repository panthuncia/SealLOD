#include "include/clodResolveCommon.hlsli"
#include "include/debugPayload.hlsli"

void EvaluateGBufferOptimized(uint2 pixel)
{
    Texture2D<uint64_t> visibilityTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::VisibilityTexture)];
    uint64_t vis = visibilityTexture[pixel];
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    uint outputType = perFrame.outputType;

    RWTexture2D<float4> normalsTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::GBuffer::Normals)];
    RWTexture2D<float4> albedoTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::GBuffer::Albedo)];
    RWTexture2D<float4> coatTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::GBuffer::Coat)];
    RWTexture2D<float4> emissiveTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::GBuffer::Emissive)];
    RWTexture2D<float4> fuzzTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::GBuffer::Fuzz)];
    RWTexture2D<float4> metallicRoughnessTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::GBuffer::MetallicRoughness)];
    RWTexture2D<float2> motionVectorTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::GBuffer::MotionVectors)];

    if (outputType == OUTPUT_COLOR)
    {
        ClodGBufferColorSample sample;
        if (!ResolveClodGBufferColorSampleFromVisKey(vis, pixel, sample))
        {
            return;
        }

        normalsTexture[pixel] = float4(sample.materialInputs.normalWS, (float)sample.materialInputs.openPBRMaterialDataIndex);
        albedoTexture[pixel] = float4(sample.materialInputs.albedo, sample.materialInputs.ambientOcclusion);
        coatTexture[pixel] = float4(sample.materialInputs.coatColor, sample.materialInputs.coatWeight);
        emissiveTexture[pixel].xyz = sample.materialInputs.emissive;
        fuzzTexture[pixel] = float4(sample.materialInputs.fuzzColor, sample.materialInputs.fuzzRoughness);
        metallicRoughnessTexture[pixel] = float4(sample.materialInputs.metallic, sample.materialInputs.roughness, sample.materialInputs.coatRoughness, sample.materialInputs.fuzzWeight);
        motionVectorTexture[pixel] = sample.motionVector;
        return;
    }

    ClodGBufferDebugSample sample;
    if (!ResolveClodGBufferDebugSampleFromVisKey(vis, pixel, sample))
    {
        return;
    }

    normalsTexture[pixel] = float4(sample.materialInputs.normalWS, (float)sample.materialInputs.openPBRMaterialDataIndex);
    albedoTexture[pixel] = float4(sample.materialInputs.albedo, sample.materialInputs.ambientOcclusion);
    coatTexture[pixel] = float4(sample.materialInputs.coatColor, sample.materialInputs.coatWeight);
    emissiveTexture[pixel].xyz = sample.materialInputs.emissive;
    fuzzTexture[pixel] = float4(sample.materialInputs.fuzzColor, sample.materialInputs.fuzzRoughness);
    metallicRoughnessTexture[pixel] = float4(sample.materialInputs.metallic, sample.materialInputs.roughness, sample.materialInputs.coatRoughness, sample.materialInputs.fuzzWeight);
    motionVectorTexture[pixel] = sample.motionVector;

    bool isReyesPatch = false;
    if (outputType == OUTPUT_REYES_GEOMETRY_PATH && vis != 0xFFFFFFFFFFFFFFFF)
    {
        float visDepth;
        uint visClusterIndex;
        uint visPrimitiveIndex;
        UnpackVisKey(vis, visDepth, visClusterIndex, visPrimitiveIndex);
        isReyesPatch =
            visClusterIndex >= VISBUF_REYES_PATCH_INDEX_BASE &&
            VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX != 0xFFFFFFFFu;
    }

    RWTexture2D<uint2> debugVisTex = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::DebugVisualization)];
    uint2 payload = uint2(DEBUG_SENTINEL, DEBUG_SENTINEL);
    switch (outputType) {
        case OUTPUT_NORMAL:
            payload = PackDebugFloat3(sample.materialInputs.normalWS * 0.5 + 0.5);
            break;
        case OUTPUT_ALBEDO:
            payload = PackDebugFloat3(sample.materialInputs.albedo);
            break;
        case OUTPUT_METALLIC:
            payload = PackDebugFloat3(sample.materialInputs.metallic.xxx);
            break;
        case OUTPUT_ROUGHNESS:
            payload = PackDebugFloat3(sample.materialInputs.roughness.xxx);
            break;
        case OUTPUT_EMISSIVE:
            payload = PackDebugFloat3(sample.materialInputs.emissive);
            break;
        case OUTPUT_AO:
            payload = PackDebugFloat3(sample.materialInputs.ambientOcclusion.xxx);
            break;
        case OUTPUT_MESHLETS:
            payload = PackDebugUint(sample.meshletIndex);
            break;
        case OUTPUT_GEOMETRY_GROUP:
            payload = PackDebugUint(sample.geometryGroupIndex);
            break;
        case OUTPUT_MODEL_NORMALS:
            payload = PackDebugFloat3(sample.normalOS * 0.5 + 0.5);
            break;
        case OUTPUT_MOTION_VECTORS:
            payload = PackDebugFloat3(float3(sample.motionVector * 0.5 + 0.5, 0.5));
            break;
        case OUTPUT_REYES_GEOMETRY_PATH:
            payload = PackDebugFloat3(isReyesPatch ? float3(0.10, 0.95, 0.20) : float3(0.95, 0.15, 0.15));
            break;
        case OUTPUT_VOXEL_GEOMETRY_PATH:
            payload = PackDebugFloat3(sample.isVoxelPath ? float3(0.95, 0.15, 0.15) : float3(0.10, 0.95, 0.20));
            break;
        case OUTPUT_MATERIAL_SELECTED_MIP:
            if (sample.materialInputs.selectedMaterialMipLevel != MATERIAL_DEBUG_INVALID_MIP_LEVEL)
            {
                payload = PackDebugUint2(sample.materialInputs.selectedMaterialMipLevel, sample.materialInputs.selectedMaterialMipMaxLevel);
            }
            break;
        case OUTPUT_PARALLAX_PIXELS:
            payload = PackDebugFloat3(sample.materialInputs.parallaxApplied != 0u ? 1.0f.xxx : 0.0f.xxx);
            break;
    }
    if (payload.x != DEBUG_SENTINEL) {
        WriteDebugPixel(debugVisTex, pixel, payload);
    }
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
