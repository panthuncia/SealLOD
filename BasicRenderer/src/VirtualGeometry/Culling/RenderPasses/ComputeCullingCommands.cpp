#include "VirtualGeometry/Culling/RenderPasses/ComputeCullingCommands.h"
#include "Render/PassBuilders.h"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>

#include <spdlog/spdlog.h>

#include "BasicRenderer/Diagnostics/CLodTelemetry.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "../shaders/PerPassRootConstants/clodClearUintBufferRootConstants.h"
#include "../shaders/PerPassRootConstants/clodCreateCommandRootConstants.h"
#include "../shaders/PerPassRootConstants/clodPureComputeCullingRootConstants.h"
#include "../shaders/PerPassRootConstants/clodWorkGraphRootConstants.h"

namespace {

static_assert(
    CLOD_PC_OBJECT_CULL_SHADOW_CASTER_CLASS >
        CLOD_WG_VIRTUAL_SHADOW_DYNAMIC_ACTIVE_BLOCK_METADATA_DESCRIPTOR_INDEX &&
    CLOD_PC_OBJECT_CULL_INVALIDATION_COUNT_SRV_INDEX >
        CLOD_WG_VIRTUAL_SHADOW_DYNAMIC_ACTIVE_BLOCK_METADATA_DESCRIPTOR_INDEX &&
    CLOD_WG_VIRTUAL_SHADOW_RECEIVER_MASK_DESCRIPTOR_INDEX >
        CLOD_WG_VIRTUAL_SHADOW_DYNAMIC_ACTIVE_BLOCK_METADATA_DESCRIPTOR_INDEX &&
    CLOD_PC_OBJECT_CULL_SHADOW_CASTER_CLASS !=
        CLOD_PC_OBJECT_CULL_INVALIDATION_COUNT_SRV_INDEX &&
    CLOD_PC_OBJECT_CULL_SHADOW_CASTER_CLASS !=
        CLOD_WG_VIRTUAL_SHADOW_RECEIVER_MASK_DESCRIPTOR_INDEX &&
    CLOD_PC_OBJECT_CULL_INVALIDATION_COUNT_SRV_INDEX !=
        CLOD_WG_VIRTUAL_SHADOW_RECEIVER_MASK_DESCRIPTOR_INDEX,
    "Object-cull-only constants must not overwrite shared CLod traversal state");

constexpr uint32_t kTraverseThreadsPerGroup = 64u;
constexpr uint32_t kReplayClusterThreadsPerGroup = 32u;
constexpr uint32_t kReplaySourceNodes = 0u;
constexpr uint32_t kReplaySourceClusters = 1u;

using RootConstants = std::array<uint32_t, NumMiscUintRootConstants>;
using BufferList = std::initializer_list<std::shared_ptr<org::Buffer>>;

class ComputeCullingEmitter {
public:
    ComputeCullingEmitter(const ComputeCullingCommandInputs& inputs, const org::PassPrepareContext& preparation)
        : m_in(inputs)
        , m_preparation(preparation)
        , m_builder(preparation, PSOManager::GetInstance().GetComputeRootSignature().GetHandle())
    {
    }

    ComputeCullingRecipe Emit()
    {
        m_builder.Retain(m_in.programs.dispatchSignature);
        m_shared = SharedConstants();
        ClearFrameCounters();
        if (m_in.config.isFirstPass) SeedFromObjectCull();
        else SeedFromOcclusionReplay();
        TraverseLevels();
        CreateRasterBucketCommand();
        return Finish();
    }

private:
    // Views and commands.

    uint32_t View(const org::Resource* resource, org::BindlessViewKind kind, uint32_t variant = UINT32_MAX)
    {
        if (!resource) throw std::invalid_argument("Cannot resolve a null culling resource");
        const auto reference = m_builder.Context().CaptureResource(resource->GetGlobalResourceID());
        return m_preparation.bindings->Views(reference).Resolve({kind, variant, 0u, 0u}).index;
    }
    template<class T> uint32_t Srv(const std::shared_ptr<T>& resource, uint32_t variant = UINT32_MAX)
    {
        return View(resource.get(), org::BindlessViewKind::ShaderResource, variant);
    }
    template<class T> uint32_t Uav(const std::shared_ptr<T>& resource, uint32_t variant = UINT32_MAX)
    {
        return View(resource.get(), org::BindlessViewKind::UnorderedAccess, variant);
    }
    template<class T> uint32_t SrvOrZero(const std::shared_ptr<T>& resource, uint32_t variant = UINT32_MAX)
    {
        return resource ? Srv(resource, variant) : 0u;
    }
    template<class T> uint32_t UavOrZero(const std::shared_ptr<T>& resource, uint32_t variant = UINT32_MAX)
    {
        return resource ? Uav(resource, variant) : 0u;
    }

    void Bind(const org::PipelineState& pipeline) { m_builder.Bind(pipeline); }

    void Push(const RootConstants& constants)
    {
        m_builder.Constants(constants.data(), NumMiscUintRootConstants, MiscUintRootSignatureIndex);
    }

    // Constants built from the shared set carry the DynamicWind cache
    // generation, which each invocation patches in at record time.
    void PushShared(const RootConstants& constants)
    {
        m_builder.Commands().emplace_back(br::render::PreparedComputeConstants{
            MiscUintRootSignatureIndex, 0,
            std::vector<uint32_t>(constants.begin(), constants.end()),
            (uint64_t{1} << CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_GENERATION)
                | (uint64_t{1} << CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_ENTRY_COUNT)});
    }

    void Dispatch() { m_builder.Dispatch(1u, 1u, 1u); }

    void ExecuteIndirect(const std::shared_ptr<org::Buffer>& arguments)
    {
        m_builder.Commands().emplace_back(br::render::PreparedExecuteIndirectCommand{
            (*m_in.programs.dispatchSignature)->GetHandle(), m_builder.Capture(arguments), 0, 1});
    }

    void Barrier(BufferList buffers, rhi::ResourceAccessType before, rhi::ResourceAccessType after,
        rhi::ResourceSyncState beforeSync = rhi::ResourceSyncState::ComputeShading,
        rhi::ResourceSyncState afterSync = rhi::ResourceSyncState::ComputeShading)
    {
        m_builder.Barriers(buffers, before, after, beforeSync, afterSync);
    }
    void UavBarrier(BufferList buffers)
    {
        Barrier(buffers, rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceAccessType::UnorderedAccess);
    }
    void UavToRead(BufferList buffers)
    {
        Barrier(buffers, rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceAccessType::ShaderResource);
    }
    void ReadToUav(BufferList buffers)
    {
        Barrier(buffers, rhi::ResourceAccessType::ShaderResource, rhi::ResourceAccessType::UnorderedAccess);
    }
    void UavToIndirect(BufferList buffers)
    {
        Barrier(buffers, rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceAccessType::IndirectArgument,
            rhi::ResourceSyncState::ComputeShading, rhi::ResourceSyncState::ExecuteIndirect);
    }
    void IndirectToUav(const std::shared_ptr<org::Buffer>& buffer)
    {
        Barrier({buffer}, rhi::ResourceAccessType::IndirectArgument, rhi::ResourceAccessType::UnorderedAccess,
            rhi::ResourceSyncState::ExecuteIndirect, rhi::ResourceSyncState::ComputeShading);
    }
    // Argument buffers are rewritten by later dispatches; the first rewrite after
    // an indirect launch must wait for that launch to read them.
    void ReclaimArgs(const std::shared_ptr<org::Buffer>& buffer, bool& inUse)
    {
        if (!inUse) return;
        IndirectToUav(buffer);
        inUse = false;
    }

    // Small utility dispatches.

    void ClearCounter(const std::shared_ptr<org::Buffer>& buffer, uint32_t count = 1u)
    {
        Bind(m_in.programs.clearCounter);
        RootConstants constants{};
        constants[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = Uav(buffer);
        constants[CLOD_CLEAR_UINT_BUFFER_VALUE] = 0u;
        constants[CLOD_CLEAR_UINT_BUFFER_COUNT] = count;
        Push(constants);
        m_builder.Dispatch((count + 63u) / 64u, 1u, 1u);
    }

    void BuildDispatchArgs(const std::shared_ptr<org::Buffer>& counter, const std::shared_ptr<org::Buffer>& arguments,
        uint32_t itemsPerGroup)
    {
        Bind(m_in.programs.buildDispatchArgs);
        RootConstants constants = m_shared;
        constants[CLOD_PC_DISPATCH_COUNTER_DESCRIPTOR_INDEX] = Srv(counter);
        constants[CLOD_PC_DISPATCH_ARGS_DESCRIPTOR_INDEX] = Uav(arguments);
        constants[CLOD_PC_DISPATCH_THREADS_PER_GROUP] = itemsPerGroup;
        constants[CLOD_PC_DISPATCH_COUNT_LIMIT] = m_in.config.maxVisibleClusters;
        PushShared(constants);
        Dispatch();
    }

    void BuildNodeAndLeafDispatchArgs()
    {
        const auto& frontiers = m_in.frontiers;
        Bind(m_in.programs.buildDualDispatchArgs);
        RootConstants constants = m_shared;
        constants[CLOD_PC_DISPATCH_COUNTER_DESCRIPTOR_INDEX] = Srv(m_currentNodeCounter);
        constants[CLOD_PC_DISPATCH_ARGS_DESCRIPTOR_INDEX] = Uav(frontiers.nodeDispatchArgs);
        constants[CLOD_PC_DISPATCH_THREADS_PER_GROUP] = kTraverseThreadsPerGroup;
        constants[CLOD_PC_DISPATCH_COUNT_LIMIT] = m_in.config.maxVisibleClusters;
        constants[CLOD_PC_SECOND_DISPATCH_COUNTER_DESCRIPTOR_INDEX] = Srv(m_currentLeafCounter);
        constants[CLOD_PC_SECOND_DISPATCH_ARGS_DESCRIPTOR_INDEX] = Uav(frontiers.leafDispatchArgs);
        PushShared(constants);
        Dispatch();
    }

    void BuildReplayDispatchArgs(const std::shared_ptr<org::Buffer>& arguments, uint32_t replaySource,
        uint32_t threadsPerGroup)
    {
        Bind(m_in.programs.buildReplayDispatchArgs);
        RootConstants constants{};
        constants[CLOD_WG_VISIBLE_CLUSTERS_CAPACITY] = m_in.config.maxVisibleClusters;
        constants[CLOD_WG_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX] = Uav(m_in.resources.occlusionReplay.state);
        constants[CLOD_PC_DISPATCH_ARGS_DESCRIPTOR_INDEX] = Uav(arguments);
        constants[CLOD_PC_DISPATCH_THREADS_PER_GROUP] = threadsPerGroup;
        constants[CLOD_PC_REPLAY_SOURCE_INDEX] = replaySource;
        Push(constants);
        Dispatch();
    }

    // The root constants every traversal stage reads.
    RootConstants SharedConstants()
    {
        const auto& config = m_in.config;
        const auto& outputs = m_in.resources.outputs;
        const auto& shadow = m_in.resources.virtualShadow;
        const auto& wind = m_in.windCache;
        const uint32_t arrayView = static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull);
        RootConstants shared{};
        shared[CLOD_WG_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = Uav(outputs.visibleClusters);
        shared[CLOD_WG_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = Uav(outputs.visibleClusterTransformIndices);
        shared[CLOD_WG_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = Uav(outputs.visibleClustersCounter);
        shared[CLOD_WG_FORCED_TRAVERSAL_DEPTH_ROOT] = m_in.settings.forcedTraversalDepth;
        shared[CLOD_WG_SW_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = UavOrZero(outputs.swVisibleClustersCounter);
        shared[CLOD_WG_TELEMETRY_DESCRIPTOR_INDEX] = Uav(m_in.resources.telemetry);
        shared[CLOD_WG_OCCLUSION_REPLAY_BUFFER_DESCRIPTOR_INDEX] = Uav(m_in.resources.occlusionReplay.replay);
        shared[CLOD_WG_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX] = Uav(m_in.resources.occlusionReplay.state);
        shared[CLOD_WG_WORKGRAPH_NODE_INPUTS_DESCRIPTOR_INDEX] = 0u;
        shared[CLOD_WG_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX] = m_in.viewRasterInfoTable;
        shared[CLOD_WG_VIEW_DEPTH_SRV_INDICES_DESCRIPTOR_INDEX] =
            config.UsesPerViewDepthOcclusion() ? m_in.viewDepthTable : 0u;
        shared[CLOD_WG_VISIBLE_CLUSTERS_CAPACITY] = config.maxVisibleClusters;

        shared[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_DESCRIPTOR_INDEX] = UavOrZero(wind.bounds);
        shared[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_ENTRY_COUNT] = wind.entryCount;
        shared[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_GENERATION] = wind.generation;
        shared[CLOD_WG_DYNAMIC_WIND_VISIBLE_MEMBERSHIP_DESCRIPTOR_INDEX] = wind.bounds
            ? View(wind.visibleMembership, org::BindlessViewKind::ShaderResource) : 0u;
        if (wind.bounds && wind.generation == 1u) {
            spdlog::info("CLod DynamicWind bounds cache bindings: entries={} cacheDescriptor={} membershipDescriptor={}",
                wind.entryCount,
                shared[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_DESCRIPTOR_INDEX],
                shared[CLOD_WG_DYNAMIC_WIND_VISIBLE_MEMBERSHIP_DESCRIPTOR_INDEX]);
        }

        shared[CLOD_WG_SHADOW_DIRTY_HIERARCHY_DESCRIPTOR_INDEX] = SrvOrZero(shadow.dirtyPageHierarchy, arrayView);
        shared[CLOD_WG_SHADOW_INVALIDATED_INSTANCES_DESCRIPTOR_INDEX] = SrvOrZero(shadow.invalidatedInstancesBitset);
        shared[CLOD_WG_SHADOW_PREDICTIVE_INVALIDATION_CANDIDATES_DESCRIPTOR_INDEX] =
            UavOrZero(shadow.predictiveInvalidationCandidates);
        shared[CLOD_WG_SHADOW_PREDICTIVE_INVALIDATION_CANDIDATE_COUNT_DESCRIPTOR_INDEX] =
            UavOrZero(shadow.predictiveInvalidationCandidateCount);
        shared[CLOD_WG_VIRTUAL_SHADOW_PAGE_TABLE_UAV_DESCRIPTOR_INDEX] = UavOrZero(shadow.pageTable,
            static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull));
        shared[CLOD_WG_VIRTUAL_SHADOW_PHYSICAL_PAGES_UAV_DESCRIPTOR_INDEX] = UavOrZero(shadow.staticPhysicalPages);
        shared[CLOD_WG_VIRTUAL_SHADOW_ACTIVE_BLOCK_METADATA_DESCRIPTOR_INDEX] = SrvOrZero(shadow.activeBlockMetadata);
        shared[CLOD_WG_VIRTUAL_SHADOW_RECEIVER_MASK_DESCRIPTOR_INDEX] = SrvOrZero(shadow.receiverSubpageMask);
        shared[CLOD_WG_VIRTUAL_SHADOW_RECEIVER_MASK_MODE] = m_in.settings.receiverSubpageMode;
        shared[CLOD_WG_VIRTUAL_SHADOW_DYNAMIC_PAGES_UAV_DESCRIPTOR_INDEX] = UavOrZero(shadow.dynamicPhysicalPages);
        shared[CLOD_WG_VIRTUAL_SHADOW_DYNAMIC_ACTIVE_BLOCK_METADATA_DESCRIPTOR_INDEX] =
            SrvOrZero(shadow.dynamicActiveBlockMetadata);

        // Phase 2 appends after phase 1's clusters.
        const auto& phase1 = m_in.resources.phase1Counters;
        shared[CLOD_WG_HW_WRITE_BASE_COUNTER_DESCRIPTOR_INDEX] =
            Srv(phase1.visibleClustersCounter ? phase1.visibleClustersCounter : outputs.visibleClustersCounter);
        shared[CLOD_WG_SW_WRITE_BASE_COUNTER_DESCRIPTOR_INDEX] =
            Srv(phase1.swVisibleClustersCounter ? phase1.swVisibleClustersCounter : outputs.swVisibleClustersCounter);
        shared[CLOD_PC_PHASE2_EXPANSION_FACTOR] = m_in.expansionFactor;
        shared[CLOD_WG_FLAGS] = br::render::BuildCullingFlags(config, m_in.settings);
        return shared;
    }

    // Stages.

    void ClearFrameCounters()
    {
        const auto& frontiers = m_in.frontiers;
        if (m_in.config.isFirstPass && m_in.settings.telemetry) {
            ClearCounter(m_in.resources.telemetry, CLodWorkGraphTelemetryBufferCount);
            UavBarrier({m_in.resources.telemetry});
        }
        const auto& voxels = m_in.resources.voxelQueues;
        if (voxels.Enabled()) {
            ClearCounter(voxels.rigidWorkCounter);
            ClearCounter(voxels.skinnedWorkCounter);
            UavBarrier({voxels.rigidWorkCounter, voxels.skinnedWorkCounter});
        }
        ClearCounter(frontiers.currentNodeCounter);
        ClearCounter(frontiers.nextNodeCounter);
        ClearCounter(frontiers.currentLeafCounter);
        ClearCounter(frontiers.nextLeafCounter);
        ClearCounter(frontiers.clusterCounter);
        UavBarrier({frontiers.currentNodeCounter, frontiers.nextNodeCounter, frontiers.currentLeafCounter,
            frontiers.nextLeafCounter, frontiers.clusterCounter});
    }

    // Phase 1: object culling writes the root nodes of visible instances into
    // the first node frontier. It dispatches once per workload at record time,
    // so the recipe splits here.
    void SeedFromObjectCull()
    {
        const auto& frontiers = m_in.frontiers;
        Bind(m_in.programs.objectCull);
        m_objectCullConstants = m_shared;
        m_objectCullConstants[CLOD_PC_FRONTIER_OUTPUT_DESCRIPTOR_INDEX] = Uav(frontiers.currentNodes);
        m_objectCullConstants[CLOD_PC_FRONTIER_OUTPUT_COUNT_DESCRIPTOR_INDEX] = Uav(frontiers.currentNodeCounter);
        m_objectCullConstants[CLOD_PC_OBJECT_CULL_INVALIDATION_COUNT_SRV_INDEX] =
            SrvOrZero(m_in.resources.virtualShadow.invalidationCount);
        m_objectCullOffset = m_builder.Commands().size();
        UavToRead({frontiers.currentNodes, frontiers.currentNodeCounter});
    }

    // Phase 2: replayed nodes reseed the node frontier, and replayed clusters
    // are culled straight away against the new depth pyramid.
    void SeedFromOcclusionReplay()
    {
        const auto& frontiers = m_in.frontiers;
        BuildReplayDispatchArgs(frontiers.nodeDispatchArgs, kReplaySourceNodes, kTraverseThreadsPerGroup);
        UavToIndirect({frontiers.nodeDispatchArgs});
        Bind(m_in.programs.seedReplayNodes);
        RootConstants nodes = m_shared;
        nodes[CLOD_PC_FRONTIER_OUTPUT_DESCRIPTOR_INDEX] = Uav(frontiers.currentNodes);
        nodes[CLOD_PC_FRONTIER_OUTPUT_COUNT_DESCRIPTOR_INDEX] = Uav(frontiers.currentNodeCounter);
        PushShared(nodes);
        ExecuteIndirect(frontiers.nodeDispatchArgs);
        m_nodeArgsInUse = true;

        BuildReplayDispatchArgs(frontiers.clusterDispatchArgs, kReplaySourceClusters, kReplayClusterThreadsPerGroup);
        UavToIndirect({frontiers.clusterDispatchArgs});
        Bind(m_in.programs.seedReplayClusters);
        RootConstants clusters = m_shared;
        clusters[CLOD_PC_CLUSTER_OUTPUT_DESCRIPTOR_INDEX] = Uav(frontiers.clusters);
        clusters[CLOD_PC_CLUSTER_OUTPUT_COUNT_DESCRIPTOR_INDEX] = Uav(frontiers.clusterCounter);
        PushShared(clusters);
        ExecuteIndirect(frontiers.clusterDispatchArgs);
        m_clusterArgsInUse = true;

        UavToRead({frontiers.currentNodes, frontiers.currentNodeCounter, frontiers.clusters, frontiers.clusterCounter});
        CullClusterFrontier();
        ReadToUav({frontiers.clusters, frontiers.clusterCounter});
    }

    // Dense cluster culling: each cluster record expands across
    // expansionFactor lanes of a group.
    void CullClusterFrontier()
    {
        const auto& frontiers = m_in.frontiers;
        ReclaimArgs(frontiers.clusterDispatchArgs, m_clusterArgsInUse);
        BuildDispatchArgs(frontiers.clusterCounter, frontiers.clusterDispatchArgs, 64u / m_in.expansionFactor);
        UavToIndirect({frontiers.clusterDispatchArgs});
        Bind(m_in.programs.cullClusters);
        RootConstants constants = m_shared;
        constants[CLOD_PC_FRONTIER_INPUT_DESCRIPTOR_INDEX] = Srv(frontiers.clusters);
        constants[CLOD_PC_FRONTIER_INPUT_COUNT_DESCRIPTOR_INDEX] = Srv(frontiers.clusterCounter);
        PushShared(constants);
        ExecuteIndirect(frontiers.clusterDispatchArgs);
        m_clusterArgsInUse = true;
    }

    // Expands one frontier (interior nodes or leaves) into the next level's
    // frontiers and the cluster frontier.
    void ExpandFrontier(const org::PipelineState& program, const std::shared_ptr<org::Buffer>& frontier,
        const std::shared_ptr<org::Buffer>& counter, const std::shared_ptr<org::Buffer>& arguments)
    {
        const auto& frontiers = m_in.frontiers;
        Bind(program);
        RootConstants constants = m_shared;
        constants[CLOD_PC_FRONTIER_INPUT_DESCRIPTOR_INDEX] = Srv(frontier);
        constants[CLOD_PC_FRONTIER_INPUT_COUNT_DESCRIPTOR_INDEX] = Srv(counter);
        constants[CLOD_PC_FRONTIER_OUTPUT_DESCRIPTOR_INDEX] = Uav(m_nextNodes);
        constants[CLOD_PC_FRONTIER_OUTPUT_COUNT_DESCRIPTOR_INDEX] = Uav(m_nextNodeCounter);
        constants[CLOD_PC_CLUSTER_OUTPUT_DESCRIPTOR_INDEX] = Uav(frontiers.clusters);
        constants[CLOD_PC_CLUSTER_OUTPUT_COUNT_DESCRIPTOR_INDEX] = Uav(frontiers.clusterCounter);
        constants[CLOD_PC_LEAF_OUTPUT_DESCRIPTOR_INDEX] = Uav(m_nextLeaves);
        constants[CLOD_PC_LEAF_OUTPUT_COUNT_DESCRIPTOR_INDEX] = Uav(m_nextLeafCounter);
        PushShared(constants);
        ExecuteIndirect(arguments);
    }

    void TraverseLevel(uint32_t level)
    {
        const auto& frontiers = m_in.frontiers;
        const auto& outputs = m_in.resources.outputs;
        const auto& replay = m_in.resources.occlusionReplay;
        if (level > 0u) ReadToUav({m_nextNodes, m_nextNodeCounter, m_nextLeaves, m_nextLeafCounter});

        Bind(m_in.programs.clearTraversalCounters);
        RootConstants clear = m_shared;
        clear[CLOD_PC_FRONTIER_OUTPUT_COUNT_DESCRIPTOR_INDEX] = Uav(m_nextNodeCounter);
        clear[CLOD_PC_LEAF_OUTPUT_COUNT_DESCRIPTOR_INDEX] = Uav(m_nextLeafCounter);
        clear[CLOD_PC_CLUSTER_OUTPUT_COUNT_DESCRIPTOR_INDEX] = Uav(frontiers.clusterCounter);
        PushShared(clear);
        Dispatch();
        UavBarrier({m_nextNodeCounter, m_nextLeafCounter, frontiers.clusterCounter});

        ReclaimArgs(frontiers.nodeDispatchArgs, m_nodeArgsInUse);
        ReclaimArgs(frontiers.leafDispatchArgs, m_leafArgsInUse);
        BuildNodeAndLeafDispatchArgs();
        UavToIndirect({frontiers.nodeDispatchArgs, frontiers.leafDispatchArgs});

        ExpandFrontier(m_in.programs.traverseNodes, m_currentNodes, m_currentNodeCounter, frontiers.nodeDispatchArgs);
        UavBarrier({m_nextNodes, m_nextNodeCounter, m_nextLeaves, m_nextLeafCounter,
            frontiers.clusters, frontiers.clusterCounter});
        ExpandFrontier(m_in.programs.traverseLeaves, m_currentLeaves, m_currentLeafCounter, frontiers.leafDispatchArgs);
        m_nodeArgsInUse = true;
        m_leafArgsInUse = true;

        UavToRead({m_nextNodes, m_nextNodeCounter, m_nextLeaves, m_nextLeafCounter,
            frontiers.clusters, frontiers.clusterCounter});
        UavBarrier({replay.replay, replay.state});
        CullClusterFrontier();
        ReadToUav({frontiers.clusters, frontiers.clusterCounter});
        UavBarrier({outputs.visibleClusters, outputs.visibleClustersCounter, replay.replay, replay.state});

        std::swap(m_currentNodes, m_nextNodes);
        std::swap(m_currentNodeCounter, m_nextNodeCounter);
        std::swap(m_currentLeaves, m_nextLeaves);
        std::swap(m_currentLeafCounter, m_nextLeafCounter);
    }

    void TraverseLevels()
    {
        const auto& frontiers = m_in.frontiers;
        const auto& replay = m_in.resources.occlusionReplay;
        UavToRead({frontiers.currentNodes, frontiers.currentNodeCounter,
            frontiers.currentLeaves, frontiers.currentLeafCounter});
        UavBarrier({m_in.resources.outputs.visibleClustersCounter, replay.replay, replay.state});

        m_currentNodes = frontiers.currentNodes;
        m_currentNodeCounter = frontiers.currentNodeCounter;
        m_nextNodes = frontiers.nextNodes;
        m_nextNodeCounter = frontiers.nextNodeCounter;
        m_currentLeaves = frontiers.currentLeaves;
        m_currentLeafCounter = frontiers.currentLeafCounter;
        m_nextLeaves = frontiers.nextLeaves;
        m_nextLeafCounter = frontiers.nextLeafCounter;
        const uint32_t levels = std::min(m_in.traversalDepth, kComputeCullingMaxTraversalLevels);
        for (uint32_t level = 0; level < levels; ++level) TraverseLevel(level);
    }

    void CreateRasterBucketCommand()
    {
        const auto& outputs = m_in.resources.outputs;
        const auto& replay = m_in.resources.occlusionReplay;
        UavToRead({outputs.visibleClustersCounter, replay.state});
        Bind(m_in.programs.createRasterBucketCommand);
        RootConstants constants = m_shared;
        constants[CLOD_CREATE_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = Srv(outputs.visibleClustersCounter);
        constants[CLOD_CREATE_RASTER_BUCKET_HISTOGRAM_COMMAND_DESCRIPTOR_INDEX] = Uav(outputs.rasterBucketHistogramCommand);
        constants[CLOD_CREATE_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX] = Srv(replay.state);
        // Compute replay seeds frontiers directly; there are no work-graph node inputs to patch.
        constants[CLOD_CREATE_WORKGRAPH_NODE_INPUTS_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
        constants[CLOD_CREATE_NUM_RASTER_BUCKETS] = m_in.rasterBucketCount;
        constants[CLOD_CREATE_VISIBLE_CLUSTERS_CAPACITY] = m_in.config.maxVisibleClusters;
        PushShared(constants);
        Dispatch();
    }

    ComputeCullingRecipe Finish()
    {
        ComputeCullingRecipe recipe;
        recipe.prefix = std::move(m_builder).FinishData();
        if (m_objectCullOffset) {
            recipe.hasObjectCull = true;
            recipe.objectCullConstants = m_objectCullConstants;
            auto& commands = recipe.prefix.commands;
            recipe.suffix.commands.assign(std::make_move_iterator(commands.begin() + *m_objectCullOffset),
                std::make_move_iterator(commands.end()));
            commands.resize(*m_objectCullOffset);
        }
        return recipe;
    }

    const ComputeCullingCommandInputs& m_in;
    const org::PassPrepareContext& m_preparation;
    br::render::PreparedComputeCommandBuilder m_builder;
    RootConstants m_shared{};
    std::optional<size_t> m_objectCullOffset;
    RootConstants m_objectCullConstants{};
    bool m_nodeArgsInUse = false;
    bool m_leafArgsInUse = false;
    bool m_clusterArgsInUse = false;
    // Ping-pong frontier roles for the level being traversed.
    std::shared_ptr<org::Buffer> m_currentNodes, m_currentNodeCounter, m_nextNodes, m_nextNodeCounter;
    std::shared_ptr<org::Buffer> m_currentLeaves, m_currentLeafCounter, m_nextLeaves, m_nextLeafCounter;
};

}

ComputeCullingRecipe EmitComputeCullingCommands(const ComputeCullingCommandInputs& inputs,
    const org::PassPrepareContext& preparation)
{
    if (!preparation.bindings) throw std::invalid_argument("Compute culling needs frozen bindings");
    return ComputeCullingEmitter(inputs, preparation).Emit();
}
