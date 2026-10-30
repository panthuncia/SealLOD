#include "VirtualGeometry/Culling/RenderPasses/ComputeCullingCommands.h"

#include <algorithm>
#include <initializer_list>
#include <utility>

#include "BasicRenderer/Diagnostics/CLodTelemetry.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
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

using RootConstants = br::render::MiscConstants;
using Token = org::DeclaredViewToken;
using TokenList = std::initializer_list<Token>;

bool Declared(const Token& token) { return token.layout != nullptr; }

class ComputeCullingRecorder {
public:
    ComputeCullingRecorder(const ComputeCullingBindings& bindings, const ComputeCullingFrame& frame,
        org::PassRecordContext& context)
        : m_bindings(bindings), m_frame(frame), m_context(context), m_programs(bindings.programs)
    {
    }

    void Record()
    {
        m_shared = SharedConstants();
        ClearFrameCounters();
        if (m_bindings.isFirstPass) SeedFromObjectCull();
        else SeedFromOcclusionReplay();
        TraverseLevels();
        CreateRasterBucketCommand();
    }

private:
    // Views and commands.

    uint32_t Uav(const Token& token) { return m_context.Index(token); }
    uint32_t Uav(const Token& token, uint32_t variant)
    {
        return m_context.Index(token.Resource(), org::UavView{variant});
    }
    // Shader-resource view of a declared resource; for frontiers and counters
    // declared for unordered access, read after an in-pass barrier.
    uint32_t Srv(const Token& token, uint32_t variant = UINT32_MAX)
    {
        return m_context.Index(token.Resource(), org::SrvView{variant});
    }
    uint32_t SrvOrZero(const Token& token, uint32_t variant = UINT32_MAX)
    {
        return Declared(token) ? Srv(token, variant) : 0u;
    }
    uint32_t UavOrZero(const Token& token) { return Declared(token) ? Uav(token) : 0u; }
    uint32_t UavOrZero(const Token& token, uint32_t variant) { return Declared(token) ? Uav(token, variant) : 0u; }

    void Bind(const org::PipelineState& pipeline) { m_context.BindComputePipeline(pipeline); }
    void Push(const RootConstants& constants) { br::render::PushMiscConstants(m_context, constants); }
    void Dispatch() { m_context.Commands().Dispatch(1u, 1u, 1u); }
    void ExecuteIndirect(const Token& arguments)
    {
        m_context.ExecuteIndirect(m_programs.dispatchSignature, arguments.Resource());
    }

    // One batch over the declared buffers in the list; absent ones are skipped.
    void Barrier(TokenList buffers, rhi::ResourceAccessType before, rhi::ResourceAccessType after,
        rhi::ResourceSyncState beforeSync = rhi::ResourceSyncState::ComputeShading,
        rhi::ResourceSyncState afterSync = rhi::ResourceSyncState::ComputeShading)
    {
        std::vector<rhi::BufferBarrier> barriers;
        barriers.reserve(buffers.size());
        for (const auto& token : buffers) {
            if (!Declared(token)) continue;
            rhi::BufferBarrier barrier{};
            barrier.buffer = m_context.Resolve(token).GetHandle();
            barrier.beforeAccess = before;
            barrier.afterAccess = after;
            barrier.beforeSync = beforeSync;
            barrier.afterSync = afterSync;
            barriers.push_back(barrier);
        }
        if (barriers.empty()) return;
        rhi::BarrierBatch batch{};
        batch.buffers = rhi::Span<rhi::BufferBarrier>(barriers.data(), static_cast<uint32_t>(barriers.size()));
        m_context.Commands().Barriers(batch);
    }
    void UavBarrier(TokenList buffers)
    {
        Barrier(buffers, rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceAccessType::UnorderedAccess);
    }
    void UavToRead(TokenList buffers)
    {
        Barrier(buffers, rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceAccessType::ShaderResource);
    }
    void ReadToUav(TokenList buffers)
    {
        Barrier(buffers, rhi::ResourceAccessType::ShaderResource, rhi::ResourceAccessType::UnorderedAccess);
    }
    void UavToIndirect(TokenList buffers)
    {
        Barrier(buffers, rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceAccessType::IndirectArgument,
            rhi::ResourceSyncState::ComputeShading, rhi::ResourceSyncState::ExecuteIndirect);
    }
    void IndirectToUav(const Token& buffer)
    {
        Barrier({buffer}, rhi::ResourceAccessType::IndirectArgument, rhi::ResourceAccessType::UnorderedAccess,
            rhi::ResourceSyncState::ExecuteIndirect, rhi::ResourceSyncState::ComputeShading);
    }
    // Argument buffers are rewritten by later dispatches; the first rewrite after
    // an indirect launch must wait for that launch to read them.
    void ReclaimArgs(const Token& buffer, bool& inUse)
    {
        if (!inUse) return;
        IndirectToUav(buffer);
        inUse = false;
    }

    // Small utility dispatches.

    void ClearCounter(const Token& buffer, uint32_t count = 1u)
    {
        Bind(m_programs.clearCounter);
        RootConstants constants{};
        constants[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = Uav(buffer);
        constants[CLOD_CLEAR_UINT_BUFFER_VALUE] = 0u;
        constants[CLOD_CLEAR_UINT_BUFFER_COUNT] = count;
        Push(constants);
        m_context.Commands().Dispatch((count + 63u) / 64u, 1u, 1u);
    }

    void BuildDispatchArgs(const Token& counter, const Token& arguments, uint32_t itemsPerGroup)
    {
        Bind(m_programs.buildDispatchArgs);
        RootConstants constants = m_shared;
        constants[CLOD_PC_DISPATCH_COUNTER_DESCRIPTOR_INDEX] = Srv(counter);
        constants[CLOD_PC_DISPATCH_ARGS_DESCRIPTOR_INDEX] = Uav(arguments);
        constants[CLOD_PC_DISPATCH_THREADS_PER_GROUP] = itemsPerGroup;
        constants[CLOD_PC_DISPATCH_COUNT_LIMIT] = m_bindings.maxVisibleClusters;
        Push(constants);
        Dispatch();
    }

    void BuildNodeAndLeafDispatchArgs()
    {
        const auto& frontiers = m_bindings.frontiers;
        Bind(m_programs.buildDualDispatchArgs);
        RootConstants constants = m_shared;
        constants[CLOD_PC_DISPATCH_COUNTER_DESCRIPTOR_INDEX] = Srv(m_currentNodeCounter);
        constants[CLOD_PC_DISPATCH_ARGS_DESCRIPTOR_INDEX] = Uav(frontiers.nodeDispatchArgs);
        constants[CLOD_PC_DISPATCH_THREADS_PER_GROUP] = kTraverseThreadsPerGroup;
        constants[CLOD_PC_DISPATCH_COUNT_LIMIT] = m_bindings.maxVisibleClusters;
        constants[CLOD_PC_SECOND_DISPATCH_COUNTER_DESCRIPTOR_INDEX] = Srv(m_currentLeafCounter);
        constants[CLOD_PC_SECOND_DISPATCH_ARGS_DESCRIPTOR_INDEX] = Uav(frontiers.leafDispatchArgs);
        Push(constants);
        Dispatch();
    }

    void BuildReplayDispatchArgs(const Token& arguments, uint32_t replaySource, uint32_t threadsPerGroup)
    {
        Bind(m_programs.buildReplayDispatchArgs);
        RootConstants constants{};
        constants[CLOD_WG_VISIBLE_CLUSTERS_CAPACITY] = m_bindings.maxVisibleClusters;
        constants[CLOD_WG_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX] = Uav(m_bindings.replayState);
        constants[CLOD_PC_DISPATCH_ARGS_DESCRIPTOR_INDEX] = Uav(arguments);
        constants[CLOD_PC_DISPATCH_THREADS_PER_GROUP] = threadsPerGroup;
        constants[CLOD_PC_REPLAY_SOURCE_INDEX] = replaySource;
        Push(constants);
        Dispatch();
    }

    // The root constants every traversal stage reads.
    RootConstants SharedConstants()
    {
        const auto& b = m_bindings;
        const auto& shadow = b.shadow;
        const uint32_t arrayView = static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull);
        RootConstants shared{};
        shared[CLOD_WG_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = Uav(b.visibleClusters);
        shared[CLOD_WG_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = Uav(b.visibleClusterTransformIndices);
        shared[CLOD_WG_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = Uav(b.visibleClustersCounter);
        shared[CLOD_WG_FORCED_TRAVERSAL_DEPTH_ROOT] = m_frame.settings.forcedTraversalDepth;
        shared[CLOD_WG_SW_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = UavOrZero(b.swVisibleClustersCounter);
        shared[CLOD_WG_TELEMETRY_DESCRIPTOR_INDEX] = Uav(b.telemetry);
        shared[CLOD_WG_OCCLUSION_REPLAY_BUFFER_DESCRIPTOR_INDEX] = Uav(b.replay);
        shared[CLOD_WG_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX] = Uav(b.replayState);
        shared[CLOD_WG_WORKGRAPH_NODE_INPUTS_DESCRIPTOR_INDEX] = 0u;
        shared[CLOD_WG_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX] = m_frame.viewRasterInfoTable;
        shared[CLOD_WG_VIEW_DEPTH_SRV_INDICES_DESCRIPTOR_INDEX] = b.perViewDepthOcclusion ? m_frame.viewDepthTable : 0u;
        shared[CLOD_WG_VISIBLE_CLUSTERS_CAPACITY] = b.maxVisibleClusters;

        shared[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_DESCRIPTOR_INDEX] = UavOrZero(b.windBounds);
        shared[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_ENTRY_COUNT] = m_frame.windEntryCount;
        shared[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_GENERATION] = m_frame.windGeneration;
        shared[CLOD_WG_DYNAMIC_WIND_VISIBLE_MEMBERSHIP_DESCRIPTOR_INDEX] =
            Declared(b.windBounds) ? m_context.Index(b.windMembership) : 0u;

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
        shared[CLOD_WG_VIRTUAL_SHADOW_RECEIVER_MASK_MODE] = m_frame.settings.receiverSubpageMode;
        shared[CLOD_WG_VIRTUAL_SHADOW_DYNAMIC_PAGES_UAV_DESCRIPTOR_INDEX] = UavOrZero(shadow.dynamicPhysicalPages);
        shared[CLOD_WG_VIRTUAL_SHADOW_DYNAMIC_ACTIVE_BLOCK_METADATA_DESCRIPTOR_INDEX] =
            SrvOrZero(shadow.dynamicActiveBlockMetadata);

        // Phase 2 appends after phase 1's clusters.
        shared[CLOD_WG_HW_WRITE_BASE_COUNTER_DESCRIPTOR_INDEX] = Srv(Declared(b.phase1VisibleClustersCounter)
            ? b.phase1VisibleClustersCounter : b.visibleClustersCounter);
        shared[CLOD_WG_SW_WRITE_BASE_COUNTER_DESCRIPTOR_INDEX] = Srv(Declared(b.phase1SwVisibleClustersCounter)
            ? b.phase1SwVisibleClustersCounter : b.swVisibleClustersCounter);
        shared[CLOD_PC_PHASE2_EXPANSION_FACTOR] = m_frame.expansionFactor;
        shared[CLOD_WG_FLAGS] = m_frame.cullingFlags;
        return shared;
    }

    // Stages.

    void ClearFrameCounters()
    {
        const auto& frontiers = m_bindings.frontiers;
        if (m_bindings.isFirstPass && m_frame.settings.telemetry) {
            ClearCounter(m_bindings.telemetry, CLodWorkGraphTelemetryBufferCount);
            UavBarrier({m_bindings.telemetry});
        }
        if (m_bindings.voxelQueues) {
            const auto& voxels = *m_bindings.voxelQueues;
            ClearCounter(voxels[1]);
            ClearCounter(voxels[3]);
            UavBarrier({voxels[1], voxels[3]});
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
    // the first node frontier, one dispatch per workload.
    void SeedFromObjectCull()
    {
        const auto& frontiers = m_bindings.frontiers;
        Bind(m_programs.objectCull);
        RootConstants constants = m_shared;
        constants[CLOD_PC_FRONTIER_OUTPUT_DESCRIPTOR_INDEX] = Uav(frontiers.currentNodes);
        constants[CLOD_PC_FRONTIER_OUTPUT_COUNT_DESCRIPTOR_INDEX] = Uav(frontiers.currentNodeCounter);
        constants[CLOD_PC_OBJECT_CULL_INVALIDATION_COUNT_SRV_INDEX] = SrvOrZero(m_bindings.shadow.invalidationCount);
        for (const auto& workload : m_frame.workloads) {
            constants[CLOD_PC_OBJECT_CULL_ACTIVE_DRAW_COUNT] = workload.activeDrawCount;
            constants[CLOD_PC_OBJECT_CULL_VIEW_DATA_INDEX] = workload.viewDataIndex;
            constants[CLOD_PC_OBJECT_CULL_ACTIVE_DRAW_SET_SRV_INDEX] = workload.activeDrawSetIndicesSRVIndex;
            constants[CLOD_PC_OBJECT_CULL_VISIBILITY_GENERATION_SRV_INDEX] = workload.drawRecordVisibilityGenerationSRVIndex;
            constants[CLOD_PC_OBJECT_CULL_VISIBILITY_GENERATION_COUNT] = workload.drawRecordVisibilityGenerationCount;
            constants[CLOD_PC_OBJECT_CULL_SHADOW_CASTER_CLASS] = workload.shadowCasterClass;
            Push(constants);
            m_context.Commands().Dispatch(workload.dispatchGridX, workload.dispatchGridY, workload.dispatchGridZ);
        }
        UavToRead({frontiers.currentNodes, frontiers.currentNodeCounter});
    }

    // Phase 2: replayed nodes reseed the node frontier, and replayed clusters
    // are culled straight away against the new depth pyramid.
    void SeedFromOcclusionReplay()
    {
        const auto& frontiers = m_bindings.frontiers;
        BuildReplayDispatchArgs(frontiers.nodeDispatchArgs, kReplaySourceNodes, kTraverseThreadsPerGroup);
        UavToIndirect({frontiers.nodeDispatchArgs});
        Bind(m_programs.seedReplayNodes);
        RootConstants nodes = m_shared;
        nodes[CLOD_PC_FRONTIER_OUTPUT_DESCRIPTOR_INDEX] = Uav(frontiers.currentNodes);
        nodes[CLOD_PC_FRONTIER_OUTPUT_COUNT_DESCRIPTOR_INDEX] = Uav(frontiers.currentNodeCounter);
        Push(nodes);
        ExecuteIndirect(frontiers.nodeDispatchArgs);
        m_nodeArgsInUse = true;

        BuildReplayDispatchArgs(frontiers.clusterDispatchArgs, kReplaySourceClusters, kReplayClusterThreadsPerGroup);
        UavToIndirect({frontiers.clusterDispatchArgs});
        Bind(m_programs.seedReplayClusters);
        RootConstants clusters = m_shared;
        clusters[CLOD_PC_CLUSTER_OUTPUT_DESCRIPTOR_INDEX] = Uav(frontiers.clusters);
        clusters[CLOD_PC_CLUSTER_OUTPUT_COUNT_DESCRIPTOR_INDEX] = Uav(frontiers.clusterCounter);
        Push(clusters);
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
        const auto& frontiers = m_bindings.frontiers;
        ReclaimArgs(frontiers.clusterDispatchArgs, m_clusterArgsInUse);
        BuildDispatchArgs(frontiers.clusterCounter, frontiers.clusterDispatchArgs, 64u / m_frame.expansionFactor);
        UavToIndirect({frontiers.clusterDispatchArgs});
        Bind(m_programs.cullClusters);
        RootConstants constants = m_shared;
        constants[CLOD_PC_FRONTIER_INPUT_DESCRIPTOR_INDEX] = Srv(frontiers.clusters);
        constants[CLOD_PC_FRONTIER_INPUT_COUNT_DESCRIPTOR_INDEX] = Srv(frontiers.clusterCounter);
        Push(constants);
        ExecuteIndirect(frontiers.clusterDispatchArgs);
        m_clusterArgsInUse = true;
    }

    // Expands one frontier (interior nodes or leaves) into the next level's
    // frontiers and the cluster frontier.
    void ExpandFrontier(const org::PipelineState& program, const Token& frontier, const Token& counter,
        const Token& arguments)
    {
        const auto& frontiers = m_bindings.frontiers;
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
        Push(constants);
        ExecuteIndirect(arguments);
    }

    void TraverseLevel(uint32_t level)
    {
        const auto& frontiers = m_bindings.frontiers;
        const auto& b = m_bindings;
        if (level > 0u) ReadToUav({m_nextNodes, m_nextNodeCounter, m_nextLeaves, m_nextLeafCounter});

        Bind(m_programs.clearTraversalCounters);
        RootConstants clear = m_shared;
        clear[CLOD_PC_FRONTIER_OUTPUT_COUNT_DESCRIPTOR_INDEX] = Uav(m_nextNodeCounter);
        clear[CLOD_PC_LEAF_OUTPUT_COUNT_DESCRIPTOR_INDEX] = Uav(m_nextLeafCounter);
        clear[CLOD_PC_CLUSTER_OUTPUT_COUNT_DESCRIPTOR_INDEX] = Uav(frontiers.clusterCounter);
        Push(clear);
        Dispatch();
        UavBarrier({m_nextNodeCounter, m_nextLeafCounter, frontiers.clusterCounter});

        ReclaimArgs(frontiers.nodeDispatchArgs, m_nodeArgsInUse);
        ReclaimArgs(frontiers.leafDispatchArgs, m_leafArgsInUse);
        BuildNodeAndLeafDispatchArgs();
        UavToIndirect({frontiers.nodeDispatchArgs, frontiers.leafDispatchArgs});

        ExpandFrontier(m_programs.traverseNodes, m_currentNodes, m_currentNodeCounter, frontiers.nodeDispatchArgs);
        UavBarrier({m_nextNodes, m_nextNodeCounter, m_nextLeaves, m_nextLeafCounter,
            frontiers.clusters, frontiers.clusterCounter});
        ExpandFrontier(m_programs.traverseLeaves, m_currentLeaves, m_currentLeafCounter, frontiers.leafDispatchArgs);
        m_nodeArgsInUse = true;
        m_leafArgsInUse = true;

        UavToRead({m_nextNodes, m_nextNodeCounter, m_nextLeaves, m_nextLeafCounter,
            frontiers.clusters, frontiers.clusterCounter});
        UavBarrier({b.replay, b.replayState});
        CullClusterFrontier();
        ReadToUav({frontiers.clusters, frontiers.clusterCounter});
        UavBarrier({b.visibleClusters, b.visibleClustersCounter, b.replay, b.replayState});

        std::swap(m_currentNodes, m_nextNodes);
        std::swap(m_currentNodeCounter, m_nextNodeCounter);
        std::swap(m_currentLeaves, m_nextLeaves);
        std::swap(m_currentLeafCounter, m_nextLeafCounter);
    }

    void TraverseLevels()
    {
        const auto& frontiers = m_bindings.frontiers;
        UavToRead({frontiers.currentNodes, frontiers.currentNodeCounter,
            frontiers.currentLeaves, frontiers.currentLeafCounter});
        UavBarrier({m_bindings.visibleClustersCounter, m_bindings.replay, m_bindings.replayState});

        m_currentNodes = frontiers.currentNodes;
        m_currentNodeCounter = frontiers.currentNodeCounter;
        m_nextNodes = frontiers.nextNodes;
        m_nextNodeCounter = frontiers.nextNodeCounter;
        m_currentLeaves = frontiers.currentLeaves;
        m_currentLeafCounter = frontiers.currentLeafCounter;
        m_nextLeaves = frontiers.nextLeaves;
        m_nextLeafCounter = frontiers.nextLeafCounter;
        const uint32_t levels = std::min(m_frame.traversalDepth, kComputeCullingMaxTraversalLevels);
        for (uint32_t level = 0; level < levels; ++level) TraverseLevel(level);
    }

    void CreateRasterBucketCommand()
    {
        const auto& b = m_bindings;
        UavToRead({b.visibleClustersCounter, b.replayState});
        Bind(m_programs.createRasterBucketCommand);
        RootConstants constants = m_shared;
        constants[CLOD_CREATE_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = Srv(b.visibleClustersCounter);
        constants[CLOD_CREATE_RASTER_BUCKET_HISTOGRAM_COMMAND_DESCRIPTOR_INDEX] = Uav(b.rasterBucketHistogramCommand);
        constants[CLOD_CREATE_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX] = Srv(b.replayState);
        // Compute replay seeds frontiers directly; there are no work-graph node inputs to patch.
        constants[CLOD_CREATE_WORKGRAPH_NODE_INPUTS_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
        constants[CLOD_CREATE_NUM_RASTER_BUCKETS] = m_frame.rasterBucketCount;
        constants[CLOD_CREATE_VISIBLE_CLUSTERS_CAPACITY] = b.maxVisibleClusters;
        Push(constants);
        Dispatch();
    }

    const ComputeCullingBindings& m_bindings;
    const ComputeCullingFrame& m_frame;
    org::PassRecordContext& m_context;
    const ComputeCullingPrograms& m_programs;
    RootConstants m_shared{};
    bool m_nodeArgsInUse = false;
    bool m_leafArgsInUse = false;
    bool m_clusterArgsInUse = false;
    // Ping-pong frontier roles for the level being traversed.
    Token m_currentNodes, m_currentNodeCounter, m_nextNodes, m_nextNodeCounter;
    Token m_currentLeaves, m_currentLeafCounter, m_nextLeaves, m_nextLeafCounter;
};

}

void RecordComputeCulling(const ComputeCullingBindings& bindings, const ComputeCullingFrame& frame,
    org::PassRecordContext& context)
{
    if (!frame.active) return;
    ComputeCullingRecorder(bindings, frame, context).Record();
}
