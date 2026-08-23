#include "Import/BRNiflyClient.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <nlohmann/json.hpp>
#include <tracy/Tracy.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace BRNiflyClient {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

std::uint64_t ElapsedMs(std::chrono::steady_clock::time_point begin)
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count());
}

std::uint64_t JsonUInt64(const json& value, const char* name)
{
    if (!value.is_object()) {
        return 0;
    }
    const auto it = value.find(name);
    if (it == value.end() || !it->is_number_integer()) {
        return 0;
    }
    return static_cast<std::uint64_t>(std::max<std::int64_t>(0, it->get<std::int64_t>()));
}

void AccumulateChildTimings(TimingStats& stats, const json& timings)
{
    stats.childLoadNiflyApiMs += JsonUInt64(timings, "loadNiflyApiMs");
    stats.childNiflyLoadMs += JsonUInt64(timings, "niflyLoadMs");
    stats.childGetGameNameMs += JsonUInt64(timings, "getGameNameMs");
    stats.childReadNodesMs += JsonUInt64(timings, "readNodesMs");
    stats.childReadShapesMs += JsonUInt64(timings, "readShapesMs");
    stats.childReadExtraDataMs += JsonUInt64(timings, "readExtraDataMs");
    stats.childDestroyNifMs += JsonUInt64(timings, "destroyNifMs");
    stats.childConvertShapesToUsdMs += JsonUInt64(timings, "convertShapesToUsdMs");
    stats.childUsdExportToStringMs += JsonUInt64(timings, "usdExportToStringMs");
    stats.childHashAndResponseMs += JsonUInt64(timings, "hashAndResponseMs");
    stats.childSharedMemoryCreateMs += JsonUInt64(timings, "sharedMemoryCreateMs");
    stats.childJsonDumpMs += JsonUInt64(timings, "jsonDumpMs");
}

std::optional<std::string> ReadSharedMemoryString(const json& descriptor, std::string* errorMessage, TimingStats* timingStats)
{
    if (!descriptor.is_object()) {
        if (errorMessage) {
            *errorMessage = "BRNifly returned an invalid shared-memory descriptor.";
        }
        return std::nullopt;
    }

    const std::string name = descriptor.value("name", "");
    const std::uint64_t size = JsonUInt64(descriptor, "size");
    if (name.empty()) {
        if (errorMessage) {
            *errorMessage = "BRNifly returned a shared-memory descriptor without a name.";
        }
        return std::nullopt;
    }
    if (size > static_cast<std::uint64_t>(std::string{}.max_size())) {
        if (errorMessage) {
            *errorMessage = "BRNifly returned a shared-memory payload that is too large.";
        }
        return std::nullopt;
    }

    const auto begin = std::chrono::steady_clock::now();
    try {
        boost::interprocess::shared_memory_object sharedMemory(
            boost::interprocess::open_only,
            name.c_str(),
            boost::interprocess::read_only);
        boost::interprocess::mapped_region region(sharedMemory, boost::interprocess::read_only);
        if (region.get_size() < size) {
            if (errorMessage) {
                *errorMessage = "BRNifly shared-memory payload is smaller than advertised.";
            }
            return std::nullopt;
        }
        std::string result(static_cast<std::size_t>(size), '\0');
        std::memcpy(result.data(), region.get_address(), static_cast<std::size_t>(size));
        if (timingStats) {
            timingStats->clientSharedMemoryReadMs += ElapsedMs(begin);
        }
        return result;
    }
    catch (const std::exception& ex) {
        if (errorMessage) {
            *errorMessage = std::string("Failed to read BRNifly shared-memory payload: ") + ex.what();
        }
        return std::nullopt;
    }
}

std::string QuoteArgument(const std::string& arg)
{
    std::string quoted = "\"";
    for (char ch : arg) {
        if (ch == '\"') {
            quoted += "\\\"";
        }
        else {
            quoted += ch;
        }
    }
    quoted += "\"";
    return quoted;
}

#ifdef _WIN32
std::optional<std::string> GetEnvironmentString(const char* name)
{
    DWORD size = GetEnvironmentVariableA(name, nullptr, 0);
    if (size == 0) {
        return std::nullopt;
    }

    std::string value(size, '\0');
    DWORD written = GetEnvironmentVariableA(name, value.data(), size);
    if (written == 0) {
        return std::nullopt;
    }
    value.resize(written);
    return value;
}

std::optional<std::string> GetCurrentExecutableDirectory()
{
    std::string buffer(MAX_PATH, '\0');
    DWORD size = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0) {
        return std::nullopt;
    }
    while (size == buffer.size()) {
        buffer.resize(buffer.size() * 2);
        size = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0) {
            return std::nullopt;
        }
    }
    buffer.resize(size);
    return fs::path(buffer).parent_path().string();
}

bool EnvironmentNameEquals(std::string_view entry, std::string_view name)
{
    const auto equals = entry.find('=');
    if (equals == std::string_view::npos || equals != name.size()) {
        return false;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        const auto left = static_cast<unsigned char>(entry[i]);
        const auto right = static_cast<unsigned char>(name[i]);
        if (std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }
    return true;
}

std::string MakeBRNiflyAsanOptions()
{
    std::vector<std::string> options;
    if (auto current = GetEnvironmentString("ASAN_OPTIONS")) {
        std::size_t begin = 0;
        while (begin <= current->size()) {
            const auto end = current->find(':', begin);
            const auto token = current->substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            if (!token.empty() && !token.starts_with("alloc_dealloc_mismatch=")) {
                options.push_back(token);
            }
            if (end == std::string::npos) {
                break;
            }
            begin = end + 1;
        }
    }
    options.push_back("alloc_dealloc_mismatch=0");

    std::string result;
    for (const auto& option : options) {
        if (!result.empty()) {
            result += ':';
        }
        result += option;
    }
    return result;
}

std::vector<char> BuildBRNiflyEnvironmentBlock()
{
    std::vector<std::string> entries;
    if (_environ) {
        for (char** cursor = _environ; *cursor; ++cursor) {
            std::string_view entry(*cursor);
            if (!EnvironmentNameEquals(entry, "ASAN_OPTIONS")) {
                entries.emplace_back(entry);
            }
        }
    }

    entries.push_back("ASAN_OPTIONS=" + MakeBRNiflyAsanOptions());
    std::sort(entries.begin(), entries.end(), [](const std::string& left, const std::string& right) {
        return _stricmp(left.c_str(), right.c_str()) < 0;
    });

    std::vector<char> block;
    for (const auto& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back('\0');
    }
    block.push_back('\0');
    return block;
}

void CloseHandleIfValid(HANDLE& handle)
{
    if (handle && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        handle = nullptr;
    }
}

class PersistentBRNiflyProcess
{
public:
    struct ResponseReadStats {
        std::uint64_t waitFirstByteMs = 0;
        std::uint64_t waitMoreResponseMs = 0;
        std::uint64_t readResponseMs = 0;
        std::uint64_t responseBytes = 0;
        std::uint64_t responseChunks = 0;
    };

    PersistentBRNiflyProcess(std::string executable, int timeoutMilliseconds, std::string* errorMessage, TimingStats* timingStats) :
        m_executable(std::move(executable)),
        m_timeoutMilliseconds(timeoutMilliseconds)
    {
        const auto begin = std::chrono::steady_clock::now();
        m_started = Start(errorMessage);
        if (timingStats) {
            timingStats->clientPersistentStartMs += ElapsedMs(begin);
        }
    }

    ~PersistentBRNiflyProcess()
    {
        Shutdown();
    }

    PersistentBRNiflyProcess(const PersistentBRNiflyProcess&) = delete;
    PersistentBRNiflyProcess& operator=(const PersistentBRNiflyProcess&) = delete;

    bool IsStarted() const
    {
        return m_started;
    }

    const std::string& Executable() const
    {
        return m_executable;
    }

    std::optional<json> SendRequest(const json& request, std::string* errorMessage, TimingStats* timingStats)
    {
        ZoneScopedN("BRNiflyClient::PersistentBRNiflyProcess::SendRequest");
        if (!m_started) {
            if (errorMessage) {
                *errorMessage = "Persistent BRNifly process is not running.";
            }
            return std::nullopt;
        }

        const std::string payload = request.dump() + "\n";
        {
            ZoneScopedN("BRNiflyClient::PersistentBRNiflyProcess::WriteRequest");
            const auto begin = std::chrono::steady_clock::now();
            DWORD bytesWritten = 0;
            const BOOL ok = WriteFile(m_stdinWrite, payload.data(), static_cast<DWORD>(payload.size()), &bytesWritten, nullptr);
            if (timingStats) {
                timingStats->clientWriteRequestMs += ElapsedMs(begin);
            }
            if (!ok || bytesWritten != payload.size()) {
                if (errorMessage) {
                    *errorMessage = "Failed to write request to persistent BRNifly process.";
                }
                return std::nullopt;
            }
        }

        std::string line;
        ResponseReadStats readStats;
        const auto waitBegin = std::chrono::steady_clock::now();
        {
            ZoneScopedN("BRNiflyClient::PersistentBRNiflyProcess::WaitResponse");
            if (!ReadStdoutLine(line, readStats, errorMessage)) {
                if (timingStats) {
                    timingStats->clientWaitResponseMs += ElapsedMs(waitBegin);
                    timingStats->clientWaitFirstByteMs += readStats.waitFirstByteMs;
                    timingStats->clientWaitMoreResponseMs += readStats.waitMoreResponseMs;
                    timingStats->clientReadResponseMs += readStats.readResponseMs;
                    timingStats->clientResponseBytes += readStats.responseBytes;
                    timingStats->clientResponseChunks += readStats.responseChunks;
                }
                return std::nullopt;
            }
        }
        if (timingStats) {
            timingStats->clientWaitResponseMs += ElapsedMs(waitBegin);
            timingStats->clientWaitFirstByteMs += readStats.waitFirstByteMs;
            timingStats->clientWaitMoreResponseMs += readStats.waitMoreResponseMs;
            timingStats->clientReadResponseMs += readStats.readResponseMs;
            timingStats->clientResponseBytes += readStats.responseBytes;
            timingStats->clientResponseChunks += readStats.responseChunks;
        }

        try {
            ZoneScopedN("BRNiflyClient::PersistentBRNiflyProcess::ParseJsonResponse");
            const auto begin = std::chrono::steady_clock::now();
            TracyPlot("BRNiflyClient.PersistentStdoutLineBytes", static_cast<int64_t>(line.size()));
            json parsed = json::parse(line);
            if (timingStats) {
                timingStats->clientParseJsonMs += ElapsedMs(begin);
            }
            return parsed;
        }
        catch (const std::exception& ex) {
            if (errorMessage) {
                *errorMessage = std::string("Persistent BRNifly returned invalid JSON: ") + ex.what();
            }
            return std::nullopt;
        }
    }

    void Shutdown()
    {
        if (m_stdinWrite) {
            json request;
            request["command"] = "shutdown";
            const std::string payload = request.dump() + "\n";
            DWORD bytesWritten = 0;
            (void)WriteFile(m_stdinWrite, payload.data(), static_cast<DWORD>(payload.size()), &bytesWritten, nullptr);
        }
        CloseHandleIfValid(m_stdinWrite);

        if (m_processInfo.hProcess) {
            const DWORD waitResult = WaitForSingleObject(m_processInfo.hProcess, 2000);
            if (waitResult == WAIT_TIMEOUT) {
                TerminateProcess(m_processInfo.hProcess, 0xFFFF);
                WaitForSingleObject(m_processInfo.hProcess, 2000);
            }
        }

        CloseHandleIfValid(m_stdoutRead);
        CloseHandleIfValid(m_processInfo.hThread);
        CloseHandleIfValid(m_processInfo.hProcess);
        m_started = false;
    }

private:
    bool Start(std::string* errorMessage)
    {
        ZoneScopedN("BRNiflyClient::PersistentBRNiflyProcess::Start");
        SECURITY_ATTRIBUTES securityAttributes{};
        securityAttributes.nLength = sizeof(SECURITY_ATTRIBUTES);
        securityAttributes.bInheritHandle = TRUE;

        HANDLE stdinRead = nullptr;
        HANDLE stdoutWrite = nullptr;
        HANDLE stderrWrite = CreateFileA(
            "NUL",
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &securityAttributes,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (stderrWrite == INVALID_HANDLE_VALUE) {
            stderrWrite = nullptr;
        }

        constexpr DWORD kStdoutPipeBufferSize = 1u << 20;
        if (!CreatePipe(&stdinRead, &m_stdinWrite, &securityAttributes, 0) ||
            !SetHandleInformation(m_stdinWrite, HANDLE_FLAG_INHERIT, 0) ||
            !CreatePipe(&m_stdoutRead, &stdoutWrite, &securityAttributes, kStdoutPipeBufferSize) ||
            !SetHandleInformation(m_stdoutRead, HANDLE_FLAG_INHERIT, 0))
        {
            if (errorMessage) {
                *errorMessage = "Failed to create persistent BRNifly process pipes.";
            }
            CloseHandleIfValid(stdinRead);
            CloseHandleIfValid(m_stdinWrite);
            CloseHandleIfValid(m_stdoutRead);
            CloseHandleIfValid(stdoutWrite);
            CloseHandleIfValid(stderrWrite);
            return false;
        }

        std::string commandLine = QuoteArgument(m_executable) + " --stdio-json";
        STARTUPINFOA startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESTDHANDLES;
        startupInfo.hStdInput = stdinRead;
        startupInfo.hStdOutput = stdoutWrite;
        startupInfo.hStdError = stderrWrite;

        std::string mutableCommandLine = commandLine;
        std::vector<char> environmentBlock = BuildBRNiflyEnvironmentBlock();
        const BOOL created = CreateProcessA(
            m_executable.c_str(),
            mutableCommandLine.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            environmentBlock.data(),
            fs::path(m_executable).parent_path().string().c_str(),
            &startupInfo,
            &m_processInfo);

        CloseHandleIfValid(stdinRead);
        CloseHandleIfValid(stdoutWrite);
        CloseHandleIfValid(stderrWrite);

        if (!created) {
            if (errorMessage) {
                *errorMessage = "Failed to launch persistent BRNifly executable: " + m_executable;
            }
            CloseHandleIfValid(m_stdinWrite);
            CloseHandleIfValid(m_stdoutRead);
            return false;
        }

        return true;
    }

    bool ReadStdoutLine(std::string& line, ResponseReadStats& stats, std::string* errorMessage)
    {
        const auto begin = std::chrono::steady_clock::now();
        bool sawResponseBytes = false;
        char buffer[65536];
        for (;;) {
            if (auto newline = m_stdoutPending.find('\n'); newline != std::string::npos) {
                line = m_stdoutPending.substr(0, newline);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                m_stdoutPending.erase(0, newline + 1);
                return true;
            }

            DWORD available = 0;
            if (!PeekNamedPipe(m_stdoutRead, nullptr, 0, nullptr, &available, nullptr)) {
                if (errorMessage) {
                    *errorMessage = "Persistent BRNifly stdout pipe closed before returning JSON.";
                }
                return false;
            }

            if (available > 0) {
                if (!sawResponseBytes) {
                    stats.waitFirstByteMs = ElapsedMs(begin);
                    sawResponseBytes = true;
                }

                const DWORD toRead = std::min<DWORD>(available, static_cast<DWORD>(sizeof(buffer)));
                DWORD bytesRead = 0;
                const auto readBegin = std::chrono::steady_clock::now();
                if (!ReadFile(m_stdoutRead, buffer, toRead, &bytesRead, nullptr) || bytesRead == 0) {
                    if (errorMessage) {
                        *errorMessage = "Failed to read persistent BRNifly JSON response.";
                    }
                    return false;
                }
                stats.readResponseMs += ElapsedMs(readBegin);
                stats.responseBytes += bytesRead;
                ++stats.responseChunks;
                m_stdoutPending.append(buffer, buffer + bytesRead);
                continue;
            }

            const DWORD waitResult = WaitForSingleObject(m_processInfo.hProcess, 0);
            if (waitResult == WAIT_OBJECT_0) {
                if (errorMessage) {
                    *errorMessage = "Persistent BRNifly process exited before returning JSON.";
                }
                return false;
            }

            if (ElapsedMs(begin) >= static_cast<std::uint64_t>(m_timeoutMilliseconds)) {
                TerminateProcess(m_processInfo.hProcess, 0xFFFF);
                if (errorMessage) {
                    *errorMessage = "Persistent BRNifly request timed out.";
                }
                return false;
            }

            if (sawResponseBytes) {
                const auto yieldBegin = std::chrono::steady_clock::now();
                SwitchToThread();
                stats.waitMoreResponseMs += ElapsedMs(yieldBegin);
            }
            else {
                Sleep(1);
            }
        }
    }

    std::string m_executable;
    int m_timeoutMilliseconds = 120000;
    bool m_started = false;
    PROCESS_INFORMATION m_processInfo{};
    HANDLE m_stdinWrite = nullptr;
    HANDLE m_stdoutRead = nullptr;
    std::string m_stdoutPending;
};

std::optional<json> RunJsonRequest(
    const ClientOptions& options,
    json request,
    std::string* executablePath,
    std::string* errorMessage,
    TimingStats* timingStats = nullptr)
{
    ZoneScopedN("BRNiflyClient::RunJsonRequest");
    auto executable = DiscoverExecutable(options);
    if (!executable) {
        if (errorMessage) {
            *errorMessage = "BRNifly executable was not found. Set BRNIFLY_EXE or place BRNifly.exe next to BasicRenderer/CLodCacheTool.";
        }
        return std::nullopt;
    }

    if (executablePath) {
        *executablePath = *executable;
    }

    thread_local std::unique_ptr<PersistentBRNiflyProcess> process;
    if (!process || process->Executable() != *executable || !process->IsStarted()) {
        process = std::make_unique<PersistentBRNiflyProcess>(*executable, options.timeoutMilliseconds, errorMessage, timingStats);
        if (!process->IsStarted()) {
            process.reset();
            return std::nullopt;
        }
    }

    auto response = process->SendRequest(request, errorMessage, timingStats);
    if (!response) {
        process.reset();
    }
    return response;
}
#else
std::optional<std::string> GetEnvironmentString(const char* name)
{
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return std::nullopt;
    }
    return std::string(value);
}

std::optional<json> RunJsonRequest(
    const ClientOptions&,
    json,
    std::string*,
    std::string* errorMessage,
    TimingStats* = nullptr)
{
    if (errorMessage) {
        *errorMessage = "BRNifly process launching is currently implemented for Windows.";
    }
    return std::nullopt;
}
#endif

std::optional<std::string> ExistingExecutable(const fs::path& candidate)
{
    std::error_code ec;
    if (fs::is_regular_file(candidate, ec)) {
        return fs::weakly_canonical(candidate, ec).string();
    }
    return std::nullopt;
}

std::string ResolveInputFilePath(const std::string& filePath)
{
    const fs::path input(filePath);
    std::error_code ec;

    if (input.is_absolute()) {
        fs::path resolved = fs::weakly_canonical(input, ec);
        return ec ? input.string() : resolved.string();
    }

    std::vector<fs::path> candidates;
    candidates.push_back(fs::current_path(ec) / input);

#ifdef _WIN32
    if (auto exeDir = GetCurrentExecutableDirectory()) {
        const fs::path base(*exeDir);
        candidates.push_back(base / input);
        candidates.push_back(base / "BasicRenderer" / input);
        candidates.push_back(base.parent_path() / input);
        candidates.push_back(base.parent_path() / "BasicRenderer" / input);
    }
#endif

    for (const fs::path& candidate : candidates) {
        ec.clear();
        if (fs::is_regular_file(candidate, ec)) {
            fs::path resolved = fs::weakly_canonical(candidate, ec);
            return ec ? candidate.string() : resolved.string();
        }
    }

    ec.clear();
    fs::path absolute = fs::absolute(input, ec);
    return ec ? filePath : absolute.string();
}

std::vector<std::string> JsonStringArray(const json& value)
{
    std::vector<std::string> result;
    if (!value.is_array()) {
        return result;
    }
    for (const auto& item : value) {
        if (item.is_string()) {
            result.push_back(item.get<std::string>());
        }
    }
    return result;
}

std::vector<Diagnostic> JsonDiagnostics(const json& value)
{
    std::vector<Diagnostic> diagnostics;
    if (!value.is_array()) {
        return diagnostics;
    }
    for (const auto& item : value) {
        Diagnostic diagnostic{};
        diagnostic.level = item.value("level", "info");
        diagnostic.message = item.value("message", "");
        diagnostics.push_back(std::move(diagnostic));
    }
    return diagnostics;
}

std::string FormatResponseError(const json& response)
{
    std::string message = response.value("message", "BRNifly conversion failed.");
    auto diagnostics = JsonDiagnostics(response["diagnostics"]);
    bool appendedNonInfoDiagnostic = false;
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.message.empty() || diagnostic.level == "info") {
            continue;
        }
        if (!message.empty()) {
            message += " ";
        }
        message += "[";
        message += diagnostic.level.empty() ? "info" : diagnostic.level;
        message += "] ";
        message += diagnostic.message;
        appendedNonInfoDiagnostic = true;
    }
    if (!appendedNonInfoDiagnostic) {
        for (const auto& diagnostic : diagnostics) {
            if (diagnostic.message.empty()) {
                continue;
            }
            if (!message.empty()) {
                message += " ";
            }
            message += "[";
            message += diagnostic.level.empty() ? "info" : diagnostic.level;
            message += "] ";
            message += diagnostic.message;
        }
    }
    return message.empty() ? std::string("BRNifly conversion failed.") : message;
}

} // namespace

std::optional<std::string> DiscoverExecutable(const ClientOptions& options)
{
    if (!options.executablePath.empty()) {
        if (auto found = ExistingExecutable(options.executablePath)) {
            return found;
        }
    }

    if (auto env = GetEnvironmentString("BRNIFLY_EXE")) {
        if (auto found = ExistingExecutable(*env)) {
            return found;
        }
    }

#ifdef _WIN32
    if (auto exeDir = GetCurrentExecutableDirectory()) {
        const fs::path base(*exeDir);
        const fs::path candidates[] = {
            base / "BRNifly.exe",
            base.parent_path() / "BRNifly" / "BRNifly.exe",
            base / ".." / "BRNifly" / "BRNifly.exe",
        };
        for (const fs::path& candidate : candidates) {
            if (auto found = ExistingExecutable(candidate)) {
                return found;
            }
        }
    }
#endif

    return std::nullopt;
}

std::optional<ServiceInfo> DescribeServices(const ClientOptions& options, std::string* errorMessage)
{
    std::string executablePath;
    auto response = RunJsonRequest(options, json{ { "command", "describe-services" } }, &executablePath, errorMessage);
    if (!response) {
        return std::nullopt;
    }

    ServiceInfo info{};
    info.executablePath = executablePath;
    info.protocolVersion = response->value("protocolVersion", "");
    info.niflyVersion = response->value("niflyVersion", "");
    info.openUsdVersion = response->value("openUsdVersion", "");
    info.services = JsonStringArray((*response)["services"]);
    info.supportedGames = JsonStringArray((*response)["supportedGames"]);
    info.diagnostics = JsonDiagnostics((*response)["diagnostics"]);
    return info;
}

std::optional<ServiceInfo> CachedDescribeServices(const ClientOptions& options, std::string* errorMessage, TimingStats* timingStats)
{
    ZoneScopedN("BRNiflyClient::CachedDescribeServices");
    static std::mutex cacheMutex;
    static std::unordered_map<std::string, ServiceInfo> servicesByExecutable;

    std::string executablePath;
    auto executable = DiscoverExecutable(options);
    if (!executable) {
        if (errorMessage) {
            *errorMessage = "BRNifly executable was not found. Set BRNIFLY_EXE or place BRNifly.exe next to BasicRenderer/CLodCacheTool.";
        }
        return std::nullopt;
    }
    executablePath = *executable;

    {
        std::scoped_lock lock(cacheMutex);
        auto it = servicesByExecutable.find(executablePath);
        if (it != servicesByExecutable.end()) {
            return it->second;
        }
    }

    ClientOptions resolvedOptions = options;
    resolvedOptions.executablePath = executablePath;
    const auto begin = std::chrono::steady_clock::now();
    auto services = DescribeServices(resolvedOptions, errorMessage);
    if (timingStats) {
        timingStats->describeServicesMs += ElapsedMs(begin);
    }
    if (!services) {
        return std::nullopt;
    }

    {
        std::scoped_lock lock(cacheMutex);
        auto [it, inserted] = servicesByExecutable.emplace(executablePath, *services);
        (void)inserted;
        return it->second;
    }
}

std::optional<UsdAssetPackage> ConvertNifToUsd(const std::string& nifPath, const ClientOptions& options, std::string* errorMessage, TimingStats* timingStats)
{
    ZoneScopedN("BRNiflyClient::ConvertNifToUsd");
    ZoneText(nifPath.data(), nifPath.size());
    auto services = CachedDescribeServices(options, errorMessage, timingStats);
    if (!services) {
        return std::nullopt;
    }

    const bool hasConvertService = std::find(services->services.begin(), services->services.end(), "nif.convert.usd") != services->services.end();
    if (!hasConvertService) {
        if (errorMessage) {
            *errorMessage = "BRNifly does not advertise nif.convert.usd.";
        }
        return std::nullopt;
    }

    std::string executablePath;
    const auto convertBegin = std::chrono::steady_clock::now();
    const std::string resolvedNifPath = ResolveInputFilePath(nifPath);
    auto response = RunJsonRequest(
        options,
        json{ { "command", "convert-usd" }, { "path", resolvedNifPath }, { "responseTransport", "shared-memory" } },
        &executablePath,
        errorMessage,
        timingStats);
    if (timingStats) {
        timingStats->convertProcessMs += ElapsedMs(convertBegin);
        if (response) {
            AccumulateChildTimings(*timingStats, (*response)["timings"]);
        }
    }
    if (!response) {
        return std::nullopt;
    }

    if (response->value("status", "") != "ok") {
        if (errorMessage) {
            *errorMessage = FormatResponseError(*response);
        }
        return std::nullopt;
    }

    UsdAssetPackage package{};
    package.sourcePath = response->value("sourcePath", nifPath);
    package.sourceIdentifier = response->value("sourceIdentifier", package.sourcePath);
    package.contentHash = response->value("contentHash", "");
    std::string sharedMemoryName;
    if (const auto sharedMemoryIt = response->find("rootLayerSharedMemory");
        sharedMemoryIt != response->end() && sharedMemoryIt->is_object())
    {
        sharedMemoryName = sharedMemoryIt->value("name", "");
        auto rootLayerText = ReadSharedMemoryString(*sharedMemoryIt, errorMessage, timingStats);
        if (!sharedMemoryName.empty()) {
            (void)RunJsonRequest(
                options,
                json{ { "command", "release-shared-memory" }, { "name", sharedMemoryName } },
                nullptr,
                nullptr,
                nullptr);
        }
        if (!rootLayerText) {
            return std::nullopt;
        }
        package.rootLayerText = std::move(*rootLayerText);
    }
    else {
        package.rootLayerText = response->value("rootLayerText", "");
    }
    package.dependencies = JsonStringArray((*response)["dependencies"]);
    package.textureSearchRoots = JsonStringArray((*response)["textureSearchRoots"]);
    package.diagnostics = JsonDiagnostics((*response)["diagnostics"]);
    if (package.rootLayerText.empty()) {
        if (errorMessage) {
            *errorMessage = "BRNifly returned an empty USD layer.";
        }
        return std::nullopt;
    }

    return package;
}

} // namespace BRNiflyClient
