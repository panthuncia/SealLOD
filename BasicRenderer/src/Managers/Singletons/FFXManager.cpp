#include "Managers/Singletons/FFXManager.h"

#include "Managers/Singletons/SettingsManager.h"
#include "Managers/Singletons/DeviceManager.h"
#include "OpenRenderGraph/OpenRenderGraph.h"
#include "FidelityFX/FfxBackendAdapters.h"
#include "ThirdParty/FFX/host/ffx_sssr.h"
#include "Managers/Singletons/ResourceManager.h"
#include "Scene/Scene.h"
#include "Render/RenderContext.h"

bool FFXManager::InitFFX() {
    Shutdown();

    m_getRenderRes = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution");
    m_getOutputRes = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("outputResolution");
    auto outputRes = m_getOutputRes();
    auto renderRes = m_getRenderRes();

    const rhi::Device device = DeviceManager::GetInstance().GetDevice();
    const rhi::Backend backend = DeviceManager::GetInstance().GetBackend();
    if (!fidelityfx_backend::host::CreateBackendInterface(m_backendInterface, m_pScratchMemory, backend, device, 1)) {
        spdlog::error("FFXManager::InitFFX failed to create backend interface for backend {}", static_cast<uint32_t>(backend));
        return false;
    }

	FfxSssrContextDescription sssrDesc{};
	sssrDesc.backendInterface = m_backendInterface;
    sssrDesc.flags = FFX_SSSR_ENABLE_DEPTH_INVERTED;
    sssrDesc.normalsHistoryBufferFormat = FFX_SURFACE_FORMAT_R32G32B32A32_TYPELESS;
	sssrDesc.renderSize = { renderRes.x, renderRes.y };

    const FfxErrorCode createResult = ffxSssrContextCreate(&m_sssrContext, &sssrDesc);
    if (createResult != FFX_OK) {
        spdlog::error("FFXManager::InitFFX failed to create SSSR context for backend {} error {}", static_cast<uint32_t>(backend), static_cast<int>(createResult));
        Shutdown();
        return false;
    }

    m_sssrContextCreated = true;

    return true;
}

void FFXManager::Shutdown() {
    if (m_sssrContextCreated) {
		ffxSssrContextDestroy(&m_sssrContext);
        m_sssrContext = {};
        m_sssrContextCreated = false;
    }
    m_backendInterface = {};
    if (m_pScratchMemory) {
        free(m_pScratchMemory);
        m_pScratchMemory = nullptr;
    }
}

void FFXManager::EvaluateSSSR(rhi::CommandList& commandList,
    const Components::Camera* currentCamera,
    PixelBuffer* pHDRTarget,
    PixelBuffer* pDepthTexture,
    PixelBuffer* pNormals,
    PixelBuffer* pMetallicRoughness,
    PixelBuffer* pMotionVectors,
    PixelBuffer* pEnvironmentCubemap,
    PixelBuffer* pBRDFLUT,
    PixelBuffer* pReflectionsTarget) {

    const rhi::Backend backend = DeviceManager::GetInstance().GetBackend();
    if (!m_sssrContextCreated) {
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
    auto renderSize = m_getRenderRes();
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

	ffxSssrContextDispatch(&m_sssrContext, &sssrDesc);
    
}
