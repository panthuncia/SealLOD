#include "Render/GraphExtensions/ClusterLOD/ReyesSplitPass.h"

#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "BuiltinResources.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodReyesSplitRootConstants.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "Utilities/Utilities.h"

ReyesSplitPass::ReyesSplitPass(
    std::shared_ptr<Buffer> visibleClustersBuffer,
    std::shared_ptr<Buffer> inputSplitQueueBuffer,
    std::shared_ptr<Buffer> inputSplitQueueCounterBuffer,
    std::shared_ptr<Buffer> outputSplitQueueBuffer,
    std::shared_ptr<Buffer> outputSplitQueueCounterBuffer,
    std::shared_ptr<Buffer> outputSplitQueueOverflowBuffer,
    std::shared_ptr<Buffer> diceQueueBuffer,
    std::shared_ptr<Buffer> diceQueueCounterBuffer,
    std::shared_ptr<Buffer> diceQueueOverflowBuffer,
    std::shared_ptr<Buffer> tessTableConfigsBuffer,
    std::shared_ptr<Buffer> tessTableVerticesBuffer,
    std::shared_ptr<Buffer> tessTableTrianglesBuffer,
    std::shared_ptr<Buffer> shadowClipmapInfoBuffer,
    std::shared_ptr<PixelBuffer> shadowDirtyHierarchyTexture,
    std::shared_ptr<PixelBuffer> shadowNonRasterableHierarchyTexture,
    std::shared_ptr<Buffer> indirectArgsBuffer,
    std::shared_ptr<Buffer> telemetryBuffer,
    uint32_t maxSplitQueueEntries,
    uint32_t splitPassIndex,
    uint32_t maxSplitPassCount,
    uint32_t phaseIndex,
    std::shared_ptr<Buffer> viewDepthSrvIndicesBuffer,
    std::shared_ptr<Buffer> replaySplitQueueBuffer,
    std::shared_ptr<Buffer> replaySplitQueueCounterBuffer,
    std::shared_ptr<Buffer> replaySplitQueueOverflowBuffer)
    : m_visibleClustersBuffer(std::move(visibleClustersBuffer))
    , m_inputSplitQueueBuffer(std::move(inputSplitQueueBuffer))
    , m_inputSplitQueueCounterBuffer(std::move(inputSplitQueueCounterBuffer))
    , m_outputSplitQueueBuffer(std::move(outputSplitQueueBuffer))
    , m_outputSplitQueueCounterBuffer(std::move(outputSplitQueueCounterBuffer))
    , m_outputSplitQueueOverflowBuffer(std::move(outputSplitQueueOverflowBuffer))
    , m_diceQueueBuffer(std::move(diceQueueBuffer))
    , m_diceQueueCounterBuffer(std::move(diceQueueCounterBuffer))
    , m_diceQueueOverflowBuffer(std::move(diceQueueOverflowBuffer))
    , m_tessTableConfigsBuffer(std::move(tessTableConfigsBuffer))
    , m_tessTableVerticesBuffer(std::move(tessTableVerticesBuffer))
    , m_tessTableTrianglesBuffer(std::move(tessTableTrianglesBuffer))
    , m_shadowClipmapInfoBuffer(std::move(shadowClipmapInfoBuffer))
    , m_shadowDirtyHierarchyTexture(std::move(shadowDirtyHierarchyTexture))
    , m_shadowNonRasterableHierarchyTexture(std::move(shadowNonRasterableHierarchyTexture))
    , m_indirectArgsBuffer(std::move(indirectArgsBuffer))
    , m_telemetryBuffer(std::move(telemetryBuffer))
    , m_viewDepthSrvIndicesBuffer(std::move(viewDepthSrvIndicesBuffer))
    , m_replaySplitQueueBuffer(std::move(replaySplitQueueBuffer))
    , m_replaySplitQueueCounterBuffer(std::move(replaySplitQueueCounterBuffer))
    , m_replaySplitQueueOverflowBuffer(std::move(replaySplitQueueOverflowBuffer))
    , m_maxSplitQueueEntries(maxSplitQueueEntries)
    , m_splitPassIndex(splitPassIndex)
    , m_maxSplitPassCount(maxSplitPassCount)
    , m_phaseIndex(phaseIndex) {
    m_clearCountersPso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"ClearReyesSplitOutputCountersCSMain",
        {},
        "CLod.ReyesSplit.ClearCounters.PSO");

    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/reyesSplit.hlsl",
        L"ReyesSplitCS",
        {},
        "CLod.ReyesSplit.PSO");

    rhi::IndirectArg dispatchArgs[] = {
        {.kind = rhi::IndirectArgKind::Dispatch }
    };

    auto device = DeviceManager::GetInstance().GetDevice();
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(dispatchArgs, 1), sizeof(CLodReyesDispatchIndirectCommand) },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        m_commandSignature);
}

void ReyesSplitPass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    builder->WithShaderResource(
            m_visibleClustersBuffer,
            m_inputSplitQueueBuffer,
            m_inputSplitQueueCounterBuffer,
            m_tessTableConfigsBuffer,
            m_tessTableVerticesBuffer,
            m_tessTableTrianglesBuffer,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerObjectBuffer,
            Builtin::PerMeshBuffer,
            Builtin::PerMaterialDataBuffer,
            Builtin::PerMaterialOpenPBRDataBuffer,
            Builtin::Material::TextureStreamingMetadataBuffer,
            Builtin::CullingCameraBuffer,
            Builtin::CameraBuffer)
		.WithUnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer)
        .WithIndirectArguments(m_indirectArgsBuffer)
        .WithUnorderedAccess(
            m_outputSplitQueueBuffer,
            m_outputSplitQueueCounterBuffer,
            m_outputSplitQueueOverflowBuffer,
            m_diceQueueBuffer,
            m_diceQueueCounterBuffer,
            m_diceQueueOverflowBuffer,
            m_telemetryBuffer);
    if (m_viewDepthSrvIndicesBuffer) {
        builder->WithShaderResource(m_viewDepthSrvIndicesBuffer);
    }
    if (m_replaySplitQueueBuffer) {
        builder->WithUnorderedAccess(m_replaySplitQueueBuffer);
    }
    if (m_replaySplitQueueCounterBuffer) {
        builder->WithUnorderedAccess(m_replaySplitQueueCounterBuffer);
    }
    if (m_replaySplitQueueOverflowBuffer) {
        builder->WithUnorderedAccess(m_replaySplitQueueOverflowBuffer);
    }
    if (m_shadowClipmapInfoBuffer) {
        builder->WithShaderResource(m_shadowClipmapInfoBuffer, Builtin::Shadows::CLodCompactShadowCameras);
    }
    if (m_shadowDirtyHierarchyTexture) {
        builder->WithShaderResource(m_shadowDirtyHierarchyTexture);
    }
    if (m_shadowNonRasterableHierarchyTexture) {
        builder->WithShaderResource(m_shadowNonRasterableHierarchyTexture);
    }

    builder->WithConstantBuffer(Builtin::PerFrameBuffer);
}

void ReyesSplitPass::Setup() {
}

PassReturn ReyesSplitPass::Execute(PassExecutionContext& executionContext)
{
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());

    uint32_t uintRootConstants[NumMiscUintRootConstants] = {};
    uintRootConstants[CLOD_REYES_SPLIT_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = m_visibleClustersBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_SPLIT_MAX_PASS_COUNT] = m_maxSplitPassCount;
    uintRootConstants[CLOD_REYES_SPLIT_INPUT_QUEUE_DESCRIPTOR_INDEX] = m_inputSplitQueueBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_SPLIT_INPUT_QUEUE_COUNTER_DESCRIPTOR_INDEX] = m_inputSplitQueueCounterBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_SPLIT_OUTPUT_SPLIT_QUEUE_DESCRIPTOR_INDEX] = m_outputSplitQueueBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_SPLIT_OUTPUT_SPLIT_QUEUE_COUNTER_DESCRIPTOR_INDEX] = m_outputSplitQueueCounterBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_SPLIT_OUTPUT_SPLIT_QUEUE_OVERFLOW_DESCRIPTOR_INDEX] = m_outputSplitQueueOverflowBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_SPLIT_OUTPUT_DICE_QUEUE_DESCRIPTOR_INDEX] = m_diceQueueBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_SPLIT_OUTPUT_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] = m_diceQueueCounterBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_SPLIT_OUTPUT_DICE_QUEUE_OVERFLOW_DESCRIPTOR_INDEX] = m_diceQueueOverflowBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_SPLIT_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = m_tessTableConfigsBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_SPLIT_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX] = m_tessTableVerticesBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_SPLIT_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX] = m_tessTableTrianglesBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_SPLIT_QUEUE_CAPACITY] = m_maxSplitQueueEntries;
    uintRootConstants[CLOD_REYES_SPLIT_TELEMETRY_DESCRIPTOR_INDEX] = m_telemetryBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_SPLIT_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX] = m_shadowClipmapInfoBuffer
        ? m_shadowClipmapInfoBuffer->GetSRVInfo(0).slot.index
        : 0xFFFFFFFFu;
    uintRootConstants[CLOD_REYES_SPLIT_SHADOW_DIRTY_HIERARCHY_DESCRIPTOR_INDEX] = m_shadowDirtyHierarchyTexture
        ? m_shadowDirtyHierarchyTexture->GetSRVInfo(SRVViewType::Texture2DArrayFull, 0).slot.index
        : 0xFFFFFFFFu;
    uintRootConstants[CLOD_REYES_SPLIT_SHADOW_NON_RASTERABLE_HIERARCHY_DESCRIPTOR_INDEX] = m_shadowNonRasterableHierarchyTexture
        ? m_shadowNonRasterableHierarchyTexture->GetSRVInfo(SRVViewType::Texture2DArrayFull, 0).slot.index
        : 0xFFFFFFFFu;
    uintRootConstants[CLOD_REYES_SPLIT_VIEW_DEPTH_SRV_INDICES_DESCRIPTOR_INDEX] = m_viewDepthSrvIndicesBuffer
        ? m_viewDepthSrvIndicesBuffer->GetSRVInfo(0).slot.index
        : 0xFFFFFFFFu;
    uintRootConstants[CLOD_REYES_SPLIT_REPLAY_SPLIT_QUEUE_DESCRIPTOR_INDEX] = m_replaySplitQueueBuffer
        ? m_replaySplitQueueBuffer->GetUAVShaderVisibleInfo(0).slot.index
        : 0xFFFFFFFFu;
    uintRootConstants[CLOD_REYES_SPLIT_REPLAY_SPLIT_QUEUE_COUNTER_DESCRIPTOR_INDEX] = m_replaySplitQueueCounterBuffer
        ? m_replaySplitQueueCounterBuffer->GetUAVShaderVisibleInfo(0).slot.index
        : 0xFFFFFFFFu;
    uintRootConstants[CLOD_REYES_SPLIT_REPLAY_SPLIT_QUEUE_OVERFLOW_DESCRIPTOR_INDEX] = m_replaySplitQueueOverflowBuffer
        ? m_replaySplitQueueOverflowBuffer->GetUAVShaderVisibleInfo(0).slot.index
        : 0xFFFFFFFFu;
    uintRootConstants[CLOD_REYES_SPLIT_ENABLE_PATCH_OCCLUSION] =
        (m_viewDepthSrvIndicesBuffer && m_replaySplitQueueBuffer && m_replaySplitQueueCounterBuffer && m_replaySplitQueueOverflowBuffer)
            ? 1u
            : 0u;
    uintRootConstants[CLOD_REYES_SPLIT_PHASE_INDEX] = m_phaseIndex;
    uintRootConstants[CLOD_REYES_SPLIT_USE_AABB_OCCLUSION] =
        SettingsManager::GetInstance().getSettingGetter<bool>(CLodReyesUseAabbOcclusionSettingName)() ? 1u : 0u;
    uintRootConstants[UintRootConstant18] = as_uint(std::max(
        SettingsManager::GetInstance().getSettingGetter<float>(CLodReyesShadowCoarseTargetPagesPerTriangleSettingName)(),
        CLodReyesShadowCoarseTargetPagesPerTriangleMin));

    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    commandList.BindPipeline(m_clearCountersPso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, m_clearCountersPso.GetResourceDescriptorSlots());
    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        uintRootConstants);
    commandList.Dispatch(1, 1, 1);

    commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());

    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        uintRootConstants);

    commandList.ExecuteIndirect(m_commandSignature->GetHandle(), m_indirectArgsBuffer->GetAPIResource().GetHandle(), 0, {}, 0, 1);

    return {};
}

void ReyesSplitPass::Update(const UpdateExecutionContext& executionContext)
{
    (void)executionContext;
}

void ReyesSplitPass::Cleanup() {}
