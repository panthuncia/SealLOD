#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Managers/SerializedTaskPump.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
void Check(bool condition) {
    if (!condition) std::abort();
}
}

int main() {
    // Deterministically exercise the handoff that boolean scheduled flags get
    // wrong: a notification arriving while the consumer is finishing must be
    // consumed by that worker, not require another scheduler submission.
    {
        std::mutex tasksMutex;
        std::vector<br::SerializedTaskPump::Task> tasks;
        std::atomic<int> drains{0};
        br::SerializedTaskPump pump;
        pump.Configure(
            [&](br::SerializedTaskPump::Task task) {
                std::lock_guard lock(tasksMutex);
                tasks.push_back(std::move(task));
                return true;
            },
            [&] {
                const int pass = drains.fetch_add(1, std::memory_order_acq_rel);
                if (pass == 0) Check(pump.Notify());
            });
        Check(pump.Notify());
        br::SerializedTaskPump::Task task;
        {
            std::lock_guard lock(tasksMutex);
            Check(tasks.size() == 1);
            task = std::move(tasks.front());
            tasks.clear();
        }
        task();
        Check(drains.load(std::memory_order_acquire) == 2);
        Check(pump.IsIdle());
        std::lock_guard lock(tasksMutex);
        Check(tasks.empty());
    }

    // Scheduler rejection closes the pump and reports failure exactly once;
    // subsequent producers cannot leave silently stranded work behind.
    {
        std::atomic<int> rejected{0};
        br::SerializedTaskPump pump;
        pump.Configure(
            [](br::SerializedTaskPump::Task) { return false; },
            [] {},
            [&] { rejected.fetch_add(1, std::memory_order_relaxed); });
        Check(!pump.Notify());
        Check(!pump.Notify());
        Check(rejected.load(std::memory_order_relaxed) == 1);
    }

    auto& scheduler = TaskSchedulerManager::GetInstance();
    TaskSchedulerManager::Config config{};
    config.workerCount = 2;
    config.blockingThreadCount = 1;
    config.staticConcurrency = 1;
    config.shaderConcurrency = 1;
    config.reserveRenderCpu = false;
    scheduler.Initialize(config);

    Check(scheduler.WorkerCount() >= 1);
    Check(scheduler.BlockingThreadCount() == 1);
    Check(scheduler.DomainConcurrency(TaskDomain::StaticImport) == 1);
    Check(scheduler.DomainConcurrency(TaskDomain::ShaderCompile) == 1);

    {
        auto scope = scheduler.CreateScope("serialized-pump-stress");
        std::atomic<int> pending{0};
        std::atomic<int> consumed{0};
        br::SerializedTaskPump pump;
        pump.Configure(
            [&](br::SerializedTaskPump::Task task) {
                return scheduler.Submit(scope, TaskLane::Streaming, TaskDomain::General,
                    "serialized-pump", [task = std::move(task)](const br::TaskContext& context) mutable {
                        if (!context.StopRequested()) task();
                    });
            },
            [&] { consumed.fetch_add(pending.exchange(0, std::memory_order_acq_rel)); });
        constexpr int producerCount = 8;
        constexpr int notificationsPerProducer = 2000;
        std::vector<std::thread> producers;
        for (int producer = 0; producer < producerCount; ++producer) {
            producers.emplace_back([&] {
                for (int notification = 0; notification < notificationsPerProducer; ++notification) {
                    pending.fetch_add(1, std::memory_order_release);
                    Check(pump.Notify());
                }
            });
        }
        for (auto& producer : producers) producer.join();
        scope.Wait();
        Check(pump.IsIdle());
        Check(pending.load(std::memory_order_acquire) == 0);
        Check(consumed.load(std::memory_order_acquire) ==
            producerCount * notificationsPerProducer);
        pump.Stop();
    }

    {
        auto scope = scheduler.CreateScope("basic");
        std::atomic<int> total{0};
        Check(scheduler.Submit(scope, TaskLane::Streaming, TaskDomain::General, "increment",
            [&total](const br::TaskContext& context) {
                Check(!context.StopRequested());
                total.fetch_add(1, std::memory_order_relaxed);
            }));
        scope.Wait();
        Check(total.load(std::memory_order_relaxed) == 1);
    }

    {
        std::atomic<int> total{0};
        scheduler.ParallelFor("nested", 64, [&total, &scheduler](std::size_t) {
            scheduler.ParallelFor(2, [&total](std::size_t) { total.fetch_add(1, std::memory_order_relaxed); });
        });
        Check(total.load(std::memory_order_relaxed) == 128);
    }

    {
        auto scope = scheduler.CreateScope("delayed-cancel");
        std::atomic<bool> ran{false};
        Check(scheduler.ScheduleAfter(scope, 100ms, TaskLane::Background, TaskDomain::Cleanup,
            "must-not-run", [&ran](const br::TaskContext&) { ran.store(true, std::memory_order_release); }));
        scope.CancelAndWait();
        Check(!ran.load(std::memory_order_acquire));
    }

    {
        auto scope = scheduler.CreateScope("cancel-running-and-delayed");
        std::atomic<bool> started{false};
        std::atomic<bool> release{false};
        std::atomic<bool> waited{false};
        Check(scheduler.Submit(scope, TaskLane::Streaming, TaskDomain::General, "running",
            [&started, &release](const br::TaskContext&) {
                started.store(true, std::memory_order_release);
                while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
            }));
        Check(scheduler.ScheduleAfter(scope, 1h, TaskLane::Background, TaskDomain::Cleanup,
            "cancelled-delay", [](const br::TaskContext&) {}));
        while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
        scope.Cancel();
        std::thread waiter([&] { scope.Wait(); waited.store(true, std::memory_order_release); });
        std::this_thread::sleep_for(10ms);
        Check(!waited.load(std::memory_order_acquire));
        release.store(true, std::memory_order_release);
        waiter.join();
        Check(waited.load(std::memory_order_acquire));
    }

    {
        auto scope = scheduler.CreateScope("exception");
        Check(scheduler.Submit(scope, TaskLane::Background, TaskDomain::General, "throws",
            [](const br::TaskContext&) { throw std::runtime_error("expected"); }));
        bool observed = false;
        try { scope.Wait(); } catch (const std::runtime_error&) { observed = true; }
        Check(observed);
    }

    {
        auto scope = scheduler.CreateScope("blocking-continuation");
        std::atomic<int> state{0};
        Check(scheduler.SubmitBlockingIo(scope, TaskDomain::AssetImport, "io",
            [&state](const br::TaskContext&) { state.store(1, std::memory_order_release); },
            TaskLane::Streaming,
            [&state](const br::TaskContext&) {
                Check(state.load(std::memory_order_acquire) == 1);
                state.store(2, std::memory_order_release);
            }));
        scope.Wait();
        Check(state.load(std::memory_order_acquire) == 2);
    }

    scheduler.Cleanup();
    return 0;
}
