#include "PostProcessing/FidelityFX/FFXManager.h"

#include <chrono>

#include "Runtime/Settings/SettingsManager.h"
#include "Runtime/Device/DeviceManager.h"
#include "OpenRenderGraph/OpenRenderGraph.h"
#include "PostProcessing/FidelityFX/Backend/FfxBackendAdapters.h"
#include "ThirdParty/FFX/host/ffx_sssr.h"
#include "Runtime/Resources/ResourceManager.h"
#include "BasicRenderer/Scene/Scene.h"
#include "BasicRenderer/Extensions/RenderContext.h"

FFXManager::SssrContext::~SssrContext() {
    if (m_native) FFXManager::GetInstance().Retire(std::move(m_native));
}

std::shared_ptr<FFXManager::SssrContext> FFXManager::AcquireSssrContext(DirectX::XMUINT2 renderSize) {
    std::scoped_lock lock(m_contextsMutex);
    std::erase_if(m_contexts, [](const std::weak_ptr<SssrContext>& context) { return context.expired(); });
    for (const auto& weak : m_contexts) {
        if (auto context = weak.lock(); context && context->m_renderSize.x == renderSize.x && context->m_renderSize.y == renderSize.y) {
            return context;
        }
    }

    const auto begin = std::chrono::steady_clock::now();
    auto native = std::make_unique<SssrContext::Native>();
    const rhi::Device device = DeviceManager::GetInstance().GetDevice();
    const rhi::Backend backend = DeviceManager::GetInstance().GetBackend();
    if (!fidelityfx_backend::host::CreateBackendInterface(native->backendInterface, native->scratchMemory, backend, device, 1)) {
        spdlog::error("FFXManager: failed to create the SSSR backend interface for backend {}", static_cast<uint32_t>(backend));
        if (native->scratchMemory) free(native->scratchMemory);
        return nullptr;
    }

	FfxSssrContextDescription sssrDesc{};
	sssrDesc.backendInterface = native->backendInterface;
    sssrDesc.flags = FFX_SSSR_ENABLE_DEPTH_INVERTED;
    // SSSR copies the normals into its history (CopyTextureRegion), so the history has the canonical surface's normal format
    // (Builtin::Surface::NormalRoughness, R16G16B16A16_Float). R32G32B32A32 made that copy invalid: Shading's command list
    // failed at Close with E_INVALIDARG whenever SSSR ran.
    sssrDesc.normalsHistoryBufferFormat = FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;
	sssrDesc.renderSize = { renderSize.x, renderSize.y };

    const FfxErrorCode createResult = ffxSssrContextCreate(&native->context, &sssrDesc);
    if (createResult != FFX_OK) {
        spdlog::error("FFXManager: failed to create the SSSR context for backend {} error {}", static_cast<uint32_t>(backend), static_cast<int>(createResult));
        if (native->scratchMemory) free(native->scratchMemory);
        return nullptr;
    }
    auto context = std::shared_ptr<SssrContext>(new SssrContext());
    context->m_renderSize = renderSize;
    context->m_native = std::move(native);
    m_contexts.push_back(context);
    spdlog::info("FFXManager: SSSR context for {}x{} made in {:.2f} ms", renderSize.x, renderSize.y,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
    return context;
}

void FFXManager::Retire(std::unique_ptr<SssrContext::Native> native) {
    // After Shutdown the device goes: what retires later is left to the process's exit.
    if (m_shutDown.load(std::memory_order_acquire)) return;
    std::scoped_lock lock(m_contextsMutex);
    m_retired.push_back(std::move(native));
}

void FFXManager::Destroy(SssrContext::Native& native) {
    ffxSssrContextDestroy(&native.context);
    native.context = {};
    native.backendInterface = {};
    if (native.scratchMemory) {
        free(native.scratchMemory);
        native.scratchMemory = nullptr;
    }
}

void FFXManager::CollectRetired() {
    std::vector<std::unique_ptr<SssrContext::Native>> retired;
    {
        std::scoped_lock lock(m_contextsMutex);
        retired.swap(m_retired);
    }
    if (retired.empty()) return;
    // The last graph that used each has retired, so the GPU is done with its resources.
    std::scoped_lock evaluateLock(m_evaluateMutex);
    for (auto& native : retired) Destroy(*native);
    spdlog::info("FFXManager: destroyed {} retired SSSR context(s)", retired.size());
}

void FFXManager::Shutdown() {
    // The graphs are gone (the device is idle).
    CollectRetired();
    m_shutDown.store(true, std::memory_order_release);
}

void FFXManager::EvaluateSSSR(SssrContext& sssr,
    rhi::CommandList& commandList,
    const Components::Camera* currentCamera,
    org::PixelBuffer* pHDRTarget,
    org::PixelBuffer* pDepthTexture,
    org::PixelBuffer* pNormals,
    org::PixelBuffer* pMetallicRoughness,
    org::PixelBuffer* pMotionVectors,
    org::PixelBuffer* pEnvironmentCubemap,
    org::PixelBuffer* pBRDFLUT,
    org::PixelBuffer* pReflectionsTarget) {
    std::scoped_lock evaluateLock(m_evaluateMutex);

    const rhi::Backend backend = DeviceManager::GetInstance().GetBackend();
    if (!sssr.m_native) {
        return;
    }

	FfxSssrDispatchDescription sssrDesc{};
    sssrDesc.brdfTexture = fidelityfx_backend::host::GetResource(backend, pBRDFLUT, L"BRDFLUT", FFX_RESOURCE_STATE_COMMON);
    sssrDesc.color = fidelityfx_backend::host::GetResource(backend, pHDRTarget, L"HDRColor", FFX_RESOURCE_STATE_COMMON);
    sssrDesc.depth = fidelityfx_backend::host::GetResource(backend, pDepthTexture, L"Depth", FFX_RESOURCE_STATE_COMMON);
    sssrDesc.environmentMap = fidelityfx_backend::host::GetResource(backend, pEnvironmentCubemap, L"EnvironmentMap", FFX_RESOURCE_STATE_COMMON);
    sssrDesc.materialParameters = fidelityfx_backend::host::GetResource(backend, pMetallicRoughness, L"MaterialParameters", FFX_RESOURCE_STATE_COMMON);
    sssrDesc.motionVectors = fidelityfx_backend::host::GetResource(backend, pMotionVectors, L"MotionVectors", FFX_RESOURCE_STATE_COMMON);
    sssrDesc.normal = fidelityfx_backend::host::GetResource(backend, pNormals, L"Normals", FFX_RESOURCE_STATE_COMMON);
    sssrDesc.brdfTexture = fidelityfx_backend::host::GetResource(backend, pBRDFLUT, L"BRDFLUT", FFX_RESOURCE_STATE_COMMON);
    sssrDesc.output = fidelityfx_backend::host::GetResource(backend, pReflectionsTarget, L"Reflections", FFX_RESOURCE_STATE_COMMON);

    auto invViewProjection = DirectX::XMMatrixInverse(nullptr, currentCamera->info.viewProjection);
    auto prevViewProjection = DirectX::XMMatrixMultiply(currentCamera->info.prevView, currentCamera->info.prevJitteredProjection);

    DirectX::XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(sssrDesc.invViewProjection), invViewProjection);
    DirectX::XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(sssrDesc.projection), currentCamera->info.jitteredProjection);
	DirectX::XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(sssrDesc.invProjection), currentCamera->info.projectionInverse);
	DirectX::XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(sssrDesc.view), currentCamera->info.view);
	DirectX::XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(sssrDesc.invView), currentCamera->info.viewInverse);
    DirectX::XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(sssrDesc.prevViewProjection), prevViewProjection);
    sssrDesc.commandList = fidelityfx_backend::host::GetCommandList(backend, commandList);
    if (sssrDesc.commandList == nullptr) {
        spdlog::warn("FFXManager::EvaluateSSSR skipped dispatch because no command list adapter is available for backend {}", static_cast<uint32_t>(backend));
        return;
    }
    // The context's render size: the graph that holds it was built for it.
    const auto renderSize = sssr.RenderSize();
    sssrDesc.renderSize = { renderSize.x, renderSize.y };
	sssrDesc.motionVectorScale = { -0.5f, 0.5f };
    sssrDesc.normalUnPackMul = 1.0f;
	sssrDesc.normalUnPackAdd = 0.0f;
    sssrDesc.roughnessChannel = 3; // canonical normal-roughness stores perceptual roughness in alpha
    sssrDesc.isRoughnessPerceptual = true;
	sssrDesc.temporalStabilityFactor = 0.7f; // TODO: make everything below configurable
    sssrDesc.iblFactor = 1.0f;
	sssrDesc.depthBufferThickness = 0.015f;
	sssrDesc.roughnessThreshold = 0.3f;
	sssrDesc.varianceThreshold = 0.1f;
	sssrDesc.maxTraversalIntersections = 128;
    sssrDesc.minTraversalOccupancy = 4;
	sssrDesc.mostDetailedMip = 0;
    sssrDesc.samplesPerQuad = 1;
	sssrDesc.temporalVarianceGuidedTracingEnabled = true;

	ffxSssrContextDispatch(&sssr.m_native->context, &sssrDesc);

}
