#include "Render/GraphExtensions/ClusterLOD/ReyesClassifyPass.h"
#include "Render/InvocationRevision.h"

#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "BuiltinResources.h"
#include "ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodReyesRootConstants.h"
#include "Resources/Buffers/Buffer.h"
#include "RenderPasses/PreparedComputeDispatch.h"

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
    rhi::CommandSignaturePtr commandSignature;
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(dispatchArgs, 1), sizeof(CLodReyesDispatchIndirectCommand) },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        commandSignature);
    m_commandSignature = std::make_shared<rhi::CommandSignaturePtr>(std::move(commandSignature));
}

ReyesClassifyBindings ReyesClassifyPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    builder.WithShaderResource(
            Builtin::PerMeshBuffer,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerMaterialDataBuffer,
            Builtin::CLod::Offsets,
            Builtin::CLod::MeshMetadata,
            Builtin::CLod::AssemblyTransforms,
            Builtin::CLod::AssemblyBoneRemaps,
            Builtin::CLod::AssemblyBoneRemapIndices,
            Builtin::Material::TextureStreamingMetadataBuffer,
            Builtin::PerObjectBuffer,
            Builtin::CullingCameraBuffer,
            Builtin::SkeletonResources::InverseBindMatrices,
            Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo)
        .WithConstantBuffer(Builtin::PerFrameBuffer)
        .WithUnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer);
    ReyesClassifyBindings bindings{builder.BindShaderResource(m_visibleClustersBuffer),
        builder.BindShaderResource(m_visibleClustersCounterBuffer)};
    bindings.fullClusters = builder.BindUnorderedAccess(m_fullClusterOutputsBuffer);
    bindings.fullCounter = builder.BindUnorderedAccess(m_fullClusterCounterBuffer);
    bindings.ownedClusters = builder.BindUnorderedAccess(m_ownedClustersBuffer);
    bindings.ownedCounter = builder.BindUnorderedAccess(m_ownedClustersCounterBuffer);
    bindings.indirectArgs = builder.BindIndirectArguments(m_indirectArgsBuffer);
    bindings.telemetry = builder.BindUnorderedAccess(m_telemetryBuffer);
    if (m_ownershipBitsetBuffer) {
        bindings.ownershipBitset = builder.BindUnorderedAccess(m_ownershipBitsetBuffer);
        bindings.hasOwnershipBitset = true;
    }
    if (m_visibleClustersReadBaseCounterBuffer) {
        bindings.readBaseCounter = builder.BindShaderResource(m_visibleClustersReadBaseCounterBuffer);
        bindings.hasReadBaseCounter = true;
    }
    bindings.fullCapacity = m_fullClusterOutputCapacity;
    bindings.ownedCapacity = m_ownedClusterCapacity;
    bindings.phase = m_phaseIndex;
    bindings.mode = static_cast<uint32_t>(m_classifyMode);
    return bindings;
}

br::render::PreparedComputeIndirect ReyesClassifyPass::Prepare(
    const ReyesClassifyBindings& bindings, const org::PassPrepareContext& preparation) const {
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
    data.constants[CLOD_REYES_CLASSIFY_VISIBLE_CLUSTERS_READ_BASE_COUNTER_DESCRIPTOR_INDEX] = bindings.hasReadBaseCounter ? srv(bindings.readBaseCounter) : 0xFFFFFFFFu;
    data.constants[CLOD_REYES_CLASSIFY_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = srv(bindings.visible);
    data.constants[CLOD_REYES_CLASSIFY_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.visibleCounter);
    data.constants[CLOD_REYES_CLASSIFY_FULL_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = uav(bindings.fullClusters);
    data.constants[CLOD_REYES_CLASSIFY_FULL_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = uav(bindings.fullCounter);
    data.constants[CLOD_REYES_CLASSIFY_FULL_CLUSTERS_CAPACITY] = bindings.fullCapacity;
    data.constants[CLOD_REYES_CLASSIFY_OWNED_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = uav(bindings.ownedClusters);
    data.constants[CLOD_REYES_CLASSIFY_OWNED_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = uav(bindings.ownedCounter);
    data.constants[CLOD_REYES_CLASSIFY_OWNED_CLUSTERS_CAPACITY] = bindings.ownedCapacity;
    data.constants[CLOD_REYES_CLASSIFY_TELEMETRY_DESCRIPTOR_INDEX] = uav(bindings.telemetry);
    data.constants[CLOD_REYES_CLASSIFY_PHASE_INDEX] = bindings.phase;
    data.constants[CLOD_REYES_CLASSIFY_OWNERSHIP_BITSET_DESCRIPTOR_INDEX] = bindings.hasOwnershipBitset ? uav(bindings.ownershipBitset) : 0xFFFFFFFFu;
    data.constants[CLOD_REYES_CLASSIFY_MODE] = bindings.mode;
    return data;
}

void ReyesClassifyPass::InvocationRevision(const org::PassPrepareContext& preparation, std::vector<uint64_t>& out) const {
    br::render::AppendFrameHeapRevision(preparation, out);
    out.push_back(br::render::PipelineRevision(m_pso));
    out.push_back(br::render::OwnerRevision(m_commandSignature));
}

void ReyesClassifyPass::Record(const ReyesClassifyBindings&,
    const br::render::PreparedComputeIndirect& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeIndirect(data, recording);
}
