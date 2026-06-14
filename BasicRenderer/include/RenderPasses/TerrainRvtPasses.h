#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "BuiltinResources.h"
#include "Managers/MeshManager.h"
#include "Managers/Singletons/CommandSignatureManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/RendererECSManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Materials/TechniqueDescriptor.h"
#include "Render/GraphExtensions/CLodExtensionComponents.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "RenderPasses/Base/ComputePass.h"
#include "Resources/Buffers/PagePool.h"
#include "Resources/Resolvers/ECSResourceResolver.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "Resources/components.h"
#include "../shaders/PerPassRootConstants/visUtilRootConstants.h"

namespace TerrainRvt
{
    inline constexpr uint32_t CounterCount = 5u;
    inline constexpr uint32_t MaxDispatchGroupsX = 65535u;
    inline constexpr uint32_t PhysicalAtlasTextureSide = 16384u;
    inline constexpr float DefaultSourceTexelsPerWorld = 24.0f;
    inline constexpr float DefaultBasePageWorldSize = 128.0f / DefaultSourceTexelsPerWorld;

    inline uint32_t FloatBits(float value)
    {
        uint32_t bits = 0u;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    inline std::pair<uint32_t, uint32_t> Dispatch2DForItems(uint32_t itemCount, uint32_t threadsPerGroup)
    {
        const uint32_t groupCount = std::max(1u, (itemCount + threadsPerGroup - 1u) / threadsPerGroup);
        const uint32_t dispatchX = std::min(groupCount, MaxDispatchGroupsX);
        const uint32_t dispatchY = std::max(1u, (groupCount + dispatchX - 1u) / dispatchX);
        return { dispatchX, dispatchY };
    }

    inline uint32_t SettingU32(const char* name, uint32_t fallback)
    {
        try {
            return SettingsManager::GetInstance().getSettingGetter<uint32_t>(name)();
        }
        catch (...) {
            return fallback;
        }
    }

    inline float SettingFloat(const char* name, float fallback)
    {
        try {
            return SettingsManager::GetInstance().getSettingGetter<float>(name)();
        }
        catch (...) {
            return fallback;
        }
    }

    inline uint32_t PageSize()
    {
        return std::clamp(SettingU32("terrainRvtPageSize", 128u), 16u, 512u);
    }

    inline uint32_t BorderTexels()
    {
        return std::min(SettingU32("terrainRvtBorderTexels", 4u), 16u);
    }

    inline uint32_t AtlasPagesWide()
    {
        const uint32_t tileSide = PageSize() + BorderTexels() * 2u;
        return std::max(1u, PhysicalAtlasTextureSide / std::max(tileSide, 1u));
    }

    inline uint32_t AtlasPagesHigh()
    {
        return AtlasPagesWide();
    }

    inline uint32_t AtlasPoolCount()
    {
        return std::clamp(SettingU32("terrainRvtPhysicalAtlasPoolCount", 1u), 1u, 8u);
    }

    inline uint32_t ClipPageTableResolution()
    {
        return std::clamp(SettingU32("terrainRvtClipPageTableResolution", 128u), 16u, 512u);
    }

    inline uint32_t MaxTerrainSets()
    {
        return std::clamp(SettingU32("terrainRvtMaxTerrainSets", 8u), 1u, 16u);
    }

    inline uint32_t MaxClipLevels()
    {
        return std::clamp(SettingU32("terrainRvtMaxClipLevels", 24u), 1u, 24u);
    }

    inline uint32_t MipCount()
    {
        return std::clamp(SettingU32("terrainRvtMipCount", 14u), 1u, MaxClipLevels());
    }

    inline uint32_t MaxPageTableEntries()
    {
        const uint32_t resolution = ClipPageTableResolution();
        return resolution * resolution * MaxTerrainSets() * MaxClipLevels();
    }

    inline uint32_t MaxClipInfoCount()
    {
        return MaxTerrainSets() * MaxClipLevels();
    }

    inline uint32_t MaxPhysicalPages()
    {
        return AtlasPagesWide() * AtlasPagesHigh() * AtlasPoolCount();
    }

    inline float BasePageWorldSize()
    {
        return (std::max)(SettingFloat("terrainRvtBasePageWorldSize", DefaultBasePageWorldSize), 0.125f);
    }

    inline void FillInfoRootConstants(uint32_t* rootConstants)
    {
        const uint32_t pageSize = PageSize();
        const uint32_t border = BorderTexels();
        const uint32_t maxEntries = MaxPageTableEntries();
        rootConstants[0] = pageSize;
        rootConstants[1] = border;
        rootConstants[2] = AtlasPagesWide();
        rootConstants[3] = AtlasPagesHigh();
        rootConstants[4] = maxEntries;
        rootConstants[5] = maxEntries;
        rootConstants[6] = MaxPhysicalPages();
        rootConstants[7] = MipCount();
        rootConstants[8] = ClipPageTableResolution();
        rootConstants[9] = FloatBits(BasePageWorldSize());
        rootConstants[10] = MaxTerrainSets();
        rootConstants[11] = AtlasPoolCount();
        rootConstants[12] = MaxClipLevels();
        rootConstants[13] = MaxClipInfoCount();
    }
}

class TerrainRvtFrameResetPass final : public ComputePass {
public:
    TerrainRvtFrameResetPass()
    {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/TerrainRvt.hlsl",
            L"TerrainRvtFrameResetCS",
            {},
            "TerrainRvt.FrameReset.PSO");
    }

    void DeclareResourceUsages(ComputePassBuilder* b) override
    {
        b->WithUnorderedAccess(
            Builtin::Terrain::RvtInfo,
            Builtin::Terrain::RvtClipInfos,
            Builtin::Terrain::RvtPageTable,
            Builtin::Terrain::RvtPageKeys,
            Builtin::Terrain::RvtPhysicalPageOwner,
            Builtin::Terrain::RvtRequestMasks,
            Builtin::Terrain::RvtCounters,
            Builtin::Terrain::RvtStats)
            .WithShaderResource(Builtin::CameraBuffer, Builtin::Terrain::Sets)
            .WithConstantBuffer(Builtin::PerFrameBuffer);
    }

    void Setup() override {}

    PassReturn Execute(PassExecutionContext& executionContext) override
    {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& commandList = executionContext.commandList;
        commandList.SetDescriptorHeaps(renderContext->textureDescriptorHeap.GetHandle(), renderContext->samplerDescriptorHeap.GetHandle());
        commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
        commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
        BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());

        uint32_t rootConstants[NumMiscUintRootConstants] = {};
        TerrainRvt::FillInfoRootConstants(rootConstants);
        commandList.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, rootConstants);
        const uint32_t maxPageTableEntries = TerrainRvt::MaxPageTableEntries();
        const auto [dispatchX, dispatchY] = TerrainRvt::Dispatch2DForItems(TerrainRvt::MaxPageTableEntries(), 64u);
        static bool loggedDispatch = false;
        if (!loggedDispatch) {
            loggedDispatch = true;
            spdlog::info(
                "SARP terrain RVT dispatch: reset max_entries={} groups={}x{} covered_threads={} base_world={} clip_table={} max_sets={} max_clips={} addressing=direct_clipmaps",
                maxPageTableEntries,
                dispatchX,
                dispatchY,
                static_cast<uint64_t>(dispatchX) * dispatchY * 64ull,
                TerrainRvt::BasePageWorldSize(),
                TerrainRvt::ClipPageTableResolution(),
                TerrainRvt::MaxTerrainSets(),
                TerrainRvt::MaxClipLevels());
        }
        commandList.Dispatch(dispatchX, dispatchY, 1u);
        return {};
    }

    void Cleanup() override {}

private:
    PipelineState m_pso;
};

class TerrainRvtMarkVisibilityMaterialPagesPass final : public ComputePass {
public:
    TerrainRvtMarkVisibilityMaterialPagesPass()
    {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/TerrainRvt.hlsl",
            L"TerrainRvtMarkVisibleClusterPagesCS",
            {},
            "TerrainRvt.MarkVisibleClusterPages.PSO");

        auto& ecsWorld = RendererECSManager::GetInstance().GetWorld();
        auto visBufferTag = ecsWorld.component<CLodExtensionVisibilityBufferTag>();
        m_visibleClustersQuery = ecsWorld.query_builder<>()
            .with<CLodExtensionTypeTag>(visBufferTag)
            .with<VisibleClustersBufferTag>()
            .build();
        m_visibleClustersCounterQuery = ecsWorld.query_builder<>()
            .with<CLodExtensionTypeTag>(visBufferTag)
            .with<VisibleClustersCounterTag>()
            .build();
        try {
            auto getter = SettingsManager::GetInstance().getSettingGetter<std::function<MeshManager*()>>(CLodStreamingMeshManagerGetterSettingName);
            if (auto* meshManager = getter()()) {
                if (auto* pool = meshManager->GetCLodPagePool()) {
                    m_slabResourceGroup = pool->GetSlabResourceGroup();
                }
            }
        } catch (...) {}
    }

    void DeclareResourceUsages(ComputePassBuilder* b) override
    {
        b->WithShaderResource(ECSResourceResolver(m_visibleClustersQuery));
        b->WithShaderResource(ECSResourceResolver(m_visibleClustersCounterQuery));
        if (m_slabResourceGroup) {
            b->WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
        }
        b->WithShaderResource(
            Builtin::CameraBuffer,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerMeshBuffer,
            "Builtin::PerMaterialEvalDataBuffer",
            Builtin::Terrain::Sets,
            Builtin::Terrain::RvtInfo,
            Builtin::Terrain::RvtClipInfos,
            Builtin::Terrain::RvtPageTable,
            Builtin::Terrain::RvtPageKeys,
            Builtin::Terrain::RvtPhysicalPageOwner)
            .WithUnorderedAccess(
                Builtin::Terrain::RvtRequestMasks,
                Builtin::Terrain::RvtRequestList,
                Builtin::Terrain::RvtCounters,
                Builtin::Terrain::RvtStats)
            .WithConstantBuffer(Builtin::PerFrameBuffer);
    }

    void Setup() override
    {
        RefreshResourcePointers();
    }

    PassReturn Execute(PassExecutionContext& executionContext) override
    {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& context = *renderContext;
        auto& commandList = executionContext.commandList;
        RefreshResourcePointers();

        commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
        commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
        commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
        BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());

        uint32_t rootConstants[NumMiscUintRootConstants] = {};
        rootConstants[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = m_visibleClusterSRVIndex;
        rootConstants[VISBUF_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = m_visibleClusterCounterSRVIndex;
        commandList.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, rootConstants);
        commandList.Dispatch((std::max(m_visibleClusterCapacity, 1u) + 63u) / 64u, 1u, 1u);
        return {};
    }

    void Cleanup() override
    {
        m_visibleClustersQuery = {};
        m_visibleClustersCounterQuery = {};
        m_slabResourceGroup.reset();
    }

private:
    void RefreshResourcePointers()
    {
        m_visibleClusterSRVIndex = 0xFFFFFFFFu;
        m_visibleClusterCounterSRVIndex = 0xFFFFFFFFu;
        m_visibleClusterCapacity = 0u;
        m_visibleClustersQuery.each([&](flecs::entity e) {
            auto& res = e.get<Components::Resource>();
            if (const auto resource = std::static_pointer_cast<GloballyIndexedResource>(res.resource.lock()); resource) {
                m_visibleClusterSRVIndex = resource->GetSRVInfo(0).slot.index;
            }
            const auto capacity = e.get<CLodVisibleClusterCapacity>();
            m_visibleClusterCapacity = capacity.maxVisibleClusters;
        });
        m_visibleClustersCounterQuery.each([&](flecs::entity e) {
            if (const auto res = e.try_get<Components::Resource>(); res) {
                if (const auto resource = std::static_pointer_cast<GloballyIndexedResource>(res->resource.lock()); resource) {
                    m_visibleClusterCounterSRVIndex = resource->GetSRVInfo(0).slot.index;
                }
            }
        });
    }

    PipelineState m_pso;
    flecs::query<> m_visibleClustersQuery;
    flecs::query<> m_visibleClustersCounterQuery;
    std::shared_ptr<ResourceGroup> m_slabResourceGroup;
    uint32_t m_visibleClusterSRVIndex = 0xFFFFFFFFu;
    uint32_t m_visibleClusterCounterSRVIndex = 0xFFFFFFFFu;
    uint32_t m_visibleClusterCapacity = 0u;
};

class TerrainRvtResolveRequestsPass final : public ComputePass {
public:
    TerrainRvtResolveRequestsPass()
    {
        auto& psoManager = PSOManager::GetInstance();
        m_clearPso = psoManager.MakeComputePipeline(
            psoManager.GetComputeRootSignature().GetHandle(),
            L"shaders/TerrainRvt.hlsl",
            L"TerrainRvtClearGenerationCounterCS",
            {},
            "TerrainRvt.ClearGenerationCounter.PSO");
        m_resolvePso = psoManager.MakeComputePipeline(
            psoManager.GetComputeRootSignature().GetHandle(),
            L"shaders/TerrainRvt.hlsl",
            L"TerrainRvtResolveRequestsCS",
            {},
            "TerrainRvt.ResolveRequests.PSO");
    }

    void DeclareResourceUsages(ComputePassBuilder* b) override
    {
        b->WithShaderResource(
            Builtin::Terrain::RvtInfo,
            Builtin::Terrain::RvtRequestList,
            Builtin::Terrain::RvtRequestMasks,
            Builtin::Terrain::RvtPageKeys)
            .WithUnorderedAccess(
                Builtin::Terrain::RvtCounters,
                Builtin::Terrain::RvtPageTable,
                Builtin::Terrain::RvtPhysicalPageOwner,
                Builtin::Terrain::RvtGenerationList,
                Builtin::Terrain::RvtStats)
            .WithConstantBuffer(Builtin::PerFrameBuffer);
    }

    void Setup() override {}

    PassReturn Execute(PassExecutionContext& executionContext) override
    {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& commandList = executionContext.commandList;
        commandList.SetDescriptorHeaps(renderContext->textureDescriptorHeap.GetHandle(), renderContext->samplerDescriptorHeap.GetHandle());
        commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());

        commandList.BindPipeline(m_clearPso.GetAPIPipelineState().GetHandle());
        BindResourceDescriptorIndices(commandList, m_clearPso.GetResourceDescriptorSlots());
        const uint32_t maxPageTableEntries = TerrainRvt::MaxPageTableEntries();
        const auto [dispatchX, dispatchY] = TerrainRvt::Dispatch2DForItems(maxPageTableEntries, 64u);
        commandList.Dispatch(1u, 1u, 1u);

        commandList.BindPipeline(m_resolvePso.GetAPIPipelineState().GetHandle());
        BindResourceDescriptorIndices(commandList, m_resolvePso.GetResourceDescriptorSlots());
        static bool loggedDispatch = false;
        if (!loggedDispatch) {
            loggedDispatch = true;
            spdlog::info(
                "SARP terrain RVT dispatch: resolve max_entries={} groups={}x{} covered_threads={}",
                maxPageTableEntries,
                dispatchX,
                dispatchY,
                static_cast<uint64_t>(dispatchX) * dispatchY * 64ull);
        }
        commandList.Dispatch(dispatchX, dispatchY, 1u);
        return {};
    }

    void Cleanup() override {}

private:
    PipelineState m_clearPso;
    PipelineState m_resolvePso;
};

class TerrainRvtClearFeedbackRequestsPass final : public ComputePass {
public:
    TerrainRvtClearFeedbackRequestsPass()
    {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/TerrainRvt.hlsl",
            L"TerrainRvtClearFeedbackRequestsCS",
            {},
            "TerrainRvt.ClearFeedbackRequests.PSO");
    }

    void DeclareResourceUsages(ComputePassBuilder* b) override
    {
        b->WithShaderResource(Builtin::Terrain::RvtInfo)
            .WithUnorderedAccess(
                Builtin::Terrain::RvtRequestMasks,
                Builtin::Terrain::RvtCounters);
    }

    void Setup() override {}

    PassReturn Execute(PassExecutionContext& executionContext) override
    {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& commandList = executionContext.commandList;
        commandList.SetDescriptorHeaps(renderContext->textureDescriptorHeap.GetHandle(), renderContext->samplerDescriptorHeap.GetHandle());
        commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
        commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
        BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());
        const auto [dispatchX, dispatchY] = TerrainRvt::Dispatch2DForItems(TerrainRvt::MaxPageTableEntries(), 64u);
        commandList.Dispatch(dispatchX, dispatchY, 1u);
        return {};
    }

    void Cleanup() override {}

private:
    PipelineState m_pso;
};

class TerrainRvtBuildGenerateDispatchArgsPass final : public ComputePass {
public:
    TerrainRvtBuildGenerateDispatchArgsPass()
    {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/TerrainRvt.hlsl",
            L"TerrainRvtBuildGenerateDispatchArgsCS",
            {},
            "TerrainRvt.BuildGenerateDispatchArgs.PSO");
    }

    void DeclareResourceUsages(ComputePassBuilder* b) override
    {
        b->WithShaderResource(Builtin::Terrain::RvtInfo, Builtin::Terrain::RvtCounters)
            .WithUnorderedAccess(Builtin::Terrain::RvtGenerateDispatchArgs);
    }

    void Setup() override {}

    PassReturn Execute(PassExecutionContext& executionContext) override
    {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& commandList = executionContext.commandList;
        commandList.SetDescriptorHeaps(renderContext->textureDescriptorHeap.GetHandle(), renderContext->samplerDescriptorHeap.GetHandle());
        commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
        commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
        BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());
        commandList.Dispatch(1u, 1u, 1u);
        return {};
    }

    void Cleanup() override {}

private:
    PipelineState m_pso;
};

class TerrainRvtGeneratePagesPass final : public ComputePass {
public:
    TerrainRvtGeneratePagesPass()
    {
        std::vector<DxcDefine> defines;
        defines.push_back({ L"PSO_TEXTURE_STREAMING", L"1" });
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/TerrainRvt.hlsl",
            L"TerrainRvtGeneratePagesCS",
            std::move(defines),
            "TerrainRvt.GeneratePages.PSO");
    }

    void DeclareResourceUsages(ComputePassBuilder* b) override
    {
        b->WithShaderResource(
            Builtin::CameraBuffer,
            Builtin::Terrain::RvtInfo,
            Builtin::Terrain::RvtCounters,
            Builtin::Terrain::RvtGenerationList,
            Builtin::Terrain::Sets,
            Builtin::Terrain::Layers,
            Builtin::Terrain::StochasticLayers,
            Builtin::Terrain::LayerRefs,
            Builtin::Terrain::Regions,
            Builtin::Terrain::WeightBlocks,
            Builtin::Terrain::TextureGroup,
            Builtin::Material::TextureStreamingMetadataBuffer)
            .WithUnorderedAccess(
                Builtin::Terrain::RvtHeightAtlas,
                Builtin::Terrain::RvtAlbedoAtlas,
                Builtin::Terrain::RvtNormalAtlas,
                Builtin::Terrain::RvtMaterialAtlas,
                Builtin::Terrain::RvtStats,
                Builtin::Material::TextureStreamingFeedbackBuffer)
            .WithIndirectArguments(Builtin::Terrain::RvtGenerateDispatchArgs)
            .WithConstantBuffer(Builtin::PerFrameBuffer);
    }

    void Setup() override
    {
        m_argsBuffer = m_resourceRegistryView->RequestPtr<Resource>(Builtin::Terrain::RvtGenerateDispatchArgs);
    }

    PassReturn Execute(PassExecutionContext& executionContext) override
    {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& commandList = executionContext.commandList;
        commandList.SetDescriptorHeaps(renderContext->textureDescriptorHeap.GetHandle(), renderContext->samplerDescriptorHeap.GetHandle());
        commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
        commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
        BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());
        if (m_argsBuffer) {
            commandList.ExecuteIndirect(
                CommandSignatureManager::GetInstance().GetRawDispatchCommandSignature().GetHandle(),
                m_argsBuffer->GetAPIResource().GetHandle(),
                0,
                {},
                0,
                1);
        }

        return {};
    }

    void Cleanup() override
    {
        m_argsBuffer = nullptr;
    }

private:
    PipelineState m_pso;
    Resource* m_argsBuffer = nullptr;
};

class TerrainRvtFinalizeGeneratedPagesPass final : public ComputePass {
public:
    TerrainRvtFinalizeGeneratedPagesPass()
    {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/TerrainRvt.hlsl",
            L"TerrainRvtFinalizeGeneratedPagesCS",
            {},
            "TerrainRvt.FinalizeGeneratedPages.PSO");
    }

    void DeclareResourceUsages(ComputePassBuilder* b) override
    {
        b->WithShaderResource(
            Builtin::Terrain::RvtInfo,
            Builtin::Terrain::RvtCounters,
            Builtin::Terrain::RvtGenerationList,
            Builtin::Terrain::RvtPageKeys)
            .WithUnorderedAccess(
                Builtin::Terrain::RvtPageTable,
                Builtin::Terrain::RvtPhysicalPageOwner)
            .WithConstantBuffer(Builtin::PerFrameBuffer);
    }

    void Setup() override {}

    PassReturn Execute(PassExecutionContext& executionContext) override
    {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& commandList = executionContext.commandList;
        commandList.SetDescriptorHeaps(renderContext->textureDescriptorHeap.GetHandle(), renderContext->samplerDescriptorHeap.GetHandle());
        commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
        commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
        BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());
        const auto [dispatchX, dispatchY] = TerrainRvt::Dispatch2DForItems(TerrainRvt::MaxPhysicalPages(), 64u);
        commandList.Dispatch(dispatchX, dispatchY, 1u);

        rhi::GlobalBarrier publishedPagesVisible{};
        publishedPagesVisible.beforeSync = rhi::ResourceSyncState::ComputeShading;
        publishedPagesVisible.afterSync = rhi::ResourceSyncState::ComputeShading;
        publishedPagesVisible.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
        publishedPagesVisible.afterAccess = rhi::ResourceAccessType::ShaderResource;
        rhi::BarrierBatch barrierBatch{};
        barrierBatch.globals = rhi::Span<rhi::GlobalBarrier>(&publishedPagesVisible, 1u);
        commandList.Barriers(barrierBatch);
        return {};
    }

    void Cleanup() override {}

private:
    PipelineState m_pso;
};
