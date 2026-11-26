#pragma once

#include <dxgi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <vector>
#include <DirectXMath.h>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

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

// What an upscaler configuration is made for: the settings, the output size and whether the graphs built with it write Ray
// Reconstruction's guides (the recipe they are built from, not the running one).
struct UpscalerRequest {
    UpscalingMode mode = UpscalingMode::DLSS;
    UpscaleQualityMode quality = UpscaleQualityMode::Balanced;
    DirectX::XMUINT2 output{};
    bool rayReconstructionGuides = false;
};

// One upscaler configuration (M5 step 3): the mode, quality and sizes the graphs built for it declare and evaluate with, on
// resources of its own: a Streamline viewport (DLSS or Ray Reconstruction) or an FSR context. UpscalingManager::Prepare makes
// it ahead of the build that uses it and allocates its feature on a thread of its own; the graphs built for it hold it (through
// their UpscalingGenerationService), so it becomes the evaluated one at their attach and is released when the last of them
// retires. No GPU idle wait: a retired graph's work is complete when it goes.
class UpscalerConfiguration {
public:
    ~UpscalerConfiguration();
    UpscalerConfiguration(const UpscalerConfiguration&) = delete;
    UpscalerConfiguration& operator=(const UpscalerConfiguration&) = delete;

    // What graphs declare: the requested mode, with Ray Reconstruction resolved to DLSS when the recipe writes no guides, the
    // adapter lacks it or its optimal settings failed.
    UpscalingMode Mode() const noexcept { return m_mode; }
    // What evaluation runs: Mode() after adapter and Streamline support.
    UpscalingMode Effective() const noexcept { return m_effective; }
    UpscaleQualityMode Quality() const noexcept { return m_quality; }
    DirectX::XMUINT2 RenderResolution() const noexcept { return m_render; }
    DirectX::XMUINT2 OutputResolution() const noexcept { return m_output; }
    bool RayReconstructionGuides() const noexcept { return m_guides; }
    uint64_t Serial() const noexcept { return m_serial; }
    uint32_t Viewport() const noexcept { return m_feature->viewport; }
    // The quality mode and output size the feature was made for. Quality() and OutputResolution() can differ only for a
    // configuration that shares an earlier configuration's feature (its render size within the feature's range).
    UpscaleQualityMode FeatureQuality() const noexcept { return m_feature->quality; }
    DirectX::XMUINT2 FeatureOutputResolution() const noexcept { return m_feature->output; }
    bool SharesFeature() const noexcept { return m_sharesFeature; }
    // Whether another configuration's graphs would be built differently or evaluate differently.
    bool SameAs(UpscalingMode mode, UpscalingMode effective, UpscaleQualityMode quality, DirectX::XMUINT2 render,
        DirectX::XMUINT2 output) const noexcept {
        return m_mode == mode && m_effective == effective && m_quality == quality && m_render.x == render.x
            && m_render.y == render.y && m_output.x == output.x && m_output.y == output.y;
    }

private:
    friend class UpscalingManager;
    // What the preparation worker makes, shared by the configuration and the worker's queue. The configuration may go before
    // the worker gets to it (a newer one replaced it): the worker then skips it, or frees what it made at once. Whichever of the
    // two finishes last releases it, so the render thread never waits for a preparation it no longer needs.
    struct Feature {
        UpscalingMode effective = UpscalingMode::None;
        UpscaleQualityMode quality = UpscaleQualityMode::Balanced;
        DirectX::XMUINT2 render{};
        DirectX::XMUINT2 output{};
        uint64_t serial = 0;
        uint32_t viewport = 0; // Streamline viewport, 0 without DLSS
        // Made by the preparation, read once it is prepared.
        ffx::Context fsrContext = nullptr; // FSR only
        bool allocatedAhead = false; // the DLSS feature exists before the first evaluation
        bool made = false; // the preparation ran (it was not skipped)
        double prepareMs = 0.0;
        std::mutex mutex;
        std::condition_variable changed;
        bool prepared = false;
        bool released = false; // the configuration is gone
        bool wanted = false; // something needs it: a build captured it, its attach or an evaluation waits (RequestPreparation)
        // Configurations that evaluate with this feature (under mutex). More than one when a later configuration's render size
        // fits the feature's dynamic resolution range (UpscalingManager::Prepare); the last one to go releases it.
        uint32_t users = 1;
        // The render sizes the feature takes at its output size and mode (DLSS's optimal settings' minimum and maximum).
        DirectX::XMUINT2 renderMin{};
        DirectX::XMUINT2 renderMax{};
    };
    UpscalerConfiguration() = default;
    // Returns once the preparation worker is done with it. Any thread.
    void WaitPrepared() const;

    UpscalingMode m_mode = UpscalingMode::None;
    UpscalingMode m_effective = UpscalingMode::None;
    UpscaleQualityMode m_quality = UpscaleQualityMode::Balanced;
    DirectX::XMUINT2 m_render{};
    DirectX::XMUINT2 m_output{};
    bool m_guides = false;
    uint64_t m_serial = 0;
    // The preparation: the feature allocated (slAllocateResources) or the FSR context made, on the preparation worker.
    std::shared_ptr<Feature> m_feature = std::make_shared<Feature>();
    bool m_sharesFeature = false; // made with an earlier configuration's feature
    // Evaluation state, under the manager's evaluation lock.
    mutable bool m_firstEvaluation = true; // resets the history
    mutable bool m_superResolutionOptionsSet = false; // the Ray Reconstruction failure fallback's DLSS options on this viewport
};

class UpscalingManager {
public:
    static UpscalingManager& GetInstance();
    void InitializeAdapter();
	void ProxyDevice();
	// Both return false when nothing defined was written to inputs.output: the caller clears it (UpscalingPass).
	// Records with the configuration a retained graph generation was built for. This does not consult the mutable settings or
	// the latest configuration.
	bool Evaluate(const UpscalerConfiguration& configuration, rhi::CommandList& commandList,
		const Components::Camera* camera, uint64_t frameNumber, double elapsedSeconds,
		const UpscalingInputs& inputs);
    // Discards temporal history on the next evaluation. Callable from any thread.
    void RequestHistoryReset() { m_resetUpscalerHistory = true; }
	void Shutdown();

    bool InitSL();

    // The request the settings make now for an output size, for graphs built from a recipe that does or does not write Ray
    // Reconstruction's guides. The output size is the caller's (the window's latest, M5 step 5), not the swapchain's.
    UpscalerRequest MakeRequest(bool rayReconstructionGuides, DirectX::XMUINT2 output) const;
    // A configuration for the request, unless the latest one would be the same (then null): resolves the mode and the render
    // resolution with the vendor queries (render thread), and starts the feature allocation (or the FSR context) on a thread of its
    // own. The latest configuration is what graphs built from now on capture. renderResolution is written here only for the first
    // configuration; later ones write it at the attach of their graphs (Renderer::SwitchResolutionObjects).
    std::shared_ptr<UpscalerConfiguration> Prepare(const UpscalerRequest& request);
    // Starts the configuration's preparation if it has not started: the worker prepares only what something needs, so a run of
    // requests with no build between them (a drag's WM_SIZE messages in one message pump) allocates once, for the last.
    // UpscalingGenerationService::CaptureCurrent calls it when a build captures the configuration.
    void RequestPreparation(const UpscalerConfiguration& configuration);
    // At the attach of graphs built for it: waits for its preparation if it is still running. Jitter and the status follow it.
    void MakeCurrent(const std::shared_ptr<const UpscalerConfiguration>& configuration);
    // What graphs built now capture, and what the attached graphs evaluate with.
    std::shared_ptr<const UpscalerConfiguration> Latest() const;
    std::shared_ptr<const UpscalerConfiguration> Current() const;
    // Releases the viewports and contexts of configurations whose last graph retired. Render thread, once a frame.
    void CollectRetired();

    DirectX::XMFLOAT2 GetJitter(uint64_t frameNumber);
	// What a graph built now uses: the latest configuration's mode. A Ray Reconstruction request resolves to DLSS unless the
	// adapter supports it and the recipe being built writes its guides, so a graph never declares guides that nothing produces.
	// It also resolves to DLSS while Ray Reconstruction's optimal settings could not be had, until a retry succeeds
	// (TakeReconfigureRequest).
	UpscalingMode GetCurrentUpscalingMode() const {
		const auto latest = Latest();
		return latest ? latest->Mode() : UpscalingMode::None;
	}
	// Whether the adapter supports Ray Reconstruction. A failure to get its optimal settings doesn't change this.
	bool IsRayReconstructionSupported() const { return m_rayReconstructionSupported; }
	// True once when the upscaler should be prepared again and the graph rebuilt: a retry of Ray Reconstruction after its optimal
	// settings failed (2 s, doubling to 32 s, while it keeps failing), or a simulated failure. The renderer polls this every frame.
	bool TakeReconfigureRequest();
	// Debug: the next outOfMemoryFrames DLSS evaluations act as if Streamline reported GPU memory pressure, and the next
	// settingsFailures slDLSSDGetOptimalSettings calls as if they failed (which also requests a reconfigure, so it is used).
	// BASICRENDERER_SIMULATE_DLSS_FAILURE="oom=N,settings=M" does the same at startup; the control pipe has
	// upscaler.simulate_failure.
	void SimulateFailures(uint32_t outOfMemoryFrames, uint32_t settingsFailures);
	// For the status only: configurations take the guides from their request.
	void SetRayReconstructionGuidesAvailable(bool available) { m_rayReconstructionGuidesAvailable = available; }

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
        bool rayReconstructionSettingsFailed = false; // its optimal settings failed: graphs use DLSS until a retry works
        uint64_t rayReconstructionSettingsFailures = 0;
        double rayReconstructionRetrySeconds = 0.0; // Until the next retry, while it is failing
        // Configurations (M5 step 3): the attached graphs' and the latest prepared one.
        struct Configuration {
            uint64_t serial = 0;
            uint32_t viewport = 0;
            UpscalingMode mode = UpscalingMode::None;
            DirectX::XMUINT2 render{};
            DirectX::XMUINT2 output{};
            bool prepared = false;
            bool allocatedAhead = false;
            double prepareMs = 0.0;
        };
        Configuration currentConfiguration;
        Configuration latestConfiguration;
        uint64_t configurationsPrepared = 0;
        uint64_t configurationsRetired = 0;
        uint64_t configurationsLive = 0;
        double lastAttachWaitMs = 0.0; // how long MakeCurrent waited for a preparation
    };
    Status GetStatus() const;

    void SetUpscalingMode(UpscalingMode mode) { m_upscalingMode = mode; }
    void SetUpscalingQualityMode(UpscaleQualityMode mode) { m_upscaleQualityMode = mode; }

private:
    friend class UpscalerConfiguration;
    UpscalingManager() = default;
    void SyncSettingsFromSettingsManager();
    // The preparation worker: one thread, in request order, skipping configurations already gone.
    void RunPreparationWorker();
    void PrepareFeature(UpscalerConfiguration::Feature& feature);
    // Frees a feature's viewport and FSR context: at once (FreeNow, the worker's own, never used by a graph) or at the next
    // CollectRetired (Retire, from a configuration's destructor).
    void FreeNow(uint32_t viewport, ffx::Context fsrContext);
    void Retire(uint32_t viewport, ffx::Context fsrContext);
	bool EvaluateDLSS(const UpscalerConfiguration& configuration, rhi::CommandList& commandList, const Components::Camera* camera,
		uint64_t frameNumber, const UpscalingInputs& inputs, bool rayReconstruction);
    void EvaluateFSR3(const UpscalerConfiguration& configuration, rhi::CommandList& commandList, const Components::Camera* camera,
        double elapsedSeconds, const UpscalingInputs& inputs);
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
	bool m_dlssSupported = false;
	bool m_rayReconstructionSupported = false;
	bool m_rayReconstructionGuidesAvailable = false;
    // Configurations (render thread, except the retire list and the counters).
    mutable std::mutex m_configurationMutex;
    std::shared_ptr<UpscalerConfiguration> m_latest;
    std::shared_ptr<const UpscalerConfiguration> m_current;
    uint64_t m_nextSerial = 1;
    uint32_t m_nextViewport = 1; // Every configuration takes a new Streamline viewport, so none is reused while a graph holds it.
    struct RetiredConfiguration { uint32_t viewport = 0; ffx::Context fsrContext = nullptr; };
    std::vector<RetiredConfiguration> m_retired;
    std::atomic<uint64_t> m_configurationsPrepared = 0;
    std::atomic<uint64_t> m_configurationsRetired = 0;
    std::atomic<uint64_t> m_configurationsLive = 0;
    std::atomic<bool> m_shutDown = false;
    double m_lastAttachWaitMs = 0.0;
    std::thread m_preparationWorker;
    std::mutex m_preparationMutex;
    std::condition_variable m_preparationChanged;
    std::vector<std::shared_ptr<UpscalerConfiguration::Feature>> m_preparationQueue;
    bool m_stopPreparation = false;
    // Streamline history is tied to the input/output dimensions of a configuration: its first evaluation resets it. This is the
    // request of RequestHistoryReset and of evaluations that resume after failures.
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
    // Ray Reconstruction's optimal settings: a failure falls back to DLSS and retries, rather than giving up for good.
    std::atomic<bool> m_rayReconstructionSettingsFailed = false;
    std::atomic<uint64_t> m_rayReconstructionSettingsFailures = 0;
    std::atomic<int64_t> m_reconfigureAtNs = 0; // steady_clock; 0 when nothing is scheduled
    std::atomic<bool> m_reconfigureNow = false;
    double m_rayReconstructionRetryDelaySeconds = 2.0;
    std::atomic<uint32_t> m_simulatedOutOfMemoryFrames = 0;
    std::atomic<uint32_t> m_simulatedSettingsFailures = 0;
    // Vendor upscalers own mutable history/context state: recording may run on workers for different frames, so evaluation
    // is serialized in frame order.
    std::mutex m_evaluateMutex;
    // Streamline is not thread safe: its calls (DLSS evaluation, taken after m_evaluateMutex; the queries, the preparations'
    // allocations and the releases) take this one. Streamline also serializes them itself: a DLSS evaluation waits for an
    // allocation in progress (35-40 ms), whatever the lock.
    std::mutex m_streamlineMutex;
};

inline UpscalingManager& UpscalingManager::GetInstance() {
    static UpscalingManager instance;
    return instance;
}
