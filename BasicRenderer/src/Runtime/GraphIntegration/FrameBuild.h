#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <flecs.h>

#include "BasicRenderer/Runtime/Detail/MaterialEvaluationBuildInputs.h"
#include "BasicRenderer/Pipeline/UpscalingTypes.h"
#include "BasicRenderer/Scene/Components.h"
#include "BasicRenderer/Scene/RendererComponents.h"

class SettingsSnapshot;
struct CLodStreamingGraphBinding;
struct PathTraceState;
class VirtualShadowCasterRegistry;

namespace org {
class Buffer;
class IResourceProvider;
class PixelBuffer;
class Resource;
class ResourceGroup;
struct IRenderGraphExtension;
}

namespace br::pipeline { class PipelineRecipe; }

namespace br::render {

class DebugSceneSnapshotService;
class UpscalingGenerationService;

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

    std::shared_ptr<org::PixelBuffer> brdfLut;

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

    UpscalingMode upscalingMode = UpscalingMode::None;
    std::shared_ptr<UpscalingGenerationService> upscaling;

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

// What the build produced for the renderer, published on the render thread after it (Renderer::PublishFrameBuild).
struct FrameBuildOutputs {
    MaterialEvaluationBuildInputs materialEvaluationInputs;
    std::shared_ptr<PathTraceState> pathTraceState;
    std::shared_ptr<org::PixelBuffer> visibilityBuffer;
    // Terrain RVT's persistent resources: a copy of the renderer's on input, with what the build added.
    std::unordered_map<std::string, std::shared_ptr<org::Resource>> terrainRvtResources;
    bool terrainRvt = false;
    // ProducerPersistentState::terrainRvtGeneration when the copy was taken. An invalidation since then (a terrain RVT
    // setting changed while the build ran) keeps the copy from being published.
    uint64_t terrainRvtGeneration = 0;
    CLodPrimaryBuffers clodPrimary;
    // What CLod streaming uses of the graph (made by the first CLod variant that needs it). The renderer attaches it to
    // the streaming service after the build; the build itself leaves the running service alone.
    std::shared_ptr<CLodStreamingGraphBinding> clodStreaming;
    // The virtual shadow casters of this build's graph (SARP grass), registered as their extensions are made. The shadow
    // CLod variant and its setup pass read it. It points at extension instances of this build's graph, so only that graph
    // may use it. Made by MakeFrameBuild, over the renderer's persistent invalidation queue.
    std::shared_ptr<VirtualShadowCasterRegistry> virtualShadowCasters;
    // In the order the build made them. PublishFrameBuild makes them and clears the list; a failed build's are dropped.
    std::vector<ResourceEntityWrite> resourceEntityWrites;
};

struct FrameBuild {
    FrameBuildInputs inputs;
    FrameBuildOutputs outputs;
};

// The build in progress on this thread, or null. Extensions reach their build's inputs and outputs through it.
FrameBuild* CurrentFrameBuild() noexcept;

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
