#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Interfaces/IResourceProvider.h"
#include "Interfaces/IResourceResolver.h"
#include <BasicRenderer/Extensions/SettingAccess.h>
#include <BasicRenderer/Streaming/ArtifactTypes.h>

namespace org {
class Buffer;
class Resource;
class RenderGraph;
}
namespace org::runtime { class IUploadService; }
namespace lens { struct LensPrescription; }
namespace br::render {
class RendererStateRequestService;
class VersionedBufferFamily;
}

namespace br::lens {

// lensMode.
enum class LensMode : uint32_t { Pinhole = 0, Explicit = 1, Polynomial = 2 };
// lensFieldMode: what a camera field wider than the lens covers does (lensDataLayout.h's LENS_FLAG_FIELD_REMAP).
enum class LensFieldMode : uint32_t { Remap = 0, Physical = 1 };
// lensFocusMode: lensDataLayout.h's LENS_FOCUS_MODE_*.
enum class LensFocusMode : uint32_t { Manual = 0, Auto = 1 };

// The CPU side of a polynomial lens (LensCore types), for the self-test: the polynomial the GPU evaluates and its domain.
struct LensPolynomialReference;

// What lens.get reports, from the CPU reference (LensCore) at build time.
struct LensInfo {
    std::string name;
    std::filesystem::path path;
    uint64_t sourceHash = 0;
    uint32_t requestedMode = 0;
    uint32_t activeMode = 0;        // what the GPU traces: explicit when polynomial was asked for and its .brlens failed
    std::filesystem::path polynomialPath;
    std::string polynomialError;    // why the polynomial isn't used, when it was asked for
    uint32_t polynomialDegree = 0;  // the one in use
    uint64_t polynomialKey = 0;     // the generated code's hash (lensDataLayout.h header row 6)
    double polynomialSensorZMin = 0.0; // the fitted sensor-plane range (focus from infinity to the fit's minimum)
    double polynomialSensorZMax = 0.0;
    double polynomialSensorRadius = 0.0;
    bool focusClampedToPolynomialDomain = false;
    std::string polynomialJson;     // its fit accuracy (from the .brlens) and validity against exact transport at this build
    double focalLengthMm = 0.0;     // paraxial EFL at the d line
    double fNumber = 0.0;           // at infinity focus, with the stop scale
    double wideOpenFNumber = 0.0;   // the same wide open (stop scale 1)
    // The focus range (per-frame focus): focus nodes uniform in the focus shift from shiftMin to shiftMax, added to the
    // prescription's (infinity focus) sensor plane. Focus outside it clamps.
    double baseSensorZ = 0.0;
    double shiftMinMm = 0.0;
    double shiftMaxMm = 0.0;
    double nearFocusMm = 0.0;       // the near limit the range was built for (a polynomial's fitted range may cut it short)
    struct FocusNode {
        double shiftMm = 0.0;
        double sensorZ = 0.0;
        double distanceMm = 0.0;    // infinity for shift 0
        double normalization = 0.0; // K at this focus
        double remapFieldRad = 0.0; // the covered field at this focus
    };
    std::vector<FocusNode> focusNodes;
    // K at LENS_NORMALIZATION_PER_INTERVAL focus coordinates per focus interval (lensDataLayout.h's normalization table).
    std::vector<double> normalizationTable;
    double maxSensorRadiusMm = 0.0; // pupil bins span [0, this]; sensor points past it are black
    double coveredSensorRadiusMm = 0.0; // the last pupil bin through which any ray passes
    double coveredFieldRad = 0.0;   // that radius's chief-ray field angle
    double imageCircleMm = 0.0;     // design half-diagonal, from the prescription
    uint32_t fieldMode = 0;         // LensFieldMode
    double remapRadiusMm = 0.0;     // the image radius remap fits the frame's half-diagonal to
    double stopScale = 1.0;
    bool monochromatic = true;
    double buildMs = 0.0;
    uint32_t pupilNodes = 0;
    std::string pupilSamplingJson;  // the exact pupil's discs and its acceptance against exact traces
    double distortionStepRad = 0.0;
    // Per focus node, the chief-ray image radius (mm) at field angles i * step, at that node's plane.
    std::vector<std::vector<double>> distortionRadius;

    // The focus coordinate (0 .. nodes - 1) of a focus shift, clamped to the range; the focus shift a distance (mm) focuses at,
    // clamped; and the interpolated per-focus values, as the shader interpolates them.
    double FocusCoordinate(double shiftMm) const;
    double ImageRadiusAt(double angle, double focusCoordinate) const;
    double RemapFieldAt(double focusCoordinate) const;
    double NormalizationAt(double focusCoordinate) const;
};

// One self-test ray (sensor side, lens millimetres) and what LensCore's TraceSequential made of it.
struct LensSelfTestRay {
    std::array<double, 3> origin{};
    std::array<double, 3> direction{};
    double lambdaNm = 0.0;
    uint32_t status = 0;
    double edgeMarginMm = 0.0; // the closest any hit came to a clear aperture's or the stop's edge
    std::array<double, 3> exitOrigin{};
    std::array<double, 3> exitDirection{};
    double throughput = 0.0;
};

// A published lens: the GPU buffer (null for the pinhole camera) and what was computed for it.
struct LensPublishedState {
    uint64_t revision = 0;
    uint64_t stateHash = 0; // 0 for the pinhole camera; fed to pathTracingCameraModelHash
    std::shared_ptr<const void> version; // the PublishedGpuBufferVersion: its lease keeps the backing
    std::shared_ptr<org::Resource> buffer;
    std::shared_ptr<const LensInfo> info;
    std::shared_ptr<const std::vector<LensSelfTestRay>> selfTest;
    std::shared_ptr<const LensPolynomialReference> polynomial; // null unless the GPU is to use the polynomial
    // The prescription the buffer was built from (LensCore), for other lens features (ghost flares) and focus conversions. Null
    // for the pinhole camera. A new revision is a new build: a lens file, mode, f-number, wavelength or field mode change, never
    // a focus change.
    std::shared_ptr<const ::lens::LensPrescription> prescription;
    // The polynomial's generated include (br::shaderstore::SetGeneratedSource): what pipelines that trace camera rays define
    // LENS_POLYNOMIAL_INCLUDE as (a quoted file name), empty when none.
    std::wstring polynomialInclude;
    bool Active() const { return buffer != nullptr; }
};

// What LensFocusPass and LensAutofocusPass push each frame (lensFocus.hlsl). From the settings and the published lens.
struct LensFocusParameters {
    bool active = false;          // a lens is published: the passes dispatch
    uint32_t mode = 0;            // LensFocusMode
    float manualShiftMm = 0.0f;   // lensFocusDistance's focus shift, clamped to the range
    float rate = 8.0f;            // lensAutofocusSpeed: the spring's rate (1/s)
    float deadBandMm = 0.0f;      // autofocus holds still while the measured target is within this of the latched one
    float epsilonMm = 1.0e-6f;    // a focus move smaller than this doesn't restart accumulation
    uint32_t identity = 0;        // the prescription: a change re-initializes the focus at the manual distance
    float pointX = 0.0f, pointY = 0.0f; // lensAutofocusPoint (NDC)
    float region = 0.02f;         // lensAutofocusRegion (fraction of the frame height)
};

// The focus state the GPU last reported (an asynchronous readback of Builtin::RayTracing::LensFocus), for lens.get and the menu.
struct LensFocusReport {
    bool valid = false;
    uint64_t revision = 0;        // the lens revision it was read under
    uint32_t mode = 0;
    double shiftMm = 0.0, targetShiftMm = 0.0, velocityMmPerS = 0.0;
    double focusDistanceM = 0.0;  // infinity for shift 0
    double targetDistanceM = 0.0; // the latched autofocus target's distance
    double measuredDistanceM = 0.0; // the autofocus pass's last measurement (0: none)
    double measuredValidFraction = 0.0;
    uint32_t measuredSamples = 0;
    uint32_t epoch = 0;           // focus moves since the focus state was initialized
    bool settled = false;         // not moving
};

// What the menu shows of the lens (LensManager::Status): the f-number range and the last focus report.
struct LensStatus {
    bool active = false;
    double wideOpenFNumber = 0.0;
    double fNumber = 0.0;
    double nearFocusM = 0.0;
    LensFocusReport focus;
};

// The physical camera lens for the path tracer (SARP's docs/lens_simulation.md). It reacts to the lens* settings: a worker thread
// loads the prescription and computes, with LensCore, every focus-dependent table (exact pupil, distortion table, normalization,
// polynomial transmission) at a set of focus nodes, and the rest once (and for the polynomial lens, its generated shader code),
// and posts the packed data as a versioned buffer to the state graph. Once that version is GPU ready it becomes the published
// state, which the path tracer reads with one atomic load per frame (Builtin::RayTracing::LensData). Until then it renders the
// previous state (pinhole at first). Focus is per frame and never rebuilds: LensFocusPass and LensAutofocusPass keep it in
// Builtin::RayTracing::LensFocus, a small GPU buffer the camera rays interpolate the focus nodes with.
class LensManager : public org::IResourceProvider {
public:
    LensManager(br::render::RendererStateRequestService* requests, std::shared_ptr<org::runtime::IUploadService> uploads);
    ~LensManager() override;
    LensManager(const LensManager&) = delete;
    LensManager& operator=(const LensManager&) = delete;

    // Stops the worker and releases the state-graph wait. The renderer calls it before the graph goes; passes may still hold
    // the manager after it. Idempotent.
    void Shutdown();

    // Any thread. Never null.
    std::shared_ptr<const LensPublishedState> Published() const;

    // The render thread, once a frame: hands the accumulation hash to its setting, and reads back a self-test.
    void OnFrame(org::RenderGraph* graph);
    // The path tracer's camera, for coverage checks and lens.get (radians, width over height). Any thread.
    void NoteCamera(float verticalFov, float aspect);

    // The self-test pass: the sequence to stamp, 0 when none is pending, and its output.
    uint64_t PendingSelfTest() const;
    const std::shared_ptr<org::Buffer>& SelfTestOutput() const { return m_selfTestOutput; }

    // The focus passes: this frame's parameters (any thread; the focus passes call it in Prepare, which also tells OnFrame that
    // the focus state is live and can be read back), and the focus state buffer (Builtin::RayTracing::LensFocus).
    LensFocusParameters FocusParameters() const;
    const std::shared_ptr<org::Buffer>& FocusBuffer() const { return m_focusBuffer; }
    // The word PathTracePass adds to its accumulation key for the focus: the manual focus, or in autofocus the focus epoch the
    // GPU last reported. The GPU restarts accumulation itself the frame the focus moves (LensFocusRestart); this only keeps
    // the CPU's sample count from running ahead of it.
    uint64_t FocusRestartKey() const;
    // The last focus report and lens status, for the menu. Any thread. Never null.
    static std::shared_ptr<const LensStatus> Status();

    // lens.get, lens.set, lens.selftest, lens.selftest.status: a JSON request, a JSON reply. Throws on a bad request.
    std::string HandleControl(const std::string& command, const std::string& requestJson);

    // The directory lens files are read from, and the names (without .lens.json) found there.
    static std::filesystem::path LensDirectory();
    static std::vector<std::string> ListLenses();

    // The defines a pipeline that traces camera rays through this published lens needs: LENS_POLYNOMIAL_INCLUDE for a
    // polynomial lens, none otherwise. Pipelines compiled without them trace the surfaces.
    static std::vector<std::pair<std::wstring, std::wstring>> ShaderDefines(const LensPublishedState& state);

    std::shared_ptr<org::Resource> ProvideResource(org::ResourceIdentifier const& key) override;
    std::vector<org::ResourceIdentifier> GetSupportedKeys() override;
    std::shared_ptr<org::IResourceResolver> ProvideResolver(org::ResourceIdentifier const& key) override;
    std::vector<org::ResourceIdentifier> GetSupportedResolverKeys() override;

    struct Shared;

private:
    struct Desired {
        uint32_t mode = 0;
        std::string file;
        bool monochromatic = true;
        float fNumber = 0.0f; // 0 or below the lens's wide-open f-number: wide open
        uint32_t fieldMode = 0; // LensFieldMode
        uint32_t polynomialDegree = 5;
        uint64_t generation = 0;
    };

    void WorkerLoop();
    void Build(const Desired& desired);
    void MarkDesiredChanged();

    std::shared_ptr<Shared> m_shared;
    br::render::RendererStateRequestService* m_requests = nullptr;
    std::shared_ptr<org::runtime::IUploadService> m_uploads;
    std::shared_ptr<br::render::VersionedBufferFamily> m_family;
    std::shared_ptr<org::IResourceResolver> m_resolver;
    std::shared_ptr<org::Buffer> m_fallback;
    std::shared_ptr<org::Buffer> m_selfTestOutput;
    std::shared_ptr<org::Buffer> m_focusBuffer;

    std::mutex m_mutex;
    std::condition_variable m_wake;
    Desired m_desired;
    uint64_t m_builtGeneration = 0;
    bool m_stop = false;
    std::thread m_worker;
    std::vector<br::extensions::SettingSubscription> m_subscriptions;

    uint64_t m_revision = 0; // worker only
    std::shared_ptr<br::render::ArtifactAwaiter> m_awaiter; // worker only

    // Render thread.
    uint64_t m_lastPublishedHash = ~0ull;
    uint64_t m_coverageLoggedRevision = 0;
    uint64_t m_focusReadbackFrame = 0;
    uint64_t m_focusPassSeen = 0;
    std::atomic<float> m_cameraFov{ 0.0f };
    std::atomic<float> m_cameraAspect{ 1.0f };
    mutable std::atomic<uint64_t> m_focusPassPrepares{ 0 };
};

} // namespace br::lens
