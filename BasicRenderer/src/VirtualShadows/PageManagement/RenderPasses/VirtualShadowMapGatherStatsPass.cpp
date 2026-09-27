#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapGatherStatsPass.h"

#include "Runtime/Settings/SettingsManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"

#include "../shaders/PerPassRootConstants/clodVirtualShadowGatherStatsRootConstants.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

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
    return {builder.ShaderResource(m_pageTableTexture, {static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull)}), builder.ShaderResource(m_allocationCountBuffer),
        builder.ShaderResource(m_allocationIndirectArgsBuffer), builder.ShaderResource(m_pageListHeaderBuffer),
        builder.ShaderResource(m_pageMetadataBuffer), builder.ShaderResource(m_clipmapInfoBuffer),
        builder.UnorderedAccess(m_statsBuffer), m_capturePreAllocateState};
}

void VirtualShadowMapGatherStatsPass::Initialize() {}



br::render::PreparedComputeDispatch VirtualShadowMapGatherStatsPass::Prepare(
    const VirtualShadowMapGatherStatsBindings& bindings, const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    const auto config = CLodVirtualShadowBuildRuntimeResolutionConfig();
    auto payload = m_pso.GetPayload(); br::render::PreparedComputeDispatch data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle(); data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle(); auto program = preparation.CaptureProgramBinding(std::move(payload));
    data.program = program.program;
    data.descriptorIndices = std::move(program.descriptorIndices);
    data.constants[CLOD_VIRTUAL_SHADOW_GATHER_STATS_PAGE_TABLE_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.pageTable).index;
    data.constants[CLOD_VIRTUAL_SHADOW_GATHER_STATS_ALLOCATION_COUNT_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.allocationCount).index;
    data.constants[CLOD_VIRTUAL_SHADOW_GATHER_STATS_ALLOCATION_INDIRECT_ARGS_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.allocationArgs).index;
    data.constants[CLOD_VIRTUAL_SHADOW_GATHER_STATS_PAGE_LIST_HEADER_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.header).index;
    data.constants[CLOD_VIRTUAL_SHADOW_GATHER_STATS_CLIPMAP_INFO_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.clipmapInfo).index;
    data.constants[CLOD_VIRTUAL_SHADOW_GATHER_STATS_PAGE_METADATA_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.pageMetadata).index;
    data.constants[CLOD_VIRTUAL_SHADOW_GATHER_STATS_STATS_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.stats).index;
    data.constants[CLOD_VIRTUAL_SHADOW_GATHER_STATS_PAGE_TABLE_RESOLUTION] = config.pageTableResolution;
    data.constants[CLOD_VIRTUAL_SHADOW_GATHER_STATS_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
    data.constants[CLOD_VIRTUAL_SHADOW_GATHER_STATS_CAPTURE_PRE_ALLOCATE_STATE] = bindings.capturePreAllocateState ? 1u : 0u;
    data.groupsX = (config.pageTableResolution + 7u) / 8u; data.groupsY = data.groupsX; data.groupsZ = CLodVirtualShadowMaxSupportedClipmapCount;
    return data;
}

void VirtualShadowMapGatherStatsPass::ShutdownPass() {}

void VirtualShadowMapGatherStatsPass::Record(const VirtualShadowMapGatherStatsBindings&,
    const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeDispatch(data, recording);
}
