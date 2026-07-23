#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Telemetry/SamplingControlServer.h"

#include <Windows.h>

#include <chrono>
#include <limits>
#include <stdexcept>

namespace br::telemetry::sampling {
namespace {

std::wstring NormalizePipeName(std::wstring name)
{
    constexpr std::wstring_view prefix = L"\\\\.\\pipe\\";
    if (!name.starts_with(prefix)) {
        name = std::wstring(prefix) + name;
    }
    return name;
}

bool WriteAll(HANDLE pipe, const std::string& bytes)
{
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD written = 0;
        const DWORD count = static_cast<DWORD>(
            (std::min)(
                bytes.size() - offset,
                static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
        if (!WriteFile(pipe, bytes.data() + offset, count, &written, nullptr) || written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

} // namespace

ControlServer::~ControlServer()
{
    Stop();
}

void ControlServer::Start(std::wstring pipeName)
{
    if (m_running.exchange(true)) {
        throw std::runtime_error("sampling control server is already running");
    }
    m_pipeName = NormalizePipeName(std::move(pipeName));
    m_stopRequested.store(false, std::memory_order_release);
    m_thread = std::thread([this] { Run(); });
}

void ControlServer::Stop()
{
    if (!m_running.load(std::memory_order_acquire)) {
        return;
    }
    m_stopRequested.store(true, std::memory_order_release);
    HANDLE wake = CreateFileW(
        m_pipeName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (wake != INVALID_HANDLE_VALUE) {
        CloseHandle(wake);
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_running.store(false, std::memory_order_release);
}

bool ControlServer::Running() const
{
    return m_running.load(std::memory_order_acquire);
}

std::vector<std::shared_ptr<ControlServer::Request>> ControlServer::DrainRequests()
{
    std::scoped_lock lock(m_requestsMutex);
    std::vector<std::shared_ptr<Request>> result;
    result.swap(m_requests);
    return result;
}

void ControlServer::Run()
{
    while (!m_stopRequested.load(std::memory_order_acquire)) {
        HANDLE pipe = CreateNamedPipeW(
            m_pipeName.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,
            64 * 1024,
            64 * 1024,
            0,
            nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            break;
        }
        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
            ? TRUE
            : GetLastError() == ERROR_PIPE_CONNECTED;
        if (connected && !m_stopRequested.load(std::memory_order_acquire)) {
            ServeConnection(pipe);
        }
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    m_running.store(false, std::memory_order_release);
}

void ControlServer::ServeConnection(void* rawPipe)
{
    HANDLE pipe = static_cast<HANDLE>(rawPipe);
    std::string pending;
    char buffer[4096];
    while (!m_stopRequested.load(std::memory_order_acquire)) {
        DWORD read = 0;
        if (!ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
            break;
        }
        pending.append(buffer, read);
        for (;;) {
            const size_t newline = pending.find('\n');
            if (newline == std::string::npos) {
                break;
            }
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!line.empty()) {
                QueueRequest(line, pipe);
            }
        }
    }
}

void ControlServer::QueueRequest(const std::string& line, void* rawPipe)
{
    HANDLE pipe = static_cast<HANDLE>(rawPipe);
    auto request = std::make_shared<Request>();
    try {
        request->document = nlohmann::json::parse(line);
    } catch (const std::exception& exception) {
        nlohmann::json response{
            { "ok", false },
            { "error", std::string("invalid JSON: ") + exception.what() }
        };
        WriteAll(pipe, response.dump() + "\n");
        return;
    }

    auto future = request->response.get_future();
    {
        std::scoped_lock lock(m_requestsMutex);
        m_requests.push_back(request);
    }
    while (!m_stopRequested.load(std::memory_order_acquire)) {
        if (future.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) {
            WriteAll(pipe, future.get().dump() + "\n");
            return;
        }
    }
}

} // namespace br::telemetry::sampling
