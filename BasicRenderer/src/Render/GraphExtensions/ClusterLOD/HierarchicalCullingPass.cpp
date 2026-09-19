#include "Render/GraphExtensions/ClusterLOD/HierarchicalCullingPass.h"
#include "Render/GraphExtensions/ClusterLOD/PreparedCullingWorkloads.h"
#include "RenderPasses/PreparedComputeCommands.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

#include <rhi_feature_info.h>
#include <rhi_interop.h>
#include <rhi_interop_dx12.h>
#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>
#include <BasicTelemetry/Telemetry.h>

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
#include "Render/IndirectStateArtifacts.h"
#include "Render/ObjectBufferStateArtifacts.h"
#include "Render/Runtime/UploadTypes.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Resources/components.h"
#include "Resources/Resolvers/ECSResourceResolver.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "Resources/Resolvers/PublishedStateResourceResolver.h"
#include "BuiltinResources.h"
#include "ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodClearUintBufferRootConstants.h"
#include "../shaders/PerPassRootConstants/clodCreateCommandRootConstants.h"
#include "../shaders/PerPassRootConstants/clodWorkGraphRootConstants.h"

namespace {

std::vector<HierarchicalCullingPass*> g_liveHierarchicalCullingPasses;

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

std::vector<uint64_t> CollectDeclaredDrawSetResourceIds(
    const std::shared_ptr<const br::render::PublishedRendererState>& state,
    RenderPhase renderPhase,
    bool clodOnlyWorkloads)
{
    std::vector<uint64_t> resourceIds;
    br::render::PublishedResourceQuery query{};
    query.owner = br::render::PublishedFragmentKind::ActiveDrawLists;
    query.usage = br::render::PublishedResourceUsage::ActiveDrawList;
    query.renderPhaseHash = renderPhase.hash;
    constexpr std::uint64_t clodBit = 1ull << 63u;
    if (clodOnlyWorkloads) query.requiredVariantMask = clodBit;
    else query.forbiddenVariantMask = clodBit;
    const auto resources = state && state->resourceCatalog ? state->resourceCatalog->FindAll(query)
                                                           : br::render::PublishedResourceCatalog::ResourceList{};
    for (const auto& resource : resources) if (resource) resourceIds.push_back(resource->GetGlobalResourceID());

    std::sort(resourceIds.begin(), resourceIds.end());
    resourceIds.erase(std::unique(resourceIds.begin(), resourceIds.end()), resourceIds.end());
    return resourceIds;
}
}

HierarchicalCullingPass::HierarchicalCullingPass(
    std::string stablePassIdentifier,
    HierarchicalCullingPassInputs inputs,
    std::shared_ptr<org::Buffer> visibleClustersBuffer,
    std::shared_ptr<org::Buffer> visibleClusterTransformIndicesBuffer,
    std::shared_ptr<org::Buffer> visibleClustersCounterBuffer,
    std::shared_ptr<org::Buffer> swVisibleClustersCounterBuffer,
    std::shared_ptr<org::Buffer> voxelRasterWorkBuffer,
    std::shared_ptr<org::Buffer> voxelRasterWorkCounterBuffer,
    std::shared_ptr<org::Buffer> skinnedVoxelRasterWorkBuffer,
    std::shared_ptr<org::Buffer> skinnedVoxelRasterWorkCounterBuffer,
    uint32_t voxelRasterWorkCapacity,
    std::shared_ptr<org::Buffer> pageJobVisibleClustersBuffer,
    std::shared_ptr<org::Buffer> pageJobVisibleClusterTransformIndicesBuffer,
    std::shared_ptr<org::Buffer> pageJobVisibleClustersCounterBuffer,
    std::shared_ptr<org::Buffer> histogramIndirectCommand,
    std::shared_ptr<org::Buffer> workGraphTelemetryBuffer,
    std::shared_ptr<org::Buffer> occlusionReplayBuffer,
    std::shared_ptr<org::Buffer> occlusionReplayStateBuffer,
    std::shared_ptr<org::Buffer> occlusionNodeGpuInputsBuffer,
    std::shared_ptr<org::Buffer> viewRasterInfoBuffer,
    std::shared_ptr<org::PixelBuffer> shadowDirtyHierarchyTexture,
    std::shared_ptr<org::ResourceGroup> slabResourceGroup,
    std::shared_ptr<org::Buffer> phase1VisibleClustersCounterBuffer,
    std::shared_ptr<org::Buffer> swWriteBaseCounterBuffer,
    std::shared_ptr<org::Buffer> shadowPredictiveInvalidationCandidatesBuffer,
    std::shared_ptr<org::Buffer> shadowPredictiveInvalidationCandidateCountBuffer,
    std::shared_ptr<org::Buffer> shadowInvalidatedInstancesBitsetBuffer,
    std::shared_ptr<org::PixelBuffer> shadowPageTableTexture,
    std::shared_ptr<org::PixelBuffer> shadowPhysicalPagesTexture,
    std::shared_ptr<org::Buffer> shadowActiveBlockMetadataBuffer,
    std::shared_ptr<org::PixelBuffer> shadowDynamicPhysicalPagesTexture,
    std::shared_ptr<org::Buffer> shadowDynamicActiveBlockMetadataBuffer,
    std::shared_ptr<org::Buffer> reyesDiceQueueBuffer,
    std::shared_ptr<org::Buffer> reyesDiceQueueCounterBuffer,
    std::shared_ptr<org::Buffer> reyesDiceQueueOverflowBuffer,
    std::shared_ptr<org::Buffer> reyesTessTableConfigsBuffer,
    std::shared_ptr<org::Buffer> reyesTessTableVerticesBuffer,
    std::shared_ptr<org::Buffer> reyesTessTableTrianglesBuffer,
    std::shared_ptr<org::Buffer> reyesTelemetryBuffer,
    uint32_t reyesDiceQueueCapacity) {
    m_workGraphMode = inputs.workGraphMode;
    m_rasterOutputKind = inputs.rasterOutputKind;
    m_isFirstPass = inputs.isFirstPass;
    m_workGraphReyesVisibility = inputs.workGraphReyesVisibility;
    m_voxelRasterWorkCapacity = voxelRasterWorkCapacity;
    m_workGraphComputePageJobDescriptorResourceId =
        std::string(CLodWorkGraphComputePageJobDescriptorBufferId) + "." + std::move(stablePassIdentifier);
    m_voxelRasterQueueDescriptorResourceId =
        std::string(CLodVoxelRasterQueueDescriptorBufferId) + "." + m_workGraphComputePageJobDescriptorResourceId;
    rhi::WorkGraphPtr initialWorkGraph;
    CreatePipelines(
        DeviceManager::GetInstance().GetDevice(),
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        initialWorkGraph,
        m_createCommandPipelineState,
        m_clearPipelineState);
    if (!initialWorkGraph) {
        spdlog::error(
            "HierarchicalCullingPass::HierarchicalCullingPass CreatePipelines returned null work graph this={} workGraphMode={} rasterOutputKind={}",
            static_cast<const void*>(this),
            static_cast<int>(m_workGraphMode),
            static_cast<int>(m_rasterOutputKind));
    }
    m_workGraph = std::make_shared<rhi::WorkGraphPtr>(std::move(initialWorkGraph));
    auto memSize = (*m_workGraph)->GetRequiredScratchMemorySize();
    m_scratchBuffer = org::Buffer::CreateShared(
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
    if (m_voxelRasterWorkCapacity != 0u) {
        m_voxelRasterQueueDescriptorsBuffer = CreateAliasedUnmaterializedStructuredBuffer(
            1,
            sizeof(CLodVoxelRasterQueueDescriptors),
            false,
            false,
            false,
            false);
        m_voxelRasterQueueDescriptorsBuffer->SetName("CLod Voxel Raster Queue Descriptors");
        org::memory::SetResourceUsageHint(*m_voxelRasterQueueDescriptorsBuffer, "Cluster LOD voxel rasterization");
    }
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
    org::memory::SetResourceUsageHint(*m_workGraphComputePageJobDescriptorsBuffer, "Cluster LOD work graph");
    m_histogramIndirectCommand = std::move(histogramIndirectCommand);
    m_workGraphTelemetryBuffer = std::move(workGraphTelemetryBuffer);
    m_occlusionReplayBuffer = std::move(occlusionReplayBuffer);
    m_occlusionReplayStateBuffer = std::move(occlusionReplayStateBuffer);
    m_occlusionNodeGpuInputsBuffer = std::move(occlusionNodeGpuInputsBuffer);
    m_viewRasterInfoBuffer = std::move(viewRasterInfoBuffer);
    m_shadowPredictiveInvalidationCandidatesBuffer = std::move(shadowPredictiveInvalidationCandidatesBuffer);
    m_shadowPredictiveInvalidationCandidateCountBuffer = std::move(shadowPredictiveInvalidationCandidateCountBuffer);
    m_shadowInvalidatedInstancesBitsetBuffer = std::move(shadowInvalidatedInstancesBitsetBuffer);
    m_shadowDirtyHierarchyTexture = std::move(shadowDirtyHierarchyTexture);
    m_shadowPageTableTexture = std::move(shadowPageTableTexture);
    m_shadowPhysicalPagesTexture = std::move(shadowPhysicalPagesTexture);
    m_shadowActiveBlockMetadataBuffer = std::move(shadowActiveBlockMetadataBuffer);
    m_shadowDynamicPhysicalPagesTexture =
        std::move(shadowDynamicPhysicalPagesTexture);
    m_shadowDynamicActiveBlockMetadataBuffer =
        std::move(shadowDynamicActiveBlockMetadataBuffer);
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
    g_liveHierarchicalCullingPasses.push_back(this);
}

HierarchicalCullingPass::~HierarchicalCullingPass()
{
    std::erase(g_liveHierarchicalCullingPasses, this);
}

size_t HierarchicalCullingPass::ReloadAllWorkGraphs()
{
    for (HierarchicalCullingPass* pass : g_liveHierarchicalCullingPasses) {
        pass->ReloadWorkGraph();
    }
    return g_liveHierarchicalCullingPasses.size();
}

void HierarchicalCullingPass::ReloadWorkGraph()
{
    rhi::WorkGraphPtr replacement;
    org::PipelineState createCommandPipeline;
    org::PipelineState clearPipeline;
    CreatePipelines(
        DeviceManager::GetInstance().GetDevice(),
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        replacement,
        createCommandPipeline,
        clearPipeline);
    if (!replacement) {
        throw std::runtime_error("live CLOD work-graph compilation returned a null work graph");
    }
    const uint64_t replacementScratchSize = replacement->GetRequiredScratchMemorySize();
    if (replacementScratchSize != (*m_workGraph)->GetRequiredScratchMemorySize()) {
        m_scratchBuffer = org::Buffer::CreateShared(
            rhi::HeapType::DeviceLocal,
            replacementScratchSize,
            true);
        m_scratchBuffer->SetName("CLod Work Graph Scratch Buffer");
        m_scratchBuffer->SetMemoryUsageHint("Work graph scratch buffer");
    }
    m_workGraph = std::make_shared<rhi::WorkGraphPtr>(std::move(replacement));
    m_workGraphInitialization->initialized.store(false, std::memory_order_release);
}

HierarchicalCullingBindings HierarchicalCullingPass::Declare(org::PassBuilder& builder) {
    const org::ResourceState computeReadState{
        rhi::ResourceAccessType::ShaderResource,
        rhi::ResourceLayout::ShaderResource,
        rhi::ResourceSyncState::ComputeShading
    };

    br::render::PublishedResourceQuery drawSetIndicesQuery{};
    drawSetIndicesQuery.owner = br::render::PublishedFragmentKind::ActiveDrawLists;
    drawSetIndicesQuery.usage = br::render::PublishedResourceUsage::ActiveDrawList;
    drawSetIndicesQuery.renderPhaseHash = m_renderPhase.hash;
    constexpr std::uint64_t clodBit = 1ull << 63u;
    if (m_clodOnlyWorkloads) drawSetIndicesQuery.requiredVariantMask = clodBit;
    else drawSetIndicesQuery.forbiddenVariantMask = clodBit;
    br::render::PublishedResourceQuery visibilityGenerationQuery{};
    visibilityGenerationQuery.owner = br::render::PublishedFragmentKind::DrawRecords;
    visibilityGenerationQuery.usage = br::render::PublishedResourceUsage::ShaderResource;
    visibilityGenerationQuery.requiredVariantMask =
        br::render::kObjectVisibilityGenerationVariant;
    builder.WithUnorderedAccess(
            m_scratchBuffer,
            m_visibleClustersBuffer,
            m_visibleClusterTransformIndicesBuffer,
            m_visibleClustersCounterBuffer,
            m_histogramIndirectCommand,
            m_workGraphTelemetryBuffer,
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
            Builtin::Material::TextureStreamingMetadataBuffer,
            m_workGraphComputePageJobDescriptorResourceId.c_str())
    		.WithUnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer)
        .WithShaderResource(PublishedStateResourceResolver(
            br::render::PublishedStateSource::ProcessSource(), drawSetIndicesQuery))
        .WithShaderResource(PublishedStateResourceResolver(
            br::render::PublishedStateSource::ProcessSource(), visibilityGenerationQuery));

    if (m_voxelRasterWorkCapacity != 0u) {
        builder.WithUnorderedAccess(
                m_voxelRasterWorkBuffer,
                m_voxelRasterWorkCounterBuffer,
                m_skinnedVoxelRasterWorkBuffer,
                m_skinnedVoxelRasterWorkCounterBuffer)
            .WithShaderResource(m_voxelRasterQueueDescriptorResourceId.c_str());
    }

    if (UsesPerViewDepthMapOcclusion(m_rasterOutputKind)) {
        builder.WithShaderResource(Builtin::PrimaryCamera::LinearDepthMap);
    }

    if (UsesSWClassification(m_workGraphMode)) {
        builder.WithUnorderedAccess(m_swVisibleClustersCounterBuffer);
    }
    if (m_pageJobVisibleClustersBuffer && m_pageJobVisibleClusterTransformIndicesBuffer && m_pageJobVisibleClustersCounterBuffer) {
        builder.WithUnorderedAccess(
            m_pageJobVisibleClustersBuffer,
            m_pageJobVisibleClusterTransformIndicesBuffer,
            m_pageJobVisibleClustersCounterBuffer);
    }
    if (UsesVirtualShadowOutput(m_rasterOutputKind)) {
        if (m_shadowPredictiveInvalidationCandidatesBuffer && m_shadowPredictiveInvalidationCandidateCountBuffer) {
            builder.WithUnorderedAccess(
                m_shadowPredictiveInvalidationCandidatesBuffer,
                m_shadowPredictiveInvalidationCandidateCountBuffer);
        }
        builder.WithShaderResource(
            Builtin::Shadows::CLodClipmapInfo,
            Builtin::Shadows::CLodDirectionalPageViewInfo,
            Builtin::Shadows::CLodCompactShadowCameras,
            m_shadowDirtyHierarchyTexture,
            m_shadowActiveBlockMetadataBuffer)
            .WithUnorderedAccess(Builtin::Shadows::CLodPageTable);
        if (m_shadowInvalidatedInstancesBitsetBuffer) {
            builder.WithShaderResource(m_shadowInvalidatedInstancesBitsetBuffer);
        }
    }
    if (UsesWorkGraphSWRaster(m_workGraphMode) && UsesVirtualShadowOutput(m_rasterOutputKind)) {
        builder.WithUnorderedAccess(
            m_shadowPhysicalPagesTexture,
            m_shadowDynamicPhysicalPagesTexture);
    }
    if (m_workGraphReyesVisibility) {
        builder.WithUnorderedAccess(
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
        builder.WithShaderResource(m_phase1VisibleClustersCounterBuffer);
    }
    if (UsesSWClassification(m_workGraphMode) && m_swWriteBaseCounterBuffer) {
        builder.WithShaderResource(m_swWriteBaseCounterBuffer);
    }

    // Declare visibility buffer UAVs for SW raster render graph tracking.
    if (UsesWorkGraphSWRaster(m_workGraphMode) && UsesVisibilityBufferOutput(m_rasterOutputKind)) {
        for (auto& vb : m_visibilityBuffers) {
            builder.WithUnorderedAccess(vb);
        }
    }
    builder.WithUnorderedAccess(Builtin::DebugVisualization);

    // Declare page pool slabs for bindless access (auto-invalidates when new slabs are added).
    if (m_slabResourceGroup) {
        builder.WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
    }

    HierarchicalCullingBindings bindings{};
    bindings.visible = builder.BindUnorderedAccess(m_visibleClustersBuffer);
    bindings.transforms = builder.BindUnorderedAccess(m_visibleClusterTransformIndicesBuffer);
    bindings.visibleCounter = builder.BindUnorderedAccess(m_visibleClustersCounterBuffer);
    bindings.swCounter = builder.BindUnorderedAccess(m_swVisibleClustersCounterBuffer);
    bindings.histogram = builder.BindUnorderedAccess(m_histogramIndirectCommand);
    bindings.telemetry = builder.BindUnorderedAccess(m_workGraphTelemetryBuffer);
    bindings.replay = builder.BindUnorderedAccess(m_occlusionReplayBuffer);
    bindings.replayState = builder.BindUnorderedAccess(m_occlusionReplayStateBuffer);
    bindings.nodeInputs = builder.BindUnorderedAccess(m_occlusionNodeGpuInputsBuffer);
    if (UsesSWClassification(m_workGraphMode)) {
        bindings.hasSw = bindings.hasViewRasterInfo = true;
    }
    if (UsesPerViewDepthMapOcclusion(m_rasterOutputKind)) {
        bindings.hasViewDepth = true;
    }
    if (UsesVirtualShadowOutput(m_rasterOutputKind)) {
        bindings.shadowPageTable = builder.BindUnorderedAccess(m_shadowPageTableTexture);
        bindings.shadowActiveMetadata = builder.BindShaderResource(m_shadowActiveBlockMetadataBuffer);
        bindings.hasVirtualShadow = true;
        if (m_shadowDirtyHierarchyTexture) {
            bindings.shadowDirty = builder.BindShaderResource(m_shadowDirtyHierarchyTexture);
            bindings.hasShadowDirty = true;
        }
        if (m_shadowInvalidatedInstancesBitsetBuffer) {
            bindings.invalidatedInstances = builder.BindShaderResource(m_shadowInvalidatedInstancesBitsetBuffer);
            bindings.hasInvalidated = true;
        }
        if (m_shadowPredictiveInvalidationCandidatesBuffer && m_shadowPredictiveInvalidationCandidateCountBuffer) {
            bindings.predictiveCandidates = builder.BindUnorderedAccess(m_shadowPredictiveInvalidationCandidatesBuffer);
            bindings.predictiveCount = builder.BindUnorderedAccess(m_shadowPredictiveInvalidationCandidateCountBuffer);
            bindings.hasPredictive = true;
        }
    }
    if (UsesWorkGraphSWRaster(m_workGraphMode) && UsesVirtualShadowOutput(m_rasterOutputKind)) {
        bindings.shadowPhysicalPages = builder.BindUnorderedAccess(m_shadowPhysicalPagesTexture);
        bindings.shadowDynamicPages = builder.BindUnorderedAccess(m_shadowDynamicPhysicalPagesTexture);
        bindings.shadowDynamicMetadata = builder.BindShaderResource(m_shadowDynamicActiveBlockMetadataBuffer);
        bindings.hasShadowRaster = true;
    }
    if (m_workGraphReyesVisibility) {
        bindings.reyesDice = builder.BindUnorderedAccess(m_reyesDiceQueueBuffer);
        bindings.reyesDiceCounter = builder.BindUnorderedAccess(m_reyesDiceQueueCounterBuffer);
        bindings.reyesOverflow = builder.BindUnorderedAccess(m_reyesDiceQueueOverflowBuffer);
        bindings.reyesConfigs = builder.BindShaderResource(m_reyesTessTableConfigsBuffer);
        bindings.reyesVertices = builder.BindShaderResource(m_reyesTessTableVerticesBuffer);
        bindings.reyesTriangles = builder.BindShaderResource(m_reyesTessTableTrianglesBuffer);
        bindings.reyesTelemetry = builder.BindUnorderedAccess(m_reyesTelemetryBuffer);
        bindings.hasReyes = true;
    }
    if (m_phase1VisibleClustersCounterBuffer) {
        bindings.phase1Counter = builder.BindShaderResource(m_phase1VisibleClustersCounterBuffer);
        bindings.hasPhase1 = true;
    }
    if (UsesSWClassification(m_workGraphMode) && m_swWriteBaseCounterBuffer) {
        bindings.swWriteBase = builder.BindShaderResource(m_swWriteBaseCounterBuffer);
        bindings.hasSwWriteBase = true;
    }
    if (m_voxelRasterWorkCapacity != 0u) {
        bindings.voxelQueues = {builder.BindUnorderedAccess(m_voxelRasterWorkBuffer),
            builder.BindUnorderedAccess(m_voxelRasterWorkCounterBuffer),
            builder.BindUnorderedAccess(m_skinnedVoxelRasterWorkBuffer),
            builder.BindUnorderedAccess(m_skinnedVoxelRasterWorkCounterBuffer)};
        bindings.hasVoxelQueues = true;
    }
    if (m_pageJobVisibleClustersBuffer && m_pageJobVisibleClusterTransformIndicesBuffer && m_pageJobVisibleClustersCounterBuffer) {
        bindings.pageJobQueues = {builder.BindUnorderedAccess(m_pageJobVisibleClustersBuffer),
            builder.BindUnorderedAccess(m_pageJobVisibleClustersCounterBuffer),
            builder.BindUnorderedAccess(m_pageJobVisibleClusterTransformIndicesBuffer)};
        bindings.hasPageJobQueues = true;
    }

    const auto uavIndex = [&](const auto& resource) {
        return builder.DeclaredBindlessIndex(resource,
            {org::BindlessViewKind::UnorderedAccess});
    };
    if (bindings.hasVoxelQueues) {
        CLodVoxelRasterQueueDescriptors descriptors{};
        descriptors.rigidWorkRecordsUAVDescriptorIndex = uavIndex(m_voxelRasterWorkBuffer);
        descriptors.rigidWorkRecordCounterUAVDescriptorIndex = uavIndex(m_voxelRasterWorkCounterBuffer);
        descriptors.skinnedWorkRecordsUAVDescriptorIndex = uavIndex(m_skinnedVoxelRasterWorkBuffer);
        descriptors.skinnedWorkRecordCounterUAVDescriptorIndex = uavIndex(m_skinnedVoxelRasterWorkCounterBuffer);
        descriptors.workRecordCapacity = m_voxelRasterWorkCapacity;
        m_cachedVoxelQueueDescriptors = descriptors;
        m_hasCachedVoxelQueueDescriptors = true;
        UploadBufferData(&descriptors, sizeof(descriptors),
            org::runtime::UploadTarget::FromShared(m_voxelRasterQueueDescriptorsBuffer), 0);
    }
    CLodWorkGraphComputePageJobDescriptors pageJobs{};
    if (bindings.hasPageJobQueues) {
        pageJobs.visibleClustersUAVDescriptorIndex = uavIndex(m_pageJobVisibleClustersBuffer);
        pageJobs.visibleClustersCounterUAVDescriptorIndex = uavIndex(m_pageJobVisibleClustersCounterBuffer);
        pageJobs.visibleClusterTransformIndicesUAVDescriptorIndex =
            uavIndex(m_pageJobVisibleClusterTransformIndicesBuffer);
    }
    m_cachedPageJobDescriptors = pageJobs;
    m_hasCachedPageJobDescriptors = true;
    UploadBufferData(&pageJobs, sizeof(pageJobs),
        org::runtime::UploadTarget::FromShared(m_workGraphComputePageJobDescriptorsBuffer), 0);

    builder.WithInternalTransition(m_visibleClustersCounterBuffer, computeReadState)
        .WithInternalTransition(m_occlusionReplayStateBuffer, computeReadState)
        .WithConstantBuffer(Builtin::PerFrameBuffer);
    return bindings;
}

void HierarchicalCullingPass::Initialize() {
	RegisterSRV(Builtin::CLod::NodeSkinningInfos);
	RegisterSRV(Builtin::CLod::NodeBoneIndices);
    if (UsesWorkGraphSWRaster(m_workGraphMode) && UsesVirtualShadowOutput(m_rasterOutputKind)) {
        RegisterSRV(org::SRVViewType::Texture2DArrayFull, Builtin::Shadows::CLodPageTable);
    }
}


br::render::PreparedComputeCommandSequence HierarchicalCullingPass::BuildRecipe(
    const HierarchicalCullingBindings& bindings, const org::PassPrepareContext& preparation) const {
    const auto* update = preparation.preparationData
        ? preparation.preparationData->Get<UpdateContext>() : nullptr;
    const auto* render = preparation.preparationData
        ? preparation.preparationData->Get<RenderContext>() : nullptr;
    if (!update || !render || !m_workGraph || !m_scratchBuffer) return {};

    br::render::PreparedComputeCommandBuilder preparedCommands(
        preparation,
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    auto& commands = preparedCommands.Commands();
    auto capture = [&](const auto& resource) {
        return preparedCommands.Capture(resource);
    };
    auto bind = [&](const org::PipelineState& pipeline) {
        preparedCommands.Bind(pipeline);
    };
    auto constants = [&](const uint32_t* values) {
        preparedCommands.Constants(
            values, NumMiscUintRootConstants, MiscUintRootSignatureIndex);
    };
    auto dispatch = [&](uint32_t x, uint32_t y = 1, uint32_t z = 1) {
        preparedCommands.Dispatch(x, y, z);
    };
    const auto srv = [&](org::ResourceBindingToken token, uint32_t variant = UINT32_MAX) {
        return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource, variant}).index;
    };
    const auto uav = [&](org::ResourceBindingToken token, uint32_t variant = UINT32_MAX) {
        return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess, variant}).index;
    };
    auto barriers = [&](std::initializer_list<std::shared_ptr<org::Buffer>> resources,
        rhi::ResourceAccessType beforeAccess, rhi::ResourceAccessType afterAccess) {
        preparedCommands.Barriers(resources, beforeAccess, afterAccess);
    };
    auto clear = [&](org::ResourceBindingToken token, const std::shared_ptr<org::Buffer>& buffer, uint32_t count = 1u) {
        if (!buffer) return;
        bind(m_clearPipelineState);
        uint32_t values[NumMiscUintRootConstants]{};
        values[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = uav(token);
        values[CLOD_CLEAR_UINT_BUFFER_COUNT] = count;
        constants(values);
        dispatch((count + 63u) / 64u);
        barriers({buffer}, rhi::ResourceAccessType::UnorderedAccess,
            rhi::ResourceAccessType::UnorderedAccess);
    };

    if (m_isFirstPass && IsCLodWorkGraphTelemetryEnabled())
        clear(bindings.telemetry, m_workGraphTelemetryBuffer, CLodWorkGraphTelemetryBufferCount);
    if (bindings.hasPageJobQueues) clear(bindings.pageJobQueues[1], m_pageJobVisibleClustersCounterBuffer);
    if (bindings.hasVoxelQueues) {
        clear(bindings.voxelQueues[1], m_voxelRasterWorkCounterBuffer);
        clear(bindings.voxelQueues[3], m_skinnedVoxelRasterWorkCounterBuffer);
    }
    if (m_workGraphReyesVisibility && m_isFirstPass) {
        clear(bindings.reyesDiceCounter, m_reyesDiceQueueCounterBuffer);
        clear(bindings.reyesOverflow, m_reyesDiceQueueOverflowBuffer);
    }

    uint32_t root[NumMiscUintRootConstants]{};
    root[CLOD_WG_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = uav(bindings.visible);
    root[CLOD_WG_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = uav(bindings.transforms);
    root[CLOD_WG_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = uav(bindings.visibleCounter);
    root[CLOD_WG_SW_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = uav(bindings.swCounter);
    root[CLOD_WG_HW_WRITE_BASE_COUNTER_DESCRIPTOR_INDEX] = uav(bindings.histogram);
    root[CLOD_WG_TELEMETRY_DESCRIPTOR_INDEX] = uav(bindings.telemetry);
    root[CLOD_WG_FORCED_TRAVERSAL_DEPTH_ROOT] = SettingsManager::GetInstance()
        .getSettingGetter<uint32_t>(CLodForceTraversalDepthRootSettingName)();
    if (UsesSWClassification(m_workGraphMode))
        root[CLOD_WG_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX] = ViewRasterInfoTable(preparation).Publish(preparation, m_viewRasterInfoTable);

    uint32_t flags = 0;
    if (IsCLodWorkGraphTelemetryEnabled()) flags |= CLOD_WG_FLAG_TELEMETRY_ENABLED;
    if (UsesPerViewDepthMapOcclusion(m_rasterOutputKind) && SettingsManager::GetInstance()
        .getSettingGetter<bool>("enableOcclusionCulling")()) flags |= CLOD_WG_FLAG_OCCLUSION_ENABLED;
    if (UsesSWClassification(m_workGraphMode)) flags |= CLOD_WG_FLAG_SW_RASTER_ENABLED;
    if (m_workGraphMode == HierarchicalCullingWorkGraphMode::SoftwareRasterCompute)
        flags |= CLOD_WG_FLAG_COMPUTE_SW_RASTER;
    if (kDisableVirtualShadowDirtyPageCulling && UsesVirtualShadowOutput(m_rasterOutputKind))
        flags |= CLOD_WG_FLAG_DISABLE_SHADOW_DIRTY_PAGE_CULLING;
    if (UsesVirtualShadowOutput(m_rasterOutputKind) && SettingsManager::GetInstance()
        .getSettingGetter<bool>(CLodDirectionalVirtualShadowPredictiveLodInvalidationSettingName)())
        flags |= CLOD_WG_FLAG_VSM_PREDICTIVE_LOD_INVALIDATION;
    const auto threshold = std::min(SettingsManager::GetInstance().getSettingGetter<uint32_t>(
        UsesVirtualShadowOutput(m_rasterOutputKind)
            ? CLodVirtualShadowSoftwareRasterDiameterThresholdSettingName
            : CLodSoftwareRasterDiameterThresholdSettingName)(), 0xFFFFu);
    flags |= threshold << CLOD_WG_SW_RASTER_THRESHOLD_SHIFT;
    if (!m_isFirstPass) flags |= CLOD_WG_FLAG_PHASE2;
    root[CLOD_WG_FLAGS] = flags;

    auto& settings = SettingsManager::GetInstance();
    uint32_t pageJobFlags = 0;
    if (UsesVirtualShadowOutput(m_rasterOutputKind) && CLodVSMRasterModeUsesLargeClusterShadowRouting(
        settings.getSettingGetter<CLodVSMRasterMode>(CLodVSMRasterModeSettingName)()))
        pageJobFlags |= CLOD_WG_PAGE_JOB_FLAG_ENABLED;
    if (settings.getSettingGetter<bool>(CLodPageJobForceAllSettingName)())
        pageJobFlags |= CLOD_WG_PAGE_JOB_FLAG_FORCE_ALL;
    pageJobFlags |= std::min(settings.getSettingGetter<uint32_t>(CLodPageJobDiameterThresholdSettingName)(), 255u)
        << CLOD_WG_PAGE_JOB_DIAMETER_THRESHOLD_SHIFT;
    pageJobFlags |= std::min(static_cast<uint32_t>(settings.getSettingGetter<float>(CLodPageJobSparseRatioSettingName)()
        * 255.0f + 0.5f), 255u) << CLOD_WG_PAGE_JOB_SPARSE_RATIO_SHIFT;
    pageJobFlags |= std::min(settings.getSettingGetter<uint32_t>(CLodPageJobMaxPagesPerClusterSettingName)(), 255u)
        << CLOD_WG_PAGE_JOB_MAX_PAGES_SHIFT;
    root[CLOD_WG_PAGE_JOB_FLAGS] = pageJobFlags;

    if (bindings.hasVirtualShadow) {
        root[CLOD_WG_VIRTUAL_SHADOW_PAGE_TABLE_UAV_DESCRIPTOR_INDEX] =
            uav(bindings.shadowPageTable, static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull));
        root[CLOD_WG_VIRTUAL_SHADOW_ACTIVE_BLOCK_METADATA_DESCRIPTOR_INDEX] = srv(bindings.shadowActiveMetadata);
    }
    if (bindings.hasShadowRaster) {
        root[CLOD_WG_VIRTUAL_SHADOW_PHYSICAL_PAGES_UAV_DESCRIPTOR_INDEX] = uav(bindings.shadowPhysicalPages);
        root[CLOD_WG_VIRTUAL_SHADOW_DYNAMIC_PAGES_UAV_DESCRIPTOR_INDEX] = uav(bindings.shadowDynamicPages);
        root[CLOD_WG_VIRTUAL_SHADOW_DYNAMIC_ACTIVE_BLOCK_METADATA_DESCRIPTOR_INDEX] = srv(bindings.shadowDynamicMetadata);
    }
    if (bindings.hasReyes) {
        root[CLOD_WG_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] = uav(bindings.reyesDice);
        root[CLOD_WG_REYES_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] = uav(bindings.reyesDiceCounter);
        root[CLOD_WG_REYES_DICE_QUEUE_OVERFLOW_DESCRIPTOR_INDEX] = uav(bindings.reyesOverflow);
        root[CLOD_WG_REYES_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = srv(bindings.reyesConfigs);
        root[CLOD_WG_REYES_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX] = srv(bindings.reyesVertices);
        root[CLOD_WG_REYES_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX] = srv(bindings.reyesTriangles);
        root[CLOD_WG_REYES_TELEMETRY_DESCRIPTOR_INDEX] = uav(bindings.reyesTelemetry);
        root[CLOD_WG_REYES_DICE_QUEUE_CAPACITY] = m_reyesDiceQueueCapacity;
    }
    root[CLOD_WG_OCCLUSION_REPLAY_BUFFER_DESCRIPTOR_INDEX] = uav(bindings.replay);
    root[CLOD_WG_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX] = uav(bindings.replayState);
    root[CLOD_WG_WORKGRAPH_NODE_INPUTS_DESCRIPTOR_INDEX] = uav(bindings.nodeInputs);
    root[CLOD_WG_VIEW_DEPTH_SRV_INDICES_DESCRIPTOR_INDEX] = bindings.hasViewDepth
        ? ViewDepthTable(preparation).Publish(preparation, m_viewDepthTable) : 0u;
    root[CLOD_WG_VISIBLE_CLUSTERS_CAPACITY] = static_cast<uint32_t>(m_maxVisibleClusters);
    root[CLOD_WG_SHADOW_DIRTY_HIERARCHY_DESCRIPTOR_INDEX] = bindings.hasShadowDirty
        ? srv(bindings.shadowDirty, static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull)) : 0u;
    root[CLOD_WG_SHADOW_INVALIDATED_INSTANCES_DESCRIPTOR_INDEX] = bindings.hasInvalidated
        ? srv(bindings.invalidatedInstances) : 0u;
    root[CLOD_WG_SHADOW_PREDICTIVE_INVALIDATION_CANDIDATES_DESCRIPTOR_INDEX] = bindings.hasPredictive
        ? uav(bindings.predictiveCandidates) : 0u;
    root[CLOD_WG_SHADOW_PREDICTIVE_INVALIDATION_CANDIDATE_COUNT_DESCRIPTOR_INDEX] = bindings.hasPredictive
        ? uav(bindings.predictiveCount) : 0u;
    root[CLOD_WG_HW_WRITE_BASE_COUNTER_DESCRIPTOR_INDEX] = srv(
        bindings.hasPhase1 ? bindings.phase1Counter : bindings.visibleCounter);
    root[CLOD_WG_SW_WRITE_BASE_COUNTER_DESCRIPTOR_INDEX] = srv(
        bindings.hasSwWriteBase ? bindings.swWriteBase : bindings.swCounter);

    preparedCommands.SetWorkGraph(m_workGraph, capture(m_scratchBuffer), false);
    commands.emplace_back(br::render::PreparedComputeDescriptorIndices{
        CaptureResourceDescriptorIndices(m_pipelineResources)});
    constants(root);

    if (m_isFirstPass) {
        commands.emplace_back(br::render::PreparedWorkGraphCpuDispatch{});
        barriers({m_visibleClustersCounterBuffer, m_occlusionReplayStateBuffer, m_occlusionReplayBuffer},
            rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceAccessType::UnorderedAccess);
    } else {
        barriers({m_occlusionReplayBuffer, m_occlusionNodeGpuInputsBuffer},
            rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceAccessType::UnorderedAccess);
        commands.emplace_back(br::render::PreparedWorkGraphGpuDispatch{capture(m_occlusionNodeGpuInputsBuffer), 0});
    }

    br::render::PreparedBufferBarrierBatch outputBarriers;
    auto appendOutput = [&](const std::shared_ptr<org::Buffer>& resource,
        rhi::ResourceAccessType after) {
        if (resource) outputBarriers.barriers.push_back({capture(resource),
            rhi::ResourceAccessType::UnorderedAccess, after,
            rhi::ResourceSyncState::ComputeShading, rhi::ResourceSyncState::ComputeShading});
    };
    appendOutput(m_visibleClustersCounterBuffer, rhi::ResourceAccessType::ShaderResource);
    appendOutput(m_occlusionReplayStateBuffer, rhi::ResourceAccessType::ShaderResource);
    if (UsesSWClassification(m_workGraphMode)) appendOutput(m_swVisibleClustersCounterBuffer,
        rhi::ResourceAccessType::UnorderedAccess);
    if (m_voxelRasterWorkCapacity) {
        appendOutput(m_voxelRasterWorkCounterBuffer, rhi::ResourceAccessType::ShaderResource);
        appendOutput(m_skinnedVoxelRasterWorkCounterBuffer, rhi::ResourceAccessType::ShaderResource);
        appendOutput(m_voxelRasterWorkBuffer, rhi::ResourceAccessType::ShaderResource);
        appendOutput(m_skinnedVoxelRasterWorkBuffer, rhi::ResourceAccessType::ShaderResource);
    }
    commands.emplace_back(std::move(outputBarriers));

    bind(m_createCommandPipelineState);
    root[CLOD_CREATE_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.visibleCounter);
    root[CLOD_CREATE_RASTER_BUCKET_HISTOGRAM_COMMAND_DESCRIPTOR_INDEX] = uav(bindings.histogram);
    root[CLOD_CREATE_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX] = srv(bindings.replayState);
    root[CLOD_CREATE_WORKGRAPH_NODE_INPUTS_DESCRIPTOR_INDEX] = uav(bindings.nodeInputs);
    root[CLOD_CREATE_NUM_RASTER_BUCKETS] = render->preparedRasterBucketCount;
    root[CLOD_CREATE_VISIBLE_CLUSTERS_CAPACITY] = static_cast<uint32_t>(m_maxVisibleClusters);
    root[CLOD_CREATE_NODE_INPUT_COUNT] = m_workGraphReyesVisibility ? 4u : 2u;
    const auto replayRef = capture(m_occlusionReplayBuffer);
    const auto inputRef = capture(m_occlusionNodeGpuInputsBuffer);
    const auto replayAddress = GetNativeBufferDeviceAddress(
        preparedCommands.Context().ResolveCapturedResource(replayRef));
    const auto inputAddress = GetNativeBufferDeviceAddress(
        preparedCommands.Context().ResolveCapturedResource(inputRef));
    commands.emplace_back(br::render::PreparedComputeAddressConstants{
        MiscUintRootSignatureIndex, 0,
        std::vector<uint32_t>(root, root + NumMiscUintRootConstants),
        {{replayAddress, CLOD_CREATE_REPLAY_ADDRESS_LOW, CLOD_CREATE_REPLAY_ADDRESS_HIGH},
         {inputAddress, CLOD_CREATE_NODE_INPUT_ADDRESS_LOW, CLOD_CREATE_NODE_INPUT_ADDRESS_HIGH}}});
    commands.emplace_back(br::render::PreparedDispatchGroups{1, 1, 1});

    return std::move(preparedCommands).FinishData();
}

std::vector<uint64_t> HierarchicalCullingPass::RecipeRevision(const org::PassPrepareContext& preparation) const {
    const auto* render = preparation.preparationData ? preparation.preparationData->Get<RenderContext>() : nullptr;
    std::vector<uint64_t> revision{SettingsManager::GetInstance().Revision(),
        reinterpret_cast<uintptr_t>(m_workGraph.get()),
        reinterpret_cast<uintptr_t>(m_clearPipelineState.PeekPayload()),
        reinterpret_cast<uintptr_t>(m_createCommandPipelineState.PeekPayload()),
        render ? render->preparedRasterBucketCount : 0u};
    if (preparation.preparationData && preparation.preparationData->Get<UpdateContext>()) {
        if (UsesSWClassification(m_workGraphMode)) ViewRasterInfoTable(preparation).AppendRevision(preparation, revision);
        if (UsesPerViewDepthMapOcclusion(m_rasterOutputKind)) ViewDepthTable(preparation).AppendRevision(preparation, revision);
    }
    return revision;
}

CLodViewRasterInfoTable HierarchicalCullingPass::ViewRasterInfoTable(const org::PassPrepareContext& preparation) const {
    const auto& context = CLodPreparationSnapshot(preparation);
    return BuildCLodVisibilityViewRasterInfo(context.Views(), context.ViewCameraBufferSize(), m_rasterOutputKind);
}

CLodViewDepthTable HierarchicalCullingPass::ViewDepthTable(const org::PassPrepareContext& preparation) const {
    return BuildCLodViewDepthTable(CLodPreparationSnapshot(preparation).Views(), m_isFirstPass);
}

HierarchicalCullingInvocation HierarchicalCullingPass::PrepareInvocation(
    const br::render::PreparedComputeCommandSequence&, const HierarchicalCullingBindings&,
    const org::PassPrepareContext& preparation) const {
    HierarchicalCullingInvocation invocation;
    const auto* render = preparation.preparationData ? preparation.preparationData->Get<RenderContext>() : nullptr;
    if (!render || !m_workGraph || !m_scratchBuffer) return invocation;
    invocation.initializeBacking = !m_workGraphInitialization->initialized.load(std::memory_order_acquire);
    if (invocation.initializeBacking) preparation.Reserve(m_workGraphInitialization,
        +[](WorkGraphInitializationState& state, org::SubmissionContext) {
            state.initialized.store(true, std::memory_order_release);
        });
    if (m_isFirstPass) {
        const auto workloads = br::render::PrepareCullingWorkloads(
            render->Views(), render->publishedRendererState, m_renderPhase,
            m_clodOnlyWorkloads, m_useShadowCascadeViews, m_rasterOutputKind, 64u,
            "HierarchicalCullingPass");
        std::vector<ObjectCullRecord> records;
        records.reserve(workloads.size());
        for (const auto& workload : workloads) records.push_back({
            workload.viewDataIndex, workload.activeDrawSetIndicesSRVIndex,
            workload.activeDrawCount, workload.drawRecordVisibilityGenerationSRVIndex,
            workload.dispatchGridX, workload.dispatchGridY, workload.dispatchGridZ});
        basic_telemetry::SetGauge("SARP.Culling.GraphWorkloadRecords.WorkGraph",
            static_cast<std::int64_t>(records.size()));
        invocation.cpuDispatch = br::render::PreparedWorkGraphCpuDispatch::From(0u, records);
    }
    return invocation;
}

void HierarchicalCullingPass::Update(const org::UpdateExecutionContext& executionContext) {
    ZoneScopedN("HierarchicalCullingPass::Update");

    auto* updateContext = executionContext.hostData ? executionContext.hostData->Get<UpdateContext>() : nullptr;
    if (!updateContext) {
        return;
    }
    auto& context = *updateContext;
    m_declaredResourcesChanged = false;

    {
        ZoneScopedN("HierarchicalCullingPass::CheckDeclaredDrawSetRevision");
        const uint64_t drawSetRevision = context.publishedRendererState
            ? context.publishedRendererState->activeDrawLists.revision : 0u;
        if (drawSetRevision != m_lastDrawSetDeclarationRevision) {
            ZoneScopedN("HierarchicalCullingPass::CollectDeclaredDrawSets");
            m_lastDrawSetDeclarationRevision = drawSetRevision;
            const std::vector<uint64_t> currentDrawSetResourceIds = CollectDeclaredDrawSetResourceIds(
                context.publishedRendererState, m_renderPhase, m_clodOnlyWorkloads);
            if (currentDrawSetResourceIds != m_declaredDrawSetResourceIds) {
                m_declaredDrawSetResourceIds = currentDrawSetResourceIds;
                m_declaredResourcesChanged = true;
            }
        }
    }

    uint32_t zero = 0u;
    {
        ZoneScopedN("HierarchicalCullingPass::UploadCounterResets");
        UploadBufferData(&zero, sizeof(uint32_t), org::runtime::UploadTarget::FromShared(m_visibleClustersCounterBuffer), 0);
        if (UsesSWClassification(m_workGraphMode)) {
            UploadBufferData(&zero, sizeof(uint32_t), org::runtime::UploadTarget::FromShared(m_swVisibleClustersCounterBuffer), 0);
        }
    }

    bool rebuildViewTables = false;
    {
        ZoneScopedN("HierarchicalCullingPass::CheckViewResourceRevision");
        const uint64_t viewResourceRevision = context.ViewResourceLayoutRevision();
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
            // Descriptor-free rows for the passes that read the shared buffer
            // (virtual shadow page jobs). Passes whose shaders need descriptors
            // publish their own table during preparation.
            m_visibilityBuffers.clear();
            const auto table = BuildCLodVisibilityViewRasterInfo(context.Views(), context.ViewCameraBufferSize(), m_rasterOutputKind);
            std::vector<CLodViewRasterInfo> viewRasterInfo(table.Rows().begin(), table.Rows().end());
            std::vector<std::pair<uint32_t, std::shared_ptr<org::PixelBuffer>>> visibilityBuffersByCameraIndex;
            if (UsesWorkGraphSWRaster(m_workGraphMode) && !UsesVirtualShadowOutput(m_rasterOutputKind))
                for (const auto& viewInfo : context.Views())
                    if (viewInfo.visibilityBuffer && viewInfo.cameraBufferIndex < viewRasterInfo.size())
                        visibilityBuffersByCameraIndex.emplace_back(viewInfo.cameraBufferIndex, viewInfo.visibilityBuffer);

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
                UploadBufferData(
                    m_cachedViewRasterInfo.data(),
                    static_cast<uint32_t>(m_cachedViewRasterInfo.size() * sizeof(CLodViewRasterInfo)),
                    org::runtime::UploadTarget::FromShared(m_viewRasterInfoBuffer),
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
        UploadBufferData(
            &replayState,
            sizeof(CLodReplayBufferState),
            org::runtime::UploadTarget::FromShared(m_occlusionReplayStateBuffer),
            0);
    }

#if 0 // Replaced by runtime GPU-address patching in the phase-1 command shader.
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
            UploadBufferData(
                nodeGpuInputs,
                sizeof(nodeGpuInputs),
                org::runtime::UploadTarget::FromShared(m_occlusionNodeGpuInputsBuffer),
                0);
        }
    }
#endif

    if (IsCLodWorkGraphTelemetryEnabled()) {
        ZoneScopedN("HierarchicalCullingPass::UploadTelemetryReset");
        m_zeroTelemetryScratch.assign(CLodWorkGraphTelemetryBufferCount, 0u);
        UploadBufferData(
            m_zeroTelemetryScratch.data(),
            static_cast<uint32_t>(m_zeroTelemetryScratch.size() * sizeof(uint32_t)),
            org::runtime::UploadTarget::FromShared(m_workGraphTelemetryBuffer),
            0);
    }

    if (m_workGraphReyesVisibility && m_reyesTelemetryBuffer) {
        ZoneScopedN("HierarchicalCullingPass::UploadReyesTelemetryReset");
        CLodReyesTelemetry telemetry{};
        telemetry.phaseIndex = m_isFirstPass ? 1u : 2u;
        telemetry.configuredMaxSplitPassCount = CLodReyesMaxSplitPassCount;
        UploadBufferData(&telemetry, sizeof(CLodReyesTelemetry), org::runtime::UploadTarget::FromShared(m_reyesTelemetryBuffer), 0);
    }
}

bool HierarchicalCullingPass::DeclaredResourcesChanged() const {
    return m_declaredResourcesChanged;
}

std::shared_ptr<org::Resource> HierarchicalCullingPass::ProvideResource(org::ResourceIdentifier const& key)
{
    if (key == m_workGraphComputePageJobDescriptorResourceId) {
        return m_workGraphComputePageJobDescriptorsBuffer;
    }

    if (key == m_voxelRasterQueueDescriptorResourceId) {
        return m_voxelRasterQueueDescriptorsBuffer;
    }

    return nullptr;
}

std::vector<org::ResourceIdentifier> HierarchicalCullingPass::GetSupportedKeys()
{
    std::vector<org::ResourceIdentifier> keys{
        org::ResourceIdentifier{ m_workGraphComputePageJobDescriptorResourceId }
    };
    if (m_voxelRasterQueueDescriptorsBuffer) {
        keys.emplace_back(m_voxelRasterQueueDescriptorResourceId);
    }
    return keys;
}

void HierarchicalCullingPass::CreatePipelines(
    rhi::Device device,
    rhi::PipelineLayoutHandle globalRootSignature,
    rhi::WorkGraphPtr& outGraph,
    org::PipelineState& outCreateCommandPipeline,
    org::PipelineState& outClearPipeline)
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
    const bool pageJobAlwaysDedicated =
        m_pageJobVisibleClustersBuffer &&
        m_pageJobVisibleClusterTransformIndicesBuffer &&
        m_pageJobVisibleClustersCounterBuffer;
    constexpr bool splitLeafTraversalNode = true;
    const bool rigidOnly = SettingsManager::GetInstance()
        .getSettingGetter<bool>(CLodWorkGraphRigidOnlySettingName)();
    std::vector<DxcDefine> defines = {
        { L"CLOD_WG_ENABLE_SW_CLASSIFICATION", UsesSWClassification(m_workGraphMode) ? L"1" : L"0" },
        { L"CLOD_WG_ENABLE_SW_NODE_OUTPUT", UsesWorkGraphSWRaster(m_workGraphMode) ? L"1" : L"0" },
        { L"CLOD_WG_ENABLE_REYES_VISIBILITY", m_workGraphReyesVisibility ? L"1" : L"0" },
        { L"CLOD_WG_ENABLE_VOXEL_OUTPUT", m_voxelRasterWorkCapacity != 0u ? L"1" : L"0" },
        { L"CLOD_WG_SPLIT_LEAF_NODE", splitLeafTraversalNode ? L"1" : L"0" },
        { L"CLOD_WG_RIGID_ONLY", rigidOnly ? L"1" : L"0" },
        { L"CLOD_WG_PAGE_JOB_ALWAYS_DEDICATED", pageJobAlwaysDedicated ? L"1" : L"0" },
        { L"CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW", UsesVirtualShadowOutput(m_rasterOutputKind) ? L"1" : L"0" },
        { L"CLOD_VSM_TWO_LAYER_WORKGRAPH_VERSION", UsesVirtualShadowOutput(m_rasterOutputKind) ? L"2" : L"0" },
        { L"CLOD_WG_ENABLE_COMPUTE_PAGE_JOB_DESCRIPTOR_BUFFER", enableComputePageJobDescriptorBuffer ? L"1" : L"0" },
        { L"CLOD_WG_COMPUTE_PAGE_JOB_DESCRIPTOR_BUFFER_ID", pageJobDescriptorResourceIdDefine.c_str() },
        { L"CLOD_WG_VOXEL_RASTER_QUEUE_DESCRIPTOR_BUFFER_ID", voxelQueueDescriptorResourceIdDefine.c_str() },
    };
    auto compiled = PSOManager::GetInstance().CompileShaderLibrary(libInfo, defines);
    m_pipelineResources = compiled.resourceDescriptorSlots;
    const org::ResourceIdentifier voxelDescriptorId{ m_voxelRasterQueueDescriptorResourceId };
    const bool shaderRequestsVoxelDescriptor = std::ranges::find(
        m_pipelineResources.mandatoryResourceDescriptorSlots,
        voxelDescriptorId) != m_pipelineResources.mandatoryResourceDescriptorSlots.end();
    if (shaderRequestsVoxelDescriptor != (m_voxelRasterWorkCapacity != 0u)) {
        throw std::runtime_error(
            "CLod traversal shader voxel-output variant has an inconsistent resource interface");
    }
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
