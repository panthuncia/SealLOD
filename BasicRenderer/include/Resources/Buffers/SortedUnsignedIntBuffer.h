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
    static std::shared_ptr<SortedUnsignedIntBuffer> CreateShared(uint64_t capacity = 64, std::string name = "", bool UAV = false) {
        return std::shared_ptr<SortedUnsignedIntBuffer>(new SortedUnsignedIntBuffer(capacity, name, UAV));
    }

    // Insert an element while maintaining sorted order (deduped)
    void Insert(unsigned int element);
    void InsertMany(const std::vector<unsigned int>& elements);

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
        return static_cast<UINT>(m_data.size());
    }

private:
    SortedUnsignedIntBuffer(uint64_t capacity = 64, std::string name = "", bool UAV = false)
        : m_capacity(capacity), m_UAV(UAV), m_earliestModifiedIndex(0) {
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

    uint64_t m_capacity;
    uint64_t m_earliestModifiedIndex; // To avoid updating the entire buffer every time

    std::vector<EntityComponentBundle> m_metadataBundles;

    inline static std::string m_name = "SortedUnsignedIntBuffer";

    bool m_UAV = false;

    void CreateBuffer(uint64_t capacity);

    void GrowBuffer(uint64_t newSize);

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
