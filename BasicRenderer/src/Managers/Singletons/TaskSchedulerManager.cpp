#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Managers/Singletons/TaskSchedulerManager.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <optional>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/info.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_scheduler_observer.h>
#include <spdlog/spdlog.h>
#include <tracy/TracyC.h>
#include <Windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace br {
namespace {

constexpr std::uint64_t kLongTaskMicros = 4'000;
constexpr std::size_t kLaneCount = static_cast<std::size_t>(TaskLane::Count);
constexpr std::size_t kDomainCount = static_cast<std::size_t>(TaskDomain::Count);
thread_local bool g_inSchedulerTask = false;
const char* DomainName(TaskDomain domain);

constexpr std::uint64_t StableTaskKind(std::string_view value) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    return hash == 0 ? 1u : hash;
}

void LogLongTask(std::string_view name, TaskDomain domain, std::uint64_t elapsedUs) {
    struct WarningState {
        std::chrono::steady_clock::time_point lastLogged{};
        std::uint64_t suppressed = 0;
    };
    static std::mutex mutex;
    static std::unordered_map<std::string, WarningState> warnings;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(mutex);
    auto& warning = warnings[std::string(name)];
    if (warning.lastLogged != std::chrono::steady_clock::time_point{} &&
        now - warning.lastLogged < std::chrono::seconds(5)) {
        ++warning.suppressed;
        return;
    }
    spdlog::warn("Long scheduler task '{}' domain={} duration={}us suppressed_since_last={}",
        name, DomainName(domain), elapsedUs, warning.suppressed);
    warning.lastLogged = now;
    warning.suppressed = 0;
}

std::uint32_t ReadEnvironmentUint(const char* name, std::uint32_t fallback) {
    char* value = nullptr;
    std::size_t valueSize = 0;
    if (_dupenv_s(&value, &valueSize, name) != 0 || value == nullptr) return fallback;
    char* end = nullptr;
    const auto parsed = std::strtoul(value, &end, 10);
    const bool valid = end != value && *end == '\0';
    std::free(value);
    return valid ? static_cast<std::uint32_t>(parsed) : fallback;
}

const char* DomainName(TaskDomain domain) {
    switch (domain) {
    case TaskDomain::General: return "General";
    case TaskDomain::RendererState: return "RendererState";
    case TaskDomain::StaticImport: return "StaticImport";
    case TaskDomain::AssetImport: return "AssetImport";
    case TaskDomain::TextureProcessing: return "TextureProcessing";
    case TaskDomain::ShaderCompile: return "ShaderCompile";
    case TaskDomain::Cleanup: return "Cleanup";
    case TaskDomain::GraphControl: return "GraphControl";
    case TaskDomain::GraphPublication: return "GraphPublication";
	case TaskDomain::StaticImportControl: return "StaticImportControl";
    case TaskDomain::MaterialAcceptance: return "MaterialAcceptance";
    case TaskDomain::GpuBufferBuild: return "GpuBufferBuild";
    default: return "Unknown";
    }
}

struct CpuSetSelection {
    std::vector<ULONG> workers;
    std::optional<ULONG> render;
};

using GetSystemCpuSetInformationFn = BOOL(WINAPI*)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
using SetProcessDefaultCpuSetsFn = BOOL(WINAPI*)(HANDLE, const ULONG*, ULONG);
using SetThreadSelectedCpuSetsFn = BOOL(WINAPI*)(HANDLE, const ULONG*, ULONG);

template <typename Function>
Function ResolveKernelFunction(const char* name) {
    return reinterpret_cast<Function>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), name));
}

CpuSetSelection SelectCpuSets(bool reserveRenderCpu) {
    CpuSetSelection result;
    const auto getCpuSets = ResolveKernelFunction<GetSystemCpuSetInformationFn>("GetSystemCpuSetInformation");
    if (!getCpuSets) return result;
    ULONG required = 0;
    (void)getCpuSets(nullptr, 0, &required, GetCurrentProcess(), 0);
    if (required == 0) return result;
    std::vector<std::byte> storage(required);
    if (!getCpuSets(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(storage.data()),
            required, &required, GetCurrentProcess(), 0)) return result;

    struct Candidate { ULONG id; BYTE efficiency; };
    std::vector<Candidate> candidates;
    for (ULONG offset = 0; offset < required;) {
        auto* info = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(storage.data() + offset);
        if (info->Type == CpuSetInformation && !info->CpuSet.Parked) {
            candidates.push_back({ info->CpuSet.Id, info->CpuSet.EfficiencyClass });
        }
        if (info->Size == 0) break;
        offset += info->Size;
    }
    if (candidates.empty()) return result;
    const auto selected = std::max_element(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.efficiency == rhs.efficiency ? lhs.id < rhs.id : lhs.efficiency < rhs.efficiency;
    });
    if (reserveRenderCpu && candidates.size() > 1) result.render = selected->id;
    for (const auto& candidate : candidates) {
        if (!result.render || candidate.id != *result.render) result.workers.push_back(candidate.id);
    }
    return result;
}

void ApplyCpuSets(const std::vector<ULONG>& ids) {
    const auto setCpuSets = ResolveKernelFunction<SetThreadSelectedCpuSetsFn>("SetThreadSelectedCpuSets");
    if (setCpuSets && !ids.empty()) (void)setCpuSets(GetCurrentThread(), ids.data(), static_cast<ULONG>(ids.size()));
}

} // namespace

struct TaskScope::State {
    explicit State(std::string value) : name(std::move(value)) {}
    std::string name;
    std::atomic_bool cancelled{ false };
    std::atomic_bool accepting{ true };
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    std::uint64_t outstanding = 0;
	std::uint64_t delayedOutstanding = 0;
    std::exception_ptr firstException;
};

struct TaskContext::State { std::weak_ptr<TaskScope::State> scope; };

bool TaskScope::StopRequested() const noexcept {
    return !m_state || m_state->cancelled.load(std::memory_order_acquire);
}
void TaskScope::Cancel() const noexcept {
    if (!m_state) return;
    m_state->accepting.store(false, std::memory_order_release);
    m_state->cancelled.store(true, std::memory_order_release);
	{
		std::lock_guard lock(m_state->mutex);
		m_state->outstanding -= (std::min)(m_state->outstanding, m_state->delayedOutstanding);
		m_state->delayedOutstanding = 0;
	}
    m_state->cv.notify_all();
}
void TaskScope::Wait() const {
    if (!m_state) return;
    std::exception_ptr error;
    {
        std::unique_lock lock(m_state->mutex);
        m_state->cv.wait(lock, [state = m_state] { return state->outstanding == 0; });
        error = m_state->firstException;
    }
    if (error) std::rethrow_exception(error);
}
void TaskScope::CancelAndWait() const { Cancel(); Wait(); }

bool TaskContext::StopRequested() const noexcept {
    const auto scope = m_state ? m_state->scope.lock() : nullptr;
    return !scope || scope->cancelled.load(std::memory_order_acquire);
}

class CpuSetObserver final : public oneapi::tbb::task_scheduler_observer {
public:
    CpuSetObserver(oneapi::tbb::task_arena& arena, std::vector<ULONG> ids)
        : oneapi::tbb::task_scheduler_observer(arena), m_ids(std::move(ids)) { observe(true); }
    void on_scheduler_entry(bool worker) override { if (worker) ApplyCpuSets(m_ids); }
private:
    std::vector<ULONG> m_ids;
};

struct TaskSchedulerManager::RuntimeState {
    struct PendingTask {
        std::shared_ptr<TaskScope::State> scope;
        TaskLane lane{};
        TaskDomain domain{};
        std::string name;
        std::function<void(const TaskContext&)> body;
        std::chrono::steady_clock::time_point queuedAt;
        TaskTraceMetadata trace{};
        std::uint64_t traceTaskID = 0;
        std::uint64_t admittedQueuedDepth = 0;
        std::uint32_t admittedActiveCount = 0;
    };
    struct DelayedTask {
        std::chrono::steady_clock::time_point due;
        std::uint64_t sequence{};
        PendingTask task;
        bool operator>(const DelayedTask& rhs) const {
            return due == rhs.due ? sequence > rhs.sequence : due > rhs.due;
        }
    };
    struct DomainRuntime {
        std::array<std::deque<PendingTask>, static_cast<std::size_t>(TaskLane::Count)> queues;
        std::uint32_t active{};
        std::uint32_t limit{ 1 };
        std::uint32_t consecutiveFrameCritical{};
        DomainStats stats;

        [[nodiscard]] std::size_t Queued() const noexcept {
            std::size_t result = 0;
            for (const auto& queue : queues) result += queue.size();
            return result;
        }
    };
    struct BlockingTask {
        std::shared_ptr<TaskScope::State> scope;
        TaskDomain domain{};
        std::string name;
        std::function<void(const TaskContext&)> operation;
        TaskLane continuationLane{};
        std::function<void(const TaskContext&)> continuation;
    };

    std::unique_ptr<oneapi::tbb::global_control> control;
    std::unique_ptr<oneapi::tbb::task_arena> frameArena, streamingArena, backgroundArena;
    std::array<std::unique_ptr<CpuSetObserver>, kLaneCount> observers;
    std::array<std::shared_ptr<TaskScope::State>, kDomainCount> processScopes;
    std::array<DomainRuntime, kDomainCount> domains;
    std::vector<ULONG> workerCpuSets;
    mutable std::mutex taskMutex;
    std::atomic_bool stopping{ false };

    std::mutex blockingMutex;
    std::condition_variable blockingCv;
    std::deque<BlockingTask> blockingQueue;
    std::vector<std::thread> blockingThreads;
    std::uint32_t blockingActive{};

    std::mutex timerMutex;
    std::condition_variable timerCv;
    std::priority_queue<DelayedTask, std::vector<DelayedTask>, std::greater<>> timers;
    std::thread timerThread;
    std::uint64_t nextTimer{};
};

namespace {
void LogTaskException(std::string_view taskName, TaskDomain domain, const std::exception_ptr& error) noexcept {
    if (!error) return;
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& exception) {
        spdlog::error("Scheduler task '{}' domain={} failed: {}", taskName, DomainName(domain), exception.what());
    } catch (...) {
        spdlog::error("Scheduler task '{}' domain={} failed with a non-standard exception", taskName, DomainName(domain));
    }
}

void CompleteScope(const std::shared_ptr<TaskScope::State>& scope, std::exception_ptr error = {}) {
    std::lock_guard lock(scope->mutex);
    if (error && !scope->firstException) scope->firstException = error;
    if (scope->outstanding > 0) --scope->outstanding;
    if (scope->outstanding == 0) scope->cv.notify_all();
}

oneapi::tbb::task_arena& SelectArena(TaskSchedulerManager::RuntimeState& state, TaskLane lane) {
    if (lane == TaskLane::FrameCritical) return *state.frameArena;
    if (lane == TaskLane::Streaming) return *state.streamingArena;
    return *state.backgroundArena;
}
} // namespace

TaskSchedulerManager& TaskSchedulerManager::GetInstance() {
    static TaskSchedulerManager scheduler;
    return scheduler;
}

TaskScope TaskSchedulerManager::CreateScope(std::string_view name) {
    return TaskScope(std::make_shared<TaskScope::State>(std::string(name)));
}
TaskScope TaskSchedulerManager::ProcessScope(TaskDomain domain) const {
    return m_runtimeState ? TaskScope(m_runtimeState->processScopes[static_cast<std::size_t>(domain)]) : TaskScope{};
}

void TaskSchedulerManager::InitializeForPlugin(std::uint32_t workerCount) {
    Config config;
    config.workerCount = std::max(1u, workerCount);
    config.blockingThreadCount = 1;
	// Static import workers are explicitly written as bounded parallel drains.
	// Keep their serialized coordinator in StaticImportControl instead of
	// collapsing preparation/materialization throughput to one worker.
	config.staticConcurrency = 0;
    config.shaderConcurrency = 1;
    config.reserveRenderCpu = false;
    Initialize(config);
}

void TaskSchedulerManager::Initialize() { Initialize(Config{}); }

void TaskSchedulerManager::Initialize(Config config) {
    bool expected = false;
    if (!m_initialized.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

    const std::uint32_t concurrency = static_cast<std::uint32_t>(std::max(1, oneapi::tbb::info::default_concurrency()));
    const std::uint32_t availableWorkers = concurrency > 1 ? concurrency - 1 : 1;
    const auto requestedWorkers = ReadEnvironmentUint("SARP_SCHEDULER_WORKERS", config.workerCount);
    m_workerCount = requestedWorkers == 0 ? availableWorkers : std::clamp(requestedWorkers, 1u, availableWorkers);
    const std::uint32_t autoBlocking = concurrency < 2 ? 1u : std::clamp((concurrency + 3u) / 4u, 2u, 4u);
    const std::uint32_t blockingCount = std::clamp(ReadEnvironmentUint("SARP_SCHEDULER_BLOCKING_THREADS",
        config.blockingThreadCount == 0 ? autoBlocking : config.blockingThreadCount), 1u, 4u);
    const std::uint32_t staticLimit = std::clamp(ReadEnvironmentUint("SARP_SCHEDULER_STATIC_CONCURRENCY",
        config.staticConcurrency == 0
            ? (std::min)(8u, (std::max)(2u, (m_workerCount + 1u) / 2u))
            : config.staticConcurrency), 1u, m_workerCount);
    const std::uint32_t shaderLimit = std::clamp(ReadEnvironmentUint("SARP_SCHEDULER_SHADER_CONCURRENCY",
        config.shaderConcurrency == 0 ? std::min(2u, std::max(1u, m_workerCount / 2u)) : config.shaderConcurrency), 1u, m_workerCount);

    m_runtimeState = std::make_unique<RuntimeState>();
    auto& state = *m_runtimeState;
    const auto cpuSets = SelectCpuSets(config.reserveRenderCpu);
    state.workerCpuSets = cpuSets.workers;
    if (!state.workerCpuSets.empty()) {
        if (const auto setProcessCpuSets = ResolveKernelFunction<SetProcessDefaultCpuSetsFn>("SetProcessDefaultCpuSets")) {
            (void)setProcessCpuSets(GetCurrentProcess(), state.workerCpuSets.data(), static_cast<ULONG>(state.workerCpuSets.size()));
        }
    }
    if (cpuSets.render) {
        const ULONG render = *cpuSets.render;
        if (const auto setThreadCpuSets = ResolveKernelFunction<SetThreadSelectedCpuSetsFn>("SetThreadSelectedCpuSets")) {
            (void)setThreadCpuSets(GetCurrentThread(), &render, 1);
        }
    }

    state.control = std::make_unique<oneapi::tbb::global_control>(oneapi::tbb::global_control::max_allowed_parallelism,
        static_cast<std::size_t>(m_workerCount + 1u));
    state.frameArena = std::make_unique<oneapi::tbb::task_arena>(static_cast<int>(m_workerCount + 1u), 1,
        oneapi::tbb::task_arena::priority::high);
    state.streamingArena = std::make_unique<oneapi::tbb::task_arena>(static_cast<int>(m_workerCount), 0,
        oneapi::tbb::task_arena::priority::normal);
    state.backgroundArena = std::make_unique<oneapi::tbb::task_arena>(static_cast<int>(std::max(1u, m_workerCount - 1u)), 0,
        oneapi::tbb::task_arena::priority::low);
    state.frameArena->initialize(); state.streamingArena->initialize(); state.backgroundArena->initialize();
    state.observers[0] = std::make_unique<CpuSetObserver>(*state.frameArena, state.workerCpuSets);
    state.observers[1] = std::make_unique<CpuSetObserver>(*state.streamingArena, state.workerCpuSets);
    state.observers[2] = std::make_unique<CpuSetObserver>(*state.backgroundArena, state.workerCpuSets);

    for (std::size_t i = 0; i < kDomainCount; ++i) {
        state.processScopes[i] = std::make_shared<TaskScope::State>(DomainName(static_cast<TaskDomain>(i)));
        state.domains[i].limit = m_workerCount;
    }
    state.domains[static_cast<std::size_t>(TaskDomain::StaticImport)].limit = staticLimit;
    state.domains[static_cast<std::size_t>(TaskDomain::RendererState)].limit = 1u;
    state.domains[static_cast<std::size_t>(TaskDomain::AssetImport)].limit = std::min(4u, m_workerCount);
    // Texture decode/reload work is the dominant CPU queue during scene import.
    // A fixed limit of two left large machines mostly idle and made queue-drain
    // time proportional to a tuning constant. Scale conservatively with the
    // available worker pool while retaining headroom for graph and renderer
    // control work and bounding concurrent decode memory.
    const auto textureLimit = (std::min)(8u, (m_workerCount + 3u) / 4u);
    state.domains[static_cast<std::size_t>(TaskDomain::TextureProcessing)].limit =
        (std::min)(m_workerCount, (std::max)(1u, textureLimit));
    state.domains[static_cast<std::size_t>(TaskDomain::ShaderCompile)].limit = shaderLimit;
    state.domains[static_cast<std::size_t>(TaskDomain::Cleanup)].limit = std::max(1u, m_workerCount - 1u);
    state.domains[static_cast<std::size_t>(TaskDomain::GraphControl)].limit = 1u;
    state.domains[static_cast<std::size_t>(TaskDomain::GraphPublication)].limit = 1u;
	state.domains[static_cast<std::size_t>(TaskDomain::StaticImportControl)].limit = 1u;
    state.domains[static_cast<std::size_t>(TaskDomain::MaterialAcceptance)].limit = 1u;
    state.domains[static_cast<std::size_t>(TaskDomain::GpuBufferBuild)].limit =
        (std::min)(4u, m_workerCount);

    state.timerThread = std::thread([this] {
        TracyCSetThreadName("Task Timer"); ApplyCpuSets(m_runtimeState->workerCpuSets);
        std::unique_lock lock(m_runtimeState->timerMutex);
        for (;;) {
            if (m_runtimeState->stopping.load(std::memory_order_acquire)) return;
            if (m_runtimeState->timers.empty()) { m_runtimeState->timerCv.wait(lock); continue; }
            const auto due = m_runtimeState->timers.top().due;
            if (m_runtimeState->timerCv.wait_until(lock, due) != std::cv_status::timeout) continue;
            if (m_runtimeState->timers.empty() || m_runtimeState->timers.top().due > std::chrono::steady_clock::now()) continue;
            auto delayed = std::move(const_cast<RuntimeState::DelayedTask&>(m_runtimeState->timers.top()));
            m_runtimeState->timers.pop(); lock.unlock();
			bool tokenActive = false;
			{
				std::lock_guard scopeLock(delayed.task.scope->mutex);
				if (delayed.task.scope->delayedOutstanding > 0) {
					--delayed.task.scope->delayedOutstanding;
					tokenActive = true;
				}
			}
			if (tokenActive) {
				if (delayed.task.traceTaskID != 0) EmitTaskTrace({
					TaskTraceEventID::Resubmitted, delayed.task.trace,
					delayed.task.traceTaskID, 0, 0, 0, 0,
					delayed.task.domain, delayed.task.lane, 0 });
				Submit(TaskScope(delayed.task.scope), delayed.task.lane, delayed.task.domain,
					delayed.task.name, std::move(delayed.task.body), delayed.task.trace);
				CompleteScope(delayed.task.scope);
			}
            lock.lock();
        }
    });

    for (std::uint32_t i = 0; i < blockingCount; ++i) {
        state.blockingThreads.emplace_back([this, i] {
            std::array<char, 32> name{}; std::snprintf(name.data(), name.size(), "Blocking IO %u", i);
            TracyCSetThreadName(name.data()); ApplyCpuSets(m_runtimeState->workerCpuSets);
            (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            for (;;) {
                RuntimeState::BlockingTask task;
                {
                    std::unique_lock lock(m_runtimeState->blockingMutex);
                    m_runtimeState->blockingCv.wait(lock, [this] { return m_runtimeState->stopping.load() || !m_runtimeState->blockingQueue.empty(); });
                    if (m_runtimeState->stopping.load() && m_runtimeState->blockingQueue.empty()) return;
                    task = std::move(m_runtimeState->blockingQueue.front()); m_runtimeState->blockingQueue.pop_front();
                    ++m_runtimeState->blockingActive;
                }
                auto contextState = std::make_shared<TaskContext::State>(); contextState->scope = task.scope;
                TaskContext context(contextState); std::exception_ptr error;
                if (!context.StopRequested()) { try { task.operation(context); } catch (...) { error = std::current_exception(); } }
				LogTaskException(task.name, task.domain, error);
                { std::lock_guard lock(m_runtimeState->blockingMutex); --m_runtimeState->blockingActive; }
                if (!error && !context.StopRequested() && task.continuation) {
                    Submit(TaskScope(task.scope), task.continuationLane, task.domain, task.name + "::Continuation", std::move(task.continuation));
                } else if (error) {
                    std::lock_guard lock(task.scope->mutex); if (!task.scope->firstException) task.scope->firstException = error;
                }
                CompleteScope(task.scope);
            }
        });
    }
    spdlog::info("Unified oneTBB scheduler: concurrency={} workers={} blocking={} static={} shaders={}",
        concurrency, m_workerCount, blockingCount, staticLimit, shaderLimit);
}

bool TaskSchedulerManager::Submit(const TaskScope& scope, TaskLane lane, TaskDomain domain, std::string_view name,
    std::function<void(const TaskContext&)>&& body, TaskTraceMetadata trace) {
    const bool tracing = TaskTraceActive();
    if (tracing && trace.taskKind == 0) trace.taskKind = StableTaskKind(name);
    const auto traceTaskID = tracing
        ? m_nextTraceTaskID.fetch_add(1, std::memory_order_relaxed) : 0;
    const auto reject = [&] {
        if (tracing) EmitTaskTrace({ TaskTraceEventID::Rejected, trace, traceTaskID,
            0, 0, 0, 0, domain, lane, 1 });
        return false;
    };
    if (!m_runtimeState || !scope.m_state ||
        !scope.m_state->accepting.load(std::memory_order_acquire)) return reject();
    {
        std::lock_guard lock(scope.m_state->mutex);
        if (!scope.m_state->accepting.load(std::memory_order_relaxed)) return reject();
        ++scope.m_state->outstanding;
    }
    std::uint64_t queuedDepth = 0;
    std::uint32_t activeCount = 0;
    {
        std::lock_guard lock(m_runtimeState->taskMutex);
        auto& runtime = m_runtimeState->domains[static_cast<std::size_t>(domain)];
        runtime.queues[static_cast<std::size_t>(lane)].push_back(
            { scope.m_state, lane, domain, std::string(name), std::move(body),
                std::chrono::steady_clock::now(), trace, traceTaskID });
        runtime.stats.queued = runtime.Queued();
        runtime.stats.highWatermark = std::max(runtime.stats.highWatermark, runtime.stats.queued);
        queuedDepth = runtime.stats.queued;
        activeCount = runtime.active;
    }
    if (tracing) EmitTaskTrace({ TaskTraceEventID::Queued, trace, traceTaskID,
        0, 0, queuedDepth, activeCount, domain, lane, 0 });
    DispatchDomain(domain);
    return true;
}

bool TaskSchedulerManager::Submit(TaskLane lane, TaskDomain domain, std::string_view name,
    std::function<void()>&& body, TaskTraceMetadata trace) {
    return Submit(ProcessScope(domain), lane, domain, name,
        [body = std::move(body)](const TaskContext&) mutable { body(); }, trace);
}

void TaskSchedulerManager::DispatchDomain(TaskDomain domain) {
    if (!m_runtimeState) return;
    std::vector<RuntimeState::PendingTask> launch;
    {
        std::lock_guard lock(m_runtimeState->taskMutex);
        auto& runtime = m_runtimeState->domains[static_cast<std::size_t>(domain)];
        while (runtime.active < runtime.limit && runtime.Queued() != 0) {
            // Graph-critical state producers must not sit behind a scene-load flood
            // of ordinary renderer mutations.  Bound the priority burst so lower
            // lanes still make deterministic progress under sustained critical work.
            constexpr std::uint32_t kMaximumCriticalBurst = 8;
            const auto critical = static_cast<std::size_t>(TaskLane::FrameCritical);
            const auto streaming = static_cast<std::size_t>(TaskLane::Streaming);
            const auto background = static_cast<std::size_t>(TaskLane::Background);
            std::size_t selected = background;
            if (!runtime.queues[critical].empty() &&
                (runtime.consecutiveFrameCritical < kMaximumCriticalBurst ||
                    (runtime.queues[streaming].empty() && runtime.queues[background].empty()))) {
                selected = critical;
                ++runtime.consecutiveFrameCritical;
            } else if (!runtime.queues[streaming].empty()) {
                selected = streaming;
                runtime.consecutiveFrameCritical = 0;
            } else if (!runtime.queues[background].empty()) {
                selected = background;
                runtime.consecutiveFrameCritical = 0;
            } else {
                selected = critical;
                ++runtime.consecutiveFrameCritical;
            }
            auto& queue = runtime.queues[selected];
            launch.push_back(std::move(queue.front())); queue.pop_front(); ++runtime.active;
            launch.back().admittedQueuedDepth = runtime.Queued();
            launch.back().admittedActiveCount = runtime.active;
        }
        runtime.stats.queued = runtime.Queued(); runtime.stats.active = runtime.active;
    }
    for (auto& task : launch) {
        if (task.traceTaskID != 0) EmitTaskTrace({ TaskTraceEventID::Admitted,
            task.trace, task.traceTaskID, 0, 0, task.admittedQueuedDepth,
            task.admittedActiveCount, task.domain, task.lane, 0 });
        SelectArena(*m_runtimeState, task.lane).enqueue([this, task = std::move(task)]() {
            const auto started = std::chrono::steady_clock::now();
            const auto queuedUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(started - task.queuedAt).count());
            if (task.traceTaskID != 0) EmitTaskTrace({ TaskTraceEventID::Started,
                task.trace, task.traceTaskID, queuedUs, 0, task.admittedQueuedDepth,
                task.admittedActiveCount, task.domain, task.lane, 0 });
            auto contextState = std::make_shared<TaskContext::State>(); contextState->scope = task.scope;
            TaskContext context(contextState); std::exception_ptr error;
            const bool cancelled = context.StopRequested();
            const bool prior = g_inSchedulerTask; g_inSchedulerTask = true;
            if (!cancelled) {
                TracyCZone(zone, 1); TracyCZoneName(zone, task.name.data(), task.name.size());
                try { task.body(context); } catch (...) { error = std::current_exception(); }
                TracyCZoneEnd(zone);
            }
            g_inSchedulerTask = prior;
			LogTaskException(task.name, task.domain, error);
            const auto elapsedUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
            if (task.lane != TaskLane::FrameCritical && elapsedUs > kLongTaskMicros) {
                LogLongTask(task.name, task.domain, elapsedUs);
            }
            CompleteScope(task.scope, error);
            {
                std::lock_guard lock(m_runtimeState->taskMutex);
                auto& runtime = m_runtimeState->domains[static_cast<std::size_t>(task.domain)];
                --runtime.active; runtime.stats.active = runtime.active;
                runtime.stats.queueWaitMicros += queuedUs;
                if (queuedUs > runtime.stats.maxQueueWaitMicros) {
                    runtime.stats.maxQueueWaitMicros = queuedUs;
                    runtime.stats.maxQueueWaitTask = task.name;
                }
                runtime.stats.executionMicros += elapsedUs;
                if (elapsedUs > runtime.stats.maxExecutionMicros) {
                    runtime.stats.maxExecutionMicros = elapsedUs;
                    runtime.stats.maxExecutionTask = task.name;
                }
                if (cancelled) ++runtime.stats.cancelled; else if (error) ++runtime.stats.failed; else ++runtime.stats.completed;
                if (task.lane != TaskLane::FrameCritical && elapsedUs > kLongTaskMicros) ++runtime.stats.longTasks;
            }
            if (task.traceTaskID != 0) EmitTaskTrace({
                cancelled ? TaskTraceEventID::Cancelled : TaskTraceEventID::Completed,
                task.trace, task.traceTaskID, queuedUs, elapsedUs, 0, 0,
                task.domain, task.lane, static_cast<std::uint8_t>(error ? 2 : 0) });
            DispatchDomain(task.domain);
        });
    }
}

bool TaskSchedulerManager::ScheduleAfter(const TaskScope& scope, std::chrono::steady_clock::duration delay,
    TaskLane lane, TaskDomain domain, std::string_view name, std::function<void(const TaskContext&)>&& body) {
    if (!m_runtimeState || !scope.m_state || !scope.m_state->accepting.load()) return false;
    { std::lock_guard lock(scope.m_state->mutex); ++scope.m_state->outstanding; ++scope.m_state->delayedOutstanding; }
    {
        std::lock_guard lock(m_runtimeState->timerMutex);
        TaskTraceMetadata trace{};
        std::uint64_t traceTaskID = 0;
        if (TaskTraceActive()) {
            trace.taskKind = StableTaskKind(name);
            traceTaskID = m_nextTraceTaskID.fetch_add(1, std::memory_order_relaxed);
        }
        m_runtimeState->timers.push({ std::chrono::steady_clock::now() + delay, ++m_runtimeState->nextTimer,
            { scope.m_state, lane, domain, std::string(name), std::move(body),
                std::chrono::steady_clock::now(), trace, traceTaskID } });
    }
    m_runtimeState->timerCv.notify_one(); return true;
}

bool TaskSchedulerManager::SubmitBlockingIo(const TaskScope& scope, TaskDomain domain, std::string_view name,
    std::function<void(const TaskContext&)>&& operation, TaskLane continuationLane,
    std::function<void(const TaskContext&)>&& continuation) {
    if (!m_runtimeState || !scope.m_state || !scope.m_state->accepting.load()) return false;
    { std::lock_guard lock(scope.m_state->mutex); ++scope.m_state->outstanding; }
    {
        std::lock_guard lock(m_runtimeState->blockingMutex);
        m_runtimeState->blockingQueue.push_back({ scope.m_state, domain, std::string(name), std::move(operation), continuationLane, std::move(continuation) });
    }
    m_runtimeState->blockingCv.notify_one(); return true;
}

void TaskSchedulerManager::ParallelForImpl(std::string_view, std::size_t count, std::function<void(std::size_t)>&& body) {
    if (count == 0) return;
    const auto parallel = [&] { oneapi::tbb::parallel_for(std::size_t{ 0 }, count, [&](std::size_t i) { body(i); }); };
    if (!m_runtimeState || count == 1) for (std::size_t i = 0; i < count; ++i) body(i);
    else if (g_inSchedulerTask) parallel();
    else m_runtimeState->frameArena->execute(parallel);
}

void TaskSchedulerManager::ParallelForLimitedImpl(std::string_view, std::size_t count,
    std::size_t maximumConcurrency, std::function<void(std::size_t)>&& body) {
    if (count == 0) return;
    const auto concurrency = (std::max)(std::size_t{ 1 },
        (std::min)({ maximumConcurrency, count, static_cast<std::size_t>((std::max)(1u, m_workerCount + 1u)) }));
    if (!m_runtimeState || concurrency == 1) {
        for (std::size_t i = 0; i < count; ++i) body(i);
        return;
    }
    oneapi::tbb::task_arena arena(static_cast<int>(concurrency), 1,
        oneapi::tbb::task_arena::priority::high);
    arena.execute([&] {
        oneapi::tbb::parallel_for(std::size_t{ 0 }, count, [&](std::size_t i) { body(i); });
    });
}

std::uint32_t TaskSchedulerManager::BlockingThreadCount() const noexcept {
    return m_runtimeState ? static_cast<std::uint32_t>(m_runtimeState->blockingThreads.size()) : 0;
}
std::uint32_t TaskSchedulerManager::DomainConcurrency(TaskDomain domain) const noexcept {
    return m_runtimeState ? m_runtimeState->domains[static_cast<std::size_t>(domain)].limit : 0;
}
TaskSchedulerManager::QueueStats TaskSchedulerManager::GetQueueStats() const {
    QueueStats result;
    if (!m_runtimeState) return result;
    {
        std::lock_guard lock(m_runtimeState->taskMutex);
        for (std::size_t i = 0; i < kDomainCount; ++i) result.domains[i] = m_runtimeState->domains[i].stats;
    }
    {
        std::lock_guard lock(m_runtimeState->blockingMutex);
        result.blockingQueued = static_cast<std::uint32_t>(m_runtimeState->blockingQueue.size());
        result.blockingActive = m_runtimeState->blockingActive;
    }
    const auto& asset = result.domains[static_cast<std::size_t>(TaskDomain::AssetImport)];
    const auto& texture = result.domains[static_cast<std::size_t>(TaskDomain::TextureProcessing)];
    const auto& shader = result.domains[static_cast<std::size_t>(TaskDomain::ShaderCompile)];
    const auto& cleanup = result.domains[static_cast<std::size_t>(TaskDomain::Cleanup)];
    result.ioQueued = result.blockingQueued + static_cast<std::uint32_t>(asset.queued);
    result.ioActive = result.blockingActive + static_cast<std::uint32_t>(asset.active);
    result.backgroundQueued = static_cast<std::uint32_t>(texture.queued + cleanup.queued);
    result.backgroundActive = static_cast<std::uint32_t>(texture.active + cleanup.active);
    result.shaderCompileQueued = static_cast<std::uint32_t>(shader.queued);
    result.shaderCompileActive = static_cast<std::uint32_t>(shader.active);
    return result;
}

bool TaskSchedulerManager::InstallTaskTraceSink(void* context,
    TaskTraceCallback callback) noexcept {
    if (!context || !callback) return false;
    void* expected = nullptr;
    m_taskTraceCallback.store(callback, std::memory_order_release);
    if (m_taskTraceContext.compare_exchange_strong(expected, context,
        std::memory_order_release, std::memory_order_acquire)) return true;
    if (expected != context) m_taskTraceCallback.store(nullptr, std::memory_order_release);
    return expected == context;
}

void TaskSchedulerManager::RemoveTaskTraceSink(void* context) noexcept {
    if (!context) return;
    void* expected = context;
    if (!m_taskTraceContext.compare_exchange_strong(expected, nullptr,
        std::memory_order_seq_cst, std::memory_order_acquire)) return;
    for (;;) {
        bool writerActive = false;
        for (auto* hazard = m_taskTraceHazards.load(std::memory_order_acquire);
            hazard; hazard = hazard->next) {
            if (hazard->context.load(std::memory_order_seq_cst) == context) {
                writerActive = true;
                break;
            }
        }
        if (!writerActive) break;
        std::this_thread::yield();
    }
    m_taskTraceCallback.store(nullptr, std::memory_order_release);
}

void TaskSchedulerManager::EmitTaskTrace(const TaskTraceEvent& event) noexcept {
    auto* hazard = ThreadTaskTraceHazard();
    for (;;) {
        auto* context = m_taskTraceContext.load(std::memory_order_acquire);
        if (!context) return;
        hazard->context.store(context, std::memory_order_seq_cst);
        if (context != m_taskTraceContext.load(std::memory_order_seq_cst)) {
            hazard->context.store(nullptr, std::memory_order_seq_cst);
            continue;
        }
        const auto callback = m_taskTraceCallback.load(std::memory_order_acquire);
        if (callback) callback(context, event);
        hazard->context.store(nullptr, std::memory_order_seq_cst);
        return;
    }
}

TaskSchedulerManager::TaskTraceHazardSlot*
TaskSchedulerManager::ThreadTaskTraceHazard() noexcept {
    struct CacheEntry {
        TaskSchedulerManager* owner = nullptr;
        TaskTraceHazardSlot* slot = nullptr;
    };
    static thread_local CacheEntry cache;
    if (cache.owner == this && cache.slot) return cache.slot;
    auto* slot = new TaskTraceHazardSlot();
    auto* head = m_taskTraceHazards.load(std::memory_order_relaxed);
    do { slot->next = head; }
    while (!m_taskTraceHazards.compare_exchange_weak(head, slot,
        std::memory_order_release, std::memory_order_relaxed));
    cache = { this, slot };
    return slot;
}

void TaskSchedulerManager::Cleanup() {
    if (!m_initialized.exchange(false) || !m_runtimeState) return;
    auto& state = *m_runtimeState;
    for (auto& scope : state.processScopes) TaskScope(scope).Cancel();
    state.stopping.store(true); state.timerCv.notify_all(); state.blockingCv.notify_all();
    if (state.timerThread.joinable()) state.timerThread.join();
    {
        std::lock_guard lock(state.timerMutex);
        while (!state.timers.empty()) {
			auto scope = state.timers.top().task.scope;
			{
				std::lock_guard scopeLock(scope->mutex);
				if (scope->delayedOutstanding > 0) {
					--scope->delayedOutstanding;
					if (scope->outstanding > 0) --scope->outstanding;
					if (scope->outstanding == 0) scope->cv.notify_all();
				}
			}
            state.timers.pop();
        }
    }
    for (auto& thread : state.blockingThreads) if (thread.joinable()) thread.join();
    for (auto& scope : state.processScopes) {
        try { TaskScope(scope).Wait(); }
        catch (const std::exception& ex) { spdlog::error("Scheduler task failed during cleanup: {}", ex.what()); }
    }
    state.observers = {};
    state.frameArena->terminate(); state.streamingArena->terminate(); state.backgroundArena->terminate();
    m_runtimeState.reset(); m_workerCount = 0;
    spdlog::info("Unified oneTBB scheduler shutdown complete");
}

} // namespace br
