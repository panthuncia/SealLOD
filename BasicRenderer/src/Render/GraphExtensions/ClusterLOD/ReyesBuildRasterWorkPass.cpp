#include "Render/GraphExtensions/ClusterLOD/ReyesBuildRasterWorkPass.h"

#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "BuiltinResources.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodReyesBuildRasterWorkRootConstants.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"

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
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(dispatchArgs, 1), sizeof(CLodReyesDispatchIndirectCommand) },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        m_commandSignature);
}

void ReyesBuildRasterWorkPass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    builder->WithShaderResource(
            m_diceQueueBuffer,
            m_diceQueueCounterBuffer,
            m_tessTableConfigsBuffer,
            Builtin::PerMeshBuffer,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerObjectBuffer,
            Builtin::CullingCameraBuffer,
            Builtin::CameraBuffer,
            Builtin::PerMaterialDataBuffer,
            Builtin::SkeletonResources::InverseBindMatrices,
            Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo)
        .WithIndirectArguments(m_indirectArgsBuffer)
        .WithUnorderedAccess(
            m_rasterWorkBuffer,
            m_rasterWorkCounterBuffer,
            m_telemetryBuffer)
        .WithConstantBuffer(Builtin::PerFrameBuffer);
    if (m_diceQueueReadOffsetBuffer) {
        builder->WithShaderResource(m_diceQueueReadOffsetBuffer);
    }
    if (m_visibleClustersBuffer) {
        builder->WithShaderResource(m_visibleClustersBuffer);
    }
    if (m_viewDepthSrvIndicesBuffer) {
        builder->WithShaderResource(m_viewDepthSrvIndicesBuffer);
    }
    if (m_replayDiceQueueBuffer) {
        builder->WithUnorderedAccess(m_replayDiceQueueBuffer);
    }
    if (m_replayDiceQueueCounterBuffer) {
        builder->WithUnorderedAccess(m_replayDiceQueueCounterBuffer);
    }
    if (m_replayDiceQueueOverflowBuffer) {
        builder->WithUnorderedAccess(m_replayDiceQueueOverflowBuffer);
    }
    if (m_slabResourceGroup) {
        builder->WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
    }
}

void ReyesBuildRasterWorkPass::Setup() {}

void ReyesBuildRasterWorkPass::Update(const UpdateExecutionContext& executionContext)
{
    (void)executionContext;
    const uint32_t zero = 0u;
    BUFFER_UPLOAD(&zero, sizeof(uint32_t), rg::runtime::UploadTarget::FromShared(m_rasterWorkCounterBuffer), 0);
}

PassReturn ReyesBuildRasterWorkPass::Execute(PassExecutionContext& executionContext)
{
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());

    uint32_t uintRootConstants[NumMiscUintRootConstants] = {};
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_DICE_QUEUE_DESCRIPTOR_INDEX] = m_diceQueueBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] = m_diceQueueCounterBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_DICE_QUEUE_READ_OFFSET_DESCRIPTOR_INDEX] = m_diceQueueReadOffsetBuffer
        ? m_diceQueueReadOffsetBuffer->GetSRVInfo(0).slot.index
        : 0xFFFFFFFFu;
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = m_tessTableConfigsBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_OUTPUT_DESCRIPTOR_INDEX] = m_rasterWorkBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_OUTPUT_COUNTER_DESCRIPTOR_INDEX] = m_rasterWorkCounterBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_TELEMETRY_DESCRIPTOR_INDEX] = m_telemetryBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_CAPACITY] = m_rasterWorkCapacity;
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = m_visibleClustersBuffer
        ? m_visibleClustersBuffer->GetSRVInfo(0).slot.index
        : 0xFFFFFFFFu;
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_VIEW_DEPTH_SRV_INDICES_DESCRIPTOR_INDEX] = m_viewDepthSrvIndicesBuffer
        ? m_viewDepthSrvIndicesBuffer->GetSRVInfo(0).slot.index
        : 0xFFFFFFFFu;
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_REPLAY_DICE_QUEUE_DESCRIPTOR_INDEX] = m_replayDiceQueueBuffer
        ? m_replayDiceQueueBuffer->GetUAVShaderVisibleInfo(0).slot.index
        : 0xFFFFFFFFu;
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_REPLAY_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] = m_replayDiceQueueCounterBuffer
        ? m_replayDiceQueueCounterBuffer->GetUAVShaderVisibleInfo(0).slot.index
        : 0xFFFFFFFFu;
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_REPLAY_DICE_QUEUE_OVERFLOW_DESCRIPTOR_INDEX] = m_replayDiceQueueOverflowBuffer
        ? m_replayDiceQueueOverflowBuffer->GetUAVShaderVisibleInfo(0).slot.index
        : 0xFFFFFFFFu;
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_ENABLE_PATCH_OCCLUSION] =
        (m_visibleClustersBuffer && m_viewDepthSrvIndicesBuffer && m_replayDiceQueueBuffer && m_replayDiceQueueCounterBuffer && m_replayDiceQueueOverflowBuffer)
            ? 1u
            : 0u;
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_PHASE_INDEX] = m_phaseIndex;
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_REPLAY_DICE_QUEUE_CAPACITY] = m_replayDiceQueueCapacity;
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_USE_AABB_OCCLUSION] =
        SettingsManager::GetInstance().getSettingGetter<bool>(CLodReyesUseAabbOcclusionSettingName)() ? 1u : 0u;
    uintRootConstants[CLOD_REYES_BUILD_RASTER_WORK_TERRAIN_RVT_ENABLED] = 0u;

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

void ReyesBuildRasterWorkPass::Cleanup() {}
