#pragma once
#include "VirtualGeometry/GeometryStorage/MeshManager.h"
#include "VirtualGeometry/GeometryStorage/CLodGeometryStorage.h"

class MeshManagerCLodGeometryStorage final : public ICLodGeometryStorage {
public:
	explicit MeshManagerCLodGeometryStorage(MeshManager& owner) : m_owner(owner) {}
	void GetCLodStreamingDomainSnapshot(br::render::CLodStreamingDomainSnapshot& value) const override { m_owner.GetCLodStreamingDomainSnapshot(value); }
	void DrainCLodStreamingDomainEvents(std::vector<br::render::CLodStreamingDomainEvent>& value, uint64_t& generation) override { m_owner.DrainCLodStreamingDomainEvents(value, generation); }
	bool TryGetCLodParentGroup(uint32_t group, uint32_t& parent) const override { return m_owner.TryGetCLodParentGroup(group, parent); }
	void GetCLodChildGroups(uint32_t group, std::vector<uint32_t>& children) const override { m_owner.GetCLodChildGroups(group, children); }
	br::render::CLodStreamingDebugStats GetCLodStreamingDebugStats() const override { return m_owner.GetCLodStreamingDebugStats(); }
	void ProcessCLodDiskStreamingIO() override { m_owner.ProcessCLodDiskStreamingIO(); }
	void DrainCompletedCLodDiskStreamingGroups(std::vector<br::render::CLodDiskStreamingCompletion>& value) override { m_owner.DrainCompletedCLodDiskStreamingGroups(value); }
	bool EvictCLodGroupResidency(uint32_t group, bool clear) override { return m_owner.EvictCLodGroupResidency(group, clear); }
	bool CommitCLodGroupResidency(uint32_t group, const ClusterLODGroupChunk& chunk,
		std::span<const uint32_t> indices, std::span<const GroupPageMapEntry> entries,
		std::span<const PagePool::PageAllocation> allocations, uint64_t bytes) override {
		return m_owner.CommitCLodGroupResidency(group, chunk, indices, entries, allocations, bytes);
	}
	uint32_t QueueCLodGroupDiskIOBatch(const std::vector<br::render::CLodGroupDiskIOBatchRequest>& requests, std::vector<bool>* queued) override { return m_owner.QueueCLodGroupDiskIOBatch(requests, queued); }
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
	br::render::CLodGroupStreamingInfo GetCLodGroupStreamingInfo(uint32_t group) const override { return m_owner.GetCLodGroupStreamingInfo(group); }
	PagePool* GetCLodPagePool() const override { return m_owner.GetCLodPagePool(); }
	void SetCLodStreamingUploadFunction(PagePool::UploadFn fn) override { m_owner.SetCLodStreamingUploadFunction(std::move(fn)); }
	void SetCLodStreamingWakeFunction(std::function<void()> fn) override { m_owner.SetCLodStreamingWakeFunction(std::move(fn)); }
	std::shared_ptr<br::render::CLodResidencyStorageDirectory> GetCLodResidencyStorages() const override { return m_owner.GetCLodResidencyStorages(); }

private:
	MeshManager& m_owner;
};
