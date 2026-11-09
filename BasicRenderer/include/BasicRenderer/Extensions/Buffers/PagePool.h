#pragma once

#include <cstdint>
#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <span>
#include <vector>

#include "Resources/Resource.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/ResourceGroup.h"
#include "Render/Runtime/UploadTypes.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"

namespace org { class GpuBufferBacking; }

// Size-class page allocator backed by multiple GPU "slab" ByteAddressBuffers.
//
// - Each **slab** is a single D3D12 ByteAddressBuffer (e.g. 256 MB).
// - Every slab contains pages of exactly one size class.
// - Logical pages are single LRU/eviction units; no contiguous-run allocation.
// - A GPU-visible **page table** maps virtual page IDs -> slab locations.
//
// Advantages over per-stream DynamicBuffer pools:
//   - No single D3D12 resource grows past the slab cap.
//   - No free-list fragmentation, pages are uniform.
//   - Adding a slab never copies existing data.
//   - Eviction is O(1)
class PagePool {
public:
	static constexpr uint32_t kPageSizeClassCount = 7u;
	// Packed visible clusters and CLAS records hold a page's offset in 4 KiB units in 14 bits.
	static constexpr uint64_t kMaxSlabBytes = 64ull * 1024ull * 1024ull;
	// Configuration for the page pool.
	struct Config {
		// 4 and 8 KiB are for pinned storage, where half the pages carry under 4 KiB. Streaming pages start at
		// minStreamingPageSize: a 64 MiB slab of a small class barely used would cost more than it saves.
		std::array<uint32_t, kPageSizeClassCount> pageSizes{
			4 * 1024, 8 * 1024, 16 * 1024, 32 * 1024, 64 * 1024, 128 * 1024, 256 * 1024
		};
		uint64_t slabSize     = 64 * 1024 * 1024; // General streaming slab size: the unit of growth and of rebalancing.
		uint64_t pinnedSlabSize = 4 * 1024 * 1024; // Per-class pinned growth increment.
		uint32_t numStreamingSlabs = 16; // Shared cap across all size classes.
		uint32_t minStreamingPageSize = 16 * 1024;
		std::array<uint32_t, kPageSizeClassCount> initialStreamingSlabs{
			0u, 0u, 1u, 1u, 1u, 1u, 1u
		};
		std::string debugName = "CLodPagePool";
	};

	// Represents a contiguous allocation of pages.
	struct PageAllocation {
		uint32_t firstPageID = 0; // Global virtual page ID of the first page.
		uint32_t pageCount   = 0; // Number of contiguous pages in this allocation.

		bool IsValid() const { return pageCount > 0; }
		void Reset() { firstPageID = 0; pageCount = 0; }
	};

	PagePool() : PagePool(Config{}) {}
	explicit PagePool(const Config& config);
	~PagePool();

	// Non-copyable, non-movable.
	PagePool(const PagePool&) = delete;
	PagePool& operator=(const PagePool&) = delete;

	// Upload `dataSize` bytes of CPU data into the slab at the given page +
	// intra-page byte offset. The data must fit within the allocation.
	void UploadToPage(uint32_t globalPageID, uint32_t intraPageByteOffset,
					  const void* data, size_t dataSize);

	// Accessors

	// Number of slabs currently allocated.
	uint32_t GetSlabCount() const;
	// Get the static Buffer backing slab `slabIndex` (for resource registration).
	std::shared_ptr<org::Buffer> GetSlab(uint32_t slabIndex) const;

	// Get the page-table Buffer (StructuredBuffer<PageTableEntry>).
	std::shared_ptr<org::Buffer> GetPageTableBuffer() const;

	// Get the ResourceGroup tracking all slab buffers (for render graph declarations).
	std::shared_ptr<org::ResourceGroup> GetSlabResourceGroup() const { return m_slabResourceGroup; }

	// Total pages across all slabs.
	uint32_t GetTotalPageCount() const;

	// Total pages across general-purpose slabs.
	uint32_t GetGeneralPageCount() const;

	// Largest supported page size (legacy payload validation accessor).
	uint64_t GetPageSize() const { return m_config.pageSizes.back(); }
	uint32_t GetPageSize(uint32_t globalPageID) const;
	uint32_t GetPageSizeClassIndex(uint32_t globalPageID) const;
	uint32_t SelectPageSize(uint64_t payloadBytes) const;
	// The streaming (general) page size for a payload: SelectPageSize, at least Config::minStreamingPageSize.
	uint32_t SelectStreamingPageSize(uint64_t payloadBytes) const {
		const uint32_t size = SelectPageSize(payloadBytes);
		return size < m_config.minStreamingPageSize ? m_config.minStreamingPageSize : size;
	}
	uint32_t SelectPageSizeClassIndex(uint64_t payloadBytes) const;
	static constexpr uint32_t GetPageSizeClassCount() { return kPageSizeClassCount; }
	uint32_t GetClassPageSize(uint32_t classIndex) const { return m_config.pageSizes[classIndex]; }

	// Slab size in bytes.
	uint64_t GetSlabSize() const { return m_config.slabSize; }

	// Number of streaming slabs created up-front.
	uint32_t GetNumStreamingSlabs() const { return m_config.numStreamingSlabs; }

	uint32_t PageToSlabIndex(uint32_t globalPageID) const;

	// Compute the byte offset within a slab for a global page ID.
	uint64_t PageToSlabByteOffset(uint32_t globalPageID) const;

	// Get the GPU descriptor-heap index of the slab that the given
	// allocation lives in.  Returns 0 if the allocation is invalid.
	uint32_t GetSlabDescriptorIndex(const PageAllocation& alloc) const;

	// Refresh the GPU-side page table buffer from the CPU mirror.
	// Should be called once per frame after any alloc/free operations.
	void FlushPageTableUpdates();

	// Add a general slab for a class, respecting the shared slab cap. Returns
	// the page IDs added so the matching LRU can be populated.
	std::vector<uint32_t> GrowGeneralPageClass(uint32_t pageSizeBytes);
	std::vector<uint32_t> GetGeneralPageIDs(uint32_t pageSizeBytes) const;

	// Rebalancing between classes. Every general slab owns a fixed range of page IDs, enough for its size in the smallest
	// class, so a slab can change class without moving any other slab's IDs.
	struct GeneralSlabInfo {
		uint32_t slabIndex = 0;
		uint32_t firstPageID = 0;
		uint32_t pageCount = 0;
		uint32_t pageSize = 0;
	};
	std::vector<GeneralSlabInfo> GetGeneralSlabs() const;
	uint32_t GetGeneralSlabCount() const { return m_generalSlabCount; }
	// Streaming worker only: every page of the slab must be free and out of every LRU. Returns the slab's new page IDs.
	std::vector<uint32_t> ConvertGeneralSlab(uint32_t slabIndex, uint32_t pageSizeBytes);

	// Allocate/free pinned pages from per-class small slabs.
	std::vector<uint32_t> AllocatePinnedPages(std::span<const uint32_t> pageSizeBytes);
	std::vector<uint32_t> AllocatePinnedPages(uint32_t count);

	// Return pinned pages to the dedicated pinned-slab free list.
	void FreePinnedPages(const std::vector<uint32_t>& pageIDs);

	// Upload callback signature: (data, dataSize, target, dstOffset).
	using UploadFn = std::function<void(const void*, size_t, org::runtime::UploadTarget, size_t)>;

	// Install the generation-bound upload function used by UploadToPage and
	// FlushPageTableUpdates. Upload entry points fail if no owner is installed.
	void SetUploadFunction(UploadFn fn) { m_uploadFn = std::move(fn); }

private:
	enum class SlabRole : uint8_t {
		General,
		Pinned,
	};

	struct Slab {
		std::shared_ptr<org::Buffer> buffer; // The GPU ByteAddressBuffer.
		SlabRole role = SlabRole::General;
		uint32_t firstPageID = 0;
		uint32_t pageCount = 0;
		uint32_t pageSize = 0;
		uint32_t pageIdCapacity = 0; // IDs reserved from firstPageID; pageCount of them are in use.
	};

	Config     m_config;
	uint32_t   m_generalSlabCount = 0;

	// The streaming worker grows the slab table while other threads (graph
	// declaration, ray-tracing refresh) read it. Guards m_slabs and
	// m_totalPageCapacity; everything else is owned by the streaming worker.
	mutable std::shared_mutex m_slabMutex;
	uint32_t   m_totalPageCapacity = 0;
	std::vector<Slab> m_slabs;
	std::array<std::vector<uint32_t>, kPageSizeClassCount> m_freePinnedPageIDs;
	// CPU-side mirror of the page table: indexed by global page ID.
	std::vector<PageTableEntry> m_pageTableCpu;
	std::vector<uint32_t> m_pageSlabIndex; // Slab of each page ID; guarded by m_slabMutex.
	bool                        m_pageTableDirty = false;

	// GPU-side StructuredBuffer<PageTableEntry>.
	std::shared_ptr<org::Buffer> m_pageTableBuffer;

	// ResourceGroup tracking all slab buffers for render graph auto-invalidation.
	std::shared_ptr<org::ResourceGroup> m_slabResourceGroup;

	// Generation-bound upload function. Cleared when the streaming owner shuts down.
	UploadFn m_uploadFn;

	uint32_t PageToSlabIndexLocked(uint32_t globalPageID) const;

	// Allocate a new slab. Streaming slabs are capped by numStreamingSlabs.
	bool AllocateNewSlab(SlabRole role, uint32_t pageSizeBytes, std::vector<uint32_t>* outPageIDs = nullptr);

	// Update the page table CPU mirror entries for pages [firstGlobal, firstGlobal+count).
	void UpdatePageTableEntries(uint32_t firstGlobalPageID, uint32_t count);
};
