#include "VirtualGeometry/Reyes/RenderPasses/ReyesClassifyPass.h"
#include "Render/PassBuilders.h"

#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BuiltinResources.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodReyesRootConstants.h"
#include "Resources/Buffers/Buffer.h"

ReyesClassifyPass::ReyesClassifyPass(
    std::shared_ptr<org::Buffer> visibleClustersBuffer,
    std::shared_ptr<org::Buffer> visibleClustersCounterBuffer,
    std::shared_ptr<org::Buffer> visibleClustersReadBaseCounterBuffer,
    std::shared_ptr<org::Buffer> fullClusterOutputsBuffer,
    std::shared_ptr<org::Buffer> fullClusterCounterBuffer,
    uint32_t fullClusterOutputCapacity,
    std::shared_ptr<org::Buffer> ownedClustersBuffer,
    std::shared_ptr<org::Buffer> ownedClustersCounterBuffer,
    uint32_t ownedClusterCapacity,
    std::shared_ptr<org::Buffer> ownershipBitsetBuffer,
    std::shared_ptr<org::Buffer> indirectArgsBuffer,
    std::shared_ptr<org::Buffer> telemetryBuffer,
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
    builder.ShaderResource(
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
            Builtin::SkeletonResources::SkinningInstanceInfo);
    builder.ConstantBuffer(Builtin::PerFrameBuffer);
    builder.UnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer);
    ReyesClassifyBindings bindings{m_pso, m_commandSignature, builder.ShaderResource(m_visibleClustersBuffer).View(),
        builder.ShaderResource(m_visibleClustersCounterBuffer).View()};
    bindings.fullClusters = builder.UnorderedAccess(m_fullClusterOutputsBuffer).View();
    bindings.fullCounter = builder.UnorderedAccess(m_fullClusterCounterBuffer).View();
    bindings.ownedClusters = builder.UnorderedAccess(m_ownedClustersBuffer).View();
    bindings.ownedCounter = builder.UnorderedAccess(m_ownedClustersCounterBuffer).View();
    bindings.indirectArgs = builder.IndirectArguments(m_indirectArgsBuffer);
    bindings.telemetry = builder.UnorderedAccess(m_telemetryBuffer).View();
    if (m_ownershipBitsetBuffer) {
        bindings.ownershipBitset = builder.UnorderedAccess(m_ownershipBitsetBuffer).View();
        bindings.hasOwnershipBitset = true;
    }
    if (m_visibleClustersReadBaseCounterBuffer) {
        bindings.readBaseCounter = builder.ShaderResource(m_visibleClustersReadBaseCounterBuffer).View();
        bindings.hasReadBaseCounter = true;
    }
    bindings.fullCapacity = m_fullClusterOutputCapacity;
    bindings.ownedCapacity = m_ownedClusterCapacity;
    bindings.phase = m_phaseIndex;
    bindings.mode = static_cast<uint32_t>(m_classifyMode);
    return bindings;
}

void ReyesClassifyPass::Record(const ReyesClassifyBindings& bindings, org::PassRecordContext& recording) {
    br::render::MiscConstants constants{};
    const auto srv = [&](const org::DeclaredViewToken& token) { return recording.Index(token); };
    const auto uav = [&](const org::DeclaredViewToken& token) { return recording.Index(token); };
    constants[CLOD_REYES_CLASSIFY_VISIBLE_CLUSTERS_READ_BASE_COUNTER_DESCRIPTOR_INDEX] = bindings.hasReadBaseCounter ? srv(bindings.readBaseCounter) : 0xFFFFFFFFu;
    constants[CLOD_REYES_CLASSIFY_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = srv(bindings.visible);
    constants[CLOD_REYES_CLASSIFY_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.visibleCounter);
    constants[CLOD_REYES_CLASSIFY_FULL_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = uav(bindings.fullClusters);
    constants[CLOD_REYES_CLASSIFY_FULL_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = uav(bindings.fullCounter);
    constants[CLOD_REYES_CLASSIFY_FULL_CLUSTERS_CAPACITY] = bindings.fullCapacity;
    constants[CLOD_REYES_CLASSIFY_OWNED_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = uav(bindings.ownedClusters);
    constants[CLOD_REYES_CLASSIFY_OWNED_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = uav(bindings.ownedCounter);
    constants[CLOD_REYES_CLASSIFY_OWNED_CLUSTERS_CAPACITY] = bindings.ownedCapacity;
    constants[CLOD_REYES_CLASSIFY_TELEMETRY_DESCRIPTOR_INDEX] = uav(bindings.telemetry);
    constants[CLOD_REYES_CLASSIFY_PHASE_INDEX] = bindings.phase;
    constants[CLOD_REYES_CLASSIFY_OWNERSHIP_BITSET_DESCRIPTOR_INDEX] = bindings.hasOwnershipBitset ? uav(bindings.ownershipBitset) : 0xFFFFFFFFu;
    constants[CLOD_REYES_CLASSIFY_MODE] = bindings.mode;
    br::render::DispatchComputeIndirect(recording, bindings.pipeline, constants, bindings.commandSignature,
        bindings.indirectArgs);
}
