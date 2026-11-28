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
#include <BasicRenderer/Extensions/SettingAccess.h>
#include <BasicRenderer/Pipeline/PSOFlags.h>
#include <BasicRenderer/Streaming/VersionedGpuBuffer.h>

namespace br::hair {

struct HairExtension::ShadowMap {
    std::shared_ptr<org::PixelBuffer> staticPages;
};

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
    br::render::AppendRevision(out, plan.renderPath);
    br::render::AppendRevision(out, plan.reactiveMask);
    br::render::AppendRevision(out, plan.rasterCulled);
    br::render::AppendRevision(out, plan.volumeVoxels);
    br::render::AppendRevision(out, plan.volumeRows);
    br::render::AppendRevision(out, plan.shadows);
    br::render::AppendRevision(out, plan.shadowEvaluateGroups);
    br::render::AppendRevision(out, plan.shadowRasterRows);
    br::render::AppendRevision(out, plan.probe);
    br::render::AppendRevision(out, plan.rootCheckCount);
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

struct HairRootCheckBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken instances, rootBindings, rootCheck, rootCheckList, frameState;
    bool hasBuffers = false;
};

// The plugin's per-frame work on the render thread, in its Update: it makes the buffers at the first groom, uploads new
// grooms and changed instance rows, resolves each instance's entity, lends guide rows and decides the frame's plan for
// every hair pass. What it records is the self-check's (hair.selfcheck, hairRootCheck.hlsl): in a frame that samples,
// the root frames of the listed roots into the check buffer. No buffer holds root frames otherwise: each pass works out
// the ones it needs.
class HairRootCheckPass final : public HairPass<HairRootCheckPass, HairRootCheckBindings> {
public:
    using HairPass::HairPass;

    HairRootCheckBindings Declare(org::PassBuilder& builder)
    {
        HairRootCheckBindings bindings{};
        bindings.pipeline = m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairRootCheck.hlsl", L"HairRootCheckCS", "Hair.RootCheck"));
        // Read by name in the shader (ResourceDescriptorIndex): the entities' transforms and the bone palettes.
        builder.ShaderResource(Builtin::PerObjectBuffer, Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo);
        if (const auto* buffers = DeclareBuffers()) {
            bindings.instances = builder.ShaderResource(buffers->instances).View();
            bindings.rootBindings = builder.ShaderResource(buffers->rootBindings).View();
            bindings.frameState = builder.ShaderResource(buffers->frameState).View();
            bindings.rootCheckList = builder.ShaderResource(buffers->rootCheckList).View();
            bindings.rootCheck = builder.UnorderedAccess(buffers->rootCheck).View();
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
        // The main view, as the select pass's frustum test reads it from the camera buffer.
        const CameraInfo& main = context->primaryCamera.info;
        DirectX::XMStoreFloat4x4(reinterpret_cast<DirectX::XMFLOAT4X4*>(inputs.mainView.view.data()), main.view);
        for (std::size_t plane = 0u; plane < inputs.mainView.planes.size(); ++plane) {
            const DirectX::XMFLOAT4& source = main.clippingPlanes[plane].plane;
            inputs.mainView.planes[plane] = { source.x, source.y, source.z, source.w };
        }
        inputs.mainView.projectionScaleX = DirectX::XMVectorGetX(main.unjitteredProjection.r[0]);
        inputs.mainView.projectionScaleY = DirectX::XMVectorGetY(main.unjitteredProjection.r[1]);
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

    static void Record(const HairRootCheckBindings& bindings, const HairPassFrame& frame, org::PassRecordContext& recording)
    {
        if (!bindings.hasBuffers || frame.plan.rootCheckCount == 0u) return;
        br::render::MiscConstants constants{};
        constants[0] = recording.Index(bindings.instances);
        constants[1] = recording.Index(bindings.rootBindings);
        constants[2] = recording.Index(bindings.rootCheck);
        constants[3] = recording.Index(bindings.rootCheckList);
        constants[4] = recording.Index(bindings.frameState);
        constants[5] = frame.plan.rootCheckCount;
        br::render::DispatchCompute(recording, bindings.pipeline, constants, Groups(frame.plan.rootCheckCount));
    }
};

struct HairSimulateBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken instances, clusters, strands, rootBindings, points, guideStrandIndices, guidePointRanges,
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
        // For the guides' root frames and the colliders: the entities' transforms and the bone palettes.
        builder.ShaderResource(Builtin::PerObjectBuffer, Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo);
        if (const auto* buffers = DeclareBuffers()) {
            bindings.instances = builder.ShaderResource(buffers->instances).View();
            bindings.clusters = builder.ShaderResource(buffers->clusters).View();
            bindings.strands = builder.ShaderResource(buffers->strands).View();
            bindings.rootBindings = builder.ShaderResource(buffers->rootBindings).View();
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
        // For the guides' root frames: the entities' transforms and the bone palettes.
        builder.ShaderResource(Builtin::PerObjectBuffer, Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo);
        if (const auto* buffers = DeclareBuffers()) {
            bindings.instances = builder.ShaderResource(buffers->instances).View();
            bindings.clusters = builder.ShaderResource(buffers->clusters).View();
            bindings.strands = builder.ShaderResource(buffers->strands).View();
            bindings.rootBindings = builder.ShaderResource(buffers->rootBindings).View();
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
    org::DeclaredViewToken instances, clusters, aggregates, rootBindings, frameState, counters, visibleClusters, clusterSeen;
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
        // A cluster's bounds follow its first strand's root, which the pass works out itself: the entities' transforms
        // and the bone palettes.
        builder.ShaderResource(Builtin::PerObjectBuffer, Builtin::CameraBuffer, Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo);
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        if (const auto* buffers = DeclareBuffers()) {
            bindings.instances = builder.ShaderResource(buffers->instances).View();
            bindings.clusters = builder.ShaderResource(buffers->clusters).View();
            bindings.aggregates = builder.ShaderResource(buffers->aggregates).View();
            bindings.rootBindings = builder.ShaderResource(buffers->rootBindings).View();
            bindings.frameState = builder.ShaderResource(buffers->frameState).View();
            bindings.counters = builder.UnorderedAccess(buffers->counters).View();
            bindings.visibleClusters = builder.UnorderedAccess(buffers->visibleClusters).View();
            bindings.clusterSeen = builder.UnorderedAccess(buffers->clusterSeen).View();
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
        constants[5] = recording.Index(bindings.frameState);
        constants[6] = recording.Index(bindings.counters);
        constants[7] = recording.Index(bindings.visibleClusters);
        constants[16] = recording.Index(bindings.clusterSeen);
        for (const auto& instance : frame.plan.instances) {
            constants[8] = instance.instanceIndex;
            br::render::DispatchCompute(recording, bindings.pipeline, constants, Groups(instance.clusterCount));
        }
    }
};

struct HairEvaluateBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken instances, clusters, strands, rootBindings, points, frameState, counters, visibleClusters,
        poolVertices, poolSegmentIds, guidePoints, guidePointRanges, simMetrics;
    bool hasBuffers = false;
    bool shadowView = false;
};

// Strand evaluation: one thread group per visible cluster, one thread per drawn strand (HairEvaluateCS). The GPU knows
// how many clusters are visible; the dispatch covers the most there can be and the rest return at once. A thread works
// out its strand's root frame itself.
// The shadow view's evaluation is the same pass over the shadow cluster list into the shadow pool (hairShadow.hlsl),
// in a frame that draws hair into the shadow map.
class HairEvaluatePass final : public HairPass<HairEvaluatePass, HairEvaluateBindings> {
public:
    explicit HairEvaluatePass(std::shared_ptr<HairRuntime> runtime, bool shadowView = false)
        : HairPass(std::move(runtime)), m_shadowView(shadowView) {}

    HairEvaluateBindings Declare(org::PassBuilder& builder)
    {
        HairEvaluateBindings bindings{};
        bindings.pipeline = m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairSelectEvaluate.hlsl", L"HairEvaluateCS", "Hair.Evaluate"));
        builder.ShaderResource(Builtin::PerObjectBuffer, Builtin::CameraBuffer, Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo);
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        if (const auto* buffers = DeclareBuffers()) {
            bindings.instances = builder.ShaderResource(buffers->instances).View();
            bindings.clusters = builder.ShaderResource(buffers->clusters).View();
            bindings.strands = builder.ShaderResource(buffers->strands).View();
            bindings.rootBindings = builder.ShaderResource(buffers->rootBindings).View();
            bindings.points = builder.ShaderResource(buffers->points).View();
            bindings.frameState = builder.ShaderResource(buffers->frameState).View();
            bindings.counters = builder.UnorderedAccess(buffers->counters).View(); // read, and the two checks' counters
            bindings.visibleClusters = builder.ShaderResource(m_shadowView ? buffers->shadowClusters : buffers->visibleClusters).View();
            bindings.poolVertices = builder.UnorderedAccess(m_shadowView ? buffers->shadowPoolVertices : buffers->poolVertices).View();
            bindings.poolSegmentIds =
                builder.UnorderedAccess(m_shadowView ? buffers->shadowPoolSegmentIds : buffers->poolSegmentIds).View();
            bindings.shadowView = m_shadowView;
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
        const std::uint32_t groups = bindings.shadowView ? frame.plan.shadowEvaluateGroups : frame.plan.evaluateGroups;
        if (bindings.shadowView && (!frame.plan.shadows || groups == 0u)) return;
        br::render::MiscConstants constants{};
        constants[0] = recording.Index(bindings.instances);
        constants[1] = recording.Index(bindings.clusters);
        constants[3] = recording.Index(bindings.rootBindings);
        constants[4] = bindings.shadowView ? 1u : 0u; // HAIR_EVALUATE_SHADOW
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
        br::render::DispatchCompute(recording, bindings.pipeline, constants, groups);
    }

private:
    bool m_shadowView = false;
};

struct HairShadowSelectBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken instances, clusters, aggregates, rootBindings, frameState, counters, shadowClusters, shadowMasks,
        clipmaps, shadowCameras, staticBlocks, dynamicBlocks;
    bool hasBuffers = false;
};

// The shadow view's selection (HairShadowSelectCS): one dispatch per active instance, one thread per cluster. A cluster
// is kept when it lies over a page the sun's shadow map draws this frame. It records only in a frame that draws hair
// into the shadow map (HairFramePlan::shadows).
class HairShadowSelectPass final : public HairPass<HairShadowSelectPass, HairShadowSelectBindings> {
public:
    HairShadowSelectPass(std::shared_ptr<HairRuntime> runtime, VirtualShadowCasterBuildContext context)
        : HairPass(std::move(runtime)), m_context(std::move(context)) {}

    HairShadowSelectBindings Declare(org::PassBuilder& builder)
    {
        HairShadowSelectBindings bindings{};
        bindings.pipeline = m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairShadow.hlsl", L"HairShadowSelectCS", "Hair.ShadowSelect"));
        builder.ShaderResource(Builtin::PerObjectBuffer, Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo);
        bindings.clipmaps = builder.ShaderResource(m_context.clipmapInfo).View();
        bindings.shadowCameras = builder.ShaderResource(m_context.compactShadowCameras).View();
        bindings.staticBlocks = builder.ShaderResource(m_context.staticActiveBlockMetadata).View();
        bindings.dynamicBlocks = builder.ShaderResource(m_context.dynamicActiveBlockMetadata).View();
        if (const auto* buffers = DeclareBuffers()) {
            bindings.instances = builder.ShaderResource(buffers->instances).View();
            bindings.clusters = builder.ShaderResource(buffers->clusters).View();
            bindings.aggregates = builder.ShaderResource(buffers->aggregates).View();
            bindings.rootBindings = builder.ShaderResource(buffers->rootBindings).View();
            bindings.frameState = builder.ShaderResource(buffers->frameState).View();
            bindings.counters = builder.UnorderedAccess(buffers->counters).View();
            bindings.shadowClusters = builder.UnorderedAccess(buffers->shadowClusters).View();
            bindings.shadowMasks = builder.UnorderedAccess(buffers->shadowClusterMasks).View();
            bindings.hasBuffers = true;
        }
        return bindings;
    }

    // A graph has this pass only while it has a shadow map: the runtime casts no shadow otherwise.
    void Update(const org::UpdateExecutionContext& execution) override
    {
        if (const auto* context = execution.hostData ? execution.hostData->Get<UpdateContext>() : nullptr) {
            m_runtime->NoteShadowPasses(context->frameNumber);
        }
    }

    static void Record(const HairShadowSelectBindings& bindings, const HairPassFrame& frame, org::PassRecordContext& recording)
    {
        if (!bindings.hasBuffers || !frame.plan.shadows) return;
        br::render::MiscConstants constants{};
        constants[0] = recording.Index(bindings.frameState);
        constants[1] = recording.Index(bindings.counters);
        constants[2] = recording.Index(bindings.shadowClusters);
        constants[3] = recording.Index(bindings.shadowMasks);
        constants[4] = recording.Index(bindings.clipmaps);
        constants[5] = recording.Index(bindings.shadowCameras);
        constants[6] = recording.Index(bindings.instances);
        constants[7] = recording.Index(bindings.clusters);
        constants[8] = recording.Index(bindings.aggregates);
        constants[9] = recording.Index(bindings.rootBindings);
        constants[11] = recording.Index(bindings.staticBlocks);
        constants[12] = recording.Index(bindings.dynamicBlocks);
        for (const auto& instance : frame.plan.instances) {
            constants[10] = instance.instanceIndex;
            br::render::DispatchCompute(recording, bindings.pipeline, constants, Groups(instance.clusterCount));
        }
    }

private:
    VirtualShadowCasterBuildContext m_context;
};

struct HairShadowRasterBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken frameState, counters, shadowClusters, shadowMasks, clipmaps, shadowCameras, poolVertices, poolSegmentIds,
        pageTable, staticPages, dynamicPages, statistics;
    bool hasBuffers = false;
};

// The shadow view's rasterizer (HairShadowRasterCS): one thread per row of the shadow pool, over the most rows the frame
// can use, into the shadow map's pages.
class HairShadowRasterPass final : public HairPass<HairShadowRasterPass, HairShadowRasterBindings> {
public:
    HairShadowRasterPass(std::shared_ptr<HairRuntime> runtime, VirtualShadowCasterBuildContext context)
        : HairPass(std::move(runtime)), m_context(std::move(context)) {}

    HairShadowRasterBindings Declare(org::PassBuilder& builder)
    {
        HairShadowRasterBindings bindings{};
        bindings.pipeline = m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairShadow.hlsl", L"HairShadowRasterCS", "Hair.ShadowRaster"));
        bindings.clipmaps = builder.ShaderResource(m_context.clipmapInfo).View();
        bindings.shadowCameras = builder.ShaderResource(m_context.compactShadowCameras).View();
        bindings.statistics = builder.ShaderResource(m_context.statistics).View();
        bindings.pageTable = builder.UnorderedAccess(m_context.pageTable,
            org::UavView{ static_cast<std::uint32_t>(org::UAVViewType::Texture2DArrayFull) }).View();
        bindings.staticPages = builder.UnorderedAccess(m_context.staticPhysicalPages).View();
        bindings.dynamicPages = builder.UnorderedAccess(m_context.dynamicPhysicalPages).View();
        if (const auto* buffers = DeclareBuffers()) {
            bindings.frameState = builder.ShaderResource(buffers->frameState).View();
            bindings.shadowClusters = builder.ShaderResource(buffers->shadowClusters).View();
            bindings.shadowMasks = builder.ShaderResource(buffers->shadowClusterMasks).View();
            bindings.poolVertices = builder.ShaderResource(buffers->shadowPoolVertices).View();
            bindings.poolSegmentIds = builder.ShaderResource(buffers->shadowPoolSegmentIds).View();
            bindings.counters = builder.UnorderedAccess(buffers->counters).View();
            bindings.hasBuffers = true;
        }
        return bindings;
    }

    static void Record(const HairShadowRasterBindings& bindings, const HairPassFrame& frame, org::PassRecordContext& recording)
    {
        const std::uint32_t groups = Groups(frame.plan.shadowRasterRows);
        if (!bindings.hasBuffers || !frame.plan.shadows || groups == 0u) return;
        br::render::MiscConstants constants{};
        constants[0] = recording.Index(bindings.frameState);
        constants[1] = recording.Index(bindings.counters);
        constants[2] = recording.Index(bindings.shadowClusters);
        constants[3] = recording.Index(bindings.shadowMasks);
        constants[4] = recording.Index(bindings.clipmaps);
        constants[5] = recording.Index(bindings.shadowCameras);
        constants[6] = recording.Index(bindings.poolVertices);
        constants[7] = recording.Index(bindings.poolSegmentIds);
        constants[8] = recording.Index(bindings.pageTable);
        constants[9] = recording.Index(bindings.staticPages);
        constants[10] = recording.Index(bindings.dynamicPages);
        constants[11] = kRasterGroupsPerRow;
        constants[12] = recording.Index(bindings.statistics);
        br::render::DispatchCompute(recording, bindings.pipeline, constants, (std::min)(groups, kRasterGroupsPerRow),
            (groups + kRasterGroupsPerRow - 1u) / kRasterGroupsPerRow);
    }

private:
    VirtualShadowCasterBuildContext m_context;
};

struct HairVoxelizeBindings {
    br::PipelineState clearPipeline;
    br::PipelineState pipeline;
    org::DeclaredViewToken frameState, counters, visibleClusters, poolVertices, poolSegmentIds, volumes, voxels;
    bool hasBuffers = false;
};

// The frame's density volumes (hairVolume.hlsl): zeroes the voxels the frame's volumes use (HairVolumeClearCS), then
// deposits every segment of the strand pool into its instance's volume (HairVoxelizeCS), one thread per pool row over
// the most rows the frame can use. It records only in a frame the runtime planned volumes for: on the layered path,
// with an instance in view.
class HairVoxelizePass final : public HairPass<HairVoxelizePass, HairVoxelizeBindings> {
public:
    using HairPass::HairPass;

    void Revision(std::vector<uint64_t>& out) const
    {
        br::render::AppendPipelineRevision(out, m_clearPipeline);
        HairPass::Revision(out);
    }

    HairVoxelizeBindings Declare(org::PassBuilder& builder)
    {
        HairVoxelizeBindings bindings{};
        bindings.clearPipeline = m_clearPipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairVolume.hlsl", L"HairVolumeClearCS", "Hair.VolumeClear"));
        bindings.pipeline = m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairVolume.hlsl", L"HairVoxelizeCS", "Hair.Voxelize"));
        if (const auto* buffers = DeclareBuffers()) {
            bindings.frameState = builder.ShaderResource(buffers->frameState).View();
            bindings.visibleClusters = builder.ShaderResource(buffers->visibleClusters).View();
            bindings.poolVertices = builder.ShaderResource(buffers->poolVertices).View();
            bindings.poolSegmentIds = builder.ShaderResource(buffers->poolSegmentIds).View();
            bindings.volumes = builder.ShaderResource(buffers->volumes).View();
            bindings.counters = builder.UnorderedAccess(buffers->counters).View();
            bindings.voxels = builder.UnorderedAccess(buffers->volumeVoxels).View();
            bindings.hasBuffers = true;
        }
        return bindings;
    }

    static void Record(const HairVoxelizeBindings& bindings, const HairPassFrame& frame, org::PassRecordContext& recording)
    {
        const std::uint32_t groups = Groups(frame.plan.rasterRows);
        if (!bindings.hasBuffers || frame.plan.volumeVoxels == 0u || groups == 0u) return;
        br::render::MiscConstants constants{};
        constants[0] = recording.Index(bindings.frameState);
        constants[1] = recording.Index(bindings.counters);
        constants[2] = recording.Index(bindings.visibleClusters);
        constants[3] = recording.Index(bindings.poolVertices);
        constants[4] = recording.Index(bindings.poolSegmentIds);
        constants[5] = recording.Index(bindings.volumes);
        constants[6] = recording.Index(bindings.voxels);
        constants[7] = kRasterGroupsPerRow;
        constants[8] = frame.plan.volumeVoxels;
        const std::uint32_t clearGroups = Groups(frame.plan.volumeVoxels);
        br::render::DispatchCompute(recording, bindings.clearPipeline, constants, (std::min)(clearGroups, kRasterGroupsPerRow),
            (clearGroups + kRasterGroupsPerRow - 1u) / kRasterGroupsPerRow);
        recording.UavBarrier();
        br::render::DispatchCompute(recording, bindings.pipeline, constants, (std::min)(groups, kRasterGroupsPerRow),
            (groups + kRasterGroupsPerRow - 1u) / kRasterGroupsPerRow);
    }

private:
    br::PipelineState m_clearPipeline;
};

struct HairRasterBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken frameState, counters, visibleClusters, poolVertices, poolSegmentIds, visibility, visibilityClear,
        coverage, coverageClear, layer1, layer1Clear, layer2, layer2Clear, poolColors;
    // The culled rasterizer's (hairRasterCulled.hlsl).
    br::PipelineState itemsPipeline, updatePipeline, latePipeline, classifyPipeline, argsPipeline, initPipeline, hintPipeline;
    br::CommandSignatureState signature;
    org::ResourceBindingToken argumentsBinding;
    org::DeclaredViewToken coverageFar, coverageFarClear, occlusion, occlusionCoarse, occlusionCoarseClear, tileFlags, splitGuess,
        rasterItems, poolLate, rasterArgs, clusterSeen, occlusionHint;
    std::uint32_t width = 0u; // the declared target's size
    std::uint32_t height = 0u;
    bool hasBuffers = false;
    bool hasLayers = false; // the layered path's targets exist
};

// The root constants hairRasterCulled.hlsl's entry points share: each reads the ones its pass declared.
br::render::MiscConstants CulledRasterConstants(const HairRasterBindings& bindings, org::PassRecordContext& recording, bool classify)
{
    br::render::MiscConstants constants{};
    constants[0] = recording.Index(bindings.frameState);
    constants[1] = recording.Index(bindings.counters);
    constants[2] = recording.Index(bindings.visibleClusters);
    constants[3] = recording.Index(bindings.poolVertices);
    constants[4] = recording.Index(bindings.poolSegmentIds);
    constants[6] = bindings.width;
    constants[7] = bindings.height;
    constants[8] = kRasterGroupsPerRow;
    constants[14] = recording.Index(bindings.occlusion);
    constants[15] = recording.Index(bindings.occlusionCoarse);
    constants[16] = recording.Index(bindings.tileFlags);
    constants[17] = recording.Index(bindings.splitGuess);
    constants[18] = recording.Index(bindings.rasterItems);
    constants[19] = recording.Index(bindings.poolLate);
    constants[22] = recording.Index(bindings.occlusionHint);
    if (classify) {
        constants[20] = recording.Index(bindings.rasterArgs);
    } else {
        constants[5] = recording.Index(bindings.visibility);
        constants[9] = recording.Index(bindings.coverage);
        constants[10] = recording.Index(bindings.layer1);
        constants[11] = recording.Index(bindings.layer2);
        constants[12] = recording.Index(bindings.poolColors);
        constants[13] = recording.Index(bindings.coverageFar);
        constants[21] = recording.Index(bindings.clusterSeen);
    }
    return constants;
}

// The culled rasterizer's first half (hairRasterCulled.hlsl): sorts the chunks of the pool's segments into the first
// pass's items and the rows' late masks (HairClassifyCS), sizes the first pass (HairRasterArgsCS), and sets the hidden
// and split depths of the tiles hair touches (HairOcclusionInitCS). It records only while the culled rasterizer draws
// the layered path.
class HairClassifyPass final : public HairPass<HairClassifyPass, HairRasterBindings> {
public:
    using HairPass::HairPass;

    void Revision(std::vector<uint64_t>& out) const
    {
        br::render::AppendPipelineRevision(out, m_argsPipeline);
        br::render::AppendPipelineRevision(out, m_initPipeline);
        HairPass::Revision(out);
    }

    HairRasterBindings Declare(org::PassBuilder& builder)
    {
        HairRasterBindings bindings{};
        bindings.classifyPipeline = m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairRasterCulled.hlsl", L"HairClassifyCS", "Hair.Classify"));
        bindings.argsPipeline = m_argsPipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairRasterCulled.hlsl", L"HairRasterArgsCS", "Hair.RasterArgs"));
        bindings.initPipeline = m_initPipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairRasterCulled.hlsl", L"HairOcclusionInitCS", "Hair.OcclusionInit"));
        builder.ShaderResource(Builtin::PrimaryCamera::VisibilityTexture, Builtin::CameraBuffer);
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        const auto* buffers = DeclareBuffers();
        if (buffers && buffers->occlusion) {
            bindings.frameState = builder.ShaderResource(buffers->frameState).View();
            bindings.visibleClusters = builder.ShaderResource(buffers->visibleClusters).View();
            bindings.poolVertices = builder.ShaderResource(buffers->poolVertices).View();
            bindings.poolSegmentIds = builder.ShaderResource(buffers->poolSegmentIds).View();
            bindings.counters = builder.UnorderedAccess(buffers->counters).View();
            bindings.occlusion = builder.UnorderedAccess(buffers->occlusion).View();
            bindings.occlusionCoarse = builder.UnorderedAccess(buffers->occlusionCoarse).View();
            bindings.tileFlags = builder.UnorderedAccess(buffers->tileFlags).View();
            bindings.splitGuess = builder.UnorderedAccess(buffers->splitGuess).View();
            bindings.rasterItems = builder.UnorderedAccess(buffers->rasterItems).View();
            bindings.poolLate = builder.UnorderedAccess(buffers->poolLate).View();
            bindings.rasterArgs = builder.UnorderedAccess(buffers->rasterArgs).View();
            bindings.occlusionHint = builder.UnorderedAccess(buffers->occlusionHint).View();
            bindings.width = buffers->visibilityWidth;
            bindings.height = buffers->visibilityHeight;
            bindings.hasBuffers = true;
        }
        return bindings;
    }

    static void Record(const HairRasterBindings& bindings, const HairPassFrame& frame, org::PassRecordContext& recording)
    {
        const std::uint32_t groups = Groups(frame.plan.rasterRows);
        if (!bindings.hasBuffers || !frame.plan.rasterCulled || frame.plan.instances.empty() || groups == 0u) return;
        const auto constants = CulledRasterConstants(bindings, recording, true);
        br::render::DispatchCompute(recording, bindings.classifyPipeline, constants, (std::min)(groups, kRasterGroupsPerRow),
            (groups + kRasterGroupsPerRow - 1u) / kRasterGroupsPerRow);
        recording.UavBarrier();
        br::render::DispatchCompute(recording, bindings.argsPipeline, constants, 1u);
        br::render::DispatchCompute(recording, bindings.initPipeline, constants,
            (bindings.width + HAIR_RASTER_TILE_SIZE - 1u) / HAIR_RASTER_TILE_SIZE, (bindings.height + HAIR_RASTER_TILE_SIZE - 1u) / HAIR_RASTER_TILE_SIZE);
    }

private:
    br::PipelineState m_argsPipeline;
    br::PipelineState m_initPipeline;
};

// Clears the hair visibility target and rasterizes the pool's segments into it against the frame's opaque visibility
// buffer (HairRasterCS): one thread per pool row, over the most rows the frame can use. On the layered path it also
// clears and fills the two further layers, always accumulates the coverage target, and marks in the shading buffer the
// vertices the shade pass has to light.
class HairRasterPass final : public HairPass<HairRasterPass, HairRasterBindings> {
public:
    using HairPass::HairPass;

    void Revision(std::vector<uint64_t>& out) const
    {
        br::render::AppendPipelineRevision(out, m_itemsPipeline);
        br::render::AppendPipelineRevision(out, m_updatePipeline);
        br::render::AppendPipelineRevision(out, m_latePipeline);
        br::render::AppendPipelineRevision(out, m_hintPipeline);
        HairPass::Revision(out);
    }

    HairRasterBindings Declare(org::PassBuilder& builder)
    {
        HairRasterBindings bindings{};
        bindings.pipeline = m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairRaster.hlsl", L"HairRasterCS", "Hair.Raster"));
        // The culled rasterizer's second half: the first pass over the items, the tile depths, the late pass.
        bindings.itemsPipeline = m_itemsPipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairRasterCulled.hlsl", L"HairRasterItemsCS", "Hair.RasterItems"));
        bindings.updatePipeline = m_updatePipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairRasterCulled.hlsl", L"HairOcclusionUpdateCS", "Hair.OcclusionUpdate"));
        bindings.latePipeline = m_latePipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairRasterCulled.hlsl", L"HairRasterLateCS", "Hair.RasterLate"));
        bindings.hintPipeline = m_hintPipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairRasterCulled.hlsl", L"HairOcclusionHintCS", "Hair.OcclusionHint"));
        bindings.signature = br::render::DeclareCommandSignature(br::render::RawDispatchCommandSignature());
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
            const auto coverage = builder.UnorderedAccessClear(buffers->coverage);
            bindings.coverage = coverage.View(0);
            bindings.coverageClear = coverage.View(1);
            if (buffers->layer1 && buffers->layer2) {
                const auto layer1 = builder.UnorderedAccessClear(buffers->layer1);
                bindings.layer1 = layer1.View(0);
                bindings.layer1Clear = layer1.View(1);
                const auto layer2 = builder.UnorderedAccessClear(buffers->layer2);
                bindings.layer2 = layer2.View(0);
                bindings.layer2Clear = layer2.View(1);
                // The marks of the vertices the shade pass has to light.
                bindings.poolColors = builder.UnorderedAccess(buffers->poolColors).View();
                const auto coverageFar = builder.UnorderedAccessClear(buffers->coverageFar);
                bindings.coverageFar = coverageFar.View(0);
                bindings.coverageFarClear = coverageFar.View(1);
                bindings.occlusion = builder.UnorderedAccess(buffers->occlusion).View();
                const auto occlusionCoarse = builder.UnorderedAccessClear(buffers->occlusionCoarse);
                bindings.occlusionCoarse = occlusionCoarse.View(0);
                bindings.occlusionCoarseClear = occlusionCoarse.View(1);
                bindings.tileFlags = builder.UnorderedAccess(buffers->tileFlags).View();
                bindings.splitGuess = builder.UnorderedAccess(buffers->splitGuess).View();
                bindings.rasterItems = builder.UnorderedAccess(buffers->rasterItems).View();
                bindings.poolLate = builder.UnorderedAccess(buffers->poolLate).View();
                bindings.clusterSeen = builder.UnorderedAccess(buffers->clusterSeen).View();
                bindings.occlusionHint = builder.UnorderedAccess(buffers->occlusionHint).View();
                bindings.argumentsBinding = builder.IndirectArguments(buffers->rasterArgs);
                bindings.hasLayers = true;
            }
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
        br::render::ClearUavUint(recording, bindings.coverage, bindings.coverageClear, 0u);
        const bool layered = frame.plan.renderPath == HairRenderPath::Layered;
        if (layered && !bindings.hasLayers) return; // the targets are made in this frame's update: declared from the next
        if (layered) {
            br::render::ClearUavUint(recording, bindings.layer1, bindings.layer1Clear, 0xFFFFFFFFu);
            br::render::ClearUavUint(recording, bindings.layer2, bindings.layer2Clear, 0xFFFFFFFFu);
            br::render::ClearUavUint(recording, bindings.coverageFar, bindings.coverageFarClear, 0u);
        }
        const std::uint32_t groups = Groups(frame.plan.rasterRows);
        if (frame.plan.instances.empty() || groups == 0u) return;
        if (layered && frame.plan.rasterCulled) {
            const auto culled = CulledRasterConstants(bindings, recording, false);
            const std::uint32_t tilesX = (bindings.width + HAIR_RASTER_TILE_SIZE - 1u) / HAIR_RASTER_TILE_SIZE;
            const std::uint32_t tilesY = (bindings.height + HAIR_RASTER_TILE_SIZE - 1u) / HAIR_RASTER_TILE_SIZE;
            br::render::DispatchComputeIndirect(recording, bindings.itemsPipeline, culled, bindings.signature, bindings.argumentsBinding);
            recording.UavBarrier();
            // The tile depths are an atomic maximum over each tile's pixels.
            br::render::ClearUavUint(recording, bindings.occlusionCoarse, bindings.occlusionCoarseClear, 0u);
            br::render::DispatchCompute(recording, bindings.updatePipeline, culled, tilesX, tilesY);
            recording.UavBarrier();
            br::render::DispatchCompute(recording, bindings.hintPipeline, culled, (tilesX + 7u) / 8u, (tilesY + 7u) / 8u);
            br::render::DispatchCompute(recording, bindings.latePipeline, culled, (std::min)(groups, kRasterGroupsPerRow),
                (groups + kRasterGroupsPerRow - 1u) / kRasterGroupsPerRow);
            return;
        }
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
        constants[9] = recording.Index(bindings.coverage);
        if (layered) {
            constants[10] = recording.Index(bindings.layer1);
            constants[11] = recording.Index(bindings.layer2);
            constants[12] = recording.Index(bindings.poolColors);
        }
        br::render::DispatchCompute(recording, bindings.pipeline, constants, (std::min)(groups, kRasterGroupsPerRow),
            (groups + kRasterGroupsPerRow - 1u) / kRasterGroupsPerRow);
    }

private:
    br::PipelineState m_itemsPipeline;
    br::PipelineState m_updatePipeline;
    br::PipelineState m_latePipeline;
    br::PipelineState m_hintPipeline;
};

struct HairResolveBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken visibility, visibleClusters, instances, poolVertices, counters, probe, compositeInfo;
    org::DeclaredViewToken linearDepth, projectedDepth, deviceDepth;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    bool hasBuffers = false;
    HairRenderPath path = HairRenderPath::Surface; // the probe's: the path it records on
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
        if (!bindings.hasBuffers || frame.plan.instances.empty() || frame.plan.renderPath != HairRenderPath::Surface) return;
        auto constants = ResolveConstants(bindings, recording);
        constants[1] = recording.Index(bindings.visibleClusters);
        constants[2] = recording.Index(bindings.instances);
        constants[3] = recording.Index(bindings.poolVertices);
        constants[9] = recording.Index(bindings.counters);
        br::render::DispatchCompute(recording, bindings.pipeline, constants, (bindings.width + 7u) / 8u, (bindings.height + 7u) / 8u);
    }
};

// hair.probe: copies what the frame's targets hold at hair pixels into the probe buffer (HairProbeCS), in the frame the
// runtime asked for one. One thread for every HAIR_PROBE_STRIDE-th pixel each way. There is one after the resolve, which
// records on the surface path, and one after the composite, which records on the layered path and also copies what the
// composite decided for the pixel.
class HairProbePass final : public HairPass<HairProbePass, HairResolveBindings> {
public:
    HairProbePass(std::shared_ptr<HairRuntime> runtime, HairRenderPath path) : HairPass(std::move(runtime)), m_path(path) {}

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
            bindings.path = m_path;
            if (m_path == HairRenderPath::Layered) {
                if (buffers->compositeInfo) bindings.compositeInfo = builder.ShaderResource(buffers->compositeInfo).View();
                else bindings.hasBuffers = false;
            }
        }
        return bindings;
    }

    static void Record(const HairResolveBindings& bindings, const HairPassFrame& frame, org::PassRecordContext& recording)
    {
        if (!bindings.hasBuffers || !frame.plan.probe || frame.plan.renderPath != bindings.path) return;
        auto constants = ResolveConstants(bindings, recording);
        constants[9] = recording.Index(bindings.probe);
        constants[10] = bindings.path == HairRenderPath::Layered ? recording.Index(bindings.compositeInfo) : HAIR_INVALID_INDEX;
        constexpr std::uint32_t pixelsPerGroup = 8u * HAIR_PROBE_STRIDE;
        br::render::DispatchCompute(recording, bindings.pipeline, constants, (bindings.width + pixelsPerGroup - 1u) / pixelsPerGroup,
            (bindings.height + pixelsPerGroup - 1u) / pixelsPerGroup);
    }

private:
    HairRenderPath m_path;
};

struct HairShadeListBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken frameState, counters, poolColors, shadeList;
    bool hasBuffers = false;
};

// The layered path's shade list (HairShadeListCS): one thread per pool row gathers the rows the rasterizer marked into
// the list the shade pass lights, over the most rows the frame can use.
class HairShadeListPass final : public HairPass<HairShadeListPass, HairShadeListBindings> {
public:
    using HairPass::HairPass;

    HairShadeListBindings Declare(org::PassBuilder& builder)
    {
        HairShadeListBindings bindings{};
        bindings.pipeline = m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairShadeList.hlsl", L"HairShadeListCS", "Hair.ShadeList"));
        if (const auto* buffers = DeclareBuffers()) {
            bindings.frameState = builder.ShaderResource(buffers->frameState).View();
            bindings.poolColors = builder.ShaderResource(buffers->poolColors).View();
            bindings.counters = builder.UnorderedAccess(buffers->counters).View();
            bindings.shadeList = builder.UnorderedAccess(buffers->shadeList).View();
            bindings.hasBuffers = true;
        }
        return bindings;
    }

    static void Record(const HairShadeListBindings& bindings, const HairPassFrame& frame, org::PassRecordContext& recording)
    {
        const std::uint32_t groups = Groups(frame.plan.rasterRows);
        if (!bindings.hasBuffers || frame.plan.renderPath != HairRenderPath::Layered || frame.plan.instances.empty() || groups == 0u) return;
        br::render::MiscConstants constants{};
        constants[0] = recording.Index(bindings.frameState);
        constants[1] = recording.Index(bindings.counters);
        constants[2] = recording.Index(bindings.poolColors);
        constants[3] = recording.Index(bindings.shadeList);
        constants[4] = kRasterGroupsPerRow;
        br::render::DispatchCompute(recording, bindings.pipeline, constants, (std::min)(groups, kRasterGroupsPerRow),
            (groups + kRasterGroupsPerRow - 1u) / kRasterGroupsPerRow);
    }
};

struct HairShadeBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken frameState, counters, visibleClusters, poolVertices, poolSegmentIds, instances, poolColors, shadeList,
        volumes, voxels, scatterTables, shadowStaticPages;
    bool hasShadowStaticPages = false;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    bool shadowMaps = false;     // the virtual shadow map is built and declared
    bool punctualLights = false;
    bool hasBuffers = false;
};

struct HairShadeFrame {
    HairPassFrame pass;
    bool environment = false; // the frame has image-based lighting
    bool operator==(const HairShadeFrame&) const = default;
};

// The layered path's shading (HairShadeCS): one thread per row of the shade list, the pool rows the rasterizer marked
// this frame (the vertices of the segments that reached some pixel's layers). Each is lit with hair's own light loop,
// and its colour written into the shading buffer. The GPU knows how long the list is; the dispatch covers the most rows
// the frame's pool can use, and the groups past the list's end return at once. The declarations are those of a forward
// lighting pass (DeepVisibilityResolvePass): the lights, the light grid, the environment and, while it is built, the
// virtual shadow map.
class HairShadePass final : public org::TypedRenderGraphPass<HairShadePass, HairShadeFrame, HairShadeBindings>,
                            public org::IDynamicDeclaredResources {
public:
    HairShadePass(std::shared_ptr<HairRuntime> runtime, std::shared_ptr<const HairExtension::ShadowMap> shadowMap)
        : m_runtime(std::move(runtime)), m_shadowMap(std::move(shadowMap)) {}
    void Initialize() {}

    bool DeclaredResourcesChanged() const override { return m_declared != m_runtime->GpuBuffers(); }

    void Revision(std::vector<uint64_t>& out) const
    {
        br::render::AppendPipelineRevision(out, m_pipeline);
        AppendPlanRevision(out, m_runtime->FramePlan());
    }

    HairShadeBindings Declare(org::PassBuilder& builder)
    {
        HairShadeBindings bindings{};
        bindings.pipeline = m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairShade.hlsl", L"HairShadeCS", "Hair.Shade"));
        // Whether the virtual shadow map is built, not just shadows on: stochastic lighting replaces it. The renderer
        // publishes this at graph build, and a change rebuilds the graph.
        bindings.shadowMaps = br::extensions::ReadBoolSetting("virtualShadowMapsActive");
        bindings.punctualLights = br::extensions::ReadBoolSetting("enablePunctualLighting");
        builder.ShaderResource(Builtin::Light::BufferGroup, Builtin::Light::ActiveLightIndices, Builtin::Light::InfoBuffer,
            Builtin::Light::DirectionalLightCascadeBuffer, Builtin::Light::GridCells, Builtin::Light::GridIndices,
            Builtin::Environment::InfoBuffer, Builtin::CameraBuffer, Builtin::Noise::BlueNoise2D);
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        if (bindings.shadowMaps) {
            builder.ShaderResource(Builtin::Shadows::CLodClipmapInfo, Builtin::Shadows::CLodDirectionalPageViewInfo,
                Builtin::Shadows::CLodPageMetadata, Builtin::Shadows::CLodPhysicalPages, Builtin::Shadows::CLodCompactMainCamera,
                Builtin::Shadows::CLodCompactShadowCameras);
            builder.ShaderResource(Builtin::Shadows::CLodPageTable,
                org::SrvView{ static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull) });
            // The cached pages, for what shadows a vertex from inside its own groom's box (hairShade.hlsl).
            if (m_shadowMap && m_shadowMap->staticPages) {
                bindings.shadowStaticPages = builder.ShaderResource(m_shadowMap->staticPages).View();
                bindings.hasShadowStaticPages = true;
            }
        }
        m_declared = m_runtime->GpuBuffers();
        if (const auto* buffers = m_declared && m_declared->visibility ? m_declared.get() : nullptr) {
            bindings.frameState = builder.ShaderResource(buffers->frameState).View();
            bindings.visibleClusters = builder.ShaderResource(buffers->visibleClusters).View();
            bindings.poolVertices = builder.ShaderResource(buffers->poolVertices).View();
            bindings.poolSegmentIds = builder.ShaderResource(buffers->poolSegmentIds).View();
            bindings.instances = builder.ShaderResource(buffers->instances).View();
            bindings.counters = builder.UnorderedAccess(buffers->counters).View();
            bindings.poolColors = builder.UnorderedAccess(buffers->poolColors).View();
            bindings.shadeList = builder.ShaderResource(buffers->shadeList).View();
            // The density volumes the vertices march, and the instances' scatter tables.
            bindings.volumes = builder.ShaderResource(buffers->volumes).View();
            bindings.voxels = builder.ShaderResource(buffers->volumeVoxels).View();
            bindings.scatterTables = builder.ShaderResource(buffers->scatterTables).View();
            bindings.width = buffers->visibilityWidth;
            bindings.height = buffers->visibilityHeight;
            bindings.hasBuffers = true;
        }
        return bindings;
    }

    HairShadeFrame Prepare(const HairShadeBindings& bindings, const org::PassPrepareContext& preparation) const
    {
        HairShadeFrame frame;
        if (!bindings.hasBuffers) return frame;
        frame.pass.plan = m_runtime->FramePlan();
        if (const auto* context = preparation.preparationData ? preparation.preparationData->Get<UpdateContext>() : nullptr) {
            frame.environment = (context->globalPSOFlags & PSOFlags::PSO_IMAGE_BASED_LIGHTING) != 0u;
        }
        return frame;
    }

    static void Record(const HairShadeBindings& bindings, const HairShadeFrame& frame, org::PassRecordContext& recording)
    {
        const HairFramePlan& plan = frame.pass.plan;
        const std::uint32_t groups = Groups(plan.rasterRows);
        if (!bindings.hasBuffers || plan.renderPath != HairRenderPath::Layered || plan.instances.empty() || groups == 0u) return;
        br::render::MiscConstants constants{};
        constants[0] = recording.Index(bindings.frameState);
        constants[1] = recording.Index(bindings.counters);
        constants[2] = recording.Index(bindings.visibleClusters);
        constants[3] = recording.Index(bindings.poolVertices);
        constants[4] = recording.Index(bindings.poolSegmentIds);
        constants[5] = recording.Index(bindings.instances);
        constants[6] = recording.Index(bindings.poolColors);
        constants[7] = bindings.width;
        constants[8] = bindings.height;
        constants[9] = kRasterGroupsPerRow;
        constants[10] = frame.environment ? 1u : 0u; // HAIR_SHADE_FLAG_ENVIRONMENT
        constants[11] = recording.Index(bindings.shadeList);
        constants[12] = recording.Index(bindings.volumes);
        constants[13] = recording.Index(bindings.voxels);
        constants[14] = recording.Index(bindings.scatterTables);
        constants[15] = bindings.hasShadowStaticPages ? recording.Index(bindings.shadowStaticPages) : HAIR_INVALID_INDEX;
        constants[MiscShadowingMode] = static_cast<std::uint32_t>(bindings.shadowMaps ? ShadowingMode::VirtualShadowMaps : ShadowingMode::None);
        constants[MiscEnablePunctualLights] = bindings.punctualLights ? 1u : 0u;
        br::render::DispatchCompute(recording, bindings.pipeline, constants, (std::min)(groups, kRasterGroupsPerRow),
            (groups + kRasterGroupsPerRow - 1u) / kRasterGroupsPerRow);
    }

private:
    std::shared_ptr<HairRuntime> m_runtime;
    std::shared_ptr<const HairExtension::ShadowMap> m_shadowMap;
    std::shared_ptr<const HairGpuBuffers> m_declared;
    br::PipelineState m_pipeline;
};

struct HairCompositeBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken visibility, layer1, layer2, coverage, coverageFar, occlusion, visibleClusters, frameState, poolVertices, poolColors, counters,
        compositeInfo, reactiveMask, reactiveMaskClear, projectedDepth;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    bool hasBuffers = false;
};

// The layered path's composite (HairCompositeCS): blends each pixel's hair layers over the scene colour, gives the
// pixels hair mostly covers hair's depth and motion vector, and writes the upscaler's reactive mask. One thread a pixel.
class HairCompositePass final : public HairPass<HairCompositePass, HairCompositeBindings> {
public:
    using HairPass::HairPass;

    HairCompositeBindings Declare(org::PassBuilder& builder)
    {
        HairCompositeBindings bindings{};
        bindings.pipeline = m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"HairShaders/hairComposite.hlsl", L"HairCompositeCS", "Hair.Composite"));
        builder.ShaderResource(Builtin::CameraBuffer);
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        builder.UnorderedAccess(Builtin::Color::HDRColorTarget, Builtin::Surface::Motion);
        bindings.projectedDepth = builder.UnorderedAccess(Builtin::PrimaryCamera::ProjectedDepthTexture).View();
        const auto* buffers = DeclareBuffers();
        if (buffers && buffers->layer1 && buffers->layer2) {
            bindings.visibility = builder.ShaderResource(buffers->visibility).View();
            bindings.layer1 = builder.ShaderResource(buffers->layer1).View();
            bindings.layer2 = builder.ShaderResource(buffers->layer2).View();
            bindings.coverage = builder.ShaderResource(buffers->coverage).View();
            bindings.coverageFar = builder.ShaderResource(buffers->coverageFar).View();
            bindings.occlusion = builder.ShaderResource(buffers->occlusion).View();
            bindings.visibleClusters = builder.ShaderResource(buffers->visibleClusters).View();
            bindings.frameState = builder.ShaderResource(buffers->frameState).View();
            bindings.poolVertices = builder.ShaderResource(buffers->poolVertices).View();
            bindings.poolColors = builder.ShaderResource(buffers->poolColors).View();
            bindings.counters = builder.UnorderedAccess(buffers->counters).View();
            bindings.compositeInfo = builder.UnorderedAccess(buffers->compositeInfo).View();
            const auto reactiveMask = builder.UnorderedAccessClear(buffers->reactiveMask);
            bindings.reactiveMask = reactiveMask.View(0);
            bindings.reactiveMaskClear = reactiveMask.View(1);
            bindings.width = buffers->visibilityWidth;
            bindings.height = buffers->visibilityHeight;
            bindings.hasBuffers = true;
        }
        return bindings;
    }

    static void Record(const HairCompositeBindings& bindings, const HairPassFrame& frame, org::PassRecordContext& recording)
    {
        if (!bindings.hasBuffers || frame.plan.renderPath != HairRenderPath::Layered) return;
        if (frame.plan.instances.empty()) {
            // No hair this frame: the upscaler still reads the mask.
            if (frame.plan.reactiveMask) br::render::ClearUavFloat(recording, bindings.reactiveMask, bindings.reactiveMaskClear, 0.0f);
            return;
        }
        br::render::MiscConstants constants{};
        constants[0] = recording.Index(bindings.visibility);
        constants[1] = recording.Index(bindings.visibleClusters);
        constants[2] = recording.Index(bindings.frameState);
        constants[3] = recording.Index(bindings.poolVertices);
        constants[4] = recording.Index(bindings.layer1);
        constants[5] = recording.Index(bindings.layer2);
        constants[6] = recording.Index(bindings.coverage);
        constants[7] = bindings.width;
        constants[8] = bindings.height;
        constants[9] = recording.Index(bindings.counters);
        constants[10] = recording.Index(bindings.poolColors);
        constants[11] = recording.Index(bindings.compositeInfo);
        constants[12] = recording.Index(bindings.reactiveMask);
        constants[13] = recording.Index(bindings.projectedDepth);
        constants[14] = recording.Index(bindings.coverageFar);
        constants[15] = recording.Index(bindings.occlusion);
        br::render::DispatchCompute(recording, bindings.pipeline, constants, (bindings.width + 7u) / 8u, (bindings.height + 7u) / 8u);
    }
};

struct HairDebugOverlayBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken visibility, coverage, coverageFar, visibleClusters, instances, clusters, strands, target, compositeInfo, reactiveMask,
        volumes, voxels;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    bool hasBuffers = false;
    bool hasCompositeInfo = false;
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
            bindings.coverage = builder.ShaderResource(buffers->coverage).View();
            bindings.visibleClusters = builder.ShaderResource(buffers->visibleClusters).View();
            bindings.instances = builder.ShaderResource(buffers->instances).View();
            bindings.clusters = builder.ShaderResource(buffers->clusters).View();
            bindings.strands = builder.ShaderResource(buffers->strands).View();
            bindings.target = builder.RenderTarget(org::ResourceIdentifier{ Builtin::PresentationColor }).View();
            bindings.volumes = builder.ShaderResource(buffers->volumes).View();
            bindings.voxels = builder.ShaderResource(buffers->volumeVoxels).View();
            if (buffers->compositeInfo) {
                bindings.compositeInfo = builder.ShaderResource(buffers->compositeInfo).View();
                bindings.reactiveMask = builder.ShaderResource(buffers->reactiveMask).View();
                bindings.coverageFar = builder.ShaderResource(buffers->coverageFar).View();
                bindings.hasCompositeInfo = true;
            }
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
        // The views from HairDebugView::LobeR on are the shade pass's, but for the density volumes' projection: nothing
        // to draw over the frame.
        const bool shadeView = frame.plan.debugView >= HairDebugView::LobeR && frame.plan.debugView != HairDebugView::Density;
        if (!bindings.hasBuffers || frame.plan.debugView == HairDebugView::Off || shadeView || frame.outputWidth == 0u ||
            frame.outputHeight == 0u) {
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
        constants[10] = recording.Index(bindings.coverage);
        constants[11] = bindings.hasCompositeInfo && frame.plan.renderPath == HairRenderPath::Layered
            ? recording.Index(bindings.compositeInfo) : HAIR_INVALID_INDEX;
        if (bindings.hasCompositeInfo) constants[12] = recording.Index(bindings.reactiveMask);
        constants[13] = recording.Index(bindings.volumes);
        constants[14] = recording.Index(bindings.voxels);
        constants[15] = frame.plan.volumeVoxels != 0u ? frame.plan.volumeRows : 0u;
        constants[16] = bindings.hasCompositeInfo ? recording.Index(bindings.coverageFar) : HAIR_INVALID_INDEX;
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

HairExtension::HairExtension(std::shared_ptr<HairRuntime> runtime)
    : m_runtime(std::move(runtime)), m_shadowMap(std::make_shared<ShadowMap>()) {}

void HairExtension::GatherStructuralPasses(org::RenderGraph& rg, std::vector<org::ExternalPassDesc>& out)
{
    // Before culling, where procedural wind writes its palettes. Selection and evaluation need the main view's camera and
    // the entities' transforms, and nothing of the frame's opaque geometry. Evaluation is last: it draws the strands
    // selection chose from the guide state the two simulation passes leave.
    const auto beforeCulling = org::ExternalInsertPoint::Before("CLodOpaque::HierarchicalCullingPass1");
    out.push_back(org::ExternalPassDesc::Compute("Hair::Select", std::make_shared<HairSelectPass>(m_runtime)).At(beforeCulling));
    out.push_back(org::ExternalPassDesc::Compute("Hair::RootCheck", std::make_shared<HairRootCheckPass>(m_runtime)).At(beforeCulling));
    out.push_back(org::ExternalPassDesc::Compute("Hair::Simulate", std::make_shared<HairSimulatePass>(m_runtime)).At(beforeCulling));
    out.push_back(org::ExternalPassDesc::Compute("Hair::GuideLevels", std::make_shared<HairGuideLevelsPass>(m_runtime)).At(beforeCulling));
    out.push_back(org::ExternalPassDesc::Compute("Hair::Evaluate", std::make_shared<HairEvaluatePass>(m_runtime)).At(beforeCulling));
    // The density volumes are filled from the pool evaluation leaves, and need nothing else of the frame.
    auto afterEvaluate = beforeCulling;
    afterEvaluate.AlsoAfter("Hair::Evaluate");
    out.push_back(org::ExternalPassDesc::Compute("Hair::Voxelize", std::make_shared<HairVoxelizePass>(m_runtime)).At(afterEvaluate));

    // The rasterizer tests against the opaque visibility buffer, so it runs once that is complete. Every pass that
    // writes it is placed before MaterialHistogramPass, the first pass to read the finished buffer, so after that pass
    // is after all of them without naming them. It is before material evaluation: a resolve of hair into the canonical
    // surface follows EvaluateMaterialGroupsPass and reads the target.
    auto afterOpaqueVisibility = org::ExternalInsertPoint::After("MaterialHistogramPass");
    afterOpaqueVisibility.AlsoBefore("EvaluateMaterialGroupsPass");
    // The culled rasterizer's first half: it reads the opaque depth too, and the rasterizer draws what it lists.
    auto beforeRaster = afterOpaqueVisibility;
    beforeRaster.AlsoBefore("Hair::Raster");
    out.push_back(org::ExternalPassDesc::Compute("Hair::Classify", std::make_shared<HairClassifyPass>(m_runtime)).At(beforeRaster));
    out.push_back(org::ExternalPassDesc::Compute("Hair::Raster", std::make_shared<HairRasterPass>(m_runtime)).At(afterOpaqueVisibility));
    // The layered path's shade list, from the marks the rasterizer left.
    auto afterRaster = afterOpaqueVisibility;
    afterRaster.AlsoAfter("Hair::Raster");
    out.push_back(org::ExternalPassDesc::Compute("Hair::ShadeList", std::make_shared<HairShadeListPass>(m_runtime)).At(afterRaster));

    // The resolve replaces what material evaluation and the frame's last depth copy wrote at hair pixels, so it follows
    // both. It runs before the linear depth's mips are made from mip 0, and so before everything that reads the frame's
    // depth: virtual shadow page marking, ambient occlusion, light culling, shading and the upscaler.
    auto afterSurface = org::ExternalInsertPoint::After("EvaluateMaterialGroupsPass");
    afterSurface.AlsoAfter("CLodOpaque::LinearDepthCopyPass2").AlsoBefore("CLodOpaque::LinearDepthDownsamplePass2");
    out.push_back(org::ExternalPassDesc::Compute("Hair::Resolve", std::make_shared<HairResolvePass>(m_runtime)).At(afterSurface));
    out.push_back(org::ExternalPassDesc::Compute("Hair::Probe", std::make_shared<HairProbePass>(m_runtime, HairRenderPath::Surface))
        .At(org::ExternalInsertPoint::After("Hair::Resolve")));

    // The layered path, in the Shading epoch: hair is lit per strand vertex and blended over the lit frame, where the
    // renderer's own transparents go: after the opaque frame is lit (the deferred pass also draws the sky) and after the
    // reflections are added to it, when the recipe has them. The epoch order puts it before the upscaler and everything
    // after that. The order against the renderer's other transparents (CLod alpha) is not pinned.
    auto transparentComposite = org::ExternalInsertPoint::After("DeferredShadingPass");
    if (br::extensions::ReadBoolSetting("enableScreenSpaceReflections")) {
        transparentComposite.AlsoAfter("Screen-Space Reflections Pass").AlsoAfter("Specular IBL & SSR Composite Pass");
    }
    if (br::extensions::ReadBoolSetting("enableRayTracedReflections")) {
        transparentComposite.AlsoAfter("Ray Traced Reflections Pass").AlsoAfter("Specular IBL & RT Reflections Composite Pass");
    }
    auto shade = transparentComposite;
    shade.AlsoBefore("Hair::Composite");
    out.push_back(org::ExternalPassDesc::Compute("Hair::Shade", std::make_shared<HairShadePass>(m_runtime, m_shadowMap))
        .At(shade)
        .Epoch(br::render::kShadingEpoch));
    out.push_back(org::ExternalPassDesc::Compute("Hair::Composite", std::make_shared<HairCompositePass>(m_runtime))
        .At(transparentComposite)
        .Epoch(br::render::kShadingEpoch));
    auto afterComposite = transparentComposite;
    afterComposite.AlsoAfter("Hair::Composite");
    out.push_back(org::ExternalPassDesc::Compute("Hair::CompositeProbe", std::make_shared<HairProbePass>(m_runtime, HairRenderPath::Layered))
        .At(afterComposite)
        .Epoch(br::render::kShadingEpoch));

    // A debug view: the Overlay epoch's, not the extension's (Visibility). Before the present pass, which makes the
    // backbuffer's last access.
    auto afterTonemapping = org::ExternalInsertPoint::After("TonemappingPass");
    if (!rg.GetPresentPass().empty()) afterTonemapping.AlsoBefore(rg.GetPresentPass());
    out.push_back(org::ExternalPassDesc::Render("Hair::DebugOverlay", std::make_shared<HairDebugOverlayPass>(m_runtime))
        .At(afterTonemapping)
        .Epoch(br::render::kOverlayEpoch));
}

float HairExtension::GetRequestedDynamicShadowRadius() const
{
    return m_runtime->RequestedDynamicShadowRadius();
}

void HairExtension::OnVirtualShadowCasterRegistered(const std::shared_ptr<VirtualShadowInvalidationQueue>& queue)
{
    m_runtime->SetShadowInvalidationQueue(queue);
}

void HairExtension::GatherVirtualShadowPreparationPasses(const VirtualShadowCasterBuildContext&, VirtualShadowPassBuilder&)
{
    // Nothing before the shadow map invalidates pages: the runtime enqueues a moved groom's bounds from its update.
}

void HairExtension::GatherVirtualShadowRasterPasses(const VirtualShadowCasterBuildContext& context, VirtualShadowPassBuilder& builder)
{
    m_shadowMap->staticPages = context.staticPhysicalPages;
    // Once the shadow map knows the pages it draws this frame: select against them, evaluate what was kept, draw it.
    builder.Add(org::ExternalPassDesc::Compute("Hair::ShadowSelect", std::make_shared<HairShadowSelectPass>(m_runtime, context)));
    builder.Add(org::ExternalPassDesc::Compute("Hair::ShadowEvaluate", std::make_shared<HairEvaluatePass>(m_runtime, true)));
    builder.Add(org::ExternalPassDesc::Compute("Hair::ShadowRaster", std::make_shared<HairShadowRasterPass>(m_runtime, context)));
}

} // namespace br::hair
