#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace br {

// Single-producer/single-consumer latest-wins mailbox with exactly three
// reusable payload allocations. The producer, consumer, and atomic mailbox
// each own one slot. Neither side waits for the other: publication and
// consumption are pointer exchanges, and intermediate generations coalesce.
// A slot leaving use (a coalesced or already-consumed generation) is reset at
// once, so what its payload holds (e.g. versioned-buffer leases) does not
// outlive its generation until the slot happens to be rewritten.
template <class T>
class TripleGenerationMailbox {
public:
    struct Slot {
        std::uint64_t generation = 0;
        T value{};
    };

    TripleGenerationMailbox()
        : m_producer(std::make_unique<Slot>()),
          m_consumer(std::make_unique<Slot>()) {
        auto mailbox = std::make_unique<Slot>();
        m_mailbox.store(mailbox.release(), std::memory_order_relaxed);
    }

    TripleGenerationMailbox(const TripleGenerationMailbox&) = delete;
    TripleGenerationMailbox& operator=(const TripleGenerationMailbox&) = delete;

    ~TripleGenerationMailbox() {
        std::unique_ptr<Slot> mailbox(
            m_mailbox.exchange(nullptr, std::memory_order_acq_rel));
    }

    // Producer-thread only. The returned value remains exclusively owned by
    // the producer until Publish is called.
    [[nodiscard]] T& ProducerValue() noexcept { return m_producer->value; }

    void Publish(std::uint64_t generation) noexcept {
        m_producer->generation = generation;
        Slot* previous = m_mailbox.exchange(
            m_producer.release(), std::memory_order_acq_rel);
        m_producer.reset(previous);
        if (m_producer) m_producer->value = T{};
        m_publishedGeneration.store(generation, std::memory_order_release);
    }

    // Consumer-thread only. Returns null when no newer generation is present.
    // The returned value remains valid until the next successful consume.
    [[nodiscard]] const T* ConsumeLatest() noexcept {
        if (m_publishedGeneration.load(std::memory_order_acquire) <=
            m_consumedGeneration) {
            return nullptr;
        }
        if (m_consumer) m_consumer->value = T{};
        Slot* incoming = m_mailbox.exchange(
            m_consumer.release(), std::memory_order_acq_rel);
        m_consumer.reset(incoming);
        if (!m_consumer || m_consumer->generation <= m_consumedGeneration) {
            return nullptr;
        }
        m_consumedGeneration = m_consumer->generation;
        return std::addressof(m_consumer->value);
    }

    // Consumer-thread only: the consumed value is no longer needed (e.g. it was
    // submitted); ConsumerValue then returns an empty value until the next consume.
    void ReleaseConsumerValue() noexcept {
        if (m_consumer) m_consumer->value = T{};
    }
    [[nodiscard]] const T* ConsumerValue() const noexcept {
        return m_consumedGeneration != 0 && m_consumer
            ? std::addressof(m_consumer->value) : nullptr;
    }
    [[nodiscard]] std::uint64_t ConsumedGeneration() const noexcept {
        return m_consumedGeneration;
    }
    [[nodiscard]] std::uint64_t PublishedGeneration() const noexcept {
        return m_publishedGeneration.load(std::memory_order_acquire);
    }

private:
    std::unique_ptr<Slot> m_producer;
    std::unique_ptr<Slot> m_consumer;
    std::atomic<Slot*> m_mailbox{ nullptr };
    std::atomic<std::uint64_t> m_publishedGeneration{ 0 };
    std::uint64_t m_consumedGeneration = 0;
};

} // namespace br
