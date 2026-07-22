#pragma once

#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

// A bounded, allocation-free single-producer/single-consumer queue.
//
// Head is written only by the producer and tail only by the consumer.  The
// monotonically increasing counters intentionally do not wrap at Capacity;
// modulo is used only to address storage.  Release/acquire publication makes
// all writes to a value visible before the consumer observes the new head.
template <typename T, size_t Capacity>
class BoundedSpscQueue {
    static_assert(Capacity > 0, "BoundedSpscQueue capacity must be non-zero");

public:
    BoundedSpscQueue() = default;
    BoundedSpscQueue(const BoundedSpscQueue&) = delete;
    BoundedSpscQueue& operator=(const BoundedSpscQueue&) = delete;

    template <typename U>
        requires std::constructible_from<T, U&&>
    bool TryPush(U&& value) {
        const uint64_t head = m_head.value.load(std::memory_order_relaxed);
        const uint64_t tail = m_tail.value.load(std::memory_order_acquire);
        if (head - tail >= Capacity) {
            m_fullEvents.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        m_storage[head % Capacity].emplace(std::forward<U>(value));
        m_head.value.store(head + 1u, std::memory_order_release);
        UpdateHighWater(static_cast<size_t>(head + 1u - tail));
        return true;
    }

    bool TryPop(T& out) {
        const uint64_t tail = m_tail.value.load(std::memory_order_relaxed);
        const uint64_t head = m_head.value.load(std::memory_order_acquire);
        if (tail == head) {
            return false;
        }

        auto& slot = m_storage[tail % Capacity];
        out = std::move(*slot);
        slot.reset();
        m_tail.value.store(tail + 1u, std::memory_order_release);
        return true;
    }

    template <typename Fn>
    size_t Drain(Fn&& fn) {
        size_t count = 0;
        while (true) {
            const uint64_t tail = m_tail.value.load(std::memory_order_relaxed);
            const uint64_t head = m_head.value.load(std::memory_order_acquire);
            if (tail == head) {
                return count;
            }

            auto& slot = m_storage[tail % Capacity];
            std::invoke(fn, std::move(*slot));
            slot.reset();
            m_tail.value.store(tail + 1u, std::memory_order_release);
            ++count;
        }
    }

    [[nodiscard]] size_t Depth() const noexcept {
        const uint64_t tail = m_tail.value.load(std::memory_order_acquire);
        const uint64_t head = m_head.value.load(std::memory_order_acquire);
        return static_cast<size_t>(head - tail);
    }

    [[nodiscard]] constexpr size_t MaxSize() const noexcept { return Capacity; }
    [[nodiscard]] uint64_t FullEvents() const noexcept { return m_fullEvents.load(std::memory_order_relaxed); }
    [[nodiscard]] size_t HighWaterMark() const noexcept { return m_highWater.load(std::memory_order_relaxed); }

    // Only call while producer and consumer are quiescent.
    void Reset() noexcept {
        for (auto& slot : m_storage) {
            slot.reset();
        }
        m_tail.value.store(0, std::memory_order_relaxed);
        m_head.value.store(0, std::memory_order_relaxed);
        m_fullEvents.store(0, std::memory_order_relaxed);
        m_highWater.store(0, std::memory_order_relaxed);
    }

private:
    struct alignas(64) Cursor {
        std::atomic<uint64_t> value{0};
    };

    void UpdateHighWater(size_t depth) noexcept {
        size_t previous = m_highWater.load(std::memory_order_relaxed);
        while (previous < depth &&
               !m_highWater.compare_exchange_weak(
                   previous, depth, std::memory_order_relaxed, std::memory_order_relaxed)) {
        }
    }

    std::array<std::optional<T>, Capacity> m_storage{};
    Cursor m_head{};
    Cursor m_tail{};
    std::atomic<uint64_t> m_fullEvents{0};
    std::atomic<size_t> m_highWater{0};
};
