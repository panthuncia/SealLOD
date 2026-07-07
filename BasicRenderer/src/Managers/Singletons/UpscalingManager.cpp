#include "Managers/Singletons/UpscalingManager.h"

#include <spdlog/spdlog.h>
#include <flecs.h>
#include <rhi.h>

#include "FidelityFX/FfxBackendAdapters.h"
#include "slHooks.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Render/RenderContext.h"
#include "Resources/PixelBuffer.h"
#include "Scene/Scene.h"
#include "Utilities/MathUtils.h"
#include "Utilities/Utilities.h"

#include <sl.h>
#include <sl_core_api.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include "rhi_interop_dx12.h"
#include "rhi_interop_vulkan.h"

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

    UpscalingMode ResolveEffectiveUpscalingMode(UpscalingMode requestedMode, bool dlssSupported) {
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

    bool MakeStreamlineVulkanTextureResource(
        rhi::Device device,
        PixelBuffer* texture,
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

void UpscalingManager::SyncSettingsFromSettingsManager()
{
    m_upscalingMode = ReadUpscalingModeSetting(m_upscalingMode);
    m_upscaleQualityMode = ReadUpscalingQualityModeSetting(m_upscaleQualityMode);
}

bool CheckDLSSSupport(rhi::Device dev, rhi::Backend backend) {
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
    if (SL_FAILED(res, slIsFeatureSupported(sl::kFeatureDLSS, ai))) {
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
        if (m_upscalingMode == UpscalingMode::DLSS) {
            m_upscalingMode = UpscalingMode::None;
        }
        spdlog::info("UpscalingManager::InitializeAdapter skipped DLSS probing because enableStreamline=false");
        return;
    }

    auto dev = DeviceManager::GetInstance().GetDevice();
	m_dlssSupported = CheckDLSSSupport(dev, backend); // TODO: Query from RHI
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

bool UpscalingManager::InitFFX() {
    if (m_fsrIntialized) {
        Shutdown();
    }
	m_fsrUpscalingContext = nullptr;

    m_getRenderRes = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution");
    m_getOutputRes = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("outputResolution");
	const rhi::Backend backend = DeviceManager::GetInstance().GetBackend();
    auto outputRes = m_getOutputRes();

    ffx::CreateContextDescUpscale createUpscaling;
    createUpscaling.maxUpscaleSize = { outputRes.x, outputRes.y };
    createUpscaling.maxRenderSize = { outputRes.x, outputRes.y };
    createUpscaling.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED;

    if (!fidelityfx_backend::api::CreateUpscaleContext(m_fsrUpscalingContext, backend, DeviceManager::GetInstance().GetDevice(), createUpscaling)) {
        spdlog::error("UpscalingManager::InitFFX failed to create context for backend {}", static_cast<uint32_t>(backend));
		m_fsrUpscalingContext = nullptr;
        return false;
    }
	m_fsrIntialized = true;

	return true;
}

bool UpscalingManager::EnsureFSRContext() {
    if (m_fsrIntialized && m_fsrUpscalingContext != nullptr) {
        return true;
    }
    return InitFFX();
}

DirectX::XMFLOAT2 UpscalingManager::GetJitter(uint64_t frameNumber) {
    SyncSettingsFromSettingsManager();

    switch (m_upscalingMode)
    {
    case UpscalingMode::None: {
        // No upscaling, no jitter
        return { 0.0f, 0.0f };
		break;
    }
    case UpscalingMode::DLSS: {
        unsigned int sequenceLength = 16;
        unsigned int sequenceIndex = frameNumber % sequenceLength;
        DirectX::XMFLOAT2 sequenceOffset = {
            Halton(sequenceIndex + 1, 2) - 0.5f,
            Halton(sequenceIndex + 1, 3) - 0.5f };
        return sequenceOffset;
        break;
    }
    case UpscalingMode::FSR3: {
        if (!EnsureFSRContext()) {
            return { 0.0f, 0.0f };
        }

        auto displayWidth = m_getOutputRes().x;
        auto renderWidth = m_getRenderRes().x;
        float jitterX = 0.0f, jitterY = 0.0f;
        int32_t jitterPhaseCount = 1;
        ffx::QueryDescUpscaleGetJitterPhaseCount getJitterPhaseDesc{};
        getJitterPhaseDesc.displayWidth = displayWidth;
        getJitterPhaseDesc.renderWidth = renderWidth;
        getJitterPhaseDesc.pOutPhaseCount = &jitterPhaseCount;
        ffx::ReturnCode retCode = fidelityfx_backend::api::Query(m_fsrUpscalingContext, getJitterPhaseDesc);
        if (retCode != ffx::ReturnCode::Ok || jitterPhaseCount <= 0) {
            spdlog::warn("UpscalingManager::GetJitter failed to query FSR jitter phase count: {}", static_cast<uint32_t>(retCode));
            return { 0.0f, 0.0f };
        }

        ffx::QueryDescUpscaleGetJitterOffset getJitterOffsetDesc{};
        getJitterOffsetDesc.index = frameNumber % jitterPhaseCount;
        getJitterOffsetDesc.phaseCount = jitterPhaseCount;
        getJitterOffsetDesc.pOutX = &jitterX;
        getJitterOffsetDesc.pOutY = &jitterY;

        retCode = fidelityfx_backend::api::Query(m_fsrUpscalingContext, getJitterOffsetDesc);
        if (retCode != ffx::ReturnCode::Ok) {
            spdlog::warn("UpscalingManager::GetJitter failed to query FSR jitter offset: {}", static_cast<uint32_t>(retCode));
            return { 0.0f, 0.0f };
        }

        return { jitterX, jitterY };
    }
    default:
		return { 0.0f, 0.0f };
		break;
    }
}

bool UpscalingManager::InitSL() {
    SyncSettingsFromSettingsManager();
    if (!IsStreamlineEnabledSetting()) {
        m_dlssSupported = false;
        if (m_upscalingMode == UpscalingMode::DLSS) {
            m_upscalingMode = UpscalingMode::None;
        }
        spdlog::info("UpscalingManager::InitSL skipped because enableStreamline=false");
        return false;
    }
    return true;
}

void UpscalingManager::Setup() {
    SyncSettingsFromSettingsManager();
    auto outputRes = m_getOutputRes();
    const UpscalingMode effectiveMode = ResolveEffectiveUpscalingMode(m_upscalingMode, m_dlssSupported);
    switch (effectiveMode)
    {
    case UpscalingMode::None: {
        // No upscaling, just set the render resolution to the output resolution
        SettingsManager::GetInstance().getSettingSetter<DirectX::XMUINT2>("renderResolution")(outputRes);
        break;
    }
    case UpscalingMode::DLSS: {
        sl::DLSSOptimalSettings dlssSettings;
        sl::DLSSOptions dlssOptions = {};
        // These are populated based on user selection in the UI
        dlssOptions.mode = ToSLQualityMode(m_upscaleQualityMode);
        dlssOptions.outputWidth = outputRes.x;
        dlssOptions.outputHeight = outputRes.y;
        // Now let's check what should our rendering resolution be
        if (SL_FAILED(result, slDLSSGetOptimalSettings(dlssOptions, dlssSettings)))
        {
            spdlog::error("DLSSGetOptimalSettings failed!");
        }
        // Setup rendering based on the provided values in the sl::DLSSSettings structure

        SettingsManager::GetInstance().getSettingSetter<DirectX::XMUINT2>("renderResolution")({ dlssSettings.optimalRenderWidth, dlssSettings.optimalRenderHeight });

        auto viewport = sl::ViewportHandle(0); // 0 is the default viewport

        // Set preferred Render Presets per Perf Quality Mode. These are typically set one time
        // and established while evaluating DLSS SR Image Quality for your Application.
        // It will be set to DSSPreset::eDefault if unspecified.
        // Please Refer to section 3.12 of the DLSS Programming Guide for details.
        dlssOptions.dlaaPreset = sl::DLSSPreset::ePresetK;
        dlssOptions.qualityPreset = sl::DLSSPreset::ePresetK;
        dlssOptions.balancedPreset = sl::DLSSPreset::ePresetK;
        dlssOptions.performancePreset = sl::DLSSPreset::ePresetK;
        dlssOptions.ultraPerformancePreset = sl::DLSSPreset::ePresetF;
        // These are populated based on user selection in the UI
        dlssOptions.outputWidth = outputRes.x;
        dlssOptions.outputHeight = outputRes.y;
        dlssOptions.sharpness = 0;
        dlssOptions.colorBuffersHDR = sl::Boolean::eTrue; // assuming HDR pipeline
        dlssOptions.useAutoExposure = sl::Boolean::eTrue; // autoexposure is not to be used if a proper exposure texture is available
        dlssOptions.alphaUpscalingEnabled = sl::Boolean::eFalse; // experimental alpha upscaling, enable to upscale alpha channel of color texture
        if (SL_FAILED(result, slDLSSSetOptions(viewport, dlssOptions)))
        {
            // Handle error here, check the logs
        }

        break;
    }
    case UpscalingMode::FSR3: {
        if (!EnsureFSRContext()) {
            spdlog::error("UpscalingManager::Setup failed to initialize FSR context; using output resolution as render resolution");
            SettingsManager::GetInstance().getSettingSetter<DirectX::XMUINT2>("renderResolution")(outputRes);
            break;
        }

        DirectX::XMUINT2 optimalRenderRes = {};
        ffxQueryDescUpscaleGetRenderResolutionFromQualityMode queryDesc{};
        queryDesc.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
        queryDesc.qualityMode = ToFFXQualityMode(m_upscaleQualityMode);
		queryDesc.displayHeight = outputRes.y;
		queryDesc.displayWidth = outputRes.x;
        queryDesc.pOutRenderWidth = &optimalRenderRes.x;
        queryDesc.pOutRenderHeight = &optimalRenderRes.y;

        const ffx::ReturnCode queryResult = fidelityfx_backend::api::Query(m_fsrUpscalingContext, queryDesc);
        if (queryResult != ffx::ReturnCode::Ok || optimalRenderRes.x == 0 || optimalRenderRes.y == 0) {
            spdlog::error("UpscalingManager::Setup failed to query FSR render resolution: {}", static_cast<uint32_t>(queryResult));
            optimalRenderRes = outputRes;
        }

        SettingsManager::GetInstance().getSettingSetter<DirectX::XMUINT2>("renderResolution")(optimalRenderRes);
        
        break;
    }
    }
}

void UpscalingManager::EvaluateDLSS(rhi::CommandList& commandList, const Components::Camera* camera, uint64_t frameNumber, PixelBuffer* pHDRTarget, PixelBuffer* pUpscaledHDRTarget, PixelBuffer* pDepthTexture, PixelBuffer* pMotionVectors) {
    const rhi::Backend backend = DeviceManager::GetInstance().GetBackend();
    if (backend != rhi::Backend::D3D12 && backend != rhi::Backend::Vulkan) {
        spdlog::warn("UpscalingManager::EvaluateDLSS called on unsupported backend {}; skipping.", static_cast<uint32_t>(backend));
        return;
    }

    sl::FrameToken* frameToken = nullptr;
    const uint32_t streamlineFrameIndex = static_cast<uint32_t>(frameNumber);
    if (SL_FAILED(result, slGetNewFrameToken(frameToken, &streamlineFrameIndex)) || frameToken == nullptr)
    {
        spdlog::error("Failed to get Streamline frame token for frame {}", frameNumber);
        return;
    }

    auto myViewport = sl::ViewportHandle(0); // 0 is the default viewport
    auto renderRes = m_getRenderRes();
    auto outputRes = m_getOutputRes();

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
    consts.reset = sl::Boolean::eFalse;

    if (SL_FAILED(result, slSetConstants(consts, *frameToken, myViewport))) // constants are changing per frame so frame index is required
    {
        spdlog::error("Failed to set DLSS constants");
    }

    sl::Resource colorIn{};
    sl::Resource colorOut{};
    sl::Resource depth{};
    sl::Resource mvec{};
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
            spdlog::error("DLSS evaluation skipped because Vulkan Streamline resource metadata could not be resolved.");
            return;
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
        : static_cast<void*>(rhi::dx12::get_cmd_list(commandList));

    if (backend == rhi::Backend::Vulkan) {
        const sl::ResourceTag tags[] = {
            sl::ResourceTag{ &colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent },
            sl::ResourceTag{ &colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &upscaleExtent },
            sl::ResourceTag{ &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent },
            sl::ResourceTag{ &mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent },
        };
        if (SL_FAILED(result, slSetTagForFrame(*frameToken, myViewport, tags, _countof(tags), nativeCommandList))) {
            spdlog::error("Failed to tag Vulkan DLSS resources!");
            return;
        }

        const sl::BaseStructure* inputs[] = { &myViewport };
        if (SL_FAILED(result, slEvaluateFeature(sl::kFeatureDLSS, *frameToken, inputs, _countof(inputs), nativeCommandList)))
        {
            spdlog::error("DLSS evaluation failed!");
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
    }
    else
    {
        sl::ResourceTag colorInTag = sl::ResourceTag{ &colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent };
        sl::ResourceTag colorOutTag = sl::ResourceTag{ &colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &upscaleExtent };
        sl::ResourceTag depthTag = sl::ResourceTag{ &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent };
        sl::ResourceTag mvecTag = sl::ResourceTag{ &mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent };

        const sl::BaseStructure* inputs[] = { &myViewport, &depthTag, &mvecTag, &colorInTag, &colorOutTag };
        if (SL_FAILED(result, slEvaluateFeature(sl::kFeatureDLSS, *frameToken, inputs, _countof(inputs), nativeCommandList)))
        {
            spdlog::error("DLSS evaluation failed!");
        }
        else
        {
            // IMPORTANT: Host is responsible for restoring state on the command list used
            //restoreState(myCmdList); ??
        }
    }
}

void UpscalingManager::EvaluateFSR3(rhi::CommandList& commandList, const Components::Camera* camera, double elapsedSeconds, PixelBuffer* pHDRTarget, PixelBuffer* pUpscaledHDRTarget, PixelBuffer* pDepthTexture, PixelBuffer* pMotionVectors) {
    if (!EnsureFSRContext()) {
        spdlog::warn("UpscalingManager::EvaluateFSR3 skipped dispatch because the FSR context is not initialized");
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
    //dispatchUpscale.reactive;
    //dispatchUpscale.transparencyAndComposition;

    auto renderRes = m_getRenderRes();
    auto outputRes = m_getOutputRes();

    // Jitter is calculated earlier in the frame using a callback from the camera update
    dispatchUpscale.jitterOffset.x = -camera->jitterPixelSpace.x;
    dispatchUpscale.jitterOffset.y = -camera->jitterPixelSpace.y;
    // Motion vectors are curNDC - prevNDC in [-1,1]. FSR expects prevScreenPixel - curScreenPixel.
    // X negates direction. Y is positive because direction flip and NDC-Y-up to screen-Y-down cancel.
    // Divide by 2 because NDC spans 2 units for full screen width/height.
    dispatchUpscale.motionVectorScale.x = -static_cast<float>(renderRes.x) / 2.0f;
    dispatchUpscale.motionVectorScale.y = static_cast<float>(renderRes.y) / 2.0f;
    dispatchUpscale.reset = false;
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

    const ffx::ReturnCode dispatchResult = fidelityfx_backend::api::Dispatch(m_fsrUpscalingContext, dispatchUpscale);
    if (dispatchResult != ffx::ReturnCode::Ok) {
        spdlog::error("UpscalingManager::EvaluateFSR3 dispatch failed: {}", static_cast<uint32_t>(dispatchResult));
    }
}

void UpscalingManager::EvaluateNone(rhi::CommandList& commandList, const Components::Camera* camera, PixelBuffer* pHDRTarget, PixelBuffer* pUpscaledHDRTarget, PixelBuffer* pDepthTexture, PixelBuffer* pMotionVectors) {
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

    const bool graphOwnsCopyBarriers = DeviceManager::GetInstance().GetBackend() == rhi::Backend::Vulkan
        && m_upscalingMode == UpscalingMode::None;

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

void UpscalingManager::Evaluate(rhi::CommandList& commandList, const Components::Camera* camera, uint64_t frameNumber, double elapsedSeconds, PixelBuffer* pHDRTarget, PixelBuffer* pUpscaledHDRTarget, PixelBuffer* pDepthTexture, PixelBuffer* pMotionVectors) {
    SyncSettingsFromSettingsManager();
    const UpscalingMode effectiveMode = ResolveEffectiveUpscalingMode(m_upscalingMode, m_dlssSupported);
    switch (effectiveMode)
    {
	    case UpscalingMode::None:
            EvaluateNone(commandList, camera, pHDRTarget, pUpscaledHDRTarget, pDepthTexture, pMotionVectors);
			break;
        case UpscalingMode::DLSS:
			EvaluateDLSS(commandList, camera, frameNumber, pHDRTarget, pUpscaledHDRTarget, pDepthTexture, pMotionVectors);
            break;
        case UpscalingMode::FSR3:
			EvaluateFSR3(commandList, camera, elapsedSeconds, pHDRTarget, pUpscaledHDRTarget, pDepthTexture, pMotionVectors);
            break;
	}
}

void UpscalingManager::Shutdown() {
    if (m_fsrIntialized) {
        fidelityfx_backend::api::DestroyContext(m_fsrUpscalingContext);
        m_fsrIntialized = false;
    }
	m_fsrUpscalingContext = nullptr;
	fidelityfx_backend::api::UnloadModule();
	// RHI now handles streamline internally
}
