#include "Resources/Buffers/SortedUnsignedIntBuffer.h"

#include <algorithm>

#include "Resources/ExternalBackingResource.h"
#include "Resources/GPUBacking/GpuBufferBacking.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "Render/Runtime/UploadPolicyServiceAccess.h"
#include "Managers/Singletons/DeviceManager.h"

void SortedUnsignedIntBuffer::OnSetName() {
    if (!m_dataBuffer) {
        return;
    }

    if (name != "") {
        m_dataBuffer->SetName((m_name + ": " + name).c_str());
    }
    else {
        m_dataBuffer->SetName(m_name.c_str());
    }
}

void SortedUnsignedIntBuffer::Insert(unsigned int element) {
    // Resize the buffer if necessary
    if (m_data.size() >= m_capacity) {
        GrowBuffer(m_capacity * 2);
    }

    // Find the insertion point
    auto it = std::lower_bound(m_data.begin(), m_data.end(), element);
    // Prevent duplicates
    if (it != m_data.end() && *it == element) {
        return; // already present
    }

    uint32_t index = static_cast<uint32_t>(std::distance(m_data.begin(), it));
    m_data.insert(it, element);

    // Update the earliest modified index
    if (index < m_earliestModifiedIndex) {
        m_earliestModifiedIndex = index;
    }

    // Upload the entire suffix so GPU content matches the CPU vector after the insertion shift
    const unsigned int* src = m_data.data() + index;
    const uint32_t count = static_cast<uint32_t>(m_data.size() - index);
    StageOrUpload(src, sizeof(unsigned int) * count, index * sizeof(unsigned int));
}

void SortedUnsignedIntBuffer::InsertMany(const std::vector<unsigned int>& elements) {
    if (elements.empty()) {
        return;
    }

    std::vector<unsigned int> sortedElements = elements;
    std::sort(sortedElements.begin(), sortedElements.end());
    sortedElements.erase(std::unique(sortedElements.begin(), sortedElements.end()), sortedElements.end());

    if (m_data.empty()) {
        while (sortedElements.size() > m_capacity) {
            GrowBuffer(m_capacity * 2);
        }
        m_data = std::move(sortedElements);
        StageOrUpload(m_data.data(), sizeof(unsigned int) * m_data.size(), 0);
        m_earliestModifiedIndex = 0;
        return;
    }

    std::vector<unsigned int> merged;
    merged.reserve(m_data.size() + sortedElements.size());
    std::set_union(
        m_data.begin(),
        m_data.end(),
        sortedElements.begin(),
        sortedElements.end(),
        std::back_inserter(merged));

    if (merged.size() == m_data.size()) {
        return;
    }

    while (merged.size() > m_capacity) {
        GrowBuffer(m_capacity * 2);
    }

    auto firstDiff = std::mismatch(m_data.begin(), m_data.end(), merged.begin(), merged.end());
    const auto dirtyIndex = static_cast<std::size_t>(std::distance(m_data.begin(), firstDiff.first));
    m_data = std::move(merged);

    const unsigned int* src = m_data.data() + dirtyIndex;
    const auto count = m_data.size() - dirtyIndex;
    StageOrUpload(src, sizeof(unsigned int) * count, dirtyIndex * sizeof(unsigned int));

    if (dirtyIndex < m_earliestModifiedIndex) {
        m_earliestModifiedIndex = dirtyIndex;
    }
}

void SortedUnsignedIntBuffer::Remove(unsigned int element) {
    // Find the element
    auto it = std::lower_bound(m_data.begin(), m_data.end(), element);

    if (it != m_data.end() && *it == element) {
        const uint32_t index = static_cast<uint32_t>(std::distance(m_data.begin(), it));

        // Erase from CPU
        m_data.erase(it);

        // Update the earliest modified index
        if (index < m_earliestModifiedIndex) {
            m_earliestModifiedIndex = index;
        }

        // Shift left in GPU: upload suffix starting at 'index'
        if (!m_data.empty() && index < m_data.size()) {
            const unsigned int* src = m_data.data() + index;
            const uint32_t count = static_cast<uint32_t>(m_data.size() - index);
            StageOrUpload(src, sizeof(unsigned int) * count, index * sizeof(unsigned int));
        }

        // Zero out the last stale slot (not strictly required if readers clamp to Size())
        if (m_data.size() < m_capacity) {
            const unsigned int zero = 0u;
            const uint32_t lastSlot = static_cast<uint32_t>(m_data.size());
            StageOrUpload(&zero, sizeof(unsigned int), lastSlot * sizeof(unsigned int));
        }
    }
}

void SortedUnsignedIntBuffer::StageOrUpload(const void* data, size_t size, size_t offset) {
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

void SortedUnsignedIntBuffer::CreateBuffer(uint64_t capacity) {
    auto device = DeviceManager::GetInstance().GetDevice();
    m_capacity = capacity;
    auto newDataBuffer = GpuBufferBacking::CreateUnique(rhi::HeapType::DeviceLocal, capacity * sizeof(unsigned int), GetGlobalResourceID(), m_UAV);
    SetBacking(std::move(newDataBuffer), capacity * sizeof(unsigned int));
    m_uploadPolicyState.OnBufferResized(GetBufferSize());
    
    for (const auto& bundle : m_metadataBundles) {
		ApplyMetadataToBacking(bundle);
	}

	AssignDescriptorSlots();
}

void SortedUnsignedIntBuffer::GrowBuffer(uint64_t newSize) {
    auto device = DeviceManager::GetInstance().GetDevice();
    auto newDataBuffer = GpuBufferBacking::CreateUnique(rhi::HeapType::DeviceLocal, newSize * sizeof(unsigned int), GetGlobalResourceID(), m_UAV);
	// Copy existing data to new buffer and discard old buffer after copy
    if (m_dataBuffer) {
        auto oldBackingResource = ExternalBackingResource::CreateShared(std::move(m_dataBuffer));
        if (auto* uploadService = rg::runtime::GetActiveUploadService()) {
            uploadService->QueueResourceCopy(shared_from_this(), oldBackingResource, m_capacity * sizeof(unsigned int));
        }
    }
    SetBacking(std::move(newDataBuffer), newSize * sizeof(unsigned int));
    m_uploadPolicyState.OnBufferResized(GetBufferSize());

    m_capacity = newSize;
    AssignDescriptorSlots();
    SetName(name);
}

void SortedUnsignedIntBuffer::AssignDescriptorSlots()
{
    BufferBase::DescriptorRequirements requirements{};

    const uint32_t numElements = static_cast<uint32_t>(m_capacity);

    requirements.createCBV = false;
    requirements.createSRV = true;
    requirements.createUAV = false;
    requirements.createNonShaderVisibleUAV = false;
    requirements.uavCounterOffset = 0;

    requirements.srvDesc = rhi::SrvDesc{
        .dimension = rhi::SrvDim::Buffer,
        .formatOverride = rhi::Format::Unknown,
        .buffer = {
            .kind = rhi::BufferViewKind::Structured,
            .firstElement = 0,
            .numElements = numElements,
            .structureByteStride = 4,
        },
    };

    SetDescriptorRequirements(requirements);
}
