#include "PostProcessing/Upscaling/UpscalingManager.h"

#include <spdlog/spdlog.h>
#include <flecs.h>
#include <rhi.h>
#include <BasicTelemetry/Tracy.h>

#include "PostProcessing/FidelityFX/Backend/FfxBackendAdapters.h"
#include "PostProcessing/Upscaling/slHooks.h"
#include "Runtime/Settings/SettingsManager.h"
#include "Runtime/Device/DeviceManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Resources/PixelBuffer.h"
#include "BasicRenderer/Scene/Scene.h"
#include "Utilities/MathUtils.h"
#include "Utilities/Utilities.h"

#include <sl.h>
#include <sl_core_api.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_d.h>
#include "rhi_interop_dx12.h"
#include "rhi_interop_vulkan.h"
#include <algorithm>
#include <condition_variable>
#include <string>

PFunCreateDXGIFactory slCreateDXGIFactory = nullptr;
PFunCreateDXGIFactory1 slCreateDXGIFactory1 = nullptr;
PFunCreateDXGIFactory2 slCreateDXGIFactory2 = nullptr;
PFunDXGIGetDebugInterface1 slDXGIGetDebugInterface1 = nullptr;
PFunD3D12CreateDevice slD3D12CreateDevice = nullptr;
decltype(&slUpgradeInterface) slGetUpgradeInterface = nullptr;

void SlLogMessageCallback(sl::LogType level, const char* message) {
    //spdlog::info("Streamline Log: {}", message);
}

namespace {
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

    bool IsUpscalingDisabledByEnvironment() {
        char* value = nullptr;
        size_t len = 0;
        if (_dupenv_s(&value, &len, "BASICRENDERER_DISABLE_UPSCALING") != 0 || value == nullptr) {
            return false;
        }
        const bool disabled = value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' || value[0] == 'Y';
        free(value);
        return disabled;
    }

    bool IsStreamlineEnabledSetting() {
        if (IsStreamlineDisabledByEnvironment()) {
            return false;
        }
        try {
            return SettingsManager::GetInstance().getSettingGetter<bool>("enableStreamline")();
        }
        catch (const std::exception&) {
            return true;
        }
    }

    // BASICRENDERER_SIMULATE_DLSS_FAILURE="oom=N,settings=M": see UpscalingManager::SimulateFailures.
    void ReadSimulatedFailuresFromEnvironment(uint32_t& outOfMemoryFrames, uint32_t& settingsFailures) {
        char* value = nullptr;
        size_t len = 0;
        if (_dupenv_s(&value, &len, "BASICRENDERER_SIMULATE_DLSS_FAILURE") != 0 || value == nullptr) {
            return;
        }
        const std::string text(value);
        free(value);
        const auto read = [&](const char* key) -> uint32_t {
            const size_t at = text.find(key);
            if (at == std::string::npos) return 0u;
            return static_cast<uint32_t>(std::strtoul(text.c_str() + at + std::strlen(key), nullptr, 10));
        };
        outOfMemoryFrames = read("oom=");
        settingsFailures = read("settings=");
    }

    // BASICRENDERER_UPSCALER_ALLOCATE_AHEAD=0 and the like: the variable is set to 0, false or no.
    bool ReadFalsyEnvironmentFlagLocal(const char* name) {
        char* value = nullptr;
        size_t len = 0;
        if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) {
            return false;
        }
        const bool off = value[0] == '0' || value[0] == 'f' || value[0] == 'F' || value[0] == 'n' || value[0] == 'N';
        free(value);
        return off;
    }

    int ReadIntEnvironmentLocal(const char* name, int fallback) {
        char* value = nullptr;
        size_t len = 0;
        if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) {
            return fallback;
        }
        const int parsed = std::atoi(value);
        free(value);
        return parsed;
    }

    int64_t SteadyNowNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // In renderer world units, which are Skyrim units in SARP (about 14 m). Well
    // above what a player or the benchmark flight covers in one frame, even at low
    // frame rates. Cuts shorter than this need RequestHistoryReset.
    constexpr float kRayReconstructionCameraCutDistance = 1000.0f;

    UpscalingMode ResolveEffectiveUpscalingMode(UpscalingMode requestedMode, bool dlssSupported, bool rayReconstructionSupported) {
        if (requestedMode == UpscalingMode::DLSSRayReconstruction && (!IsStreamlineEnabledSetting() || !rayReconstructionSupported)) {
            requestedMode = UpscalingMode::DLSS;
        }
        if (requestedMode == UpscalingMode::DLSS && (!IsStreamlineEnabledSetting() || !dlssSupported)) {
            return UpscalingMode::None;
        }
        return requestedMode;
    }

    UpscalingMode ReadUpscalingModeSetting(UpscalingMode fallback) {
        try {
            return SettingsManager::GetInstance().getSettingGetter<UpscalingMode>("upscalingMode")();
        }
        catch (const std::exception&) {
            return fallback;
        }
    }

    UpscaleQualityMode ReadUpscalingQualityModeSetting(UpscaleQualityMode fallback) {
        try {
            return SettingsManager::GetInstance().getSettingGetter<UpscaleQualityMode>("upscalingQualityMode")();
        }
        catch (const std::exception&) {
            return fallback;
        }
    }

    sl::DLSSDOptions MakeRayReconstructionOptions(UpscaleQualityMode qualityMode, DirectX::XMUINT2 outputRes) {
        sl::DLSSDOptions options{};
        options.mode = ToSLQualityMode(qualityMode);
        options.outputWidth = outputRes.x;
        options.outputHeight = outputRes.y;
        options.colorBuffersHDR = sl::Boolean::eTrue;
        options.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
        options.dlaaPreset = sl::DLSSDPreset::ePresetE;
        options.qualityPreset = sl::DLSSDPreset::ePresetE;
        options.balancedPreset = sl::DLSSDPreset::ePresetE;
        options.performancePreset = sl::DLSSDPreset::ePresetE;
        options.ultraPerformancePreset = sl::DLSSDPreset::ePresetE;
        options.ultraQualityPreset = sl::DLSSDPreset::ePresetE;
        return options;
    }

    bool MakeStreamlineVulkanTextureResource(
        rhi::Device device,
        org::PixelBuffer* texture,
        rhi::DescriptorSlot viewSlot,
        VkImageLayout layout,
        sl::Resource& resource)
    {
        if (!texture) {
            return false;
        }

        rhi::VulkanResourceInfo resourceInfo{};
        rhi::VulkanDescriptorSlotInfo slotInfo{};
        if (!rhi::vulkan::get_resource_info(texture->GetAPIResource(), resourceInfo) ||
            !rhi::vulkan::get_descriptor_slot_info(device, viewSlot, slotInfo)) {
            return false;
        }

        VkImage image = rhi::vulkan::from_native_void<VkImage>(resourceInfo.resource);
        VkImageView view = rhi::vulkan::from_native_void<VkImageView>(slotInfo.imageView);
        if (image == VK_NULL_HANDLE || view == VK_NULL_HANDLE) {
            return false;
        }

        resource = sl::Resource{ sl::ResourceType::eTex2d, image, nullptr, view, static_cast<uint32_t>(layout) };
        resource.width = resourceInfo.width;
        resource.height = resourceInfo.height;
        resource.nativeFormat = slotInfo.nativeFormat != VK_FORMAT_UNDEFINED ? slotInfo.nativeFormat : resourceInfo.nativeFormat;
        resource.mipLevels = slotInfo.levelCount != 0 ? slotInfo.levelCount : resourceInfo.mipLevels;
        resource.arrayLayers = slotInfo.layerCount != 0 ? slotInfo.layerCount : resourceInfo.arrayLayers;
        resource.flags = resourceInfo.flags;
        resource.usage = resourceInfo.usage;
        return resource.nativeFormat != VK_FORMAT_UNDEFINED;
    }

}

// Extensions/UpscalerReactiveMask.h: the mask the upscaling pass declares and hands to the evaluation.
namespace {
std::mutex g_reactiveMaskMutex;
std::shared_ptr<org::PixelBuffer> g_reactiveMask;
}

namespace br::render {

void SetUpscalerReactiveMask(std::shared_ptr<org::PixelBuffer> mask) {
    std::scoped_lock lock(g_reactiveMaskMutex);
    g_reactiveMask = std::move(mask);
}

std::shared_ptr<org::PixelBuffer> GetUpscalerReactiveMask() {
    std::scoped_lock lock(g_reactiveMaskMutex);
    return g_reactiveMask;
}

} // namespace br::render

void UpscalingManager::SyncSettingsFromSettingsManager()
{
    m_upscalingMode = ReadUpscalingModeSetting(m_upscalingMode);
    m_upscaleQualityMode = ReadUpscalingQualityModeSetting(m_upscaleQualityMode);
    // Without Streamline (multi-RHI turns it off after the setting is registered) DLSS cannot run. The mode
    // still selects jitter and DLSS-only passes, so it must not linger.
    if (m_upscalingMode == UpscalingMode::DLSS && !IsStreamlineEnabledSetting()) {
        m_upscalingMode = UpscalingMode::None;
    }
    // Vulkan temporal upscaling is not yet stable (including the single-device
    // path, where Streamline can fail while setting constants and corrupt its
    // teardown state). Keep it disabled until that integration is validated;
    // this also prevents motion-vector/history defects from masking interop.
    const auto& deviceManager = DeviceManager::GetInstance();
    const bool forceDisabled = IsUpscalingDisabledByEnvironment()
        || deviceManager.GetBackend() == rhi::Backend::Vulkan;
    if (forceDisabled && m_upscalingMode != UpscalingMode::None) {
        static bool logged = false;
        if (!logged) {
            spdlog::info("UpscalingManager: forcing upscaling off for Vulkan stability");
            logged = true;
        }
        m_upscalingMode = UpscalingMode::None;
    }
}

bool CheckStreamlineFeatureSupport(rhi::Device dev, rhi::Backend backend, sl::Feature feature) {
    sl::AdapterInfo ai{};

    if (backend == rhi::Backend::D3D12) {
        IDXGIAdapter4* ad = rhi::dx12::get_adapter(dev);
        if (!ad) {
            return false;
        }
        DXGI_ADAPTER_DESC desc{};
        if (FAILED(ad->GetDesc(&desc))) {
            return false;
        }
        ai.deviceLUID = reinterpret_cast<uint8_t*>(&desc.AdapterLuid);
        ai.deviceLUIDSizeInBytes = sizeof(LUID);
    }
    else if (backend == rhi::Backend::Vulkan) {
        VkPhysicalDevice physicalDevice = rhi::vulkan::get_physical_device(dev);
        if (physicalDevice == VK_NULL_HANDLE) {
            return false;
        }
        ai.vkPhysicalDevice = physicalDevice;
    }
    else {
        return false;
    }

    sl::Result res = sl::Result::eOk;
    if (SL_FAILED(res, slIsFeatureSupported(feature, ai))) {
        return false;
    }
    return true;
}

inline void StoreFloat4x4(const DirectX::XMMATRIX& m, sl::float4x4& target, bool transpose = false)
{
    DirectX::XMMATRIX mTransposed = transpose ? DirectX::XMMatrixTranspose(m) : m;
    DirectX::XMStoreFloat4(
        reinterpret_cast<DirectX::XMFLOAT4*>(&target.row[0]),
        mTransposed.r[0]
    );
    DirectX::XMStoreFloat4(
        reinterpret_cast<DirectX::XMFLOAT4*>(&target.row[1]),
        mTransposed.r[1]
    );
    DirectX::XMStoreFloat4(
        reinterpret_cast<DirectX::XMFLOAT4*>(&target.row[2]),
        mTransposed.r[2]
    );
    DirectX::XMStoreFloat4(
        reinterpret_cast<DirectX::XMFLOAT4*>(&target.row[3]),
        mTransposed.r[3]
    );
}

void UpscalingManager::InitializeAdapter()
{
    SyncSettingsFromSettingsManager();
    const rhi::Backend backend = DeviceManager::GetInstance().GetBackend();
    if (!IsStreamlineEnabledSetting()) {
        m_dlssSupported = false;
        m_rayReconstructionSupported = false;
        if (m_upscalingMode == UpscalingMode::DLSS || m_upscalingMode == UpscalingMode::DLSSRayReconstruction) {
            m_upscalingMode = UpscalingMode::None;
        }
        spdlog::info("UpscalingManager::InitializeAdapter skipped DLSS probing because enableStreamline=false");
        return;
    }

    auto dev = DeviceManager::GetInstance().GetDevice();
	m_dlssSupported = CheckStreamlineFeatureSupport(dev, backend, sl::kFeatureDLSS); // TODO: Query from RHI
	// Only the D3D12 RHI loads the Ray Reconstruction plugin.
	m_rayReconstructionSupported = backend == rhi::Backend::D3D12
		&& CheckStreamlineFeatureSupport(dev, backend, sl::kFeatureDLSS_RR);
	spdlog::info("UpscalingManager: DLSS Ray Reconstruction supported: {}", m_rayReconstructionSupported);
	uint32_t outOfMemoryFrames = 0;
	uint32_t settingsFailures = 0;
	ReadSimulatedFailuresFromEnvironment(outOfMemoryFrames, settingsFailures);
	if (outOfMemoryFrames != 0 || settingsFailures != 0) {
		SimulateFailures(outOfMemoryFrames, settingsFailures);
	}
}

void UpscalingManager::SimulateFailures(uint32_t outOfMemoryFrames, uint32_t settingsFailures) {
    m_simulatedOutOfMemoryFrames.store(outOfMemoryFrames, std::memory_order_relaxed);
    m_simulatedSettingsFailures.store(settingsFailures, std::memory_order_relaxed);
    spdlog::warn("UpscalingManager: simulating {} DLSS evaluations skipped for GPU memory pressure and {} failed "
        "slDLSSDGetOptimalSettings calls", outOfMemoryFrames, settingsFailures);
    if (settingsFailures != 0) {
        // Setup runs only when something reconfigures the upscaler: make that happen.
        m_reconfigureNow.store(true, std::memory_order_relaxed);
    }
}

bool UpscalingManager::TakeSimulatedOutOfMemory() {
    uint32_t remaining = m_simulatedOutOfMemoryFrames.load(std::memory_order_relaxed);
    while (remaining != 0) {
        if (m_simulatedOutOfMemoryFrames.compare_exchange_weak(remaining, remaining - 1, std::memory_order_relaxed)) return true;
    }
    return false;
}

bool UpscalingManager::TakeSimulatedSettingsFailure() {
    uint32_t remaining = m_simulatedSettingsFailures.load(std::memory_order_relaxed);
    while (remaining != 0) {
        if (m_simulatedSettingsFailures.compare_exchange_weak(remaining, remaining - 1, std::memory_order_relaxed)) return true;
    }
    return false;
}

bool UpscalingManager::TakeReconfigureRequest() {
    if (m_reconfigureNow.exchange(false, std::memory_order_relaxed)) {
        return true;
    }
    const int64_t due = m_reconfigureAtNs.load(std::memory_order_relaxed);
    if (due == 0 || SteadyNowNs() < due) {
        return false;
    }
    // Only one caller takes it; Setup schedules the next one if the retry fails too.
    int64_t expected = due;
    if (!m_reconfigureAtNs.compare_exchange_strong(expected, 0, std::memory_order_relaxed)) {
        return false;
    }
    if (m_upscalingMode != UpscalingMode::DLSSRayReconstruction || !m_rayReconstructionSettingsFailed.load(std::memory_order_relaxed)) {
        return false; // Switched away from Ray Reconstruction, or something else already set it up again.
    }
    spdlog::info("UpscalingManager: retrying DLSS Ray Reconstruction (optimal settings failed {} times so far)",
        m_rayReconstructionSettingsFailures.load(std::memory_order_relaxed));
    return true;
}

void UpscalingManager::ProxyDevice() { // TODO: RHI now handles this internally
    SyncSettingsFromSettingsManager();
    switch (m_upscalingMode)
    {
    case UpscalingMode::DLSS: {
        break;
    }
    case UpscalingMode::FSR3: {
        break;
    }
    default:
		break;
    }
}

namespace {
    sl::DLSSOptions MakeSuperResolutionOptions(UpscaleQualityMode qualityMode, DirectX::XMUINT2 outputRes) {
        sl::DLSSOptions dlssOptions = {};
        dlssOptions.mode = ToSLQualityMode(qualityMode);
        dlssOptions.outputWidth = outputRes.x;
        dlssOptions.outputHeight = outputRes.y;
        // Set preferred Render Presets per Perf Quality Mode. These are typically set one time
        // and established while evaluating DLSS SR Image Quality for your Application.
        // It will be set to DSSPreset::eDefault if unspecified.
        // Please Refer to section 3.12 of the DLSS Programming Guide for details.
        dlssOptions.dlaaPreset = sl::DLSSPreset::ePresetK;
        dlssOptions.qualityPreset = sl::DLSSPreset::ePresetK;
        dlssOptions.balancedPreset = sl::DLSSPreset::ePresetK;
        dlssOptions.performancePreset = sl::DLSSPreset::ePresetK;
        dlssOptions.ultraPerformancePreset = sl::DLSSPreset::ePresetF;
        dlssOptions.sharpness = 0;
        dlssOptions.colorBuffersHDR = sl::Boolean::eTrue; // assuming HDR pipeline
        dlssOptions.useAutoExposure = sl::Boolean::eTrue; // autoexposure is not to be used if a proper exposure texture is available
        dlssOptions.alphaUpscalingEnabled = sl::Boolean::eFalse; // experimental alpha upscaling, enable to upscale alpha channel of color texture
        return dlssOptions;
    }

    // FSR's render size for a quality mode, as ffxQueryDescUpscaleGetRenderResolutionFromQualityMode computes it (display size
    // over the mode's ratio), so the render resolution is known before the configuration's context exists.
    DirectX::XMUINT2 FsrRenderResolution(UpscaleQualityMode qualityMode, DirectX::XMUINT2 outputRes) {
        float ratio = 1.7f;
        switch (ToFFXQualityMode(qualityMode)) {
        case FFX_UPSCALE_QUALITY_MODE_NATIVEAA: ratio = 1.0f; break;
        case FFX_UPSCALE_QUALITY_MODE_QUALITY: ratio = 1.5f; break;
        case FFX_UPSCALE_QUALITY_MODE_BALANCED: ratio = 1.7f; break;
        case FFX_UPSCALE_QUALITY_MODE_PERFORMANCE: ratio = 2.0f; break;
        case FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE: ratio = 3.0f; break;
        default: break;
        }
        return { std::max(1u, static_cast<uint32_t>(static_cast<float>(outputRes.x) / ratio)),
            std::max(1u, static_cast<uint32_t>(static_cast<float>(outputRes.y) / ratio)) };
    }

    // A texture description with no memory (a reserved resource), for the tags slAllocateResources needs (see PrepareFeature).
    Microsoft::WRL::ComPtr<ID3D12Resource> MakeStreamlineStandIn(ID3D12Device* device, DXGI_FORMAT format, DirectX::XMUINT2 size, bool unorderedAccess) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = size.x;
        desc.Height = size.y;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
        desc.Flags = unorderedAccess ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        if (FAILED(device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)))) return nullptr;
        return resource;
    }

    double MillisecondsSince(std::chrono::steady_clock::time_point begin) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    }
}

UpscalerConfiguration::~UpscalerConfiguration() {
    auto& manager = UpscalingManager::GetInstance();
    bool prepared = false;
    {
        std::scoped_lock lock(m_feature->mutex);
        // Another configuration still evaluates with this feature.
        if (--m_feature->users != 0) {
            manager.m_configurationsLive.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
        m_feature->released = true;
        prepared = m_feature->prepared;
    }
    manager.m_configurationsLive.fetch_sub(1, std::memory_order_relaxed);
    // Not prepared yet: the worker skips it or frees what it makes. Prepared: a graph may have used it, so after its retirement.
    if (prepared && m_feature->made) manager.Retire(m_feature->viewport, m_feature->fsrContext);
}

void UpscalerConfiguration::WaitPrepared() const {
    {
        std::scoped_lock lock(m_feature->mutex);
        if (m_feature->prepared) return;
    }
    UpscalingManager::GetInstance().RequestPreparation(*this);
    std::unique_lock lock(m_feature->mutex);
    m_feature->changed.wait(lock, [&] { return m_feature->prepared; });
}

void UpscalingManager::RequestPreparation(const UpscalerConfiguration& configuration) {
    {
        std::scoped_lock lock(configuration.m_feature->mutex);
        if (configuration.m_feature->prepared || configuration.m_feature->wanted) return;
        configuration.m_feature->wanted = true;
    }
    std::scoped_lock lock(m_preparationMutex);
    m_preparationChanged.notify_all();
}

void UpscalingManager::RunPreparationWorker() {
    for (;;) {
        std::shared_ptr<UpscalerConfiguration::Feature> feature;
        bool stopping = false;
        {
            // The first queued feature that is wanted or gone (gone: skipped). One that is neither waits for a build to want it.
            const auto ready = [](const std::shared_ptr<UpscalerConfiguration::Feature>& queued) {
                std::scoped_lock featureLock(queued->mutex);
                return queued->wanted || queued->released;
            };
            std::unique_lock lock(m_preparationMutex);
            auto found = m_preparationQueue.end();
            m_preparationChanged.wait(lock, [&] {
                found = std::find_if(m_preparationQueue.begin(), m_preparationQueue.end(), ready);
                return m_stopPreparation || found != m_preparationQueue.end();
            });
            stopping = m_stopPreparation;
            if (stopping) {
                if (m_preparationQueue.empty()) return;
                found = m_preparationQueue.begin();
            }
            feature = *found;
            m_preparationQueue.erase(found);
        }
        bool skip = stopping;
        {
            std::scoped_lock lock(feature->mutex);
            skip = skip || feature->released;
        }
        if (!skip) {
            PrepareFeature(*feature);
            feature->made = true;
        }
        else {
            spdlog::info("UpscalingManager: configuration {} was replaced before its preparation; skipped", feature->serial);
        }
        bool released = false;
        {
            std::scoped_lock lock(feature->mutex);
            feature->prepared = true;
            released = feature->released;
        }
        feature->changed.notify_all();
        // Replaced while it was prepared: no graph used it, and its allocation's commands completed.
        if (released && feature->made) {
            FreeNow(feature->viewport, feature->fsrContext);
            spdlog::info("UpscalingManager: configuration {} was replaced while it was prepared; freed", feature->serial);
        }
    }
}

UpscalerRequest UpscalingManager::MakeRequest(bool rayReconstructionGuides, DirectX::XMUINT2 output) const {
    UpscalerRequest request;
    request.mode = ReadUpscalingModeSetting(m_upscalingMode);
    request.quality = ReadUpscalingQualityModeSetting(m_upscaleQualityMode);
    request.output = output;
    request.rayReconstructionGuides = rayReconstructionGuides;
    return request;
}

std::shared_ptr<UpscalerConfiguration> UpscalingManager::Prepare(const UpscalerRequest& request) {
    BT_ZONE_SCOPE("UpscalingManager::Prepare");
    const auto begin = std::chrono::steady_clock::now();
    m_upscalingMode = request.mode;
    m_upscaleQualityMode = request.quality;
    m_rayReconstructionGuidesAvailable = request.rayReconstructionGuides;
    UpscalingMode mode = request.mode;
    // As SyncSettingsFromSettingsManager: DLSS cannot run without Streamline, and Vulkan and BASICRENDERER_DISABLE_UPSCALING force
    // upscaling off.
    if (mode == UpscalingMode::DLSS && !IsStreamlineEnabledSetting()) mode = UpscalingMode::None;
    if (IsUpscalingDisabledByEnvironment() || DeviceManager::GetInstance().GetBackend() == rhi::Backend::Vulkan) mode = UpscalingMode::None;
    if (mode == UpscalingMode::DLSSRayReconstruction && !(m_rayReconstructionSupported && request.rayReconstructionGuides)) {
        mode = UpscalingMode::DLSS;
    }
    UpscalingMode effective = ResolveEffectiveUpscalingMode(mode, m_dlssSupported, m_rayReconstructionSupported);
    const DirectX::XMUINT2 output = request.output;
    DirectX::XMUINT2 render = output;
    // DLSS's dynamic resolution range at this mode and output size: render sizes a feature made for it takes per evaluation.
    DirectX::XMUINT2 renderMin = output;
    DirectX::XMUINT2 renderMax = output;
    // Every preparation asks for Ray Reconstruction again, whether or not the last one could get its settings.
    m_rayReconstructionSettingsFailed.store(false, std::memory_order_relaxed);
    {
        // The queries are Streamline calls too.
        std::scoped_lock streamlineLock(m_streamlineMutex);
        if (effective == UpscalingMode::DLSSRayReconstruction) {
            sl::DLSSDOptimalSettings rayReconstructionSettings{};
            const sl::DLSSDOptions options = MakeRayReconstructionOptions(request.quality, output);
            const bool simulated = TakeSimulatedSettingsFailure();
            const sl::Result result = simulated ? sl::Result::eErrorInvalidParameter : slDLSSDGetOptimalSettings(options, rayReconstructionSettings);
            if (result != sl::Result::eOk || rayReconstructionSettings.optimalRenderWidth == 0 || rayReconstructionSettings.optimalRenderHeight == 0) {
                // A failure here can be transient (GPU memory pressure, a reconfiguration in flight). It used to clear the adapter's
                // Ray Reconstruction support for good: the menu hid its checkbox and every graph after used DLSS SR, which handles
                // path tracing's noise badly. Now graphs use DLSS SR only until a retry gets the settings: 2 s later, doubling to
                // 32 s while it keeps failing.
                const uint64_t failures = m_rayReconstructionSettingsFailures.fetch_add(1, std::memory_order_relaxed) + 1;
                const double delay = m_rayReconstructionRetryDelaySeconds;
                m_rayReconstructionRetryDelaySeconds = std::min(m_rayReconstructionRetryDelaySeconds * 2.0, 32.0);
                m_rayReconstructionSettingsFailed.store(true, std::memory_order_relaxed);
                m_reconfigureAtNs.store(SteadyNowNs() + static_cast<int64_t>(delay * 1e9), std::memory_order_relaxed);
                spdlog::warn("UpscalingManager: slDLSSDGetOptimalSettings failed with result {}{} ({} failures so far); using DLSS until a "
                    "retry in {:.0f} s", static_cast<int32_t>(result), simulated ? " (simulated)" : "", failures, delay);
                mode = UpscalingMode::DLSS;
                effective = ResolveEffectiveUpscalingMode(UpscalingMode::DLSS, m_dlssSupported, false);
            }
            else {
                if (m_rayReconstructionRetryDelaySeconds != 2.0) {
                    spdlog::info("UpscalingManager: DLSS Ray Reconstruction is back ({} failed optimal-settings requests in all)",
                        m_rayReconstructionSettingsFailures.load(std::memory_order_relaxed));
                }
                m_rayReconstructionRetryDelaySeconds = 2.0;
                m_reconfigureAtNs.store(0, std::memory_order_relaxed);
                render = { rayReconstructionSettings.optimalRenderWidth, rayReconstructionSettings.optimalRenderHeight };
                renderMin = { rayReconstructionSettings.renderWidthMin, rayReconstructionSettings.renderHeightMin };
                renderMax = { rayReconstructionSettings.renderWidthMax, rayReconstructionSettings.renderHeightMax };
            }
        }
        else if (request.mode != UpscalingMode::DLSSRayReconstruction) {
            m_reconfigureAtNs.store(0, std::memory_order_relaxed);
            m_rayReconstructionRetryDelaySeconds = 2.0;
        }
        // Once per output size: every mode's dynamic resolution range, for the log (which render sizes a feature can share).
        static DirectX::XMUINT2 loggedRangesFor{};
        if (effective != UpscalingMode::None && effective != UpscalingMode::FSR3 && (loggedRangesFor.x != output.x || loggedRangesFor.y != output.y)) {
            loggedRangesFor = output;
            for (UpscaleQualityMode q : { UpscaleQualityMode::DLAA, UpscaleQualityMode::Quality, UpscaleQualityMode::Balanced,
                     UpscaleQualityMode::Performance, UpscaleQualityMode::UltraPerformance }) {
                sl::DLSSOptimalSettings sr{};
                sl::DLSSDOptimalSettings rr{};
                const sl::Result srResult = slDLSSGetOptimalSettings(MakeSuperResolutionOptions(q, output), sr);
                const sl::Result rrResult = m_rayReconstructionSupported
                    ? slDLSSDGetOptimalSettings(MakeRayReconstructionOptions(q, output), rr) : sl::Result::eErrorFeatureNotSupported;
                spdlog::info("UpscalingManager: output {}x{} quality {}: DLSS ({}) optimal {}x{} range {}x{} to {}x{}; Ray Reconstruction ({}) "
                    "optimal {}x{} range {}x{} to {}x{}", output.x, output.y, static_cast<int>(q), static_cast<int>(srResult),
                    sr.optimalRenderWidth, sr.optimalRenderHeight, sr.renderWidthMin, sr.renderHeightMin, sr.renderWidthMax,
                    sr.renderHeightMax, static_cast<int>(rrResult), rr.optimalRenderWidth, rr.optimalRenderHeight, rr.renderWidthMin,
                    rr.renderHeightMin, rr.renderWidthMax, rr.renderHeightMax);
            }
        }
        if (effective == UpscalingMode::DLSS) {
            sl::DLSSOptimalSettings dlssSettings{};
            const sl::DLSSOptions dlssOptions = MakeSuperResolutionOptions(request.quality, output);
            if (SL_FAILED(result, slDLSSGetOptimalSettings(dlssOptions, dlssSettings)) || dlssSettings.optimalRenderWidth == 0
                || dlssSettings.optimalRenderHeight == 0) {
                spdlog::error("DLSSGetOptimalSettings failed!");
            }
            else {
                render = { dlssSettings.optimalRenderWidth, dlssSettings.optimalRenderHeight };
                renderMin = { dlssSettings.renderWidthMin, dlssSettings.renderHeightMin };
                renderMax = { dlssSettings.renderWidthMax, dlssSettings.renderHeightMax };
            }
        }
    }
    if (effective == UpscalingMode::FSR3) {
        render = FsrRenderResolution(request.quality, output);
    }
    if (request.mode == UpscalingMode::DLSSRayReconstruction && effective != UpscalingMode::DLSSRayReconstruction) {
        spdlog::warn(
            "UpscalingManager: DLSS Ray Reconstruction is unavailable (supported={}, pipeline writes guides={}, optimal settings failed={}); using {}",
            m_rayReconstructionSupported,
            request.rayReconstructionGuides,
            m_rayReconstructionSettingsFailed.load(std::memory_order_relaxed),
            UpscalingModeNames[static_cast<int>(effective)]);
    }

    // Quality does not change anything without upscaling.
    const UpscaleQualityMode quality = effective == UpscalingMode::None ? UpscaleQualityMode::DLAA : request.quality;
    if (const auto latest = Latest(); latest && latest->SameAs(mode, effective, quality, render, output)) {
        spdlog::info("UpscalingManager: configuration {} ({}) still fits the request; nothing to prepare", latest->Serial(),
            UpscalingModeNames[static_cast<int>(effective)]);
        return nullptr;
    }

    // The mode a new feature is made in. DLAA's range is its own size (99-100%), so a DLAA feature takes no other render size; a
    // Quality one takes 50-100% (the optimal settings' range, preset K in both), DLAA's size among them. So DLAA's feature is made
    // as Quality and evaluated at the output size, and a later DLAA, Quality, Balanced or Performance configuration at this output
    // size shares it (M5 step 6). DLSS SR only (Ray Reconstruction makes its feature again for any other size or mode, see below).
    // BASICRENDERER_UPSCALER_DLAA_AS_QUALITY=0 makes it a DLAA one.
    UpscaleQualityMode featureQuality = quality;
    DirectX::XMUINT2 featureMin = renderMin, featureMax = renderMax;
    if (effective == UpscalingMode::DLSS && quality == UpscaleQualityMode::DLAA && !ReadFalsyEnvironmentFlagLocal("BASICRENDERER_UPSCALER_DLAA_AS_QUALITY")) {
        std::scoped_lock streamlineLock(m_streamlineMutex);
        sl::DLSSOptimalSettings settings{};
        const sl::Result result = slDLSSGetOptimalSettings(MakeSuperResolutionOptions(UpscaleQualityMode::Quality, output), settings);
        if (result == sl::Result::eOk && render.x >= settings.renderWidthMin && render.y >= settings.renderHeightMin
            && render.x <= settings.renderWidthMax && render.y <= settings.renderHeightMax) {
            featureQuality = UpscaleQualityMode::Quality;
            featureMin = { settings.renderWidthMin, settings.renderHeightMin };
            featureMax = { settings.renderWidthMax, settings.renderHeightMax };
        }
    }

    auto configuration = std::shared_ptr<UpscalerConfiguration>(new UpscalerConfiguration());
    auto& made = *configuration;
    made.m_mode = mode;
    made.m_effective = effective;
    made.m_quality = quality;
    made.m_render = render;
    made.m_output = output;
    made.m_guides = request.rayReconstructionGuides;
    made.m_serial = m_nextSerial++;
    const bool streamline = effective == UpscalingMode::DLSS || effective == UpscalingMode::DLSSRayReconstruction;
    // DLSS SR's dynamic resolution: a feature takes any render size in its range per evaluation (the tags' extents), so a new
    // render size at the same output size can evaluate with the latest configuration's feature instead of allocating one (35-40
    // ms that block the running graphs' evaluations, M5 step 3). Measured in M5 step 6: SR evaluates the new size with no new
    // feature (Post 0.7-1.0 ms in the first frame). Ray Reconstruction does not: its first evaluation at another render size makes
    // the feature again inside slEvaluateFeature (36-48 ms on the render thread), so its configurations never share.
    // BASICRENDERER_UPSCALER_SHARE_FEATURE: 0 never, 1 same output size (the default), 2 also a smaller output (experiment),
    // 3 Ray Reconstruction too (experiment).
    const int share = ReadIntEnvironmentLocal("BASICRENDERER_UPSCALER_SHARE_FEATURE", 1);
    if (streamline && share > 0 && (effective == UpscalingMode::DLSS || share >= 3)) {
        std::shared_ptr<UpscalerConfiguration> latest;
        {
            std::scoped_lock lock(m_configurationMutex);
            latest = m_latest;
        }
        if (latest) {
            auto& candidate = *latest->m_feature;
            std::scoped_lock lock(candidate.mutex);
            const bool sameOutput = candidate.output.x == output.x && candidate.output.y == output.y;
            const bool smallerOutput = candidate.output.x >= output.x && candidate.output.y >= output.y;
            const bool inRange = render.x >= candidate.renderMin.x && render.y >= candidate.renderMin.y
                && render.x <= candidate.renderMax.x && render.y <= candidate.renderMax.y;
            if (!candidate.released && candidate.effective == effective && (sameOutput || (share >= 2 && smallerOutput)) && inRange) {
                ++candidate.users;
                made.m_feature = latest->m_feature;
                made.m_sharesFeature = true;
            }
        }
    }
    auto& feature = *made.m_feature;
    if (!made.m_sharesFeature) {
        feature.effective = effective;
        feature.quality = featureQuality;
        feature.render = render;
        feature.output = output;
        feature.renderMin = featureMin;
        feature.renderMax = featureMax;
        feature.serial = made.m_serial;
        if (streamline) feature.viewport = m_nextViewport++;
    }
    m_configurationsLive.fetch_add(1, std::memory_order_relaxed);
    m_configurationsPrepared.fetch_add(1, std::memory_order_relaxed);
    // The first configuration's size is the setting's at once (nothing is attached yet). Later ones' from the attach of the graphs
    // built for them, so readers of the setting outside builds follow the attached graphs.
    if (!Current()) SettingsManager::GetInstance().getSettingSetter<DirectX::XMUINT2>("renderResolution")(render);
    std::shared_ptr<const UpscalerConfiguration> replaced;
    {
        std::scoped_lock lock(m_configurationMutex);
        replaced = std::move(m_latest);
        m_latest = configuration;
    }
    // Outside the lock: a replaced configuration nothing else holds goes here, and its destructor takes it.
    replaced.reset();
    if (made.m_sharesFeature) {
        // Prepared with the configuration that made it (or being prepared: WaitPrepared waits for that).
        RequestPreparation(made);
    }
    else if (streamline || effective == UpscalingMode::FSR3) {
        // On the preparation worker: the DLSS feature takes 35-65 ms to make (NGX), FSR's context its pipelines. It starts when a
        // build captures the configuration (RequestPreparation).
        std::scoped_lock lock(m_preparationMutex);
        if (!m_preparationWorker.joinable()) m_preparationWorker = std::thread([this] { RunPreparationWorker(); });
        m_preparationQueue.push_back(made.m_feature);
        m_preparationChanged.notify_one();
    }
    else {
        std::scoped_lock lock(feature.mutex);
        feature.prepared = true;
    }
    spdlog::info("UpscalingManager: configuration {}: {} (effective {}), quality {}, render {}x{} (range {}x{} to {}x{}), output {}x{}, "
        "viewport {}{}; resolved in {:.2f} ms{}", made.m_serial, UpscalingModeNames[static_cast<int>(mode)],
        UpscalingModeNames[static_cast<int>(effective)], static_cast<int>(quality), render.x, render.y, feature.renderMin.x,
        feature.renderMin.y, feature.renderMax.x, feature.renderMax.y, output.x, output.y, feature.viewport,
        made.m_sharesFeature ? std::string(" (shares the feature of configuration ") + std::to_string(feature.serial) + ", made for quality "
            + std::to_string(static_cast<int>(feature.quality)) + " at " + std::to_string(feature.output.x) + "x" + std::to_string(feature.output.y)
            + ": no allocation)" : std::string(),
        MillisecondsSince(begin), made.m_sharesFeature ? std::string()
            : std::string(", its feature (made for quality ") + std::to_string(static_cast<int>(feature.quality)) + ") is prepared once a build captures it");
    return configuration;
}

void UpscalingManager::PrepareFeature(UpscalerConfiguration::Feature& configuration) {
    BT_ZONE_SCOPE("UpscalingManager::PrepareFeature");
    const auto begin = std::chrono::steady_clock::now();
    const rhi::Backend backend = DeviceManager::GetInstance().GetBackend();
    std::string outcome;
    if (configuration.effective == UpscalingMode::FSR3) {
        ffx::CreateContextDescUpscale createUpscaling;
        createUpscaling.maxUpscaleSize = { configuration.output.x, configuration.output.y };
        createUpscaling.maxRenderSize = { configuration.output.x, configuration.output.y };
        createUpscaling.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
        if (!fidelityfx_backend::api::CreateUpscaleContext(configuration.fsrContext, backend, DeviceManager::GetInstance().GetDevice(), createUpscaling)) {
            spdlog::error("UpscalingManager: configuration {} could not create its FSR context for backend {}", configuration.serial,
                static_cast<uint32_t>(backend));
            configuration.fsrContext = nullptr;
            outcome = "FSR context failed";
        }
        else {
            outcome = "FSR context made";
        }
    }
    else if (backend != rhi::Backend::D3D12) {
        outcome = "DLSS allocated at its first evaluation (not D3D12)";
    }
    else if (ReadFalsyEnvironmentFlagLocal("BASICRENDERER_UPSCALER_ALLOCATE_AHEAD")) {
        outcome = "DLSS allocated at its first evaluation (BASICRENDERER_UPSCALER_ALLOCATE_AHEAD=0)";
    }
    else {
        // slAllocateResources needs the options, tags and constants of the viewport, or it returns eErrorInvalidParameter and logs
        // nothing (M5 step 3 spike). The feature's sizes come from the tags' extents, so the tags are stand-ins of the right sizes
        // and formats with no memory, removed again after. Its initialization commands run on the graphics queue, before any
        // frame that evaluates the configuration (MakeCurrent waits for this thread).
        const bool rayReconstruction = configuration.effective == UpscalingMode::DLSSRayReconstruction;
        const sl::Feature feature = rayReconstruction ? sl::kFeatureDLSS_RR : sl::kFeatureDLSS;
        const sl::ViewportHandle viewport(configuration.viewport);
        auto device = DeviceManager::GetInstance().GetDevice();
        auto* d3d = rhi::dx12::get_device(device);
        rhi::CommandAllocatorPtr allocator;
        rhi::CommandListPtr list;
        rhi::TimelinePtr done;
        if (!d3d || rhi::Failed(device.CreateCommandAllocator(rhi::QueueKind::Graphics, allocator))
            || rhi::Failed(device.CreateCommandList(rhi::QueueKind::Graphics, allocator.Get(), list))
            || rhi::Failed(device.CreateTimeline(done, 0, "UpscalerConfigurationAllocation"))) {
            outcome = "no command list for the allocation; DLSS allocated at its first evaluation";
        }
        else {
            const auto renderSize = configuration.render;
            const auto outputSize = configuration.output;
            auto colorIn = MakeStreamlineStandIn(d3d, DXGI_FORMAT_R16G16B16A16_FLOAT, renderSize, false);
            auto colorOut = MakeStreamlineStandIn(d3d, DXGI_FORMAT_R16G16B16A16_FLOAT, outputSize, true);
            auto depth = MakeStreamlineStandIn(d3d, DXGI_FORMAT_R32_FLOAT, renderSize, false);
            auto motion = MakeStreamlineStandIn(d3d, DXGI_FORMAT_R16G16_FLOAT, renderSize, false);
            sl::Result result = sl::Result::eErrorInvalidParameter;
            double allocateMs = 0.0;
            if (colorIn && colorOut && depth && motion) {
                sl::Resource colorInResource{ sl::ResourceType::eTex2d, colorIn.Get(), nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };
                sl::Resource colorOutResource{ sl::ResourceType::eTex2d, colorOut.Get(), nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };
                sl::Resource depthResource{ sl::ResourceType::eTex2d, depth.Get(), nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };
                sl::Resource motionResource{ sl::ResourceType::eTex2d, motion.Get(), nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };
                sl::Extent renderExtent{ 0, 0, renderSize.x, renderSize.y };
                sl::Extent outputExtent{ 0, 0, outputSize.x, outputSize.y };
                sl::ResourceTag tags[] = {
                    { &colorInResource, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent },
                    { &colorOutResource, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &outputExtent },
                    { &depthResource, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent },
                    { &motionResource, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent },
                };
                auto* nativeList = rhi::dx12::get_streamline_cmd_list(list.Get());
                std::scoped_lock streamlineLock(m_streamlineMutex);
                if (rayReconstruction) {
                    slDLSSDSetOptions(viewport, MakeRayReconstructionOptions(configuration.quality, outputSize));
                }
                else {
                    slDLSSSetOptions(viewport, MakeSuperResolutionOptions(configuration.quality, outputSize));
                }
                slSetTag(viewport, tags, _countof(tags), nativeList);
                // Constants on a frame token of their own, far from the frames' (their index is the frame number).
                sl::FrameToken* token = nullptr;
                const uint32_t tokenIndex = 0x80000000u + static_cast<uint32_t>(configuration.serial);
                sl::Constants constants{};
                const sl::float4x4 identity{ sl::float4(1, 0, 0, 0), sl::float4(0, 1, 0, 0), sl::float4(0, 0, 1, 0), sl::float4(0, 0, 0, 1) };
                constants.cameraViewToClip = constants.clipToCameraView = constants.clipToPrevClip = constants.prevClipToClip = identity;
                constants.cameraUp = { 0, 1, 0 };
                constants.cameraRight = { 1, 0, 0 };
                constants.cameraFwd = { 0, 0, 1 };
                constants.cameraNear = 1.0f;
                constants.cameraFar = 1000.0f;
                constants.cameraFOV = 1.0f;
                constants.cameraAspectRatio = static_cast<float>(renderSize.x) / static_cast<float>(std::max(1u, renderSize.y));
                constants.mvecScale = { -0.5f, 0.5f };
                constants.depthInverted = sl::Boolean::eTrue;
                constants.cameraMotionIncluded = sl::Boolean::eTrue;
                constants.motionVectors3D = sl::Boolean::eFalse;
                constants.reset = sl::Boolean::eTrue;
                if (slGetNewFrameToken(token, &tokenIndex) == sl::Result::eOk && token) {
                    slSetConstants(constants, *token, viewport);
                }
                const auto allocateBegin = std::chrono::steady_clock::now();
                result = slAllocateResources(nativeList, feature, viewport);
                allocateMs = MillisecondsSince(allocateBegin);
                // The stand-ins go with this function: untag them.
                for (auto& tag : tags) tag.resource = nullptr;
                slSetTag(viewport, tags, _countof(tags), nativeList);
            }
            list->End();
            rhi::CommandList commands = list.Get();
            const rhi::TimelinePoint signal{ done->GetHandle(), 1 };
            rhi::SubmitDesc submit{};
            submit.signals = { &signal, 1u };
            const bool submitted = !rhi::Failed(device.GetQueue(rhi::QueueKind::Graphics).Submit({ &commands, 1u }, submit))
                && done->HostWait(1, 10000) == rhi::Result::Ok;
            configuration.allocatedAhead = result == sl::Result::eOk && submitted;
            outcome = configuration.allocatedAhead
                ? std::string(rayReconstruction ? "Ray Reconstruction" : "DLSS") + " feature allocated (slAllocateResources " + std::to_string(allocateMs).substr(0, 5) + " ms)"
                : std::string("slAllocateResources result ") + std::to_string(static_cast<int>(result)) + (submitted ? "" : ", submission failed")
                    + "; allocated at its first evaluation";
        }
    }
    configuration.prepareMs = MillisecondsSince(begin);
    spdlog::info("UpscalingManager: configuration {} prepared on the preparation worker in {:.2f} ms: {}", configuration.serial,
        configuration.prepareMs, outcome);
}

void UpscalingManager::MakeCurrent(const std::shared_ptr<const UpscalerConfiguration>& configuration) {
    if (!configuration) return;
    {
        std::scoped_lock lock(m_configurationMutex);
        if (m_current == configuration) return;
    }
    const auto begin = std::chrono::steady_clock::now();
    configuration->WaitPrepared();
    const double waited = MillisecondsSince(begin);
    std::shared_ptr<const UpscalerConfiguration> replaced;
    {
        std::scoped_lock lock(m_configurationMutex);
        replaced = std::move(m_current);
        m_current = configuration;
        m_lastAttachWaitMs = waited;
    }
    replaced.reset(); // Outside the lock, as in Prepare.
    spdlog::info("UpscalingManager: configuration {} ({}, render {}x{}, output {}x{}, viewport {}) is current from this attach; waited "
        "{:.2f} ms for its preparation ({:.2f} ms, feature allocated ahead: {})", configuration->Serial(),
        UpscalingModeNames[static_cast<int>(configuration->Effective())], configuration->RenderResolution().x,
        configuration->RenderResolution().y, configuration->OutputResolution().x, configuration->OutputResolution().y,
        configuration->Viewport(), waited, configuration->m_feature->prepareMs, configuration->m_feature->allocatedAhead);
}

std::shared_ptr<const UpscalerConfiguration> UpscalingManager::Latest() const {
    std::scoped_lock lock(m_configurationMutex);
    return m_latest;
}

std::shared_ptr<const UpscalerConfiguration> UpscalingManager::Current() const {
    std::scoped_lock lock(m_configurationMutex);
    return m_current;
}

void UpscalingManager::FreeNow(uint32_t viewport, ffx::Context fsrContext) {
    if (m_shutDown.load(std::memory_order_acquire)) return;
    std::scoped_lock streamlineLock(m_streamlineMutex);
    if (viewport != 0) {
        slFreeResources(sl::kFeatureDLSS, sl::ViewportHandle(viewport));
        if (m_rayReconstructionSupported) slFreeResources(sl::kFeatureDLSS_RR, sl::ViewportHandle(viewport));
    }
    if (fsrContext) fidelityfx_backend::api::DestroyContext(fsrContext);
    m_configurationsRetired.fetch_add(1, std::memory_order_relaxed);
}

void UpscalingManager::Retire(uint32_t viewport, ffx::Context fsrContext) {
    // After Shutdown, Streamline and the FFX module go with the device: what retires later is left to the process's exit.
    if ((viewport == 0 && fsrContext == nullptr) || m_shutDown.load(std::memory_order_acquire)) return;
    std::scoped_lock lock(m_configurationMutex);
    m_retired.push_back({ viewport, fsrContext });
}

void UpscalingManager::CollectRetired() {
    std::vector<RetiredConfiguration> retired;
    {
        std::scoped_lock lock(m_configurationMutex);
        retired.swap(m_retired);
    }
    if (retired.empty()) return;
    // The last graph holding each of these has retired, so the GPU is done with their resources (slFreeResources frees at once).
    const auto begin = std::chrono::steady_clock::now();
    std::scoped_lock streamlineLock(m_streamlineMutex);
    for (auto& configuration : retired) {
        if (configuration.viewport != 0) {
            // A Ray Reconstruction viewport may also hold the DLSS SR fallback's feature.
            slFreeResources(sl::kFeatureDLSS, sl::ViewportHandle(configuration.viewport));
            if (m_rayReconstructionSupported) slFreeResources(sl::kFeatureDLSS_RR, sl::ViewportHandle(configuration.viewport));
        }
        if (configuration.fsrContext) fidelityfx_backend::api::DestroyContext(configuration.fsrContext);
        m_configurationsRetired.fetch_add(1, std::memory_order_relaxed);
    }
    spdlog::info("UpscalingManager: released {} retired configuration(s) in {:.2f} ms ({} live)", retired.size(), MillisecondsSince(begin),
        m_configurationsLive.load(std::memory_order_relaxed));
}

DirectX::XMFLOAT2 UpscalingManager::GetJitter(uint64_t frameNumber) {
    // The attached graphs' configuration: a frame jitters for the upscaler that evaluates it.
    const auto current = Current();
    switch (current ? current->Effective() : UpscalingMode::None)
    {
    case UpscalingMode::DLSS:
    case UpscalingMode::DLSSRayReconstruction: {
        unsigned int sequenceLength = 16;
        unsigned int sequenceIndex = frameNumber % sequenceLength;
        DirectX::XMFLOAT2 sequenceOffset = {
            Halton(sequenceIndex + 1, 2) - 0.5f,
            Halton(sequenceIndex + 1, 3) - 0.5f };
        return sequenceOffset;
    }
    case UpscalingMode::FSR3: {
        ffx::Context context = current->m_feature->fsrContext; // prepared: MakeCurrent waited
        if (context == nullptr) {
            return { 0.0f, 0.0f };
        }
        float jitterX = 0.0f, jitterY = 0.0f;
        int32_t jitterPhaseCount = 1;
        ffx::QueryDescUpscaleGetJitterPhaseCount getJitterPhaseDesc{};
        getJitterPhaseDesc.displayWidth = current->OutputResolution().x;
        getJitterPhaseDesc.renderWidth = current->RenderResolution().x;
        getJitterPhaseDesc.pOutPhaseCount = &jitterPhaseCount;
        ffx::ReturnCode retCode = fidelityfx_backend::api::Query(context, getJitterPhaseDesc);
        if (retCode != ffx::ReturnCode::Ok || jitterPhaseCount <= 0) {
            spdlog::warn("UpscalingManager::GetJitter failed to query FSR jitter phase count: {}", static_cast<uint32_t>(retCode));
            return { 0.0f, 0.0f };
        }

        ffx::QueryDescUpscaleGetJitterOffset getJitterOffsetDesc{};
        getJitterOffsetDesc.index = frameNumber % jitterPhaseCount;
        getJitterOffsetDesc.phaseCount = jitterPhaseCount;
        getJitterOffsetDesc.pOutX = &jitterX;
        getJitterOffsetDesc.pOutY = &jitterY;

        retCode = fidelityfx_backend::api::Query(context, getJitterOffsetDesc);
        if (retCode != ffx::ReturnCode::Ok) {
            spdlog::warn("UpscalingManager::GetJitter failed to query FSR jitter offset: {}", static_cast<uint32_t>(retCode));
            return { 0.0f, 0.0f };
        }

        return { jitterX, jitterY };
    }
    default:
		return { 0.0f, 0.0f };
    }
}

bool UpscalingManager::InitSL() {
    SyncSettingsFromSettingsManager();
    if (!IsStreamlineEnabledSetting()) {
        m_dlssSupported = false;
        m_rayReconstructionSupported = false;
        if (m_upscalingMode == UpscalingMode::DLSS || m_upscalingMode == UpscalingMode::DLSSRayReconstruction) {
            m_upscalingMode = UpscalingMode::None;
        }
        spdlog::info("UpscalingManager::InitSL skipped because enableStreamline=false");
        return false;
    }
    return true;
}

bool UpscalingManager::EvaluateDLSS(const UpscalerConfiguration& configuration, rhi::CommandList& commandList, const Components::Camera* camera,
    uint64_t frameNumber, const UpscalingInputs& inputs, bool rayReconstruction) {
    org::PixelBuffer* pHDRTarget = inputs.color;
    org::PixelBuffer* pUpscaledHDRTarget = inputs.output;
    org::PixelBuffer* pDepthTexture = inputs.depth;
    org::PixelBuffer* pMotionVectors = inputs.motionVectors;
    const rhi::Backend backend = DeviceManager::GetInstance().GetBackend();
    if (backend != rhi::Backend::D3D12 && backend != rhi::Backend::Vulkan) {
        spdlog::warn("UpscalingManager::EvaluateDLSS called on unsupported backend {}; skipping.", static_cast<uint32_t>(backend));
        return false;
    }
    // Streamline calls, after the evaluation order's lock (Evaluate): a preparation takes only this one, so a None or FSR
    // evaluation never waits for an allocation.
    std::scoped_lock streamlineLock(m_streamlineMutex);

    sl::FrameToken* frameToken = nullptr;
    const uint32_t streamlineFrameIndex = static_cast<uint32_t>(frameNumber);
    if (SL_FAILED(result, slGetNewFrameToken(frameToken, &streamlineFrameIndex)) || frameToken == nullptr)
    {
        return HandleFailedEvaluation(commandList, frameNumber, inputs, rayReconstruction, false, "no Streamline frame token", 0, FailedOutput::Undefined);
    }

    // The configuration's own viewport: another configuration's graphs may still be evaluating theirs.
    auto myViewport = sl::ViewportHandle(configuration.Viewport());
    auto renderRes = configuration.RenderResolution();
    auto outputRes = configuration.OutputResolution();

    if (rayReconstruction) {
        if (!inputs.diffuseAlbedo || !inputs.specularAlbedo || !inputs.normalRoughness || !inputs.specularHitDistance) {
            return HandleFailedEvaluation(commandList, frameNumber, inputs, true, false, "a guide resource is missing", 0, FailedOutput::Undefined);
        }
        // The options the feature was made with: other options would make Streamline allocate it again at this evaluation.
        sl::DLSSDOptions rayReconstructionOptions = MakeRayReconstructionOptions(configuration.FeatureQuality(), configuration.FeatureOutputResolution());
        StoreFloat4x4(camera->info.view, rayReconstructionOptions.worldToCameraView);
        StoreFloat4x4(camera->info.viewInverse, rayReconstructionOptions.cameraViewToWorld);
        if (SL_FAILED(result, slDLSSDSetOptions(myViewport, rayReconstructionOptions))) {
            return HandleFailedEvaluation(commandList, frameNumber, inputs, true, false, "slDLSSDSetOptions failed", static_cast<int32_t>(result),
                FailedOutput::Undefined);
        }
        // Ray reconstruction's own allocations are invisible to the render graph's memory accounting.
        static uint32_t stateQueries = 0u;
        if (++stateQueries == 300u || stateQueries == 1200u) {
            sl::DLSSDState state{};
            if (slDLSSDGetState(myViewport, state) == sl::Result::eOk) {
                spdlog::info("DLSS Ray Reconstruction estimated VRAM: {:.1f} MiB", static_cast<double>(state.estimatedVRAMUsageInBytes) / 1048576.0);
            }
        }
    }

    sl::Constants consts = {};

    DirectX::XMMATRIX unjitteredProjectionInverse = XMMatrixInverse(nullptr, camera->info.unjitteredProjection);
    sl::float4x4 cameraViewToWorld;
    StoreFloat4x4(camera->info.viewInverse, cameraViewToWorld);
    sl::float4x4 cameraViewToWorldPrev;
    DirectX::XMMATRIX viewPrevInverse = XMMatrixInverse(nullptr, camera->info.prevView);
    StoreFloat4x4(viewPrevInverse, cameraViewToWorldPrev);
    sl::float4x4 cameraViewToPrevCameraView;
    sl::calcCameraToPrevCamera(cameraViewToPrevCameraView, cameraViewToWorld, cameraViewToWorldPrev);
    sl::float4x4 clipToPrevCameraView;

    StoreFloat4x4(camera->info.unjitteredProjection, consts.cameraViewToClip); // Projection matrix
    StoreFloat4x4(unjitteredProjectionInverse, consts.clipToCameraView); // Inverse projection matrix

    sl::matrixMul(clipToPrevCameraView, consts.clipToCameraView, cameraViewToPrevCameraView);

    sl::float4x4 cameraViewToClipPrev;
    StoreFloat4x4(camera->info.prevUnjitteredProjection, cameraViewToClipPrev);
    sl::matrixMul(consts.clipToPrevClip, clipToPrevCameraView, cameraViewToClipPrev); // Transform between current and previous clip space
    sl::matrixFullInvert(consts.prevClipToClip, consts.clipToPrevClip); // Transform between previous and current clip space
    consts.jitterOffset.x = camera->jitterPixelSpace.x;
    consts.jitterOffset.y = camera->jitterPixelSpace.y;

    // The motion buffer stores currentNdc - prevNdc with unjittered projections.
    // Streamline expects normalized current-to-previous screen motion, so convert
    // NDC delta here instead of treating the buffer as pixel-space velocity.
    consts.mvecScale = { -0.5f, 0.5f };

    consts.cameraPinholeOffset = { 0, 0 };
    consts.cameraPos = { camera->info.positionWorldSpace.x, camera->info.positionWorldSpace.y, camera->info.positionWorldSpace.z };

    auto basisVectors = GetBasisVectors3f(camera->info.view);
    consts.cameraUp = { basisVectors.Up.x, basisVectors.Up.y, basisVectors.Up.z };
    consts.cameraRight = { basisVectors.Right.x, basisVectors.Right.y, basisVectors.Right.z };
    consts.cameraFwd = { basisVectors.Forward.x, basisVectors.Forward.y, basisVectors.Forward.z };

    consts.cameraNear = camera->info.zNear;
    consts.cameraFar = camera->info.zFar;
    consts.cameraFOV = camera->info.fov;
    consts.cameraAspectRatio = camera->info.aspectRatio;
    consts.depthInverted = sl::Boolean::eTrue; // Reverse-Z: near=1, far=0
    consts.cameraMotionIncluded = sl::Boolean::eTrue;
    consts.motionVectors3D = sl::Boolean::eFalse;
    consts.motionVectorsDilated = !rayReconstruction && SettingsManager::GetInstance().getSettingGetter<bool>("enableDilatedMotionVectors")()
        ? sl::Boolean::eTrue
        : sl::Boolean::eFalse;
    consts.motionVectorsJittered = sl::Boolean::eFalse;
    bool cameraCut = false;
    if (rayReconstruction) {
        const DirectX::XMVECTOR cameraStep = DirectX::XMVectorSubtract(camera->info.viewInverse.r[3], viewPrevInverse.r[3]);
        cameraCut = DirectX::XMVectorGetX(DirectX::XMVector3Length(cameraStep)) > kRayReconstructionCameraCutDistance;
    }
    // A configuration's first evaluation starts its history: its sizes or mode are new.
    const bool resetHistory = m_resetUpscalerHistory || configuration.m_firstEvaluation || cameraCut;
    consts.reset = resetHistory ? sl::Boolean::eTrue : sl::Boolean::eFalse;

    if (SL_FAILED(result, slSetConstants(consts, *frameToken, myViewport))) // constants are changing per frame so frame index is required
    {
        spdlog::error("Failed to set DLSS constants");
    }

    sl::Resource colorIn{};
    sl::Resource colorOut{};
    sl::Resource depth{};
    sl::Resource mvec{};
    // An extension's reactive mask (Extensions/UpscalerReactiveMask.h), for Super Resolution on D3D12: how far each pixel
    // takes the current frame over its history.
    org::PixelBuffer* pReactiveMask = !rayReconstruction && backend == rhi::Backend::D3D12 ? inputs.reactiveMask : nullptr;
    if (backend == rhi::Backend::Vulkan) {
        rhi::Device device = DeviceManager::GetInstance().GetDevice();
        const rhi::DescriptorSlot colorInView = pHDRTarget->GetSRVInfo(0).slot;
        const rhi::DescriptorSlot colorOutView = pUpscaledHDRTarget->HasUAVShaderVisible()
            ? pUpscaledHDRTarget->GetUAVShaderVisibleInfo(0).slot
            : pUpscaledHDRTarget->GetSRVInfo(0).slot;
        const rhi::DescriptorSlot depthView = pDepthTexture->GetSRVInfo(0).slot;
        const rhi::DescriptorSlot motionVectorView = pMotionVectors->GetSRVInfo(0).slot;

        if (!MakeStreamlineVulkanTextureResource(device, pHDRTarget, colorInView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, colorIn) ||
            !MakeStreamlineVulkanTextureResource(device, pUpscaledHDRTarget, colorOutView, VK_IMAGE_LAYOUT_GENERAL, colorOut) ||
            !MakeStreamlineVulkanTextureResource(device, pDepthTexture, depthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, depth) ||
            !MakeStreamlineVulkanTextureResource(device, pMotionVectors, motionVectorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, mvec)) {
            return HandleFailedEvaluation(commandList, frameNumber, inputs, rayReconstruction, false,
                "Vulkan Streamline resource metadata could not be resolved", 0, FailedOutput::Undefined);
        }
    }
    else {
        colorIn = sl::Resource{ sl::ResourceType::eTex2d, rhi::dx12::get_resource(pHDRTarget->GetAPIResource()), nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };
        colorOut = sl::Resource{ sl::ResourceType::eTex2d, rhi::dx12::get_resource(pUpscaledHDRTarget->GetAPIResource()), nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };
        depth = sl::Resource{ sl::ResourceType::eTex2d, rhi::dx12::get_resource(pDepthTexture->GetAPIResource()), nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };
        mvec = sl::Resource{ sl::ResourceType::eTex2d, rhi::dx12::get_resource(pMotionVectors->GetAPIResource()), nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };
    }
    //sl::Resource exposure = { sl::ResourceType::Tex2d, myExposureBuffer, nullptr, nullptr, nullptr }; // TODO

    sl::Extent renderExtent = { 0, 0, renderRes.x, renderRes.y };
    sl::Extent upscaleExtent = { 0, 0, outputRes.x, outputRes.y };

    void* nativeCommandList = backend == rhi::Backend::Vulkan
        ? static_cast<void*>(rhi::vulkan::get_cmd_list(commandList))
		: static_cast<void*>(rhi::dx12::get_streamline_cmd_list(commandList));

    if (backend == rhi::Backend::Vulkan) {
        const sl::ResourceTag tags[] = {
            sl::ResourceTag{ &colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent },
            sl::ResourceTag{ &colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &upscaleExtent },
            sl::ResourceTag{ &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent },
            sl::ResourceTag{ &mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent },
        };
        if (SL_FAILED(result, slSetTagForFrame(*frameToken, myViewport, tags, _countof(tags), nativeCommandList))) {
            return HandleFailedEvaluation(commandList, frameNumber, inputs, rayReconstruction, false, "tagging Vulkan resources failed",
                static_cast<int32_t>(result), FailedOutput::Undefined);
        }

        const sl::BaseStructure* evaluateInputs[] = { &myViewport };
        const auto evaluateResult = [&] {
            BT_ZONE_SCOPE("UpscalingManager::EvaluateDLSS::slEvaluateFeature");
            if (TakeSimulatedOutOfMemory()) return sl::Result::eWarnOutOfVRAM;
            return slEvaluateFeature(sl::kFeatureDLSS, *frameToken, evaluateInputs, _countof(evaluateInputs), nativeCommandList);
        }();
        bool written = true;
        if (SL_FAILED(result, evaluateResult))
        {
            // The Vulkan graph declares no copy access here: the output is only counted and logged.
            HandleFailedEvaluation(commandList, frameNumber, inputs, rayReconstruction, result == sl::Result::eWarnOutOfVRAM,
                "slEvaluateFeature failed", static_cast<int32_t>(result), FailedOutput::LeaveUndefined);
            written = false;
        }
        else if (NoteSuccessfulEvaluation(frameNumber, rayReconstruction); resetHistory)
        {
            m_resetUpscalerHistory = false;
            configuration.m_firstEvaluation = false;
            spdlog::info(
                "DLSS history reset submitted for frame {} render={}x{} output={}x{}",
                frameNumber,
                renderRes.x,
                renderRes.y,
                outputRes.x,
                outputRes.y);
        }

        rhi::TextureBarrier streamlineOutputBarrier{};
        streamlineOutputBarrier.texture = pUpscaledHDRTarget->GetAPIResource().GetHandle();
        streamlineOutputBarrier.range = { 0, pUpscaledHDRTarget->GetMipLevels(), 0, pUpscaledHDRTarget->GetArraySize() };
        streamlineOutputBarrier.beforeSync = rhi::ResourceSyncState::ClearUnorderedAccessView;
        streamlineOutputBarrier.afterSync = rhi::ResourceSyncState::AllShading | rhi::ResourceSyncState::ClearUnorderedAccessView;
        streamlineOutputBarrier.beforeAccess = rhi::ResourceAccessType::UnorderedAccessClear;
        streamlineOutputBarrier.afterAccess = rhi::ResourceAccessType::UnorderedAccess | rhi::ResourceAccessType::UnorderedAccessClear;
        streamlineOutputBarrier.beforeLayout = rhi::ResourceLayout::UnorderedAccess;
        streamlineOutputBarrier.afterLayout = rhi::ResourceLayout::UnorderedAccess;

        rhi::BarrierBatch streamlineOutputBatch{};
        streamlineOutputBatch.textures = { &streamlineOutputBarrier };
        commandList.Barriers(streamlineOutputBatch);
        return written;
    }
    else
    {
        sl::ResourceTag colorInTag = sl::ResourceTag{ &colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent };
        sl::ResourceTag colorOutTag = sl::ResourceTag{ &colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &upscaleExtent };
        sl::ResourceTag depthTag = sl::ResourceTag{ &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent };
        sl::ResourceTag mvecTag = sl::ResourceTag{ &mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent };

        const auto makeGuideResource = [](org::PixelBuffer* texture) {
            return texture
                ? sl::Resource{ sl::ResourceType::eTex2d, rhi::dx12::get_resource(texture->GetAPIResource()), nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON }
                : sl::Resource{};
        };
        sl::Resource diffuseAlbedo = makeGuideResource(inputs.diffuseAlbedo);
        sl::Resource specularAlbedo = makeGuideResource(inputs.specularAlbedo);
        sl::Resource normalRoughness = makeGuideResource(inputs.normalRoughness);
        sl::Resource specularHitDistance = makeGuideResource(inputs.specularHitDistance);
        sl::ResourceTag diffuseAlbedoTag = sl::ResourceTag{ &diffuseAlbedo, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent };
        sl::ResourceTag specularAlbedoTag = sl::ResourceTag{ &specularAlbedo, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent };
        sl::ResourceTag normalRoughnessTag = sl::ResourceTag{ &normalRoughness, sl::kBufferTypeNormalRoughness, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent };
        sl::ResourceTag specularHitDistanceTag = sl::ResourceTag{ &specularHitDistance, sl::kBufferTypeSpecularHitDistance, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent };

        sl::Resource reactiveMask = makeGuideResource(pReactiveMask);
        sl::ResourceTag reactiveMaskTag = sl::ResourceTag{ &reactiveMask, sl::kBufferTypeBiasCurrentColorHint, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent };

        // The upscale's own five, then either Ray Reconstruction's guides or Super Resolution's optional reactive mask.
        constexpr uint32_t kUpscaleOnlyInputCount = 5;
        const sl::BaseStructure* evaluateInputs[] = {
            &myViewport, &depthTag, &mvecTag, &colorInTag, &colorOutTag,
            &diffuseAlbedoTag, &specularAlbedoTag, &normalRoughnessTag, &specularHitDistanceTag };
        if (pReactiveMask) evaluateInputs[kUpscaleOnlyInputCount] = &reactiveMaskTag;
        const uint32_t evaluateInputCount = rayReconstruction ? _countof(evaluateInputs)
            : kUpscaleOnlyInputCount + (pReactiveMask ? 1u : 0u);
        const sl::Feature evaluatedFeature = rayReconstruction ? sl::kFeatureDLSS_RR : sl::kFeatureDLSS;
        const auto evaluateResult = [&] {
            BT_ZONE_SCOPE("UpscalingManager::EvaluateDLSS::slEvaluateFeature");
            if (TakeSimulatedOutOfMemory()) return sl::Result::eWarnOutOfVRAM;
            return slEvaluateFeature(evaluatedFeature, *frameToken, evaluateInputs, evaluateInputCount, nativeCommandList);
        }();
        if (SL_FAILED(result, evaluateResult))
        {
            // Nothing was written to the output, which is aliased. Where the sizes differ, DLSS SR may still fit: it needs a
            // fraction of Ray Reconstruction's memory (643 MiB at 2560x1600).
            bool superResolutionWrote = false;
            const bool sameSize = pHDRTarget->GetWidth() == pUpscaledHDRTarget->GetWidth() && pHDRTarget->GetHeight() == pUpscaledHDRTarget->GetHeight();
            if (rayReconstruction && !sameSize && m_dlssSupported) {
                if (!configuration.m_superResolutionOptionsSet) {
                    sl::DLSSOptions superResolutionOptions{};
                    superResolutionOptions.mode = ToSLQualityMode(configuration.FeatureQuality());
                    superResolutionOptions.outputWidth = outputRes.x;
                    superResolutionOptions.outputHeight = outputRes.y;
                    superResolutionOptions.colorBuffersHDR = sl::Boolean::eTrue;
                    superResolutionOptions.useAutoExposure = sl::Boolean::eTrue;
                    configuration.m_superResolutionOptionsSet = slDLSSSetOptions(myViewport, superResolutionOptions) == sl::Result::eOk;
                }
                // This frame's constants are already set (a second slSetConstants is eErrorDuplicatedConstants), so SR's history
                // isn't reset: frames it last saw may show through, which is still better than nothing or black.
                sl::Result superResolutionResult = sl::Result::eErrorNotInitialized;
                if (configuration.m_superResolutionOptionsSet) {
                    superResolutionResult = slEvaluateFeature(sl::kFeatureDLSS, *frameToken, evaluateInputs, kUpscaleOnlyInputCount, nativeCommandList);
                    superResolutionWrote = superResolutionResult == sl::Result::eOk;
                }
                static bool reportedSuperResolution = false;
                if (!superResolutionWrote && !reportedSuperResolution) {
                    reportedSuperResolution = true;
                    spdlog::warn("DLSS SR fallback did not run: options set {}, evaluate result {}", configuration.m_superResolutionOptionsSet,
                        static_cast<int32_t>(superResolutionResult));
                }
            }
            return HandleFailedEvaluation(commandList, frameNumber, inputs, rayReconstruction, result == sl::Result::eWarnOutOfVRAM,
                "slEvaluateFeature failed", static_cast<int32_t>(result),
                superResolutionWrote ? FailedOutput::SuperResolution : FailedOutput::Undefined);
        }
        else if (NoteSuccessfulEvaluation(frameNumber, rayReconstruction); resetHistory)
        {
            m_resetUpscalerHistory = false;
            configuration.m_firstEvaluation = false;
            spdlog::info(
                "DLSS history reset submitted for frame {} render={}x{} output={}x{}",
                frameNumber,
                renderRes.x,
                renderRes.y,
                outputRes.x,
                outputRes.y);
        }
        else
        {
            // IMPORTANT: Host is responsible for restoring state on the command list used
            //restoreState(myCmdList); ??
        }
        return true;
    }
}

void UpscalingManager::EvaluateFSR3(const UpscalerConfiguration& configuration, rhi::CommandList& commandList, const Components::Camera* camera,
    double elapsedSeconds, const UpscalingInputs& inputs) {
    org::PixelBuffer* pHDRTarget = inputs.color;
    org::PixelBuffer* pUpscaledHDRTarget = inputs.output;
    org::PixelBuffer* pDepthTexture = inputs.depth;
    org::PixelBuffer* pMotionVectors = inputs.motionVectors;
    ffx::Context context = configuration.m_feature->fsrContext; // prepared: Evaluate waited
    if (context == nullptr) {
        spdlog::warn("UpscalingManager::EvaluateFSR3 skipped dispatch because configuration {} has no FSR context", configuration.Serial());
        return;
    }

    ffx::DispatchDescUpscale dispatchUpscale{};
    const rhi::Backend backend = DeviceManager::GetInstance().GetBackend();

    dispatchUpscale.commandList = fidelityfx_backend::api::GetCommandList(backend, commandList);
    if (dispatchUpscale.commandList == nullptr) {
        spdlog::warn("UpscalingManager::EvaluateFSR3 skipped dispatch because no command list adapter is available for backend {}", static_cast<uint32_t>(backend));
        return;
    }

    dispatchUpscale.color = fidelityfx_backend::api::GetResource(backend, pHDRTarget, L"UpscaleColorIn", FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatchUpscale.depth = fidelityfx_backend::api::GetResource(backend, pDepthTexture, L"UpscaleDepth", FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatchUpscale.motionVectors = fidelityfx_backend::api::GetResource(backend, pMotionVectors, L"UpscaleMotionVectors", FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatchUpscale.output = fidelityfx_backend::api::GetResource(backend, pUpscaledHDRTarget, L"UpscaleColorOut", FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    // An extension's reactive mask (Extensions/UpscalerReactiveMask.h). The transparency and composition mask stays unset.
    if (inputs.reactiveMask) {
        dispatchUpscale.reactive = fidelityfx_backend::api::GetResource(backend, inputs.reactiveMask, L"UpscaleReactiveMask", FFX_API_RESOURCE_STATE_COMPUTE_READ);
    }
    //dispatchUpscale.transparencyAndComposition;

    auto renderRes = configuration.RenderResolution();
    auto outputRes = configuration.OutputResolution();

    // Jitter is calculated earlier in the frame using a callback from the camera update
    dispatchUpscale.jitterOffset.x = -camera->jitterPixelSpace.x;
    dispatchUpscale.jitterOffset.y = -camera->jitterPixelSpace.y;
    // Motion vectors are curNDC - prevNDC in [-1,1]. FSR expects prevScreenPixel - curScreenPixel.
    // X negates direction. Y is positive because direction flip and NDC-Y-up to screen-Y-down cancel.
    // Divide by 2 because NDC spans 2 units for full screen width/height.
    dispatchUpscale.motionVectorScale.x = -static_cast<float>(renderRes.x) / 2.0f;
    dispatchUpscale.motionVectorScale.y = static_cast<float>(renderRes.y) / 2.0f;
    // A configuration's first dispatch starts its history (a new context has none; this also covers a requested reset).
    dispatchUpscale.reset = configuration.m_firstEvaluation || m_resetUpscalerHistory;
    configuration.m_firstEvaluation = false;
    dispatchUpscale.enableSharpening = false;
    //dispatchUpscale.sharpness = m_Sharpness;

    // Engine keeps time in seconds, but FSR expects milliseconds
    dispatchUpscale.frameTimeDelta = static_cast<float>(elapsedSeconds * 1000.f);

    //dispatchUpscale.preExposure = GetScene()->GetSceneExposure();
    dispatchUpscale.renderSize.width = renderRes.x;
    dispatchUpscale.renderSize.height = renderRes.y;
    dispatchUpscale.upscaleSize.width = outputRes.x;
    dispatchUpscale.upscaleSize.height = outputRes.y;

    // Setup camera params as required
    dispatchUpscale.cameraFovAngleVertical = camera->fov;

    // FFX expects reversed-Z dispatch planes in reversed order when DEPTH_INVERTED is set.
    dispatchUpscale.cameraNear = camera->zFar;
    dispatchUpscale.cameraFar = camera->zNear;

    const ffx::ReturnCode dispatchResult = fidelityfx_backend::api::Dispatch(context, dispatchUpscale);
    if (dispatchResult != ffx::ReturnCode::Ok) {
        spdlog::error("UpscalingManager::EvaluateFSR3 dispatch failed: {}", static_cast<uint32_t>(dispatchResult));
    }
}

void UpscalingManager::EvaluateNone(rhi::CommandList& commandList, const Components::Camera* camera, const UpscalingInputs& inputs) {
    org::PixelBuffer* pHDRTarget = inputs.color;
    org::PixelBuffer* pUpscaledHDRTarget = inputs.output;
    UINT mipSlice = 0;
    UINT arraySlice = 0;
    UINT dstSubresource = CalcSubresource(
        /*MipSlice=*/mipSlice,
        /*ArraySlice=*/arraySlice,
        /*PlaneSlice=*/0,
        /*TotalMipCount=*/pUpscaledHDRTarget->GetNumSRVMipLevels(),
        /*ArraySize=*/1);

    rhi::TextureCopyRegion dst = {
        .texture = pUpscaledHDRTarget->GetAPIResource().GetHandle(),
        .mip = mipSlice,
        .arraySlice = arraySlice,
        .x = 0,
        .y = 0,
        .z = 0,
        .depth = 1,
    };
    rhi::TextureCopyRegion src = {
        .texture = pHDRTarget->GetAPIResource().GetHandle(),
        .mip = mipSlice,
        .arraySlice = arraySlice,
        .x = 0,
        .y = 0,
        .z = 0,
        .depth = 1,
    };

    // This function is reached only for a generation whose captured mode is
    // None. The mutable manager selection may already describe a successor.
    const bool graphOwnsCopyBarriers = DeviceManager::GetInstance().GetBackend() == rhi::Backend::Vulkan;

    rhi::TextureSubresourceRange copyRange{};
    copyRange.baseMip = mipSlice;
    copyRange.mipCount = 1;
    copyRange.baseLayer = arraySlice;
    copyRange.layerCount = 1;

    if (!graphOwnsCopyBarriers) {
        rhi::TextureBarrier preCopyBarriers[2]{};
        preCopyBarriers[0].texture = src.texture;
        preCopyBarriers[0].range = copyRange;
        preCopyBarriers[0].beforeSync = rhi::ResourceSyncState::All;
        preCopyBarriers[0].afterSync = rhi::ResourceSyncState::Copy;
        preCopyBarriers[0].beforeAccess = rhi::ResourceAccessType::Common;
        preCopyBarriers[0].afterAccess = rhi::ResourceAccessType::CopySource;
        preCopyBarriers[0].beforeLayout = rhi::ResourceLayout::Common;
        preCopyBarriers[0].afterLayout = rhi::ResourceLayout::CopySource;
        preCopyBarriers[1].texture = dst.texture;
        preCopyBarriers[1].range = copyRange;
        preCopyBarriers[1].beforeSync = rhi::ResourceSyncState::All;
        preCopyBarriers[1].afterSync = rhi::ResourceSyncState::Copy;
        preCopyBarriers[1].beforeAccess = rhi::ResourceAccessType::Common;
        preCopyBarriers[1].afterAccess = rhi::ResourceAccessType::CopyDest;
        preCopyBarriers[1].beforeLayout = rhi::ResourceLayout::Common;
        preCopyBarriers[1].afterLayout = rhi::ResourceLayout::CopyDest;

        rhi::BarrierBatch preCopyBatch{};
        preCopyBatch.textures = rhi::Span<rhi::TextureBarrier>(preCopyBarriers, 2);
        commandList.Barriers(preCopyBatch);
    }

    commandList.CopyTextureRegion(dst, src);

    if (!graphOwnsCopyBarriers) {
        rhi::TextureBarrier postCopyBarriers[2]{};
        postCopyBarriers[0].texture = src.texture;
        postCopyBarriers[0].range = copyRange;
        postCopyBarriers[0].beforeSync = rhi::ResourceSyncState::Copy;
        postCopyBarriers[0].afterSync = rhi::ResourceSyncState::All;
        postCopyBarriers[0].beforeAccess = rhi::ResourceAccessType::CopySource;
        postCopyBarriers[0].afterAccess = rhi::ResourceAccessType::Common;
        postCopyBarriers[0].beforeLayout = rhi::ResourceLayout::CopySource;
        postCopyBarriers[0].afterLayout = rhi::ResourceLayout::Common;
        postCopyBarriers[1].texture = dst.texture;
        postCopyBarriers[1].range = copyRange;
        postCopyBarriers[1].beforeSync = rhi::ResourceSyncState::Copy;
        postCopyBarriers[1].afterSync = rhi::ResourceSyncState::All;
        postCopyBarriers[1].beforeAccess = rhi::ResourceAccessType::CopyDest;
        postCopyBarriers[1].afterAccess = rhi::ResourceAccessType::Common;
        postCopyBarriers[1].beforeLayout = rhi::ResourceLayout::CopyDest;
        postCopyBarriers[1].afterLayout = rhi::ResourceLayout::Common;

        rhi::BarrierBatch postCopyBatch{};
        postCopyBatch.textures = rhi::Span<rhi::TextureBarrier>(postCopyBarriers, 2);
        commandList.Barriers(postCopyBatch);
    }
}

bool UpscalingManager::HandleFailedEvaluation(rhi::CommandList& commandList, uint64_t frameNumber, const UpscalingInputs& inputs,
    bool rayReconstruction, bool outOfMemory, const char* reason, int32_t result, FailedOutput output) {
    const uint64_t failures = m_failedEvaluations.fetch_add(1, std::memory_order_relaxed) + 1;
    if (outOfMemory) m_outOfMemorySkips.fetch_add(1, std::memory_order_relaxed);
    m_lastFailedFrame.store(frameNumber, std::memory_order_relaxed);
    const uint64_t consecutive = m_consecutiveFailures.fetch_add(1, std::memory_order_relaxed) + 1;

    // A defined image in place of the one that wasn't made, so nothing downstream reads the aliased output's old contents.
    bool written = output == FailedOutput::SuperResolution;
    const char* fallback = written ? "DLSS SR" : "nothing";
    if (output == FailedOutput::SuperResolution) {
        m_fallbackSuperResolution.fetch_add(1, std::memory_order_relaxed);
    }
    else if (output == FailedOutput::Undefined && DeviceManager::GetInstance().GetBackend() == rhi::Backend::D3D12 && inputs.color && inputs.output
        && inputs.color->GetWidth() == inputs.output->GetWidth() && inputs.color->GetHeight() == inputs.output->GetHeight()) {
        // The pass declares both as legacy interop in the common state, as it does for no upscaling, which copies the same way.
        EvaluateNone(commandList, nullptr, inputs);
        m_fallbackCopies.fetch_add(1, std::memory_order_relaxed);
        written = true;
        fallback = "the input, copied";
    }
    else if (output == FailedOutput::Undefined) {
        m_fallbackClears.fetch_add(1, std::memory_order_relaxed);
        fallback = "black (cleared by the pass)";
    }

    // At most one line every 5 s, with what happened since the last.
    const auto now = std::chrono::steady_clock::now();
    if (failures == 1 || now - m_lastFailureLog >= std::chrono::seconds(5)) {
        const char* feature = rayReconstruction ? "DLSS Ray Reconstruction" : "DLSS";
        if (outOfMemory) {
            spdlog::warn("{} skipped at frame {} because Streamline reported GPU memory pressure; output: {}. {} failed evaluations in "
                "all ({} since the last report, {} for memory), {} in a row", feature, frameNumber, fallback, failures,
                failures - m_failuresAtLastLog, m_outOfMemorySkips.load(std::memory_order_relaxed), consecutive);
        }
        else {
            spdlog::error("{} evaluation failed at frame {}: {} (result {}); output: {}. {} failed evaluations in all ({} since the last "
                "report), {} in a row", feature, frameNumber, reason, result, fallback, failures, failures - m_failuresAtLastLog, consecutive);
        }
        m_lastFailureLog = now;
        m_failuresAtLastLog = failures;
    }
    return written;
}

void UpscalingManager::NoteSuccessfulEvaluation(uint64_t frameNumber, bool rayReconstruction) {
    const uint64_t skipped = m_consecutiveFailures.exchange(0, std::memory_order_relaxed);
    if (skipped == 0) return;
    spdlog::info("{} evaluated again at frame {} after {} failed evaluations in a row", rayReconstruction ? "DLSS Ray Reconstruction" : "DLSS",
        frameNumber, skipped);
    if (skipped > 1) {
        // Its history is more than a frame old, and this frame's motion vectors only reach back one.
        m_resetUpscalerHistory = true;
    }
}

UpscalingManager::Status UpscalingManager::GetStatus() const {
    Status status;
    status.requested = m_upscalingMode;
    status.current = GetCurrentUpscalingMode();
    status.effective = ResolveEffectiveUpscalingMode(status.current, m_dlssSupported, m_rayReconstructionSupported);
    status.graph = m_lastGraphMode.load(std::memory_order_relaxed);
    status.evaluated = m_lastEvaluatedMode.load(std::memory_order_relaxed);
    status.dlssSupported = m_dlssSupported;
    status.rayReconstructionSupported = m_rayReconstructionSupported;
    status.rayReconstructionGuidesAvailable = m_rayReconstructionGuidesAvailable;
    status.evaluatedFrame = m_lastEvaluatedFrame.load(std::memory_order_relaxed);
    status.evaluations = m_evaluations.load(std::memory_order_relaxed);
    status.failedEvaluations = m_failedEvaluations.load(std::memory_order_relaxed);
    status.outOfMemorySkips = m_outOfMemorySkips.load(std::memory_order_relaxed);
    status.lastFailedFrame = m_lastFailedFrame.load(std::memory_order_relaxed);
    status.consecutiveFailures = m_consecutiveFailures.load(std::memory_order_relaxed);
    status.fallbackCopies = m_fallbackCopies.load(std::memory_order_relaxed);
    status.fallbackSuperResolution = m_fallbackSuperResolution.load(std::memory_order_relaxed);
    status.fallbackClears = m_fallbackClears.load(std::memory_order_relaxed);
    status.rayReconstructionSettingsFailed = m_rayReconstructionSettingsFailed.load(std::memory_order_relaxed);
    status.rayReconstructionSettingsFailures = m_rayReconstructionSettingsFailures.load(std::memory_order_relaxed);
    if (const int64_t due = m_reconfigureAtNs.load(std::memory_order_relaxed); due != 0) {
        status.rayReconstructionRetrySeconds = std::max(0.0, static_cast<double>(due - SteadyNowNs()) * 1e-9);
    }
    const auto describe = [](const std::shared_ptr<const UpscalerConfiguration>& configuration) {
        Status::Configuration out;
        if (!configuration) return out;
        out.serial = configuration->Serial();
        out.viewport = configuration->Viewport();
        out.mode = configuration->Effective();
        out.render = configuration->RenderResolution();
        out.output = configuration->OutputResolution();
        std::scoped_lock featureLock(configuration->m_feature->mutex);
        out.prepared = configuration->m_feature->prepared;
        out.allocatedAhead = out.prepared && configuration->m_feature->allocatedAhead;
        out.prepareMs = out.prepared ? configuration->m_feature->prepareMs : 0.0;
        return out;
    };
    {
        std::scoped_lock lock(m_configurationMutex);
        status.currentConfiguration = describe(m_current);
        status.latestConfiguration = describe(m_latest);
        status.lastAttachWaitMs = m_lastAttachWaitMs;
    }
    status.configurationsPrepared = m_configurationsPrepared.load(std::memory_order_relaxed);
    status.configurationsRetired = m_configurationsRetired.load(std::memory_order_relaxed);
    status.configurationsLive = m_configurationsLive.load(std::memory_order_relaxed);
    return status;
}

bool UpscalingManager::Evaluate(const UpscalerConfiguration& configuration, rhi::CommandList& commandList,
    const Components::Camera* camera, uint64_t frameNumber, double elapsedSeconds,
    const UpscalingInputs& inputs) {
    // Its attach waited for this (MakeCurrent); a graph attached some other way must not evaluate before the feature is ready.
    // Outside the lock: the preparation takes it.
    configuration.WaitPrepared();
    std::scoped_lock evaluateLock(m_evaluateMutex);
    const UpscalingMode effectiveMode = configuration.Effective();
    m_lastGraphMode.store(configuration.Mode(), std::memory_order_relaxed);
    m_lastEvaluatedMode.store(effectiveMode, std::memory_order_relaxed);
    m_lastEvaluatedFrame.store(frameNumber, std::memory_order_relaxed);
    m_evaluations.fetch_add(1, std::memory_order_relaxed);
    switch (effectiveMode)
    {
	    case UpscalingMode::None:
            EvaluateNone(commandList, camera, inputs);
			break;
        case UpscalingMode::DLSS:
			return EvaluateDLSS(configuration, commandList, camera, frameNumber, inputs, false);
        case UpscalingMode::DLSSRayReconstruction:
			return EvaluateDLSS(configuration, commandList, camera, frameNumber, inputs, true);
        case UpscalingMode::FSR3:
			EvaluateFSR3(configuration, commandList, camera, elapsedSeconds, inputs);
            break;
	}
    return true;
}

void UpscalingManager::Shutdown() {
    // The graphs are gone (the device is idle): the configurations the manager holds retire now, with what retired before.
    std::shared_ptr<const UpscalerConfiguration> latest;
    std::shared_ptr<const UpscalerConfiguration> current;
    {
        std::scoped_lock lock(m_configurationMutex);
        latest = std::move(m_latest);
        current = std::move(m_current);
    }
    latest.reset();
    current.reset();
    {
        // What is still queued is skipped.
        std::scoped_lock lock(m_preparationMutex);
        m_stopPreparation = true;
        m_preparationChanged.notify_all();
    }
    if (m_preparationWorker.joinable()) m_preparationWorker.join();
    CollectRetired();
    m_shutDown.store(true, std::memory_order_release);
	fidelityfx_backend::api::UnloadModule();
	// RHI now handles streamline internally
}
