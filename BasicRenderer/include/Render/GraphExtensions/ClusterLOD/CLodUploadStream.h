#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "Render/Runtime/UploadTypes.h"

class Buffer;
class Resource;

enum class CLodUploadTicketState : uint8_t {
    Published,
    Claimed,
    Submitted,
    Cancelled,
    Completed,
};

struct CLodUploadTicket {
    std::atomic<CLodUploadTicketState> state{CLodUploadTicketState::Published};
    std::atomic<uint64_t> completionValue{0};
    uint64_t generation = 0;
    uint64_t batchId = 0;
};

struct CLodUploadPage {
    std::shared_ptr<Buffer> buffer;
    size_t capacity = 0;
    size_t tail = 0;
};

struct CLodUploadCopy {
    std::shared_ptr<Resource> destination;
    std::shared_ptr<Buffer> staging;
    size_t destinationOffset = 0;
    size_t stagingOffset = 0;
    size_t size = 0;
};

struct CLodUploadBatch {
    std::shared_ptr<CLodUploadTicket> ticket;
    std::vector<CLodUploadCopy> copies;
    std::vector<std::shared_ptr<CLodUploadPage>> pages;
    std::vector<std::shared_ptr<Resource>> destinations;
    std::vector<uint32_t> affectedGroups;
    std::vector<uint32_t> retiringPages;
    uint64_t nonResidentEpoch = 0;
    bool submissionObserved = false; // worker-owned
};

// Single-producer staging allocator used only by the CLod streaming worker.
// Sealing transfers every open page to an immutable batch; the render thread
// reads batches but never mutates allocator state.
class CLodUploadStream {
public:
    static constexpr size_t DefaultPageSize = 16u * 1024u * 1024u;

    explicit CLodUploadStream(size_t pageSize = DefaultPageSize) : m_pageSize(pageSize) {}

    void BeginBulkUpload();
    void EndBulkUpload();
    void UploadPageData(
        const void* data,
        size_t size,
        rg::runtime::UploadTarget target,
        size_t destinationOffset);
    void UploadData(
        const void* data,
        size_t size,
        rg::runtime::UploadTarget target,
        size_t destinationOffset);
    [[nodiscard]] bool HasPendingWork() const noexcept { return !m_copies.empty(); }
    std::shared_ptr<CLodUploadBatch> Seal(
        uint64_t generation,
        uint64_t batchId,
        std::vector<uint32_t>& affectedGroups,
        std::vector<uint32_t>& retiringPages,
        uint64_t nonResidentEpoch);
    void Recycle(const std::shared_ptr<CLodUploadBatch>& batch);
    void Cleanup();

private:
    struct DeferredUpload {
        const void* data = nullptr;
        size_t size = 0;
        rg::runtime::UploadTarget target;
        size_t destinationOffset = 0;
    };

    std::shared_ptr<CLodUploadPage> AcquirePage(size_t minimumSize);

    size_t m_pageSize;
    uint64_t m_nextPageId = 0;
    std::deque<std::shared_ptr<CLodUploadPage>> m_freePages;
    std::shared_ptr<CLodUploadPage> m_activePage;
    bool m_bulkUploadActive = false;
    std::vector<DeferredUpload> m_deferredUploads;
    std::vector<std::shared_ptr<CLodUploadPage>> m_openPages;
    std::vector<CLodUploadCopy> m_copies;
};
