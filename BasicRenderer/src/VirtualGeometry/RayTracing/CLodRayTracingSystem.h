#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Render/PipelineState.h"
#include <rhi.h>

class MeshManager;
namespace CLodCache { class OpacityMicromapFile; }
namespace org { class Buffer; }
namespace org { class RecordingContext; }

namespace br::render {

// The ray tracing scene for CLod geometry.
//
// CLAS live in one pool at their real sizes, about a quarter of the prebuild bound. A group the RT view's traversal asks
// for has its sizes measured on the GPU first, and is built and published once they come back, a few frames later. Until
// then traversal uses its coarser parent, as it does for anything not resident. A cluster's slot is its ClusterID, and
// indexes the tables of CLAS addresses and hit information.
// BLASes and the TLAS are rebuilt from that view's cluster list each frame.
//
// With opacity micromaps on, a group an alpha-tested draw asked for takes the micromaps the preprocessor baked for that
// draw's material (CLodOpacityMicromapCache.h, a .clodomm beside the mesh's container): they're uploaded, the group's
// micromap array is built, and its CLAS are measured and built as usual, all without waiting on the GPU. A group with
// none for its material is built without, and its alpha test stays in any-hit.
//
// Its GPU resources are private and not tracked by the render graph. They're created once at their limits, and the
// passes that record through this class do their own barriers.
class CLodRayTracingSystem {
public:
    struct Limits {
        uint64_t clasPoolBytes = 1024ull * 1024ull * 1024ull;
        uint32_t maxClasSlots = 1u << 20;
        // What the RT view may select. Far more than the BLASes reference, because instances with the same cut share one.
        uint32_t maxClustersInBvh = 1u << 22;
        uint32_t maxClusterReferences = 1u << 20; // Clusters across all distinct BLASes.
        uint32_t maxBlases = 1u << 16;
        uint32_t maxInstances = 1u << 18;
        uint32_t maxClustersPerInstance = 1u << 16;
        uint32_t maxClusterBuildsPerFrame = 1u << 13;
        uint32_t maxDrawRecords = 1u << 22;
        uint32_t maxGroups = 1u << 22;
        uint32_t maxSegments = 1u << 22;
        uint32_t requestCapacity = 1u << 16;
        uint32_t visibilityHashEntries = 1u << 19;
        uint32_t blasShareEntries = 1u << 17; // A power of two, at least twice the BLASes expected.
        uint64_t ommArrayPoolBytes = 256ull * 1024ull * 1024ull; // 77 MiB in use on SaveLab at radius 20.
        uint32_t ommIndexUnits = 1u << 17; // Clusters with micromap indices at once, at CLOD_RT_OMM_INDEX_UNIT_BYTES each.
        uint64_t ommUploadBytesPerFrame = 32ull << 20; // Baked micromaps and indices uploaded per frame.
        uint64_t ommScratchBytes = 64ull << 20; // For a frame's micromap array builds.
        uint32_t ommFilesCached = 1u << 13; // Open .clodomm files kept, absent ones included.
    };

    struct Stats {
        bool initialized = false;
        uint64_t clasPoolBytes = 0;
        uint64_t clasBytesUsed = 0;
        uint32_t clasSlotsUsed = 0;
        uint32_t groupsAwaitingBuild = 0;
        uint32_t cachedGroups = 0;
        uint32_t clasBuildsLastFrame = 0;
        uint64_t clasBuildsTotal = 0;
        uint64_t clasFreedTotal = 0;
        uint32_t requestsLastFrame = 0;
        uint32_t requestsDeferred = 0;
        uint32_t instances = 0;
        uint32_t distinctBlases = 0;
        uint32_t clustersInBvh = 0;
        uint32_t droppedInstances = 0;
        uint32_t droppedClusters = 0;
        uint32_t droppedBlases = 0;
        uint32_t listClusters = 0;
        uint32_t coarseConsumers = 0;
        uint64_t blasResultBytes = 0;
        uint64_t tlasBytes = 0;
        uint64_t totalMemoryBytes = 0;
        // GPU milliseconds per stage of the scene build, from the last slice to come back.
        float msClasBuild = 0.0f;
        float msCount = 0.0f;
        float msAllocate = 0.0f;
        float msPlace = 0.0f;
        float msBlasBuild = 0.0f;
        float msWriteInstances = 0.0f;
        float msTlasBuild = 0.0f;
        // Opacity micromaps.
        bool ommEnabled = false;
        uint64_t ommArrayBytesUsed = 0;
        uint32_t ommIndexUnitsUsed = 0;
        uint64_t ommGroupsLoaded = 0; // Built with their baked micromaps, since start.
        uint64_t ommGroupsWithout = 0; // Asked for by an alpha-tested draw, but nothing baked for its material.
        uint64_t ommMicromapsTotal = 0; // Micromaps uploaded, since start.
        uint64_t ommUploadBytesTotal = 0;
        uint32_t ommFilesOpen = 0;
        uint32_t alphaInstances = 0;
        uint32_t ommInstances = 0;
        uint32_t ommDisabledInstances = 0;
        std::array<uint32_t, 16> comparison{};
        bool comparisonValid = false;
    };

    struct CullingDescriptors {
        uint32_t groupTable = 0;
        uint32_t requests = 0;
        uint32_t requestCounter = 0;
        uint32_t requestCapacity = 0;
        uint32_t coarseCut = 0;
    };

    struct ScenePipelines {
        org::PipelineState clear, buildClasArgs, resetDraws, count, allocate, addCoarseConsumers, allocateFinish, place, writeInstances;
    };

    struct VisibilityDescriptors {
        uint32_t tlas = 0;
        uint32_t clasInfo = 0;
        uint32_t hash = 0;
        uint32_t hashEntries = 0;
        uint32_t stats = 0;
    };

    CLodRayTracingSystem() = default;

    // Use the preprocessor's opacity micromaps for alpha-tested groups, only for BVH users whose alpha test is
    // clodRayTracingAlpha.hlsli's. Resources exist only if this was on before EnsureInitialized, on a device that has
    // micromaps. Turned off later, existing micromaps are disabled on every instance and no new ones load.
    void RequestOpacityMicromaps(bool enabled) noexcept { m_ommRequested = enabled; }
    bool OpacityMicromapsEnabled() const noexcept { return m_ommEnabled; }
    bool OpacityMicromapsActive() const noexcept { return m_ommEnabled && m_ommRequested; }

    // Creates everything at its limits. Safe to call repeatedly; false when the device can't do clusters.
    bool EnsureInitialized(rhi::Device device, const RayTracingFeatureInfo& features);
    bool IsInitialized() const noexcept { return m_stats.initialized; }
    CullingDescriptors GetCullingDescriptors() const noexcept;
    VisibilityDescriptors GetVisibilityDescriptors() const noexcept;

    // Once per frame, before recording: takes in last frame's requests and frees what streaming evicted.
    void Refresh(const MeshManager& meshManager);

    // Records CLAS builds, BLASes and the TLAS. clusters and counter are the RT view's culling output.
    void RecordSceneBuild(org::RecordingContext& recording, const ScenePipelines& pipelines, uint32_t clustersSrv, uint32_t counterSrv, uint32_t clusterCapacity);
    void RecordVisibilityBegin(org::RecordingContext& recording, const org::PipelineState& clearPipeline, bool clearStats);
    void RecordVisibilityEnd(org::RecordingContext& recording, bool readStats);
    // After the frame's raytracing passes have recorded.
    void AdvanceFrame() noexcept;

    // Serializes Refresh with recording.
    std::mutex& FrameOperationMutex() noexcept { return m_frameOperationMutex; }
    const Stats& GetStats() const noexcept { return m_stats; }
    const Limits& GetLimits() const noexcept { return m_limits; }
    bool HasTlas() const noexcept { return m_tlasBuilt; }
    uint32_t GetTlasSrv() const noexcept { return m_tlasSrv; }
    uint32_t GetClasInfoSrv() const noexcept { return m_clasInfo.srv; }

    // Where raytraced primary visibility writes its visible cluster records.
    void SetPrimaryVisibleClusters(std::shared_ptr<org::Buffer> clusters, std::shared_ptr<org::Buffer> transformIndices, uint32_t capacity);
    std::shared_ptr<org::Buffer> GetPrimaryVisibleClusters() const { return m_primaryVisibleClusters; }
    std::shared_ptr<org::Buffer> GetPrimaryTransformIndices() const { return m_primaryTransformIndices; }
    uint32_t GetPrimaryVisibleClusterCapacity() const noexcept { return m_primaryVisibleClusterCapacity; }

private:
    struct RawBuffer {
        rhi::ResourcePtr resource;
        uint64_t bytes = 0;
        uint64_t address = 0;
        uint32_t srv = 0;
        uint32_t uav = 0;
        uint8_t* mapped = nullptr;
        rhi::ResourceHandle Handle() const { return resource->GetHandle(); }
    };
    enum class View : uint8_t { None, Raw, Structured };
    bool CreateBuffer(RawBuffer& out, uint64_t bytes, rhi::HeapType heap, rhi::ResourceFlags flags, const char* name, View view = View::None, uint32_t stride = 0);

    // Contiguous ranges of a fixed-size space.
    class RangeAllocator {
    public:
        void Reset(uint32_t total);
        bool Allocate(uint32_t count, uint32_t& outFirst);
        void Free(uint32_t first, uint32_t count);
        uint32_t Used() const noexcept { return m_used; }
    private:
        std::map<uint32_t, uint32_t> m_free; // first -> count, coalesced.
        uint32_t m_used = 0;
    };

    struct Run { uint32_t first = 0; uint32_t count = 0; };
    struct CachedGroup {
        std::shared_ptr<const void> owner;
        uint32_t localIndex = 0;
        uint32_t firstPageId = 0;
        uint32_t firstSegment = 0;
        uint32_t segmentCount = 0;
        std::vector<Run> slots;
        Run memory; // In units of the CLAS alignment. Empty until built.
        Run ommArray; // In units of the micromap array alignment.
        Run ommIndices; // One unit per cluster.
        uint64_t id = 0;
        bool sizesKnown = false;
    };
    struct alignas(8) BuildSource {
        uint64_t slabAddress = 0;
        uint32_t pageByteOffset = 0;
        uint32_t slabDescriptor = 0;
        uint32_t pageOffsetUnits = 0;
        uint32_t firstMeshlet = 0;
        uint32_t meshletCount = 0;
        uint32_t firstBuildIndex = 0;
        uint32_t firstClasSlot = 0;
        uint32_t groupLocalIndex = 0;
        uint32_t ommKey = 0; // The material identity the micromaps were baked for, when the source's CLAS reference some.
        uint32_t ommIndexUnit = 0;
        uint64_t ommArray = 0;
        uint32_t padding[2]{};
    };
    static_assert(sizeof(BuildSource) == 64);
    struct Segment {
        uint32_t slabDescriptor = 0;
        uint32_t pageOffsetUnits = 0;
        uint32_t firstMeshlet = 0;
        uint32_t meshletCount = 0;
        uint32_t firstClasSlot = 0;
        uint32_t padding = 0;
    };
    static_assert(sizeof(Segment) == 24);
    struct GroupUpdate { uint32_t index = 0; uint32_t first = 0; uint32_t count = 0; };
    struct SegmentUpdate { uint32_t index = 0; Segment value{}; };
    // Measured, and waiting for memory and this frame's build budget.
    struct AwaitingBuild {
        uint32_t groupGlobal = 0;
        uint64_t id = 0;
        std::vector<BuildSource> sources;
        std::vector<Segment> segments;
        std::vector<uint32_t> sizes; // Per cluster, in the order of sources.
    };
    struct MeasureRequest {
        uint32_t groupGlobal = 0;
        uint64_t id = 0;
        uint32_t clusterCount = 0;
        uint32_t firstMeasureIndex = 0;
        std::vector<BuildSource> sources;
        std::vector<Segment> segments;
    };
    // A group's micromap array, built this frame from its baked data in the slice's upload.
    struct OmmArrayBuild {
        uint64_t destination = 0;
        uint64_t scratchOffset = 0;
        uint64_t descsOffset = 0; // In the micromap upload buffer.
        uint64_t dataOffset = 0;
        uint32_t count = 0;
        std::array<uint32_t, 13> levelCounts{};
    };
    // A group's indices, copied from the slice's upload into the index pool.
    struct OmmIndexCopy {
        uint64_t uploadOffset = 0;
        uint32_t firstUnit = 0;
        uint32_t units = 0;
    };
    struct UploadSlice {
        std::vector<BuildSource> sources; // What's being built this frame.
        std::vector<uint64_t> destinations;
        std::vector<BuildSource> measureSources;
        std::vector<MeasureRequest> measuring; // What the size readback of this slice will answer.
        uint32_t measureCount = 0;
        std::vector<GroupUpdate> groupUpdates;
        std::vector<SegmentUpdate> segmentUpdates;
        uint32_t buildCount = 0;
        std::vector<OmmArrayBuild> ommArrayBuilds;
        std::vector<OmmIndexCopy> ommIndexCopies;
        uint64_t ommUploadBytes = 0; // Used of the slice's micromap upload.
        uint64_t ommScratchBytes = 0;
    };

    void FreeGroup(uint32_t groupGlobal, CachedGroup& group);
    bool MeasureGroup(const MeshManager& meshManager, uint32_t groupGlobal, uint32_t ommIdentity, uint32_t& measuresLeft);
    // Uploads a group's baked micromaps into this slice and points its sources at them. False to try again next frame;
    // a group with nothing usable gets none and true.
    bool LoadOmmGroup(const std::wstring& containerPath, uint32_t groupLocalIndex, uint32_t ommIdentity, uint32_t groupGlobal,
        std::vector<BuildSource>& sources, Run& outIndices, Run& outArray);
    std::shared_ptr<const CLodCache::OpacityMicromapFile> OmmFile(const std::wstring& containerPath, uint32_t ommIdentity);
    bool OmmArraySize(const std::array<uint32_t, 13>& levelCounts, uint64_t& resultBytes, uint64_t& scratchBytes);
    void RecordOpacityMicromaps(org::RecordingContext& recording, const UploadSlice& slice, uint32_t sliceIndex);
    bool BuildMeasuredGroup(const MeshManager& meshManager, AwaitingBuild& awaiting, uint32_t& buildsLeft);
    bool EvictOldest(uint32_t except);
    void ReadBack(uint32_t slice);
    uint64_t SliceOffset(uint64_t sliceBytes, uint32_t slice) const noexcept { return sliceBytes * slice; }

    std::mutex m_frameOperationMutex;
    rhi::Device m_device{};
    Limits m_limits{};
    Stats m_stats{};
    uint32_t m_framesInFlight = 3;
    uint32_t m_frame = 0;
    bool m_tablesCleared = false;
    bool m_tlasBuilt = false;
    uint32_t m_clusterInfoStride = 0;

    RawBuffer m_clasPool, m_clasInfo, m_clasAddresses, m_groupTable, m_segmentTable;
    RawBuffer m_buildArgs, m_buildDestinations, m_buildScratch, m_clasSizes, m_clasSizesReadback;
    RawBuffer m_requests, m_requestCounter;
    RawBuffer m_coarseCut;
    RawBuffer m_drawTable, m_instanceList, m_instanceBlas, m_shareTable, m_counters;
    RawBuffer m_blasArgs, m_blasAddressArray, m_blasResult, m_blasScratch, m_blasAddresses, m_blasSizes;
    RawBuffer m_tlasInstances, m_tlasStorage, m_tlasScratch;
    RawBuffer m_visibilityHash, m_visibilityStats;
    RawBuffer m_upload, m_readback;
    RawBuffer m_ommArrayPool, m_ommIndexPool, m_ommScratch, m_ommUpload;
    RawBuffer m_slotOmmKeys, m_blasOmmKeys;
    uint64_t m_uploadSliceBytes = 0;
    uint64_t m_readbackSliceBytes = 0;

    rhi::AccelerationStructurePtr m_tlas;
    uint32_t m_tlasSrv = 0;
    uint32_t m_tlasInstanceCount = 256;
    // Sizes the grouping dispatches from last frame's list, so threads don't load a counter just to exit.
    uint32_t m_listClusterEstimate = 1u << 20;
    rhi::QueryPoolPtr m_timers;
    uint64_t m_timerTicksPerSecond = 0;

    bool m_ommRequested = false;
    bool m_ommEnabled = false;
    RangeAllocator m_ommArrayAllocator;
    RangeAllocator m_ommIndexAllocator;
    std::map<std::array<uint32_t, 13>, std::pair<uint64_t, uint64_t>> m_ommArraySizes; // Histogram -> result and scratch bytes.
    // .clodomm files by path, opened on first use; null for a mesh with nothing baked for that material.
    std::unordered_map<std::wstring, std::shared_ptr<const CLodCache::OpacityMicromapFile>> m_ommFiles;

    RangeAllocator m_slotAllocator;
    RangeAllocator m_memoryAllocator;
    std::deque<AwaitingBuild> m_awaitingBuild;
    uint64_t m_nextGroupId = 1;
    RangeAllocator m_segmentAllocator;
    std::unordered_map<uint32_t, CachedGroup> m_groups;
    std::deque<uint32_t> m_buildOrder;
    std::vector<uint32_t> m_validationQueue;
    size_t m_validationCursor = 0;
    std::vector<UploadSlice> m_slices;
    std::vector<uint8_t> m_sliceHasReadback;
    std::vector<uint8_t> m_sliceHasStats;

    std::shared_ptr<org::Buffer> m_primaryVisibleClusters;
    std::shared_ptr<org::Buffer> m_primaryTransformIndices;
    uint32_t m_primaryVisibleClusterCapacity = 0;
};

} // namespace br::render
