#pragma once

#include <vector>
#include <string>
#include <memory>
#include <deque>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>
#include <spdlog/spdlog.h>
#include <rhi.h>

#include "OpenRenderGraph/OpenRenderGraph.h"
#include "Resources/Buffers/BufferView.h"
#include "Resources/GPUBacking/GpuBufferBacking.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "Render/Runtime/UploadPolicyServiceAccess.h"


using Microsoft::WRL::ComPtr;

namespace org {

class LazyDynamicStructuredBufferBase : public ViewedDynamicBufferBase, public IHasMemoryMetadata { // Necessary to store these in a templateless vector
public:
	virtual size_t GetElementSize() const = 0;
    virtual void UpdateView(BufferView* view, const void* data) = 0;
};

template <typename T>
class LazyDynamicStructuredBuffer : public LazyDynamicStructuredBufferBase, public IDeferredBackingResizeClient {
public:

	static std::shared_ptr<LazyDynamicStructuredBuffer<T>> CreateShared(UINT capacity = 64, std::string name = "", uint64_t alignment = 1, bool UAV = false) {
		return std::shared_ptr<LazyDynamicStructuredBuffer<T>>(new LazyDynamicStructuredBuffer<T>(capacity, name, alignment, UAV));
	}

    ~LazyDynamicStructuredBuffer() override {
        UnregisterDeferredBackingResizeClient(this);
    }

    std::shared_ptr<BufferView> Add() {
        auto viewedWeak = std::weak_ptr<ViewedDynamicBufferBase>(
            std::dynamic_pointer_cast<ViewedDynamicBufferBase>(Resource::weak_from_this().lock())
        );
		if (!m_freeIndices.empty()) { // Reuse a free index
			uint64_t index = m_freeIndices.front();
            m_freeIndices.pop_front();
            return BufferView::CreateShared(viewedWeak, index * m_elementSize, m_elementSize, sizeof(T));
        }
        const uint64_t requiredCapacity = m_usedCapacity + 1u;
		if (requiredCapacity > m_capacity) { // Resize the buffer if necessary
            uint32_t newCapacity = m_capacity > 0u ? m_capacity : 1u;
            while (requiredCapacity > newCapacity) {
                newCapacity *= 2u;
            }
            if (!TryResize(newCapacity)) {
                return nullptr;
            }
        }
        m_usedCapacity = requiredCapacity;
		size_t index = m_usedCapacity - 1;
        return BufferView::CreateShared(viewedWeak, index * m_elementSize, m_elementSize, sizeof(T));
    }

	std::shared_ptr<BufferView> Add(const T& data) {
		auto view = Add();
        if (!view) {
            return nullptr;
        }
		UpdateView(view.get(), &data);
		return view;
	}

    std::vector<std::shared_ptr<BufferView>> AddMany(const T* data, size_t count) {
        std::vector<std::shared_ptr<BufferView>> views;
        if (count == 0) {
            return views;
        }

        auto viewedWeak = std::weak_ptr<ViewedDynamicBufferBase>(
            std::dynamic_pointer_cast<ViewedDynamicBufferBase>(Resource::weak_from_this().lock())
        );
        views.reserve(count);

        const size_t reusableCount = std::min(count, m_freeIndices.size());
        const size_t newCount = count - reusableCount;
        if (newCount != 0) {
            const uint64_t requiredCapacity = m_usedCapacity + newCount;
            if (requiredCapacity > m_capacity) {
                uint32_t newCapacity = m_capacity > 0u ? m_capacity : 1u;
                while (requiredCapacity > newCapacity) {
                    newCapacity *= 2u;
                }
                if (!TryResize(newCapacity)) {
                    return views;
                }
            }
        }

        size_t copiedFromFreeList = 0;
        while (!m_freeIndices.empty() && copiedFromFreeList < count) {
            const uint64_t index = m_freeIndices.front();
            m_freeIndices.pop_front();
            views.push_back(BufferView::CreateShared(viewedWeak, index * m_elementSize, m_elementSize, sizeof(T)));
            if (data != nullptr) {
                StageOrUpload(&data[copiedFromFreeList], sizeof(T), index * m_elementSize);
            }
            ++copiedFromFreeList;
        }

        const size_t remainingNewCount = count - copiedFromFreeList;
        if (remainingNewCount != 0) {
            const uint64_t firstIndex = m_usedCapacity;
            const uint64_t requiredCapacity = m_usedCapacity + remainingNewCount;

            for (size_t i = 0; i < remainingNewCount; ++i) {
                const uint64_t index = firstIndex + i;
                views.push_back(BufferView::CreateShared(viewedWeak, index * m_elementSize, m_elementSize, sizeof(T)));
            }
            m_usedCapacity = requiredCapacity;

            if (data != nullptr) {
                StageOrUpload(data + copiedFromFreeList, sizeof(T) * remainingNewCount, firstIndex * m_elementSize);
            }
        }

        return views;
    }

    std::pair<uint64_t, uint64_t> AddContiguousRange(const T* data, size_t count) {
        if (count == 0) {
            return { 0, 0 };
        }

        const uint64_t firstIndex = m_usedCapacity;
        const uint64_t requiredCapacity = m_usedCapacity + count;
        if (requiredCapacity > m_capacity) {
            uint32_t newCapacity = m_capacity > 0u ? m_capacity : 1u;
            while (requiredCapacity > newCapacity) {
                newCapacity *= 2u;
            }
            if (!TryResize(newCapacity)) {
                return { 0, 0 };
            }
        }

        m_usedCapacity = requiredCapacity;
        if (data != nullptr) {
            StageOrUpload(data, sizeof(T) * count, firstIndex * m_elementSize);
        }

        return { firstIndex * m_elementSize, count * m_elementSize };
    }

    void ReserveAdditional(size_t count) {
        if (count == 0) {
            return;
        }

        const size_t reusableCount = std::min(count, m_freeIndices.size());
        const size_t newCount = count - reusableCount;
        if (newCount == 0) {
            return;
        }

        const uint64_t requiredCapacity = m_usedCapacity + newCount;
        if (requiredCapacity <= m_capacity) {
            return;
        }

        uint32_t newCapacity = m_capacity > 0u ? m_capacity : 1u;
        while (requiredCapacity > newCapacity) {
            newCapacity *= 2u;
        }
        (void)TryResize(newCapacity);
    }

    void Remove(BufferView* view) {
        if (!view) {
            return;
        }

        // Reject views not created by this buffer instance (stale view from old pass/buffer)
        auto owner = view->GetBuffer();
        if (!owner || owner.get() != static_cast<ViewedDynamicBufferBase*>(this)) {
#if BUILD_TYPE == BUILD_DEBUG
			throw std::runtime_error("Attempted to remove a BufferView from a LazyDynamicStructuredBuffer that does not own it.");
#endif
            return;
        }

        const uint64_t index = view->GetOffset() / m_elementSize;
        m_freeIndices.push_back(index);
    }

    void RemoveRange(uint64_t offset, uint64_t size) {
        if (size == 0) {
            return;
        }

        const uint64_t firstIndex = offset / m_elementSize;
        const uint64_t count = size / m_elementSize;
        for (uint64_t i = 0; i < count; ++i) {
            m_freeIndices.push_back(firstIndex + i);
        }
    }

    void Resize(uint32_t newCapacity) {
        (void)TryResize(newCapacity);
    }

    bool TryResize(uint32_t newCapacity) {
        if (newCapacity <= m_capacity) {
            return true;
        }

        if (BufferBase::IsBackingMutationAllowedOnThisThread()) {
            CreateBuffer(newCapacity, m_capacity);
            m_capacity = newCapacity;
            return true;
        }

        RequestAsyncResize(newCapacity);
        return false;
    }

    void UpdateView(BufferView* view, const void* data) override {
        if (view == nullptr) {
            return;
        }
        const uint64_t index = view->GetOffset() / m_elementSize;
        if (!TryEnsureCapacityForIndex(index)) {
            return;
        }
        StageOrUpload(data, sizeof(T), view->GetOffset());
    }

	void UpdateAt(uint64_t index, const T& data) {
        if (!TryUpdateAt(index, data)) {
            return;
        }
    }

	bool TryUpdateAt(uint64_t index, const T& data) {
        if (!TryEnsureCapacityForIndex(index)) {
            return false;
        }
        StageOrUpload(&data, sizeof(T), index * m_elementSize);
        return true;
    }

    org::runtime::BulkWriteHandle BeginBulkWrite() {
        auto lock = std::make_shared<std::unique_lock<std::recursive_mutex>>(m_uploadPolicyMirrorMutex);
        SyncUploadPolicyState();
        EnsureUploadPolicyRegistration();
        EnsureCpuShadowSize(GetBufferSize());
        org::runtime::BulkWriteHandle handle{
            reinterpret_cast<uint8_t*>(m_cpuShadowData.data()),
            m_cpuShadowData.size()
        };
        handle.lock = std::move(lock);
        return handle;
    }

    void EndBulkWrite(size_t dirtyOffset, size_t dirtySize) {
        std::lock_guard<std::recursive_mutex> lock(m_uploadPolicyMirrorMutex);
        if (dirtySize == 0) {
            return;
        }

        EnsureCpuShadowSize(dirtyOffset + dirtySize);
        StageOrUploadLocked(m_cpuShadowData.data() + static_cast<std::ptrdiff_t>(dirtyOffset), dirtySize, dirtyOffset);
        if (org::runtime::GetActiveUploadPolicyService() != nullptr && m_uploadPolicyState.HasPendingWork()) {
            MarkUploadPolicyDirty();
        }
    }

    uint64_t Size() {
        return m_usedCapacity;
    }

    uint32_t Capacity() const {
        return m_capacity;
    }

	size_t GetElementSize() const override {
		return m_elementSize;
	}

    bool TryEnsureCapacityForIndex(uint64_t index) {
        if (index >= m_capacity) {
            uint32_t newCapacity = m_capacity > 0u ? m_capacity : 1u;
            while (index >= static_cast<uint64_t>(newCapacity)) {
                newCapacity *= 2u;
            }
            if (!TryResize(newCapacity)) {
                return false;
            }
        }

        const uint64_t requiredUsedCapacity = index + 1u;
        if (requiredUsedCapacity > m_usedCapacity) {
            m_usedCapacity = requiredUsedCapacity;
        }
        return true;
    }

    bool PublishReadyAsyncResize(bool wait = false) {
        return PublishReadyAsyncResizeInternal(wait);
    }

    bool PublishPendingBackingResize(bool wait) override {
        return PublishReadyAsyncResize(wait);
    }

    bool HasPendingBackingResize() const override {
        return m_pendingResizeValid || m_asyncResizeState.HasPending();
    }

    std::string GetDeferredBackingResizeDebugName() const override {
        return name;
    }

    void OnUploadPolicyBeginFrame() override {
        std::lock_guard<std::recursive_mutex> lock(m_uploadPolicyMirrorMutex);
        SyncUploadPolicyState();
        m_uploadPolicyState.BeginFrame();
    }

    void OnUploadPolicyFlush() override {
        std::lock_guard<std::recursive_mutex> lock(m_uploadPolicyMirrorMutex);
        SyncUploadPolicyState();
        m_uploadPolicyState.FlushToUploadService(
            org::runtime::UploadTarget::FromShared(shared_from_this()),
            [this](size_t offset, size_t size) -> const void* {
                if (offset + size > m_cpuShadowData.size()) {
                    return nullptr;
                }
                return m_cpuShadowData.data() + static_cast<std::ptrdiff_t>(offset);
            });
    }

    bool HasPendingUploadPolicyWork() const override {
        std::lock_guard<std::recursive_mutex> lock(m_uploadPolicyMirrorMutex);
        return m_uploadPolicyState.HasPendingWork();
    }

    uint64_t GetUploadPolicyLastFlushWrites() const override {
        std::lock_guard<std::recursive_mutex> lock(m_uploadPolicyMirrorMutex);
        return m_uploadPolicyState.GetLastFlushStats().flushedWrites;
    }

    uint64_t GetUploadPolicyLastFlushBytes() const override {
        std::lock_guard<std::recursive_mutex> lock(m_uploadPolicyMirrorMutex);
        return m_uploadPolicyState.GetLastFlushStats().flushedBytes;
    }

private:
    LazyDynamicStructuredBuffer(UINT capacity = 64, std::string name = "", uint64_t alignment = 1, bool UAV = false)
        : m_capacity(capacity), m_needsUpdate(false), m_UAV(UAV) {
        SetUploadPolicyTag(org::runtime::UploadPolicyTag::Coalesced);
        if (alignment == 0) {
			alignment = 1;
        }
		m_elementSize = static_cast<uint32_t>(((sizeof(T) + alignment - 1) / alignment) * alignment);
        CreateBuffer(capacity);
		SetName(name);
        RegisterDeferredBackingResizeClient(this);
    }
    void OnSetName() override {
        SetBackingName(m_name, name);
    }

    uint32_t m_capacity;
    uint64_t m_usedCapacity = 0;
    bool m_needsUpdate;
	std::deque<uint64_t> m_freeIndices;
    uint32_t m_elementSize = 0;

    inline static std::string m_name = "LazyDynamicStructuredBuffer";

    bool m_UAV = false;

    std::vector<EntityComponentBundle> m_metadataBundles;
    // Authoritative CPU bytes for backing replacement. Lazy buffers do not keep
    // typed element storage, so the byte shadow is the resize replay source.
    std::vector<std::byte> m_cpuShadowData;

    void EnsureCapacityForIndex(uint64_t index) {
        (void)TryEnsureCapacityForIndex(index);
    }

    void RequestAsyncResize(uint32_t newCapacity) {
        if (m_pendingResizeValid && m_pendingResizeCapacity >= newCapacity) {
            return;
        }

        if (m_pendingResizeValid) {
            m_pendingResizeCapacity = (std::max)(m_pendingResizeCapacity, newCapacity);
            m_asyncResizeState.Request(AsyncBufferBackingResizeRequest{
                .resourceID = GetGlobalResourceID(),
                .heapType = rhi::HeapType::DeviceLocal,
                .byteSize = static_cast<uint64_t>(m_elementSize) * m_pendingResizeCapacity,
                .unorderedAccess = m_UAV,
                .debugName = name,
            });
            return;
        }

        const auto resourceID = GetGlobalResourceID();
        m_pendingResizeCapacity = newCapacity;
        m_pendingResizeValid = true;
        m_asyncResizeState.Request(AsyncBufferBackingResizeRequest{
            .resourceID = resourceID,
            .heapType = rhi::HeapType::DeviceLocal,
            .byteSize = static_cast<uint64_t>(m_elementSize) * newCapacity,
            .unorderedAccess = m_UAV,
            .debugName = name,
        });
    }

    bool PublishReadyAsyncResizeInternal(bool wait) {
        if ((!m_pendingResizeValid && !m_asyncResizeState.HasPending()) ||
            !BufferBase::IsBackingMutationAllowedOnThisThread()) {
            return false;
        }
        auto resizeResult = m_asyncResizeState.ConsumeReady(wait);
        if (!resizeResult.has_value()) {
            return false;
        }

        if (resizeResult->exception) {
            try {
                std::rethrow_exception(resizeResult->exception);
            }
            catch (const std::exception& e) {
                spdlog::error(
                    "LazyDynamicStructuredBuffer '{}' id={} async resize failed: {}",
                    name,
                    GetGlobalResourceID(),
                    e.what());
            }
            catch (...) {
                spdlog::error(
                    "LazyDynamicStructuredBuffer '{}' id={} async resize failed with unknown exception",
                    name,
                    GetGlobalResourceID());
            }
            m_pendingResizeCapacity = 0u;
            m_pendingResizeValid = false;
            return false;
        }

        auto backing = std::move(resizeResult->backing);
        const uint32_t newCapacity = m_elementSize == 0u
            ? 0u
            : static_cast<uint32_t>(resizeResult->byteSize / m_elementSize);
        m_pendingResizeCapacity = 0u;
        m_pendingResizeValid = false;
        if (!backing || newCapacity <= m_capacity) {
            return false;
        }

        ApplyResizeBacking(std::move(backing), newCapacity, m_capacity);
        m_capacity = newCapacity;
        return true;
    }

    void AssignDescriptorSlots(uint32_t newCapacity)
    {
        BufferBase::DescriptorRequirements requirements{};

        requirements.createCBV = false;
        requirements.createSRV = true;
        requirements.createUAV = m_UAV;
        requirements.createNonShaderVisibleUAV = false;
        requirements.uavCounterOffset = 0;

        // SRV (structured)
        requirements.srvDesc = rhi::SrvDesc{
            .dimension = rhi::SrvDim::Buffer,
            .formatOverride = rhi::Format::Unknown,
            .buffer = {
                .kind = rhi::BufferViewKind::Structured,
                .firstElement = 0,
                .numElements = newCapacity,
                .structureByteStride = m_elementSize,
            },
        };

        // UAV (structured), no counter
        requirements.uavDesc = rhi::UavDesc{
            .dimension = rhi::UavDim::Buffer,
            .formatOverride = rhi::Format::Unknown,
            .buffer = {
                .kind = rhi::BufferViewKind::Structured,
                .firstElement = 0,
                .numElements = newCapacity,
                .structureByteStride = m_elementSize,
                .counterOffsetInBytes = 0,
            },
        };

        SetDescriptorRequirements(requirements);
    }

    void CreateBuffer(uint64_t capacity, size_t previousCapacity = 0) {
        auto backing = GpuBufferBacking::CreateUnique(
            rhi::HeapType::DeviceLocal,
            m_elementSize * capacity,
            GetGlobalResourceID(),
            m_UAV);
		ApplyResizeBacking(std::move(backing), capacity, previousCapacity);
    }

    void ApplyResizeBacking(std::unique_ptr<GpuBufferBacking> backing, uint64_t capacity, size_t previousCapacity = 0) {
		SetBacking(std::move(backing), m_elementSize * capacity);
        {
            std::lock_guard<std::recursive_mutex> lock(m_uploadPolicyMirrorMutex);
            SyncUploadPolicyState();
            EnsureCpuShadowSize(GetBufferSize());
            m_uploadPolicyState.OnBufferResized(GetBufferSize());
            const size_t previousBytes = previousCapacity * static_cast<size_t>(m_elementSize);
            const size_t replayBytes = (std::min)(previousBytes, m_cpuShadowData.size());
            if (replayBytes > 0u) {
                // Lazy buffers own an explicit CPU shadow. Re-upload preserved
                // bytes from that shadow after backing replacement so sparse and
                // bulk-written data survives buffer growth.
                if (org::runtime::GetActiveUploadService() != nullptr) {
                    BUFFER_UPLOAD(m_cpuShadowData.data(), replayBytes, org::runtime::UploadTarget::FromShared(shared_from_this()), 0u);
                } else {
                    StageOrUploadLocked(m_cpuShadowData.data(), replayBytes, 0u);
                    if (m_uploadPolicyState.HasPendingWork()) {
                        MarkUploadPolicyDirty();
                    }
                }
            }
        }

        for (const auto& bundle : m_metadataBundles) {
            ApplyMetadataToBacking(bundle);
        }

        AssignDescriptorSlots(static_cast<uint32_t>(capacity));

        SetName(name);

    }

    void SyncUploadPolicyState() {
        const auto tag = GetUploadPolicyTag();
        if (m_uploadPolicyState.GetPolicy().tag == tag) {
            return;
        }

        org::runtime::UploadPolicyConfig config{};
        config.tag = tag;
        m_uploadPolicyState.SetPolicy(config, GetBufferSize());
    }

    void StageOrUpload(const void* data, size_t size, size_t offset) {
        std::lock_guard<std::recursive_mutex> lock(m_uploadPolicyMirrorMutex);
        RetainCpuShadowWrite(data, size, offset);
        StageOrUploadLocked(data, size, offset);
    }

    void StageOrUploadLocked(const void* data, size_t size, size_t offset) {
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

    void EnsureCpuShadowSize(size_t size) {
        if (m_cpuShadowData.size() < size) {
            m_cpuShadowData.resize(size, std::byte{ 0 });
        }
    }

    void RetainCpuShadowWrite(const void* data, size_t size, size_t offset) {
        if (data == nullptr || size == 0) {
            return;
        }

        EnsureCpuShadowSize(offset + size);
        std::memcpy(m_cpuShadowData.data() + static_cast<std::ptrdiff_t>(offset), data, size);
    }

    void ApplyMetadataComponentBundle(const EntityComponentBundle& bundle) override {
        m_metadataBundles.emplace_back(bundle);
        ApplyMetadataToBacking(bundle);
    }

    org::runtime::BufferUploadPolicyState m_uploadPolicyState{};
    mutable std::recursive_mutex m_uploadPolicyMirrorMutex;
    AsyncBufferBackingResizeState m_asyncResizeState;
    uint32_t m_pendingResizeCapacity = 0u;
    bool m_pendingResizeValid = false;
};

} // namespace org

using org::LazyDynamicStructuredBuffer;
using org::LazyDynamicStructuredBufferBase;
