#include "Render/GraphExtensions/ClusterLOD/CLodUploadStream.h"
#include "Utilities/BoundedSpscQueue.h"

#include <cassert>
#include <memory>
#include <vector>

namespace {
std::shared_ptr<CLodUploadBatch> MakeBatch(uint64_t id) {
    auto batch = std::make_shared<CLodUploadBatch>();
    batch->ticket = std::make_shared<CLodUploadTicket>();
    batch->ticket->batchId = id;
    batch->ticket->generation = 7;
    batch->affectedGroups.push_back(static_cast<uint32_t>(id));
    return batch;
}
}

int main() {
    // Publish -> claim -> submit -> complete, including several batches sharing
    // one graph-pass signal value.
    BoundedSpscQueue<std::shared_ptr<CLodUploadBatch>, 16> queue;
    for (uint64_t id = 1; id <= 3; ++id) assert(queue.TryPush(MakeBatch(id)));

    std::vector<std::shared_ptr<CLodUploadBatch>> claimed;
    queue.Drain([&](std::shared_ptr<CLodUploadBatch>&& batch) {
        auto expected = CLodUploadTicketState::Published;
        assert(batch->ticket->state.compare_exchange_strong(expected, CLodUploadTicketState::Claimed));
        claimed.push_back(std::move(batch));
    });
    assert(claimed.size() == 3);
    for (const auto& batch : claimed) {
        batch->ticket->completionValue.store(41, std::memory_order_relaxed);
        batch->ticket->state.store(CLodUploadTicketState::Submitted, std::memory_order_release);
    }
    for (const auto& batch : claimed) {
        assert(batch->ticket->completionValue.load(std::memory_order_acquire) == 41);
        batch->ticket->state.store(CLodUploadTicketState::Completed, std::memory_order_release);
    }

    // A compile-cancelled claim is replayed intact; affected groups and staging
    // ownership remain attached to the same immutable batch.
    auto replay = MakeBatch(9);
    assert(queue.TryPush(replay));
    std::shared_ptr<CLodUploadBatch> consumerBatch;
    assert(queue.TryPop(consumerBatch));
    auto published = CLodUploadTicketState::Published;
    assert(consumerBatch->ticket->state.compare_exchange_strong(published, CLodUploadTicketState::Claimed));
    consumerBatch->ticket->state.store(CLodUploadTicketState::Cancelled, std::memory_order_release);
    assert(consumerBatch->ticket->state.load(std::memory_order_acquire) == CLodUploadTicketState::Cancelled);
    consumerBatch->ticket->state.store(CLodUploadTicketState::Published, std::memory_order_release);
    assert(queue.TryPush(consumerBatch));
    std::shared_ptr<CLodUploadBatch> replayed;
    assert(queue.TryPop(replayed));
    assert(replayed == replay);
    assert(replayed->affectedGroups == std::vector<uint32_t>{9});

    // Lossless bounded backpressure: a seventeenth batch is retained by the
    // producer and can be published after one consumer slot opens.
    std::shared_ptr<CLodUploadBatch> retained;
    for (uint64_t id = 100; id < 117; ++id) {
        auto batch = MakeBatch(id);
        if (!queue.TryPush(batch)) retained = std::move(batch);
    }
    assert(retained && queue.Depth() == 16);
    assert(queue.TryPop(replayed));
    assert(queue.TryPush(retained));
    assert(queue.Depth() == 16);

    return 0;
}
