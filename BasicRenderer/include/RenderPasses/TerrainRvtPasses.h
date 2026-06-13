#pragma once

#include <algorithm>
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
    inline constexpr uint32_t CounterCount = 4u;
    inline constexpr uint32_t MaxDispatchGroupsX = 65535u;
    inline constexpr uint32_t PhysicalAtlasTextureSide = 16384u;
    inline constexpr float DefaultBasePageWorldSize = 128.0f;

    inline uint32_t FloatBits(float value)
    {
        uint32_t bits = 0u;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    inline uint32_t MipAxis(uint32_t maxAxis, uint32_t mip)
    {
        return std::max(1u, maxAxis >> std::min(mip, 31u));
    }

    inline uint32_t PageTableEntryCount(uint32_t maxAxis, uint32_t mipCount)
    {
        uint32_t total = 0u;
        for (uint32_t mip = 0u; mip < std::max(1u, mipCount); ++mip) {
            const uint32_t axis = MipAxis(std::max(1u, maxAxis), mip);
            total += axis * axis;
        }
        return total;
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

    inline uint32_t MaxVirtualPagesPerAxis()
    {
        return std::clamp(SettingU32("terrainRvtMaxVirtualPagesPerAxis", 4096u), 1u, 4096u);
    }

    inline uint32_t MipCount()
    {
        return std::clamp(SettingU32("terrainRvtMipCount", 10u), 1u, 16u);
    }

    inline uint32_t MaxPageTableEntries()
    {
        return PageTableEntryCount(MaxVirtualPagesPerAxis(), MipCount());
    }

    inline uint32_t MaxPhysicalPages()
    {
        return AtlasPagesWide() * AtlasPagesHigh() * AtlasPoolCount();
    }

    inline uint32_t BasePageWorldSize()
    {
        return (std::max)(SettingU32("terrainRvtBasePageWorldSize", static_cast<uint32_t>(DefaultBasePageWorldSize)), 1u);
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
        rootConstants[8] = MaxVirtualPagesPerAxis();
        rootConstants[9] = FloatBits(static_cast<float>(BasePageWorldSize()));
        rootConstants[11] = AtlasPoolCount();
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
            Builtin::Terrain::RvtPageTable,
            Builtin::Terrain::RvtPhysicalPageOwner,
            Builtin::Terrain::RvtRequestMasks,
            Builtin::Terrain::RvtCounters,
            Builtin::Terrain::RvtStats);
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
        rootConstants[10] = 1u;
        commandList.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, rootConstants);
        const uint32_t maxPageTableEntries = TerrainRvt::MaxPageTableEntries();
        const auto [dispatchX, dispatchY] = TerrainRvt::Dispatch2DForItems(TerrainRvt::MaxPageTableEntries(), 64u);
        static bool loggedDispatch = false;
        if (!loggedDispatch) {
            loggedDispatch = true;
            spdlog::info(
                "SARP terrain RVT dispatch: reset max_entries={} groups={}x{} covered_threads={}",
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
            Builtin::Terrain::RvtInfo)
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
            Builtin::Terrain::RvtRequestMasks)
            .WithUnorderedAccess(
                Builtin::Terrain::RvtCounters,
                Builtin::Terrain::RvtPageTable,
                Builtin::Terrain::RvtGenerationList,
                Builtin::Terrain::RvtStats)
            .WithConstantBuffer(Builtin::PerFrameBuffer);
    }

    void Setup() override
    {
        m_counterBuffer = m_resourceRegistryView->RequestPtr<Resource>(Builtin::Terrain::RvtCounters);
    }

    PassReturn Execute(PassExecutionContext& executionContext) override
    {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& commandList = executionContext.commandList;
        commandList.SetDescriptorHeaps(renderContext->textureDescriptorHeap.GetHandle(), renderContext->samplerDescriptorHeap.GetHandle());
        commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());

        commandList.BindPipeline(m_clearPso.GetAPIPipelineState().GetHandle());
        BindResourceDescriptorIndices(commandList, m_clearPso.GetResourceDescriptorSlots());
        commandList.Dispatch(1u, 1u, 1u);

        if (m_counterBuffer) {
            rhi::BufferBarrier barrier{};
            barrier.buffer = m_counterBuffer->GetAPIResource().GetHandle();
            barrier.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
            barrier.afterAccess = rhi::ResourceAccessType::UnorderedAccess;
            barrier.beforeSync = rhi::ResourceSyncState::ComputeShading;
            barrier.afterSync = rhi::ResourceSyncState::ComputeShading;
            rhi::BarrierBatch batch{};
            batch.buffers = rhi::Span<rhi::BufferBarrier>(&barrier, 1u);
            commandList.Barriers(batch);
        }

        commandList.BindPipeline(m_resolvePso.GetAPIPipelineState().GetHandle());
        BindResourceDescriptorIndices(commandList, m_resolvePso.GetResourceDescriptorSlots());
        const uint32_t maxPageTableEntries = TerrainRvt::MaxPageTableEntries();
        const auto [dispatchX, dispatchY] = TerrainRvt::Dispatch2DForItems(maxPageTableEntries, 64u);
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

    void Cleanup() override
    {
        m_counterBuffer = nullptr;
    }

private:
    PipelineState m_clearPso;
    PipelineState m_resolvePso;
    Resource* m_counterBuffer = nullptr;
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
                Builtin::Terrain::RvtPhysicalPageOwner,
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

        rhi::GlobalBarrier generationWritesVisible{};
        generationWritesVisible.beforeSync = rhi::ResourceSyncState::ComputeShading;
        generationWritesVisible.afterSync = rhi::ResourceSyncState::ComputeShading;
        generationWritesVisible.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
        generationWritesVisible.afterAccess = rhi::ResourceAccessType::ShaderResource;
        rhi::BarrierBatch barrierBatch{};
        barrierBatch.globals = rhi::Span<rhi::GlobalBarrier>(&generationWritesVisible, 1u);
        commandList.Barriers(barrierBatch);
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
