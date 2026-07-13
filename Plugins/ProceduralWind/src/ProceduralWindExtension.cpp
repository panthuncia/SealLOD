#include "ProceduralWind/ProceduralWindExtension.h"

#include "Animation/Skeleton.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Managers/ObjectManager.h"
#include "Managers/SkeletonManager.h"
#include "Managers/ViewManager.h"
#include "Render/BuiltinResources.h"
#include "Render/IndirectCommand.h"
#include "Render/PassBuilders.h"
#include "Render/RenderContext.h"
#include "Render/OutputTypes.h"
#include "RenderPasses/Base/RenderPass.h"
#include "Resources/Buffers/DynamicStructuredBuffer.h"
#include "ShaderBuffers.h"

#include <tracy/Tracy.hpp>

#include <algorithm>
#include <array>
#include <bit>

namespace br::wind {
namespace {

constexpr std::uint32_t kThreadsPerGroup = 64u;
constexpr std::uint32_t kMaxInstancesPerType = 4096u;
constexpr std::uint32_t kTransientBoneCapacity = 262144u;

struct WindBoneGPU {
    std::uint32_t skinningSlot = 0u;
    std::uint32_t jointIndex = 0u;
    std::uint32_t parentEntry = kInvalidSimulationGroup;
    std::uint32_t simulationGroup = kInvalidSimulationGroup;
    std::uint32_t phaseSeed = 0u;
    float influence = 0.0f;
    float meanBend = 0.0f;
    float parallelAmplitude = 0.0f;
    float perpendicularRatio = 0.0f;
    float torsionRatio = 0.0f;
    float frequencyScale = 1.0f;
    float maximumAngle = 0.0f;
    std::array<float, 3> frequencies{};
    float pad0 = 0.0f;
    std::array<float, 3> weights{};
    float pad1 = 0.0f;
};

struct WindRootConstants {
    std::uint32_t boneEntries = 0u;
    std::uint32_t scratchForward = 0u;
    std::uint32_t scratchInverse = 0u;
    std::uint32_t skinningInfo = 0u;
    std::uint32_t forwardSkin = 0u;
    std::uint32_t inverseSkin = 0u;
    std::uint32_t inverseBind = 0u;
    std::uint32_t boneCount = 0u;
    float elapsedSeconds = 0.0f;
    float windX = 1.0f;
    float windY = 0.0f;
    float strength = 0.0f;
    float gustStrength = 0.0f;
    std::uint32_t fieldSlice0 = 0u;
    std::uint32_t fieldSlice1 = 0u;
    std::uint32_t fieldDimensions = 0u;
    float fieldCellSize = 0.0f;
    float fieldOriginX = 0.0f;
    float fieldOriginY = 0.0f;
    float fieldInterpolation = 0.0f;
    std::uint32_t fieldValid = 0u;
};

struct WindTypeGPU {
    std::uint32_t firstBone = 0u;
    std::uint32_t boneCount = 0u;
    std::uint32_t sourceSkinningSlot = 0u;
    std::uint32_t bucketBase = 0u;
    std::uint32_t bucketCapacity = kMaxInstancesPerType;
    std::uint32_t pad[3]{};
};

struct WindActiveInstanceGPU {
    std::uint32_t drawRecordIndex = 0u;
    std::uint32_t stableSceneId = 0u;
    std::uint32_t transformOffsetMatrices = 0u;
    std::uint32_t inverseSkinOffsetMatrices = 0u;
};

struct WindIndirectCommand {
    std::uint32_t typeId = 0u;
    std::uint32_t pad0 = 0u;
    std::uint32_t pad1 = 0u;
    D3D12_DISPATCH_ARGUMENTS dispatch{};
};

struct WindTransientConstants {
    std::uint32_t types, bones, activeInstances, typeCounters;
    std::uint32_t counters, visibilityGenerations, indirectCommands, skinningInfo;
    std::uint32_t forwardSkin, inverseSkin, inverseBind, drawCount;
    std::uint32_t typeCount, transformBase, inverseBase, matrixCapacity;
    std::uint32_t cameraIndex, instanceBucketCapacity, fieldSlice0, fieldSlice1;
    std::uint32_t fieldDimensions;
    float fieldCellSize, fieldOriginX, fieldOriginY;
    float fieldInterpolation, elapsedSeconds, windX, windY;
    float strength, gustStrength;
};

static_assert(sizeof(WindBoneGPU) == 80u);
static_assert(sizeof(WindRootConstants) % sizeof(std::uint32_t) == 0u);

struct WindSharedResources {
    explicit WindSharedResources(std::shared_ptr<ProceduralWindRuntime> runtimeIn)
        : runtime(std::move(runtimeIn))
        , fieldSlices{ DynamicStructuredBuffer<std::uint32_t>::CreateShared(1u, "ProceduralWind.FieldSlice0"),
                       DynamicStructuredBuffer<std::uint32_t>::CreateShared(1u, "ProceduralWind.FieldSlice1") }
        , boneEntries(DynamicStructuredBuffer<WindBoneGPU>::CreateShared(1u, "ProceduralWind.BoneEntries"))
        , windTypes(DynamicStructuredBuffer<WindTypeGPU>::CreateShared(1u, "ProceduralWind.Types"))
        , activeInstances(DynamicStructuredBuffer<WindActiveInstanceGPU>::CreateShared(1u, "ProceduralWind.ActiveInstances", true))
        , typeCounters(DynamicStructuredBuffer<std::uint32_t>::CreateShared(1u, "ProceduralWind.TypeCounters", true))
        , allocationCounters(DynamicStructuredBuffer<std::uint32_t>::CreateShared(8u, "ProceduralWind.AllocationCounters", true))
        , visibilityGenerations(DynamicStructuredBuffer<std::uint32_t>::CreateShared(1u, "ProceduralWind.VisibilityGenerations"))
        , indirectCommands(DynamicStructuredBuffer<WindIndirectCommand>::CreateShared(1u, "ProceduralWind.IndirectCommands", true))
    {}

    void UpdateFieldPair()
    {
        const auto pair = runtime->SnapshotResidentPair();
        if (!pair.valid || (pair.revision == fieldRevision && fieldReady)) return;
        bool ready = true;
        for (std::size_t slice = 0; slice < 2u; ++slice) {
            std::vector<std::uint32_t> packed((pair.rgba16f[slice].size() + 1u) / 2u, 0u);
            for (std::size_t i = 0; i < pair.rgba16f[slice].size(); ++i)
                packed[i / 2u] |= static_cast<std::uint32_t>(pair.rgba16f[slice][i]) << ((i & 1u) * 16u);
            fieldSlices[slice]->ReplaceData(std::move(packed));
            ready = ready && fieldSlices[slice]->ResidentCapacity() >= (pair.rgba16f[slice].size() + 1u) / 2u;
        }
        fieldRevision = pair.revision;
        fieldReady = ready;
        residentPair = ready ? pair : ResidentWindPair{};
        TracyPlot("ProceduralWind.ResidentDirectionLower", static_cast<std::int64_t>(pair.bracket.lower));
        TracyPlot("ProceduralWind.ResidentDirectionUpper", static_cast<std::int64_t>(pair.bracket.upper));
    }

    void UpdateTypes(const UpdateExecutionContext& context)
    {
        const auto* update = context.hostData ? context.hostData->Get<UpdateContext>() : nullptr;
        if (!update || !update->skeletonManager) {
            activeBoneCount = 0u;
            return;
        }
        const auto drawCount = update->objectManager ? static_cast<std::uint32_t>(update->objectManager->GetResidentInstanceDrawRecordCount()) : 0u;
        std::vector<WindBoneGPU> next;
        std::vector<WindTypeGPU> types;
        std::uint32_t registeredTypes = 0u;
        for (const auto& instance : update->skeletonManager->GetActiveInstanceViews()) {
            if (!instance.skeleton || !instance.skeleton->HasWindSimulationGroups()) continue;
            if (types.size() <= instance.instanceSlot) types.resize(instance.instanceSlot + 1u);
            const auto groups = instance.skeleton->GetWindSimulationGroupIndices();
            const auto parents = instance.skeleton->GetParentIndices();
            const auto& authoredWind = instance.skeleton->GetDynamicWindMetadata();
            const auto profile = runtime->ResolveProfile(instance.skeleton->GetWindProfileIdentity());
            const auto firstEntry = static_cast<std::uint32_t>(next.size());
            auto& type = types[instance.instanceSlot];
            ++registeredTypes;
            type.firstBone = firstEntry;
            type.boneCount = instance.boneCount;
            type.sourceSkinningSlot = instance.instanceSlot;
            type.bucketBase = instance.instanceSlot * kMaxInstancesPerType;
            for (std::uint32_t joint = 0u; joint < instance.boneCount; ++joint) {
                WindBoneGPU entry{};
                entry.skinningSlot = instance.instanceSlot;
                entry.jointIndex = joint;
                entry.phaseSeed = instance.instanceSlot * 747796405u + joint * 2891336453u + 277803737u;
                entry.simulationGroup = joint < groups.size() ? groups[joint] : kInvalidSimulationGroup;
                if (joint < parents.size() && parents[joint] >= 0)
                    entry.parentEntry = firstEntry + static_cast<std::uint32_t>(parents[joint]);
                const auto found = std::ranges::find(profile.groups, entry.simulationGroup, &WindSimulationGroupProfile::id);
                if (found != profile.groups.end()) {
                    entry.influence = found->influence;
                    entry.meanBend = found->meanBendRadians;
                    entry.parallelAmplitude = found->parallelAmplitudeRadians;
                    entry.perpendicularRatio = found->perpendicularRatio;
                    entry.torsionRatio = found->torsionRatio;
                    entry.frequencyScale = found->frequencyScale;
                    entry.maximumAngle = found->maximumAngleRadians;
                }
                if (authoredWind.enabled && entry.simulationGroup < authoredWind.groups.size()) {
                    const auto& authoredGroup = authoredWind.groups[entry.simulationGroup];
                    entry.influence = authoredGroup.influence;
                    if ((authoredGroup.flags & DynamicWindMetadata::GroupFlagDualInfluence) != 0u &&
                        joint < authoredWind.bones.size()) {
                        const auto& bone = authoredWind.bones[joint];
                        const float denominator = static_cast<float>(bone.chainBoneCount > 1u ? bone.chainBoneCount - 1u : 1u);
                        const float chainT = std::clamp(
                            static_cast<float>(bone.indexInBoneChain) / denominator + authoredGroup.shiftTop,
                            0.0f,
                            1.0f);
                        entry.influence = std::lerp(authoredGroup.minInfluence, authoredGroup.maxInfluence, chainT);
                    }
                }
                entry.frequencies = profile.harmonicFrequenciesHz;
                entry.weights = profile.harmonicWeights;
                next.push_back(entry);
            }
        }
        const auto requestedBoneCount = static_cast<std::uint32_t>(next.size());
        boneEntries->ReplaceData(std::move(next));
        windTypes->ReplaceData(std::move(types));
        typeCount = windTypes->Size();
        if (registeredTypes != 0u) {
            transientRegion = update->skeletonManager->ReserveTransientWindRegion(kTransientBoneCapacity);
            update->skeletonManager->EnsureTransientWindInstanceSlots(drawCount);
            if (update->objectManager) {
                const auto generations = update->objectManager->GetDrawRecordVisibilityGenerations();
                visibilityGenerations->ReplaceData(std::vector<std::uint32_t>(generations.begin(), generations.end()));
            }
            residentDrawCount = std::min(drawCount, visibilityGenerations->ResidentCapacity());
        }
        else {
            residentDrawCount = 0u;
        }
        typeCounters->EnsureSize(std::max(1u, typeCount));
        indirectCommands->EnsureSize(std::max(1u, typeCount));
        activeInstances->EnsureSize(std::max(1u, typeCount * kMaxInstancesPerType));
        activeBoneCount = std::min(requestedBoneCount, boneEntries->ResidentCapacity());
        TracyPlot("ProceduralWind.CandidateDrawRecords", static_cast<std::int64_t>(residentDrawCount));
        TracyPlot("ProceduralWind.RegisteredTypes", static_cast<std::int64_t>(registeredTypes));
        if (registeredTypes != lastLoggedRegisteredTypes || residentDrawCount != lastLoggedDrawCount) {
            spdlog::info(
                "ProceduralWind transient: registeredTypes={} typeSlots={} typeBones={} candidateDrawRecords={} matrixCapacity={} bucketCapacityPerType={} distance=32768",
                registeredTypes, typeCount, activeBoneCount, residentDrawCount,
                transientRegion.capacityMatrices, kMaxInstancesPerType);
            lastLoggedRegisteredTypes = registeredTypes;
            lastLoggedDrawCount = residentDrawCount;
        }
        elapsedSeconds += context.deltaTime;
        state = runtime->SnapshotWindState();
        TracyPlot("ProceduralWind.SimulatedBones", static_cast<std::int64_t>(activeBoneCount));
    }

    std::shared_ptr<ProceduralWindRuntime> runtime;
    std::array<std::shared_ptr<DynamicStructuredBuffer<std::uint32_t>>, 2> fieldSlices;
    std::shared_ptr<DynamicStructuredBuffer<WindBoneGPU>> boneEntries;
    std::shared_ptr<DynamicStructuredBuffer<WindTypeGPU>> windTypes;
    std::shared_ptr<DynamicStructuredBuffer<WindActiveInstanceGPU>> activeInstances;
    std::shared_ptr<DynamicStructuredBuffer<std::uint32_t>> typeCounters;
    std::shared_ptr<DynamicStructuredBuffer<std::uint32_t>> allocationCounters;
    std::shared_ptr<DynamicStructuredBuffer<std::uint32_t>> visibilityGenerations;
    std::shared_ptr<DynamicStructuredBuffer<WindIndirectCommand>> indirectCommands;
    std::uint64_t fieldRevision = 0u;
    bool fieldReady = false;
    std::uint32_t activeBoneCount = 0u;
    std::uint32_t typeCount = 0u;
    std::uint32_t residentDrawCount = 0u;
    std::uint32_t lastLoggedRegisteredTypes = ~0u;
    std::uint32_t lastLoggedDrawCount = ~0u;
    float elapsedSeconds = 0.0f;
    WindState state{};
    ResidentWindPair residentPair{};
    SkeletonManager::TransientWindRegion transientRegion{};
};

void BindAndDispatch(PassExecutionContext& executionContext, const PipelineState& pso, const WindRootConstants& constants)
{
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& commandList = executionContext.commandList;
    commandList.SetDescriptorHeaps(renderContext->textureDescriptorHeap.GetHandle(), renderContext->samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    commandList.BindPipeline(pso.GetAPIPipelineState().GetHandle());
    commandList.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0,
        sizeof(constants) / sizeof(std::uint32_t), reinterpret_cast<const std::uint32_t*>(&constants));
    commandList.Dispatch((constants.boneCount + kThreadsPerGroup - 1u) / kThreadsPerGroup, 1u, 1u);
}

class WindResidencyPass final : public ComputePass {
public:
    explicit WindResidencyPass(std::shared_ptr<WindSharedResources> resources) : m_resources(std::move(resources)) {}
    void DeclareResourceUsages(ComputePassBuilder* builder) override { builder->WithShaderResource(m_resources->fieldSlices[0], m_resources->fieldSlices[1]); }
    void Setup() override {}
    void Update(const UpdateExecutionContext&) override { m_resources->UpdateFieldPair(); }
    PassReturn Execute(PassExecutionContext&) override { return {}; }
    void Cleanup() override {}
private:
    std::shared_ptr<WindSharedResources> m_resources;
};

void BindTransient(PassExecutionContext& context, const PipelineState& pso, const WindTransientConstants& constants,
    std::uint32_t groupsX, std::uint32_t groupsY = 1u)
{
    auto* renderContext = context.hostData->Get<RenderContext>();
    auto& commandList = context.commandList;
    commandList.SetDescriptorHeaps(renderContext->textureDescriptorHeap.GetHandle(), renderContext->samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    commandList.BindPipeline(pso.GetAPIPipelineState().GetHandle());
    commandList.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0,
        sizeof(constants) / sizeof(std::uint32_t), reinterpret_cast<const std::uint32_t*>(&constants));
    commandList.Dispatch(groupsX, groupsY, 1u);
}

WindTransientConstants MakeTransientConstants(WindSharedResources& resources, GloballyIndexedResource* skinInfo,
    GloballyIndexedResource* forward, GloballyIndexedResource* inverse, GloballyIndexedResource* inverseBind)
{
    WindTransientConstants c{};
    c.types = resources.windTypes->GetSRVInfo(0).slot.index;
    c.bones = resources.boneEntries->GetSRVInfo(0).slot.index;
    c.activeInstances = resources.activeInstances->GetUAVShaderVisibleInfo(0).slot.index;
    c.typeCounters = resources.typeCounters->GetUAVShaderVisibleInfo(0).slot.index;
    c.counters = resources.allocationCounters->GetUAVShaderVisibleInfo(0).slot.index;
    c.visibilityGenerations = resources.visibilityGenerations->GetSRVInfo(0).slot.index;
    c.indirectCommands = resources.indirectCommands->GetUAVShaderVisibleInfo(0).slot.index;
    c.skinningInfo = skinInfo->GetUAVShaderVisibleInfo(0).slot.index;
    c.forwardSkin = forward->GetUAVShaderVisibleInfo(0).slot.index;
    c.inverseSkin = inverse->GetUAVShaderVisibleInfo(0).slot.index;
    c.inverseBind = inverseBind->GetSRVInfo(0).slot.index;
    c.drawCount = resources.residentDrawCount;
    c.typeCount = resources.typeCount;
    c.transformBase = resources.transientRegion.transformBaseMatrices;
    c.inverseBase = resources.transientRegion.inverseSkinBaseMatrices;
    c.matrixCapacity = resources.transientRegion.capacityMatrices;
    c.instanceBucketCapacity = kMaxInstancesPerType;
    c.fieldSlice0 = resources.fieldSlices[0]->GetSRVInfo(0).slot.index;
    c.fieldSlice1 = resources.fieldSlices[1]->GetSRVInfo(0).slot.index;
    const auto& resident = resources.residentPair;
    c.fieldDimensions = (resident.metadata.width & 0xffffu) | ((resident.metadata.height & 0xffffu) << 16u);
    c.fieldCellSize = resident.metadata.cellSize;
    c.fieldOriginX = resident.metadata.origin.x;
    c.fieldOriginY = resident.metadata.origin.y;
    c.fieldInterpolation = resident.bracket.interpolation;
    c.elapsedSeconds = resources.elapsedSeconds;
    c.windX = resources.state.directionToWS.x;
    c.windY = resources.state.directionToWS.y;
    c.strength = resources.state.strength;
    c.gustStrength = resources.state.gustStrength;
    return c;
}

class WindResetPass final : public ComputePass {
public:
    explicit WindResetPass(std::shared_ptr<WindSharedResources> resources) : m_resources(std::move(resources))
    {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"SARPShaders/ProceduralWind.hlsl", L"ResetWindTransientCS", {}, "ProceduralWind.ResetTransient");
    }
    void DeclareResourceUsages(ComputePassBuilder* builder) override
    {
        builder->WithShaderResource(m_resources->windTypes, m_resources->visibilityGenerations,
            Builtin::SkeletonResources::InverseBindMatrices)
            .WithUnorderedAccess(m_resources->typeCounters, m_resources->allocationCounters,
                m_resources->indirectCommands, m_resources->activeInstances,
                Builtin::SkeletonResources::SkinningInstanceInfo,
                Builtin::SkeletonResources::BoneTransforms, Builtin::SkeletonResources::InverseSkinMatrices);
    }
    void Setup() override {}
    void Update(const UpdateExecutionContext& context) override { m_resources->UpdateTypes(context); }
    PassReturn Execute(PassExecutionContext& context) override
    {
        if (!m_resources->transientRegion.valid) return {};
        auto c = MakeTransientConstants(*m_resources,
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::SkinningInstanceInfo),
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::BoneTransforms),
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::InverseSkinMatrices),
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::InverseBindMatrices));
        BindTransient(context, m_pso, c, (std::max(c.drawCount, c.typeCount) + 63u) / 64u);
        return {};
    }
    void Cleanup() override {}
private:
    std::shared_ptr<WindSharedResources> m_resources;
    PipelineState m_pso;
};

class WindActivatePass final : public ComputePass {
public:
    explicit WindActivatePass(std::shared_ptr<WindSharedResources> resources) : m_resources(std::move(resources))
    {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"SARPShaders/ProceduralWind.hlsl", L"ActivateWindInstancesCS", {}, "ProceduralWind.ActivateInstances");
    }
    void DeclareResourceUsages(ComputePassBuilder* builder) override
    {
        builder->WithShaderResource(m_resources->windTypes, m_resources->visibilityGenerations,
            Builtin::InstanceDrawRecordBuffer, Builtin::PerMeshInstanceBuffer,
            Builtin::PerInstanceTransformBuffer, Builtin::CameraBuffer,
            Builtin::SkeletonResources::InverseBindMatrices)
            .WithUnorderedAccess(m_resources->activeInstances, m_resources->typeCounters,
                m_resources->allocationCounters, Builtin::SkeletonResources::SkinningInstanceInfo,
                Builtin::SkeletonResources::BoneTransforms, Builtin::SkeletonResources::InverseSkinMatrices);
    }
    void Setup() override {}
    void Update(const UpdateExecutionContext&) override {}
    PassReturn Execute(PassExecutionContext& context) override
    {
        if (!m_resources->residentDrawCount) return {};
        auto c = MakeTransientConstants(*m_resources,
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::SkinningInstanceInfo),
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::BoneTransforms),
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::InverseSkinMatrices),
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::InverseBindMatrices));
        const auto* rc = context.hostData->Get<RenderContext>();
        if (rc && rc->viewManager) {
            if (const auto* view = rc->viewManager->Get(rc->primaryViewID)) c.cameraIndex = view->gpu.cameraBufferIndex;
        }
        BindTransient(context, m_pso, c, (c.drawCount + 63u) / 64u);
        return {};
    }
    void Cleanup() override {}
private:
    std::shared_ptr<WindSharedResources> m_resources;
    PipelineState m_pso;
};

class WindBuildCommandsPass final : public ComputePass {
public:
    explicit WindBuildCommandsPass(std::shared_ptr<WindSharedResources> r) : m_resources(std::move(r)) {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"SARPShaders/ProceduralWind.hlsl", L"BuildWindCommandsCS", {}, "ProceduralWind.BuildCommands");
    }
    void DeclareResourceUsages(ComputePassBuilder* b) override { b->WithShaderResource(m_resources->windTypes, m_resources->typeCounters, Builtin::SkeletonResources::InverseBindMatrices).WithUnorderedAccess(m_resources->allocationCounters, m_resources->indirectCommands, Builtin::SkeletonResources::SkinningInstanceInfo, Builtin::SkeletonResources::BoneTransforms, Builtin::SkeletonResources::InverseSkinMatrices); }
    void Setup() override {} void Update(const UpdateExecutionContext&) override {}
    PassReturn Execute(PassExecutionContext& context) override {
        if (!m_resources->typeCount) return {};
        auto c = MakeTransientConstants(*m_resources,
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::SkinningInstanceInfo),
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::BoneTransforms),
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::InverseSkinMatrices),
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::InverseBindMatrices));
        BindTransient(context, m_pso, c, (c.typeCount + 63u) / 64u); return {};
    }
    void Cleanup() override {}
private: std::shared_ptr<WindSharedResources> m_resources; PipelineState m_pso;
};

class WindIndirectSimulatePass final : public ComputePass {
public:
    explicit WindIndirectSimulatePass(std::shared_ptr<WindSharedResources> r) : m_resources(std::move(r)) {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(PSOManager::GetInstance().GetComputeRootSignature().GetHandle(), L"SARPShaders/ProceduralWind.hlsl", L"SimulateWindInstancesCS", {}, "ProceduralWind.SimulateIndirect");
        rhi::IndirectArg args[] = {{.kind=rhi::IndirectArgKind::Constant,.u={.rootConstants={IndirectCommandSignatureRootSignatureIndex,0,3}}},{.kind=rhi::IndirectArgKind::Dispatch}};
        DeviceManager::GetInstance().GetDevice().CreateCommandSignature({rhi::Span<rhi::IndirectArg>(args,2),sizeof(WindIndirectCommand)}, PSOManager::GetInstance().GetComputeRootSignature().GetHandle(), m_signature);
    }
    void DeclareResourceUsages(ComputePassBuilder* b) override { b->WithShaderResource(m_resources->windTypes,m_resources->boneEntries,m_resources->activeInstances,m_resources->fieldSlices[0],m_resources->fieldSlices[1],Builtin::InstanceDrawRecordBuffer,Builtin::PerInstanceTransformBuffer,Builtin::SkeletonResources::InverseBindMatrices).WithUnorderedAccess(Builtin::SkeletonResources::SkinningInstanceInfo,Builtin::SkeletonResources::BoneTransforms,Builtin::SkeletonResources::InverseSkinMatrices).WithIndirectArguments(m_resources->indirectCommands,m_resources->allocationCounters); }
    void Setup() override {} void Update(const UpdateExecutionContext&) override {}
    PassReturn Execute(PassExecutionContext& context) override {
        if (!m_resources->typeCount) return {};
        auto c=MakeTransientConstants(*m_resources,m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::SkinningInstanceInfo),m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::BoneTransforms),m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::InverseSkinMatrices),m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::InverseBindMatrices));
        auto* rc=context.hostData->Get<RenderContext>(); auto& cmd=context.commandList; cmd.SetDescriptorHeaps(rc->textureDescriptorHeap.GetHandle(),rc->samplerDescriptorHeap.GetHandle()); cmd.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle()); cmd.BindPipeline(m_pso.GetAPIPipelineState().GetHandle()); cmd.PushConstants(rhi::ShaderStage::Compute,0,MiscUintRootSignatureIndex,0,sizeof(c)/4,reinterpret_cast<const uint32_t*>(&c)); cmd.ExecuteIndirect(m_signature->GetHandle(),m_resources->indirectCommands->GetAPIResource().GetHandle(),0,m_resources->allocationCounters->GetAPIResource().GetHandle(),sizeof(uint32_t),m_resources->typeCount); return {};
    }
    void Cleanup() override { m_signature.Reset(); }
private: std::shared_ptr<WindSharedResources> m_resources; PipelineState m_pso; rhi::CommandSignaturePtr m_signature;
};

class WindSkeletonDebugPass final : public RenderPass {
public:
    explicit WindSkeletonDebugPass(std::shared_ptr<WindSharedResources> resources) : m_resources(std::move(resources))
    {
        ShaderInfoBundle shaders;
        shaders.meshShader = { L"shaders/debugSkeleton.hlsl", L"MSWindMain", L"ms_6_6" };
        shaders.pixelShader = { L"shaders/debugSkeleton.hlsl", L"PSMain", L"ps_6_6" };
        const auto compiled = PSOManager::GetInstance().CompileShaders(shaders);
        m_bindings = compiled.resourceDescriptorSlots;
        auto& layout = PSOManager::GetInstance().GetRootSignature();
        rhi::SubobjLayout soLayout{ layout.GetHandle() };
        rhi::SubobjShader soMS{ rhi::ShaderStage::Mesh, rhi::DXIL(compiled.meshShader.Get()), "MSWindMain" };
        rhi::SubobjShader soPS{ rhi::ShaderStage::Pixel, rhi::DXIL(compiled.pixelShader.Get()), "PSMain" };
        rhi::RasterState raster{}; raster.fill = rhi::FillMode::Solid; raster.cull = rhi::CullMode::None;
        rhi::SubobjRaster soRaster{ raster };
        rhi::BlendState blend{}; blend.numAttachments = 1; blend.attachments[0].writeMask = rhi::ColorWriteEnable::All;
        rhi::SubobjBlend soBlend{ blend };
        rhi::DepthStencilState depth{}; depth.depthEnable = false; depth.depthWrite = false; depth.depthFunc = rhi::CompareOp::Always;
        rhi::SubobjDepth soDepth{ depth };
        rhi::RenderTargets targets{}; targets.count = 1; targets.formats[0] = rhi::Format::R8G8B8A8_UNorm;
        rhi::SubobjRTVs soTargets{ targets };
        rhi::SubobjSample soSample{ rhi::SampleDesc{1, 0} };
        rhi::SubobjPrimitiveTopology soTopology{ rhi::PrimitiveTopology::LineList };
        const rhi::PipelineStreamItem items[] = { rhi::Make(soLayout), rhi::Make(soMS), rhi::Make(soPS),
            rhi::Make(soRaster), rhi::Make(soBlend), rhi::Make(soDepth), rhi::Make(soTargets),
            rhi::Make(soSample), rhi::Make(soTopology) };
        if (Failed(DeviceManager::GetInstance().GetDevice().CreatePipeline(items, static_cast<uint32_t>(std::size(items)), m_pso)))
            throw std::runtime_error("Failed to create procedural-wind skeleton debug PSO");
        m_pso->SetName("ProceduralWind.SkeletonDebug.PSO");
        rhi::IndirectArg args[] = {
            {.kind=rhi::IndirectArgKind::Constant,.u={.rootConstants={IndirectCommandSignatureRootSignatureIndex,0,3}}},
            {.kind=rhi::IndirectArgKind::DispatchMesh}
        };
        DeviceManager::GetInstance().GetDevice().CreateCommandSignature(
            {rhi::Span<rhi::IndirectArg>(args, 2), sizeof(WindIndirectCommand)}, layout.GetHandle(), m_signature);
    }
    void DeclareResourceUsages(RenderPassBuilder* b) override
    {
        b->WithShaderResource(m_resources->windTypes, m_resources->boneEntries, m_resources->activeInstances,
            Builtin::SkeletonResources::BoneTransforms, Builtin::SkeletonResources::InverseBindMatrices,
            Builtin::SkeletonResources::SkinningInstanceInfo, Builtin::InstanceDrawRecordBuffer,
            Builtin::PerMeshInstanceBuffer, Builtin::PerInstanceTransformBuffer, Builtin::CameraBuffer)
            .WithConstantBuffer(Builtin::PerFrameBuffer)
            .WithIndirectArguments(m_resources->indirectCommands, m_resources->allocationCounters)
            .WithRenderTarget(Builtin::Backbuffer);
    }
    void Setup() override {} void Update(const UpdateExecutionContext&) override {}
    PassReturn Execute(PassExecutionContext& context) override
    {
        const auto outputType = SettingsManager::GetInstance().getSettingGetter<unsigned int>("outputType")();
        if (outputType != static_cast<unsigned int>(OutputType::SKELETONS) || !m_resources->typeCount) return {};
        if (!m_loggedDispatch) {
            spdlog::info("ProceduralWind skeleton debug: executing GPU indirect mesh dispatch over typeSlots={} (command count from allocationCounters[1])",
                m_resources->typeCount);
            m_loggedDispatch = true;
        }
        auto* rc = context.hostData->Get<RenderContext>(); auto& cmd = context.commandList;
        cmd.SetDescriptorHeaps(rc->textureDescriptorHeap.GetHandle(), rc->samplerDescriptorHeap.GetHandle());
        rhi::PassBeginInfo pass{}; rhi::ColorAttachment color{};
        color.rtv = { rc->rtvHeap.GetHandle(), rc->frameIndex }; color.loadOp = rhi::LoadOp::Load; color.storeOp = rhi::StoreOp::Store;
        pass.colors = { &color }; pass.width = rc->outputResolution.x; pass.height = rc->outputResolution.y; pass.debugName = "Wind Skeleton Debug Overlay";
        cmd.BeginPass(pass); cmd.SetPrimitiveTopology(rhi::PrimitiveTopology::LineList);
        cmd.BindLayout(PSOManager::GetInstance().GetRootSignature().GetHandle()); cmd.BindPipeline(m_pso->GetHandle());
        BindResourceDescriptorIndices(cmd, m_bindings);
        uint32_t constants[8] = {
            m_resources->windTypes->GetSRVInfo(0).slot.index, m_resources->boneEntries->GetSRVInfo(0).slot.index,
            m_resources->activeInstances->GetSRVInfo(0).slot.index,
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::BoneTransforms)->GetSRVInfo(0).slot.index,
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::InverseBindMatrices)->GetSRVInfo(0).slot.index,
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::SkinningInstanceInfo)->GetSRVInfo(0).slot.index,
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::PerFrameBuffer)->GetCBVInfo().slot.index,
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::CameraBuffer)->GetSRVInfo(0).slot.index };
        cmd.PushConstants(rhi::ShaderStage::AllGraphics, 0, MiscUintRootSignatureIndex, 0, 8, constants);
        cmd.ExecuteIndirect(m_signature->GetHandle(), m_resources->indirectCommands->GetAPIResource().GetHandle(), 0,
            m_resources->allocationCounters->GetAPIResource().GetHandle(), sizeof(uint32_t), m_resources->typeCount);
        return {};
    }
    void Cleanup() override { m_signature.Reset(); m_pso.Reset(); }
private:
    std::shared_ptr<WindSharedResources> m_resources; rhi::PipelinePtr m_pso; rhi::CommandSignaturePtr m_signature; PipelineResources m_bindings; bool m_loggedDispatch = false;
};

} // namespace

ProceduralWindExtension::ProceduralWindExtension(std::shared_ptr<ProceduralWindRuntime> runtime) : m_runtime(std::move(runtime)) {}

void ProceduralWindExtension::GatherStructuralPasses(RenderGraph&, std::vector<RenderGraph::ExternalPassDesc>& out)
{
    auto resources = std::make_shared<WindSharedResources>(m_runtime);
    auto insertion = RenderGraph::ExternalInsertPoint::Before("CLodOpaque::HierarchicalCullingPass1");
    out.push_back(RenderGraph::ExternalPassDesc::Compute("ProceduralWind::UploadFieldPair", std::make_shared<WindResidencyPass>(resources)).At(insertion));
    out.push_back(RenderGraph::ExternalPassDesc::Compute("ProceduralWind::ResetTransient", std::make_shared<WindResetPass>(resources)).At(insertion));
    out.push_back(RenderGraph::ExternalPassDesc::Compute("ProceduralWind::ActivateInstances", std::make_shared<WindActivatePass>(resources)).At(insertion));
    out.push_back(RenderGraph::ExternalPassDesc::Compute("ProceduralWind::BuildSimulationCommands", std::make_shared<WindBuildCommandsPass>(resources)).At(insertion));
    out.push_back(RenderGraph::ExternalPassDesc::Compute("ProceduralWind::SimulateInstances", std::make_shared<WindIndirectSimulatePass>(resources)).At(insertion));
    out.push_back(RenderGraph::ExternalPassDesc::Render("ProceduralWind::DebugActiveSkeletons", std::make_shared<WindSkeletonDebugPass>(resources))
        .At(RenderGraph::ExternalInsertPoint::Before("MenuRenderPass")));
}

} // namespace br::wind
