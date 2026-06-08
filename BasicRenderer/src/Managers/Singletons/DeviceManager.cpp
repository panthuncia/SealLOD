#include "Managers/Singletons/DeviceManager.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

#include <spdlog/spdlog.h>
#include <rhi_debug.h>

#include "Managers/Singletons/SettingsManager.h"

namespace br {

namespace {
void LogInstrumentationDiagnostics(rhi::Device device) {
    auto diagnostics = rhi::debug::GetInstrumentationDiagnostics(device);
    for (const auto& diagnostic : diagnostics) {
        switch (diagnostic.severity) {
        case rhi::DebugInstrumentationDiagnosticSeverity::Info:
            spdlog::info("DeviceManager ReShape diagnostic: {}", diagnostic.message);
            break;
        case rhi::DebugInstrumentationDiagnosticSeverity::Warning:
            spdlog::warn("DeviceManager ReShape diagnostic: {}", diagnostic.message);
            break;
        case rhi::DebugInstrumentationDiagnosticSeverity::Error:
        default:
            spdlog::error("DeviceManager ReShape diagnostic: {}", diagnostic.message);
            break;
        }
    }
}

bool IsStreamlineDisabledByEnvironment() {
    char* value = nullptr;
    size_t len = 0;
    if (_dupenv_s(&value, &len, "BASICRENDERER_DISABLE_STREAMLINE") != 0 || value == nullptr) {
        return false;
    }
    const bool disabled = value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' || value[0] == 'Y';
    free(value);
    return disabled;
}

bool IsDiagnosticsBuild() {
#if BUILD_TYPE == BUILD_TYPE_DEBUG //|| BUILD_TYPE == BUILD_TYPE_RELEASE_DEBUG
    return true;
#else
    return false;
#endif
}

std::string GetEnvironmentString(const char* name) {
    char* value = nullptr;
    size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) {
        return {};
    }

    std::string result(value);
    free(value);
    return result;
}

std::string NormalizeBackendName(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch) != 0 || ch == '-' || ch == '_'; }), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

rhi::Backend ParseBackendName(const std::string& value, rhi::Backend fallback) {
    const std::string normalized = NormalizeBackendName(value);
    if (normalized == "vulkan" || normalized == "vk") {
        return rhi::Backend::Vulkan;
    }
    if (normalized == "d3d12" || normalized == "dx12" || normalized == "direct3d12") {
        return rhi::Backend::D3D12;
    }
    return fallback;
}

rhi::Backend GetRequestedBackend() {
    rhi::Backend backend = backend = SettingsManager::GetInstance().getSettingGetter<rhi::Backend>("rhiBackend")();

    const std::string envBackend = GetEnvironmentString("BASICRENDERER_RHI_BACKEND");
    if (!envBackend.empty()) {
        backend = ParseBackendName(envBackend, backend);
    }

    return backend;
}
}

DeviceManager& DeviceManager::GetInstance() {
    static DeviceManager instance;
    return instance;
}

void DeviceManager::Initialize() {
    auto& settingsManager = SettingsManager::GetInstance();
    auto numFramesInFlight = settingsManager.getSettingGetter<uint8_t>("numFramesInFlight")();
    bool enableStreamline = !IsStreamlineDisabledByEnvironment();
    try {
        enableStreamline = settingsManager.getSettingGetter<bool>("enableStreamline")();
    }
    catch (const std::exception&) {
        enableStreamline = !IsStreamlineDisabledByEnvironment();
    }
    if (IsStreamlineDisabledByEnvironment()) {
        enableStreamline = false;
    }

    const bool enableDebug = IsDiagnosticsBuild();

    bool enableRuntimeInstrumentation = false;
    bool enableSynchronousRecording = false;
    bool enableTexelAddressing = true;
    try {
        enableRuntimeInstrumentation = settingsManager.getSettingGetter<bool>("enableReShape")();
    }
    catch (const std::exception&) {
        enableRuntimeInstrumentation = false;
    }

#if !BASICRHI_ENABLE_RESHAPE
    enableRuntimeInstrumentation = false;
#endif

    try {
        enableSynchronousRecording = settingsManager.getSettingGetter<bool>("reshapeSynchronousRecording")();
    }
    catch (const std::exception&) {
        enableSynchronousRecording = false;
    }

    try {
        enableTexelAddressing = settingsManager.getSettingGetter<bool>("reshapeTexelAddressing")();
    }
    catch (const std::exception&) {
        enableTexelAddressing = true;
    }

    spdlog::info(
        "DeviceManager::Initialize enableStreamline={} enableDebug={} enableReShape={} reshapeSync={} reshapeTexel={} framesInFlight={}",
        enableStreamline,
        enableDebug,
        enableRuntimeInstrumentation,
        enableSynchronousRecording,
        enableTexelAddressing,
        numFramesInFlight);

    const rhi::Backend backend = GetRequestedBackend();

    const rhi::DeviceCreateInfo createInfo{
        .backend = backend,
        .framesInFlight = numFramesInFlight,
        .enableDebug = enableDebug,
        .instrumentation = {
            .enableRuntimeInstrumentation = enableRuntimeInstrumentation,
            .enableSynchronousRecording = enableSynchronousRecording,
            .enableTexelAddressing = enableTexelAddressing,
        },
    };

    const rhi::Result createResult = backend == rhi::Backend::Vulkan
        ? rhi::CreateVulkanDevice(createInfo, m_device, enableStreamline)
        : rhi::CreateD3D12Device(createInfo, m_device, enableStreamline);

    if (!rhi::IsOk(createResult) || !m_device) {
        spdlog::error("DeviceManager::Initialize failed to create backend {} result={}", static_cast<uint32_t>(backend), static_cast<uint32_t>(createResult));
        m_backend = rhi::Backend::Null;
        return;
    }

    m_backend = backend;

    m_graphicsQueue = m_device->GetQueue(rhi::QueueKind::Graphics);
    m_computeQueue = m_device->GetQueue(rhi::QueueKind::Compute);
    m_copyQueue = m_device->GetQueue(rhi::QueueKind::Copy);

    rhi::DebugInstrumentationCapabilities instrumentationCapabilities{};
    if (rhi::IsOk(rhi::debug::GetInstrumentationCapabilities(m_device.Get(), instrumentationCapabilities))) {
        spdlog::info(
            "DeviceManager::Initialize ReShape caps buildEnabled={} installSupported={} globalSupported={} syncSupported={} featureCount={}",
            instrumentationCapabilities.backendBuildEnabled,
            instrumentationCapabilities.installSupported,
            instrumentationCapabilities.globalInstrumentationSupported,
            instrumentationCapabilities.synchronousRecordingSupported,
            instrumentationCapabilities.featureCount);
    }

        rhi::DebugInstrumentationState instrumentationState{};
        if (rhi::IsOk(rhi::debug::GetInstrumentationState(m_device.Get(), instrumentationState))) {
            spdlog::info(
                "DeviceManager::Initialize ReShape state requested={} active={} sync={} texel={} featureMask=0x{:X}",
                instrumentationState.requested,
                instrumentationState.active,
                instrumentationState.synchronousRecording,
                instrumentationState.texelAddressingEnabled,
                instrumentationState.globalFeatureMask);
        }

        auto instrumentationFeatures = rhi::debug::GetInstrumentationFeatures(m_device.Get());
        spdlog::info("DeviceManager::Initialize ReShape queried feature count={}", instrumentationFeatures.size());
        for (const auto& feature : instrumentationFeatures) {
            spdlog::info(
                "DeviceManager::Initialize ReShape feature bit=0x{:X} name='{}' description='{}'",
                feature.featureBit,
                feature.name,
                feature.description);
        }

        if (enableRuntimeInstrumentation && instrumentationState.active && instrumentationFeatures.empty()) {
            spdlog::warn("DeviceManager::Initialize requested ReShape instrumentation, but no backend features were discovered after startup queries.");
        }

        LogInstrumentationDiagnostics(m_device.Get());

    CheckGPUFeatures();
}

void DeviceManager::Cleanup() {
    m_graphicsQueue.Reset();
    m_computeQueue.Reset();
    m_copyQueue.Reset();
    m_backend = rhi::Backend::Null;
    m_meshShadersSupported = false;
    m_rayTracingFeatures = {};
    m_rayTracingSupported = false;
    m_clodRayTracingSupported = false;

    if (m_device) {
        m_device.Reset();
    }
}

void DeviceManager::CheckGPUFeatures() {
    m_meshShadersSupported = false;
    m_rayTracingFeatures = {};
    m_rayTracingSupported = false;
    m_clodRayTracingSupported = false;
    if (!m_device) {
        return;
    }

    MeshShaderFeatureInfo meshShaderFeatures{};
    if (rhi::IsOk(m_device->QueryFeatureInfo(&meshShaderFeatures.header))) {
        m_meshShadersSupported = meshShaderFeatures.meshShader;
    }

    RayTracingFeatureInfo rayTracingFeatures{};
    if (rhi::IsOk(m_device->QueryFeatureInfo(&rayTracingFeatures.header))) {
        m_rayTracingFeatures = rayTracingFeatures;
        m_rayTracingSupported = rayTracingFeatures.pipeline && rayTracingFeatures.accelerationStructure;
        m_clodRayTracingSupported = m_rayTracingSupported
            && rayTracingFeatures.gpuRtasOperations
            && rayTracingFeatures.clusterAccelerationStructure
            && rayTracingFeatures.maxClusterVertices > 0u
            && rayTracingFeatures.maxClusterTriangles > 0u;

        spdlog::info(
            "DeviceManager RT caps pipeline={} rayQuery={} indirect={} gpuRtas={} clusters={} backendTier={} maxClusterVerts={} maxClusterTris={}",
            rayTracingFeatures.pipeline,
            rayTracingFeatures.rayQuery,
            rayTracingFeatures.indirect,
            rayTracingFeatures.gpuRtasOperations,
            rayTracingFeatures.clusterAccelerationStructure,
            static_cast<uint32_t>(rayTracingFeatures.backendTier),
            rayTracingFeatures.maxClusterVertices,
            rayTracingFeatures.maxClusterTriangles);
    }
}

}
