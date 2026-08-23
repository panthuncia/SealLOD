#include "Utilities/BoundedSpscQueue.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

int main() {
    {
        BoundedSpscQueue<uint32_t, 4> queue;
        assert(queue.Depth() == 0);
        for (uint32_t i = 0; i < 4; ++i) {
            assert(queue.TryPush(i));
        }
        assert(!queue.TryPush(4));
        assert(queue.FullEvents() == 1);
        assert(queue.HighWaterMark() == 4);

        uint32_t value = ~0u;
        for (uint32_t i = 0; i < 4; ++i) {
            assert(queue.TryPop(value));
            assert(value == i);
        }
        assert(!queue.TryPop(value));

        // Exercise repeated modulo wraparound, not just the first storage lap.
        for (uint32_t i = 0; i < 1000; ++i) {
            assert(queue.TryPush(i));
            assert(queue.TryPop(value));
            assert(value == i);
        }
    }

    {
        BoundedSpscQueue<std::unique_ptr<uint32_t>, 2> queue;
        assert(queue.TryPush(std::make_unique<uint32_t>(42)));
        std::unique_ptr<uint32_t> value;
        assert(queue.TryPop(value));
        assert(value && *value == 42);
    }

    {
        constexpr uint32_t kCount = 500'000;
        BoundedSpscQueue<uint32_t, 1024> queue;
        std::atomic<bool> start{false};
        std::thread producer([&] {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (uint32_t i = 0; i < kCount; ++i) {
                while (!queue.TryPush(i)) {
                    std::this_thread::yield();
                }
            }
        });

        start.store(true, std::memory_order_release);
        for (uint32_t expected = 0; expected < kCount; ++expected) {
            uint32_t value = 0;
            while (!queue.TryPop(value)) {
                std::this_thread::yield();
            }
            assert(value == expected);
        }
        producer.join();
        assert(queue.Depth() == 0);
    }

    return 0;
}
