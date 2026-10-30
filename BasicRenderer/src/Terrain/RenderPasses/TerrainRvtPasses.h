#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Render/PassBuilders.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include <BasicRenderer/Extensions/BuiltinResources.h>
#include "Pipeline/PipelineState/CommandSignatureManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Assets/TechniqueDescriptor.h"
#include "VirtualGeometry/GraphIntegration/CLodExtensionComponents.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Runtime/Detail/MaterialEvaluationBuildInputs.h"
#include "BasicRenderer/Diagnostics/TerrainRvtTelemetry.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "Runtime/GraphIntegration/Resolvers/ECSResourceResolver.h"
#include "Runtime/GraphIntegration/Resolvers/ResourceGroupResolver.h"
#include <BasicRenderer/Extensions/ResourceComponent.h>
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/visUtilRootConstants.h"

namespace TerrainRvt
{
    inline constexpr uint32_t CounterCount = 5u;
    inline constexpr uint32_t MaxDispatchGroupsX = 65535u;
    inline constexpr uint32_t MaxPhysicalAtlasTextureSide = 16384u;
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

    inline uint32_t MaxAtlasPagesPerAxis()
    {
        const uint32_t tileSide = PageSize() + BorderTexels() * 2u;
        return std::max(1u, MaxPhysicalAtlasTextureSide / std::max(tileSide, 1u));
    }

    inline uint32_t AtlasPagesWide()
    {
        return std::clamp(SettingU32("terrainRvtPhysicalAtlasPagesWide", MaxAtlasPagesPerAxis()), 1u, MaxAtlasPagesPerAxis());
    }

    inline uint32_t AtlasPagesHigh()
    {
        return std::clamp(SettingU32("terrainRvtPhysicalAtlasPagesHigh", MaxAtlasPagesPerAxis()), 1u, MaxAtlasPagesPerAxis());
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

    inline float MipOffset()
    {
        return std::clamp(SettingFloat("terrainRvtMipOffset", 0.0f), -8.0f, 8.0f);
    }

    inline float SourceTexelsPerWorld()
    {
        return (std::max)(SettingFloat("terrainRvtSourceTexelsPerWorld", DefaultSourceTexelsPerWorld), 0.001f);
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

    inline uint32_t MaxGeneratedPagesPerFrame()
    {
        return std::clamp(SettingU32("terrainRvtMaxGeneratedPagesPerFrame", 64u), 1u, MaxPhysicalPages());
    }

    inline float BasePageWorldSize()
    {
        return (std::max)(static_cast<float>(PageSize()) / SourceTexelsPerWorld(), 0.125f);
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
        rootConstants[14] = MaxGeneratedPagesPerFrame();
        rootConstants[15] = FloatBits(MipOffset());
    }

    inline std::vector<DxcDefine> ShaderDefines()
    {
        if (!IsTerrainRvtTelemetryDebugEnabled()) {
            return {};
        }
        return { DxcDefine{ L"TERRAIN_RVT_TELEMETRY", L"1" } };
    }

    struct PipelineBindings {
        org::PipelineState pipeline;
    };

    // Dispatch sizes derived from the RVT settings of the frame being prepared.
    struct DispatchFrame {
        uint32_t groupsX = 0;
        uint32_t groupsY = 1;
    };

    inline DispatchFrame DispatchForItems(uint32_t itemCount, uint32_t threadsPerGroup)
    {
        const auto dispatch = Dispatch2DForItems(itemCount, threadsPerGroup);
        return { dispatch.first, dispatch.second };
    }
}

struct TerrainRvtFrameResetFrame {
    br::render::MiscConstants constants{};
    TerrainRvt::DispatchFrame dispatch;
};

class TerrainRvtFrameResetPass final : public org::TypedRenderGraphPass<TerrainRvtFrameResetPass,
    TerrainRvtFrameResetFrame, TerrainRvt::PipelineBindings> {
public:
    TerrainRvtFrameResetPass()
    {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/TerrainRvt.hlsl",
            L"TerrainRvtFrameResetCS",
            TerrainRvt::ShaderDefines(),
            "TerrainRvt.FrameReset.PSO");
    }

    TerrainRvt::PipelineBindings Declare(org::PassBuilder& b)
    {
        b.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        b.UnorderedAccess(
            Builtin::Terrain::RvtInfo,
            Builtin::Terrain::RvtClipInfos,
            Builtin::Terrain::RvtPageTable,
            Builtin::Terrain::RvtPageKeys,
            Builtin::Terrain::RvtPhysicalPageOwner,
            Builtin::Terrain::RvtPhysicalPageAtlas,
            Builtin::Terrain::RvtRequestMasks,
            Builtin::Terrain::RvtCounters,
            Builtin::Terrain::RvtStats);
        b.ShaderResource(Builtin::CameraBuffer, Builtin::Terrain::Sets);
        b.ConstantBuffer(Builtin::PerFrameBuffer);
        return { m_pso };
    }

    TerrainRvtFrameResetFrame Prepare(const TerrainRvt::PipelineBindings&, const org::PassPrepareContext&) const
    {
        TerrainRvtFrameResetFrame frame{};
        TerrainRvt::FillInfoRootConstants(frame.constants.data());
        frame.dispatch = TerrainRvt::DispatchForItems(TerrainRvt::MaxPageTableEntries(), 64u);
        return frame;
    }

    static void Record(const TerrainRvt::PipelineBindings& bindings, const TerrainRvtFrameResetFrame& frame,
        org::PassRecordContext& context) {
        br::render::DispatchCompute(context, bindings.pipeline, frame.constants, frame.dispatch.groupsX, frame.dispatch.groupsY);
    }

private:
    org::PipelineState m_pso;
};

struct TerrainRvtMarkVisibilityMaterialPagesBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken visibleClusters, visibleCount;
    uint32_t visibleCapacity = 0;
};

class TerrainRvtMarkVisibilityMaterialPagesPass final : public org::TypedRenderGraphPass<TerrainRvtMarkVisibilityMaterialPagesPass,
    org::EmptyPassFrameData, TerrainRvtMarkVisibilityMaterialPagesBindings> {
public:
    explicit TerrainRvtMarkVisibilityMaterialPagesPass(const MaterialEvaluationBuildInputs& services)
    {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/TerrainRvt.hlsl",
            L"TerrainRvtMarkVisibleClusterPagesCS",
            TerrainRvt::ShaderDefines(),
            "TerrainRvt.MarkVisibleClusterPages.PSO");

        m_visibleClustersResource = services.visibleClusters;
        m_visibleClustersCounterResource = services.visibleClusterCounter;
        m_visibleClusterCapacity = services.visibleClusterCapacity;
        m_slabResourceGroup = services.clodSlabResources;
    }
    TerrainRvtMarkVisibilityMaterialPagesBindings Declare(org::PassBuilder& b)
    {
        b.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        TerrainRvtMarkVisibilityMaterialPagesBindings bindings{
            m_pso,
            b.ShaderResource(m_visibleClustersResource).View(),
            b.ShaderResource(m_visibleClustersCounterResource).View(),
            m_visibleClusterCapacity};
        if (m_slabResourceGroup) {
            b.ShaderResource(ResourceGroupResolver(m_slabResourceGroup));
        }
        b.ShaderResource(
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
            Builtin::Terrain::RvtPhysicalPageOwner);
        b.UnorderedAccess(
                Builtin::Terrain::RvtRequestMasks,
                Builtin::Terrain::RvtRequestList,
                Builtin::Terrain::RvtCounters,
                Builtin::Terrain::RvtStats);
        b.ConstantBuffer(Builtin::PerFrameBuffer);
        return bindings;
    }

    void Initialize() {}

    void ShutdownPass()
    {
        m_slabResourceGroup.reset();
    }

    static void Record(const TerrainRvtMarkVisibilityMaterialPagesBindings& bindings, org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = context.Index(bindings.visibleClusters);
        constants[VISBUF_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = context.Index(bindings.visibleCount);
        br::render::DispatchCompute(context, bindings.pipeline, constants, (std::max(bindings.visibleCapacity, 1u) + 63u) / 64u);
    }

private:
    org::PipelineState m_pso;
    std::shared_ptr<org::ResourceGroup> m_slabResourceGroup;
    std::shared_ptr<org::GloballyIndexedResource> m_visibleClustersResource;
    std::shared_ptr<org::GloballyIndexedResource> m_visibleClustersCounterResource;
    uint32_t m_visibleClusterCapacity = 0u;
};

struct TerrainRvtResolveRequestsBindings {
    org::PipelineState clear, resolve;
};

class TerrainRvtResolveRequestsPass final : public org::TypedRenderGraphPass<TerrainRvtResolveRequestsPass,
    TerrainRvt::DispatchFrame, TerrainRvtResolveRequestsBindings> {
public:
    TerrainRvtResolveRequestsPass()
    {
        auto& psoManager = PSOManager::GetInstance();
        m_clearPso = psoManager.MakeComputePipeline(
            psoManager.GetComputeRootSignature().GetHandle(),
            L"shaders/TerrainRvt.hlsl",
            L"TerrainRvtClearGenerationCounterCS",
            TerrainRvt::ShaderDefines(),
            "TerrainRvt.ClearGenerationCounter.PSO");
        m_resolvePso = psoManager.MakeComputePipeline(
            psoManager.GetComputeRootSignature().GetHandle(),
            L"shaders/TerrainRvt.hlsl",
            L"TerrainRvtResolveRequestsCS",
            TerrainRvt::ShaderDefines(),
            "TerrainRvt.ResolveRequests.PSO");
    }

    TerrainRvtResolveRequestsBindings Declare(org::PassBuilder& b)
    {
        b.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        b.ShaderResource(
            Builtin::Terrain::RvtInfo,
            Builtin::Terrain::RvtRequestList,
            Builtin::Terrain::RvtRequestMasks,
            Builtin::Terrain::RvtPageKeys);
        b.UnorderedAccess(
                Builtin::Terrain::RvtCounters,
                Builtin::Terrain::RvtPageTable,
                Builtin::Terrain::RvtPhysicalPageOwner,
                Builtin::Terrain::RvtGenerationList,
                Builtin::Terrain::RvtStats);
        b.ConstantBuffer(Builtin::PerFrameBuffer);
        return { m_clearPso, m_resolvePso };
    }

    TerrainRvt::DispatchFrame Prepare(const TerrainRvtResolveRequestsBindings&, const org::PassPrepareContext&) const
    {
        return TerrainRvt::DispatchForItems(TerrainRvt::MaxGeneratedPagesPerFrame(), 64u);
    }

    static void Record(const TerrainRvtResolveRequestsBindings& bindings, const TerrainRvt::DispatchFrame& frame,
        org::PassRecordContext& context) {
        br::render::DispatchCompute(context, bindings.clear, {}, 1u, 1u);
        br::render::DispatchCompute(context, bindings.resolve, {}, frame.groupsX, frame.groupsY);
    }

private:
    org::PipelineState m_clearPso;
    org::PipelineState m_resolvePso;
};

class TerrainRvtClearFeedbackRequestsPass final : public org::TypedRenderGraphPass<TerrainRvtClearFeedbackRequestsPass,
    TerrainRvt::DispatchFrame, TerrainRvt::PipelineBindings> {
public:
    TerrainRvtClearFeedbackRequestsPass()
    {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/TerrainRvt.hlsl",
            L"TerrainRvtClearFeedbackRequestsCS",
            TerrainRvt::ShaderDefines(),
            "TerrainRvt.ClearFeedbackRequests.PSO");
    }

    TerrainRvt::PipelineBindings Declare(org::PassBuilder& b)
    {
        b.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        b.ShaderResource(Builtin::Terrain::RvtInfo);
        b.UnorderedAccess(
                Builtin::Terrain::RvtRequestMasks,
                Builtin::Terrain::RvtCounters);
        return { m_pso };
    }

    TerrainRvt::DispatchFrame Prepare(const TerrainRvt::PipelineBindings&, const org::PassPrepareContext&) const
    {
        return TerrainRvt::DispatchForItems(TerrainRvt::MaxPageTableEntries(), 64u);
    }

    static void Record(const TerrainRvt::PipelineBindings& bindings, const TerrainRvt::DispatchFrame& frame,
        org::PassRecordContext& context) {
        br::render::DispatchCompute(context, bindings.pipeline, {}, frame.groupsX, frame.groupsY);
    }

private:
    org::PipelineState m_pso;
};

class TerrainRvtBuildHeightResidentCachePass final : public org::TypedRenderGraphPass<TerrainRvtBuildHeightResidentCachePass,
    TerrainRvt::DispatchFrame, TerrainRvt::PipelineBindings> {
public:
    TerrainRvtBuildHeightResidentCachePass()
    {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/TerrainRvt.hlsl",
            L"TerrainRvtBuildHeightResidentCacheCS",
            TerrainRvt::ShaderDefines(),
            "TerrainRvt.BuildHeightResidentCache.PSO");
    }

    TerrainRvt::PipelineBindings Declare(org::PassBuilder& b)
    {
        b.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        b.ShaderResource(
            Builtin::Terrain::RvtInfo,
            Builtin::Terrain::RvtClipInfos,
            Builtin::Terrain::RvtPageTable,
            Builtin::Terrain::RvtPageKeys);
        b.UnorderedAccess(Builtin::Terrain::RvtHeightResidentCache);
        b.ConstantBuffer(Builtin::PerFrameBuffer);
        return { m_pso };
    }

    TerrainRvt::DispatchFrame Prepare(const TerrainRvt::PipelineBindings&, const org::PassPrepareContext&) const
    {
        return TerrainRvt::DispatchForItems(TerrainRvt::MaxPageTableEntries(), 64u);
    }

    static void Record(const TerrainRvt::PipelineBindings& bindings, const TerrainRvt::DispatchFrame& frame,
        org::PassRecordContext& context) {
        br::render::DispatchCompute(context, bindings.pipeline, {}, frame.groupsX, frame.groupsY);
    }

private:
    org::PipelineState m_pso;
};

class TerrainRvtBuildGenerateDispatchArgsPass final : public org::TypedRenderGraphPass<TerrainRvtBuildGenerateDispatchArgsPass,
    org::EmptyPassFrameData, TerrainRvt::PipelineBindings> {
public:
    TerrainRvtBuildGenerateDispatchArgsPass()
    {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/TerrainRvt.hlsl",
            L"TerrainRvtBuildGenerateDispatchArgsCS",
            TerrainRvt::ShaderDefines(),
            "TerrainRvt.BuildGenerateDispatchArgs.PSO");
    }

    TerrainRvt::PipelineBindings Declare(org::PassBuilder& b)
    {
        b.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        b.ShaderResource(Builtin::Terrain::RvtInfo, Builtin::Terrain::RvtCounters);
        b.UnorderedAccess(Builtin::Terrain::RvtGenerateDispatchArgs);
        return { m_pso };
    }

    static void Record(const TerrainRvt::PipelineBindings& bindings, org::PassRecordContext& context) {
        br::render::DispatchCompute(context, bindings.pipeline, {}, 1u);
    }

private:
    org::PipelineState m_pso;
};

struct TerrainRvtGeneratePagesBindings {
    org::PipelineState pipeline;
    org::ResourceBindingToken arguments;
};
struct TerrainRvtGeneratePagesFrame {
    std::shared_ptr<const rhi::CommandSignaturePtr> signature;
};

class TerrainRvtGeneratePagesPass final : public org::TypedRenderGraphPass<TerrainRvtGeneratePagesPass,
    TerrainRvtGeneratePagesFrame, TerrainRvtGeneratePagesBindings> {
public:
    TerrainRvtGeneratePagesPass()
    {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/TerrainRvt.hlsl",
            L"TerrainRvtGeneratePagesCS",
            TerrainRvt::ShaderDefines(),
            "TerrainRvt.GeneratePages.PSO");
    }

    TerrainRvtGeneratePagesBindings Declare(org::PassBuilder& b)
    {
        b.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        b.ShaderResource(
            Builtin::CameraBuffer,
            Builtin::Terrain::RvtInfo,
            Builtin::Terrain::RvtClipInfos,
            Builtin::Terrain::RvtCounters,
            Builtin::Terrain::RvtGenerationList,
            Builtin::Terrain::Sets,
            Builtin::Terrain::Layers,
            Builtin::Terrain::StochasticLayers,
            Builtin::Terrain::LayerRefs,
            Builtin::Terrain::Regions,
            Builtin::Terrain::WeightBlocks,
            Builtin::Terrain::TextureGroup,
            Builtin::Material::TextureStreamingMetadataBuffer);
        b.UnorderedAccess(
                Builtin::Terrain::RvtHeightAtlas,
                Builtin::Terrain::RvtAlbedoAtlas,
                Builtin::Terrain::RvtNormalAtlas,
                Builtin::Terrain::RvtMaterialAtlas,
                Builtin::Terrain::RvtStats,
                Builtin::Material::TextureStreamingFeedbackBuffer);
        const auto arguments = b.IndirectArguments(Builtin::Terrain::RvtGenerateDispatchArgs);
        b.ConstantBuffer(Builtin::PerFrameBuffer);
        return { m_pso, arguments };
    }

    TerrainRvtGeneratePagesFrame Prepare(const TerrainRvtGeneratePagesBindings&, const org::PassPrepareContext&) const
    {
        return { CommandSignatureManager::GetInstance().CaptureRawDispatchCommandSignature() };
    }

    static void Record(const TerrainRvtGeneratePagesBindings& bindings, const TerrainRvtGeneratePagesFrame& frame,
        org::PassRecordContext& context) {
        br::render::DispatchComputeIndirect(context, bindings.pipeline, {}, frame.signature, bindings.arguments);
    }

private:
    org::PipelineState m_pso;
};

class TerrainRvtFinalizeGeneratedPagesPass final : public org::TypedRenderGraphPass<TerrainRvtFinalizeGeneratedPagesPass,
    TerrainRvt::DispatchFrame, TerrainRvt::PipelineBindings> {
public:
    TerrainRvtFinalizeGeneratedPagesPass()
    {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/TerrainRvt.hlsl",
            L"TerrainRvtFinalizeGeneratedPagesCS",
            TerrainRvt::ShaderDefines(),
            "TerrainRvt.FinalizeGeneratedPages.PSO");
    }

    TerrainRvt::PipelineBindings Declare(org::PassBuilder& b)
    {
        b.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        b.ShaderResource(
            Builtin::Terrain::RvtInfo,
            Builtin::Terrain::RvtCounters,
            Builtin::Terrain::RvtGenerationList,
            Builtin::Terrain::RvtPageKeys);
        b.UnorderedAccess(
                Builtin::Terrain::RvtPageTable,
                Builtin::Terrain::RvtPhysicalPageOwner);
        b.ConstantBuffer(Builtin::PerFrameBuffer);
        return { m_pso };
    }

    TerrainRvt::DispatchFrame Prepare(const TerrainRvt::PipelineBindings&, const org::PassPrepareContext&) const
    {
        return TerrainRvt::DispatchForItems(TerrainRvt::MaxPhysicalPages(), 64u);
    }

    static void Record(const TerrainRvt::PipelineBindings& bindings, const TerrainRvt::DispatchFrame& frame,
        org::PassRecordContext& context) {
        br::render::DispatchCompute(context, bindings.pipeline, {}, frame.groupsX, frame.groupsY);
        context.UavBarrier();
    }

private:
    org::PipelineState m_pso;
};
