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

    // A delayed recovery token has independent ownership: it cannot suppress
    // an immediate producer notification, and a replaced token is a no-op.
    {
        std::vector<br::SerializedTaskPump::Task> immediateTasks;
        std::vector<br::SerializedTaskPump::Task> delayedTasks;
        std::atomic<int> drains{0};
        br::SerializedTaskPump pump;
        pump.Configure(
            [&](br::SerializedTaskPump::Task task) {
                immediateTasks.push_back(std::move(task));
                return true;
            },
            [&] { drains.fetch_add(1, std::memory_order_relaxed); },
            {},
            [&](std::chrono::steady_clock::duration, br::SerializedTaskPump::Task task) {
                delayedTasks.push_back(std::move(task));
                return true;
            });

        Check(pump.NotifyAfter(10ms));
        Check(pump.NotifyAfter(5ms));
        Check(delayedTasks.size() == 2);
        Check(pump.Notify());
        Check(immediateTasks.size() == 1);
        auto immediate = std::move(immediateTasks.front());
        immediateTasks.clear();
        immediate();
        Check(drains.load(std::memory_order_relaxed) == 1);

        auto staleTimer = std::move(delayedTasks.front());
        delayedTasks.erase(delayedTasks.begin());
        staleTimer();
        Check(immediateTasks.empty());
        auto currentTimer = std::move(delayedTasks.front());
        delayedTasks.clear();
        currentTimer();
        Check(immediateTasks.size() == 1);
        auto delayedDrain = std::move(immediateTasks.front());
        immediateTasks.clear();
        delayedDrain();
        Check(drains.load(std::memory_order_relaxed) == 2);

        const auto stats = pump.GetStats();
        Check(stats.requestedEpoch == 2);
        Check(stats.drainedEpoch == 2);
        Check(stats.delayedRequests == 2);
        Check(stats.delayedFired == 1);
        Check(stats.staleDelayed == 1);
        Check(!stats.runnerActive);
        Check(!stats.delayedArmed);
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
    Check(scheduler.DomainConcurrency(TaskDomain::GraphControl) == 1);
    Check(scheduler.DomainConcurrency(TaskDomain::GraphPublication) == 1);
	Check(scheduler.DomainConcurrency(TaskDomain::StaticImportControl) == 1);

    {
        auto producerScope = scheduler.CreateScope("renderer-state-saturation");
        auto isolatedScope = scheduler.CreateScope("graph-domain-isolation");
        std::atomic<bool> producerStarted{ false };
        std::atomic<bool> releaseProducer{ false };
        std::atomic<bool> controlStarted{ false };
        std::atomic<bool> publicationStarted{ false };
        Check(scheduler.Submit(producerScope, TaskLane::Streaming, TaskDomain::RendererState,
            "held-renderer-state-producer", [&](const br::TaskContext&) {
                producerStarted.store(true, std::memory_order_release);
                while (!releaseProducer.load(std::memory_order_acquire)) std::this_thread::yield();
            }));
        while (!producerStarted.load(std::memory_order_acquire)) std::this_thread::yield();
        const auto submittedAt = std::chrono::steady_clock::now();
        Check(scheduler.Submit(isolatedScope, TaskLane::Streaming, TaskDomain::GraphControl,
            "isolated-graph-control", [&](const br::TaskContext&) {
                controlStarted.store(true, std::memory_order_release);
            }));
        Check(scheduler.Submit(isolatedScope, TaskLane::Streaming, TaskDomain::GraphPublication,
            "isolated-graph-publication", [&](const br::TaskContext&) {
                publicationStarted.store(true, std::memory_order_release);
            }));
        const auto deadline = submittedAt + 100ms;
        while ((!controlStarted.load(std::memory_order_acquire) ||
                !publicationStarted.load(std::memory_order_acquire)) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        releaseProducer.store(true, std::memory_order_release);
        producerScope.Wait();
        isolatedScope.Wait();
        Check(controlStarted.load(std::memory_order_acquire));
        Check(publicationStarted.load(std::memory_order_acquire));
    }

	{
		auto workerScope = scheduler.CreateScope("static-import-worker-saturation");
		auto controlScope = scheduler.CreateScope("static-import-control-isolation");
		std::atomic<bool> workerStarted{ false };
		std::atomic<bool> releaseWorker{ false };
		std::atomic<bool> controlStarted{ false };
		Check(scheduler.Submit(workerScope, TaskLane::Streaming, TaskDomain::StaticImport,
			"held-static-import-worker", [&](const br::TaskContext&) {
				workerStarted.store(true, std::memory_order_release);
				while (!releaseWorker.load(std::memory_order_acquire)) std::this_thread::yield();
			}));
		while (!workerStarted.load(std::memory_order_acquire)) std::this_thread::yield();
		Check(scheduler.Submit(controlScope, TaskLane::Streaming, TaskDomain::StaticImportControl,
			"isolated-static-import-control", [&](const br::TaskContext&) {
				controlStarted.store(true, std::memory_order_release);
			}));
		const auto deadline = std::chrono::steady_clock::now() + 100ms;
		while (!controlStarted.load(std::memory_order_acquire) &&
			std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
		releaseWorker.store(true, std::memory_order_release);
		workerScope.Wait();
		controlScope.Wait();
		Check(controlStarted.load(std::memory_order_acquire));
	}

    // Shared scene-graph admission rotates among producer keys without violating
    // FIFO within a key. A deep static-ingestion prefix must not hide ready grass,
    // dependency, or publication work using another key in the same domain/lane.
    {
        auto scope = scheduler.CreateScope("scene-graph-admission-key-rotation");
        std::atomic<bool> heldStarted{ false };
        std::atomic<bool> releaseHeld{ false };
        std::mutex orderMutex;
        std::vector<int> order;
        const br::TaskTraceMetadata staticTrace{
            .admissionKey = 11, .workClass = 2, .admissionGroup = 1 };
        const br::TaskTraceMetadata otherTrace{
            .admissionKey = 22, .workClass = 2, .admissionGroup = 1 };
        Check(scheduler.Submit(scope, TaskLane::Streaming, TaskDomain::StaticImport,
            "held-static-key", [&](const br::TaskContext&) {
                heldStarted.store(true, std::memory_order_release);
                while (!releaseHeld.load(std::memory_order_acquire)) std::this_thread::yield();
            }, staticTrace));
        while (!heldStarted.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int index = 0; index < 16; ++index) {
            Check(scheduler.Submit(scope, TaskLane::Streaming, TaskDomain::StaticImport,
                "queued-static-key", [&, index](const br::TaskContext&) {
                    std::lock_guard lock(orderMutex);
                    order.push_back(index);
                }, staticTrace));
        }
        Check(scheduler.Submit(scope, TaskLane::Streaming, TaskDomain::StaticImport,
            "queued-other-key", [&](const br::TaskContext&) {
                std::lock_guard lock(orderMutex);
                order.push_back(100);
            }, otherTrace));
        releaseHeld.store(true, std::memory_order_release);
        scope.Wait();
        std::lock_guard lock(orderMutex);
        Check(!order.empty());
        Check(order.front() == 100);
        for (int index = 0; index < 16; ++index) Check(order[index + 1] == index);
    }

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
