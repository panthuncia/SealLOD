#pragma once

#include <dxgi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <vector>
#include <DirectXMath.h>
#include <functional>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_matrix_helpers.h>

#include <rhi.h>

#include "ThirdParty/FFX/ffx_api.hpp"
#include "ThirdParty/FFX/ffx_upscale.hpp"

#include "Scene/Components.h"

enum class UpscalingMode {
    None,
    FSR3,
    DLSS
};

static constexpr const char* UpscalingModeNames[] = {
    "None",
    "FSR3",
    "DLSS",
};
static constexpr int UpscalingModeCount = ARRAYSIZE(UpscalingModeNames);

enum class UpscaleQualityMode {
    DLAA,
	//UltraQuality, // DLSS UltraQuality returns a resolution of 0? What is this?
    Quality,
    Balanced,
    Performance,
    UltraPerformance
};

static constexpr const char* UpscaleQualityModeNames[] = {
    "DLAA",
    //"UltraQuality",
    "Quality",
    "Balanced",
    "Performance",
    "UltraPerformance"
};
static constexpr int UpscaleQualityModeCount = ARRAYSIZE(UpscaleQualityModeNames);

inline FfxApiUpscaleQualityMode ToFFXQualityMode(UpscaleQualityMode mode) {
    switch (mode) {
    case UpscaleQualityMode::DLAA:
        return FFX_UPSCALE_QUALITY_MODE_NATIVEAA;
    //case UpscaleQualityMode::UltraQuality:
        //return FFX_UPSCALE_QUALITY_MODE_QUALITY; // FFX does not have a separate UltraQuality mode
    case UpscaleQualityMode::Quality:
        return FFX_UPSCALE_QUALITY_MODE_QUALITY;
    case UpscaleQualityMode::Balanced:
        return FFX_UPSCALE_QUALITY_MODE_BALANCED;
    case UpscaleQualityMode::Performance:
        return FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;
    case UpscaleQualityMode::UltraPerformance:
        return FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE;
    default:
        return FFX_UPSCALE_QUALITY_MODE_BALANCED; // Default to balanced
    }
}

inline sl::DLSSMode ToSLQualityMode(UpscaleQualityMode mode) {
    switch (mode) {
    case UpscaleQualityMode::DLAA:
        return sl::DLSSMode::eDLAA;
    //case UpscaleQualityMode::UltraQuality:
        //return sl::DLSSMode::eUltraQuality;
    case UpscaleQualityMode::Quality:
        return sl::DLSSMode::eMaxQuality;
    case UpscaleQualityMode::Balanced:
        return sl::DLSSMode::eBalanced;
    case UpscaleQualityMode::Performance:
        return sl::DLSSMode::eMaxPerformance;
    case UpscaleQualityMode::UltraPerformance:
        return sl::DLSSMode::eUltraPerformance;
    default:
        return sl::DLSSMode::eBalanced; // Default to balanced
    }
}

class PixelBuffer;
struct RenderContext;

class UpscalingManager {
public:
    static UpscalingManager& GetInstance();
    void InitializeAdapter();
	void ProxyDevice();
    void Setup();
	void Evaluate(rhi::CommandList& commandList, const Components::Camera* camera, uint64_t frameNumber, double elapsedSeconds, PixelBuffer* pHDRTarget, PixelBuffer* pUpscaledHDRTarget, PixelBuffer* pDepthTexture, PixelBuffer* pMotionVectors);
	void Shutdown();

    bool InitSL();
	bool InitFFX();
    DirectX::XMFLOAT2 GetJitter(uint64_t frameNumber);
	UpscalingMode GetCurrentUpscalingMode() const { return m_upscalingMode; }
    UpscaleQualityMode GetCurrentUpscalingQualityMode() const { return m_upscaleQualityMode; }

    void SetUpscalingMode(UpscalingMode mode) { m_upscalingMode = mode; }
    void SetUpscalingQualityMode(UpscaleQualityMode mode) { m_upscaleQualityMode = mode; }

private:
    UpscalingManager() = default;
    bool EnsureFSRContext();
	void EvaluateDLSS(rhi::CommandList& commandList, const Components::Camera* camera, uint64_t frameNumber, PixelBuffer* pHDRTarget, PixelBuffer* pUpscaledHDRTarget, PixelBuffer* pDepthTexture, PixelBuffer* pMotionVectors);
    void EvaluateFSR3(rhi::CommandList& commandList, const Components::Camera* camera, double elapsedSeconds, PixelBuffer* pHDRTarget, PixelBuffer* pUpscaledHDRTarget, PixelBuffer* pDepthTexture, PixelBuffer* pMotionVectors);
	void EvaluateNone(rhi::CommandList& commandList, const Components::Camera* camera, PixelBuffer* pHDRTarget, PixelBuffer* pUpscaledHDRTarget, PixelBuffer* pDepthTexture, PixelBuffer* pMotionVectors);
	UpscalingMode m_upscalingMode = UpscalingMode::DLSS;
    UpscaleQualityMode m_upscaleQualityMode = UpscaleQualityMode::Balanced;
    std::function<DirectX::XMUINT2()> m_getRenderRes;
	std::function<DirectX::XMUINT2()> m_getOutputRes;
    bool m_fsrIntialized = false;
    ffx::Context m_fsrUpscalingContext = nullptr;
	bool m_dlssSupported = false;
};

inline UpscalingManager& UpscalingManager::GetInstance() {
    static UpscalingManager instance;
    return instance;
}