#include "ProceduralWind/ProceduralWindExtension.h"

#include "Animation/Skeleton.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/SkeletonManager.h"
#include "Render/BuiltinResources.h"
#include "Render/IndirectCommand.h"
#include "Render/PassBuilders.h"
#include "Render/RenderContext.h"
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

static_assert(sizeof(WindBoneGPU) == 80u);
static_assert(sizeof(WindRootConstants) % sizeof(std::uint32_t) == 0u);

struct WindSharedResources {
    explicit WindSharedResources(std::shared_ptr<ProceduralWindRuntime> runtimeIn)
        : runtime(std::move(runtimeIn))
        , fieldSlices{ DynamicStructuredBuffer<std::uint32_t>::CreateShared(1u, "ProceduralWind.FieldSlice0"),
                       DynamicStructuredBuffer<std::uint32_t>::CreateShared(1u, "ProceduralWind.FieldSlice1") }
        , boneEntries(DynamicStructuredBuffer<WindBoneGPU>::CreateShared(1u, "ProceduralWind.BoneEntries"))
        , scratchForward(DynamicStructuredBuffer<DirectX::XMFLOAT4X4>::CreateShared(1u, "ProceduralWind.BaseForward", true))
        , scratchInverse(DynamicStructuredBuffer<DirectX::XMFLOAT4X4>::CreateShared(1u, "ProceduralWind.BaseInverse", true))
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

    void UpdateBones(const UpdateExecutionContext& context)
    {
        const auto* update = context.hostData ? context.hostData->Get<UpdateContext>() : nullptr;
        if (!update || !update->skeletonManager) {
            activeBoneCount = 0u;
            return;
        }
        std::vector<WindBoneGPU> next;
        for (const auto& instance : update->skeletonManager->GetActiveInstanceViews()) {
            if (!instance.skeleton || !instance.skeleton->HasWindSimulationGroups()) continue;
            const auto groups = instance.skeleton->GetWindSimulationGroupIndices();
            const auto parents = instance.skeleton->GetParentIndices();
            const auto profile = runtime->ResolveProfile(instance.skeleton->GetWindProfileIdentity());
            const auto firstEntry = static_cast<std::uint32_t>(next.size());
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
                entry.frequencies = profile.harmonicFrequenciesHz;
                entry.weights = profile.harmonicWeights;
                next.push_back(entry);
            }
        }
        const auto requestedBoneCount = static_cast<std::uint32_t>(next.size());
        boneEntries->ReplaceData(std::move(next));
        scratchForward->EnsureSize(std::max<std::uint32_t>(1u, requestedBoneCount));
        scratchInverse->EnsureSize(std::max<std::uint32_t>(1u, requestedBoneCount));
        activeBoneCount = std::min({ requestedBoneCount, boneEntries->ResidentCapacity(),
            scratchForward->ResidentCapacity(), scratchInverse->ResidentCapacity() });
        elapsedSeconds += context.deltaTime;
        state = runtime->SnapshotWindState();
        TracyPlot("ProceduralWind.SimulatedBones", static_cast<std::int64_t>(activeBoneCount));
    }

    std::shared_ptr<ProceduralWindRuntime> runtime;
    std::array<std::shared_ptr<DynamicStructuredBuffer<std::uint32_t>>, 2> fieldSlices;
    std::shared_ptr<DynamicStructuredBuffer<WindBoneGPU>> boneEntries;
    std::shared_ptr<DynamicStructuredBuffer<DirectX::XMFLOAT4X4>> scratchForward;
    std::shared_ptr<DynamicStructuredBuffer<DirectX::XMFLOAT4X4>> scratchInverse;
    std::uint64_t fieldRevision = 0u;
    bool fieldReady = false;
    std::uint32_t activeBoneCount = 0u;
    float elapsedSeconds = 0.0f;
    WindState state{};
    ResidentWindPair residentPair{};
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

class WindCapturePass final : public ComputePass {
public:
    explicit WindCapturePass(std::shared_ptr<WindSharedResources> resources) : m_resources(std::move(resources))
    {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"SARPShaders/ProceduralWind.hlsl", L"CaptureWindBasePoseCS", {}, "ProceduralWind.CaptureBasePose");
    }
    void DeclareResourceUsages(ComputePassBuilder* builder) override
    {
        builder->WithShaderResource(m_resources->boneEntries, Builtin::SkeletonResources::SkinningInstanceInfo,
            Builtin::SkeletonResources::BoneTransforms, Builtin::SkeletonResources::InverseSkinMatrices)
            .WithUnorderedAccess(m_resources->scratchForward, m_resources->scratchInverse);
    }
    void Setup() override {}
    void Update(const UpdateExecutionContext& context) override { m_resources->UpdateBones(context); }
    PassReturn Execute(PassExecutionContext& context) override
    {
        if (!m_resources->activeBoneCount) return {};
        WindRootConstants constants{};
        constants.boneEntries = m_resources->boneEntries->GetSRVInfo(0).slot.index;
        constants.scratchForward = m_resources->scratchForward->GetUAVShaderVisibleInfo(0).slot.index;
        constants.scratchInverse = m_resources->scratchInverse->GetUAVShaderVisibleInfo(0).slot.index;
        constants.skinningInfo = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::SkinningInstanceInfo)->GetSRVInfo(0).slot.index;
        constants.forwardSkin = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::BoneTransforms)->GetSRVInfo(0).slot.index;
        constants.inverseSkin = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::InverseSkinMatrices)->GetSRVInfo(0).slot.index;
        constants.boneCount = m_resources->activeBoneCount;
        BindAndDispatch(context, m_pso, constants);
        return {};
    }
    void Cleanup() override {}
private:
    std::shared_ptr<WindSharedResources> m_resources;
    PipelineState m_pso;
};

class WindSimulatePass final : public ComputePass {
public:
    explicit WindSimulatePass(std::shared_ptr<WindSharedResources> resources) : m_resources(std::move(resources))
    {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"SARPShaders/ProceduralWind.hlsl", L"SimulateWindBonesCS", {}, "ProceduralWind.SimulateBones");
    }
    void DeclareResourceUsages(ComputePassBuilder* builder) override
    {
        builder->WithShaderResource(m_resources->boneEntries, m_resources->scratchForward, m_resources->scratchInverse,
            Builtin::SkeletonResources::SkinningInstanceInfo, Builtin::SkeletonResources::InverseBindMatrices,
            m_resources->fieldSlices[0], m_resources->fieldSlices[1])
            .WithUnorderedAccess(Builtin::SkeletonResources::BoneTransforms, Builtin::SkeletonResources::InverseSkinMatrices);
    }
    void Setup() override {}
    void Update(const UpdateExecutionContext&) override {}
    PassReturn Execute(PassExecutionContext& context) override
    {
        if (!m_resources->activeBoneCount) return {};
        WindRootConstants constants{};
        constants.boneEntries = m_resources->boneEntries->GetSRVInfo(0).slot.index;
        constants.scratchForward = m_resources->scratchForward->GetSRVInfo(0).slot.index;
        constants.scratchInverse = m_resources->scratchInverse->GetSRVInfo(0).slot.index;
        constants.skinningInfo = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::SkinningInstanceInfo)->GetSRVInfo(0).slot.index;
        constants.forwardSkin = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::BoneTransforms)->GetUAVShaderVisibleInfo(0).slot.index;
        constants.inverseSkin = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::InverseSkinMatrices)->GetUAVShaderVisibleInfo(0).slot.index;
        constants.inverseBind = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::InverseBindMatrices)->GetSRVInfo(0).slot.index;
        constants.boneCount = m_resources->activeBoneCount;
        constants.elapsedSeconds = m_resources->elapsedSeconds;
        constants.windX = m_resources->state.directionToWS.x;
        constants.windY = m_resources->state.directionToWS.y;
        constants.strength = m_resources->state.strength;
        constants.gustStrength = m_resources->state.gustStrength;
        constants.fieldSlice0 = m_resources->fieldSlices[0]->GetSRVInfo(0).slot.index;
        constants.fieldSlice1 = m_resources->fieldSlices[1]->GetSRVInfo(0).slot.index;
        const auto& resident = m_resources->residentPair;
        constants.fieldDimensions = (resident.metadata.width & 0xFFFFu) | ((resident.metadata.height & 0xFFFFu) << 16u);
        constants.fieldCellSize = resident.metadata.cellSize;
        constants.fieldOriginX = resident.metadata.origin.x;
        constants.fieldOriginY = resident.metadata.origin.y;
        constants.fieldInterpolation = resident.bracket.interpolation;
        constants.fieldValid = resident.valid ? 1u : 0u;
        BindAndDispatch(context, m_pso, constants);
        return {};
    }
    void Cleanup() override {}
private:
    std::shared_ptr<WindSharedResources> m_resources;
    PipelineState m_pso;
};

} // namespace

ProceduralWindExtension::ProceduralWindExtension(std::shared_ptr<ProceduralWindRuntime> runtime) : m_runtime(std::move(runtime)) {}

void ProceduralWindExtension::GatherStructuralPasses(RenderGraph&, std::vector<RenderGraph::ExternalPassDesc>& out)
{
    auto resources = std::make_shared<WindSharedResources>(m_runtime);
    auto insertion = RenderGraph::ExternalInsertPoint::Before("CLodOpaque::HierarchicalCullingPass1");
    out.push_back(RenderGraph::ExternalPassDesc::Compute("ProceduralWind::UploadFieldPair", std::make_shared<WindResidencyPass>(resources)).At(insertion));
    out.push_back(RenderGraph::ExternalPassDesc::Compute("ProceduralWind::CaptureBasePose", std::make_shared<WindCapturePass>(resources)).At(insertion));
    out.push_back(RenderGraph::ExternalPassDesc::Compute("ProceduralWind::SimulateBones", std::make_shared<WindSimulatePass>(resources)).At(insertion));
}

} // namespace br::wind
