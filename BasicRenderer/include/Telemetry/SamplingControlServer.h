#pragma once

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace br::telemetry::sampling {

class ControlServer {
public:
    struct Request {
        nlohmann::json document;
        std::promise<nlohmann::json> response;
    };

    ControlServer() = default;
    ~ControlServer();
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    void Start(std::wstring pipeName);
    void Stop();
    bool Running() const;
    std::vector<std::shared_ptr<Request>> DrainRequests();
    const std::wstring& PipeName() const { return m_pipeName; }

private:
    void Run();
    void ServeConnection(void* pipe);
    void QueueRequest(const std::string& line, void* pipe);

    std::wstring m_pipeName;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_stopRequested{ false };
    std::thread m_thread;
    mutable std::mutex m_requestsMutex;
    std::vector<std::shared_ptr<Request>> m_requests;
};

} // namespace br::telemetry::sampling
