// End-to-end test of the native host: loads CoreCLR through hostfxr, hands the managed runtime the ABI table, and runs
// real scenarios by ticking. CPU only, no device.
// Usage: ScriptHostTests <managed runtime dir> <scenarios dir> <fixtures dir>

#include "BasicRendererScripting/ScriptAbi.h"
#include "BasicRendererScripting/ScriptHost.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace {

    int g_failures = 0;

    void Expect(bool condition, const std::string& message)
    {
        if (!condition) {
            ++g_failures;
            std::printf("FAIL  %s\n", message.c_str());
        }
    }

    br::scripting::ScenarioState RunToEnd(br::scripting::ScriptHost& host, const std::filesystem::path& source, std::string_view scenario = {})
    {
        std::string error;
        if (!host.RunScenario(source, scenario, error)) {
            Expect(false, "RunScenario(" + source.string() + "): " + error);
            return host.State();
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        uint64_t frame = 0;
        while (!br::scripting::IsTerminal(host.State()) && std::chrono::steady_clock::now() < deadline) {
            host.Tick(frame++, 1.0 / 60.0);
            if (host.State() == br::scripting::ScenarioState::Compiling) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        Expect(br::scripting::IsTerminal(host.State()), source.string() + " did not finish within 60 s");
        return host.State();
    }

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "usage: ScriptHostTests <managed runtime dir> <scenarios dir> <fixtures dir>\n");
        return 100;
    }
    const std::filesystem::path runtimeDir = argv[1];
    const std::filesystem::path scenarios = argv[2];
    const std::filesystem::path fixtures = argv[3];

    br::scripting::ScriptHostOptions options;
    options.runtimeDirectory = runtimeDir;
    std::string logs;
    options.logSink = [&](br::scripting::LogLevel, std::string_view line) {
        logs.append(line);
        logs.push_back('\n');
    };

    std::string error;
    auto host = br::scripting::ScriptHost::Create(options, error);
    if (!host) {
        std::printf("FAIL  ScriptHost::Create: %s\n", error.c_str());
        return 1;
    }
    Expect(logs.find("scripting runtime ready") != std::string::npos, "the runtime did not log its handshake");

    {
        std::string second;
        Expect(br::scripting::ScriptHost::Create(options, second) == nullptr, "a second live ScriptHost was allowed");
    }

    using br::scripting::ScenarioState;
    const auto hello = RunToEnd(*host, scenarios / "Hello.cs");
    Expect(hello == ScenarioState::Passed, std::string("Hello.cs: ") + br::scripting::ToString(hello) + "\n" + host->ResultJson());
    Expect(host->ResultJson().find("\"status\": \"passed\"") != std::string::npos, "Hello.cs result document");
    Expect(logs.find("hello from C#") != std::string::npos, "the scenario's log line did not reach the host");
    Expect(br::scripting::ScriptHost::ExitCodeFor(hello) == 0, "exit code for passed");

    const auto compileError = RunToEnd(*host, fixtures / "CompileError.cs");
    Expect(compileError == ScenarioState::CompileError, std::string("CompileError.cs: ") + br::scripting::ToString(compileError));
    Expect(br::scripting::ScriptHost::ExitCodeFor(compileError) == 2, "exit code for compile_error");

    const auto throws = RunToEnd(*host, fixtures / "Throws.cs");
    Expect(throws == ScenarioState::Failed, std::string("Throws.cs: ") + br::scripting::ToString(throws));
    Expect(br::scripting::ScriptHost::ExitCodeFor(throws) == 1, "exit code for failed");

    // The host can run scenarios back to back; each one's code is unloaded when it ends.
    const auto again = RunToEnd(*host, scenarios / "Hello.cs");
    Expect(again == ScenarioState::Passed, "Hello.cs a second time");

    Expect(!host->RunScenario(fixtures / "DoesNotExist.cs", {}, error), "a missing source was accepted");

    host.reset();
    if (g_failures != 0) {
        std::printf("--- host log ---\n%s", logs.c_str());
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ScriptHostTests passed\n");
    return 0;
}
