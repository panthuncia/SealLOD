#include "Resources/Buffers/PagePool.h"

#include <cassert>
#include <spdlog/spdlog.h>

#include "Render/MemoryIntrospectionAPI.h"
#include "Resources/Buffers/Buffer.h"
#include "Render/Runtime/UploadServiceAccess.h"

namespace {
	std::shared_ptr<Buffer> CreatePagePoolSlabBuffer(uint64_t byteSize, const std::string& name)
	{
		auto buffer = Buffer::CreateShared(rhi::HeapType::DeviceLocal, byteSize, false);
		buffer->SetName(name);

		BufferBase::DescriptorRequirements requirements{};
		requirements.createSRV = true;
		requirements.srvDesc = rhi::SrvDesc{
			.dimension = rhi::SrvDim::Buffer,
			.formatOverride = rhi::Format::R32_Typeless,
			.buffer = {
				.kind = rhi::BufferViewKind::Raw,
				.firstElement = 0,
				.numElements = static_cast<uint32_t>(byteSize / 4u),
				.structureByteStride = 0,
			},
		};
		buffer->SetDescriptorRequirements(requirements);
		return buffer;
	}

	std::shared_ptr<Buffer> CreatePageTableBuffer(uint32_t pageCount, const std::string& name)
	{
		auto buffer = Buffer::CreateUnmaterializedStructuredBuffer(
			pageCount,
			static_cast<uint32_t>(sizeof(PageTableEntry)),
			false,
			false,
			false,
			rhi::HeapType::DeviceLocal);
		buffer->SetName(name);
		buffer->Materialize();
		return buffer;
	}
}

// PagePool implementation
PagePool::PagePool(const Config& config)
	: m_config(config)
{
	assert(m_config.pageSize > 0 && (m_config.pageSize & (m_config.pageSize - 1)) == 0
		   && "pageSize must be a power of two");
	assert(m_config.slabSize >= m_config.pageSize);
	assert(m_config.numStreamingSlabs > 0);

	m_pagesPerSlab = static_cast<uint32_t>(m_config.slabSize / m_config.pageSize);

	// Create the page table buffer (initially empty, grows as slabs are added).
	m_pageTableBuffer = CreatePageTableBuffer(m_pagesPerSlab, m_config.debugName + "::PageTable");
	rg::memory::SetResourceUsageHint(*m_pageTableBuffer, "Cluster LOD page table");

	// Resource group for slab buffers (render graph auto-invalidation).
	m_slabResourceGroup = std::make_shared<ResourceGroup>(m_config.debugName + "::Slabs");

	for (uint32_t i = 0; i < m_config.numStreamingSlabs; ++i) {
		if (!AllocateNewSlab(SlabRole::General)) {
			spdlog::error("PagePool: failed to allocate streaming slab {}", i);
			break;
		}
	}

	spdlog::info("PagePool: initialized {} general slabs ({} total pages)",
		static_cast<uint32_t>(m_generalSlabCount), m_totalPageCapacity);
}

PagePool::~PagePool() = default;

// Slab management
bool PagePool::AllocateNewSlab(SlabRole role) {
	if (role == SlabRole::General && m_generalSlabCount >= m_config.numStreamingSlabs) {
		spdlog::error("PagePool: cannot allocate new streaming slab - numStreamingSlabs ({}) reached", m_config.numStreamingSlabs);
		return false;
	}

	const uint32_t slabIndex = static_cast<uint32_t>(m_slabs.size());
	Slab slab;
	slab.role = role;
	slab.buffer = CreatePagePoolSlabBuffer(
		m_config.slabSize,
		m_config.debugName + "::" + (role == SlabRole::Pinned ? "PinnedSlab" : "Slab") + std::to_string(slabIndex));
	rg::memory::SetResourceUsageHint(*slab.buffer, role == SlabRole::Pinned ? "Cluster LOD pinned page slabs" : "Cluster LOD page slabs");

	m_slabs.push_back(std::move(slab));
	m_totalPageCapacity += m_pagesPerSlab;
	if (role == SlabRole::General) {
		m_generalSlabCount++;
	}

	// Register new slab in the resource group for render graph tracking.
	m_slabResourceGroup->AddResource(m_slabs.back().buffer);

	// Extend the CPU page table mirror.
	const uint32_t oldCapacity = static_cast<uint32_t>(m_pageTableCpu.size());
	m_pageTableCpu.resize(m_totalPageCapacity, PageTableEntry{});

	// Fill page table entries for the new slab.
	const uint32_t firstGlobal = slabIndex * m_pagesPerSlab;
	for (uint32_t i = 0; i < m_pagesPerSlab; ++i) {
		auto& entry = m_pageTableCpu[firstGlobal + i];
		entry.slabIndex = slabIndex;
		entry.slabByteOffset = static_cast<uint32_t>(static_cast<uint64_t>(i) * m_config.pageSize);
	}

	m_pageTableDirty = true;

	spdlog::info("PagePool: allocated {} slab {} ({:.1f} MB, {} pages)",
				 role == SlabRole::Pinned ? "pinned" : "general",
				 slabIndex,
				 static_cast<double>(m_config.slabSize) / (1024.0 * 1024.0),
				 m_pagesPerSlab);
	return true;
}

// Upload
void PagePool::UploadToPage(uint32_t globalPageID, uint32_t intraPageByteOffset,
							const void* data, size_t dataSize) {
	const uint32_t si = PageToSlabIndex(globalPageID);
	assert(si < m_slabs.size());

	const uint64_t slabOffset = PageToSlabByteOffset(globalPageID) + intraPageByteOffset;
	assert(intraPageByteOffset + dataSize <= m_config.pageSize);

	auto& slab = m_slabs[si];
	auto target = rg::runtime::UploadTarget::FromShared(slab.buffer);
	if (m_uploadFn) {
		m_uploadFn(data, dataSize, target, slabOffset);
	} else {
		BUFFER_UPLOAD(data, dataSize, target, slabOffset);
	}
}

// Page table
void PagePool::UpdatePageTableEntries(uint32_t firstGlobalPageID, uint32_t count) {
	// The page table entries are already correct in m_pageTableCpu (set at slab creation).
	// This method exists for future use when pages might be remapped.
	(void)firstGlobalPageID;
	(void)count;
	m_pageTableDirty = true;
}

void PagePool::FlushPageTableUpdates() {
	if (!m_pageTableDirty || m_pageTableCpu.empty()) return;

	// Ensure the GPU-side page table buffer is large enough.
	// We re-upload the entire table for simplicity.
	const size_t tableBytes = m_pageTableCpu.size() * sizeof(PageTableEntry);

	if (m_pageTableBuffer->GetSize() < tableBytes) {
		// Recreate with a larger capacity
		m_pageTableBuffer = CreatePageTableBuffer(
			static_cast<uint32_t>(m_pageTableCpu.size()),
			m_config.debugName + "::PageTable");
		rg::memory::SetResourceUsageHint(*m_pageTableBuffer, "Cluster LOD page table");
	}

	auto target = rg::runtime::UploadTarget::FromShared(m_pageTableBuffer);
	if (m_uploadFn) {
		m_uploadFn(m_pageTableCpu.data(), tableBytes, target, 0);
	} else {
		BUFFER_UPLOAD(m_pageTableCpu.data(), tableBytes, target, 0);
	}

	m_pageTableDirty = false;
}

// Accessors
uint32_t PagePool::GetSlabCount() const {
	return static_cast<uint32_t>(m_slabs.size());
}

std::shared_ptr<Buffer> PagePool::GetSlab(uint32_t slabIndex) const {
	assert(slabIndex < m_slabs.size());
	return m_slabs[slabIndex].buffer;
}

uint32_t PagePool::GetSlabDescriptorIndex(const PageAllocation& alloc) const {
	if (!alloc.IsValid()) return 0u;
	const uint32_t si = PageToSlabIndex(alloc.firstPageID);
	assert(si < m_slabs.size());
	return m_slabs[si].buffer->GetSRVInfo(0).slot.index;
}

std::shared_ptr<Buffer> PagePool::GetPageTableBuffer() const {
	return m_pageTableBuffer;
}

uint32_t PagePool::GetTotalPageCount() const {
	return m_totalPageCapacity;
}

uint32_t PagePool::GetGeneralPageCount() const {
	return m_generalSlabCount * m_pagesPerSlab;
}

std::vector<uint32_t> PagePool::AllocatePinnedPages(uint32_t count) {
	std::vector<uint32_t> pageIDs;
	pageIDs.reserve(count);
	if (count == 0u) {
		return pageIDs;
	}

	while (m_freePinnedPageIDs.size() < count) {
		if (!AllocateNewSlab(SlabRole::Pinned)) {
			spdlog::error("PagePool: unable to grow pinned slab pool for {} requested pages", count);
			return {};
		}

		const uint32_t slabIndex = static_cast<uint32_t>(m_slabs.size() - 1u);
		const uint32_t firstGlobalPageID = slabIndex * m_pagesPerSlab;
		for (uint32_t pageOffset = 0; pageOffset < m_pagesPerSlab; ++pageOffset) {
			m_freePinnedPageIDs.push_back(firstGlobalPageID + pageOffset);
		}
	}

	for (uint32_t i = 0; i < count; ++i) {
		pageIDs.push_back(m_freePinnedPageIDs.back());
		m_freePinnedPageIDs.pop_back();
	}

	return pageIDs;
}

void PagePool::FreePinnedPages(const std::vector<uint32_t>& pageIDs) {
	for (uint32_t pageID : pageIDs) {
		const uint32_t slabIndex = PageToSlabIndex(pageID);
		if (slabIndex >= m_slabs.size()) {
			continue;
		}

		if (m_slabs[slabIndex].role != SlabRole::Pinned) {
			continue;
		}

		m_freePinnedPageIDs.push_back(pageID);
	}
}
