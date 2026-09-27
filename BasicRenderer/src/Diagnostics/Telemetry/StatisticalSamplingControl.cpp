#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

std::wstring Widen(const std::string& value)
{
    if (value.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}

bool WriteAll(HANDLE pipe, const std::string& bytes)
{
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD written = 0;
        if (!WriteFile(pipe, bytes.data() + offset, static_cast<DWORD>(bytes.size() - offset), &written, nullptr) ||
            written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

bool SendRequest(
    const std::wstring& fullPipe,
    const nlohmann::json& request,
    nlohmann::json& response,
    std::string& error)
{
    if (!WaitNamedPipeW(fullPipe.c_str(), 10000)) {
        error = "sampling control pipe is unavailable";
        return false;
    }
    HANDLE pipe = CreateFileW(
        fullPipe.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        error = "failed to connect to sampling control pipe";
        return false;
    }

    const std::string line = request.dump() + "\n";
    if (!WriteAll(pipe, line)) {
        CloseHandle(pipe);
        error = "failed to write sampling control request";
        return false;
    }
    std::string responseLine;
    char buffer[4096];
    while (responseLine.find('\n') == std::string::npos) {
        DWORD read = 0;
        if (!ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
            CloseHandle(pipe);
            error = "sampling control connection closed before a response";
            return false;
        }
        responseLine.append(buffer, read);
    }
    CloseHandle(pipe);
    responseLine.resize(responseLine.find('\n'));
    try {
        response = nlohmann::json::parse(responseLine);
    } catch (const std::exception& exception) {
        error = std::string("invalid sampling control response: ") + exception.what();
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    std::string pipeName = "BasicRenderer.Sampling";
    bool rawJson = false;
    bool asynchronous = false;
    std::string dataFile;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--pipe" && i + 1 < argc) {
            pipeName = argv[++i];
        } else if (argument == "--json") {
            rawJson = true;
        } else if (argument == "--async") {
            asynchronous = true;
        } else if (argument == "--data" && i + 1 < argc) {
            dataFile = argv[++i];
        } else {
            positional.push_back(argument);
        }
    }
    if (positional.empty()) {
        std::cerr << "usage: StatisticalSamplingControl [--pipe name] [--json] [--async] "
                     "[--data request.json] <command> [JSON object]\n";
        return 2;
    }

    nlohmann::json request{
        { "request_id", GetTickCount64() },
        { "command", positional[0] }
    };
    if (!dataFile.empty() || positional.size() > 1) {
        try {
            nlohmann::json arguments;
            if (!dataFile.empty()) {
                std::ifstream input(dataFile);
                if (!input) {
                    throw std::runtime_error("failed to open command data file");
                }
                input >> arguments;
            } else {
                arguments = nlohmann::json::parse(positional[1]);
            }
            if (!arguments.is_object()) {
                throw std::runtime_error("command arguments must be a JSON object");
            }
            request.update(arguments);
        } catch (const std::exception& exception) {
            std::cerr << exception.what() << "\n";
            return 2;
        }
    }

    std::wstring fullPipe = L"\\\\.\\pipe\\" + Widen(pipeName);
    nlohmann::json response;
    std::string error;
    if (!SendRequest(fullPipe, request, response, error)) {
        std::cerr << error << "\n";
        return 3;
    }

    const std::string command = positional[0];
    if (response.value("ok", false) && !asynchronous &&
        (command == "pso.recompile" || command == "pso.activate")) {
        const uint64_t jobId = response.value("job_id", uint64_t{ 0 });
        while (jobId != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            nlohmann::json statusRequest{
                { "request_id", ++request["request_id"].get_ref<uint64_t&>() },
                { "command", "job.status" },
                { "job_id", jobId }
            };
            if (!SendRequest(fullPipe, statusRequest, response, error)) {
                std::cerr << error << "\n";
                return 3;
            }
            const std::string state = response.value("job", nlohmann::json::object()).value("state", "");
            if (state == "published" || state == "failed") {
                break;
            }
        }
    } else if (response.value("ok", false) && !asynchronous &&
        (command == "profile.run" || command == "profile.cancel")) {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            nlohmann::json statusRequest{
                { "request_id", ++request["request_id"].get_ref<uint64_t&>() },
                { "command", "profile.status" }
            };
            if (!SendRequest(fullPipe, statusRequest, response, error)) {
                std::cerr << error << "\n";
                return 3;
            }
            if (response.value("finalized", false)) {
                break;
            }
        }
    }

    if (rawJson) {
        std::cout << response.dump() << "\n";
    } else {
        std::cout << response.dump(2) << "\n";
    }
    const auto job = response.value("job", nlohmann::json::object());
    return response.value("ok", false) && job.value("state", "") != "failed" ? 0 : 1;
}
