#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>

namespace br {

// A level-triggered single-consumer task pump. Notify() and the Running->Idle
// transition are serialized by the same mutex, so a producer can never observe
// an owned pump after the consumer has decided to exit.
class SerializedTaskPump {
public:
    using Task = std::function<void()>;
    using Submit = std::function<bool(Task)>;
    using SubmitDelayed = std::function<bool(std::chrono::steady_clock::duration, Task)>;
    using Drain = std::function<void()>;
    using Reject = std::function<void()>;

    struct Stats {
        std::uint64_t requestedEpoch = 0;
        std::uint64_t drainedEpoch = 0;
        std::uint64_t notifications = 0;
        std::uint64_t coalescedNotifications = 0;
        std::uint64_t drainPasses = 0;
        std::uint64_t handoffRetries = 0;
        std::uint64_t delayedRequests = 0;
        std::uint64_t delayedCoalesced = 0;
        std::uint64_t delayedFired = 0;
        std::uint64_t staleDelayed = 0;
        std::uint64_t rejected = 0;
        bool runnerActive = false;
        bool delayedArmed = false;
    };

    SerializedTaskPump() = default;
    SerializedTaskPump(const SerializedTaskPump&) = delete;
    SerializedTaskPump& operator=(const SerializedTaskPump&) = delete;

    void Configure(Submit submit, Drain drain, Reject reject = {}, SubmitDelayed submitDelayed = {}) {
        std::lock_guard lock(m_mutex);
        m_submit = std::move(submit);
        m_submitDelayed = std::move(submitDelayed);
        m_drain = std::move(drain);
        m_reject = std::move(reject);
        m_state = State::Idle;
        m_requestedEpoch = 0;
        m_drainedEpoch = 0;
        ++m_delayedGeneration;
        m_delayedDue.reset();
        m_stats = {};
    }

    [[nodiscard]] bool Notify() {
        Submit submit;
        {
            std::lock_guard lock(m_mutex);
            if (m_state == State::Stopping || !m_submit || !m_drain) return false;
            ++m_requestedEpoch;
            ++m_stats.notifications;
            m_stats.requestedEpoch = m_requestedEpoch;
            if (m_state != State::Idle) {
                ++m_stats.coalescedNotifications;
                return true;
            }
            m_state = State::Scheduled;
            submit = m_submit;
        }
        if (submit([this] { Run(); })) return true;

        Reject reject;
        {
            std::lock_guard lock(m_mutex);
            // A rejected task never acquired consumer ownership. Permanently
            // close this pump so queued work is failed rather than stranded.
            m_state = State::Stopping;
            ++m_stats.rejected;
            reject = m_reject;
        }
        if (reject) reject();
        return false;
    }

    // Delayed ownership is deliberately independent from immediate runner
    // ownership. A pending timer can therefore never suppress Notify(). When a
    // new earlier deadline replaces an existing one, the old token becomes a
    // harmless stale callback.
    [[nodiscard]] bool NotifyAfter(std::chrono::steady_clock::duration delay) {
        if (delay <= std::chrono::steady_clock::duration::zero()) return Notify();
        SubmitDelayed submitDelayed;
        std::uint64_t generation = 0;
        const auto due = std::chrono::steady_clock::now() + delay;
        {
            std::lock_guard lock(m_mutex);
            if (m_state == State::Stopping || !m_submitDelayed) return false;
            ++m_stats.delayedRequests;
            if (m_delayedDue && *m_delayedDue <= due) {
                ++m_stats.delayedCoalesced;
                return true;
            }
            generation = ++m_delayedGeneration;
            m_delayedDue = due;
            submitDelayed = m_submitDelayed;
        }
        if (submitDelayed(delay, [this, generation] { FireDelayed(generation); })) return true;

        Reject reject;
        {
            std::lock_guard lock(m_mutex);
            if (m_delayedGeneration == generation) m_delayedDue.reset();
            ++m_stats.rejected;
            reject = m_reject;
        }
        if (reject) reject();
        return false;
    }

    void Stop() {
        std::lock_guard lock(m_mutex);
        m_state = State::Stopping;
        ++m_delayedGeneration;
        m_delayedDue.reset();
    }

    [[nodiscard]] bool IsIdle() const {
        std::lock_guard lock(m_mutex);
        return m_state == State::Idle;
    }

    [[nodiscard]] Stats GetStats() const {
        std::lock_guard lock(m_mutex);
        auto result = m_stats;
        result.requestedEpoch = m_requestedEpoch;
        result.drainedEpoch = m_drainedEpoch;
        result.runnerActive = m_state == State::Scheduled || m_state == State::Running;
        result.delayedArmed = m_delayedDue.has_value();
        return result;
    }

private:
    enum class State : std::uint8_t { Idle, Scheduled, Running, Stopping };

    void FireDelayed(std::uint64_t generation) {
        {
            std::lock_guard lock(m_mutex);
            if (m_state == State::Stopping) return;
            if (!m_delayedDue || generation != m_delayedGeneration) {
                ++m_stats.staleDelayed;
                return;
            }
            m_delayedDue.reset();
            ++m_stats.delayedFired;
        }
        (void)Notify();
    }

    void Run() noexcept {
        for (;;) {
            Drain drain;
            std::uint64_t observedEpoch = 0;
            {
                std::lock_guard lock(m_mutex);
                if (m_state == State::Stopping) return;
                m_state = State::Running;
                observedEpoch = m_requestedEpoch;
                drain = m_drain;
            }
            try {
                drain();
            } catch (...) {
                Reject reject;
                {
                    std::lock_guard lock(m_mutex);
                    m_state = State::Stopping;
                    ++m_stats.rejected;
                    reject = m_reject;
                }
                if (reject) reject();
                return;
            }
            {
                std::lock_guard lock(m_mutex);
                if (m_state == State::Stopping) return;
                m_drainedEpoch = observedEpoch;
                m_stats.drainedEpoch = observedEpoch;
                ++m_stats.drainPasses;
                if (m_requestedEpoch == observedEpoch) {
                    m_state = State::Idle;
                    return;
                }
                // Work was signalled while the consumer was running. Keep
                // ownership and drain it without another scheduler handoff.
                ++m_stats.handoffRetries;
            }
        }
    }

    mutable std::mutex m_mutex;
    State m_state = State::Stopping;
    std::uint64_t m_requestedEpoch = 0;
    std::uint64_t m_drainedEpoch = 0;
    std::uint64_t m_delayedGeneration = 0;
    std::optional<std::chrono::steady_clock::time_point> m_delayedDue;
    Stats m_stats;
    Submit m_submit;
    SubmitDelayed m_submitDelayed;
    Drain m_drain;
    Reject m_reject;
};

} // namespace br
