#include "VirtualGeometry/Reyes/RenderPasses/ReyesBuildRasterWorkPass.h"
#include "Render/PassBuilders.h"

#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BuiltinResources.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Render/Runtime/UploadTypes.h"
#include "Resources/Buffers/Buffer.h"
#include "Runtime/GraphIntegration/Resolvers/ResourceGroupResolver.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodReyesBuildRasterWorkRootConstants.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"

ReyesBuildRasterWorkPass::ReyesBuildRasterWorkPass(
    std::shared_ptr<org::Buffer> diceQueueBuffer,
    std::shared_ptr<org::Buffer> diceQueueCounterBuffer,
    std::shared_ptr<org::Buffer> diceQueueReadOffsetBuffer,
    std::shared_ptr<org::Buffer> tessTableConfigsBuffer,
    std::shared_ptr<org::Buffer> rasterWorkBuffer,
    std::shared_ptr<org::Buffer> rasterWorkCounterBuffer,
    std::shared_ptr<org::Buffer> indirectArgsBuffer,
    std::shared_ptr<org::Buffer> telemetryBuffer,
    uint32_t rasterWorkCapacity,
    uint32_t phaseIndex,
    std::shared_ptr<org::Buffer> visibleClustersBuffer,
    std::shared_ptr<org::Buffer> visibleClusterTransformIndicesBuffer,
    bool enableViewDepthOcclusion,
    std::shared_ptr<org::Buffer> replayDiceQueueBuffer,
    std::shared_ptr<org::Buffer> replayDiceQueueCounterBuffer,
    std::shared_ptr<org::Buffer> replayDiceQueueOverflowBuffer,
    uint32_t replayDiceQueueCapacity,
    std::shared_ptr<org::ResourceGroup> slabResourceGroup)
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
    , m_enableViewDepthOcclusion(enableViewDepthOcclusion)
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
    builder.ShaderResource(
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
            Builtin::SkeletonResources::SkinningInstanceInfo);
    builder.ConstantBuffer(Builtin::PerFrameBuffer);
    ReyesBuildRasterWorkBindings bindings{m_pso, m_commandSignature, builder.ShaderResource(m_diceQueueBuffer).View(),
        builder.ShaderResource(m_diceQueueCounterBuffer).View()};
    if (m_diceQueueReadOffsetBuffer) {
        bindings.readOffset = builder.ShaderResource(m_diceQueueReadOffsetBuffer).View();
        bindings.hasReadOffset = true;
    }
    bindings.tessConfigs = builder.ShaderResource(m_tessTableConfigsBuffer).View();
    bindings.output = builder.UnorderedAccess(m_rasterWorkBuffer).View();
    bindings.outputCounter = builder.UnorderedAccess(m_rasterWorkCounterBuffer).View();
    bindings.indirectArgs = builder.IndirectArguments(m_indirectArgsBuffer);
    bindings.telemetry = builder.UnorderedAccess(m_telemetryBuffer).View();
    if (m_visibleClustersBuffer) {
        bindings.visibleClusters = builder.ShaderResource(m_visibleClustersBuffer).View();
        bindings.hasVisibleClusters = true;
    }
    if (m_visibleClusterTransformIndicesBuffer) {
        bindings.visibleTransforms = builder.ShaderResource(m_visibleClusterTransformIndicesBuffer).View();
        bindings.hasVisibleTransforms = true;
    }
    if (m_enableViewDepthOcclusion) {
        m_viewDepthTable.Declare(builder);
        bindings.viewDepthLayout = m_viewDepthTable.Layout();
        bindings.hasViewDepthIndices = true;
    }
    if (m_replayDiceQueueBuffer) {
        bindings.replayQueue = builder.UnorderedAccess(m_replayDiceQueueBuffer).View();
        bindings.hasReplayQueue = true;
    }
    if (m_replayDiceQueueCounterBuffer) {
        bindings.replayCounter = builder.UnorderedAccess(m_replayDiceQueueCounterBuffer).View();
        bindings.hasReplayCounter = true;
    }
    if (m_replayDiceQueueOverflowBuffer) {
        bindings.replayOverflow = builder.UnorderedAccess(m_replayDiceQueueOverflowBuffer).View();
        bindings.hasReplayOverflow = true;
    }
    if (m_slabResourceGroup) {
        builder.ShaderResource(ResourceGroupResolver(m_slabResourceGroup));
    }
    bindings.capacity = m_rasterWorkCapacity;
    bindings.phase = m_phaseIndex;
    bindings.replayCapacity = m_replayDiceQueueCapacity;
    bindings.useAabbOcclusion = SettingsManager::GetInstance().getSettingGetter<bool>(CLodReyesUseAabbOcclusionSettingName)();
    return bindings;
}

void ReyesBuildRasterWorkPass::Update(const org::UpdateExecutionContext& executionContext)
{
    const auto* context = executionContext.hostData->Get<UpdateContext>();
    m_declaredResourcesChanged = m_enableViewDepthOcclusion
        && m_viewDepthTable.Update(context->Views(), m_phaseIndex == 1u);
    const uint32_t zero = 0u;
    UploadBufferData(&zero, sizeof(uint32_t), org::runtime::UploadTarget::FromShared(m_rasterWorkCounterBuffer), 0);
}

ReyesBuildRasterWorkFrame ReyesBuildRasterWorkPass::Prepare(
    const ReyesBuildRasterWorkBindings& bindings, const org::PassPrepareContext& preparation) const {
    ReyesBuildRasterWorkFrame frame{};
    if (bindings.hasViewDepthIndices) {
        frame.viewDepthTableIndex = bindings.viewDepthLayout.Publish(preparation, m_viewDepthPublisher, m_viewDepthTable.Rows());
    }
    return frame;
}

void ReyesBuildRasterWorkPass::Record(const ReyesBuildRasterWorkBindings& bindings,
    const ReyesBuildRasterWorkFrame& frame, org::PassRecordContext& recording) {
    br::render::MiscConstants constants{};
    const auto srv = [&](const org::DeclaredViewToken& token) { return recording.Index(token); };
    const auto uav = [&](const org::DeclaredViewToken& token) { return recording.Index(token); };
    constants[CLOD_REYES_BUILD_RASTER_WORK_DICE_QUEUE_DESCRIPTOR_INDEX] = srv(bindings.diceQueue);
    constants[CLOD_REYES_BUILD_RASTER_WORK_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.diceCounter);
    constants[CLOD_REYES_BUILD_RASTER_WORK_DICE_QUEUE_READ_OFFSET_DESCRIPTOR_INDEX] = bindings.hasReadOffset ? srv(bindings.readOffset) : 0xFFFFFFFFu;
    constants[CLOD_REYES_BUILD_RASTER_WORK_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = srv(bindings.tessConfigs);
    constants[CLOD_REYES_BUILD_RASTER_WORK_OUTPUT_DESCRIPTOR_INDEX] = uav(bindings.output);
    constants[CLOD_REYES_BUILD_RASTER_WORK_OUTPUT_COUNTER_DESCRIPTOR_INDEX] = uav(bindings.outputCounter);
    constants[CLOD_REYES_BUILD_RASTER_WORK_TELEMETRY_DESCRIPTOR_INDEX] = uav(bindings.telemetry);
    constants[CLOD_REYES_BUILD_RASTER_WORK_CAPACITY] = bindings.capacity;
    constants[CLOD_REYES_BUILD_RASTER_WORK_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = bindings.hasVisibleClusters ? srv(bindings.visibleClusters) : 0xFFFFFFFFu;
    constants[CLOD_REYES_BUILD_RASTER_WORK_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = bindings.hasVisibleTransforms ? srv(bindings.visibleTransforms) : 0xFFFFFFFFu;
    constants[CLOD_REYES_BUILD_RASTER_WORK_VIEW_DEPTH_SRV_INDICES_DESCRIPTOR_INDEX] = frame.viewDepthTableIndex;
    constants[CLOD_REYES_BUILD_RASTER_WORK_REPLAY_DICE_QUEUE_DESCRIPTOR_INDEX] = bindings.hasReplayQueue ? uav(bindings.replayQueue) : 0xFFFFFFFFu;
    constants[CLOD_REYES_BUILD_RASTER_WORK_REPLAY_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] = bindings.hasReplayCounter ? uav(bindings.replayCounter) : 0xFFFFFFFFu;
    constants[CLOD_REYES_BUILD_RASTER_WORK_REPLAY_DICE_QUEUE_OVERFLOW_DESCRIPTOR_INDEX] = bindings.hasReplayOverflow ? uav(bindings.replayOverflow) : 0xFFFFFFFFu;
    constants[CLOD_REYES_BUILD_RASTER_WORK_ENABLE_PATCH_OCCLUSION] = (bindings.hasVisibleClusters && bindings.hasViewDepthIndices && bindings.hasReplayQueue && bindings.hasReplayCounter && bindings.hasReplayOverflow) ? 1u : 0u;
    constants[CLOD_REYES_BUILD_RASTER_WORK_PHASE_INDEX] = bindings.phase;
    constants[CLOD_REYES_BUILD_RASTER_WORK_REPLAY_DICE_QUEUE_CAPACITY] = bindings.replayCapacity;
    constants[CLOD_REYES_BUILD_RASTER_WORK_USE_AABB_OCCLUSION] = bindings.useAabbOcclusion ? 1u : 0u;
    constants[CLOD_REYES_BUILD_RASTER_WORK_TERRAIN_RVT_ENABLED] = 0u;
    br::render::DispatchComputeIndirect(recording, bindings.pipeline, constants, bindings.commandSignature,
        bindings.indirectArgs);
}
