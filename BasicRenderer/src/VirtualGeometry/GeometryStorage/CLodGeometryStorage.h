#pragma once
#include <BasicRenderer/Streaming/CLodGeometryTypes.h>

// Narrow renderer-scoped storage and residency service used by CLOD streaming.
// It deliberately excludes mesh authoring, scene objects, views and resource
// provider lookup from the streaming worker contract.
class ICLodGeometryStorage {
public:
	virtual ~ICLodGeometryStorage() = default;
	virtual void GetCLodStreamingDomainSnapshot(br::render::CLodStreamingDomainSnapshot&) const = 0;
	virtual void DrainCLodStreamingDomainEvents(
		std::vector<br::render::CLodStreamingDomainEvent>&, uint64_t&) = 0;
	virtual bool TryGetCLodParentGroup(uint32_t, uint32_t&) const = 0;
	virtual void GetCLodChildGroups(uint32_t, std::vector<uint32_t>&) const = 0;
	virtual br::render::CLodStreamingDebugStats GetCLodStreamingDebugStats() const = 0;
	virtual void ProcessCLodDiskStreamingIO() = 0;
	virtual void DrainCompletedCLodDiskStreamingGroups(
		std::vector<br::render::CLodDiskStreamingCompletion>&) = 0;
	virtual bool EvictCLodGroupResidency(uint32_t, bool) = 0;
	virtual bool CommitCLodGroupResidency(uint32_t, const ClusterLODGroupChunk&,
		std::span<const uint32_t>, std::span<const GroupPageMapEntry>,
		std::span<const PagePool::PageAllocation>, uint64_t = 0u) = 0;
	virtual uint32_t QueueCLodGroupDiskIOBatch(
		const std::vector<br::render::CLodGroupDiskIOBatchRequest>&,
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
	virtual br::render::CLodGroupStreamingInfo GetCLodGroupStreamingInfo(uint32_t) const = 0;
	virtual PagePool* GetCLodPagePool() const = 0;
	virtual void SetCLodStreamingUploadFunction(PagePool::UploadFn) = 0;
	virtual void SetCLodStreamingWakeFunction(std::function<void()>) = 0;
	virtual std::shared_ptr<br::render::CLodResidencyStorageDirectory> GetCLodResidencyStorages() const = 0;
};

