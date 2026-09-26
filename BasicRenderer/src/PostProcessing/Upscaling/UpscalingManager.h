#pragma once

#include <dxgi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <vector>
#include <DirectXMath.h>
#include <functional>
#include <mutex>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_matrix_helpers.h>

#include <rhi.h>

#include "ThirdParty/FFX/ffx_api.hpp"
#include "ThirdParty/FFX/ffx_upscale.hpp"

#include "BasicRenderer/Scene/Components.h"

#include <BasicRenderer/Pipeline/UpscalingTypes.h>

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

namespace org { class PixelBuffer; }
struct RenderContext;

class UpscalingManager {
public:
    static UpscalingManager& GetInstance();
    void InitializeAdapter();
	void ProxyDevice();
    void Setup();
	void Evaluate(rhi::CommandList& commandList, const Components::Camera* camera, uint64_t frameNumber, double elapsedSeconds, org::PixelBuffer* pHDRTarget, org::PixelBuffer* pUpscaledHDRTarget, org::PixelBuffer* pDepthTexture, org::PixelBuffer* pMotionVectors);
	// Records using the mode captured by a retained graph generation. This does
	// not consult the mutable settings/current-mode selection.
	void EvaluateCaptured(UpscalingMode mode, rhi::CommandList& commandList,
		const Components::Camera* camera, uint64_t frameNumber, double elapsedSeconds,
		org::PixelBuffer* pHDRTarget, org::PixelBuffer* pUpscaledHDRTarget,
		org::PixelBuffer* pDepthTexture, org::PixelBuffer* pMotionVectors);
    void RequestHistoryReset() { m_resetUpscalerHistory = true; }
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
    void SyncSettingsFromSettingsManager();
    bool EnsureFSRContext();
	void EvaluateDLSS(rhi::CommandList& commandList, const Components::Camera* camera, uint64_t frameNumber, org::PixelBuffer* pHDRTarget, org::PixelBuffer* pUpscaledHDRTarget, org::PixelBuffer* pDepthTexture, org::PixelBuffer* pMotionVectors);
    void EvaluateFSR3(rhi::CommandList& commandList, const Components::Camera* camera, double elapsedSeconds, org::PixelBuffer* pHDRTarget, org::PixelBuffer* pUpscaledHDRTarget, org::PixelBuffer* pDepthTexture, org::PixelBuffer* pMotionVectors);
	void EvaluateNone(rhi::CommandList& commandList, const Components::Camera* camera, org::PixelBuffer* pHDRTarget, org::PixelBuffer* pUpscaledHDRTarget, org::PixelBuffer* pDepthTexture, org::PixelBuffer* pMotionVectors);
	UpscalingMode m_upscalingMode = UpscalingMode::DLSS;
    UpscaleQualityMode m_upscaleQualityMode = UpscaleQualityMode::Balanced;
    std::function<DirectX::XMUINT2()> m_getRenderRes;
	std::function<DirectX::XMUINT2()> m_getOutputRes;
	bool m_fsrIntialized = false;
    ffx::Context m_fsrUpscalingContext = nullptr;
	bool m_dlssSupported = false;
    // Streamline history is tied to the input/output dimensions selected by
    // Setup(). A quality-mode or output-size change must invalidate it on the
    // first evaluation using the replacement render targets.
    bool m_resetUpscalerHistory = true;
    // Streamline reports memory pressure as a warning result. Avoid turning a
    // recoverable, persistent condition into one error log entry per frame.
	bool m_reportedDlssOutOfMemory = false;
    // Vendor upscalers own mutable history/context state. Recording may run on
    // workers for different frames, so serialize evaluation in frame order.
    std::mutex m_evaluateMutex;
};

inline UpscalingManager& UpscalingManager::GetInstance() {
    static UpscalingManager instance;
    return instance;
}
