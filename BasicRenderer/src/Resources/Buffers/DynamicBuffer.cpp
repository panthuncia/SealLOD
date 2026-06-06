#include "Resources/Buffers/DynamicBuffer.h"

#include <cstddef>
#include <chrono>

#include <spdlog/spdlog.h>

#include "Resources/Buffers/BufferView.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Resources/GPUBacking/GpuBufferBacking.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "Render/Runtime/UploadPolicyServiceAccess.h"

std::unique_ptr<BufferView> DynamicBuffer::Allocate(size_t size, size_t elementSize) {
    std::lock_guard lock(m_allocationMutex);
	size_t requiredSize = size;

	// Search for a free block using the size-indexed set - O(log n)
	auto freeIt = m_freeBlocks.lower_bound({ requiredSize, 0 });
	if (freeIt != m_freeBlocks.end())
	{
		size_t blockOffset = freeIt->second;

		// Remove from free index
		m_freeBlocks.erase(freeIt);

		// Update block in the offset map
		auto& block = m_blocksByOffset[blockOffset];
		size_t remainingSize = block.size - requiredSize;

		block.isFree = false;
		block.size = requiredSize;

		if (remainingSize > 0)
		{
			// Split the block
			size_t newOffset = blockOffset + requiredSize;
			m_blocksByOffset[newOffset] = { newOffset, remainingSize, true };
			m_freeBlocks.insert({ remainingSize, newOffset });
		}

		// Cache the weak pointer to avoid repeated dynamic_pointer_cast
		if (!m_weakPtrCached) {
			m_cachedWeakPtr = std::weak_ptr(
				std::dynamic_pointer_cast<DynamicBuffer>(Resource::weak_from_this().lock())
			);
			m_weakPtrCached = true;
		}
        // Return BufferView
        return BufferView::CreateUnique(m_cachedWeakPtr, blockOffset, requiredSize, elementSize);
	}

    if (PublishReadyAsyncResizeLocked(false)) {
        return Allocate(size, elementSize);
    }

	// No suitable block found, need to grow the buffer
    spdlog::info(
        "DynamicBuffer '{}' id={} growing allocation request={} elementSize={} currentCapacity={}",
        m_name,
        GetGlobalResourceID(),
        requiredSize,
        elementSize,
        m_capacity);

	// Absorb the last block if it is free
    size_t previousCapacity = m_capacity;
	size_t newBlockSize = (std::max)(m_capacity, requiredSize);
	size_t growBy = newBlockSize;
    size_t newBlockOffset = previousCapacity;
	if (!m_blocksByOffset.empty())
	{
		auto lastIt = std::prev(m_blocksByOffset.end());
		if (lastIt->second.isFree)
		{
            newBlockOffset = lastIt->second.offset;
			growBy -= lastIt->second.size;
			m_freeBlocks.erase({ lastIt->second.size, lastIt->second.offset });
			m_blocksByOffset.erase(lastIt);
		}
	}
    size_t newCapacity = DynamicBuffer::AlignBufferCapacity(previousCapacity + growBy, m_byteAddress);

	GrowBuffer(newCapacity);
    size_t trackedFreeSize = m_capacity - newBlockOffset;
    m_blocksByOffset[newBlockOffset] = { newBlockOffset, trackedFreeSize, true };
    m_freeBlocks.insert({ trackedFreeSize, newBlockOffset });
	spdlog::debug("Growing buffer to {} bytes", newCapacity);
	// Try allocating again
    return Allocate(size, elementSize);
}

void DynamicBuffer::ReserveBytes(size_t size) {
    std::lock_guard lock(m_allocationMutex);
    if (size == 0) {
        return;
    }

    if (m_freeBlocks.lower_bound({ size, 0 }) != m_freeBlocks.end()) {
        return;
    }

    (void)PublishReadyAsyncResizeLocked(false);
    if (m_freeBlocks.lower_bound({ size, 0 }) != m_freeBlocks.end()) {
        return;
    }

    auto view = Allocate(size, m_elementSize);
    Deallocate(view.get());
}

size_t DynamicBuffer::ComputeReserveCapacityLocked(size_t size) const {
    if (size == 0 || m_freeBlocks.lower_bound({ size, 0 }) != m_freeBlocks.end()) {
        return m_capacity;
    }

    const size_t previousCapacity = m_capacity;
    size_t newBlockSize = (std::max)(m_capacity, size);
    size_t growBy = newBlockSize;
    if (!m_blocksByOffset.empty()) {
        auto lastIt = std::prev(m_blocksByOffset.end());
        if (lastIt->second.isFree) {
            growBy -= lastIt->second.size;
        }
    }
    return DynamicBuffer::AlignBufferCapacity(previousCapacity + growBy, m_byteAddress);
}

void DynamicBuffer::RequestAsyncReserveBytes(size_t size) {
    std::lock_guard lock(m_allocationMutex);
    const size_t requestedCapacity = ComputeReserveCapacityLocked(size);
    if (requestedCapacity <= m_capacity ||
        (m_pendingResizeValid && m_pendingResizeCapacity >= requestedCapacity)) {
        return;
    }

    if (m_pendingResizeValid) {
        if (BufferBase::IsBackingMutationAllowedOnThisThread()) {
            (void)PublishReadyAsyncResizeLocked(false);
        }
        if (m_pendingResizeValid && m_pendingResizeCapacity >= requestedCapacity) {
            return;
        }
        if (m_pendingResizeValid) {
            spdlog::debug(
                "DynamicBuffer '{}' id={} async resize request deferred because pending resize capacity={} requestedCapacity={} mutationAllowed={}",
                m_name,
                GetGlobalResourceID(),
                m_pendingResizeCapacity,
                requestedCapacity,
                BufferBase::IsBackingMutationAllowedOnThisThread());
            return;
        }
    }

    const auto resourceID = GetGlobalResourceID();
    const bool unorderedAccess = m_UAV;
    const auto bufferName = m_name;
    m_pendingResizeCapacity = requestedCapacity;
    m_pendingResizeValid = true;
    m_pendingResizeFuture = std::async(std::launch::async, [resourceID, unorderedAccess, requestedCapacity, bufferName]() {
        spdlog::debug(
            "DynamicBuffer '{}' id={} async resize backing create begin capacity={}",
            bufferName,
            resourceID,
            requestedCapacity);
        auto backing = GpuBufferBacking::CreateUnique(
            rhi::HeapType::DeviceLocal,
            requestedCapacity,
            resourceID,
            unorderedAccess);
        spdlog::debug(
            "DynamicBuffer '{}' id={} async resize backing create complete capacity={}",
            bufferName,
            resourceID,
            requestedCapacity);
        return backing;
    });
}

bool DynamicBuffer::PublishReadyAsyncResize(bool wait) {
    std::lock_guard lock(m_allocationMutex);
    return PublishReadyAsyncResizeLocked(wait);
}

bool DynamicBuffer::PublishReadyAsyncResizeLocked(bool wait) {
    if (!m_pendingResizeValid) {
        return false;
    }
    if (!wait &&
        m_pendingResizeFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return false;
    }

    auto newBacking = m_pendingResizeFuture.get();
    const size_t newCapacity = m_pendingResizeCapacity;
    m_pendingResizeCapacity = 0;
    m_pendingResizeValid = false;
    if (!newBacking || newCapacity <= m_capacity) {
        return false;
    }

    const size_t previousCapacity = m_capacity;
    ApplyResizeBackingLocked(std::move(newBacking), newCapacity, previousCapacity);
    size_t newBlockOffset = previousCapacity;
    if (!m_blocksByOffset.empty()) {
        auto lastIt = std::prev(m_blocksByOffset.end());
        if (lastIt->second.isFree) {
            newBlockOffset = lastIt->second.offset;
            m_freeBlocks.erase({ lastIt->second.size, lastIt->second.offset });
            m_blocksByOffset.erase(lastIt);
        }
    }
    const size_t trackedFreeSize = m_capacity - newBlockOffset;
    if (trackedFreeSize != 0) {
        m_blocksByOffset[newBlockOffset] = { newBlockOffset, trackedFreeSize, true };
        m_freeBlocks.insert({ trackedFreeSize, newBlockOffset });
    }
    return true;
}

std::vector<std::shared_ptr<BufferView>> DynamicBuffer::AddDataBatch(const void* data, size_t count, size_t elementSize) {
    std::lock_guard lock(m_allocationMutex);
    std::vector<std::shared_ptr<BufferView>> views;
    if (count == 0 || elementSize == 0) {
        return views;
    }

    const size_t totalSize = count * elementSize;
    if (!m_weakPtrCached) {
        m_cachedWeakPtr = std::weak_ptr(
            std::dynamic_pointer_cast<DynamicBuffer>(Resource::weak_from_this().lock())
        );
        m_weakPtrCached = true;
    }

    auto allocateFromBlock = [&](size_t blockOffset) {
        views.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const size_t itemOffset = blockOffset + i * elementSize;
            m_blocksByOffset[itemOffset] = { itemOffset, elementSize, false };
            views.push_back(BufferView::CreateShared(m_cachedWeakPtr, itemOffset, elementSize, elementSize));
        }
    };

    auto freeIt = m_freeBlocks.lower_bound({ totalSize, 0 });
    if (freeIt == m_freeBlocks.end()) {
        ReserveBytes(totalSize);
        freeIt = m_freeBlocks.lower_bound({ totalSize, 0 });
    }
    if (freeIt == m_freeBlocks.end()) {
        return views;
    }

    const size_t blockOffset = freeIt->second;
    m_freeBlocks.erase(freeIt);
    auto blockIt = m_blocksByOffset.find(blockOffset);
    const size_t blockSize = blockIt != m_blocksByOffset.end() ? blockIt->second.size : totalSize;
    if (blockIt != m_blocksByOffset.end()) {
        m_blocksByOffset.erase(blockIt);
    }

    allocateFromBlock(blockOffset);

    if (blockSize > totalSize) {
        const size_t remainingOffset = blockOffset + totalSize;
        const size_t remainingSize = blockSize - totalSize;
        m_blocksByOffset[remainingOffset] = { remainingOffset, remainingSize, true };
        m_freeBlocks.insert({ remainingSize, remainingOffset });
    }

    if (data != nullptr) {
        StageOrUpload(data, totalSize, blockOffset);
    }

    return views;
}

std::pair<size_t, size_t> DynamicBuffer::AddDataRange(const void* data, size_t count, size_t elementSize) {
    std::lock_guard lock(m_allocationMutex);
    if (count == 0 || elementSize == 0) {
        return { 0, 0 };
    }

    const size_t totalSize = count * elementSize;
    auto freeIt = m_freeBlocks.lower_bound({ totalSize, 0 });
    if (freeIt == m_freeBlocks.end()) {
        ReserveBytes(totalSize);
        freeIt = m_freeBlocks.lower_bound({ totalSize, 0 });
    }
    if (freeIt == m_freeBlocks.end()) {
        return { 0, 0 };
    }

    const size_t blockOffset = freeIt->second;
    m_freeBlocks.erase(freeIt);
    auto blockIt = m_blocksByOffset.find(blockOffset);
    const size_t blockSize = blockIt != m_blocksByOffset.end() ? blockIt->second.size : totalSize;
    if (blockIt != m_blocksByOffset.end()) {
        m_blocksByOffset.erase(blockIt);
    }

    m_blocksByOffset[blockOffset] = { blockOffset, totalSize, false };

    if (blockSize > totalSize) {
        const size_t remainingOffset = blockOffset + totalSize;
        const size_t remainingSize = blockSize - totalSize;
        m_blocksByOffset[remainingOffset] = { remainingOffset, remainingSize, true };
        m_freeBlocks.insert({ remainingSize, remainingOffset });
    }

    if (data != nullptr) {
        StageOrUpload(data, totalSize, blockOffset);
    }

    return { blockOffset, totalSize };
}

std::vector<DynamicBuffer::PagedAllocation> DynamicBuffer::AddDataPaged(
    const void* data,
    size_t count,
    size_t elementSize,
    size_t pageElementCount)
{
    std::lock_guard lock(m_allocationMutex);
    std::vector<PagedAllocation> pages;
    if (count == 0 || elementSize == 0) {
        return pages;
    }

    if (pageElementCount == 0) {
        pageElementCount = 1;
    }

    const auto* bytes = static_cast<const std::byte*>(data);
    size_t remaining = count;
    size_t elementCursor = 0;
    pages.reserve((count + pageElementCount - 1) / pageElementCount);

    while (remaining != 0) {
        const size_t pageCount = (std::min)(remaining, pageElementCount);
        const size_t usedSize = pageCount * elementSize;
        const size_t allocationSize = usedSize;
        auto view = Allocate(allocationSize, elementSize);
        if (!view) {
            break;
        }

        const size_t offset = view->GetOffset();
        if (bytes != nullptr) {
            StageOrUpload(bytes + elementCursor * elementSize, usedSize, offset);
        }

        pages.push_back(PagedAllocation{
            offset,
            usedSize,
            allocationSize,
            elementSize,
            pageCount
        });

        elementCursor += pageCount;
        remaining -= pageCount;
    }

    return pages;
}

std::unique_ptr<BufferView> DynamicBuffer::AddData(const void* data, size_t size, size_t elementSize, size_t fullAllocationSize) {
    std::lock_guard lock(m_allocationMutex);
	size_t actualSize = size;
    if (fullAllocationSize != 0) {
		actualSize = fullAllocationSize;
		if (actualSize < size) {
			spdlog::warn("Full allocation size is smaller than the data size. Using data size instead.");
			actualSize = size;
		}
    }
    std::unique_ptr<BufferView> view = Allocate(actualSize, elementSize);
    
	if (data != nullptr) {
        StageOrUpload(data, size, view->GetOffset());
	}

    return view;
}

void DynamicBuffer::UpdateView(BufferView* view, const void* data) {
    StageOrUpload(data, view->GetSize(), view->GetOffset());
}

void DynamicBuffer::StageOrUpload(const void* data, size_t size, size_t offset) {
    std::lock_guard<std::recursive_mutex> uploadLock(m_uploadPolicyMirrorMutex);
    if (rg::runtime::GetActiveUploadPolicyService() == nullptr) {
        SyncUploadPolicyState();
#if BUILD_TYPE == BUILD_TYPE_DEBUG
        m_uploadPolicyState.StageWrite(data, size, offset, GetBufferSize(), __FILE__, __LINE__);
#else
        m_uploadPolicyState.StageWrite(data, size, offset, GetBufferSize());
#endif
        BUFFER_UPLOAD(data, size, rg::runtime::UploadTarget::FromShared(shared_from_this()), offset);
        return;
    }

    SyncUploadPolicyState();
    EnsureUploadPolicyRegistration();

#if BUILD_TYPE == BUILD_TYPE_DEBUG
    const bool staged = m_uploadPolicyState.StageWrite(data, size, offset, GetBufferSize(), __FILE__, __LINE__);
#else
    const bool staged = m_uploadPolicyState.StageWrite(data, size, offset, GetBufferSize());
#endif
    if (staged) {
        MarkUploadPolicyDirty();
        return;
    }

    BUFFER_UPLOAD(data, size, rg::runtime::UploadTarget::FromShared(shared_from_this()), offset);
}

void DynamicBuffer::Deallocate(const BufferView* view) {
    if (view == nullptr) {
        return;
    }

    DeallocateRange(view->GetOffset(), view->GetSize());
}

void DynamicBuffer::DeallocateRange(size_t offset, size_t size) {
    std::lock_guard lock(m_allocationMutex);
    if (size == 0) {
        return;
    }

    // Find the block by offset - O(log n)
    auto it = m_blocksByOffset.find(offset);
    if (it == m_blocksByOffset.end() || it->second.size != size || it->second.isFree)
    {
        return;
    }

    it->second.isFree = true;

    // Coalesce with next block if free
    auto nextIt = std::next(it);
    if (nextIt != m_blocksByOffset.end() && nextIt->second.isFree)
    {
        m_freeBlocks.erase({ nextIt->second.size, nextIt->second.offset });
        it->second.size += nextIt->second.size;
        m_blocksByOffset.erase(nextIt);
    }

    // Coalesce with previous block if free
    if (it != m_blocksByOffset.begin())
    {
        auto prevIt = std::prev(it);
        if (prevIt->second.isFree)
        {
            m_freeBlocks.erase({ prevIt->second.size, prevIt->second.offset });
            prevIt->second.size += it->second.size;
            m_blocksByOffset.erase(it);
            it = prevIt;
        }
    }

    // Add the (possibly coalesced) block to the free index
    m_freeBlocks.insert({ it->second.size, it->second.offset });
}

void DynamicBuffer::DeallocatePages(const std::vector<PagedAllocation>& pages) {
    for (const auto& page : pages) {
        if (!page.IsValid()) {
            continue;
        }
        DeallocateRange(page.offset, page.allocationSize);
    }
}

void DynamicBuffer::AssignDescriptorSlots()
{
    BufferBase::DescriptorRequirements requirements{};

    const uint32_t viewElements =
        static_cast<uint32_t>(m_byteAddress ? (m_capacity / 4) : m_capacity/m_elementSize);

    requirements.createCBV = false;
    requirements.createSRV = true;
    requirements.createUAV = m_UAV;
    requirements.createNonShaderVisibleUAV = false;
    requirements.uavCounterOffset = 0;

    // SRV
    requirements.srvDesc = rhi::SrvDesc{
        .dimension = rhi::SrvDim::Buffer,
        .formatOverride = m_byteAddress ? rhi::Format::R32_Typeless : rhi::Format::Unknown,
        .buffer = {
            .kind = m_byteAddress ? rhi::BufferViewKind::Raw : rhi::BufferViewKind::Structured,
            .firstElement = 0,
            .numElements = viewElements,
            .structureByteStride = static_cast<uint32_t>(m_byteAddress ? 0 : m_elementSize),
        },
    };

    // UAV
    requirements.uavDesc = rhi::UavDesc{
        .dimension = rhi::UavDim::Buffer,
        .buffer = {
            .kind = m_byteAddress ? rhi::BufferViewKind::Raw : rhi::BufferViewKind::Structured,
            .firstElement = 0,
            .numElements = viewElements,
            .structureByteStride = static_cast<uint32_t>(m_byteAddress ? 0 : m_elementSize),
        },
    };

    SetDescriptorRequirements(requirements);
}

void DynamicBuffer::CreateBuffer(size_t capacity) {
    std::lock_guard lock(m_allocationMutex);
	auto device = DeviceManager::GetInstance().GetDevice();
	m_capacity = capacity;
	auto newDataBuffer = GpuBufferBacking::CreateUnique(rhi::HeapType::DeviceLocal, capacity, GetGlobalResourceID(), m_UAV);
	SetBacking(std::move(newDataBuffer), capacity);
    {
        std::lock_guard<std::recursive_mutex> uploadLock(m_uploadPolicyMirrorMutex);
        SyncUploadPolicyState();
        m_uploadPolicyState.OnBufferResized(GetBufferSize());
    }
	m_blocksByOffset[0] = { 0, capacity, true };
	m_freeBlocks.insert({ capacity, 0 });

	for (const auto& bundle : m_metadataBundles) {
		ApplyMetadataToBacking(bundle);
	}

	AssignDescriptorSlots();
}

void DynamicBuffer::GrowBuffer(size_t newSize) {
    std::lock_guard lock(m_allocationMutex);
    const size_t previousCapacity = m_capacity;
    spdlog::info(
        "DynamicBuffer '{}' id={} GrowBuffer begin oldCapacity={} newCapacity={} hasBacking={}",
        m_name,
        GetGlobalResourceID(),
        previousCapacity,
        newSize,
        m_dataBuffer != nullptr);
    auto device = DeviceManager::GetInstance().GetDevice();
    spdlog::info(
        "DynamicBuffer '{}' id={} GrowBuffer creating new GPU backing",
        m_name,
        GetGlobalResourceID());
    auto newDataBuffer = GpuBufferBacking::CreateUnique(rhi::HeapType::DeviceLocal, newSize, GetGlobalResourceID(), m_UAV);
    spdlog::info(
        "DynamicBuffer '{}' id={} GrowBuffer created new GPU backing",
        m_name,
        GetGlobalResourceID());
	ApplyResizeBackingLocked(std::move(newDataBuffer), newSize, previousCapacity);
}

void DynamicBuffer::ApplyResizeBackingLocked(std::unique_ptr<GpuBufferBacking> newDataBuffer, size_t newSize, size_t previousCapacity) {
	spdlog::info(
		"DynamicBuffer '{}' id={} GrowBuffer SetBacking begin",
		m_name,
		GetGlobalResourceID());
	SetBacking(std::move(newDataBuffer), newSize);
    spdlog::info(
        "DynamicBuffer '{}' id={} GrowBuffer SetBacking complete bufferSize={} backingGeneration={} ",
        m_name,
        GetGlobalResourceID(),
        GetBufferSize(),
        GetBackingGeneration());
    {
        std::lock_guard<std::recursive_mutex> uploadLock(m_uploadPolicyMirrorMutex);
        SyncUploadPolicyState();
        m_uploadPolicyState.OnBufferResized(GetBufferSize());
        if (previousCapacity > 0u) {
            // DynamicBuffer is CPU-authoritative under CoalescedRetained. Preserve
            // logical bytes through resize by uploading the retained mirror into the
            // new backing; do not copy from the old GPU backing because descriptor
            // updates are immediate on DX12 and prior frames may still reference it.
            m_uploadPolicyState.CommitBulkRegion(0u, previousCapacity);
            if (m_uploadPolicyState.HasPendingWork()) {
                if (rg::runtime::GetActiveUploadService() != nullptr) {
                    m_uploadPolicyState.FlushToUploadService(rg::runtime::UploadTarget::FromShared(shared_from_this()));
                    const auto replayStats = m_uploadPolicyState.GetLastFlushStats();
                    spdlog::debug(
                        "DynamicBuffer '{}' id={} GrowBuffer replayed retained bytes writes={} bytes={}",
                        m_name,
                        GetGlobalResourceID(),
                        replayStats.flushedWrites,
                        replayStats.flushedBytes);
                } else {
                    MarkUploadPolicyDirty();
                }
            }
        }
    }

    m_capacity = newSize;

    for (const auto& bundle : m_metadataBundles) {
        ApplyMetadataToBacking(bundle);
    }

    spdlog::info(
        "DynamicBuffer '{}' id={} GrowBuffer AssignDescriptorSlots begin",
        m_name,
        GetGlobalResourceID());
    AssignDescriptorSlots();
    spdlog::info(
        "DynamicBuffer '{}' id={} GrowBuffer AssignDescriptorSlots complete",
        m_name,
        GetGlobalResourceID());

	SetName(m_name);
    spdlog::info(
        "DynamicBuffer '{}' id={} GrowBuffer complete finalCapacity={}",
        m_name,
        GetGlobalResourceID(),
        m_capacity);
}
