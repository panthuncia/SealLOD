#pragma once

#include <bit>

#include "RenderPasses/Base/ComputePass.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/CommandSignatureManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Managers/MaterialManager.h"
#include "Managers/MeshManager.h"
#include "Render/RenderContext.h"
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
}

class TerrainRegionCounterResetPass : public ComputePass {
public:
    TerrainRegionCounterResetPass() {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/VisUtil.hlsl",
            L"ClearTerrainRegionCountersCS",
            {},
            "VisUtil_ClearTerrainRegionCountersPSO");
    }

    void DeclareResourceUsages(ComputePassBuilder* b) override {
        b->WithUnorderedAccess(
            "Builtin::VisUtil::TerrainRegionPixelCountBuffer",
            "Builtin::VisUtil::TerrainRegionWriteCursorBuffer",
            "Builtin::VisUtil::TerrainRegionActiveCountBuffer");
    }

    void Setup() override {}

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& ctx = *renderContext;
        auto& cl = executionContext.commandList;
        auto& pm = PSOManager::GetInstance();
        cl.SetDescriptorHeaps(ctx.textureDescriptorHeap.GetHandle(), ctx.samplerDescriptorHeap.GetHandle());
        cl.BindLayout(pm.GetComputeRootSignature().GetHandle());
        cl.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
        BindResourceDescriptorIndices(cl, m_pso.GetResourceDescriptorSlots());
        uint32_t rc[NumMiscUintRootConstants] = {};
        rc[0] = TerrainRegionMaterialEval::MaxTerrainRegions;
        cl.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, rc);
        cl.Dispatch((TerrainRegionMaterialEval::MaxTerrainRegions + 63u) / 64u, 1u, 1u);
        return {};
    }

    void Cleanup() override {}

private:
    PipelineState m_pso;
};

class TerrainRegionMaterialRangePassBase : public ComputePass {
protected:
    explicit TerrainRegionMaterialRangePassBase(const wchar_t* entryPoint, const char* debugName) {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/VisUtil.hlsl",
            entryPoint,
            {},
            debugName);
    }

    void Setup() override {
        m_materialEvalCmds = m_resourceRegistryView->RequestPtr<Resource>("Builtin::IndirectCommandBuffers::MaterialEvaluationCommandBuffer");
    }

    void Cleanup() override {
        m_materialEvalCmds = nullptr;
    }

    void ExecuteTerrainRanges(PassExecutionContext& executionContext) {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& ctx = *renderContext;
        auto& cl = executionContext.commandList;
        auto& pm = PSOManager::GetInstance();
        const auto& sig = CommandSignatureManager::GetInstance().GetMaterialEvaluationCommandSignature();
        const uint64_t stride = sizeof(MaterialEvaluationIndirectCommand);
        auto argBuf = m_materialEvalCmds->GetAPIResource();

        cl.SetDescriptorHeaps(ctx.textureDescriptorHeap.GetHandle(), ctx.samplerDescriptorHeap.GetHandle());
        cl.BindLayout(pm.GetComputeRootSignature().GetHandle());
        cl.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
        BindResourceDescriptorIndices(cl, m_pso.GetResourceDescriptorSlots());

        uint32_t rc[NumMiscUintRootConstants] = {};
        rc[0] = TerrainRegionMaterialEval::TerrainSetIndexV1;
        cl.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, rc);

        for (MaterialCompileFlags flags : ctx.materialManager->GetActiveCompileFlags()) {
            if (!TerrainRegionMaterialEval::IsTerrainMaterialFlags(flags)) {
                continue;
            }
            uint32_t slot = 0u;
            if (!ctx.materialManager->TryGetCompileFlagsSlot(flags, slot) ||
                slot >= ctx.materialManager->GetCompileFlagsSlotsUsed()) {
                continue;
            }
            const uint64_t argOffset = static_cast<uint64_t>(slot) * stride;
            cl.ExecuteIndirect(sig.GetHandle(), argBuf.GetHandle(), argOffset, rhi::ResourceHandle{}, 0, 1);
        }
    }

    Resource* m_materialEvalCmds = nullptr;
    PipelineState m_pso;
};

class TerrainRegionHistogramPass : public TerrainRegionMaterialRangePassBase {
public:
    TerrainRegionHistogramPass()
        : TerrainRegionMaterialRangePassBase(L"TerrainRegionHistogramFromMaterialRangeCS", "VisUtil_TerrainRegionHistogramPSO") {}

    void DeclareResourceUsages(ComputePassBuilder* b) override {
        b->WithShaderResource(
            "Builtin::VisUtil::PixelListBuffer",
            Builtin::PrimaryCamera::VisibilityTexture,
            Builtin::CameraBuffer,
            Builtin::Terrain::Sets)
            .WithUnorderedAccess(
                "Builtin::VisUtil::TerrainRegionPixelCountBuffer",
                "Builtin::VisUtil::TerrainRegionActiveListBuffer",
                "Builtin::VisUtil::TerrainRegionActiveCountBuffer")
            .WithIndirectArguments("Builtin::IndirectCommandBuffers::MaterialEvaluationCommandBuffer")
            .WithConstantBuffer(Builtin::PerFrameBuffer);
    }

    PassReturn Execute(PassExecutionContext& executionContext) override {
        ExecuteTerrainRanges(executionContext);
        return {};
    }
};

class TerrainRegionBlockScanPass : public ComputePass {
public:
    TerrainRegionBlockScanPass() {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/materialPrefixSum.hlsl",
            L"TerrainRegionBlockScanCS",
            {},
            "VisUtil_TerrainRegionBlockScanPSO");
    }

    void DeclareResourceUsages(ComputePassBuilder* b) override {
        b->WithShaderResource("Builtin::VisUtil::TerrainRegionPixelCountBuffer")
            .WithUnorderedAccess(
                "Builtin::VisUtil::TerrainRegionOffsetBuffer",
                "Builtin::VisUtil::TerrainRegionBlockSumsBuffer");
    }

    void Setup() override {}

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& ctx = *renderContext;
        auto& cl = executionContext.commandList;
        auto& pm = PSOManager::GetInstance();
        cl.SetDescriptorHeaps(ctx.textureDescriptorHeap.GetHandle(), ctx.samplerDescriptorHeap.GetHandle());
        cl.BindLayout(pm.GetComputeRootSignature().GetHandle());
        cl.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
        BindResourceDescriptorIndices(cl, m_pso.GetResourceDescriptorSlots());
        uint32_t rc[NumMiscUintRootConstants] = {};
        rc[0] = TerrainRegionMaterialEval::MaxTerrainRegions;
        cl.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, rc);
        cl.Dispatch((TerrainRegionMaterialEval::MaxTerrainRegions + TerrainRegionMaterialEval::PrefixBlockSize - 1u) / TerrainRegionMaterialEval::PrefixBlockSize, 1u, 1u);
        return {};
    }

    void Cleanup() override {}

private:
    PipelineState m_pso;
};

class TerrainRegionBlockOffsetsPass : public ComputePass {
public:
    TerrainRegionBlockOffsetsPass() {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/materialPrefixSum.hlsl",
            L"TerrainRegionBlockOffsetsCS",
            {},
            "VisUtil_TerrainRegionBlockOffsetsPSO");
    }

    void DeclareResourceUsages(ComputePassBuilder* b) override {
        b->WithShaderResource(
            "Builtin::VisUtil::TerrainRegionPixelCountBuffer",
            "Builtin::VisUtil::TerrainRegionBlockSumsBuffer")
            .WithUnorderedAccess(
                "Builtin::VisUtil::TerrainRegionOffsetBuffer",
                "Builtin::VisUtil::TerrainRegionScannedBlockSumsBuffer",
                "Builtin::VisUtil::TerrainRegionTotalPixelCountBuffer");
    }

    void Setup() override {}

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& ctx = *renderContext;
        auto& cl = executionContext.commandList;
        auto& pm = PSOManager::GetInstance();
        cl.SetDescriptorHeaps(ctx.textureDescriptorHeap.GetHandle(), ctx.samplerDescriptorHeap.GetHandle());
        cl.BindLayout(pm.GetComputeRootSignature().GetHandle());
        cl.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
        BindResourceDescriptorIndices(cl, m_pso.GetResourceDescriptorSlots());
        uint32_t rc[NumMiscUintRootConstants] = {};
        rc[0] = TerrainRegionMaterialEval::MaxTerrainRegions;
        rc[1] = (TerrainRegionMaterialEval::MaxTerrainRegions + TerrainRegionMaterialEval::PrefixBlockSize - 1u) / TerrainRegionMaterialEval::PrefixBlockSize;
        cl.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, rc);
        cl.Dispatch(1u, 1u, 1u);
        return {};
    }

    void Cleanup() override {}

private:
    PipelineState m_pso;
};

class TerrainRegionPixelListPass : public TerrainRegionMaterialRangePassBase {
public:
    TerrainRegionPixelListPass()
        : TerrainRegionMaterialRangePassBase(L"TerrainRegionListFromMaterialRangeCS", "VisUtil_TerrainRegionPixelListPSO") {}

    void DeclareResourceUsages(ComputePassBuilder* b) override {
        b->WithShaderResource(
            "Builtin::VisUtil::PixelListBuffer",
            Builtin::PrimaryCamera::VisibilityTexture,
            Builtin::CameraBuffer,
            Builtin::Terrain::Sets,
            "Builtin::VisUtil::TerrainRegionOffsetBuffer")
            .WithUnorderedAccess(
                "Builtin::VisUtil::TerrainRegionWriteCursorBuffer",
                "Builtin::VisUtil::TerrainRegionPixelListBuffer")
            .WithIndirectArguments("Builtin::IndirectCommandBuffers::MaterialEvaluationCommandBuffer")
            .WithConstantBuffer(Builtin::PerFrameBuffer);
    }

    PassReturn Execute(PassExecutionContext& executionContext) override {
        ExecuteTerrainRanges(executionContext);
        return {};
    }
};

class BuildTerrainRegionMaterialIndirectCommandBuildDispatchArgsPass : public ComputePass {
public:
    BuildTerrainRegionMaterialIndirectCommandBuildDispatchArgsPass() {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/VisUtil.hlsl",
            L"BuildTerrainRegionCommandBuildDispatchArgsCS",
            {},
            "VisUtil_BuildTerrainRegionCommandBuildDispatchArgsPSO");
    }

    void DeclareResourceUsages(ComputePassBuilder* b) override {
        b->WithShaderResource("Builtin::VisUtil::TerrainRegionActiveCountBuffer")
            .WithUnorderedAccess("Builtin::IndirectCommandBuffers::TerrainRegionMaterialEvaluationCommandBuildDispatchArgsBuffer");
    }

    void Setup() override {}

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& ctx = *renderContext;
        auto& cl = executionContext.commandList;
        auto& pm = PSOManager::GetInstance();
        cl.SetDescriptorHeaps(ctx.textureDescriptorHeap.GetHandle(), ctx.samplerDescriptorHeap.GetHandle());
        cl.BindLayout(pm.GetComputeRootSignature().GetHandle());
        cl.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
        BindResourceDescriptorIndices(cl, m_pso.GetResourceDescriptorSlots());
        cl.Dispatch(1u, 1u, 1u);
        return {};
    }

    void Cleanup() override {}

private:
    PipelineState m_pso;
};

class BuildTerrainRegionMaterialIndirectCommandBufferPass : public ComputePass {
public:
    BuildTerrainRegionMaterialIndirectCommandBufferPass() {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/VisUtil.hlsl",
            L"BuildTerrainRegionEvaluateIndirectArgsCS",
            {},
            "VisUtil_BuildTerrainRegionEvaluateIndirectArgsPSO");
    }

    void DeclareResourceUsages(ComputePassBuilder* b) override {
        b->WithShaderResource(
            "Builtin::VisUtil::TerrainRegionActiveCountBuffer",
            "Builtin::VisUtil::TerrainRegionActiveListBuffer",
            "Builtin::VisUtil::TerrainRegionPixelCountBuffer",
            "Builtin::VisUtil::TerrainRegionOffsetBuffer")
            .WithUnorderedAccess("Builtin::IndirectCommandBuffers::TerrainRegionMaterialEvaluationCommandBuffer")
            .WithIndirectArguments("Builtin::IndirectCommandBuffers::TerrainRegionMaterialEvaluationCommandBuildDispatchArgsBuffer");
    }

    void Setup() override {
        m_dispatchArgs = m_resourceRegistryView->RequestPtr<Resource>("Builtin::IndirectCommandBuffers::TerrainRegionMaterialEvaluationCommandBuildDispatchArgsBuffer");
    }

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& ctx = *renderContext;
        auto& cl = executionContext.commandList;
        auto& pm = PSOManager::GetInstance();
        cl.SetDescriptorHeaps(ctx.textureDescriptorHeap.GetHandle(), ctx.samplerDescriptorHeap.GetHandle());
        cl.BindLayout(pm.GetComputeRootSignature().GetHandle());
        cl.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
        BindResourceDescriptorIndices(cl, m_pso.GetResourceDescriptorSlots());
        uint32_t rc[NumMiscUintRootConstants] = {};
        rc[0] = TerrainRegionMaterialEval::MaxTerrainRegions;
        rc[1] = TerrainRegionMaterialEval::TerrainSetIndexV1;
        cl.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, rc);
        const auto& sig = CommandSignatureManager::GetInstance().GetRawDispatchCommandSignature();
        cl.ExecuteIndirect(
            sig.GetHandle(),
            m_dispatchArgs->GetAPIResource().GetHandle(),
            0,
            rhi::ResourceHandle{},
            0,
            1);
        return {};
    }

    void Cleanup() override {
        m_dispatchArgs = nullptr;
    }

private:
    PipelineState m_pso;
    Resource* m_dispatchArgs = nullptr;
};

class EvaluateTerrainRegionMaterialGroupsPass : public ComputePass {
public:
    EvaluateTerrainRegionMaterialGroupsPass() {
        std::vector<DxcDefine> defines;
        defines.push_back({ L"PSO_TERRAIN", L"1" });
        defines.push_back({ L"VISUTIL_SPECIALIZED_MATERIAL_EVAL", L"1" });
        defines.push_back({ L"VISUTIL_USE_COMPACT_MATERIAL_EVAL", L"1" });
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/VisUtilEvaluateTerrainRegion.hlsl",
            L"EvaluateTerrainRegionMaterialGroupCS",
            std::move(defines),
            "VisUtil_EvaluateTerrainRegionMaterialGroupPSO");

        auto& ecsWorld = RendererECSManager::GetInstance().GetWorld();
        auto visBufferTag = ecsWorld.component<CLodExtensionVisibilityBufferTag>();
        m_visibleClustersQuery =
            ecsWorld.query_builder<>()
            .with<CLodExtensionTypeTag>(visBufferTag)
            .with<VisibleClustersBufferTag>()
            .build();
        m_reyesDiceQueueQuery =
            ecsWorld.query_builder<>()
            .with<CLodExtensionTypeTag>(visBufferTag)
            .with<CLodReyesDiceQueueTag>()
            .build();
        m_reyesTessTableConfigsQuery =
            ecsWorld.query_builder<>()
            .with<CLodExtensionTypeTag>(visBufferTag)
            .with<CLodReyesTessTableConfigsTag>()
            .build();
        m_reyesTessTableVerticesQuery =
            ecsWorld.query_builder<>()
            .with<CLodExtensionTypeTag>(visBufferTag)
            .with<CLodReyesTessTableVerticesTag>()
            .build();
        m_reyesTessTableTrianglesQuery =
            ecsWorld.query_builder<>()
            .with<CLodExtensionTypeTag>(visBufferTag)
            .with<CLodReyesTessTableTrianglesTag>()
            .build();

        try {
            auto getter = SettingsManager::GetInstance().getSettingGetter<std::function<MeshManager*()>>(CLodStreamingMeshManagerGetterSettingName);
            if (auto* mm = getter()()) {
                if (auto* pool = mm->GetCLodPagePool()) {
                    m_slabResourceGroup = pool->GetSlabResourceGroup();
                }
            }
        } catch (...) {}
    }

    void DeclareResourceUsages(ComputePassBuilder* b) override {
        b->WithShaderResource(ECSResourceResolver(m_visibleClustersQuery));
        b->WithShaderResource(ECSResourceResolver(m_reyesDiceQueueQuery));
        b->WithShaderResource(ECSResourceResolver(m_reyesTessTableConfigsQuery));
        b->WithShaderResource(ECSResourceResolver(m_reyesTessTableVerticesQuery));
        b->WithShaderResource(ECSResourceResolver(m_reyesTessTableTrianglesQuery));

        if (m_slabResourceGroup) {
            b->WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
        }

        b->WithShaderResource(
            "Builtin::VisUtil::TerrainRegionPixelListBuffer",
            Builtin::PrimaryCamera::VisibilityTexture,
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
            Builtin::Material::TextureGroup,
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
            Builtin::SkeletonResources::InverseSkinMatrices,
            Builtin::PerMaterialOpenPBRDataBuffer)
            .WithUnorderedAccess(
                Builtin::GBuffer::Normals,
                Builtin::GBuffer::Albedo,
                Builtin::GBuffer::Coat,
                Builtin::GBuffer::Emissive,
                Builtin::GBuffer::Fuzz,
                Builtin::GBuffer::MetallicRoughness,
                Builtin::GBuffer::MotionVectors,
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
    }

    void Setup() override {
        RefreshResourcePointers();
        RefreshDescriptorIndices();
        m_terrainRegionEvalCmds = m_resourceRegistryView->RequestPtr<Resource>("Builtin::IndirectCommandBuffers::TerrainRegionMaterialEvaluationCommandBuffer");
        m_activeCount = m_resourceRegistryView->RequestPtr<Resource>("Builtin::VisUtil::TerrainRegionActiveCountBuffer");
    }

    void RefreshResourcePointers() {
        std::vector<GloballyIndexedResource*> visibleClusterResources;
        m_visibleClustersQuery.each([&](flecs::entity e) {
            auto& res = e.get<Components::Resource>();
            auto test = std::static_pointer_cast<GloballyIndexedResource>(res.resource.lock());
            if (test) {
                visibleClusterResources.push_back(test.get());
            }
            const auto capacity = e.get<CLodVisibleClusterCapacity>();
            m_patchVisibilityIndexBase = CLodReyesPatchVisibilityIndexBase(capacity.maxVisibleClusters);
        });
        if (visibleClusterResources.size() != 1) {
            throw std::runtime_error("EvaluateTerrainRegionMaterialGroupsPass: Expected exactly one visible cluster buffer resource.");
        }

        m_visibleClusterResource = visibleClusterResources[0];
        m_reyesDiceQueueResource = nullptr;
        m_reyesTessTableConfigsResource = nullptr;
        m_reyesTessTableVerticesResource = nullptr;
        m_reyesTessTableTrianglesResource = nullptr;

        std::vector<GloballyIndexedResource*> reyesDiceQueueResources;
        m_reyesDiceQueueQuery.each([&](flecs::entity e) {
            if (const auto res = e.try_get<Components::Resource>(); res) {
                if (const auto test = std::static_pointer_cast<GloballyIndexedResource>(res->resource.lock()); test) {
                    reyesDiceQueueResources.push_back(test.get());
                }
            }
        });
        if (reyesDiceQueueResources.size() == 1) {
            m_reyesDiceQueueResource = reyesDiceQueueResources[0];
        }

        std::vector<GloballyIndexedResource*> reyesTessTableConfigResources;
        m_reyesTessTableConfigsQuery.each([&](flecs::entity e) {
            if (const auto res = e.try_get<Components::Resource>(); res) {
                if (const auto resource = std::static_pointer_cast<GloballyIndexedResource>(res->resource.lock()); resource) {
                    reyesTessTableConfigResources.push_back(resource.get());
                }
            }
        });
        if (reyesTessTableConfigResources.size() == 1) {
            m_reyesTessTableConfigsResource = reyesTessTableConfigResources[0];
        }

        std::vector<GloballyIndexedResource*> reyesTessTableVertexResources;
        m_reyesTessTableVerticesQuery.each([&](flecs::entity e) {
            if (const auto res = e.try_get<Components::Resource>(); res) {
                if (const auto resource = std::static_pointer_cast<GloballyIndexedResource>(res->resource.lock()); resource) {
                    reyesTessTableVertexResources.push_back(resource.get());
                }
            }
        });
        if (reyesTessTableVertexResources.size() == 1) {
            m_reyesTessTableVerticesResource = reyesTessTableVertexResources[0];
        }

        std::vector<GloballyIndexedResource*> reyesTessTableTriangleResources;
        m_reyesTessTableTrianglesQuery.each([&](flecs::entity e) {
            if (const auto res = e.try_get<Components::Resource>(); res) {
                if (const auto resource = std::static_pointer_cast<GloballyIndexedResource>(res->resource.lock()); resource) {
                    reyesTessTableTriangleResources.push_back(resource.get());
                }
            }
        });
        if (reyesTessTableTriangleResources.size() == 1) {
            m_reyesTessTableTrianglesResource = reyesTessTableTriangleResources[0];
        }
    }

    void RefreshDescriptorIndices() {
        if (m_visibleClusterResource) {
            m_visibleClusterBufferSRVIndex = m_visibleClusterResource->GetSRVInfo(0).slot.index;
        }
        m_reyesDiceQueueBufferSRVIndex = m_reyesDiceQueueResource
            ? m_reyesDiceQueueResource->GetSRVInfo(0).slot.index
            : 0xFFFFFFFFu;
        m_reyesTessTableConfigsBufferSRVIndex = m_reyesTessTableConfigsResource
            ? m_reyesTessTableConfigsResource->GetSRVInfo(0).slot.index
            : 0xFFFFFFFFu;
        m_reyesTessTableVerticesBufferSRVIndex = m_reyesTessTableVerticesResource
            ? m_reyesTessTableVerticesResource->GetSRVInfo(0).slot.index
            : 0xFFFFFFFFu;
        m_reyesTessTableTrianglesBufferSRVIndex = m_reyesTessTableTrianglesResource
            ? m_reyesTessTableTrianglesResource->GetSRVInfo(0).slot.index
            : 0xFFFFFFFFu;
    }

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& ctx = *renderContext;
        auto& cl = executionContext.commandList;
        auto& pm = PSOManager::GetInstance();
        const auto& sig = CommandSignatureManager::GetInstance().GetTerrainRegionMaterialEvaluationCommandSignature();

        cl.SetDescriptorHeaps(ctx.textureDescriptorHeap.GetHandle(), ctx.samplerDescriptorHeap.GetHandle());
        cl.BindLayout(pm.GetComputeRootSignature().GetHandle());
        cl.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
        BindResourceDescriptorIndices(cl, m_pso.GetResourceDescriptorSlots());
        RefreshDescriptorIndices();

        unsigned int miscRootConstants[NumMiscUintRootConstants] = {};
        miscRootConstants[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = m_visibleClusterBufferSRVIndex;
        miscRootConstants[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] = m_reyesDiceQueueBufferSRVIndex;
        miscRootConstants[VISBUF_REYES_PATCH_INDEX_BASE] = m_patchVisibilityIndexBase;
        miscRootConstants[VISBUF_REYES_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = m_reyesTessTableConfigsBufferSRVIndex;
        miscRootConstants[VISBUF_REYES_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX] = m_reyesTessTableVerticesBufferSRVIndex;
        miscRootConstants[VISBUF_REYES_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX] = m_reyesTessTableTrianglesBufferSRVIndex;
        miscRootConstants[VISBUF_REYES_USE_NORMAL_MAPS] = CLodReyesUseNormalMaps() ? 1u : 0u;
        miscRootConstants[VISBUF_REYES_TERRAIN_NORMAL_BLEND_AS_UINT] = std::bit_cast<uint32_t>(CLodReyesTerrainNormalBlend());
        miscRootConstants[VISBUF_REYES_TERRAIN_NORMAL_MIP_BIAS] = CLodReyesTerrainNormalMipBias();
        cl.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, miscRootConstants);

        cl.ExecuteIndirect(
            sig.GetHandle(),
            m_terrainRegionEvalCmds->GetAPIResource().GetHandle(),
            0,
            m_activeCount->GetAPIResource().GetHandle(),
            0,
            TerrainRegionMaterialEval::MaxTerrainRegions);
        return {};
    }

    void Cleanup() override {
        m_visibleClustersQuery = {};
        m_reyesDiceQueueQuery = {};
        m_reyesTessTableConfigsQuery = {};
        m_reyesTessTableVerticesQuery = {};
        m_reyesTessTableTrianglesQuery = {};
        m_slabResourceGroup.reset();
        m_terrainRegionEvalCmds = nullptr;
        m_activeCount = nullptr;
    }

private:
    PipelineState m_pso;
    flecs::query<> m_visibleClustersQuery;
    flecs::query<> m_reyesDiceQueueQuery;
    flecs::query<> m_reyesTessTableConfigsQuery;
    flecs::query<> m_reyesTessTableVerticesQuery;
    flecs::query<> m_reyesTessTableTrianglesQuery;
    std::shared_ptr<ResourceGroup> m_slabResourceGroup;
    GloballyIndexedResource* m_visibleClusterResource = nullptr;
    GloballyIndexedResource* m_reyesDiceQueueResource = nullptr;
    GloballyIndexedResource* m_reyesTessTableConfigsResource = nullptr;
    GloballyIndexedResource* m_reyesTessTableVerticesResource = nullptr;
    GloballyIndexedResource* m_reyesTessTableTrianglesResource = nullptr;
    uint32_t m_visibleClusterBufferSRVIndex = 0;
    uint32_t m_reyesDiceQueueBufferSRVIndex = 0xFFFFFFFFu;
    uint32_t m_patchVisibilityIndexBase = 0u;
    uint32_t m_reyesTessTableConfigsBufferSRVIndex = 0xFFFFFFFFu;
    uint32_t m_reyesTessTableVerticesBufferSRVIndex = 0xFFFFFFFFu;
    uint32_t m_reyesTessTableTrianglesBufferSRVIndex = 0xFFFFFFFFu;
    Resource* m_terrainRegionEvalCmds = nullptr;
    Resource* m_activeCount = nullptr;
};
