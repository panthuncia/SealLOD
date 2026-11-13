#include <iostream>
#include <Windows.h>
#include <windowsx.h>
#include <memory>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <imgui.h>
#include <random>
#include <cmath>
#include <thread>
#ifndef USE_PIX
#define USE_PIX 1
#endif
#include <pix3.h>
#include <stacktrace>
#include <sstream>      // ostringstream for formatting
#include <unordered_map>
#include <vector>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <set>
#include <nlohmann/json.hpp>
#include <format>
//#include <tracy/Tracy.hpp>
#if BASICRENDERER_HAS_SCRIPTING
#include <BasicRendererScripting/RendererScriptBackend.h>
#include <BasicRendererScripting/ScriptHost.h>
#include <atomic>
#endif
#include <BasicRenderer/Diagnostics/ControlCommandRegistry.h>

#include "BasicRenderer/Assets/Geometry/Mesh.h"
#include <BasicRenderer/Renderer.h>
#include "BasicRenderer/Assets/Material.h"
#include <BasicRenderer/Assets/MaterialFlags.h>
#include <BasicRenderer/Pipeline/PSOFlags.h>
#include <BasicRenderer/Diagnostics/OutputTypes.h>
#include <BasicRenderer/Pipeline/RendererSettings.h>
#include "BasicRenderer/Diagnostics/CLodTelemetry.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include <BasicRenderer/Diagnostics/NvPerfIntegration.h>
#include <BasicRenderer/Diagnostics/SamplingControlServer.h>
#include <BasicRenderer/Diagnostics/StatisticalSampler.h>
#include "Resources/Buffers/DynamicBufferBase.h"
#include "Render/RenderGraph/RenderGraph.h"
#include "Runtime/Device/DeviceManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include <BasicRenderer/StaticImport/ImportedAssetLibrary.h>
#include <BasicRenderer/Assets/Import/SceneManifest.h>
#include <BasicRenderer/StaticImport/StaticImportRuntime.h>
#include <rhi_interop_dx12.h>
#include <d3d12sdklayers.h>
#include <BasicRenderer/Extensions/RenderDeviceAccess.h>
#include <BasicRenderer/Extensions/SettingAccess.h>
#include <BasicRenderer/Diagnostics/PipelineControl.h>
#include <streambuf>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <DirectXPackedVector.h>
#include "Render/Runtime/IReadbackService.h"
#include "Render/Runtime/IStatisticsService.h"
#include "Resources/ReadbackRequest.h"

class spdlog_streambuf : public std::streambuf {
public:
    explicit spdlog_streambuf(std::shared_ptr<spdlog::logger> logger)
        : _logger(std::move(logger)) {
    }

protected:
    int_type overflow(int_type ch) override {
        if (traits_type::eq_int_type(ch, traits_type::eof()))
            return ch;
        char c = traits_type::to_char_type(ch);
        _buffer += c;
        if (c == '\n') {
            _logger->info(_buffer);
            _buffer.clear();
        }
        return ch;
    }

private:
    std::shared_ptr<spdlog::logger> _logger;
    std::string                     _buffer;
};

// Activate dedicated GPU on NVIDIA laptops with both integrated and dedicated GPUs
extern "C" {
    _declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
}

// Set Agility SDK version
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 722;}

extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D\\"; }

#pragma comment(lib, "WinPixEventRuntime.lib")

namespace {
    void ResetD3D12SmokeValidationMessages() {
        auto device = br::extensions::GetRenderDevice();
        ID3D12Device* nativeDevice = rhi::dx12::get_device(device);
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
        if (nativeDevice && SUCCEEDED(nativeDevice->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
            infoQueue->ClearStoredMessages();
        }
    }

    bool D3D12SmokeValidationFailed() {
        auto device = br::extensions::GetRenderDevice();
        ID3D12Device* nativeDevice = rhi::dx12::get_device(device);
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
        if (!nativeDevice || FAILED(nativeDevice->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
            spdlog::error("Pipeline replacement smoke test could not query the D3D12 InfoQueue.");
            return true;
        }

        bool failed = false;
        UINT64 matchedCount = 0;
        UINT64 loggedCount = 0;
        const UINT64 messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 index = 0; index < messageCount; ++index) {
            SIZE_T messageBytes = 0;
            if (FAILED(infoQueue->GetMessage(index, nullptr, &messageBytes)) || messageBytes == 0) continue;
            std::vector<std::byte> storage(messageBytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (FAILED(infoQueue->GetMessage(index, message, &messageBytes))) continue;

            const bool isClearMismatch =
                message->ID == D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE;
            const bool isError =
                message->Severity == D3D12_MESSAGE_SEVERITY_ERROR
                || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION;
            if (!isClearMismatch && !isError) continue;

            failed = true;
            ++matchedCount;
            if (loggedCount < 16) {
                ++loggedCount;
                spdlog::error(
                    "Pipeline replacement smoke D3D12 validation message: severity={} id={} {}",
                    static_cast<uint32_t>(message->Severity),
                    static_cast<uint32_t>(message->ID),
                    message->pDescription ? message->pDescription : "<no description>");
            }
        }
        if (matchedCount > loggedCount) {
            spdlog::error(
                "Pipeline replacement smoke suppressed {} additional matching D3D12 messages.",
                matchedCount - loggedCount);
        }
        return failed;
    }
}

namespace crashlog {

    inline std::terminate_handler g_prev = nullptr;

    static std::string StacktraceString() {
#if defined(__cpp_lib_stacktrace) && (__cpp_lib_stacktrace >= 202011L)
        try {
            std::ostringstream oss;
            oss << std::stacktrace::current();
            return oss.str();
        }
        catch (...) {
            return "(stacktrace capture failed)";
        }
#else
        return "(no <stacktrace> support in this build)";
#endif
    }

    [[noreturn]] void TerminateHandler() noexcept
    {
        // Best-effort logging; never let this handler throw.
        try {
            if (auto eptr = std::current_exception()) {
                try {
                    std::rethrow_exception(eptr);
                }
                catch (const std::exception& e) {
                    spdlog::critical("FATAL: uncaught exception: {}\nwhat(): {}",
                        typeid(e).name(), e.what());
                }
                catch (...) {
                    spdlog::critical("FATAL: uncaught non-std exception");
                }
            }
            else {
                spdlog::critical("FATAL: std::terminate called (no active exception)");
            }

            spdlog::critical("Stacktrace:\n{}", StacktraceString());

            // Force logs out
            spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& lg) { lg->flush(); });
            spdlog::shutdown();
        }
        catch (...) {
            // ?
            __debugbreak();
        }

        std::abort();
    }

    inline void InstallTerminateHandler()
    {
        g_prev = std::set_terminate(&TerminateHandler);
    }

}

Renderer renderer;
UINT default_x_res = 3840;
UINT default_y_res = 2160;

namespace {

DWORD_PTR PickHighestAllowedProcessorMask(DWORD_PTR processMask) {
    if (processMask == 0) {
        return 0;
    }

    DWORD_PTR selectedMask = 0;
    for (DWORD_PTR candidateMask = 1; candidateMask != 0; candidateMask <<= 1) {
        if ((processMask & candidateMask) != 0) {
            selectedMask = candidateMask;
        }
    }

    return selectedMask;
}

void ConfigureMainRenderThreadScheduling() {
    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) {
        spdlog::warn("Failed to query process affinity mask for main render thread: {}", GetLastError());
        return;
    }

    const DWORD_PTR renderThreadMask = PickHighestAllowedProcessorMask(processMask);
    if (renderThreadMask == 0) {
        spdlog::warn("Could not choose a processor affinity mask for the main render thread");
        return;
    }

    if (SetThreadAffinityMask(GetCurrentThread(), renderThreadMask) == 0) {
        spdlog::warn("Failed to set main render thread affinity mask {:#x}: {}", renderThreadMask, GetLastError());
    }
    else {
        spdlog::info("Pinned main render thread to affinity mask {:#x}", renderThreadMask);
    }

    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL)) {
        spdlog::warn("Failed to set main render thread priority above normal: {}", GetLastError());
    }
    else {
        spdlog::info("Set main render thread priority to above normal");
    }
}

bool IsRendererInputMessage(UINT message) {
    switch (message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MOUSEMOVE:
    case WM_MOUSEWHEEL:
    case WM_INPUT:
        return true;
    default:
        return false;
    }
}

bool IsRendererKeyboardInputMessage(UINT message) {
    switch (message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
        return true;
    default:
        return false;
    }
}

bool IsRendererMouseReleaseMessage(UINT message) {
    switch (message) {
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        return true;
    default:
        return false;
    }
}

bool ShouldBlockRendererInputForImGui(UINT message) {
    if (ImGui::GetCurrentContext() == nullptr) {
        return false;
    }

    const ImGuiIO& io = ImGui::GetIO();
    if (IsRendererKeyboardInputMessage(message)) {
        return message != WM_KEYUP && message != WM_SYSKEYUP && io.WantCaptureKeyboard;
    }

    if (IsRendererMouseReleaseMessage(message)) {
        return false;
    }

    // ImGui computes this from the previous frame specifically so the host
    // application can decide where to dispatch mouse input.  Hover/active
    // queries are broader than capture intent and caused the renderer camera
    // to lose mouse input whenever the cursor happened to be over a menu.
    return io.WantCaptureMouse;
}

}


void ProcessRawInput(LPARAM lParam) {
    UINT dwSize = 0;

    // Get the size of the raw input data
    GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &dwSize, sizeof(RAWINPUTHEADER));

    // Allocate memory for the raw input data
    LPBYTE lpb = new BYTE[dwSize];
    if (lpb == nullptr) {
        return;
    }

    // Get the raw input data
    if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, lpb, &dwSize, sizeof(RAWINPUTHEADER)) != dwSize) {
        std::cerr << "GetRawInputData does not return correct size!" << std::endl;
    }

    RAWINPUT* raw = (RAWINPUT*)lpb;

    if (raw->header.dwType == RIM_TYPEKEYBOARD) {
        // Process keyboard input
        RAWKEYBOARD& rawKB = raw->data.keyboard;
        //std::cout << "Virtual key: " << rawKB.VKey << ", Scan code: " << rawKB.MakeCode << std::endl;

        // Check if the escape key is pressed
        if (rawKB.VKey == VK_ESCAPE) {
            PostQuitMessage(0); // Exit the application
        }

    }
    else if (raw->header.dwType == RIM_TYPEMOUSE) {
        // Process mouse input
        RAWMOUSE& rawMouse = raw->data.mouse;
    }

    delete[] lpb;
}

// Window callback procedure
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

// --no-input: the renderer takes no keyboard or mouse input, so the camera stays where it starts. Raw input is registered
// with RIDEV_INPUTSINK, which delivers it while other windows have focus: unattended runs (measurements, captures) would
// otherwise follow whatever the user does elsewhere.
bool g_rendererInputDisabled = false;

void RegisterRawInputDevices(HWND hwnd) {
    RAWINPUTDEVICE rid[2];

    // Register keyboard
    rid[0].usUsagePage = 0x01;
    rid[0].usUsage = 0x06;
    rid[0].dwFlags = RIDEV_INPUTSINK; // Receive input even when not in focus
    rid[0].hwndTarget = hwnd;

    // Register mouse
    rid[1].usUsagePage = 0x01;
    rid[1].usUsage = 0x02;
    rid[1].dwFlags = RIDEV_INPUTSINK; // Receive input even when not in focus
    rid[1].hwndTarget = hwnd;

    if (!RegisterRawInputDevices(rid, 2, sizeof(rid[0]))) {
        MessageBox(nullptr, L"Failed to register raw input devices", L"Error", MB_OK);
        throw std::runtime_error("Failed to register raw input devices.");
    }
}


HWND InitWindow(HINSTANCE hInstance, int nCmdShow) {
    const wchar_t CLASS_NAME[] = L"DX12WindowClass";

    WNDCLASS wc = { };

    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;

    if (!RegisterClass(&wc)) {
        MessageBox(nullptr, L"Failed to register window class", L"Error", MB_OK);
        throw std::runtime_error("Failed to register window class.");
    }

    HWND hwnd = CreateWindowEx(
        0,
        CLASS_NAME,
        L"DirectX 12 Basic Renderer",
        WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, default_x_res, default_y_res,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (hwnd == nullptr) {
        MessageBox(nullptr, L"Failed to create window", L"Error", MB_OK);
        throw std::runtime_error("Failed to create window.");
    }

    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);

    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(hMon, &mi);

    // mi.rcMonitor is the *entire* display area, including taskbar‐covered parts
    int monX = mi.rcMonitor.left;
    int monY = mi.rcMonitor.top;
    int monWidth = mi.rcMonitor.right - mi.rcMonitor.left;
    int monHeight = mi.rcMonitor.bottom - mi.rcMonitor.top;

    SetWindowPos(
        hwnd,
        HWND_TOP,           // or HWND_TOPMOST if you want to stay above every other window
        monX, monY,        // top-left corner of the monitor
        monWidth, monHeight,  // exactly fill it
        0
    );

    ShowWindow(hwnd, nCmdShow);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    if (!g_rendererInputDisabled) RegisterRawInputDevices(hwnd);

    return hwnd;
}

struct point {
	float x, y, z;
};

point randomPointInSphere(float radius) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    float x, y, z, len2;
    do {
        x = dist(gen);
        y = dist(gen);
        z = dist(gen);
        len2 = x * x + y * y + z * z;
    } while (len2 > 1.0f); // Ensure the point is inside the unit sphere

    // Scale to desired radius
    x *= radius;
    y *= radius;
    z *= radius;

    return {x, y, z};
}

point getRandomPointInVolume(double xmin, double xmax, 
    double ymin, double ymax, 
    double zmin, double zmax)
{
    std::random_device rd;
    std::mt19937 gen(rd());

    std::uniform_real_distribution<float> distX(static_cast<float>(xmin), static_cast<float>(xmax));
    std::uniform_real_distribution<float> distY(static_cast<float>(ymin), static_cast<float>(ymax));
    std::uniform_real_distribution<float> distZ(static_cast<float>(zmin), static_cast<float>(zmax));

    point p;
    p.x = distX(gen);
    p.y = distY(gen);
    p.z = distZ(gen);
    return p;
}

float randomFloat(float min, float max) {
	static std::random_device rd;
	static std::mt19937 gen(rd());
	std::uniform_real_distribution<float> dist(min, max);
	return dist(gen);
}

namespace {

std::optional<std::filesystem::path> FindCommandLinePath(
    std::string_view commandLine,
    std::string_view option)
{
    const auto optionPosition = commandLine.find(option);
    if (optionPosition == std::string_view::npos) return std::nullopt;
    auto valuePosition = commandLine.find_first_not_of(" \t", optionPosition + option.size());
    if (valuePosition == std::string_view::npos) return std::nullopt;
    if (commandLine[valuePosition] == '"') {
        const auto end = commandLine.find('"', valuePosition + 1);
        if (end == std::string_view::npos) return std::nullopt;
        return std::filesystem::path(commandLine.substr(valuePosition + 1, end - valuePosition - 1));
    }
    const auto end = commandLine.find_first_of(" \t", valuePosition);
    return std::filesystem::path(commandLine.substr(valuePosition, end - valuePosition));
}

// A model file as it is, or a directory's scene caches.
std::vector<std::string> ExpandDemoModelPath(const std::string& path)
{
    std::error_code error;
    if (!std::filesystem::is_directory(path, error)) return { path };
    std::vector<std::string> manifests;
    for (const auto& manifest : br::import::FindSceneManifests(path)) manifests.push_back(manifest.string());
    if (manifests.empty()) spdlog::error("{}: the directory holds no scene cache (*.brscene.json)", path);
    return manifests;
}

// The demo's static scene: Scene Explorer's Load Model placements streamed through the
// static import runtime, which publishes their static parts into the renderer's scene the
// way SARP's LiveSceneBridge does. Each static part of a model is its own group, so a scene
// streams in node by node; models can be added while the runtime runs. Skinned parts are
// kept by the asset library for the dynamic scene. The runtime runs even with no models:
// it publishes the geometry of dynamic (scripted) renderables too.
struct DemoStaticScene {
    struct Model {
        std::string path;
        XMFLOAT4X4 world;
        // Seeds the model's group ids and is its batch key, so a model added twice places
        // two copies.
        std::uint64_t key = 0;
    };

    br::staticimport::StaticImportShared shared;
    br::staticimport::ImportedAssetLibrary assets;
    std::unique_ptr<br::staticimport::StaticImportRuntime> runtime;
    // Models whose parts are not listed yet.
    std::vector<Model> unplacedModels;
    std::uint32_t modelsAdded = 0;

    DemoStaticScene() {
        assets.SetSkinnedPartsCallback([](const std::string& key, const std::vector<br::import::RenderableAssetPart>& parts) {
            spdlog::info("Demo static scene: '{}' has {} skinned parts; they are not imported as statics", key, parts.size());
        });
    }

    ~DemoStaticScene() {
        if (runtime) runtime->Shutdown();
        runtime.reset();
        assets.Shutdown();
    }

    void AddModel(const std::string& path, FXMMATRIX world) {
        Model model{ path };
        XMStoreFloat4x4(&model.world, world);
        model.key = br::staticimport::recipe::HashString(std::to_string(++modelsAdded),
            br::staticimport::recipe::HashString("br-demo-model"));
        (void)assets.TryGetLoadedParts(path);
        unplacedModels.push_back(std::move(model));
    }

    // Adds a group per static part of every model whose parts are listed.
    void PlaceLoadedModels() {
        std::erase_if(unplacedModels, [&](const Model& model) {
            const auto parts = assets.TryGetLoadedParts(model.path);
            if (!parts) return false;
            std::vector<br::staticimport::StaticImportGroup> groups;
            groups.reserve(parts->size());
            for (const auto& part : *parts) {
                if (part.skinned) continue;
                auto& group = groups.emplace_back();
                group.assetKey = br::staticimport::ImportedAssetLibrary::MakePartAssetKey(model.path, part.index);
                group.groupID = br::staticimport::recipe::HashString(group.assetKey, model.key);
                group.batchKey = model.key;
                group.name = part.name.empty() ? std::filesystem::path(model.path).filename().string() : part.name;
                // A part instanced within its file (EXT_mesh_gpu_instancing) is one group of all its placements.
                if (part.instanceMatrices.empty()) {
                    XMStoreFloat4x4(&group.instanceWorldMatrices.emplace_back(),
                        XMLoadFloat4x4(&part.localMatrix) * XMLoadFloat4x4(&model.world));
                }
                group.instanceWorldMatrices.reserve(part.instanceMatrices.size());
                for (const auto& instance : part.instanceMatrices) {
                    XMStoreFloat4x4(&group.instanceWorldMatrices.emplace_back(),
                        XMLoadFloat4x4(&instance) * XMLoadFloat4x4(&model.world));
                }
            }
            spdlog::info("Demo static scene: '{}' placed {} of {} parts", model.path, groups.size(), parts->size());
            runtime->AddOrUpdateGroups(std::move(groups));
            return true;
        });
    }

    void Start(Renderer& renderer) {
        runtime = std::make_unique<br::staticimport::StaticImportRuntime>(
            shared, br::staticimport::StaticImportSettings{}, assets,
            br::staticimport::StaticImportRuntime::Hooks{});
        runtime->AttachServices(renderer.GetSceneIngestionServices().StaticScene());
    }

    void Tick(Renderer& renderer, unsigned int frameIndex) {
        if (!runtime) Start(renderer);
        PlaceLoadedModels();
        runtime->Tick();
        if (frameIndex % 100 == 0) {
            const auto stats = runtime->GetStatsSnapshot();
            spdlog::info("Demo static scene: frame={} live_static_objects={} applied_placements={} groups_imported={} pending_groups={}",
                frameIndex, runtime->LiveStaticObjectCount(),
                runtime->Counters().lastStaticAppliedPlacements, runtime->Counters().staticDirectGroupsImported,
                stats.pendingGroups);
            spdlog::info("Demo static scene: waiting(assets={} variants={} templates={} shader_variants={}) queued(templates={} shader_variants={} ready={}) dropped(unsupported={} stale={}) acks(published={} requeued={} dropped={})",
                stats.groupsWaitingAssets, stats.groupsWaitingVariants, stats.groupsWaitingTemplates, stats.groupsWaitingShaderVariants,
                stats.templateRequestsQueued, stats.shaderVariantRequestsQueued, stats.readyGroupsQueued,
                stats.unsupportedGroupsDropped, stats.staleGroupsDropped,
                stats.groupsAckPublished, stats.groupsAckRequeued, stats.groupsAckDropped);
            for (const auto& diagnostic : stats.pendingGroupDiagnostics) spdlog::info("Demo static scene: pending {}", diagnostic);
        }
    }
};

std::optional<std::string> FindCommandLineValue(std::string_view commandLine, std::string_view option)
{
    const auto path = FindCommandLinePath(commandLine, option);
    return path ? std::optional<std::string>(path->string()) : std::nullopt;
}

// Where an option appears as a whole token: "--script" does not match inside "--script-exit". npos when absent.
size_t FindCommandLineOption(std::string_view commandLine, std::string_view option)
{
    for (size_t at = commandLine.find(option); at != std::string_view::npos; at = commandLine.find(option, at + 1)) {
        const bool startsToken = at == 0 || commandLine[at - 1] == ' ' || commandLine[at - 1] == '\t';
        const size_t end = at + option.size();
        const bool endsToken = end == commandLine.size() || commandLine[end] == ' ' || commandLine[end] == '\t';
        if (startsToken && endsToken) return at;
    }
    return std::string_view::npos;
}

bool HasCommandLineOption(std::string_view commandLine, std::string_view option)
{
    return FindCommandLineOption(commandLine, option) != std::string_view::npos;
}

// The value after a whole-token option ("--script <path>"), quoted or not.
std::optional<std::string> FindCommandLineOptionValue(std::string_view commandLine, std::string_view option)
{
    const size_t at = FindCommandLineOption(commandLine, option);
    if (at == std::string_view::npos) return std::nullopt;
    return FindCommandLineValue(commandLine.substr(at), option);
}

#if BASICRENDERER_HAS_SCRIPTING
// F5 in the demo window: run the last scenario again from its (re-read) sources.
std::atomic<bool> g_scriptReloadRequested{ false };

// D3D12 validation messages fail a running scenario. Only with the debug layer (an InfoQueue); the full check runs only
// when new messages have arrived.
std::optional<std::string> ProbeD3D12Validation(UINT64& seenMessages)
{
    auto device = br::extensions::GetRenderDevice();
    ID3D12Device* nativeDevice = rhi::dx12::get_device(device);
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
    if (!nativeDevice || FAILED(nativeDevice->QueryInterface(IID_PPV_ARGS(&infoQueue)))) return std::nullopt;
    const UINT64 count = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    if (count == seenMessages) return std::nullopt;
    seenMessages = count;
    if (D3D12SmokeValidationFailed()) return std::string("D3D12 validation errors during the scenario (see the log)");
    return std::nullopt;
}

// C# scenarios (Scripting/, docs/scripting.md).
//   --script <file.cs|dir> [--scenario <name>] [--script-fixed-dt <seconds>] [--script-result <path>] [--script-exit]
// With --script-exit the demo quits when the scenario ends: exit code 0 passed, 1 failed, 2 compile error, 3 the
// scripting runtime could not start. The control pipe gets script.run, script.reload, script.stop and script.status,
// and F5 reruns the last scenario.
class DemoScriptRun {
public:
    std::optional<double> FixedDeltaSeconds() const { return m_fixedDelta; }
    std::optional<int> ExitCode() const { return m_exitCode; }
    // A scenario ran: the camera pose it left is not the user's to remember.
    bool RanScenario() const { return m_ranScenario; }

    void Start(std::string_view commandLine, HWND hwnd, Renderer& renderer, br::diagnostics::ControlCommandRegistry& commands, bool controlPipe)
    {
        m_hwnd = hwnd;
        // The menu runs on the host thread. Start .NET on demand when no command-line scenario or pipe needs it.
        SettingsManager::GetInstance().getSettingSetter<std::function<void(const std::string&)>>("loadScript")(
            [this, &renderer, &commands](const std::string& path) {
                std::string error;
                if (!InitializeHost(renderer, commands, error) || !Run(path, {}, {}, error)) {
                    spdlog::error("Load Script '{}': {}", path, error);
                    return;
                }
                spdlog::info("Load Script '{}': started", path);
            });
        const auto source = FindCommandLineOptionValue(commandLine, "--script");
        // Without --script, the runtime still starts when there is a control pipe, so script.run can use it.
        if (!source && !controlPipe) return;
        m_exitWhenDone = source && HasCommandLineOption(commandLine, "--script-exit");
        if (const auto result = FindCommandLineOptionValue(commandLine, "--script-result")) m_resultPath = *result;
        if (const auto dt = FindCommandLineOptionValue(commandLine, "--script-fixed-dt")) {
            m_fixedDelta = std::stod(*dt);
            if (!(*m_fixedDelta > 0.0)) m_fixedDelta.reset();
        }

        std::string error;
        if (!InitializeHost(renderer, commands, error)) {
            spdlog::error("scripting runtime: {}", error);
            Finish(3);
            return;
        }
        if (!source) return;
        const auto scenario = FindCommandLineOptionValue(commandLine, "--scenario").value_or("");
        if (!Run(*source, scenario, {}, error)) {
            spdlog::error("--script {}: {}", *source, error);
            Finish(3);
            return;
        }
        spdlog::info("--script {}: started{}{}", *source, scenario.empty() ? "" : " scenario " + scenario,
            m_fixedDelta ? std::format(", fixed time step {} s", *m_fixedDelta) : std::string());
    }

    // Before renderer.Update, with the time step the renderer will also be given.
    void Tick(uint64_t frameIndex, double deltaSeconds)
    {
        if (!m_host) return;
        if (g_scriptReloadRequested.exchange(false)) {
            std::string error;
            if (m_host->Reload(error)) {
                m_reported = false;
                spdlog::info("script: reloading {}", m_host->SourcePath().string());
            } else {
                spdlog::warn("script: F5 reload: {}", error);
            }
        }
        m_host->Tick(frameIndex, deltaSeconds);
        const auto state = m_host->State();
        if (const auto requested = m_host->RequestedExitCode(); requested && !m_exitCode) {
            WriteResult(state);
            m_exitCode = *requested;
            PostMessage(m_hwnd, WM_CLOSE, 0, 0);
            return;
        }
        if (!br::scripting::IsTerminal(state) || m_reported) return;
        m_reported = true;
        WriteResult(state);
        spdlog::info("script: scenario {}", br::scripting::ToString(state));
        if (m_exitWhenDone) Finish(br::scripting::ScriptHost::ExitCodeFor(state));
    }

    // Before the renderer shuts down: the backend lets go of the scene first.
    void Shutdown()
    {
        SettingsManager::GetInstance().getSettingSetter<std::function<void(const std::string&)>>("loadScript")({});
        m_host.reset();
    }

private:
    bool InitializeHost(Renderer& renderer, br::diagnostics::ControlCommandRegistry& commands, std::string& error)
    {
        if (m_host) return true;
        br::scripting::ScriptHostOptions options;
        options.logSink = [](br::scripting::LogLevel level, std::string_view line) {
            spdlog::log(static_cast<spdlog::level::level_enum>(static_cast<int>(level)), "[script] {}", line);
        };
        br::scripting::RendererScriptBackendOptions backend;
        backend.renderer = &renderer;
        backend.commands = &commands;
        options.backend = br::scripting::CreateRendererScriptBackend(backend);
        options.failureProbe = [seen = UINT64(0)]() mutable { return ProbeD3D12Validation(seen); };
        m_host = br::scripting::ScriptHost::Create(std::move(options), error);
        if (!m_host) return false;
        RegisterCommands(commands);
        return true;
    }

    bool Run(const std::filesystem::path& source, std::string_view scenario, std::string_view options, std::string& error)
    {
        if (br::scripting::IsBusy(m_host->State())) m_host->Stop("replaced by a new script");
        if (!m_host->RunScenario(source, scenario, error, options)) return false;
        m_reported = false;
        m_ranScenario = true;
        return true;
    }

    void RegisterCommands(br::diagnostics::ControlCommandRegistry& commands)
    {
        // script.run {path, scenario?, timeout_seconds?}: compiles and runs a scenario (stopping one still running).
        commands.Register("script.run", [this](const br::diagnostics::ControlRequest& request) {
            const auto path = request.args.at("path").get<std::string>();
            const auto scenario = request.args.value("scenario", "");
            nlohmann::json options = nlohmann::json::object();
            if (request.args.contains("timeout_seconds")) options["timeout_seconds"] = request.args.at("timeout_seconds");
            std::string error;
            if (!Run(path, scenario, options.empty() ? std::string() : options.dump(), error)) throw std::runtime_error(error);
            return Describe();
        });
        commands.Register("script.reload", [this](const br::diagnostics::ControlRequest&) {
            std::string error;
            if (!m_host->Reload(error)) throw std::runtime_error(error);
            m_reported = false;
            return Describe();
        });
        commands.Register("script.stop", [this](const br::diagnostics::ControlRequest& request) {
            m_host->Stop(request.args.value("reason", "stopped by script.stop"));
            return Describe();
        });
        // script.status: state (idle, compiling, running, passed, failed, compile_error), source, scenario, and the
        // last result document.
        commands.Register("script.status", [this](const br::diagnostics::ControlRequest&) { return Describe(); });
    }

    nlohmann::json Describe() const
    {
        nlohmann::json reply = {
            { "state", br::scripting::ToString(m_host->State()) },
            { "source", m_host->SourcePath().string() },
            { "scenario", m_host->ScenarioName() },
        };
        if (!m_host->ResultJson().empty()) reply["result"] = nlohmann::json::parse(m_host->ResultJson(), nullptr, false);
        return reply;
    }

    void WriteResult(br::scripting::ScenarioState state)
    {
        if (!m_resultPath) return;
        std::error_code ec;
        if (m_resultPath->has_parent_path()) std::filesystem::create_directories(m_resultPath->parent_path(), ec);
        std::ofstream out(*m_resultPath, std::ios::binary | std::ios::trunc);
        out << (m_host ? m_host->ResultJson() : std::string("{}"));
        if (!out) spdlog::error("--script-result: could not write {}", m_resultPath->string());
        else spdlog::info("--script-result: {} ({})", m_resultPath->string(), br::scripting::ToString(state));
    }

    // Without --script-exit the demo keeps running after the scenario ends.
    void Finish(int exitCode)
    {
        if (!m_exitWhenDone) return;
        m_exitCode = exitCode;
        PostMessage(m_hwnd, WM_CLOSE, 0, 0);
    }

    std::unique_ptr<br::scripting::ScriptHost> m_host;
    std::optional<std::filesystem::path> m_resultPath;
    std::optional<double> m_fixedDelta;
    std::optional<int> m_exitCode;
    HWND m_hwnd = nullptr;
    bool m_exitWhenDone = false;
    bool m_reported = false;
    bool m_ranScenario = false;
};
#endif

br::telemetry::sampling::ReadinessSnapshot BuildDemoSamplingReadiness(const Renderer& renderer)
{
    const auto source = renderer.GetSamplingReadinessSnapshot();
    br::telemetry::sampling::ReadinessSnapshot result;
    result.values = {
        { "scene_task_in_flight", source.sceneTaskInFlight ? 1 : 0 },
        { "pending_texture_reloads", source.pendingTextureReloads },
        { "full_resolution_textures", source.fullResolutionTextures },
        { "resident_clod_groups", source.residentClodGroups },
        { "queued_clod_requests", source.queuedClodRequests },
        { "in_flight_clod_groups", source.inFlightClodGroups },
        { "completed_clod_results", source.completedClodResults },
        { "pending_direct_storage_launches", source.pendingDirectStorageLaunches },
        { "pending_direct_storage_uploads", source.pendingDirectStorageUploads },
        { "io_tasks", source.ioTasks },
        { "background_tasks", source.backgroundTasks },
        { "shader_compile_tasks", source.shaderCompileTasks },
        { "deferred_retire_queue", static_cast<std::int64_t>(source.deferredRetireQueueDepth) },
        { "draw_records_allocated", static_cast<std::int64_t>(source.drawRecordsAllocated) }
    };
    return result;
}

std::string DemoSamplingSignature(const br::telemetry::sampling::ReadinessSnapshot& snapshot)
{
    std::ostringstream output;
    for (const char* key : {
        "full_resolution_textures", "resident_clod_groups", "draw_records_allocated" }) {
        if (const auto found = snapshot.values.find(key); found != snapshot.values.end()) {
            output << key << '=' << found->second << ';';
        }
    }
    return output.str();
}

namespace {

// capture.hdr: one frame's radiance as an RGBA32F DDS, from the path tracer's accumulation buffer or the HDR colour that
// tonemapping reads. Like pso.recompile it answers with a job id; job.status reports it.
class DemoHdrCaptures {
public:
    // Apart from the pipeline jobs' ids, so job.status can tell them apart.
    static constexpr std::uint64_t kFirstJobId = 1ull << 48;

    std::uint64_t Request(Renderer& renderer, const nlohmann::json& document)
    {
        const auto& pathField = document.at("path");
        if (!pathField.is_string() || pathField.get_ref<const std::string&>().empty()) {
            throw std::runtime_error("capture.hdr 'path' must be a non-empty string");
        }
        const auto accumulation = renderer.GetPathTracingAccumulation();
        const std::string target = document.value("target", accumulation.buffer ? std::string("accumulation") : std::string("hdr"));
        if (target != "accumulation" && target != "hdr") throw std::runtime_error("capture.hdr 'target' must be 'accumulation' or 'hdr'");
        const std::uint64_t spp = document.value("spp", std::uint64_t{ 0 });
        if ((target == "accumulation" || spp > 0u) && !accumulation.buffer) {
            throw std::runtime_error("path tracing is not accumulating: set enablePathTracing and pathTracingAccumulate first");
        }
        const double timeoutSeconds = document.value("timeout_seconds", 300.0);
        if (!(timeoutSeconds > 0.0)) throw std::runtime_error("capture.hdr 'timeout_seconds' must be positive");

        auto job = std::make_shared<Job>();
        job->id = m_nextId++;
        job->path = std::filesystem::absolute(std::filesystem::path(pathField.get<std::string>()));
        job->accumulation = target == "accumulation";
        job->spp = spp;
        job->deadline = std::chrono::steady_clock::now()
            + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(timeoutSeconds));
        if (job->path.has_parent_path()) std::filesystem::create_directories(job->path.parent_path());
        std::scoped_lock lock(m_mutex);
        m_jobs.emplace(job->id, job);
        return job->id;
    }

    std::optional<nlohmann::json> Status(std::uint64_t id)
    {
        std::shared_ptr<Job> job;
        {
            std::scoped_lock lock(m_mutex);
            const auto found = m_jobs.find(id);
            if (found == m_jobs.end()) return std::nullopt;
            job = found->second;
        }
        std::scoped_lock lock(job->mutex);
        return nlohmann::json{
            { "id", job->id }, { "kind", "capture.hdr" }, { "state", StateName(job->state) }, { "error", job->error },
            { "path", job->path.string() }, { "target", job->accumulation ? "accumulation" : "hdr" },
            { "spp_requested", job->spp }, { "width", job->width }, { "height", job->height },
            { "samples_min", job->samplesMin }, { "samples_max", job->samplesMax }, { "reads", job->reads },
            { "frames_since_restart", job->framesSinceRestart },
        };
    }

    // Each frame, on the thread that renders: starts the readbacks of the jobs whose samples are in, and expires the rest.
    void Pump(Renderer& renderer)
    {
        std::vector<std::shared_ptr<Job>> jobs;
        {
            std::scoped_lock lock(m_mutex);
            for (const auto& [id, job] : m_jobs) jobs.push_back(job);
        }
        const auto now = std::chrono::steady_clock::now();
        for (const auto& job : jobs) {
            std::scoped_lock lock(job->mutex);
            if (job->state == State::Published || job->state == State::Failed) continue;
            if (now >= job->deadline) {
                Fail(*job, job->state == State::Reading ? "timed out waiting for the readback" :
                    "timed out with " + std::to_string(job->framesSinceRestart) + " of " + std::to_string(job->spp) + " samples");
                continue;
            }
            if (job->state != State::Waiting) continue;
            try {
                StartRead(renderer, job);
            } catch (const std::exception& exception) {
                Fail(*job, exception.what());
            }
        }
    }

private:
    enum class State { Waiting, Reading, Published, Failed };

    struct Job {
        std::mutex mutex;
        std::uint64_t id = 0;
        std::filesystem::path path;
        bool accumulation = true;
        std::uint64_t spp = 0;
        std::chrono::steady_clock::time_point deadline;
        State state = State::Waiting;
        std::string error;
        std::uint32_t width = 0, height = 0;
        float samplesMin = 0.0f, samplesMax = 0.0f;
        std::uint64_t framesSinceRestart = 0;
        std::uint32_t reads = 0;
    };

    static const char* StateName(State state)
    {
        switch (state) {
        case State::Waiting: return "waiting";
        case State::Reading: return "reading";
        case State::Published: return "published";
        case State::Failed: return "failed";
        }
        return "unknown";
    }

    static void Fail(Job& job, std::string error)
    {
        job.state = State::Failed;
        job.error = std::move(error);
        spdlog::error("capture.hdr {} '{}': {}", job.id, job.path.string(), job.error);
    }

    // Called with job->mutex held.
    static void StartRead(Renderer& renderer, const std::shared_ptr<Job>& job)
    {
        auto* graph = renderer.GetRenderGraph();
        if (!graph) return; // Between graphs; try next frame.
        auto* readbacks = graph->GetReadbackService();
        if (!readbacks) throw std::runtime_error("the render graph has no readback service");
        const auto accumulation = renderer.GetPathTracingAccumulation();
        job->framesSinceRestart = accumulation.framesSinceRestart;
        if (job->accumulation || job->spp > 0u) {
            // A graph rebuild (an accumulation toggle) has no buffer until it is built.
            if (!accumulation.buffer || accumulation.framesSinceRestart < job->spp) return;
        }
        auto callback = [job](org::ReadbackCaptureResult&& result) { OnRead(job, std::move(result)); };
        job->state = State::Reading;
        ++job->reads;
        if (job->accumulation) {
            // Not aliased (it is the history), so it can be read after the whole graph without interrupting it.
            readbacks->RequestReadbackCaptureAfterGraph(accumulation.buffer.get(), org::RangeSpec{}, std::move(callback));
            return;
        }
        // The upscaled HDR colour aliases, so it is read where tonemapping reads it, after the upscaler's write: output
        // resolution, without bloom (tonemapping adds that).
        constexpr const char* kAnchor = "TonemappingPass";
        if (const auto* statistics = graph->GetStatisticsService()) {
            const auto& names = statistics->GetPassNames();
            if (!names.empty() && std::ranges::find(names, std::string(kAnchor)) == names.end()) {
                throw std::runtime_error("the render graph has no TonemappingPass to read the HDR colour at");
            }
        }
        auto hdr = graph->RequestResourcePtr(org::ResourceIdentifier{ Builtin::PostProcessing::UpscaledHDR }, true);
        if (!hdr) throw std::runtime_error("the render graph has no Builtin::PostProcessing::UpscaledHDR");
        readbacks->RequestReadbackCapture(kAnchor, hdr.get(), org::RangeSpec{}, std::move(callback));
    }

    static void OnRead(const std::shared_ptr<Job>& job, org::ReadbackCaptureResult&& result)
    {
        std::scoped_lock lock(job->mutex);
        if (job->state != State::Reading) return;
        try {
            std::vector<float> pixels = ToRgba32F(result);
            job->width = result.width;
            job->height = result.height;
            if (job->accumulation) {
                // The shader keeps rgb as the running average, so the radiance needs no division; alpha is the count.
                const auto [minimum, maximum] = SampleRange(pixels);
                job->samplesMin = minimum;
                job->samplesMax = maximum;
                if (job->spp > 0u && minimum < static_cast<float>(job->spp)) {
                    // A restart the counter did not see, or frames counted that never traced: wait for more and read again.
                    job->state = State::Waiting;
                    return;
                }
            }
            WriteRgba32FDds(job->path, result.width, result.height, pixels);
            job->state = State::Published;
            spdlog::info("capture.hdr {}: wrote {}x{} {} to '{}' (samples {}..{})", job->id, result.width, result.height,
                job->accumulation ? "accumulation" : "hdr", job->path.string(), job->samplesMin, job->samplesMax);
        } catch (const std::exception& exception) {
            Fail(*job, exception.what());
        }
    }

    static std::pair<float, float> SampleRange(const std::vector<float>& pixels)
    {
        float minimum = std::numeric_limits<float>::max(), maximum = 0.0f;
        for (std::size_t index = 3; index < pixels.size(); index += 4) {
            minimum = std::min(minimum, pixels[index]);
            maximum = std::max(maximum, pixels[index]);
        }
        return { pixels.empty() ? 0.0f : minimum, maximum };
    }

    static std::vector<float> ToRgba32F(const org::ReadbackCaptureResult& result)
    {
        if (result.layouts.empty()) throw std::runtime_error("texture readback has no footprint");
        std::size_t bytesPerPixel = 0;
        switch (result.format) {
        case rhi::Format::R32G32B32A32_Float: bytesPerPixel = 16; break;
        case rhi::Format::R16G16B16A16_Float: bytesPerPixel = 8; break;
        default: throw std::runtime_error("capture.hdr target has an unsupported format " + std::to_string(static_cast<int>(result.format)));
        }
        const auto& layout = result.layouts.front();
        const std::size_t tightPitch = static_cast<std::size_t>(result.width) * bytesPerPixel;
        const std::size_t rowPitch = static_cast<std::size_t>(layout.rowPitch);
        const std::size_t offset = static_cast<std::size_t>(layout.offset);
        // The footprint's total size does not pad the last row.
        if (result.height == 0 || rowPitch < tightPitch || offset + rowPitch * (result.height - 1) + tightPitch > result.data.size()) {
            throw std::runtime_error("texture readback footprint does not match its data");
        }
        std::vector<float> pixels(static_cast<std::size_t>(result.width) * result.height * 4u);
        for (std::uint32_t row = 0; row < result.height; ++row) {
            const std::byte* source = result.data.data() + offset + rowPitch * row;
            float* destination = pixels.data() + static_cast<std::size_t>(row) * result.width * 4u;
            if (bytesPerPixel == 16) {
                std::memcpy(destination, source, tightPitch);
            } else {
                for (std::size_t channel = 0; channel < static_cast<std::size_t>(result.width) * 4u; ++channel) {
                    DirectX::PackedVector::HALF half;
                    std::memcpy(&half, source + channel * sizeof(half), sizeof(half));
                    destination[channel] = DirectX::PackedVector::XMConvertHalfToFloat(half);
                }
            }
        }
        return pixels;
    }

    // Written beside the target and renamed over it, so a file at the path is always a whole one.
    static void WriteRgba32FDds(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height, const std::vector<float>& pixels)
    {
        constexpr std::uint32_t kCaps = 0x1, kHeight = 0x2, kWidth = 0x4, kPitch = 0x8, kPixelFormat = 0x1000, kMipCount = 0x20000;
        constexpr std::uint32_t kFourCc = 0x4, kCapsTexture = 0x1000, kDimensionTexture2D = 3;
        std::array<std::uint32_t, 31> header{};
        header[0] = 124;
        header[1] = kCaps | kHeight | kWidth | kPitch | kPixelFormat | kMipCount;
        header[2] = height;
        header[3] = width;
        header[4] = width * 16u;
        header[6] = 1; // mip count
        header[18] = 32; // pixel format size
        header[19] = kFourCc;
        header[20] = 0x30315844; // "DX10"
        header[26] = kCapsTexture;
        const std::array<std::uint32_t, 5> dx10{ static_cast<std::uint32_t>(DXGI_FORMAT_R32G32B32A32_FLOAT), kDimensionTexture2D, 0, 1, 0 };
        auto temporary = path;
        temporary += ".partial";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write("DDS ", 4);
            output.write(reinterpret_cast<const char*>(header.data()), sizeof(header));
            output.write(reinterpret_cast<const char*>(dx10.data()), sizeof(dx10));
            output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size() * sizeof(float)));
            output.flush();
            if (!output) throw std::runtime_error("failed to write " + temporary.string());
        }
        std::filesystem::rename(temporary, path);
    }

    std::mutex m_mutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<Job>> m_jobs;
    std::uint64_t m_nextId = kFirstJobId;
};

} // namespace

class DemoStatisticalSamplingRun {
public:
    bool Initialize(
        const std::filesystem::path& configurationPath,
        const std::optional<std::string>& controlPipe,
        std::string& error)
    {
        try {
            m_configuration = br::telemetry::sampling::LoadConfiguration(configurationPath);
            br::telemetry::nvperf::CaptureConfiguration captureConfiguration;
            for (const auto& metric : m_configuration->metrics) {
                if (metric.source == br::telemetry::sampling::MeasurementSource::NvPerf) {
                    m_usesNvPerf = true;
                    captureConfiguration.metrics.push_back(metric.request);
                } else {
                    m_usesRenderGraphGpuTime = true;
                }
            }
            if (m_usesNvPerf && m_usesRenderGraphGpuTime) {
                error = "a sampling run cannot mix NVPerf and render-graph timestamp metrics";
                return false;
            }
            std::set<std::pair<std::string, std::string>> uniquePasses;
            std::optional<std::string> controllerQueue;
            if (m_usesNvPerf) {
                for (const auto& pass : m_configuration->passes) {
                    if (uniquePasses.emplace(pass.name, pass.queue).second) {
                        captureConfiguration.passes.push_back({ pass.name, pass.queue });
                    }
                    if (!pass.queue.empty()) {
                        if (controllerQueue && *controllerQueue != pass.queue) {
                            error = "one sampling run cannot capture passes from multiple queues";
                            return false;
                        }
                        controllerQueue = pass.queue;
                    }
                }
                if (controllerQueue) captureConfiguration.controllerQueue = *controllerQueue;
                if (!br::telemetry::nvperf::ConfigureCapture(captureConfiguration, error)) return false;
            }

            m_database = std::make_unique<br::telemetry::sampling::Database>();
            m_database->Open(*m_configuration);
            m_configurationPath = std::filesystem::absolute(configurationPath);
            m_persistent = controlPipe.has_value();
            if (controlPipe) {
                const int count = MultiByteToWideChar(CP_UTF8, 0, controlPipe->data(), static_cast<int>(controlPipe->size()), nullptr, 0);
                std::wstring pipeName(static_cast<std::size_t>(count), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, controlPipe->data(), static_cast<int>(controlPipe->size()), pipeName.data(), count);
                m_controlServer.Start(std::move(pipeName));
                m_finished = true;
                m_finalized = true;
                spdlog::info("BasicRenderer sampling control server listening on '{}'", *controlPipe);
            } else {
                StartExperiment("command-line");
            }
            spdlog::info("BasicRenderer sampling: source={}",
                m_usesNvPerf ? "NVPerf" : "render-graph GPU timestamps");
            return true;
        } catch (const std::exception& exception) {
            error = exception.what();
            return false;
        }
    }

    [[nodiscard]] bool UsesNvPerf() const noexcept { return m_usesNvPerf; }

    bool Enabled() const noexcept { return m_configuration.has_value(); }
    int ExitCode() const noexcept { return m_exitCode; }

    // Registers the control commands. Once, after the renderer is initialized; the pipe and scripts use them after that.
    void RegisterControlCommands(Renderer& renderer, HWND hwnd);
    br::diagnostics::ControlCommandRegistry& Commands() { return m_commands; }

    // The control pipe without a sampling configuration: every command but profile.run is available.
    void StartControlServer(const std::string& controlPipe)
    {
        const int count = MultiByteToWideChar(CP_UTF8, 0, controlPipe.data(), static_cast<int>(controlPipe.size()), nullptr, 0);
        std::wstring pipeName(static_cast<std::size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, controlPipe.data(), static_cast<int>(controlPipe.size()), pipeName.data(), count);
        m_controlServer.Start(std::move(pipeName));
        m_persistent = true;
        spdlog::info("BasicRenderer control server listening on '{}'", controlPipe);
    }

    void AfterFrame(Renderer& renderer, HWND hwnd)
    {
        PumpControlRequests();
        m_hdrCaptures.Pump(renderer);
        if (!m_configuration || m_finished) return;
        switch (m_phase) {
        case Phase::Warmup:
        {
            const auto readiness = BuildDemoSamplingReadiness(renderer);
            const auto signature = DemoSamplingSignature(readiness);
            const bool noPendingWork = std::ranges::all_of(readiness.values, [](const auto& entry) {
                static const std::set<std::string> nonBlocking{
                    "full_resolution_textures", "resident_clod_groups", "draw_records_allocated"
                };
                return nonBlocking.contains(entry.first) || entry.second == 0;
            });
            if (noPendingWork && signature == m_readinessSignature) ++m_readyStableFrames;
            else m_readyStableFrames = 0;
            m_readinessSignature = signature;
            if (m_readyStableFrames >= m_configuration->readyFrames) {
                renderer.SetDeterministicSamplingMode(true);
                br::telemetry::nvperf::SetStreamingSuppressed(true);
                m_phase = Phase::Settling;
                m_phaseFrames = 0;
                m_readinessDurationMs = ElapsedMs();
                spdlog::info("BasicRenderer sampling: readiness stable for {} frames after {} ms; deterministic settling started",
                    m_readyStableFrames, m_readinessDurationMs);
            } else if (ElapsedMs() >= m_configuration->readinessTimeoutMs) {
                Finish(renderer, "failed", "timed out waiting for stable readiness", hwnd, 8);
            }
            break;
        }
        case Phase::Settling:
            if (++m_phaseFrames >= m_configuration->settlingFrames) {
                if (m_usesNvPerf) ArmNextCapture(renderer, hwnd);
                else m_phase = Phase::RenderGraphSampling;
            }
            break;
        case Phase::Capturing:
            if (br::telemetry::nvperf::CaptureComplete()) ConsumeCapture(renderer, hwnd);
            break;
        case Phase::RenderGraphSampling:
            ConsumeRenderGraphSample(renderer, hwnd);
            break;
        }
    }

private:
    enum class Phase { Warmup, Settling, Capturing, RenderGraphSampling };

    std::uint64_t ElapsedMs() const;
    void StartExperiment(std::string label, Renderer* renderer = nullptr);
    const char* PhaseName() const;
    void PumpControlRequests();

    void ArmNextCapture(Renderer& renderer, HWND hwnd)
    {
        std::string error;
        m_captureReadiness = BuildDemoSamplingReadiness(renderer);
        m_captureSignature = DemoSamplingSignature(m_captureReadiness);
        if (!br::telemetry::nvperf::ArmCapture(m_nextOrdinal, error)) {
            Finish(renderer, "failed", "failed to arm NVPerf: " + error, hwnd, 3);
            return;
        }
        m_phase = Phase::Capturing;
        spdlog::info("BasicRenderer sampling: armed sample {}", m_nextOrdinal);
    }

    void ConsumeCapture(Renderer& renderer, HWND hwnd)
    {
        auto capture = br::telemetry::nvperf::TakeCaptureResult();
        if (!capture) {
            Finish(renderer, "failed", "NVPerf completed without a capture result", hwnd, 4);
            return;
        }

        br::telemetry::sampling::SampleRecord sample;
        sample.ordinal = m_nextOrdinal++;
        sample.startFrame = capture->startFrame;
        sample.endFrame = capture->endFrame;
        sample.before = m_captureReadiness;
        sample.after = BuildDemoSamplingReadiness(renderer);
        sample.capture = std::move(*capture);
        if (!sample.capture.success) {
            sample.rejectionReason = sample.capture.error.empty() ? "NVPerf capture failed" : sample.capture.error;
        } else if (DemoSamplingSignature(sample.after) != m_captureSignature) {
            sample.rejectionReason = "workload-defining state changed during capture";
        } else {
            sample.measurements = br::telemetry::sampling::SelectMeasurements(
                *m_configuration, sample.capture, sample.rejectionReason);
        }
        sample.accepted = sample.rejectionReason.empty();
        CommitSample(std::move(sample), renderer, hwnd);
    }

    void ConsumeRenderGraphSample(Renderer& renderer, HWND hwnd)
    {
        const auto* graph = renderer.GetRenderGraph();
        const auto* service = graph ? graph->GetStatisticsService() : nullptr;
        if (!service) {
            Finish(renderer, "failed", "render graph statistics service is unavailable", hwnd, 6);
            return;
        }
        const auto& names = service->GetPassNames();
        const auto& stats = service->GetPassStats();
        br::telemetry::sampling::SampleRecord sample;
        sample.ordinal = m_nextOrdinal;
        sample.before = BuildDemoSamplingReadiness(renderer);
        sample.after = sample.before;
        sample.capture.success = true;
        std::uint64_t sampleSerial = 0;
        for (const auto& pass : m_configuration->passes) {
            const auto found = std::ranges::find(names, pass.name);
            if (found == names.end()) {
                if (pass.required) sample.rejectionReason = "render graph pass not found: " + pass.name;
                continue;
            }
            const auto index = static_cast<std::size_t>(std::distance(names.begin(), found));
            if (index >= stats.size() || stats[index].gpuSampleSerial == 0) return;
            if (sampleSerial == 0) sampleSerial = stats[index].gpuSampleSerial;
            if (stats[index].gpuSampleSerial != sampleSerial) return;
            for (const auto& metricId : pass.metricIds) {
                sample.measurements.push_back({ pass.id, metricId, stats[index].gpuTimeMs });
            }
        }
        if (sampleSerial <= m_lastRenderGraphSampleSerial) {
            if (std::chrono::steady_clock::now() - m_startedAt >
                std::chrono::milliseconds(m_configuration->readinessTimeoutMs)) {
                Finish(renderer, "failed", "timed out waiting for fresh render-graph GPU timestamps", hwnd, 7);
            }
            return;
        }
        m_lastRenderGraphSampleSerial = sampleSerial;
        sample.startFrame = sampleSerial;
        sample.endFrame = sampleSerial;
        sample.accepted = sample.rejectionReason.empty();
        ++m_nextOrdinal;
        CommitSample(std::move(sample), renderer, hwnd);
    }

    void CommitSample(br::telemetry::sampling::SampleRecord sample, Renderer& renderer, HWND hwnd)
    {
        m_database->RecordSample(m_experimentId, sample);
        m_samples.push_back(std::move(sample));
        m_summaries = br::telemetry::sampling::ComputeSummaries(*m_configuration, m_samples);
        m_database->ReplaceSummaries(m_experimentId, m_summaries);

        const auto accepted = static_cast<std::uint32_t>(std::ranges::count_if(
            m_samples, [](const auto& item) { return item.accepted; }));
        const auto& recorded = m_samples.back();
        spdlog::info(
            "BasicRenderer sampling: sample {} {} (accepted={}, attempted={}){}",
            recorded.ordinal,
            recorded.accepted ? "accepted" : "rejected",
            accepted,
            m_samples.size(),
            recorded.rejectionReason.empty() ? std::string{} : ": " + recorded.rejectionReason);

        if (accepted >= m_configuration->minimumSamples &&
            br::telemetry::sampling::AllRequiredTargetsConverged(*m_configuration, m_summaries)) {
            Finish(renderer, "complete", "all required targets converged", hwnd, 0);
        } else if (accepted >= m_configuration->maximumSamples) {
            Finish(renderer, "complete", "maximum accepted sample count reached", hwnd, 0);
        } else if (m_samples.size() >= static_cast<std::size_t>(m_configuration->maximumSamples) * 3u) {
            Finish(renderer, "failed", "maximum capture attempts reached", hwnd, 5);
        } else if (m_usesNvPerf) {
            ArmNextCapture(renderer, hwnd);
        }
    }

    void Finish(Renderer& renderer, const char* status, std::string reason, HWND hwnd, int exitCode)
    {
        // Back to normal frames: deterministic mode (frame time 0) changes what temporal passes such as Ray
        // Reconstruction produce, so it must not outlive the run.
        renderer.SetDeterministicSamplingMode(false);
        br::telemetry::nvperf::SetStreamingSuppressed(false);
        m_finished = true;
        m_finalized = true;
        m_stoppingReason = reason;
        m_exitCode = exitCode;
        const auto readinessDuration = m_readinessDurationMs;
        if (m_database) {
            m_database->ReplaceSummaries(m_experimentId, m_summaries);
            m_database->FinishExperiment(m_experimentId, status, reason);
            br::telemetry::sampling::WriteLastRunSummary(
                *m_configuration,
                m_experimentId,
                readinessDuration,
                m_samples.empty() ? 0 : m_samples.back().capture.scheduledPasses,
                m_samples,
                m_summaries,
                reason);
        }
        spdlog::info("BasicRenderer sampling: {} ({}) report='{}'", status, reason, m_configuration->summaryPath.string());
        if (!m_persistent) PostMessage(hwnd, WM_CLOSE, 0, 0);
    }

    Phase m_phase{ Phase::Warmup };
    std::optional<br::telemetry::sampling::Configuration> m_configuration;
    std::unique_ptr<br::telemetry::sampling::Database> m_database;
    std::vector<br::telemetry::sampling::SampleRecord> m_samples;
    std::vector<br::telemetry::sampling::Summary> m_summaries;
    br::telemetry::sampling::ReadinessSnapshot m_captureReadiness;
    std::string m_captureSignature;
    std::chrono::steady_clock::time_point m_startedAt{};
    std::filesystem::path m_configurationPath;
    br::telemetry::sampling::ControlServer m_controlServer;
    br::diagnostics::ControlCommandRegistry m_commands;
    DemoHdrCaptures m_hdrCaptures;
    std::string m_readinessSignature;
    std::string m_stoppingReason;
    std::uint32_t m_phaseFrames{ 0 };
    std::uint32_t m_readyStableFrames{ 0 };
    std::uint64_t m_readinessDurationMs{ 0 };
    std::uint32_t m_nextOrdinal{ 1 };
    std::int64_t m_experimentId{ 0 };
    int m_exitCode{ 0 };
    bool m_finished{ true };   // No experiment is running until StartExperiment.
    bool m_finalized{ true };
    bool m_persistent{ false };
    bool m_usesNvPerf{ false };
    bool m_usesRenderGraphGpuTime{ false };
    std::uint64_t m_lastRenderGraphSampleSerial{ 0 };
};

constexpr std::string_view DemoCameraStatePath = "logs/demo_camera_state.json";

struct DemoCameraState {
    bool rememberCameraPose = false;
    std::optional<DirectX::XMFLOAT3> position;
    std::optional<DirectX::XMFLOAT4> rotation;
};

bool IsFinite(const DirectX::XMFLOAT3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool IsFinite(const DirectX::XMFLOAT4& value) {
    return std::isfinite(value.x) && std::isfinite(value.y)
        && std::isfinite(value.z) && std::isfinite(value.w);
}

DemoCameraState LoadDemoCameraState() {
    DemoCameraState state;
    std::ifstream input(DemoCameraStatePath.data());
    if (!input) {
        return state;
    }

    try {
        const auto document = nlohmann::json::parse(input);
        state.rememberCameraPose = document.value("rememberCameraPose", false);
        if (!state.rememberCameraPose) {
            return state;
        }

        const auto& position = document.at("position");
        const auto& rotation = document.at("rotation");
        if (!position.is_array() || position.size() != 3
            || !rotation.is_array() || rotation.size() != 4) {
            throw std::runtime_error("position or rotation has an invalid shape");
        }

        DirectX::XMFLOAT3 loadedPosition{
            position.at(0).get<float>(),
            position.at(1).get<float>(),
            position.at(2).get<float>() };
        DirectX::XMFLOAT4 loadedRotation{
            rotation.at(0).get<float>(),
            rotation.at(1).get<float>(),
            rotation.at(2).get<float>(),
            rotation.at(3).get<float>() };
        if (!IsFinite(loadedPosition) || !IsFinite(loadedRotation)) {
            throw std::runtime_error("position or rotation contains a non-finite value");
        }

        const float rotationLengthSquared =
            loadedRotation.x * loadedRotation.x
            + loadedRotation.y * loadedRotation.y
            + loadedRotation.z * loadedRotation.z
            + loadedRotation.w * loadedRotation.w;
        if (rotationLengthSquared < 1.0e-8f) {
            throw std::runtime_error("rotation is not a usable quaternion");
        }

        DirectX::XMStoreFloat4(
            &loadedRotation,
            DirectX::XMQuaternionNormalize(DirectX::XMLoadFloat4(&loadedRotation)));
        state.position = loadedPosition;
        state.rotation = loadedRotation;
    }
    catch (const std::exception& error) {
        spdlog::warn(
            "Could not load remembered demo camera pose from '{}': {}",
            DemoCameraStatePath,
            error.what());
        state.position.reset();
        state.rotation.reset();
    }
    return state;
}

void SaveDemoCameraState(Scene& scene, bool rememberCameraPose) {
    nlohmann::json document{
        { "rememberCameraPose", rememberCameraPose }
    };

    if (rememberCameraPose && scene.HasUsablePrimaryCamera()) {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT4 rotation;
        DirectX::XMStoreFloat3(
            &position,
            scene.GetPrimaryCamera().get<Components::Position>().pos);
        DirectX::XMStoreFloat4(
            &rotation,
            DirectX::XMQuaternionNormalize(
                scene.GetPrimaryCamera().get<Components::Rotation>().rot));
        document["position"] = { position.x, position.y, position.z };
        document["rotation"] = { rotation.x, rotation.y, rotation.z, rotation.w };
    }

    std::ofstream output(DemoCameraStatePath.data(), std::ios::trunc);
    if (!output) {
        spdlog::warn(
            "Could not open '{}' to save the demo camera pose",
            DemoCameraStatePath);
        return;
    }
    output << document.dump(2) << '\n';
    if (!output) {
        spdlog::warn(
            "Could not write the demo camera pose to '{}'",
            DemoCameraStatePath);
    }
}

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd) {
    //tracy::SetThreadName("Main");

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // What the demo loads (shaders, textures) and writes (logs, cache) is beside the exe: run from there, however it was
    // started (a shortcut's working folder, another directory's command line).
    {
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) != 0) {
            std::error_code error;
            std::filesystem::current_path(std::filesystem::path(exePath).parent_path(), error);
        }
    }
    std::filesystem::path logPath = "logs/log.txt";
    char* configuredLogPath = nullptr;
    size_t configuredLogPathLength = 0;
    if (_dupenv_s(&configuredLogPath, &configuredLogPathLength, "BASICRENDERER_LOG_PATH") == 0 &&
        configuredLogPath != nullptr && configuredLogPath[0] != '\0') {
        logPath = configuredLogPath;
    }
    std::free(configuredLogPath);
    if (const auto parentPath = logPath.parent_path(); !parentPath.empty()) {
        std::filesystem::create_directories(parentPath);
    }
    std::filesystem::remove(logPath);
    auto file_logger = spdlog::basic_logger_mt("file_logger", logPath.string(), true);
    spdlog::set_default_logger(file_logger);
    file_logger->flush_on(spdlog::level::info);

    const std::string_view commandLine = lpCmdLine ? std::string_view(lpCmdLine) : std::string_view{};
    const DemoCameraState cameraState = LoadDemoCameraState();
    DemoStatisticalSamplingRun statisticalSampling;
    if (const auto samplingConfig = FindCommandLinePath(commandLine, "--sampling-config")) {
        std::string samplingError;
        const auto samplingPipe = FindCommandLineValue(commandLine, "--sampling-control-pipe");
        if (!statisticalSampling.Initialize(*samplingConfig, samplingPipe, samplingError)) {
            spdlog::critical("BasicRenderer sampling initialization failed: {}", samplingError);
            return 2;
        }
        // Streamline may open its own NVIDIA device-level sampling session.
        // NVPerf replay capture requires exclusive ownership of that context.
        // BASICRENDERER_SAMPLING_KEEP_STREAMLINE=1 keeps it (and so DLSS and Ray
        // Reconstruction) for a control-pipe run whose config doesn't use NVPerf.
        const char* keepStreamline = std::getenv("BASICRENDERER_SAMPLING_KEEP_STREAMLINE");
        if (statisticalSampling.UsesNvPerf() || !keepStreamline || keepStreamline[0] != '1') {
            _putenv_s("BASICRENDERER_DISABLE_STREAMLINE", "1");
        }
        // Device creation happens after sampling configuration is loaded. Mirror
        // environment-triggered NVPerf startup so diagnostics builds do not enable
        // the D3D12 debug layer, which makes Queue_BeginSession invalid.
        if (statisticalSampling.UsesNvPerf()) {
            _putenv_s("BASICRENDERER_NVPERF_CAPTURE", "1");
        }
    }
    const bool pipelineReplacementSmokeTest =
        commandLine.find("--pipeline-replacement-smoke-test") != std::string_view::npos;
    const bool clodGraphRebuildSmokeTest =
        commandLine.find("--clod-graph-rebuild-smoke-test") != std::string_view::npos;
    const bool clodStreamingCleanupTest =
        commandLine.find("--clod-streaming-cleanup-test") != std::string_view::npos;
    const bool clodVsmCpuBenchmark =
        commandLine.find("--clod-vsm-cpu-benchmark") !=
        std::string_view::npos;
    const bool clodStreamingStressTest =
        commandLine.find("--clod-streaming-stress-test") != std::string_view::npos ||
        clodStreamingCleanupTest;
    const bool vsmOrbitTest =
        commandLine.find("--vsm-orbit-test") != std::string_view::npos;
    const bool vsmRetreatTest =
        commandLine.find("--vsm-retreat-test") != std::string_view::npos;
    const bool vsmPageStateTest =
        commandLine.find("--vsm-page-state") != std::string_view::npos;
    const bool vsmRerenderedTest =
        commandLine.find("--vsm-rerendered") != std::string_view::npos;
    const bool graphRebuildSmokeTest =
        pipelineReplacementSmokeTest || clodGraphRebuildSmokeTest || clodStreamingStressTest;

    ConfigureMainRenderThreadScheduling();

    crashlog::InstallTerminateHandler();

    static spdlog_streambuf sci{ file_logger };
    std::cout.rdbuf(&sci);
    std::cerr.rdbuf(&sci);

    HINSTANCE hGetPixDLL = LoadLibrary(L"WinPixEventRuntime.dll");

    if (!hGetPixDLL) {
        spdlog::warn("could not load the PIX library");
    }
#if BUILD_TYPE == BUILD_TYPE_DEBUG
    HMODULE pixLoaded = PIXLoadLatestWinPixGpuCapturerLibrary();
    if (!pixLoaded) {
        // Print the error code for debugging purposes
        spdlog::warn("Could not load PIX! Error: {}", GetLastError());
    }
#endif

    SetDllDirectoryA(".\\D3D\\");

    g_rendererInputDisabled = commandLine.find("--no-input") != std::string_view::npos;
    if (g_rendererInputDisabled) spdlog::info("--no-input: keyboard and mouse input are ignored");
    HWND hwnd = InitWindow(hInstance, nShowCmd);

    spdlog::info("initializing renderer...");
    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    UINT x_res = clientRect.right - clientRect.left;
    UINT y_res = clientRect.bottom - clientRect.top;

    // Initialize Nvidia Streamline

    if (clodStreamingStressTest) {
        // Keep the coordinator stress signal independent of vendor upscaler
        // lifecycle behavior across repeated graph replacement.
        _putenv_s("BASICRENDERER_DISABLE_STREAMLINE", "1");
    }
    renderer.Initialize(hwnd, x_res, y_res, br::pipeline::MakeBasicRendererDemoPipeline());
    spdlog::info("Renderer initialized.");
    br::extensions::MakeSettingSetter<CLodTransparencyMode>(
        CLodTransparencyModeSettingName)(CLodTransparencyMode::AVBOIT);
    spdlog::info("CLOD transparency mode: AVBOIT");
    br::extensions::MakeSettingSetter<bool>("rememberCameraPose")(
        cameraState.rememberCameraPose);
    if (const char* outputTypeValue = std::getenv("SARP_DEBUG_OUTPUT_TYPE");
        outputTypeValue && outputTypeValue[0] != '\0') {
        char* end = nullptr;
        const auto parsed = std::strtoul(outputTypeValue, &end, 10);
        if (end != outputTypeValue && *end == '\0' && parsed < OutputTypeNames.size()) {
            br::extensions::MakeSettingSetter<unsigned int>("outputType")(
                static_cast<unsigned int>(parsed));
            spdlog::info("Debug output type selected from SARP_DEBUG_OUTPUT_TYPE: {}", parsed);
        }
    }
    if (vsmPageStateTest) {
        br::extensions::MakeSettingSetter<unsigned int>("outputType")(
            static_cast<unsigned int>(OutputType::VSM_PAGE_STATE));
    }
    if (vsmRerenderedTest) {
        br::extensions::MakeSettingSetter<unsigned int>("outputType")(
            static_cast<unsigned int>(OutputType::VSM_RERENDERED_THIS_FRAME));
    }
    if (clodStreamingStressTest) {
        br::extensions::MakeSettingSetter<uint32_t>(
            "clodStreamingCpuUploadBudgetRequests")(256u);
    }
    if (clodVsmCpuBenchmark) {
        spdlog::info(
            "CLOD VSM CPU benchmark armed: deterministic residency camera "
            "sweeps through frame 480 with graph rebuilds disabled.");
    }
    if (graphRebuildSmokeTest) {
        br::extensions::MakeSettingSetter<bool>("renderGraphCompileDumpEnabled")(true);
        ResetD3D12SmokeValidationMessages();
        if (clodStreamingStressTest) {
            spdlog::info("CLod streaming stress test armed: deterministic camera sweeps, teleports, and graph rebuilds through frame 1200.");
        }
        else if (pipelineReplacementSmokeTest) {
            spdlog::info("Pipeline replacement smoke test armed: bloom will toggle off/on/off at frames 120/240/360.");
        }
        else {
            spdlog::info("CLod graph-rebuild smoke test armed: occlusion culling will toggle off/on/off at frames 120/240/360.");
        }
    }
    renderer.SetInputMode(InputMode::wasd);
    std::unique_ptr<DemoStaticScene> staticScene;

    {
        org::BufferBase::ScopedBackingMutation startupSceneBackingMutation;

        // Loaded models' static parts stream through the static import runtime, like SARP does. Scenarios
        // (--script) place their models as dynamic renderables, which also rely on this runtime.
        renderer.SetCurrentScene(std::make_shared<Scene>());
        staticScene = std::make_unique<DemoStaticScene>();
        // Scene Explorer's Load Model adds to the running static scene. The menu calls it on this thread.
        SettingsManager::GetInstance().getSettingSetter<std::function<void(const std::string&)>>("loadModel")(
            [scene = staticScene.get()](const std::string& path) {
                for (const auto& model : ExpandDemoModelPath(path)) scene->AddModel(model, XMMatrixIdentity());
            });

        renderer.SetEnvironment("sky");

        XMFLOAT3 pos = XMFLOAT3(-3.0f, 5.0f, 1.0f);
        XMFLOAT3 lookAt = XMFLOAT3(-3.0f, 5.0f, -0.125f);
        XMFLOAT3 up = XMFLOAT3(0.0f, 1.0f, 0.0f);
        float fov = 80.0f * (XM_PI / 180.0f); // Converting degrees to radians
        float aspectRatio;
        float zNear = 0.1f;
        float zFar = 1000.0f;


        int clientWidth = x_res; // TODO
        int clientHeight = y_res; // TODO

        aspectRatio = static_cast<float>(clientWidth) / static_cast<float>(clientHeight);
        auto& scene = renderer.GetCurrentScene();
        if (cameraState.rememberCameraPose
            && cameraState.position
            && cameraState.rotation) {
            pos = *cameraState.position;
            const DirectX::XMVECTOR savedRotation =
                DirectX::XMLoadFloat4(&*cameraState.rotation);
            DirectX::XMStoreFloat3(
                &up,
                DirectX::XMVector3Rotate(
                    DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
                    savedRotation));
            DirectX::XMFLOAT3 forward;
            DirectX::XMStoreFloat3(
                &forward,
                DirectX::XMVector3Rotate(
                    DirectX::XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f),
                    savedRotation));
            lookAt = {
                pos.x + forward.x,
                pos.y + forward.y,
                pos.z + forward.z };
            spdlog::info(
                "Restoring demo camera pose from '{}'",
                DemoCameraStatePath);
        }
        scene->SetCamera(pos, lookAt, up, fov, aspectRatio, zNear, zFar);
    
	    auto light = renderer.GetCurrentScene()->CreateDirectionalLightECS(L"light1", XMFLOAT3(1, 1, 1), 10.0, XMFLOAT3(-0.1, -0.5, -0.9));
        //auto light3 = renderer.GetCurrentScene()->CreateSpotLightECS(L"light3", XMFLOAT3(0, 10, 3), XMFLOAT3(1, 1, 1), 2000.0, {0, -1, 0}, .5, .8);
        //auto light1 = renderer.GetCurrentScene()->CreatePointLightECS(L"light1", XMFLOAT3(0, 1, 3), XMFLOAT3(1, 1, 1), 100.0);
        // BASICRENDERER_DEMO_SHADOW_LIGHT=spot|point (or spot-unshadowed|point-unshadowed): a shadow-casting local light beside the camera, aimed where it looks,
        // for checking spot and point light virtual shadow maps.
        if (const char* demoShadowLight = std::getenv("BASICRENDERER_DEMO_SHADOW_LIGHT")) {
            const XMVECTOR eye = XMLoadFloat3(&pos);
            const XMVECTOR forward = XMVector3Normalize(XMVectorSubtract(XMLoadFloat3(&lookAt), eye));
            XMFLOAT3 lightPosition;
            XMStoreFloat3(&lightPosition, XMVectorAdd(eye, XMVectorSet(1.5f, 1.0f, 0.0f, 0.0f)));
            XMFLOAT3 lightDirection;
            XMStoreFloat3(&lightDirection, XMVector3Normalize(XMVectorAdd(forward, XMVectorSet(0.0f, -0.3f, 0.0f, 0.0f))));
            // BASICRENDERER_DEMO_SHADOW_LIGHT_POSE="x,y,z,dx,dy,dz" places it instead (a remembered camera pose is not
            // known here).
            if (const char* pose = std::getenv("BASICRENDERER_DEMO_SHADOW_LIGHT_POSE")) {
                float v[6] = {};
                if (std::sscanf(pose, "%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
                    lightPosition = XMFLOAT3(v[0], v[1], v[2]);
                    XMStoreFloat3(&lightDirection, XMVector3Normalize(XMVectorSet(v[3], v[4], v[5], 0.0f)));
                }
            }
            if (std::string_view(demoShadowLight).starts_with("point")) {
                // "point-unshadowed": the same light without shadows, for comparison.
                renderer.GetCurrentScene()->CreatePointLightECS(L"demoShadowPoint", lightPosition, XMFLOAT3(1.0f, 0.8f, 0.6f), 3000.0f, 30.0f, 0.05f,
                    std::string_view(demoShadowLight) != "point-unshadowed");
            }
            else {
                // "spot-unshadowed": the same light without shadows, for comparison.
                renderer.GetCurrentScene()->CreateSpotLightECS(L"demoShadowSpot", lightPosition, XMFLOAT3(1.0f, 0.8f, 0.6f), 2000.0f,
                    lightDirection, 0.35f, 0.5f, 40.0f, 0.05f, std::string_view(demoShadowLight) != "spot-unshadowed");
            }
        }
    
        for (int i = 0; i < 0; i++) {
		    auto point = getRandomPointInVolume(-20, 20, -2, 0, -20, 20);
		    auto color = XMFLOAT3(randomFloat(0.0, 1.0), randomFloat(0.0, 1.0), randomFloat(0.0, 1.0));
            auto light1 = renderer.GetCurrentScene()->CreatePointLightECS(L"light"+std::to_wstring(i), XMFLOAT3(point.x, point.y, point.z), color, 3.0, 0.0f, 0.0f, false);
        }
    }

    // --no-scene-overlap: the scene update runs on this thread inside Update, not overlapped with the previous frame's
    // rendering, so a change reaches the very next frame. Switched after the first frames (the switch syncs the scene
    // bridge, which the first frame sets up).
    const bool noSceneOverlap = HasCommandLineOption(commandLine, "--no-scene-overlap");

    // The control commands, for the pipe and for scripts. The pipe also runs without a sampling configuration.
    statisticalSampling.RegisterControlCommands(renderer, hwnd);
    const auto controlPipe = FindCommandLineValue(commandLine, "--sampling-control-pipe");
    if (controlPipe && !statisticalSampling.Enabled()) statisticalSampling.StartControlServer(*controlPipe);

#if BASICRENDERER_HAS_SCRIPTING
    // After scene setup: the scenario compiles in the background while the first frames render.
    DemoScriptRun scriptRun;
    scriptRun.Start(commandLine, hwnd, renderer, statisticalSampling.Commands(), controlPipe.has_value());
#endif

    MSG msg = {};
    unsigned int frameIndex = 0;
    uint64_t clodStressTelemetrySequence = 0;
    uint32_t clodStressResidentAtLastMove = 0;
    bool clodStressFailed = false;
    auto lastUpdateTime = std::chrono::system_clock::now();
    // --exit-after-seconds <n>: quit on its own, for scripted runs.
    std::optional<std::chrono::steady_clock::time_point> exitAt;
    if (const auto seconds = FindCommandLineValue(commandLine, "--exit-after-seconds"))
        exitAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int64_t>(std::stod(*seconds) * 1000.0));
    while (msg.message != WM_QUIT) {
        if (exitAt && std::chrono::steady_clock::now() >= *exitAt) {
            exitAt.reset();
            PostQuitMessage(0);
        }
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else {

            auto currentTime = std::chrono::system_clock::now();
            std::chrono::duration<float> elapsedSeconds = currentTime - lastUpdateTime;
            lastUpdateTime = currentTime;

            frameIndex += 1;
            if (vsmRetreatTest) {
                // Hold the initial close view long enough to populate its
                // clipmaps, then move straight backwards while continuing to
                // look at the same point. This deterministically exercises
                // the transition to cached coarser VSM pages.
                constexpr uint32_t kRetreatStartFrame = 180u;
                constexpr uint32_t kRetreatEndFrame = 540u;
                const float retreatT = std::clamp(
                    static_cast<float>(
                        static_cast<int64_t>(frameIndex) -
                        static_cast<int64_t>(kRetreatStartFrame)) /
                        static_cast<float>(
                            kRetreatEndFrame - kRetreatStartFrame),
                    0.0f,
                    1.0f);
                const XMFLOAT3 retreatPosition{
                    0.0f,
                    5.0f,
                    4.0f + retreatT * 36.0f};
                const XMFLOAT3 retreatTarget{0.0f, 5.0f, 0.0f};
                const XMFLOAT3 retreatUp{0.0f, 1.0f, 0.0f};
                const XMMATRIX retreatView = XMMatrixLookAtRH(
                    XMLoadFloat3(&retreatPosition),
                    XMLoadFloat3(&retreatTarget),
                    XMLoadFloat3(&retreatUp));
                const XMMATRIX retreatModel =
                    XMMatrixInverse(nullptr, retreatView);
                const XMVECTOR retreatRotation =
                    XMQuaternionNormalize(
                        XMQuaternionRotationMatrix(retreatModel));
                auto& retreatCamera =
                    renderer.GetCurrentScene()->GetPrimaryCamera();
                retreatCamera
                    .set<Components::Position>(
                        {retreatPosition.x,
                         retreatPosition.y,
                         retreatPosition.z})
                    .set<Components::Rotation>(retreatRotation)
                    .set<Components::Matrix>(retreatModel);
            }
            else if (vsmOrbitTest) {
                constexpr float kOrbitRadius = 8.0f;
                constexpr float kOrbitCenterY = 5.0f;
                // About one revolution every twelve seconds at 60 Hz. This
                // remains centered on the scene instead of eventually flying
                // beyond its geometry like a held movement key.
                const float angle =
                    static_cast<float>(frameIndex % 720u) *
                    (XM_2PI / 720.0f);
                const XMFLOAT3 orbitPosition{
                    std::cos(angle) * kOrbitRadius,
                    kOrbitCenterY + std::sin(angle * 2.0f) * 1.5f,
                    std::sin(angle) * kOrbitRadius};
                const XMFLOAT3 orbitTarget{
                    0.0f,
                    kOrbitCenterY,
                    0.0f};
                const XMFLOAT3 orbitUp{0.0f, 1.0f, 0.0f};
                const XMMATRIX orbitView = XMMatrixLookAtRH(
                    XMLoadFloat3(&orbitPosition),
                    XMLoadFloat3(&orbitTarget),
                    XMLoadFloat3(&orbitUp));
                const XMMATRIX orbitModel =
                    XMMatrixInverse(nullptr, orbitView);
                const XMVECTOR orbitRotation =
                    XMQuaternionNormalize(
                        XMQuaternionRotationMatrix(orbitModel));
                auto& orbitCamera =
                    renderer.GetCurrentScene()->GetPrimaryCamera();
                orbitCamera
                    .set<Components::Position>(
                        {orbitPosition.x,
                         orbitPosition.y,
                         orbitPosition.z})
                    .set<Components::Rotation>(orbitRotation)
                    .set<Components::Matrix>(orbitModel);
            }
            if (clodStreamingStressTest) {
                constexpr float kStressFov = 80.0f * (XM_PI / 180.0f);
                constexpr float kStressAspect = 16.0f / 9.0f;
                const uint32_t leg = frameIndex / 300u;
                const float phase = static_cast<float>((frameIndex / 30u) % 10u) / 10.0f;
                const float angle = phase * XM_2PI + static_cast<float>(leg) * 0.73f;
                const float radius = (leg & 1u) ? 12.0f : 4.0f;
                const XMFLOAT3 stressPosition{
                    2.0f + std::cos(angle) * radius,
                    5.0f + ((leg % 3u) == 2u ? 4.0f : 0.0f),
                    std::sin(angle) * radius};
                if (frameIndex == 1u || frameIndex % 30u == 0u) {
                    const XMFLOAT3 stressTarget{0.0f, 5.0f, 0.0f};
                    const XMFLOAT3 stressUp{0.0f, 1.0f, 0.0f};
                    const XMMATRIX stressView = XMMatrixLookAtRH(
                        XMLoadFloat3(&stressPosition),
                        XMLoadFloat3(&stressTarget),
                        XMLoadFloat3(&stressUp));
                    const XMMATRIX stressModel = XMMatrixInverse(nullptr, stressView);
                    const XMVECTOR stressRotation = XMQuaternionNormalize(
                        XMQuaternionRotationMatrix(stressModel));
                    auto& stressCamera =
                        renderer.GetCurrentScene()->GetPrimaryCamera();
                    auto stressCameraComponent =
                        stressCamera.get<Components::Camera>();
                    stressCameraComponent.fov = kStressFov;
                    stressCameraComponent.aspect = kStressAspect;
                    stressCameraComponent.zNear = 0.1f;
                    stressCameraComponent.zFar = 1000.0f;
                    stressCamera
                        .set<Components::Camera>(stressCameraComponent)
                        .set<Components::Position>(
                            {stressPosition.x,
                             stressPosition.y,
                             stressPosition.z})
                        .set<Components::Rotation>(stressRotation)
                        .set<Components::Matrix>(stressModel);
                }

                if (frameIndex % 120u == 0u) {
                    CLodStreamingOperationStats stats{};
                    if (TryReadCLodStreamingOperationStats(clodStressTelemetrySequence, stats)) {
                        spdlog::info(
                            "CLod stress telemetry frame={}: resident={} pendingCpu={} diskIo={} pendingCommit={} ready={} maxCommitAge={} streamedBytes={}",
                            frameIndex,
                            stats.residentGroups,
                            stats.pendingCpuRequests,
                            stats.diskIoRequests,
                            stats.pendingCommitGroups,
                            stats.readyCompletions,
                            stats.pendingCommitMaxAgeTicks,
                            stats.streamedBytesThisFrame);
                        if (stats.pendingCommitGroups != 0u && stats.pendingCommitMaxAgeTicks > 120u) {
                            clodStressFailed = true;
                            spdlog::error("CLod stress invariant failed: pending commit age {} exceeds 120 ticks", stats.pendingCommitMaxAgeTicks);
                        }
                        clodStressResidentAtLastMove = stats.residentGroups;
                    }
                }
            }
            else if (clodVsmCpuBenchmark) {
                // Follow a smooth, projection-preserving arc around the
                // normal demo view. Unlike the streaming stress test, this
                // never teleports the camera, forces an aspect ratio, or
                // places the camera near the scene origin.
                constexpr XMFLOAT3 benchmarkTarget{
                    100.0f,
                    5.0f,
                    0.0f};
                constexpr float benchmarkRadius = 65.0f;
                const float cycle =
                    static_cast<float>(frameIndex % 480u) / 480.0f;
                const float sweepAngle =
                    XM_PI + std::sin(cycle * XM_2PI) * 0.25f;
                const XMFLOAT3 benchmarkPosition{
                    benchmarkTarget.x +
                        std::cos(sweepAngle) * benchmarkRadius,
                    10.0f +
                        std::sin(cycle * XM_2PI * 2.0f) * 2.0f,
                    benchmarkTarget.z +
                        std::sin(sweepAngle) * benchmarkRadius};
                const XMFLOAT3 benchmarkUp{0.0f, 1.0f, 0.0f};
                const XMMATRIX benchmarkView = XMMatrixLookAtRH(
                    XMLoadFloat3(&benchmarkPosition),
                    XMLoadFloat3(&benchmarkTarget),
                    XMLoadFloat3(&benchmarkUp));
                const XMMATRIX benchmarkModel =
                    XMMatrixInverse(nullptr, benchmarkView);
                const XMVECTOR benchmarkRotation =
                    XMQuaternionNormalize(
                        XMQuaternionRotationMatrix(benchmarkModel));
                auto& benchmarkCamera =
                    renderer.GetCurrentScene()->GetPrimaryCamera();
                benchmarkCamera
                    .set<Components::Position>(
                        {benchmarkPosition.x,
                         benchmarkPosition.y,
                         benchmarkPosition.z})
                    .set<Components::Rotation>(benchmarkRotation)
                    .set<Components::Matrix>(benchmarkModel);
            }
            if (graphRebuildSmokeTest && frameIndex == 120) {
                if (pipelineReplacementSmokeTest) {
                    spdlog::info("Pipeline replacement smoke test: disabling bloom at frame {}.", frameIndex);
                    br::extensions::MakeSettingSetter<bool>("enableBloom")(false);
                }
                else {
                    spdlog::info("CLod graph-rebuild smoke test: disabling occlusion culling at frame {}.", frameIndex);
                    br::extensions::MakeSettingSetter<bool>("enableOcclusionCulling")(false);
                }
                if (clodStreamingStressTest) {
                    br::extensions::MakeSettingSetter<bool>("enableBloom")(false);
                }
            }
            if (graphRebuildSmokeTest && frameIndex == 240) {
                if (pipelineReplacementSmokeTest) {
                    spdlog::info("Pipeline replacement smoke test: enabling bloom at frame {}.", frameIndex);
                    br::extensions::MakeSettingSetter<bool>("enableBloom")(true);
                }
                else {
                    spdlog::info("CLod graph-rebuild smoke test: enabling occlusion culling at frame {}.", frameIndex);
                    br::extensions::MakeSettingSetter<bool>("enableOcclusionCulling")(true);
                }
                if (clodStreamingStressTest) {
                    br::extensions::MakeSettingSetter<bool>("enableBloom")(true);
                }
            }
            if (graphRebuildSmokeTest && frameIndex == 360) {
                if (pipelineReplacementSmokeTest) {
                    spdlog::info("Pipeline replacement smoke test: disabling bloom again at frame {}.", frameIndex);
                    br::extensions::MakeSettingSetter<bool>("enableBloom")(false);
                }
                else {
                    spdlog::info("CLod graph-rebuild smoke test: disabling occlusion culling again at frame {}.", frameIndex);
                    br::extensions::MakeSettingSetter<bool>("enableOcclusionCulling")(false);
                }
            }
            if (graphRebuildSmokeTest && !clodStreamingStressTest && frameIndex == 480) {
                spdlog::info("Graph-rebuild smoke test completed after {} frames; closing.", frameIndex);
                PostMessage(hwnd, WM_CLOSE, 0, 0);
            }
            if (clodStreamingStressTest && (frameIndex == 600 || frameIndex == 840)) {
                const bool enabled = frameIndex == 840;
                spdlog::info("CLod stress: graph rebuild at frame {} bloom={} occlusion={}", frameIndex, enabled, enabled);
                br::extensions::MakeSettingSetter<bool>("enableBloom")(enabled);
                br::extensions::MakeSettingSetter<bool>("enableOcclusionCulling")(enabled);
            }
            if (clodStreamingStressTest && frameIndex == 1200) {
                spdlog::info("CLod streaming stress test completed after {} frames; closing.", frameIndex);
                PostMessage(hwnd, WM_CLOSE, 0, 0);
            }
            if (clodVsmCpuBenchmark && frameIndex == 480) {
                spdlog::info(
                    "CLOD VSM CPU benchmark completed after {} frames; "
                    "closing.",
                    frameIndex);
                PostMessage(hwnd, WM_CLOSE, 0, 0);
            }
            if (clodStreamingCleanupTest && frameIndex == 180) {
                spdlog::info("CLod streaming cleanup test reached frame {} after graph replacement; closing.", frameIndex);
                PostMessage(hwnd, WM_CLOSE, 0, 0);
            }
            if (noSceneOverlap && frameIndex == 3) {
                renderer.SetSceneRenderOverlapEnabled(false);
                spdlog::info("--no-scene-overlap: scene updates run synchronously");
            }
            if (staticScene) staticScene->Tick(renderer, frameIndex);
            float frameSeconds = elapsedSeconds.count();
#if BASICRENDERER_HAS_SCRIPTING
            // --script-fixed-dt: the scenario and the renderer both advance by exactly this much per frame.
            if (const auto fixedDelta = scriptRun.FixedDeltaSeconds()) frameSeconds = static_cast<float>(*fixedDelta);
            scriptRun.Tick(frameIndex, frameSeconds);
#endif
            renderer.Update(frameSeconds);
            if (frameIndex % 100 == 0) {
                spdlog::info("FPS: {}", 1 / elapsedSeconds.count());
            }
            renderer.Render();
            statisticalSampling.AfterFrame(renderer, hwnd);
        }
    }

    const bool smokeValidationFailed = graphRebuildSmokeTest && D3D12SmokeValidationFailed();
    bool keepCameraState = true;
#if BASICRENDERER_HAS_SCRIPTING
    keepCameraState = !scriptRun.RanScenario();   // a scenario's camera is not the user's pose to remember
#endif
    if (const auto& scene = renderer.GetCurrentScene(); scene && keepCameraState) {
        SaveDemoCameraState(
            *scene,
            br::extensions::MakeSettingGetter<bool>("rememberCameraPose")());
    }
    // The static import runtime stops before the renderer whose services it uses.
    if (staticScene) {
        SettingsManager::GetInstance().getSettingSetter<std::function<void(const std::string&)>>("loadModel")(
            [](const std::string& path) { spdlog::warn("Load Model: the static scene has shut down; '{}' was not loaded", path); });
    }
    staticScene.reset();
#if BASICRENDERER_HAS_SCRIPTING
    scriptRun.Shutdown();
#endif
    renderer.Cleanup();

#if BASICRENDERER_HAS_SCRIPTING
    if (const auto scriptExitCode = scriptRun.ExitCode()) return *scriptExitCode;
#endif

    if (statisticalSampling.ExitCode() != 0) return statisticalSampling.ExitCode();
    return (smokeValidationFailed || clodStressFailed) ? 1 : 0;
}

// Window callback procedure
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {

	const bool imguiHandled = renderer.HandleMenuInput(hWnd, message, wParam, lParam);
    const bool blockRendererInput = IsRendererInputMessage(message) && ShouldBlockRendererInputForImGui(message);

    if (IsRendererInputMessage(message) && !blockRendererInput && !g_rendererInputDisabled) {
        renderer.GetInputManager().ProcessInput(message, wParam, lParam);
    }

    if (imguiHandled) {
        return 0;
    }

    switch (message)
    {
    case WM_INPUT:
        //ProcessRawInput(lParam);
        break;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            UINT newWidth = LOWORD(lParam);
            UINT newHeight = HIWORD(lParam);
            if (renderer.IsInitialized()) {
                renderer.OnResize(newWidth, newHeight);
            }
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (toupper(static_cast<int>(wParam)) == VK_ESCAPE) {
            PostQuitMessage(0);
        }
#if BASICRENDERER_HAS_SCRIPTING
        if (wParam == VK_F5) g_scriptReloadRequested = true;
#endif
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}
std::uint64_t DemoStatisticalSamplingRun::ElapsedMs() const
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_startedAt).count());
    }

void DemoStatisticalSamplingRun::StartExperiment(std::string label, Renderer* renderer)
    {
        if (renderer) renderer->SetDeterministicSamplingMode(false);
        br::telemetry::nvperf::SetStreamingSuppressed(false);
        m_phase = Phase::Warmup;
        m_phaseFrames = 0;
        m_readyStableFrames = 0;
        m_readinessSignature.clear();
        m_lastRenderGraphSampleSerial = 0;
        m_nextOrdinal = 1;
        m_samples.clear();
        m_summaries.clear();
        m_stoppingReason.clear();
        m_readinessDurationMs = 0;
        m_finished = false;
        m_finalized = false;
        m_exitCode = 0;
        m_startedAt = std::chrono::steady_clock::now();
        m_experimentId = m_database->BeginExperiment(*m_configuration);
        m_database->RecordDiagnostic(m_experimentId, "configuration_path", m_configurationPath.string());
        m_database->RecordDiagnostic(m_experimentId, "label", label);
        m_database->RecordDiagnostic(m_experimentId, "process_id", std::to_string(GetCurrentProcessId()));
        spdlog::info(
            "BasicRenderer sampling: experiment={} label='{}' waiting for {} stable frames; settling={} samples={}..{} pid={}",
            m_experimentId, label, m_configuration->readyFrames, m_configuration->settlingFrames,
            m_configuration->minimumSamples, m_configuration->maximumSamples, GetCurrentProcessId());
    }

const char* DemoStatisticalSamplingRun::PhaseName() const
    {
        switch (m_phase) {
        case Phase::Warmup: return "waiting_for_stability";
        case Phase::Settling: return "settling";
        case Phase::Capturing: return "capturing_nvperf";
        case Phase::RenderGraphSampling: return "sampling_render_graph";
        }
        return "unknown";
    }

void DemoStatisticalSamplingRun::RegisterControlCommands(Renderer& renderer, HWND hwnd)
    {
        // The demo's control commands (TESTING.md). The pipe and C# scenarios (ctx.Command) both reach them through m_commands.
        m_commands.Register("session.status", [this, &renderer, hwnd](const br::diagnostics::ControlRequest& request) {
            nlohmann::json response = nlohmann::json::object();
            response["pid"] = GetCurrentProcessId();
            response["profile_active"] = !m_finished;
            response["experiment_id"] = m_experimentId;
            response["pipeline_epoch"] = br::diagnostics::GetPipelineEpoch();
            return response;
        });
        m_commands.Register("shadercache.stats", [this, &renderer, hwnd](const br::diagnostics::ControlRequest& request) {
            nlohmann::json response = nlohmann::json::object();
            // The shader store's batch summary and sizes.
            response["shader_store"] = nlohmann::json::parse(br::diagnostics::DescribeShaderStore());
            return response;
        });
        m_commands.Register("pso.list", [this, &renderer, hwnd](const br::diagnostics::ControlRequest& request) {
            nlohmann::json response = nlohmann::json::object();
            response["pipelines"] = nlohmann::json::array();
            for (const auto& pipeline : br::diagnostics::ListPipelines()) {
                response["pipelines"].push_back({
                    { "id", pipeline.id },
                    { "name", pipeline.displayName },
                    { "active_generation", pipeline.activeGeneration },
                    { "source_hash", pipeline.sourceHash },
                    { "bytecode_hash", pipeline.bytecodeHash },
                    { "label", pipeline.label },
                    { "compiling", pipeline.compiling }
                });
            }
            return response;
        });
        m_commands.Register("pso.recompile", [this, &renderer, hwnd](const br::diagnostics::ControlRequest& request) {
            nlohmann::json response = nlohmann::json::object();
            if (!m_finished) throw std::runtime_error("cannot recompile a pipeline while profiling is active");
            br::diagnostics::RecompileOptions options;
            options.label = request.args.value("label", "pipe-reload");
            if (const auto defines = request.args.find("defines");
                defines != request.args.end()) {
                if (!defines->is_object()) {
                    throw std::runtime_error("pso.recompile 'defines' must be an object");
                }
                for (const auto& [name, value] : defines->items()) {
                    if (!value.is_string()) {
                        throw std::runtime_error(
                            "pso.recompile define values must be strings");
                    }
                    options.defineOverrides.emplace(
                        std::wstring(name.begin(), name.end()),
                        std::wstring(value.get_ref<const std::string&>().begin(),
                                     value.get_ref<const std::string&>().end()));
                }
            }
            response["job_id"] = br::diagnostics::RequestRecompile(
                request.args.at("pipeline_id").get<std::string>(), std::move(options));
            return response;
        });
        m_commands.Register("pso.activate", [this, &renderer, hwnd](const br::diagnostics::ControlRequest& request) {
            nlohmann::json response = nlohmann::json::object();
            if (!m_finished) throw std::runtime_error("cannot activate a pipeline while profiling is active");
            response["job_id"] = br::diagnostics::RequestActivation(
                request.args.at("pipeline_id").get<std::string>(),
                request.args.at("generation").get<std::uint64_t>());
            return response;
        });
        m_commands.Register("capture.hdr", [this, &renderer, hwnd](const br::diagnostics::ControlRequest& request) {
            nlohmann::json response = nlohmann::json::object();
            response["job_id"] = m_hdrCaptures.Request(renderer, request.args);
            return response;
        });
        {
            const br::diagnostics::ControlCommandRegistry::Handler handler = [this, &renderer, hwnd](const br::diagnostics::ControlRequest& request) {
                nlohmann::json response = nlohmann::json::object();
                // The physical lens (SARP's docs/lens_simulation.md). lens.set takes mode, file, focus_m, focus_mode ("manual" or
                // "auto"), af_point ([x, y] NDC), af_region, af_speed, monochromatic, f_number, field_mode ("remap" or "physical")
                // and polynomial_degree. Focus is per frame; the rest builds off this thread, so lens.get's "state" says when it
                // is in use, and its "focus" has the autofocus state.
                response["lens"] = nlohmann::json::parse(renderer.HandleLensControl(std::string(request.command), request.args.dump()));
                return response;
            };
            m_commands.Register("lens.get", handler);
            m_commands.Register("lens.set", handler);
            m_commands.Register("lens.selftest", handler);
            m_commands.Register("lens.selftest.status", handler);
        }
        {
            const br::diagnostics::ControlCommandRegistry::Handler handler = [this, &renderer, hwnd](const br::diagnostics::ControlRequest& request) {
                nlohmann::json response = nlohmann::json::object();
                // The lens's ghost flares (SARP's docs/lens_simulation.md "Ghost flares"): lens.flare.get, lens.flare.set
                // {enabled, max_ghosts, grid, wavelengths, max_lights, intensity, test_source}, lens.flare.selftest and
                // lens.flare.selftest.status.
                response["flare"] = nlohmann::json::parse(renderer.HandleLensControl(std::string(request.command), request.args.dump()));
                return response;
            };
            m_commands.Register("lens.flare.get", handler);
            m_commands.Register("lens.flare.set", handler);
            m_commands.Register("lens.flare.selftest", handler);
            m_commands.Register("lens.flare.selftest.status", handler);
        }
        m_commands.Register("upscaler.get", [this, &renderer, hwnd](const br::diagnostics::ControlRequest& request) {
            nlohmann::json response = nlohmann::json::object();
            // What upscaler runs: "evaluated" is what the last frame's upscaling pass ran, "graph" the mode the graph
            // was built with, "setting" the upscalingMode setting. A path traced frame should say DLSS Ray
            // Reconstruction in all three unless pathTracingAccumulate is on.
            response["upscaler"] = nlohmann::json::parse(renderer.DescribeUpscaler());
            return response;
        });
        m_commands.Register("upscaler.set", [this, &renderer, hwnd](const br::diagnostics::ControlRequest& request) {
            nlohmann::json response = nlohmann::json::object();
            // The menu's upscaler settings, for repeated toggling in tests: "quality" 0 DLAA, 1 Quality, 2 Balanced,
            // 3 Performance, 4 Ultra Performance; "ray_reconstruction" true or false (DLSS otherwise). Each change sets
            // the upscaler up again and rebuilds the graph before the next frame.
            if (request.args.contains("quality")) {
                const uint32_t quality = request.args.at("quality").get<uint32_t>();
                if (quality > static_cast<uint32_t>(UpscaleQualityMode::UltraPerformance)) throw std::runtime_error("quality is 0-4");
                br::extensions::MakeSettingSetter<UpscaleQualityMode>("upscalingQualityMode")(static_cast<UpscaleQualityMode>(quality));
            }
            if (request.args.contains("ray_reconstruction")) {
                SettingsManager::GetInstance().getSettingSetter<UpscalingMode>("upscalingMode")(request.args.at("ray_reconstruction").get<bool>()
                    ? UpscalingMode::DLSSRayReconstruction : UpscalingMode::DLSS);
            }
            response["upscaler"] = nlohmann::json::parse(renderer.DescribeUpscaler());
            return response;
        });
        m_commands.Register("upscaler.simulate_failure", [this, &renderer, hwnd](const br::diagnostics::ControlRequest& request) {
            nlohmann::json response = nlohmann::json::object();
            // Debug: the next out_of_memory_frames DLSS evaluations act as if Streamline reported GPU memory pressure, and
            // the next settings_failures requests for Ray Reconstruction's optimal settings fail (the first right away,
            // through a reconfiguration). upscaler.get shows what happened.
            renderer.SimulateUpscalerFailures(request.args.value("out_of_memory_frames", 0u),
                request.args.value("settings_failures", 0u));
            response["upscaler"] = nlohmann::json::parse(renderer.DescribeUpscaler());
            return response;
        });
        m_commands.Register("camera.set", [this, &renderer, hwnd](const br::diagnostics::ControlRequest& request) {
            nlohmann::json response = nlohmann::json::object();
            // The primary camera's vertical field of view: a lens covers only its design field.
            auto& scene = renderer.GetCurrentScene();
            if (!scene) throw std::runtime_error("no scene");
            const auto* camera = scene->GetPrimaryCamera().try_get<Components::Camera>();
            if (!camera) throw std::runtime_error("no primary camera");
            if (request.args.contains("fov_deg")) {
                const float fovDegrees = request.args.at("fov_deg").get<float>();
                if (!(fovDegrees > 1.0f && fovDegrees < 170.0f)) throw std::runtime_error("camera.set 'fov_deg' must be in (1, 170)");
                scene->SetCameraProjection(fovDegrees * (XM_PI / 180.0f), camera->aspect, camera->zNear, camera->zFar);
                camera = scene->GetPrimaryCamera().try_get<Components::Camera>();
            }
            response["fov_deg"] = camera->fov * (180.0f / XM_PI);
            response["aspect"] = camera->aspect;
            return response;
        });
        m_commands.Register("job.status", [this, &renderer, hwnd](const br::diagnostics::ControlRequest& request) {
            nlohmann::json response = nlohmann::json::object();
            if (request.args.at("job_id").get<std::uint64_t>() >= DemoHdrCaptures::kFirstJobId) {
                const auto job = m_hdrCaptures.Status(request.args.at("job_id").get<std::uint64_t>());
                if (!job) throw std::runtime_error("unknown capture job");
                response["job"] = *job;
                return response;
            }
            const auto job = br::diagnostics::GetLiveJob(
                request.args.at("job_id").get<std::uint64_t>());
            if (!job) throw std::runtime_error("unknown pipeline job");
            const auto stateName = [](br::diagnostics::LiveJobState state) {
                switch (state) {
                case br::diagnostics::LiveJobState::Queued: return "queued";
                case br::diagnostics::LiveJobState::Compiling: return "compiling";
                case br::diagnostics::LiveJobState::ReadyToPublish: return "ready_to_publish";
                case br::diagnostics::LiveJobState::Published: return "published";
                case br::diagnostics::LiveJobState::Failed: return "failed";
                }
                return "unknown";
            };
            response["job"] = {
                { "id", job->id }, { "pipeline_id", job->pipelineId },
                { "state", stateName(job->state) }, { "generation", job->generation },
                { "error", job->error }
            };
            return response;
        });
        m_commands.Register("clod.phase2_expansion.get", [this, &renderer, hwnd](const br::diagnostics::ControlRequest& request) {
            nlohmann::json response = nlohmann::json::object();
            const uint32_t pass = request.args.value("pass", 1u);
            const char* settingName = pass == 2u
                ? CLodPureComputeReplayExpansionFactorSettingName
                : CLodPureComputePhase2ExpansionFactorSettingName;
            response["value"] = br::extensions::MakeSettingGetter<uint32_t>(settingName)();
            response["pass"] = pass == 2u ? 2u : 1u;
            return response;
        });
        m_commands.Register("clod.phase2_expansion.set", [this, &renderer, hwnd](const br::diagnostics::ControlRequest& request) {
            nlohmann::json response = nlohmann::json::object();
            if (!m_finished) throw std::runtime_error("cannot change traversal expansion while profiling is active");
            const uint32_t pass = request.args.value("pass", 1u);
            const char* settingName = pass == 2u
                ? CLodPureComputeReplayExpansionFactorSettingName
                : CLodPureComputePhase2ExpansionFactorSettingName;
            const uint32_t requested = request.args.at("value").get<uint32_t>();
            const uint32_t normalized = CLodNormalizePureComputePhase2ExpansionFactor(requested);
            br::extensions::MakeSettingSetter<uint32_t>(settingName)(normalized);
            response["value"] = normalized;
            response["pass"] = pass == 2u ? 2u : 1u;
            return response;
        });
        m_commands.Register("clod.mode.get", [this, &renderer, hwnd](const br::diagnostics::ControlRequest& request) {
            nlohmann::json response = nlohmann::json::object();
            const auto culling =
                br::extensions::MakeSettingGetter<CLodCullingBackend>(CLodCullingBackendSettingName)();
            const auto softwareRaster =
                br::extensions::MakeSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)();
            response["culling"] =
                culling == CLodCullingBackend::WorkGraph ? "work_graph" : "pure_compute";
            response["software_raster"] =
                softwareRaster == CLodSoftwareRasterMode::WorkGraph ? "work_graph" :
                softwareRaster == CLodSoftwareRasterMode::Compute ? "compute" : "disabled";
            response["rigid_only"] =
                br::extensions::MakeSettingGetter<bool>(CLodWorkGraphRigidOnlySettingName)();
            return response;
        });
        m_commands.Register("clod.mode.set", [this, &renderer, hwnd](const br::diagnostics::ControlRequest& request) {
            nlohmann::json response = nlohmann::json::object();
            if (!m_finished) throw std::runtime_error("cannot change CLOD mode while profiling is active");
            const std::string cullingName = request.args.at("culling").get<std::string>();
            const std::string softwareRasterName =
                request.args.at("software_raster").get<std::string>();
            const CLodCullingBackend culling =
                cullingName == "work_graph" ? CLodCullingBackend::WorkGraph :
                cullingName == "pure_compute" ? CLodCullingBackend::PureCompute :
                throw std::runtime_error("culling must be 'pure_compute' or 'work_graph'");
            const CLodSoftwareRasterMode softwareRaster =
                softwareRasterName == "work_graph" ? CLodSoftwareRasterMode::WorkGraph :
                softwareRasterName == "compute" ? CLodSoftwareRasterMode::Compute :
                softwareRasterName == "disabled" ? CLodSoftwareRasterMode::Disabled :
                throw std::runtime_error(
                    "software_raster must be 'disabled', 'compute', or 'work_graph'");
            br::extensions::MakeSettingSetter<CLodCullingBackend>(CLodCullingBackendSettingName)(culling);
            br::extensions::MakeSettingSetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)(
                softwareRaster);
            if (request.args.contains("rigid_only")) {
                br::extensions::MakeSettingSetter<bool>(CLodWorkGraphRigidOnlySettingName)(
                    request.args.at("rigid_only").get<bool>());
            }
            response["culling"] = cullingName;
            response["software_raster"] = softwareRasterName;
            response["rigid_only"] =
                br::extensions::MakeSettingGetter<bool>(CLodWorkGraphRigidOnlySettingName)();
            return response;
        });
        {
            const br::diagnostics::ControlCommandRegistry::Handler handler = [this, &renderer, hwnd](const br::diagnostics::ControlRequest& request) {
                nlohmann::json response = nlohmann::json::object();
                // The menu's switch, for switching at runtime without it. It takes effect at the next frame.
                if (request.command == "pathtracing.set") {
                    if (!m_finished) throw std::runtime_error("cannot switch path tracing while profiling is active");
                    if (request.args.contains("enabled")) {
                        br::extensions::MakeSettingSetter<bool>("enablePathTracing")(request.args.at("enabled").get<bool>());
                    }
                    // Rebuilds the graph, with the accumulation buffer, when path tracing is on.
                    if (request.args.contains("accumulate")) {
                        br::extensions::MakeSettingSetter<bool>("pathTracingAccumulate")(request.args.at("accumulate").get<bool>());
                    }
                }
                response["accumulate"] = br::extensions::MakeSettingGetter<bool>("pathTracingAccumulate")();
                const auto accumulation = renderer.GetPathTracingAccumulation();
                response["accumulating"] = accumulation.buffer != nullptr;
                response["accumulated_frames"] = accumulation.framesSinceRestart;
                response["accumulation_restarts"] = accumulation.restarts;
                const auto& deviceManager = br::DeviceManager::GetInstance();
                response["enabled"] = br::extensions::MakeSettingGetter<bool>("enablePathTracing")();
                response["supported"] = deviceManager.GetPathTracingSupported();
                response["unsupported_reason"] = deviceManager.GetPathTracingUnsupportedReason();
                return response;
            };
            m_commands.Register("pathtracing.get", handler);
            m_commands.Register("pathtracing.set", handler);
        }
        {
            const br::diagnostics::ControlCommandRegistry::Handler handler = [this, &renderer, hwnd](const br::diagnostics::ControlRequest& request) {
                nlohmann::json response = nlohmann::json::object();
                if (request.command == "clod.vsm_perf.set" && !m_finished) {
                    throw std::runtime_error("cannot change VSM performance settings while profiling is active");
                }
                if (request.command == "clod.vsm_perf.set") {
    						const float requestedWindInner = request.args.value(
    							"dynamic_wind_inner_radius",
    							br::extensions::MakeSettingGetter<float>(ProceduralWindInnerRadiusSettingName)());
    						const float requestedWindOuter = request.args.value(
    							"dynamic_wind_outer_radius",
    							br::extensions::MakeSettingGetter<float>(ProceduralWindOuterRadiusSettingName)());
    						if (requestedWindInner < 0.0f || requestedWindOuter < requestedWindInner) {
    							throw std::runtime_error("DynamicWind radii require 0 <= inner <= outer");
    						}
    						br::extensions::MakeSettingSetter<float>(ProceduralWindInnerRadiusSettingName)(requestedWindInner);
    						br::extensions::MakeSettingSetter<float>(ProceduralWindOuterRadiusSettingName)(requestedWindOuter);
    						if (request.args.contains("skinned_shadow_radius")) {
    							const float radius = request.args.at("skinned_shadow_radius").get<float>();
    							if (radius < 0.0f) throw std::runtime_error("skinned_shadow_radius must be non-negative");
    							const float innerRadius = br::extensions::MakeSettingGetter<float>(ProceduralWindInnerRadiusSettingName)();
    							if (radius < innerRadius) {
    								br::extensions::MakeSettingSetter<float>(ProceduralWindInnerRadiusSettingName)(radius);
    							}
    							br::extensions::MakeSettingSetter<float>(ProceduralWindOuterRadiusSettingName)(radius);
    						}
    						if (request.args.contains("skinned_shadow_dynamic_clipmap_count_override")) {
    							int32_t requestedOverride = request.args.at(
    								"skinned_shadow_dynamic_clipmap_count_override").get<int32_t>();
    							if (requestedOverride < -1) {
    								throw std::runtime_error("skinned shadow clipmap override must be -1 or non-negative");
    							}
    							const uint32_t activeClipmapCount =
    								g_clodSkinnedShadowActiveClipmapCount.load(std::memory_order_relaxed);
    							if (requestedOverride >= 0 && activeClipmapCount != 0u) {
    								requestedOverride = std::min(requestedOverride, static_cast<int32_t>(activeClipmapCount));
    							}
    							br::extensions::MakeSettingSetter<int32_t>(CLodSkinnedShadowDynamicClipmapCountOverrideSettingName)(
    								requestedOverride);
    						}
                    if (request.args.contains("page_budget")) {
                        br::extensions::MakeSettingSetter<uint32_t>(
                            CLodDirectionalVirtualShadowPageRenderBudgetSettingName)(
                            request.args.at("page_budget").get<uint32_t>());
                    }
                    if (request.args.contains("upgrade_budget")) {
                        br::extensions::MakeSettingSetter<uint32_t>(
                            CLodDirectionalVirtualShadowUpgradePageRenderBudgetSettingName)(
                            request.args.at("upgrade_budget").get<uint32_t>());
                    }
                    if (request.args.contains("cache_disabled")) {
                        br::extensions::MakeSettingSetter<bool>(
                            CLodDisableVirtualShadowPageCachingSettingName)(
                            request.args.at("cache_disabled").get<bool>());
                    }
                    if (request.args.contains("sw_raster_threshold")) {
                        br::extensions::MakeSettingSetter<uint32_t>(
                            CLodVirtualShadowSoftwareRasterDiameterThresholdSettingName)(
                            std::min(
                                request.args.at("sw_raster_threshold").get<uint32_t>(),
                                0xFFFFu));
                    }
                    if (request.args.contains("dynamic_wind_bounds_cache_enabled")) {
                        br::extensions::MakeSettingSetter<bool>(CLodDynamicWindBoundsCacheEnabledSettingName)(
                            request.args.at("dynamic_wind_bounds_cache_enabled").get<bool>());
                    }
                    if (request.args.contains("dynamic_wind_bounds_cache_mib")) {
                        br::extensions::MakeSettingSetter<uint32_t>(CLodDynamicWindBoundsCacheMiBSettingName)(
                            std::min(request.args.at("dynamic_wind_bounds_cache_mib").get<uint32_t>(), 256u));
                    }
                    if (request.args.contains("dynamic_wind_vertex_cache_enabled")) {
                        br::extensions::MakeSettingSetter<bool>(CLodDynamicWindVertexCacheEnabledSettingName)(
                            request.args.at("dynamic_wind_vertex_cache_enabled").get<bool>());
                    }
                    if (request.args.contains("dynamic_wind_vertex_cache_mib")) {
                        br::extensions::MakeSettingSetter<uint32_t>(CLodDynamicWindVertexCacheMiBSettingName)(
                            std::min(request.args.at("dynamic_wind_vertex_cache_mib").get<uint32_t>(), 512u));
                    }
                    if (request.args.contains("directional_lod_bias")) {
                        br::extensions::MakeSettingSetter<float>(CLodDirectionalVirtualShadowLodBiasSettingName)(
                            request.args.at("directional_lod_bias").get<float>());
                    }
                    if (request.args.contains("local_lod_bias")) {
                        br::extensions::MakeSettingSetter<float>(CLodLocalVirtualShadowLodBiasSettingName)(
                            request.args.at("local_lod_bias").get<float>());
                    }
                    if (request.args.contains("local_moving_lod_bias")) {
                        br::extensions::MakeSettingSetter<float>(CLodLocalVirtualShadowMovingLodBiasSettingName)(
                            request.args.at("local_moving_lod_bias").get<float>());
                    }
                    if (request.args.contains("local_force_invalidate")) {
                        br::extensions::MakeSettingSetter<bool>(CLodLocalVirtualShadowForceInvalidateSettingName)(
                            request.args.at("local_force_invalidate").get<bool>());
                    }
                    if (request.args.contains("raster_mode")) {
                        const std::string mode = request.args.at("raster_mode").get<std::string>();
                        const CLodVSMRasterMode rasterMode =
                            mode == "hardware" ? CLodVSMRasterMode::HardwareOnly :
                            mode == "standard" ? CLodVSMRasterMode::Standard :
                            mode == "reyes" ? CLodVSMRasterMode::Reyes :
                            throw std::runtime_error(
                                "raster_mode must be 'hardware', 'standard', or 'reyes'");
                        br::extensions::MakeSettingSetter<CLodVSMRasterMode>(
                            CLodVSMRasterModeSettingName)(rasterMode);
                    }
                }
                response["page_budget"] = br::extensions::MakeSettingGetter<uint32_t>(
                    CLodDirectionalVirtualShadowPageRenderBudgetSettingName)();
                response["upgrade_budget"] = br::extensions::MakeSettingGetter<uint32_t>(
                    CLodDirectionalVirtualShadowUpgradePageRenderBudgetSettingName)();
                response["cache_disabled"] = br::extensions::MakeSettingGetter<bool>(
                    CLodDisableVirtualShadowPageCachingSettingName)();
                response["sw_raster_threshold"] = br::extensions::MakeSettingGetter<uint32_t>(
                    CLodVirtualShadowSoftwareRasterDiameterThresholdSettingName)();
                response["directional_lod_bias"] = br::extensions::MakeSettingGetter<float>(
                    CLodDirectionalVirtualShadowLodBiasSettingName)();
                response["local_lod_bias"] = br::extensions::MakeSettingGetter<float>(
                    CLodLocalVirtualShadowLodBiasSettingName)();
                response["local_moving_lod_bias"] = br::extensions::MakeSettingGetter<float>(
                    CLodLocalVirtualShadowMovingLodBiasSettingName)();
                response["local_force_invalidate"] = br::extensions::MakeSettingGetter<bool>(
                    CLodLocalVirtualShadowForceInvalidateSettingName)();
    					response["dynamic_wind_inner_radius"] = br::extensions::MakeSettingGetter<float>(ProceduralWindInnerRadiusSettingName)();
    					response["dynamic_wind_outer_radius"] = br::extensions::MakeSettingGetter<float>(ProceduralWindOuterRadiusSettingName)();
    					response["skinned_shadow_radius"] = br::extensions::MakeSettingGetter<float>(ProceduralWindOuterRadiusSettingName)();
    					response["skinned_shadow_dynamic_clipmap_count_override"] =
    						br::extensions::MakeSettingGetter<int32_t>(CLodSkinnedShadowDynamicClipmapCountOverrideSettingName)();
    					response["skinned_shadow_effective_dynamic_clipmap_count"] =
    						g_clodSkinnedShadowEffectiveDynamicClipmapCount.load(std::memory_order_relaxed);
    					response["skinned_shadow_clipmap_classification"] = nlohmann::json::array();
    					const uint32_t activeShadowClipmapCount =
    						g_clodSkinnedShadowActiveClipmapCount.load(std::memory_order_relaxed);
    					const uint32_t dynamicShadowClipmapMask =
    						g_clodSkinnedShadowDynamicClipmapMask.load(std::memory_order_relaxed);
    					for (uint32_t clipmapIndex = 0u; clipmapIndex < activeShadowClipmapCount; ++clipmapIndex) {
    						response["skinned_shadow_clipmap_classification"].push_back(
    							(dynamicShadowClipmapMask & (1u << clipmapIndex)) != 0u ? "dynamic" : "static");
    					}
    					response["skinned_shadow_classification_generation"] =
    						g_clodSkinnedShadowClassificationGeneration.load(std::memory_order_relaxed);
    					response["skinned_shadow_one_shot_invalidations"] =
    						g_clodSkinnedShadowOneShotInvalidationCount.load(std::memory_order_relaxed);
                response["dynamic_wind_bounds_cache_enabled"] = br::extensions::MakeSettingGetter<bool>(
                    CLodDynamicWindBoundsCacheEnabledSettingName)();
                response["dynamic_wind_bounds_cache_mib"] = br::extensions::MakeSettingGetter<uint32_t>(
                    CLodDynamicWindBoundsCacheMiBSettingName)();
                response["dynamic_wind_vertex_cache_enabled"] = br::extensions::MakeSettingGetter<bool>(
                    CLodDynamicWindVertexCacheEnabledSettingName)();
                response["dynamic_wind_vertex_cache_mib"] = br::extensions::MakeSettingGetter<uint32_t>(
                    CLodDynamicWindVertexCacheMiBSettingName)();
                const CLodVSMRasterMode rasterMode = br::extensions::MakeSettingGetter<CLodVSMRasterMode>(
                    CLodVSMRasterModeSettingName)();
                response["raster_mode"] =
                    rasterMode == CLodVSMRasterMode::HardwareOnly ? "hardware" :
                    rasterMode == CLodVSMRasterMode::Reyes ? "reyes" : "standard";
                return response;
            };
            m_commands.Register("clod.vsm_perf.get", handler);
            m_commands.Register("clod.vsm_perf.set", handler);
        }
        m_commands.Register("clod.workgraph.reload", [this, &renderer, hwnd](const br::diagnostics::ControlRequest& request) {
            nlohmann::json response = nlohmann::json::object();
            if (!m_finished) throw std::runtime_error("cannot reload CLOD work graphs while profiling is active");
            response["reloaded_passes"] = br::diagnostics::ReloadCLodWorkGraphs();
            response["pipeline_epoch"] = br::diagnostics::GetPipelineEpoch();
            return response;
        });
        m_commands.Register("profile.run", [this, &renderer, hwnd](const br::diagnostics::ControlRequest& request) {
            nlohmann::json response = nlohmann::json::object();
            if (!m_configuration) throw std::runtime_error("profile.run needs --sampling-config");
            if (!m_finished) throw std::runtime_error("a profiling experiment is already active");
            StartExperiment(request.args.value("label", "experiment"), &renderer);
            response["experiment_id"] = m_experimentId;
            return response;
        });
        m_commands.Register("profile.status", [this, &renderer, hwnd](const br::diagnostics::ControlRequest& request) {
            nlohmann::json response = nlohmann::json::object();
            response["pid"] = GetCurrentProcessId();
            response["experiment_id"] = m_experimentId;
            response["active"] = !m_finished;
            response["finalized"] = m_finalized;
            response["phase"] = PhaseName();
            response["readiness_stable_frames"] = m_readyStableFrames;
            response["readiness_required_frames"] = m_configuration ? m_configuration->readyFrames : 0u;
            response["readiness_duration_ms"] = m_readinessDurationMs;
            response["attempted_samples"] = m_samples.size();
            response["accepted_samples"] = std::ranges::count_if(m_samples, [](const auto& sample) { return sample.accepted; });
            response["stopping_reason"] = m_stoppingReason;
            return response;
        });
        m_commands.Register("profile.cancel", [this, &renderer, hwnd](const br::diagnostics::ControlRequest& request) {
            nlohmann::json response = nlohmann::json::object();
            if (m_finished) throw std::runtime_error("no profiling experiment is active");
            Finish(renderer, "cancelled", "cancelled by control request", hwnd, 0);
            return response;
        });
        m_commands.Register("shutdown", [this, &renderer, hwnd](const br::diagnostics::ControlRequest& request) {
            nlohmann::json response = nlohmann::json::object();
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            return response;
        });
    }

void DemoStatisticalSamplingRun::PumpControlRequests()
    {
        if (m_controlServer.Running()) m_commands.Serve(m_controlServer);
    }
