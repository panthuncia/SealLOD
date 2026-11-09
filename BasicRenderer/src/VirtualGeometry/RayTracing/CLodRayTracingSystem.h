#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <vector>

#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "Render/ResourceUseDeclaration.h"
#include "Render/PipelineState.h"
#include <BasicRenderer/Streaming/TaskScheduler.h>
#include <rhi.h>

class MeshManager;
class PagePool;
namespace CLodCache { class OpacityMicromapFile; }
namespace org { class Buffer; class RecordingContext; class RenderGraph; class RenderPassBuilder; class ResourceGroup; }

namespace br::render {

// The ray tracing scene for CLod geometry.
//
// The CLAS cache is kept by the GPU, within the frame (clodRayTracingCache.hlsli):
// - The RT view's culling uses a resident group with a CLAS built from where its pages are now, and marks it. Any other
//   it admits within the frame's build budget, and uses at once, if it also gets either the cache room it can need at
//   most (reserved from what the free queues can give this frame) or a share of the transient budget. Otherwise the
//   coarser parent stands in, as for anything not resident.
// - The scene pass builds what was admitted into the one build arena. Groups that get an entry, CLAS ID chunks and pages
//   are moved into their pages and cached; the rest are transient: moved into this frame's transient arena, used from
//   there this frame, and built again while they are needed and don't fit. So running out of cache space costs builds,
//   never a hole or a stall, and a burst can build the whole build budget while the transient arenas stay small.
// - Pages, chunks and entries come from FIFO queues of free items, which give out an item only once the frames that
//   might trace it are done. Space is freed only while it runs short, from groups idle for the keep age, and never from
//   one used in the protected frames.
// The CPU reads statistics back, tells the GPU where page slabs are when the page pool adds one, and loads opacity
// micromaps: the GPU lists alpha-tested groups it built without the ones baked for the asking draw's material, the CPU
// opens their .clodomm through blocking IO and uploads them, and the GPU builds those groups again with them. A group
// keeps its CLAS until the new ones are packed. When a micromap pool runs short, the GPU retires the micromaps of idle
// groups, with the CLAS built from them, and the CPU frees them once that frame is back.
//
// A cluster's CLAS ID (slot) indexes the tables of CLAS addresses and hit information. BLASes and the TLAS are rebuilt
// from the RT view's cluster list each frame.
//
// Its buffers are render graph buffers, created once at their limits. The persistent ones keep their memory, address and
// descriptors for the system's life (the GPU stores CLAS and BLAS addresses into them); the ones rebuilt each frame are
// placed by the graph. Every pass that touches one declares it (the Declare* functions here), so the graph orders those
// passes and makes every barrier between them. Within a pass, the recording functions sync only between their own
// dispatches and acceleration structure operations, and a buffer the pass leaves in another state than it declared has
// that state declared as an internal transition.
class CLodRayTracingSystem {
public:
    struct Limits {
        // The cache, in 8 KiB pages: its one size setting. Zero sizes it from the device's memory budget, a sixth of it
        // within [768 MiB, 2 GiB]: an r100 flight's working set is about 730 MiB, Zorah's 1.6 GiB. SARP_RT_CLAS_POOL_MB
        // overrides it.
        uint64_t clasPoolBytes = 0;
        uint32_t maxClasSlots = 1u << 21; // Cached CLAS IDs, in chunks of 32.
        uint32_t maxEntries = 1u << 17; // Cached groups; r100 caches about 46k.
        // Sized from SaveLab peaks (docs/merge_hierarchies.md, "VRAM"); overflow drops work and the stats log counts it.
        // What the RT view may select. Far more than the BLASes reference, because instances with the same cut share one.
        // It also sizes the RT view's traversal frontiers and visible cluster buffers (36 B per cluster in all). Merged
        // scenes peak at 0.58M (r100), Zorah at 2.8M; an unmerged r100 scene needs 4.3M: SARP_RT_MAX_LIST_CLUSTERS overrides it.
        uint32_t maxClustersInBvh = 1u << 22;
        // Clusters across all distinct BLASes: about 160 B each with the BLAS batch and its scratch. Peak 229k (r100),
        // 770k (Zorah); past it BLASes are truncated. SARP_RT_MAX_CLUSTER_REFERENCES overrides it.
        uint32_t maxClusterReferences = 1u << 20;
        uint32_t maxBlases = 1u << 15; // Peak 16k.
        // TLAS rows: 330 B each with the TLAS and its scratch. Peak 324k merged (r100); an unmerged r100 scene needs 552k:
        // SARP_RT_MAX_INSTANCES overrides it.
        uint32_t maxInstances = 1u << 19;
        uint32_t maxClustersPerInstance = 1u << 16;
        // Clusters built per frame at most; it sizes the one build arena, at 8.4 KiB each. SARP_RT_BUILD_BUDGET overrides it.
        uint32_t clusterBuildBudget = 1u << 14;
        // Sizes each frame in flight's transient arena, which holds this frame's groups that don't fit the cache, by real
        // size: this many clusters at the largest a cluster can be. SARP_RT_TRANSIENT_BUDGET overrides it.
        uint32_t transientBuildBudget = 1u << 11;
        uint32_t maxDrawRecords = 1u << 21; // Draw record indices; the draw buffer holds 512k at r100.
        // Instance keys for part portal occurrences, one per assembly transform (docs/merge_hierarchies.md).
        uint32_t maxPortalOccurrences = 1u << 20;
        uint32_t maxGroups = 1u << 21; // Global group indices the cache can hold; r100 reaches 0.3M.
        uint32_t requestCapacity = 1u << 16;
        uint32_t visibilityHashEntries = 1u << 19;
        uint32_t blasShareEntries = 1u << 17; // A power of two, at least twice the BLASes expected.
        uint32_t slabAddressEntries = 1u << 20; // Slab descriptor indices the slab address table covers.
        uint32_t protectedFrames = 2u; // Frames that may still trace what a frame frees; at least the frames in flight.
        uint32_t maxKeepAge = 600u; // Frames a group the RT view stopped using is kept when space runs short, at most.
        // SARP_RT_OMM_POOL_MB overrides it; the index pool is scaled with it.
        uint64_t ommArrayPoolBytes = 128ull * 1024ull * 1024ull; // 27 MiB in use after an r100 flight.
        uint32_t ommIndexUnits = 1u << 17; // Clusters with micromap indices, at CLOD_RT_OMM_INDEX_UNIT_BYTES each; 26k at r100.
        uint64_t ommUploadBytesPerFrame = 32ull << 20; // Baked micromaps and indices uploaded per frame.
        uint64_t ommScratchBytes = 64ull << 20; // For a frame's micromap array builds.
        uint32_t ommRebuildsPerFrame = 1024u; // Cached groups built again with micromaps per frame.
        uint32_t ommTableWritesPerFrame = 4096u;
        uint32_t ommRetryCapacity = 1u << 14; // Loads waiting for micromap pool room.
        uint32_t ommIdleFiles = 64u; // Opened .clodomm files kept with no loaded group.
    };

    struct Stats {
        bool initialized = false;
        uint64_t clasPoolBytes = 0;
        uint64_t clasBytesUsed = 0;
        uint32_t cachedGroups = 0;
        uint32_t clasBuildsLastFrame = 0;
        uint32_t rejectedClasArgs = 0; // Cumulative: clusters built empty because their arguments were invalid.
        uint64_t clasBuildsTotal = 0;
        uint64_t clasFreedTotal = 0; // Clusters evicted or replaced.
        // The cache, per frame: groups the list needed without a current CLAS (requests), built, cached, transient (and
        // their clusters), that went transient for lack of room, and evicted (stale among them). Groups with no CLAS this
        // frame: over the build budget (not built), or built with no room in the cache or the transient arena (discarded).
        uint32_t requestsLastFrame = 0;
        uint32_t admittedLastFrame = 0;
        uint32_t persistentLastFrame = 0;
        uint32_t transientLastFrame = 0;
        uint32_t transientClustersLastFrame = 0;
        uint32_t refusedLastFrame = 0;
        uint32_t evictedLastFrame = 0;
        uint32_t staleLastFrame = 0;
        uint32_t notBuiltLastFrame = 0;
        uint32_t discardedLastFrame = 0;
        uint32_t discardedClustersLastFrame = 0;
        uint64_t evictedTotal = 0;
        uint64_t transientClustersTotal = 0;
        uint64_t notBuiltTotal = 0;
        uint64_t discardedTotal = 0;
        // Clusters of the list placed without a CLAS, as null references: missing from the BVH that frame.
        uint32_t missingClustersLastFrame = 0;
        uint64_t missingClustersTotal = 0;
        uint32_t keepAge = 0;
        uint32_t cachedPages = 0;
        uint32_t cachedClusters = 0;
        float packingWaste = 0.0f; // The share of cached pages' space unused.
        uint32_t oversizeClusters = 0; // Cumulative: CLAS larger than a page, which stay transient.
        // Free, including what still waits out the frames in flight.
        uint32_t freePages = 0;
        uint32_t freeChunks = 0;
        uint32_t freeEntries = 0;
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
        float msCount = 0.0f; // Grouping the list by instance, and recording what it needs from the cache.
        float msClasBuild = 0.0f; // The cache's passes and the CLAS build.
        float msAllocate = 0.0f;
        float msPlace = 0.0f;
        float msBlasBuild = 0.0f;
        float msWriteInstances = 0.0f;
        float msTlasBuild = 0.0f;
        // Opacity micromaps.
        bool ommEnabled = false;
        uint64_t ommArrayBytesUsed = 0;
        uint32_t ommIndexUnitsUsed = 0;
        uint64_t ommGroupsLoaded = 0; // Loaded with their baked micromaps, since start.
        uint64_t ommGroupsWithout = 0; // Asked for, but nothing baked for the material, or no room.
        // Why, by OmmLoad::NoneReason: no pages or container, no file, the group not in it, a cluster count that doesn't
        // match, too large to load.
        std::array<uint64_t, 5> ommWithoutReasons{};
        uint64_t ommMicromapsTotal = 0; // Micromaps uploaded, since start.
        uint64_t ommUploadBytesTotal = 0;
        uint32_t ommFilesOpen = 0;
        uint32_t ommPending = 0; // Opening or waiting for upload room.
        uint32_t ommRetries = 0; // Waiting for micromap pool room.
        uint64_t ommReleasedTotal = 0; // Retired under pressure and freed, since start.
        bool ommPressure = false;
        uint32_t rebuiltLastFrame = 0; // Cached groups built again with micromaps.
        uint32_t alphaInstances = 0;
        uint32_t ommInstances = 0;
        uint32_t ommDisabledInstances = 0;
        std::array<uint32_t, 16> comparison{};
        bool comparisonValid = false;
    };

    struct ScenePipelines {
        org::PipelineState clear, resetDraws, count, allocate, addCoarseConsumers, allocateFinish, place, writeInstances;
        org::PipelineState cacheInit, cacheCollect, cacheOmmRetire, cacheEvict, cacheAdmit, cacheAssign, cacheArgs, cachePack, cacheEnd;
    };

    struct VisibilityDescriptors {
        uint32_t tlas = 0;
        uint32_t clasInfo = 0;
        uint32_t instanceRows = 0;
        uint32_t hash = 0;
        uint32_t hashEntries = 0;
        uint32_t stats = 0;
    };

    CLodRayTracingSystem(); // Reads the limit overrides from the environment.
    CLodRayTracingSystem(const CLodRayTracingSystem&) = delete;
    CLodRayTracingSystem& operator=(const CLodRayTracingSystem&) = delete;

    // Use the preprocessor's opacity micromaps for alpha-tested groups, only for BVH users whose alpha test is
    // clodRayTracingAlpha.hlsli's. Resources exist only if this was on before EnsureInitialized, on a device that has
    // micromaps. Turned off later, existing micromaps are disabled on every instance and no new ones load.
    void RequestOpacityMicromaps(bool enabled) noexcept { m_ommRequested = enabled; }
    bool OpacityMicromapsEnabled() const noexcept { return m_ommEnabled; }
    bool OpacityMicromapsActive() const noexcept { return m_ommEnabled && m_ommRequested; }

    // Creates everything at its limits. Safe to call repeatedly; false when the device can't do clusters.
    bool EnsureInitialized(rhi::Device device, const RayTracingFeatureInfo& features);
    bool IsInitialized() const noexcept { return m_stats.initialized; }
    VisibilityDescriptors GetVisibilityDescriptors() const noexcept;

    // ---- Render graph declarations, one per kind of pass that uses this system's buffers.
    // What passes that trace the scene read: the TLAS, and through it the BLAS batch, the CLAS pool and micromap arrays,
    // plus the CLAS info and instance rows hits read. By name, for passes made without the system.
    static void DeclareTracing(org::RenderPassBuilder& builder);
    // The names DeclareTracing declares. Once the graph exists, before its passes are declared.
    void RegisterGraphResources(org::RenderGraph& graph);
    // The scene build's passes, in this order.
    void DeclareSceneClas(org::RenderPassBuilder& builder) const;
    void DeclareSceneBlas(org::RenderPassBuilder& builder) const;
    // Returns the TLAS rows' view: a placed buffer, whose descriptor is the recording's to give.
    org::DeclaredViewToken DeclareSceneTlas(org::RenderPassBuilder& builder) const;
    // Raytraced visibility, which clears the hash (and the comparison, which it then reads back when readStats).
    void DeclareVisibility(org::RenderPassBuilder& builder, bool readStats) const;
    // The path trace pass's pixel watch, when it's on.
    void DeclarePixelWatch(org::RenderPassBuilder& builder) const;
    // The RT view's culling writes which coarse clusters stand in for others; the BLAS pass reads it, the TLAS pass clears it.
    // The culling pass declares it as unordered access.
    std::shared_ptr<org::Buffer> GetCoarseCut() const;
    uint32_t GetCoarseCutUav() const noexcept { return m_coarseCut.uav; }

    // Once per frame, before recording and after AdvanceFrame: reads back statistics, and records new page slabs in the
    // slab address table. Constant time apart from new slabs.
    void BeginFrame(MeshManager& meshManager);
    // Waits for micromap file opens. Before the mesh manager goes away.
    void Shutdown();
    ~CLodRayTracingSystem();

    // The scene build, in three passes ordered by what they write and read. Each stage's scratch is graph memory live only
    // in its pass, so the stages share it. clusters and counter are the RT view's culling output; a stage that doesn't read
    // an input leaves it 0.
    struct SceneInputs {
        uint32_t clustersSrv = 0, sideBandSrv = 0, counterSrv = 0, clusterCapacity = 0, tlasInstancesUav = 0;
    };
    // Counts what the cluster list needs and keeps the CLAS cache: builds into the arena, then moves into the pool.
    void RecordSceneClas(org::RecordingContext& recording, const ScenePipelines& pipelines, const SceneInputs& inputs);
    // Groups clusters by instance and builds the BLASes into the BLAS data.
    void RecordSceneBlas(org::RecordingContext& recording, const ScenePipelines& pipelines, const SceneInputs& inputs);
    // Writes the instance rows, builds the TLAS, and copies out what the CPU reads back.
    void RecordSceneTlas(org::RecordingContext& recording, const ScenePipelines& pipelines, const SceneInputs& inputs);
    void RecordVisibilityBegin(org::RecordingContext& recording, const org::PipelineState& clearPipeline, bool clearStats);
    void RecordVisibilityEnd(org::RecordingContext& recording, bool readStats);
    // Pixel watch (SARP_RT_PIXEL_WATCH=<luminance>): the path trace pass clears it before tracing and reads it back after.
    // GetPixelWatchUav is CLOD_RT_PT_NO_DESCRIPTOR when it's off.
    uint32_t GetPixelWatchUav() const noexcept;
    float GetPixelWatchBright() const noexcept { return m_watchBright; }
    void RecordPixelWatchBegin(org::RecordingContext& recording, const org::PipelineState& clearPipeline);
    void RecordPixelWatchEnd(org::RecordingContext& recording);
    // At the start of each frame, before BeginFrame.
    void AdvanceFrame() noexcept;

    // Serializes BeginFrame with recording.
    std::mutex& FrameOperationMutex() noexcept { return m_frameOperationMutex; }
    const Stats& GetStats() const noexcept { return m_stats; }
    const Limits& GetLimits() const noexcept { return m_limits; }
    bool HasTlas() const noexcept { return m_tlasBuilt; }
    uint32_t GetTlasSrv() const noexcept { return m_tlasSrv; }
    uint32_t GetClasInfoSrv() const noexcept { return m_clasInfo.srv; }
    // Per TLAS row (InstanceIndex()): uint4(draw, assembly transform, a cluster's group, 0). See CLOD_RT_INSTANCE_ID_PORTAL.
    uint32_t GetInstanceRowsSrv() const noexcept { return m_instanceRows.srv; }

    // Where raytraced primary visibility writes its visible cluster records.
    void SetPrimaryVisibleClusters(std::shared_ptr<org::Buffer> clusters, std::shared_ptr<org::Buffer> transformIndices, uint32_t capacity);
    std::shared_ptr<org::Buffer> GetPrimaryVisibleClusters() const { return m_primaryVisibleClusters; }
    std::shared_ptr<org::Buffer> GetPrimaryTransformIndices() const { return m_primaryTransformIndices; }
    uint32_t GetPrimaryVisibleClusterCapacity() const noexcept { return m_primaryVisibleClusterCapacity; }

private:
    // A persistent render graph buffer, with what the recording needs of it: its views' descriptor indices, its address,
    // and its mapping for host-visible heaps. All fixed for the system's life.
    struct GraphBuffer {
        std::shared_ptr<org::Buffer> buffer;
        uint64_t bytes = 0;
        uint64_t address = 0;
        uint32_t srv = 0;
        uint32_t uav = 0;
        uint8_t* mapped = nullptr;
        rhi::ResourceHandle Handle() const;
        explicit operator bool() const noexcept { return static_cast<bool>(buffer); }
    };
    // Every buffer CreateBuffer made: name, bytes and heap, for the memory log.
    std::vector<std::tuple<std::string, uint64_t, rhi::HeapType>> m_allocations;
    enum class View : uint8_t { None, Raw, Structured };
    // High: over budget, the OS demotes what it likes, and the per-instance reads here get about twenty times slower.
    bool CreateBuffer(GraphBuffer& out, uint64_t bytes, rhi::HeapType heap, rhi::ResourceFlags flags, const char* name, View view = View::None, uint32_t stride = 0,
        bool highPriority = false);
    void RecordCache(org::RecordingContext& recording, const ScenePipelines& pipelines);
    MiscConstants CacheConstants() const;
    MiscConstants SceneConstants(const SceneInputs& inputs) const;
    void RecordSceneClear(org::RecordingContext& recording, const ScenePipelines& pipelines, const GraphBuffer& buffer, uint64_t bytes);
    void SceneStamp(rhi::CommandList& commands, uint32_t index);
    uint32_t ArenaSlot() const noexcept { return m_frame % m_transientSlices; }
    void LogStats();

    // ---- Opacity micromaps (CPU side).
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
    // A group's baked micromaps, read off the IO thread.
    struct OmmLoad {
        uint32_t group = 0;
        uint32_t identity = 0;
        bool none = true; // Nothing baked for this material, or it doesn't match the group.
        enum NoneReason : uint8_t { NoContainer, NoFile, NoGroup, ClusterMismatch, TooLarge };
        NoneReason noneReason = NoContainer;
        uint32_t clusters = 0;
        uint32_t descCount = 0;
        uint32_t localIndex = 0;
        std::array<uint32_t, 13> levelCounts{};
        std::vector<std::byte> indices, descs, data;
        std::wstring path; // The .clodomm, for its reference count.
        std::wstring containerPath;
    };
    // What a table slot holds, for freeing it.
    struct OmmSlotAllocation {
        uint32_t firstUnit = 0, units = 0;
        uint32_t arrayFirst = 0, arrayUnits = 0; // arrayUnits 0: no array.
        std::wstring path; // Empty: a "none" entry.
    };
    // A group to load again once the pools have room.
    struct OmmRetry {
        uint32_t group = 0, identity = 0, clusters = 0, localIndex = 0;
        std::wstring containerPath;
    };
    struct OmmArrayBuild {
        uint64_t destination = 0;
        uint64_t scratchOffset = 0;
        uint64_t descsOffset = 0; // In the micromap upload buffer.
        uint64_t dataOffset = 0;
        uint32_t count = 0;
        std::array<uint32_t, 13> levelCounts{};
    };
    struct OmmIndexCopy { uint64_t uploadOffset = 0; uint32_t firstUnit = 0; uint32_t units = 0; };
    // Requests the GPU made, opened through blocking IO.
    void RequestOmmLoads(MeshManager& meshManager, const uint32_t* wanted, uint32_t count);
    void SubmitOmmOpen(uint32_t group, uint32_t identity, uint32_t clusters, uint32_t localIndex, const std::wstring& containerPath);
    // What the GPU retired, from a readback slice.
    void ReleaseOmmEntries(const uint32_t* released, uint32_t count);
    void ReleaseOmmFile(const std::wstring& path);
    // Loaded micromaps into this frame's upload, table and rebuild list.
    void StageOmmLoads();
    bool InsertOmmEntry(uint32_t group, const std::array<uint32_t, 4>& entry);
    // The micromap array and index pools (192 MiB at the defaults), made when the first group with micromaps loads: a
    // scene baked without them never pays for them. Their scratch is placed by the graph.
    bool EnsureOmmPools();
    bool OmmArraySize(const std::array<uint32_t, 13>& levelCounts, uint64_t& resultBytes, uint64_t& scratchBytes);
    void RecordOmmUploads(rhi::CommandList& commands);
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

    GraphBuffer m_clasPool, m_clasInfo, m_clasAddresses, m_slotOmmKeys, m_blasOmmKeys;
    // The cache (clodRayTracingCache.hlsli).
    GraphBuffer m_groupState, m_cacheCtrl, m_cacheEntries, m_pageNext, m_pageRefs, m_cacheQueues, m_transient, m_cacheRanges, m_cachePending;
    GraphBuffer m_buildArgs, m_arenaAddresses, m_arenaSizes, m_moveArgs, m_moveDestinations, m_packScratch;
    // The build arena: every frame's builds land here, and are moved into pages or into that frame's transient arena,
    // a slice of m_clasPool past its pages, one per frame that may still be tracing it. All within the CLAS pass, so the
    // graph places it, like the scratch buffers.
    std::shared_ptr<org::Buffer> m_buildArena;
    uint32_t m_transientSlices = 1;
    uint64_t m_transientSliceBytes = 0;
    // Device addresses by slab descriptor index, in an upload heap: written as slabs appear, and slabs are only ever
    // added, so no frame the GPU still runs reads an entry that changes.
    GraphBuffer m_slabAddresses;
    GraphBuffer m_requests, m_demand; // This frame's requests, collected from the demand bitset (CLOD_RT_DEMAND_*).
    GraphBuffer m_coarseCut;
    GraphBuffer m_drawTable, m_instanceList, m_instanceRows, m_instanceBlas, m_shareTable, m_counters;
    GraphBuffer m_blasArgs, m_blasAddressArray, m_blasAddresses, m_blasSizes;
    GraphBuffer m_tlasStorage;
    // Rebuilt every frame, so placed by the graph: the BLAS batch, from the BLAS pass to the last pass that traces
    // (Builtin::RayTracing::BlasData), and the TLAS rows, within the TLAS pass. The TLAS itself (m_tlasStorage) is
    // persistent: its view holds its address.
    std::shared_ptr<org::Buffer> m_blasResult, m_tlasInstances;
    // Scratch for the CLAS builds and moves, the BLAS batch, the TLAS and micromap arrays, each live only in its pass, so
    // the graph shares their memory with whatever else isn't live then (the RT view's culling frontiers, in path tracing).
    std::shared_ptr<org::Buffer> m_buildScratch, m_blasScratch, m_tlasScratch, m_ommScratch;
    GraphBuffer m_visibilityHash, m_visibilityStats;
    GraphBuffer m_pixelWatch;
    // Per frame in flight, a slice: the scene build's (counters, cache control, timers, micromap requests and releases),
    // the visibility comparison's, the pixel watch's. One buffer per pass that writes it.
    GraphBuffer m_readback, m_statsReadback, m_watchReadback;
    uint64_t m_readbackSliceBytes = 0;
    uint64_t m_arenaBytes = 0; // The build arena's size.
    uint32_t m_pageCount = 0;
    uint32_t m_chunkCount = 0;
    uint32_t m_protectedFrames = 2;
    uint32_t m_knownSlabs = 0; // Slabs already in the slab address table.

    rhi::AccelerationStructurePtr m_tlas;
    uint32_t m_tlasSrv = 0;
    // Every row, every frame: sized from a count read back frames ago, a load that added instances faster than that left
    // them out of the TLAS until it caught up. Rows past this frame's instances are inactive.
    uint32_t m_tlasInstanceCount = 0;
    rhi::QueryPoolPtr m_timers;
    uint64_t m_timerTicksPerSecond = 0;

    bool m_ommRequested = false;
    bool m_ommEnabled = false;
    GraphBuffer m_ommArrayPool, m_ommIndexPool, m_ommUpload, m_ommTable, m_ommWanted, m_ommRebuild, m_ommRelease;
    // The pools are made on first use, so passes declare them through these, empty until then.
    std::shared_ptr<org::ResourceGroup> m_ommArrays, m_ommIndices;
    RangeAllocator m_ommArrayAllocator, m_ommIndexAllocator;
    std::map<std::array<uint32_t, 13>, std::pair<uint64_t, uint64_t>> m_ommArraySizes; // Histogram -> result and scratch bytes.
    // The GPU table's contents. This side inserts; the GPU only turns entries into tombstones, which this side mirrors
    // when it reads the release back, and only then reuses.
    std::vector<std::array<uint32_t, 4>> m_ommTableCpu;
    std::vector<OmmSlotAllocation> m_ommSlotAllocations;
    std::unordered_set<uint32_t> m_ommKnown; // Groups with a table entry, or loading, or waiting to retry.
    std::vector<OmmRetry> m_ommRetries;
    bool m_ommFreedSinceRetry = false;
    uint32_t m_ommInsertedSlot = 0; // InsertOmmEntry's last slot.
    bool m_ommPressure = false;
    uint64_t m_ommUploadSliceBytes = 0;
    // This frame's staging, recorded at the start of its scene build.
    std::vector<OmmArrayBuild> m_ommBuilds;
    std::vector<OmmIndexCopy> m_ommCopies;
    std::vector<uint32_t> m_ommTableWrites; // Table slots.
    std::vector<uint32_t> m_ommRebuilds;
    uint64_t m_ommUploadBytes = 0;
    uint64_t m_ommScratchUsed = 0;
    bool m_ommPoolsReady = false;
    bool m_ommPoolsFailed = false;
    bool m_ommStaged = false; // This frame has staging to record (BeginFrame ran for it).
    // Shared with the IO threads.
    TaskScope m_ommScope;
    std::mutex m_ommMutex;
    // Opened files: null for none. A file with no loaded group is closed once more than ommIdleFiles are idle.
    struct OmmFile {
        std::shared_ptr<const CLodCache::OpacityMicromapFile> file;
        uint32_t entries = 0; // Loaded table entries from it, and loads in flight.
        uint64_t idleSince = 0;
    };
    std::unordered_map<std::wstring, OmmFile> m_ommFiles;
    uint64_t m_ommFileClock = 0;
    std::vector<OmmLoad> m_ommLoaded;
    uint32_t m_ommOpening = 0;
    std::vector<OmmLoad> m_ommWaiting; // Loaded, no upload room yet this frame.

    // Per frame in flight: the readback slice the scene build writes and BeginFrame reads.
    std::vector<uint8_t> m_sliceHasReadback;
    std::vector<uint8_t> m_sliceHasStats;
    std::vector<uint8_t> m_sliceHasWatch;
    float m_watchBright = 0.0f; // Zero when the watch is off.
    std::array<uint64_t, 16> m_watchTotals{};
    std::array<uint64_t, 16> m_watchLogged{};
    std::chrono::steady_clock::time_point m_watchLastLog{};
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> m_watchCsv{ nullptr, &std::fclose };
    void ReadPixelWatch(const uint32_t* words);
    // SARP_RT_FRAME_CSV: one row of cache statistics per frame read back, for analysis. Off unless set.
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> m_frameCsv{ nullptr, &std::fclose };
    uint64_t m_frameCsvStartTicks = 0;

    std::shared_ptr<org::Buffer> m_primaryVisibleClusters;
    std::shared_ptr<org::Buffer> m_primaryTransformIndices;
    uint32_t m_primaryVisibleClusterCapacity = 0;
};

} // namespace br::render
