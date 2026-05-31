#include "Render/GraphExtensions/ClusterLOD/ReyesClassifyPass.h"

#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "BuiltinResources.h"
#include "ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodReyesRootConstants.h"
#include "Resources/Buffers/Buffer.h"

ReyesClassifyPass::ReyesClassifyPass(
    std::shared_ptr<Buffer> visibleClustersBuffer,
    std::shared_ptr<Buffer> visibleClustersCounterBuffer,
    std::shared_ptr<Buffer> visibleClustersReadBaseCounterBuffer,
    std::shared_ptr<Buffer> fullClusterOutputsBuffer,
    std::shared_ptr<Buffer> fullClusterCounterBuffer,
    uint32_t fullClusterOutputCapacity,
    std::shared_ptr<Buffer> ownedClustersBuffer,
    std::shared_ptr<Buffer> ownedClustersCounterBuffer,
    uint32_t ownedClusterCapacity,
    std::shared_ptr<Buffer> ownershipBitsetBuffer,
    std::shared_ptr<Buffer> indirectArgsBuffer,
    std::shared_ptr<Buffer> telemetryBuffer,
    uint32_t phaseIndex,
    ReyesClassifyMode classifyMode)
    : m_visibleClustersBuffer(std::move(visibleClustersBuffer))
    , m_visibleClustersCounterBuffer(std::move(visibleClustersCounterBuffer))
    , m_visibleClustersReadBaseCounterBuffer(std::move(visibleClustersReadBaseCounterBuffer))
    , m_fullClusterOutputsBuffer(std::move(fullClusterOutputsBuffer))
    , m_fullClusterCounterBuffer(std::move(fullClusterCounterBuffer))
    , m_fullClusterOutputCapacity(fullClusterOutputCapacity)
    , m_ownedClustersBuffer(std::move(ownedClustersBuffer))
    , m_ownedClustersCounterBuffer(std::move(ownedClustersCounterBuffer))
    , m_ownedClusterCapacity(ownedClusterCapacity)
    , m_ownershipBitsetBuffer(std::move(ownershipBitsetBuffer))
    , m_indirectArgsBuffer(std::move(indirectArgsBuffer))
    , m_telemetryBuffer(std::move(telemetryBuffer))
    , m_phaseIndex(phaseIndex)
    , m_classifyMode(classifyMode) {
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/reyesClassify.hlsl",
        L"ReyesClassifyCS",
        {},
        "CLod.ReyesClassify.PSO");

    rhi::IndirectArg dispatchArgs[] = {
        {.kind = rhi::IndirectArgKind::Dispatch }
    };

    auto device = DeviceManager::GetInstance().GetDevice();
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(dispatchArgs, 1), sizeof(CLodReyesDispatchIndirectCommand) },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        m_commandSignature);
}

void ReyesClassifyPass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    builder->WithShaderResource(m_visibleClustersBuffer, m_visibleClustersCounterBuffer)
        .WithShaderResource(
            Builtin::PerMeshBuffer,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerMaterialDataBuffer,
            Builtin::Material::TextureGroup,
            Builtin::Material::TextureStreamingMetadataBuffer,
            Builtin::PerObjectBuffer,
            Builtin::CullingCameraBuffer)
        .WithIndirectArguments(m_indirectArgsBuffer)
        .WithUnorderedAccess(
            Builtin::Material::TextureStreamingFeedbackBuffer,
            m_fullClusterOutputsBuffer,
            m_fullClusterCounterBuffer,
            m_ownedClustersBuffer,
            m_ownedClustersCounterBuffer,
            m_telemetryBuffer);
    if (m_ownershipBitsetBuffer) {
        builder->WithUnorderedAccess(m_ownershipBitsetBuffer);
    }
    if (m_visibleClustersReadBaseCounterBuffer) {
        builder->WithShaderResource(m_visibleClustersReadBaseCounterBuffer);
    }
}

void ReyesClassifyPass::Setup() {
}

PassReturn ReyesClassifyPass::Execute(PassExecutionContext& executionContext)
{
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());

    uint32_t uintRootConstants[NumMiscUintRootConstants] = {};
    uintRootConstants[CLOD_REYES_CLASSIFY_VISIBLE_CLUSTERS_READ_BASE_COUNTER_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    uintRootConstants[CLOD_REYES_CLASSIFY_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = m_visibleClustersBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_CLASSIFY_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = m_visibleClustersCounterBuffer->GetSRVInfo(0).slot.index;
    if (m_visibleClustersReadBaseCounterBuffer) {
        uintRootConstants[CLOD_REYES_CLASSIFY_VISIBLE_CLUSTERS_READ_BASE_COUNTER_DESCRIPTOR_INDEX] = m_visibleClustersReadBaseCounterBuffer->GetSRVInfo(0).slot.index;
    }
    uintRootConstants[CLOD_REYES_CLASSIFY_FULL_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = m_fullClusterOutputsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_CLASSIFY_FULL_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = m_fullClusterCounterBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_CLASSIFY_FULL_CLUSTERS_CAPACITY] = m_fullClusterOutputCapacity;
    uintRootConstants[CLOD_REYES_CLASSIFY_OWNED_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = m_ownedClustersBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_CLASSIFY_OWNED_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = m_ownedClustersCounterBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_CLASSIFY_OWNED_CLUSTERS_CAPACITY] = m_ownedClusterCapacity;
    uintRootConstants[CLOD_REYES_CLASSIFY_TELEMETRY_DESCRIPTOR_INDEX] = m_telemetryBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_CLASSIFY_PHASE_INDEX] = m_phaseIndex;
    uintRootConstants[CLOD_REYES_CLASSIFY_OWNERSHIP_BITSET_DESCRIPTOR_INDEX] = m_ownershipBitsetBuffer
        ? m_ownershipBitsetBuffer->GetUAVShaderVisibleInfo(0).slot.index
        : 0xFFFFFFFFu;
    uintRootConstants[CLOD_REYES_CLASSIFY_MODE] = static_cast<uint32_t>(m_classifyMode);

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

void ReyesClassifyPass::Update(const UpdateExecutionContext& executionContext)
{
}

void ReyesClassifyPass::Cleanup() {}
