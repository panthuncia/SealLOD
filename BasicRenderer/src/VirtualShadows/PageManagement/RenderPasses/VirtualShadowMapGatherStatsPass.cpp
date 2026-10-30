#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapGatherStatsPass.h"
#include "Render/PassBuilders.h"

#include "Runtime/Settings/SettingsManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"

#include "../shaders/PerPassRootConstants/clodVirtualShadowGatherStatsRootConstants.h"

VirtualShadowMapGatherStatsPass::VirtualShadowMapGatherStatsPass(
    std::shared_ptr<org::PixelBuffer> pageTableTexture,
    std::shared_ptr<org::Buffer> allocationCountBuffer,
    std::shared_ptr<org::Buffer> allocationIndirectArgsBuffer,
    std::shared_ptr<org::Buffer> pageListHeaderBuffer,
    std::shared_ptr<org::Buffer> pageMetadataBuffer,
    std::shared_ptr<org::Buffer> clipmapInfoBuffer,
    std::shared_ptr<org::Buffer> statsBuffer,
    bool capturePreAllocateState)
    : m_pageTableTexture(std::move(pageTableTexture))
    , m_allocationCountBuffer(std::move(allocationCountBuffer))
    , m_allocationIndirectArgsBuffer(std::move(allocationIndirectArgsBuffer))
    , m_pageListHeaderBuffer(std::move(pageListHeaderBuffer))
    , m_pageMetadataBuffer(std::move(pageMetadataBuffer))
    , m_clipmapInfoBuffer(std::move(clipmapInfoBuffer))
    , m_statsBuffer(std::move(statsBuffer))
    , m_capturePreAllocateState(capturePreAllocateState)
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowGatherStatsCSMain",
        {},
        "CLod.VirtualShadow.GatherStats.PSO");
}

VirtualShadowMapGatherStatsBindings VirtualShadowMapGatherStatsPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    builder.ConstantBuffer(Builtin::PerFrameBuffer);
    return {m_pso, builder.ShaderResource(m_pageTableTexture, {static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull)}), builder.ShaderResource(m_allocationCountBuffer),
        builder.ShaderResource(m_allocationIndirectArgsBuffer), builder.ShaderResource(m_pageListHeaderBuffer),
        builder.ShaderResource(m_pageMetadataBuffer), builder.ShaderResource(m_clipmapInfoBuffer),
        builder.UnorderedAccess(m_statsBuffer), m_capturePreAllocateState};
}

void VirtualShadowMapGatherStatsPass::Initialize() {}

VirtualShadowMapGatherStatsFrame VirtualShadowMapGatherStatsPass::Prepare(
    const VirtualShadowMapGatherStatsBindings&, const org::PassPrepareContext&) const {
    return {CLodVirtualShadowBuildRuntimeResolutionConfig().pageTableResolution};
}

void VirtualShadowMapGatherStatsPass::ShutdownPass() {}

void VirtualShadowMapGatherStatsPass::Record(const VirtualShadowMapGatherStatsBindings& bindings,
    const VirtualShadowMapGatherStatsFrame& frame, org::PassRecordContext& recording) {
    br::render::MiscConstants constants{};
    constants[CLOD_VIRTUAL_SHADOW_GATHER_STATS_PAGE_TABLE_DESCRIPTOR_INDEX] = recording.Index(bindings.pageTable);
    constants[CLOD_VIRTUAL_SHADOW_GATHER_STATS_ALLOCATION_COUNT_DESCRIPTOR_INDEX] = recording.Index(bindings.allocationCount);
    constants[CLOD_VIRTUAL_SHADOW_GATHER_STATS_ALLOCATION_INDIRECT_ARGS_DESCRIPTOR_INDEX] = recording.Index(bindings.allocationArgs);
    constants[CLOD_VIRTUAL_SHADOW_GATHER_STATS_PAGE_LIST_HEADER_DESCRIPTOR_INDEX] = recording.Index(bindings.header);
    constants[CLOD_VIRTUAL_SHADOW_GATHER_STATS_CLIPMAP_INFO_DESCRIPTOR_INDEX] = recording.Index(bindings.clipmapInfo);
    constants[CLOD_VIRTUAL_SHADOW_GATHER_STATS_PAGE_METADATA_DESCRIPTOR_INDEX] = recording.Index(bindings.pageMetadata);
    constants[CLOD_VIRTUAL_SHADOW_GATHER_STATS_STATS_DESCRIPTOR_INDEX] = recording.Index(bindings.stats);
    constants[CLOD_VIRTUAL_SHADOW_GATHER_STATS_PAGE_TABLE_RESOLUTION] = frame.pageTableResolution;
    constants[CLOD_VIRTUAL_SHADOW_GATHER_STATS_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
    constants[CLOD_VIRTUAL_SHADOW_GATHER_STATS_CAPTURE_PRE_ALLOCATE_STATE] = bindings.capturePreAllocateState ? 1u : 0u;
    const uint32_t groups = (frame.pageTableResolution + 7u) / 8u;
    br::render::DispatchCompute(recording, bindings.pipeline, constants, groups, groups, CLodVirtualShadowMaxSupportedClipmapCount);
}
