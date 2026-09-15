#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ShaderBuffers.h"
#include "Mesh/Mesh.h"
#include "Import/CLodCache.h"
#include "Managers/Singletons/DirectStorageManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "RenderPasses/Base/PassReturn.h"
#include "Render/AsyncStateGraph.h"
#include "Resources/Buffers/LazyDynamicStructuredBuffer.h"
#include "Resources/Buffers/PagePool.h"
#include "Interfaces/IResourceProvider.h"

class Mesh;
class Skeleton;
class SkeletonManager;
class MeshInstance;
class Material;
namespace org { class DynamicBuffer; }
using org::DynamicBuffer;
namespace org { class ResourceGroup; }
using org::ResourceGroup;
namespace org { class BufferView; }
using org::BufferView;
class ViewManager;
class ICLodGeometryStorage;
class MeshManagerCLodGeometryStorage;
namespace br::render { class RendererStateRequestService; }
namespace br::render { struct PublishedRendererState; class VersionedGpuBufferBackingPool; }
namespace org::runtime { class IUploadService; }
class PublishedStateResourceResolver;

class MeshManager : public IResourceProvider {
public:
	struct CLodActiveGroupRange {
		uint32_t groupsBase = 0;
		uint32_t groupCount = 0;
	};

	struct CLodStreamingDebugStats {
		uint32_t residentGroups = 0;
		uint32_t residentAllocations = 0;
		uint32_t queuedRequests = 0;
		uint32_t queuedOrInFlightGroups = 0;
		uint32_t dispatchedOrInFlightGroups = 0;
		uint32_t completedResults = 0;
		uint32_t pendingDirectStorageLaunches = 0;
		uint32_t pendingDirectStorageUploads = 0;
		uint32_t ioAdmissionTarget = 0;
		uint32_t ioWorkerCount = 0;
		uint32_t ioTaskBatchSize = 0;
		uint64_t residentAllocationBytes = 0;
		uint64_t completedResultBytes = 0;
		uint64_t totalStreamedBytes = 0;
	};

	struct CLodRayTracingResidentGroup {
		uint32_t groupGlobalIndex = 0;
		uint32_t groupLocalIndex = 0;
		ClusterLODGroup group{};
		ClusterLODGroupChunk chunk{};
		std::vector<ClusterLODGroupSegment> segments;
		std::vector<uint32_t> meshPageIndices;
		std::vector<PagePool::PageAllocation> pageAllocations;
	};

	struct CLodRayTracingResidencySnapshot {
		std::vector<CLodRayTracingResidentGroup> residentGroups;
		std::shared_ptr<PagePool> pagePool;
		uint64_t pagePoolGeneration = 0;
	};

	// Represents the outcome of a single disk-streamed group IO.
	struct CLodPrefetchedChildLayout {
		uint32_t groupGlobalIndex = 0;
		CLodCache::GroupPayloadLayoutMetadata layout;
	};

	enum class CLodDiskStreamingPayloadKind : uint8_t {
		CpuPageBlobs,
		CpuMappedPageViews,
		GpuPagesReady,
		ReusedExistingPages,
	};

	struct CLodDiskStreamingCompletion {
		uint32_t groupGlobalIndex = 0;
		uint32_t groupsBase = 0;
		bool success = false;
		CLodDiskStreamingPayloadKind payloadKind = CLodDiskStreamingPayloadKind::CpuPageBlobs;
		ClusterLODGroupChunk chunk{};
		std::vector<uint32_t> meshPageIndices;
		std::vector<bool> segmentNeedsFetch;
		std::vector<std::vector<std::byte>> pageBlobs;
		std::shared_ptr<const CLodCache::MappedContainerLease> mappedContainer;
		std::vector<uint32_t> mappedPageBlobSizes;
		std::vector<uint64_t> mappedPageBlobOffsets;
		std::vector<uint32_t> preAllocatedPages;
		std::vector<PagePool::PageAllocation> pageAllocations;
		std::vector<GroupPageMapEntry> pageMapEntries;
		uint64_t generation = 0;
		uint64_t totalStreamedBytes = 0;
		uint64_t ioTaskQueuedNs = 0;
		uint64_t ioTaskStartedNs = 0;
		uint64_t ioTaskCompletedNs = 0;
		uint32_t fetchedPageCount = 0;
		std::string uploadPathLabel;
		std::vector<CLodPrefetchedChildLayout> prefetchedChildLayouts;
	};

	static std::unique_ptr<MeshManager> CreateUnique() {
		return std::unique_ptr<MeshManager>(new MeshManager());
	}
	~MeshManager();
	bool AddMesh(std::shared_ptr<Mesh>& mesh, bool useMeshletReorderedVertices);
	bool AddMeshInstance(MeshInstance* mesh, bool useMeshletReorderedVertices);
	void RemoveMesh(Mesh* mesh);
	void RemoveMeshInstance(MeshInstance* mesh);

	struct StaticMeshTemplateRequest {
		std::shared_ptr<Mesh> mesh;
		std::shared_ptr<Material> material;
	};

	struct StaticMeshTemplateRegistration {
		uint32_t meshTemplateIndex = 0;
		uint32_t clodOffsetIndex = 0;
		uint32_t skinnedAssemblyTypeSlot = 0xFFFFFFFFu;
		BoundingSphere skinnedAssemblyBounds{};
		float skinnedBoundsScale = 1.0f;
		bool valid = false;
		bool pendingResources = false;
	};

	void AddMeshesBulk(const std::vector<std::shared_ptr<Mesh>>& meshes, bool useMeshletReorderedVertices);
	void SetSkeletonManager(SkeletonManager* manager) { m_skeletonManager = manager; }
	void SetRendererStateServices(br::render::RendererStateRequestService* service,
		std::shared_ptr<org::runtime::IUploadService> uploads, std::uint32_t framesInFlight);
	void SetRendererStateRequestService(br::render::RendererStateRequestService* service);
	std::uint64_t PublishDesiredBufferState();
	std::optional<br::render::ArtifactRequirement> DesiredBufferStateRequirement() const;
	void AcknowledgePublishedBufferState(
		const std::shared_ptr<const br::render::PublishedRendererState>& published);
	ICLodGeometryStorage& GetCLodGeometryStorage() noexcept;
	[[nodiscard]] std::optional<br::render::ArtifactVersionHandle>
		GeometryResidencyVersion() const;
	std::vector<StaticMeshTemplateRegistration> AddStaticMeshTemplatesBulk(const std::vector<StaticMeshTemplateRequest>& requests);
	void PrepareStaticMeshTemplateResourcesAsync(const std::vector<StaticMeshTemplateRequest>& requests);
	uint32_t GetCLodMaxTraversalDepth() const { return m_clodActiveMaxTraversalDepth.load(std::memory_order_acquire); }

	void GetCLodActiveUniqueAssetGroupRanges(std::vector<CLodActiveGroupRange>& outRanges, uint32_t& outMaxGroupIndex) const;
	void GetCLodCoarsestUniqueAssetGroupRanges(std::vector<CLodActiveGroupRange>& outRanges) const;

	// Slow fallback snapshot for registry/device reset recovery. Normal CLod
	// streaming consumes incremental domain events instead.
	struct CLodStreamingDomainSnapshot {
		std::vector<CLodActiveGroupRange> activeRanges;
		std::vector<CLodActiveGroupRange> coarsestRanges;
		uint32_t maxGroupIndex = 0;
	};
	void GetCLodStreamingDomainSnapshot(CLodStreamingDomainSnapshot& outSnapshot) const;

	enum class CLodStreamingDomainEventKind : uint8_t {
		SharedMeshAdded,
		ActiveRangeAdded,
		ActiveRangeRemoved,
		FullReset,
	};

	struct CLodStreamingDomainEvent {
		CLodStreamingDomainEventKind kind = CLodStreamingDomainEventKind::FullReset;
		uint32_t groupsBase = 0;
		uint32_t groupCount = 0;
		uint32_t maxTraversalDepth = 0;
		std::vector<CLodActiveGroupRange> coarsestRanges;
	};

	void DrainCLodStreamingDomainEvents(std::vector<CLodStreamingDomainEvent>& outEvents, uint64_t& outGeneration);
	bool TryGetCLodParentGroup(uint32_t groupGlobalIndex, uint32_t& outParentGlobalIndex) const;
	void GetCLodChildGroups(uint32_t parentGroupGlobalIndex, std::vector<uint32_t>& outChildGroups) const;

	// Patch a single group's error field in the GPU groups buffer.
	// Used by the streaming system to override error for residency transitions.
	void PatchCLodGroupError(uint32_t groupGlobalIndex, float error);

	CLodStreamingDebugStats GetCLodStreamingDebugStats() const;
	void GetCLodRayTracingResidencySnapshot(CLodRayTracingResidencySnapshot& outSnapshot) const;
	void ProcessCLodDiskStreamingIO();

	// Drains groups that completed disk streaming since the last call.
	// The extension uses this to learn which groups became resident (or failed)
	// so it can update the GPU-visible non-resident bitset accordingly.
	void DrainCompletedCLodDiskStreamingGroups(std::vector<CLodDiskStreamingCompletion>& outCompletions);

	// Focused eviction: frees a resident group's page-pool pages, marks it
	// non-resident, and uploads the chunk table.  Returns true on success.
	bool FreeCLodGroupEviction(uint32_t groupGlobalIndex);
	bool EvictCLodGroupResidency(uint32_t groupGlobalIndex, bool clearPageMapEntries);

	bool CommitCLodGroupResidency(
		uint32_t groupGlobalIndex,
		const ClusterLODGroupChunk& chunk,
		std::span<const uint32_t> meshPageIndices,
		std::span<const GroupPageMapEntry> pageMapEntries,
		std::span<const PagePool::PageAllocation> pageAllocations,
		uint64_t streamedBytes = 0u);

	// Queues disk I/O for a group without any residency side-effects.
	// Returns true if the request was queued (or was already in the queue).
	struct CLodGroupDiskIOBatchRequest {
		uint32_t groupGlobalIndex = 0u;
		std::vector<bool> segmentNeedsFetch;
		std::vector<uint32_t> preAllocatedPages;
		std::vector<uint32_t> childLayoutPrefetchGroups;
		bool deferCpuPayloadCopy = false;
		uint32_t priority = 0u;
		std::optional<CLodCache::GroupPayloadLayoutMetadata> prefetchedLayout;
	};
	uint32_t QueueCLodGroupDiskIOBatch(const std::vector<CLodGroupDiskIOBatchRequest>& requests, std::vector<bool>* outQueuedByRequest = nullptr);
	bool QueueCLodGroupDiskIO(uint32_t groupGlobalIndex, const std::vector<bool>& segmentNeedsFetch = {}, const std::vector<uint32_t>& preAllocatedPages = {}, uint32_t priority = 0u, const CLodCache::GroupPayloadLayoutMetadata* prefetchedLayout = nullptr);
	bool TryGetCLodGroupPayloadLayout(uint32_t groupGlobalIndex, CLodCache::GroupPayloadLayoutMetadata& outLayout, std::string* outMessage = nullptr);
	bool IsCLodStreamingDirectStorageEnabled() const { return m_clodStreamingDirectStorageEnabled.load(std::memory_order_acquire); }
	bool HasPendingCLodDirectStorageLaunches() const;
	bool HasPendingCLodDirectStorageUploads() const;
	std::pair<std::size_t, std::size_t> GetPendingCLodDirectStorageCounts() const;
	bool LaunchPendingCLodDirectStorageUploads(rhi::Timeline waitTimeline, uint64_t waitValue);

	// Returns true if the group currently has disk I/O queued or in-flight.
	bool IsCLodGroupDiskIOQueued(uint32_t groupGlobalIndex) const;

	// Invalidates all in-flight and queued disk streaming IO.
	// Bumps a generation counter so that stale in-flight results are rejected.
	// Must be called when page allocations are invalidated (e.g. render graph rebuild).
	void InvalidateCLodDiskStreamingPipeline();

	struct CLodGroupStreamingInfo {
		struct ReferencedPageSegment {
			uint32_t meshPageIndex = 0;
			uint32_t sourceGroupLocalIndex = 0;
			uint32_t sourceGroupGlobalIndex = 0;
			uint32_t segmentGlobalIndex = 0;
			ClusterLODGroupSegment segment{};
		};

		ClusterLODRuntimeSummary::GroupChunkHint hint{};
		uint32_t groupsBase = 0;
		uint32_t pageMapBase = 0;
		uint32_t pageCount = 0;
		ClusterLODGroup group{};
		std::vector<ClusterLODGroupSegment> segments;
		std::vector<ReferencedPageSegment> referencedPageSegments;
		std::vector<uint32_t> meshPageIndices;
		std::vector<uint32_t> meshPageBlobSizes;
		uint32_t vertexByteSize = 0;
		bool valid = false;
	};
	// Retrieves the chunk hint and vertex byte size for a group so that
	// the caller can compute the estimated page count before dispatching I/O.
	CLodGroupStreamingInfo GetCLodGroupStreamingInfo(uint32_t groupGlobalIndex) const;

	void UpdatePerMeshBuffer(std::unique_ptr<BufferView>& view, PerMeshCB& data);
	void UpdatePerMeshInstanceBuffer(std::unique_ptr<BufferView>& view, PerMeshInstanceCB& data);
	std::unique_ptr<BufferView> AllocatePerMeshOverrideBuffer(const PerMeshCB& data);
	void ReleasePerMeshOverrideBuffer(std::unique_ptr<BufferView>& view);
	void SetViewManager(ViewManager* viewManager) { m_pViewManager = viewManager; }

	// Access the CLod page pool (may be null if no CLod meshes loaded).
	PagePool* GetCLodPagePool() const { return m_clodPagePool.get(); }
	std::shared_ptr<ResourceGroup> GetCLodSlabResourceGroup() const {
		return m_clodPagePool ? m_clodPagePool->GetSlabResourceGroup() : nullptr;
	}
	void SetCLodStreamingUploadFunction(PagePool::UploadFn fn);
	void SetCLodStreamingWakeFunction(std::function<void()> fn);
	uint64_t GetActiveMeshletCount() const { return m_activeMeshletCount; }

	std::shared_ptr<Resource> ProvideResource(ResourceIdentifier const& key) override;
	std::vector<ResourceIdentifier> GetSupportedKeys() override;
	std::shared_ptr<IResourceResolver> ProvideResolver(ResourceIdentifier const& key) override;
	std::vector<ResourceIdentifier> GetSupportedResolverKeys() override;

private:
	MeshManager();
	std::unordered_map<ResourceIdentifier, std::shared_ptr<Resource>, ResourceIdentifier::Hasher> m_resources;

	// Base meshes
	std::shared_ptr<DynamicBuffer> m_perMeshBuffers;

	// mesh instances
	std::shared_ptr<DynamicBuffer> m_perMeshInstanceBuffers;

	std::shared_ptr<DynamicBuffer> m_perMeshInstanceClodOffsets;
	std::shared_ptr<DynamicBuffer> m_clodSharedGroupChunks;
	std::shared_ptr<DynamicBuffer> m_clodMeshMetadata;
	std::shared_ptr<DynamicBuffer> m_clodHierarchyLevelInfos;
	std::shared_ptr<DynamicBuffer> m_clusterLODGroups;
	std::shared_ptr<DynamicBuffer> m_clusterLODSegments;

	//std::shared_ptr<DynamicBuffer> m_clusterLODMeshlets;
	//std::shared_ptr<DynamicBuffer> m_clusterLODMeshletBounds;
	std::shared_ptr<DynamicBuffer> m_clusterLODNodes;
	std::shared_ptr<DynamicBuffer> m_clusterLODNodeSkinningInfos;
	std::shared_ptr<DynamicBuffer> m_clusterLODNodeBoneIndices;
	std::shared_ptr<DynamicBuffer> m_clusterLODAssemblyTransforms;
	std::shared_ptr<DynamicBuffer> m_clusterLODAssemblyInstances;
	std::shared_ptr<DynamicBuffer> m_clusterLODAssemblyBoneRemaps;
	std::shared_ptr<DynamicBuffer> m_clusterLODAssemblyBoneRemapIndices;
	std::shared_ptr<DynamicBuffer> m_clodGroupPageMap;
	uint64_t m_activeMeshletCount = 0;

	struct PreparedCLodContainer {
		std::weak_ptr<Mesh> mesh;
		std::wstring resolvedPath;
		std::shared_ptr<const CLodCache::MappedContainerLease> lease;
		uint32_t pageCount = 0;
	};
	std::mutex m_preparedCLodContainersMutex;
	std::unordered_map<Mesh*, PreparedCLodContainer> m_preparedCLodContainers;

	struct CLodSharedStreamingState {
		struct ResidentGroupAllocations {
			// Per-child page allocations (one page per child)
			std::vector<PagePool::PageAllocation> pageAllocations;

			void Reset() {
				pageAllocations.clear();
			}
		};

		Mesh* mesh = nullptr;
		uint32_t maxTraversalDepth = 0;
		uint32_t vertexByteSize = 0;
		ClusterLODCacheSource cacheSource{};
		std::wstring resolvedContainerPath;
		std::vector<ClusterLODGroupDiskLocator> pageDiskLocators;
		std::shared_ptr<const CLodCache::MappedContainerLease>
			mappedContainerLease;
		std::unique_ptr<std::atomic<uint8_t>[]> mappedPageWarmStates;
		uint32_t mappedPageWarmStateCount = 0u;
		std::vector<ClusterLODRuntimeSummary::GroupChunkHint> groupChunkHints;
		std::unique_ptr<BufferView> ownedMeshMetadataView;
		uint32_t clodMeshMetadataIndex = 0;
		uint32_t groupsBase = 0;
		uint32_t groupCount = 0;
		std::unique_ptr<BufferView> ownedGroupChunksView;
		BufferView* groupChunksView = nullptr;
		std::vector<ClusterLODGroupChunk> baselineGroupChunks;
		std::vector<uint8_t> groupResidentFlags;
		std::vector<ResidentGroupAllocations> residentGroupAllocations;
		uint32_t activeInstanceCount = 0;

		// Copies of hierarchy data needed at streaming-apply time.
		// (The Mesh releases its CPU copies after setup via ReleaseCLodHierarchyCpuData.)
		std::vector<ClusterLODGroup> groups;
		std::vector<ClusterLODGroupSegment> segments;
		std::vector<uint32_t> groupPageReferences;
		std::vector<uint32_t> groupPageReferenceOffsets;

		// Parent-child mapping and original error values, copied from
		// the runtime summary at AddMesh time so that the streaming
		// domain snapshot always has reliable data regardless of mesh
		// object lifetime or summary state.
		std::vector<int32_t> parentGroupByLocal;
		std::vector<std::vector<uint32_t>> childrenByLocalParent;
		std::vector<float> groupErrorByLocal;
		std::vector<ClusterLODRuntimeSummary::GroupRange> coarsestRanges;

		// GroupPageMap buffer view for this mesh's page map entries.
		std::unique_ptr<BufferView> ownedPageMapView;
		uint32_t pageMapGlobalBase = 0; // global offset into GroupPageMap buffer
		uint32_t totalPageMapEntries = 0;
		std::vector<GroupPageMapEntry> pageMapEntriesCPU; // CPU mirror for UpdateView
	};

	struct CLodSharedStreamingRange {
		uint32_t begin = 0;
		uint32_t end = 0;
		std::shared_ptr<CLodSharedStreamingState> state;
	};

	struct CLodStreamingInstanceState {
		MeshInstance* instance = nullptr;
		uint32_t meshInstanceIndex = 0;
		uint32_t groupsBase = 0;
		uint32_t groupCount = 0;
		std::shared_ptr<CLodSharedStreamingState> sharedMeshState;
	};

	std::unordered_map<uint32_t, CLodStreamingInstanceState> m_clodStreamingStateByInstanceIndex;
	std::unordered_map<const MeshInstance*, uint32_t> m_clodStreamingInstanceIndexByPtr;
	std::unordered_map<const Mesh*, std::shared_ptr<CLodSharedStreamingState>> m_clodSharedStreamingStateByMesh;
	// Serializes the mutable authoring registry. Render consumers never acquire
	// this lock; they consume immutable geometry-buffer versions from a frame snapshot.
	mutable std::recursive_mutex m_staticTemplatePublicationMutex;
	struct GraphBufferBinding {
		ResourceIdentifier identifier;
		std::shared_ptr<DynamicBuffer> buffer;
		br::render::ArtifactKey key;
		std::uint64_t catalogVariant = 0;
		std::uint32_t elementStride = 0;
		br::render::ArtifactVersionID submittedVersion{};
		std::shared_ptr<br::render::VersionedGpuBufferBackingPool> backingPool;
	};
	std::vector<GraphBufferBinding> m_graphBufferBindings;
	std::unordered_map<ResourceIdentifier, std::shared_ptr<PublishedStateResourceResolver>,
		ResourceIdentifier::Hasher> m_graphBufferResolvers;
	std::shared_ptr<org::runtime::IUploadService> m_geometryUploadService;
	mutable std::mutex m_geometryBufferGraphMutex;
	std::atomic_bool m_geometryBufferGraphDirty{ true };
	std::uint64_t m_geometryBufferStateRevision = 0;
	std::uint64_t m_geometryBufferFingerprint = 0;
	br::render::ArtifactVersionHandle m_geometryBufferStateVersion{};
	std::uint32_t m_geometryFramesInFlight = 1;
	SkeletonManager* m_skeletonManager = nullptr;
	std::unordered_map<const Skeleton*, std::shared_ptr<Skeleton>> m_windTypeSkeletons;
	std::vector<CLodSharedStreamingRange> m_clodSharedStreamingRanges;
	bool m_clodSharedStreamingRangesDirty = true;
	mutable std::mutex m_clodStreamingDomainEventsMutex;
	std::vector<CLodStreamingDomainEvent> m_clodStreamingDomainEvents;
	std::atomic<uint64_t> m_clodStreamingDomainEventGeneration{0};
	mutable std::mutex m_geometryResidencyPublicationMutex;
	br::render::RendererStateRequestService* m_rendererStateRequests = nullptr;
	br::render::ArtifactVersionHandle m_geometryResidencyVersion;
	uint64_t m_geometryResidencyRevision = 0;
	std::atomic<bool> m_clodStreamingDirectStorageEnabled{true};
	SettingsManager::Subscription m_clodStreamingDirectStorageSubscription;

	// Incremental debug-stats counters — updated in place by residency mutations.
	std::atomic<uint32_t> m_debugResidentGroups{0};
	std::atomic<uint32_t> m_debugResidentAllocations{0};
	std::atomic<uint64_t> m_debugResidentAllocationBytes{0};
	std::atomic<uint64_t> m_debugTotalStreamedBytes{0};
	std::atomic<uint32_t> m_clodActiveMaxTraversalDepth{0};

	struct CLodDiskStreamingRequest {
		uint32_t groupGlobalIndex = 0;
		ClusterLODCacheSource cacheSource{};
		uint32_t groupsBase = 0;
		uint32_t groupLocalIndex = 0;
		std::optional<CLodCache::GroupPayloadLayoutMetadata> prefetchedLayout;
		std::shared_ptr<CLodSharedStreamingState> sharedState;
		uint32_t pageMapBase = 0;
		uint32_t pageCount = 0;
		std::vector<uint32_t> meshPageIndices;
		std::vector<bool> segmentNeedsFetch; // true = fetch from disk; false = reuse existing slab data
		std::vector<uint32_t> preAllocatedPages; // page IDs pre-allocated by the LRU
		std::vector<uint32_t> childLayoutPrefetchGroups;
		bool deferCpuPayloadCopy = false;
		uint64_t generation = 0; // generation at time of request
		uint32_t priority = 0; // streaming priority for I/O dispatch ordering
		uint64_t ioTaskQueuedNs = 0;
	};

	struct CLodDiskStreamingResult {
		uint32_t groupGlobalIndex = 0;
		bool success = false;
		ClusterLODCacheSource cacheSource{};
		std::string uploadPathLabel = "CpuReadThenCpuUpload";
		std::optional<ClusterLODGroupChunk> groupChunkMetadata;
		std::vector<bool> segmentNeedsFetch;
		std::vector<uint32_t> meshPageIndices;
		std::vector<uint32_t> directStoragePageBlobSizes;
		std::vector<uint64_t> directStoragePageBlobOffsets;
		bool directStorageGpuUploadPending = false;
		std::vector<std::vector<std::byte>> pageBlobs;
		std::shared_ptr<const CLodCache::MappedContainerLease> mappedContainer;
		std::vector<uint32_t> mappedPageBlobSizes;
		std::vector<uint64_t> mappedPageBlobOffsets;
		std::vector<uint32_t> preAllocatedPages; // forwarded from request
		std::vector<CLodPrefetchedChildLayout> prefetchedChildLayouts;
		uint64_t generation = 0; // generation at time of request
		uint64_t ioTaskQueuedNs = 0;
		uint64_t ioTaskStartedNs = 0;
		uint64_t ioTaskCompletedNs = 0;
	};

	struct CLodPendingDirectStorageUpload {
		uint32_t groupGlobalIndex = 0;
		uint64_t generation = 0;
		std::shared_ptr<CLodSharedStreamingState> sharedState;
		uint32_t groupLocalIndex = 0;
		ClusterLODGroupChunk chunk{};
		std::vector<PagePool::PageAllocation> pageAllocations;
		std::vector<GroupPageMapEntry> pageMapEntries;
		std::vector<uint32_t> meshPageIndices;
		std::vector<bool> segmentNeedsFetch;
		uint32_t fetchedPageCount = 0;
		uint64_t totalBlobBytes = 0;
		std::string uploadPathLabel = "DirectStorageGpuDirect";
		DirectStorageAsyncRequestHandle uploadHandle;
		std::vector<uint32_t> pageIds;
		std::vector<CLodPrefetchedChildLayout> prefetchedChildLayouts;
	};

	struct CLodPendingDirectStorageLaunch {
		uint32_t groupGlobalIndex = 0;
		uint64_t generation = 0;
		ClusterLODCacheSource cacheSource{};
		std::shared_ptr<CLodSharedStreamingState> sharedState;
		uint32_t groupLocalIndex = 0;
		ClusterLODGroupChunk chunk{};
		std::vector<PagePool::PageAllocation> pageAllocations;
		std::vector<GroupPageMapEntry> pageMapEntries;
		std::vector<uint32_t> meshPageIndices;
		std::vector<bool> segmentNeedsFetch;
		std::vector<br::DirectStorageBufferRegionCopy> copies;
		std::vector<uint32_t> pageIds;
		std::vector<CLodPrefetchedChildLayout> prefetchedChildLayouts;
		uint32_t fetchedPageCount = 0;
		uint64_t totalBlobBytes = 0;
		std::string uploadPathLabel = "DirectStorageGpuDirect";
	};

	// Pending requests waiting to be dispatched (guarded by m_clodDiskStreamingMutex).
	mutable std::mutex m_clodDiskStreamingMutex;
	std::vector<CLodDiskStreamingRequest> m_clodDiskStreamingRequests;
	std::unordered_set<uint32_t> m_clodDiskStreamingQueuedGroups;

	// Generation counter for invalidating in-flight disk IO across rebuilds.
	std::atomic<uint64_t> m_clodDiskStreamingGeneration{0};

	// Guards m_clodDiskStreamingResults and m_clodDiskStreamingCompletions.
	mutable std::mutex m_clodDiskStreamingResultsMutex;

	// Completed results waiting to be applied on the main thread.
	std::vector<CLodDiskStreamingResult> m_clodDiskStreamingResults;
	std::vector<CLodDiskStreamingCompletion> m_clodDiskStreamingCompletions;
	std::vector<CLodPendingDirectStorageLaunch> m_clodPendingDirectStorageLaunches;
	std::vector<CLodPendingDirectStorageUpload> m_clodPendingDirectStorageUploads;
	PagePool::UploadFn m_clodStreamingUploadFn;
	// Guarded by m_clodDiskStreamingResultsMutex. I/O workers copy the
	// callback while publishing, then invoke it after releasing the lock.
	std::function<void()> m_clodStreamingWakeFn;

	rhi::TimelinePtr m_clodDirectStorageCompletionFencePtr;
	rhi::Timeline m_clodDirectStorageCompletionFenceHandle;
	std::atomic<uint64_t> m_clodDirectStorageCompletionFenceCounter{0};

	// Guards CLodSharedStreamingState interiors (groupResidentFlags,
	// baselineGroupChunks, residentGroupAllocations),
	// m_clodPagePool, and m_clodSharedGroupChunks UpdateView calls.
	mutable std::mutex m_clodResidencyMutex;

	// Keep only a small amount of work beyond the active I/O workers. Priority
	// remains mutable in m_clodDiskStreamingRequests until a task is admitted.
	static constexpr uint32_t kMaxIoBatchSize = 128u;

	void DispatchCLodDiskStreamingBatch();
	bool QueueCLodDiskStreamingRequest(uint32_t groupGlobalIndex, const std::shared_ptr<CLodSharedStreamingState>& state, uint32_t groupLocalIndex, bool& outQueued, const std::vector<bool>& segmentNeedsFetch = {}, const std::vector<uint32_t>& preAllocatedPages = {}, uint32_t priority = 0u, const CLodCache::GroupPayloadLayoutMetadata* prefetchedLayout = nullptr);

	enum class DiskStreamingApplyResult {
		Prepared,
		DeferredPendingUpload,
		FailedPermanent,
	};
	DiskStreamingApplyResult PrepareCompletedCLodDiskStreamingResult(CLodDiskStreamingResult& result, const std::vector<uint32_t>& preAllocatedPages, CLodDiskStreamingCompletion& outCompletion);
	void FinalizePendingCLodDirectStorageUploads(uint64_t currentGeneration, std::vector<CLodDiskStreamingCompletion>& outCompletions, std::vector<uint32_t>& outFinishedGroups);
	void UploadCLodGroupChunkTable(const CLodSharedStreamingState& state);
	void UploadCLodGroupChunk(const CLodSharedStreamingState& state, uint32_t groupLocalIndex);
	void UploadCLodGroupPageMapRange(
		CLodSharedStreamingState& state,
		uint32_t pageMapOffset,
		std::span<const GroupPageMapEntry> pageMapEntries);
	bool IsCLodGroupResident(const CLodSharedStreamingState& state, uint32_t groupLocalIndex) const;
	bool IsCLodMeshPageReferencedByResidentGroup(const CLodSharedStreamingState& state, uint32_t meshPageIndex) const;
	void DeallocateCLodGroupChunkAllocations(CLodSharedStreamingState& state, uint32_t groupLocalIndex);
	void ReleaseAllCLodGroupChunkAllocations(CLodSharedStreamingState& state);
 	static void ZeroCLodGroupChunkCounts(ClusterLODGroupChunk& chunk);
	bool ApplyCLodGroupEviction(CLodSharedStreamingState& state, uint32_t groupLocalIndex, bool clearPageMapEntries);

	void RebuildCLodSharedStreamingRangeIndex();
	void PublishCLodStreamingDomainEvent(CLodStreamingDomainEvent event);
	void PublishGeometryResidencyDelta(const CLodStreamingDomainEvent& event);
	void PublishCLodStreamingDomainEventForSharedState(CLodStreamingDomainEventKind kind, const std::shared_ptr<CLodSharedStreamingState>& sharedState);
	void RecomputeCLodActiveMaxTraversalDepth();
	std::shared_ptr<CLodSharedStreamingState> FindCLodSharedStreamingStateByGlobalGroup(uint32_t groupGlobalIndex, uint32_t& outGroupLocalIndex);
	std::vector<uint32_t> GetCLodGroupMeshPageIndices(const CLodSharedStreamingState& state, uint32_t groupLocalIndex) const;
	std::vector<uint32_t> GetCLodGroupPageMapOffsets(const CLodSharedStreamingState& state, uint32_t groupLocalIndex) const;

	ViewManager* m_pViewManager;

	// Page pool for CLod streaming
	std::shared_ptr<PagePool> m_clodPagePool;
	std::unique_ptr<MeshManagerCLodGeometryStorage> m_clodGeometryStorage;
};

// Narrow renderer-scoped storage and residency service used by CLOD streaming.
// It deliberately excludes mesh authoring, scene objects, views and resource
// provider lookup from the streaming worker contract.
class ICLodGeometryStorage {
public:
	virtual ~ICLodGeometryStorage() = default;
	virtual void GetCLodStreamingDomainSnapshot(MeshManager::CLodStreamingDomainSnapshot&) const = 0;
	virtual void DrainCLodStreamingDomainEvents(
		std::vector<MeshManager::CLodStreamingDomainEvent>&, uint64_t&) = 0;
	virtual bool TryGetCLodParentGroup(uint32_t, uint32_t&) const = 0;
	virtual void GetCLodChildGroups(uint32_t, std::vector<uint32_t>&) const = 0;
	virtual MeshManager::CLodStreamingDebugStats GetCLodStreamingDebugStats() const = 0;
	virtual void ProcessCLodDiskStreamingIO() = 0;
	virtual void DrainCompletedCLodDiskStreamingGroups(
		std::vector<MeshManager::CLodDiskStreamingCompletion>&) = 0;
	virtual bool EvictCLodGroupResidency(uint32_t, bool) = 0;
	virtual bool CommitCLodGroupResidency(uint32_t, const ClusterLODGroupChunk&,
		std::span<const uint32_t>, std::span<const GroupPageMapEntry>,
		std::span<const PagePool::PageAllocation>, uint64_t = 0u) = 0;
	virtual uint32_t QueueCLodGroupDiskIOBatch(
		const std::vector<MeshManager::CLodGroupDiskIOBatchRequest>&,
		std::vector<bool>* = nullptr) = 0;
	virtual bool QueueCLodGroupDiskIO(uint32_t, const std::vector<bool>& = {},
		const std::vector<uint32_t>& = {}, uint32_t = 0u,
		const CLodCache::GroupPayloadLayoutMetadata* = nullptr) = 0;
	virtual bool TryGetCLodGroupPayloadLayout(uint32_t,
		CLodCache::GroupPayloadLayoutMetadata&, std::string* = nullptr) = 0;
	virtual bool IsCLodStreamingDirectStorageEnabled() const = 0;
	virtual std::pair<std::size_t, std::size_t> GetPendingCLodDirectStorageCounts() const = 0;
	virtual bool LaunchPendingCLodDirectStorageUploads(rhi::Timeline, uint64_t) = 0;
	virtual void InvalidateCLodDiskStreamingPipeline() = 0;
	virtual MeshManager::CLodGroupStreamingInfo GetCLodGroupStreamingInfo(uint32_t) const = 0;
	virtual PagePool* GetCLodPagePool() const = 0;
	virtual void SetCLodStreamingUploadFunction(PagePool::UploadFn) = 0;
	virtual void SetCLodStreamingWakeFunction(std::function<void()>) = 0;
};

class MeshManagerCLodGeometryStorage final : public ICLodGeometryStorage {
public:
	explicit MeshManagerCLodGeometryStorage(MeshManager& owner) : m_owner(owner) {}
	void GetCLodStreamingDomainSnapshot(MeshManager::CLodStreamingDomainSnapshot& value) const override { m_owner.GetCLodStreamingDomainSnapshot(value); }
	void DrainCLodStreamingDomainEvents(std::vector<MeshManager::CLodStreamingDomainEvent>& value, uint64_t& generation) override { m_owner.DrainCLodStreamingDomainEvents(value, generation); }
	bool TryGetCLodParentGroup(uint32_t group, uint32_t& parent) const override { return m_owner.TryGetCLodParentGroup(group, parent); }
	void GetCLodChildGroups(uint32_t group, std::vector<uint32_t>& children) const override { m_owner.GetCLodChildGroups(group, children); }
	MeshManager::CLodStreamingDebugStats GetCLodStreamingDebugStats() const override { return m_owner.GetCLodStreamingDebugStats(); }
	void ProcessCLodDiskStreamingIO() override { m_owner.ProcessCLodDiskStreamingIO(); }
	void DrainCompletedCLodDiskStreamingGroups(std::vector<MeshManager::CLodDiskStreamingCompletion>& value) override { m_owner.DrainCompletedCLodDiskStreamingGroups(value); }
	bool EvictCLodGroupResidency(uint32_t group, bool clear) override { return m_owner.EvictCLodGroupResidency(group, clear); }
	bool CommitCLodGroupResidency(uint32_t group, const ClusterLODGroupChunk& chunk,
		std::span<const uint32_t> indices, std::span<const GroupPageMapEntry> entries,
		std::span<const PagePool::PageAllocation> allocations, uint64_t bytes) override {
		return m_owner.CommitCLodGroupResidency(group, chunk, indices, entries, allocations, bytes);
	}
	uint32_t QueueCLodGroupDiskIOBatch(const std::vector<MeshManager::CLodGroupDiskIOBatchRequest>& requests, std::vector<bool>* queued) override { return m_owner.QueueCLodGroupDiskIOBatch(requests, queued); }
	bool QueueCLodGroupDiskIO(uint32_t group, const std::vector<bool>& fetch,
		const std::vector<uint32_t>& pages, uint32_t priority,
		const CLodCache::GroupPayloadLayoutMetadata* layout) override {
		return m_owner.QueueCLodGroupDiskIO(group, fetch, pages, priority, layout);
	}
	bool TryGetCLodGroupPayloadLayout(uint32_t group,
		CLodCache::GroupPayloadLayoutMetadata& layout, std::string* message) override {
		return m_owner.TryGetCLodGroupPayloadLayout(group, layout, message);
	}
	bool IsCLodStreamingDirectStorageEnabled() const override { return m_owner.IsCLodStreamingDirectStorageEnabled(); }
	std::pair<std::size_t, std::size_t> GetPendingCLodDirectStorageCounts() const override { return m_owner.GetPendingCLodDirectStorageCounts(); }
	bool LaunchPendingCLodDirectStorageUploads(rhi::Timeline timeline, uint64_t value) override { return m_owner.LaunchPendingCLodDirectStorageUploads(timeline, value); }
	void InvalidateCLodDiskStreamingPipeline() override { m_owner.InvalidateCLodDiskStreamingPipeline(); }
	MeshManager::CLodGroupStreamingInfo GetCLodGroupStreamingInfo(uint32_t group) const override { return m_owner.GetCLodGroupStreamingInfo(group); }
	PagePool* GetCLodPagePool() const override { return m_owner.GetCLodPagePool(); }
	void SetCLodStreamingUploadFunction(PagePool::UploadFn fn) override { m_owner.SetCLodStreamingUploadFunction(std::move(fn)); }
	void SetCLodStreamingWakeFunction(std::function<void()> fn) override { m_owner.SetCLodStreamingWakeFunction(std::move(fn)); }

private:
	MeshManager& m_owner;
};
