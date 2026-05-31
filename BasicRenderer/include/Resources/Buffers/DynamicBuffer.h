#pragma once

#pragma once

#include <vector>
#include <map>
#include <set>
#include <functional>
#include <typeinfo>
#include <string>

#include "Resources/Resource.h"
#include "Resources/Buffers/BufferView.h"
#include "Resources/Buffers/DynamicBufferBase.h"
#include "Resources/Buffers/MemoryBlock.h"
#include "Interfaces/IHasMemoryMetadata.h"
#include "Render/Runtime/UploadPolicyServiceAccess.h"

class DynamicBuffer : public ViewedDynamicBufferBase, public IHasMemoryMetadata {
public:

    static std::shared_ptr<DynamicBuffer> CreateShared(size_t elementSize, size_t capacity = 64, std::string name = "", bool byteAddress = false, bool UAV = false) {
        return std::shared_ptr<DynamicBuffer>(new DynamicBuffer(byteAddress, elementSize, capacity, name, UAV));
    }

    std::unique_ptr<BufferView> Allocate(size_t size, size_t elementSize);
    void ReserveBytes(size_t size);
    void Deallocate(const BufferView* view);
	std::unique_ptr<BufferView> AddData(const void* data, size_t size, size_t elementSize, size_t fullAllocationSize = 0);
	std::vector<std::shared_ptr<BufferView>> AddDataBatch(const void* data, size_t count, size_t elementSize);
	void UpdateView(BufferView* view, const void* data) override;

    rg::runtime::BulkWriteHandle BeginBulkWrite() {
        SyncUploadPolicyState();
        EnsureUploadPolicyRegistration();
        return m_uploadPolicyState.PrepareBulkWrite(GetBufferSize());
    }

    void EndBulkWrite(size_t dirtyOffset, size_t dirtySize) {
        m_uploadPolicyState.CommitBulkRegion(dirtyOffset, dirtySize);
        if (m_uploadPolicyState.HasPendingWork()) {
            MarkUploadPolicyDirty();
        }
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
        : m_byteAddress(byteAddress), m_elementSize(elementSize), m_UAV(UAV), m_needsUpdate(false) {
        SetUploadPolicyTag(rg::runtime::UploadPolicyTag::Coalesced);

        size_t bufferSize = AlignBufferCapacity(elementSize * capacity, m_byteAddress);
		m_capacity = bufferSize;
        CreateBuffer(bufferSize);
        SetName(name);
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
