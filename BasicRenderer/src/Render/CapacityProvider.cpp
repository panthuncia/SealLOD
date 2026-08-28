#include "Render/CapacityProvider.h"

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

namespace br::render {

struct CapacityLease::State {
    std::uint64_t cost = 0;
    ArtifactVersionID owner;
    std::function<void(std::uint64_t)> release;
    ~State() { if (release) release(cost); }
};

std::uint64_t CapacityLease::Cost() const noexcept { return m_state ? m_state->cost : 0; }
ArtifactVersionID CapacityLease::Owner() const noexcept {
    return m_state ? m_state->owner : ArtifactVersionID{};
}

struct CapacityProvider::Impl : std::enable_shared_from_this<Impl> {
    struct PendingRequest {
        CapacityRequest request;
        Callback callback;
    };
    struct Grant {
        Callback callback;
        CapacityLease lease;
    };

    TaskSchedulerManager& scheduler;
    TaskScope scope;
    std::string name;
    TaskLane lane;
    TaskDomain domain;
    mutable std::mutex mutex;
    std::uint64_t capacity = 0;
    std::uint64_t available = 0;
    bool shuttingDown = false;
    std::vector<PendingRequest> pending;

    Impl(TaskSchedulerManager& schedulerValue, std::string nameValue,
        std::uint64_t capacityValue, TaskLane laneValue, TaskDomain domainValue)
        : scheduler(schedulerValue), scope(schedulerValue.CreateScope(nameValue)),
          name(std::move(nameValue)), lane(laneValue), domain(domainValue),
          capacity(capacityValue), available(capacityValue) {}

    static bool Before(const PendingRequest& left, const PendingRequest& right) {
        if (left.request.priority != right.request.priority)
            return left.request.priority > right.request.priority;
        return left.request.admissionSequence < right.request.admissionSequence;
    }

    void CollectGrants(std::vector<Grant>& grants) {
        std::stable_sort(pending.begin(), pending.end(), Before);
        // Strict head-of-line ordering is intentional: available capacity does
        // not permit a younger request to change deterministic packing.
        while (!pending.empty() && pending.front().request.cost <= available) {
            auto item = std::move(pending.front());
            pending.erase(pending.begin());
            available -= item.request.cost;
            auto weak = weak_from_this();
            auto state = std::make_shared<CapacityLease::State>();
            state->cost = item.request.cost;
            state->owner = item.request.owner;
            state->release = [weak](std::uint64_t cost) {
                if (auto self = weak.lock()) self->Release(cost);
            };
            grants.push_back({ std::move(item.callback), CapacityLease(std::move(state)) });
        }
    }

    void Dispatch(std::vector<Grant> grants) {
        for (auto& grant : grants) {
            auto shared = std::make_shared<Grant>(std::move(grant));
            if (!scheduler.Submit(scope, lane, domain, name,
                [shared](const TaskContext& context) mutable {
                    if (!context.StopRequested() && shared->callback)
                        shared->callback(std::move(shared->lease));
                })) {
                // Dropping the lease synchronously returns the credit. The
                // callback is never run on the acquiring thread.
                shared.reset();
            }
        }
    }

    void Release(std::uint64_t cost) {
        std::vector<Grant> grants;
        {
            std::lock_guard lock(mutex);
            available = (std::min)(capacity, available + cost);
            if (!shuttingDown) CollectGrants(grants);
        }
        Dispatch(std::move(grants));
    }
};

CapacityProvider::CapacityProvider(TaskSchedulerManager& scheduler, std::string name,
    std::uint64_t capacity, TaskLane lane, TaskDomain domain)
    : m_impl(std::make_shared<Impl>(scheduler, std::move(name), capacity, lane, domain)) {}

CapacityProvider::~CapacityProvider() { Shutdown(); }

bool CapacityProvider::AcquireAsync(CapacityRequest request, Callback callback) {
    if (!m_impl || !callback || request.cost == 0) return false;
    std::vector<Impl::Grant> grants;
    {
        std::lock_guard lock(m_impl->mutex);
        if (m_impl->shuttingDown || request.cost > m_impl->capacity) return false;
        m_impl->pending.push_back({ std::move(request), std::move(callback) });
        m_impl->CollectGrants(grants);
    }
    m_impl->Dispatch(std::move(grants));
    return true;
}

void CapacityProvider::Cancel(ArtifactVersionID owner, std::uint64_t cancellationGeneration) {
    if (!m_impl) return;
    std::lock_guard lock(m_impl->mutex);
    std::erase_if(m_impl->pending, [&](const Impl::PendingRequest& item) {
        return item.request.owner == owner &&
            item.request.cancellationGeneration == cancellationGeneration;
    });
}

void CapacityProvider::SetCapacity(std::uint64_t capacity) {
    if (!m_impl) return;
    std::vector<Impl::Grant> grants;
    {
        std::lock_guard lock(m_impl->mutex);
        const auto used = m_impl->capacity - m_impl->available;
        m_impl->capacity = capacity;
        m_impl->available = capacity > used ? capacity - used : 0;
        if (!m_impl->shuttingDown) m_impl->CollectGrants(grants);
    }
    m_impl->Dispatch(std::move(grants));
}

std::uint64_t CapacityProvider::Capacity() const {
    if (!m_impl) return 0;
    std::lock_guard lock(m_impl->mutex);
    return m_impl->capacity;
}

std::uint64_t CapacityProvider::Available() const {
    if (!m_impl) return 0;
    std::lock_guard lock(m_impl->mutex);
    return m_impl->available;
}

std::size_t CapacityProvider::Pending() const {
    if (!m_impl) return 0;
    std::lock_guard lock(m_impl->mutex);
    return m_impl->pending.size();
}

void CapacityProvider::Shutdown() {
    if (!m_impl) return;
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->shuttingDown = true;
        m_impl->pending.clear();
    }
    m_impl->scope.CancelAndWait();
    m_impl.reset();
}

} // namespace br::render
