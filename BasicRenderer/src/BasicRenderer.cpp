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
//#include <tracy/Tracy.hpp>

#include "Mesh/Mesh.h"
#include "Renderer.h"
#include "Utilities/Utilities.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Materials/Material.h"
#include "Menu/Menu.h"
#include "Materials/MaterialFlags.h"
#include "Render/PSOFlags.h"
#include "Render/OutputTypes.h"
#include "Render/GraphExtensions/CLodTelemetry.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/GraphExtensions/ClusterLOD/HierarchicalCullingPass.h"
#include "Telemetry/NvPerfIntegration.h"
#include "Telemetry/SamplingControlServer.h"
#include "Telemetry/StatisticalSampler.h"
#include "Resources/Buffers/DynamicBufferBase.h"
#include "Managers/Singletons/DeletionManager.h"
#include "Import/ModelLoader.h"
#include "spdlogStreambuf.h"
#include <rhi_interop_dx12.h>
#include <d3d12sdklayers.h>

#define STB_IMAGE_IMPLEMENTATION
#include "ThirdParty/stb/stb_image.h"

// Activate dedicated GPU on NVIDIA laptops with both integrated and dedicated GPUs
extern "C" {
    _declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
}

// Set Agility SDK version
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 614;}

extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D\\"; }

#pragma comment(lib, "WinPixEventRuntime.lib")

namespace {
    void ResetD3D12SmokeValidationMessages() {
        auto device = DeviceManager::GetInstance().GetDevice();
        ID3D12Device* nativeDevice = rhi::dx12::get_device(device);
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
        if (nativeDevice && SUCCEEDED(nativeDevice->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
            infoQueue->ClearStoredMessages();
        }
    }

    bool D3D12SmokeValidationFailed() {
        auto device = DeviceManager::GetInstance().GetDevice();
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

    RegisterRawInputDevices(hwnd);

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

std::optional<std::string> FindCommandLineValue(std::string_view commandLine, std::string_view option)
{
    const auto path = FindCommandLinePath(commandLine, option);
    return path ? std::optional<std::string>(path->string()) : std::nullopt;
}

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

    void AfterFrame(Renderer& renderer, HWND hwnd)
    {
        if (!m_configuration) return;
        PumpControlRequests(renderer, hwnd);
        if (m_finished) return;
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
                Finish("failed", "timed out waiting for stable readiness", hwnd, 8);
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
    void PumpControlRequests(Renderer& renderer, HWND hwnd);

    void ArmNextCapture(Renderer& renderer, HWND hwnd)
    {
        std::string error;
        m_captureReadiness = BuildDemoSamplingReadiness(renderer);
        m_captureSignature = DemoSamplingSignature(m_captureReadiness);
        if (!br::telemetry::nvperf::ArmCapture(m_nextOrdinal, error)) {
            Finish("failed", "failed to arm NVPerf: " + error, hwnd, 3);
            return;
        }
        m_phase = Phase::Capturing;
        spdlog::info("BasicRenderer sampling: armed sample {}", m_nextOrdinal);
    }

    void ConsumeCapture(Renderer& renderer, HWND hwnd)
    {
        auto capture = br::telemetry::nvperf::TakeCaptureResult();
        if (!capture) {
            Finish("failed", "NVPerf completed without a capture result", hwnd, 4);
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
            Finish("failed", "render graph statistics service is unavailable", hwnd, 6);
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
                Finish("failed", "timed out waiting for fresh render-graph GPU timestamps", hwnd, 7);
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
            Finish("complete", "all required targets converged", hwnd, 0);
        } else if (accepted >= m_configuration->maximumSamples) {
            Finish("complete", "maximum accepted sample count reached", hwnd, 0);
        } else if (m_samples.size() >= static_cast<std::size_t>(m_configuration->maximumSamples) * 3u) {
            Finish("failed", "maximum capture attempts reached", hwnd, 5);
        } else if (m_usesNvPerf) {
            ArmNextCapture(renderer, hwnd);
        }
    }

    void Finish(const char* status, std::string reason, HWND hwnd, int exitCode)
    {
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
    std::string m_readinessSignature;
    std::string m_stoppingReason;
    std::uint32_t m_phaseFrames{ 0 };
    std::uint32_t m_readyStableFrames{ 0 };
    std::uint64_t m_readinessDurationMs{ 0 };
    std::uint32_t m_nextOrdinal{ 1 };
    std::int64_t m_experimentId{ 0 };
    int m_exitCode{ 0 };
    bool m_finished{ false };
    bool m_finalized{ false };
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
    std::filesystem::create_directories("logs");
    std::filesystem::remove("logs/log.txt");
    auto file_logger = spdlog::basic_logger_mt("file_logger", "logs/log.txt", true);
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
        _putenv_s("BASICRENDERER_DISABLE_STREAMLINE", "1");
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
    SettingsManager::GetInstance().getSettingSetter<bool>("rememberCameraPose")(
        cameraState.rememberCameraPose);
    if (vsmPageStateTest) {
        SettingsManager::GetInstance().getSettingSetter<unsigned int>("outputType")(
            static_cast<unsigned int>(OutputType::VSM_PAGE_STATE));
    }
    if (vsmRerenderedTest) {
        SettingsManager::GetInstance().getSettingSetter<unsigned int>("outputType")(
            static_cast<unsigned int>(OutputType::VSM_RERENDERED_THIS_FRAME));
    }
    if (clodStreamingStressTest) {
        SettingsManager::GetInstance().getSettingSetter<uint32_t>(
            "clodStreamingCpuUploadBudgetRequests")(256u);
    }
    if (clodVsmCpuBenchmark) {
        spdlog::info(
            "CLOD VSM CPU benchmark armed: deterministic residency camera "
            "sweeps through frame 480 with graph rebuilds disabled.");
    }
    if (graphRebuildSmokeTest) {
        SettingsManager::GetInstance().getSettingSetter<bool>("renderGraphCompileDumpEnabled")(true);
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

    {
        BufferBase::ScopedBackingMutation startupSceneBackingMutation;

        auto baseScene = std::make_shared<Scene>();

        auto dragonScene = LoadModel("models/dragon.glb");
        dragonScene->GetRoot().set<Components::Scale>({ 100, 100, 100 });
        dragonScene->GetRoot().set<Components::Position>({ -3, 5, 0 });

    //auto carScene = LoadModel("models/porche.glb");
    //carScene->GetRoot().set<Components::Scale>({ 0.6, 0.6, 0.6 });
    //carScene->GetRoot().set<Components::Position>({ 1.0, 0.0, 1.0 });
    //auto sphereScene = LoadModel("models/sphere.glb");

	//auto mountainScene = LoadModel("models/terrain.glb");
	//mountainScene->GetRoot().set<Components::Scale>({ 50.0, 50.0, 50.0 });
	//mountainScene->GetRoot().set<Components::Position>({ 0.0, -2.0, 0.0 });

    auto tigerScene = LoadModel("models/tiger.glb");
    tigerScene->GetRoot().set<Components::Scale>({ 0.01, 0.01, 0.01 });
	tigerScene->GetRoot().set<Components::Position>({3, 5, 0});

	//auto shiba = LoadModel("models/shiba.glb");

    //auto usdScene = LoadModel("models/sponza.usdz");
    
    //auto bistro = LoadModel("models/bistroExteriorNoMats.usdz");
    //auto bistro = LoadModel("models/bistroExterior.glb");
    //auto wine = LoadModel("models/bistroInterior.usdz");
    //bistro->GetRoot().set<Components::Scale>({ 0.01, 0.01, 0.01 });

    //auto robot = LoadModel("models/robot.usdz");

	auto zorah = LoadModel("models/zorahv2/zorah_main_public.v2.gltf");

	//auto zorah = LoadModel("models/zorah_materials/zorah.usdc");

	//auto island = LoadModel("models/island/usd/elements/isMountainB/instance.usda");

	//auto quad = LoadModel("models/quad.usdz");
	
	    //auto cubes = LoadModel("models/cubes/suspicious_cubes.usda");

    //auto cherry = LoadModel("models/Trees/CherryTree.usd");

    //auto pine = LoadModel("models/Trees/branch.usdz");
	//pine->GetRoot().set<Components::Position>({ 0.0, 2.0, 0.0 });

    //auto needles = LoadModel("models/Trees/Tree_Baltic_Pine_01_A.usd");

	//auto farmhouse = LoadModel("models/iceberglarge.nif");

        renderer.SetCurrentScene(baseScene);
    	//renderer.GetCurrentScene()->AppendScene(needles->Clone());

	//renderer.GetCurrentScene()->AppendScene(farmhouse->Clone());

    constexpr int NeedleCloneCount = 0;
    constexpr float NeedleDistributionRadius = 50.0f;
    constexpr float NeedleMinSpacing = 5.0f;
    constexpr float NeedleMinSpacingSq = NeedleMinSpacing * NeedleMinSpacing;
    constexpr int MaxNeedleFailedPlacementAttempts = 200000;
    constexpr uint32_t NeedleSkeletonVariantCount = 1;

    std::mt19937 needleRng{ 1337 };
    std::uniform_real_distribution<float> needleUnitDist(0.0f, 1.0f);
    std::uniform_real_distribution<float> needleAngleDist(0.0f, DirectX::XM_2PI);
    std::vector<point> needlePositions;
    needlePositions.reserve(NeedleCloneCount);
    std::unordered_map<std::int64_t, std::vector<std::size_t>> needlePlacementGrid;
    needlePlacementGrid.reserve(static_cast<std::size_t>(NeedleCloneCount));

    const auto gridCoord = [](float value) -> int {
        return static_cast<int>(std::floor(value / NeedleMinSpacing));
    };
    const auto gridKey = [](int x, int z) -> std::int64_t {
        return (static_cast<std::int64_t>(x) << 32) ^ static_cast<std::uint32_t>(z);
    };

    int failedPlacementAttempts = 0;
    std::uint64_t totalPlacementAttempts = 0;
    while (needlePositions.size() < NeedleCloneCount &&
        failedPlacementAttempts < MaxNeedleFailedPlacementAttempts) {
        ++totalPlacementAttempts;
        const float radius = NeedleDistributionRadius * std::sqrt(needleUnitDist(needleRng));
        const float angle = needleAngleDist(needleRng);
        const point candidate{
            radius * std::cos(angle),
            0.0f,
            radius * std::sin(angle)
        };

        bool hasEnoughSpacing = true;
        const int candidateCellX = gridCoord(candidate.x);
        const int candidateCellZ = gridCoord(candidate.z);
        for (int dzCell = -1; dzCell <= 1 && hasEnoughSpacing; ++dzCell) {
            for (int dxCell = -1; dxCell <= 1 && hasEnoughSpacing; ++dxCell) {
                const auto it = needlePlacementGrid.find(gridKey(candidateCellX + dxCell, candidateCellZ + dzCell));
                if (it == needlePlacementGrid.end()) {
                    continue;
                }

                for (const std::size_t existingIndex : it->second) {
                    const point& existing = needlePositions[existingIndex];
                    const float dx = candidate.x - existing.x;
                    const float dz = candidate.z - existing.z;
                    if (dx * dx + dz * dz < NeedleMinSpacingSq) {
                        hasEnoughSpacing = false;
                        break;
                    }
                }
            }
        }

        if (hasEnoughSpacing) {
            const std::size_t insertedIndex = needlePositions.size();
            needlePositions.push_back(candidate);
            needlePlacementGrid[gridKey(candidateCellX, candidateCellZ)].push_back(insertedIndex);
            failedPlacementAttempts = 0;
        }
        else {
            ++failedPlacementAttempts;
        }
    }

    spdlog::info(
        "Needle placement generated {} / {} requested positions (radius={} minSpacing={} attempts={} failedTailLimit={} failedTail={})",
        needlePositions.size(),
        NeedleCloneCount,
        NeedleDistributionRadius,
        NeedleMinSpacing,
        totalPlacementAttempts,
        MaxNeedleFailedPlacementAttempts,
        failedPlacementAttempts);

    SkeletonVariantSet needleSkeletonVariants(NeedleSkeletonVariantCount);
    std::size_t appendedNeedleScenes = 0;
    for (const point& position : needlePositions) {
        //needles->GetRoot().set<Components::Position>({ position.x, position.y, position.z });
        //auto needleClone = needles->Clone();
        //needleClone->AssignSkeletonVariants(needleSkeletonVariants);
        //renderer.GetCurrentScene()->AppendScene(needleClone);
        ++appendedNeedleScenes;
    }
    spdlog::info("Needle append completed: appended {} cloned scenes", appendedNeedleScenes);
	//renderer.GetCurrentScene()->AppendScene(pine->Clone());

    //renderer.GetCurrentScene()->AppendScene(cherry->Clone());
    	//renderer.AppendScene(cubes->Clone());
    
	//renderer.GetCurrentScene()->AppendScene(carScene->Clone());

	//renderer.GetCurrentScene()->AppendScene(quad->Clone());
	//quad->GetRoot().set<Components::Position>({ 0.0, -2.0, 0.0 });
	//renderer.GetCurrentScene()->AppendScene(quad->Clone());

	//renderer.GetCurrentScene()->AppendScene(island->Clone());

	renderer.GetCurrentScene()->AppendScene(zorah->Clone());

    //mountainScene = LoadModel("models/terrain.glb");
 //   mountainScene->GetRoot().set<Components::Scale>({ 50.0, 50.0, 50.0 });
 //   mountainScene->GetRoot().set<Components::Position>({ 0.0, -10.0, 0.0 });
	//renderer.GetCurrentScene()->AppendScene(mountainScene->Clone());

	renderer.GetCurrentScene()->AppendScene(dragonScene->Clone());
    
	renderer.GetCurrentScene()->AppendScene(tigerScene->Clone());

	//renderer.GetCurrentScene()->AppendScene(robot->Clone());

    //renderer.GetCurrentScene()->AppendScene(bistro->Clone());

	//sphereScene->GetRoot().set<Components::Position>({ 0.0, 2.0, 0.0 });
    //renderer.GetCurrentScene()->AppendScene(sphereScene->Clone());

    //for (int i = 0; i < 5; i++) {
    //    auto sphereInstance = renderer.GetCurrentScene()->AppendScene(sphereScene->Clone());
    //    auto point = getRandomPointInVolume(-2, 2, -2, 2, -2, 2);
    //    sphereInstance->GetRoot().set<Components::Position>({ point.x, point.y, point.z });
    //}


        renderer.SetEnvironment("sky");

        XMFLOAT3 pos = XMFLOAT3(10.f, 5.f, 0.f);
        XMFLOAT3 lookAt = XMFLOAT3(11.0f, 5.0f, 0.0f);
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
        //auto light3 = renderer.GetCurrentScene()->CreateSpotLightECS(L"light3", XMFLOAT3(0, 10, 3), XMFLOAT3(1, 1, 1), 2000.0, {0, -1, 0}, .5, .8, 0.0, 0.0, 1.0);
        //auto light1 = renderer.GetCurrentScene()->CreatePointLightECS(L"light1", XMFLOAT3(0, 1, 3), XMFLOAT3(1, 1, 1), 100.0, 0.0, 0.0, 1.0);
    
        for (int i = 0; i < 0; i++) {
		    auto point = getRandomPointInVolume(-20, 20, -2, 0, -20, 20);
		    auto color = XMFLOAT3(randomFloat(0.0, 1.0), randomFloat(0.0, 1.0), randomFloat(0.0, 1.0));
            auto light1 = renderer.GetCurrentScene()->CreatePointLightECS(L"light"+std::to_wstring(i), XMFLOAT3(point.x, point.y, point.z), color, 3.0, 0.0, 0.0, 1.0, false);
        }
    }

    MSG msg = {};
    unsigned int frameIndex = 0;
    uint64_t clodStressTelemetrySequence = 0;
    uint32_t clodStressResidentAtLastMove = 0;
    bool clodStressFailed = false;
    auto lastUpdateTime = std::chrono::system_clock::now();
    while (msg.message != WM_QUIT) {
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
                    SettingsManager::GetInstance().getSettingSetter<bool>("enableBloom")(false);
                }
                else {
                    spdlog::info("CLod graph-rebuild smoke test: disabling occlusion culling at frame {}.", frameIndex);
                    SettingsManager::GetInstance().getSettingSetter<bool>("enableOcclusionCulling")(false);
                }
                if (clodStreamingStressTest) {
                    SettingsManager::GetInstance().getSettingSetter<bool>("enableBloom")(false);
                }
            }
            if (graphRebuildSmokeTest && frameIndex == 240) {
                if (pipelineReplacementSmokeTest) {
                    spdlog::info("Pipeline replacement smoke test: enabling bloom at frame {}.", frameIndex);
                    SettingsManager::GetInstance().getSettingSetter<bool>("enableBloom")(true);
                }
                else {
                    spdlog::info("CLod graph-rebuild smoke test: enabling occlusion culling at frame {}.", frameIndex);
                    SettingsManager::GetInstance().getSettingSetter<bool>("enableOcclusionCulling")(true);
                }
                if (clodStreamingStressTest) {
                    SettingsManager::GetInstance().getSettingSetter<bool>("enableBloom")(true);
                }
            }
            if (graphRebuildSmokeTest && frameIndex == 360) {
                if (pipelineReplacementSmokeTest) {
                    spdlog::info("Pipeline replacement smoke test: disabling bloom again at frame {}.", frameIndex);
                    SettingsManager::GetInstance().getSettingSetter<bool>("enableBloom")(false);
                }
                else {
                    spdlog::info("CLod graph-rebuild smoke test: disabling occlusion culling again at frame {}.", frameIndex);
                    SettingsManager::GetInstance().getSettingSetter<bool>("enableOcclusionCulling")(false);
                }
            }
            if (graphRebuildSmokeTest && !clodStreamingStressTest && frameIndex == 480) {
                spdlog::info("Graph-rebuild smoke test completed after {} frames; closing.", frameIndex);
                PostMessage(hwnd, WM_CLOSE, 0, 0);
            }
            if (clodStreamingStressTest && (frameIndex == 600 || frameIndex == 840)) {
                const bool enabled = frameIndex == 840;
                spdlog::info("CLod stress: graph rebuild at frame {} bloom={} occlusion={}", frameIndex, enabled, enabled);
                SettingsManager::GetInstance().getSettingSetter<bool>("enableBloom")(enabled);
                SettingsManager::GetInstance().getSettingSetter<bool>("enableOcclusionCulling")(enabled);
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
            renderer.Update(elapsedSeconds.count());
            renderer.PostUpdate();
            if (frameIndex % 100 == 0) {
                spdlog::info("FPS: {}", 1 / elapsedSeconds.count());
            }
            renderer.Render();
            statisticalSampling.AfterFrame(renderer, hwnd);
        }
    }

    const bool smokeValidationFailed = graphRebuildSmokeTest && D3D12SmokeValidationFailed();
    if (const auto& scene = renderer.GetCurrentScene(); scene) {
        SaveDemoCameraState(
            *scene,
            SettingsManager::GetInstance()
                .getSettingGetter<bool>("rememberCameraPose")());
    }
    renderer.Cleanup();

    if (statisticalSampling.ExitCode() != 0) return statisticalSampling.ExitCode();
    return (smokeValidationFailed || clodStressFailed) ? 1 : 0;
}

// Window callback procedure
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {

	const bool imguiHandled = Menu::GetInstance().HandleInput(hWnd, message, wParam, lParam);
    const bool blockRendererInput = IsRendererInputMessage(message) && ShouldBlockRendererInputForImGui(message);

    if (IsRendererInputMessage(message) && !blockRendererInput) {
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

void DemoStatisticalSamplingRun::PumpControlRequests(Renderer& renderer, HWND hwnd)
    {
        for (const auto& request : m_controlServer.DrainRequests()) {
            nlohmann::json response;
            try {
                response["request_id"] = request->document.at("request_id");
                const auto command = request->document.value("command", "");
                if (command == "session.status") {
                    response["pid"] = GetCurrentProcessId();
                    response["profile_active"] = !m_finished;
                    response["experiment_id"] = m_experimentId;
                    response["pipeline_epoch"] = PSOManager::GetInstance().GetPipelineEpoch();
                } else if (command == "pso.list") {
                    response["pipelines"] = nlohmann::json::array();
                    for (const auto& pipeline : PSOManager::GetInstance().ListPipelines()) {
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
                } else if (command == "pso.recompile") {
                    if (!m_finished) throw std::runtime_error("cannot recompile a pipeline while profiling is active");
                    PSOManager::RecompileOptions options;
                    options.label = request->document.value("label", "pipe-reload");
                    if (const auto defines = request->document.find("defines");
                        defines != request->document.end()) {
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
                    response["job_id"] = PSOManager::GetInstance().RequestRecompile(
                        request->document.at("pipeline_id").get<std::string>(), std::move(options));
                } else if (command == "pso.activate") {
                    if (!m_finished) throw std::runtime_error("cannot activate a pipeline while profiling is active");
                    response["job_id"] = PSOManager::GetInstance().RequestActivation(
                        request->document.at("pipeline_id").get<std::string>(),
                        request->document.at("generation").get<std::uint64_t>());
                } else if (command == "job.status") {
                    const auto job = PSOManager::GetInstance().GetLiveJob(
                        request->document.at("job_id").get<std::uint64_t>());
                    if (!job) throw std::runtime_error("unknown pipeline job");
                    const auto stateName = [](PSOManager::LiveJobState state) {
                        switch (state) {
                        case PSOManager::LiveJobState::Queued: return "queued";
                        case PSOManager::LiveJobState::Compiling: return "compiling";
                        case PSOManager::LiveJobState::ReadyToPublish: return "ready_to_publish";
                        case PSOManager::LiveJobState::Published: return "published";
                        case PSOManager::LiveJobState::Failed: return "failed";
                        }
                        return "unknown";
                    };
                    response["job"] = {
                        { "id", job->id }, { "pipeline_id", job->pipelineId },
                        { "state", stateName(job->state) }, { "generation", job->generation },
                        { "error", job->error }
                    };
                } else if (command == "clod.phase2_expansion.get") {
                    const uint32_t pass = request->document.value("pass", 1u);
                    const char* settingName = pass == 2u
                        ? CLodPureComputeReplayExpansionFactorSettingName
                        : CLodPureComputePhase2ExpansionFactorSettingName;
                    response["value"] = SettingsManager::GetInstance()
                        .getSettingGetter<uint32_t>(settingName)();
                    response["pass"] = pass == 2u ? 2u : 1u;
                } else if (command == "clod.phase2_expansion.set") {
                    if (!m_finished) throw std::runtime_error("cannot change traversal expansion while profiling is active");
                    const uint32_t pass = request->document.value("pass", 1u);
                    const char* settingName = pass == 2u
                        ? CLodPureComputeReplayExpansionFactorSettingName
                        : CLodPureComputePhase2ExpansionFactorSettingName;
                    const uint32_t requested = request->document.at("value").get<uint32_t>();
                    const uint32_t normalized = CLodNormalizePureComputePhase2ExpansionFactor(requested);
                    SettingsManager::GetInstance()
                        .getSettingSetter<uint32_t>(settingName)(normalized);
                    response["value"] = normalized;
                    response["pass"] = pass == 2u ? 2u : 1u;
                } else if (command == "clod.mode.get") {
                    auto& settings = SettingsManager::GetInstance();
                    const auto culling =
                        settings.getSettingGetter<CLodCullingBackend>(CLodCullingBackendSettingName)();
                    const auto softwareRaster =
                        settings.getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)();
                    response["culling"] =
                        culling == CLodCullingBackend::WorkGraph ? "work_graph" : "pure_compute";
                    response["software_raster"] =
                        softwareRaster == CLodSoftwareRasterMode::WorkGraph ? "work_graph" :
                        softwareRaster == CLodSoftwareRasterMode::Compute ? "compute" : "disabled";
                    response["rigid_only"] =
                        settings.getSettingGetter<bool>(CLodWorkGraphRigidOnlySettingName)();
                } else if (command == "clod.mode.set") {
                    if (!m_finished) throw std::runtime_error("cannot change CLOD mode while profiling is active");
                    const std::string cullingName = request->document.at("culling").get<std::string>();
                    const std::string softwareRasterName =
                        request->document.at("software_raster").get<std::string>();
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
                    auto& settings = SettingsManager::GetInstance();
                    settings.getSettingSetter<CLodCullingBackend>(CLodCullingBackendSettingName)(culling);
                    settings.getSettingSetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)(
                        softwareRaster);
                    if (request->document.contains("rigid_only")) {
                        settings.getSettingSetter<bool>(CLodWorkGraphRigidOnlySettingName)(
                            request->document.at("rigid_only").get<bool>());
                    }
                    response["culling"] = cullingName;
                    response["software_raster"] = softwareRasterName;
                    response["rigid_only"] =
                        settings.getSettingGetter<bool>(CLodWorkGraphRigidOnlySettingName)();
                } else if (command == "clod.vsm_perf.get" || command == "clod.vsm_perf.set") {
                    if (command == "clod.vsm_perf.set" && !m_finished) {
                        throw std::runtime_error("cannot change VSM performance settings while profiling is active");
                    }
                    auto& settings = SettingsManager::GetInstance();
                    if (command == "clod.vsm_perf.set") {
                        if (request->document.contains("page_budget")) {
                            settings.getSettingSetter<uint32_t>(
                                CLodDirectionalVirtualShadowPageRenderBudgetSettingName)(
                                request->document.at("page_budget").get<uint32_t>());
                        }
                        if (request->document.contains("upgrade_budget")) {
                            settings.getSettingSetter<uint32_t>(
                                CLodDirectionalVirtualShadowUpgradePageRenderBudgetSettingName)(
                                request->document.at("upgrade_budget").get<uint32_t>());
                        }
                        if (request->document.contains("cache_disabled")) {
                            settings.getSettingSetter<bool>(
                                CLodDisableVirtualShadowPageCachingSettingName)(
                                request->document.at("cache_disabled").get<bool>());
                        }
                        if (request->document.contains("block_soft_cap")) {
                            settings.getSettingSetter<uint32_t>(
                                CLodPageJobMaxPagesPerClusterSettingName)(
                                request->document.at("block_soft_cap").get<uint32_t>());
                        }
                        if (request->document.contains("sw_raster_threshold")) {
                            settings.getSettingSetter<uint32_t>(
                                CLodVirtualShadowSoftwareRasterDiameterThresholdSettingName)(
                                std::min(
                                    request->document.at("sw_raster_threshold").get<uint32_t>(),
                                    0xFFFFu));
                        }
                        if (request->document.contains("page_job_force_all")) {
                            settings.getSettingSetter<bool>(
                                CLodPageJobForceAllSettingName)(
                                request->document.at("page_job_force_all").get<bool>());
                        }
                        if (request->document.contains("raster_mode")) {
                            const std::string mode = request->document.at("raster_mode").get<std::string>();
                            const CLodVSMRasterMode rasterMode =
                                mode == "hardware" ? CLodVSMRasterMode::HardwareOnly :
                                mode == "standard" ? CLodVSMRasterMode::Standard :
                                mode == "page_job" ? CLodVSMRasterMode::PageJob :
                                mode == "reyes" ? CLodVSMRasterMode::Reyes :
                                throw std::runtime_error(
                                    "raster_mode must be 'hardware', 'standard', 'page_job', or 'reyes'");
                            settings.getSettingSetter<CLodVSMRasterMode>(
                                CLodVSMRasterModeSettingName)(rasterMode);
                        }
                    }
                    response["page_budget"] = settings.getSettingGetter<uint32_t>(
                        CLodDirectionalVirtualShadowPageRenderBudgetSettingName)();
                    response["upgrade_budget"] = settings.getSettingGetter<uint32_t>(
                        CLodDirectionalVirtualShadowUpgradePageRenderBudgetSettingName)();
                    response["cache_disabled"] = settings.getSettingGetter<bool>(
                        CLodDisableVirtualShadowPageCachingSettingName)();
                    response["block_soft_cap"] = settings.getSettingGetter<uint32_t>(
                        CLodPageJobMaxPagesPerClusterSettingName)();
                    response["sw_raster_threshold"] = settings.getSettingGetter<uint32_t>(
                        CLodVirtualShadowSoftwareRasterDiameterThresholdSettingName)();
                    response["page_job_force_all"] = settings.getSettingGetter<bool>(
                        CLodPageJobForceAllSettingName)();
                    const CLodVSMRasterMode rasterMode = settings.getSettingGetter<CLodVSMRasterMode>(
                        CLodVSMRasterModeSettingName)();
                    response["raster_mode"] =
                        rasterMode == CLodVSMRasterMode::HardwareOnly ? "hardware" :
                        rasterMode == CLodVSMRasterMode::PageJob ? "page_job" :
                        rasterMode == CLodVSMRasterMode::Reyes ? "reyes" : "standard";
                } else if (command == "clod.workgraph.reload") {
                    if (!m_finished) throw std::runtime_error("cannot reload CLOD work graphs while profiling is active");
                    response["reloaded_passes"] = HierarchicalCullingPass::ReloadAllWorkGraphs();
                    response["pipeline_epoch"] = PSOManager::GetInstance().GetPipelineEpoch();
                } else if (command == "profile.run") {
                    if (!m_finished) throw std::runtime_error("a profiling experiment is already active");
                    StartExperiment(request->document.value("label", "experiment"), &renderer);
                    response["experiment_id"] = m_experimentId;
                } else if (command == "profile.status") {
                    response["pid"] = GetCurrentProcessId();
                    response["experiment_id"] = m_experimentId;
                    response["active"] = !m_finished;
                    response["finalized"] = m_finalized;
                    response["phase"] = PhaseName();
                    response["readiness_stable_frames"] = m_readyStableFrames;
                    response["readiness_required_frames"] = m_configuration->readyFrames;
                    response["readiness_duration_ms"] = m_readinessDurationMs;
                    response["attempted_samples"] = m_samples.size();
                    response["accepted_samples"] = std::ranges::count_if(m_samples, [](const auto& sample) { return sample.accepted; });
                    response["stopping_reason"] = m_stoppingReason;
                } else if (command == "profile.cancel") {
                    if (m_finished) throw std::runtime_error("no profiling experiment is active");
                    Finish("cancelled", "cancelled by control request", hwnd, 0);
                } else if (command == "shutdown") {
                    PostMessage(hwnd, WM_CLOSE, 0, 0);
                } else {
                    throw std::runtime_error("unknown command '" + command + "'");
                }
                response["ok"] = true;
            } catch (const std::exception& exception) {
                response["ok"] = false;
                response["error"] = exception.what();
            }
            request->response.set_value(std::move(response));
        }
    }
