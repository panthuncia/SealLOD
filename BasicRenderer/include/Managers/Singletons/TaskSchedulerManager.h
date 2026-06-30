#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <future>
#include <mutex>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace br {

class TaskSchedulerManager {
public:
    static TaskSchedulerManager& GetInstance();

    void Initialize(uint32_t ioThreadCount = 2, uint32_t backgroundThreadCount = 0);
    void Cleanup();
    void RunIoTask(std::function<void()>&& task);
    void RunIoTask(std::string_view taskName, std::function<void()>&& task);
    void QueueIoTask(std::function<void()>&& task);
    void QueueIoTask(std::string_view taskName, std::function<void()>&& task);
    void RunBackgroundTask(std::function<void()>&& task);
    void RunBackgroundTask(std::string_view taskName, std::function<void()>&& task);
    void QueueShaderCompileTask(std::function<void()>&& task);
    void QueueShaderCompileTask(std::string_view taskName, std::function<void()>&& task);

    bool IsInitialized() const {
        return m_initialized;
    }

    uint32_t GetNumTaskThreads() const {
        return m_initialized ? m_workerThreadCount : 0u;
    }

    uint32_t GetNumIoThreads() const {
        return static_cast<uint32_t>(m_ioThreads.size());
    }

    uint32_t GetNumBackgroundThreads() const {
        return static_cast<uint32_t>(m_backgroundThreads.size());
    }

    uint32_t GetNumShaderCompileThreads() const {
        return static_cast<uint32_t>(m_shaderCompileThreads.size());
    }

    template <typename Func>
    void ParallelFor(size_t itemCount, Func&& func) {
        ParallelFor({}, itemCount, std::forward<Func>(func));
    }

    template <typename Func>
    void ParallelFor(std::string_view taskName, size_t itemCount, Func&& func) {
        using Callable = std::decay_t<Func>;
        Callable callable = std::forward<Func>(func);

        ParallelForImpl(taskName, itemCount, [callable = std::move(callable)](size_t itemIndex) mutable {
            callable(itemIndex);
        });
    }

private:
    struct RuntimeState;

    TaskSchedulerManager() = default;

    void IoWorkerLoop();
    void BackgroundWorkerLoop();
    void ShaderCompileWorkerLoop();
    void ParallelForImpl(std::string_view taskName, size_t itemCount, std::function<void(size_t)>&& func);

    std::unique_ptr<RuntimeState> m_runtimeState;
    std::vector<std::thread> m_ioThreads;
    std::vector<std::thread> m_backgroundThreads;
    std::vector<std::thread> m_shaderCompileThreads;
    std::deque<std::function<void()>> m_ioTasks;
    std::deque<std::function<void()>> m_backgroundTasks;
    std::deque<std::function<void()>> m_shaderCompileTasks;
    std::mutex m_ioMutex;
    std::mutex m_backgroundMutex;
    std::mutex m_shaderCompileMutex;
    std::condition_variable m_ioCv;
    std::condition_variable m_backgroundCv;
    std::condition_variable m_shaderCompileCv;
    std::atomic<uint32_t> m_ioRoundRobin = 0;
    std::atomic<bool> m_ioShutdownRequested = false;
    std::atomic<bool> m_backgroundShutdownRequested = false;
    std::atomic<bool> m_shaderCompileShutdownRequested = false;
    uint32_t m_workerThreadCount = 0;
    bool m_initialized = false;
};

}

using TaskSchedulerManager = br::TaskSchedulerManager;
