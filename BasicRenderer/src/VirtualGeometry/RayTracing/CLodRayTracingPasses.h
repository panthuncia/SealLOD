#pragma once

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>

#include "BuiltinResources.h"
#include "Render/PassBuilders.h"
#include "BasicRenderer/Extensions/PipelineDescs.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Extensions/PassRevision.h"
#include <BasicRenderer/Pipeline/PipelineDesc.h>
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "Runtime/GraphIntegration/Resolvers/ResourceGroupResolver.h"
#include "VirtualGeometry/RayTracing/CLodRayTracingSystem.h"
#include "../shaders/PerPassRootConstants/clodRayTracingSetupRootConstants.h"

// A compute pipeline with ray queries against clusters, which need shader model 6.10.
inline br::ComputePipelineDesc CLodRtComputePipeline(std::wstring path, std::wstring entry, std::string name) {
    br::ComputePipelineDesc desc = br::render::ComputePipeline(std::move(path), std::move(entry), std::move(name));
    desc.shader.target = L"cs_6_10";
    return desc;
}

// Builds the ray tracing scene from the RT view's culling output, in three passes: the CLAS cache, the BLASes, then the
// TLAS. Raytracing passes that run after the last in the frame can use the TLAS. Each stage's scratch is graph memory
// live only in its pass, so the graph can alias the stages' scratch with each other.
enum class CLodRayTracingSceneStage : uint8_t { Clas, Blas, Tlas };

struct CLodRayTracingSceneFrame {
    std::shared_ptr<br::render::CLodRayTracingSystem> system;
    std::vector<uint64_t> state; // Kept: what the system's recording depends on (CLodRayTracingSystem::AppendSceneRevision).
    bool operator==(const CLodRayTracingSceneFrame&) const = default;
};

// How the scene build is recorded (BASICRENDERER_RT_SCENE_AT_EXECUTION):
// - Kept (the default since M6 step 9c): the three stages are recorded ahead with a revision and replayed. Their per-frame
//   values come from the per-frame buffer on the GPU (arena slice, BLAS cache frame); the micromap uploads and the readback
//   into this frame's slice are two small passes recorded at execution (CLodRayTracingSceneOmmPass, CLodRayTracingSceneEndPass).
// - AtExecution (=1, step 9b's default): the three stages are recorded at execution, in a batch of their own.
// - Ahead (=0): recorded ahead without a revision, so their batch is recorded every run and Visibility is not staged.
enum class CLodRayTracingSceneRecording : uint8_t { Ahead, AtExecution, Kept };
inline CLodRayTracingSceneRecording CLodRayTracingSceneRecordingMode() {
    static const CLodRayTracingSceneRecording mode = [] {
        const char* value = std::getenv("BASICRENDERER_RT_SCENE_AT_EXECUTION");
        if (value && value[0] == '0') return CLodRayTracingSceneRecording::Ahead;
        if (value && value[0] == '1') return CLodRayTracingSceneRecording::AtExecution;
        return CLodRayTracingSceneRecording::Kept;
    }();
    return mode;
}

// Every pipeline of the scene build's set, for a kept stage's revision.
inline void AppendScenePipelinesRevision(std::vector<uint64_t>& out, const br::render::CLodRayTracingSystem::ScenePipelines& p) {
    for (const br::PipelineState* pipeline : { &p.clear, &p.resetDraws, &p.count, &p.allocate, &p.addCoarseConsumers, &p.allocateFinish,
             &p.place, &p.writeInstances, &p.cacheInit, &p.cacheCollect, &p.cacheOmmRetire, &p.cacheEvict, &p.cacheAdmit, &p.cacheAssign,
             &p.cacheArgs, &p.cachePack, &p.cacheEnd, &p.ptlasInit, &p.ptlasClaimCells, &p.ptlasAssignCells, &p.ptlasRetire, &p.ptlasFinish,
             &p.bcInit, &p.bcBegin, &p.bcIndex, &p.bcLookup, &p.bcFinish, &p.bcScatter }) {
        br::render::AppendPipelineRevision(out, *pipeline);
    }
}

struct CLodRayTracingSceneBindings {
    br::render::CLodRayTracingSystem::ScenePipelines pipelines;
    org::DeclaredViewToken clusters, sideBand, counter, tlasInstances;
    uint32_t clusterCapacity = 0u;
    CLodRayTracingSceneStage stage = CLodRayTracingSceneStage::Clas;
};

// AtExecution (M6 step 9; the default since step 9b, BASICRENDERER_RT_SCENE_AT_EXECUTION=0 turns it off): the pass is recorded at
// execution, in a batch of its own, with an empty revision. Its Record reads and advances the system's per-frame state (the arena
// slice, the TLAS instance count its readbacks returned, OMM uploads, the readback copies), so a kept recording records it again
// at each submission and reuses the rest of Visibility, and Visibility can be staged. Off, the pass has no revision, so its batch
// is recorded every run and Visibility is not staged.
// Step 9's hang in PathTracePass with this on: ORG dropped RecordAtExecution set on the builder of an extension's pass, so the
// scene build stayed in the RT culling's batch, which was kept and submitted again without recording it. A frozen frame of
// the scene build then rebuilt one transient arena while earlier frames still traced it.
template<CLodRayTracingSceneRecording Mode>
class CLodRayTracingScenePassT
    : public org::TypedRenderGraphPass<CLodRayTracingScenePassT<Mode>, CLodRayTracingSceneFrame, CLodRayTracingSceneBindings> {
    static constexpr bool AtExecution = Mode == CLodRayTracingSceneRecording::AtExecution;
    static constexpr bool Kept = Mode == CLodRayTracingSceneRecording::Kept;
public:
    CLodRayTracingScenePassT(std::shared_ptr<br::render::CLodRayTracingSystem> system, CLodRayTracingSceneStage stage, std::shared_ptr<org::Buffer> clusters,
        std::shared_ptr<org::Buffer> sideBand, std::shared_ptr<org::Buffer> counter, std::shared_ptr<org::ResourceGroup> slabs, uint32_t clusterCapacity)
        : m_system(std::move(system)), m_stage(stage), m_clusters(std::move(clusters)), m_sideBand(std::move(sideBand)), m_counter(std::move(counter)),
          m_slabs(std::move(slabs)), m_clusterCapacity(clusterCapacity) {
    }

    CLodRayTracingSceneBindings Declare(org::PassBuilder& builder) {
        using Stage = CLodRayTracingSceneStage;
        CLodRayTracingSceneBindings bindings{};
        bindings.stage = m_stage;
        const auto make = [&](const wchar_t* entry, const char* name) {
            return br::render::DeclarePipeline(br::render::ComputePipeline(L"Shaders/ClusterLOD/rayTracingSceneBuild.hlsl", entry, name));
        };
        auto& pipelines = bindings.pipelines;
        if (m_stage != Stage::Blas) pipelines.clear = make(L"CLodRtClearCS", "CLod.RayTracing.Clear.PSO");
        if (m_stage == Stage::Clas) {
            pipelines.cacheInit = make(L"CLodRtCacheInitCS", "CLod.RayTracing.CacheInit.PSO");
            pipelines.cacheCollect = make(L"CLodRtCacheCollectCS", "CLod.RayTracing.CacheCollect.PSO");
            pipelines.cacheOmmRetire = make(L"CLodRtCacheOmmRetireCS", "CLod.RayTracing.CacheOmmRetire.PSO");
            pipelines.cacheEvict = make(L"CLodRtCacheEvictCS", "CLod.RayTracing.CacheEvict.PSO");
            pipelines.cacheAdmit = make(L"CLodRtCacheAdmitCS", "CLod.RayTracing.CacheAdmit.PSO");
            pipelines.cacheAssign = make(L"CLodRtCacheAssignCS", "CLod.RayTracing.CacheAssign.PSO");
            pipelines.cacheArgs = make(L"CLodRtCacheArgsCS", "CLod.RayTracing.CacheArgs.PSO");
            pipelines.cachePack = make(L"CLodRtCachePackCS", "CLod.RayTracing.CachePack.PSO");
            pipelines.resetDraws = make(L"CLodRtResetDrawsCS", "CLod.RayTracing.ResetDraws.PSO");
            pipelines.count = make(L"CLodRtCountCS", "CLod.RayTracing.Count.PSO");
        }
        else if (m_stage == Stage::Blas) {
            pipelines.allocate = make(L"CLodRtAllocateCS", "CLod.RayTracing.Allocate.PSO");
            pipelines.addCoarseConsumers = make(L"CLodRtAddCoarseConsumersCS", "CLod.RayTracing.AddCoarseConsumers.PSO");
            pipelines.allocateFinish = make(L"CLodRtAllocateFinishCS", "CLod.RayTracing.AllocateFinish.PSO");
            pipelines.place = make(L"CLodRtPlaceCS", "CLod.RayTracing.Place.PSO");
            pipelines.bcInit = make(L"CLodRtBcInitCS", "CLod.RayTracing.BlasCacheInit.PSO");
            pipelines.bcBegin = make(L"CLodRtBcBeginCS", "CLod.RayTracing.BlasCacheBegin.PSO");
            pipelines.bcIndex = make(L"CLodRtBcIndexCS", "CLod.RayTracing.BlasCacheIndex.PSO");
            pipelines.bcLookup = make(L"CLodRtBcLookupCS", "CLod.RayTracing.BlasCacheLookup.PSO");
            pipelines.bcFinish = make(L"CLodRtBcFinishCS", "CLod.RayTracing.BlasCacheFinish.PSO");
            pipelines.bcScatter = make(L"CLodRtBcScatterCS", "CLod.RayTracing.BlasCacheScatter.PSO");
            if (m_stage == Stage::Blas) pipelines.clear = make(L"CLodRtClearCS", "CLod.RayTracing.Clear.PSO");
        }
        else {
            pipelines.writeInstances = make(L"CLodRtWriteInstancesCS", "CLod.RayTracing.WriteInstances.PSO");
            pipelines.cacheEnd = make(L"CLodRtCacheEndCS", "CLod.RayTracing.CacheEnd.PSO");
            if (m_system->UsesPartitionedTlas()) {
                pipelines.ptlasInit = make(L"CLodRtPtlasInitCS", "CLod.RayTracing.PtlasInit.PSO");
                pipelines.ptlasClaimCells = make(L"CLodRtPtlasClaimCellsCS", "CLod.RayTracing.PtlasClaimCells.PSO");
                pipelines.ptlasAssignCells = make(L"CLodRtPtlasAssignCellsCS", "CLod.RayTracing.PtlasAssignCells.PSO");
                pipelines.ptlasRetire = make(L"CLodRtPtlasRetireCS", "CLod.RayTracing.PtlasRetire.PSO");
                pipelines.ptlasFinish = make(L"CLodRtPtlasFinishCS", "CLod.RayTracing.PtlasFinish.PSO");
            }
        }
        bindings.clusterCapacity = m_clusterCapacity;
        if (m_stage != Stage::Tlas) { // Count and place read the cluster list; the TLAS stage doesn't.
            bindings.clusters = builder.ShaderResource(m_clusters).View();
            bindings.sideBand = builder.ShaderResource(m_sideBand).View(); // Which part portal occurrence reached each cluster.
            bindings.counter = builder.ShaderResource(m_counter).View();
        }
        // What each stage reads and writes, and nothing else: the graph orders the stages, and makes the barriers between
        // them, from it. The system's own buffers it declares itself.
        builder.ConstantBuffer(Builtin::PerFrameBuffer); // The texture image table's index (materialTextureBinding.hlsli).
        switch (m_stage) {
        case Stage::Clas:
            m_system->DeclareSceneClas(builder);
            // Read by the cache's shaders, and by the CLAS build, which builds from the pages' vertices and indices.
            if (m_slabs) builder.AccelerationStructureBuildInput(ResourceGroupResolver(m_slabs));
            builder.ShaderResource(Builtin::PerMeshBuffer, Builtin::PerMeshInstanceBuffer, Builtin::InstanceDrawRecordBuffer, Builtin::PerMaterialDataBuffer,
                Builtin::CLod::MaterialPalettes, Builtin::CLod::AssemblyMaterialBases, Builtin::CLod::MeshMetadata, Builtin::CLod::Groups,
                Builtin::CLod::Segments, Builtin::CLod::GroupPageMap, // The cache finds a group's pages, to build it.
                Builtin::CLod::StreamingRuntimeState, Builtin::CLod::StreamingNonResidentBits); // And builds again only resident groups.
            break;
        case Stage::Blas:
            m_system->DeclareSceneBlas(builder);
            builder.ShaderResource(Builtin::CLod::Groups, Builtin::CLod::Segments, Builtin::CLod::GroupPageMap); // Place finds each cluster's CLAS.
            break;
        case Stage::Tlas:
            bindings.tlasInstances = m_system->DeclareSceneTlas(builder, Kept);
            builder.ShaderResource(Builtin::PerMeshBuffer, Builtin::PerMeshInstanceBuffer, Builtin::InstanceDrawRecordBuffer, Builtin::PerInstanceTransformBuffer,
                Builtin::PerMaterialDataBuffer, Builtin::CLod::MaterialPalettes, Builtin::CLod::AssemblyMaterialBases, Builtin::CLod::MeshMetadata,
                Builtin::CLod::Groups, Builtin::CLod::AssemblyTransforms,
                Builtin::Material::TextureStreamingMetadataBuffer); // The instance check reads alpha textures' residency,
            builder.UnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer); // and keeps them resident at their test mip.
            break;
        }
        if constexpr (AtExecution) builder.RecordAtExecution();
        m_pipelines = bindings.pipelines;
        return bindings;
    }

    // AtExecution: recorded at each run, so only fixed objects name its recording. Kept: the pipelines and the system's state
    // its recording depends on (the first-frame flags, the TLAS row count, micromap state).
    void Revision(std::vector<uint64_t>& out) const requires (AtExecution || Kept) {
        if constexpr (Kept) {
            AppendScenePipelinesRevision(out, m_pipelines);
            const auto frame = MakeFrame();
            out.insert(out.end(), frame.state.begin(), frame.state.end());
        }
    }

    CLodRayTracingSceneFrame Prepare(const CLodRayTracingSceneBindings&, const org::PassPrepareContext&) const { return MakeFrame(); }

    static void Record(const CLodRayTracingSceneBindings& bindings, const CLodRayTracingSceneFrame& frame, org::PassRecordContext& recording) {
        std::scoped_lock lock(frame.system->FrameOperationMutex());
        br::render::CLodRayTracingSystem::SceneInputs inputs{};
        inputs.clusterCapacity = bindings.clusterCapacity;
        inputs.kept = Kept;
        if (bindings.stage == CLodRayTracingSceneStage::Tlas) {
            inputs.tlasInstancesUav = recording.Index(bindings.tlasInstances);
            frame.system->RecordSceneTlas(recording, bindings.pipelines, inputs);
            return;
        }
        inputs.clustersSrv = recording.Index(bindings.clusters);
        inputs.sideBandSrv = recording.Index(bindings.sideBand);
        inputs.counterSrv = recording.Index(bindings.counter);
        if (bindings.stage == CLodRayTracingSceneStage::Clas) frame.system->RecordSceneClas(recording, bindings.pipelines, inputs);
        else frame.system->RecordSceneBlas(recording, bindings.pipelines, inputs);
    }

private:
    CLodRayTracingSceneFrame MakeFrame() const {
        CLodRayTracingSceneFrame frame{ m_system };
        if constexpr (Kept) {
            std::scoped_lock lock(m_system->FrameOperationMutex());
            m_system->AppendSceneRevision(frame.state);
        }
        return frame;
    }

    std::shared_ptr<br::render::CLodRayTracingSystem> m_system;
    CLodRayTracingSceneStage m_stage = CLodRayTracingSceneStage::Clas;
    std::shared_ptr<org::Buffer> m_clusters, m_sideBand, m_counter;
    std::shared_ptr<org::ResourceGroup> m_slabs;
    uint32_t m_clusterCapacity = 0u;
    br::render::CLodRayTracingSystem::ScenePipelines m_pipelines;
};

struct CLodRayTracingSceneSystemFrame {
    std::shared_ptr<br::render::CLodRayTracingSystem> system;
    bool operator==(const CLodRayTracingSceneSystemFrame&) const = default;
};

// A kept scene build's micromap uploads, before the CLAS pass, recorded at execution. A run without uploads skips it, and its late
// batch keeps its list while it skips (RecordKeyAtExecution: the uploads' serial).
class CLodRayTracingSceneOmmPass
    : public org::TypedRenderGraphPass<CLodRayTracingSceneOmmPass, CLodRayTracingSceneSystemFrame> {
public:
    static constexpr bool kRecordKeyedBySkip = true;
    explicit CLodRayTracingSceneOmmPass(std::shared_ptr<br::render::CLodRayTracingSystem> system) : m_system(std::move(system)) {}

    void Declare(org::PassBuilder& builder) {
        m_system->DeclareSceneOmm(builder);
        builder.RecordAtExecution();
    }
    void Revision(std::vector<uint64_t>&) const {}
    CLodRayTracingSceneSystemFrame Prepare(const org::PassPrepareContext&) const { return { m_system }; }
    static bool SkipsAtExecution(const CLodRayTracingSceneSystemFrame& frame, const org::PassRecordContext&) {
        std::scoped_lock lock(frame.system->FrameOperationMutex());
        return !frame.system->SceneOmmPending();
    }
    static uint64_t RecordKeyAtExecution(const CLodRayTracingSceneSystemFrame& frame, const org::PassRecordContext&) {
        std::scoped_lock lock(frame.system->FrameOperationMutex());
        return frame.system->SceneOmmPending() ? frame.system->SceneOmmSerial() + 1u : 0u;
    }
    static void Record(const CLodRayTracingSceneSystemFrame& frame, org::PassRecordContext& recording) {
        std::scoped_lock lock(frame.system->FrameOperationMutex());
        frame.system->RecordSceneOmm(recording);
    }

private:
    std::shared_ptr<br::render::CLodRayTracingSystem> m_system;
};

struct CLodRayTracingSceneEndBindings {
    br::render::CLodRayTracingSystem::ScenePipelines pipelines; // clear and cacheEnd only.
};

// A kept scene build's end of frame, after the TLAS pass, recorded at execution: the readback into this frame's slice (its
// offset and the slice bookkeeping are per frame), the coarse cut's clear and the cache's end of frame.
class CLodRayTracingSceneEndPass
    : public org::TypedRenderGraphPass<CLodRayTracingSceneEndPass, CLodRayTracingSceneSystemFrame, CLodRayTracingSceneEndBindings> {
public:
    explicit CLodRayTracingSceneEndPass(std::shared_ptr<br::render::CLodRayTracingSystem> system) : m_system(std::move(system)) {}

    CLodRayTracingSceneEndBindings Declare(org::PassBuilder& builder) {
        CLodRayTracingSceneEndBindings bindings{};
        const auto make = [&](const wchar_t* entry, const char* name) {
            return br::render::DeclarePipeline(br::render::ComputePipeline(L"Shaders/ClusterLOD/rayTracingSceneBuild.hlsl", entry, name));
        };
        bindings.pipelines.clear = make(L"CLodRtClearCS", "CLod.RayTracing.Clear.PSO");
        bindings.pipelines.cacheEnd = make(L"CLodRtCacheEndCS", "CLod.RayTracing.CacheEnd.PSO");
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        m_system->DeclareSceneEnd(builder);
        builder.RecordAtExecution();
        return bindings;
    }
    void Revision(std::vector<uint64_t>&) const {}
    CLodRayTracingSceneSystemFrame Prepare(const CLodRayTracingSceneEndBindings&, const org::PassPrepareContext&) const { return { m_system }; }
    static void Record(const CLodRayTracingSceneEndBindings& bindings, const CLodRayTracingSceneSystemFrame& frame, org::PassRecordContext& recording) {
        std::scoped_lock lock(frame.system->FrameOperationMutex());
        frame.system->RecordSceneEnd(recording, bindings.pipelines);
    }

private:
    std::shared_ptr<br::render::CLodRayTracingSystem> m_system;
};

// Primary visibility by ray, or (compare) counting how raster's result agrees with the rays.
struct RayTracedVisibilityFrame {
    std::shared_ptr<br::render::CLodRayTracingSystem> system;
    uint32_t mainCameraIndex = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;
};

struct RayTracedVisibilityBindings {
    br::PipelineState pipeline, clearPipeline;
    org::DeclaredViewToken visibility, visibleClusters, transformIndices;
    uint32_t mode = CLOD_RT_VIS_MODE_WRITE;
    uint32_t recordBase = 0u;
    uint32_t capacity = 0u;
};

class RayTracedVisibilityPass
    : public org::TypedRenderGraphPass<RayTracedVisibilityPass, RayTracedVisibilityFrame, RayTracedVisibilityBindings> {
public:
    // `visibleClusters`, `transformIndices` and `capacity`: the primary (opaque) CLod variant's list of the same graph, where
    // the pass writes its visible cluster records.
    RayTracedVisibilityPass(std::shared_ptr<br::render::CLodRayTracingSystem> system, std::shared_ptr<org::ResourceGroup> slabs, uint32_t mode,
        std::shared_ptr<org::Buffer> visibleClusters, std::shared_ptr<org::Buffer> transformIndices, uint32_t capacity)
        : m_system(std::move(system)), m_slabs(std::move(slabs)), m_mode(mode), m_visibleClusters(std::move(visibleClusters)),
          m_transformIndices(std::move(transformIndices)), m_capacity(capacity) {
    }

    RayTracedVisibilityBindings Declare(org::PassBuilder& builder) {
        RayTracedVisibilityBindings bindings{};
        bindings.pipeline = br::render::DeclarePipeline(CLodRtComputePipeline(L"Shaders/ClusterLOD/rayTracedVisibility.hlsl", L"CLodRtVisibilityCS",
            "CLod.RayTracing.Visibility.PSO"));
        bindings.clearPipeline = br::render::DeclarePipeline(br::render::ComputePipeline(L"Shaders/ClusterLOD/rayTracingSceneBuild.hlsl", L"CLodRtClearCS",
            "CLod.RayTracing.ClearVisibility.PSO"));
        bindings.mode = m_mode;
        bindings.visibility = builder.UnorderedAccess(Builtin::PrimaryCamera::VisibilityTexture).View();
        bindings.visibleClusters = builder.UnorderedAccess(m_visibleClusters).View();
        bindings.transformIndices = builder.UnorderedAccess(m_transformIndices).View();
        bindings.capacity = m_capacity;
        br::render::CLodRayTracingSystem::DeclareTracing(builder);
        m_system->DeclareVisibility(builder, m_mode == CLOD_RT_VIS_MODE_COMPARE);
        // Far from where raster's lists grow: hardware from the bottom, Reyes from the top.
        bindings.recordBase = bindings.capacity / 2u;
        if (m_slabs) builder.ShaderResource(ResourceGroupResolver(m_slabs));
        builder.ShaderResource(Builtin::CameraBuffer, Builtin::PerMeshBuffer, Builtin::CLod::MaterialPalettes, Builtin::CLod::AssemblyMaterialBases, Builtin::PerMaterialDataBuffer, Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer, Builtin::PerInstanceTransformBuffer, Builtin::PerObjectBuffer, Builtin::CullingCameraBuffer,
            Builtin::CLod::Offsets, Builtin::CLod::MeshMetadata, Builtin::CLod::Groups, Builtin::CLod::AssemblyTransforms,
            Builtin::CLod::AssemblyBoneRemaps, Builtin::CLod::AssemblyBoneRemapIndices, Builtin::SkeletonResources::SkinningInstanceInfo,
            Builtin::Material::TextureStreamingMetadataBuffer);
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        return bindings;
    }

    RayTracedVisibilityFrame Prepare(const RayTracedVisibilityBindings&, const org::PassPrepareContext& preparation) const {
        RayTracedVisibilityFrame frame{ m_system };
        const auto* update = preparation.preparationData->Get<UpdateContext>();
        if (update) {
            frame.width = update->renderResolution.x;
            frame.height = update->renderResolution.y;
        }
        return frame;
    }

    static void Record(const RayTracedVisibilityBindings& bindings, const RayTracedVisibilityFrame& frame, org::PassRecordContext& recording) {
        std::scoped_lock lock(frame.system->FrameOperationMutex());
        if (!frame.system->HasTlas() || frame.width == 0u) return;
        const bool compare = bindings.mode == CLOD_RT_VIS_MODE_COMPARE;
        frame.system->RecordVisibilityBegin(recording, bindings.clearPipeline, compare);

        const auto descriptors = frame.system->GetVisibilityDescriptors();
        br::render::MiscConstants constants{};
        constants[CLOD_RT_VIS_TLAS_SRV] = descriptors.tlas;
        constants[CLOD_RT_VIS_CLAS_INFO_SRV] = descriptors.clasInfo;
        constants[CLOD_RT_VIS_INSTANCE_ROWS_SRV] = descriptors.instanceRows;
        constants[CLOD_RT_VIS_VISIBILITY_UAV] = recording.Index(bindings.visibility);
        constants[CLOD_RT_VIS_VISIBLE_CLUSTERS_UAV] = recording.Index(bindings.visibleClusters);
        constants[CLOD_RT_VIS_TRANSFORM_INDICES_UAV] = recording.Index(bindings.transformIndices);
        constants[CLOD_RT_VIS_HASH_UAV] = descriptors.hash;
        constants[CLOD_RT_VIS_HASH_ENTRIES] = std::min(descriptors.hashEntries, bindings.capacity / 4u);
        constants[CLOD_RT_VIS_RECORD_BASE] = bindings.recordBase;
        constants[CLOD_RT_VIS_MODE] = bindings.mode;
        constants[CLOD_RT_VIS_STATS_UAV] = descriptors.stats;
        constants[CLOD_RT_VIS_VIEW_ID] = 0u;
        constants[CLOD_RT_VIS_CAPACITY] = bindings.capacity;
        recording.BindComputePipeline(bindings.pipeline);
        br::render::PushMiscConstants(recording, constants);
        recording.Commands().Dispatch((frame.width + 7u) / 8u, (frame.height + 7u) / 8u, 1u);

        frame.system->RecordVisibilityEnd(recording, compare);
    }

private:
    std::shared_ptr<br::render::CLodRayTracingSystem> m_system;
    std::shared_ptr<org::ResourceGroup> m_slabs;
    uint32_t m_mode = CLOD_RT_VIS_MODE_WRITE;
    std::shared_ptr<org::Buffer> m_visibleClusters;
    std::shared_ptr<org::Buffer> m_transformIndices;
    uint32_t m_capacity = 0u;
};

// Sun shadows by ray. Trace writes a working texture, filter writes the mask the deferred pass reads.
struct RayTracedShadowFrame {
    std::shared_ptr<br::render::CLodRayTracingSystem> system;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t frame = 0u;
};

struct RayTracedShadowBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken output, input;
    bool filter = false;
};

class RayTracedShadowPass
    : public org::TypedRenderGraphPass<RayTracedShadowPass, RayTracedShadowFrame, RayTracedShadowBindings> {
public:
    RayTracedShadowPass(std::shared_ptr<br::render::CLodRayTracingSystem> system, std::shared_ptr<org::ResourceGroup> slabs,
        std::shared_ptr<org::PixelBuffer> working, bool filter)
        : m_system(std::move(system)), m_slabs(std::move(slabs)), m_working(std::move(working)), m_filter(filter) {
    }

    RayTracedShadowBindings Declare(org::PassBuilder& builder) {
        RayTracedShadowBindings bindings{};
        bindings.pipeline = br::render::DeclarePipeline(CLodRtComputePipeline(L"Shaders/ClusterLOD/rayTracedShadows.hlsl",
            m_filter ? L"CLodRtShadowFilterCS" : L"CLodRtShadowTraceCS",
            m_filter ? "CLod.RayTracing.ShadowFilter.PSO" : "CLod.RayTracing.ShadowTrace.PSO"));
        bindings.filter = m_filter;
        builder.ShaderResource(Builtin::PrimaryCamera::VisibilityTexture, Builtin::CameraBuffer);
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        if (m_filter) {
            bindings.input = builder.ShaderResource(m_working).View();
            bindings.output = builder.UnorderedAccess(Builtin::RayTracing::SunShadow).View();
        }
        else {
            bindings.output = builder.UnorderedAccess(m_working).View();
            br::render::CLodRayTracingSystem::DeclareTracing(builder);
            if (m_slabs) builder.ShaderResource(ResourceGroupResolver(m_slabs));
            builder.ShaderResource(Builtin::Surface::NormalRoughness, Builtin::Light::InfoBuffer, Builtin::Light::ActiveLightIndices,
                Builtin::PerMeshBuffer, Builtin::CLod::MaterialPalettes, Builtin::CLod::AssemblyMaterialBases, Builtin::PerMaterialDataBuffer, Builtin::PerMeshInstanceBuffer, Builtin::InstanceDrawRecordBuffer,
                Builtin::PerInstanceTransformBuffer, Builtin::PerObjectBuffer, Builtin::CullingCameraBuffer, Builtin::CLod::Offsets,
                Builtin::CLod::MeshMetadata, Builtin::CLod::Groups, Builtin::CLod::AssemblyTransforms, Builtin::CLod::AssemblyBoneRemaps,
                Builtin::CLod::AssemblyBoneRemapIndices, Builtin::SkeletonResources::SkinningInstanceInfo, Builtin::Material::TextureStreamingMetadataBuffer);
        }
        return bindings;
    }

    RayTracedShadowFrame Prepare(const RayTracedShadowBindings&, const org::PassPrepareContext& preparation) const {
        RayTracedShadowFrame frame{ m_system };
        if (const auto* update = preparation.preparationData->Get<UpdateContext>()) {
            frame.width = update->renderResolution.x;
            frame.height = update->renderResolution.y;
            frame.frame = static_cast<uint32_t>(update->frameIndex);
        }
        return frame;
    }

    static void Record(const RayTracedShadowBindings& bindings, const RayTracedShadowFrame& frame, org::PassRecordContext& recording) {
        std::scoped_lock lock(frame.system->FrameOperationMutex());
        br::render::MiscConstants constants{};
        constants[CLOD_RT_SEC_TLAS_SRV] = frame.system->GetTlasSrv();
        constants[CLOD_RT_SEC_CLAS_INFO_SRV] = frame.system->GetClasInfoSrv();
        constants[CLOD_RT_SEC_INSTANCE_ROWS_SRV] = frame.system->GetInstanceRowsSrv();
        constants[CLOD_RT_SEC_OUTPUT_UAV] = recording.Index(bindings.output);
        constants[CLOD_RT_SEC_FRAME] = frame.frame;
        if (bindings.filter) constants[CLOD_RT_SEC_INPUT_SRV] = recording.Index(bindings.input);
        recording.BindComputePipeline(bindings.pipeline);
        br::render::PushMiscConstants(recording, constants);
        recording.Commands().Dispatch((frame.width + 7u) / 8u, (frame.height + 7u) / 8u, 1u);
    }

private:
    std::shared_ptr<br::render::CLodRayTracingSystem> m_system;
    std::shared_ptr<org::ResourceGroup> m_slabs;
    std::shared_ptr<org::PixelBuffer> m_working;
    bool m_filter = false;
};
