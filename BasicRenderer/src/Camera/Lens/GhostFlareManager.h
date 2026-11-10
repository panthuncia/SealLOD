#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
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
namespace lens { struct LensPrescription; }

namespace br::lens {

class LensManager;

// The names the flare shaders read (lensGhostLayout.h).
inline constexpr std::string_view kLensFlareGhostsName = "Builtin::LensFlare::Ghosts";
inline constexpr std::string_view kLensFlareSourcesName = "Builtin::LensFlare::Sources";

// One ghost the GPU traces: reflect at surface a, then at b < a (LensCore GhostPath), and what LensCore's ranking gave it.
struct GhostFlarePath {
    uint32_t a = 0;
    uint32_t b = 0;
    std::string code;          // "R<a>R<b>"
    double energy = 0.0;       // relative to transmission, mean over the F, d and C lines (LensBake rank's measure)
    std::vector<std::array<float, 3>> boxes; // per field node: x min, x max, y max on the front plane, canonical frame
};

// The published ghost data: what the GPU buffer holds and what it was built from.
struct GhostFlarePublished {
    uint64_t revision = 0;
    uint64_t lensRevision = 0; // the LensManager revision it was built for (0: none, no ghosts)
    std::shared_ptr<const void> version; // the PublishedGpuBufferVersion: its lease keeps the backing
    std::shared_ptr<org::Resource> buffer; // null: the fallback (no ghosts)
    std::shared_ptr<const ::lens::LensPrescription> prescription;
    std::vector<GhostFlarePath> ghosts;
    uint32_t pathsRanked = 0;
    double stopScale = 1.0;
    double maxFieldRad = 0.0;
    double fieldStepRad = 0.0;
    double rankMs = 0.0;
    double boxesMs = 0.0;
    double buildMs = 0.0;
    std::string lensName;
};

// What the flare passes use this frame, chosen once per frame (FrameConfig) so that the passes agree on the layout.
struct GhostFlareFrame {
    bool active = false;          // a lens is published and its ghosts are built
    uint32_t sourceSlots = 0;     // 1 + max lights
    uint32_t ghostSlots = 0;
    uint32_t wavelengthSlots = 0; // 1 for a monochromatic lens
    uint32_t grid = 0;
    float intensity = 1.0f;
    uint64_t selfTest = 0;        // the sequence to stamp, 0 none
    bool testSource = false;
    std::array<float, 3> testDirection{ 0.0f, 0.0f, -1.0f }; // view space, toward the source
    float testDistance = 0.0f;    // metres, 0 at infinity
    std::array<float, 3> testIrradiance{};
    uint32_t InstanceCount() const { return sourceSlots * ghostSlots * wavelengthSlots; }
};

// Physically based ghost flares for the physical camera lens (SARP's docs/lens_simulation.md "Ghost flares"; Hullin et al.
// 2011). When LensManager publishes a lens, a worker ranks the prescription's two-reflection ghost paths with LensCore, keeps
// the lensFlareMaxGhosts brightest, finds each one's box on the front plane over the field, and posts that as a versioned
// buffer on the state graph (published once GPU ready, as the lens buffer is). Each frame the flare passes
// (RenderPasses/LensFlarePasses.h) pick the sources, trace a lattice of rays per source, ghost and wavelength forward through
// the lens, and draw the lattice's cells additively; the result is added to the upscaled HDR colour.
class GhostFlareManager : public org::IResourceProvider {
public:
    GhostFlareManager(br::render::RendererStateRequestService* requests, std::shared_ptr<org::runtime::IUploadService> uploads,
        std::shared_ptr<LensManager> lens);
    ~GhostFlareManager() override;
    GhostFlareManager(const GhostFlareManager&) = delete;
    GhostFlareManager& operator=(const GhostFlareManager&) = delete;

    void Shutdown();

    // Any thread. Never null.
    std::shared_ptr<const GhostFlarePublished> Published() const;
    const std::shared_ptr<LensManager>& Lens() const { return m_lens; }

    // The render thread, once a frame: notices a new lens revision (O(1)) and reads back a pending self-test.
    void OnFrame(org::RenderGraph* graph);

    // The flare passes, from Prepare: this frame's configuration (the first call of a frame decides it).
    GhostFlareFrame FrameConfig(uint64_t frameNumber);

    // The vertex buffer for the graph being built (sized by the settings, which rebuild the graph when they change), and the
    // per-frame sources buffer.
    std::shared_ptr<org::Buffer> EnsureVertexBuffer();
    const std::shared_ptr<org::Buffer>& SourcesBuffer() const { return m_sources; }
    // The settings the graph is built with: grid, max ghosts, wavelengths, max lights.
    static std::array<uint32_t, 4> GraphSettings();

    // lens.flare.get, lens.flare.set, lens.flare.selftest, lens.flare.selftest.status. Throws on a bad request.
    std::string HandleControl(const std::string& command, const std::string& requestJson);

    std::shared_ptr<org::Resource> ProvideResource(org::ResourceIdentifier const& key) override;
    std::vector<org::ResourceIdentifier> GetSupportedKeys() override;
    std::shared_ptr<org::IResourceResolver> ProvideResolver(org::ResourceIdentifier const& key) override;
    std::vector<org::ResourceIdentifier> GetSupportedResolverKeys() override;

    struct Shared;

private:
    struct Desired {
        uint64_t lensRevision = 0;
        uint32_t maxGhosts = 8;
        uint64_t generation = 0;
    };

    void WorkerLoop();
    void Build(const Desired& desired);

    std::shared_ptr<Shared> m_shared;
    br::render::RendererStateRequestService* m_requests = nullptr;
    std::shared_ptr<org::runtime::IUploadService> m_uploads;
    std::shared_ptr<LensManager> m_lens;
    std::shared_ptr<br::render::VersionedBufferFamily> m_family;
    std::shared_ptr<org::IResourceResolver> m_resolver;
    std::shared_ptr<org::Buffer> m_fallback;
    std::shared_ptr<org::Buffer> m_sources;
    std::shared_ptr<org::Buffer> m_vertices;
    size_t m_vertexRows = 0;

    // The worker's state is guarded by Shared::workMutex and woken by Shared::wake (readback callbacks post self-test captures).
    Desired m_desired;
    uint64_t m_builtGeneration = 0;
    bool m_stop = false;
    std::thread m_worker;
    std::vector<br::extensions::SettingSubscription> m_subscriptions;

    uint64_t m_revision = 0; // worker only
    std::shared_ptr<br::render::ArtifactAwaiter> m_awaiter; // worker only

    // Render thread.
    uint64_t m_seenLensRevision = ~0ull;

    std::mutex m_frameMutex;
    uint64_t m_frameNumber = ~0ull;
    GhostFlareFrame m_frame;
};

} // namespace br::lens
