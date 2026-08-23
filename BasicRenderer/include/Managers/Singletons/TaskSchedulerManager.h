#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace br {

enum class TaskLane : std::uint8_t { FrameCritical, Streaming, Background, Count };
enum class TaskDomain : std::uint8_t {
    General, StaticImport, AssetImport, TextureProcessing, ShaderCompile, Cleanup, Count
};

class TaskScope {
public:
    struct State;
    TaskScope() = default;

    [[nodiscard]] bool Valid() const noexcept { return static_cast<bool>(m_state); }
    [[nodiscard]] bool StopRequested() const noexcept;
    void Cancel() const noexcept;
    void Wait() const;
    void CancelAndWait() const;

private:
    explicit TaskScope(std::shared_ptr<State> state) : m_state(std::move(state)) {}
    std::shared_ptr<State> m_state;
    friend class TaskSchedulerManager;
};

class TaskContext {
public:
    struct State;
    [[nodiscard]] bool StopRequested() const noexcept;

private:
    explicit TaskContext(std::shared_ptr<State> state) : m_state(std::move(state)) {}
    std::shared_ptr<State> m_state;
    friend class TaskSchedulerManager;
};

class TaskSchedulerManager {
public:
    struct RuntimeState;
    struct Config {
        std::uint32_t workerCount = 0;
        std::uint32_t blockingThreadCount = 0;
        std::uint32_t staticConcurrency = 0;
        std::uint32_t shaderConcurrency = 0;
        bool reserveRenderCpu = true;
    };
    struct DomainStats {
        std::uint64_t queued = 0, active = 0, completed = 0, cancelled = 0, failed = 0;
        std::uint64_t queueWaitMicros = 0, executionMicros = 0, highWatermark = 0, longTasks = 0;
    };
    struct QueueStats {
        DomainStats domains[static_cast<std::size_t>(TaskDomain::Count)]{};
        std::uint32_t blockingQueued = 0, blockingActive = 0;
        std::uint32_t ioQueued = 0, ioActive = 0;
        std::uint32_t backgroundQueued = 0, backgroundActive = 0;
        std::uint32_t shaderCompileQueued = 0, shaderCompileActive = 0;
    };

    static TaskSchedulerManager& GetInstance();
    void Initialize();
    void Initialize(Config config);
    void InitializeForPlugin(std::uint32_t workerCount = 1);
    void Cleanup();

    [[nodiscard]] bool IsInitialized() const noexcept { return m_initialized.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint32_t WorkerCount() const noexcept { return m_workerCount; }
    [[nodiscard]] std::uint32_t BlockingThreadCount() const noexcept;
    [[nodiscard]] std::uint32_t DomainConcurrency(TaskDomain domain) const noexcept;
    [[nodiscard]] QueueStats GetQueueStats() const;
    [[nodiscard]] TaskScope CreateScope(std::string_view name);
    [[nodiscard]] TaskScope ProcessScope(TaskDomain domain) const;

    bool Submit(const TaskScope&, TaskLane, TaskDomain, std::string_view,
        std::function<void(const TaskContext&)>&& task);
    bool Submit(TaskLane, TaskDomain, std::string_view, std::function<void()>&& task);
    bool ScheduleAfter(const TaskScope&, std::chrono::steady_clock::duration, TaskLane, TaskDomain,
        std::string_view, std::function<void(const TaskContext&)>&& task);
    bool SubmitBlockingIo(const TaskScope&, TaskDomain, std::string_view,
        std::function<void(const TaskContext&)>&& blockingOperation, TaskLane continuationLane,
        std::function<void(const TaskContext&)>&& continuation);

    template <typename Func>
    void ParallelFor(std::size_t itemCount, Func&& func) {
        ParallelFor({}, itemCount, std::forward<Func>(func));
    }
    template <typename Func>
    void ParallelFor(std::string_view name, std::size_t itemCount, Func&& func) {
        using Callable = std::decay_t<Func>;
        Callable callable = std::forward<Func>(func);
        ParallelForImpl(name, itemCount, [callable = std::move(callable)](std::size_t i) mutable { callable(i); });
    }

private:
    TaskSchedulerManager() = default;
    void DispatchDomain(TaskDomain domain);
    void ParallelForImpl(std::string_view, std::size_t, std::function<void(std::size_t)>&&);
    std::unique_ptr<RuntimeState> m_runtimeState;
    std::atomic_bool m_initialized{ false };
    std::uint32_t m_workerCount = 0;
};

} // namespace br

using TaskSchedulerManager = br::TaskSchedulerManager;
using TaskLane = br::TaskLane;
using TaskDomain = br::TaskDomain;
using TaskScope = br::TaskScope;
