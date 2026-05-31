#pragma once

#include <vector>
#include <string>
#include <memory>
#include <deque>
#include <algorithm>
#include <spdlog/spdlog.h>
#include <rhi.h>

#include "OpenRenderGraph/OpenRenderGraph.h"
#include "Resources/Buffers/BufferView.h"


using Microsoft::WRL::ComPtr;

class LazyDynamicStructuredBufferBase : public ViewedDynamicBufferBase, public IHasMemoryMetadata { // Necessary to store these in a templateless vector
public:
	virtual size_t GetElementSize() const = 0;
    virtual void UpdateView(BufferView* view, const void* data) = 0;
};

template <typename T>
class LazyDynamicStructuredBuffer : public LazyDynamicStructuredBufferBase {
public:

	static std::shared_ptr<LazyDynamicStructuredBuffer<T>> CreateShared(UINT capacity = 64, std::string name = "", uint64_t alignment = 1, bool UAV = false) {
		return std::shared_ptr<LazyDynamicStructuredBuffer<T>>(new LazyDynamicStructuredBuffer<T>(capacity, name, alignment, UAV));
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
        m_usedCapacity++;
		if (m_usedCapacity > m_capacity) { // Resize the buffer if necessary
            Resize(m_capacity * 2);
        }
		size_t index = m_usedCapacity - 1;
        return BufferView::CreateShared(viewedWeak, index * m_elementSize, m_elementSize, sizeof(T));
    }

	std::shared_ptr<BufferView> Add(const T& data) {
		auto view = Add();
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

        const size_t newCount = count - copiedFromFreeList;
        if (newCount != 0) {
            const uint64_t firstIndex = m_usedCapacity;
            const uint64_t requiredCapacity = m_usedCapacity + newCount;
            if (requiredCapacity > m_capacity) {
                uint32_t newCapacity = m_capacity > 0u ? m_capacity : 1u;
                while (requiredCapacity > newCapacity) {
                    newCapacity *= 2u;
                }
                Resize(newCapacity);
            }

            for (size_t i = 0; i < newCount; ++i) {
                const uint64_t index = firstIndex + i;
                views.push_back(BufferView::CreateShared(viewedWeak, index * m_elementSize, m_elementSize, sizeof(T)));
            }
            m_usedCapacity = requiredCapacity;

            if (data != nullptr) {
                StageOrUpload(data + copiedFromFreeList, sizeof(T) * newCount, firstIndex * m_elementSize);
            }
        }

        return views;
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

    void Resize(uint32_t newCapacity) {
        if (newCapacity > m_capacity) {
            CreateBuffer(newCapacity, m_capacity);
            m_capacity = newCapacity;
        }
    }

    void UpdateView(BufferView* view, const void* data) {
        if (view == nullptr) {
            return;
        }
        const uint64_t index = view->GetOffset() / m_elementSize;
        EnsureCapacityForIndex(index);
        StageOrUpload(data, sizeof(T), view->GetOffset());
    }

	void UpdateAt(uint64_t index, const T& data) {
        EnsureCapacityForIndex(index);
        StageOrUpload(&data, sizeof(T), index * m_elementSize);
    }

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

    uint64_t Size() {
        return m_usedCapacity;
    }

	size_t GetElementSize() const {
		return m_elementSize;
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

private:
    LazyDynamicStructuredBuffer(UINT capacity = 64, std::string name = "", uint64_t alignment = 1, bool UAV = false)
        : m_capacity(capacity), m_UAV(UAV), m_needsUpdate(false) {
        SetUploadPolicyTag(rg::runtime::UploadPolicyTag::CoalescedRetained);
        if (alignment == 0) {
			alignment = 1;
        }
		m_elementSize = static_cast<uint32_t>(((sizeof(T) + alignment - 1) / alignment) * alignment);
        CreateBuffer(capacity);
		SetName(name);
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

    void EnsureCapacityForIndex(uint64_t index) {
        if (index >= m_capacity) {
            uint32_t newCapacity = m_capacity > 0u ? m_capacity : 1u;
            while (index >= static_cast<uint64_t>(newCapacity)) {
                newCapacity *= 2u;
            }
            Resize(newCapacity);
        }

        const uint64_t requiredUsedCapacity = index + 1u;
        if (requiredUsedCapacity > m_usedCapacity) {
            m_usedCapacity = requiredUsedCapacity;
        }
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
        if (m_dataBuffer != nullptr) {
            QueueResourceCopyFromOldBacking(previousCapacity * static_cast<size_t>(m_elementSize));
        }
		CreateAndSetBacking(rhi::HeapType::DeviceLocal, m_elementSize * capacity, m_UAV);
        m_uploadPolicyState.OnBufferResized(GetBufferSize());

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

        rg::runtime::UploadPolicyConfig config{};
        config.tag = tag;
        m_uploadPolicyState.SetPolicy(config, GetBufferSize());
    }

    void StageOrUpload(const void* data, size_t size, size_t offset) {
        if (GetUploadPolicyTag() != rg::runtime::UploadPolicyTag::Immediate
            && rg::runtime::GetActiveUploadPolicyService() == nullptr) {
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

    void ApplyMetadataComponentBundle(const EntityComponentBundle& bundle) override {
        m_metadataBundles.emplace_back(bundle);
        ApplyMetadataToBacking(bundle);
    }

    rg::runtime::BufferUploadPolicyState m_uploadPolicyState{};
};
