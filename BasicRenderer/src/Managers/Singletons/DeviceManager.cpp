#include "Managers/Singletons/DeviceManager.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <Windows.h>

#include <spdlog/spdlog.h>
#include <rhi_debug.h>
#include <rhi_interop_dx12.h>
#include <rhi_interop_vulkan.h>

#include "Managers/Singletons/SettingsManager.h"
#include "Telemetry/NvPerfIntegration.h"

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

bool IsTruthyEnvironmentValue(const char* name) {
    char* value = nullptr;
    size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) {
        return false;
    }

    std::string text(value);
    free(value);
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text == "1" || text == "true" || text == "yes" || text == "on";
}

bool IsNvPerfCaptureRequestedByEnvironment() {
    return IsTruthyEnvironmentValue("BASICRENDERER_NVPERF_CAPTURE");
}

bool IsMultiRHIRequested() {
    return IsTruthyEnvironmentValue("BASICRENDERER_MULTI_RHI");
}

bool IsNvPerfD3D12DebugLayerAllowedByEnvironment() {
    return IsTruthyEnvironmentValue("BASICRENDERER_NVPERF_ALLOW_D3D12_DEBUG_LAYER");
}

bool IsDiagnosticsBuild() {
#if BUILD_TYPE == BUILD_TYPE_DEBUG || BUILD_TYPE == BUILD_TYPE_RELEASE_DEBUG
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

    bool enableDebug = IsDiagnosticsBuild() || IsTruthyEnvironmentValue("BASICRENDERER_VULKAN_VALIDATION");
    // Allow an explicit false value to override diagnostics-build defaults.
    // This is useful for isolating validation-layer/driver interactions while
    // retaining validation by default for RelWithDebInfo development builds.
    const std::string validationSetting = GetEnvironmentString("BASICRENDERER_VULKAN_VALIDATION");
    if (!validationSetting.empty() && !IsTruthyEnvironmentValue("BASICRENDERER_VULKAN_VALIDATION")) {
        enableDebug = false;
    }
    const bool nvPerfCaptureRequested =
        IsNvPerfCaptureRequestedByEnvironment() ||
        telemetry::nvperf::CaptureConfigured();
    if (enableDebug && nvPerfCaptureRequested && !IsNvPerfD3D12DebugLayerAllowedByEnvironment()) {
        spdlog::info("DeviceManager::Initialize disabling D3D12 debug layer for NVPerf capture");
        enableDebug = false;
    }

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

	const bool multiRHI = IsMultiRHIRequested();
	if (multiRHI && enableStreamline) {
		// Streamline wraps the D3D12 device and queues. External-memory handles
		// must be created from the native device, so keep the experimental
		// multi-device path free of proxy ownership ambiguity.
		spdlog::info("DeviceManager::Initialize disabling Streamline in multi-RHI mode");
		enableStreamline = false;
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
        .enableExternalInterop = multiRHI,
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

    if (multiRHI) {
        const uint64_t adapterLuid = backend == rhi::Backend::D3D12
            ? rhi::dx12::get_adapter_luid(m_device.Get())
            : rhi::vulkan::get_adapter_luid(m_device.Get());
        if (adapterLuid == 0) {
            spdlog::error("DeviceManager::Initialize multi-RHI adapter does not expose a valid Windows LUID");
            Cleanup();
            return;
        }
        m_peerBackend = backend == rhi::Backend::D3D12 ? rhi::Backend::Vulkan : rhi::Backend::D3D12;
        rhi::DeviceCreateInfo peerInfo = createInfo;
        peerInfo.backend = m_peerBackend;
        peerInfo.enableExternalInterop = true;
        peerInfo.adapterLuid = adapterLuid;
        peerInfo.requireAdapterLuid = true;
        peerInfo.instrumentation = {};
        const rhi::Result peerResult = m_peerBackend == rhi::Backend::Vulkan
            ? rhi::CreateVulkanDevice(peerInfo, m_peerDevice, false)
            : rhi::CreateD3D12Device(peerInfo, m_peerDevice, false);
        if (!rhi::IsOk(peerResult) || !m_peerDevice) {
            spdlog::error("DeviceManager::Initialize failed to create multi-RHI peer backend {} result={}",
                static_cast<uint32_t>(m_peerBackend), static_cast<uint32_t>(peerResult));
            Cleanup();
            return;
        }
        spdlog::info("DeviceManager multi-RHI enabled primary={} peer={} adapterLuid=0x{:016X}",
            static_cast<uint32_t>(m_backend), static_cast<uint32_t>(m_peerBackend), adapterLuid);
    }

    telemetry::nvperf::LogStartupProbe(m_backend, m_device.Get(), m_graphicsQueue);

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
    m_peerBackend = rhi::Backend::Null;
    m_meshShadersSupported = false;
    m_rayTracingFeatures = {};
    m_rayTracingSupported = false;
    m_clodRayTracingSupported = false;

    if (m_device) {
        m_device.Reset();
    }
    if (m_peerDevice) {
        m_peerDevice.Reset();
    }
}

rhi::Result DeviceManager::CreateSharedBuffer(const rhi::ResourceDesc& requestedDesc, SharedBufferPair& out) const {
    out = {};
    if (!m_peerDevice || requestedDesc.type != rhi::ResourceType::Buffer ||
        requestedDesc.heapType != rhi::HeapType::DeviceLocal || requestedDesc.buffer.sizeBytes == 0) {
        return rhi::Result::InvalidArgument;
    }
    rhi::Device d3d12Device = GetDevice(rhi::Backend::D3D12);
    rhi::Device vulkanDevice = GetDevice(rhi::Backend::Vulkan);
    if (!d3d12Device || !vulkanDevice) return rhi::Result::Unsupported;

    rhi::ResourceDesc desc = requestedDesc;
    desc.heapFlags |= rhi::HeapFlags::Shared;
    auto result = d3d12Device.CreateCommittedResource(desc, out.d3d12);
    if (rhi::Failed(result)) return result;
    rhi::dx12::SharedHandle shared{};
    result = rhi::dx12::export_shared_resource(d3d12Device, out.d3d12.Get(), shared);
    if (rhi::Failed(result)) {
        out = {};
        return result;
    }
    result = rhi::vulkan::import_d3d12_buffer(vulkanDevice, shared.value, desc, out.vulkan);
    CloseHandle(static_cast<HANDLE>(shared.value));
    if (rhi::Failed(result)) {
        out = {};
        return result;
    }
    out.sizeBytes = desc.buffer.sizeBytes;
    return rhi::Result::Ok;
}

rhi::Result DeviceManager::CreateSharedTexture(const rhi::ResourceDesc& requestedDesc, SharedTexturePair& out) const {
    out = {};
    if (!m_peerDevice || requestedDesc.type != rhi::ResourceType::Texture2D ||
        requestedDesc.heapType != rhi::HeapType::DeviceLocal) return rhi::Result::InvalidArgument;
    rhi::Device d3d12Device = GetDevice(rhi::Backend::D3D12);
    rhi::Device vulkanDevice = GetDevice(rhi::Backend::Vulkan);
    if (!d3d12Device || !vulkanDevice) return rhi::Result::Unsupported;
    if (!rhi::vulkan::query_d3d12_texture_support(vulkanDevice, requestedDesc).supported) return rhi::Result::Unsupported;
    rhi::ResourceDesc desc = requestedDesc;
    desc.heapFlags |= rhi::HeapFlags::Shared;
    auto result = d3d12Device.CreateCommittedResource(desc, out.d3d12);
    if (rhi::Failed(result)) return result;
    rhi::dx12::SharedHandle handle{};
    result = rhi::dx12::export_shared_resource(d3d12Device, out.d3d12.Get(), handle);
    if (rhi::IsOk(result)) result = rhi::vulkan::import_d3d12_texture(vulkanDevice, handle.value, desc, out.vulkan);
    if (handle.value) CloseHandle(static_cast<HANDLE>(handle.value));
    if (rhi::Failed(result)) { out = {}; return result; }
    out.description = desc;
    return rhi::Result::Ok;
}

rhi::Result DeviceManager::CreateSharedHeap(const rhi::HeapDesc& requestedDesc, SharedHeapPair& out) const {
    out = {};
    if (!m_peerDevice || requestedDesc.sizeBytes == 0 || requestedDesc.memory != rhi::HeapType::DeviceLocal) {
        return rhi::Result::InvalidArgument;
    }
    rhi::Device d3d12Device = GetDevice(rhi::Backend::D3D12);
    rhi::Device vulkanDevice = GetDevice(rhi::Backend::Vulkan);
    if (!d3d12Device || !vulkanDevice || !rhi::vulkan::query_win32_external_interop(vulkanDevice).d3d12Heaps) {
        return rhi::Result::Unsupported;
    }
    rhi::HeapDesc desc = requestedDesc;
    desc.flags |= rhi::HeapFlags::Shared;
    auto result = d3d12Device.CreateHeap(desc, out.d3d12);
    if (rhi::Failed(result)) return result;
    rhi::dx12::SharedHandle handle{};
    result = rhi::dx12::export_shared_heap(d3d12Device, out.d3d12.Get(), handle);
    if (rhi::IsOk(result)) result = rhi::vulkan::import_d3d12_heap(vulkanDevice, handle.value, desc, out.vulkan);
    if (handle.value) CloseHandle(static_cast<HANDLE>(handle.value));
    if (rhi::Failed(result)) { out = {}; return result; }
    out.description = desc;
    return rhi::Result::Ok;
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
        spdlog::info(
            "DeviceManager mesh shader caps meshShader={} taskShader={} derivatives={}",
            meshShaderFeatures.meshShader,
            meshShaderFeatures.taskShader,
            meshShaderFeatures.derivatives);
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
