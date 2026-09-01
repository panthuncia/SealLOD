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
    General, RendererState, StaticImport, AssetImport, TextureProcessing, ShaderCompile, Cleanup,
    // Keep graph topology transitions and manifest assembly out of the
    // single-slot RendererState producer queue. Appended to preserve the
    // numeric identity of existing telemetry domains.
	GraphControl, GraphPublication, StaticImportControl,
    // Mutable resource work must not queue behind serialized renderer-state
    // acceptance or texture decoding.
    MaterialAcceptance, GpuBufferBuild, Count
};

// Numeric scheduler trace metadata is deliberately independent of the graph
// headers. The scheduler can therefore feed the renderer's unified async trace
// without formatting strings or allocating trace records on its hot path.
enum class TaskTraceEventID : std::uint8_t {
    Queued, Admitted, Started, Completed, Cancelled, Rejected, Resubmitted
};

struct TaskTraceMetadata {
    std::uint64_t taskKind = 0;
    std::uint64_t correlationID = 0;
    std::uint64_t admissionKey = 0;
    std::uint8_t workClass = 0;
    std::uint8_t schedulingReason = 0;
    std::uint8_t admissionGroup = 0;
};

struct TaskTraceEvent {
    TaskTraceEventID event{ TaskTraceEventID::Queued };
    TaskTraceMetadata metadata{};
    std::uint64_t taskID = 0;
    std::uint64_t queueWaitMicros = 0;
    std::uint64_t executionMicros = 0;
    std::uint64_t queuedDepth = 0;
    std::uint32_t activeCount = 0;
    TaskDomain domain{ TaskDomain::General };
    TaskLane lane{ TaskLane::Streaming };
    std::uint8_t outcome = 0;
};
static_assert(std::is_trivially_copyable_v<TaskTraceEvent>);

using TaskTraceCallback = void(*)(void*, const TaskTraceEvent&) noexcept;

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
        std::uint64_t queueWaitMicros = 0, maxQueueWaitMicros = 0;
        std::uint64_t executionMicros = 0, maxExecutionMicros = 0;
        std::uint64_t highWatermark = 0, longTasks = 0;
        std::string maxQueueWaitTask;
        std::string maxExecutionTask;
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
        std::function<void(const TaskContext&)>&& task, TaskTraceMetadata trace = {});
    bool Submit(TaskLane, TaskDomain, std::string_view, std::function<void()>&& task,
        TaskTraceMetadata trace = {});
    bool ScheduleAfter(const TaskScope&, std::chrono::steady_clock::duration, TaskLane, TaskDomain,
        std::string_view, std::function<void(const TaskContext&)>&& task);
    bool SubmitBlockingIo(const TaskScope&, TaskDomain, std::string_view,
        std::function<void(const TaskContext&)>&& blockingOperation, TaskLane continuationLane,
        std::function<void(const TaskContext&)>&& continuation);
    bool InstallTaskTraceSink(void* context, TaskTraceCallback callback) noexcept;
    void RemoveTaskTraceSink(void* context) noexcept;

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
    template <typename Func>
    void ParallelForLimited(std::string_view name, std::size_t itemCount,
        std::size_t maximumConcurrency, Func&& func) {
        using Callable = std::decay_t<Func>;
        Callable callable = std::forward<Func>(func);
        ParallelForLimitedImpl(name, itemCount, maximumConcurrency,
            [callable = std::move(callable)](std::size_t i) mutable { callable(i); });
    }

private:
    TaskSchedulerManager() = default;
    void DispatchDomain(TaskDomain domain);
    void DispatchSceneGraphWork();
    void ParallelForImpl(std::string_view, std::size_t, std::function<void(std::size_t)>&&);
    void ParallelForLimitedImpl(std::string_view, std::size_t, std::size_t,
        std::function<void(std::size_t)>&&);
    std::unique_ptr<RuntimeState> m_runtimeState;
    std::atomic_bool m_initialized{ false };
    std::uint32_t m_workerCount = 0;
    void EmitTaskTrace(const TaskTraceEvent& event) noexcept;
    [[nodiscard]] bool TaskTraceActive() const noexcept {
        return m_taskTraceContext.load(std::memory_order_acquire) != nullptr;
    }
    std::atomic<void*> m_taskTraceContext{ nullptr };
    std::atomic<TaskTraceCallback> m_taskTraceCallback{ nullptr };
    struct TaskTraceHazardSlot {
        std::atomic<void*> context{ nullptr };
        TaskTraceHazardSlot* next = nullptr;
    };
    [[nodiscard]] TaskTraceHazardSlot* ThreadTaskTraceHazard() noexcept;
    std::atomic<TaskTraceHazardSlot*> m_taskTraceHazards{ nullptr };
    std::atomic_uint64_t m_nextTraceTaskID{ 1 };
};

} // namespace br

using TaskSchedulerManager = br::TaskSchedulerManager;
using TaskLane = br::TaskLane;
using TaskDomain = br::TaskDomain;
using TaskScope = br::TaskScope;
