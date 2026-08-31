#pragma once

#include <vector>
#include <string>
#include <algorithm> // For std::lower_bound, std::upper_bound
#include <cstddef>
#include <cstring>
#include <functional>
#include <mutex>
#include <rhi.h>

#include "Resources/Buffers/Buffer.h"
#include "Resources/Resource.h"
#include "Resources/Buffers/DynamicBufferBase.h"
#include "Resources/GPUBacking/GpuBufferBacking.h"
#include "Interfaces/IHasMemoryMetadata.h"
#include "Render/Runtime/UploadPolicyServiceAccess.h"

using Microsoft::WRL::ComPtr;

class SortedUnsignedIntBuffer : public BufferBase, public IHasMemoryMetadata, public IDeferredBackingResizeClient {
public:
    struct ActiveDrawSetEntry {
        uint32_t drawRecordIndex = 0;
        uint32_t generation = 0;
    };
    using ActiveMutationCallback = std::function<void(
        bool replace, std::uint64_t revision,
        std::shared_ptr<const std::vector<ActiveDrawSetEntry>> entries)>;

    static std::shared_ptr<SortedUnsignedIntBuffer> CreateShared(uint64_t capacity = 64, std::string name = "", bool UAV = false) {
        return std::shared_ptr<SortedUnsignedIntBuffer>(new SortedUnsignedIntBuffer(capacity, name, UAV));
    }

    static std::shared_ptr<SortedUnsignedIntBuffer> CreateActiveDrawSetShared(uint64_t capacity = 64, std::string name = "") {
        return std::shared_ptr<SortedUnsignedIntBuffer>(new SortedUnsignedIntBuffer(capacity, name, false, true));
    }

    static std::shared_ptr<SortedUnsignedIntBuffer> CreateGraphActiveDrawSetShared(uint64_t capacity = 64, std::string name = "") {
        return std::shared_ptr<SortedUnsignedIntBuffer>(new SortedUnsignedIntBuffer(capacity, name, false, true, true));
    }

    ~SortedUnsignedIntBuffer() override;

    // Insert an element while maintaining sorted order (deduped)
    void Insert(unsigned int element);
    void InsertMany(const std::vector<unsigned int>& elements);
    void RequestAsyncReserveCapacity(uint64_t requiredSize);
    bool PublishReadyAsyncResize(bool wait = false);
    bool PublishPendingBackingResize(bool wait) override { return PublishReadyAsyncResize(wait); }
    bool HasPendingBackingResize() const override { return m_pendingResizeValid || m_asyncResizeState.HasPending(); }
    std::string GetDeferredBackingResizeDebugName() const override { return GetName(); }
    void AppendActiveEntries(const std::vector<ActiveDrawSetEntry>& entries);
    void AssignActiveSnapshot(std::vector<ActiveDrawSetEntry> entries);
    std::vector<ActiveDrawSetEntry> SnapshotActiveEntries() const;
    uint64_t MutationRevision() const {
        if (m_activeEntryMode) {
            std::lock_guard lock(m_activeStateMutex);
            return m_mutationRevision;
        }
        return m_mutationRevision;
    }
    void SetActiveMutationCallback(ActiveMutationCallback callback);

    // Remove an element (and shift the tail on GPU)
    void Remove(unsigned int element);
    void RemoveMany(const std::vector<unsigned int>& elements);

    // Get element at index
    unsigned int& operator[](UINT index) {
        return m_data[index];
    }

    const unsigned int& operator[](UINT index) const {
        return m_data[index];
    }

    UINT Size() const {
        if (m_activeEntryMode) {
            std::lock_guard lock(m_activeStateMutex);
            return static_cast<UINT>(m_activeEntries.size());
        }
        return static_cast<UINT>(m_data.size());
    }

    uint64_t ResidentCapacity() const {
        if (m_graphManaged) return m_capacity;
        const auto stride = ElementStride();
        return stride == 0u ? 0u : GetBufferSize() / stride;
    }

    uint64_t ResidentSize() const {
        if (m_graphManaged) return Size();
        return std::min<uint64_t>(Size(), ResidentCapacity());
    }

    uint64_t ElementStride() const {
        return m_activeEntryMode ? sizeof(ActiveDrawSetEntry) : sizeof(unsigned int);
    }

    UINT LiveSize() const {
        return static_cast<UINT>(m_liveSize);
    }

    void SetLiveSize(uint64_t size) {
        m_liveSize = size;
    }

    UINT ActiveTombstoneEstimate() const {
        return static_cast<UINT>(m_activeTombstoneEstimate);
    }

    UINT EstimatedActiveLiveSize() const {
        const auto total = static_cast<uint64_t>(Size());
        const auto stale = std::min(m_activeTombstoneEstimate, total);
        return static_cast<UINT>(total - stale);
    }

    void AddActiveTombstoneEstimate(uint64_t count) {
        const auto total = static_cast<uint64_t>(Size());
        m_activeTombstoneEstimate = std::min(total, m_activeTombstoneEstimate + count);
    }

    void ResetActiveTombstoneEstimate() {
        m_activeTombstoneEstimate = 0;
    }

    bool ActiveEntryMode() const {
        return m_activeEntryMode;
    }

private:
    SortedUnsignedIntBuffer(uint64_t capacity = 64, std::string name = "", bool UAV = false, bool activeEntryMode = false, bool graphManaged = false)
        : m_capacity(capacity), m_earliestModifiedIndex(0), m_UAV(UAV), m_activeEntryMode(activeEntryMode), m_graphManaged(graphManaged) {
        SetUploadPolicyTag(org::runtime::UploadPolicyTag::Coalesced);
        if (!m_graphManaged) CreateBuffer(capacity);
        SetName(name);
        if (!m_graphManaged) RegisterDeferredBackingResizeClient(this);
    }

    void OnUploadPolicyBeginFrame() override {
        SyncUploadPolicyState();
        m_uploadPolicyState.BeginFrame();
    }

    void OnUploadPolicyFlush() override {
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
        return m_uploadPolicyState.HasPendingWork();
    }

    uint64_t GetUploadPolicyLastFlushWrites() const override {
        return m_uploadPolicyState.GetLastFlushStats().flushedWrites;
    }

    uint64_t GetUploadPolicyLastFlushBytes() const override {
        return m_uploadPolicyState.GetLastFlushStats().flushedBytes;
    }

    void OnSetName() override;

    void AssignDescriptorSlots();

    // Sorted list of unsigned integers
    std::vector<unsigned int> m_data;
    // Graph publication mutates active lists on streaming workers while wind
    // and other extensions may snapshot them on the render thread. The owning
    // ObjectManager mutex orders writers but cannot protect those external
    // readers, so active-list state has its own narrow synchronization domain.
    mutable std::mutex m_activeStateMutex;
    std::vector<ActiveDrawSetEntry> m_activeEntries;
    std::vector<std::byte> m_cpuShadowData;

    uint64_t m_capacity;
    uint64_t m_liveSize = 0;
    uint64_t m_activeTombstoneEstimate = 0;
    uint64_t m_mutationRevision = 1;
    ActiveMutationCallback m_activeMutationCallback;
    uint64_t m_earliestModifiedIndex; // To avoid updating the entire buffer every time

    std::vector<EntityComponentBundle> m_metadataBundles;

    inline static std::string m_name = "SortedUnsignedIntBuffer";

    bool m_UAV = false;
    bool m_activeEntryMode = false;
    bool m_graphManaged = false;
    AsyncBufferBackingResizeState m_asyncResizeState;
    uint64_t m_pendingResizeCapacity = 0;
    bool m_pendingResizeValid = false;

    void CreateBuffer(uint64_t capacity);

    void GrowBuffer(uint64_t newSize);
    void ApplyResizeBacking(std::unique_ptr<GpuBufferBacking> newDataBuffer, uint64_t newCapacity);
    void EnsureCapacityForSize(uint64_t requiredSize);

    void SyncUploadPolicyState() {
        const auto tag = GetUploadPolicyTag();
        if (m_uploadPolicyState.GetPolicy().tag == tag) {
            return;
        }

        org::runtime::UploadPolicyConfig config{};
        config.tag = tag;
        m_uploadPolicyState.SetPolicy(config, GetBufferSize());
    }

    void StageOrUpload(const void* data, size_t size, size_t offset);

    void ApplyMetadataComponentBundle(const EntityComponentBundle& bundle) override {
        m_metadataBundles.emplace_back(bundle);
        ApplyMetadataToBacking(bundle);
    }

    org::runtime::BufferUploadPolicyState m_uploadPolicyState{};

    void EnsureCpuShadowSize(size_t size) {
        if (m_cpuShadowData.size() < size) {
            m_cpuShadowData.resize(size, std::byte{ 0 });
        }
    }

    void RetainCpuShadowWrite(const void* data, size_t size, size_t offset) {
        if (!data || size == 0) {
            return;
        }
        EnsureCpuShadowSize(offset + size);
        std::memcpy(m_cpuShadowData.data() + static_cast<std::ptrdiff_t>(offset), data, size);
    }
};
