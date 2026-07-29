#include "Render/GraphExtensions/ClusterLOD/CLodUploadStream.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>

#include <spdlog/spdlog.h>
#include <tbb/parallel_for.h>
#include <tracy/Tracy.hpp>

#include "Render/MemoryIntrospectionAPI.h"
#include "Resources/Buffers/Buffer.h"

namespace {
size_t AlignUp(size_t value, size_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}
}

void CLodUploadStream::BeginBulkUpload() {
    if (m_bulkUploadActive) {
        return;
    }
    m_bulkUploadActive = true;
}

void CLodUploadStream::EndBulkUpload() {
    ZoneScopedN("CLodUploadStream::EndBulkUpload");
    if (!m_bulkUploadActive) {
        return;
    }
    m_bulkUploadActive = false;
    if (m_deferredUploads.empty()) {
        return;
    }
    ZoneValue(m_deferredUploads.size());

    struct StagedUpload {
        DeferredUpload* upload = nullptr;
        std::shared_ptr<CLodUploadPage> page;
        size_t stagingOffset = 0u;
        void* mappedData = nullptr;
    };
    std::vector<StagedUpload> stagedUploads;
    stagedUploads.reserve(m_deferredUploads.size());
    {
        ZoneScopedN("CLodUploadStream::EndBulkUpload::PlanStaging");
        for (auto& upload : m_deferredUploads) {
            if (!m_activePage) {
                m_activePage = AcquirePage(upload.size + 15u);
            }
            size_t stagingOffset = AlignUp(m_activePage->tail, 16u);
            if (stagingOffset + upload.size > m_activePage->capacity) {
                m_activePage = AcquirePage(upload.size + 15u);
                stagingOffset = 0u;
            }
            stagedUploads.push_back({
                .upload = &upload,
                .page = m_activePage,
                .stagingOffset = stagingOffset,
            });
            m_activePage->tail = stagingOffset + upload.size;
        }
    }

    struct MappedPage {
        std::shared_ptr<CLodUploadPage> page;
        void* data = nullptr;
    };
    std::vector<MappedPage> mappedPages;
    mappedPages.reserve(m_openPages.size());
    CLodUploadPage* previousPage = nullptr;
    void* mappedData = nullptr;
    {
        ZoneScopedN("CLodUploadStream::EndBulkUpload::MapStagingPages");
        for (auto& staged : stagedUploads) {
            if (staged.page.get() != previousPage) {
                mappedData = nullptr;
                staged.page->buffer->GetAPIResource().Map(
                    &mappedData,
                    0,
                    0);
                if (mappedData == nullptr) {
                    spdlog::error(
                        "CLodUploadStream failed to map a bulk staging page");
                }
                mappedPages.push_back({staged.page, mappedData});
                previousPage = staged.page.get();
            }
            staged.mappedData = mappedData;
        }
    }

    {
        ZoneScopedN("CLodUploadStream::EndBulkUpload::ParallelCopyPayloads");
        tbb::parallel_for(
            size_t{0u},
            stagedUploads.size(),
            [&stagedUploads](size_t index) {
                const auto& staged = stagedUploads[index];
                if (staged.mappedData == nullptr) {
                    return;
                }
                std::memcpy(
                    static_cast<std::byte*>(staged.mappedData) +
                        staged.stagingOffset,
                    staged.upload->data,
                    staged.upload->size);
            });
    }

    {
        ZoneScopedN("CLodUploadStream::EndBulkUpload::UnmapStagingPages");
        for (auto& mapped : mappedPages) {
            if (mapped.data != nullptr) {
                mapped.page->buffer->GetAPIResource().Unmap(
                    0,
                    mapped.page->tail);
            }
        }
    }

    {
        ZoneScopedN("CLodUploadStream::EndBulkUpload::PublishCopies");
        for (const auto& staged : stagedUploads) {
            if (staged.mappedData == nullptr) {
                continue;
            }
            CLodUploadCopy copy{
                .destination = std::move(staged.upload->target.pinned),
                .staging = staged.page->buffer,
                .destinationOffset = staged.upload->destinationOffset,
                .stagingOffset = staged.stagingOffset,
                .size = staged.upload->size,
            };
            if (!m_copies.empty()) {
                auto& previous = m_copies.back();
                if (previous.destination == copy.destination &&
                    previous.staging == copy.staging &&
                    previous.destinationOffset + previous.size ==
                        copy.destinationOffset &&
                    previous.stagingOffset + previous.size ==
                        copy.stagingOffset) {
                    previous.size += copy.size;
                    continue;
                }
            }
            m_copies.push_back(std::move(copy));
        }
    }
    m_deferredUploads.clear();
}

std::shared_ptr<CLodUploadPage> CLodUploadStream::AcquirePage(size_t minimumSize) {
    const size_t capacity = std::max(m_pageSize, minimumSize);
    std::shared_ptr<CLodUploadPage> page;
    if (capacity == m_pageSize && !m_freePages.empty()) {
        page = std::move(m_freePages.front());
        m_freePages.pop_front();
    } else {
        page = std::make_shared<CLodUploadPage>();
        page->capacity = capacity;
        page->buffer = Buffer::CreateShared(rhi::HeapType::Upload, capacity, false);
        page->buffer->SetName("CLodStreamingUploadPage_" + std::to_string(++m_nextPageId));
        rg::memory::SetResourceUsageHint(*page->buffer, "Cluster LOD streaming upload staging");
    }
    page->tail = 0;
    m_openPages.push_back(page);
    return page;
}

void CLodUploadStream::UploadPageData(
    const void* data,
    size_t size,
    rg::runtime::UploadTarget target,
    size_t destinationOffset) {
    if (m_bulkUploadActive && data != nullptr && size != 0u &&
        target.kind == rg::runtime::UploadTarget::Kind::PinnedShared &&
        target.pinned != nullptr) {
        m_deferredUploads.push_back({
            .data = data,
            .size = size,
            .target = std::move(target),
            .destinationOffset = destinationOffset,
        });
        return;
    }
    UploadData(
        data,
        size,
        std::move(target),
        destinationOffset);
}

void CLodUploadStream::UploadData(
    const void* data,
    size_t size,
    rg::runtime::UploadTarget target,
    size_t destinationOffset) {
    if (!data || size == 0u) return;
    if (target.kind != rg::runtime::UploadTarget::Kind::PinnedShared || !target.pinned) {
        spdlog::error("CLodUploadStream requires a pinned shared destination");
        return;
    }
    if (!m_activePage) m_activePage = AcquirePage(size + 15u);
    size_t stagingOffset = AlignUp(m_activePage->tail, 16u);
    if (stagingOffset + size > m_activePage->capacity) {
        m_activePage = AcquirePage(size + 15u);
        stagingOffset = 0u;
    }

    void* mapped = nullptr;
    m_activePage->buffer->GetAPIResource().Map(&mapped, 0, 0);
    if (!mapped) {
        spdlog::error("CLodUploadStream failed to map a staging page");
        return;
    }
    std::memcpy(static_cast<std::byte*>(mapped) + stagingOffset, data, size);
    m_activePage->tail = stagingOffset + size;
    m_activePage->buffer->GetAPIResource().Unmap(stagingOffset, size);

    CLodUploadCopy copy{
        .destination = std::move(target.pinned),
        .staging = m_activePage->buffer,
        .destinationOffset = destinationOffset,
        .stagingOffset = stagingOffset,
        .size = size,
    };
    if (!m_copies.empty()) {
        auto& previous = m_copies.back();
        if (previous.destination == copy.destination && previous.staging == copy.staging &&
            previous.destinationOffset + previous.size == copy.destinationOffset &&
            previous.stagingOffset + previous.size == copy.stagingOffset) {
            previous.size += copy.size;
            return;
        }
    }
    m_copies.push_back(std::move(copy));
}

std::shared_ptr<CLodUploadBatch> CLodUploadStream::Seal(
    uint64_t generation,
    uint64_t batchId,
    std::vector<uint32_t>& affectedGroups,
    std::vector<uint32_t>& retiringPages,
    uint64_t nonResidentEpoch) {
    EndBulkUpload();
    if (m_copies.empty()) return {};
    auto batch = std::make_shared<CLodUploadBatch>();
    batch->ticket = std::make_shared<CLodUploadTicket>();
    batch->ticket->generation = generation;
    batch->ticket->batchId = batchId;
    batch->copies.swap(m_copies);
    batch->pages.swap(m_openPages);
    batch->affectedGroups.swap(affectedGroups);
    batch->retiringPages.swap(retiringPages);
    batch->nonResidentEpoch = nonResidentEpoch;
    m_activePage.reset();

    std::unordered_set<Resource*> seen;
    for (const auto& copy : batch->copies) {
        if (copy.destination && seen.insert(copy.destination.get()).second) {
            batch->destinations.push_back(copy.destination);
        }
    }
    return batch;
}

void CLodUploadStream::Recycle(const std::shared_ptr<CLodUploadBatch>& batch) {
    if (!batch) return;
    for (const auto& page : batch->pages) {
        if (page && page->capacity == m_pageSize &&
            m_freePages.size() < MaxCachedPages) {
            page->tail = 0;
            m_freePages.push_back(page);
        }
    }
}

void CLodUploadStream::Cleanup() {
    EndBulkUpload();
    m_deferredUploads.clear();
    m_copies.clear();
    m_openPages.clear();
    m_activePage.reset();
    m_freePages.clear();
}
