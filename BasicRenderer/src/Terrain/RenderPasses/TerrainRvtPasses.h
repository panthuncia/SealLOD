#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Runtime/StateGraph/InvocationRevision.h"

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
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"
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

    inline br::render::PreparedComputeDispatch PrepareDispatch(const org::PassPrepareContext& preparation, const org::PipelineState& pso,
        uint32_t groupsX, uint32_t groupsY = 1u, uint32_t groupsZ = 1u)
    {
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        br::render::PreparedComputeDispatch data{};
        data.resourceHeap = context->textureDescriptorHeap.GetHandle();
        data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
        data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
        auto program = preparation.CaptureProgramBinding(pso);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);
        data.groupsX = groupsX; data.groupsY = groupsY; data.groupsZ = groupsZ;
        return data;
    }
}

class TerrainRvtFrameResetPass final : public org::TypedRenderGraphPass<TerrainRvtFrameResetPass, br::render::PreparedComputeDispatch> {
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

    void Declare(org::PassBuilder& b)
    {
        b.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        b.WithUnorderedAccess(
            Builtin::Terrain::RvtInfo,
            Builtin::Terrain::RvtClipInfos,
            Builtin::Terrain::RvtPageTable,
            Builtin::Terrain::RvtPageKeys,
            Builtin::Terrain::RvtPhysicalPageOwner,
            Builtin::Terrain::RvtPhysicalPageAtlas,
            Builtin::Terrain::RvtRequestMasks,
            Builtin::Terrain::RvtCounters,
            Builtin::Terrain::RvtStats)
            .WithShaderResource(Builtin::CameraBuffer, Builtin::Terrain::Sets)
            .WithConstantBuffer(Builtin::PerFrameBuffer);
    }

    void InvocationRevision(const org::PassPrepareContext& preparation, std::vector<uint64_t>& out) const {
        br::render::AppendFrameHeapRevision(preparation, out);
        out.push_back(SettingsManager::GetInstance().Revision());
        out.push_back(br::render::PipelineRevision(m_pso));
        out.push_back(br::render::HandleRevision(PSOManager::GetInstance().GetComputeRootSignature().GetHandle()));
    }
    br::render::PreparedComputeDispatch Prepare(const org::PassPrepareContext& preparation)
    {
        const auto* context = preparation.preparationData->Get<UpdateContext>();

        br::render::PreparedComputeDispatch data{};
        data.resourceHeap = context->textureDescriptorHeap.GetHandle();
        data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
        data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();


        auto program = preparation.CaptureProgramBinding(m_pso);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);
        TerrainRvt::FillInfoRootConstants(data.constants.data());
        const auto dispatch = TerrainRvt::Dispatch2DForItems(TerrainRvt::MaxPageTableEntries(), 64u);
        data.groupsX = dispatch.first;
        data.groupsY = dispatch.second;
        return data;
    }


    static void Record(const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatch(data, recording);
    }

private:
    org::PipelineState m_pso;
};

struct TerrainRvtMarkVisibilityMaterialPagesBindings {
    org::ResourceBindingToken visibleClusters, visibleCount;
    uint32_t visibleCapacity = 0;
};

class TerrainRvtMarkVisibilityMaterialPagesPass final : public org::TypedRenderGraphPass<TerrainRvtMarkVisibilityMaterialPagesPass,
    br::render::PreparedComputeDispatch, TerrainRvtMarkVisibilityMaterialPagesBindings> {
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
            b.BindShaderResource(m_visibleClustersResource),
            b.BindShaderResource(m_visibleClustersCounterResource),
            m_visibleClusterCapacity};
        if (m_slabResourceGroup) {
            b.WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
        }
        b.WithShaderResource(
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
        return bindings;
    }

    void Initialize() {}

    br::render::PreparedComputeDispatch Prepare(
        const TerrainRvtMarkVisibilityMaterialPagesBindings& bindings,
        const org::PassPrepareContext& preparation) const
    {
        const auto* context = preparation.preparationData->Get<UpdateContext>();

        br::render::PreparedComputeDispatch data{};
        data.resourceHeap = context->textureDescriptorHeap.GetHandle();
        data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
        data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();


        auto program = preparation.CaptureProgramBinding(m_pso);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);
        data.constants[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = preparation.ResolveView(
            bindings.visibleClusters, {org::BindlessViewKind::ShaderResource}).index;
        data.constants[VISBUF_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = preparation.ResolveView(
            bindings.visibleCount, {org::BindlessViewKind::ShaderResource}).index;
        data.groupsX = (std::max(bindings.visibleCapacity, 1u) + 63u) / 64u;
        return data;
    }

    void ShutdownPass()
    {
        m_slabResourceGroup.reset();
    }

    static void Record(const TerrainRvtMarkVisibilityMaterialPagesBindings&,
        const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatch(data, recording);
    }

private:
    org::PipelineState m_pso;
    std::shared_ptr<org::ResourceGroup> m_slabResourceGroup;
    std::shared_ptr<org::GloballyIndexedResource> m_visibleClustersResource;
    std::shared_ptr<org::GloballyIndexedResource> m_visibleClustersCounterResource;
    uint32_t m_visibleClusterCapacity = 0u;
};

class TerrainRvtResolveRequestsPass final : public org::TypedRenderGraphPass<TerrainRvtResolveRequestsPass, br::render::PreparedComputePipelineSequence> {
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

    void Declare(org::PassBuilder& b)
    {
        b.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        b.WithShaderResource(
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

    br::render::PreparedComputePipelineSequence Prepare(const org::PassPrepareContext& preparation)
    {
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        br::render::PreparedComputePipelineSequence data{};
        data.resourceHeap = context->textureDescriptorHeap.GetHandle();
        data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
        data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
        auto append = [&](org::PipelineState& pso, uint32_t x, uint32_t y) {

            br::render::PreparedComputePipelineSequence::Step step{};


            auto program = preparation.CaptureProgramBinding(pso);
            step.program = program.program;
            step.descriptorIndices = std::move(program.descriptorIndices);
            step.groupsX = x; step.groupsY = y;
            data.steps.push_back(std::move(step));
        };
        append(m_clearPso, 1u, 1u);
        const auto dispatch = TerrainRvt::Dispatch2DForItems(TerrainRvt::MaxGeneratedPagesPerFrame(), 64u);
        append(m_resolvePso, dispatch.first, dispatch.second);
        return data;
    }


    static void Record(const br::render::PreparedComputePipelineSequence& data, org::PassRecordContext& recording) {
        br::render::RecordPreparedComputePipelineSequence(data, recording);
    }

private:
    org::PipelineState m_clearPso;
    org::PipelineState m_resolvePso;
};

class TerrainRvtClearFeedbackRequestsPass final : public org::TypedRenderGraphPass<TerrainRvtClearFeedbackRequestsPass, br::render::PreparedComputeDispatch> {
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

    void Declare(org::PassBuilder& b)
    {
        b.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        b.WithShaderResource(Builtin::Terrain::RvtInfo)
            .WithUnorderedAccess(
                Builtin::Terrain::RvtRequestMasks,
                Builtin::Terrain::RvtCounters);
    }

    br::render::PreparedComputeDispatch Prepare(const org::PassPrepareContext& preparation)
    {
        const auto dispatch = TerrainRvt::Dispatch2DForItems(TerrainRvt::MaxPageTableEntries(), 64u);
        return TerrainRvt::PrepareDispatch(preparation, m_pso,
            dispatch.first, dispatch.second);
    }


    static void Record(const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatch(data, recording);
    }

private:
    org::PipelineState m_pso;
};

class TerrainRvtBuildHeightResidentCachePass final : public org::TypedRenderGraphPass<TerrainRvtBuildHeightResidentCachePass, br::render::PreparedComputeDispatch> {
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

    void Declare(org::PassBuilder& b)
    {
        b.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        b.WithShaderResource(
            Builtin::Terrain::RvtInfo,
            Builtin::Terrain::RvtClipInfos,
            Builtin::Terrain::RvtPageTable,
            Builtin::Terrain::RvtPageKeys)
            .WithUnorderedAccess(Builtin::Terrain::RvtHeightResidentCache)
            .WithConstantBuffer(Builtin::PerFrameBuffer);
    }

    br::render::PreparedComputeDispatch Prepare(const org::PassPrepareContext& preparation)
    {
        const auto dispatch = TerrainRvt::Dispatch2DForItems(TerrainRvt::MaxPageTableEntries(), 64u);
        return TerrainRvt::PrepareDispatch(preparation, m_pso,
            dispatch.first, dispatch.second);
    }


    static void Record(const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatch(data, recording);
    }

private:
    org::PipelineState m_pso;
};

class TerrainRvtBuildGenerateDispatchArgsPass final : public org::TypedRenderGraphPass<TerrainRvtBuildGenerateDispatchArgsPass, br::render::PreparedComputeDispatch> {
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

    void Declare(org::PassBuilder& b)
    {
        b.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        b.WithShaderResource(Builtin::Terrain::RvtInfo, Builtin::Terrain::RvtCounters)
            .WithUnorderedAccess(Builtin::Terrain::RvtGenerateDispatchArgs);
    }

    br::render::PreparedComputeDispatch Prepare(const org::PassPrepareContext& preparation)
    {
        return TerrainRvt::PrepareDispatch(preparation, m_pso,
            1u);
    }


    static void Record(const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatch(data, recording);
    }

private:
    org::PipelineState m_pso;
};

class TerrainRvtGeneratePagesPass final : public org::TypedRenderGraphPass<TerrainRvtGeneratePagesPass, br::render::PreparedComputeIndirect> {
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

    void Declare(org::PassBuilder& b)
    {
        b.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        b.WithShaderResource(
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

    void Initialize()
    {
        m_argsBuffer = m_resourceRegistryView->RequestPtr<org::Resource>(Builtin::Terrain::RvtGenerateDispatchArgs);
    }

    br::render::PreparedComputeIndirect Prepare(const org::PassPrepareContext& preparation)
    {
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        br::render::PreparedComputeIndirect data{};
        data.enabled = m_argsBuffer != nullptr;
        data.resourceHeap = context->textureDescriptorHeap.GetHandle();
        data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
        data.commandSignature = preparation.CaptureCommandSignature(CommandSignatureManager::GetInstance().CaptureRawDispatchCommandSignature());
        if (m_argsBuffer) data.argumentsReference = preparation.CaptureResource(m_argsBuffer->GetGlobalResourceID());
        auto program = preparation.CaptureProgramBinding(m_pso);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);
        return data;
    }

    void ShutdownPass()
    {
        m_argsBuffer = nullptr;
    }

    static void Record(const br::render::PreparedComputeIndirect& data, org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeIndirect(data, recording);
    }

private:
    org::PipelineState m_pso;
    org::Resource* m_argsBuffer = nullptr;
};

class TerrainRvtFinalizeGeneratedPagesPass final : public org::TypedRenderGraphPass<TerrainRvtFinalizeGeneratedPagesPass, br::render::PreparedComputeDispatchSequence> {
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

    void Declare(org::PassBuilder& b)
    {
        b.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        b.WithShaderResource(
            Builtin::Terrain::RvtInfo,
            Builtin::Terrain::RvtCounters,
            Builtin::Terrain::RvtGenerationList,
            Builtin::Terrain::RvtPageKeys)
            .WithUnorderedAccess(
                Builtin::Terrain::RvtPageTable,
                Builtin::Terrain::RvtPhysicalPageOwner)
            .WithConstantBuffer(Builtin::PerFrameBuffer);
    }

    br::render::PreparedComputeDispatchSequence Prepare(const org::PassPrepareContext& preparation)
    {
        const auto* context = preparation.preparationData->Get<UpdateContext>();

        br::render::PreparedComputeDispatchSequence data{};
        data.resourceHeap = context->textureDescriptorHeap.GetHandle();
        data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
        data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();


        auto program = preparation.CaptureProgramBinding(m_pso);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);
        const auto dispatch = TerrainRvt::Dispatch2DForItems(TerrainRvt::MaxPhysicalPages(), 64u);
        data.steps.push_back({.groupsX = dispatch.first, .groupsY = dispatch.second, .groupsZ = 1u, .uavBarrierAfter = true});
        return data;
    }


    static void Record(const br::render::PreparedComputeDispatchSequence& data, org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatchSequence(data, recording);
    }

private:
    org::PipelineState m_pso;
};
