#include <iostream>
#include <Windows.h>
#include <windowsx.h>
#include <iostream>
#include <memory>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <imgui.h>
#include <random>
#include <io.h>        // _pipe, _dup2, _read, _close
#include <fcntl.h>     // _O_BINARY
#include <thread>
#ifndef USE_PIX
#define USE_PIX 1
#endif
#include <pix3.h>
#include <stacktrace>
#include <sstream>      // ostringstream for formatting
//#include <tracy/Tracy.hpp>

#include "Mesh/Mesh.h"
#include "Renderer.h"
#include "Utilities/Utilities.h"
#include "Managers/Singletons/PSOManager.h"
#include "Materials/Material.h"
#include "Menu/Menu.h"
#include "Materials/MaterialFlags.h"
#include "Render/PSOFlags.h"
#include "Managers/Singletons/DeletionManager.h"
#include "Import/ModelLoader.h"
#include "spdlogStreambuf.h"

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
UINT default_x_res = 1920;
UINT default_y_res = 1080;

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
        SWP_NOACTIVATE      // don’t steal focus, or add SWP_SHOWWINDOW if needed
    );

    if (hwnd == nullptr) {
        MessageBox(nullptr, L"Failed to create window", L"Error", MB_OK);
        throw std::runtime_error("Failed to create window.");
    }

    ShowWindow(hwnd, nCmdShow);

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

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd) {
    //tracy::SetThreadName("Main");

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    auto file_logger = spdlog::basic_logger_mt("file_logger", "logs/log.txt");
    spdlog::set_default_logger(file_logger);
    file_logger->flush_on(spdlog::level::info);

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

    renderer.Initialize(hwnd, x_res, y_res);
    spdlog::info("Renderer initialized.");
    renderer.SetInputMode(InputMode::wasd);

    auto baseScene = std::make_shared<Scene>();

    //auto dragonScene = LoadModel("models/dragon.glb");
    //dragonScene->GetRoot().set<Components::Scale>({ 100, 100, 100 });
    //dragonScene->GetRoot().set<Components::Position>({ 0.0, 1, 1.0 });

    //auto carScene = LoadModel("models/porche.glb");
    //carScene->GetRoot().set<Components::Scale>({ 0.6, 0.6, 0.6 });
    //carScene->GetRoot().set<Components::Position>({ 1.0, 0.0, 1.0 });
    //auto sphereScene = LoadModel("models/sphere.glb");

	//auto mountainScene = LoadModel("models/terrain.glb");
	//mountainScene->GetRoot().set<Components::Scale>({ 50.0, 50.0, 50.0 });
	//mountainScene->GetRoot().set<Components::Position>({ 0.0, -2.0, 0.0 });

    //auto tigerScene = LoadModel("models/tiger.glb");
    //tigerScene->GetRoot().set<Components::Scale>({ 0.01, 0.01, 0.01 });

	//auto shiba = LoadModel("models/shiba.glb");

    //auto usdScene = LoadModel("models/sponza.usdz");
    
    //auto bistro = LoadModel("models/bistroExteriorNoMats.usdz");
    //auto bistro = LoadModel("models/bistroExterior.glb");
    //auto wine = LoadModel("models/bistroInterior.usdz");
    //bistro->GetRoot().set<Components::Scale>({ 0.01, 0.01, 0.01 });

    //auto robot = LoadModel("models/robot.usdz");

	//auto zorah = LoadModel("models/zorahv2/zorah_main_public.v2.gltf");

	//auto island = LoadModel("models/island/usd/elements/isMountainB/instance.usda");

	//auto quad = LoadModel("models/quad.usdz");
	
	auto cubes = LoadModel("models/cubes/suspicious_cubes.usda");

    //auto cherry = LoadModel("models/Trees/CherryTree.usd");

    //auto pine = LoadModel("models/Trees/branch.usdz");
	//pine->GetRoot().set<Components::Position>({ 0.0, 2.0, 0.0 });

    //auto needles = LoadModel("models/Trees/PineTree.usd");

	//auto farmhouse = LoadModel("models/iceberglarge.nif");

    renderer.SetCurrentScene(baseScene);

	//renderer.GetCurrentScene()->AppendScene(farmhouse->Clone());

    //renderer.GetCurrentScene()->AppendScene(needles->Clone());

	//renderer.GetCurrentScene()->AppendScene(pine->Clone());

    //renderer.GetCurrentScene()->AppendScene(cherry->Clone());

	renderer.GetCurrentScene()->AppendScene(cubes->Clone());
    
	//renderer.GetCurrentScene()->AppendScene(carScene->Clone());

	//renderer.GetCurrentScene()->AppendScene(quad->Clone());
	//quad->GetRoot().set<Components::Position>({ 0.0, -2.0, 0.0 });
	//renderer.GetCurrentScene()->AppendScene(quad->Clone());

	//renderer.GetCurrentScene()->AppendScene(island->Clone());

	//renderer.GetCurrentScene()->AppendScene(zorah->Clone());

    //mountainScene = LoadModel("models/terrain.glb");
 //   mountainScene->GetRoot().set<Components::Scale>({ 50.0, 50.0, 50.0 });
 //   mountainScene->GetRoot().set<Components::Position>({ 0.0, -10.0, 0.0 });
	//renderer.GetCurrentScene()->AppendScene(mountainScene->Clone());

	//renderer.GetCurrentScene()->AppendScene(dragonScene->Clone());
    
	//renderer.GetCurrentScene()->AppendScene(tigerScene->Clone());

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

    XMFLOAT3 lookAt = XMFLOAT3(0.0f, 0.0f, 0.0f);
    XMFLOAT3 up = XMFLOAT3(0.0f, 1.0f, 0.0f);
    float fov = 80.0f * (XM_PI / 180.0f); // Converting degrees to radians
    float aspectRatio;
    float zNear = 0.1f;
    float zFar = 1000.0f;


    int clientWidth = x_res; // TODO
    int clientHeight = y_res; // TODO

    aspectRatio = static_cast<float>(clientWidth) / static_cast<float>(clientHeight);
    auto& scene = renderer.GetCurrentScene();
    scene->SetCamera(lookAt, up, fov, aspectRatio, zNear, zFar);
    
	auto light = renderer.GetCurrentScene()->CreateDirectionalLightECS(L"light1", XMFLOAT3(1, 1, 1), 10.0, XMFLOAT3(0, -6, -1));
    //auto light3 = renderer.GetCurrentScene()->CreateSpotLightECS(L"light3", XMFLOAT3(0, 10, 3), XMFLOAT3(1, 1, 1), 2000.0, {0, -1, 0}, .5, .8, 0.0, 0.0, 1.0);
    //auto light1 = renderer.GetCurrentScene()->CreatePointLightECS(L"light1", XMFLOAT3(0, 1, 3), XMFLOAT3(1, 1, 1), 100.0, 0.0, 0.0, 1.0);
    
    for (int i = 0; i < 0; i++) {
		auto point = getRandomPointInVolume(-20, 20, -2, 0, -20, 20);
		auto color = XMFLOAT3(randomFloat(0.0, 1.0), randomFloat(0.0, 1.0), randomFloat(0.0, 1.0));
        auto light1 = renderer.GetCurrentScene()->CreatePointLightECS(L"light"+std::to_wstring(i), XMFLOAT3(point.x, point.y, point.z), color, 3.0, 0.0, 0.0, 1.0, false);
    }

    MSG msg = {};
    unsigned int frameIndex = 0;
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
            renderer.Update(elapsedSeconds.count());
            if (frameIndex % 100 == 0) {
                spdlog::info("FPS: {}", 1 / elapsedSeconds.count());
            }
            renderer.Render();
        }
    }

    renderer.Cleanup();

    return 0;
}

// Window callback procedure
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {

	Menu::GetInstance().HandleInput(hWnd, message, wParam, lParam);

    bool isMouseOverAnyWindow = false;
	bool isMouseCaptured = false;
	bool isKeyboardCaptured = false;

    if (ImGui::GetCurrentContext() != nullptr) {

        ImGuiIO& io = ImGui::GetIO();

        // Check if the mouse is hovering any ImGui window
        isMouseOverAnyWindow = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || ImGui::IsAnyItemHovered();

        // Otherwise, we rely on ImGui's own input capture logic
        isMouseCaptured = io.WantCaptureMouse;
        isKeyboardCaptured = io.WantCaptureKeyboard;
    }

    // If neither the mouse nor the keyboard is captured by ImGui, pass input to the renderer
	// Also allow the renderer to process key up events to prevent the camera from getting stuck moving.
    if ((!isMouseCaptured && !isKeyboardCaptured) || message == WM_KEYUP || !isMouseOverAnyWindow) {
        renderer.GetInputManager().ProcessInput(message, wParam, lParam);
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
