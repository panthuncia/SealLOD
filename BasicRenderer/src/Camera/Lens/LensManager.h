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
namespace br::render {
class RendererStateRequestService;
class VersionedBufferFamily;
}

namespace br::lens {

// lensMode.
enum class LensMode : uint32_t { Pinhole = 0, Explicit = 1, Polynomial = 2 };
// lensFieldMode: what a camera field wider than the lens covers does (lensDataLayout.h's LENS_FLAG_FIELD_REMAP).
enum class LensFieldMode : uint32_t { Remap = 0, Physical = 1 };

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
    double focusDistanceM = 0.0;
    double sensorShiftMm = 0.0;     // focus shift added to the infinity sensor plane
    double sensorZ = 0.0;           // focused sensor plane (mm from the front vertex)
    double maxSensorRadiusMm = 0.0; // pupil bins span [0, this]; sensor points past it are black
    double coveredSensorRadiusMm = 0.0; // the last pupil bin through which any ray passes
    double coveredFieldRad = 0.0;   // that radius's chief-ray field angle
    double imageCircleMm = 0.0;     // design half-diagonal, from the prescription
    uint32_t fieldMode = 0;         // LensFieldMode
    double remapRadiusMm = 0.0;     // the image radius remap fits the frame's half-diagonal to
    double remapFieldRad = 0.0;     // its chief-ray field angle (the shader's covered field)
    double normalization = 0.0;     // K: the centre pixel's estimator expectation at the design wavelength
    double stopScale = 1.0;
    bool monochromatic = true;
    double buildMs = 0.0;
    uint32_t pupilNodes = 0;
    std::string pupilSamplingJson;  // the exact pupil's discs and its acceptance against exact traces
    double distortionStepRad = 0.0;
    std::vector<double> distortionRadius; // chief-ray image radius (mm) at field angles i * step, at the focused plane
    double ImageRadiusAt(double angle) const;
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
    // The polynomial's generated include (br::shaderstore::SetGeneratedSource): what pipelines that trace camera rays define
    // LENS_POLYNOMIAL_INCLUDE as (a quoted file name), empty when none.
    std::wstring polynomialInclude;
    bool Active() const { return buffer != nullptr; }
};

// The physical camera lens for the path tracer (SARP's docs/lens_simulation.md). It reacts to the lens* settings: a worker thread
// loads the prescription and computes the focus shift, the exact pupil, distortion table and normalization with LensCore
// (and for the polynomial lens, its generated shader code), and posts the packed data as a versioned buffer to the state graph. Once that version is GPU ready it becomes the
// published state, which the path tracer reads with one atomic load per frame (Builtin::RayTracing::LensData). Until then
// it renders the previous state (pinhole at first).
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
        float focusDistance = 3.0f;
        bool monochromatic = true;
        float stopScale = 1.0f;
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
    std::atomic<float> m_cameraFov{ 0.0f };
    std::atomic<float> m_cameraAspect{ 1.0f };
};

} // namespace br::lens
