#include "VirtualGeometry/Culling/RenderPasses/ComputeCullingPass.h"
#include "Render/PassBuilders.h"

#include <algorithm>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>
#include <BasicTelemetry/Telemetry.h>

#include "BasicRenderer/Diagnostics/CLodTelemetry.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BuiltinResources.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Render/Runtime/UploadTypes.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "Runtime/Device/DeviceManager.h"
#include "Runtime/GraphIntegration/Resolvers/ResourceGroupResolver.h"
#include "Runtime/Settings/SettingsManager.h"
#include "VirtualGeometry/Streaming/Publication/GeometryResidencyStateArtifacts.h"
#include "../shaders/PerPassRootConstants/clodPureComputeCullingRootConstants.h"
#include "../shaders/PerPassRootConstants/clodWorkGraphRootConstants.h"

namespace {

struct DispatchArguments {
    uint32_t x, y, z;
};

std::shared_ptr<org::Buffer> CreateWorkingBuffer(uint32_t elementCount, uint32_t elementSize, bool aliasable,
    const char* name, const char* usage)
{
    auto buffer = CreateAliasedUnmaterializedStructuredBuffer(elementCount, elementSize, true, false, false, aliasable);
    buffer->SetName(name);
    org::memory::SetResourceUsageHint(*buffer, usage);
    return buffer;
}

}

ComputeCullingPass::ComputeCullingPass(CullingPassConfig config, CullingPassResources resources)
    : m_config(std::move(config))
    , m_resources(std::move(resources))
    , m_voxelQueueTable(m_config.stablePassIdentifier, "CLod Pure Compute Voxel Raster Queue Descriptors",
        m_resources.voxelQueues.Enabled())
{
    CreateWindCache();
    CreateFrontiers();
    CreatePrograms();
}

ComputeCullingPass::~ComputeCullingPass() = default;

void ComputeCullingPass::CreateWindCache()
{
    auto& settings = SettingsManager::GetInstance();
    if (!m_config.isFirstPass || !m_config.UsesVirtualShadowOutput()
        || !settings.getSettingGetter<bool>(CLodDynamicWindBoundsCacheEnabledSettingName)()) {
        return;
    }
    constexpr uint64_t entryStride = 48u;
    const uint64_t budgetMiB = std::min<uint32_t>(
        settings.getSettingGetter<uint32_t>(CLodDynamicWindBoundsCacheMiBSettingName)(), 256u);
    m_windCache.entryCount = static_cast<uint32_t>(budgetMiB * 1024u * 1024u / entryStride);
    if (m_windCache.entryCount == 0u) return;
    m_windCache.bounds = CreateAliasedUnmaterializedRawBuffer(
        static_cast<uint64_t>(m_windCache.entryCount) * entryStride, true, false, true);
    m_windCache.bounds->SetName("CLod DynamicWind Meshlet Bounds Cache");
    org::memory::SetResourceUsageHint(*m_windCache.bounds, "DynamicWind VSM bounds cache");
}

void ComputeCullingPass::CreateFrontiers()
{
    const uint32_t capacity = std::max(1u, m_config.maxVisibleClusters);
    spdlog::info(
        "CLod pure compute frontier allocation '{}': capacity={} nodeFrontier={} MiB each clusterFrontier={} MiB",
        m_config.stablePassIdentifier,
        capacity,
        static_cast<double>(uint64_t{capacity} * CLodPureComputeNodeFrontierStrideBytes) / (1024.0 * 1024.0),
        static_cast<double>(uint64_t{capacity} * CLodPureComputeClusterFrontierStrideBytes) / (1024.0 * 1024.0));

    constexpr const char* frontierUsage = "Cluster LOD pure compute frontiers";
    constexpr const char* usage = "Cluster LOD pure compute";
    const auto frontier = [&](uint32_t stride, const char* name) {
        return CreateWorkingBuffer(capacity, stride, true, name, frontierUsage);
    };
    const auto scalar = [&](uint32_t size, const char* name) {
        return CreateWorkingBuffer(1u, size, false, name, usage);
    };
    auto& f = m_frontiers;
    f.currentNodes = frontier(CLodPureComputeNodeFrontierStrideBytes, "CLod Pure Compute Current Node Frontier");
    f.nextNodes = frontier(CLodPureComputeNodeFrontierStrideBytes, "CLod Pure Compute Next Node Frontier");
    f.currentLeaves = frontier(CLodPureComputeNodeFrontierStrideBytes, "CLod Pure Compute Current Leaf Frontier");
    f.nextLeaves = frontier(CLodPureComputeNodeFrontierStrideBytes, "CLod Pure Compute Next Leaf Frontier");
    f.clusters = frontier(CLodPureComputeClusterFrontierStrideBytes, "CLod Pure Compute Cluster Frontier");
    f.currentNodeCounter = scalar(sizeof(uint32_t), "CLod Pure Compute Current Node Counter");
    f.nextNodeCounter = scalar(sizeof(uint32_t), "CLod Pure Compute Next Node Counter");
    f.currentLeafCounter = scalar(sizeof(uint32_t), "CLod Pure Compute Current Leaf Counter");
    f.nextLeafCounter = scalar(sizeof(uint32_t), "CLod Pure Compute Next Leaf Counter");
    f.clusterCounter = scalar(sizeof(uint32_t), "CLod Pure Compute Cluster Counter");
    f.nodeDispatchArgs = scalar(sizeof(DispatchArguments), "CLod Pure Compute Node Dispatch Args");
    f.leafDispatchArgs = scalar(sizeof(DispatchArguments), "CLod Pure Compute Leaf Dispatch Args");
    f.clusterDispatchArgs = scalar(sizeof(DispatchArguments), "CLod Pure Compute Cluster Dispatch Args");
}

void ComputeCullingPass::CreatePrograms()
{
    const std::wstring voxelTableDefine = m_voxelQueueTable.ShaderDefine();
    std::vector<DxcDefine> defines = {
        { L"CLOD_WG_ENABLE_SW_CLASSIFICATION", m_config.UsesSWClassification() ? L"1" : L"0" },
        { L"CLOD_WG_ENABLE_SW_NODE_OUTPUT", L"0" },
        { L"CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW", m_config.UsesVirtualShadowOutput() ? L"1" : L"0" },
        { L"CLOD_VSM_TWO_LAYER_COMPUTE_CULL_VERSION", m_config.UsesVirtualShadowOutput() ? L"2" : L"0" },
        { L"CLOD_WG_ENABLE_VOXEL_OUTPUT", m_voxelQueueTable.Enabled() ? L"1" : L"0" },
        { L"CLOD_COMPUTE_INCLUDE_ONLY", L"1" },
        { L"CLOD_WG_VOXEL_RASTER_QUEUE_DESCRIPTOR_BUFFER_ID", voxelTableDefine.c_str() },
    };
    std::vector<DxcDefine> leafDefines = defines;
    leafDefines.push_back({ L"CLOD_PC_LEAF_ONLY", L"1" });

    auto& psoManager = PSOManager::GetInstance();
    const auto layout = psoManager.GetComputeRootSignature().GetHandle();
    const auto culling = [&](const wchar_t* entry, const std::vector<DxcDefine>& programDefines, const char* name) {
        return psoManager.MakeComputePipeline(layout, L"shaders/ClusterLOD/computeCulling.hlsl", entry,
            programDefines, name);
    };
    auto& p = m_programs;
    p.clearCounter = psoManager.MakeComputePipeline(layout, L"shaders/ClusterLOD/clodUtil.hlsl",
        L"ClearUintStructuredBufferCSMain", {}, "HierarchicalDispatchCulling.ClearUint");
    p.createRasterBucketCommand = psoManager.MakeComputePipeline(layout, L"shaders/ClusterLOD/clodUtil.hlsl",
        L"CreateRasterBucketsHistogramCommandCSMain", {}, "HierarchicalDispatchCulling.CreateRasterBucketsHistogramCommand");
    p.buildDispatchArgs = culling(L"BuildPureComputeDispatchArgsCS", defines, "CLod.PureCompute.BuildDispatchArgs");
    p.buildDualDispatchArgs = culling(L"BuildPureComputeDualDispatchArgsCS", defines, "CLod.PureCompute.BuildDualDispatchArgs");
    p.clearTraversalCounters = culling(L"ClearPureComputeTraversalCountersCS", defines, "CLod.PureCompute.ClearTraversalCounters");
    p.buildReplayDispatchArgs = culling(L"BuildPureComputeReplayDispatchArgsCS", defines, "CLod.PureCompute.BuildReplayDispatchArgs");
    p.objectCull = culling(L"PureComputeObjectCullCS", defines, "CLod.PureCompute.ObjectCull");
    p.seedReplayNodes = culling(L"SeedPureComputeReplayNodesCS", defines, "CLod.PureCompute.SeedReplayNodes");
    p.seedReplayClusters = culling(L"SeedPureComputeReplayClustersCS", defines, "CLod.PureCompute.SeedReplayClusters");
    p.traverseNodes = culling(L"PureComputeTraverseFrontierCS", defines, "CLod.PureCompute.Traverse");
    p.traverseLeaves = culling(L"PureComputeTraverseFrontierCS", leafDefines, "CLod.PureCompute.Leaf");
    p.cullClusters = culling(L"PureComputeDenseClusterWorkCS", defines, "CLod.PureCompute.DenseClusterCull");

    // Traversal emits voxel work; nothing else may read the voxel table.
    const bool voxels = m_voxelQueueTable.Enabled();
    m_voxelQueueTable.ValidateInterface(p.objectCull.GetResourceDescriptorSlots(), false, "object-cull");
    m_voxelQueueTable.ValidateInterface(p.traverseNodes.GetResourceDescriptorSlots(), voxels, "traverse");
    m_voxelQueueTable.ValidateInterface(p.traverseLeaves.GetResourceDescriptorSlots(), voxels, "leaf");
    m_voxelQueueTable.ValidateInterface(p.cullClusters.GetResourceDescriptorSlots(), false, "dense-cluster");

    rhi::IndirectArg dispatchArg[] = {
        {.kind = rhi::IndirectArgKind::Dispatch }
    };
    rhi::CommandSignaturePtr signature;
    DeviceManager::GetInstance().GetDevice().CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(dispatchArg, 1), sizeof(DispatchArguments) },
        layout,
        signature);
    p.dispatchSignature = std::make_shared<rhi::CommandSignaturePtr>(std::move(signature));
}

br::render::CullingUploader ComputeCullingPass::Uploader() const
{
    return [this](const void* data, size_t size, const std::shared_ptr<org::Buffer>& target) {
        UploadBufferData(data, size, org::runtime::UploadTarget::FromShared(target), 0);
    };
}

void ComputeCullingPass::DeclareVirtualShadow(org::PassBuilder& builder) const
{
    const auto& shadow = m_resources.virtualShadow;
    builder.ShaderResource(
        Builtin::Shadows::CLodClipmapInfo,
        Builtin::Shadows::CLodDirectionalPageViewInfo,
        Builtin::Shadows::CLodCompactShadowCameras);
    if (shadow.dirtyPageHierarchy) builder.ShaderResource(shadow.dirtyPageHierarchy);
    if (shadow.invalidatedInstancesBitset) builder.ShaderResource(shadow.invalidatedInstancesBitset);
    if (shadow.invalidationCount) builder.ShaderResource(shadow.invalidationCount);
    if (shadow.predictiveInvalidationCandidates) builder.UnorderedAccess(shadow.predictiveInvalidationCandidates);
    if (shadow.predictiveInvalidationCandidateCount) builder.UnorderedAccess(shadow.predictiveInvalidationCandidateCount);
    if (shadow.pageTable) builder.UnorderedAccess(shadow.pageTable);
    if (shadow.staticPhysicalPages) builder.UnorderedAccess(shadow.staticPhysicalPages);
    if (shadow.dynamicPhysicalPages) builder.UnorderedAccess(shadow.dynamicPhysicalPages);
    if (shadow.activeBlockMetadata) builder.ShaderResource(shadow.activeBlockMetadata);
    if (shadow.receiverSubpageMask) builder.ShaderResource(shadow.receiverSubpageMask);
    if (shadow.dynamicActiveBlockMetadata) builder.ShaderResource(shadow.dynamicActiveBlockMetadata);
}

ComputeCullingBindings ComputeCullingPass::Declare(org::PassBuilder& builder)
{
    const org::ResourceState computeReadState{
        rhi::ResourceAccessType::ShaderResource,
        rhi::ResourceLayout::ShaderResource,
        rhi::ResourceSyncState::ComputeShading
    };
    const org::ResourceState indirectState{
        rhi::ResourceAccessType::IndirectArgument,
        rhi::ResourceLayout::GenericRead,
        rhi::ResourceSyncState::ExecuteIndirect
    };
    const auto& outputs = m_resources.outputs;
    const auto& replay = m_resources.occlusionReplay;
    const auto& f = m_frontiers;

    ComputeCullingBindings bindings{};
    builder.UnorderedAccess(
        outputs.visibleClusters,
        outputs.visibleClusterTransformIndices,
        outputs.visibleClustersCounter,
        outputs.rasterBucketHistogramCommand,
        m_resources.telemetry,
        replay.replay,
        replay.state,
        f.currentNodes, f.nextNodes, f.currentLeaves, f.nextLeaves, f.clusters,
        f.currentNodeCounter, f.nextNodeCounter, f.currentLeafCounter, f.nextLeafCounter, f.clusterCounter,
        f.nodeDispatchArgs, f.leafDispatchArgs, f.clusterDispatchArgs,
        Builtin::CLod::StreamingLoadRequestKeys,
        Builtin::CLod::StreamingLoadRequests,
        Builtin::CLod::StreamingLoadCounter,
        Builtin::CLod::StreamingTouchedGroupsCounter,
        Builtin::CLod::StreamingTouchedGroups);
    builder.ShaderResource(
        Builtin::IndirectCommandBuffers::Master,
        Builtin::CLod::Offsets,
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
        Builtin::CLod::StreamingRuntimeState,
        Builtin::CullingCameraBuffer,
        Builtin::PerMeshInstanceBuffer,
        Builtin::InstanceDrawRecordBuffer,
        Builtin::SkinnedAssemblyPlacements,
        Builtin::PerInstanceTransformBuffer,
        Builtin::PerObjectBuffer,
        Builtin::CameraBuffer,
        Builtin::PerMeshBuffer,
        Builtin::PerMaterialDataBuffer,
        Builtin::Material::TextureStreamingMetadataBuffer,
        Builtin::SkeletonResources::InverseBindMatrices,
        Builtin::SkeletonResources::BoneTransforms,
        Builtin::SkeletonResources::SkinningInstanceInfo);
    builder.UnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer);
    br::render::DeclareCullingWorkloadInputs(builder, m_config);
    builder.InternalTransition(outputs.visibleClustersCounter, computeReadState);
    builder.InternalTransition(replay.state, computeReadState);
    builder.InternalTransition(f.currentNodes, computeReadState);
    builder.InternalTransition(f.currentNodeCounter, computeReadState);
    builder.InternalTransition(f.currentLeaves, computeReadState);
    builder.InternalTransition(f.currentLeafCounter, computeReadState);

    if (m_voxelQueueTable.Enabled()) {
        const auto& voxels = m_resources.voxelQueues;
        bindings.voxelQueues = std::array{
            builder.UnorderedAccess(voxels.rigidWork).View(),
            builder.UnorderedAccess(voxels.rigidWorkCounter).View(),
            builder.UnorderedAccess(voxels.skinnedWork).View(),
            builder.UnorderedAccess(voxels.skinnedWorkCounter).View()};
        builder.ShaderResource(m_voxelQueueTable.Id().c_str());
    }

    // Where the frame leaves the traversal buffers depends on how many levels ran.
    const uint32_t traversalLevels = std::min(m_traversalDepth, kComputeCullingMaxTraversalLevels);
    if (!m_config.isFirstPass || traversalLevels > 0u) {
        builder.InternalTransition(f.nodeDispatchArgs, indirectState)
            .InternalTransition(f.leafDispatchArgs, indirectState)
            .InternalTransition(f.clusterDispatchArgs, indirectState);
    }
    if (traversalLevels > 0u) {
        builder.InternalTransition(f.nextNodes, computeReadState)
            .InternalTransition(f.nextNodeCounter, computeReadState)
            .InternalTransition(f.nextLeaves, computeReadState)
            .InternalTransition(f.nextLeafCounter, computeReadState);
    }

    if (m_config.UsesSWClassification() && outputs.swVisibleClustersCounter) {
        builder.UnorderedAccess(outputs.swVisibleClustersCounter);
    }
    if (m_resources.pagePoolSlabs) {
        builder.ShaderResource(ResourceGroupResolver(m_resources.pagePoolSlabs));
    }
    if (m_windCache.bounds) {
        builder.UnorderedAccess(m_windCache.bounds);
        // This read both enforces SimulateInstancesPhase2 -> shadow traversal
        // ordering and limits caching to placements accepted by DynamicWind.
        m_windCache.visibleMembership = m_resourceRegistryView
            ->RequestPtr<org::GloballyIndexedResource>("Builtin::DynamicWind::VisibleSkeletonMembership");
        builder.ShaderResource("Builtin::DynamicWind::VisibleSkeletonMembership");
    }
    if (m_config.UsesVirtualShadowOutput()) {
        DeclareVirtualShadow(builder);
    }
    if (m_config.UsesPerViewDepthOcclusion()) {
        m_viewDepthLayout.Declare(builder);
        bindings.viewDepthLayout = m_viewDepthLayout.Layout();
    }
    m_viewRasterInfoLayout.Declare(builder);
    bindings.viewRasterInfoLayout = m_viewRasterInfoLayout.Layout();
    if (!m_config.isFirstPass) {
        if (m_resources.phase1Counters.visibleClustersCounter)
            builder.ShaderResource(m_resources.phase1Counters.visibleClustersCounter);
        if (m_resources.phase1Counters.swVisibleClustersCounter)
            builder.ShaderResource(m_resources.phase1Counters.swVisibleClustersCounter);
    }

    builder.ConstantBuffer(Builtin::PerFrameBuffer);
    return bindings;
}

ComputeCullingRecipe ComputeCullingPass::BuildRecipe(
    const ComputeCullingBindings& declared, const org::PassPrepareContext& preparation) const
{
    auto* renderContext = preparation.preparationData
        ? preparation.preparationData->Get<RenderContext>() : nullptr;
    if (!renderContext || !preparation.bindings) return {};

    if (declared.voxelQueues) {
        const auto& queues = *declared.voxelQueues;
        const auto view = [&](org::DeclaredViewToken token) { return preparation.Resolve(token).index; };
        CLodVoxelRasterQueueDescriptors descriptors{};
        descriptors.rigidWorkRecordsUAVDescriptorIndex = view(queues[0]);
        descriptors.rigidWorkRecordCounterUAVDescriptorIndex = view(queues[1]);
        descriptors.skinnedWorkRecordsUAVDescriptorIndex = view(queues[2]);
        descriptors.skinnedWorkRecordCounterUAVDescriptorIndex = view(queues[3]);
        descriptors.workRecordCapacity = m_resources.voxelQueues.capacity;
        m_voxelQueueTable.Upload(descriptors, Uploader());
    }

    auto& settings = SettingsManager::GetInstance();
    ComputeCullingCommandInputs inputs{
        .config = m_config,
        .resources = m_resources,
        .programs = m_programs,
        .frontiers = m_frontiers,
        .windCache = m_windCache,
        .settings = br::render::ReadCullingSettings(m_config, m_resources.virtualShadow.receiverSubpageMask != nullptr),
        .traversalDepth = m_traversalDepth,
        .expansionFactor = CLodNormalizePureComputePhase2ExpansionFactor(settings.getSettingGetter<uint32_t>(
            m_config.isFirstPass ? CLodPureComputePhase2ExpansionFactorSettingName
                                 : CLodPureComputeReplayExpansionFactorSettingName)()),
        .rasterBucketCount = renderContext->preparedRasterBucketCount,
    };
    inputs.viewRasterInfoTable =
        declared.viewRasterInfoLayout.Publish(preparation, m_viewRasterInfoTable, m_viewRasterInfoLayout.Rows());
    if (m_config.UsesPerViewDepthOcclusion())
        inputs.viewDepthTable =
            declared.viewDepthLayout.Publish(preparation, m_viewDepthTable, m_viewDepthLayout.Rows());
    return EmitComputeCullingCommands(inputs, preparation);
}

std::vector<uint64_t> ComputeCullingPass::RecipeRevision(const org::PassPrepareContext& preparation) const
{
    const auto* context = preparation.preparationData ? preparation.preparationData->Get<RenderContext>() : nullptr;
    std::vector<uint64_t> revision{SettingsManager::GetInstance().Revision(), m_traversalDepth,
        m_config.maxVisibleClusters, m_resources.voxelQueues.capacity,
        context ? context->preparedRasterBucketCount : 0u,
        reinterpret_cast<uintptr_t>(m_programs.dispatchSignature.get())};
    const auto layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
    revision.push_back((uint64_t{layout.generation} << 32) | layout.index);
    for (const auto* program : m_programs.All())
        revision.push_back(reinterpret_cast<uintptr_t>(program->PeekPayload()));
    if (preparation.preparationData && preparation.preparationData->Get<UpdateContext>()) {
        m_viewRasterInfoLayout.AppendRowRevision(revision);
    }
    return revision;
}

ComputeCullingInvocation ComputeCullingPass::PrepareInvocation(
    const ComputeCullingRecipe& recipe, const ComputeCullingBindings&,
    const org::PassPrepareContext& preparation) const
{
    const auto* context = preparation.preparationData ? preparation.preparationData->Get<RenderContext>() : nullptr;
    ComputeCullingInvocation invocation;
    invocation.publication = context ? context->publishedRendererState : nullptr;
    invocation.constants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_GENERATION] = m_windCache.generation;
    invocation.constants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_ENTRY_COUNT] = m_windCache.entryCount;
    if (recipe.hasObjectCull) {
        const auto views = context ? std::span<const PreparedViewFrameData>{context->Views()}
                                   : std::span<const PreparedViewFrameData>{};
        invocation.workloads = br::render::PrepareCullingWorkloads(views, invocation.publication,
            m_config.renderPhase, m_config.clodOnlyWorkloads, m_config.useShadowCascadeViews,
            m_config.rasterOutputKind, kComputeCullingObjectThreadsPerGroup, "ComputeCullingPass");
        basic_telemetry::SetGauge("SARP.Culling.GraphWorkloadRecords.Dispatch",
            static_cast<std::int64_t>(invocation.workloads.size()));
    }
    return invocation;
}

void ComputeCullingPass::Record(const ComputeCullingRecipe& recipe,
    const ComputeCullingInvocation& invocation, org::PassRecordContext& recording)
{
    br::render::RecordPreparedComputeCommands(recipe.prefix, recording, nullptr, {}, invocation.constants);
    auto constants = recipe.objectCullConstants;
    constants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_GENERATION] =
        invocation.constants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_GENERATION];
    constants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_ENTRY_COUNT] =
        invocation.constants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_ENTRY_COUNT];
    for (const auto& workload : invocation.workloads) {
        constants[CLOD_PC_OBJECT_CULL_ACTIVE_DRAW_COUNT] = workload.activeDrawCount;
        constants[CLOD_PC_OBJECT_CULL_VIEW_DATA_INDEX] = workload.viewDataIndex;
        constants[CLOD_PC_OBJECT_CULL_ACTIVE_DRAW_SET_SRV_INDEX] = workload.activeDrawSetIndicesSRVIndex;
        constants[CLOD_PC_OBJECT_CULL_VISIBILITY_GENERATION_SRV_INDEX] = workload.drawRecordVisibilityGenerationSRVIndex;
        constants[CLOD_PC_OBJECT_CULL_VISIBILITY_GENERATION_COUNT] = workload.drawRecordVisibilityGenerationCount;
        constants[CLOD_PC_OBJECT_CULL_SHADOW_CASTER_CLASS] = workload.shadowCasterClass;
        recording.Commands().PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0,
            NumMiscUintRootConstants, constants.data());
        recording.Commands().Dispatch(workload.dispatchGridX, workload.dispatchGridY, workload.dispatchGridZ);
    }
    br::render::RecordPreparedComputeCommands(recipe.suffix, recording, nullptr, {}, invocation.constants);
}

void ComputeCullingPass::UpdateTraversalDepth(const UpdateContext& context)
{
    ZoneScopedN("ComputeCullingPass::UpdateTraversalDepth");
    const auto residency = context.publishedRendererState
        ? context.publishedRendererState->geometryResidency.payload.Get<br::render::PublishedGeometryResidencyState>()
        : nullptr;
    if (!residency) basic_telemetry::AddCounter("BasicRenderer.GeometryResidency.MissingAcceptedPublication");
    const uint32_t depth = residency ? residency->maxTraversalDepth : 0u;
    basic_telemetry::SetGauge("BasicRenderer.GeometryResidency.PublishedDepth", static_cast<std::int64_t>(depth));
    basic_telemetry::SetGauge("BasicRenderer.GeometryResidency.SelectedDepth", static_cast<std::int64_t>(depth));
    // The traversal depth decides how Declare leaves the frontier buffers.
    if (depth != m_traversalDepth) m_declaredResourcesChanged = true;
    m_traversalDepth = depth;
}

void ComputeCullingPass::Update(const org::UpdateExecutionContext& executionContext)
{
    ZoneScopedN("ComputeCullingPass::Update");

    auto* updateContext = executionContext.hostData ? executionContext.hostData->Get<UpdateContext>() : nullptr;
    if (!updateContext) {
        return;
    }
    auto& context = *updateContext;
    const auto upload = Uploader();
    // This is a logical-frame cache tag, not a recording side effect. Advance
    // it on the preparation owner so delayed/concurrent recording consumes the
    // generation captured for that frame and never mutates pass state.
    if (m_windCache.bounds) {
        ++m_windCache.generation;
        if (m_windCache.generation == 0u || m_windCache.generation >= 0x7FFFFFFFu) {
            m_windCache.generation = 1u;
        }
    }

    m_declaredResourcesChanged = m_viewRasterInfoLayout.Update(
        context.Views(), context.ViewCameraBufferSize(), m_config.rasterOutputKind, false);
    if (m_config.UsesPerViewDepthOcclusion())
        m_declaredResourcesChanged |= m_viewDepthLayout.Update(context.Views(), m_config.isFirstPass);
    m_declaredResourcesChanged |= m_drawSets.Update(context.publishedRendererState, m_config);
    UpdateTraversalDepth(context);

    br::render::ResetCullingCounter(m_resources.outputs.visibleClustersCounter, upload);
    if (m_resources.outputs.swVisibleClustersCounter) {
        br::render::ResetCullingCounter(m_resources.outputs.swVisibleClustersCounter, upload);
    }

    auto& f = m_frontiers;
    const uint32_t frontierCapacity = std::max(1u, m_config.maxVisibleClusters);
    if (frontierCapacity != m_sizedFrontierCapacity) {
        ZoneScopedN("ComputeCullingPass::ResizeFrontiers");
        m_sizedFrontierCapacity = frontierCapacity;
        for (const auto& frontier : {f.currentNodes, f.nextNodes, f.currentLeaves, f.nextLeaves, f.clusters})
            frontier->ResizeStructured(frontierCapacity);
        for (const auto& scalar : {f.currentNodeCounter, f.nextNodeCounter, f.currentLeafCounter, f.nextLeafCounter,
                 f.clusterCounter, f.nodeDispatchArgs, f.leafDispatchArgs, f.clusterDispatchArgs})
            scalar->ResizeStructured(1u);
    }
    for (const auto& counter : {f.currentNodeCounter, f.nextNodeCounter, f.currentLeafCounter, f.nextLeafCounter,
             f.clusterCounter})
        br::render::ResetCullingCounter(counter, upload);

    // Phase 2 continues from phase 1's replay state and telemetry.
    if (!m_config.isFirstPass) {
        return;
    }
    br::render::ResetOcclusionReplayState(m_resources.occlusionReplay.state, upload);
    if (IsCLodWorkGraphTelemetryEnabled()) {
        br::render::ResetCullingTelemetry(m_resources.telemetry, m_zeroTelemetryScratch, upload);
    }
}

std::shared_ptr<org::Resource> ComputeCullingPass::ProvideResource(org::ResourceIdentifier const& key)
{
    if (m_voxelQueueTable.Enabled() && key == m_voxelQueueTable.Id()) return m_voxelQueueTable.Buffer();
    return nullptr;
}

std::vector<org::ResourceIdentifier> ComputeCullingPass::GetSupportedKeys()
{
    if (!m_voxelQueueTable.Enabled()) return {};
    return {org::ResourceIdentifier{m_voxelQueueTable.Id()}};
}
