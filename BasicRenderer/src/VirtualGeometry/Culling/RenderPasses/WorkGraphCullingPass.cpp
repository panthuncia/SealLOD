#include "VirtualGeometry/Culling/RenderPasses/WorkGraphCullingPass.h"
#include "Render/PassBuilders.h"
#include "Render/ShaderAPI.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <stdexcept>
#include <utility>
#include <string>
#include <vector>

#include <rhi_interop.h>
#include <rhi_interop_dx12.h>
#include <tracy/Tracy.hpp>
#include <BasicTelemetry/Telemetry.h>

#include "BasicRenderer/Diagnostics/CLodTelemetry.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BuiltinResources.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Render/Runtime/UploadTypes.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "Runtime/Device/DeviceManager.h"
#include "Runtime/GraphIntegration/Resolvers/ResourceGroupResolver.h"
#include "Runtime/Settings/SettingsManager.h"
#include "VirtualGeometry/Culling/PreparedCullingWorkloads.h"
#include "../shaders/PerPassRootConstants/clodClearUintBufferRootConstants.h"
#include "../shaders/PerPassRootConstants/clodCreateCommandRootConstants.h"
#include "../shaders/PerPassRootConstants/clodWorkGraphRootConstants.h"

namespace {

std::vector<WorkGraphCullingPass*> g_liveWorkGraphCullingPasses;

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

}

WorkGraphCullingPass::WorkGraphCullingPass(CullingPassConfig config, CullingPassResources resources)
    : m_config(std::move(config))
    , m_resources(std::move(resources))
    , m_voxelQueueTable(m_config.stablePassIdentifier, "CLod Work Graph Voxel Raster Queue Descriptors",
        m_resources.voxelQueues.Enabled())
{
    auto& psoManager = PSOManager::GetInstance();
    const auto layout = psoManager.GetComputeRootSignature().GetHandle();
    m_createCommandPipelineState = psoManager.MakeComputePipeline(layout,
        L"shaders/ClusterLOD/clodUtil.hlsl", L"CreateRasterBucketsHistogramCommandCSMain", {},
        "HierarchicalLODCommandCreation");
    m_clearPipelineState = psoManager.MakeComputePipeline(layout,
        L"shaders/ClusterLOD/clodUtil.hlsl", L"ClearUintStructuredBufferCSMain", {},
        "HierarchicalCullingClearUintPSO");
    InstallWorkGraph(CompileWorkGraph());
    g_liveWorkGraphCullingPasses.push_back(this);
}

WorkGraphCullingPass::~WorkGraphCullingPass()
{
    std::erase(g_liveWorkGraphCullingPasses, this);
}

size_t WorkGraphCullingPass::ReloadAllWorkGraphs()
{
    for (WorkGraphCullingPass* pass : g_liveWorkGraphCullingPasses) {
        pass->InstallWorkGraph(pass->CompileWorkGraph());
    }
    return g_liveWorkGraphCullingPasses.size();
}

void WorkGraphCullingPass::InstallWorkGraph(CompiledWorkGraph compiled)
{
    if (!compiled.graph) throw std::runtime_error("CLod work-graph compilation returned a null work graph");
    const uint64_t scratchSize = compiled.graph->GetRequiredScratchMemorySize();
    if (!m_scratchBuffer || scratchSize != (*m_workGraph)->GetRequiredScratchMemorySize()) {
        m_scratchBuffer = org::Buffer::CreateShared(rhi::HeapType::DeviceLocal, scratchSize, true);
        m_scratchBuffer->SetName("CLod Work Graph Scratch Buffer");
        m_scratchBuffer->SetMemoryUsageHint("Work graph scratch buffer");
    }
    m_workGraph = std::make_shared<rhi::WorkGraphPtr>(std::move(compiled.graph));
    // The graph is not a pipeline state, but its descriptor slots resolve from
    // the pass's declarations the same way a pipeline's do.
    auto slots = std::make_shared<org::PipelineStatePayload>(rhi::PipelinePtr{}, 0u, std::move(compiled.resources),
        1u, 0u, 0u, std::string("HierarchicalCulling work graph"));
    slots->layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
    m_workGraphSlots = std::move(slots);
    m_workGraphInitialization->initialized.store(false, std::memory_order_release);
}

WorkGraphCullingPass::CompiledWorkGraph WorkGraphCullingPass::CompileWorkGraph() const
{
    const bool swClassification = m_config.UsesSWClassification();
    const bool swRasterNode = m_config.softwareRaster == CLodSoftwareRasterMode::WorkGraph;
    const bool virtualShadow = m_config.UsesVirtualShadowOutput();
    const bool reyes = m_resources.reyes.enabled;
    const bool voxels = m_voxelQueueTable.Enabled();
    const bool rigidOnly = SettingsManager::GetInstance().getSettingGetter<bool>(CLodWorkGraphRigidOnlySettingName)();
    const std::wstring voxelTableDefine = m_voxelQueueTable.ShaderDefine();
    const std::vector<DxcDefine> defines = {
        { L"CLOD_WG_ENABLE_SW_CLASSIFICATION", swClassification ? L"1" : L"0" },
        { L"CLOD_WG_ENABLE_SW_NODE_OUTPUT", swRasterNode ? L"1" : L"0" },
        { L"CLOD_WG_ENABLE_REYES_VISIBILITY", reyes ? L"1" : L"0" },
        { L"CLOD_WG_ENABLE_VOXEL_OUTPUT", voxels ? L"1" : L"0" },
        { L"CLOD_WG_SPLIT_LEAF_NODE", L"1" },
        { L"CLOD_WG_RIGID_ONLY", rigidOnly ? L"1" : L"0" },
        { L"CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW", virtualShadow ? L"1" : L"0" },
        { L"CLOD_VSM_TWO_LAYER_WORKGRAPH_VERSION", virtualShadow ? L"2" : L"0" },
        { L"CLOD_WG_VOXEL_RASTER_QUEUE_DESCRIPTOR_BUFFER_ID", voxelTableDefine.c_str() },
    };
    ShaderLibraryInfo libInfo(L"shaders/ClusterLOD/workGraphCulling.hlsl", L"lib_6_8");
    auto compiled = PSOManager::GetInstance().CompileShaderLibrary(libInfo, defines);
    m_voxelQueueTable.ValidateInterface(compiled.resourceDescriptorSlots, voxels, "workGraphCulling");

    std::vector<rhi::ShaderExportDesc> exports = {
        { "WG_ObjectCull", nullptr },
        { "WG_TraverseNodes", nullptr },
        { "WG_LeafNodes", nullptr },
        { "WG_ClusterCull1", nullptr },
        { "WG_ClusterCull2", nullptr },
        { "WG_ClusterCull4", nullptr },
        { "WG_ClusterCull8", nullptr },
        { "WG_ClusterCull16", nullptr },
        { "WG_ClusterCull32", nullptr },
        { "WG_ClusterCull64", nullptr },
    };
    if (swRasterNode) exports.push_back({ "WG_SWRaster", nullptr });
    std::vector<rhi::NodeIDDesc> entrypoints = {
        { "ObjectCull", 0 },
        { "TraverseNodes", 0 },
        { "ClusterCull1", 0 },
    };
    if (reyes) {
        for (const char* node : { "WG_ReyesSeed", "WG_ReyesSplit1", "WG_ReyesSplit2", "WG_ReyesSplit3",
                 "WG_ReyesSplit4", "WG_ReyesSplit5", "WG_ReyesDice", "WG_ReyesRaster",
                 "WG_ReyesSplitReplay", "WG_ReyesDiceReplay" }) {
            exports.push_back({ node, nullptr });
        }
        // Phase 2 replays Reyes work through these two extra entry points.
        entrypoints.push_back({ "ReyesSplitReplay", 0 });
        entrypoints.push_back({ "ReyesDiceReplay", 0 });
    }

    rhi::ShaderLibraryDesc library{};
    library.dxil = rhi::ShaderBinary{
        compiled.libraryBlob->GetBufferPointer(),
        static_cast<uint32_t>(compiled.libraryBlob->GetBufferSize()) };
    library.exports = rhi::Span<rhi::ShaderExportDesc>(exports.data(), static_cast<uint32_t>(exports.size()));

    rhi::WorkGraphDesc desc{};
    desc.programName = "HierarchicalCulling";
    desc.flags = rhi::WorkGraphFlags::WorkGraphFlagsIncludeAllAvailableNodes;
    desc.globalRootSignature = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
    desc.libraries = rhi::Span<rhi::ShaderLibraryDesc>(&library, 1);
    desc.entrypoints = rhi::Span<rhi::NodeIDDesc>(entrypoints.data(), static_cast<uint32_t>(entrypoints.size()));
    desc.allowStateObjectAdditions = false;
    switch (m_config.softwareRaster) {
    case CLodSoftwareRasterMode::Disabled: desc.debugName = "HierarchicalCullingWG.HW"; break;
    case CLodSoftwareRasterMode::Compute: desc.debugName = "HierarchicalCullingWG.ComputeSW"; break;
    case CLodSoftwareRasterMode::WorkGraph: desc.debugName = "HierarchicalCullingWG.WorkGraphSW"; break;
    }

    CompiledWorkGraph result;
    DeviceManager::GetInstance().GetDevice().CreateWorkGraph(desc, result.graph);
    result.resources = compiled.resourceDescriptorSlots;
    return result;
}

br::render::CullingUploader WorkGraphCullingPass::Uploader() const
{
    return [this](const void* data, size_t size, const std::shared_ptr<org::Buffer>& target) {
        UploadBufferData(data, size, org::runtime::UploadTarget::FromShared(target), 0);
    };
}

org::ResourceBindingToken WorkGraphCullingPass::DeclareSceneInputs(org::PassBuilder& builder)
{
    const auto scratch = builder.UnorderedAccess(m_scratchBuffer).Resource();
    builder.UnorderedAccess(
        Builtin::CLod::StreamingLoadRequestKeys,
        Builtin::CLod::StreamingLoadRequests,
        Builtin::CLod::StreamingLoadCounter,
        Builtin::CLod::StreamingRuntimeState,
        Builtin::CLod::StreamingTouchedGroupsCounter,
        Builtin::CLod::StreamingTouchedGroups);
    builder.ShaderResource(
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
        Builtin::PerMaterialDataBuffer,
        Builtin::Material::TextureStreamingMetadataBuffer);
    builder.UnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer);
    br::render::DeclareCullingWorkloadInputs(builder, m_config);

    if (m_voxelQueueTable.Enabled()) {
        builder.ShaderResource(m_voxelQueueTable.Id().c_str());
    }
    if (m_config.UsesPerViewDepthOcclusion()) {
        m_viewDepthLayout.Declare(builder);
    }
    if (m_config.UsesSWClassification()) {
        m_viewRasterInfoLayout.Declare(builder);
    }
    if (m_config.UsesVirtualShadowOutput()) {
        builder.ShaderResource(
            Builtin::Shadows::CLodClipmapInfo,
            Builtin::Shadows::CLodDirectionalPageViewInfo,
            Builtin::Shadows::CLodCompactShadowCameras);
        builder.UnorderedAccess(Builtin::Shadows::CLodPageTable);
        if (m_config.softwareRaster == CLodSoftwareRasterMode::WorkGraph)
            builder.ShaderResource(Builtin::Shadows::CLodPageTable,
                org::SrvView{static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull)});
    }
    if (m_resources.reyes.enabled) {
        builder.ShaderResource(
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
            Builtin::Terrain::RvtMaterialAtlas);
        builder.UnorderedAccess(
            Builtin::Terrain::RvtRequestMasks,
            Builtin::Terrain::RvtRequestList,
            Builtin::Terrain::RvtCounters,
            Builtin::Terrain::RvtStats);
    }

    builder.UnorderedAccess(Builtin::DebugVisualization);
    // Page pool slabs are a group, so new slabs invalidate the declaration.
    if (m_resources.pagePoolSlabs) {
        builder.ShaderResource(ResourceGroupResolver(m_resources.pagePoolSlabs));
    }
    return scratch;
}

std::optional<WorkGraphVirtualShadowBindings> WorkGraphCullingPass::DeclareVirtualShadow(org::PassBuilder& builder)
{
    if (!m_config.UsesVirtualShadowOutput()) return std::nullopt;
    const auto& shadow = m_resources.virtualShadow;
    const org::SrvView arraySrv{static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull)};
    WorkGraphVirtualShadowBindings bindings{
        .pageTable = builder.UnorderedAccess(shadow.pageTable,
            org::UavView{static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull)}).View(),
        .activeMetadata = builder.ShaderResource(shadow.activeBlockMetadata).View(),
    };
    if (shadow.dirtyPageHierarchy)
        bindings.dirtyHierarchy = builder.ShaderResource(shadow.dirtyPageHierarchy, arraySrv).View();
    if (shadow.invalidatedInstancesBitset)
        bindings.invalidatedInstances = builder.ShaderResource(shadow.invalidatedInstancesBitset).View();
    if (shadow.predictiveInvalidationCandidates && shadow.predictiveInvalidationCandidateCount) {
        bindings.predictiveInvalidation = WorkGraphVirtualShadowBindings::PredictiveInvalidation{
            builder.UnorderedAccess(shadow.predictiveInvalidationCandidates).View(),
            builder.UnorderedAccess(shadow.predictiveInvalidationCandidateCount).View()};
    }
    if (m_config.softwareRaster == CLodSoftwareRasterMode::WorkGraph) {
        bindings.pageRaster = WorkGraphVirtualShadowBindings::PageRaster{
            builder.UnorderedAccess(shadow.staticPhysicalPages).View(),
            builder.UnorderedAccess(shadow.dynamicPhysicalPages).View(),
            builder.ShaderResource(shadow.dynamicActiveBlockMetadata).View()};
    }
    return bindings;
}

std::optional<WorkGraphReyesBindings> WorkGraphCullingPass::DeclareReyes(org::PassBuilder& builder)
{
    const auto& reyes = m_resources.reyes;
    if (!reyes.enabled) return std::nullopt;
    return WorkGraphReyesBindings{
        builder.UnorderedAccess(reyes.diceQueue).View(),
        builder.UnorderedAccess(reyes.diceQueueCounter).View(),
        builder.UnorderedAccess(reyes.diceQueueOverflow).View(),
        builder.ShaderResource(reyes.tessTableConfigs).View(),
        builder.ShaderResource(reyes.tessTableVertices).View(),
        builder.ShaderResource(reyes.tessTableTriangles).View(),
        builder.UnorderedAccess(reyes.telemetry).View()};
}

WorkGraphCullingBindings WorkGraphCullingPass::Declare(org::PassBuilder& builder)
{
    const auto scratch = DeclareSceneInputs(builder);

    const auto& outputs = m_resources.outputs;
    const auto& replay = m_resources.occlusionReplay;
    WorkGraphCullingBindings bindings{};
    bindings.clearPipeline = m_clearPipelineState;
    bindings.createCommandPipeline = m_createCommandPipelineState;
    bindings.scratch = scratch;
    bindings.maxVisibleClusters = m_config.maxVisibleClusters;
    bindings.reyesDiceQueueCapacity = m_resources.reyes.diceQueueCapacity;
    bindings.isFirstPass = m_config.isFirstPass;
    bindings.swClassification = m_config.UsesSWClassification();
    bindings.perViewDepthOcclusion = m_config.UsesPerViewDepthOcclusion();
    if (m_config.UsesPerViewDepthOcclusion()) bindings.viewDepthLayout = m_viewDepthLayout.Layout();
    if (m_config.UsesSWClassification()) bindings.viewRasterInfoLayout = m_viewRasterInfoLayout.Layout();
    bindings.visible = builder.UnorderedAccess(outputs.visibleClusters).View();
    bindings.transforms = builder.UnorderedAccess(outputs.visibleClusterTransformIndices).View();
    bindings.visibleCounter = builder.UnorderedAccess(outputs.visibleClustersCounter).View();
    bindings.visibleCounterSrv = builder.ShaderResource(outputs.visibleClustersCounter).View();
    bindings.swCounter = builder.UnorderedAccess(outputs.swVisibleClustersCounter).View();
    bindings.histogram = builder.UnorderedAccess(outputs.rasterBucketHistogramCommand).View();
    bindings.telemetry = builder.UnorderedAccess(m_resources.telemetry).View();
    bindings.replay = builder.UnorderedAccess(replay.replay).View();
    bindings.replayState = builder.UnorderedAccess(replay.state).View();
    bindings.replayStateSrv = builder.ShaderResource(replay.state).View();
    bindings.nodeInputs = builder.UnorderedAccess(replay.nodeGpuInputs).View();
    bindings.virtualShadow = DeclareVirtualShadow(builder);
    bindings.reyes = DeclareReyes(builder);
    if (m_resources.phase1Counters.visibleClustersCounter) {
        bindings.phase1Counter = builder.ShaderResource(m_resources.phase1Counters.visibleClustersCounter).View();
    }
    if (m_config.UsesSWClassification() && m_resources.phase1Counters.swVisibleClustersCounter) {
        bindings.phase1SwCounter = builder.ShaderResource(m_resources.phase1Counters.swVisibleClustersCounter).View();
    }
    if (m_voxelQueueTable.Enabled()) {
        const auto& voxels = m_resources.voxelQueues;
        bindings.voxelQueues = std::array{
            builder.UnorderedAccess(voxels.rigidWork).View(),
            builder.UnorderedAccess(voxels.rigidWorkCounter).View(),
            builder.UnorderedAccess(voxels.skinnedWork).View(),
            builder.UnorderedAccess(voxels.skinnedWorkCounter).View()};
    }

    const org::ResourceState computeReadState{
        rhi::ResourceAccessType::ShaderResource,
        rhi::ResourceLayout::ShaderResource,
        rhi::ResourceSyncState::ComputeShading
    };
    builder.InternalTransition(outputs.visibleClustersCounter, computeReadState)
        .InternalTransition(replay.state, computeReadState);
    builder.ConstantBuffer(Builtin::PerFrameBuffer);
    return bindings;
}

namespace {

// The root constants every node of the graph reads.
void FillGraphConstants(br::render::MiscConstants& root, const WorkGraphCullingBindings& bindings,
    const WorkGraphCullingFrame& frame, org::PassRecordContext& context)
{
    const auto view = [&](const org::DeclaredViewToken& token) { return context.Index(token); };
    root[CLOD_WG_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = view(bindings.visible);
    root[CLOD_WG_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = view(bindings.transforms);
    root[CLOD_WG_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = view(bindings.visibleCounter);
    root[CLOD_WG_SW_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = view(bindings.swCounter);
    root[CLOD_WG_TELEMETRY_DESCRIPTOR_INDEX] = view(bindings.telemetry);
    root[CLOD_WG_FORCED_TRAVERSAL_DEPTH_ROOT] = frame.settings.forcedTraversalDepth;
    if (bindings.swClassification) root[CLOD_WG_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX] = frame.viewRasterInfoTable;
    root[CLOD_WG_FLAGS] = frame.cullingFlags;

    if (const auto& shadow = bindings.virtualShadow) {
        root[CLOD_WG_VIRTUAL_SHADOW_PAGE_TABLE_UAV_DESCRIPTOR_INDEX] = view(shadow->pageTable);
        root[CLOD_WG_VIRTUAL_SHADOW_ACTIVE_BLOCK_METADATA_DESCRIPTOR_INDEX] = view(shadow->activeMetadata);
        if (shadow->pageRaster) {
            root[CLOD_WG_VIRTUAL_SHADOW_PHYSICAL_PAGES_UAV_DESCRIPTOR_INDEX] = view(shadow->pageRaster->staticPages);
            root[CLOD_WG_VIRTUAL_SHADOW_DYNAMIC_PAGES_UAV_DESCRIPTOR_INDEX] = view(shadow->pageRaster->dynamicPages);
            root[CLOD_WG_VIRTUAL_SHADOW_DYNAMIC_ACTIVE_BLOCK_METADATA_DESCRIPTOR_INDEX] =
                view(shadow->pageRaster->dynamicMetadata);
        }
        if (shadow->dirtyHierarchy)
            root[CLOD_WG_SHADOW_DIRTY_HIERARCHY_DESCRIPTOR_INDEX] = view(*shadow->dirtyHierarchy);
        if (shadow->invalidatedInstances)
            root[CLOD_WG_SHADOW_INVALIDATED_INSTANCES_DESCRIPTOR_INDEX] = view(*shadow->invalidatedInstances);
        if (shadow->predictiveInvalidation) {
            root[CLOD_WG_SHADOW_PREDICTIVE_INVALIDATION_CANDIDATES_DESCRIPTOR_INDEX] =
                view(shadow->predictiveInvalidation->candidates);
            root[CLOD_WG_SHADOW_PREDICTIVE_INVALIDATION_CANDIDATE_COUNT_DESCRIPTOR_INDEX] =
                view(shadow->predictiveInvalidation->count);
        }
    }
    if (const auto& reyes = bindings.reyes) {
        root[CLOD_WG_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] = view(reyes->diceQueue);
        root[CLOD_WG_REYES_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] = view(reyes->diceQueueCounter);
        root[CLOD_WG_REYES_DICE_QUEUE_OVERFLOW_DESCRIPTOR_INDEX] = view(reyes->diceQueueOverflow);
        root[CLOD_WG_REYES_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = view(reyes->tessTableConfigs);
        root[CLOD_WG_REYES_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX] = view(reyes->tessTableVertices);
        root[CLOD_WG_REYES_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX] = view(reyes->tessTableTriangles);
        root[CLOD_WG_REYES_TELEMETRY_DESCRIPTOR_INDEX] = view(reyes->telemetry);
        root[CLOD_WG_REYES_DICE_QUEUE_CAPACITY] = bindings.reyesDiceQueueCapacity;
    }
    root[CLOD_WG_OCCLUSION_REPLAY_BUFFER_DESCRIPTOR_INDEX] = view(bindings.replay);
    root[CLOD_WG_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX] = view(bindings.replayState);
    root[CLOD_WG_WORKGRAPH_NODE_INPUTS_DESCRIPTOR_INDEX] = view(bindings.nodeInputs);
    root[CLOD_WG_VIEW_DEPTH_SRV_INDICES_DESCRIPTOR_INDEX] = bindings.perViewDepthOcclusion ? frame.viewDepthTable : 0u;
    root[CLOD_WG_VISIBLE_CLUSTERS_CAPACITY] = bindings.maxVisibleClusters;
    // Phase 2 appends after phase 1's clusters.
    root[CLOD_WG_HW_WRITE_BASE_COUNTER_DESCRIPTOR_INDEX] = view(bindings.phase1Counter.value_or(bindings.visibleCounter));
    root[CLOD_WG_SW_WRITE_BASE_COUNTER_DESCRIPTOR_INDEX] = view(bindings.phase1SwCounter.value_or(bindings.swCounter));
}

}

WorkGraphCullingFrame WorkGraphCullingPass::Prepare(
    const WorkGraphCullingBindings& bindings, const org::PassPrepareContext& preparation) const
{
    const auto* update = preparation.preparationData
        ? preparation.preparationData->Get<UpdateContext>() : nullptr;
    const auto* render = preparation.preparationData
        ? preparation.preparationData->Get<RenderContext>() : nullptr;
    if (!update || !render || !m_workGraph || !m_scratchBuffer) return {};

    if (bindings.voxelQueues) {
        const auto view = [&](org::DeclaredViewToken token) { return preparation.Resolve(token).index; };
        const auto& queues = *bindings.voxelQueues;
        CLodVoxelRasterQueueDescriptors descriptors{};
        descriptors.rigidWorkRecordsUAVDescriptorIndex = view(queues[0]);
        descriptors.rigidWorkRecordCounterUAVDescriptorIndex = view(queues[1]);
        descriptors.skinnedWorkRecordsUAVDescriptorIndex = view(queues[2]);
        descriptors.skinnedWorkRecordCounterUAVDescriptorIndex = view(queues[3]);
        descriptors.workRecordCapacity = m_resources.voxelQueues.capacity;
        m_voxelQueueTable.Upload(descriptors, Uploader());
    }

    WorkGraphCullingFrame frame;
    frame.active = true;
    frame.workGraph = m_workGraph;
    frame.workGraphSlots = m_workGraphSlots;
    // The work graph never implemented the frustum-culling toggle or the
    // receiver subpage mask, so both stay at their defaults.
    frame.settings = br::render::ReadCullingSettings(m_config, false);
    frame.settings.frustumCulling = true;
    frame.cullingFlags = br::render::BuildCullingFlags(m_config, frame.settings);
    if (m_config.UsesSWClassification())
        frame.viewRasterInfoTable =
            bindings.viewRasterInfoLayout.Publish(preparation, m_viewRasterInfoTable, m_viewRasterInfoLayout.Rows());
    if (m_config.UsesPerViewDepthOcclusion())
        frame.viewDepthTable = bindings.viewDepthLayout.Publish(preparation, m_viewDepthTable, m_viewDepthLayout.Rows());
    frame.rasterBucketCount = render->preparedRasterBucketCount;

    frame.initializeBacking = !m_workGraphInitialization->initialized.load(std::memory_order_acquire);
    if (frame.initializeBacking) preparation.Reserve(m_workGraphInitialization,
        +[](WorkGraphInitializationState& state, org::SubmissionContext) {
            state.initialized.store(true, std::memory_order_release);
        });
    if (m_config.isFirstPass) {
        const auto workloads = br::render::PrepareCullingWorkloads(
            render->Views(), render->publishedRendererState, m_config.renderPhase,
            m_config.clodOnlyWorkloads, m_config.useShadowCascadeViews, m_config.rasterOutputKind, 64u,
            "WorkGraphCullingPass");
        std::vector<ObjectCullRecord> records;
        records.reserve(workloads.size());
        for (const auto& workload : workloads) records.push_back({
            workload.viewDataIndex, workload.activeDrawSetIndicesSRVIndex,
            workload.activeDrawCount, workload.drawRecordVisibilityGenerationSRVIndex,
            workload.drawRecordVisibilityGenerationCount, workload.dispatchGridX, workload.dispatchGridY, workload.dispatchGridZ});
        basic_telemetry::SetGauge("SARP.Culling.GraphWorkloadRecords.WorkGraph",
            static_cast<std::int64_t>(records.size()));
        frame.cpuRecordStride = static_cast<uint32_t>(sizeof(ObjectCullRecord));
        frame.cpuRecords.resize(records.size() * sizeof(ObjectCullRecord));
        if (!records.empty()) std::memcpy(frame.cpuRecords.data(), records.data(), frame.cpuRecords.size());
    }
    return frame;
}

void WorkGraphCullingPass::Record(const WorkGraphCullingBindings& bindings, const WorkGraphCullingFrame& frame,
    org::PassRecordContext& context)
{
    if (!frame.active) return;
    auto& commands = context.Commands();
    // The graph runs against the global compute root signature.
    context.Retain(frame.workGraphSlots);
    commands.BindLayout(frame.workGraphSlots->layout);

    using OutputBarrier = std::pair<org::ResourceBindingToken, rhi::ResourceAccessType>;
    const auto barriers = [&](std::span<const OutputBarrier> list) {
        std::vector<rhi::BufferBarrier> batch;
        batch.reserve(list.size());
        for (const auto& [token, after] : list) {
            rhi::BufferBarrier barrier{};
            barrier.buffer = context.Resolve(token).GetHandle();
            barrier.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
            barrier.afterAccess = after;
            barrier.beforeSync = barrier.afterSync = rhi::ResourceSyncState::ComputeShading;
            batch.push_back(barrier);
        }
        if (batch.empty()) return;
        rhi::BarrierBatch barrierBatch{};
        barrierBatch.buffers = rhi::Span<rhi::BufferBarrier>(batch.data(), static_cast<uint32_t>(batch.size()));
        commands.Barriers(barrierBatch);
    };
    constexpr auto uav = rhi::ResourceAccessType::UnorderedAccess;
    constexpr auto read = rhi::ResourceAccessType::ShaderResource;
    const auto clear = [&](const org::DeclaredViewToken& token, uint32_t count = 1u) {
        br::render::MiscConstants values{};
        values[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = context.Index(token);
        values[CLOD_CLEAR_UINT_BUFFER_COUNT] = count;
        br::render::DispatchCompute(context, bindings.clearPipeline, values, (count + 63u) / 64u);
        barriers(std::array{OutputBarrier{token.Resource(), uav}});
    };

    if (bindings.isFirstPass && frame.settings.telemetry)
        clear(bindings.telemetry, CLodWorkGraphTelemetryBufferCount);
    if (bindings.voxelQueues) {
        clear((*bindings.voxelQueues)[1]);
        clear((*bindings.voxelQueues)[3]);
    }
    if (bindings.reyes && bindings.isFirstPass) {
        clear(bindings.reyes->diceQueueCounter);
        clear(bindings.reyes->diceQueueOverflow);
    }

    br::render::MiscConstants root{};
    FillGraphConstants(root, bindings, frame, context);

    context.SetWorkGraph(frame.workGraph, bindings.scratch, frame.initializeBacking);
    const auto slots = context.DescriptorSlotIndices(*frame.workGraphSlots);
    if (!slots.empty()) commands.PushConstants(rhi::ShaderStage::Compute, 0,
        org::shaderapi::kResourceDescriptorIndicesRootParameter, 0,
        static_cast<uint32_t>(slots.size()), slots.data());
    commands.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0,
        NumMiscUintRootConstants, root.data());

    if (bindings.isFirstPass) {
        if (!frame.cpuRecords.empty()) {
            rhi::WorkGraphDispatchDesc dispatch{};
            dispatch.dispatchMode = rhi::WorkGraphDispatchMode::NodeCpuInput;
            dispatch.nodeCpuInput.entryPointIndex = 0u;
            dispatch.nodeCpuInput.pRecords = frame.cpuRecords.data();
            dispatch.nodeCpuInput.numRecords = static_cast<uint32_t>(frame.cpuRecords.size() / frame.cpuRecordStride);
            dispatch.nodeCpuInput.recordByteStride = frame.cpuRecordStride;
            commands.DispatchWorkGraph(dispatch);
        }
        barriers(std::array{OutputBarrier{bindings.visibleCounter.Resource(), uav},
            OutputBarrier{bindings.replayState.Resource(), uav}, OutputBarrier{bindings.replay.Resource(), uav}});
    } else {
        barriers(std::array{OutputBarrier{bindings.replay.Resource(), uav},
            OutputBarrier{bindings.nodeInputs.Resource(), uav}});
        rhi::WorkGraphDispatchDesc dispatch{};
        dispatch.dispatchMode = rhi::WorkGraphDispatchMode::MultiNodeGpuInput;
        dispatch.multiNodeGpuInput.inputBuffer = context.Resolve(bindings.nodeInputs).GetHandle();
        dispatch.multiNodeGpuInput.inputAddressOffset = 0;
        commands.DispatchWorkGraph(dispatch);
    }

    std::vector<OutputBarrier> outputs{
        {bindings.visibleCounter.Resource(), read}, {bindings.replayState.Resource(), read}};
    if (bindings.swClassification) outputs.push_back({bindings.swCounter.Resource(), uav});
    if (bindings.voxelQueues) {
        const auto& voxels = *bindings.voxelQueues;
        for (const size_t queue : {1u, 3u, 0u, 2u}) outputs.push_back({voxels[queue].Resource(), read});
    }
    barriers(outputs);

    // Build the raster bucket histogram command, and patch phase 2's GPU node
    // inputs with this frame's replay record counts and addresses. The command
    // shader also reads the graph constants already in root.
    context.BindComputePipeline(bindings.createCommandPipeline);
    root[CLOD_CREATE_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = context.Index(bindings.visibleCounterSrv);
    root[CLOD_CREATE_RASTER_BUCKET_HISTOGRAM_COMMAND_DESCRIPTOR_INDEX] = context.Index(bindings.histogram);
    root[CLOD_CREATE_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX] = context.Index(bindings.replayStateSrv);
    root[CLOD_CREATE_WORKGRAPH_NODE_INPUTS_DESCRIPTOR_INDEX] = context.Index(bindings.nodeInputs);
    root[CLOD_CREATE_NUM_RASTER_BUCKETS] = frame.rasterBucketCount;
    root[CLOD_CREATE_VISIBLE_CLUSTERS_CAPACITY] = bindings.maxVisibleClusters;
    root[CLOD_CREATE_NODE_INPUT_COUNT] = bindings.reyes ? 4u : 2u;
    const auto replayAddress = GetNativeBufferDeviceAddress(context.Resolve(bindings.replay));
    const auto inputAddress = GetNativeBufferDeviceAddress(context.Resolve(bindings.nodeInputs));
    root[CLOD_CREATE_REPLAY_ADDRESS_LOW] = static_cast<uint32_t>(replayAddress);
    root[CLOD_CREATE_REPLAY_ADDRESS_HIGH] = static_cast<uint32_t>(replayAddress >> 32u);
    root[CLOD_CREATE_NODE_INPUT_ADDRESS_LOW] = static_cast<uint32_t>(inputAddress);
    root[CLOD_CREATE_NODE_INPUT_ADDRESS_HIGH] = static_cast<uint32_t>(inputAddress >> 32u);
    br::render::PushMiscConstants(context, root);
    commands.Dispatch(1, 1, 1);
}

void WorkGraphCullingPass::Update(const org::UpdateExecutionContext& executionContext)
{
    ZoneScopedN("WorkGraphCullingPass::Update");

    auto* updateContext = executionContext.hostData ? executionContext.hostData->Get<UpdateContext>() : nullptr;
    if (!updateContext) {
        return;
    }
    auto& context = *updateContext;
    const auto upload = Uploader();

    m_declaredResourcesChanged = false;
    if (m_config.UsesSWClassification())
        m_declaredResourcesChanged |= m_viewRasterInfoLayout.Update(
            context.Views(), context.ViewCameraBufferSize(), m_config.rasterOutputKind);
    if (m_config.UsesPerViewDepthOcclusion())
        m_declaredResourcesChanged |= m_viewDepthLayout.Update(context.Views(), m_config.isFirstPass);
    m_declaredResourcesChanged |= m_drawSets.Update(context.publishedRendererState, m_config);

    br::render::ResetCullingCounter(m_resources.outputs.visibleClustersCounter, upload);
    if (m_config.UsesSWClassification()) {
        br::render::ResetCullingCounter(m_resources.outputs.swVisibleClustersCounter, upload);
    }

    // Phase 2 continues from phase 1's replay state and telemetry.
    if (!m_config.isFirstPass) {
        return;
    }
    br::render::ResetOcclusionReplayState(m_resources.occlusionReplay.state, upload);
    if (IsCLodWorkGraphTelemetryEnabled()) {
        br::render::ResetCullingTelemetry(m_resources.telemetry, m_zeroTelemetryScratch, upload);
    }
    if (m_resources.reyes.enabled && m_resources.reyes.telemetry) {
        CLodReyesTelemetry telemetry{};
        telemetry.phaseIndex = 1u;
        telemetry.configuredMaxSplitPassCount = CLodReyesMaxSplitPassCount;
        upload(&telemetry, sizeof(telemetry), m_resources.reyes.telemetry);
    }
}

std::shared_ptr<org::Resource> WorkGraphCullingPass::ProvideResource(org::ResourceIdentifier const& key)
{
    if (m_voxelQueueTable.Enabled() && key == m_voxelQueueTable.Id()) return m_voxelQueueTable.Buffer();
    return nullptr;
}

std::vector<org::ResourceIdentifier> WorkGraphCullingPass::GetSupportedKeys()
{
    if (!m_voxelQueueTable.Enabled()) return {};
    return {org::ResourceIdentifier{m_voxelQueueTable.Id()}};
}
