#include "Import/BRNiflyClient.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

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

struct ProcessResult {
    DWORD exitCode = 1;
    std::string stdoutText;
    std::string stderrText;
};

void ReadPipeToString(HANDLE readHandle, std::string& outText)
{
    char buffer[4096];
    DWORD bytesRead = 0;
    while (ReadFile(readHandle, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
        outText.append(buffer, buffer + bytesRead);
    }
}

std::optional<ProcessResult> RunProcessCapture(
    const std::string& executable,
    const std::vector<std::string>& arguments,
    int timeoutMilliseconds,
    std::string* errorMessage)
{
    SECURITY_ATTRIBUTES securityAttributes{};
    securityAttributes.nLength = sizeof(SECURITY_ATTRIBUTES);
    securityAttributes.bInheritHandle = TRUE;

    HANDLE stdoutRead = nullptr;
    HANDLE stdoutWrite = nullptr;
    HANDLE stderrRead = nullptr;
    HANDLE stderrWrite = nullptr;

    if (!CreatePipe(&stdoutRead, &stdoutWrite, &securityAttributes, 0) ||
        !SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&stderrRead, &stderrWrite, &securityAttributes, 0) ||
        !SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0))
    {
        if (errorMessage) {
            *errorMessage = "Failed to create BRNifly process pipes.";
        }
        if (stdoutRead) CloseHandle(stdoutRead);
        if (stdoutWrite) CloseHandle(stdoutWrite);
        if (stderrRead) CloseHandle(stderrRead);
        if (stderrWrite) CloseHandle(stderrWrite);
        return std::nullopt;
    }

    std::string commandLine = QuoteArgument(executable);
    for (const std::string& argument : arguments) {
        commandLine += " ";
        commandLine += QuoteArgument(argument);
    }

    STARTUPINFOA startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdOutput = stdoutWrite;
    startupInfo.hStdError = stderrWrite;
    startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION processInfo{};
    std::string mutableCommandLine = commandLine;
    std::vector<char> environmentBlock = BuildBRNiflyEnvironmentBlock();
    BOOL created = CreateProcessA(
        executable.c_str(),
        mutableCommandLine.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        environmentBlock.data(),
        fs::path(executable).parent_path().string().c_str(),
        &startupInfo,
        &processInfo);

    CloseHandle(stdoutWrite);
    CloseHandle(stderrWrite);

    if (!created) {
        if (errorMessage) {
            *errorMessage = "Failed to launch BRNifly executable: " + executable;
        }
        CloseHandle(stdoutRead);
        CloseHandle(stderrRead);
        return std::nullopt;
    }

    ProcessResult result{};
    std::string stdoutText;
    std::string stderrText;
    std::thread stdoutReader(ReadPipeToString, stdoutRead, std::ref(stdoutText));
    std::thread stderrReader(ReadPipeToString, stderrRead, std::ref(stderrText));

    const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, static_cast<DWORD>(timeoutMilliseconds));
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(processInfo.hProcess, 0xFFFF);
        if (stdoutReader.joinable()) {
            stdoutReader.join();
        }
        if (stderrReader.joinable()) {
            stderrReader.join();
        }
        if (errorMessage) {
            *errorMessage = "BRNifly timed out.";
        }
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        CloseHandle(stdoutRead);
        CloseHandle(stderrRead);
        return std::nullopt;
    }

    if (stdoutReader.joinable()) {
        stdoutReader.join();
    }
    if (stderrReader.joinable()) {
        stderrReader.join();
    }

    GetExitCodeProcess(processInfo.hProcess, &result.exitCode);
    result.stdoutText = std::move(stdoutText);
    result.stderrText = std::move(stderrText);

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    CloseHandle(stdoutRead);
    CloseHandle(stderrRead);
    return result;
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

std::optional<json> RunJsonCommand(
    const ClientOptions& options,
    const std::vector<std::string>& arguments,
    std::string* executablePath,
    std::string* errorMessage)
{
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

#ifdef _WIN32
    auto processResult = RunProcessCapture(*executable, arguments, options.timeoutMilliseconds, errorMessage);
    if (!processResult) {
        return std::nullopt;
    }
    if (processResult->exitCode != 0) {
        if (errorMessage) {
            *errorMessage = "BRNifly failed with exit code " + std::to_string(processResult->exitCode) + ": " + processResult->stderrText;
        }
        return std::nullopt;
    }

    try {
        return json::parse(processResult->stdoutText);
    }
    catch (const std::exception& ex) {
        if (errorMessage) {
            *errorMessage = std::string("BRNifly returned invalid JSON: ") + ex.what() + " stderr=" + processResult->stderrText;
        }
        return std::nullopt;
    }
#else
    if (errorMessage) {
        *errorMessage = "BRNifly process launching is currently implemented for Windows.";
    }
    return std::nullopt;
#endif
}

std::optional<json> RunJsonFileCommand(
    const ClientOptions& options,
    const std::string& nifPath,
    std::string* executablePath,
    std::string* errorMessage)
{
    static std::atomic<std::uint64_t> responseCounter{ 0 };
    const fs::path responsePath = fs::temp_directory_path() /
        ("brnifly_response_" +
            std::to_string(GetCurrentProcessId()) + "_" +
            std::to_string(GetCurrentThreadId()) + "_" +
            std::to_string(responseCounter.fetch_add(1, std::memory_order_relaxed)) + ".json");

    const std::string resolvedNifPath = ResolveInputFilePath(nifPath);
    auto envelope = RunJsonCommand(options, { "--convert-usd-json-file", resolvedNifPath, responsePath.string() }, executablePath, errorMessage);
    if (!envelope) {
        return std::nullopt;
    }

    std::ifstream input(responsePath, std::ios::binary);
    if (!input) {
        if (errorMessage) {
            *errorMessage = "BRNifly did not write its response file: " + responsePath.string();
        }
        return std::nullopt;
    }

    std::ostringstream contents;
    contents << input.rdbuf();
    std::error_code ec;
    fs::remove(responsePath, ec);

    try {
        return json::parse(contents.str());
    }
    catch (const std::exception& ex) {
        if (errorMessage) {
            *errorMessage = std::string("BRNifly response file contained invalid JSON: ") + ex.what();
        }
        return std::nullopt;
    }
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
    auto response = RunJsonCommand(options, { "--describe-services" }, &executablePath, errorMessage);
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
    auto response = RunJsonFileCommand(options, nifPath, &executablePath, errorMessage);
    if (timingStats) {
        timingStats->convertProcessMs += ElapsedMs(convertBegin);
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
    package.rootLayerText = response->value("rootLayerText", "");
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
