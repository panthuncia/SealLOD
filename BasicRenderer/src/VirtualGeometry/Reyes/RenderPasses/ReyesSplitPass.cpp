#include "VirtualGeometry/Reyes/RenderPasses/ReyesSplitPass.h"
#include "Runtime/StateGraph/InvocationRevision.h"

#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BuiltinResources.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodReyesSplitRootConstants.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "Utilities/Utilities.h"

ReyesSplitPass::ReyesSplitPass(
    std::shared_ptr<org::Buffer> visibleClustersBuffer,
    std::shared_ptr<org::Buffer> inputSplitQueueBuffer,
    std::shared_ptr<org::Buffer> inputSplitQueueCounterBuffer,
    std::shared_ptr<org::Buffer> outputSplitQueueBuffer,
    std::shared_ptr<org::Buffer> outputSplitQueueCounterBuffer,
    std::shared_ptr<org::Buffer> outputSplitQueueOverflowBuffer,
    std::shared_ptr<org::Buffer> diceQueueBuffer,
    std::shared_ptr<org::Buffer> diceQueueCounterBuffer,
    std::shared_ptr<org::Buffer> diceQueueOverflowBuffer,
    std::shared_ptr<org::Buffer> tessTableConfigsBuffer,
    std::shared_ptr<org::Buffer> tessTableVerticesBuffer,
    std::shared_ptr<org::Buffer> tessTableTrianglesBuffer,
    std::shared_ptr<org::Buffer> shadowClipmapInfoBuffer,
    std::shared_ptr<org::PixelBuffer> shadowDirtyHierarchyTexture,
    std::shared_ptr<org::PixelBuffer> shadowNonRasterableHierarchyTexture,
    std::shared_ptr<org::Buffer> indirectArgsBuffer,
    std::shared_ptr<org::Buffer> telemetryBuffer,
    uint32_t maxSplitQueueEntries,
    uint32_t splitPassIndex,
    uint32_t maxSplitPassCount,
    uint32_t phaseIndex,
    bool enableViewDepthOcclusion,
    std::shared_ptr<org::Buffer> replaySplitQueueBuffer,
    std::shared_ptr<org::Buffer> replaySplitQueueCounterBuffer,
    std::shared_ptr<org::Buffer> replaySplitQueueOverflowBuffer)
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
    , m_enableViewDepthOcclusion(enableViewDepthOcclusion)
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
    rhi::CommandSignaturePtr commandSignature;
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(dispatchArgs, 1), sizeof(CLodReyesDispatchIndirectCommand) },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        commandSignature);
    m_commandSignature = std::make_shared<rhi::CommandSignaturePtr>(std::move(commandSignature));
}

ReyesSplitBindings ReyesSplitPass::Declare(org::PassBuilder& declaration)
{
    declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    auto* builder = &declaration;
    builder->ShaderResource(
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerObjectBuffer,
            Builtin::PerMeshBuffer,
            Builtin::PerMaterialDataBuffer,
            Builtin::PerMaterialOpenPBRDataBuffer,
            Builtin::Material::TextureStreamingMetadataBuffer,
            Builtin::CullingCameraBuffer,
            Builtin::CameraBuffer);
    builder->UnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer);
    ReyesSplitBindings bindings;
    bindings.visible = builder->ShaderResource(m_visibleClustersBuffer).View();
    bindings.inputQueue = builder->ShaderResource(m_inputSplitQueueBuffer).View();
    bindings.inputCounter = builder->ShaderResource(m_inputSplitQueueCounterBuffer).View();
    bindings.outputQueue = builder->UnorderedAccess(m_outputSplitQueueBuffer).View();
    bindings.outputCounter = builder->UnorderedAccess(m_outputSplitQueueCounterBuffer).View();
    bindings.outputOverflow = builder->UnorderedAccess(m_outputSplitQueueOverflowBuffer).View();
    bindings.diceQueue = builder->UnorderedAccess(m_diceQueueBuffer).View();
    bindings.diceCounter = builder->UnorderedAccess(m_diceQueueCounterBuffer).View();
    bindings.diceOverflow = builder->UnorderedAccess(m_diceQueueOverflowBuffer).View();
    bindings.tessConfigs = builder->ShaderResource(m_tessTableConfigsBuffer).View();
    bindings.tessVertices = builder->ShaderResource(m_tessTableVerticesBuffer).View();
    bindings.tessTriangles = builder->ShaderResource(m_tessTableTrianglesBuffer).View();
    bindings.indirectArgs = builder->IndirectArguments(m_indirectArgsBuffer);
    bindings.telemetry = builder->UnorderedAccess(m_telemetryBuffer).View();
    if (m_enableViewDepthOcclusion) {
        m_viewDepthTable.Declare(*builder);
        bindings.viewDepthLayout = m_viewDepthTable.Layout();
        bindings.hasViewDepth = true;
    }
    if (m_replaySplitQueueBuffer) {
        bindings.replayQueue = builder->UnorderedAccess(m_replaySplitQueueBuffer).View(); bindings.hasReplayQueue = true;
    }
    if (m_replaySplitQueueCounterBuffer) {
        bindings.replayCounter = builder->UnorderedAccess(m_replaySplitQueueCounterBuffer).View(); bindings.hasReplayCounter = true;
    }
    if (m_replaySplitQueueOverflowBuffer) {
        bindings.replayOverflow = builder->UnorderedAccess(m_replaySplitQueueOverflowBuffer).View(); bindings.hasReplayOverflow = true;
    }
    if (m_shadowClipmapInfoBuffer) {
        bindings.shadowClipmap = builder->ShaderResource(m_shadowClipmapInfoBuffer).View();
        builder->ShaderResource(Builtin::Shadows::CLodCompactShadowCameras);
        bindings.hasShadowClipmap = true;
    }
    if (m_shadowDirtyHierarchyTexture) {
        bindings.shadowDirty = builder->ShaderResource(m_shadowDirtyHierarchyTexture, org::SrvView{static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull)}).View(); bindings.hasShadowDirty = true;
    }
    if (m_shadowNonRasterableHierarchyTexture) {
        bindings.shadowNonRasterable = builder->ShaderResource(m_shadowNonRasterableHierarchyTexture, org::SrvView{static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull)}).View(); bindings.hasShadowNonRasterable = true;
    }

    builder->ConstantBuffer(Builtin::PerFrameBuffer);
    bindings.capacity = m_maxSplitQueueEntries;
    bindings.maxPassCount = m_maxSplitPassCount;
    bindings.phase = m_phaseIndex;
    bindings.useAabbOcclusion = SettingsManager::GetInstance().getSettingGetter<bool>(CLodReyesUseAabbOcclusionSettingName)();
    bindings.coarseTargetBits = as_uint(std::max(SettingsManager::GetInstance().getSettingGetter<float>(CLodReyesShadowCoarseTargetPagesPerTriangleSettingName)(), CLodReyesShadowCoarseTargetPagesPerTriangleMin));
    return bindings;
}



void ReyesSplitPass::Update(const org::UpdateExecutionContext& executionContext) {
    const auto* context = executionContext.hostData->Get<UpdateContext>();
    m_declaredResourcesChanged = m_enableViewDepthOcclusion
        && m_viewDepthTable.Update(context->Views(), m_phaseIndex == 1u);
}

ReyesSplitFrameData ReyesSplitPass::Prepare(const ReyesSplitBindings& bindings,
    const org::PassPrepareContext& preparation) const
{
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    ReyesSplitFrameData data{};
    data.clear.resourceHeap = data.split.resourceHeap = context->textureDescriptorHeap.GetHandle();
    data.clear.samplerHeap = data.split.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    auto clearProgram = preparation.CaptureProgramBinding(m_clearCountersPso);
    data.clear.program = clearProgram.program;
    data.clear.descriptorIndices = std::move(clearProgram.descriptorIndices);
    data.clear.groupsX = 1;
    auto splitProgram = preparation.CaptureProgramBinding(m_pso);
    data.split.program = splitProgram.program;
    data.split.descriptorIndices = std::move(splitProgram.descriptorIndices);
    data.split.commandSignature = preparation.CaptureCommandSignature(m_commandSignature);
    data.split.argumentsReference = preparation.CaptureResource(bindings.indirectArgs);
    data.outputCounters = {preparation.DeclaredReference(bindings.outputCounter), preparation.DeclaredReference(bindings.outputOverflow)};
    auto& c = data.split.constants;
    const auto srv = [&](org::DeclaredViewToken token) { return preparation.Resolve(token).index; };
    const auto uav = [&](org::DeclaredViewToken token) { return preparation.Resolve(token).index; };
    c[CLOD_REYES_SPLIT_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = srv(bindings.visible);
    c[CLOD_REYES_SPLIT_MAX_PASS_COUNT] = bindings.maxPassCount;
    c[CLOD_REYES_SPLIT_INPUT_QUEUE_DESCRIPTOR_INDEX] = srv(bindings.inputQueue);
    c[CLOD_REYES_SPLIT_INPUT_QUEUE_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.inputCounter);
    c[CLOD_REYES_SPLIT_OUTPUT_SPLIT_QUEUE_DESCRIPTOR_INDEX] = uav(bindings.outputQueue);
    c[CLOD_REYES_SPLIT_OUTPUT_SPLIT_QUEUE_COUNTER_DESCRIPTOR_INDEX] = uav(bindings.outputCounter);
    c[CLOD_REYES_SPLIT_OUTPUT_SPLIT_QUEUE_OVERFLOW_DESCRIPTOR_INDEX] = uav(bindings.outputOverflow);
    c[CLOD_REYES_SPLIT_OUTPUT_DICE_QUEUE_DESCRIPTOR_INDEX] = uav(bindings.diceQueue);
    c[CLOD_REYES_SPLIT_OUTPUT_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] = uav(bindings.diceCounter);
    c[CLOD_REYES_SPLIT_OUTPUT_DICE_QUEUE_OVERFLOW_DESCRIPTOR_INDEX] = uav(bindings.diceOverflow);
    c[CLOD_REYES_SPLIT_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = srv(bindings.tessConfigs);
    c[CLOD_REYES_SPLIT_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX] = srv(bindings.tessVertices);
    c[CLOD_REYES_SPLIT_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX] = srv(bindings.tessTriangles);
    c[CLOD_REYES_SPLIT_QUEUE_CAPACITY] = bindings.capacity;
    c[CLOD_REYES_SPLIT_TELEMETRY_DESCRIPTOR_INDEX] = uav(bindings.telemetry);
    c[CLOD_REYES_SPLIT_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX] = bindings.hasShadowClipmap ? srv(bindings.shadowClipmap) : 0xFFFFFFFFu;
    c[CLOD_REYES_SPLIT_SHADOW_DIRTY_HIERARCHY_DESCRIPTOR_INDEX] = bindings.hasShadowDirty ? srv(bindings.shadowDirty) : 0xFFFFFFFFu;
    c[CLOD_REYES_SPLIT_SHADOW_NON_RASTERABLE_HIERARCHY_DESCRIPTOR_INDEX] = bindings.hasShadowNonRasterable ? srv(bindings.shadowNonRasterable) : 0xFFFFFFFFu;
    c[CLOD_REYES_SPLIT_VIEW_DEPTH_SRV_INDICES_DESCRIPTOR_INDEX] = bindings.hasViewDepth
        ? bindings.viewDepthLayout.Publish(preparation, m_viewDepthPublisher, m_viewDepthTable.Rows())
        : 0xFFFFFFFFu;
    c[CLOD_REYES_SPLIT_REPLAY_SPLIT_QUEUE_DESCRIPTOR_INDEX] = bindings.hasReplayQueue ? uav(bindings.replayQueue) : 0xFFFFFFFFu;
    c[CLOD_REYES_SPLIT_REPLAY_SPLIT_QUEUE_COUNTER_DESCRIPTOR_INDEX] = bindings.hasReplayCounter ? uav(bindings.replayCounter) : 0xFFFFFFFFu;
    c[CLOD_REYES_SPLIT_REPLAY_SPLIT_QUEUE_OVERFLOW_DESCRIPTOR_INDEX] = bindings.hasReplayOverflow ? uav(bindings.replayOverflow) : 0xFFFFFFFFu;
    c[CLOD_REYES_SPLIT_ENABLE_PATCH_OCCLUSION] = bindings.hasViewDepth && bindings.hasReplayQueue && bindings.hasReplayCounter && bindings.hasReplayOverflow;
    c[CLOD_REYES_SPLIT_PHASE_INDEX] = bindings.phase;
    c[CLOD_REYES_SPLIT_USE_AABB_OCCLUSION] = bindings.useAabbOcclusion ? 1u : 0u;
    c[UintRootConstant18] = bindings.coarseTargetBits;
    data.clear.constants = data.split.constants;
    return data;
}

void ReyesSplitPass::InvocationRevision(const org::PassPrepareContext& preparation, std::vector<uint64_t>& out) const {
    br::render::AppendFrameHeapRevision(preparation, out);
    out.push_back(br::render::PipelineRevision(m_clearCountersPso));
    out.push_back(br::render::PipelineRevision(m_pso));
    out.push_back(br::render::OwnerRevision(m_commandSignature));
}

void ReyesSplitPass::Record(const ReyesSplitBindings&, const ReyesSplitFrameData& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeDispatch(data.clear, recording);
    // Split atomics must observe the preceding counter reset.
    std::array<rhi::BufferBarrier, 2> counters{};
    for (size_t index = 0; index < counters.size(); ++index) {
        counters[index].buffer = recording.Resolve(data.outputCounters[index]).GetHandle();
        counters[index].beforeAccess = counters[index].afterAccess = rhi::ResourceAccessType::UnorderedAccess;
        counters[index].beforeSync = counters[index].afterSync = rhi::ResourceSyncState::ComputeShading;
    }
    rhi::BarrierBatch barriers{};
    barriers.buffers = {counters.data(), static_cast<uint32_t>(counters.size())};
    recording.Commands().Barriers(barriers);
    br::render::RecordPreparedComputeIndirect(data.split, recording);
}
