#include <BasicRenderer/Renderer.h>
#include "OpenRenderGraph/GraphHost.h"
#include <spdlog/spdlog.h>
#include <rhi_debug.h>
#include "Utilities/Utilities.h"
#include <BasicRenderer/Assets/TexturePipeline.h>
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
#include "Runtime/GraphIntegration/FrameBuild.h"
#include "OpenRenderGraph/OpenRenderGraph.h"
#include "Resources/ExternalTextureResource.h"
#include "../../../generated/BuiltinResources.h"
#include "OpenRenderGraph/GraphDesc.h"
#include "Lighting/Environment/RenderPasses/brdfIntegrationPass.h"

void Renderer::CreateGlobalResources() {
    // Needed by the first frame's passes: the one texture load renderer initialization waits on.
    br::textures::TextureRequest blueNoiseRequest;
    blueNoiseRequest.source = br::textures::MakeFileTextureSource("BuiltinTextures/BlueNoise470.png");
    blueNoiseRequest.preparation = br::textures::TexturePreparation::LoadOnly;
    blueNoiseRequest.debugName = "Blue Noise 2D";
    auto blueNoiseAsset = br::textures::RequestTexture(std::move(blueNoiseRequest));
    if (br::textures::WaitForTexture(blueNoiseAsset, std::chrono::seconds(10))) {
        m_blueNoiseTexture = blueNoiseAsset->ImagePtr();
        if (m_blueNoiseTexture) {
            m_blueNoiseTexture->SetName("Blue Noise 2D");
            org::memory::SetResourceUsageHint(*m_blueNoiseTexture, "Noise lookup resources");
        }
    }

    m_openPBRLookupResources = CreateOpenPBRLookupResources(*m_pTextureFactory);
}

std::shared_ptr<org::PixelBuffer> Renderer::EnsureBrdfLut() {
    if (m_brdfLut) return m_brdfLut;
    org::TextureDescription desc;
    desc.arraySize = 1;
    desc.channels = 2;
    desc.isCubemap = false;
    desc.hasRTV = true;
    desc.format = rhi::Format::R16G16_Float;
    desc.generateMipMaps = false;
    desc.hasSRV = true;
    desc.srvFormat = rhi::Format::R16G16_Float;
    desc.imageDimensions.push_back({512, 512, 0, 0});
    // Materialized and kept: it is written once, so it must never be dematerialized.
    auto lut = org::PixelBuffer::CreateShared(desc);
    lut->SetName("BRDF Integration Texture");
    org::memory::SetResourceUsageHint(*lut, "Environment lighting");
    {
        // One execution. The frame graph's reads of the LUT are ordered after it by the runtime; the graph waits for the
        // GPU when it goes, which happens once, while the renderer builds its first graph.
        org::GraphDesc job("BRDFIntegration");
        job.Add(org::ExternalPassDesc::Render("BRDF Integration Pass", std::make_shared<BRDFIntegrationPass>(lut)));
        org::JobGraph::Create(m_orgRuntime, std::move(job))->Execute();
    }
    m_brdfLut = std::move(lut);
    return m_brdfLut;
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

    // Blank stand-ins until an environment exists: their contents are uniform, so their size buys nothing. At the skybox and
    // reflection resolutions they held 104 MiB for the whole session.
    constexpr uint32_t reflectionResolution = 16u;
    constexpr uint32_t skyboxResolution = 16u;

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

std::shared_ptr<br::render::ResolutionObjects> Renderer::CreateTextures(DirectX::XMUINT2 resolution, DirectX::XMUINT2 outputResolution) {
    auto objects = std::make_shared<br::render::ResolutionObjects>();
    objects->render = resolution;
    objects->output = outputResolution;
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
	objects->hdrColor = hdrColorTarget;

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
	objects->upscaledHdrColor = upscaledHDRColorTarget;

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
    objects->presentationColor.reserve(m_numFramesInFlight);
    for (uint32_t slot = 0; slot < m_numFramesInFlight; ++slot) {
        auto target = org::PixelBuffer::CreateSharedUnmaterialized(presentationDesc);
        target->SetName("Frame Presentation Color " + std::to_string(slot));
        org::memory::SetResourceUsageHint(*target, "Frame presentation surfaces");
        objects->presentationColor.push_back(std::move(target));
    }
    if (!objects->presentationColor.empty()) {
        objects->dynamicPresentationColor = std::make_shared<org::DynamicResource>(objects->presentationColor.front());
        objects->dynamicPresentationColor->SetName("Frame Presentation Color");
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
	objects->dilatedMotionVectors = dilatedMotionVectorsBuffer;

    // What builds snapshot from now on (MakeFrameBuild). The attached graphs registered their own snapshot of the old ones.
    m_coreResourceProvider.m_HDRColorTarget = objects->hdrColor;
    m_coreResourceProvider.m_upscaledHDRColorTarget = objects->upscaledHdrColor;
    m_coreResourceProvider.m_gbufferDilatedMotionVectors = objects->dilatedMotionVectors;
    m_buildResolutionObjects = objects;
    return objects;
}

void Renderer::CreateRTVs() {
    const bool renderGraphBatchTraceEnabled = SettingsManager::GetInstance().getSettingGetter<bool>("renderGraphBatchTraceEnabled")();
    const auto outputResolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("outputResolution")();
    UpdateBackbufferViews(outputResolution.x, outputResolution.y, renderGraphBatchTraceEnabled);
}

void Renderer::UpdateBackbufferViews(UINT width, UINT height, bool renderGraphBatchTraceEnabled) {
    auto device = DeviceManager::GetInstance().GetDevice();
    const DirectX::XMUINT2 outputResolution{ width, height };
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

void Renderer::RequestOutputSize(UINT newWidth, UINT newHeight) {
    // A minimized window reports 0x0: nothing to render to, the swapchain stays as it is.
    if (newWidth == 0 || newHeight == 0) return;
    m_frameRevisions.RequestOutputSize(newWidth, newHeight, "window size");
}

void Renderer::ResizeSwapChainAtPresent(uint64_t fenceValue, UINT width, UINT height, bool trace) {
    BT_ZONE_SCOPE("Renderer::ResizeSwapChainAtPresent");
    auto& shared = *m_presentShared;
    // The earlier frames presented from the backbuffers. Their closures ran before this one, so their fence values are on the
    // queue: waiting for the last of them is waiting for all. The attach frame's own work is on the queues already.
    const auto waitBegin = std::chrono::steady_clock::now();
    const auto sincePresent = std::chrono::duration<double, std::milli>(waitBegin - m_lastPresentReturned).count();
    if (fenceValue > 1) {
        uint32_t timeouts = 0;
        while (m_frameFence->GetCompletedValue() < fenceValue - 1) {
            if (m_frameFence->HostWait(fenceValue - 1, 1000) != rhi::Result::WaitTimeout) break;
            if (++timeouts % 5 == 0)
                spdlog::warn("Renderer: the swapchain resize at present waits for frame fence {} (completed {})", fenceValue - 1,
                    m_frameFence->GetCompletedValue());
        }
    }
    const auto resizeBegin = std::chrono::steady_clock::now();
    const auto result = m_swapChain->ResizeBuffersAfterFrames(m_numFramesInFlight, width, height, rhi::Format::R8G8B8A8_UNorm,
        DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
    const auto resizeEnd = std::chrono::steady_clock::now();
    shared.resizeWaitMs = std::chrono::duration<double, std::milli>(resizeBegin - waitBegin).count();
    shared.resizeBuffersMs = std::chrono::duration<double, std::milli>(resizeEnd - resizeBegin).count();
    shared.resizeFailed = result != rhi::Result::Ok;
    if (!shared.resizeFailed) {
        // A Vulkan surface's extent is the window's, which a drag may have changed since the size was asked for.
        if (uint32_t w = 0, h = 0; m_swapChain->Extent(w, h) && w && h) {
            width = w;
            height = h;
        }
        UpdateBackbufferViews(width, height, trace);
    }
    shared.resizedWidth = width;
    shared.resizedHeight = height;
    shared.resized = true;
    spdlog::info("Renderer: swapchain resize {}x{} in the attach frame's present on the submission thread: {:.2f} ms waiting for the "
        "frames that used the backbuffers, {:.2f} ms in ResizeBuffers{}; {:.1f} ms after the last Present returned (it took {:.1f} ms)",
        width, height, shared.resizeWaitMs, shared.resizeBuffersMs, shared.resizeFailed ? " (failed)" : "", sincePresent, m_lastPresentMs);
}

bool Renderer::ResizeSwapChain(UINT newWidth, UINT newHeight) {
    BT_ZONE_SCOPE("Renderer::ResizeSwapChain");
    // With the submission thread the presents run there: everything queued goes first, so no present is pending at the resize and
    // the frame fences below are on the queue. A resize at present that was asked for is answered by this one.
    if (m_frameHost) m_frameHost->DrainSubmissions("swapchain resize");
    m_pendingSwapchainResize.reset();
    // At the attach of the graphs built for the new size (M5 step 5). The backbuffers are used only by the presentation copies and
    // the presents, both on the graphics queue before each frame's fence: waiting for every slot's fence is waiting for the last of
    // them. Work on the other queues (compute, copies, uploads, a build's upscaler allocation) goes on.
    const auto waitBegin = std::chrono::steady_clock::now();
    // Each slot's frame, oldest first, and when its fence was reached (ms after the wait began): how far ahead of the GPU and the
    // display the frames were.
    std::string slots;
    {
        std::vector<uint8_t> order(m_numFramesInFlight);
        for (uint8_t slot = 0; slot < m_numFramesInFlight; ++slot) order[slot] = slot;
        std::sort(order.begin(), order.end(), [&](uint8_t a, uint8_t b) { return m_frameFenceValues[a] < m_frameFenceValues[b]; });
        for (const auto slot : order) {
            const bool pending = m_frameFence->GetCompletedValue() < m_frameFenceValues[slot];
            WaitForFrame(slot);
            slots += fmt::format("{}{}{:.1f}", slots.empty() ? "" : "/", pending ? "" : "~",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitBegin).count());
        }
    }
    const auto resizeBegin = std::chrono::steady_clock::now();
    auto& stats = m_resolutionStats;
    stats.lastSwapchainWaitMs = std::chrono::duration<double, std::milli>(resizeBegin - waitBegin).count();
    stats.maxSwapchainWaitMs = (std::max)(stats.maxSwapchainWaitMs, stats.lastSwapchainWaitMs);

    // Resize the swap chain
    m_swapChainReady = false;
    m_loggedSwapChainNotReady = false;
    auto resizeResult = m_swapChain->ResizeBuffersAfterFrames(m_numFramesInFlight, newWidth, newHeight, rhi::Format::R8G8B8A8_UNorm, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH); // TODO: Port flags to RHI
    stats.lastResizeBuffersMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - resizeBegin).count();
    stats.maxResizeBuffersMs = (std::max)(stats.maxResizeBuffersMs, stats.lastResizeBuffersMs);
    spdlog::info(
        "Renderer: swapchain resize {}x{} frameIndex={} totalFramesRendered={}: {:.2f} ms waiting for the frames that used the backbuffers, "
        "{:.2f} ms in ResizeBuffers (no device wait); slots reached at {} ms (oldest first, ~ already), {:.1f} ms after the last Present "
        "returned (it took {:.1f} ms)",
        newWidth,
        newHeight,
        static_cast<unsigned>(m_frameIndex),
        m_totalFramesRendered,
        stats.lastSwapchainWaitMs,
        stats.lastResizeBuffersMs,
        slots,
        std::chrono::duration<double, std::milli>(waitBegin - m_lastPresentReturned).count(),
        m_lastPresentMs);
    if (resizeResult != rhi::Result::Ok) {
        spdlog::critical(
            "Renderer: ResizeBuffers failed for {}x{} result={} frameIndex={} totalFramesRendered={}",
            newWidth,
            newHeight,
            static_cast<unsigned>(resizeResult),
            static_cast<unsigned>(m_frameIndex),
            m_totalFramesRendered);
        return false;
    }

    // The size the images have: a Vulkan surface's extent is the window's, which a drag may have changed since the size was asked
    // for. The backbuffers and outputResolution take it; the presentation copy clips to the smaller size (M5 step 8).
    if (uint32_t width = 0, height = 0; m_swapChain->Extent(width, height) && width && height && (width != newWidth || height != newHeight)) {
        spdlog::info("Renderer: the swapchain is {}x{}, not the {}x{} asked for", width, height, newWidth, newHeight);
        newWidth = width;
        newHeight = height;
    }
    SettingsManager::GetInstance().getSettingSetter<DirectX::XMUINT2>("outputResolution")({ newWidth, newHeight });

    // Inside Update (the consolidation point), after the frame took its slot: the frame continues on the image the resized
    // swapchain gives, as a frame after a resize between frames did. Every slot's frame is complete (the waits above). With the
    // submission thread the frame slot does not follow the image: the present reads the image itself.
    if (!m_submissionThread) {
        m_frameIndex = static_cast<uint8_t>(m_swapChain->CurrentImageIndex());
        m_preparationFrameIndex = m_frameIndex;
    }

    CreateRTVs();
    m_swapChainReady = true;
    m_loggedSwapChainNotReady = false;
    ++m_resolutionStats.swapchainResizes;
    ++m_swapchainGeneration;
    return true;
}

Renderer::FrameResolution Renderer::BuildResolution() const {
    if (const auto latest = UpscalingManager::GetInstance().Latest()) return { latest->RenderResolution(), latest->OutputResolution() };
    auto& settings = SettingsManager::GetInstance();
    return { settings.getSettingGetter<DirectX::XMUINT2>("renderResolution")(), settings.getSettingGetter<DirectX::XMUINT2>("outputResolution")() };
}

Renderer::FrameResolution Renderer::AttachedResolution() const {
    return m_attachedResolution ? *m_attachedResolution : BuildResolution();
}

void Renderer::SwitchResolutionObjects(const std::shared_ptr<br::render::ResolutionObjects>& objects) {
    if (!objects || objects == m_attachedResolutionObjects) return;
    BT_ZONE_SCOPE("Renderer::SwitchResolutionObjects");
    const auto begin = std::chrono::steady_clock::now();
    auto& settings = SettingsManager::GetInstance();
    // The swapchain's size (outputResolution is written only where the swapchain is made or resized).
    const auto swapchainSize = settings.getSettingGetter<DirectX::XMUINT2>("outputResolution")();
    const bool resizeSwapchain = m_swapChain && (objects->recreateSwapchain || swapchainSize.x != objects->output.x || swapchainSize.y != objects->output.y);
    if (resizeSwapchain && m_submissionThread && m_resizeAtPresent) {
        // In this frame's present closure, before its presentation tail (ResizeSwapChainAtPresent): the wait for the old frames'
        // presents then overlaps this frame's work on the render thread instead of stalling it here. Readers of the size see the
        // new one from now on; the closure corrects it if the swapchain was made at another (EnqueuePresent).
        m_pendingSwapchainResize = PendingSwapchainResize{ objects->output.x, objects->output.y };
        settings.getSettingSetter<DirectX::XMUINT2>("outputResolution")(objects->output);
    } else if (resizeSwapchain && !ResizeSwapChain(objects->output.x, objects->output.y)) {
        spdlog::critical("Renderer: the swapchain could not be resized to {}x{} at the attach of the graphs built for it", objects->output.x, objects->output.y);
    }
    objects->recreateSwapchain = false;
    // The presentation copy reads the slot surfaces of the graphs that ran the frame: from this frame on, the new ones.
    m_presentationColorResources = objects->presentationColor;
    m_dynamicPresentationColor = objects->dynamicPresentationColor;
    // The primary view takes the new depth maps and sizes. It stays the same view (its id, its camera slot): only its depth and its
    // camera's sizes change. The old maps live on in the retiring graphs' builds.
    if (objects->primaryDepth) {
        m_sceneRenderBridge.ReplacePrimaryCameraDepth(m_sceneIngestionServices, *objects->primaryDepth, objects->render.x, objects->render.y,
            objects->primaryLodHeight);
        objects->primaryDepth.reset();
    }
    // Readers of the settings outside a build (scene ingestion, passes declared again at preparation, the upscaler status) see the
    // attached graphs' sizes, never a newer build's.
    settings.getSettingSetter<DirectX::XMUINT2>("renderResolution")(objects->render);
    const bool first = !m_attachedResolutionObjects;
    m_attachedResolutionObjects = objects;
    if (first) return;
    auto& stats = m_resolutionStats;
    ++stats.switches;
    stats.lastSwitchMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    stats.maxSwitchMs = (std::max)(stats.maxSwitchMs, stats.lastSwitchMs);
    spdlog::info("Renderer: render {}x{}, output {}x{} are current from this attach{}; {:.2f} ms on the render thread", objects->render.x,
        objects->render.y, objects->output.x, objects->output.y,
        resizeSwapchain ? (m_pendingSwapchainResize ? " (swapchain resized in this frame's present)" : " (swapchain resized)") : "", stats.lastSwitchMs);
}

void Renderer::ApplyResolutionRequest() {
    if (!m_frameRevisions.ResolutionPending()) return;
    // One resolution build at a time, and none beside another build: a request waits until the build in flight attaches, and
    // whatever arrives meanwhile is answered by one consumption (the latest size wins). The attached graphs keep rendering at
    // their size meanwhile; the window stretches the old backbuffers (M5 design, D2).
    if (FrameBuildInFlight()) {
        ++m_resolutionStats.deferredFrames;
        return;
    }
    // A window size is taken once no newer one came for a while (BASICRENDERER_RESIZE_SETTLE_MS, 50 by default; 0: at once). A drag
    // then makes one set, at its end, instead of one per build: each set at a new output size allocates a DLSS feature (35-67 ms),
    // which makes one frame's evaluation wait (M5 step 6). The attached graphs keep presenting at their size meanwhile (D2). A
    // present's mode change waits too while the size changes (a Vulkan drag reports one per frame); one at the size the swapchain
    // and the graphs already have only makes the swapchain again, and does not wait (M5 step 9).
    {
        static const int settleMs = [] {
            char* value = nullptr;
            size_t length = 0;
            int parsed = 50;
            if (_dupenv_s(&value, &length, "BASICRENDERER_RESIZE_SETTLE_MS") == 0 && value) {
                parsed = std::atoi(value);
                free(value);
            }
            return (std::max)(0, parsed);
        }();
        if (settleMs > 0) {
            const auto pending = m_frameRevisions.PeekResolution();
            const auto swapchainOnly = [&] {
                if (!pending.recreateSwapchain || pending.upscaler) return false;
                const auto swapchainSize = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("outputResolution")();
                const auto newest = BuildResolution().output;
                const auto attached = AttachedResolution().output;
                return swapchainSize.x == pending.width && swapchainSize.y == pending.height && newest.x == pending.width
                    && newest.y == pending.height && attached.x == pending.width && attached.y == pending.height;
            };
            if (pending.outputSize && !swapchainOnly()
                && std::chrono::steady_clock::now() - m_frameRevisions.LastOutputSizeRequest() < std::chrono::milliseconds(settleMs)) {
                ++m_resolutionStats.settleFrames;
                return;
            }
        }
    }
    BT_ZONE_SCOPE("Renderer::ApplyResolutionRequest");
    const auto begin = std::chrono::steady_clock::now();
    auto request = m_frameRevisions.ConsumeResolution();
    auto& stats = m_resolutionStats;
    ++stats.consumed;
    stats.lastCoalesced = request.requests;
    stats.maxCoalesced = (std::max)(stats.maxCoalesced, request.requests);
    stats.lastFrame = m_totalFramesRendered;
    stats.lastReasons = request.Describe();

    // Against the newest sizes (the latest configuration's): the attached graphs' unless a build for others failed. Nothing the
    // attached graphs use changes here: the swapchain, the presentation surfaces and the primary view's depth switch at the attach
    // of the build started below (SwitchResolutionObjects), and the old graphs render at their size until then (M5 step 5).
    const auto newest = BuildResolution();
    // A present's mode change (Vulkan's SUBOPTIMAL or OUT_OF_DATE) reported by a swapchain that has been made again since is
    // answered: while a resize set was built, the old swapchain presented into the resized window (D2) and reported it, and the
    // set's attach then made the swapchain at that size (M5 step 8). Without this, each resize built every epoch twice.
    if (request.recreateSwapchain && m_modeChangeSwapchainGeneration != m_swapchainGeneration) {
        request.recreateSwapchain = false;
        ++m_resolutionStats.staleModeChanges;
    }
    // A mode change at the size the swapchain and the newest graphs have, with nothing else asked: the swapchain is made again in
    // place, with no build (the graphs bind the backbuffers through the presentation copy, which follows CreateRTVs).
    {
        const auto swapchainSize = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("outputResolution")();
        const auto attached = AttachedResolution();
        if (request.recreateSwapchain && !request.upscaler && request.outputSize && request.width == newest.output.x
            && request.height == newest.output.y && swapchainSize.x == request.width && swapchainSize.y == request.height
            && attached.output.x == request.width && attached.output.y == request.height) {
            const bool resized = ResizeSwapChain(request.width, request.height);
            if (!resized) spdlog::critical("Renderer: the swapchain could not be made again at {}x{} after a mode change", request.width, request.height);
            ++stats.swapchainOnlyResizes;
            ++stats.unchanged;
            stats.lastApplyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            stats.maxApplyMs = (std::max)(stats.maxApplyMs, stats.lastApplyMs);
            spdlog::info("Renderer: resolution request consumed ({} request(s): {}): the swapchain was made again at {}x{}, nothing built; "
                "{:.2f} ms on the render thread", request.requests, stats.lastReasons, request.width, request.height, stats.lastApplyMs);
            return;
        }
    }
    const DirectX::XMUINT2 requestedOutput = request.outputSize ? DirectX::XMUINT2{ request.width, request.height } : newest.output;
    const bool outputChanged = request.outputSize
        && (request.recreateSwapchain || request.width != newest.output.x || request.height != newest.output.y);

    // The configuration for the (new) output size and the upscaler settings, made ahead (M5 step 3): the old one keeps its viewport
    // until its graphs retire. SSSR contexts follow the render resolution of the graphs that use them (FFXManager::AcquireSssrContext).
    auto& upscaling = UpscalingManager::GetInstance();
    const auto before = upscaling.Latest();
    std::shared_ptr<UpscalerConfiguration> configuration;
    if (outputChanged || request.upscaler) {
        auto upscalerRequest = upscaling.MakeRequest(RayReconstructionGuidesForNextBuild(), requestedOutput);
        const auto prepareBegin = std::chrono::steady_clock::now();
        configuration = upscaling.Prepare(upscalerRequest);
        stats.lastPrepareMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepareBegin).count();
        stats.maxPrepareMs = (std::max)(stats.maxPrepareMs, stats.lastPrepareMs);
    }
    const auto sameSize = [](DirectX::XMUINT2 a, DirectX::XMUINT2 b) { return a.x == b.x && a.y == b.y; };
    const bool resized = outputChanged || (configuration && (!before || !sameSize(before->RenderResolution(), configuration->RenderResolution())
        || !sameSize(before->OutputResolution(), configuration->OutputResolution())));
    // The sizes the next build is made for. The textures and the primary view's depth are made for the same ones.
    const auto resolution = BuildResolution();
    if (resized) {
        // New objects for the build, beside the attached graphs' (M5 step 5): no GPU wait, and the build runs on the build thread.
        auto objects = CreateTextures(resolution.render, resolution.output);
        objects->recreateSwapchain = request.recreateSwapchain;
        auto ingestionConfiguration = CaptureSceneIngestionConfiguration();
        ingestionConfiguration.renderResolution = resolution.render;
        ingestionConfiguration.outputResolution = resolution.output;
        objects->primaryLodHeight = PrimaryCameraLodHeight(ingestionConfiguration);
        // The primary view's depth maps for the new render size, when its own are of another. The view takes them at the attach.
        if (const auto camera = m_sceneRenderBridge.GetPrimaryCameraEntity(); camera && camera.is_alive()) {
            const auto* depth = camera.try_get<Components::DepthMap>();
            const bool sameSize = depth && depth->depthMap && depth->depthMap->GetWidth() == resolution.render.x
                && depth->depthMap->GetHeight() == resolution.render.y;
            if (!sameSize) objects->primaryDepth = CreateDepthMapComponent(resolution.render.x, resolution.render.y, 1, false);
        }
    }
    const bool changed = resized || configuration != nullptr;
    if (changed) {
        ++stats.applied;
        RequestGraphRebuild("resolution: " + stats.lastReasons);
        m_resolutionBuildRequested = true;
    }
    else {
        ++stats.unchanged;
    }
    stats.lastApplyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    stats.maxApplyMs = (std::max)(stats.maxApplyMs, stats.lastApplyMs);
    spdlog::info("Renderer: resolution request consumed ({} request(s): {}): {}; output {}x{}, render {}x{}; {:.2f} ms on the render thread "
        "(configuration {:.2f} ms)", request.requests, stats.lastReasons,
        !changed ? "nothing changed" : resized ? "resized, every epoch is built for it while the attached graphs keep rendering"
                                               : "same sizes, rebuilding for the new configuration",
        resolution.output.x, resolution.output.y, resolution.render.x, resolution.render.y, stats.lastApplyMs,
        (outputChanged || request.upscaler) ? stats.lastPrepareMs : 0.0);
}

void Renderer::ApplyWindowResolutionPreset(WindowResolutionPreset preset)
{
    const auto resolution = ResolveWindowResolutionPreset(preset);
    // The size the window is going to have: the latest request's, else the newest configuration's (a build for it may be in flight).
    auto currentResolution = BuildResolution().output;
    if (const auto pending = m_frameRevisions.PeekResolution(); pending.outputSize) currentResolution = { pending.width, pending.height };
    if (currentResolution.x == resolution.x && currentResolution.y == resolution.y) {
        return;
    }

    if (!m_hwnd) {
        m_frameRevisions.RequestOutputSize(resolution.x, resolution.y, "window preset");
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
        RequestGraphRebuild("environment missing, using the blank one");
        if (!m_warnedUsingFallbackEnvironment) {
            spdlog::warn("Environment file not found: {}. Falling back to blank environment resources.", envpath.string());
            m_warnedUsingFallbackEnvironment = true;
        }
    }
}
