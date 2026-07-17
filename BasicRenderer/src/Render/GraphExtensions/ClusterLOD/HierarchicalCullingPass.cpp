#include "Render/GraphExtensions/ClusterLOD/HierarchicalCullingPass.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <rhi_feature_info.h>
#include <rhi_interop.h>
#include <rhi_interop_dx12.h>
#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

#include "Managers\IndirectCommandBufferManager.h"
#include "Managers\MaterialManager.h"
#include "Managers\ObjectManager.h"
#include "Managers/Singletons/CommandSignatureManager.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/RendererECSManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/ViewManager.h"
#include "Render/GraphExtensions/CLodTelemetry.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Resources/components.h"
#include "Resources/Resolvers/ECSResourceResolver.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "BuiltinResources.h"
#include "ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodClearUintBufferRootConstants.h"
#include "../shaders/PerPassRootConstants/clodCreateCommandRootConstants.h"
#include "../shaders/PerPassRootConstants/clodWorkGraphRootConstants.h"

namespace {

bool SarpClodImportDebugLoggingEnabled()
{
    static const bool enabled = [] {
        char* value = nullptr;
        size_t length = 0;
        if (_dupenv_s(&value, &length, "SARP_DEBUG_CLOD_IMPORT") != 0 || value == nullptr) {
            return false;
        }
        const bool result = length > 1 && value[0] != '0';
        std::free(value);
        return result;
    }();
    return enabled;
}

uint64_t GetNativeBufferDeviceAddress(rhi::Resource resource) noexcept
{
    if (ID3D12Resource* nativeResource = rhi::dx12::get_resource(resource)) {
        return nativeResource->GetGPUVirtualAddress();
    }

    rhi::VulkanResourceInfo vulkanInfo{};
    if (rhi::QueryNativeResource(resource, rhi::RHI_IID_VK_RESOURCE, &vulkanInfo, sizeof(vulkanInfo))) {
        return vulkanInfo.deviceAddress;
    }

    return 0u;
}

constexpr bool kDisableVirtualShadowDirtyPageCulling = false;

bool UsesSWClassification(HierarchicalCullingWorkGraphMode mode)
{
    return mode != HierarchicalCullingWorkGraphMode::HardwareOnly;
}

bool UsesWorkGraphSWRaster(HierarchicalCullingWorkGraphMode mode)
{
    return mode == HierarchicalCullingWorkGraphMode::SoftwareRasterWorkGraph;
}

bool UsesVisibilityBufferOutput(CLodRasterOutputKind outputKind)
{
    return outputKind == CLodRasterOutputKind::VisibilityBuffer;
}

bool UsesVirtualShadowOutput(CLodRasterOutputKind outputKind)
{
    return outputKind == CLodRasterOutputKind::VirtualShadow;
}

bool UsesPerViewDepthMapOcclusion(CLodRasterOutputKind outputKind)
{
    return !UsesVirtualShadowOutput(outputKind);
}

template <typename T>
bool BytesEqual(const T& left, const T& right)
{
    return std::memcmp(&left, &right, sizeof(T)) == 0;
}

ViewFilter GetCullViewFilter(bool useShadowCascadeViews)
{
    if (!useShadowCascadeViews) {
        return ViewFilter::PrimaryCameras();
    }

    ViewFilter filter = ViewFilter::Shadows();
    filter.requireCascade = true;
    filter.requireLightType = true;
    filter.lightType = Components::LightType::Directional;
    return filter;
}

std::vector<uint64_t> CollectDeclaredDrawSetResourceIds(RenderPhase renderPhase, bool clodOnlyWorkloads)
{
    auto& ecsWorld = RendererECSManager::GetInstance().GetWorld();
    auto queryBuilder = ecsWorld.query_builder<>()
        .with<Components::IsActiveDrawSetIndices>()
        .with<Components::ParticipatesInPass>(RendererECSManager::GetInstance().GetRenderPhaseEntity(renderPhase));
    if (clodOnlyWorkloads) {
        queryBuilder.with<Components::CLodOnlyDrawWorkload>();
    }
    else {
        queryBuilder.with<Components::GeneralDrawWorkload>();
    }

    std::vector<uint64_t> resourceIds;
    queryBuilder.build().each([&](flecs::entity entity) {
        if (const auto resource = entity.try_get<Components::Resource>(); resource) {
            if (const auto shared = resource->resource.lock(); shared) {
                resourceIds.push_back(shared->GetGlobalResourceID());
            }
        }
    });

    std::sort(resourceIds.begin(), resourceIds.end());
    resourceIds.erase(std::unique(resourceIds.begin(), resourceIds.end()), resourceIds.end());
    return resourceIds;
}
}

HierarchicalCullingPass::HierarchicalCullingPass(
    std::string stablePassIdentifier,
    HierarchicalCullingPassInputs inputs,
    std::shared_ptr<Buffer> visibleClustersBuffer,
    std::shared_ptr<Buffer> visibleClusterTransformIndicesBuffer,
    std::shared_ptr<Buffer> visibleClustersCounterBuffer,
    std::shared_ptr<Buffer> swVisibleClustersCounterBuffer,
    std::shared_ptr<Buffer> voxelRasterWorkBuffer,
    std::shared_ptr<Buffer> voxelRasterWorkCounterBuffer,
    std::shared_ptr<Buffer> skinnedVoxelRasterWorkBuffer,
    std::shared_ptr<Buffer> skinnedVoxelRasterWorkCounterBuffer,
    uint32_t voxelRasterWorkCapacity,
    std::shared_ptr<Buffer> pageJobVisibleClustersBuffer,
    std::shared_ptr<Buffer> pageJobVisibleClusterTransformIndicesBuffer,
    std::shared_ptr<Buffer> pageJobVisibleClustersCounterBuffer,
    std::shared_ptr<Buffer> histogramIndirectCommand,
    std::shared_ptr<Buffer> workGraphTelemetryBuffer,
    std::shared_ptr<Buffer> occlusionReplayBuffer,
    std::shared_ptr<Buffer> occlusionReplayStateBuffer,
    std::shared_ptr<Buffer> occlusionNodeGpuInputsBuffer,
    std::shared_ptr<Buffer> viewDepthSrvIndicesBuffer,
    std::shared_ptr<Buffer> viewRasterInfoBuffer,
    std::shared_ptr<PixelBuffer> shadowDirtyHierarchyTexture,
    std::shared_ptr<ResourceGroup> slabResourceGroup,
    std::shared_ptr<Buffer> phase1VisibleClustersCounterBuffer,
    std::shared_ptr<Buffer> swWriteBaseCounterBuffer,
    std::shared_ptr<Buffer> shadowPredictiveInvalidationCandidatesBuffer,
    std::shared_ptr<Buffer> shadowPredictiveInvalidationCandidateCountBuffer,
    std::shared_ptr<Buffer> shadowInvalidatedInstancesBitsetBuffer,
    std::shared_ptr<PixelBuffer> shadowPageTableTexture,
    std::shared_ptr<PixelBuffer> shadowPhysicalPagesTexture,
    std::shared_ptr<Buffer> reyesDiceQueueBuffer,
    std::shared_ptr<Buffer> reyesDiceQueueCounterBuffer,
    std::shared_ptr<Buffer> reyesDiceQueueOverflowBuffer,
    std::shared_ptr<Buffer> reyesTessTableConfigsBuffer,
    std::shared_ptr<Buffer> reyesTessTableVerticesBuffer,
    std::shared_ptr<Buffer> reyesTessTableTrianglesBuffer,
    std::shared_ptr<Buffer> reyesTelemetryBuffer,
    uint32_t reyesDiceQueueCapacity) {
    m_workGraphMode = inputs.workGraphMode;
    m_rasterOutputKind = inputs.rasterOutputKind;
    m_isFirstPass = inputs.isFirstPass;
    m_workGraphReyesVisibility = inputs.workGraphReyesVisibility;
    m_workGraphComputePageJobDescriptorResourceId =
        std::string(CLodWorkGraphComputePageJobDescriptorBufferId) + "." + std::move(stablePassIdentifier);
    m_voxelRasterQueueDescriptorResourceId =
        std::string(CLodVoxelRasterQueueDescriptorBufferId) + "." + m_workGraphComputePageJobDescriptorResourceId;
    CreatePipelines(
        DeviceManager::GetInstance().GetDevice(),
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        m_workGraph,
        m_createCommandPipelineState,
        m_clearPipelineState);
    if (!m_workGraph) {
        spdlog::error(
            "HierarchicalCullingPass::HierarchicalCullingPass CreatePipelines returned null work graph this={} workGraphMode={} rasterOutputKind={}",
            static_cast<const void*>(this),
            static_cast<int>(m_workGraphMode),
            static_cast<int>(m_rasterOutputKind));
    }
    auto memSize = m_workGraph->GetRequiredScratchMemorySize();
    m_scratchBuffer = Buffer::CreateShared(
        rhi::HeapType::DeviceLocal,
        memSize,
        true);
    m_scratchBuffer->SetName("CLod Work Graph Scratch Buffer");
    m_scratchBuffer->SetMemoryUsageHint("Work graph scratch buffer");
    m_visibleClustersBuffer = std::move(visibleClustersBuffer);
    m_visibleClusterTransformIndicesBuffer = std::move(visibleClusterTransformIndicesBuffer);
    m_visibleClustersCounterBuffer = std::move(visibleClustersCounterBuffer);
    m_swVisibleClustersCounterBuffer = std::move(swVisibleClustersCounterBuffer);
    m_voxelRasterWorkBuffer = std::move(voxelRasterWorkBuffer);
    m_voxelRasterWorkCounterBuffer = std::move(voxelRasterWorkCounterBuffer);
    m_skinnedVoxelRasterWorkBuffer = std::move(skinnedVoxelRasterWorkBuffer);
    m_skinnedVoxelRasterWorkCounterBuffer = std::move(skinnedVoxelRasterWorkCounterBuffer);
    m_voxelRasterWorkCapacity = voxelRasterWorkCapacity;
    m_voxelRasterQueueDescriptorsBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        1,
        sizeof(CLodVoxelRasterQueueDescriptors),
        false,
        false,
        false,
        false);
    m_voxelRasterQueueDescriptorsBuffer->SetName("CLod Voxel Raster Queue Descriptors");
    rg::memory::SetResourceUsageHint(*m_voxelRasterQueueDescriptorsBuffer, "Cluster LOD work graph");
    m_pageJobVisibleClustersBuffer = std::move(pageJobVisibleClustersBuffer);
    m_pageJobVisibleClusterTransformIndicesBuffer = std::move(pageJobVisibleClusterTransformIndicesBuffer);
    m_pageJobVisibleClustersCounterBuffer = std::move(pageJobVisibleClustersCounterBuffer);
    m_workGraphComputePageJobDescriptorsBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        1,
        sizeof(CLodWorkGraphComputePageJobDescriptors),
        false,
        false,
        false,
        false);
    m_workGraphComputePageJobDescriptorsBuffer->SetName("CLod Work Graph Compute Page Job Descriptors");
    rg::memory::SetResourceUsageHint(*m_workGraphComputePageJobDescriptorsBuffer, "Cluster LOD work graph");
    m_histogramIndirectCommand = std::move(histogramIndirectCommand);
    m_workGraphTelemetryBuffer = std::move(workGraphTelemetryBuffer);
    m_occlusionReplayBuffer = std::move(occlusionReplayBuffer);
    m_occlusionReplayStateBuffer = std::move(occlusionReplayStateBuffer);
    m_occlusionNodeGpuInputsBuffer = std::move(occlusionNodeGpuInputsBuffer);
    m_viewDepthSrvIndicesBuffer = std::move(viewDepthSrvIndicesBuffer);
    m_viewRasterInfoBuffer = std::move(viewRasterInfoBuffer);
    m_shadowPredictiveInvalidationCandidatesBuffer = std::move(shadowPredictiveInvalidationCandidatesBuffer);
    m_shadowPredictiveInvalidationCandidateCountBuffer = std::move(shadowPredictiveInvalidationCandidateCountBuffer);
    m_shadowInvalidatedInstancesBitsetBuffer = std::move(shadowInvalidatedInstancesBitsetBuffer);
    m_shadowDirtyHierarchyTexture = std::move(shadowDirtyHierarchyTexture);
    m_shadowPageTableTexture = std::move(shadowPageTableTexture);
    m_shadowPhysicalPagesTexture = std::move(shadowPhysicalPagesTexture);
    m_reyesDiceQueueBuffer = std::move(reyesDiceQueueBuffer);
    m_reyesDiceQueueCounterBuffer = std::move(reyesDiceQueueCounterBuffer);
    m_reyesDiceQueueOverflowBuffer = std::move(reyesDiceQueueOverflowBuffer);
    m_reyesTessTableConfigsBuffer = std::move(reyesTessTableConfigsBuffer);
    m_reyesTessTableVerticesBuffer = std::move(reyesTessTableVerticesBuffer);
    m_reyesTessTableTrianglesBuffer = std::move(reyesTessTableTrianglesBuffer);
    m_reyesTelemetryBuffer = std::move(reyesTelemetryBuffer);
    m_reyesDiceQueueCapacity = reyesDiceQueueCapacity;
    m_slabResourceGroup = std::move(slabResourceGroup);
    m_phase1VisibleClustersCounterBuffer = std::move(phase1VisibleClustersCounterBuffer);
    m_swWriteBaseCounterBuffer = std::move(swWriteBaseCounterBuffer);
    m_maxVisibleClusters = inputs.maxVisibleClusters;
    m_renderPhase = std::move(inputs.renderPhase);
    m_clodOnlyWorkloads = inputs.clodOnlyWorkloads;
    m_useShadowCascadeViews = inputs.useShadowCascadeViews;
}

HierarchicalCullingPass::~HierarchicalCullingPass() = default;

void HierarchicalCullingPass::DeclareResourceUsages(ComputePassBuilder* builder) {
    const ResourceState computeReadState{
        rhi::ResourceAccessType::ShaderResource,
        rhi::ResourceLayout::ShaderResource,
        rhi::ResourceSyncState::ComputeShading
    };

    auto& ecsWorld = RendererECSManager::GetInstance().GetWorld();
    auto queryBuilder = ecsWorld.query_builder<>()
        .with<Components::IsActiveDrawSetIndices>()
        .with<Components::ParticipatesInPass>(RendererECSManager::GetInstance().GetRenderPhaseEntity(m_renderPhase));
    if (m_clodOnlyWorkloads) {
        queryBuilder.with<Components::CLodOnlyDrawWorkload>();
    }
    else {
        queryBuilder.with<Components::GeneralDrawWorkload>();
    }
    flecs::query<> drawSetIndicesQuery = queryBuilder.build();
    builder->WithUnorderedAccess(
            m_scratchBuffer,
            m_visibleClustersBuffer,
            m_visibleClusterTransformIndicesBuffer,
            m_visibleClustersCounterBuffer,
            m_histogramIndirectCommand,
            m_workGraphTelemetryBuffer,
            m_voxelRasterWorkBuffer,
            m_voxelRasterWorkCounterBuffer,
            m_skinnedVoxelRasterWorkBuffer,
            m_skinnedVoxelRasterWorkCounterBuffer,
            m_occlusionReplayBuffer,
            m_occlusionReplayStateBuffer,
            m_occlusionNodeGpuInputsBuffer)
        .WithUnorderedAccess(
            Builtin::CLod::StreamingLoadRequestKeys,
            Builtin::CLod::StreamingLoadRequests,
            Builtin::CLod::StreamingLoadCounter,
            Builtin::CLod::StreamingRuntimeState,
            Builtin::CLod::StreamingTouchedGroupsCounter,
            Builtin::CLod::StreamingTouchedGroups)
        .WithShaderResource(
            Builtin::IndirectCommandBuffers::Master,
            Builtin::CLod::Offsets,
            Builtin::CLod::GroupChunks,
            Builtin::CLod::Groups,
            Builtin::CLod::Segments,
            Builtin::CLod::Nodes,
			Builtin::CLod::NodeSkinningInfos,
			Builtin::CLod::NodeBoneIndices,
            Builtin::CLod::AssemblyInstances,
            Builtin::CLod::AssemblyTransforms,
            Builtin::CLod::AssemblyBoneRemaps,
            Builtin::CLod::AssemblyBoneRemapIndices,
            Builtin::CLod::StreamingActiveGroupsBits,
            Builtin::CLod::StreamingNonResidentBits,
            Builtin::CLod::MeshMetadata,
            CLodLevelInfosBufferId,
            Builtin::CLod::GroupPageMap,
            Builtin::CullingCameraBuffer,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::SkinnedAssemblyPlacements,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerObjectBuffer,
            Builtin::CameraBuffer,
            Builtin::PerMeshBuffer,
            Builtin::SkeletonResources::InverseBindMatrices,
            Builtin::SkeletonResources::InverseSkinMatrices,
            Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo,
            m_visibleClustersCounterBuffer,
            m_occlusionReplayStateBuffer,
            Builtin::PerMaterialDataBuffer,
            Builtin::Material::TextureGroup,
            Builtin::Material::TextureStreamingMetadataBuffer,
            m_workGraphComputePageJobDescriptorResourceId.c_str(),
            m_voxelRasterQueueDescriptorResourceId.c_str())
    		.WithUnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer)
        .WithShaderResource(ECSResourceResolver(drawSetIndicesQuery));

    if (UsesPerViewDepthMapOcclusion(m_rasterOutputKind)) {
        builder->WithUnorderedAccess(m_viewDepthSrvIndicesBuffer)
            .WithShaderResource(m_isFirstPass
                ? Builtin::LastFrameLinearDepthMaps
                : Builtin::PrimaryCamera::LinearDepthMap);
    }

    if (UsesSWClassification(m_workGraphMode)) {
        builder->WithUnorderedAccess(m_swVisibleClustersCounterBuffer);
    }
    if (m_pageJobVisibleClustersBuffer && m_pageJobVisibleClusterTransformIndicesBuffer && m_pageJobVisibleClustersCounterBuffer) {
        builder->WithUnorderedAccess(
            m_pageJobVisibleClustersBuffer,
            m_pageJobVisibleClusterTransformIndicesBuffer,
            m_pageJobVisibleClustersCounterBuffer);
    }
    if (UsesSWClassification(m_workGraphMode)) {
        builder->WithShaderResource(m_viewRasterInfoBuffer);
    }
    if (UsesVirtualShadowOutput(m_rasterOutputKind)) {
        if (m_shadowPredictiveInvalidationCandidatesBuffer && m_shadowPredictiveInvalidationCandidateCountBuffer) {
            builder->WithUnorderedAccess(
                m_shadowPredictiveInvalidationCandidatesBuffer,
                m_shadowPredictiveInvalidationCandidateCountBuffer);
        }
        builder->WithShaderResource(
            Builtin::Shadows::CLodClipmapInfo,
            Builtin::Shadows::CLodCompactShadowCameras,
            m_shadowDirtyHierarchyTexture)
            .WithUnorderedAccess(Builtin::Shadows::CLodPageTable);
        if (m_shadowInvalidatedInstancesBitsetBuffer) {
            builder->WithShaderResource(m_shadowInvalidatedInstancesBitsetBuffer);
        }
    }
    if (UsesWorkGraphSWRaster(m_workGraphMode) && UsesVirtualShadowOutput(m_rasterOutputKind)) {
        builder->WithUnorderedAccess(Builtin::Shadows::CLodPhysicalPages);
    }
    if (m_workGraphReyesVisibility) {
        builder->WithUnorderedAccess(
                m_reyesDiceQueueBuffer,
                m_reyesDiceQueueCounterBuffer,
                m_reyesDiceQueueOverflowBuffer,
                m_reyesTelemetryBuffer)
            .WithShaderResource(
                m_reyesTessTableConfigsBuffer,
                m_reyesTessTableVerticesBuffer,
                m_reyesTessTableTrianglesBuffer,
                Builtin::PerMaterialOpenPBRDataBuffer,
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
                Builtin::Terrain::RvtMaterialAtlas)
            .WithUnorderedAccess(
                Builtin::Terrain::RvtRequestMasks,
                Builtin::Terrain::RvtRequestList,
                Builtin::Terrain::RvtCounters,
                Builtin::Terrain::RvtStats);
    }

    // Phase 2 reads Phase 1's HW counter to offset writes in the visible clusters buffer.
    if (m_phase1VisibleClustersCounterBuffer) {
        builder->WithShaderResource(m_phase1VisibleClustersCounterBuffer);
    }
    if (UsesSWClassification(m_workGraphMode) && m_swWriteBaseCounterBuffer) {
        builder->WithShaderResource(m_swWriteBaseCounterBuffer);
    }

    // Declare visibility buffer UAVs for SW raster render graph tracking.
    if (UsesWorkGraphSWRaster(m_workGraphMode) && UsesVisibilityBufferOutput(m_rasterOutputKind)) {
        for (auto& vb : m_visibilityBuffers) {
            builder->WithUnorderedAccess(vb);
        }
    }
    builder->WithUnorderedAccess(Builtin::DebugVisualization);

    // Declare page pool slabs for bindless access (auto-invalidates when new slabs are added).
    if (m_slabResourceGroup) {
        builder->WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
    }

    builder->WithInternalTransition(m_visibleClustersCounterBuffer, computeReadState)
        .WithInternalTransition(m_occlusionReplayStateBuffer, computeReadState)
        .WithConstantBuffer(Builtin::PerFrameBuffer);
}

void HierarchicalCullingPass::Setup() {
	RegisterSRV(Builtin::CLod::NodeSkinningInfos);
	RegisterSRV(Builtin::CLod::NodeBoneIndices);
    if (UsesWorkGraphSWRaster(m_workGraphMode) && UsesVirtualShadowOutput(m_rasterOutputKind)) {
        RegisterSRV(SRVViewType::Texture2DArrayFull, Builtin::Shadows::CLodPageTable);
    }
}

PassReturn HierarchicalCullingPass::Execute(PassExecutionContext& executionContext) {
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());

    if (m_pageJobVisibleClustersCounterBuffer) {
        BindResourceDescriptorIndices(commandList, m_clearPipelineState.GetResourceDescriptorSlots());
        commandList.BindPipeline(m_clearPipelineState.GetAPIPipelineState().GetHandle());

        uint32_t clearRootConstants[NumMiscUintRootConstants] = {};
        clearRootConstants[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] =
            m_pageJobVisibleClustersCounterBuffer->GetUAVShaderVisibleInfo(0).slot.index;
        clearRootConstants[CLOD_CLEAR_UINT_BUFFER_VALUE] = 0u;
        clearRootConstants[CLOD_CLEAR_UINT_BUFFER_COUNT] = 1u;
        commandList.PushConstants(
            rhi::ShaderStage::Compute,
            0,
            MiscUintRootSignatureIndex,
            0,
            NumMiscUintRootConstants,
            clearRootConstants);
        commandList.Dispatch(1u, 1u, 1u);

        rhi::BufferBarrier pageJobCounterBarrier{};
        pageJobCounterBarrier.buffer = m_pageJobVisibleClustersCounterBuffer->GetAPIResource().GetHandle();
        pageJobCounterBarrier.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
        pageJobCounterBarrier.afterAccess = rhi::ResourceAccessType::UnorderedAccess;
        pageJobCounterBarrier.beforeSync = rhi::ResourceSyncState::ComputeShading;
        pageJobCounterBarrier.afterSync = rhi::ResourceSyncState::ComputeShading;

        rhi::BarrierBatch clearBarrierBatch{};
        clearBarrierBatch.buffers = { &pageJobCounterBarrier };
        commandList.Barriers(clearBarrierBatch);
    }

    auto clearCounterBuffer = [&](const std::shared_ptr<Buffer>& buffer) {
        if (!buffer) {
            return;
        }
        BindResourceDescriptorIndices(commandList, m_clearPipelineState.GetResourceDescriptorSlots());
        commandList.BindPipeline(m_clearPipelineState.GetAPIPipelineState().GetHandle());

        uint32_t clearRootConstants[NumMiscUintRootConstants] = {};
        clearRootConstants[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] =
            buffer->GetUAVShaderVisibleInfo(0).slot.index;
        clearRootConstants[CLOD_CLEAR_UINT_BUFFER_VALUE] = 0u;
        clearRootConstants[CLOD_CLEAR_UINT_BUFFER_COUNT] = 1u;
        commandList.PushConstants(
            rhi::ShaderStage::Compute,
            0,
            MiscUintRootSignatureIndex,
            0,
            NumMiscUintRootConstants,
            clearRootConstants);
        commandList.Dispatch(1u, 1u, 1u);

        rhi::BufferBarrier voxelCounterBarrier{};
        voxelCounterBarrier.buffer = buffer->GetAPIResource().GetHandle();
        voxelCounterBarrier.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
        voxelCounterBarrier.afterAccess = rhi::ResourceAccessType::UnorderedAccess;
        voxelCounterBarrier.beforeSync = rhi::ResourceSyncState::ComputeShading;
        voxelCounterBarrier.afterSync = rhi::ResourceSyncState::ComputeShading;

        rhi::BarrierBatch clearBarrierBatch{};
        clearBarrierBatch.buffers = { &voxelCounterBarrier };
        commandList.Barriers(clearBarrierBatch);
    };
    clearCounterBuffer(m_voxelRasterWorkCounterBuffer);
    clearCounterBuffer(m_skinnedVoxelRasterWorkCounterBuffer);

    if (m_workGraphReyesVisibility && m_isFirstPass) {
        BindResourceDescriptorIndices(commandList, m_clearPipelineState.GetResourceDescriptorSlots());
        commandList.BindPipeline(m_clearPipelineState.GetAPIPipelineState().GetHandle());

        auto clearUintBuffer = [&](const std::shared_ptr<Buffer>& buffer) {
            uint32_t clearRootConstants[NumMiscUintRootConstants] = {};
            clearRootConstants[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] =
                buffer->GetUAVShaderVisibleInfo(0).slot.index;
            clearRootConstants[CLOD_CLEAR_UINT_BUFFER_VALUE] = 0u;
            clearRootConstants[CLOD_CLEAR_UINT_BUFFER_COUNT] = 1u;
            commandList.PushConstants(
                rhi::ShaderStage::Compute,
                0,
                MiscUintRootSignatureIndex,
                0,
                NumMiscUintRootConstants,
                clearRootConstants);
            commandList.Dispatch(1u, 1u, 1u);
        };

        clearUintBuffer(m_reyesDiceQueueCounterBuffer);
        clearUintBuffer(m_reyesDiceQueueOverflowBuffer);

        std::array<rhi::BufferBarrier, 2> reyesCounterBarriers{};
        reyesCounterBarriers[0].buffer = m_reyesDiceQueueCounterBuffer->GetAPIResource().GetHandle();
        reyesCounterBarriers[1].buffer = m_reyesDiceQueueOverflowBuffer->GetAPIResource().GetHandle();
        for (auto& barrier : reyesCounterBarriers) {
            barrier.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
            barrier.afterAccess = rhi::ResourceAccessType::UnorderedAccess;
            barrier.beforeSync = rhi::ResourceSyncState::ComputeShading;
            barrier.afterSync = rhi::ResourceSyncState::ComputeShading;
        }
        rhi::BarrierBatch clearBarrierBatch{};
        clearBarrierBatch.buffers = rhi::Span<rhi::BufferBarrier>(reyesCounterBarriers.data(), static_cast<uint32_t>(reyesCounterBarriers.size()));
        commandList.Barriers(clearBarrierBatch);
    }

    uint32_t uintRootConstants[NumMiscUintRootConstants] = {};
    uintRootConstants[CLOD_WG_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = m_visibleClustersBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_WG_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = m_visibleClusterTransformIndicesBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_WG_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = m_visibleClustersCounterBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_WG_SW_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = m_swVisibleClustersCounterBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_WG_HW_WRITE_BASE_COUNTER_DESCRIPTOR_INDEX] = m_histogramIndirectCommand->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_WG_TELEMETRY_DESCRIPTOR_INDEX] = m_workGraphTelemetryBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_WG_FORCED_TRAVERSAL_DEPTH_ROOT] =
        SettingsManager::GetInstance().getSettingGetter<uint32_t>(CLodForceTraversalDepthRootSettingName)();
    if (UsesSWClassification(m_workGraphMode)) {
        uintRootConstants[CLOD_WG_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX] = m_viewRasterInfoBuffer->GetSRVInfo(0).slot.index;
    }
    uint32_t workGraphFlags = 0u;
    if (IsCLodWorkGraphTelemetryEnabled()) {
        workGraphFlags |= CLOD_WG_FLAG_TELEMETRY_ENABLED;
    }
    if (UsesPerViewDepthMapOcclusion(m_rasterOutputKind) &&
        SettingsManager::GetInstance().getSettingGetter<bool>("enableOcclusionCulling")()) {
        workGraphFlags |= CLOD_WG_FLAG_OCCLUSION_ENABLED;
    }
    const bool enableSoftwareRaster = UsesSWClassification(m_workGraphMode);
    if (enableSoftwareRaster) {
        workGraphFlags |= CLOD_WG_FLAG_SW_RASTER_ENABLED;
    }
    if (m_workGraphMode == HierarchicalCullingWorkGraphMode::SoftwareRasterCompute) {
        workGraphFlags |= CLOD_WG_FLAG_COMPUTE_SW_RASTER;
    }
    if (kDisableVirtualShadowDirtyPageCulling && UsesVirtualShadowOutput(m_rasterOutputKind)) {
        workGraphFlags |= CLOD_WG_FLAG_DISABLE_SHADOW_DIRTY_PAGE_CULLING;
    }
    if (UsesVirtualShadowOutput(m_rasterOutputKind) &&
        SettingsManager::GetInstance().getSettingGetter<bool>(CLodDirectionalVirtualShadowPredictiveLodInvalidationSettingName)()) {
        workGraphFlags |= CLOD_WG_FLAG_VSM_PREDICTIVE_LOD_INVALIDATION;
    }
    constexpr uint32_t swRasterThreshold = 16; // pixel diameter threshold
    workGraphFlags |= (swRasterThreshold << CLOD_WG_SW_RASTER_THRESHOLD_SHIFT);
    if (!m_isFirstPass) {
        workGraphFlags |= CLOD_WG_FLAG_PHASE2;
    }
    uintRootConstants[CLOD_WG_FLAGS] = workGraphFlags;

    // Pack page-job VSM flags
    uint32_t pageJobFlags = 0;
    {
        auto& settings = SettingsManager::GetInstance();
        const bool pageJobEnabled =
            CLodVSMRasterModeUsesLargeClusterShadowRouting(
                settings.getSettingGetter<CLodVSMRasterMode>(CLodVSMRasterModeSettingName)());
        if (pageJobEnabled && UsesVirtualShadowOutput(m_rasterOutputKind)) {
            pageJobFlags |= CLOD_WG_PAGE_JOB_FLAG_ENABLED;
        }
        const bool pageJobForceAll = settings.getSettingGetter<bool>(CLodPageJobForceAllSettingName)();
        if (pageJobForceAll) {
            pageJobFlags |= CLOD_WG_PAGE_JOB_FLAG_FORCE_ALL;
        }
        const uint32_t diameterThreshold = std::min(settings.getSettingGetter<uint32_t>(CLodPageJobDiameterThresholdSettingName)(), 255u);
        pageJobFlags |= (diameterThreshold << CLOD_WG_PAGE_JOB_DIAMETER_THRESHOLD_SHIFT);
        const float sparseRatio = settings.getSettingGetter<float>(CLodPageJobSparseRatioSettingName)();
        const uint32_t sparseRatioEncoded = std::min(static_cast<uint32_t>(sparseRatio * 255.0f + 0.5f), 255u);
        pageJobFlags |= (sparseRatioEncoded << CLOD_WG_PAGE_JOB_SPARSE_RATIO_SHIFT);
        const uint32_t maxPages = std::min(settings.getSettingGetter<uint32_t>(CLodPageJobMaxPagesPerClusterSettingName)(), 255u);
        pageJobFlags |= (maxPages << CLOD_WG_PAGE_JOB_MAX_PAGES_SHIFT);
    }
    uintRootConstants[CLOD_WG_PAGE_JOB_FLAGS] = pageJobFlags;
    if (m_shadowPageTableTexture) {
        uintRootConstants[CLOD_WG_VIRTUAL_SHADOW_PAGE_TABLE_UAV_DESCRIPTOR_INDEX] =
            m_shadowPageTableTexture->GetUAVShaderVisibleInfo(UAVViewType::Texture2DArrayFull, 0).slot.index;
    }
    if (m_shadowPhysicalPagesTexture) {
        uintRootConstants[CLOD_WG_VIRTUAL_SHADOW_PHYSICAL_PAGES_UAV_DESCRIPTOR_INDEX] =
            m_shadowPhysicalPagesTexture->GetUAVShaderVisibleInfo(0).slot.index;
    }
    if (m_workGraphReyesVisibility) {
        uintRootConstants[CLOD_WG_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] =
            m_reyesDiceQueueBuffer->GetUAVShaderVisibleInfo(0).slot.index;
        uintRootConstants[CLOD_WG_REYES_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] =
            m_reyesDiceQueueCounterBuffer->GetUAVShaderVisibleInfo(0).slot.index;
        uintRootConstants[CLOD_WG_REYES_DICE_QUEUE_OVERFLOW_DESCRIPTOR_INDEX] =
            m_reyesDiceQueueOverflowBuffer->GetUAVShaderVisibleInfo(0).slot.index;
        uintRootConstants[CLOD_WG_REYES_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] =
            m_reyesTessTableConfigsBuffer->GetSRVInfo(0).slot.index;
        uintRootConstants[CLOD_WG_REYES_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX] =
            m_reyesTessTableVerticesBuffer->GetSRVInfo(0).slot.index;
        uintRootConstants[CLOD_WG_REYES_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX] =
            m_reyesTessTableTrianglesBuffer->GetSRVInfo(0).slot.index;
        uintRootConstants[CLOD_WG_REYES_TELEMETRY_DESCRIPTOR_INDEX] =
            m_reyesTelemetryBuffer->GetUAVShaderVisibleInfo(0).slot.index;
        uintRootConstants[CLOD_WG_REYES_DICE_QUEUE_CAPACITY] = m_reyesDiceQueueCapacity;
    }
    uintRootConstants[CLOD_WG_OCCLUSION_REPLAY_BUFFER_DESCRIPTOR_INDEX] = m_occlusionReplayBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_WG_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX] = m_occlusionReplayStateBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_WG_WORKGRAPH_NODE_INPUTS_DESCRIPTOR_INDEX] = m_occlusionNodeGpuInputsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_WG_VIEW_DEPTH_SRV_INDICES_DESCRIPTOR_INDEX] =
        UsesPerViewDepthMapOcclusion(m_rasterOutputKind)
            ? m_viewDepthSrvIndicesBuffer->GetSRVInfo(0).slot.index
            : 0u;
    uintRootConstants[CLOD_WG_VISIBLE_CLUSTERS_CAPACITY] = static_cast<uint32_t>(m_maxVisibleClusters);
    uintRootConstants[CLOD_WG_SHADOW_DIRTY_HIERARCHY_DESCRIPTOR_INDEX] =
        m_shadowDirtyHierarchyTexture
            ? m_shadowDirtyHierarchyTexture->GetSRVInfo(SRVViewType::Texture2DArrayFull, 0).slot.index
            : 0u;
    uintRootConstants[CLOD_WG_SHADOW_INVALIDATED_INSTANCES_DESCRIPTOR_INDEX] =
        m_shadowInvalidatedInstancesBitsetBuffer
            ? m_shadowInvalidatedInstancesBitsetBuffer->GetSRVInfo(0).slot.index
            : 0u;
    uintRootConstants[CLOD_WG_SHADOW_PREDICTIVE_INVALIDATION_CANDIDATES_DESCRIPTOR_INDEX] =
        m_shadowPredictiveInvalidationCandidatesBuffer
            ? m_shadowPredictiveInvalidationCandidatesBuffer->GetUAVShaderVisibleInfo(0).slot.index
            : 0u;
    uintRootConstants[CLOD_WG_SHADOW_PREDICTIVE_INVALIDATION_CANDIDATE_COUNT_DESCRIPTOR_INDEX] =
        m_shadowPredictiveInvalidationCandidateCountBuffer
            ? m_shadowPredictiveInvalidationCandidateCountBuffer->GetUAVShaderVisibleInfo(0).slot.index
            : 0u;

    // Always bind valid SRV descriptors for the aliased write-base slots.
    // Phase 1 does not read these counters, but the shader still forms StructuredBuffer
    // views from the aliased root constants before branching on the phase flag.
    uintRootConstants[CLOD_WG_HW_WRITE_BASE_COUNTER_DESCRIPTOR_INDEX] =
        (m_phase1VisibleClustersCounterBuffer ? m_phase1VisibleClustersCounterBuffer : m_visibleClustersCounterBuffer)
            ->GetSRVInfo(0)
            .slot.index;
    uintRootConstants[CLOD_WG_SW_WRITE_BASE_COUNTER_DESCRIPTOR_INDEX] =
        (m_swWriteBaseCounterBuffer ? m_swWriteBaseCounterBuffer : m_swVisibleClustersCounterBuffer)
            ->GetSRVInfo(0)
            .slot.index;

    commandList.SetWorkGraph(m_workGraph->GetHandle(), m_scratchBuffer->GetAPIResource().GetHandle(), true);

    BindResourceDescriptorIndices(commandList, m_pipelineResources);

    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        uintRootConstants);

    if (m_isFirstPass) {
        std::vector<ObjectCullRecord> cullRecords;

        ViewFilter filter = GetCullViewFilter(m_useShadowCascadeViews);
        context.viewManager->ForEachFiltered(filter, [&](uint64_t view) {
            auto viewInfo = context.viewManager->Get(view);
            auto cameraBufferIndex = viewInfo->gpu.cameraBufferIndex;
			auto workloads = context.indirectCommandBufferManager->GetViewIndirectBuffersForRenderPhase(
				view,
				m_renderPhase,
				m_clodOnlyWorkloads);
			for (auto& wl : workloads) {
				const auto activeDrawSetIndices = wl.workload.activeDrawSetIndices;
				if (!activeDrawSetIndices) {
					spdlog::warn(
						"HierarchicalCullingPass: skipping stale workload without active draw set indices flags={} phase={} clodOnly={} count={}",
						static_cast<std::uint64_t>(wl.key.compileFlags),
						wl.key.renderPhase.hash,
						wl.key.clodOnly,
						wl.workload.count);
					continue;
				}
				const auto count = wl.workload.activeDrawCount;
				if (count == 0) {
					continue;
				}
				ObjectCullRecord record{};
				record.viewDataIndex = cameraBufferIndex;
				record.activeDrawSetIndicesSRVIndex = activeDrawSetIndices->GetSRVInfo(0).slot.index;
				record.drawRecordVisibilityGenerationSRVIndex = context.objectManager->GetDrawRecordVisibilityGenerationBuffer()->GetSRVInfo(0).slot.index;
                record.activeDrawCount = count;
                record.dispatchGridX = static_cast<uint>((count + 63) / 64);
                record.dispatchGridY = 1;
                record.dispatchGridZ = 1;
                cullRecords.push_back(record);
            }
        });

        rhi::WorkGraphDispatchDesc dispatchDesc{};
        dispatchDesc.dispatchMode = rhi::WorkGraphDispatchMode::NodeCpuInput;
        dispatchDesc.nodeCpuInput.entryPointIndex = 0;
        dispatchDesc.nodeCpuInput.pRecords = cullRecords.data();
        dispatchDesc.nodeCpuInput.numRecords = static_cast<uint32_t>(cullRecords.size());
        dispatchDesc.nodeCpuInput.recordByteStride = sizeof(ObjectCullRecord);
        
        // Dispatching a zero-record work graph seems to break the driver on some platforms
        // It was reusing old dispatch records from a previous graph dispatch
		if (!cullRecords.empty()) {
            commandList.DispatchWorkGraph(dispatchDesc);
        }

        rhi::BufferBarrier postWorkGraphBarriers[3] = {};
        postWorkGraphBarriers[0].buffer = m_visibleClustersCounterBuffer->GetAPIResource().GetHandle();
        postWorkGraphBarriers[0].beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
        postWorkGraphBarriers[0].afterAccess = rhi::ResourceAccessType::UnorderedAccess;
        postWorkGraphBarriers[0].beforeSync = rhi::ResourceSyncState::ComputeShading;
        postWorkGraphBarriers[0].afterSync = rhi::ResourceSyncState::ComputeShading;
        postWorkGraphBarriers[1].buffer = m_occlusionReplayStateBuffer->GetAPIResource().GetHandle();
        postWorkGraphBarriers[1].beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
        postWorkGraphBarriers[1].afterAccess = rhi::ResourceAccessType::UnorderedAccess;
        postWorkGraphBarriers[1].beforeSync = rhi::ResourceSyncState::ComputeShading;
        postWorkGraphBarriers[1].afterSync = rhi::ResourceSyncState::ComputeShading;
        postWorkGraphBarriers[2].buffer = m_occlusionReplayBuffer->GetAPIResource().GetHandle();
        postWorkGraphBarriers[2].beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
        postWorkGraphBarriers[2].afterAccess = rhi::ResourceAccessType::UnorderedAccess;
        postWorkGraphBarriers[2].beforeSync = rhi::ResourceSyncState::ComputeShading;
        postWorkGraphBarriers[2].afterSync = rhi::ResourceSyncState::ComputeShading;
        rhi::BarrierBatch bufferBarriers{};
        bufferBarriers.buffers = rhi::Span<rhi::BufferBarrier>(postWorkGraphBarriers, 3);
        commandList.Barriers(bufferBarriers);
    }
    else {
        rhi::BufferBarrier replayDispatchBarriers[2] = {};
        replayDispatchBarriers[0].buffer = m_occlusionReplayBuffer->GetAPIResource().GetHandle();
        replayDispatchBarriers[0].beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
        replayDispatchBarriers[0].afterAccess = rhi::ResourceAccessType::UnorderedAccess;
        replayDispatchBarriers[0].beforeSync = rhi::ResourceSyncState::ComputeShading;
        replayDispatchBarriers[0].afterSync = rhi::ResourceSyncState::ComputeShading;

        replayDispatchBarriers[1].buffer = m_occlusionNodeGpuInputsBuffer->GetAPIResource().GetHandle();
        replayDispatchBarriers[1].beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
        replayDispatchBarriers[1].afterAccess = rhi::ResourceAccessType::UnorderedAccess;
        replayDispatchBarriers[1].beforeSync = rhi::ResourceSyncState::ComputeShading;
        replayDispatchBarriers[1].afterSync = rhi::ResourceSyncState::ComputeShading;

        rhi::BarrierBatch replayBarrierBatch{};
        replayBarrierBatch.buffers = rhi::Span<rhi::BufferBarrier>(replayDispatchBarriers, 2);
        commandList.Barriers(replayBarrierBatch);

        rhi::WorkGraphDispatchDesc replayDispatchDesc{};
        replayDispatchDesc.dispatchMode = rhi::WorkGraphDispatchMode::MultiNodeGpuInput;
        replayDispatchDesc.multiNodeGpuInput.inputBuffer = m_occlusionNodeGpuInputsBuffer->GetAPIResource().GetHandle();
        replayDispatchDesc.multiNodeGpuInput.inputAddressOffset = 0;
        commandList.DispatchWorkGraph(replayDispatchDesc);
    }

    std::array<rhi::BufferBarrier, 7> counterBarriers{};
    counterBarriers[0].buffer = m_visibleClustersCounterBuffer->GetAPIResource().GetHandle();
    counterBarriers[0].beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
    counterBarriers[0].afterAccess = rhi::ResourceAccessType::ShaderResource;
    counterBarriers[0].beforeSync = rhi::ResourceSyncState::ComputeShading;
    counterBarriers[0].afterSync = rhi::ResourceSyncState::ComputeShading;
    counterBarriers[1].buffer = m_occlusionReplayStateBuffer->GetAPIResource().GetHandle();
    counterBarriers[1].beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
    counterBarriers[1].afterAccess = rhi::ResourceAccessType::ShaderResource;
    counterBarriers[1].beforeSync = rhi::ResourceSyncState::ComputeShading;
    counterBarriers[1].afterSync = rhi::ResourceSyncState::ComputeShading;
    uint32_t counterBarrierCount = 2;
    if (UsesSWClassification(m_workGraphMode)) {
        counterBarriers[2].buffer = m_swVisibleClustersCounterBuffer->GetAPIResource().GetHandle();
        counterBarriers[2].beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
        counterBarriers[2].afterAccess = rhi::ResourceAccessType::UnorderedAccess;
        counterBarriers[2].beforeSync = rhi::ResourceSyncState::ComputeShading;
        counterBarriers[2].afterSync = rhi::ResourceSyncState::ComputeShading;
        counterBarrierCount = 3;
    }
    auto appendVoxelRasterReadBarrier = [&](const std::shared_ptr<Buffer>& buffer) {
        auto& barrier = counterBarriers[counterBarrierCount++];
        barrier.buffer = buffer->GetAPIResource().GetHandle();
        barrier.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
        barrier.afterAccess = rhi::ResourceAccessType::ShaderResource;
        barrier.beforeSync = rhi::ResourceSyncState::ComputeShading;
        barrier.afterSync = rhi::ResourceSyncState::ComputeShading;
    };
    appendVoxelRasterReadBarrier(m_voxelRasterWorkCounterBuffer);
    appendVoxelRasterReadBarrier(m_skinnedVoxelRasterWorkCounterBuffer);
    appendVoxelRasterReadBarrier(m_voxelRasterWorkBuffer);
    appendVoxelRasterReadBarrier(m_skinnedVoxelRasterWorkBuffer);
    rhi::BarrierBatch counterBarrierBatch{};
    counterBarrierBatch.buffers = rhi::Span<rhi::BufferBarrier>(counterBarriers.data(), counterBarrierCount);
    commandList.Barriers(counterBarrierBatch);

    BindResourceDescriptorIndices(commandList, m_createCommandPipelineState.GetResourceDescriptorSlots());
    // Reset aliased slots for CreateRasterBucketsHistogramCommandCSMain.
    // The work-graph dispatch binds slot 3/6 as UAV-facing descriptors, but the create-command
    // shader reads them as SRVs when building the indirect dispatch arguments.
    uintRootConstants[CLOD_CREATE_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = m_visibleClustersCounterBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_CREATE_RASTER_BUCKET_HISTOGRAM_COMMAND_DESCRIPTOR_INDEX] = m_histogramIndirectCommand->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_CREATE_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX] = m_occlusionReplayStateBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_CREATE_WORKGRAPH_NODE_INPUTS_DESCRIPTOR_INDEX] = m_occlusionNodeGpuInputsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_CREATE_NUM_RASTER_BUCKETS] = context.materialManager->GetRasterBucketCount();
    uintRootConstants[CLOD_CREATE_VISIBLE_CLUSTERS_CAPACITY] = static_cast<uint32_t>(m_maxVisibleClusters);
    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        uintRootConstants);

    commandList.BindPipeline(m_createCommandPipelineState.GetAPIPipelineState().GetHandle());
    commandList.Dispatch(1, 1, 1);

    return {};
}

void HierarchicalCullingPass::Update(const UpdateExecutionContext& executionContext) {
    ZoneScopedN("HierarchicalCullingPass::Update");

    auto* updateContext = executionContext.hostData ? executionContext.hostData->Get<UpdateContext>() : nullptr;
    if (!updateContext) {
        return;
    }
    auto& context = *updateContext;
    m_declaredResourcesChanged = false;

    {
        ZoneScopedN("HierarchicalCullingPass::CheckDeclaredDrawSetRevision");
        const uint64_t drawSetRevision = context.objectManager
            ? context.objectManager->GetDrawSetDeclarationRevision()
            : m_lastDrawSetDeclarationRevision + 1u;
        if (drawSetRevision != m_lastDrawSetDeclarationRevision) {
            ZoneScopedN("HierarchicalCullingPass::CollectDeclaredDrawSets");
            m_lastDrawSetDeclarationRevision = drawSetRevision;
            const std::vector<uint64_t> currentDrawSetResourceIds = CollectDeclaredDrawSetResourceIds(m_renderPhase, m_clodOnlyWorkloads);
            if (currentDrawSetResourceIds != m_declaredDrawSetResourceIds) {
                m_declaredDrawSetResourceIds = currentDrawSetResourceIds;
                m_declaredResourcesChanged = true;
                if (SarpClodImportDebugLoggingEnabled()) {
                    spdlog::info(
                        "SARPDBG HierarchicalCulling declared draw sets changed passFirst={} phase={} clodOnly={} count={} rev={}",
                        m_isFirstPass ? 1 : 0,
                        m_renderPhase.hash,
                        m_clodOnlyWorkloads ? 1 : 0,
                        m_declaredDrawSetResourceIds.size(),
                        drawSetRevision);
                }
            }
        }
    }

    uint32_t zero = 0u;
    {
        ZoneScopedN("HierarchicalCullingPass::UploadCounterResets");
        BUFFER_UPLOAD(&zero, sizeof(uint32_t), rg::runtime::UploadTarget::FromShared(m_visibleClustersCounterBuffer), 0);
        if (UsesSWClassification(m_workGraphMode)) {
            BUFFER_UPLOAD(&zero, sizeof(uint32_t), rg::runtime::UploadTarget::FromShared(m_swVisibleClustersCounterBuffer), 0);
        }
    }

    {
        ZoneScopedN("HierarchicalCullingPass::UpdateDescriptorTables");
        CLodVoxelRasterQueueDescriptors voxelQueueDescriptors{};
        voxelQueueDescriptors.rigidWorkRecordsUAVDescriptorIndex = m_voxelRasterWorkBuffer->GetUAVShaderVisibleInfo(0).slot.index;
        voxelQueueDescriptors.rigidWorkRecordCounterUAVDescriptorIndex = m_voxelRasterWorkCounterBuffer->GetUAVShaderVisibleInfo(0).slot.index;
        voxelQueueDescriptors.skinnedWorkRecordsUAVDescriptorIndex = m_skinnedVoxelRasterWorkBuffer->GetUAVShaderVisibleInfo(0).slot.index;
        voxelQueueDescriptors.skinnedWorkRecordCounterUAVDescriptorIndex = m_skinnedVoxelRasterWorkCounterBuffer->GetUAVShaderVisibleInfo(0).slot.index;
        voxelQueueDescriptors.workRecordCapacity = m_voxelRasterWorkCapacity;
        if (!m_hasCachedVoxelQueueDescriptors || !BytesEqual(voxelQueueDescriptors, m_cachedVoxelQueueDescriptors)) {
            m_cachedVoxelQueueDescriptors = voxelQueueDescriptors;
            m_hasCachedVoxelQueueDescriptors = true;
            BUFFER_UPLOAD(
                &voxelQueueDescriptors,
                sizeof(CLodVoxelRasterQueueDescriptors),
                rg::runtime::UploadTarget::FromShared(m_voxelRasterQueueDescriptorsBuffer),
                0);
        }

        CLodWorkGraphComputePageJobDescriptors pageJobDescriptors{};
        if (m_pageJobVisibleClustersBuffer && m_pageJobVisibleClusterTransformIndicesBuffer && m_pageJobVisibleClustersCounterBuffer) {
            pageJobDescriptors.visibleClustersUAVDescriptorIndex = m_pageJobVisibleClustersBuffer->GetUAVShaderVisibleInfo(0).slot.index;
            pageJobDescriptors.visibleClustersCounterUAVDescriptorIndex = m_pageJobVisibleClustersCounterBuffer->GetUAVShaderVisibleInfo(0).slot.index;
            pageJobDescriptors.visibleClusterTransformIndicesUAVDescriptorIndex =
                m_pageJobVisibleClusterTransformIndicesBuffer->GetUAVShaderVisibleInfo(0).slot.index;
        }
        if (!m_hasCachedPageJobDescriptors || !BytesEqual(pageJobDescriptors, m_cachedPageJobDescriptors)) {
            m_cachedPageJobDescriptors = pageJobDescriptors;
            m_hasCachedPageJobDescriptors = true;
            BUFFER_UPLOAD(
                &pageJobDescriptors,
                sizeof(CLodWorkGraphComputePageJobDescriptors),
                rg::runtime::UploadTarget::FromShared(m_workGraphComputePageJobDescriptorsBuffer),
                0);
        }
    }

    bool rebuildViewTables = false;
    {
        ZoneScopedN("HierarchicalCullingPass::CheckViewResourceRevision");
        const uint64_t viewResourceRevision = context.viewManager
            ? context.viewManager->GetResourceLayoutRevision()
            : m_lastViewResourceLayoutRevision + 1u;
        rebuildViewTables = viewResourceRevision != m_lastViewResourceLayoutRevision;
        if (rebuildViewTables) {
            m_lastViewResourceLayoutRevision = viewResourceRevision;
        }
    }

    // Keep the shared per-view visibility UAV table valid for any visibility-buffer path.
    // Reyes patch raster consumes this buffer even when the primary CLod path is not using SW classification.
    if (UsesVisibilityBufferOutput(m_rasterOutputKind) || UsesSWClassification(m_workGraphMode)) {
        if (rebuildViewTables || m_cachedViewRasterInfo.empty()) {
            ZoneScopedN("HierarchicalCullingPass::RebuildViewRasterInfo");
            m_visibilityBuffers.clear();
            auto numViews = context.viewManager->GetCameraBufferSize();
            std::vector<CLodViewRasterInfo> viewRasterInfo(numViews);
            std::vector<std::pair<uint32_t, std::shared_ptr<PixelBuffer>>> visibilityBuffersByCameraIndex;
            const CLodVirtualShadowResolutionConfig virtualShadowConfig = CLodVirtualShadowBuildRuntimeResolutionConfig();
            context.viewManager->ForEachView([&](uint64_t v) {
                auto viewInfo = context.viewManager->Get(v);
                if (!viewInfo) {
                    return;
                }

                auto cameraIndex = viewInfo->gpu.cameraBufferIndex;
                CLodViewRasterInfo info{};
                info.scissorMinX = 0;
                info.scissorMinY = 0;

                if (UsesVirtualShadowOutput(m_rasterOutputKind)) {
                    if (viewInfo->flags.shadow && viewInfo->lightType == Components::LightType::Directional) {
                        info.scissorMaxX = virtualShadowConfig.virtualResolution;
                        info.scissorMaxY = virtualShadowConfig.virtualResolution;
                        info.viewportScaleX = 1.0f;
                        info.viewportScaleY = 1.0f;
                    }
                    viewRasterInfo[cameraIndex] = info;
                    return;
                }

                if (viewInfo->gpu.visibilityBuffer != nullptr) {
                    info.visibilityUAVDescriptorIndex = viewInfo->gpu.visibilityBuffer->GetUAVShaderVisibleInfo(0).slot.index;
                    info.scissorMaxX = viewInfo->gpu.visibilityBuffer->GetWidth();
                    info.scissorMaxY = viewInfo->gpu.visibilityBuffer->GetHeight();
                    info.viewportScaleX = 1.0f;
                    info.viewportScaleY = 1.0f;
                    viewRasterInfo[cameraIndex] = info;
                    if (UsesWorkGraphSWRaster(m_workGraphMode)) {
                        visibilityBuffersByCameraIndex.emplace_back(cameraIndex, viewInfo->gpu.visibilityBuffer);
                    }
                }
            });

            std::sort(
                visibilityBuffersByCameraIndex.begin(),
                visibilityBuffersByCameraIndex.end(),
                [](const auto& left, const auto& right) {
                    return left.first < right.first;
                });

            std::vector<uint64_t> currentVisibilityBufferIds;
            currentVisibilityBufferIds.reserve(visibilityBuffersByCameraIndex.size());
            m_visibilityBuffers.reserve(visibilityBuffersByCameraIndex.size());
            for (auto& [cameraIndex, visibilityBuffer] : visibilityBuffersByCameraIndex) {
                (void)cameraIndex;
                m_visibilityBuffers.push_back(visibilityBuffer);
                currentVisibilityBufferIds.push_back(visibilityBuffer->GetGlobalResourceID());
            }

            const bool sizeChanged = m_cachedViewRasterInfo.size() != viewRasterInfo.size();
            m_cachedViewRasterInfo = std::move(viewRasterInfo);
            if (sizeChanged) {
                m_viewRasterInfoBuffer->ResizeStructured(static_cast<uint32_t>(m_cachedViewRasterInfo.size()));
            }
            if (!m_cachedViewRasterInfo.empty()) {
                BUFFER_UPLOAD(
                    m_cachedViewRasterInfo.data(),
                    static_cast<uint32_t>(m_cachedViewRasterInfo.size() * sizeof(CLodViewRasterInfo)),
                    rg::runtime::UploadTarget::FromShared(m_viewRasterInfoBuffer),
                    0);
            }
            if (currentVisibilityBufferIds != m_declaredVisibilityBufferIds) {
                m_declaredVisibilityBufferIds = std::move(currentVisibilityBufferIds);
                m_declaredResourcesChanged = true;
            }
        }
    }
    else {
        ZoneScopedN("HierarchicalCullingPass::ClearVisibilityDeclarations");
        m_visibilityBuffers.clear();
        if (!m_declaredVisibilityBufferIds.empty()) {
            m_declaredVisibilityBufferIds.clear();
            m_declaredResourcesChanged = true;
        }
    }

    if (UsesPerViewDepthMapOcclusion(m_rasterOutputKind)) {
        if (rebuildViewTables || !m_hasUploadedViewDepthSrvIndices) {
            ZoneScopedN("HierarchicalCullingPass::RebuildViewDepthSrvIndices");
            std::vector<CLodViewDepthSRVIndex> viewDepthSrvIndices(CLodMaxViewDepthIndices);
            const bool useHistoryDepth = m_isFirstPass;
            for (uint32_t i = 0; i < CLodMaxViewDepthIndices; ++i) {
                viewDepthSrvIndices[i].cameraBufferIndex = i;
                viewDepthSrvIndices[i].linearDepthSRVIndex = 0;
            }

            context.viewManager->ForEachView([&](uint64_t viewID) {
                const auto* view = context.viewManager->Get(viewID);
                if (!view) {
                    return;
                }

                const uint32_t cameraBufferIndex = view->gpu.cameraBufferIndex;
                if (cameraBufferIndex >= CLodMaxViewDepthIndices) {
                    return;
                }

                const auto linearDepthMap = useHistoryDepth
                    ? (view->gpu.lastFrameLinearDepthValid ? view->gpu.lastFrameLinearDepthMap : nullptr)
                    : view->gpu.linearDepthMap;
                if (!linearDepthMap) {
                    return;
                }

                uint32_t slice = 0;
                if (view->cameraInfo.depthBufferArrayIndex >= 0) {
                    slice = static_cast<uint32_t>(view->cameraInfo.depthBufferArrayIndex);
                }

                const uint32_t maxSlices = linearDepthMap->GetNumSRVSlices();
                if (maxSlices == 0) {
                    return;
                }

                slice = (std::min)(slice, maxSlices - 1);
                viewDepthSrvIndices[cameraBufferIndex].cameraBufferIndex = cameraBufferIndex;
                viewDepthSrvIndices[cameraBufferIndex].linearDepthSRVIndex = linearDepthMap->GetSRVInfo(0, slice).slot.index;
            });

            m_cachedViewDepthSrvIndices = std::move(viewDepthSrvIndices);
            m_hasUploadedViewDepthSrvIndices = true;
            BUFFER_UPLOAD(
                m_cachedViewDepthSrvIndices.data(),
                static_cast<uint32_t>(m_cachedViewDepthSrvIndices.size() * sizeof(CLodViewDepthSRVIndex)),
                rg::runtime::UploadTarget::FromShared(m_viewDepthSrvIndicesBuffer),
                0);
        }
    }

    {
        ZoneScopedN("HierarchicalCullingPass::CheckFirstPassWork");
        if (!m_isFirstPass) {
            return;
        }
    }

    {
        ZoneScopedN("HierarchicalCullingPass::UploadReplayStateReset");
        CLodReplayBufferState replayState{};
        replayState.nodeWriteCount = 0;
        replayState.meshletWriteCount = 0;
        replayState.nodeDropped = 0;
        replayState.meshletDropped = 0;
        replayState.visibleClusterCombinedCount = 0;
        replayState.reyesSplitWriteCount = 0;
        replayState.reyesDiceWriteCount = 0;
        replayState.reyesSplitDropped = 0;
        replayState.reyesDiceDropped = 0;
        BUFFER_UPLOAD(
            &replayState,
            sizeof(CLodReplayBufferState),
            rg::runtime::UploadTarget::FromShared(m_occlusionReplayStateBuffer),
            0);
    }

    {
        ZoneScopedN("HierarchicalCullingPass::UpdateReplayNodeInputs");
        CLodNodeGpuInput nodeGpuInputs[5] = {};
        CLodMultiNodeGpuInput multiNodeGpuInput{};
        multiNodeGpuInput.numNodeInputs = m_workGraphReyesVisibility ? 4u : 2u;
        multiNodeGpuInput.pad0 = 0;
        multiNodeGpuInput.nodeInputStride = sizeof(CLodNodeGpuInput);

    // Replay dispatch descriptors need stable GPU virtual addresses during Update.
    // These CLod-owned control buffers are small non-aliased resources, so it is safe to materialize
    // them eagerly here before building the replay node input table.
        if (!m_occlusionNodeGpuInputsBuffer->IsMaterialized()) {
            m_occlusionNodeGpuInputsBuffer->Materialize();
        }
        if (!m_occlusionReplayBuffer->IsMaterialized()) {
            m_occlusionReplayBuffer->Materialize();
        }

        if (const uint64_t nodeInputBufferAddress = GetNativeBufferDeviceAddress(m_occlusionNodeGpuInputsBuffer->GetAPIResource())) {
            multiNodeGpuInput.nodeInputsAddress = nodeInputBufferAddress + sizeof(CLodNodeGpuInput);
        }

        if (const uint64_t replayAddress = GetNativeBufferDeviceAddress(m_occlusionReplayBuffer->GetAPIResource())) {
        // Entry point 1 = TraverseNodes — node replay region at offset 0
        nodeGpuInputs[1].entrypointIndex = 1;
        nodeGpuInputs[1].numRecords = 0; // patched by GPU in CreateRasterBucketsHistogramCommandCSMain
        nodeGpuInputs[1].recordsAddress = replayAddress;
        nodeGpuInputs[1].recordStride = CLodNodeReplayStrideBytes;

        // Entry point 2 = ClusterCull1 — meshlet replay region at midpoint offset
        nodeGpuInputs[2].entrypointIndex = 2;
        nodeGpuInputs[2].numRecords = 0; // patched by GPU in CreateRasterBucketsHistogramCommandCSMain
        nodeGpuInputs[2].recordsAddress = replayAddress + CLodReplayMeshletRegionOffset;
        nodeGpuInputs[2].recordStride = CLodMeshletReplayStrideBytes;

        if (m_workGraphReyesVisibility) {
            // Entry point 3 = ReyesSplitReplay — split replay region.
            nodeGpuInputs[3].entrypointIndex = 3;
            nodeGpuInputs[3].numRecords = 0;
            nodeGpuInputs[3].recordsAddress = replayAddress + CLodReplayReyesSplitRegionOffset;
            nodeGpuInputs[3].recordStride = CLodReyesSplitReplayStrideBytes;

            // Entry point 4 = ReyesDiceReplay — dice replay region.
            nodeGpuInputs[4].entrypointIndex = 4;
            nodeGpuInputs[4].numRecords = 0;
            nodeGpuInputs[4].recordsAddress = replayAddress + CLodReplayReyesDiceRegionOffset;
            nodeGpuInputs[4].recordStride = CLodReyesDiceReplayStrideBytes;
        }
        }

        static_assert(sizeof(CLodMultiNodeGpuInput) == sizeof(CLodNodeGpuInput));
        std::memcpy(&nodeGpuInputs[0], &multiNodeGpuInput, sizeof(CLodMultiNodeGpuInput));

        if (!m_hasCachedNodeGpuInputs
            || !std::equal(
                std::begin(nodeGpuInputs),
                std::end(nodeGpuInputs),
                m_cachedNodeGpuInputs.begin(),
                [](const CLodNodeGpuInput& left, const CLodNodeGpuInput& right) {
                    return BytesEqual(left, right);
                })) {
            std::copy(std::begin(nodeGpuInputs), std::end(nodeGpuInputs), m_cachedNodeGpuInputs.begin());
            m_hasCachedNodeGpuInputs = true;
            BUFFER_UPLOAD(
                nodeGpuInputs,
                sizeof(nodeGpuInputs),
                rg::runtime::UploadTarget::FromShared(m_occlusionNodeGpuInputsBuffer),
                0);
        }
    }

    if (IsCLodWorkGraphTelemetryEnabled()) {
        ZoneScopedN("HierarchicalCullingPass::UploadTelemetryReset");
        m_zeroTelemetryScratch.assign(CLodWorkGraphCounterCount, 0u);
        BUFFER_UPLOAD(
            m_zeroTelemetryScratch.data(),
            static_cast<uint32_t>(m_zeroTelemetryScratch.size() * sizeof(uint32_t)),
            rg::runtime::UploadTarget::FromShared(m_workGraphTelemetryBuffer),
            0);
    }

    if (m_workGraphReyesVisibility && m_reyesTelemetryBuffer) {
        ZoneScopedN("HierarchicalCullingPass::UploadReyesTelemetryReset");
        CLodReyesTelemetry telemetry{};
        telemetry.phaseIndex = m_isFirstPass ? 1u : 2u;
        telemetry.configuredMaxSplitPassCount = CLodReyesMaxSplitPassCount;
        BUFFER_UPLOAD(&telemetry, sizeof(CLodReyesTelemetry), rg::runtime::UploadTarget::FromShared(m_reyesTelemetryBuffer), 0);
    }
}

bool HierarchicalCullingPass::DeclaredResourcesChanged() const {
    return m_declaredResourcesChanged;
}

std::shared_ptr<Resource> HierarchicalCullingPass::ProvideResource(ResourceIdentifier const& key)
{
    if (key == m_workGraphComputePageJobDescriptorResourceId) {
        return m_workGraphComputePageJobDescriptorsBuffer;
    }

    if (key == m_voxelRasterQueueDescriptorResourceId) {
        return m_voxelRasterQueueDescriptorsBuffer;
    }

    return nullptr;
}

std::vector<ResourceIdentifier> HierarchicalCullingPass::GetSupportedKeys()
{
    return {
        ResourceIdentifier{ m_workGraphComputePageJobDescriptorResourceId },
        ResourceIdentifier{ m_voxelRasterQueueDescriptorResourceId }
    };
}

void HierarchicalCullingPass::Cleanup() {
}

void HierarchicalCullingPass::CreatePipelines(
    rhi::Device device,
    rhi::PipelineLayoutHandle globalRootSignature,
    rhi::WorkGraphPtr& outGraph,
    PipelineState& outCreateCommandPipeline,
    PipelineState& outClearPipeline)
{
    WorkGraphFeatureInfo workGraphFeatureInfo{};
    const rhi::Result workGraphFeatureResult = device.QueryFeatureInfo(&workGraphFeatureInfo.header);
    ShaderLibraryInfo libInfo(L"shaders/ClusterLOD/workGraphCulling.hlsl", L"lib_6_8");
    std::wstring pageJobDescriptorResourceIdWide(
        m_workGraphComputePageJobDescriptorResourceId.begin(),
        m_workGraphComputePageJobDescriptorResourceId.end());
    std::wstring pageJobDescriptorResourceIdDefine = L"\"" + pageJobDescriptorResourceIdWide + L"\"";
    std::wstring voxelQueueDescriptorResourceIdWide(
        m_voxelRasterQueueDescriptorResourceId.begin(),
        m_voxelRasterQueueDescriptorResourceId.end());
    std::wstring voxelQueueDescriptorResourceIdDefine = L"\"" + voxelQueueDescriptorResourceIdWide + L"\"";
    constexpr bool enableComputePageJobDescriptorBuffer = true;
    constexpr bool splitLeafTraversalNode = true;
    std::vector<DxcDefine> defines = {
        { L"CLOD_WG_ENABLE_SW_CLASSIFICATION", UsesSWClassification(m_workGraphMode) ? L"1" : L"0" },
        { L"CLOD_WG_ENABLE_SW_NODE_OUTPUT", UsesWorkGraphSWRaster(m_workGraphMode) ? L"1" : L"0" },
        { L"CLOD_WG_ENABLE_REYES_VISIBILITY", m_workGraphReyesVisibility ? L"1" : L"0" },
        { L"CLOD_WG_SPLIT_LEAF_NODE", splitLeafTraversalNode ? L"1" : L"0" },
        { L"CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW", UsesVirtualShadowOutput(m_rasterOutputKind) ? L"1" : L"0" },
        { L"CLOD_WG_ENABLE_COMPUTE_PAGE_JOB_DESCRIPTOR_BUFFER", enableComputePageJobDescriptorBuffer ? L"1" : L"0" },
        { L"CLOD_WG_COMPUTE_PAGE_JOB_DESCRIPTOR_BUFFER_ID", pageJobDescriptorResourceIdDefine.c_str() },
        { L"CLOD_WG_VOXEL_RASTER_QUEUE_DESCRIPTOR_BUFFER_ID", voxelQueueDescriptorResourceIdDefine.c_str() },
    };
    auto compiled = PSOManager::GetInstance().CompileShaderLibrary(libInfo, defines);
    m_pipelineResources = compiled.resourceDescriptorSlots;

    rhi::ShaderBinary libDxil{
        compiled.libraryBlob->GetBufferPointer(),
        static_cast<uint32_t>(compiled.libraryBlob->GetBufferSize())
    };

    std::vector<rhi::ShaderExportDesc> exports = {
        { "WG_ObjectCull", nullptr },
        { "WG_TraverseNodes", nullptr },
        { "WG_ClusterCull1", nullptr },
        { "WG_ClusterCull2", nullptr },
        { "WG_ClusterCull4", nullptr },
        { "WG_ClusterCull8", nullptr },
        { "WG_ClusterCull16", nullptr },
        { "WG_ClusterCull32", nullptr },
        { "WG_ClusterCull64", nullptr },
    };
    if constexpr (splitLeafTraversalNode) {
        exports.push_back({ "WG_LeafNodes", nullptr });
    }
    if (UsesWorkGraphSWRaster(m_workGraphMode)) {
        exports.push_back({ "WG_SWRaster", nullptr });
        if (UsesVirtualShadowOutput(m_rasterOutputKind)) {
            exports.push_back({ "WG_PageJobBuild", nullptr });
            exports.push_back({ "WG_PageJobExpand", nullptr });
            exports.push_back({ "WG_PageJobRasterPage", nullptr });
        }
    }
    if (m_workGraphReyesVisibility) {
        exports.push_back({ "WG_ReyesSeed", nullptr });
        exports.push_back({ "WG_ReyesSplit1", nullptr });
        exports.push_back({ "WG_ReyesSplit2", nullptr });
        exports.push_back({ "WG_ReyesSplit3", nullptr });
        exports.push_back({ "WG_ReyesSplit4", nullptr });
        exports.push_back({ "WG_ReyesSplit5", nullptr });
        exports.push_back({ "WG_ReyesDice", nullptr });
        exports.push_back({ "WG_ReyesRaster", nullptr });
        exports.push_back({ "WG_ReyesSplitReplay", nullptr });
        exports.push_back({ "WG_ReyesDiceReplay", nullptr });
    }

    rhi::ShaderLibraryDesc library{};
    library.dxil = libDxil;
    library.exports = rhi::Span<rhi::ShaderExportDesc>(exports.data(), static_cast<uint32_t>(exports.size()));

    std::vector<rhi::NodeIDDesc> entrypoints = {
        { "ObjectCull", 0 },
        { "TraverseNodes", 0 },
        { "ClusterCull1", 0 }
    };
    if (m_workGraphReyesVisibility) {
        entrypoints.push_back({ "ReyesSplitReplay", 0 });
        entrypoints.push_back({ "ReyesDiceReplay", 0 });
    }

    rhi::WorkGraphDesc wg{};
    wg.programName = "HierarchicalCulling";
    wg.flags = rhi::WorkGraphFlags::WorkGraphFlagsIncludeAllAvailableNodes;
    wg.globalRootSignature = globalRootSignature;
    wg.libraries = rhi::Span<rhi::ShaderLibraryDesc>(&library, 1);
    wg.entrypoints = rhi::Span<rhi::NodeIDDesc>(entrypoints.data(), static_cast<uint32_t>(entrypoints.size()));
    wg.allowStateObjectAdditions = false;
    switch (m_workGraphMode) {
    case HierarchicalCullingWorkGraphMode::HardwareOnly:
        wg.debugName = "HierarchicalCullingWG.HW";
        break;
    case HierarchicalCullingWorkGraphMode::SoftwareRasterCompute:
        wg.debugName = "HierarchicalCullingWG.ComputeSW";
        break;
    case HierarchicalCullingWorkGraphMode::SoftwareRasterWorkGraph:
        wg.debugName = "HierarchicalCullingWG.WorkGraphSW";
        break;
    }

    device.CreateWorkGraph(wg, outGraph);

    outCreateCommandPipeline = PSOManager::GetInstance().MakeComputePipeline(
        globalRootSignature,
        L"shaders/ClusterLOD/clodUtil.hlsl",
        L"CreateRasterBucketsHistogramCommandCSMain",
        {},
        "HierarchicalLODCommandCreation");
    outClearPipeline = PSOManager::GetInstance().MakeComputePipeline(
        globalRootSignature,
        L"shaders/ClusterLOD/clodUtil.hlsl",
        L"ClearUintStructuredBufferCSMain",
        {},
        "HierarchicalCullingClearUintPSO");
}
