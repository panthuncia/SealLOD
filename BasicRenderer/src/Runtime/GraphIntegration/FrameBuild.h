#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <flecs.h>

#include "BasicRenderer/Extensions/FrameEpochs.h"
#include "BasicRenderer/Extensions/ProducerPersistentState.h"
#include "Render/RenderGraph/RenderGraphExtension.h"
#include "BasicRenderer/Runtime/Detail/MaterialEvaluationBuildInputs.h"
#include "BasicRenderer/Pipeline/UpscalingTypes.h"
#include "BasicRenderer/Scene/Components.h"
#include "BasicRenderer/Scene/RendererComponents.h"

class SettingsSnapshot;
struct CLodStreamingGraphBinding;
struct PathTraceState;
class VirtualShadowCasterRegistry;
struct UpdateContext;

namespace org {
class Buffer;
class DynamicResource;
class IResourceProvider;
class PixelBuffer;
class Resource;
class ResourceGroup;
struct IRenderGraphExtension;
struct ScheduleTrace;
}

namespace br::pipeline { class PipelineRecipe; }

namespace br::render {

class DebugSceneSnapshotService;
class UpscalingGenerationService;

// The frame graph host's epochs (M4 step 6): Visibility, Shading, Post and Overlay each run in a host epoch of their own (the
// frame epoch's id), each a graph, executed in this order every frame.
constexpr uint32_t kFrameHostEpoch = kVisibilityEpoch;  // the first frame host epoch, Visibility's
// The host epoch a frame epoch runs in: its own (Visibility's for anything that is not a frame epoch).
uint32_t HostEpochOf(uint32_t frameEpoch) noexcept;
// The frame host epochs in execution order: {1, 2, 3, 4}.
const std::vector<uint32_t>& FrameHostEpochs() noexcept;
// The bit of a host epoch in a FrameRevisions epoch mask.
constexpr uint32_t HostEpochBit(uint32_t hostEpoch) noexcept { return 1u << hostEpoch; }
// The host epochs of a FrameRevisions epoch mask, in execution order (all of them for kAllEpochs).
std::vector<uint32_t> HostEpochsOfMask(uint32_t mask);
// The mask of host epochs whose frame epochs are in `frameEpochs`.
uint32_t HostEpochMaskOf(std::initializer_list<uint32_t> frameEpochs) noexcept;
// What ORG tags a pass of that frame epoch with (ExternalPassDesc::epoch): untagged. Each host epoch's graph is built only from
// that epoch's declarations and extensions (EpochTaggedExtension drops or forwards another host epoch's passes), so ORG's own
// filter has nothing to drop. (A graph with tagged passes orders its untagged ones ahead of every epoch.)
constexpr uint32_t OrgPassEpochOf(uint32_t frameEpoch) noexcept { return (void)frameEpoch, kNoFrameEpoch; }

// The objects the frame's epochs share, owned by the renderer and not by a build (docs/frame_graph_composition_plan.md, M4).
// Every epoch's declarations use these same objects, so a resource that crosses epochs is one object in every graph that
// uses it, and ORG can match it by identity (never by name). Objects only: their lifetimes in the frame come from the
// graphs' declarations. Made and replaced on the render thread (Renderer::UpdateFrameInterface); a build reads its own copy
// (FrameBuildInputs::frameInterface).
struct FrameInterface {
    // The primary view's visibility buffer and its deep visibility head pointers (CanonicalSurfaceResources). Made again only
    // for another render resolution or primary view, so a rebuild finds the ones the view has, and the first frame after
    // its attach declares nothing new.
    uint64_t primaryViewID = 0;
    std::shared_ptr<org::PixelBuffer> visibilityBuffer;
    std::shared_ptr<org::PixelBuffer> visibilityHeadPointers;
    // The cross-epoch transients every epoch's graph registers by these ids (DeclareFrameProviders): the canonical surface
    // textures and records, the debug visualization and, with Ray Reconstruction, its guides. Made again only for another
    // render resolution. So rebuilding one epoch makes none of them again and the other epochs keep their graphs, and frame
    // placement matches each by identity across the epochs that use it.
    struct Registration {
        std::string id;
        std::shared_ptr<org::Resource> resource;
    };
    std::vector<Registration> registrations;
    // Shading's inputs that Visibility's passes (or none) write and Shading's read, made with the registrations but registered
    // only when the build's options use them (DeclareFrameProviders): the ray traced sun shadow mask (the CLod ray tracing view
    // traces it in Visibility after material evaluation), stochastic lighting's diffuse and specular radiance, and the VSM
    // flicker telemetry's receiver history. Unused, they hold no memory.
    std::shared_ptr<org::PixelBuffer> sunShadowMask;
    std::shared_ptr<org::PixelBuffer> stochasticDiffuse;
    std::shared_ptr<org::PixelBuffer> stochasticSpecular;
    std::shared_ptr<org::PixelBuffer> vsmReceiverHistory;
    uint32_t width = 0, height = 0;   // the render resolution they were made for
    bool rayReconstructionGuides = false;  // whether the guides are among the registrations
    // The attached graph's CLod streaming binding, which its Visibility epoch made (the opaque variant owns streaming). The
    // CLod variants of other epochs write their streaming feedback into it, so a build that does not declare Visibility
    // takes this one (CLodExtension::BuildStreamingBinding). Set when a build is published.
    std::shared_ptr<CLodStreamingGraphBinding> clodStreaming;

    // The resource objects above, for the epoch interface check.
    std::vector<const org::Resource*> Objects() const;
};

// The renderer's objects of one render and output size (M5 step 5): the core colour targets, the presentation surfaces and, for a
// size the primary view does not have yet, its new depth maps. Made on the render thread when a resolution request is applied
// (Renderer::ApplyResolutionRequest), passed to the build made for that size, and current from that build's attach
// (Renderer::PublishFrameBuild), where the swapchain, the view's depth and the presentation surfaces switch to them. The graphs
// attached before keep theirs until they retire. Persistent ones become versions in M5 step 6.
struct ResolutionObjects {
    DirectX::XMUINT2 render{};
    DirectX::XMUINT2 output{};
    std::shared_ptr<org::PixelBuffer> hdrColor;
    std::shared_ptr<org::PixelBuffer> upscaledHdrColor;
    std::shared_ptr<org::PixelBuffer> dilatedMotionVectors;
    // One per frame in flight, and the dynamic resource the graphs bind as PresentationColor (pointed at the slot's each frame).
    std::vector<std::shared_ptr<org::PixelBuffer>> presentationColor;
    std::shared_ptr<org::DynamicResource> dynamicPresentationColor;
    // The bloom mip chain at the output size (M6 step 3): Post's bloom writes it and Overlay's tonemapping reads it, so every build
    // of this size takes this one. Made by the first build that needs it (MakeFrameBuild); unmaterialized, it holds no memory.
    std::shared_ptr<org::PixelBuffer> bloom;
    // The primary view's depth maps for this render size, until the attach gives them to the view (its id stays). Unset when the
    // view has them already: builds then take the camera's.
    std::optional<Components::DepthMap> primaryDepth;
    uint32_t primaryLodHeight = 0;
    // A present reported a mode change: the swapchain is made again at the attach even at the same size.
    bool recreateSwapchain = false;
};

// What one frame-graph build reads from the renderer, taken on the render thread before the build starts
// (Renderer::MakeFrameBuild). The build's declarations (DeclareFrameProviders, DeclareFramePasses) read these instead of
// live renderer or ECS state, so the build can later run on another thread while the renderer keeps changing
// (docs/frame_graph_composition_plan.md, M3).
struct FrameBuildInputs {
    std::shared_ptr<const SettingsSnapshot> settings;
    // The recipe the graph is built from: a copy, so the render thread may replace the renderer's while the build runs.
    std::shared_ptr<const br::pipeline::PipelineRecipe> recipe;
    // The renderer's resource providers (its managers), copied on the render thread (SnapshotResourceProvider): the build
    // registers these instead of the live managers, whose maps the render thread keeps changing.
    std::vector<std::shared_ptr<org::IResourceProvider>> providers;
    // CLod's geometry slabs (null before the first CLod mesh).
    std::shared_ptr<org::ResourceGroup> clodSlabs;

    // The primary camera.
    uint64_t primaryViewID = 0;
    Components::DepthMap depth;
    std::optional<Components::Camera> camera;
    std::shared_ptr<org::Resource> cameraBuffer;
    std::shared_ptr<org::Resource> cullingCameraBuffer;

    // The renderer's cross-epoch objects when the build started: the primary view's visibility buffer and head pointers (with
    // CanonicalSurfaceResources; attached to the view when the build is published) and the attached streaming binding.
    FrameInterface frameInterface;
    // The update values the first frame after this build's attach is expected to have: the last frame's (published state,
    // poses, settings), with its views as they will be then (the interface's visibility buffer on the primary view). Passes
    // whose declarations follow their Update declare from these at Setup (BuildUpdateContext), so the new graph's first frame
    // finds them declared already. Null before any frame or when the views changed since; dropped when the build finishes.
    std::shared_ptr<const UpdateContext> lastUpdate;

    std::shared_ptr<org::PixelBuffer> brdfLut;
    // The AMD LPM constants (M6 step 3): Post's exposure writes the tonemapping curve into them and Overlay's tonemapping reads it.
    // The renderer's one object, which both epochs' graphs register as "FFX::LPMConstants".
    std::shared_ptr<org::Resource> lpmConstants;

    // The environment the graph samples: the current one's cubemaps, or the blank fallback's.
    std::shared_ptr<org::Resource> environmentCubemap;
    std::shared_ptr<org::Resource> environmentPrefilteredCubemap;
    // Whether an environment is current, for the skybox. A flag, not the environment: the render thread may replace it
    // while the build runs.
    bool hasEnvironment = false;

    // Ray tracing: whether the BVH is initialized and its resources are registered, and what traces.
    bool rayTracingReady = false;
    bool stochasticLightingActive = false;
    bool rayTracedSunShadow = false;
    bool rayTracedReflections = false;
    bool screenSpaceReflections = false;
    bool lensFlareRayQuery = false;
    // The directional virtual shadow map's renderer-owned resources when the build makes the shadow variant (null otherwise):
    // the variant binds them in Visibility's graph, and the other epochs' graphs register them for their receivers (M5 step 8).
    std::shared_ptr<const ProducerPersistentState::DirectionalVsmResources> directionalVsm;

    UpscalingMode upscalingMode = UpscalingMode::None;
    std::shared_ptr<UpscalingGenerationService> upscaling;
    // The render and output sizes the graphs are built for: the latest upscaler configuration's when the build started (M5 step
    // 4). The build's settings snapshot has these as renderResolution and outputResolution, whatever the live settings hold, and
    // per-frame values follow them from the attach (Renderer::AttachedResolution).
    DirectX::XMUINT2 renderResolution{};
    DirectX::XMUINT2 outputResolution{};
    // The objects of those sizes (ResolutionObjects): the build registers these, and its attach makes them current.
    std::shared_ptr<ResolutionObjects> resolution;

    std::shared_ptr<DebugSceneSnapshotService> debugSnapshots;
    bool drawBoundingSpheres = false;

    // The objects whose transforms changed this frame, for virtual shadow page invalidation. Made once on the render
    // thread: making a flecs query writes the world.
    std::shared_ptr<flecs::query<const Components::ObjectDrawInfo>> vsmTransformChangedQuery;

    // The frame graph's extensions, in build order: the ids the host has a factory for (Renderer::RegisterFrameHostExtensions).
    // The list changes only when the recipe's extension ids do, so a recipe change does not change the host's configuration.
    std::vector<std::string> extensionIds;
    // What each id makes in this build. MakeFrameBuild decides it on the render thread (which CLod variants, their options,
    // stochastic lighting's options); the host's factory calls it during the build, so every build gets new instances that
    // live and die with its graph. An id with no maker makes no extension in this build.
    using ExtensionMaker = std::function<std::unique_ptr<org::IRenderGraphExtension>()>;
    std::unordered_map<std::string, ExtensionMaker> extensionMakers;
    // Each id's frame epoch: its passes' unless a pass names its own (EpochTaggedExtension). kNoFrameEpoch: each pass takes
    // the epoch of the pass it follows (readback captures).
    std::unordered_map<std::string, uint32_t> extensionEpochs;
    uint32_t ExtensionEpoch(const std::string& id) const {
        const auto found = extensionEpochs.find(id);
        return found == extensionEpochs.end() ? kVisibilityEpoch : found->second;
    }
    // The frame epoch of each pass the attached graphs have, by name, when the build started: what a readback capture's anchor
    // in an epoch this build does not declare (or declares later) belongs to (FramePassEpochs::EpochOf).
    std::shared_ptr<const std::unordered_map<std::string, uint32_t>> knownPassEpochs;
};

// A write to a resource's ECS entity that the build makes, such as the tags of the CLod resource directory that
// diagnostics query. The build records it (WriteResourceEntity) and the renderer makes it on the render thread when the
// build is published: the ECS world is render-thread only, and a resource made on another thread has no entity until
// RendererECSManager::FlushDeferredWorldOperations.
struct ResourceEntityWrite {
    std::shared_ptr<org::Resource> resource;
    std::function<void(flecs::world&, flecs::entity)> write;
    // Set when the write publishes the resource (Components::Resource) under this name. Another resource published
    // earlier under the same name is unpublished then, so a replaced resource leaves the directory.
    std::string directoryKey;
};

// The opaque (visibility buffer) CLod variant's buffers that material evaluation reads. The variant fills these in its
// PrepareForBuild, which runs before the renderer's pass declarations (DeclareFramePasses).
struct CLodPrimaryBuffers {
    std::shared_ptr<org::Buffer> visibleClusters;
    std::shared_ptr<org::Buffer> visibleClusterCounter;
    std::shared_ptr<org::Buffer> visibleClusterTransformIndices;
    uint32_t visibleClusterCapacity = 0;
    // Null unless Reyes tessellation is on for the variant.
    std::shared_ptr<org::Buffer> reyesDiceQueue;
    std::shared_ptr<org::Buffer> reyesTessTableConfigs;
    std::shared_ptr<org::Buffer> reyesTessTableVertices;
    std::shared_ptr<org::Buffer> reyesTessTableTriangles;
};

// What one frame epoch's declarations produced for the renderer, published on the render thread after the build
// (Renderer::PublishFrameBuild). A build keeps one per frame epoch (FrameBuild::outputs). Each field belongs to the epoch
// named on it and is set only in that epoch's outputs, so an epoch that is not rebuilt can keep what it published before.
struct FrameBuildOutputs {
    // Visibility: material binning and evaluation's inputs (they read the opaque CLod variant's buffers, clodPrimary).
    MaterialEvaluationBuildInputs materialEvaluationInputs;
    // Shading: the path tracer's state, which only its own passes and the renderer read.
    std::shared_ptr<PathTraceState> pathTraceState;
    // Visibility: the interface's visibility buffer, when the canonical surface was declared (attached to the primary view).
    std::shared_ptr<org::PixelBuffer> visibilityBuffer;
    // Visibility. Terrain RVT's persistent resources: a copy of the renderer's on input, with what the build added.
    std::unordered_map<std::string, std::shared_ptr<org::Resource>> terrainRvtResources;
    bool terrainRvt = false;
    // ProducerPersistentState::terrainRvtGeneration when the copy was taken. An invalidation since then (a terrain RVT
    // setting changed while the build ran) keeps the copy from being published.
    uint64_t terrainRvtGeneration = 0;
    // Visibility: the opaque CLod variant's buffers.
    CLodPrimaryBuffers clodPrimary;
    // Visibility: what CLod streaming uses of the graph (made by the first CLod variant that needs it). The renderer attaches
    // it to the streaming service after the build; the build itself leaves the running service alone.
    std::shared_ptr<CLodStreamingGraphBinding> clodStreaming;
    // Visibility: the virtual shadow casters of this build's graph (SARP grass), registered as their extensions are made. The shadow
    // CLod variant and its setup pass read it. It points at extension instances of this build's graph, so only that graph
    // may use it. Made by MakeFrameBuild, over the renderer's persistent invalidation queue.
    std::shared_ptr<VirtualShadowCasterRegistry> virtualShadowCasters;
    // Every epoch: the ECS writes of the epoch's declarations and extensions (WriteResourceEntity), in the order they were
    // made. PublishFrameBuild makes them and clears the lists; a failed build's are dropped.
    std::vector<ResourceEntityWrite> resourceEntityWrites;
};

// The frame epoch of each pass a build declared, by pass name, for the epoch interface check. The renderer's declarations
// tag their passes (Renderer::DeclareFramePasses); each extension's are tagged as it gathers them (EpochTaggedExtension).
// Shared with the graph's extensions, which may gather again after the build (a structural build at a later preparation).
struct FramePassEpochs {
    void Tag(const std::string& pass, uint32_t epoch);
    // A pass that takes the epoch of the pass it follows (a readback capture after its anchor).
    void Follows(const std::string& pass, const std::string& anchor);
    // The pass's epoch; kNoFrameEpoch for a pass nothing tagged. A follower takes its anchor's. A follower whose anchor is not
    // in the graph (ORG then places it unanchored) takes Visibility, the frame's first epoch, and sets *dangling.
    uint32_t EpochOf(const std::string& pass, bool* dangling = nullptr) const;
    // The epoch of a pass this build tagged, else of an attached graph's pass (knownPassEpochs); kNoFrameEpoch for neither. A
    // follower's anchor is not followed.
    uint32_t KnownEpochOf(const std::string& pass) const;
    // The tagged passes' epochs, for the next builds' knownPassEpochs.
    std::unordered_map<std::string, uint32_t> Tagged() const;
    // What EpochOf answers for a pass this build did not tag (the attached graphs' passes): FrameBuildInputs::knownPassEpochs.
    void SetKnown(std::shared_ptr<const std::unordered_map<std::string, uint32_t>> known);

private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, uint32_t> m_epochs;
    std::unordered_map<std::string, std::string> m_anchors;
    std::shared_ptr<const std::unordered_map<std::string, uint32_t>> m_known;
};

// Extension passes of one host epoch's graph that belong to a later host epoch (a Visibility extension's debug view, which
// draws in Overlay). The earlier graph's build hands them to the later one's (EpochTaggedExtension), which gathers them
// (the renderer's pass declaration, DeclarationGraphExtension). So a build that declares the earlier epoch also declares the
// later one (Renderer::StartFrameBuild), and the pass objects live in the graph that runs them.
struct ForwardedPasses {
    std::mutex mutex;
    std::unordered_map<uint32_t, std::vector<org::ExternalPassDesc>> byHostEpoch;
    uint32_t hostEpochMask = 0;  // the host epochs passes were forwarded to (HostEpochBit)
};

struct FrameBuild {
    FrameBuildInputs inputs;
    // Per frame epoch (FrameEpochIndex): what its declarations produced.
    std::array<FrameBuildOutputs, kFrameEpochCount> outputs;
    // The frame epochs this build declares: those of its host epochs.
    std::vector<uint32_t> epochs{ std::begin(kFrameEpochs), std::end(kFrameEpochs) };
    // The host epochs it builds a graph for, in execution order.
    std::vector<uint32_t> hostEpochs;
    std::shared_ptr<FramePassEpochs> passEpochs = std::make_shared<FramePassEpochs>();
    std::shared_ptr<ForwardedPasses> forwarded = std::make_shared<ForwardedPasses>();

    FrameBuildOutputs& Outputs(uint32_t epoch) { return outputs.at(FrameEpochIndex(epoch)); }
    const FrameBuildOutputs& Outputs(uint32_t epoch) const { return outputs.at(FrameEpochIndex(epoch)); }
    bool Declares(uint32_t epoch) const noexcept;
    // Whether the build declares the frame epoch in the graph of that host epoch (each graph declares its own epochs).
    bool DeclaresIn(uint32_t epoch, uint32_t hostEpoch) const noexcept { return Declares(epoch) && HostEpochOf(epoch) == hostEpoch; }
    // Sets hostEpochs and the frame epochs they hold.
    void SetHostEpochs(std::vector<uint32_t> hosts);
};

// The build in progress on this thread, or null. Extensions reach their build's inputs and outputs through it.
FrameBuild* CurrentFrameBuild() noexcept;

// The frame epoch whose declarations run on this thread (a DeclareFramePasses technique, an extension's calls through
// EpochTaggedExtension), or kNoFrameEpoch. WriteResourceEntity records into that epoch's outputs.
uint32_t CurrentDeclarationEpoch() noexcept;

class ScopedDeclarationEpoch {
public:
    explicit ScopedDeclarationEpoch(uint32_t epoch) noexcept;
    ~ScopedDeclarationEpoch();
    ScopedDeclarationEpoch(const ScopedDeclarationEpoch&) = delete;
    ScopedDeclarationEpoch& operator=(const ScopedDeclarationEpoch&) = delete;

private:
    uint32_t m_previous = kNoFrameEpoch;
};

// The epoch interface check (docs/frame_graph_composition_plan.md, M4), from a built graph's schedule
// (RenderGraph::TraceSelectedSchedule) and the build's pass epochs. Resources are matched by identity, never by name.
// Building thread.
struct FrameEpochCheck {
    // Passes with no epoch: a build that has any fails.
    std::vector<std::string> untagged;
    // Followers whose anchor is not in the graph (a readback capture after a pass this recipe lacks), taken as Visibility's.
    std::vector<std::string> danglingFollowers;
    // A resource that passes of two epochs use and that is neither one of the interface's objects nor persistent: a
    // transient whose memory the graph's alias plan gives it, and which a split frame has to carry between its epochs.
    struct Crossing {
        std::string name;
        uint64_t objectId = 0;
        uint64_t bytes = 0;
        std::vector<uint32_t> epochs;  // the epochs whose passes use it, in frame order
        uint32_t firstWriter = kNoFrameEpoch;  // the epoch of its first writer in schedule order
        bool readBeforeWritten = false;        // its first use in the frame is a read
    };
    std::vector<Crossing> crossing;
    uint64_t crossingBytes = 0;
    // What crosses through the interface's objects, and the persistent resources that cross.
    size_t interfaceResources = 0;
    uint64_t interfaceBytes = 0;
    size_t persistentResources = 0;
    std::array<size_t, kFrameEpochCount> passes{};
    size_t resources = 0;

    std::string Describe(size_t most = 80) const;
};
FrameEpochCheck CheckFrameEpochs(const org::ScheduleTrace& trace, const FrameBuild& build);

// Makes `build` this thread's current build for the scope's lifetime.
class ScopedFrameBuild {
public:
    explicit ScopedFrameBuild(FrameBuild& build) noexcept;
    ~ScopedFrameBuild();
    ScopedFrameBuild(const ScopedFrameBuild&) = delete;
    ScopedFrameBuild& operator=(const ScopedFrameBuild&) = delete;

private:
    FrameBuild* m_previous = nullptr;
};

// A copy of a provider's keys, resources and resolvers, taken now. ORG asks a registered provider for every key it lists
// at registration (RenderGraph::RegisterProvider), so registering the copy gives the graph what registering the provider
// would have given it at that moment, on any thread. Render thread (the provider's own thread).
std::shared_ptr<org::IResourceProvider> SnapshotResourceProvider(org::IResourceProvider& provider);

// Writes to a resource's ECS entity. Inside a build the write is recorded in its outputs and made when the build is
// published; outside one it is made now (render thread only). A non-empty `directoryKey` marks a write that publishes the
// resource (see ResourceEntityWrite).
void WriteResourceEntity(std::shared_ptr<org::Resource> resource, std::function<void(flecs::world&, flecs::entity)> write,
    std::string directoryKey = {});

} // namespace br::render
