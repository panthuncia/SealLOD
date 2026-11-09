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
#include <BasicRenderer/Extensions/PipelineDescs.h>
#include "Runtime/Settings/SettingsManager.h"
#include "Runtime/Device/DeviceManager.h"
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
    inline constexpr uint32_t CounterCount = 8u; // TERRAIN_RVT_COUNTER_COUNT
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

    // A page keeps the source texture mips that were resident when it was generated. On, a page that was short of a
    // texture is re-rendered once that texture improves; off, it stays as rendered (pages generated later get the mips).
    inline constexpr const char* RefreshStalePagesSettingName = "terrainRvtRefreshStalePages";
    inline bool RefreshStalePages()
    {
        try {
            return SettingsManager::GetInstance().getSettingGetter<bool>(RefreshStalePagesSettingName)();
        }
        catch (...) {
            return true;
        }
    }

    inline uint32_t MaxGeneratedPagesPerFrame()
    {
        // At most 2048: with compression, each page of the budget is a slice of the scratch texture arrays.
        return std::clamp(SettingU32("terrainRvtMaxGeneratedPagesPerFrame", 64u), 1u, std::min(MaxPhysicalPages(), 2048u));
    }

    // Block-compressed albedo (BC7), normal (BC5) and material (BC7) atlases. Generation writes uncompressed tiles to a
    // scratch sized for the per-frame budget, and TerrainRvtEncodePagesPass writes their blocks straight into the atlases'
    // pages through a UAV of one R32G32B32A32_UINT texel per block, which needs relaxed format casting (D3D12; Vulkan
    // reports it unsupported). Height stays R16F. Where it can't be done, the atlases stay uncompressed.
    inline constexpr const char* CompressionSettingName = "terrainRvtCompression";
    inline bool CompressionSupported()
    {
        static const bool supported = [] {
            ::EnhancedBarriersFeatureInfo info{};
            const bool queried = rhi::IsOk(DeviceManager::GetInstance().GetDevice().QueryFeatureInfo(&info.header));
            return queried && info.relaxedFormatCasting;
        }();
        return supported;
    }
    inline bool CompressionRequested()
    {
        try {
            return SettingsManager::GetInstance().getSettingGetter<bool>(CompressionSettingName)();
        }
        catch (...) {
            return true;
        }
    }
    inline bool CompressionEnabled()
    {
        const uint32_t tileSide = PageSize() + BorderTexels() * 2u;
        const bool enabled = CompressionRequested() && CompressionSupported() && tileSide % 4u == 0u;
        static bool reported = false;
        if (CompressionRequested() && !enabled && !reported) {
            reported = true;
            spdlog::warn("Terrain RVT compression is off: {}", !CompressionSupported()
                ? "the device cannot write block-compressed textures through a UINT view (relaxed format casting)"
                : "the tile side (page size plus both borders) is not a multiple of 4");
        }
        return enabled;
    }
    inline constexpr uint32_t CompressionBlocksPerTileSide(uint32_t tileSide) { return tileSide / 4u; }

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

    inline std::vector<org::PipelineShaderDefine> ShaderDefines()
    {
        std::vector<org::PipelineShaderDefine> defines;
        if (IsTerrainRvtTelemetryDebugEnabled()) defines.push_back({ L"TERRAIN_RVT_TELEMETRY", L"1" });
        if (CompressionEnabled()) defines.push_back({ L"TERRAIN_RVT_COMPRESSED", L"1" });
        return defines;
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
        return { b.Pipeline(br::render::ComputePipeline(L"shaders/TerrainRvt.hlsl", L"TerrainRvtFrameResetCS",
            "TerrainRvt.FrameReset.PSO", TerrainRvt::ShaderDefines())) };
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
        m_visibleClustersResource = services.visibleClusters;
        m_visibleClustersCounterResource = services.visibleClusterCounter;
        m_visibleClusterCapacity = services.visibleClusterCapacity;
        m_slabResourceGroup = services.clodSlabResources;
    }
    TerrainRvtMarkVisibilityMaterialPagesBindings Declare(org::PassBuilder& b)
    {
        b.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        TerrainRvtMarkVisibilityMaterialPagesBindings bindings{
            b.Pipeline(br::render::ComputePipeline(L"shaders/TerrainRvt.hlsl", L"TerrainRvtMarkVisibleClusterPagesCS",
                "TerrainRvt.MarkVisibleClusterPages.PSO", TerrainRvt::ShaderDefines())),
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
    std::shared_ptr<org::ResourceGroup> m_slabResourceGroup;
    std::shared_ptr<org::GloballyIndexedResource> m_visibleClustersResource;
    std::shared_ptr<org::GloballyIndexedResource> m_visibleClustersCounterResource;
    uint32_t m_visibleClusterCapacity = 0u;
};

struct TerrainRvtResolveRequestsBindings {
    org::PipelineState clear, resolve, refresh;
};

struct TerrainRvtResolveRequestsFrame {
    TerrainRvt::DispatchFrame resolve;
    TerrainRvt::DispatchFrame refresh;
    br::render::MiscConstants refreshConstants{};
};

// Resolve allocates and queues missing pages; the refresh dispatch after it scans resident pages that were short of a
// source texture (TerrainRvtRefreshStalePagesCS), keeps asking for it, and queues a re-render into the leftover budget.
class TerrainRvtResolveRequestsPass final : public org::TypedRenderGraphPass<TerrainRvtResolveRequestsPass,
    TerrainRvtResolveRequestsFrame, TerrainRvtResolveRequestsBindings> {
public:
    TerrainRvtResolveRequestsBindings Declare(org::PassBuilder& b)
    {
        b.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        b.ShaderResource(
            Builtin::Terrain::RvtInfo,
            Builtin::Terrain::RvtRequestList,
            Builtin::Terrain::RvtRequestMasks,
            Builtin::Terrain::RvtPageKeys,
            Builtin::Material::TextureStreamingMetadataBuffer);
        b.UnorderedAccess(
                Builtin::Terrain::RvtCounters,
                Builtin::Terrain::RvtPageTable,
                Builtin::Terrain::RvtPhysicalPageOwner,
                Builtin::Terrain::RvtGenerationList,
                Builtin::Terrain::RvtStats,
                Builtin::Material::TextureStreamingFeedbackBuffer);
        b.ConstantBuffer(Builtin::PerFrameBuffer);
        return { b.Pipeline(br::render::ComputePipeline(L"shaders/TerrainRvt.hlsl", L"TerrainRvtClearGenerationCounterCS",
                     "TerrainRvt.ClearGenerationCounter.PSO", TerrainRvt::ShaderDefines())),
                 b.Pipeline(br::render::ComputePipeline(L"shaders/TerrainRvt.hlsl", L"TerrainRvtResolveRequestsCS",
                     "TerrainRvt.ResolveRequests.PSO", TerrainRvt::ShaderDefines())),
                 b.Pipeline(br::render::ComputePipeline(L"shaders/TerrainRvt.hlsl", L"TerrainRvtRefreshStalePagesCS",
                     "TerrainRvt.RefreshStalePages.PSO", TerrainRvt::ShaderDefines())) };
    }

    TerrainRvtResolveRequestsFrame Prepare(const TerrainRvtResolveRequestsBindings&, const org::PassPrepareContext&) const
    {
        TerrainRvtResolveRequestsFrame frame{};
        frame.resolve = TerrainRvt::DispatchForItems(TerrainRvt::MaxGeneratedPagesPerFrame(), 64u);
        frame.refresh = TerrainRvt::DispatchForItems(TerrainRvt::MaxPhysicalPages(), 64u);
        frame.refreshConstants[0] = TerrainRvt::RefreshStalePages() ? 1u : 0u;
        return frame;
    }

    static void Record(const TerrainRvtResolveRequestsBindings& bindings, const TerrainRvtResolveRequestsFrame& frame,
        org::PassRecordContext& context) {
        br::render::DispatchCompute(context, bindings.clear, {}, 1u, 1u);
        context.UavBarrier();
        br::render::DispatchCompute(context, bindings.resolve, {}, frame.resolve.groupsX, frame.resolve.groupsY);
        // Refreshes take only what new pages left of the generation budget.
        context.UavBarrier();
        br::render::DispatchCompute(context, bindings.refresh, frame.refreshConstants, frame.refresh.groupsX, frame.refresh.groupsY);
    }
};

class TerrainRvtClearFeedbackRequestsPass final : public org::TypedRenderGraphPass<TerrainRvtClearFeedbackRequestsPass,
    TerrainRvt::DispatchFrame, TerrainRvt::PipelineBindings> {
public:
    TerrainRvt::PipelineBindings Declare(org::PassBuilder& b)
    {
        b.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        b.ShaderResource(Builtin::Terrain::RvtInfo);
        b.UnorderedAccess(
                Builtin::Terrain::RvtRequestMasks,
                Builtin::Terrain::RvtCounters);
        return { b.Pipeline(br::render::ComputePipeline(L"shaders/TerrainRvt.hlsl", L"TerrainRvtClearFeedbackRequestsCS",
            "TerrainRvt.ClearFeedbackRequests.PSO", TerrainRvt::ShaderDefines())) };
    }

    TerrainRvt::DispatchFrame Prepare(const TerrainRvt::PipelineBindings&, const org::PassPrepareContext&) const
    {
        return TerrainRvt::DispatchForItems(TerrainRvt::MaxPageTableEntries(), 64u);
    }

    static void Record(const TerrainRvt::PipelineBindings& bindings, const TerrainRvt::DispatchFrame& frame,
        org::PassRecordContext& context) {
        br::render::DispatchCompute(context, bindings.pipeline, {}, frame.groupsX, frame.groupsY);
    }
};

class TerrainRvtBuildHeightResidentCachePass final : public org::TypedRenderGraphPass<TerrainRvtBuildHeightResidentCachePass,
    TerrainRvt::DispatchFrame, TerrainRvt::PipelineBindings> {
public:
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
        return { b.Pipeline(br::render::ComputePipeline(L"shaders/TerrainRvt.hlsl", L"TerrainRvtBuildHeightResidentCacheCS",
            "TerrainRvt.BuildHeightResidentCache.PSO", TerrainRvt::ShaderDefines())) };
    }

    TerrainRvt::DispatchFrame Prepare(const TerrainRvt::PipelineBindings&, const org::PassPrepareContext&) const
    {
        return TerrainRvt::DispatchForItems(TerrainRvt::MaxPageTableEntries(), 64u);
    }

    static void Record(const TerrainRvt::PipelineBindings& bindings, const TerrainRvt::DispatchFrame& frame,
        org::PassRecordContext& context) {
        br::render::DispatchCompute(context, bindings.pipeline, {}, frame.groupsX, frame.groupsY);
    }
};

class TerrainRvtBuildGenerateDispatchArgsPass final : public org::TypedRenderGraphPass<TerrainRvtBuildGenerateDispatchArgsPass,
    org::EmptyPassFrameData, TerrainRvt::PipelineBindings> {
public:
    TerrainRvt::PipelineBindings Declare(org::PassBuilder& b)
    {
        b.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        b.ShaderResource(Builtin::Terrain::RvtInfo, Builtin::Terrain::RvtCounters);
        b.UnorderedAccess(Builtin::Terrain::RvtGenerateDispatchArgs);
        if (TerrainRvt::CompressionEnabled()) b.UnorderedAccess(Builtin::Terrain::RvtEncodeDispatchArgs);
        return { b.Pipeline(br::render::ComputePipeline(L"shaders/TerrainRvt.hlsl", L"TerrainRvtBuildGenerateDispatchArgsCS",
            "TerrainRvt.BuildGenerateDispatchArgs.PSO", TerrainRvt::ShaderDefines())) };
    }

    static void Record(const TerrainRvt::PipelineBindings& bindings, org::PassRecordContext& context) {
        br::render::DispatchCompute(context, bindings.pipeline, {}, 1u);
    }
};

struct TerrainRvtGeneratePagesBindings {
    org::PipelineState pipeline;
    org::ResourceBindingToken arguments;
    org::CommandSignatureState signature;
};

class TerrainRvtGeneratePagesPass final : public org::TypedRenderGraphPass<TerrainRvtGeneratePagesPass,
    org::EmptyPassFrameData, TerrainRvtGeneratePagesBindings> {
public:
    TerrainRvtGeneratePagesBindings Declare(org::PassBuilder& b)
    {
        b.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        b.ShaderResource(
            Builtin::CameraBuffer,
            Builtin::Terrain::RvtInfo,
            Builtin::Terrain::RvtClipInfos,
            Builtin::Terrain::RvtCounters,
            Builtin::Terrain::Sets,
            Builtin::Terrain::Layers,
            Builtin::Terrain::StochasticLayers,
            Builtin::Terrain::LayerRefs,
            Builtin::Terrain::Regions,
            Builtin::Terrain::WeightBlocks,
            Builtin::Material::TextureStreamingMetadataBuffer);
        b.UnorderedAccess(
                Builtin::Terrain::RvtGenerationList,
                Builtin::Terrain::RvtHeightAtlas,
                Builtin::Terrain::RvtStats,
                Builtin::Material::TextureStreamingFeedbackBuffer);
        // Compressed, the material tiles go to the scratch, and TerrainRvtEncodePagesPass writes the atlases.
        if (TerrainRvt::CompressionEnabled()) {
            b.UnorderedAccess(
                Builtin::Terrain::RvtScratchAlbedo,
                Builtin::Terrain::RvtScratchNormal,
                Builtin::Terrain::RvtScratchMaterial);
        }
        else {
            b.UnorderedAccess(
                Builtin::Terrain::RvtAlbedoAtlas,
                Builtin::Terrain::RvtNormalAtlas,
                Builtin::Terrain::RvtMaterialAtlas);
        }
        const auto arguments = b.IndirectArguments(Builtin::Terrain::RvtGenerateDispatchArgs);
        b.ConstantBuffer(Builtin::PerFrameBuffer);
        return { b.Pipeline(br::render::ComputePipeline(L"shaders/TerrainRvt.hlsl", L"TerrainRvtGeneratePagesCS",
            "TerrainRvt.GeneratePages.PSO", TerrainRvt::ShaderDefines())), arguments,
            b.CommandSignature(br::render::RawDispatchCommandSignature()) };
    }

    static void Record(const TerrainRvtGeneratePagesBindings& bindings, org::PassRecordContext& context) {
        br::render::DispatchComputeIndirect(context, bindings.pipeline, {}, bindings.signature, bindings.arguments);
    }
};

// Compressed atlases (TerrainRvt::CompressionEnabled): encodes the tiles TerrainRvtGeneratePagesPass left in the scratch and
// writes their blocks into the atlases' pages (TerrainRvtEncode.hlsl). Before finalization publishes the pages; readers
// sample the atlases after these writes.
class TerrainRvtEncodePagesPass final : public org::TypedRenderGraphPass<TerrainRvtEncodePagesPass,
    org::EmptyPassFrameData, TerrainRvtGeneratePagesBindings> {
public:
    TerrainRvtGeneratePagesBindings Declare(org::PassBuilder& b)
    {
        b.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        b.ShaderResource(
            Builtin::Terrain::RvtInfo,
            Builtin::Terrain::RvtClipInfos,
            Builtin::Terrain::RvtCounters,
            Builtin::Terrain::RvtGenerationList,
            Builtin::Terrain::RvtScratchAlbedo,
            Builtin::Terrain::RvtScratchNormal,
            Builtin::Terrain::RvtScratchMaterial);
        b.UnorderedAccess(
            Builtin::Terrain::RvtAlbedoAtlas,
            Builtin::Terrain::RvtNormalAtlas,
            Builtin::Terrain::RvtMaterialAtlas);
        const auto arguments = b.IndirectArguments(Builtin::Terrain::RvtEncodeDispatchArgs);
        return { b.Pipeline(br::render::ComputePipeline(L"shaders/TerrainRvtEncode.hlsl", L"TerrainRvtEncodePagesCS",
            "TerrainRvt.EncodePages.PSO", TerrainRvt::ShaderDefines())), arguments,
            b.CommandSignature(br::render::RawDispatchCommandSignature()) };
    }

    static void Record(const TerrainRvtGeneratePagesBindings& bindings, org::PassRecordContext& context) {
        br::render::DispatchComputeIndirect(context, bindings.pipeline, {}, bindings.signature, bindings.arguments);
    }
};

class TerrainRvtFinalizeGeneratedPagesPass final : public org::TypedRenderGraphPass<TerrainRvtFinalizeGeneratedPagesPass,
    TerrainRvt::DispatchFrame, TerrainRvt::PipelineBindings> {
public:
    TerrainRvt::PipelineBindings Declare(org::PassBuilder& b)
    {
        b.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        b.ShaderResource(
            Builtin::Terrain::RvtInfo,
            Builtin::Terrain::RvtGenerationList,
            Builtin::Terrain::RvtPageKeys);
        b.UnorderedAccess(
                Builtin::Terrain::RvtCounters,
                Builtin::Terrain::RvtPageTable,
                Builtin::Terrain::RvtPhysicalPageOwner);
        b.ConstantBuffer(Builtin::PerFrameBuffer);
        return { b.Pipeline(br::render::ComputePipeline(L"shaders/TerrainRvt.hlsl", L"TerrainRvtFinalizeGeneratedPagesCS",
            "TerrainRvt.FinalizeGeneratedPages.PSO", TerrainRvt::ShaderDefines())) };
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
};
