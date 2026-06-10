#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>
#include <functional>
#include <string>
#include <span>
#include <mutex>
#include <rhi.h>
#include <memory>
#include <utility>

#include <spdlog/spdlog.h>

#include "Resources/Resource.h"
#include "Resources/Buffers/DynamicBufferBase.h"
#include "Resources/GPUBacking/GpuBufferBacking.h"
#include "Interfaces/IHasMemoryMetadata.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "Render/Runtime/UploadPolicyServiceAccess.h"

using Microsoft::WRL::ComPtr;

template<class T>
class DynamicStructuredBuffer : public BufferBase, public IHasMemoryMetadata, public IDeferredBackingResizeClient {
public:

    static std::shared_ptr<DynamicStructuredBuffer<T>> CreateShared(UINT capacity = 64, std::string name = "", bool UAV = false) {
        return std::shared_ptr<DynamicStructuredBuffer<T>>(new DynamicStructuredBuffer<T>(capacity, name, UAV));
    }

    ~DynamicStructuredBuffer() override {
        UnregisterDeferredBackingResizeClient(this);
    }

    unsigned int Add(const T& element) {
        std::scoped_lock lock(m_mutex);
        if (m_data.size() >= m_capacity && !TryResize(m_capacity > 0u ? m_capacity * 2u : 1u)) {
            return InvalidIndex();
        }
        m_data.push_back(element);

        unsigned int index = static_cast<uint32_t>(m_data.size()) - 1; // TODO: Fix buffer max sizes

        StageOrUpload(&element, sizeof(T), index * sizeof(T));

        return index;
    }

    void RemoveAt(UINT index) {
        std::scoped_lock lock(m_mutex);
        if (index < m_data.size()) {
            m_data.erase(m_data.begin() + index);

			// If capacity is half or less, shrink the buffer
            if (m_data.size() <= m_capacity / 2 && m_capacity > 64) {
				auto newCapacity = m_capacity / 2;
				Resize(newCapacity);
			}

			// batch upload data after the removed index
			unsigned int countToUpload = static_cast<unsigned int>(m_data.size()) - index;
            if (countToUpload > 0) {
                StageOrUpload(&m_data[index], sizeof(T) * countToUpload, index * sizeof(T));
            }
        }
    }

    T& operator[](UINT index) {
        std::scoped_lock lock(m_mutex);
        return m_data[index];
    }

    const T& operator[](UINT index) const {
        std::scoped_lock lock(m_mutex);
        return m_data[index];
    }

    void Resize(uint32_t newCapacity) {
        std::scoped_lock lock(m_mutex);
        (void)TryResize(newCapacity);

    }

    bool TryResize(uint32_t newCapacity) {
        std::scoped_lock lock(m_mutex);
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

    void UpdateAt(UINT index, const T& element) {
        if (!TryUpdateAt(index, element)) {
            return;
        }
    }

    bool TryUpdateAt(UINT index, const T& element) {
        std::scoped_lock lock(m_mutex);
        if (!TryEnsureCapacityForIndex(index)) {
            return false;
        }
        if (static_cast<size_t>(index) >= m_data.size()) {
            m_data.resize(static_cast<size_t>(index) + 1u);
        }
        m_data[index] = element;
        StageOrUpload(&element, sizeof(T), index * sizeof(T));
        return true;
    }

    void EnsureSize(size_t elementCount, const T& fill = T{}) {
        std::scoped_lock lock(m_mutex);
        if (elementCount == 0u) {
            return;
        }
        if (!TryEnsureCapacityForIndex(elementCount - 1u)) {
            return;
        }
        if (m_data.size() < elementCount) {
            m_data.resize(elementCount, fill);
        }
    }

    void StageCurrentRange(size_t firstElement, size_t elementCount) {
        std::scoped_lock lock(m_mutex);
        if (elementCount == 0u || firstElement >= m_data.size()) {
            return;
        }
        const auto clampedCount = (std::min)(elementCount, m_data.size() - firstElement);
        StageOrUpload(
            m_data.data() + firstElement,
            clampedCount * sizeof(T),
            firstElement * sizeof(T));
    }

    void StageRange(size_t firstElement, std::span<const T> elements) {
        std::scoped_lock lock(m_mutex);
        if (elements.empty()) {
            return;
        }
        if (!TryEnsureCapacityForIndex(firstElement + elements.size() - 1u)) {
            return;
        }
        EnsureSize(firstElement + elements.size());
        std::copy(elements.begin(), elements.end(), m_data.begin() + firstElement);
        StageCurrentRange(firstElement, elements.size());
    }

    const std::vector<T>& Data() const {
        std::scoped_lock lock(m_mutex);
        return m_data;
    }

    void ReplaceData(std::vector<T> data) {
        std::scoped_lock lock(m_mutex);
        const auto elementCount = data.size();
        if (elementCount > 0u) {
            if (!TryEnsureCapacityForIndex(elementCount - 1u)) {
                m_data = std::move(data);
                return;
            }
        }

        m_data = std::move(data);
        if (!m_data.empty()) {
            StageOrUpload(m_data.data(), m_data.size() * sizeof(T), 0u);
        }
    }

    UINT Size() {
        std::scoped_lock lock(m_mutex);
        return static_cast<uint32_t>(m_data.size());
    }

    UINT Capacity() const {
        std::scoped_lock lock(m_mutex);
        return m_capacity;
    }

    UINT ResidentCapacity() const {
        std::scoped_lock lock(m_mutex);
        return static_cast<UINT>(GetBufferSize() / sizeof(T));
    }

    bool TryEnsureCapacityForIndex(size_t index) {
        std::scoped_lock lock(m_mutex);
        if (index < m_capacity) {
            return true;
        }

        uint32_t newCapacity = m_capacity > 0u ? m_capacity : 1u;
        while (index >= static_cast<size_t>(newCapacity)) {
            newCapacity *= 2u;
        }

        return TryResize(newCapacity);
    }

    bool PublishReadyAsyncResize(bool wait = false) {
        std::scoped_lock lock(m_mutex);
        return PublishReadyAsyncResizeInternal(wait);
    }

    bool PublishPendingBackingResize(bool wait) override {
        return PublishReadyAsyncResize(wait);
    }

    bool HasPendingBackingResize() const override {
        std::scoped_lock lock(m_mutex);
        return m_pendingResizeValid || m_asyncResizeState.HasPending();
    }

    std::string GetDeferredBackingResizeDebugName() const override {
        return name;
    }

    static constexpr unsigned int InvalidIndex() {
        return (std::numeric_limits<unsigned int>::max)();
    }

private:
    DynamicStructuredBuffer(UINT capacity = 64, std::string bufName = "", bool UAV = false)
        : m_capacity(capacity), m_UAV(UAV), m_needsUpdate(false) {
		SetUploadPolicyTag(rg::runtime::UploadPolicyTag::Coalesced);
		name = bufName;
        CreateBuffer(capacity);
        RegisterDeferredBackingResizeClient(this);
    }

    void OnUploadPolicyBeginFrame() override {
        std::scoped_lock lock(m_mutex);
        SyncUploadPolicyState();
        m_uploadPolicyState.BeginFrame();
    }

    void OnUploadPolicyFlush() override {
        std::scoped_lock lock(m_mutex);
        SyncUploadPolicyState();
        m_uploadPolicyState.FlushToUploadService(
            rg::runtime::UploadTarget::FromShared(shared_from_this()),
            [this](size_t offset, size_t size) -> const void* {
                const auto byteSize = m_data.size() * sizeof(T);
                if (offset + size > byteSize) {
                    return nullptr;
                }
                return reinterpret_cast<const std::byte*>(m_data.data()) + static_cast<std::ptrdiff_t>(offset);
            });
    }

    bool HasPendingUploadPolicyWork() const override {
        std::scoped_lock lock(m_mutex);
        return m_uploadPolicyState.HasPendingWork();
    }

    uint64_t GetUploadPolicyLastFlushWrites() const override {
        std::scoped_lock lock(m_mutex);
        return m_uploadPolicyState.GetLastFlushStats().flushedWrites;
    }

    uint64_t GetUploadPolicyLastFlushBytes() const override {
        std::scoped_lock lock(m_mutex);
        return m_uploadPolicyState.GetLastFlushStats().flushedBytes;
    }

    void OnSetName() override {
        std::scoped_lock lock(m_mutex);
        SetBackingName(m_name, name);
    }

    mutable std::recursive_mutex m_mutex;
    std::vector<T> m_data;
    uint32_t m_capacity;
    bool m_needsUpdate;

    inline static std::string m_name = "DynamicStructuredBuffer";

    bool m_UAV = false;

    std::vector<EntityComponentBundle> m_metadataBundles;

    void EnsureCapacityForIndex(size_t index) {
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
                .byteSize = sizeof(T) * static_cast<size_t>(m_pendingResizeCapacity),
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
            .byteSize = sizeof(T) * static_cast<size_t>(newCapacity),
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
                    "DynamicStructuredBuffer '{}' id={} async resize failed: {}",
                    name,
                    GetGlobalResourceID(),
                    e.what());
            }
            catch (...) {
                spdlog::error(
                    "DynamicStructuredBuffer '{}' id={} async resize failed with unknown exception",
                    name,
                    GetGlobalResourceID());
            }
            m_pendingResizeCapacity = 0u;
            m_pendingResizeValid = false;
            return false;
        }

        auto backing = std::move(resizeResult->backing);
        const uint32_t newCapacity = static_cast<uint32_t>(resizeResult->byteSize / sizeof(T));
        m_pendingResizeCapacity = 0u;
        m_pendingResizeValid = false;
        if (!backing || newCapacity <= m_capacity) {
            return false;
        }

        ApplyResizeBacking(std::move(backing), newCapacity, m_capacity);
        m_capacity = newCapacity;
        return true;
    }

    void SyncUploadPolicyState() {
        const auto tag = GetUploadPolicyTag();
        if (m_uploadPolicyState.GetPolicy().tag == tag) {
            return;
        }

        rg::runtime::UploadPolicyConfig config{};
        config.tag = tag;
        m_uploadPolicyState.SetPolicy(config, GetBufferSize());
    }

    void StageOrUpload(const void* data, size_t size, size_t offset) {
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

    void AssignDescriptorSlots(uint32_t capacity)
    {
        BufferBase::DescriptorRequirements requirements{};

        requirements.createCBV = false;
        requirements.createSRV = true;
        requirements.createUAV = m_UAV;
        requirements.createNonShaderVisibleUAV = false;
        requirements.uavCounterOffset = 0;

        // SRV (structured buffer)
        requirements.srvDesc = rhi::SrvDesc{
        	.dimension = rhi::SrvDim::Buffer,
        	.formatOverride = rhi::Format::Unknown,
            .buffer = {
                .kind = rhi::BufferViewKind::Structured,
                .firstElement = 0,
                .numElements = capacity,
                .structureByteStride = static_cast<uint32_t>(sizeof(T)),
            },
        };

        // UAV (structured buffer), no counter
        requirements.uavDesc = rhi::UavDesc{
        	.dimension = rhi::UavDim::Buffer,
        	.formatOverride = rhi::Format::Unknown,
            .buffer = {
                .kind = rhi::BufferViewKind::Structured,
                .firstElement = 0,
                .numElements = capacity,
                .structureByteStride = static_cast<uint32_t>(sizeof(T)),
                .counterOffsetInBytes = 0,
            },
        };

        SetDescriptorRequirements(requirements);
    }


    void CreateBuffer(size_t capacity, size_t previousCapacity = 0) {
        auto backing = GpuBufferBacking::CreateUnique(
            rhi::HeapType::DeviceLocal,
            sizeof(T) * capacity,
            GetGlobalResourceID(),
            m_UAV);
        ApplyResizeBacking(std::move(backing), capacity, previousCapacity);
    }

    void ApplyResizeBacking(std::unique_ptr<GpuBufferBacking> backing, size_t capacity, size_t previousCapacity = 0) {
        const size_t replayElements = (std::min)(m_data.size(), capacity);
        if (previousCapacity != 0u) {
            spdlog::info(
                "DynamicStructuredBuffer '{}' id={} GrowBuffer SetBacking begin previousCapacity={} newCapacity={}",
                name,
                GetGlobalResourceID(),
                previousCapacity,
                capacity);
        }

		SetBacking(std::move(backing), sizeof(T) * capacity);
        if (previousCapacity != 0u) {
            spdlog::info(
                "DynamicStructuredBuffer '{}' id={} GrowBuffer SetBacking complete bufferSize={} backingGeneration={}",
                name,
                GetGlobalResourceID(),
                GetBufferSize(),
                GetBackingGeneration());
        }
        m_uploadPolicyState.OnBufferResized(GetBufferSize());
        SetName(name);

        for (const auto& bundle : m_metadataBundles) {
            ApplyMetadataToBacking(bundle);
        }

        AssignDescriptorSlots(static_cast<uint32_t>(capacity));

        // DynamicStructuredBuffer keeps a CPU-side authoritative copy in m_data.
        // After backing replacement, descriptor users can see the new resource
        // immediately, so replay existing rows directly to the active upload
        // service when one is available. Falling back to retained coalescing here
        // can leave generation/visibility sidecars temporarily zeroed after a
        // grow, which makes append-only active draw entries look stale on GPU.
        if (replayElements > 0u) {
            SyncUploadPolicyState();
            const size_t replayBytes = replayElements * sizeof(T);
            if (rg::runtime::GetActiveUploadService() != nullptr) {
                BUFFER_UPLOAD(m_data.data(), replayBytes, rg::runtime::UploadTarget::FromShared(shared_from_this()), 0u);
                spdlog::debug(
                    "DynamicStructuredBuffer '{}' id={} GrowBuffer replayed CPU rows={} bytes={}",
                    name,
                    GetGlobalResourceID(),
                    replayElements,
                    replayBytes);
            }
            else {
                StageOrUpload(m_data.data(), replayBytes, 0u);
                if (m_uploadPolicyState.HasPendingWork()) {
                    MarkUploadPolicyDirty();
                }
            }
        }

        if (previousCapacity != 0u) {
            spdlog::info(
                "DynamicStructuredBuffer '{}' id={} GrowBuffer complete finalCapacity={}",
                name,
                GetGlobalResourceID(),
                capacity);
        }
    }

    void ApplyMetadataComponentBundle(const EntityComponentBundle& bundle) override {
        m_metadataBundles.emplace_back(bundle);
        ApplyMetadataToBacking(bundle);
    }

    rg::runtime::BufferUploadPolicyState m_uploadPolicyState{};
    AsyncBufferBackingResizeState m_asyncResizeState;
    uint32_t m_pendingResizeCapacity = 0u;
    bool m_pendingResizeValid = false;
};
