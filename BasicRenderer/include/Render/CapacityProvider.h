#pragma once

#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Render/AsyncStateGraph.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace br::render {

struct CapacityRequest {
    std::uint64_t cost = 0;
    std::int32_t priority = 0;
    std::uint64_t admissionSequence = 0;
    std::uint64_t cancellationGeneration = 0;
    ArtifactVersionID owner;
};

class CapacityLease {
public:
    CapacityLease() = default;
    CapacityLease(const CapacityLease&) = delete;
    CapacityLease& operator=(const CapacityLease&) = delete;
    CapacityLease(CapacityLease&&) noexcept = default;
    CapacityLease& operator=(CapacityLease&&) noexcept = default;
    ~CapacityLease() = default;

    [[nodiscard]] explicit operator bool() const noexcept { return m_state != nullptr; }
    [[nodiscard]] std::uint64_t Cost() const noexcept;
    [[nodiscard]] ArtifactVersionID Owner() const noexcept;
    void Reset() noexcept { m_state.reset(); }

private:
    struct State;
    explicit CapacityLease(std::shared_ptr<State> state) : m_state(std::move(state)) {}
    std::shared_ptr<State> m_state;
    friend class CapacityProvider;
};

class CapacityProvider {
public:
    using Callback = std::function<void(CapacityLease)>;

    CapacityProvider(TaskSchedulerManager& scheduler, std::string name,
        std::uint64_t capacity, TaskLane lane = TaskLane::Streaming,
        TaskDomain domain = TaskDomain::GraphControl);
    ~CapacityProvider();
    CapacityProvider(const CapacityProvider&) = delete;
    CapacityProvider& operator=(const CapacityProvider&) = delete;

    // Complete grants are dispatched in stable (priority, admissionSequence)
    // order. No callback is invoked while the provider lock is held.
    bool AcquireAsync(CapacityRequest request, Callback callback);
    void Cancel(ArtifactVersionID owner, std::uint64_t cancellationGeneration);
    void SetCapacity(std::uint64_t capacity);
    [[nodiscard]] std::uint64_t Capacity() const;
    [[nodiscard]] std::uint64_t Available() const;
    [[nodiscard]] std::size_t Pending() const;
    void Shutdown();

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};

} // namespace br::render
