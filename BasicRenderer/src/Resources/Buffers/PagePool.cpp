#include "Resources/Buffers/PagePool.h"

#include <cassert>
#include <algorithm>
#include <stdexcept>
#include <spdlog/spdlog.h>

#include "Render/MemoryIntrospectionAPI.h"
#include "Resources/Buffers/Buffer.h"

namespace {
	std::shared_ptr<org::Buffer> CreatePagePoolSlabBuffer(uint64_t byteSize, const std::string& name)
	{
		auto buffer = org::Buffer::CreateShared(rhi::HeapType::DeviceLocal, byteSize, false);
		buffer->SetName(name);

		org::BufferBase::DescriptorRequirements requirements{};
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

	std::shared_ptr<org::Buffer> CreatePageTableBuffer(uint32_t pageCount, const std::string& name)
	{
		auto buffer = org::Buffer::CreateUnmaterializedStructuredBuffer(
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
	for (uint32_t pageSize : m_config.pageSizes) {
		assert(pageSize > 0u && (pageSize & (pageSize - 1u)) == 0u);
		assert(m_config.slabSize >= pageSize);
		assert(m_config.pinnedSlabSize >= pageSize);
	}
	assert(m_config.numStreamingSlabs > 0);

	m_pageTableBuffer = CreatePageTableBuffer(1u, m_config.debugName + "::PageTable");
	org::memory::SetResourceUsageHint(*m_pageTableBuffer, "Cluster LOD page table");

	// Resource group for slab buffers (render graph auto-invalidation).
	m_slabResourceGroup = std::make_shared<org::ResourceGroup>(m_config.debugName + "::Slabs");

	for (uint32_t classIndex = 0u;
		classIndex < static_cast<uint32_t>(m_config.pageSizes.size());
		++classIndex) {
		for (uint32_t slab = 0u;
			slab < m_config.initialStreamingSlabs[classIndex];
			++slab) {
			const uint32_t pageSize = m_config.pageSizes[classIndex];
			if (!AllocateNewSlab(SlabRole::General, pageSize)) {
				spdlog::error("PagePool: failed to allocate initial {} KiB streaming slab", pageSize / 1024u);
				break;
			}
		}
	}

	spdlog::info("PagePool: initialized {} general slabs ({} total pages)",
		static_cast<uint32_t>(m_generalSlabCount), m_totalPageCapacity);
}

PagePool::~PagePool() = default;

// Slab management
bool PagePool::AllocateNewSlab(
	SlabRole role,
	uint32_t pageSizeBytes,
	std::vector<uint32_t>* outPageIDs) {
	if (role == SlabRole::General && m_generalSlabCount >= m_config.numStreamingSlabs) {
		spdlog::error("PagePool: cannot allocate new streaming slab - numStreamingSlabs ({}) reached", m_config.numStreamingSlabs);
		return false;
	}

	// Only the streaming worker grows the table, so the index and first page
	// read here stay valid until the exclusive section below publishes the slab.
	uint32_t slabIndex = 0u;
	uint32_t firstGlobal = 0u;
	{
		std::shared_lock lock(m_slabMutex);
		slabIndex = static_cast<uint32_t>(m_slabs.size());
		firstGlobal = m_totalPageCapacity;
	}
	Slab slab;
	slab.role = role;
	slab.pageSize = pageSizeBytes;
	const uint64_t slabByteSize =
		role == SlabRole::Pinned ? m_config.pinnedSlabSize : m_config.slabSize;
	const uint32_t slabPageCount = static_cast<uint32_t>(slabByteSize / pageSizeBytes);
	slab.firstPageID = firstGlobal;
	slab.pageCount = slabPageCount;
	slab.buffer = CreatePagePoolSlabBuffer(
		slabByteSize,
		m_config.debugName + "::" +
			(role == SlabRole::Pinned ? "Pinned" : "Streaming") +
			std::to_string(pageSizeBytes / 1024u) + "KSlab" +
			std::to_string(slabIndex));
	org::memory::SetResourceUsageHint(*slab.buffer, role == SlabRole::Pinned ? "Cluster LOD pinned page slabs" : "Cluster LOD page slabs");

	auto slabBuffer = slab.buffer;
	{
		std::unique_lock lock(m_slabMutex);
		m_slabs.push_back(std::move(slab));
		m_totalPageCapacity += slabPageCount;
	}
	if (role == SlabRole::General) {
		m_generalSlabCount++;
	}

	// Register new slab in the resource group for render graph tracking.
	m_slabResourceGroup->AddResource(slabBuffer);

	// Extend the CPU page-table mirror and describe every physical page.
	m_pageTableCpu.resize(static_cast<size_t>(firstGlobal) + slabPageCount, PageTableEntry{});
	for (uint32_t i = 0; i < slabPageCount; ++i) {
		auto& entry = m_pageTableCpu[firstGlobal + i];
		entry.slabIndex = slabIndex;
		entry.slabByteOffset = i * pageSizeBytes;
	}

	// Return global physical page identifiers for the new slab.
	for (uint32_t i = 0; i < slabPageCount; ++i) {
		if (outPageIDs != nullptr) {
			outPageIDs->push_back(firstGlobal + i);
		}
	}

	m_pageTableDirty = true;

	spdlog::info("PagePool: allocated {} {} KiB slab {} ({:.1f} MB, {} pages)",
				 role == SlabRole::Pinned ? "pinned" : "general",
				 pageSizeBytes / 1024u,
				 slabIndex,
				 static_cast<double>(slabByteSize) / (1024.0 * 1024.0),
				 slabPageCount);
	return true;
}

// Upload
void PagePool::UploadToPage(uint32_t globalPageID, uint32_t intraPageByteOffset,
							const void* data, size_t dataSize) {
	std::shared_ptr<org::Buffer> slabBuffer;
	uint64_t slabOffset = 0u;
	{
		std::shared_lock lock(m_slabMutex);
		const uint32_t si = PageToSlabIndexLocked(globalPageID);
		if (si >= m_slabs.size()) {
			throw std::out_of_range("PagePool upload targets an unknown page");
		}
		assert(intraPageByteOffset + dataSize <= m_slabs[si].pageSize);
		slabOffset = static_cast<uint64_t>(globalPageID - m_slabs[si].firstPageID) * m_slabs[si].pageSize +
			intraPageByteOffset;
		slabBuffer = m_slabs[si].buffer;
	}
	auto target = org::runtime::UploadTarget::FromShared(slabBuffer);
	if (m_uploadFn) {
		m_uploadFn(data, dataSize, target, slabOffset);
	} else {
		throw std::runtime_error("PagePool upload owner is not configured");
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

	const size_t tableBytes = m_pageTableCpu.size() * sizeof(PageTableEntry);
	if (m_pageTableBuffer->GetSize() < tableBytes) {
		m_pageTableBuffer = CreatePageTableBuffer(
			static_cast<uint32_t>(m_pageTableCpu.size()),
			m_config.debugName + "::PageTable");
		org::memory::SetResourceUsageHint(*m_pageTableBuffer, "Cluster LOD page table");
	}

	auto target = org::runtime::UploadTarget::FromShared(m_pageTableBuffer);
	if (m_uploadFn) {
		m_uploadFn(m_pageTableCpu.data(), tableBytes, target, 0);
	} else {
		throw std::runtime_error("PagePool upload owner is not configured");
	}

	m_pageTableDirty = false;
}

// Accessors
uint32_t PagePool::GetSlabCount() const {
	std::shared_lock lock(m_slabMutex);
	return static_cast<uint32_t>(m_slabs.size());
}

std::shared_ptr<org::Buffer> PagePool::GetSlab(uint32_t slabIndex) const {
	std::shared_lock lock(m_slabMutex);
	return slabIndex < m_slabs.size() ? m_slabs[slabIndex].buffer : nullptr;
}

uint32_t PagePool::GetSlabDescriptorIndex(const PageAllocation& alloc) const {
	if (!alloc.IsValid()) return 0u;
	std::shared_lock lock(m_slabMutex);
	const uint32_t si = PageToSlabIndexLocked(alloc.firstPageID);
	if (si >= m_slabs.size()) return 0u;
	return m_slabs[si].buffer->GetSRVInfo(0).slot.index;
}

uint32_t PagePool::PageToSlabIndex(uint32_t globalPageID) const {
	std::shared_lock lock(m_slabMutex);
	return PageToSlabIndexLocked(globalPageID);
}

uint32_t PagePool::PageToSlabIndexLocked(uint32_t globalPageID) const {
	for (uint32_t slabIndex = 0; slabIndex < static_cast<uint32_t>(m_slabs.size()); ++slabIndex) {
		const Slab& slab = m_slabs[slabIndex];
		if (globalPageID >= slab.firstPageID &&
			globalPageID - slab.firstPageID < slab.pageCount) {
			return slabIndex;
		}
	}
	return UINT32_MAX;
}

uint64_t PagePool::PageToSlabByteOffset(uint32_t globalPageID) const {
	std::shared_lock lock(m_slabMutex);
	const uint32_t slabIndex = PageToSlabIndexLocked(globalPageID);
	if (slabIndex >= m_slabs.size()) return 0u;
	return static_cast<uint64_t>(globalPageID - m_slabs[slabIndex].firstPageID) *
		m_slabs[slabIndex].pageSize;
}

std::shared_ptr<org::Buffer> PagePool::GetPageTableBuffer() const {
	return m_pageTableBuffer;
}

uint32_t PagePool::GetTotalPageCount() const {
	std::shared_lock lock(m_slabMutex);
	return m_totalPageCapacity;
}

uint32_t PagePool::GetGeneralPageCount() const {
	std::shared_lock lock(m_slabMutex);
	uint32_t result = 0u;
	for (const Slab& slab : m_slabs) {
		if (slab.role == SlabRole::General) {
			result += slab.pageCount;
		}
	}
	return result;
}

uint32_t PagePool::SelectPageSize(uint64_t payloadBytes) const {
	return m_config.pageSizes[SelectPageSizeClassIndex(payloadBytes)];
}

uint32_t PagePool::SelectPageSizeClassIndex(uint64_t payloadBytes) const {
	for (uint32_t index = 0u; index < static_cast<uint32_t>(m_config.pageSizes.size()); ++index) {
		if (payloadBytes <= m_config.pageSizes[index]) {
			return index;
		}
	}
	return static_cast<uint32_t>(m_config.pageSizes.size() - 1u);
}

uint32_t PagePool::GetPageSize(uint32_t globalPageID) const {
	std::shared_lock lock(m_slabMutex);
	const uint32_t slabIndex = PageToSlabIndexLocked(globalPageID);
	return slabIndex < m_slabs.size() ? m_slabs[slabIndex].pageSize : 0u;
}

uint32_t PagePool::GetPageSizeClassIndex(uint32_t globalPageID) const {
	return SelectPageSizeClassIndex(GetPageSize(globalPageID));
}

std::vector<uint32_t> PagePool::GrowGeneralPageClass(uint32_t pageSizeBytes) {
	std::vector<uint32_t> pages;
	if (SelectPageSize(pageSizeBytes) != pageSizeBytes ||
		m_generalSlabCount >= m_config.numStreamingSlabs) {
		return pages;
	}
	AllocateNewSlab(SlabRole::General, pageSizeBytes, &pages);
	return pages;
}

std::vector<uint32_t> PagePool::GetGeneralPageIDs(uint32_t pageSizeBytes) const {
	std::shared_lock lock(m_slabMutex);
	std::vector<uint32_t> pages;
	for (const Slab& slab : m_slabs) {
		if (slab.role != SlabRole::General || slab.pageSize != pageSizeBytes) {
			continue;
		}
		for (uint32_t offset = 0u; offset < slab.pageCount; ++offset) {
			pages.push_back(slab.firstPageID + offset);
		}
	}
	return pages;
}

std::vector<uint32_t> PagePool::AllocatePinnedPages(std::span<const uint32_t> pageSizeBytes) {
	std::vector<uint32_t> pageIDs;
	pageIDs.reserve(pageSizeBytes.size());
	for (uint32_t requestedBytes : pageSizeBytes) {
		const uint32_t classIndex = SelectPageSizeClassIndex(requestedBytes);
		auto& freePages = m_freePinnedPageIDs[classIndex];
		if (freePages.empty()) {
			std::vector<uint32_t> newPages;
			if (!AllocateNewSlab(SlabRole::Pinned, m_config.pageSizes[classIndex], &newPages)) {
				FreePinnedPages(pageIDs);
				return {};
			}
			freePages.insert(freePages.end(), newPages.begin(), newPages.end());
		}
		pageIDs.push_back(freePages.back());
		freePages.pop_back();
	}
	return pageIDs;
}

std::vector<uint32_t> PagePool::AllocatePinnedPages(uint32_t count) {
	std::vector<uint32_t> sizes(count, m_config.pageSizes.back());
	return AllocatePinnedPages(sizes);
}

void PagePool::FreePinnedPages(const std::vector<uint32_t>& pageIDs) {
	for (uint32_t pageID : pageIDs) {
		uint32_t pageSize = 0u;
		{
			std::shared_lock lock(m_slabMutex);
			const uint32_t slabIndex = PageToSlabIndexLocked(pageID);
			if (slabIndex >= m_slabs.size() || m_slabs[slabIndex].role != SlabRole::Pinned) {
				continue;
			}
			pageSize = m_slabs[slabIndex].pageSize;
		}
		m_freePinnedPageIDs[SelectPageSizeClassIndex(pageSize)].push_back(pageID);
	}
}
