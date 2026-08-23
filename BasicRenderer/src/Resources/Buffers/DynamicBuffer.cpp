#include "Resources/Buffers/DynamicBuffer.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>

#include <spdlog/spdlog.h>
#include <BasicTelemetry/Tracy.h>

#include "Resources/Buffers/BufferView.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Resources/GPUBacking/GpuBufferBacking.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "Render/Runtime/UploadPolicyServiceAccess.h"

namespace {

size_t TotalAllocationProbeSize(const std::vector<size_t>& counts, size_t elementSize)
{
    if (counts.empty() || elementSize == 0) {
        return 0;
    }

    size_t totalCount = 0;
    for (const auto count : counts) {
        totalCount += count;
    }
    return totalCount * elementSize;
}

}

DynamicBuffer::~DynamicBuffer() {
    UnregisterDeferredBackingResizeClient(this);
}

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

    if (BufferBase::IsBackingMutationAllowedOnThisThread() &&
        PublishReadyAsyncResizeLocked(false)) {
        return Allocate(size, elementSize);
    }

    if (!BufferBase::IsBackingMutationAllowedOnThisThread()) {
        const size_t previousCapacity = m_capacity;
        RequestAsyncReserveBytesLocked(requiredSize);
        const size_t desiredLogicalCapacity = (std::max)(m_pendingResizeCapacity, m_requestedResizeCapacity);
        if (m_pendingResizeValid &&
            desiredLogicalCapacity > previousCapacity &&
            ExtendTrackedCapacityLocked(desiredLogicalCapacity)) {
            return Allocate(size, elementSize);
        }
        return nullptr;
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

    if (BufferBase::IsBackingMutationAllowedOnThisThread()) {
        (void)PublishReadyAsyncResizeLocked(false);
    }
    if (m_freeBlocks.lower_bound({ size, 0 }) != m_freeBlocks.end()) {
        return;
    }

    if (!BufferBase::IsBackingMutationAllowedOnThisThread()) {
        const size_t previousCapacity = m_capacity;
        RequestAsyncReserveBytesLocked(size);
        const size_t desiredLogicalCapacity = (std::max)(m_pendingResizeCapacity, m_requestedResizeCapacity);
        if (m_pendingResizeValid && desiredLogicalCapacity > previousCapacity) {
            (void)ExtendTrackedCapacityLocked(desiredLogicalCapacity);
        }
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

bool DynamicBuffer::ExtendTrackedCapacityLocked(size_t newCapacity) {
    if (newCapacity <= m_capacity) {
        return false;
    }

    const size_t previousCapacity = m_capacity;
    size_t newBlockOffset = previousCapacity;
    if (!m_blocksByOffset.empty()) {
        auto lastIt = std::prev(m_blocksByOffset.end());
        if (lastIt->second.isFree) {
            newBlockOffset = lastIt->second.offset;
            m_freeBlocks.erase({ lastIt->second.size, lastIt->second.offset });
            m_blocksByOffset.erase(lastIt);
        }
    }

    m_capacity = newCapacity;
    const size_t trackedFreeSize = m_capacity - newBlockOffset;
    if (trackedFreeSize != 0) {
        m_blocksByOffset[newBlockOffset] = { newBlockOffset, trackedFreeSize, true };
        m_freeBlocks.insert({ trackedFreeSize, newBlockOffset });
    }
    TracyPlot("DynamicBuffer.Resize.LogicalCapacityBytes", static_cast<int64_t>(m_capacity));
    spdlog::debug(
        "DynamicBuffer '{}' id={} extended logical allocation capacity oldCapacity={} newCapacity={} backingSize={}",
        m_name,
        GetGlobalResourceID(),
        previousCapacity,
        newCapacity,
        GetBufferSize());
    return true;
}

void DynamicBuffer::RequestAsyncReserveBytes(size_t size) {
    BT_ZONE_SCOPE("DynamicBuffer::RequestAsyncReserveBytes");
    BT_ZONE_TEXT(m_name.data(), m_name.size());
    TracyPlot("DynamicBuffer.RequestAsyncReserveBytes.RequestBytes", static_cast<int64_t>(size));
    if (size == 0) {
        return;
    }

    std::unique_lock<std::recursive_mutex> lock(m_allocationMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        BT_ZONE_SCOPE("DynamicBuffer::RequestAsyncReserveBytes::DeferredAllocationLockBusy");
        BT_ZONE_TEXT(m_name.data(), m_name.size());
        RaiseDeferredAsyncReserveBytes(size);
        TracyPlot("DynamicBuffer.RequestAsyncReserveBytes.DeferredLockBusy", int64_t{ 1 });
        return;
    }
    TracyPlot("DynamicBuffer.RequestAsyncReserveBytes.DeferredLockBusy", int64_t{ 0 });

    const size_t deferredSize = ConsumeDeferredAsyncReserveBytes();
    TracyPlot("DynamicBuffer.RequestAsyncReserveBytes.DrainedDeferredBytes", static_cast<int64_t>(deferredSize));
    RequestAsyncReserveBytesLocked((std::max)(size, deferredSize));
}

void DynamicBuffer::RequestAsyncReserveBytesLocked(size_t size) {
    BT_ZONE_SCOPE("DynamicBuffer::RequestAsyncReserveBytesLocked");
    BT_ZONE_TEXT(m_name.data(), m_name.size());
    TracyPlot("DynamicBuffer.RequestAsyncReserveBytesLocked.InputBytes", static_cast<int64_t>(size));
    TracyPlot("DynamicBuffer.RequestAsyncReserveBytesLocked.LogicalCapacityBytes", static_cast<int64_t>(m_capacity));
    TracyPlot("DynamicBuffer.RequestAsyncReserveBytesLocked.BackingSizeBytes", static_cast<int64_t>(GetBufferSize()));
    TracyPlot("DynamicBuffer.RequestAsyncReserveBytesLocked.PendingResizeValid", m_pendingResizeValid ? int64_t{ 1 } : int64_t{ 0 });
    TracyPlot("DynamicBuffer.RequestAsyncReserveBytesLocked.PendingResizeCapacityBytes", static_cast<int64_t>(m_pendingResizeCapacity));
    TracyPlot("DynamicBuffer.RequestAsyncReserveBytesLocked.RequestedResizeCapacityBytes", static_cast<int64_t>(m_requestedResizeCapacity));

    size_t requestedCapacity = 0;
    {
        BT_ZONE_SCOPE("DynamicBuffer::RequestAsyncReserveBytesLocked::ComputeReserveCapacity");
        BT_ZONE_TEXT(m_name.data(), m_name.size());
        requestedCapacity = ComputeReserveCapacityLocked(size);
    }
    const size_t desiredBackingCapacity = (std::max)(requestedCapacity, m_capacity);
    TracyPlot("DynamicBuffer.RequestAsyncReserveBytesLocked.ComputedRequestedCapacityBytes", static_cast<int64_t>(requestedCapacity));
    TracyPlot("DynamicBuffer.RequestAsyncReserveBytesLocked.DesiredBackingCapacityBytes", static_cast<int64_t>(desiredBackingCapacity));
    if (desiredBackingCapacity <= static_cast<size_t>(GetBufferSize()) ||
        (m_pendingResizeValid && m_pendingResizeCapacity >= desiredBackingCapacity)) {
        BT_ZONE_SCOPE("DynamicBuffer::RequestAsyncReserveBytesLocked::NoResizeNeeded");
        BT_ZONE_TEXT(m_name.data(), m_name.size());
        TracyPlot("DynamicBuffer.RequestAsyncReserveBytesLocked.NoResizeNeeded", int64_t{ 1 });
        return;
    }
    TracyPlot("DynamicBuffer.RequestAsyncReserveBytesLocked.NoResizeNeeded", int64_t{ 0 });

    if (m_pendingResizeValid) {
        BT_ZONE_SCOPE("DynamicBuffer::RequestAsyncReserveBytesLocked::CoalesceAsyncResizeRequest");
        BT_ZONE_TEXT(m_name.data(), m_name.size());
        m_requestedResizeCapacity = (std::max)(m_requestedResizeCapacity, desiredBackingCapacity);
        TracyPlot("DynamicBuffer.RequestAsyncReserveBytesLocked.CoalescedResizeCapacityBytes", static_cast<int64_t>(m_requestedResizeCapacity));
        {
            BT_ZONE_SCOPE("DynamicBuffer::RequestAsyncReserveBytesLocked::CoalesceAsyncResizeRequest::Submit");
            BT_ZONE_TEXT(m_name.data(), m_name.size());
            m_asyncResizeState.Request(AsyncBufferBackingResizeRequest{
                .resourceID = GetGlobalResourceID(),
                .heapType = rhi::HeapType::DeviceLocal,
                .byteSize = m_requestedResizeCapacity,
                .unorderedAccess = m_UAV,
                .debugName = m_name,
            });
        }
        spdlog::debug(
            "DynamicBuffer '{}' id={} async resize request coalesced pendingResizeCapacity={} requestedCapacity={} desiredBackingCapacity={} mutationAllowed={}",
            m_name,
            GetGlobalResourceID(),
            m_pendingResizeCapacity,
            requestedCapacity,
            desiredBackingCapacity,
            BufferBase::IsBackingMutationAllowedOnThisThread());
        return;
    }

    const auto resourceID = GetGlobalResourceID();
    m_pendingResizeCapacity = desiredBackingCapacity;
    m_requestedResizeCapacity = (std::max)(m_requestedResizeCapacity, desiredBackingCapacity);
    m_pendingResizeValid = true;
    TracyPlot("DynamicBuffer.RequestAsyncReserveBytesLocked.NewResizeCapacityBytes", static_cast<int64_t>(m_requestedResizeCapacity));
    {
        BT_ZONE_SCOPE("DynamicBuffer::RequestAsyncReserveBytesLocked::SubmitAsyncResizeRequest");
        BT_ZONE_TEXT(m_name.data(), m_name.size());
        m_asyncResizeState.Request(AsyncBufferBackingResizeRequest{
            .resourceID = resourceID,
            .heapType = rhi::HeapType::DeviceLocal,
            .byteSize = desiredBackingCapacity,
            .unorderedAccess = m_UAV,
            .debugName = m_name,
        });
    }
}

void DynamicBuffer::RaiseDeferredAsyncReserveBytes(size_t size) {
    size_t current = m_deferredAsyncReserveBytes.load(std::memory_order_relaxed);
    while (current < size &&
           !m_deferredAsyncReserveBytes.compare_exchange_weak(
               current,
               size,
               std::memory_order_release,
               std::memory_order_relaxed)) {
    }
    TracyPlot("DynamicBuffer.RequestAsyncReserveBytes.DeferredBytes", static_cast<int64_t>((std::max)(current, size)));
}

size_t DynamicBuffer::ConsumeDeferredAsyncReserveBytes() {
    return m_deferredAsyncReserveBytes.exchange(0, std::memory_order_acq_rel);
}

bool DynamicBuffer::PublishReadyAsyncResize(bool wait) {
    BT_ZONE_SCOPE("DynamicBuffer::PublishReadyAsyncResize");
    std::lock_guard lock(m_allocationMutex);
    const size_t deferredSize = ConsumeDeferredAsyncReserveBytes();
    if (deferredSize != 0) {
        BT_ZONE_SCOPE("DynamicBuffer::PublishReadyAsyncResize::DrainDeferredReserve");
        BT_ZONE_TEXT(m_name.data(), m_name.size());
        TracyPlot("DynamicBuffer.PublishReadyAsyncResize.DrainedDeferredBytes", static_cast<int64_t>(deferredSize));
        RequestAsyncReserveBytesLocked(deferredSize);
    }
    return PublishReadyAsyncResizeLocked(wait);
}

bool DynamicBuffer::PublishReadyAsyncResizeLocked(bool wait) {
    BT_ZONE_SCOPE("DynamicBuffer::PublishReadyAsyncResizeLocked");
    if (!m_pendingResizeValid && !m_asyncResizeState.HasPending()) {
        TracyPlot("DynamicBuffer.Resize.Pending", int64_t{ 0 });
        return false;
    }
    if (!BufferBase::IsBackingMutationAllowedOnThisThread()) {
        return false;
    }
    TracyPlot("DynamicBuffer.Resize.Pending", int64_t{ 1 });
    auto resizeResult = m_asyncResizeState.ConsumeReady(wait);
    if (!resizeResult.has_value()) {
        TracyPlot("DynamicBuffer.Resize.FutureReady", int64_t{ 0 });
        return false;
    }
    TracyPlot("DynamicBuffer.Resize.FutureReady", int64_t{ 1 });
    if (resizeResult->exception) {
        try {
            std::rethrow_exception(resizeResult->exception);
        }
        catch (const std::exception& e) {
            spdlog::error(
                "DynamicBuffer '{}' id={} async resize failed: {}",
                m_name,
                GetGlobalResourceID(),
                e.what());
        }
        catch (...) {
            spdlog::error(
                "DynamicBuffer '{}' id={} async resize failed with unknown exception",
                m_name,
                GetGlobalResourceID());
        }
        m_pendingResizeCapacity = 0;
        m_requestedResizeCapacity = 0;
        m_pendingResizeValid = false;
        return false;
    }

    std::unique_ptr<GpuBufferBacking> newBacking = std::move(resizeResult->backing);
    const size_t previousBackingCapacity = static_cast<size_t>(GetBufferSize());
    size_t newCapacity = (std::max)({ static_cast<size_t>(resizeResult->byteSize), m_requestedResizeCapacity, m_capacity });
    if (newCapacity > static_cast<size_t>(resizeResult->byteSize)) {
        m_pendingResizeCapacity = newCapacity;
        m_requestedResizeCapacity = newCapacity;
        m_pendingResizeValid = true;
        m_asyncResizeState.Request(AsyncBufferBackingResizeRequest{
            .resourceID = GetGlobalResourceID(),
            .heapType = rhi::HeapType::DeviceLocal,
            .byteSize = newCapacity,
            .unorderedAccess = m_UAV,
            .debugName = m_name,
        });
        return false;
    }
    const bool logicalCapacityAlreadyExtended = m_capacity >= newCapacity;
    m_pendingResizeCapacity = 0;
    m_requestedResizeCapacity = 0;
    m_pendingResizeValid = false;
    TracyPlot("DynamicBuffer.Resize.NewCapacityBytes", static_cast<int64_t>(newCapacity));
    TracyPlot("DynamicBuffer.Resize.OldCapacityBytes", static_cast<int64_t>(previousBackingCapacity));
    if (!newBacking || newCapacity <= previousBackingCapacity) {
        return false;
    }

    {
        BT_ZONE_SCOPE("DynamicBuffer::PublishReadyAsyncResizeLocked::ApplyBacking");
        ApplyResizeBackingLocked(std::move(newBacking), newCapacity, previousBackingCapacity);
    }
    if (!logicalCapacityAlreadyExtended) {
        BT_ZONE_SCOPE("DynamicBuffer::PublishReadyAsyncResizeLocked::MergeFreeBlock");
        size_t newBlockOffset = previousBackingCapacity;
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
        TracyPlot("DynamicBuffer.Resize.TrackedFreeBytes", static_cast<int64_t>(trackedFreeSize));
    }
    return true;
}

bool DynamicBuffer::CanAllocateBytes(size_t size) const {
    std::lock_guard lock(m_allocationMutex);
    if (size == 0) {
        return true;
    }
    return m_freeBlocks.lower_bound({ size, 0 }) != m_freeBlocks.end();
}

bool DynamicBuffer::HasPendingBackingResize() const {
    if (m_deferredAsyncReserveBytes.load(std::memory_order_acquire) != 0) {
        return true;
    }
    std::lock_guard lock(m_allocationMutex);
    return m_pendingResizeValid || m_asyncResizeState.HasPending();
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

std::vector<DynamicBuffer::PagedAllocation> DynamicBuffer::AllocateRangesBatch(
    const std::vector<size_t>& counts,
    size_t elementSize)
{
    std::lock_guard lock(m_allocationMutex);
    std::vector<PagedAllocation> ranges(counts.size());
    if (counts.empty() || elementSize == 0) {
        return ranges;
    }

    size_t totalCount = 0;
    for (const auto count : counts) {
        totalCount += count;
    }
    if (totalCount == 0) {
        return ranges;
    }

    const size_t totalSize = totalCount * elementSize;
    auto freeIt = m_freeBlocks.lower_bound({ totalSize, 0 });
    if (freeIt == m_freeBlocks.end()) {
        ReserveBytes(totalSize);
        freeIt = m_freeBlocks.lower_bound({ totalSize, 0 });
    }
    if (freeIt == m_freeBlocks.end()) {
        return ranges;
    }

    const size_t blockOffset = freeIt->second;
    m_freeBlocks.erase(freeIt);
    auto blockIt = m_blocksByOffset.find(blockOffset);
    const size_t blockSize = blockIt != m_blocksByOffset.end() ? blockIt->second.size : totalSize;
    if (blockIt != m_blocksByOffset.end()) {
        m_blocksByOffset.erase(blockIt);
    }

    size_t cursor = blockOffset;
    for (size_t i = 0; i < counts.size(); ++i) {
        const size_t count = counts[i];
        if (count == 0) {
            continue;
        }

        const size_t size = count * elementSize;
        m_blocksByOffset[cursor] = { cursor, size, false };
        ranges[i] = PagedAllocation{
            cursor,
            size,
            size,
            elementSize,
            count
        };
        cursor += size;
    }

    if (blockSize > totalSize) {
        const size_t remainingOffset = blockOffset + totalSize;
        const size_t remainingSize = blockSize - totalSize;
        m_blocksByOffset[remainingOffset] = { remainingOffset, remainingSize, true };
        m_freeBlocks.insert({ remainingSize, remainingOffset });
    }

    return ranges;
}

bool DynamicBuffer::TryAllocateRangesBatch(
    const std::vector<size_t>& counts,
    size_t elementSize,
    std::vector<PagedAllocation>& ranges,
    ReadyResizePublishMode resizePublishMode)
{
    std::lock_guard lock(m_allocationMutex);
    ranges.assign(counts.size(), PagedAllocation{});
    if (counts.empty() || elementSize == 0) {
        return true;
    }

    size_t totalCount = 0;
    for (const auto count : counts) {
        totalCount += count;
    }
    if (totalCount == 0) {
        return true;
    }

    if (resizePublishMode == ReadyResizePublishMode::PublishIfReady &&
        PublishReadyAsyncResizeLocked(false)) {
        // Publishing an already-built resize is intentionally allowed here:
        // it is bounded state publication, not GPU backing creation.
    }

    const size_t totalSize = totalCount * elementSize;
    auto freeIt = m_freeBlocks.lower_bound({ totalSize, 0 });
    if (freeIt == m_freeBlocks.end()) {
        return false;
    }

    const size_t blockOffset = freeIt->second;
    m_freeBlocks.erase(freeIt);
    auto blockIt = m_blocksByOffset.find(blockOffset);
    const size_t blockSize = blockIt != m_blocksByOffset.end() ? blockIt->second.size : totalSize;
    if (blockIt != m_blocksByOffset.end()) {
        m_blocksByOffset.erase(blockIt);
    }

    size_t cursor = blockOffset;
    for (size_t i = 0; i < counts.size(); ++i) {
        const size_t count = counts[i];
        if (count == 0) {
            continue;
        }

        const size_t size = count * elementSize;
        m_blocksByOffset[cursor] = { cursor, size, false };
        ranges[i] = PagedAllocation{
            cursor,
            size,
            size,
            elementSize,
            count
        };
        cursor += size;
    }

    if (blockSize > totalSize) {
        const size_t remainingOffset = blockOffset + totalSize;
        const size_t remainingSize = blockSize - totalSize;
        m_blocksByOffset[remainingOffset] = { remainingOffset, remainingSize, true };
        m_freeBlocks.insert({ remainingSize, remainingOffset });
    }

    return true;
}

DynamicBuffer::AllocationProbe DynamicBuffer::SnapshotAllocationProbe() const
{
    std::lock_guard lock(m_allocationMutex);
    AllocationProbe probe;
    probe.freeBlocks.reserve(m_freeBlocks.size());
    probe.freeBlocks.assign(m_freeBlocks.begin(), m_freeBlocks.end());
    return probe;
}

bool DynamicBuffer::CanConsumeAllocationProbe(
    const AllocationProbe& probe,
    const std::vector<size_t>& counts,
    size_t elementSize)
{
    return CanConsumeAllocationProbeBytes(probe, TotalAllocationProbeSize(counts, elementSize));
}

bool DynamicBuffer::CanConsumeAllocationProbeBytes(
    const AllocationProbe& probe,
    size_t totalSize)
{
    if (totalSize == 0) {
        return true;
    }

    return std::lower_bound(
        probe.freeBlocks.begin(),
        probe.freeBlocks.end(),
        std::pair<size_t, size_t>{ totalSize, 0 }) != probe.freeBlocks.end();
}

bool DynamicBuffer::TryConsumeAllocationProbeBytes(
    AllocationProbe& probe,
    size_t totalSize)
{
    if (totalSize == 0) {
        return true;
    }

    auto freeIt = std::lower_bound(
        probe.freeBlocks.begin(),
        probe.freeBlocks.end(),
        std::pair<size_t, size_t>{ totalSize, 0 });
    if (freeIt == probe.freeBlocks.end()) {
        return false;
    }

    const size_t blockSize = freeIt->first;
    const size_t blockOffset = freeIt->second;
    probe.freeBlocks.erase(freeIt);
    if (blockSize > totalSize) {
        const std::pair<size_t, size_t> remainingBlock{ blockSize - totalSize, blockOffset + totalSize };
        const auto insertIt = std::lower_bound(
            probe.freeBlocks.begin(),
            probe.freeBlocks.end(),
            remainingBlock);
        probe.freeBlocks.insert(insertIt, remainingBlock);
    }
    return true;
}

bool DynamicBuffer::TryConsumeAllocationProbe(
    AllocationProbe& probe,
    const std::vector<size_t>& counts,
    size_t elementSize)
{
    return TryConsumeAllocationProbeBytes(probe, TotalAllocationProbeSize(counts, elementSize));
}

std::vector<DynamicBuffer::PagedAllocation> DynamicBuffer::AddDataPaged(
    const void* data,
    size_t count,
    size_t elementSize,
    size_t pageElementCount)
{
    auto pages = AllocatePages(count, elementSize, pageElementCount);
    StageWritePages(data, count, elementSize, pages, pageElementCount);
    return pages;
}

std::vector<DynamicBuffer::PagedAllocation> DynamicBuffer::AllocatePages(
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

    size_t remaining = count;
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
        pages.push_back(PagedAllocation{
            offset,
            usedSize,
            allocationSize,
            elementSize,
            pageCount
        });

        remaining -= pageCount;
    }

    return pages;
}

void DynamicBuffer::StageWriteRange(const void* data, size_t size, size_t offset) {
    if (data == nullptr || size == 0) {
        return;
    }
    StageOrUpload(data, size, offset);
}

void DynamicBuffer::StageWritePages(
    const void* data,
    size_t count,
    size_t elementSize,
    const std::vector<PagedAllocation>& pages,
    size_t pageElementCount)
{
    if (data == nullptr || count == 0 || elementSize == 0 || pages.empty()) {
        return;
    }
    if (pageElementCount == 0) {
        pageElementCount = 1;
    }

    const auto* bytes = static_cast<const std::byte*>(data);
    size_t elementCursor = 0;
    for (const auto& page : pages) {
        if (!page.IsValid() || elementCursor >= count) {
            continue;
        }
        const size_t pageCount = (std::min)(page.count, count - elementCursor);
        const size_t usedSize = pageCount * elementSize;
        StageWriteRange(bytes + elementCursor * elementSize, usedSize, page.offset);
        elementCursor += pageCount;
    }
}

std::unique_ptr<BufferView> DynamicBuffer::AddData(const void* data, size_t size, size_t elementSize, size_t fullAllocationSize) {
    BT_ZONE_SCOPE("DynamicBuffer::AddData");
    BT_ZONE_VALUE(static_cast<int64_t>(size));
    BT_ZONE_TEXT(m_name.data(), m_name.size());
    std::unique_lock<std::recursive_mutex> lock(m_allocationMutex, std::defer_lock);
    {
        BT_ZONE_SCOPE("DynamicBuffer::AddData::WaitAllocationMutex");
        lock.lock();
    }
	size_t actualSize = size;
    if (fullAllocationSize != 0) {
		actualSize = fullAllocationSize;
		if (actualSize < size) {
			spdlog::warn("Full allocation size is smaller than the data size. Using data size instead.");
			actualSize = size;
		}
    }
    std::unique_ptr<BufferView> view;
    {
        BT_ZONE_SCOPE("DynamicBuffer::AddData::AllocateView");
        view = Allocate(actualSize, elementSize);
    }
    if (!view) {
        return nullptr;
    }
    
	if (data != nullptr) {
        BT_ZONE_SCOPE("DynamicBuffer::AddData::StageWrite");
        StageOrUpload(data, size, view->GetOffset());
	}

    return view;
}

void DynamicBuffer::ReserveCpuShadowAdditionalBytes(size_t additionalBytes) {
    BT_ZONE_SCOPE("DynamicBuffer::ReserveCpuShadowAdditionalBytes");
    BT_ZONE_VALUE(static_cast<int64_t>(additionalBytes));
    BT_ZONE_TEXT(m_name.data(), m_name.size());
    if (additionalBytes == 0) {
        return;
    }

    std::unique_lock<std::recursive_mutex> uploadLock(m_uploadPolicyMirrorMutex, std::defer_lock);
    {
        BT_ZONE_SCOPE("DynamicBuffer::ReserveCpuShadowAdditionalBytes::WaitUploadPolicyMutex");
        uploadLock.lock();
    }
    if (additionalBytes > (std::numeric_limits<size_t>::max)() - m_cpuShadowData.size()) {
        return;
    }
    const size_t desiredCapacity = m_cpuShadowData.size() + additionalBytes;
    if (m_cpuShadowData.capacity() < desiredCapacity) {
        BT_ZONE_SCOPE("DynamicBuffer::ReserveCpuShadowAdditionalBytes::Reserve");
        m_cpuShadowData.reserve(desiredCapacity);
    }
}

void DynamicBuffer::UpdateView(BufferView* view, const void* data) {
    StageOrUpload(data, view->GetSize(), view->GetOffset());
}

void DynamicBuffer::StageOrUpload(const void* data, size_t size, size_t offset) {
    BT_ZONE_SCOPE("DynamicBuffer::StageOrUpload");
    BT_ZONE_VALUE(static_cast<int64_t>(size));
    BT_ZONE_TEXT(m_name.data(), m_name.size());
    std::unique_lock<std::recursive_mutex> uploadLock(m_uploadPolicyMirrorMutex, std::defer_lock);
    {
        BT_ZONE_SCOPE("DynamicBuffer::StageOrUpload::WaitUploadPolicyMutex");
        uploadLock.lock();
    }
    {
        BT_ZONE_SCOPE("DynamicBuffer::StageOrUpload::RetainCpuShadow");
        RetainCpuShadowWrite(data, size, offset);
    }
    if (offset + size > GetBufferSize()) {
        // The logical view has been allocated ahead of the GPU backing resize.
        // Keep the CPU shadow authoritative and replay it when the resize publishes.
        return;
    }
    {
        BT_ZONE_SCOPE("DynamicBuffer::StageOrUpload::StagePolicyWrite");
        StageOrUploadLocked(data, size, offset);
    }
}

void DynamicBuffer::StageOrUploadLocked(const void* data, size_t size, size_t offset) {
    if (data == nullptr || size == 0) {
        return;
    }

    if (org::runtime::GetActiveUploadPolicyService() == nullptr) {
        SyncUploadPolicyState();
#if BUILD_TYPE == BUILD_TYPE_DEBUG
        m_uploadPolicyState.StageWrite(data, size, offset, GetBufferSize(), __FILE__, __LINE__);
#else
        m_uploadPolicyState.StageWrite(data, size, offset, GetBufferSize());
#endif
        BUFFER_UPLOAD(data, size, org::runtime::UploadTarget::FromShared(shared_from_this()), offset);
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

    BUFFER_UPLOAD(data, size, org::runtime::UploadTarget::FromShared(shared_from_this()), offset);
}

void DynamicBuffer::EnsureCpuShadowSize(size_t size) {
    if (m_cpuShadowData.size() < size) {
        BT_ZONE_SCOPE("DynamicBuffer::EnsureCpuShadowSize::Grow");
        BT_ZONE_VALUE(static_cast<int64_t>(size - m_cpuShadowData.size()));
        m_cpuShadowData.resize(size, std::byte{ 0 });
    }
}

void DynamicBuffer::RetainCpuShadowWrite(const void* data, size_t size, size_t offset) {
    if (data == nullptr || size == 0) {
        return;
    }

    EnsureCpuShadowSize(offset + size);
    {
        BT_ZONE_SCOPE("DynamicBuffer::RetainCpuShadowWrite::Memcpy");
        std::memcpy(m_cpuShadowData.data() + static_cast<std::ptrdiff_t>(offset), data, size);
    }
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
        EnsureCpuShadowSize(GetBufferSize());
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
    spdlog::debug(
        "DynamicBuffer '{}' id={} GrowBuffer begin oldCapacity={} newCapacity={} hasBacking={}",
        m_name,
        GetGlobalResourceID(),
        previousCapacity,
        newSize,
        m_dataBuffer != nullptr);
    auto device = DeviceManager::GetInstance().GetDevice();
    spdlog::debug(
        "DynamicBuffer '{}' id={} GrowBuffer creating new GPU backing",
        m_name,
        GetGlobalResourceID());
    auto newDataBuffer = GpuBufferBacking::CreateUnique(rhi::HeapType::DeviceLocal, newSize, GetGlobalResourceID(), m_UAV);
    spdlog::debug(
        "DynamicBuffer '{}' id={} GrowBuffer created new GPU backing",
        m_name,
        GetGlobalResourceID());
	ApplyResizeBackingLocked(std::move(newDataBuffer), newSize, previousCapacity);
}

void DynamicBuffer::ApplyResizeBackingLocked(std::unique_ptr<GpuBufferBacking> newDataBuffer, size_t newSize, size_t previousCapacity) {
    BT_ZONE_SCOPE("DynamicBuffer::ApplyResizeBackingLocked");
    TracyPlot("DynamicBuffer.Resize.ApplyNewSizeBytes", static_cast<int64_t>(newSize));
    TracyPlot("DynamicBuffer.Resize.ApplyPreviousCapacityBytes", static_cast<int64_t>(previousCapacity));
    spdlog::debug(
        "DynamicBuffer '{}' id={} GrowBuffer SetBacking begin",
		m_name,
		GetGlobalResourceID());
    {
        BT_ZONE_SCOPE("DynamicBuffer::ApplyResizeBackingLocked::SetBacking");
	    SetBacking(std::move(newDataBuffer), newSize);
    }
    spdlog::debug(
        "DynamicBuffer '{}' id={} GrowBuffer SetBacking complete bufferSize={} backingGeneration={} ",
        m_name,
        GetGlobalResourceID(),
        GetBufferSize(),
        GetBackingGeneration());
    {
        BT_ZONE_SCOPE("DynamicBuffer::ApplyResizeBackingLocked::UploadPolicyAndReplay");
        std::lock_guard<std::recursive_mutex> uploadLock(m_uploadPolicyMirrorMutex);
        {
            BT_ZONE_SCOPE("DynamicBuffer::ApplyResizeBackingLocked::SyncUploadPolicyState");
            SyncUploadPolicyState();
        }
        {
            BT_ZONE_SCOPE("DynamicBuffer::ApplyResizeBackingLocked::EnsureCpuShadowSize");
            EnsureCpuShadowSize(newSize);
        }
        {
            BT_ZONE_SCOPE("DynamicBuffer::ApplyResizeBackingLocked::OnBufferResized");
            m_uploadPolicyState.OnBufferResized(GetBufferSize());
        }
        const size_t replayBytes = (std::min)(newSize, m_cpuShadowData.size());
        TracyPlot("DynamicBuffer.Resize.ReplayBytes", static_cast<int64_t>(replayBytes));
        if (replayBytes > 0u) {
            // DynamicBuffer owns an explicit CPU shadow. Replays after backing
            // replacement must come from that shadow, not from the upload-policy
            // coalescing mirror, because long-lived sparse buffers can contain
            // bytes written through bulk or external upload paths.
            if (org::runtime::GetActiveUploadService() != nullptr) {
                BT_ZONE_SCOPE("DynamicBuffer::ApplyResizeBackingLocked::ReplayCpuShadowUploadService");
                BUFFER_UPLOAD(m_cpuShadowData.data(), replayBytes, org::runtime::UploadTarget::FromShared(shared_from_this()), 0u);
                spdlog::debug(
                    "DynamicBuffer '{}' id={} GrowBuffer replayed CPU shadow bytes={}",
                    m_name,
                    GetGlobalResourceID(),
                    replayBytes);
            } else {
                BT_ZONE_SCOPE("DynamicBuffer::ApplyResizeBackingLocked::ReplayCpuShadowStageOrUpload");
                StageOrUploadLocked(m_cpuShadowData.data(), replayBytes, 0u);
                if (m_uploadPolicyState.HasPendingWork()) {
                    MarkUploadPolicyDirty();
                }
            }
        }
    }

    m_capacity = newSize;

    {
        BT_ZONE_SCOPE("DynamicBuffer::ApplyResizeBackingLocked::ApplyMetadata");
        BT_ZONE_VALUE(static_cast<int64_t>(m_metadataBundles.size()));
        for (const auto& bundle : m_metadataBundles) {
            ApplyMetadataToBacking(bundle);
        }
    }

    spdlog::debug(
        "DynamicBuffer '{}' id={} GrowBuffer AssignDescriptorSlots begin",
        m_name,
        GetGlobalResourceID());
    {
        BT_ZONE_SCOPE("DynamicBuffer::ApplyResizeBackingLocked::AssignDescriptorSlots");
        AssignDescriptorSlots();
    }
    spdlog::debug(
        "DynamicBuffer '{}' id={} GrowBuffer AssignDescriptorSlots complete",
        m_name,
        GetGlobalResourceID());

    {
        BT_ZONE_SCOPE("DynamicBuffer::ApplyResizeBackingLocked::SetName");
	    SetName(m_name);
    }
    spdlog::debug(
        "DynamicBuffer '{}' id={} GrowBuffer complete finalCapacity={}",
        m_name,
        GetGlobalResourceID(),
        m_capacity);
}
