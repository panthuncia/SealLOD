#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Resources/Resource.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/ResourceGroup.h"
#include "Render/Runtime/UploadTypes.h"
#include "ShaderBuffers.h"

class GpuBufferBacking;

// Fixed-size page allocator backed by multiple GPU "slab" ByteAddressBuffers.
//
// - Each **slab** is a single D3D12 ByteAddressBuffer (e.g. 256 MB).
// - Each **page** is a fixed-size region within a slab (e.g. 64 KB).
// - Groups allocate one or more contiguous pages.
// - A GPU-visible **page table** maps virtual page IDs -> (slab, offset).
//
// Advantages over per-stream DynamicBuffer pools:
//   - No single D3D12 resource grows past the slab cap.
//   - No free-list fragmentation, pages are uniform.
//   - Adding a slab never copies existing data.
//   - Eviction is O(1)
class PagePool {
public:
	// Configuration for the page pool.
	struct Config {
		uint64_t pageSize     = 256 * 1024; // Bytes per page (default 256 KB).
		uint64_t slabSize     = 256 * 1024 * 1024; // Bytes per slab (default 256 MB).
		uint32_t numStreamingSlabs = 16; // General-purpose streaming slabs created up-front.
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
	std::shared_ptr<Buffer> GetSlab(uint32_t slabIndex) const;

	// Get the page-table Buffer (StructuredBuffer<PageTableEntry>).
	std::shared_ptr<Buffer> GetPageTableBuffer() const;

	// Get the ResourceGroup tracking all slab buffers (for render graph declarations).
	std::shared_ptr<ResourceGroup> GetSlabResourceGroup() const { return m_slabResourceGroup; }

	// Total pages across all slabs.
	uint32_t GetTotalPageCount() const;

	// Total pages across general-purpose slabs.
	uint32_t GetGeneralPageCount() const;

	// Page size in bytes.
	uint64_t GetPageSize() const { return m_config.pageSize; }

	// Slab size in bytes.
	uint64_t GetSlabSize() const { return m_config.slabSize; }

	// Number of streaming slabs created up-front.
	uint32_t GetNumStreamingSlabs() const { return m_config.numStreamingSlabs; }

	// Pages per slab.
	uint32_t GetPagesPerSlab() const { return m_pagesPerSlab; }

	// Compute the slab index for a global page ID.
	uint32_t PageToSlabIndex(uint32_t globalPageID) const {
		return globalPageID / m_pagesPerSlab;
	}

	// Compute the byte offset within a slab for a global page ID.
	uint64_t PageToSlabByteOffset(uint32_t globalPageID) const {
		return static_cast<uint64_t>(globalPageID % m_pagesPerSlab) * m_config.pageSize;
	}

	// Get the GPU descriptor-heap index of the slab that the given
	// allocation lives in.  Returns 0 if the allocation is invalid.
	uint32_t GetSlabDescriptorIndex(const PageAllocation& alloc) const;

	// Refresh the GPU-side page table buffer from the CPU mirror.
	// Should be called once per frame after any alloc/free operations.
	void FlushPageTableUpdates();

	// Allocate pages from dedicated pinned slabs. The returned pages do not
	// participate in the CLod eviction LRU.
	std::vector<uint32_t> AllocatePinnedPages(uint32_t count);

	// Return pinned pages to the dedicated pinned-slab free list.
	void FreePinnedPages(const std::vector<uint32_t>& pageIDs);

	// Upload callback signature: (data, dataSize, target, dstOffset).
	using UploadFn = std::function<void(const void*, size_t, rg::runtime::UploadTarget, size_t)>;

	// Override the upload function used by UploadToPage / FlushPageTableUpdates.
	// When not set, the default BUFFER_UPLOAD macro path is used.
	void SetUploadFunction(UploadFn fn) { m_uploadFn = std::move(fn); }

private:
	enum class SlabRole : uint8_t {
		General,
		Pinned,
	};

	struct Slab {
		std::shared_ptr<Buffer> buffer; // The GPU ByteAddressBuffer.
		SlabRole role = SlabRole::General;
	};

	Config     m_config;
	uint32_t   m_pagesPerSlab = 0;
	uint32_t   m_totalPageCapacity = 0;
	uint32_t   m_generalSlabCount = 0;

	std::vector<Slab> m_slabs;
	std::vector<uint32_t> m_freePinnedPageIDs;

	// CPU-side mirror of the page table: indexed by global page ID.
	std::vector<PageTableEntry> m_pageTableCpu;
	bool                        m_pageTableDirty = false;

	// GPU-side StructuredBuffer<PageTableEntry>.
	std::shared_ptr<Buffer> m_pageTableBuffer;

	// ResourceGroup tracking all slab buffers for render graph auto-invalidation.
	std::shared_ptr<ResourceGroup> m_slabResourceGroup;

	// Optional upload function override; when empty, falls back to BUFFER_UPLOAD.
	UploadFn m_uploadFn;

	// Allocate a new slab. Streaming slabs are capped by numStreamingSlabs.
	bool AllocateNewSlab(SlabRole role);

	// Update the page table CPU mirror entries for pages [firstGlobal, firstGlobal+count).
	void UpdatePageTableEntries(uint32_t firstGlobalPageID, uint32_t count);
};
