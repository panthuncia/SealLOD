#include <iostream>
#include <Windows.h>
#include <windowsx.h>
#include <iostream>
#include <bit>
#include <memory>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <imgui.h>
#include <random>
#include <cmath>
#include <io.h>        // _pipe, _dup2, _read, _close
#include <fcntl.h>     // _O_BINARY
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
#include <mutex>
#include <optional>
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
    struct DeferredTelemetryCapture {
        std::mutex mutex;
        std::string label;
        std::optional<ReadbackCaptureResult> depth;
        std::optional<ReadbackCaptureResult> normals;
        std::optional<ReadbackCaptureResult> albedo;
        std::optional<ReadbackCaptureResult> metallicRoughness;
        std::optional<ReadbackCaptureResult> hdr;
        std::optional<ReadbackCaptureResult> debug;
        std::optional<ReadbackCaptureResult> environmentInfo;
        std::optional<ReadbackCaptureResult> opaqueDielectricAverage;
        bool complete = false;
    };

    float HalfToFloat(uint16_t value) {
        const uint32_t sign = static_cast<uint32_t>(value & 0x8000u) << 16u;
        uint32_t exponent = (value >> 10u) & 0x1fu;
        uint32_t mantissa = value & 0x03ffu;
        uint32_t bits = 0;
        if (exponent == 0u) {
            if (mantissa == 0u) {
                bits = sign;
            }
            else {
                int shift = 0;
                while ((mantissa & 0x0400u) == 0u) {
                    mantissa <<= 1u;
                    ++shift;
                }
                mantissa &= 0x03ffu;
                bits = sign | (static_cast<uint32_t>(127 - 15 - shift) << 23u) | (mantissa << 13u);
            }
        }
        else if (exponent == 0x1fu) {
            bits = sign | 0x7f800000u | (mantissa << 13u);
        }
        else {
            bits = sign | ((exponent + (127u - 15u)) << 23u) | (mantissa << 13u);
        }
        return std::bit_cast<float>(bits);
    }

    void TryLogDeferredTelemetry(const std::shared_ptr<DeferredTelemetryCapture>& capture) {
        std::scoped_lock lock(capture->mutex);
        if (capture->complete || !capture->depth || !capture->normals ||
            !capture->albedo || !capture->metallicRoughness || !capture->hdr ||
            !capture->debug || !capture->environmentInfo || !capture->opaqueDielectricAverage) {
            return;
        }

        const auto& depth = *capture->depth;
        const auto& normals = *capture->normals;
        const auto& albedo = *capture->albedo;
        const auto& material = *capture->metallicRoughness;
        const auto& hdr = *capture->hdr;
        const auto& debug = *capture->debug;
        const auto& environmentInfo = *capture->environmentInfo;
        const auto& opaqueDielectricAverage = *capture->opaqueDielectricAverage;
        if (depth.layouts.empty() || normals.layouts.empty() || albedo.layouts.empty() ||
            material.layouts.empty() || hdr.layouts.empty() || debug.layouts.empty()) {
            spdlog::error("Deferred telemetry capture has no texture footprint.");
            capture->complete = true;
            return;
        }

        const uint32_t width = (std::min)({ depth.width, normals.width, albedo.width, material.width, hdr.width, debug.width });
        const uint32_t height = (std::min)({ depth.height, normals.height, albedo.height, material.height, hdr.height, debug.height });
        const auto& depthLayout = depth.layouts.front();
        const auto& normalLayout = normals.layouts.front();
        const auto& albedoLayout = albedo.layouts.front();
        const auto& materialLayout = material.layouts.front();
        const auto& hdrLayout = hdr.layouts.front();
        const auto& debugLayout = debug.layouts.front();
        uint64_t geometryPixels = 0;
        uint64_t zeroNormalPixels = 0;
        uint64_t invalidNormalPixels = 0;
        uint64_t zeroAlbedoPixels = 0;
        uint64_t zeroRoughnessPixels = 0;
        uint64_t lowRoughnessPixels = 0;
        double normalLengthSum = 0.0;
        double metallicSum = 0.0;
        double roughnessSum = 0.0;
        float minRoughness = 1.0f;
        float maxRoughness = 0.0f;
        double hdrLuminanceSum = 0.0;
        double debugLuminanceSum = 0.0;
        float hdrLuminanceMax = 0.0f;
        float debugLuminanceMax = 0.0f;
        uint64_t hdrOverOne = 0;
        uint64_t hdrInvalid = 0;
        uint64_t debugInvalid = 0;
        uint64_t diffuseCompOverOneHundred = 0;
        float maxDiffuseComp = 0.0f;
        float maxDiffuseCompIor = 0.0f;
        float maxDiffuseCompAlpha = 0.0f;
        float minSampledAverageComplement = 1.0f;

        for (uint32_t y = 0; y < height; ++y) {
            const auto* depthRow = reinterpret_cast<const float*>(
                depth.data.data() + depthLayout.offset + static_cast<uint64_t>(y) * depthLayout.rowPitch);
            const auto* normalRow = reinterpret_cast<const float*>(
                normals.data.data() + normalLayout.offset + static_cast<uint64_t>(y) * normalLayout.rowPitch);
            const auto* albedoRow = reinterpret_cast<const uint8_t*>(
                albedo.data.data() + albedoLayout.offset + static_cast<uint64_t>(y) * albedoLayout.rowPitch);
            const auto* materialRow = reinterpret_cast<const uint8_t*>(
                material.data.data() + materialLayout.offset + static_cast<uint64_t>(y) * materialLayout.rowPitch);
            const auto* hdrRow = reinterpret_cast<const uint16_t*>(
                hdr.data.data() + hdrLayout.offset + static_cast<uint64_t>(y) * hdrLayout.rowPitch);
            const auto* debugRow = reinterpret_cast<const uint32_t*>(
                debug.data.data() + debugLayout.offset + static_cast<uint64_t>(y) * debugLayout.rowPitch);
            for (uint32_t x = 0; x < width; ++x) {
                const float z = depthRow[x];
                if (!std::isfinite(z) || std::bit_cast<uint32_t>(z) == 0x7f7fffffu) {
                    continue;
                }
                ++geometryPixels;
                const float nx = normalRow[x * 4u + 0u];
                const float ny = normalRow[x * 4u + 1u];
                const float nz = normalRow[x * 4u + 2u];
                const float normalLength = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (!std::isfinite(normalLength)) ++invalidNormalPixels;
                else {
                    normalLengthSum += normalLength;
                    if (normalLength < 0.1f) ++zeroNormalPixels;
                }
                const auto* a = albedoRow + x * 4u;
                if ((a[0] | a[1] | a[2] | a[3]) == 0u) ++zeroAlbedoPixels;
                const auto* mr = materialRow + x * 4u;
                const float metallic = static_cast<float>(mr[0]) / 255.0f;
                const float roughness = static_cast<float>(mr[1]) / 255.0f;
                metallicSum += metallic;
                roughnessSum += roughness;
                minRoughness = (std::min)(minRoughness, roughness);
                maxRoughness = (std::max)(maxRoughness, roughness);
                if (mr[1] == 0u) ++zeroRoughnessPixels;
                if (roughness < 0.1f) ++lowRoughnessPixels;

                const float hr = HalfToFloat(hdrRow[x * 4u + 0u]);
                const float hg = HalfToFloat(hdrRow[x * 4u + 1u]);
                const float hb = HalfToFloat(hdrRow[x * 4u + 2u]);
                const float hdrLuminance = 0.2126f * hr + 0.7152f * hg + 0.0722f * hb;
                if (std::isfinite(hdrLuminance)) {
                    hdrLuminanceSum += hdrLuminance;
                    hdrLuminanceMax = (std::max)(hdrLuminanceMax, hdrLuminance);
                    if (hdrLuminance > 1.0f) ++hdrOverOne;
                }
                else {
                    ++hdrInvalid;
                }

                const uint32_t packedXY = debugRow[x * 2u + 0u];
                const uint32_t packedZ = debugRow[x * 2u + 1u];
                if (capture->label == "openpbr-diffuse") {
                    const float ior = HalfToFloat(static_cast<uint16_t>(packedXY & 0xffffu));
                    const float alpha = HalfToFloat(static_cast<uint16_t>(packedXY >> 16u));
                    const float averageComplement = HalfToFloat(static_cast<uint16_t>(packedZ & 0xffffu));
                    const float diffuseComp = HalfToFloat(static_cast<uint16_t>(packedZ >> 16u));
                    if (std::isfinite(averageComplement)) {
                        minSampledAverageComplement = (std::min)(minSampledAverageComplement, averageComplement);
                    }
                    if (std::isfinite(diffuseComp)) {
                        if (diffuseComp > 100.0f) ++diffuseCompOverOneHundred;
                        if (diffuseComp > maxDiffuseComp) {
                            maxDiffuseComp = diffuseComp;
                            maxDiffuseCompIor = ior;
                            maxDiffuseCompAlpha = alpha;
                        }
                    }
                    else {
                        ++debugInvalid;
                    }
                    continue;
                }
                const float dr = HalfToFloat(static_cast<uint16_t>(packedXY & 0xffffu));
                const float dg = HalfToFloat(static_cast<uint16_t>(packedXY >> 16u));
                const float db = HalfToFloat(static_cast<uint16_t>(packedZ & 0xffffu));
                const float debugLuminance = 0.2126f * dr + 0.7152f * dg + 0.0722f * db;
                if (std::isfinite(debugLuminance)) {
                    debugLuminanceSum += debugLuminance;
                    debugLuminanceMax = (std::max)(debugLuminanceMax, debugLuminance);
                }
                else {
                    ++debugInvalid;
                }
            }
        }

        const double divisor = geometryPixels ? static_cast<double>(geometryPixels) : 1.0;
        spdlog::info(
            "Deferred telemetry [{}]: dimensions={}x{} geometry={} normal(zero={} invalid={} avg_len={:.4f}) albedo_zero={} material(metal_avg={:.4f} rough_avg={:.4f} rough_min={:.4f} rough_max={:.4f} rough_zero={} rough_lt_0.1={}) HDR(avg_luma={:.4f} max_luma={:.4f} over_one={} invalid={}) debug(avg_luma={:.4f} max_luma={:.4f} invalid={})",
            capture->label, width, height, geometryPixels, zeroNormalPixels, invalidNormalPixels,
            normalLengthSum / divisor, zeroAlbedoPixels, metallicSum / divisor, roughnessSum / divisor,
            minRoughness, maxRoughness, zeroRoughnessPixels, lowRoughnessPixels,
            hdrLuminanceSum / divisor, hdrLuminanceMax, hdrOverOne, hdrInvalid,
            debugLuminanceSum / divisor, debugLuminanceMax, debugInvalid);
        if (capture->label == "openpbr-diffuse") {
            spdlog::info(
                "Deferred telemetry [openpbr-diffuse]: min_average_complement={} max_diffuse_comp={} max_comp_ior={} max_comp_alpha={} comp_over_100={} invalid={}",
                minSampledAverageComplement, maxDiffuseComp, maxDiffuseCompIor,
                maxDiffuseCompAlpha, diffuseCompOverOneHundred, debugInvalid);
        }

        if (!opaqueDielectricAverage.layouts.empty()) {
            uint16_t minRaw = UINT16_MAX;
            uint16_t maxRaw = 0u;
            uint64_t zeroRaw = 0u;
            for (const auto& layout : opaqueDielectricAverage.layouts) {
                const uint32_t rows = layout.height;
                const uint32_t valuesPerRow = layout.width;
                for (uint32_t row = 0; row < rows; ++row) {
                    const auto* values = reinterpret_cast<const uint16_t*>(
                        opaqueDielectricAverage.data.data() + layout.offset +
                        static_cast<uint64_t>(row) * layout.rowPitch);
                    uint16_t rowMin = UINT16_MAX;
                    uint16_t rowMax = 0u;
                    uint32_t rowZeros = 0u;
                    for (uint32_t column = 0; column < valuesPerRow; ++column) {
                        minRaw = (std::min)(minRaw, values[column]);
                        maxRaw = (std::max)(maxRaw, values[column]);
                        if (values[column] == 0u) ++zeroRaw;
                        rowMin = (std::min)(rowMin, values[column]);
                        rowMax = (std::max)(rowMax, values[column]);
                        if (values[column] == 0u) ++rowZeros;
                    }
                    spdlog::info(
                        "Deferred telemetry [openpbr-diffuse]: opaque-average row={} min={} max={} zeros={}",
                        row, rowMin, rowMax, rowZeros);
                }
            }
            spdlog::info(
                "Deferred telemetry [{}]: opaque-average LUT raw_min={} raw_max={} zeros={} layouts={}",
                capture->label, minRaw, maxRaw, zeroRaw, opaqueDielectricAverage.layouts.size());
        }

        if (environmentInfo.data.size() >= sizeof(EnvironmentInfo)) {
            EnvironmentInfo info{};
            std::memcpy(&info, environmentInfo.data.data(), sizeof(info));
            int minSH = info.sphericalHarmonics[0];
            int maxSH = info.sphericalHarmonics[0];
            int64_t absSH = 0;
            for (const int coefficient : info.sphericalHarmonics) {
                minSH = (std::min)(minSH, coefficient);
                maxSH = (std::max)(maxSH, coefficient);
                absSH += std::abs(static_cast<int64_t>(coefficient));
            }
            spdlog::info(
                "Deferred telemetry [{}]: environment cubemap_srv={} prefiltered_srv={} sh_scale={} sh_min={} sh_max={} sh_abs_sum={}",
                capture->label, info.cubeMapDescriptorIndex, info.prefilteredCubemapDescriptorIndex,
                info.sphericalHarmonicsScale, minSH, maxSH, absSH);
        }
        capture->complete = true;
    }

    void RequestDeferredTelemetry(Renderer& renderer, const std::shared_ptr<DeferredTelemetryCapture>& capture) {
        auto* graph = renderer.GetRenderGraph();
        auto* readback = graph ? graph->GetReadbackService() : nullptr;
        if (!graph || !readback) {
            spdlog::error("Deferred telemetry could not access the active render graph readback service.");
            return;
        }
        const auto request = [&](ResourceIdentifier id, std::optional<ReadbackCaptureResult> DeferredTelemetryCapture::* member) {
            auto resource = graph->RequestResourcePtr(id, true);
            if (!resource) {
                spdlog::error("Deferred telemetry resource is unavailable: {}", id.ToString());
                return;
            }
            readback->RequestReadbackCapture(
                "DeferredShadingPass", resource.get(), RangeSpec{},
                [capture, member](ReadbackCaptureResult&& result) {
                    {
                        std::scoped_lock lock(capture->mutex);
                        capture.get()->*member = std::move(result);
                    }
                    TryLogDeferredTelemetry(capture);
                });
        };
        request(Builtin::PrimaryCamera::LinearDepthMap, &DeferredTelemetryCapture::depth);
        request(Builtin::GBuffer::Normals, &DeferredTelemetryCapture::normals);
        request(Builtin::GBuffer::Albedo, &DeferredTelemetryCapture::albedo);
        request(Builtin::GBuffer::MetallicRoughness, &DeferredTelemetryCapture::metallicRoughness);
        request(Builtin::Color::HDRColorTarget, &DeferredTelemetryCapture::hdr);
        request(Builtin::DebugVisualization, &DeferredTelemetryCapture::debug);
        request(Builtin::Environment::InfoBuffer, &DeferredTelemetryCapture::environmentInfo);
        request(Builtin::OpenPBR::OpaqueDielectricAverageEnergyComplement,
            &DeferredTelemetryCapture::opaqueDielectricAverage);
    }

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

    return io.WantCaptureMouse ||
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) ||
        ImGui::IsAnyItemHovered() ||
        ImGui::IsAnyItemActive();
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

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd) {
    //tracy::SetThreadName("Main");

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::filesystem::create_directories("logs");
    std::filesystem::remove("logs/log.txt");
    auto file_logger = spdlog::basic_logger_mt("file_logger", "logs/log.txt", true);
    spdlog::set_default_logger(file_logger);
    file_logger->flush_on(spdlog::level::info);

    const std::string_view commandLine = lpCmdLine ? std::string_view(lpCmdLine) : std::string_view{};
    const bool pipelineReplacementSmokeTest =
        commandLine.find("--pipeline-replacement-smoke-test") != std::string_view::npos;
    const bool clodGraphRebuildSmokeTest =
        commandLine.find("--clod-graph-rebuild-smoke-test") != std::string_view::npos;
    const bool deferredTelemetryTest =
        commandLine.find("--deferred-telemetry-test") != std::string_view::npos;
    const bool deferredTelemetryDiffuse =
        commandLine.find("--deferred-telemetry-diffuse") != std::string_view::npos;
    const bool deferredTelemetrySpecular =
        commandLine.find("--deferred-telemetry-specular") != std::string_view::npos;
    const bool deferredTelemetryNoIbl =
        commandLine.find("--deferred-telemetry-no-ibl") != std::string_view::npos;
    const bool deferredTelemetryNoPunctual =
        commandLine.find("--deferred-telemetry-no-punctual") != std::string_view::npos;
    const bool graphRebuildSmokeTest = pipelineReplacementSmokeTest || clodGraphRebuildSmokeTest;
    auto deferredTelemetryCapture = std::make_shared<DeferredTelemetryCapture>();

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

    renderer.Initialize(hwnd, x_res, y_res, br::pipeline::MakeBasicRendererDemoPipeline());
    spdlog::info("Renderer initialized.");
    if (deferredTelemetryTest) {
        SettingsManager::GetInstance().getSettingSetter<bool>("renderGraphCompileDumpEnabled")(true);
        ResetD3D12SmokeValidationMessages();
        if (deferredTelemetryDiffuse) {
            deferredTelemetryCapture->label = "diffuse-ibl";
        }
        else if (deferredTelemetrySpecular) {
            deferredTelemetryCapture->label = "specular-ibl";
        }
        else if (deferredTelemetryNoIbl) {
            deferredTelemetryCapture->label = "no-ibl";
        }
        else if (deferredTelemetryNoPunctual) {
            deferredTelemetryCapture->label = "no-punctual";
        }
        else {
            deferredTelemetryCapture->label = "color";
        }
    }
    if (graphRebuildSmokeTest) {
        SettingsManager::GetInstance().getSettingSetter<bool>("renderGraphCompileDumpEnabled")(true);
        ResetD3D12SmokeValidationMessages();
        if (pipelineReplacementSmokeTest) {
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

	auto zorah = LoadModel("models/zorahv2/zorah_main_public.v2.gltf");
	//auto zorah = LoadModel("models/bunny.usdc");

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

        XMFLOAT3 pos = XMFLOAT3(2.f, 5.f, 0.f);
        XMFLOAT3 lookAt = XMFLOAT3(0.0f, 5.0f, 0.0f);
        XMFLOAT3 up = XMFLOAT3(0.0f, 1.0f, 0.0f);
        float fov = 80.0f * (XM_PI / 180.0f); // Converting degrees to radians
        float aspectRatio;
        float zNear = 0.1f;
        float zFar = 1000.0f;


        int clientWidth = x_res; // TODO
        int clientHeight = y_res; // TODO

        aspectRatio = static_cast<float>(clientWidth) / static_cast<float>(clientHeight);
        auto& scene = renderer.GetCurrentScene();
        scene->SetCamera(pos, lookAt, up, fov, aspectRatio, zNear, zFar);
    
	    auto light = renderer.GetCurrentScene()->CreateDirectionalLightECS(L"light1", XMFLOAT3(1, 1, 1), 10.0, XMFLOAT3(0, -6, -1));
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
            if (deferredTelemetryTest && frameIndex == 5) {
                auto& settings = SettingsManager::GetInstance();
                if (deferredTelemetryDiffuse) {
                    settings.getSettingSetter<unsigned int>("outputType")(OutputType::OUTPUT_DIFFUSE_IBL);
                }
                else if (deferredTelemetrySpecular) {
                    settings.getSettingSetter<unsigned int>("outputType")(OutputType::OUTPUT_SPECULAR_IBL);
                }
                else if (deferredTelemetryNoIbl) {
                    settings.getSettingSetter<bool>("enableImageBasedLighting")(false);
                }
                else if (deferredTelemetryNoPunctual) {
                    settings.getSettingSetter<bool>("enablePunctualLighting")(false);
                }
            }
            if (deferredTelemetryTest && frameIndex == 20) {
                spdlog::info("Deferred telemetry: requesting correlated GBuffer readbacks at frame {}.", frameIndex);
                D3D12SmokeValidationFailed();
                RequestDeferredTelemetry(renderer, deferredTelemetryCapture);
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
            if (graphRebuildSmokeTest && frameIndex == 480) {
                spdlog::info("Graph-rebuild smoke test completed after {} frames; closing.", frameIndex);
                PostMessage(hwnd, WM_CLOSE, 0, 0);
            }
            if (deferredTelemetryTest && frameIndex >= 240 && deferredTelemetryCapture->complete) {
                spdlog::info("Deferred telemetry test completed after {} frames; closing.", frameIndex);
                PostMessage(hwnd, WM_CLOSE, 0, 0);
            }
            if (deferredTelemetryTest && frameIndex == 600) {
                spdlog::error("Deferred telemetry test timed out waiting for readback; closing.");
                PostMessage(hwnd, WM_CLOSE, 0, 0);
            }
            renderer.Update(elapsedSeconds.count());
            renderer.PostUpdate();
            if (frameIndex % 100 == 0) {
                spdlog::info("FPS: {}", 1 / elapsedSeconds.count());
            }
            renderer.Render();
        }
    }

    const bool smokeValidationFailed = graphRebuildSmokeTest && D3D12SmokeValidationFailed();
    renderer.Cleanup();

    return smokeValidationFailed ? 1 : 0;
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
