#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>

namespace br {

// A level-triggered single-consumer task pump. Notify() and the Running->Idle
// transition are serialized by the same mutex, so a producer can never observe
// an owned pump after the consumer has decided to exit.
class SerializedTaskPump {
public:
    using Task = std::function<void()>;
    using Submit = std::function<bool(Task)>;
    using Drain = std::function<void()>;
    using Reject = std::function<void()>;

    SerializedTaskPump() = default;
    SerializedTaskPump(const SerializedTaskPump&) = delete;
    SerializedTaskPump& operator=(const SerializedTaskPump&) = delete;

    void Configure(Submit submit, Drain drain, Reject reject = {}) {
        std::lock_guard lock(m_mutex);
        m_submit = std::move(submit);
        m_drain = std::move(drain);
        m_reject = std::move(reject);
        m_state = State::Idle;
        m_notifications = 0;
    }

    [[nodiscard]] bool Notify() {
        Submit submit;
        {
            std::lock_guard lock(m_mutex);
            if (m_state == State::Stopping || !m_submit || !m_drain) return false;
            ++m_notifications;
            if (m_state != State::Idle) return true;
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
            m_notifications = 0;
            reject = m_reject;
        }
        if (reject) reject();
        return false;
    }

    void Stop() {
        std::lock_guard lock(m_mutex);
        m_state = State::Stopping;
        m_notifications = 0;
    }

    [[nodiscard]] bool IsIdle() const {
        std::lock_guard lock(m_mutex);
        return m_state == State::Idle;
    }

private:
    enum class State : std::uint8_t { Idle, Scheduled, Running, Stopping };

    void Run() noexcept {
        for (;;) {
            Drain drain;
            {
                std::lock_guard lock(m_mutex);
                if (m_state == State::Stopping) return;
                m_state = State::Running;
                m_notifications = 0;
                drain = m_drain;
            }
            try {
                drain();
            } catch (...) {
                Reject reject;
                {
                    std::lock_guard lock(m_mutex);
                    m_state = State::Stopping;
                    m_notifications = 0;
                    reject = m_reject;
                }
                if (reject) reject();
                return;
            }
            {
                std::lock_guard lock(m_mutex);
                if (m_state == State::Stopping) return;
                if (m_notifications == 0) {
                    m_state = State::Idle;
                    return;
                }
                // Work was signalled while the consumer was running. Keep
                // ownership and drain it without another scheduler handoff.
            }
        }
    }

    mutable std::mutex m_mutex;
    State m_state = State::Stopping;
    std::uint64_t m_notifications = 0;
    Submit m_submit;
    Drain m_drain;
    Reject m_reject;
};

} // namespace br
