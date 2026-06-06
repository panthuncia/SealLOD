#include "Resources/Buffers/SortedUnsignedIntBuffer.h"

#include <algorithm>

#include <spdlog/spdlog.h>

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
        EnsureCapacityForSize(m_data.size() + 1);
    }

    // Find the insertion point
    auto it = std::lower_bound(m_data.begin(), m_data.end(), element);
    // Prevent duplicates
    if (it != m_data.end() && *it == element) {
        return; // already present
    }

    uint32_t index = static_cast<uint32_t>(std::distance(m_data.begin(), it));
    m_data.insert(it, element);
    ++m_mutationRevision;

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
    if (m_activeEntryMode) {
        std::vector<ActiveDrawSetEntry> entries;
        entries.reserve(elements.size());
        for (const auto element : elements) {
            entries.push_back(ActiveDrawSetEntry{
                .drawRecordIndex = element,
                .generation = 1u
            });
        }
        AppendActiveEntries(entries);
        return;
    }

    std::vector<unsigned int> sortedElements = elements;
    std::sort(sortedElements.begin(), sortedElements.end());
    sortedElements.erase(std::unique(sortedElements.begin(), sortedElements.end()), sortedElements.end());

    if (m_data.empty()) {
        EnsureCapacityForSize(sortedElements.size());
        m_data = std::move(sortedElements);
        ++m_mutationRevision;
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

    EnsureCapacityForSize(merged.size());

    auto firstDiff = std::mismatch(m_data.begin(), m_data.end(), merged.begin(), merged.end());
    const auto dirtyIndex = static_cast<std::size_t>(std::distance(m_data.begin(), firstDiff.first));
    m_data = std::move(merged);
    ++m_mutationRevision;

    const unsigned int* src = m_data.data() + dirtyIndex;
    const auto count = m_data.size() - dirtyIndex;
    StageOrUpload(src, sizeof(unsigned int) * count, dirtyIndex * sizeof(unsigned int));

    if (dirtyIndex < m_earliestModifiedIndex) {
        m_earliestModifiedIndex = dirtyIndex;
    }
}

void SortedUnsignedIntBuffer::AppendActiveEntries(const std::vector<ActiveDrawSetEntry>& entries) {
    if (entries.empty()) {
        return;
    }
    if (!m_activeEntryMode) {
        std::vector<unsigned int> indices;
        indices.reserve(entries.size());
        for (const auto& entry : entries) {
            indices.push_back(entry.drawRecordIndex);
        }
        InsertMany(indices);
        return;
    }

    const auto firstIndex = m_activeEntries.size();
    EnsureCapacityForSize(firstIndex + entries.size());
    m_activeEntries.insert(m_activeEntries.end(), entries.begin(), entries.end());
    ++m_mutationRevision;
    StageOrUpload(
        entries.data(),
        sizeof(ActiveDrawSetEntry) * entries.size(),
        firstIndex * sizeof(ActiveDrawSetEntry));
}

void SortedUnsignedIntBuffer::AssignActiveSnapshot(std::vector<ActiveDrawSetEntry> entries) {
    if (!m_activeEntryMode) {
        m_data.clear();
        m_data.reserve(entries.size());
        for (const auto& entry : entries) {
            m_data.push_back(entry.drawRecordIndex);
        }
        std::sort(m_data.begin(), m_data.end());
        m_data.erase(std::unique(m_data.begin(), m_data.end()), m_data.end());
        EnsureCapacityForSize(m_data.size());
        ++m_mutationRevision;
        if (!m_data.empty()) {
            StageOrUpload(m_data.data(), sizeof(unsigned int) * m_data.size(), 0);
        }
        return;
    }

    const auto oldSize = m_activeEntries.size();
    EnsureCapacityForSize(entries.size());
    m_activeEntries = std::move(entries);
    m_liveSize = m_activeEntries.size();
    m_activeTombstoneEstimate = 0;
    ++m_mutationRevision;
    if (!m_activeEntries.empty()) {
        StageOrUpload(m_activeEntries.data(), sizeof(ActiveDrawSetEntry) * m_activeEntries.size(), 0);
    }
    if (oldSize > m_activeEntries.size()) {
        std::vector<ActiveDrawSetEntry> zeros(oldSize - m_activeEntries.size());
        StageOrUpload(
            zeros.data(),
            sizeof(ActiveDrawSetEntry) * zeros.size(),
            m_activeEntries.size() * sizeof(ActiveDrawSetEntry));
    }
}

std::vector<SortedUnsignedIntBuffer::ActiveDrawSetEntry> SortedUnsignedIntBuffer::SnapshotActiveEntries() const {
    if (m_activeEntryMode) {
        return m_activeEntries;
    }

    std::vector<ActiveDrawSetEntry> entries;
    entries.reserve(m_data.size());
    for (const auto index : m_data) {
        entries.push_back(ActiveDrawSetEntry{
            .drawRecordIndex = index,
            .generation = 1u
        });
    }
    return entries;
}

void SortedUnsignedIntBuffer::Remove(unsigned int element) {
    // Find the element
    auto it = std::lower_bound(m_data.begin(), m_data.end(), element);

    if (it != m_data.end() && *it == element) {
        const uint32_t index = static_cast<uint32_t>(std::distance(m_data.begin(), it));

        // Erase from CPU
        m_data.erase(it);
        ++m_mutationRevision;

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

void SortedUnsignedIntBuffer::RemoveMany(const std::vector<unsigned int>& elements) {
    if (m_activeEntryMode) {
        return;
    }
    if (elements.empty() || m_data.empty()) {
        return;
    }

    std::vector<unsigned int> sortedElements = elements;
    std::sort(sortedElements.begin(), sortedElements.end());
    sortedElements.erase(std::unique(sortedElements.begin(), sortedElements.end()), sortedElements.end());

    std::vector<unsigned int> remaining;
    remaining.reserve(m_data.size());
    std::set_difference(
        m_data.begin(),
        m_data.end(),
        sortedElements.begin(),
        sortedElements.end(),
        std::back_inserter(remaining));

    if (remaining.size() == m_data.size()) {
        spdlog::warn(
            "SortedUnsignedIntBuffer::RemoveMany '{}' removed no indices requested={} size={}",
            GetName(),
            sortedElements.size(),
            m_data.size());
        return;
    }

    const auto firstDiff = std::mismatch(m_data.begin(), m_data.end(), remaining.begin(), remaining.end());
    const auto dirtyIndex = static_cast<std::size_t>(std::distance(m_data.begin(), firstDiff.first));
    const auto oldSize = m_data.size();
    const auto removed = oldSize - remaining.size();
    if (removed != sortedElements.size()) {
        spdlog::warn(
            "SortedUnsignedIntBuffer::RemoveMany '{}' partial remove requested={} removed={} size_before={} size_after={}",
            GetName(),
            sortedElements.size(),
            removed,
            oldSize,
            remaining.size());
    }
    m_data = std::move(remaining);
    ++m_mutationRevision;

    if (dirtyIndex < m_earliestModifiedIndex) {
        m_earliestModifiedIndex = dirtyIndex;
    }

    if (!m_data.empty() && dirtyIndex < m_data.size()) {
        const unsigned int* src = m_data.data() + dirtyIndex;
        const auto count = m_data.size() - dirtyIndex;
        StageOrUpload(src, sizeof(unsigned int) * count, dirtyIndex * sizeof(unsigned int));
    }

    if (m_data.size() < oldSize) {
        const std::vector<unsigned int> zeros(oldSize - m_data.size(), 0u);
        StageOrUpload(zeros.data(), sizeof(unsigned int) * zeros.size(), m_data.size() * sizeof(unsigned int));
    }
}

void SortedUnsignedIntBuffer::StageOrUpload(const void* data, size_t size, size_t offset) {
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

void SortedUnsignedIntBuffer::CreateBuffer(uint64_t capacity) {
    auto device = DeviceManager::GetInstance().GetDevice();
    m_capacity = capacity;
    const auto stride = m_activeEntryMode ? sizeof(ActiveDrawSetEntry) : sizeof(unsigned int);
    auto newDataBuffer = GpuBufferBacking::CreateUnique(rhi::HeapType::DeviceLocal, capacity * stride, GetGlobalResourceID(), m_UAV);
    SetBacking(std::move(newDataBuffer), capacity * stride);
    m_uploadPolicyState.OnBufferResized(GetBufferSize());
    
    for (const auto& bundle : m_metadataBundles) {
		ApplyMetadataToBacking(bundle);
	}

	AssignDescriptorSlots();
}

void SortedUnsignedIntBuffer::GrowBuffer(uint64_t newSize) {
    const uint64_t previousCapacity = m_capacity;
    auto device = DeviceManager::GetInstance().GetDevice();
    const auto stride = m_activeEntryMode ? sizeof(ActiveDrawSetEntry) : sizeof(unsigned int);
    auto newDataBuffer = GpuBufferBacking::CreateUnique(rhi::HeapType::DeviceLocal, newSize * stride, GetGlobalResourceID(), m_UAV);
    spdlog::info(
        "SortedUnsignedIntBuffer '{}' id={} GrowBuffer SetBacking begin previousCapacity={} newCapacity={} activeEntryMode={}",
        GetName(),
        GetGlobalResourceID(),
        previousCapacity,
        newSize,
        m_activeEntryMode ? 1 : 0);
    SetBacking(std::move(newDataBuffer), newSize * stride);
    spdlog::info(
        "SortedUnsignedIntBuffer '{}' id={} GrowBuffer SetBacking complete bufferSize={} backingGeneration={}",
        GetName(),
        GetGlobalResourceID(),
        GetBufferSize(),
        GetBackingGeneration());
    m_uploadPolicyState.OnBufferResized(GetBufferSize());
    const void* replayData = nullptr;
    size_t replayBytes = 0u;
    if (m_activeEntryMode && !m_activeEntries.empty()) {
        replayData = m_activeEntries.data();
        replayBytes = m_activeEntries.size() * sizeof(ActiveDrawSetEntry);
    }
    else if (!m_data.empty()) {
        replayData = m_data.data();
        replayBytes = m_data.size() * sizeof(unsigned int);
    }
    if (replayData && replayBytes > 0u) {
        SyncUploadPolicyState();
        if (rg::runtime::GetActiveUploadService() != nullptr) {
            BUFFER_UPLOAD(replayData, replayBytes, rg::runtime::UploadTarget::FromShared(shared_from_this()), 0u);
            m_uploadPolicyState.RetainExternalWrite(replayData, replayBytes, 0u, GetBufferSize());
            spdlog::debug(
                "SortedUnsignedIntBuffer '{}' id={} GrowBuffer replayed CPU bytes={}",
                GetName(),
                GetGlobalResourceID(),
                replayBytes);
        }
        else {
            StageOrUpload(replayData, replayBytes, 0u);
            if (m_uploadPolicyState.HasPendingWork()) {
                MarkUploadPolicyDirty();
            }
        }
    }

    m_capacity = newSize;
    AssignDescriptorSlots();
    SetName(name);
    spdlog::info(
        "SortedUnsignedIntBuffer '{}' id={} GrowBuffer complete finalCapacity={}",
        GetName(),
        GetGlobalResourceID(),
        m_capacity);
}

void SortedUnsignedIntBuffer::EnsureCapacityForSize(uint64_t requiredSize) {
    if (requiredSize <= m_capacity) {
        return;
    }

    uint64_t newCapacity = (std::max<uint64_t>)(m_capacity, 1u);
    while (newCapacity < requiredSize) {
        newCapacity *= 2;
    }
    GrowBuffer(newCapacity);
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
            .structureByteStride = static_cast<uint32_t>(m_activeEntryMode ? sizeof(ActiveDrawSetEntry) : sizeof(unsigned int)),
        },
    };

    SetDescriptorRequirements(requirements);
}
