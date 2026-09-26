#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "Runtime/StateGraph/AsyncStateGraph.h"

namespace org { class Buffer; }

namespace br::render {

class RendererStateRequestService;

// Geometry catalog variant under which the CLod non-resident bitset is published.
inline constexpr std::uint64_t kCLodNonResidentBitsCatalogVariant = 0x434c4f444e524231ull;

// One immutable allocation of the CLod non-resident bitset, covering groups
// [0, capacity). Storage never grows in place: a larger capacity is a new
// allocation and a new revision, filled by the CLod streaming worker before it
// is published, so no frame can observe a bitset whose contents are undefined.
struct CLodResidencyStorageInput {
    std::uint32_t capacity = 0;
    std::shared_ptr<org::Buffer> nonResidentBits;
};

[[nodiscard]] inline ArtifactAddress CLodResidencyStorageAddress() {
    return { ArtifactKind::CLodResidencyStorage, 0, 0 };
}

// CLodResidencyCapacityGate{capacity}: ready once a filled residency storage
// covering capacity groups exists. Its payload forwards that storage's catalog
// fragment, so the geometry state that requires it publishes the bitset its
// group table needs. Revision 1 only; it never changes once built.
[[nodiscard]] inline ArtifactAddress CLodResidencyCapacityGateAddress(std::uint32_t capacity) {
    return { ArtifactKind::CLodResidencyCapacityGate, capacity, 0 };
}

// Hand-off between the CLod streaming worker, which builds storages, and
// geometry publication, which requires capacity for the groups it references.
class CLodResidencyStorageDirectory {
public:
    explicit CLodResidencyStorageDirectory(RendererStateRequestService& requests);

    // Streaming worker: the storage's fill has completed on the GPU. Requests
    // its artifact revision and wakes gates it covers.
    void PublishStorage(std::uint32_t capacity, std::shared_ptr<org::Buffer> nonResidentBits);
    // Gate producer: the newest published storage covering capacity, or an empty
    // handle and a suspension identity notified when one is published.
    [[nodiscard]] ArtifactVersionHandle Covering(std::uint32_t capacity, std::uint64_t& waitIdentity);
    // The largest capacity any gate has asked for. The streaming worker grows
    // its storage to cover it, so a gate never waits on capacity nobody builds.
    [[nodiscard]] std::uint32_t RequestedCapacity() const noexcept {
        return m_requestedCapacity.load(std::memory_order_acquire);
    }
    // Set once a CLod streaming system services this directory. Geometry only
    // requires capacity gates when something will satisfy them.
    void AttachProducer() noexcept { m_producerAttached.store(true, std::memory_order_release); }
    [[nodiscard]] bool HasProducer() const noexcept { return m_producerAttached.load(std::memory_order_acquire); }

private:
    RendererStateRequestService& m_requests;
    std::function<void(std::uint64_t)> m_notify;
    std::mutex m_mutex;
    std::uint64_t m_revision = 0;
    ArtifactVersionHandle m_newest;
    std::uint32_t m_newestCapacity = 0;
    std::multimap<std::uint32_t, std::uint64_t> m_waiting;
    // One per published storage; a handful over a session.
    std::vector<ArtifactAwaiter> m_awaiters;
    std::atomic<std::uint32_t> m_requestedCapacity{ 0 };
    std::atomic<bool> m_producerAttached{ false };
};

struct CLodResidencyCapacityGateInput {
    std::uint32_t capacity = 0;
    std::shared_ptr<CLodResidencyStorageDirectory> directory;
};

void RegisterCLodResidencyStorageProducers(AsyncStateGraph& graph);

} // namespace br::render
