#pragma once

// ScriptHost: the C++ entry point a host application (the demo, later SARP's renderer host) uses to run C# scenarios.
// It loads the .NET runtime in-process, hands the managed runtime the native API table (ScriptAbi.h), and forwards
// one tick per frame. See docs/scripting.md.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace br::scripting {

    enum class LogLevel : int32_t { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4 };

    enum class ScenarioState : int32_t {
        Idle = 0,
        Compiling = 1,
        Running = 2,
        Passed = 3,
        Failed = 4,
        CompileError = 5,
    };

    inline bool IsTerminal(ScenarioState state) { return state >= ScenarioState::Passed; }
    const char* ToString(ScenarioState state);

    struct ScriptHostOptions {
        // Holds BasicRenderer.Scripting.dll, its runtimeconfig.json and its dependencies. Empty: "scripting" beside
        // the running executable.
        std::filesystem::path runtimeDirectory;
        // Receives the runtime's and scripts' log lines. Empty: OutputDebugString and stderr.
        std::function<void(LogLevel, std::string_view)> logSink;
    };

    class ScriptHost {
    public:
        // Loads the .NET runtime and initializes the managed scripting runtime. The runtime can be loaded once per
        // process, so a second live ScriptHost is refused. Returns null and sets error on failure.
        static std::unique_ptr<ScriptHost> Create(const ScriptHostOptions& options, std::string& error);

        ~ScriptHost();
        ScriptHost(const ScriptHost&) = delete;
        ScriptHost& operator=(const ScriptHost&) = delete;

        // Starts compiling and running a scenario (asynchronous; see BRScriptRuntimeCallbacks::run_scenario).
        // sourcePath is a .cs file or a directory of them. An empty scenarioName selects the only scenario.
        bool RunScenario(const std::filesystem::path& sourcePath, std::string_view scenarioName, std::string& error,
            std::string_view optionsJson = {});

        // Once per host frame, before the renderer's update, on the thread that created the host.
        void Tick(uint64_t hostFrameIndex, double deltaSeconds);

        // Cancels the running scenario; it ends Failed ("stopped").
        void Stop();

        ScenarioState State() const;
        // The result document of the last scenario that reached a terminal state; empty before one has.
        const std::string& ResultJson() const;
        // Set when a script asked the application to exit.
        std::optional<int32_t> RequestedExitCode() const;

        // The conventional process exit code for a terminal state: 0 passed, 1 failed, 2 compile error.
        static int32_t ExitCodeFor(ScenarioState state);

    private:
        struct Impl;
        explicit ScriptHost(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> m_impl;
    };

} // namespace br::scripting
