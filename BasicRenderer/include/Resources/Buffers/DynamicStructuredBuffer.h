#pragma once

#include <algorithm>
#include <vector>
#include <functional>
#include <string>
#include <span>
#include <rhi.h>
#include <memory>
#include <utility>

#include <spdlog/spdlog.h>

#include "Resources/Resource.h"
#include "Resources/Buffers/DynamicBufferBase.h"
#include "Interfaces/IHasMemoryMetadata.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "Render/Runtime/UploadPolicyServiceAccess.h"

using Microsoft::WRL::ComPtr;

template<class T>
class DynamicStructuredBuffer : public BufferBase, public IHasMemoryMetadata {
public:

    static std::shared_ptr<DynamicStructuredBuffer<T>> CreateShared(UINT capacity = 64, std::string name = "", bool UAV = false) {
        return std::shared_ptr<DynamicStructuredBuffer<T>>(new DynamicStructuredBuffer<T>(capacity, name, UAV));
    }

    unsigned int Add(const T& element) {
        if (m_data.size() >= m_capacity) {
            Resize(m_capacity * 2);
        }
        m_data.push_back(element);

        unsigned int index = static_cast<uint32_t>(m_data.size()) - 1; // TODO: Fix buffer max sizes

        StageOrUpload(&element, sizeof(T), index * sizeof(T));

        return index;
    }

    void RemoveAt(UINT index) {
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
        return m_data[index];
    }

    const T& operator[](UINT index) const {
        return m_data[index];
    }

    void Resize(uint32_t newCapacity) {
        if (newCapacity > m_capacity) {
            CreateBuffer(newCapacity, m_capacity);
            m_capacity = newCapacity;
        }

    }

    void UpdateAt(UINT index, const T& element) {
        EnsureCapacityForIndex(index);
        if (static_cast<size_t>(index) >= m_data.size()) {
            m_data.resize(static_cast<size_t>(index) + 1u);
        }
        m_data[index] = element;
        StageOrUpload(&element, sizeof(T), index * sizeof(T));
    }

    void EnsureSize(size_t elementCount, const T& fill = T{}) {
        if (elementCount == 0u) {
            return;
        }
        EnsureCapacityForIndex(elementCount - 1u);
        if (m_data.size() < elementCount) {
            m_data.resize(elementCount, fill);
        }
    }

    void StageCurrentRange(size_t firstElement, size_t elementCount) {
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
        if (elements.empty()) {
            return;
        }
        EnsureSize(firstElement + elements.size());
        std::copy(elements.begin(), elements.end(), m_data.begin() + firstElement);
        StageCurrentRange(firstElement, elements.size());
    }

    const std::vector<T>& Data() const {
        return m_data;
    }

    void ReplaceData(std::vector<T> data) {
        const auto elementCount = data.size();
        if (elementCount > 0u) {
            EnsureCapacityForIndex(elementCount - 1u);
        }

        m_data = std::move(data);
        if (!m_data.empty()) {
            StageOrUpload(m_data.data(), m_data.size() * sizeof(T), 0u);
        }
    }

    UINT Size() {
        return static_cast<uint32_t>(m_data.size());
    }

private:
    DynamicStructuredBuffer(UINT capacity = 64, std::string bufName = "", bool UAV = false)
        : m_capacity(capacity), m_UAV(UAV), m_needsUpdate(false) {
		SetUploadPolicyTag(rg::runtime::UploadPolicyTag::CoalescedRetained);
		name = bufName;
        CreateBuffer(capacity);
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

    void OnSetName() override {
        SetBackingName(m_name, name);
    }

    std::vector<T> m_data;
    uint32_t m_capacity;
    bool m_needsUpdate;

    inline static std::string m_name = "DynamicStructuredBuffer";

    bool m_UAV = false;

    std::vector<EntityComponentBundle> m_metadataBundles;

    void EnsureCapacityForIndex(size_t index) {
        if (index < m_capacity) {
            return;
        }

        uint32_t newCapacity = m_capacity > 0u ? m_capacity : 1u;
        while (index >= static_cast<size_t>(newCapacity)) {
            newCapacity *= 2u;
        }

        Resize(newCapacity);
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
        const size_t replayElements = (std::min)(m_data.size(), capacity);
        if (previousCapacity != 0u) {
            spdlog::info(
                "DynamicStructuredBuffer '{}' id={} GrowBuffer SetBacking begin previousCapacity={} newCapacity={}",
                name,
                GetGlobalResourceID(),
                previousCapacity,
                capacity);
        }

		CreateAndSetBacking(rhi::HeapType::DeviceLocal, sizeof(T) * capacity, m_UAV);
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
                m_uploadPolicyState.RetainExternalWrite(m_data.data(), replayBytes, 0u, GetBufferSize());
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
};
