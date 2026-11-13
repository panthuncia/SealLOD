// End-to-end test of the native host: loads CoreCLR through hostfxr, hands the managed runtime the ABI table, and runs
// real scenarios by ticking, against a fake ScriptBackend (no renderer, no device).
// Usage: ScriptHostTests <managed runtime dir> <scenarios dir> <fixtures dir>

#include "BasicRendererScripting/ScriptAbi.h"
#include "BasicRendererScripting/ScriptBackend.h"
#include "BasicRendererScripting/ScriptHost.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
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

    // Predictable answers for every scene entry, checked by fixtures/ApiRoundTrip.cs.
    class FakeBackend final : public br::scripting::ScriptBackend {
    public:
        struct Entity {
            BRTransform transform{};
            BRScriptHandle parent = 0;
            bool light = false;
            BRLightDesc desc{};
        };

        std::map<BRScriptHandle, Entity> entities;
        BRScriptHandle next = 0x100000001ull;
        BRScriptHandle camera = 0;
        BRCameraProjection projection{ 1.0f, 1.5f, 0.1f, 1000.0f };
        bool inputEnabled = true;
        std::string environment;
        std::map<std::string, std::pair<int32_t, uint64_t>> settings{ { "flag", { BR_SCRIPT_SETTING_BOOL, 0 } },
            { "scale", { BR_SCRIPT_SETTING_FLOAT, 0 } }, { "count", { BR_SCRIPT_SETTING_UINT32, 0 } } };
        int starting = 0;
        int finished = 0;

        void OnScenarioStarting() override
        {
            ++starting;
            entities.clear();
            camera = 0;
        }
        void OnScenarioFinished() override
        {
            ++finished;
            inputEnabled = true;
        }

        BRScriptStatus InstantiateModel(std::string_view path, std::string_view, const BRTransform& transform, BRScriptHandle& out) override
        {
            if (!std::filesystem::exists(std::filesystem::path(std::u8string(path.begin(), path.end())))) {
                return br::scripting::SetScriptError(BR_SCRIPT_E_NOT_FOUND, "model not found: " + std::string(path));
            }
            out = next++;
            entities[out].transform = transform;
            return BR_SCRIPT_OK;
        }
        Entity* Find(BRScriptHandle handle)
        {
            const auto found = entities.find(handle);
            return found == entities.end() ? nullptr : &found->second;
        }
        BRScriptStatus Stale() { return br::scripting::SetScriptError(BR_SCRIPT_E_STALE_HANDLE, "stale"); }
        BRScriptStatus GetEntityState(BRScriptHandle handle, int32_t& state) override
        {
            if (!Find(handle)) return Stale();
            state = BR_SCRIPT_ENTITY_READY;
            return BR_SCRIPT_OK;
        }
        BRScriptStatus DestroyEntity(BRScriptHandle handle) override { return entities.erase(handle) ? BR_SCRIPT_OK : Stale(); }
        BRScriptStatus SetTransform(BRScriptHandle handle, const BRTransform& t) override
        {
            if (auto* e = Find(handle)) {
                e->transform = t;
                return BR_SCRIPT_OK;
            }
            return Stale();
        }
        BRScriptStatus GetTransform(BRScriptHandle handle, BRTransform& t) override
        {
            if (auto* e = Find(handle)) {
                t = e->transform;
                return BR_SCRIPT_OK;
            }
            return Stale();
        }
        BRScriptStatus SetParent(BRScriptHandle handle, BRScriptHandle parent) override
        {
            if (auto* e = Find(handle)) {
                e->parent = parent;
                return BR_SCRIPT_OK;
            }
            return Stale();
        }
        BRScriptStatus GetWorldMatrix(BRScriptHandle handle, float* out) override
        {
            auto* e = Find(handle);
            if (!e) return Stale();
            const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, e->transform.position.x, e->transform.position.y, e->transform.position.z, 1 };
            std::memcpy(out, identity, sizeof(identity));
            return BR_SCRIPT_OK;
        }
        BRScriptStatus GetCamera(BRScriptHandle& out) override
        {
            if (!camera) {
                camera = next++;
                entities[camera].transform.rotation.w = 1;
            }
            out = camera;
            return BR_SCRIPT_OK;
        }
        BRScriptStatus SetCameraProjection(float fov, float zNear, float zFar) override
        {
            if (fov > 0) projection.fovYRadians = fov;
            if (zNear > 0) projection.zNear = zNear;
            if (zFar > 0) projection.zFar = zFar;
            return BR_SCRIPT_OK;
        }
        BRScriptStatus GetCameraProjection(BRCameraProjection& out) override
        {
            out = projection;
            return BR_SCRIPT_OK;
        }
        BRScriptStatus SetCameraInputEnabled(bool enabled) override
        {
            inputEnabled = enabled;
            return BR_SCRIPT_OK;
        }
        BRScriptStatus CreateLight(const BRLightDesc& desc, const BRTransform& t, std::string_view, BRScriptHandle& out) override
        {
            out = next++;
            auto& e = entities[out];
            e.transform = t;
            e.light = true;
            e.desc = desc;
            return BR_SCRIPT_OK;
        }
        BRScriptStatus SetLight(BRScriptHandle handle, const BRLightDesc& desc) override
        {
            auto* e = Find(handle);
            if (!e || !e->light) return Stale();
            const auto type = e->desc.type;
            e->desc = desc;
            e->desc.type = type;
            return BR_SCRIPT_OK;
        }
        BRScriptStatus GetLight(BRScriptHandle handle, BRLightDesc& out) override
        {
            auto* e = Find(handle);
            if (!e || !e->light) return Stale();
            out = e->desc;
            return BR_SCRIPT_OK;
        }
        BRScriptStatus SetEnvironment(std::string_view name) override
        {
            environment = name;
            return BR_SCRIPT_OK;
        }
        BRScriptStatus GetSceneStatus(BRSceneStatus& out) override
        {
            out = { 1234, 5, 0xABCDEF, 6, 7 };
            return BR_SCRIPT_OK;
        }
        BRScriptStatus GetSetting(std::string_view name, int32_t type, void* out) override
        {
            const auto found = settings.find(std::string(name));
            if (found == settings.end()) return br::scripting::SetScriptError(BR_SCRIPT_E_NOT_FOUND, "no setting");
            if (found->second.first != type) return br::scripting::SetScriptError(BR_SCRIPT_E_TYPE_MISMATCH, "wrong type");
            std::memcpy(out, &found->second.second, 4);
            return BR_SCRIPT_OK;
        }
        BRScriptStatus SetSetting(std::string_view name, int32_t type, const void* value) override
        {
            const auto found = settings.find(std::string(name));
            if (found == settings.end()) return br::scripting::SetScriptError(BR_SCRIPT_E_NOT_FOUND, "no setting");
            if (found->second.first != type) return br::scripting::SetScriptError(BR_SCRIPT_E_TYPE_MISMATCH, "wrong type");
            found->second.second = 0;
            std::memcpy(&found->second.second, value, 4);
            return BR_SCRIPT_OK;
        }
        std::string InvokeCommand(std::string_view request) override
        {
            return std::string(R"({"ok":true,"request":)") + std::string(request) + "}";
        }
    };

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

    std::string logs;
    auto makeOptions = [&]() {
        br::scripting::ScriptHostOptions options;
        options.runtimeDirectory = runtimeDir;
        options.logSink = [&](br::scripting::LogLevel, std::string_view line) {
            logs.append(line);
            logs.push_back('\n');
        };
        return options;
    };

    auto options = makeOptions();
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend& fake = *backend;
    options.backend = std::move(backend);
    bool failNext = false;
    options.failureProbe = [&]() -> std::optional<std::string> {
        if (!failNext) return std::nullopt;
        failNext = false;
        return "probe failure";
    };

    std::string error;
    auto host = br::scripting::ScriptHost::Create(std::move(options), error);
    if (!host) {
        std::printf("FAIL  ScriptHost::Create: %s\n", error.c_str());
        return 1;
    }
    Expect(logs.find("scripting runtime ready") != std::string::npos, "the runtime did not log its handshake");
    {
        std::string second;
        Expect(br::scripting::ScriptHost::Create(makeOptions(), second) == nullptr, "a second live ScriptHost was allowed");
    }

    using br::scripting::ScenarioState;
    const auto hello = RunToEnd(*host, scenarios / "Hello.cs");
    Expect(hello == ScenarioState::Passed, std::string("Hello.cs: ") + br::scripting::ToString(hello) + "\n" + host->ResultJson());
    Expect(host->ResultJson().find("\"status\": \"passed\"") != std::string::npos, "Hello.cs result document");
    Expect(logs.find("hello from C#") != std::string::npos, "the scenario's log line did not reach the host");
    Expect(fake.starting == 1 && fake.finished == 1, "the backend heard the scenario start and finish");

    const auto roundTrip = RunToEnd(*host, fixtures / "ApiRoundTrip.cs");
    Expect(roundTrip == ScenarioState::Passed, std::string("ApiRoundTrip.cs: ") + br::scripting::ToString(roundTrip) + "\n" + host->ResultJson());
    Expect(fake.environment == "sky", "scene_set_environment reached the backend");
    Expect(fake.inputEnabled, "camera input is restored when the scenario finishes");

    const auto compileError = RunToEnd(*host, fixtures / "CompileError.cs");
    Expect(compileError == ScenarioState::CompileError, std::string("CompileError.cs: ") + br::scripting::ToString(compileError));
    Expect(br::scripting::ScriptHost::ExitCodeFor(compileError) == 2, "exit code for compile_error");

    const auto throws = RunToEnd(*host, fixtures / "Throws.cs");
    Expect(throws == ScenarioState::Failed, std::string("Throws.cs: ") + br::scripting::ToString(throws));
    Expect(br::scripting::ScriptHost::ExitCodeFor(throws) == 1, "exit code for failed");

    // A failure probe (D3D12 validation in the demo) fails a running scenario with its message.
    {
        if (host->RunScenario(fixtures / "Timeout.cs", {}, error, R"({"timeout_seconds": 0})")) {
            for (int frame = 0; frame < 2000 && host->State() != ScenarioState::Running; ++frame) {
                host->Tick(frame, 1.0 / 60);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            failNext = true;
            host->Tick(0, 1.0 / 60);
            Expect(host->State() == ScenarioState::Failed && host->ResultJson().find("probe failure") != std::string::npos,
                "a failure probe fails the scenario with its message");
        } else {
            Expect(false, "RunScenario(Timeout.cs): " + error);
        }
    }

    // Reload runs the last scenario again (after stopping it if it is still going).
    if (host->RunScenario(fixtures / "Timeout.cs", {}, error, R"({"timeout_seconds": 0})")) {
        for (int frame = 0; frame < 2000 && host->State() != ScenarioState::Running; ++frame) {
            host->Tick(frame, 1.0 / 60);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const int startsBefore = fake.starting;
        Expect(host->Reload(error), "Reload: " + error);
        Expect(fake.starting == startsBefore + 1 && br::scripting::IsBusy(host->State()), "a reload starts the scenario again");
        host->Stop("done");
        Expect(host->State() == ScenarioState::Failed && host->ResultJson().find("done") != std::string::npos, "Stop ends it with the reason");
    }

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
