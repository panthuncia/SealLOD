#pragma once

#include <dxgi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <atomic>
#include <chrono>
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
	// Both return false when nothing defined was written to inputs.output: the caller clears it (UpscalingPass).
	bool Evaluate(rhi::CommandList& commandList, const Components::Camera* camera, uint64_t frameNumber, double elapsedSeconds, const UpscalingInputs& inputs);
	// Records using the mode captured by a retained graph generation. This does
	// not consult the mutable settings/current-mode selection.
	bool EvaluateCaptured(UpscalingMode mode, rhi::CommandList& commandList,
		const Components::Camera* camera, uint64_t frameNumber, double elapsedSeconds,
		const UpscalingInputs& inputs);
    // Discards temporal history on the next evaluation. Callable from any thread.
    void RequestHistoryReset() { m_resetUpscalerHistory = true; }
	void Shutdown();

    bool InitSL();
	bool InitFFX();
    DirectX::XMFLOAT2 GetJitter(uint64_t frameNumber);
	// A Ray Reconstruction request resolves to DLSS unless the adapter supports it
	// and the active pipeline writes its guides, so a graph never declares guides
	// that nothing produces.
	// It also resolves to DLSS while Setup could not get Ray Reconstruction's optimal settings, until a retry succeeds
	// (TakeReconfigureRequest).
	UpscalingMode GetCurrentUpscalingMode() const {
		if (m_upscalingMode == UpscalingMode::DLSSRayReconstruction &&
			(m_rayReconstructionSettingsFailed.load(std::memory_order_relaxed) ||
			!(m_rayReconstructionSupported && m_rayReconstructionGuidesAvailable))) {
			return UpscalingMode::DLSS;
		}
		return m_upscalingMode;
	}
	// Whether the adapter supports Ray Reconstruction. A failure to get its optimal settings doesn't change this.
	bool IsRayReconstructionSupported() const { return m_rayReconstructionSupported; }
	// True once when the upscaler should be set up again and the graph rebuilt: a retry of Ray Reconstruction after Setup
	// failed to get its optimal settings (2 s, doubling to 32 s, while it keeps failing), or a simulated failure. The renderer
	// polls this every frame.
	bool TakeReconfigureRequest();
	// Debug: the next outOfMemoryFrames DLSS evaluations act as if Streamline reported GPU memory pressure, and the next
	// settingsFailures slDLSSDGetOptimalSettings calls as if they failed (which also requests a reconfigure, so it is used).
	// BASICRENDERER_SIMULATE_DLSS_FAILURE="oom=N,settings=M" does the same at startup; the control pipe has
	// upscaler.simulate_failure.
	void SimulateFailures(uint32_t outOfMemoryFrames, uint32_t settingsFailures);
	void SetRayReconstructionGuidesAvailable(bool available) { m_rayReconstructionGuidesAvailable = available; }
    UpscaleQualityMode GetCurrentUpscalingQualityMode() const { return m_upscaleQualityMode; }

    // What the upscaler is doing, for the upscaler.get control command: the mode requested, the one the current graph was
    // built with, and what the last evaluation actually ran.
    struct Status {
        UpscalingMode requested = UpscalingMode::None; // m_upscalingMode: the setting, as last synced
        UpscalingMode current = UpscalingMode::None;   // GetCurrentUpscalingMode(): what a graph built now uses
        UpscalingMode effective = UpscalingMode::None; // current, after adapter support
        UpscalingMode graph = UpscalingMode::None;     // the mode the last evaluation was recorded with (the graph's)
        UpscalingMode evaluated = UpscalingMode::None; // what that evaluation ran
        bool dlssSupported = false;
        bool rayReconstructionSupported = false;
        bool rayReconstructionGuidesAvailable = false;
        uint64_t evaluatedFrame = 0;
        uint64_t evaluations = 0;
        uint64_t failedEvaluations = 0; // Streamline evaluations that returned an error, or were skipped for a missing input
        uint64_t outOfMemorySkips = 0; // Of those, the ones Streamline skipped for GPU memory pressure
        uint64_t lastFailedFrame = 0;
        uint64_t consecutiveFailures = 0; // Failed evaluations since the last one that succeeded
        // What failed evaluations wrote instead: the input copied (same size), DLSS SR, or black.
        uint64_t fallbackCopies = 0;
        uint64_t fallbackSuperResolution = 0;
        uint64_t fallbackClears = 0;
        bool rayReconstructionSettingsFailed = false; // Setup couldn't get its optimal settings: graphs use DLSS until a retry works
        uint64_t rayReconstructionSettingsFailures = 0;
        double rayReconstructionRetrySeconds = 0.0; // Until the next retry, while it is failing
    };
    Status GetStatus() const;

    void SetUpscalingMode(UpscalingMode mode) { m_upscalingMode = mode; }
    void SetUpscalingQualityMode(UpscaleQualityMode mode) { m_upscaleQualityMode = mode; }

private:
    UpscalingManager() = default;
    void SyncSettingsFromSettingsManager();
    bool EnsureFSRContext();
	bool EvaluateDLSS(rhi::CommandList& commandList, const Components::Camera* camera, uint64_t frameNumber, const UpscalingInputs& inputs, bool rayReconstruction);
    void EvaluateFSR3(rhi::CommandList& commandList, const Components::Camera* camera, double elapsedSeconds, const UpscalingInputs& inputs);
	void EvaluateNone(rhi::CommandList& commandList, const Components::Camera* camera, const UpscalingInputs& inputs);
    // A DLSS evaluation that didn't run leaves the aliased output undefined. HandleFailedEvaluation counts and logs it (at
    // most a line every 5 s) and makes the output defined: DLSS SR wrote it (EvaluateDLSS tries that when the sizes differ),
    // or it copies the input when the sizes match, or it returns false for the pass to clear it.
    enum class FailedOutput { Undefined, SuperResolution, LeaveUndefined };
    bool HandleFailedEvaluation(rhi::CommandList& commandList, uint64_t frameNumber, const UpscalingInputs& inputs, bool rayReconstruction,
        bool outOfMemory, const char* reason, int32_t result, FailedOutput output);
    void NoteSuccessfulEvaluation(uint64_t frameNumber, bool rayReconstruction);
    bool TakeSimulatedOutOfMemory();
    bool TakeSimulatedSettingsFailure();
	UpscalingMode m_upscalingMode = UpscalingMode::DLSS;
    UpscaleQualityMode m_upscaleQualityMode = UpscaleQualityMode::Balanced;
    std::function<DirectX::XMUINT2()> m_getRenderRes;
	std::function<DirectX::XMUINT2()> m_getOutputRes;
	bool m_fsrIntialized = false;
    ffx::Context m_fsrUpscalingContext = nullptr;
	bool m_dlssSupported = false;
	bool m_rayReconstructionSupported = false;
	bool m_rayReconstructionGuidesAvailable = false;
    // Streamline history is tied to the input/output dimensions selected by
    // Setup(). A quality-mode or output-size change must invalidate it on the
    // first evaluation using the replacement render targets.
    std::atomic<bool> m_resetUpscalerHistory = true;
    // Read by GetStatus from the control thread.
    std::atomic<UpscalingMode> m_lastGraphMode = UpscalingMode::None;
    std::atomic<UpscalingMode> m_lastEvaluatedMode = UpscalingMode::None;
    std::atomic<uint64_t> m_lastEvaluatedFrame = 0;
    std::atomic<uint64_t> m_evaluations = 0;
    std::atomic<uint64_t> m_failedEvaluations = 0;
    std::atomic<uint64_t> m_outOfMemorySkips = 0;
    std::atomic<uint64_t> m_lastFailedFrame = 0;
    std::atomic<uint64_t> m_consecutiveFailures = 0;
    std::atomic<uint64_t> m_fallbackCopies = 0;
    std::atomic<uint64_t> m_fallbackSuperResolution = 0;
    std::atomic<uint64_t> m_fallbackClears = 0;
    // Failure log rate limit (evaluation runs under m_evaluateMutex).
    std::chrono::steady_clock::time_point m_lastFailureLog{};
    uint64_t m_failuresAtLastLog = 0;
    bool m_superResolutionOptionsSet = false;
    // Ray Reconstruction's optimal settings: a failure falls back to DLSS and retries, rather than giving up for good.
    std::atomic<bool> m_rayReconstructionSettingsFailed = false;
    std::atomic<uint64_t> m_rayReconstructionSettingsFailures = 0;
    std::atomic<int64_t> m_reconfigureAtNs = 0; // steady_clock; 0 when nothing is scheduled
    std::atomic<bool> m_reconfigureNow = false;
    double m_rayReconstructionRetryDelaySeconds = 2.0;
    std::atomic<uint32_t> m_simulatedOutOfMemoryFrames = 0;
    std::atomic<uint32_t> m_simulatedSettingsFailures = 0;
    // Vendor upscalers own mutable history/context state. Recording may run on
    // workers for different frames, so serialize evaluation in frame order.
    std::mutex m_evaluateMutex;
};

inline UpscalingManager& UpscalingManager::GetInstance() {
    static UpscalingManager instance;
    return instance;
}
