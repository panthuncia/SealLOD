#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <BasicRenderer/Assets/Import/CLodCache.h>
#include <BasicRenderer/Assets/ClusterLODTypes.h>
#include "BasicRenderer/Extensions/Buffers/PagePool.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"

namespace br::render {
class CLodResidencyStorageDirectory;

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

struct CLodStreamingDomainSnapshot {
	std::vector<CLodActiveGroupRange> activeRanges;
	std::vector<CLodActiveGroupRange> coarsestRanges;
	uint32_t maxGroupIndex = 0;
};

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

struct CLodGroupDiskIOBatchRequest {
	uint32_t groupGlobalIndex = 0u;
	std::vector<bool> segmentNeedsFetch;
	std::vector<uint32_t> preAllocatedPages;
	std::vector<uint32_t> childLayoutPrefetchGroups;
	bool deferCpuPayloadCopy = false;
	uint32_t priority = 0u;
	std::optional<CLodCache::GroupPayloadLayoutMetadata> prefetchedLayout;
};

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

} // namespace br::render
