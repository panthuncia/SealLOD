#include <BasicRenderer/Renderer.h>
#include <spdlog/spdlog.h>
#include <rhi_debug.h>
#include "Utilities/Utilities.h"
#include "Runtime/Settings/SettingsManager.h"
#include "Runtime/Device/DeviceManager.h"
#include "Runtime/Resources/ResourceManager.h"
#include "Runtime/Settings/RendererSettingsHelpers.h"
#include "Assets/Textures/TextureFactory.h"
#include "Materials/MaterialManager.h"
#include "Lighting/Environment/EnvironmentManager.h"
#include "PostProcessing/Upscaling/UpscalingManager.h"
#include "PostProcessing/FidelityFX/FFXManager.h"
#include "Resources/TextureDescription.h"
#include "Resources/DynamicResource.h"
#include "OpenRenderGraph/OpenRenderGraph.h"
#include "Resources/ExternalTextureResource.h"
#include "../../../generated/BuiltinResources.h"

void Renderer::CreateGlobalResources() {
    auto blueNoiseAsset = LoadTextureFromFile(L"BuiltinTextures/BlueNoise470.png", nullptr, false);
    if (blueNoiseAsset) {
        blueNoiseAsset->EnsureUploaded(*m_pTextureFactory);
        m_blueNoiseTexture = blueNoiseAsset->ImagePtr();
        if (m_blueNoiseTexture) {
            m_blueNoiseTexture->SetName("Blue Noise 2D");
            org::memory::SetResourceUsageHint(*m_blueNoiseTexture, "Noise lookup resources");
        }
    }

    m_openPBRLookupResources = CreateOpenPBRLookupResources(*m_pTextureFactory);
}

void Renderer::CreateDefaultEnvironmentResources() {
    auto makeFallbackCubemap = [](uint32_t resolution, bool generateMipMaps, const char* name) {
        org::TextureDescription desc;
        desc.channels = 4;
        desc.isCubemap = true;
        desc.format = rhi::Format::R8G8B8A8_UNorm;
        desc.hasSRV = true;
        desc.generateMipMaps = generateMipMaps;

        org::ImageDimensions dims;
        dims.width = resolution;
        dims.height = resolution;
        dims.rowPitch = resolution * 4;
        dims.slicePitch = resolution * resolution * 4;
        for (int i = 0; i < 6; ++i) {
            desc.imageDimensions.push_back(dims);
        }

        auto cubemap = org::PixelBuffer::CreateShared(desc);
        cubemap->SetName(name);
        return cubemap;
    };

    auto reflectionResolution = SettingsManager::GetInstance().getSettingGetter<uint16_t>("reflectionCubemapResolution")();
    auto skyboxResolution = SettingsManager::GetInstance().getSettingGetter<uint16_t>("skyboxResolution")();

    if (!m_pipelineRecipe.Bindings().Contains(Builtin::Environment::CurrentCubemap) && !m_defaultEnvironmentCubemap) {
        m_defaultEnvironmentCubemap = makeFallbackCubemap(skyboxResolution, false, "Fallback Environment Cubemap");
        org::memory::SetResourceUsageHint(*m_defaultEnvironmentCubemap, "Fallback environment resources");
    }
    if (!m_pipelineRecipe.Bindings().Contains(Builtin::Environment::CurrentPrefilteredCubemap) && !m_defaultEnvironmentPrefilteredCubemap) {
        m_defaultEnvironmentPrefilteredCubemap = makeFallbackCubemap(
            reflectionResolution,
            true,
            "Fallback Prefiltered Environment Cubemap");
        org::memory::SetResourceUsageHint(*m_defaultEnvironmentPrefilteredCubemap, "Fallback environment resources");
    }
}

void Renderer::CreateTextures() {
    auto resolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
    // Create HDR color target
    org::TextureDescription hdrDesc;
    hdrDesc.arraySize = 1;
    hdrDesc.channels = 4; // RGBA
    hdrDesc.isCubemap = false;
    hdrDesc.hasRTV = true;
    hdrDesc.hasUAV = true;
    hdrDesc.hasNonShaderVisibleUAV = true;
    hdrDesc.format = rhi::Format::R16G16B16A16_Float; // HDR format
    hdrDesc.generateMipMaps = false; // For bloom downsampling
    org::ImageDimensions dims;
    dims.height = resolution.y;
    dims.width = resolution.x;
    hdrDesc.imageDimensions.push_back(dims);
    hdrDesc.allowAlias = true;
    auto hdrColorTarget = org::PixelBuffer::CreateSharedUnmaterialized(hdrDesc);
    hdrColorTarget->SetName("Primary Camera HDR Color Target");
    org::memory::SetResourceUsageHint(*hdrColorTarget, "Primary color buffers");
	m_coreResourceProvider.m_HDRColorTarget = hdrColorTarget;

    auto outputResolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("outputResolution")();
    hdrDesc.imageDimensions[0].width = outputResolution.x;
    hdrDesc.imageDimensions[0].height = outputResolution.y;
    // Streamline's D3D12 interop operates on the native resource as a whole.
    // Keep its output single-mip; the optional bloom technique owns a separate
    // mip chain so removing bloom also removes that allocation.
    hdrDesc.generateMipMaps = false;
    hdrDesc.allowAlias = true;
	auto upscaledHDRColorTarget = org::PixelBuffer::CreateSharedUnmaterialized(hdrDesc);
	upscaledHDRColorTarget->SetName("Upscaled HDR Color Target");
    org::memory::SetResourceUsageHint(*upscaledHDRColorTarget, "Upscaled color buffers");
	m_coreResourceProvider.m_upscaledHDRColorTarget = upscaledHDRColorTarget;

    // Scene recording targets a logical-frame-owned LDR surface.  Only the
    // presentation copy depends on the acquired swapchain image, so compiled
    // scene work can eventually be planned and recorded before acquisition.
    org::TextureDescription presentationDesc{};
    presentationDesc.arraySize = 1;
    presentationDesc.channels = 4;
    presentationDesc.hasRTV = true;
    presentationDesc.format = rhi::Format::R8G8B8A8_UNorm;
    presentationDesc.rtvFormat = rhi::Format::R8G8B8A8_UNorm;
    presentationDesc.imageDimensions.push_back({outputResolution.x, outputResolution.y, 0, 0});
    presentationDesc.allowAlias = false;
    m_presentationColorResources.clear();
    m_presentationColorResources.reserve(m_numFramesInFlight);
    for (uint32_t slot = 0; slot < m_numFramesInFlight; ++slot) {
        auto target = org::PixelBuffer::CreateSharedUnmaterialized(presentationDesc);
        target->SetName("Frame Presentation Color " + std::to_string(slot));
        org::memory::SetResourceUsageHint(*target, "Frame presentation surfaces");
        m_presentationColorResources.push_back(std::move(target));
    }
    if (!m_presentationColorResources.empty()) {
        m_dynamicPresentationColor = std::make_shared<org::DynamicResource>(m_presentationColorResources.front());
        m_dynamicPresentationColor->SetName("Frame Presentation Color");
    }

    org::TextureDescription motionVectors;
    motionVectors.arraySize = 1;
    motionVectors.channels = 2;
    motionVectors.isCubemap = false;
    motionVectors.hasRTV = true;
    motionVectors.format = rhi::Format::R16G16_Float;
    motionVectors.generateMipMaps = false;
    motionVectors.hasSRV = true;
    motionVectors.hasUAV = true;
    motionVectors.hasNonShaderVisibleUAV = true;
    motionVectors.srvFormat = rhi::Format::R16G16_Float;
    org::ImageDimensions motionVectorsDims = { resolution.x, resolution.y, 0, 0 };
    motionVectors.imageDimensions.push_back(motionVectorsDims);
	motionVectors.allowAlias = true;
    auto dilatedMotionVectorsBuffer = org::PixelBuffer::CreateSharedUnmaterialized(motionVectors);
    dilatedMotionVectorsBuffer->SetName("Dilated Motion Vectors");
    org::memory::SetResourceUsageHint(*dilatedMotionVectorsBuffer, "Upscaling resources");
	m_coreResourceProvider.m_gbufferDilatedMotionVectors = dilatedMotionVectorsBuffer;
}

void Renderer::CreateRTVs() {
    auto device = DeviceManager::GetInstance().GetDevice();
    const bool renderGraphBatchTraceEnabled = SettingsManager::GetInstance().getSettingGetter<bool>("renderGraphBatchTraceEnabled")();
    const auto outputResolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("outputResolution")();
    // Recreate the render target views
    for (UINT n = 0; n < m_numFramesInFlight; n++) {
        renderTargets[n] = m_swapChain->Image(n);
        rhi::RtvDesc rtvDesc = {};
        rtvDesc.dimension = rhi::RtvDim::Texture2D;
        rtvDesc.formatOverride = rhi::Format::R8G8B8A8_UNorm;
        rtvDesc.range = { 0, 1, 0, 1 };
        device.CreateRenderTargetView({ rtvHeap->GetHandle(), n }, renderTargets[n], rtvDesc);

        // Keep external texture wrappers in sync after resize
        if (n < m_backbufferResources.size() && m_backbufferResources[n]) {
            m_backbufferResources[n]->SetHandle(renderTargets[n]);
            m_backbufferResources[n]->SetDimensions(outputResolution.x, outputResolution.y);
            m_backbufferResources[n]->SetRTVSlot({ rtvHeap->GetHandle(), n });
            m_backbufferResources[n]->ResetToUndefined();
            if (renderGraphBatchTraceEnabled) {
                spdlog::info(
                    "Renderer: CreateRTVs slot={} resourceID={} handle=({}, {}) rtv=({}, {})",
                    n,
                    m_backbufferResources[n]->GetGlobalResourceID(),
                    renderTargets[n].index,
                    renderTargets[n].generation,
                    rtvHeap->GetHandle().index,
                    n);
            }
        }
    }
}

void Renderer::OnResize(UINT newWidth, UINT newHeight) {
    spdlog::info(
        "Renderer: OnResize {}x{} frameIndex={} totalFramesRendered={}",
        newWidth,
        newHeight,
        static_cast<unsigned>(m_frameIndex),
        m_totalFramesRendered);
    // Wait for all in-flight GPU work before destroying resources
	StallPipeline();

    // Release the resources tied to the swap chain
    auto numFramesInFlight = getNumFramesInFlight();

    // Resize the swap chain
    m_swapChainReady = false;
    m_loggedSwapChainNotReady = false;
    auto resizeResult = m_swapChain->ResizeBuffers(m_numFramesInFlight, newWidth, newHeight, rhi::Format::R8G8B8A8_UNorm, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH); // TODO: Port flags to RHI
    if (resizeResult != rhi::Result::Ok) {
        spdlog::critical(
            "Renderer: ResizeBuffers failed during OnResize {}x{} result={} frameIndex={} totalFramesRendered={}",
            newWidth,
            newHeight,
            static_cast<unsigned>(resizeResult),
            static_cast<unsigned>(m_frameIndex),
            m_totalFramesRendered);
        return;
    }

	SettingsManager::GetInstance().getSettingSetter<DirectX::XMUINT2>("outputResolution")({ newWidth, newHeight });

    m_frameIndex = static_cast<uint8_t>(m_swapChain->CurrentImageIndex());

    CreateRTVs();
    m_swapChainReady = true;
    m_loggedSwapChainNotReady = false;

    UpscalingManager::GetInstance().Shutdown();
    UpscalingManager::GetInstance().Setup();
    FFXManager::GetInstance().Shutdown();
    FFXManager::GetInstance().InitFFX();

    CreateTextures();

    const auto ingestionConfiguration = CaptureSceneIngestionConfiguration();
    m_sceneRenderBridge.ResyncPrimaryCameraDepth(m_sceneIngestionServices,
        ingestionConfiguration.renderResolution.x, ingestionConfiguration.renderResolution.y,
        PrimaryCameraLodHeight(ingestionConfiguration));

	//Rebuild the render graph
	rebuildRenderGraph = true;
}

void Renderer::ApplyWindowResolutionPreset(WindowResolutionPreset preset)
{
    const auto resolution = ResolveWindowResolutionPreset(preset);
    const auto currentResolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("outputResolution")();
    if (currentResolution.x == resolution.x && currentResolution.y == resolution.y) {
        return;
    }

    if (!m_hwnd) {
        OnResize(resolution.x, resolution.y);
        return;
    }

    RECT windowRect{
        0,
        0,
        static_cast<LONG>(resolution.x),
        static_cast<LONG>(resolution.y),
    };
    const DWORD style = static_cast<DWORD>(GetWindowLongPtr(m_hwnd, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtr(m_hwnd, GWL_EXSTYLE));
    if (!AdjustWindowRectEx(&windowRect, style, FALSE, exStyle)) {
        spdlog::warn("Renderer: AdjustWindowRectEx failed while applying {}x{} window preset", resolution.x, resolution.y);
        return;
    }

    const int windowWidth = static_cast<int>(windowRect.right - windowRect.left);
    const int windowHeight = static_cast<int>(windowRect.bottom - windowRect.top);
    if (!SetWindowPos(
            m_hwnd,
            nullptr,
            0,
            0,
            windowWidth,
            windowHeight,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        spdlog::warn("Renderer: SetWindowPos failed while applying {}x{} window preset", resolution.x, resolution.y);
    }
}



void Renderer::SetEnvironmentInternal(std::wstring name) {

    std::filesystem::path envpath = std::filesystem::path(GetExePath()) / L"textures" / L"environment" / (name+L".hdr");

    if (std::filesystem::exists(envpath)) {
		m_warnedUsingFallbackEnvironment = false;
		m_preFrameDeferredFunctions.defer([envpath, name, this]() { // Don't change this during rendering
            m_currentEnvironment = m_pEnvironmentManager->CreateEnvironment(name);
            m_pEnvironmentManager->SetFromHDRI(m_currentEnvironment.get(), envpath.string());
			::ResourceManager::GetInstance().SetActiveEnvironmentIndex(m_currentEnvironment->GetEnvironmentIndex());
			});
    }
    else {
        m_currentEnvironment.reset();
        ::ResourceManager::GetInstance().SetActiveEnvironmentIndex(0);
        rebuildRenderGraph = true;
        if (!m_warnedUsingFallbackEnvironment) {
            spdlog::warn("Environment file not found: {}. Falling back to blank environment resources.", envpath.string());
            m_warnedUsingFallbackEnvironment = true;
        }
    }
}
