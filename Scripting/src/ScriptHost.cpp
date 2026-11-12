#include "BasicRendererScripting/ScriptHost.h"

#include "BasicRendererScripting/ScriptAbi.h"
#include "DotNetRuntime.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <format>

namespace br::scripting {

    namespace {

        constexpr const wchar_t* kRuntimeAssemblyName = L"BasicRenderer.Scripting.dll";
        constexpr const wchar_t* kRuntimeConfigName = L"BasicRenderer.Scripting.runtimeconfig.json";
        constexpr const wchar_t* kRuntimeType = L"BasicRenderer.Scripting.Runtime.ScriptRuntime, BasicRenderer.Scripting";
        constexpr const wchar_t* kRuntimeInitialize = L"Initialize";

        // CoreCLR can be loaded once per process, and the managed runtime's entry points are static.
        std::atomic<bool> g_hostAlive{ false };
        detail::DotNetRuntime g_runtime;

        thread_local std::string t_lastError;

        std::filesystem::path ExecutableDirectory()
        {
            std::wstring path(MAX_PATH, L'\0');
            for (;;) {
                const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
                if (length < path.size()) {
                    path.resize(length);
                    break;
                }
                path.resize(path.size() * 2);
            }
            return std::filesystem::path(path).parent_path();
        }

        void DefaultLogSink(LogLevel level, std::string_view message)
        {
            static constexpr const char* kNames[] = { "trace", "debug", "info", "warn", "error" };
            const auto index = std::clamp(static_cast<int32_t>(level), 0, 4);
            const std::string line = std::format("[script:{}] {}\n", kNames[index], message);
            OutputDebugStringA(line.c_str());
            std::fputs(line.c_str(), stderr);
        }

    } // namespace

    const char* ToString(ScenarioState state)
    {
        switch (state) {
        case ScenarioState::Idle: return "idle";
        case ScenarioState::Compiling: return "compiling";
        case ScenarioState::Running: return "running";
        case ScenarioState::Passed: return "passed";
        case ScenarioState::Failed: return "failed";
        case ScenarioState::CompileError: return "compile_error";
        }
        return "unknown";
    }

    struct ScriptHost::Impl {
        std::function<void(LogLevel, std::string_view)> logSink;
        BRScriptApiV1 api{};
        BRScriptRuntimeCallbacks callbacks{};
        ScenarioState state = ScenarioState::Idle;
        std::string resultJson;
        std::optional<int32_t> requestedExitCode;

        void Log(LogLevel level, std::string_view message) const { logSink(level, message); }

        static Impl& From(void* context) { return *static_cast<Impl*>(context); }

        static void BR_SCRIPT_CALL ApiLog(void* context, int32_t level, const char* message)
        {
            From(context).Log(static_cast<LogLevel>(std::clamp(level, 0, 4)), message ? message : "");
        }

        static int32_t BR_SCRIPT_CALL ApiGetLastError(void*, char* buffer, int32_t capacity)
        {
            if (buffer && capacity > 0) {
                const auto count = std::min<size_t>(t_lastError.size(), static_cast<size_t>(capacity - 1));
                std::memcpy(buffer, t_lastError.data(), count);
                buffer[count] = '\0';
            }
            return static_cast<int32_t>(t_lastError.size());
        }

        static void BR_SCRIPT_CALL ApiReportState(void* context, int32_t state, const char* resultJson)
        {
            auto& impl = From(context);
            impl.state = static_cast<ScenarioState>(std::clamp(state, BR_SCRIPT_STATE_IDLE, BR_SCRIPT_STATE_COMPILE_ERROR));
            if (IsTerminal(impl.state)) {
                impl.resultJson = resultJson ? resultJson : "{}";
            }
        }

        static void BR_SCRIPT_CALL ApiRequestExit(void* context, int32_t exitCode) { From(context).requestedExitCode = exitCode; }

        static const void* BR_SCRIPT_CALL ApiGetExtension(void*, const char*, uint32_t)
        {
            // No host extension tables yet; host applications will register them here.
            return nullptr;
        }
    };

    ScriptHost::ScriptHost(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

    std::unique_ptr<ScriptHost> ScriptHost::Create(const ScriptHostOptions& options, std::string& error)
    {
        if (g_hostAlive.exchange(true)) {
            error = "a ScriptHost already exists in this process";
            return nullptr;
        }
        auto impl = std::make_unique<Impl>();
        impl->logSink = options.logSink ? options.logSink : DefaultLogSink;

        const auto directory = options.runtimeDirectory.empty() ? ExecutableDirectory() / "scripting" : options.runtimeDirectory;
        const auto fail = [&](std::string message) -> std::unique_ptr<ScriptHost> {
            error = std::move(message);
            // The runtime itself stays loaded (it cannot be unloaded), but a later Create may retry initialization.
            g_hostAlive.store(false);
            return nullptr;
        };
        if (!g_runtime.Initialize(directory / kRuntimeConfigName, error)) return fail(error);
        const auto initialize = reinterpret_cast<BRScriptRuntimeInitializeFn>(
            g_runtime.GetUnmanagedEntryPoint(directory / kRuntimeAssemblyName, kRuntimeType, kRuntimeInitialize, error));
        if (!initialize) return fail(error);

        auto& api = impl->api;
        api.structSize = sizeof(BRScriptApiV1);
        api.version = BR_SCRIPT_ABI_VERSION;
        api.vec3Size = sizeof(BRVec3);
        api.quatSize = sizeof(BRQuat);
        api.transformSize = sizeof(BRTransform);
        api.context = impl.get();
        api.log = &Impl::ApiLog;
        api.get_last_error = &Impl::ApiGetLastError;
        api.report_state = &Impl::ApiReportState;
        api.request_exit = &Impl::ApiRequestExit;
        api.get_extension = &Impl::ApiGetExtension;

        impl->callbacks.structSize = sizeof(BRScriptRuntimeCallbacks);
        if (const BRScriptStatus status = initialize(&impl->api, &impl->callbacks); status != BR_SCRIPT_OK) {
            return fail(std::format("the managed scripting runtime refused to initialize ({}); see the log", status));
        }
        if (!impl->callbacks.run_scenario || !impl->callbacks.tick || !impl->callbacks.stop || !impl->callbacks.shutdown) {
            return fail("the managed scripting runtime did not provide its entry points");
        }
        if (impl->callbacks.version != BR_SCRIPT_ABI_VERSION) {
            // Append-only tables make an older runtime safe to drive with the entries both sides know.
            impl->Log(LogLevel::Warn, std::format("managed scripting runtime ABI v{}, host ABI v{}", impl->callbacks.version, BR_SCRIPT_ABI_VERSION));
        }
        return std::unique_ptr<ScriptHost>(new ScriptHost(std::move(impl)));
    }

    ScriptHost::~ScriptHost()
    {
        if (m_impl && m_impl->callbacks.shutdown) m_impl->callbacks.shutdown();
        g_hostAlive.store(false);
    }

    bool ScriptHost::RunScenario(const std::filesystem::path& sourcePath, std::string_view scenarioName, std::string& error,
        std::string_view optionsJson)
    {
        std::error_code ec;
        const auto absolute = std::filesystem::absolute(sourcePath, ec);
        if (ec || !std::filesystem::exists(absolute)) {
            error = std::format("script source not found: {}", sourcePath.string());
            return false;
        }
        const auto pathUtf8 = absolute.u8string();
        const std::string name(scenarioName);
        const std::string options(optionsJson);
        m_impl->resultJson.clear();
        const BRScriptStatus status = m_impl->callbacks.run_scenario(reinterpret_cast<const char*>(pathUtf8.c_str()), name.c_str(),
            options.empty() ? nullptr : options.c_str());
        if (status == BR_SCRIPT_E_BUSY) {
            error = "a scenario is already compiling or running";
            return false;
        }
        if (status != BR_SCRIPT_OK) {
            error = std::format("run_scenario failed ({})", status);
            return false;
        }
        return true;
    }

    void ScriptHost::Tick(uint64_t hostFrameIndex, double deltaSeconds) { m_impl->callbacks.tick(hostFrameIndex, deltaSeconds); }

    void ScriptHost::Stop() { m_impl->callbacks.stop(); }

    ScenarioState ScriptHost::State() const { return m_impl->state; }

    const std::string& ScriptHost::ResultJson() const { return m_impl->resultJson; }

    std::optional<int32_t> ScriptHost::RequestedExitCode() const { return m_impl->requestedExitCode; }

    int32_t ScriptHost::ExitCodeFor(ScenarioState state)
    {
        switch (state) {
        case ScenarioState::Passed: return 0;
        case ScenarioState::CompileError: return 2;
        default: return 1;
        }
    }

} // namespace br::scripting
