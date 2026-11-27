#include "Hair/HairExtension.h"

#include "HairGpuBuffers.h"

#include "Interfaces/IDynamicDeclaredResources.h"
#include "Render/PassBuilders.h"
#include "Render/RenderGraph/RenderGraph.h"
#include "RenderPasses/Base/RenderPass.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Resources/DynamicResource.h"

#include <BasicRenderer/Extensions/BuiltinResources.h>
#include <BasicRenderer/Extensions/FrameEpochs.h>
#include <BasicRenderer/Extensions/PassRevision.h>
#include <BasicRenderer/Extensions/PipelineDescs.h>
#include <BasicRenderer/Extensions/RecordHelpers.h>
#include <BasicRenderer/Extensions/RenderContext.h>
#include <BasicRenderer/Streaming/VersionedGpuBuffer.h>

namespace br::hair {
namespace {

constexpr std::uint32_t kThreadsPerGroup = 64u;     // every hair compute shader's numthreads
constexpr std::uint32_t kRasterGroupsPerRow = 1024u; // hairRaster.hlsl: a dispatch dimension stays under 65536

std::uint32_t Groups(std::uint32_t threads) { return (threads + kThreadsPerGroup - 1u) / kThreadsPerGroup; }

// What a frame's Record needs from the runtime. The passes take it in Prepare, after every pass's Update.
struct HairPassFrame {
    HairFramePlan plan;
    std::uint32_t outputWidth = 0u; // the debug overlay's target
    std::uint32_t outputHeight = 0u;
    bool operator==(const HairPassFrame&) const = default;
};

void AppendPlanRevision(std::vector<uint64_t>& out, const HairFramePlan& plan)
{
    br::render::AppendRevisionRows(out, plan.instances.data(), plan.instances.size());
    br::render::AppendRevision(out, plan.select);
    br::render::AppendRevision(out, plan.simulate);
    br::render::AppendRevision(out, plan.simulateGroups);
    br::render::AppendRevisionRows(out, plan.levelGroupFirst.data(), plan.levelGroupFirst.size());
    br::render::AppendRevision(out, plan.evaluateGroups);
    br::render::AppendRevision(out, plan.rasterRows);
    br::render::AppendRevision(out, plan.debugView);
    br::render::AppendRevision(out, plan.probe);
}

// A hair pass declares the plugin's buffers, which appear with the first groom and whose visibility target is made
// again at a new resolution. Until then it declares nothing of the plugin's and records nothing.
template<class Derived, class Bindings>
class HairPass : public org::TypedRenderGraphPass<Derived, HairPassFrame, Bindings>, public org::IDynamicDeclaredResources {
public:
    explicit HairPass(std::shared_ptr<HairRuntime> runtime) : m_runtime(std::move(runtime)) {}
    void Initialize() {}

    bool DeclaredResourcesChanged() const override { return m_declared != m_runtime->GpuBuffers(); }

    // What Record bakes in: the frame's plan. Everything else the shaders read from GPU memory.
    void Revision(std::vector<uint64_t>& out) const
    {
        br::render::AppendPipelineRevision(out, m_pipeline);
        AppendPlanRevision(out, m_runtime->FramePlan());
    }

    HairPassFrame Prepare(const Bindings& bindings, const org::PassPrepareContext& preparation) const
    {
        HairPassFrame frame;
        if (!bindings.hasBuffers) return frame;
        frame.plan = m_runtime->FramePlan();
        if (const auto* context = preparation.preparationData ? preparation.preparationData->Get<UpdateContext>() : nullptr) {
            frame.outputWidth = context->outputResolution.x;
            frame.outputHeight = context->outputResolution.y;
        }
        return frame;
    }

protected:
    // The buffers this declaration uses, or null before the first groom and before the first frame's target.
    const HairGpuBuffers* DeclareBuffers()
    {
        m_declared = m_runtime->GpuBuffers();
        return m_declared && m_declared->visibility ? m_declared.get() : nullptr;
    }

    std::shared_ptr<HairRuntime> m_runtime;
    std::shared_ptr<const HairGpuBuffers> m_declared;
    br::PipelineState m_pipeline;
};

struct HairRootBindingBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken instances, rootBindings, rootFrames, counters, frameState;
    bool hasBuffers = false;
};

// One dispatch per active instance, one thread per root (hairRootBinding.hlsl). Its Update is the plugin's per-frame
// work on the render thread: it makes the buffers at the first groom, uploads new grooms and changed instance rows,
// resolves each instance's entity and decides the frame's plan for every hair pass.
class HairRootBindingPass final : public HairPass<HairRootBindingPass, HairRootBindingBindings> {
public:
    using HairPass::HairPass;

    HairRootBindingBindings Declare(org::PassBuilder& builder)
    {
        HairRootBindingBindings bindings{};
        bindings.pipeline = m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairRootBinding.hlsl", L"HairRootBindingCS", "Hair.RootBinding"));
        // Read by name in the shader (ResourceDescriptorIndex): the entities' transforms and the bone palettes.
        builder.ShaderResource(Builtin::PerObjectBuffer, Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo);
        if (const auto* buffers = DeclareBuffers()) {
            bindings.instances = builder.ShaderResource(buffers->instances).View();
            bindings.rootBindings = builder.ShaderResource(buffers->rootBindings).View();
            bindings.frameState = builder.ShaderResource(buffers->frameState).View();
            bindings.rootFrames = builder.UnorderedAccess(buffers->rootFrames).View();
            bindings.counters = builder.UnorderedAccess(buffers->counters).View();
            bindings.hasBuffers = true;
        }
        return bindings;
    }

    void Update(const org::UpdateExecutionContext& execution) override
    {
        const auto* context = execution.hostData ? execution.hostData->Get<UpdateContext>() : nullptr;
        if (!context) return;
        HairFrameInputs inputs;
        inputs.frameNumber = context->frameNumber;
        inputs.renderWidth = context->renderResolution.x;
        inputs.renderHeight = context->renderResolution.y;
        const auto& camera = context->primaryCamera.info.positionWorldSpace;
        inputs.cameraPosition = { camera.x, camera.y, camera.z };
        inputs.deltaSeconds = context->deltaTime;
        inputs.projectionScaleY = DirectX::XMVectorGetY(context->primaryCamera.info.unjitteredProjection.r[1]);
        // The per-object buffer this frame binds, as the renderer published it.
        const auto publishedObjects = context->publishedRendererState
            ? context->publishedRendererState->drawRecords.payload.Get<br::render::PublishedObjectBufferState>()
            : nullptr;
        if (const auto version = publishedObjects ? publishedObjects->FindVersion(br::render::kObjectPerObjectVariant) : nullptr) {
            inputs.objects.rowCount = version->elementCount;
            inputs.objects.rowStride = version->elementStride;
            inputs.objects.image = version->image;
        }
        m_runtime->UpdateFrame(inputs,
            [this](const std::shared_ptr<org::Buffer>& buffer, std::size_t byteOffset, const void* data, std::size_t byteCount) {
                UploadBufferData(data, byteCount, org::runtime::UploadTarget::FromShared(buffer), byteOffset);
            });
    }

    static void Record(const HairRootBindingBindings& bindings, const HairPassFrame& frame, org::PassRecordContext& recording)
    {
        if (!bindings.hasBuffers) return;
        br::render::MiscConstants constants{};
        constants[0] = recording.Index(bindings.instances);
        constants[1] = recording.Index(bindings.rootBindings);
        constants[2] = recording.Index(bindings.rootFrames);
        constants[3] = recording.Index(bindings.counters);
        constants[4] = recording.Index(bindings.frameState);
        for (const auto& instance : frame.plan.instances) {
            constants[5] = instance.instanceIndex;
            br::render::DispatchCompute(recording, bindings.pipeline, constants, Groups(instance.rootCount));
        }
    }
};

struct HairSimulateBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken instances, clusters, strands, rootBindings, rootFrames, points, guideStrandIndices, guidePointRanges,
        guidePoints, colliders, simFrame, simMetrics, guideBindings, simGroups;
    bool hasBuffers = false;
};

// Guide simulation (HairSimulateCS): one dispatch over the frame's simulation thread groups (HairSimGroup), which cover
// the guides of every simulated instance that can have work; one thread per guide. The frame's steps, every control
// and each instance's simulation level are in GPU memory.
class HairSimulatePass final : public HairPass<HairSimulatePass, HairSimulateBindings> {
public:
    using HairPass::HairPass;

    HairSimulateBindings Declare(org::PassBuilder& builder)
    {
        HairSimulateBindings bindings{};
        bindings.pipeline = m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairSimulate.hlsl", L"HairSimulateCS", "Hair.Simulate"));
        // For the colliders: the entities' transforms and the attach bones.
        builder.ShaderResource(Builtin::PerObjectBuffer, Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo);
        if (const auto* buffers = DeclareBuffers()) {
            bindings.instances = builder.ShaderResource(buffers->instances).View();
            bindings.clusters = builder.ShaderResource(buffers->clusters).View();
            bindings.strands = builder.ShaderResource(buffers->strands).View();
            bindings.rootBindings = builder.ShaderResource(buffers->rootBindings).View();
            bindings.rootFrames = builder.ShaderResource(buffers->rootFrames).View();
            bindings.points = builder.ShaderResource(buffers->points).View();
            bindings.guideStrandIndices = builder.ShaderResource(buffers->guideStrandIndices).View();
            bindings.guidePointRanges = builder.ShaderResource(buffers->guidePointRanges).View();
            bindings.colliders = builder.ShaderResource(buffers->colliders).View();
            bindings.simFrame = builder.ShaderResource(buffers->simFrame).View();
            bindings.guideBindings = builder.ShaderResource(buffers->guideBindings).View();
            bindings.simGroups = builder.ShaderResource(buffers->simGroups).View();
            bindings.guidePoints = builder.UnorderedAccess(buffers->guidePoints).View();
            bindings.simMetrics = builder.UnorderedAccess(buffers->simMetrics).View();
            bindings.hasBuffers = true;
        }
        return bindings;
    }

    static void Record(const HairSimulateBindings& bindings, const HairPassFrame& frame, org::PassRecordContext& recording)
    {
        if (!bindings.hasBuffers || !frame.plan.simulate) return;
        br::render::MiscConstants constants{};
        constants[0] = recording.Index(bindings.instances);
        constants[1] = recording.Index(bindings.clusters);
        constants[2] = recording.Index(bindings.strands);
        constants[3] = recording.Index(bindings.rootBindings);
        constants[4] = recording.Index(bindings.rootFrames);
        constants[5] = recording.Index(bindings.points);
        constants[6] = recording.Index(bindings.guideStrandIndices);
        constants[7] = recording.Index(bindings.guidePointRanges);
        constants[8] = recording.Index(bindings.guidePoints);
        constants[9] = recording.Index(bindings.colliders);
        constants[10] = recording.Index(bindings.simFrame);
        constants[11] = recording.Index(bindings.simMetrics);
        constants[12] = 0u; // the first row of the dispatch's groups
        constants[13] = recording.Index(bindings.guideBindings);
        constants[14] = recording.Index(bindings.simGroups);
        if (frame.plan.simulateGroups != 0u) {
            br::render::DispatchCompute(recording, bindings.pipeline, constants, frame.plan.simulateGroups);
        }
    }
};

// Simulation LOD (HairGuideLevelCS): the guides the solver does not own are evaluated from their parents, and the ones
// in a blend are pulled toward that. One dispatch per guide level over that level's guides of every simulated instance
// (the frame's HairSimGroup rows), coarsest first, each behind a barrier: a level reads the rows the level below wrote.
class HairGuideLevelsPass final : public HairPass<HairGuideLevelsPass, HairSimulateBindings> {
public:
    using HairPass::HairPass;

    HairSimulateBindings Declare(org::PassBuilder& builder)
    {
        HairSimulateBindings bindings{};
        bindings.pipeline = m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairGuideLevels.hlsl", L"HairGuideLevelCS", "Hair.GuideLevels"));
        builder.ShaderResource(Builtin::PerObjectBuffer);
        if (const auto* buffers = DeclareBuffers()) {
            bindings.instances = builder.ShaderResource(buffers->instances).View();
            bindings.clusters = builder.ShaderResource(buffers->clusters).View();
            bindings.strands = builder.ShaderResource(buffers->strands).View();
            bindings.rootBindings = builder.ShaderResource(buffers->rootBindings).View();
            bindings.rootFrames = builder.ShaderResource(buffers->rootFrames).View();
            bindings.points = builder.ShaderResource(buffers->points).View();
            bindings.guideStrandIndices = builder.ShaderResource(buffers->guideStrandIndices).View();
            bindings.guidePointRanges = builder.ShaderResource(buffers->guidePointRanges).View();
            bindings.simFrame = builder.ShaderResource(buffers->simFrame).View();
            bindings.guideBindings = builder.ShaderResource(buffers->guideBindings).View();
            bindings.simGroups = builder.ShaderResource(buffers->simGroups).View();
            bindings.guidePoints = builder.UnorderedAccess(buffers->guidePoints).View();
            bindings.simMetrics = builder.UnorderedAccess(buffers->simMetrics).View();
            bindings.hasBuffers = true;
        }
        return bindings;
    }

    static void Record(const HairSimulateBindings& bindings, const HairPassFrame& frame, org::PassRecordContext& recording)
    {
        if (!bindings.hasBuffers || !frame.plan.simulate) return;
        br::render::MiscConstants constants{};
        constants[0] = recording.Index(bindings.instances);
        constants[1] = recording.Index(bindings.clusters);
        constants[2] = recording.Index(bindings.strands);
        constants[3] = recording.Index(bindings.rootBindings);
        constants[4] = recording.Index(bindings.rootFrames);
        constants[5] = recording.Index(bindings.points);
        constants[6] = recording.Index(bindings.guideStrandIndices);
        constants[7] = recording.Index(bindings.guidePointRanges);
        constants[8] = recording.Index(bindings.guidePoints);
        constants[9] = recording.Index(bindings.guideBindings);
        constants[10] = recording.Index(bindings.simFrame);
        constants[11] = recording.Index(bindings.simMetrics);
        constants[13] = recording.Index(bindings.simGroups);
        for (std::uint32_t level = 0u; level < HAIR_MAX_GUIDE_LEVELS; ++level) {
            const std::uint32_t groups = frame.plan.levelGroupFirst[level + 1u] - frame.plan.levelGroupFirst[level];
            if (groups == 0u) continue;
            constants[12] = frame.plan.levelGroupFirst[level];
            br::render::DispatchCompute(recording, bindings.pipeline, constants, groups);
            recording.UavBarrier();
        }
    }
};

struct HairSelectBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken instances, clusters, aggregates, rootBindings, rootFrames, frameState, counters, visibleClusters;
    bool hasBuffers = false;
};

// LOD selection for the main view: one dispatch per active instance, one thread per cluster (HairSelectCS). It does not
// run while the LOD is frozen: the visible-cluster list and its counters are then the last selection's.
class HairSelectPass final : public HairPass<HairSelectPass, HairSelectBindings> {
public:
    using HairPass::HairPass;

    HairSelectBindings Declare(org::PassBuilder& builder)
    {
        HairSelectBindings bindings{};
        bindings.pipeline = m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairSelectEvaluate.hlsl", L"HairSelectCS", "Hair.Select"));
        builder.ShaderResource(Builtin::PerObjectBuffer, Builtin::CameraBuffer);
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        if (const auto* buffers = DeclareBuffers()) {
            bindings.instances = builder.ShaderResource(buffers->instances).View();
            bindings.clusters = builder.ShaderResource(buffers->clusters).View();
            bindings.aggregates = builder.ShaderResource(buffers->aggregates).View();
            bindings.rootBindings = builder.ShaderResource(buffers->rootBindings).View();
            bindings.rootFrames = builder.ShaderResource(buffers->rootFrames).View();
            bindings.frameState = builder.ShaderResource(buffers->frameState).View();
            bindings.counters = builder.UnorderedAccess(buffers->counters).View();
            bindings.visibleClusters = builder.UnorderedAccess(buffers->visibleClusters).View();
            bindings.hasBuffers = true;
        }
        return bindings;
    }

    static void Record(const HairSelectBindings& bindings, const HairPassFrame& frame, org::PassRecordContext& recording)
    {
        if (!bindings.hasBuffers || !frame.plan.select) return;
        br::render::MiscConstants constants{};
        constants[0] = recording.Index(bindings.instances);
        constants[1] = recording.Index(bindings.clusters);
        constants[2] = recording.Index(bindings.aggregates);
        constants[3] = recording.Index(bindings.rootBindings);
        constants[4] = recording.Index(bindings.rootFrames);
        constants[5] = recording.Index(bindings.frameState);
        constants[6] = recording.Index(bindings.counters);
        constants[7] = recording.Index(bindings.visibleClusters);
        for (const auto& instance : frame.plan.instances) {
            constants[8] = instance.instanceIndex;
            br::render::DispatchCompute(recording, bindings.pipeline, constants, Groups(instance.clusterCount));
        }
    }
};

struct HairEvaluateBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken instances, clusters, strands, rootBindings, rootFrames, points, frameState, counters, visibleClusters,
        poolVertices, poolSegmentIds, guidePoints, guidePointRanges, simMetrics;
    bool hasBuffers = false;
};

// Strand evaluation: one thread group per visible cluster, one thread per drawn strand (HairEvaluateCS). The GPU knows
// how many clusters are visible; the dispatch covers the most there can be and the rest return at once.
class HairEvaluatePass final : public HairPass<HairEvaluatePass, HairEvaluateBindings> {
public:
    using HairPass::HairPass;

    HairEvaluateBindings Declare(org::PassBuilder& builder)
    {
        HairEvaluateBindings bindings{};
        bindings.pipeline = m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairSelectEvaluate.hlsl", L"HairEvaluateCS", "Hair.Evaluate"));
        builder.ShaderResource(Builtin::PerObjectBuffer, Builtin::CameraBuffer);
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        if (const auto* buffers = DeclareBuffers()) {
            bindings.instances = builder.ShaderResource(buffers->instances).View();
            bindings.clusters = builder.ShaderResource(buffers->clusters).View();
            bindings.strands = builder.ShaderResource(buffers->strands).View();
            bindings.rootBindings = builder.ShaderResource(buffers->rootBindings).View();
            bindings.rootFrames = builder.ShaderResource(buffers->rootFrames).View();
            bindings.points = builder.ShaderResource(buffers->points).View();
            bindings.frameState = builder.ShaderResource(buffers->frameState).View();
            bindings.counters = builder.ShaderResource(buffers->counters).View();
            bindings.visibleClusters = builder.ShaderResource(buffers->visibleClusters).View();
            bindings.poolVertices = builder.UnorderedAccess(buffers->poolVertices).View();
            bindings.poolSegmentIds = builder.UnorderedAccess(buffers->poolSegmentIds).View();
            // Under simulation: the guides' state, and where the roots it evaluates ended up.
            bindings.guidePoints = builder.ShaderResource(buffers->guidePoints).View();
            bindings.guidePointRanges = builder.ShaderResource(buffers->guidePointRanges).View();
            bindings.simMetrics = builder.UnorderedAccess(buffers->simMetrics).View();
            bindings.hasBuffers = true;
        }
        return bindings;
    }

    static void Record(const HairEvaluateBindings& bindings, const HairPassFrame& frame, org::PassRecordContext& recording)
    {
        if (!bindings.hasBuffers) return;
        br::render::MiscConstants constants{};
        constants[0] = recording.Index(bindings.instances);
        constants[1] = recording.Index(bindings.clusters);
        constants[3] = recording.Index(bindings.rootBindings);
        constants[4] = recording.Index(bindings.rootFrames);
        constants[5] = recording.Index(bindings.frameState);
        constants[6] = recording.Index(bindings.counters);
        constants[7] = recording.Index(bindings.visibleClusters);
        constants[9] = recording.Index(bindings.strands);
        constants[10] = recording.Index(bindings.points);
        constants[11] = recording.Index(bindings.poolVertices);
        constants[12] = recording.Index(bindings.poolSegmentIds);
        constants[13] = recording.Index(bindings.guidePoints);
        constants[14] = recording.Index(bindings.guidePointRanges);
        constants[15] = recording.Index(bindings.simMetrics);
        br::render::DispatchCompute(recording, bindings.pipeline, constants, frame.plan.evaluateGroups);
    }
};

struct HairRasterBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken frameState, counters, visibleClusters, poolVertices, poolSegmentIds, visibility, visibilityClear;
    std::uint32_t width = 0u; // the declared target's size
    std::uint32_t height = 0u;
    bool hasBuffers = false;
};

// Clears the hair visibility target and rasterizes the pool's segments into it against the frame's opaque visibility
// buffer (HairRasterCS): one thread per pool row, over the most rows the frame can use.
class HairRasterPass final : public HairPass<HairRasterPass, HairRasterBindings> {
public:
    using HairPass::HairPass;

    HairRasterBindings Declare(org::PassBuilder& builder)
    {
        HairRasterBindings bindings{};
        bindings.pipeline = m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairRaster.hlsl", L"HairRasterCS", "Hair.Raster"));
        // The opaque geometry of this frame: its depth is the high bits of each visibility key.
        builder.ShaderResource(Builtin::PrimaryCamera::VisibilityTexture, Builtin::CameraBuffer);
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        if (const auto* buffers = DeclareBuffers()) {
            bindings.frameState = builder.ShaderResource(buffers->frameState).View();
            bindings.visibleClusters = builder.ShaderResource(buffers->visibleClusters).View();
            bindings.poolVertices = builder.ShaderResource(buffers->poolVertices).View();
            bindings.poolSegmentIds = builder.ShaderResource(buffers->poolSegmentIds).View();
            bindings.counters = builder.UnorderedAccess(buffers->counters).View();
            // View 0 is the shader's, view 1 the clear's own (not shader-visible).
            const auto visibility = builder.UnorderedAccessClear(buffers->visibility);
            bindings.visibility = visibility.View(0);
            bindings.visibilityClear = visibility.View(1);
            bindings.width = buffers->visibilityWidth;
            bindings.height = buffers->visibilityHeight;
            bindings.hasBuffers = true;
        }
        return bindings;
    }

    static void Record(const HairRasterBindings& bindings, const HairPassFrame& frame, org::PassRecordContext& recording)
    {
        if (!bindings.hasBuffers) return;
        // Every frame, drawn or not: all ones is "no hair".
        br::render::ClearUavUint(recording, bindings.visibility, bindings.visibilityClear, 0xFFFFFFFFu);
        const std::uint32_t groups = Groups(frame.plan.rasterRows);
        if (frame.plan.instances.empty() || groups == 0u) return;
        br::render::MiscConstants constants{};
        constants[0] = recording.Index(bindings.frameState);
        constants[1] = recording.Index(bindings.counters);
        constants[2] = recording.Index(bindings.visibleClusters);
        constants[3] = recording.Index(bindings.poolVertices);
        constants[4] = recording.Index(bindings.poolSegmentIds);
        constants[5] = recording.Index(bindings.visibility);
        constants[6] = bindings.width;
        constants[7] = bindings.height;
        constants[8] = kRasterGroupsPerRow;
        br::render::DispatchCompute(recording, bindings.pipeline, constants, (std::min)(groups, kRasterGroupsPerRow),
            (groups + kRasterGroupsPerRow - 1u) / kRasterGroupsPerRow);
    }
};

struct HairResolveBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken visibility, visibleClusters, instances, poolVertices, counters, probe;
    org::DeclaredViewToken linearDepth, projectedDepth, deviceDepth;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    bool hasBuffers = false;
};

// The root constants hairResolve.hlsl's two entry points share. The resolve adds its buffers in slots 1 to 3 and the
// counters in slot 9; the probe adds its buffer in slot 9.
br::render::MiscConstants ResolveConstants(const HairResolveBindings& bindings, org::PassRecordContext& recording)
{
    br::render::MiscConstants constants{};
    constants[0] = recording.Index(bindings.visibility);
    constants[4] = recording.Index(bindings.linearDepth);
    constants[5] = recording.Index(bindings.projectedDepth);
    constants[6] = recording.Index(bindings.deviceDepth);
    constants[7] = bindings.width;
    constants[8] = bindings.height;
    return constants;
}

// Writes every hair pixel into the frame's surface data (HairResolveCS): the canonical surface targets and record,
// the three depth targets and a motion vector. One thread a pixel.
class HairResolvePass final : public HairPass<HairResolvePass, HairResolveBindings> {
public:
    using HairPass::HairPass;

    HairResolveBindings Declare(org::PassBuilder& builder)
    {
        HairResolveBindings bindings{};
        bindings.pipeline = m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairResolve.hlsl", L"HairResolveCS", "Hair.Resolve"));
        builder.ShaderResource(Builtin::CameraBuffer);
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        // What material evaluation writes for a pixel of the opaque visibility buffer, less the two payloads (the
        // resolve writes payload profile 0), and what the depth copy derives from that buffer.
        builder.UnorderedAccess(Builtin::Surface::BaseColorOpacity, Builtin::Surface::NormalRoughness, Builtin::Surface::SpecularAo,
            Builtin::Surface::Emissive, Builtin::Surface::Motion, Builtin::Surface::Identity, Builtin::Surface::Records);
        bindings.linearDepth = builder.UnorderedAccess(org::Subresources(Builtin::PrimaryCamera::LinearDepthMap, org::Mip{ 0, 1 })).View();
        bindings.projectedDepth = builder.UnorderedAccess(Builtin::PrimaryCamera::ProjectedDepthTexture).View();
        bindings.deviceDepth = builder.UnorderedAccess(Builtin::Surface::DeviceDepth).View();
        if (const auto* buffers = DeclareBuffers()) {
            bindings.visibility = builder.ShaderResource(buffers->visibility).View();
            bindings.visibleClusters = builder.ShaderResource(buffers->visibleClusters).View();
            bindings.instances = builder.ShaderResource(buffers->instances).View();
            bindings.poolVertices = builder.ShaderResource(buffers->poolVertices).View();
            bindings.counters = builder.UnorderedAccess(buffers->counters).View();
            bindings.width = buffers->visibilityWidth;
            bindings.height = buffers->visibilityHeight;
            bindings.hasBuffers = true;
        }
        return bindings;
    }

    static void Record(const HairResolveBindings& bindings, const HairPassFrame& frame, org::PassRecordContext& recording)
    {
        if (!bindings.hasBuffers || frame.plan.instances.empty()) return;
        auto constants = ResolveConstants(bindings, recording);
        constants[1] = recording.Index(bindings.visibleClusters);
        constants[2] = recording.Index(bindings.instances);
        constants[3] = recording.Index(bindings.poolVertices);
        constants[9] = recording.Index(bindings.counters);
        br::render::DispatchCompute(recording, bindings.pipeline, constants, (bindings.width + 7u) / 8u, (bindings.height + 7u) / 8u);
    }
};

// hair.probe: copies what the frame's targets hold at hair pixels into the probe buffer (HairProbeCS), in the frame the
// runtime asked for one. One thread for every HAIR_PROBE_STRIDE-th pixel each way.
class HairProbePass final : public HairPass<HairProbePass, HairResolveBindings> {
public:
    using HairPass::HairPass;

    HairResolveBindings Declare(org::PassBuilder& builder)
    {
        HairResolveBindings bindings{};
        bindings.pipeline = m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairResolve.hlsl", L"HairProbeCS", "Hair.Probe"));
        builder.ShaderResource(Builtin::CameraBuffer, Builtin::Surface::Motion, Builtin::Surface::NormalRoughness);
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        bindings.linearDepth = builder.ShaderResource(org::Subresources(Builtin::PrimaryCamera::LinearDepthMap, org::Mip{ 0, 1 })).View();
        bindings.projectedDepth = builder.ShaderResource(Builtin::PrimaryCamera::ProjectedDepthTexture).View();
        bindings.deviceDepth = builder.ShaderResource(Builtin::Surface::DeviceDepth).View();
        if (const auto* buffers = DeclareBuffers()) {
            bindings.visibility = builder.ShaderResource(buffers->visibility).View();
            bindings.probe = builder.UnorderedAccess(buffers->probe).View();
            bindings.width = buffers->visibilityWidth;
            bindings.height = buffers->visibilityHeight;
            bindings.hasBuffers = true;
        }
        return bindings;
    }

    static void Record(const HairResolveBindings& bindings, const HairPassFrame& frame, org::PassRecordContext& recording)
    {
        if (!bindings.hasBuffers || !frame.plan.probe) return;
        auto constants = ResolveConstants(bindings, recording);
        constants[9] = recording.Index(bindings.probe);
        constexpr std::uint32_t pixelsPerGroup = 8u * HAIR_PROBE_STRIDE;
        br::render::DispatchCompute(recording, bindings.pipeline, constants, (bindings.width + pixelsPerGroup - 1u) / pixelsPerGroup,
            (bindings.height + pixelsPerGroup - 1u) / pixelsPerGroup);
    }
};

struct HairDebugOverlayBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken visibility, visibleClusters, instances, clusters, strands, target;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    bool hasBuffers = false;
};

// The debug view (hair.debug): colours the pixels of the hair visibility target over the presentation colour, after
// tonemapping. It records nothing while the view is off.
class HairDebugOverlayPass final : public HairPass<HairDebugOverlayPass, HairDebugOverlayBindings> {
public:
    using HairPass::HairPass;

    HairDebugOverlayBindings Declare(org::PassBuilder& builder)
    {
        HairDebugOverlayBindings bindings{};
        bindings.pipeline = m_pipeline = br::render::DeclarePipeline(PipelineDesc());
        builder.ShaderResource(Builtin::CameraBuffer);
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        if (const auto* buffers = DeclareBuffers()) {
            bindings.visibility = builder.ShaderResource(buffers->visibility).View();
            bindings.visibleClusters = builder.ShaderResource(buffers->visibleClusters).View();
            bindings.instances = builder.ShaderResource(buffers->instances).View();
            bindings.clusters = builder.ShaderResource(buffers->clusters).View();
            bindings.strands = builder.ShaderResource(buffers->strands).View();
            bindings.target = builder.RenderTarget(org::ResourceIdentifier{ Builtin::PresentationColor }).View();
            bindings.width = buffers->visibilityWidth;
            bindings.height = buffers->visibilityHeight;
            bindings.hasBuffers = true;
        }
        return bindings;
    }

    // A capture (hair.capture) reads the texture the frame presents. The graph names it through a wrapper that points
    // at a different texture each frame, and a readback needs the texture itself: this frame's.
    void Update(const org::UpdateExecutionContext&) override
    {
        std::string path;
        if (!m_runtime->TakeDebugCaptureRequest(path)) return;
        const auto wrapper = m_resourceRegistryView
            ? m_resourceRegistryView->RequestSharedAs<org::DynamicResource>(org::ResourceIdentifier{ Builtin::PresentationColor })
            : nullptr;
        const auto texture = wrapper ? wrapper->GetResource() : nullptr;
        m_runtime->ReadDebugCapture(path, texture.get());
    }

    static void Record(const HairDebugOverlayBindings& bindings, const HairPassFrame& frame, org::PassRecordContext& recording)
    {
        if (!bindings.hasBuffers || frame.plan.debugView == HairDebugView::Off || frame.outputWidth == 0u || frame.outputHeight == 0u) {
            return;
        }
        br::render::MiscConstants constants{};
        constants[0] = recording.Index(bindings.visibility);
        constants[1] = recording.Index(bindings.visibleClusters);
        constants[2] = recording.Index(bindings.instances);
        constants[3] = recording.Index(bindings.clusters);
        constants[4] = recording.Index(bindings.strands);
        constants[5] = static_cast<std::uint32_t>(frame.plan.debugView);
        constants[6] = bindings.width;
        constants[7] = bindings.height;
        constants[8] = frame.outputWidth;
        constants[9] = frame.outputHeight;
        br::render::DrawFullscreen(recording, bindings.pipeline, constants, br::render::ColorTarget(recording, bindings.target),
            frame.outputWidth, frame.outputHeight, "Hair Debug Overlay");
    }

private:
    static br::GraphicsPipelineDesc PipelineDesc()
    {
        br::GraphicsPipelineDesc desc;
        desc.name = "Hair.DebugOverlay";
        desc.vertex = { L"shaders/fullscreenVS.hlsli", L"FullscreenVSNoViewRayMain", L"vs_6_6" };
        desc.pixel = { L"HairShaders/hairDebugOverlay.hlsl", L"HairDebugOverlayPS", L"ps_6_6" };
        rhi::RasterState raster{};
        raster.fill = rhi::FillMode::Solid;
        raster.cull = rhi::CullMode::None;
        desc.raster = raster;
        // Hair pixels have alpha 1, the rest alpha 0: the frame shows through where there is no hair.
        rhi::BlendState blend{};
        blend.numAttachments = 1;
        auto& attachment = blend.attachments[0];
        attachment.enable = true;
        attachment.srcColor = rhi::BlendFactor::SrcAlpha;
        attachment.dstColor = rhi::BlendFactor::InvSrcAlpha;
        attachment.colorOp = rhi::BlendOp::Add;
        attachment.srcAlpha = rhi::BlendFactor::One;
        attachment.dstAlpha = rhi::BlendFactor::InvSrcAlpha;
        attachment.alphaOp = rhi::BlendOp::Add;
        attachment.writeMask = rhi::ColorWriteEnable::All;
        desc.blend = blend;
        rhi::DepthStencilState depth{};
        depth.depthEnable = false;
        depth.depthWrite = false;
        depth.depthFunc = rhi::CompareOp::Always;
        desc.depth = depth;
        rhi::RenderTargets targets{};
        targets.count = 1;
        targets.formats[0] = rhi::Format::R8G8B8A8_UNorm;
        desc.renderTargets = targets;
        desc.sample = rhi::SampleDesc{ 1, 0 };
        desc.topology = rhi::PrimitiveTopology::TriangleStrip;
        return desc;
    }
};

} // namespace

HairExtension::HairExtension(std::shared_ptr<HairRuntime> runtime) : m_runtime(std::move(runtime)) {}

void HairExtension::GatherStructuralPasses(org::RenderGraph& rg, std::vector<org::ExternalPassDesc>& out)
{
    // Before culling, where procedural wind writes its palettes. Selection and evaluation need the roots and the main
    // view's camera, and nothing of the frame's opaque geometry.
    const auto beforeCulling = org::ExternalInsertPoint::Before("CLodOpaque::HierarchicalCullingPass1");
    out.push_back(org::ExternalPassDesc::Compute("Hair::RootBinding", std::make_shared<HairRootBindingPass>(m_runtime)).At(beforeCulling));
    out.push_back(org::ExternalPassDesc::Compute("Hair::Simulate", std::make_shared<HairSimulatePass>(m_runtime)).At(beforeCulling));
    out.push_back(org::ExternalPassDesc::Compute("Hair::GuideLevels", std::make_shared<HairGuideLevelsPass>(m_runtime)).At(beforeCulling));
    out.push_back(org::ExternalPassDesc::Compute("Hair::Select", std::make_shared<HairSelectPass>(m_runtime)).At(beforeCulling));
    out.push_back(org::ExternalPassDesc::Compute("Hair::Evaluate", std::make_shared<HairEvaluatePass>(m_runtime)).At(beforeCulling));

    // The rasterizer tests against the opaque visibility buffer, so it runs once that is complete. Every pass that
    // writes it is placed before MaterialHistogramPass, the first pass to read the finished buffer, so after that pass
    // is after all of them without naming them. It is before material evaluation: a resolve of hair into the canonical
    // surface follows EvaluateMaterialGroupsPass and reads the target.
    auto afterOpaqueVisibility = org::ExternalInsertPoint::After("MaterialHistogramPass");
    afterOpaqueVisibility.AlsoBefore("EvaluateMaterialGroupsPass");
    out.push_back(org::ExternalPassDesc::Compute("Hair::Raster", std::make_shared<HairRasterPass>(m_runtime)).At(afterOpaqueVisibility));

    // The resolve replaces what material evaluation and the frame's last depth copy wrote at hair pixels, so it follows
    // both. It runs before the linear depth's mips are made from mip 0, and so before everything that reads the frame's
    // depth: virtual shadow page marking, ambient occlusion, light culling, shading and the upscaler.
    auto afterSurface = org::ExternalInsertPoint::After("EvaluateMaterialGroupsPass");
    afterSurface.AlsoAfter("CLodOpaque::LinearDepthCopyPass2").AlsoBefore("CLodOpaque::LinearDepthDownsamplePass2");
    out.push_back(org::ExternalPassDesc::Compute("Hair::Resolve", std::make_shared<HairResolvePass>(m_runtime)).At(afterSurface));
    out.push_back(org::ExternalPassDesc::Compute("Hair::Probe", std::make_shared<HairProbePass>(m_runtime))
        .At(org::ExternalInsertPoint::After("Hair::Resolve")));

    // A debug view: the Overlay epoch's, not the extension's (Visibility). Before the present pass, which makes the
    // backbuffer's last access.
    auto afterTonemapping = org::ExternalInsertPoint::After("TonemappingPass");
    if (!rg.GetPresentPass().empty()) afterTonemapping.AlsoBefore(rg.GetPresentPass());
    out.push_back(org::ExternalPassDesc::Render("Hair::DebugOverlay", std::make_shared<HairDebugOverlayPass>(m_runtime))
        .At(afterTonemapping)
        .Epoch(br::render::kOverlayEpoch));
}

} // namespace br::hair
