#pragma once

#include <bit>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/CommandSignatureManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Managers/MaterialManager.h"
#include "Managers/MeshManager.h"
#include "Render/RenderContext.h"
#include "Render/MaterialEvaluationBuildInputs.h"
#include "Render/MaterialStateArtifacts.h"
#include "Render/IndirectCommand.h"
#include "Render/GraphExtensions/CLodExtensionComponents.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Resources/Buffers/PagePool.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "../shaders/PerPassRootConstants/visUtilRootConstants.h"
#include "Materials/TechniqueDescriptor.h"

namespace TerrainRegionMaterialEval
{
    static constexpr uint32_t MaxTerrainRegions = 65536u;
    static constexpr uint32_t PrefixBlockSize = 1024u;
    static constexpr uint32_t TerrainSetIndexV1 = 0u;

    inline bool IsTerrainMaterialFlags(MaterialCompileFlags flags)
    {
        return (flags & MaterialCompileFlags::MaterialCompileTerrain) != 0;
    }

    inline std::vector<uint64_t> RecipeRevision(const PipelineState& pso, const org::PassPrepareContext& preparation) {
        const auto& context = *preparation.preparationData->Get<UpdateContext>();
        const auto& signatures = CommandSignatureManager::GetInstance();
        return {reinterpret_cast<uintptr_t>(pso.PeekPayload()), SettingsManager::GetInstance().Revision(),
            context.publishedRendererState ? context.publishedRendererState->materials.revision : 0u,
            reinterpret_cast<uintptr_t>(signatures.CaptureRawDispatchCommandSignature().get()),
            reinterpret_cast<uintptr_t>(signatures.CaptureMaterialEvaluationCommandSignature().get()),
            reinterpret_cast<uintptr_t>(signatures.CaptureTerrainRegionMaterialEvaluationCommandSignature().get())};
    }
}

class TerrainRegionCounterResetPass : public org::TypedRenderGraphPass<TerrainRegionCounterResetPass, org::EmptyPassFrameData, org::LegacyPassBindings, br::render::PreparedComputeDispatch> {
public:
    TerrainRegionCounterResetPass() {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/VisUtil.hlsl",
            L"ClearTerrainRegionCountersCS",
            {},
            "VisUtil_ClearTerrainRegionCountersPSO");
    }

    void Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* b = &builder;
        b->WithUnorderedAccess(
            "Builtin::VisUtil::TerrainRegionPixelCountBuffer",
            "Builtin::VisUtil::TerrainRegionWriteCursorBuffer",
            "Builtin::VisUtil::TerrainRegionActiveCountBuffer");
    }

    br::render::PreparedComputeDispatch BuildRecipe(const org::PassPrepareContext& preparation) const {
        const auto& ctx = *preparation.preparationData->Get<UpdateContext>();
        br::render::PreparedComputeDispatch data{};
        data.resourceHeap = ctx.textureDescriptorHeap.GetHandle();
        data.samplerHeap = ctx.samplerDescriptorHeap.GetHandle();
        auto program = preparation.CaptureProgramBinding(m_pso);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);
        data.constants[0] = TerrainRegionMaterialEval::MaxTerrainRegions;
        data.groupsX = (TerrainRegionMaterialEval::MaxTerrainRegions + 63u) / 64u; data.groupsY = 1u; data.groupsZ = 1u;
        return data;
    }

    std::vector<uint64_t> RecipeRevision(const org::PassPrepareContext& preparation) const { return TerrainRegionMaterialEval::RecipeRevision(m_pso, preparation); }
    org::EmptyPassFrameData PrepareInvocation(const br::render::PreparedComputeDispatch&, const org::PassPrepareContext&) const { return {}; }
    static void Record(const br::render::PreparedComputeDispatch& data, const org::EmptyPassFrameData&, org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatch(data, recording);
    }

private:
    PipelineState m_pso;
};

template<class Derived>
class TerrainRegionMaterialRangePassBase : public org::TypedRenderGraphPass<Derived, org::EmptyPassFrameData, org::LegacyPassBindings, std::vector<br::render::PreparedComputeIndirect>> {
public:
    explicit TerrainRegionMaterialRangePassBase(const wchar_t* entryPoint, const char* debugName) {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/VisUtil.hlsl",
            entryPoint,
            {},
            debugName);
    }

    void Initialize() {
        m_materialEvalCmds = this->m_resourceRegistryView->template RequestPtr<Resource>("Builtin::IndirectCommandBuffers::MaterialEvaluationCommandBuffer");
    }

    void ShutdownPass() { m_materialEvalCmds = nullptr; }

    std::vector<br::render::PreparedComputeIndirect> BuildRecipe(const org::PassPrepareContext& preparation) const {
        const auto& ctx = *preparation.preparationData->Get<UpdateContext>();
        std::vector<br::render::PreparedComputeIndirect> result;
        br::render::PreparedComputeIndirect data{};
        data.resourceHeap = ctx.textureDescriptorHeap.GetHandle();
        data.samplerHeap = ctx.samplerDescriptorHeap.GetHandle();
        auto program = preparation.CaptureProgramBinding(m_pso);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);
        data.commandSignature = preparation.CaptureCommandSignature(
            CommandSignatureManager::GetInstance().CaptureMaterialEvaluationCommandSignature());
        data.argumentsReference = preparation.CaptureResource(m_materialEvalCmds->GetGlobalResourceID());
        data.constants[0] = TerrainRegionMaterialEval::TerrainSetIndexV1;
        const uint64_t stride = sizeof(MaterialEvaluationIndirectCommand);
        const auto materialState = ctx.publishedRendererState
            ? ctx.publishedRendererState->materials.payload.Get<br::render::PublishedMaterialState>()
            : nullptr;
        if (!materialState) return result;
        for (std::size_t activeIndex = 0; activeIndex < materialState->activeCompileFlags.size(); ++activeIndex) {
            const MaterialCompileFlags flags = materialState->activeCompileFlags[activeIndex];
            if (!TerrainRegionMaterialEval::IsTerrainMaterialFlags(flags)) {
                continue;
            }
            if (activeIndex >= materialState->activeCompileFlagSlots.size()) {
                continue;
            }
            const uint32_t slot = materialState->activeCompileFlagSlots[activeIndex];
            if (slot >= materialState->compileFlagSlotsUsed) continue;
            const uint64_t argOffset = static_cast<uint64_t>(slot) * stride;
            data.argumentsOffset = argOffset;
            result.push_back(data);
        }
        return result;
    }

    std::vector<uint64_t> RecipeRevision(const org::PassPrepareContext& preparation) const { return TerrainRegionMaterialEval::RecipeRevision(m_pso, preparation); }
    org::EmptyPassFrameData PrepareInvocation(const std::vector<br::render::PreparedComputeIndirect>&, const org::PassPrepareContext&) const { return {}; }
    static void Record(const std::vector<br::render::PreparedComputeIndirect>& work, const org::EmptyPassFrameData&, org::PassRecordContext& recording) {
        for (const auto& data : work) br::render::RecordPreparedComputeIndirect(data, recording);
    }

    Resource* m_materialEvalCmds = nullptr;
    PipelineState m_pso;
};

class TerrainRegionHistogramPass : public TerrainRegionMaterialRangePassBase<TerrainRegionHistogramPass> {
public:
    TerrainRegionHistogramPass()
        : TerrainRegionMaterialRangePassBase<TerrainRegionHistogramPass>(L"TerrainRegionHistogramFromMaterialRangeCS", "VisUtil_TerrainRegionHistogramPSO") {}

    void Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* b = &builder;
        b->WithShaderResource(
            "Builtin::VisUtil::PixelListBuffer",
            Builtin::PrimaryCamera::VisibilityTexture,
            Builtin::CameraBuffer,
            Builtin::Terrain::Sets)
            .WithUnorderedAccess(
                "Builtin::VisUtil::TerrainRegionPixelCountBuffer",
                "Builtin::VisUtil::TerrainRegionActiveListBuffer",
                "Builtin::VisUtil::TerrainRegionActiveCountBuffer")

            .WithConstantBuffer(Builtin::PerFrameBuffer);
        builder.WithIndirectArguments("Builtin::IndirectCommandBuffers::MaterialEvaluationCommandBuffer");
    }

};

class TerrainRegionBlockScanPass : public org::TypedRenderGraphPass<TerrainRegionBlockScanPass, org::EmptyPassFrameData, org::LegacyPassBindings, br::render::PreparedComputeDispatch> {
public:
    TerrainRegionBlockScanPass() {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/materialPrefixSum.hlsl",
            L"TerrainRegionBlockScanCS",
            {},
            "VisUtil_TerrainRegionBlockScanPSO");
    }

    void Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* b = &builder;
        b->WithShaderResource("Builtin::VisUtil::TerrainRegionPixelCountBuffer")
            .WithUnorderedAccess(
                "Builtin::VisUtil::TerrainRegionOffsetBuffer",
                "Builtin::VisUtil::TerrainRegionBlockSumsBuffer");
    }

    br::render::PreparedComputeDispatch BuildRecipe(const org::PassPrepareContext& preparation) const {
        const auto& ctx = *preparation.preparationData->Get<UpdateContext>();
        br::render::PreparedComputeDispatch data{};
        data.resourceHeap = ctx.textureDescriptorHeap.GetHandle();
        data.samplerHeap = ctx.samplerDescriptorHeap.GetHandle();
        auto program = preparation.CaptureProgramBinding(m_pso);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);
        data.constants[0] = TerrainRegionMaterialEval::MaxTerrainRegions;
        data.groupsX = (TerrainRegionMaterialEval::MaxTerrainRegions + TerrainRegionMaterialEval::PrefixBlockSize - 1u) / TerrainRegionMaterialEval::PrefixBlockSize; data.groupsY = 1u; data.groupsZ = 1u;
        return data;
    }

    std::vector<uint64_t> RecipeRevision(const org::PassPrepareContext& preparation) const { return TerrainRegionMaterialEval::RecipeRevision(m_pso, preparation); }
    org::EmptyPassFrameData PrepareInvocation(const br::render::PreparedComputeDispatch&, const org::PassPrepareContext&) const { return {}; }
    static void Record(const br::render::PreparedComputeDispatch& data, const org::EmptyPassFrameData&, org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatch(data, recording);
    }

private:
    PipelineState m_pso;
};

class TerrainRegionBlockOffsetsPass : public org::TypedRenderGraphPass<TerrainRegionBlockOffsetsPass, org::EmptyPassFrameData, org::LegacyPassBindings, br::render::PreparedComputeDispatch> {
public:
    TerrainRegionBlockOffsetsPass() {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/materialPrefixSum.hlsl",
            L"TerrainRegionBlockOffsetsCS",
            {},
            "VisUtil_TerrainRegionBlockOffsetsPSO");
    }

    void Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* b = &builder;
        b->WithShaderResource(
            "Builtin::VisUtil::TerrainRegionPixelCountBuffer",
            "Builtin::VisUtil::TerrainRegionBlockSumsBuffer")
            .WithUnorderedAccess(
                "Builtin::VisUtil::TerrainRegionOffsetBuffer",
                "Builtin::VisUtil::TerrainRegionScannedBlockSumsBuffer",
                "Builtin::VisUtil::TerrainRegionTotalPixelCountBuffer");
    }

    br::render::PreparedComputeDispatch BuildRecipe(const org::PassPrepareContext& preparation) const {
        const auto& ctx = *preparation.preparationData->Get<UpdateContext>();
        br::render::PreparedComputeDispatch data{};
        data.resourceHeap = ctx.textureDescriptorHeap.GetHandle();
        data.samplerHeap = ctx.samplerDescriptorHeap.GetHandle();
        auto program = preparation.CaptureProgramBinding(m_pso);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);
        data.constants[0] = TerrainRegionMaterialEval::MaxTerrainRegions;
        data.constants[1] = (TerrainRegionMaterialEval::MaxTerrainRegions + TerrainRegionMaterialEval::PrefixBlockSize - 1u) / TerrainRegionMaterialEval::PrefixBlockSize;
        data.groupsX = 1u; data.groupsY = 1u; data.groupsZ = 1u;
        return data;
    }

    std::vector<uint64_t> RecipeRevision(const org::PassPrepareContext& preparation) const { return TerrainRegionMaterialEval::RecipeRevision(m_pso, preparation); }
    org::EmptyPassFrameData PrepareInvocation(const br::render::PreparedComputeDispatch&, const org::PassPrepareContext&) const { return {}; }
    static void Record(const br::render::PreparedComputeDispatch& data, const org::EmptyPassFrameData&, org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatch(data, recording);
    }

private:
    PipelineState m_pso;
};

class TerrainRegionPixelListPass : public TerrainRegionMaterialRangePassBase<TerrainRegionPixelListPass> {
public:
    TerrainRegionPixelListPass()
        : TerrainRegionMaterialRangePassBase<TerrainRegionPixelListPass>(L"TerrainRegionListFromMaterialRangeCS", "VisUtil_TerrainRegionPixelListPSO") {}

    void Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* b = &builder;
        b->WithShaderResource(
            "Builtin::VisUtil::PixelListBuffer",
            Builtin::PrimaryCamera::VisibilityTexture,
            Builtin::CameraBuffer,
            Builtin::Terrain::Sets,
            "Builtin::VisUtil::TerrainRegionOffsetBuffer")
            .WithUnorderedAccess(
                "Builtin::VisUtil::TerrainRegionWriteCursorBuffer",
                "Builtin::VisUtil::TerrainRegionPixelListBuffer")

            .WithConstantBuffer(Builtin::PerFrameBuffer);
        builder.WithIndirectArguments("Builtin::IndirectCommandBuffers::MaterialEvaluationCommandBuffer");
    }

};

class BuildTerrainRegionMaterialIndirectCommandBuildDispatchArgsPass : public org::TypedRenderGraphPass<BuildTerrainRegionMaterialIndirectCommandBuildDispatchArgsPass, org::EmptyPassFrameData, org::LegacyPassBindings, br::render::PreparedComputeDispatch> {
public:
    BuildTerrainRegionMaterialIndirectCommandBuildDispatchArgsPass() {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/VisUtil.hlsl",
            L"BuildTerrainRegionCommandBuildDispatchArgsCS",
            {},
            "VisUtil_BuildTerrainRegionCommandBuildDispatchArgsPSO");
    }

    void Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* b = &builder;
        b->WithShaderResource("Builtin::VisUtil::TerrainRegionActiveCountBuffer")
            .WithUnorderedAccess("Builtin::IndirectCommandBuffers::TerrainRegionMaterialEvaluationCommandBuildDispatchArgsBuffer");
    }

    br::render::PreparedComputeDispatch BuildRecipe(const org::PassPrepareContext& preparation) const {
        const auto& ctx = *preparation.preparationData->Get<UpdateContext>();
        br::render::PreparedComputeDispatch data{};
        data.resourceHeap = ctx.textureDescriptorHeap.GetHandle();
        data.samplerHeap = ctx.samplerDescriptorHeap.GetHandle();
        auto program = preparation.CaptureProgramBinding(m_pso);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);

        data.groupsX = 1u; data.groupsY = 1u; data.groupsZ = 1u;
        return data;
    }

    std::vector<uint64_t> RecipeRevision(const org::PassPrepareContext& preparation) const { return TerrainRegionMaterialEval::RecipeRevision(m_pso, preparation); }
    org::EmptyPassFrameData PrepareInvocation(const br::render::PreparedComputeDispatch&, const org::PassPrepareContext&) const { return {}; }
    static void Record(const br::render::PreparedComputeDispatch& data, const org::EmptyPassFrameData&, org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatch(data, recording);
    }

private:
    PipelineState m_pso;
};

class BuildTerrainRegionMaterialIndirectCommandBufferPass : public org::TypedRenderGraphPass<BuildTerrainRegionMaterialIndirectCommandBufferPass, org::EmptyPassFrameData, org::LegacyPassBindings, br::render::PreparedComputeIndirect> {
public:
    BuildTerrainRegionMaterialIndirectCommandBufferPass() {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/VisUtil.hlsl",
            L"BuildTerrainRegionEvaluateIndirectArgsCS",
            {},
            "VisUtil_BuildTerrainRegionEvaluateIndirectArgsPSO");
    }

    void Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* b = &builder;
        b->WithShaderResource(
            "Builtin::VisUtil::TerrainRegionActiveCountBuffer",
            "Builtin::VisUtil::TerrainRegionActiveListBuffer",
            "Builtin::VisUtil::TerrainRegionPixelCountBuffer",
            "Builtin::VisUtil::TerrainRegionOffsetBuffer")
            .WithUnorderedAccess("Builtin::IndirectCommandBuffers::TerrainRegionMaterialEvaluationCommandBuffer");
        builder.WithIndirectArguments("Builtin::IndirectCommandBuffers::TerrainRegionMaterialEvaluationCommandBuildDispatchArgsBuffer");
    }

    void Initialize() {
        m_dispatchArgs = m_resourceRegistryView->RequestPtr<Resource>("Builtin::IndirectCommandBuffers::TerrainRegionMaterialEvaluationCommandBuildDispatchArgsBuffer");
    }

    void ShutdownPass() { m_dispatchArgs = nullptr; }

    br::render::PreparedComputeIndirect BuildRecipe(const org::PassPrepareContext& preparation) const {
        const auto& ctx = *preparation.preparationData->Get<UpdateContext>();
        br::render::PreparedComputeIndirect data{};
        data.resourceHeap = ctx.textureDescriptorHeap.GetHandle();
        data.samplerHeap = ctx.samplerDescriptorHeap.GetHandle();
        auto program = preparation.CaptureProgramBinding(m_pso);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);
        data.constants[0] = TerrainRegionMaterialEval::MaxTerrainRegions;
        data.constants[1] = TerrainRegionMaterialEval::TerrainSetIndexV1;
        data.commandSignature = preparation.CaptureCommandSignature(CommandSignatureManager::GetInstance().CaptureRawDispatchCommandSignature());
        data.argumentsReference = preparation.CaptureResource(m_dispatchArgs->GetGlobalResourceID());
        return data;
    }

    std::vector<uint64_t> RecipeRevision(const org::PassPrepareContext& preparation) const { return TerrainRegionMaterialEval::RecipeRevision(m_pso, preparation); }
    org::EmptyPassFrameData PrepareInvocation(const br::render::PreparedComputeIndirect&, const org::PassPrepareContext&) const { return {}; }
    static void Record(const br::render::PreparedComputeIndirect& data, const org::EmptyPassFrameData&, org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeIndirect(data, recording);
    }

private:
    PipelineState m_pso;
    Resource* m_dispatchArgs = nullptr;
};

struct EvaluateTerrainRegionMaterialGroupsBindings {
    org::ResourceBindingToken visibleClusters, reyesDiceQueue;
    org::ResourceBindingToken reyesTessTableConfigs, reyesTessTableVertices, reyesTessTableTriangles;
    bool hasReyesDiceQueue = false, hasReyesTessTables = false;
    uint32_t patchVisibilityIndexBase = 0;
};

class EvaluateTerrainRegionMaterialGroupsPass : public org::TypedRenderGraphPass<EvaluateTerrainRegionMaterialGroupsPass,
    org::EmptyPassFrameData, EvaluateTerrainRegionMaterialGroupsBindings, br::render::PreparedComputeIndirect> {
public:
    explicit EvaluateTerrainRegionMaterialGroupsPass(const MaterialEvaluationBuildInputs& services) {
        std::vector<DxcDefine> defines;
        defines.push_back({ L"PSO_TERRAIN", L"1" });
        defines.push_back({ L"VISUTIL_SPECIALIZED_MATERIAL_EVAL", L"1" });
        defines.push_back({ L"VISUTIL_USE_COMPACT_MATERIAL_EVAL", L"1" });
        defines.push_back({ L"CLOD_VSM_ADAPTIVE_RECEIVER_SCREEN_TRACE", L"1" });
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/VisUtilEvaluateTerrainRegion.hlsl",
            L"EvaluateTerrainRegionMaterialGroupCS",
            std::move(defines),
            "VisUtil_EvaluateTerrainRegionMaterialGroupPSO");

        m_visibleClusterResource = services.visibleClusters;
        m_reyesDiceQueueResource = services.reyesDiceQueue;
        m_reyesTessTableConfigsResource = services.reyesTessTableConfigs;
        m_reyesTessTableVerticesResource = services.reyesTessTableVertices;
        m_reyesTessTableTrianglesResource = services.reyesTessTableTriangles;
        m_patchVisibilityIndexBase = CLodReyesPatchVisibilityIndexBase(services.visibleClusterCapacity);
        m_slabResourceGroup = services.clodSlabResources;
    }
    EvaluateTerrainRegionMaterialGroupsBindings Declare(org::PassBuilder& builder) {
        EvaluateTerrainRegionMaterialGroupsBindings bindings{};
        bindings.visibleClusters = builder.BindShaderResource(m_visibleClusterResource);
        if (m_reyesDiceQueueResource) {
            bindings.reyesDiceQueue = builder.BindShaderResource(m_reyesDiceQueueResource);
            bindings.hasReyesDiceQueue = true;
        }
        if (m_reyesTessTableConfigsResource && m_reyesTessTableVerticesResource && m_reyesTessTableTrianglesResource) {
            bindings.reyesTessTableConfigs = builder.BindShaderResource(m_reyesTessTableConfigsResource);
            bindings.reyesTessTableVertices = builder.BindShaderResource(m_reyesTessTableVerticesResource);
            bindings.reyesTessTableTriangles = builder.BindShaderResource(m_reyesTessTableTrianglesResource);
            bindings.hasReyesTessTables = true;
        }
        bindings.patchVisibilityIndexBase = m_patchVisibilityIndexBase;
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* b = &builder;

        if (m_slabResourceGroup) {
            b->WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
        }

        b->WithShaderResource(
            "Builtin::VisUtil::TerrainRegionPixelListBuffer",
            Builtin::PrimaryCamera::VisibilityTexture,
            Builtin::PrimaryCamera::LinearDepthMap,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerObjectBuffer,
            Builtin::PerMeshBuffer,
            Builtin::CameraBuffer,
            Builtin::NormalMatrixBuffer,
            Builtin::SkeletonResources::InverseBindMatrices,
            Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo,
            Builtin::PerMaterialDataBuffer,
            "Builtin::PerMaterialEvalDataBuffer",
            Builtin::Terrain::Sets,
            Builtin::Terrain::Layers,
            Builtin::Terrain::StochasticLayers,
            Builtin::Terrain::LayerRefs,
            Builtin::Terrain::Regions,
            Builtin::Terrain::WeightBlocks,
            Builtin::Terrain::TextureGroup,
            Builtin::Terrain::RvtInfo,
            Builtin::Terrain::RvtClipInfos,
            Builtin::Terrain::RvtPageTable,
            Builtin::Terrain::RvtPageKeys,
            Builtin::Terrain::RvtPhysicalPageOwner,
            Builtin::Terrain::RvtPhysicalPageAtlas,
            Builtin::Terrain::RvtHeightResidentCache,
            Builtin::Terrain::RvtHeightAtlas,
            Builtin::Terrain::RvtAlbedoAtlas,
            Builtin::Terrain::RvtNormalAtlas,
            Builtin::Terrain::RvtMaterialAtlas,
            Builtin::Material::TextureStreamingMetadataBuffer,
            Builtin::CLod::Offsets,
            Builtin::CLod::GroupChunks,
            Builtin::CLod::Groups,
            Builtin::CLod::GroupPageMap,
            Builtin::CLod::MeshMetadata,
            Builtin::CLod::AssemblyTransforms,
            Builtin::CLod::AssemblyBoneRemaps,
            Builtin::CLod::AssemblyBoneRemapIndices,
            Builtin::SkeletonResources::InverseSkinMatrices,
            Builtin::PerMaterialOpenPBRDataBuffer)
            .WithUnorderedAccess(
                Builtin::Surface::BaseColorOpacity,
                Builtin::Surface::NormalRoughness,
                Builtin::Surface::SpecularAo,
                Builtin::Surface::Emissive,
                Builtin::Surface::Motion,
                Builtin::Surface::Payload0,
                Builtin::Surface::Payload1,
				Builtin::Surface::Identity,
				Builtin::Surface::Records,
				Builtin::DebugVisualization,
                Builtin::Terrain::RvtRequestMasks,
                Builtin::Terrain::RvtRequestList,
                Builtin::Terrain::RvtCounters,
                Builtin::Terrain::RvtStats,
				Builtin::Material::TextureStreamingFeedbackBuffer)
            .WithConstantBuffer(Builtin::PerFrameBuffer);
        b->WithIndirectArguments(
            "Builtin::IndirectCommandBuffers::TerrainRegionMaterialEvaluationCommandBuffer",
            "Builtin::VisUtil::TerrainRegionActiveCountBuffer");
        return bindings;
    }

    void Initialize() {
        m_terrainRegionEvalCmds = m_resourceRegistryView->RequestPtr<Resource>("Builtin::IndirectCommandBuffers::TerrainRegionMaterialEvaluationCommandBuffer");
        m_activeCount = m_resourceRegistryView->RequestPtr<Resource>("Builtin::VisUtil::TerrainRegionActiveCountBuffer");
    }


    br::render::PreparedComputeIndirect BuildRecipe(const EvaluateTerrainRegionMaterialGroupsBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        const auto& ctx = *preparation.preparationData->Get<UpdateContext>();
        br::render::PreparedComputeIndirect data{};
        data.resourceHeap = ctx.textureDescriptorHeap.GetHandle();
        data.samplerHeap = ctx.samplerDescriptorHeap.GetHandle();
        auto program = preparation.CaptureProgramBinding(m_pso);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);
        data.constants[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = preparation.ResolveView(
            bindings.visibleClusters, {org::BindlessViewKind::ShaderResource}).index;
        data.constants[VISBUF_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
        data.constants[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] = bindings.hasReyesDiceQueue
            ? preparation.ResolveView(bindings.reyesDiceQueue, {org::BindlessViewKind::ShaderResource}).index : 0xFFFFFFFFu;
        data.constants[VISBUF_REYES_PATCH_INDEX_BASE] = bindings.patchVisibilityIndexBase;
        data.constants[VISBUF_REYES_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = bindings.hasReyesTessTables
            ? preparation.ResolveView(bindings.reyesTessTableConfigs, {org::BindlessViewKind::ShaderResource}).index : 0xFFFFFFFFu;
        data.constants[VISBUF_REYES_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX] = bindings.hasReyesTessTables
            ? preparation.ResolveView(bindings.reyesTessTableVertices, {org::BindlessViewKind::ShaderResource}).index : 0xFFFFFFFFu;
        data.constants[VISBUF_REYES_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX] = bindings.hasReyesTessTables
            ? preparation.ResolveView(bindings.reyesTessTableTriangles, {org::BindlessViewKind::ShaderResource}).index : 0xFFFFFFFFu;
        data.constants[VISBUF_REYES_USE_NORMAL_MAPS] = CLodReyesUseNormalMaps() ? 1u : 0u;
        data.constants[VISBUF_REYES_TERRAIN_NORMAL_BLEND_AS_UINT] = std::bit_cast<uint32_t>(CLodReyesTerrainNormalBlend());
        data.constants[VISBUF_REYES_TERRAIN_NORMAL_MIP_BIAS] = CLodReyesTerrainNormalMipBias();
        data.constants[VISBUF_REYES_OBJECT_NORMAL_MAP_BLEND_AS_UINT] = std::bit_cast<uint32_t>(CLodReyesObjectNormalMapBlend());
        data.commandSignature = preparation.CaptureCommandSignature(CommandSignatureManager::GetInstance().CaptureTerrainRegionMaterialEvaluationCommandSignature());
        data.argumentsReference = preparation.CaptureResource(m_terrainRegionEvalCmds->GetGlobalResourceID());
        data.countBufferReference = preparation.CaptureResource(m_activeCount->GetGlobalResourceID());
        data.maximumCount = TerrainRegionMaterialEval::MaxTerrainRegions;
        return data;
    }

    std::vector<uint64_t> RecipeRevision(const org::PassPrepareContext& preparation) const { return TerrainRegionMaterialEval::RecipeRevision(m_pso, preparation); }
    org::EmptyPassFrameData PrepareInvocation(const br::render::PreparedComputeIndirect&, const EvaluateTerrainRegionMaterialGroupsBindings&, const org::PassPrepareContext&) const { return {}; }
    static void Record(const br::render::PreparedComputeIndirect& data, const org::EmptyPassFrameData&,
        org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeIndirect(data, recording);
    }

    void ShutdownPass() {
        m_slabResourceGroup.reset();
        m_terrainRegionEvalCmds = nullptr;
        m_activeCount = nullptr;
    }

private:
    PipelineState m_pso;
    std::shared_ptr<ResourceGroup> m_slabResourceGroup;
    std::shared_ptr<GloballyIndexedResource> m_visibleClusterResource;
    std::shared_ptr<GloballyIndexedResource> m_reyesDiceQueueResource;
    std::shared_ptr<GloballyIndexedResource> m_reyesTessTableConfigsResource;
    std::shared_ptr<GloballyIndexedResource> m_reyesTessTableVerticesResource;
    std::shared_ptr<GloballyIndexedResource> m_reyesTessTableTrianglesResource;
    uint32_t m_patchVisibilityIndexBase = 0u;
    Resource* m_terrainRegionEvalCmds = nullptr;
    Resource* m_activeCount = nullptr;
};
