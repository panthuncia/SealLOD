#include "BasicRendererScripting/ScriptHost.h"

#include "BasicRendererScripting/ScriptAbi.h"
#include "BasicRendererScripting/ScriptBackend.h"
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
#include <exception>
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

        std::string_view View(const char* text) { return text ? std::string_view(text) : std::string_view(); }

        // Backend calls never let an exception cross into managed code.
        template <typename Fn>
        BRScriptStatus Guard(const char* entry, Fn&& fn)
        {
            try {
                t_lastError.clear();
                return fn();
            } catch (const std::exception& e) {
                return SetScriptError(BR_SCRIPT_E_INTERNAL, std::format("{}: {}", entry, e.what()));
            } catch (...) {
                return SetScriptError(BR_SCRIPT_E_INTERNAL, std::format("{}: unknown exception", entry));
            }
        }

    } // namespace

    BRScriptStatus SetScriptError(BRScriptStatus status, std::string message)
    {
        t_lastError = std::move(message);
        return status;
    }

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
        std::unique_ptr<ScriptBackend> backend;
        std::function<std::optional<std::string>()> failureProbe;
        BRScriptApiV1 api{};
        BRScriptRuntimeCallbacks callbacks{};
        ScenarioState state = ScenarioState::Idle;
        std::string resultJson;
        std::string commandReply;
        std::optional<int32_t> requestedExitCode;
        std::filesystem::path sourcePath;
        std::string scenarioName;
        std::string optionsJson;

        void Log(LogLevel level, std::string_view message) const { logSink(level, message); }

        static Impl& From(void* context) { return *static_cast<Impl*>(context); }
        static ScriptBackend& Backend(void* context) { return *From(context).backend; }

        static void BR_SCRIPT_CALL ApiLog(void* context, int32_t level, const char* message)
        {
            From(context).Log(static_cast<LogLevel>(std::clamp(level, 0, 4)), View(message));
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
            const auto previous = impl.state;
            impl.state = static_cast<ScenarioState>(std::clamp(state, BR_SCRIPT_STATE_IDLE, BR_SCRIPT_STATE_COMPILE_ERROR));
            if (IsTerminal(impl.state)) impl.resultJson = resultJson ? resultJson : "{}";
            try {
                if (impl.state == ScenarioState::Compiling && previous != ScenarioState::Compiling) impl.backend->OnScenarioStarting();
                if (IsTerminal(impl.state) && !IsTerminal(previous)) impl.backend->OnScenarioFinished();
            } catch (const std::exception& e) {
                impl.Log(LogLevel::Error, std::format("script backend: {}", e.what()));
            }
        }

        static void BR_SCRIPT_CALL ApiRequestExit(void* context, int32_t exitCode) { From(context).requestedExitCode = exitCode; }

        static const void* BR_SCRIPT_CALL ApiGetExtension(void*, const char*, uint32_t)
        {
            // No host extension tables yet; host applications will register them here.
            return nullptr;
        }

        static BRScriptStatus BR_SCRIPT_CALL ApiModelInstantiate(void* context, const char* path, const char* name, const BRTransform* transform,
            BRScriptHandle* outHandle)
        {
            if (!path || !transform || !outHandle) return SetScriptError(BR_SCRIPT_E_INVALID_ARGUMENT, "model_instantiate: null argument");
            return Guard("model_instantiate", [&] { return Backend(context).InstantiateModel(View(path), View(name), *transform, *outHandle); });
        }
        static BRScriptStatus BR_SCRIPT_CALL ApiEntityGetState(void* context, BRScriptHandle handle, int32_t* outState)
        {
            if (!outState) return SetScriptError(BR_SCRIPT_E_INVALID_ARGUMENT, "entity_get_state: null argument");
            return Guard("entity_get_state", [&] { return Backend(context).GetEntityState(handle, *outState); });
        }
        static BRScriptStatus BR_SCRIPT_CALL ApiEntityDestroy(void* context, BRScriptHandle handle)
        {
            return Guard("entity_destroy", [&] { return Backend(context).DestroyEntity(handle); });
        }
        static BRScriptStatus BR_SCRIPT_CALL ApiEntitySetTransform(void* context, BRScriptHandle handle, const BRTransform* transform)
        {
            if (!transform) return SetScriptError(BR_SCRIPT_E_INVALID_ARGUMENT, "entity_set_transform: null argument");
            return Guard("entity_set_transform", [&] { return Backend(context).SetTransform(handle, *transform); });
        }
        static BRScriptStatus BR_SCRIPT_CALL ApiEntityGetTransform(void* context, BRScriptHandle handle, BRTransform* outTransform)
        {
            if (!outTransform) return SetScriptError(BR_SCRIPT_E_INVALID_ARGUMENT, "entity_get_transform: null argument");
            return Guard("entity_get_transform", [&] { return Backend(context).GetTransform(handle, *outTransform); });
        }
        static BRScriptStatus BR_SCRIPT_CALL ApiEntitySetParent(void* context, BRScriptHandle handle, BRScriptHandle parent)
        {
            return Guard("entity_set_parent", [&] { return Backend(context).SetParent(handle, parent); });
        }
        static BRScriptStatus BR_SCRIPT_CALL ApiEntityGetWorldMatrix(void* context, BRScriptHandle handle, float* outMatrix16)
        {
            if (!outMatrix16) return SetScriptError(BR_SCRIPT_E_INVALID_ARGUMENT, "entity_get_world_matrix: null argument");
            return Guard("entity_get_world_matrix", [&] { return Backend(context).GetWorldMatrix(handle, outMatrix16); });
        }
        static BRScriptStatus BR_SCRIPT_CALL ApiCameraGet(void* context, BRScriptHandle* outHandle)
        {
            if (!outHandle) return SetScriptError(BR_SCRIPT_E_INVALID_ARGUMENT, "camera_get: null argument");
            return Guard("camera_get", [&] { return Backend(context).GetCamera(*outHandle); });
        }
        static BRScriptStatus BR_SCRIPT_CALL ApiCameraSetProjection(void* context, float fovYRadians, float zNear, float zFar)
        {
            return Guard("camera_set_projection", [&] { return Backend(context).SetCameraProjection(fovYRadians, zNear, zFar); });
        }
        static BRScriptStatus BR_SCRIPT_CALL ApiCameraGetProjection(void* context, BRCameraProjection* outProjection)
        {
            if (!outProjection) return SetScriptError(BR_SCRIPT_E_INVALID_ARGUMENT, "camera_get_projection: null argument");
            return Guard("camera_get_projection", [&] { return Backend(context).GetCameraProjection(*outProjection); });
        }
        static BRScriptStatus BR_SCRIPT_CALL ApiCameraSetInputEnabled(void* context, int32_t enabled)
        {
            return Guard("camera_set_input_enabled", [&] { return Backend(context).SetCameraInputEnabled(enabled != 0); });
        }
        static BRScriptStatus BR_SCRIPT_CALL ApiLightCreate(void* context, const BRLightDesc* desc, const BRTransform* transform, const char* name,
            BRScriptHandle* outHandle)
        {
            if (!desc || !transform || !outHandle) return SetScriptError(BR_SCRIPT_E_INVALID_ARGUMENT, "light_create: null argument");
            return Guard("light_create", [&] { return Backend(context).CreateLight(*desc, *transform, View(name), *outHandle); });
        }
        static BRScriptStatus BR_SCRIPT_CALL ApiLightSet(void* context, BRScriptHandle handle, const BRLightDesc* desc)
        {
            if (!desc) return SetScriptError(BR_SCRIPT_E_INVALID_ARGUMENT, "light_set: null argument");
            return Guard("light_set", [&] { return Backend(context).SetLight(handle, *desc); });
        }
        static BRScriptStatus BR_SCRIPT_CALL ApiLightGet(void* context, BRScriptHandle handle, BRLightDesc* outDesc)
        {
            if (!outDesc) return SetScriptError(BR_SCRIPT_E_INVALID_ARGUMENT, "light_get: null argument");
            return Guard("light_get", [&] { return Backend(context).GetLight(handle, *outDesc); });
        }
        static BRScriptStatus BR_SCRIPT_CALL ApiSceneSetEnvironment(void* context, const char* name)
        {
            if (!name) return SetScriptError(BR_SCRIPT_E_INVALID_ARGUMENT, "scene_set_environment: null argument");
            return Guard("scene_set_environment", [&] { return Backend(context).SetEnvironment(View(name)); });
        }
        static BRScriptStatus BR_SCRIPT_CALL ApiSceneGetStatus(void* context, BRSceneStatus* outStatus)
        {
            if (!outStatus) return SetScriptError(BR_SCRIPT_E_INVALID_ARGUMENT, "scene_get_status: null argument");
            return Guard("scene_get_status", [&] { return Backend(context).GetSceneStatus(*outStatus); });
        }
        static BRScriptStatus BR_SCRIPT_CALL ApiSettingGet(void* context, const char* name, int32_t type, void* outValue)
        {
            if (!name || !outValue) return SetScriptError(BR_SCRIPT_E_INVALID_ARGUMENT, "setting_get: null argument");
            return Guard("setting_get", [&] { return Backend(context).GetSetting(View(name), type, outValue); });
        }
        static BRScriptStatus BR_SCRIPT_CALL ApiSettingSet(void* context, const char* name, int32_t type, const void* value)
        {
            if (!name || !value) return SetScriptError(BR_SCRIPT_E_INVALID_ARGUMENT, "setting_set: null argument");
            return Guard("setting_set", [&] { return Backend(context).SetSetting(View(name), type, value); });
        }
        static const char* BR_SCRIPT_CALL ApiCommandInvoke(void* context, const char* requestJson)
        {
            auto& impl = From(context);
            try {
                impl.commandReply = impl.backend->InvokeCommand(View(requestJson));
            } catch (const std::exception& e) {
                std::string message = e.what();
                std::replace(message.begin(), message.end(), '"', '\'');
                std::replace(message.begin(), message.end(), '\\', '/');
                impl.commandReply = std::format(R"({{"ok":false,"error":"command_invoke: {}"}})", message);
            }
            return impl.commandReply.c_str();
        }
    };

    ScriptHost::ScriptHost(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

    std::unique_ptr<ScriptHost> ScriptHost::Create(ScriptHostOptions options, std::string& error)
    {
        if (g_hostAlive.exchange(true)) {
            error = "a ScriptHost already exists in this process";
            return nullptr;
        }
        auto impl = std::make_unique<Impl>();
        impl->logSink = options.logSink ? std::move(options.logSink) : DefaultLogSink;
        impl->backend = options.backend ? std::move(options.backend) : std::make_unique<ScriptBackend>();
        impl->failureProbe = std::move(options.failureProbe);

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
        api.model_instantiate = &Impl::ApiModelInstantiate;
        api.entity_get_state = &Impl::ApiEntityGetState;
        api.entity_destroy = &Impl::ApiEntityDestroy;
        api.entity_set_transform = &Impl::ApiEntitySetTransform;
        api.entity_get_transform = &Impl::ApiEntityGetTransform;
        api.entity_set_parent = &Impl::ApiEntitySetParent;
        api.entity_get_world_matrix = &Impl::ApiEntityGetWorldMatrix;
        api.camera_get = &Impl::ApiCameraGet;
        api.camera_set_projection = &Impl::ApiCameraSetProjection;
        api.camera_get_projection = &Impl::ApiCameraGetProjection;
        api.camera_set_input_enabled = &Impl::ApiCameraSetInputEnabled;
        api.light_create = &Impl::ApiLightCreate;
        api.light_set = &Impl::ApiLightSet;
        api.light_get = &Impl::ApiLightGet;
        api.scene_set_environment = &Impl::ApiSceneSetEnvironment;
        api.scene_get_status = &Impl::ApiSceneGetStatus;
        api.setting_get = &Impl::ApiSettingGet;
        api.setting_set = &Impl::ApiSettingSet;
        api.command_invoke = &Impl::ApiCommandInvoke;

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
        if (m_impl) {
            if (m_impl->callbacks.shutdown) m_impl->callbacks.shutdown();
            try {
                m_impl->backend->Shutdown();
            } catch (const std::exception& e) {
                m_impl->Log(LogLevel::Error, std::format("script backend shutdown: {}", e.what()));
            }
        }
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
        m_impl->sourcePath = absolute;
        m_impl->scenarioName = std::string(scenarioName);
        m_impl->optionsJson = std::string(optionsJson);
        const auto pathUtf8 = absolute.u8string();
        m_impl->resultJson.clear();
        m_impl->requestedExitCode.reset();
        const BRScriptStatus status = m_impl->callbacks.run_scenario(reinterpret_cast<const char*>(pathUtf8.c_str()), m_impl->scenarioName.c_str(),
            m_impl->optionsJson.empty() ? nullptr : m_impl->optionsJson.c_str());
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

    bool ScriptHost::Reload(std::string& error)
    {
        if (m_impl->sourcePath.empty()) {
            error = "no scenario has been run";
            return false;
        }
        if (IsBusy(m_impl->state)) Stop("reloading");
        const auto source = m_impl->sourcePath;
        const auto name = m_impl->scenarioName;
        const auto options = m_impl->optionsJson;
        return RunScenario(source, name, error, options);
    }

    void ScriptHost::Tick(uint64_t hostFrameIndex, double deltaSeconds)
    {
        if (m_impl->failureProbe && m_impl->state == ScenarioState::Running) {
            if (auto failure = m_impl->failureProbe()) {
                Stop(*failure);
                return;
            }
        }
        m_impl->callbacks.tick(hostFrameIndex, deltaSeconds);
    }

    void ScriptHost::Stop(std::string_view reason)
    {
        const std::string text(reason);
        m_impl->callbacks.stop(text.c_str());
    }

    ScenarioState ScriptHost::State() const { return m_impl->state; }

    const std::string& ScriptHost::ResultJson() const { return m_impl->resultJson; }

    const std::filesystem::path& ScriptHost::SourcePath() const { return m_impl->sourcePath; }

    const std::string& ScriptHost::ScenarioName() const { return m_impl->scenarioName; }

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
