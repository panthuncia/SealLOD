#include "Render/GraphExtensions/ClusterLOD/ReyesBuildRasterWorkPass.h"
#include "Render/InvocationRevision.h"

#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "BuiltinResources.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadTypes.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodReyesBuildRasterWorkRootConstants.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "RenderPasses/PreparedComputeDispatch.h"

ReyesBuildRasterWorkPass::ReyesBuildRasterWorkPass(
    std::shared_ptr<Buffer> diceQueueBuffer,
    std::shared_ptr<Buffer> diceQueueCounterBuffer,
    std::shared_ptr<Buffer> diceQueueReadOffsetBuffer,
    std::shared_ptr<Buffer> tessTableConfigsBuffer,
    std::shared_ptr<Buffer> rasterWorkBuffer,
    std::shared_ptr<Buffer> rasterWorkCounterBuffer,
    std::shared_ptr<Buffer> indirectArgsBuffer,
    std::shared_ptr<Buffer> telemetryBuffer,
    uint32_t rasterWorkCapacity,
    uint32_t phaseIndex,
    std::shared_ptr<Buffer> visibleClustersBuffer,
    std::shared_ptr<Buffer> visibleClusterTransformIndicesBuffer,
    std::shared_ptr<Buffer> viewDepthSrvIndicesBuffer,
    std::shared_ptr<Buffer> replayDiceQueueBuffer,
    std::shared_ptr<Buffer> replayDiceQueueCounterBuffer,
    std::shared_ptr<Buffer> replayDiceQueueOverflowBuffer,
    uint32_t replayDiceQueueCapacity,
    std::shared_ptr<ResourceGroup> slabResourceGroup)
    : m_diceQueueBuffer(std::move(diceQueueBuffer))
    , m_diceQueueCounterBuffer(std::move(diceQueueCounterBuffer))
    , m_diceQueueReadOffsetBuffer(std::move(diceQueueReadOffsetBuffer))
    , m_tessTableConfigsBuffer(std::move(tessTableConfigsBuffer))
    , m_rasterWorkBuffer(std::move(rasterWorkBuffer))
    , m_rasterWorkCounterBuffer(std::move(rasterWorkCounterBuffer))
    , m_indirectArgsBuffer(std::move(indirectArgsBuffer))
    , m_telemetryBuffer(std::move(telemetryBuffer))
    , m_visibleClustersBuffer(std::move(visibleClustersBuffer))
    , m_visibleClusterTransformIndicesBuffer(std::move(visibleClusterTransformIndicesBuffer))
    , m_viewDepthSrvIndicesBuffer(std::move(viewDepthSrvIndicesBuffer))
    , m_replayDiceQueueBuffer(std::move(replayDiceQueueBuffer))
    , m_replayDiceQueueCounterBuffer(std::move(replayDiceQueueCounterBuffer))
    , m_replayDiceQueueOverflowBuffer(std::move(replayDiceQueueOverflowBuffer))
    , m_slabResourceGroup(std::move(slabResourceGroup))
    , m_rasterWorkCapacity(rasterWorkCapacity)
    , m_phaseIndex(phaseIndex)
    , m_replayDiceQueueCapacity(replayDiceQueueCapacity) {
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/reyesBuildRasterWork.hlsl",
        L"BuildReyesRasterWorkCS",
        {},
        "CLod.ReyesBuildRasterWork.PSO");

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

ReyesBuildRasterWorkBindings ReyesBuildRasterWorkPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    builder.WithShaderResource(
            Builtin::PerMeshBuffer,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerObjectBuffer,
            Builtin::CullingCameraBuffer,
            Builtin::CameraBuffer,
            Builtin::PerMaterialDataBuffer,
            Builtin::CLod::Offsets,
            Builtin::CLod::MeshMetadata,
            Builtin::CLod::AssemblyTransforms,
            Builtin::CLod::AssemblyBoneRemaps,
            Builtin::CLod::AssemblyBoneRemapIndices,
            Builtin::SkeletonResources::InverseBindMatrices,
            Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo)
        .WithConstantBuffer(Builtin::PerFrameBuffer);
    ReyesBuildRasterWorkBindings bindings{builder.BindShaderResource(m_diceQueueBuffer),
        builder.BindShaderResource(m_diceQueueCounterBuffer)};
    if (m_diceQueueReadOffsetBuffer) {
        bindings.readOffset = builder.BindShaderResource(m_diceQueueReadOffsetBuffer);
        bindings.hasReadOffset = true;
    }
    bindings.tessConfigs = builder.BindShaderResource(m_tessTableConfigsBuffer);
    bindings.output = builder.BindUnorderedAccess(m_rasterWorkBuffer);
    bindings.outputCounter = builder.BindUnorderedAccess(m_rasterWorkCounterBuffer);
    bindings.indirectArgs = builder.BindIndirectArguments(m_indirectArgsBuffer);
    bindings.telemetry = builder.BindUnorderedAccess(m_telemetryBuffer);
    if (m_visibleClustersBuffer) {
        bindings.visibleClusters = builder.BindShaderResource(m_visibleClustersBuffer);
        bindings.hasVisibleClusters = true;
    }
    if (m_visibleClusterTransformIndicesBuffer) {
        bindings.visibleTransforms = builder.BindShaderResource(m_visibleClusterTransformIndicesBuffer);
        bindings.hasVisibleTransforms = true;
    }
    if (m_viewDepthSrvIndicesBuffer) {
        bindings.viewDepthIndices = builder.BindShaderResource(m_viewDepthSrvIndicesBuffer);
        bindings.hasViewDepthIndices = true;
    }
    if (m_replayDiceQueueBuffer) {
        bindings.replayQueue = builder.BindUnorderedAccess(m_replayDiceQueueBuffer);
        bindings.hasReplayQueue = true;
    }
    if (m_replayDiceQueueCounterBuffer) {
        bindings.replayCounter = builder.BindUnorderedAccess(m_replayDiceQueueCounterBuffer);
        bindings.hasReplayCounter = true;
    }
    if (m_replayDiceQueueOverflowBuffer) {
        bindings.replayOverflow = builder.BindUnorderedAccess(m_replayDiceQueueOverflowBuffer);
        bindings.hasReplayOverflow = true;
    }
    if (m_slabResourceGroup) {
        builder.WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
    }
    bindings.capacity = m_rasterWorkCapacity;
    bindings.phase = m_phaseIndex;
    bindings.replayCapacity = m_replayDiceQueueCapacity;
    bindings.useAabbOcclusion = SettingsManager::GetInstance().getSettingGetter<bool>(CLodReyesUseAabbOcclusionSettingName)();
    return bindings;
}

void ReyesBuildRasterWorkPass::Update(const UpdateExecutionContext& executionContext)
{
    (void)executionContext;
    const uint32_t zero = 0u;
    UploadBufferData(&zero, sizeof(uint32_t), org::runtime::UploadTarget::FromShared(m_rasterWorkCounterBuffer), 0);
}

br::render::PreparedComputeIndirect ReyesBuildRasterWorkPass::Prepare(
    const ReyesBuildRasterWorkBindings& bindings, const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    br::render::PreparedComputeIndirect data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle(); data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.commandSignature = preparation.CaptureCommandSignature(m_commandSignature);
    data.argumentsReference = preparation.CaptureResource(bindings.indirectArgs);
    auto program = preparation.CaptureProgramBinding(m_pso);
    data.program = program.program;
    data.descriptorIndices = std::move(program.descriptorIndices);
    const auto srv = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource}).index; };
    const auto uav = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess}).index; };
    data.constants[CLOD_REYES_BUILD_RASTER_WORK_DICE_QUEUE_DESCRIPTOR_INDEX] = srv(bindings.diceQueue);
    data.constants[CLOD_REYES_BUILD_RASTER_WORK_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.diceCounter);
    data.constants[CLOD_REYES_BUILD_RASTER_WORK_DICE_QUEUE_READ_OFFSET_DESCRIPTOR_INDEX] = bindings.hasReadOffset ? srv(bindings.readOffset) : 0xFFFFFFFFu;
    data.constants[CLOD_REYES_BUILD_RASTER_WORK_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = srv(bindings.tessConfigs);
    data.constants[CLOD_REYES_BUILD_RASTER_WORK_OUTPUT_DESCRIPTOR_INDEX] = uav(bindings.output);
    data.constants[CLOD_REYES_BUILD_RASTER_WORK_OUTPUT_COUNTER_DESCRIPTOR_INDEX] = uav(bindings.outputCounter);
    data.constants[CLOD_REYES_BUILD_RASTER_WORK_TELEMETRY_DESCRIPTOR_INDEX] = uav(bindings.telemetry);
    data.constants[CLOD_REYES_BUILD_RASTER_WORK_CAPACITY] = bindings.capacity;
    data.constants[CLOD_REYES_BUILD_RASTER_WORK_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = bindings.hasVisibleClusters ? srv(bindings.visibleClusters) : 0xFFFFFFFFu;
    data.constants[CLOD_REYES_BUILD_RASTER_WORK_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = bindings.hasVisibleTransforms ? srv(bindings.visibleTransforms) : 0xFFFFFFFFu;
    data.constants[CLOD_REYES_BUILD_RASTER_WORK_VIEW_DEPTH_SRV_INDICES_DESCRIPTOR_INDEX] = bindings.hasViewDepthIndices ? srv(bindings.viewDepthIndices) : 0xFFFFFFFFu;
    data.constants[CLOD_REYES_BUILD_RASTER_WORK_REPLAY_DICE_QUEUE_DESCRIPTOR_INDEX] = bindings.hasReplayQueue ? uav(bindings.replayQueue) : 0xFFFFFFFFu;
    data.constants[CLOD_REYES_BUILD_RASTER_WORK_REPLAY_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] = bindings.hasReplayCounter ? uav(bindings.replayCounter) : 0xFFFFFFFFu;
    data.constants[CLOD_REYES_BUILD_RASTER_WORK_REPLAY_DICE_QUEUE_OVERFLOW_DESCRIPTOR_INDEX] = bindings.hasReplayOverflow ? uav(bindings.replayOverflow) : 0xFFFFFFFFu;
    data.constants[CLOD_REYES_BUILD_RASTER_WORK_ENABLE_PATCH_OCCLUSION] = (bindings.hasVisibleClusters && bindings.hasViewDepthIndices && bindings.hasReplayQueue && bindings.hasReplayCounter && bindings.hasReplayOverflow) ? 1u : 0u;
    data.constants[CLOD_REYES_BUILD_RASTER_WORK_PHASE_INDEX] = bindings.phase;
    data.constants[CLOD_REYES_BUILD_RASTER_WORK_REPLAY_DICE_QUEUE_CAPACITY] = bindings.replayCapacity;
    data.constants[CLOD_REYES_BUILD_RASTER_WORK_USE_AABB_OCCLUSION] = bindings.useAabbOcclusion ? 1u : 0u;
    data.constants[CLOD_REYES_BUILD_RASTER_WORK_TERRAIN_RVT_ENABLED] = 0u;
    return data;
}

void ReyesBuildRasterWorkPass::InvocationRevision(const org::PassPrepareContext& preparation, std::vector<uint64_t>& out) const {
    br::render::AppendFrameHeapRevision(preparation, out);
    out.push_back(br::render::PipelineRevision(m_pso));
    out.push_back(br::render::OwnerRevision(m_commandSignature));
}

void ReyesBuildRasterWorkPass::Record(const ReyesBuildRasterWorkBindings&,
    const br::render::PreparedComputeIndirect& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeIndirect(data, recording);
}
