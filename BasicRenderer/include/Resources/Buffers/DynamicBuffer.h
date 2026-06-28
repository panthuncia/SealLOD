#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <map>
#include <mutex>
#include <set>
#include <functional>
#include <memory>
#include <typeinfo>
#include <string>
#include <utility>

#include "Resources/Resource.h"
#include "Resources/Buffers/BufferView.h"
#include "Resources/Buffers/DynamicBufferBase.h"
#include "Resources/GPUBacking/GpuBufferBacking.h"
#include "Resources/Buffers/MemoryBlock.h"
#include "Interfaces/IHasMemoryMetadata.h"
#include "Render/Runtime/UploadPolicyServiceAccess.h"

class DynamicBuffer : public ViewedDynamicBufferBase, public IHasMemoryMetadata, public IDeferredBackingResizeClient {
public:
    enum class ReadyResizePublishMode : std::uint8_t {
        PublishIfReady,
        DoNotPublish
    };

    struct PagedAllocation {
        size_t offset = 0;
        size_t size = 0;
        size_t allocationSize = 0;
        size_t stride = 0;
        size_t count = 0;

        bool IsValid() const {
            return allocationSize != 0 && stride != 0;
        }
    };

    struct AllocationProbe {
        std::vector<std::pair<size_t, size_t>> freeBlocks;
    };

    static std::shared_ptr<DynamicBuffer> CreateShared(size_t elementSize, size_t capacity = 64, std::string name = "", bool byteAddress = false, bool UAV = false) {
        return std::shared_ptr<DynamicBuffer>(new DynamicBuffer(byteAddress, elementSize, capacity, name, UAV));
    }

    ~DynamicBuffer() override;

    std::unique_ptr<BufferView> Allocate(size_t size, size_t elementSize);
    void ReserveBytes(size_t size);
    void RequestAsyncReserveBytes(size_t size);
    bool PublishReadyAsyncResize(bool wait = false);
    bool CanAllocateBytes(size_t size) const;
    bool PublishPendingBackingResize(bool wait) override { return PublishReadyAsyncResize(wait); }
    bool HasPendingBackingResize() const override;
    std::string GetDeferredBackingResizeDebugName() const override { return m_name; }
    void Deallocate(const BufferView* view);
    void DeallocateRange(size_t offset, size_t size);
    void DeallocatePages(const std::vector<PagedAllocation>& pages);
	std::unique_ptr<BufferView> AddData(const void* data, size_t size, size_t elementSize, size_t fullAllocationSize = 0);
    std::pair<size_t, size_t> AddDataRange(const void* data, size_t count, size_t elementSize);
    std::vector<PagedAllocation> AllocateRangesBatch(const std::vector<size_t>& counts, size_t elementSize);
    bool TryAllocateRangesBatch(
        const std::vector<size_t>& counts,
        size_t elementSize,
        std::vector<PagedAllocation>& ranges,
        ReadyResizePublishMode resizePublishMode = ReadyResizePublishMode::PublishIfReady);
    AllocationProbe SnapshotAllocationProbe() const;
    static bool CanConsumeAllocationProbeBytes(
        const AllocationProbe& probe,
        size_t totalSize);
    static bool CanConsumeAllocationProbe(
        const AllocationProbe& probe,
        const std::vector<size_t>& counts,
        size_t elementSize);
    static bool TryConsumeAllocationProbeBytes(
        AllocationProbe& probe,
        size_t totalSize);
    static bool TryConsumeAllocationProbe(
        AllocationProbe& probe,
        const std::vector<size_t>& counts,
        size_t elementSize);
    std::vector<PagedAllocation> AllocatePages(size_t count, size_t elementSize, size_t pageElementCount);
    void StageWriteRange(const void* data, size_t size, size_t offset);
    void StageWritePages(const void* data, size_t count, size_t elementSize, const std::vector<PagedAllocation>& pages, size_t pageElementCount);
    std::vector<PagedAllocation> AddDataPaged(const void* data, size_t count, size_t elementSize, size_t pageElementCount);
	std::vector<std::shared_ptr<BufferView>> AddDataBatch(const void* data, size_t count, size_t elementSize);
	void UpdateView(BufferView* view, const void* data) override;

    rg::runtime::BulkWriteHandle BeginBulkWrite() {
        auto lock = std::make_shared<std::unique_lock<std::recursive_mutex>>(m_uploadPolicyMirrorMutex);
        SyncUploadPolicyState();
        EnsureUploadPolicyRegistration();
        EnsureCpuShadowSize(GetBufferSize());
        rg::runtime::BulkWriteHandle handle{
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
        if (rg::runtime::GetActiveUploadPolicyService() != nullptr && m_uploadPolicyState.HasPendingWork()) {
            MarkUploadPolicyDirty();
        }
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
            rg::runtime::UploadTarget::FromShared(shared_from_this()),
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

    void RetainExternalUpload(const void* data, size_t size, size_t offset) {
        std::lock_guard<std::recursive_mutex> lock(m_uploadPolicyMirrorMutex);
        RetainCpuShadowWrite(data, size, offset);
    }

    uint64_t GetUploadPolicyLastFlushWrites() const override {
        std::lock_guard<std::recursive_mutex> lock(m_uploadPolicyMirrorMutex);
        return m_uploadPolicyState.GetLastFlushStats().flushedWrites;
    }

    uint64_t GetUploadPolicyLastFlushBytes() const override {
        std::lock_guard<std::recursive_mutex> lock(m_uploadPolicyMirrorMutex);
        return m_uploadPolicyState.GetLastFlushStats().flushedBytes;
    }

    size_t Size() const {
        return m_capacity;
    }

	void* GetMappedData() const {
		return m_mappedData;
	}

    static size_t AlignBufferCapacity(size_t size, bool byteAddress) {
        if (!byteAddress) {
            return size;
        }

        const size_t align = 4;
        const size_t rem = size % align;
        return rem ? (size + (align - rem)) : size;
    }

private:
    DynamicBuffer(bool byteAddress, size_t elementSize, size_t capacity, std::string name = "", bool UAV = false)
        : m_elementSize(elementSize), m_byteAddress(byteAddress), m_needsUpdate(false), m_UAV(UAV) {
        SetUploadPolicyTag(rg::runtime::UploadPolicyTag::Coalesced);

        size_t bufferSize = AlignBufferCapacity(elementSize * capacity, m_byteAddress);
		m_capacity = bufferSize;
        CreateBuffer(bufferSize);
        SetName(name);
        RegisterDeferredBackingResizeClient(this);
    }

    void OnSetName() override {
        if (name != "") {
			m_name = name;
            SetBackingName(m_baseName, m_name);
        }
        else {
            SetBackingName(m_baseName, "");
        }
    }

    void AssignDescriptorSlots();

	size_t m_elementSize;
	bool m_byteAddress;

    void* m_mappedData = nullptr;

    size_t m_capacity;
    bool m_needsUpdate;

    std::map<size_t, MemoryBlock> m_blocksByOffset;
    std::set<std::pair<size_t, size_t>> m_freeBlocks; // (size, offset)

    std::weak_ptr<ViewedDynamicBufferBase> m_cachedWeakPtr;
    bool m_weakPtrCached = false;

    inline static std::string m_baseName = "DynamicBuffer";
	std::string m_name = m_baseName;

    bool m_UAV = false;

    std::vector<EntityComponentBundle> m_metadataBundles;

    void CreateBuffer(size_t capacity);
    void GrowBuffer(size_t newSize);
    size_t ComputeReserveCapacityLocked(size_t size) const;
    bool ExtendTrackedCapacityLocked(size_t newCapacity);
    void RequestAsyncReserveBytesLocked(size_t size);
    void RaiseDeferredAsyncReserveBytes(size_t size);
    size_t ConsumeDeferredAsyncReserveBytes();
    bool PublishReadyAsyncResizeLocked(bool wait);
    void ApplyResizeBackingLocked(std::unique_ptr<GpuBufferBacking> newDataBuffer, size_t newSize, size_t previousCapacity);

    void SyncUploadPolicyState() {
        const auto tag = GetUploadPolicyTag();
        if (m_uploadPolicyState.GetPolicy().tag == tag) {
            return;
        }

        rg::runtime::UploadPolicyConfig config{};
        config.tag = tag;
        m_uploadPolicyState.SetPolicy(config, GetBufferSize());
    }

    void StageOrUpload(const void* data, size_t size, size_t offset);
    void StageOrUploadLocked(const void* data, size_t size, size_t offset);
    void EnsureCpuShadowSize(size_t size);
    void RetainCpuShadowWrite(const void* data, size_t size, size_t offset);

    void ApplyMetadataComponentBundle(const EntityComponentBundle& bundle) override {
        m_metadataBundles.emplace_back(bundle);
        ApplyMetadataToBacking(bundle);
    }

    rg::runtime::BufferUploadPolicyState m_uploadPolicyState{};
    // Authoritative CPU bytes for backing replacement. The upload-policy state
    // coalesces writes, but this shadow owns the long-lived contents.
    std::vector<std::byte> m_cpuShadowData;
    mutable std::recursive_mutex m_uploadPolicyMirrorMutex;
    mutable std::recursive_mutex m_allocationMutex;
    AsyncBufferBackingResizeState m_asyncResizeState;
    std::atomic_size_t m_deferredAsyncReserveBytes = 0;
    size_t m_pendingResizeCapacity = 0;
    size_t m_requestedResizeCapacity = 0;
    bool m_pendingResizeValid = false;
};
