#pragma once

#include <vector>
#include <string>
#include <algorithm> // For std::lower_bound, std::upper_bound
#include <rhi.h>

#include "Resources/Buffers/Buffer.h"
#include "Resources/Resource.h"
#include "Resources/Buffers/DynamicBufferBase.h"
#include "Interfaces/IHasMemoryMetadata.h"
#include "Render/Runtime/UploadPolicyServiceAccess.h"

using Microsoft::WRL::ComPtr;

class SortedUnsignedIntBuffer : public BufferBase, public IHasMemoryMetadata {
public:
    struct ActiveDrawSetEntry {
        uint32_t drawRecordIndex = 0;
        uint32_t generation = 0;
    };

    static std::shared_ptr<SortedUnsignedIntBuffer> CreateShared(uint64_t capacity = 64, std::string name = "", bool UAV = false) {
        return std::shared_ptr<SortedUnsignedIntBuffer>(new SortedUnsignedIntBuffer(capacity, name, UAV));
    }

    static std::shared_ptr<SortedUnsignedIntBuffer> CreateActiveDrawSetShared(uint64_t capacity = 64, std::string name = "") {
        return std::shared_ptr<SortedUnsignedIntBuffer>(new SortedUnsignedIntBuffer(capacity, name, false, true));
    }

    // Insert an element while maintaining sorted order (deduped)
    void Insert(unsigned int element);
    void InsertMany(const std::vector<unsigned int>& elements);
    void AppendActiveEntries(const std::vector<ActiveDrawSetEntry>& entries);
    void AssignActiveSnapshot(std::vector<ActiveDrawSetEntry> entries);
    std::vector<ActiveDrawSetEntry> SnapshotActiveEntries() const;
    uint64_t MutationRevision() const {
        return m_mutationRevision;
    }

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
        return m_activeEntryMode ? static_cast<UINT>(m_activeEntries.size()) : static_cast<UINT>(m_data.size());
    }

    UINT LiveSize() const {
        return static_cast<UINT>(m_liveSize);
    }

    void SetLiveSize(uint64_t size) {
        m_liveSize = size;
    }

    bool ActiveEntryMode() const {
        return m_activeEntryMode;
    }

private:
    SortedUnsignedIntBuffer(uint64_t capacity = 64, std::string name = "", bool UAV = false, bool activeEntryMode = false)
        : m_capacity(capacity), m_UAV(UAV), m_earliestModifiedIndex(0), m_activeEntryMode(activeEntryMode) {
        SetUploadPolicyTag(rg::runtime::UploadPolicyTag::CoalescedRetained);
        CreateBuffer(capacity);
        SetName(name);
    }

    void OnUploadPolicyBeginFrame() override {
        SyncUploadPolicyState();
        m_uploadPolicyState.BeginFrame();
    }

    void OnUploadPolicyFlush() override {
        SyncUploadPolicyState();
        m_uploadPolicyState.FlushToUploadService(rg::runtime::UploadTarget::FromShared(shared_from_this()));
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
    std::vector<ActiveDrawSetEntry> m_activeEntries;

    uint64_t m_capacity;
    uint64_t m_liveSize = 0;
    uint64_t m_mutationRevision = 1;
    uint64_t m_earliestModifiedIndex; // To avoid updating the entire buffer every time

    std::vector<EntityComponentBundle> m_metadataBundles;

    inline static std::string m_name = "SortedUnsignedIntBuffer";

    bool m_UAV = false;
    bool m_activeEntryMode = false;

    void CreateBuffer(uint64_t capacity);

    void GrowBuffer(uint64_t newSize);
    void EnsureCapacityForSize(uint64_t requiredSize);

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

    void ApplyMetadataComponentBundle(const EntityComponentBundle& bundle) override {
        m_metadataBundles.emplace_back(bundle);
        ApplyMetadataToBacking(bundle);
    }

    rg::runtime::BufferUploadPolicyState m_uploadPolicyState{};
};
