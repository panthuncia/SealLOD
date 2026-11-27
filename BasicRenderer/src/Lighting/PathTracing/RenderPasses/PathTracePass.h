#pragma once

#include <array>
#include <bit>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>

#include "BuiltinResources.h"
#include "Camera/Lens/LensManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Render/PassBuilders.h"
#include <BasicRenderer/Pipeline/PipelineDesc.h>
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "Runtime/Device/DeviceManager.h"
#include "Runtime/GraphIntegration/Resolvers/ResourceGroupResolver.h"
#include "Runtime/Settings/SettingsManager.h"
#include "VirtualGeometry/RayTracing/CLodRayTracingSystem.h"
#include "VirtualGeometry/RayTracing/CLodRtShaderTable.h"
#include "BasicRenderer/Extensions/PipelineDescs.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Extensions/PassRevision.h"
#include <cstring>
#include "../shaders/PerPassRootConstants/clodRayTracingSetupRootConstants.h"

// The state object, its shader table and the dispatch arguments. The two passes share them: PathTraceArgsPass writes the
// arguments that PathTracePass launches, and the arguments name the table's records.
struct PathTraceState {
    RayTracingPipelineRecipe recipe;
    std::shared_ptr<org::Buffer> arguments;
    std::mutex mutex;
    std::shared_ptr<const br::render::CLodRtShaderTable> table;
    // What accumulated samples depend on. Averaging across a change would keep what is no longer there. The settings that
    // change the image are in entry 5, the camera's view and unjittered projection in 6, and pathTracingCameraModelHash
    // (whatever else shapes primary rays, such as a lens) mixed with tableCodeHash in 7.
    std::array<uint64_t, 8> accumulationScene{};
    // Frames traced since the last restart: the accumulation buffer's alpha once they have executed. Read under mutex.
    uint64_t accumulatedFrames = 0u;
    uint64_t accumulationRestarts = 0u;
    // RGBA32F, rgb the running average and a the sample count. Null unless pathTracingAccumulate was on at graph build.
    std::shared_ptr<org::PixelBuffer> accumulation;
    // The physical lens, whose published state's shader defines (a polynomial lens's generated code) join the recipe.
    std::shared_ptr<br::lens::LensManager> lens;
    // The table this frame's passes use (the arguments pass and the trace pass must agree), chosen once per frame, and a
    // hash of the lens code the table's pipeline was compiled with (0: none), which accumulation depends on.
    uint64_t tableFrame = ~0ull;
    uint64_t tableCodeHash = 0u;
    std::shared_ptr<const br::render::CLodRtShaderTable> frameTable;
    // The lens publication and pipeline cache generation the table was last found current for, with the pipeline it wanted:
    // while both are the same, the next frame takes the table without copying the recipe and looking the pipeline up (about
    // 6 us of the path traced Shading update per frame). Shader reloads advance the generation.
    std::shared_ptr<const br::lens::LensPublishedState> cachedPublished;
    uint64_t cachedGeneration = ~0ull;
    bool cachedWanted = false;

    // The table for this frame. The pipeline the published lens needs compiles on a worker: until it exists the last table
    // stays (its shader traces the surfaces for a polynomial it wasn't compiled with). Null until any has compiled.
    std::shared_ptr<const br::render::CLodRtShaderTable> CurrentTable(uint64_t frame) {
        std::scoped_lock lock(mutex);
        if (frame == tableFrame) return frameTable;
        tableFrame = frame;
        auto& manager = PSOManager::GetInstance();
        const auto published = lens ? lens->Published() : nullptr;
        const auto generation = manager.PipelineCacheGeneration();
        if (cachedWanted && table && published == cachedPublished && generation == cachedGeneration) {
            frameTable = table;
            return frameTable;
        }
        cachedWanted = false;
        RayTracingPipelineRecipe wanted = recipe;
        std::wstring code;
        if (published) {
            for (auto& define : br::lens::LensManager::ShaderDefines(*published)) wanted.defines.push_back(std::move(define));
            code = published->polynomialInclude;
        }
        const br::PipelineState* pipeline = manager.TryGetRayTracingPipeline(wanted);
        if (!pipeline && table) {
            frameTable = table;
            return frameTable;
        }
        const bool found = pipeline != nullptr;
        if (!pipeline && !code.empty()) {
            pipeline = manager.TryGetRayTracingPipeline(recipe);
            code.clear();
        }
        if (!pipeline) {
            frameTable = nullptr;
            return frameTable;
        }
        if (!table || !table->IsCurrent(*pipeline)) {
            auto& device = DeviceManager::GetInstance();
            table = br::render::CLodRtShaderTable::Create(device.GetDevice(), device.GetRayTracingFeatures(), *pipeline,
                { recipe.GroupNames(), "PathTraceRayGen", { "PathTraceRadianceMiss", "PathTraceShadowMiss" },
                    { "PathTraceGeneralRadiance", "PathTraceGeneralShadow", "PathTraceTerrainRadiance", "PathTraceTerrainShadow" } },
                "Path trace shader table");
            tableCodeHash = code.empty() ? 0u : std::hash<std::wstring>{}(code);
        }
        cachedWanted = found;
        cachedPublished = published;
        cachedGeneration = generation;
        frameTable = table;
        return frameTable;
    }
};

inline std::shared_ptr<PathTraceState> MakePathTraceState() {
    auto state = std::make_shared<PathTraceState>();
    state->recipe.libraryPath = L"Shaders/ClusterLOD/pathTrace.lib.hlsl";
    state->recipe.shaders = {
        { rhi::ShaderStage::RayGen, "PathTraceRayGen" },
        { rhi::ShaderStage::Miss, "PathTraceRadianceMiss" },
        { rhi::ShaderStage::Miss, "PathTraceShadowMiss" },
        { rhi::ShaderStage::ClosestHit, "PathTraceGeneralClosestHit" },
        { rhi::ShaderStage::ClosestHit, "PathTraceTerrainClosestHit" },
        { rhi::ShaderStage::AnyHit, "PathTraceRadianceAnyHit" },
        { rhi::ShaderStage::AnyHit, "PathTraceShadowAnyHit" },
    };
    state->recipe.hitGroups = {
        { "PathTraceGeneralRadiance", "PathTraceGeneralClosestHit", "PathTraceRadianceAnyHit" },
        { "PathTraceGeneralShadow", "", "PathTraceShadowAnyHit" },
        { "PathTraceTerrainRadiance", "PathTraceTerrainClosestHit", "PathTraceRadianceAnyHit" },
        { "PathTraceTerrainShadow", "", "PathTraceShadowAnyHit" },
    };
    // pathTrace.lib.hlsl: PathTracePayload, 22 words.
    state->recipe.maxPayloadBytes = 88u;
    // The BVH's alpha-tested CLAS may carry opacity micromaps (CLodRayTracingSystem::RequestOpacityMicromaps), where the
    // device has them: a pipeline that allows them can't be created below ray tracing tier 1.2.
    if (DeviceManager::GetInstance().GetRayTracingFeatures().opacityMicromap) {
        state->recipe.flags = static_cast<rhi::RayTracingPipelineFlags>(state->recipe.flags | rhi::RTPipeline_AllowOpacityMicromaps);
    }
    // The pixel watch (pathTracingWatch.hlsli) costs registers even when off, so it is compiled only when asked for.
    if (const char* watch = std::getenv("SARP_RT_PIXEL_WATCH"); watch && std::strtof(watch, nullptr) > 0.0f) {
        state->recipe.defines.emplace_back(L"PATH_TRACE_WATCH", L"1");
    }
    state->recipe.debugName = "PathTrace.PSO";
    state->arguments = CreateAliasedUnmaterializedRawBuffer(sizeof(rhi::RayTracingDispatchRaysArguments), true, false);
    state->arguments->SetName("Path trace dispatch arguments");
    return state;
}

struct PathTraceArgsFrameData {
    std::optional<rhi::RayTracingDispatchRaysArguments> arguments;
    bool operator==(const PathTraceArgsFrameData& other) const {
        if (arguments.has_value() != other.arguments.has_value()) return false;
        return !arguments || std::memcmp(&*arguments, &*other.arguments, sizeof(rhi::RayTracingDispatchRaysArguments)) == 0;
    }
};

struct PathTraceArgsBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken output;
};

// Writes what TraceRaysIndirect reads. Always the whole screen today. A GPU-written count is where dispatching only some
// pixels or tiles would start.
class PathTraceArgsPass
    : public org::TypedRenderGraphPass<PathTraceArgsPass, PathTraceArgsFrameData, PathTraceArgsBindings> {
public:
    explicit PathTraceArgsPass(std::shared_ptr<PathTraceState> state) : m_state(std::move(state)) {}

    PathTraceArgsBindings Declare(org::PassBuilder& builder) {
        PathTraceArgsBindings bindings{};
        bindings.pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(L"Shaders/ClusterLOD/pathTraceDispatchArgs.hlsl",
            L"PathTraceDispatchArgsCS", "PathTrace.DispatchArgs.PSO"));
        bindings.output = builder.UnorderedAccess(m_state->arguments).View();
        m_pipeline = bindings.pipeline;
        return bindings;
    }

    // Kept recordings (M6 step 5): the frame's table is chosen here, each run (CurrentTable keeps it for the frame, so the
    // trace pass's Update gets the same one), and the arguments the revision names are made from it.
    // Staged recordings (M6 step 6): the values the revision names, taken without side effects (RenderGraphPass::TakeFrameValues).
    void TakeFrameValues(const org::UpdateExecutionContext& execution) override {
        PathTraceArgsFrameData frame{};
        const auto* context = execution.hostData ? execution.hostData->Get<UpdateContext>() : nullptr;
        m_table.reset();
        if (context) {
            m_table = m_state->CurrentTable(context->frameNumber);
            if (m_table) frame.arguments = m_table->DispatchArguments(context->renderResolution.x, context->renderResolution.y);
        }
        m_frame = frame;
    }
    void Revision(std::vector<uint64_t>& out) const {
        br::render::AppendPipelineRevision(out, m_pipeline);
        out.push_back(reinterpret_cast<uintptr_t>(m_table.get()));
        if (!m_frame.arguments) return;
        constexpr size_t kWords = sizeof(rhi::RayTracingDispatchRaysArguments) / sizeof(uint64_t);
        std::array<uint64_t, kWords> words{};
        std::memcpy(words.data(), &*m_frame.arguments, sizeof(words));
        out.insert(out.end(), words.begin(), words.end());
    }

    PathTraceArgsFrameData Prepare(const PathTraceArgsBindings&, const org::PassPrepareContext&) const { return m_frame; }

    static void Record(const PathTraceArgsBindings& bindings, const PathTraceArgsFrameData& frame, org::PassRecordContext& recording) {
        if (!frame.arguments) return;
        br::render::MiscConstants constants{};
        constexpr uint32_t kOutputSlot = 26u;
        constexpr uint32_t kWords = sizeof(rhi::RayTracingDispatchRaysArguments) / sizeof(uint32_t);
        static_assert(kWords == kOutputSlot);
        const auto words = std::bit_cast<std::array<uint32_t, kWords>>(*frame.arguments);
        for (uint32_t index = 0u; index < kWords; ++index) {
            constants[index] = words[index];
        }
        constants[kOutputSlot] = recording.Index(bindings.output);
        recording.BindComputePipeline(bindings.pipeline);
        br::render::PushMiscConstants(recording, constants);
        recording.Commands().Dispatch(1u, 1u, 1u);
    }

private:
    std::shared_ptr<PathTraceState> m_state;
    br::PipelineState m_pipeline;
    std::shared_ptr<const br::render::CLodRtShaderTable> m_table;
    PathTraceArgsFrameData m_frame{};
};

// One run's values. Update makes them each run (the frame number, the accumulation restart, the table).
struct PathTraceRun {
    std::shared_ptr<br::render::CLodRayTracingSystem> system;
    std::shared_ptr<const br::lens::LensPublishedState> lens; // The physical lens this frame traces through; inactive is a pinhole.
    std::shared_ptr<const br::render::CLodRtShaderTable> table;
    uint32_t frame = 0u;
    uint32_t maxBounces = 2u;
    uint32_t flags = 0u;
};

// What Record bakes (M6 step 9c: the pass is recorded ahead and kept). The frame number is the per-frame buffer's
// (pathTrace.lib.hlsl), and the accumulation restart is in the flags only when the pass accumulates, so a still camera replays
// an accumulating trace and any camera replays the default one. The revision names the rest.
struct PathTraceFrameData {
    std::shared_ptr<br::render::CLodRayTracingSystem> system;
    std::shared_ptr<const br::render::CLodRtShaderTable> table;
    uint32_t maxBounces = 0u;
    uint32_t flags = 0u;
    bool hasTlas = false;
    uint32_t tlasSrv = 0u, clasInfoSrv = 0u, instanceRowsSrv = 0u, watchUav = 0u, watchBright = 0u;
    bool operator==(const PathTraceFrameData&) const = default;
};

struct PathTraceBindings {
    br::PipelineState clearPipeline; // The pixel watch's.
    org::ResourceBindingToken arguments;
    org::DeclaredViewToken output, baseColor, normalRoughness, motion, projectedDepth, accumulation, specularAlbedo, specularHitDistance;
    bool hasAccumulation = false;
    bool hasRayReconstructionGuides = false;
    // The RVT is declared only when the pipeline recipe has terrain RVT. Without it the terrain hit shader's RVT slots bind
    // as absent, as material evaluation's do.
    bool terrainRvt = false;
};

// Replaces raster, material evaluation and deferred lighting. Everything after it reads HDRColorTarget as usual.
class PathTracePass : public org::TypedRenderGraphPass<PathTracePass, PathTraceFrameData, PathTraceBindings> {
public:
    PathTracePass(std::shared_ptr<PathTraceState> state, std::shared_ptr<br::render::CLodRayTracingSystem> system, std::shared_ptr<org::ResourceGroup> slabs,
        bool terrainRvt, bool rayReconstruction, std::shared_ptr<br::lens::LensManager> lens = {})
        : m_state(std::move(state)), m_system(std::move(system)), m_slabs(std::move(slabs)), m_terrainRvt(terrainRvt), m_rayReconstruction(rayReconstruction),
          m_lens(std::move(lens)) {
        if (m_lens) m_state->lens = m_lens;
        if (SettingsManager::GetInstance().getSettingGetter<bool>("pathTracingAccumulate")()) {
            const auto resolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
            org::TextureDescription description;
            description.channels = 4;
            description.format = rhi::Format::R32G32B32A32_Float;
            description.hasSRV = description.hasUAV = true;
            description.imageDimensions.emplace_back(resolution.x, resolution.y, 0, 0);
            m_accumulation = org::PixelBuffer::CreateSharedUnmaterialized(description);
            m_accumulation->SetName("Path trace accumulation");
            m_state->accumulation = m_accumulation;
        }
    }

    PathTraceBindings Declare(org::PassBuilder& builder) {
        PathTraceBindings bindings{};
        bindings.clearPipeline = br::render::DeclarePipeline(br::render::ComputePipeline(L"Shaders/ClusterLOD/rayTracingSceneBuild.hlsl", L"CLodRtClearCS",
            "PathTrace.ClearWatch.PSO"));
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        builder.ShaderResource(Builtin::CameraBuffer, Builtin::PerMeshBuffer, Builtin::CLod::MaterialPalettes, Builtin::CLod::AssemblyMaterialBases, Builtin::PerMaterialDataBuffer, Builtin::InstanceDrawRecordBuffer,
            Builtin::PerMeshInstanceBuffer, Builtin::PerInstanceTransformBuffer, Builtin::PerObjectBuffer, Builtin::Material::TextureStreamingMetadataBuffer,
            Builtin::CLod::Offsets, Builtin::CLod::MeshMetadata, Builtin::CLod::Groups, Builtin::CLod::GroupChunks, Builtin::CLod::GroupPageMap,
            Builtin::CLod::AssemblyTransforms, Builtin::CLod::AssemblyBoneRemaps, Builtin::CLod::AssemblyBoneRemapIndices,
            Builtin::SkeletonResources::InverseBindMatrices, Builtin::SkeletonResources::BoneTransforms, Builtin::SkeletonResources::SkinningInstanceInfo,
            Builtin::CullingCameraBuffer, Builtin::NormalMatrixBuffer, "Builtin::PerMaterialEvalDataBuffer", Builtin::PerMaterialOpenPBRDataBuffer,
            Builtin::Light::InfoBuffer, Builtin::Light::ActiveLightIndices, Builtin::Environment::InfoBuffer,
            Builtin::Terrain::Sets, Builtin::Terrain::Layers, Builtin::Terrain::StochasticLayers, Builtin::Terrain::LayerRefs,
            Builtin::Terrain::Regions, Builtin::Terrain::WeightBlocks,
            Builtin::OpenPBR::FuzzLTC, Builtin::OpenPBR::IdealMetalEnergyComplement, Builtin::OpenPBR::IdealMetalAverageEnergyComplement,
            Builtin::OpenPBR::OpaqueDielectricAverageEnergyComplement, Builtin::Noise::BlueNoise2D);
        // The physical lens (LensManager), declared whether or not it is on: the graph validates every name a shader uses.
        builder.ShaderResource(Builtin::RayTracing::LensData, Builtin::RayTracing::LensFocus);
        builder.ShaderResource(Builtin::OpenPBR::OpaqueDielectricEnergyComplement,
            org::SrvView{ static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull) });
        if (m_slabs) builder.ShaderResource(ResourceGroupResolver(m_slabs));
        // Primary hits ask for the mips they see. Nothing past them does: see pathTracingHit.hlsli.
        builder.UnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer);
        br::render::CLodRayTracingSystem::DeclareTracing(builder);
        if (m_system) m_system->DeclarePixelWatch(builder);
        bindings.terrainRvt = m_terrainRvt;
        if (m_terrainRvt) {
            builder.ShaderResource(Builtin::Terrain::RvtInfo, Builtin::Terrain::RvtClipInfos, Builtin::Terrain::RvtPageTable,
                Builtin::Terrain::RvtPageKeys, Builtin::Terrain::RvtPhysicalPageOwner, Builtin::Terrain::RvtPhysicalPageAtlas,
                Builtin::Terrain::RvtHeightResidentCache, Builtin::Terrain::RvtHeightAtlas, Builtin::Terrain::RvtAlbedoAtlas,
                Builtin::Terrain::RvtNormalAtlas, Builtin::Terrain::RvtMaterialAtlas);
            builder.UnorderedAccess(Builtin::Terrain::RvtRequestMasks, Builtin::Terrain::RvtRequestList,
                Builtin::Terrain::RvtCounters, Builtin::Terrain::RvtStats);
        }
        bindings.arguments = builder.IndirectArguments(m_state->arguments);
        bindings.output = builder.UnorderedAccess(Builtin::Color::HDRColorTarget).View();
        bindings.baseColor = builder.UnorderedAccess(Builtin::Surface::BaseColorOpacity).View();
        bindings.normalRoughness = builder.UnorderedAccess(Builtin::Surface::NormalRoughness).View();
        bindings.motion = builder.UnorderedAccess(Builtin::Surface::Motion).View();
        bindings.projectedDepth = builder.UnorderedAccess(Builtin::PrimaryCamera::ProjectedDepthTexture).View();
        // With the pixel watch, Record copies it to a readback slice per frame: recorded at execution then.
        if (m_system && m_system->HasPixelWatch()) builder.RecordAtExecution();
        if (m_accumulation) {
            bindings.accumulation = builder.UnorderedAccess(m_accumulation).View();
            bindings.hasAccumulation = true;
        }
        if (m_rayReconstruction) {
            bindings.specularAlbedo = builder.UnorderedAccess(Builtin::RayTracing::SpecularAlbedo).View();
            bindings.specularHitDistance = builder.UnorderedAccess(Builtin::RayTracing::SpecularHitDistance).View();
            bindings.hasRayReconstructionGuides = true;
        }
        return bindings;
    }

    void Update(const org::UpdateExecutionContext& execution) override {
        const auto* context = execution.hostData ? execution.hostData->Get<UpdateContext>() : nullptr;
        *m_run = context ? MakeRun(*context) : PathTraceRun{};
    }
    // The frame Update made (Revision is asked after Update).
    void Revision(std::vector<uint64_t>& out) const {
        const PathTraceFrameData frame = MakeFrame();
        out.push_back(reinterpret_cast<uintptr_t>(frame.system.get()));
        out.push_back(reinterpret_cast<uintptr_t>(frame.table.get()));
        out.push_back((uint64_t(frame.maxBounces) << 32u) | frame.flags);
        out.push_back((uint64_t(frame.hasTlas) << 32u) | frame.instanceRowsSrv);
        out.push_back((uint64_t(frame.tlasSrv) << 32u) | frame.clasInfoSrv);
        out.push_back((uint64_t(frame.watchUav) << 32u) | frame.watchBright);
    }

    PathTraceFrameData Prepare(const PathTraceBindings&, const org::PassPrepareContext&) const { return MakeFrame(); }

    PathTraceFrameData MakeFrame() const {
        PathTraceFrameData frame{};
        const PathTraceRun& run = *m_run;
        frame.system = run.system;
        frame.table = run.table;
        frame.maxBounces = run.maxBounces;
        frame.flags = run.flags;
        // The restart only matters to the accumulation (pathTrace.lib.hlsl).
        if (!m_accumulation) frame.flags &= ~CLOD_RT_PT_FLAG_RESTART_ACCUMULATION;
        if (frame.system) {
            std::scoped_lock lock(frame.system->FrameOperationMutex());
            frame.hasTlas = frame.system->HasTlas();
            frame.tlasSrv = frame.system->GetTlasSrv();
            frame.clasInfoSrv = frame.system->GetClasInfoSrv();
            frame.instanceRowsSrv = frame.system->GetInstanceRowsSrv();
            frame.watchUav = frame.system->GetPixelWatchUav();
            frame.watchBright = std::bit_cast<uint32_t>(frame.system->GetPixelWatchBright());
        }
        return frame;
    }

    PathTraceRun MakeRun(const UpdateContext& update) const {
        const auto* context = &update;
        PathTraceRun frame{};
        frame.system = context->clodRayTracingSystem;
        frame.table = m_state->CurrentTable(context->frameNumber);
        frame.frame = static_cast<uint32_t>(context->frameNumber);
        auto& settings = SettingsManager::GetInstance();
        frame.maxBounces = settings.getSettingGetter<uint32_t>("pathTracingMaxBounces")();
        const uint32_t reorderMode = settings.getSettingGetter<uint32_t>("pathTracingReorderMode")();
        frame.flags = (reorderMode >= 1u ? CLOD_RT_PT_FLAG_REORDER_SECONDARY : 0u) | (reorderMode >= 2u ? CLOD_RT_PT_FLAG_REORDER_PRIMARY : 0u);
        if (settings.getSettingGetter<bool>("pathTracingSunOnly")()) frame.flags |= CLOD_RT_PT_FLAG_SUN_ONLY;
        // 0 exact everywhere, 1 bounce and shadow rays use micromaps' 2-state guess, 2 every ray does.
        const uint32_t twoState = settings.getSettingGetter<uint32_t>("pathTracingOpacityMicromapTwoState")();
        if (twoState >= 1u) frame.flags |= CLOD_RT_PT_FLAG_OMM_2_STATE_SECONDARY;
        if (twoState >= 2u) frame.flags |= CLOD_RT_PT_FLAG_OMM_2_STATE_PRIMARY;
        if (m_lens) {
            // One atomic load: the lens is built and uploaded off this thread, and only a GPU-ready revision gets here.
            frame.lens = m_lens->Published();
            if (frame.lens->Active()) frame.flags |= CLOD_RT_PT_FLAG_LENS;
            if (context->hasPrimaryCamera) m_lens->NoteCamera(context->primaryCamera.fov, context->primaryCamera.aspect);
        }
        if (frame.system && frame.table) {
            const auto& stats = frame.system->GetStats();
            // The unjittered projection: a jittering upscaler moves the jittered one every frame, and the ray generator's own
            // subpixel offset already spreads accumulated samples over the pixel. The view is also compared on the GPU; it is
            // here too so that accumulatedFrames counts what the buffer's alpha does.
            const uint64_t camera = context->hasPrimaryCamera
                ? HashCameraMatrices(context->primaryCamera.info.view, context->primaryCamera.info.unjitteredProjection) : 0u;
            std::array<uint64_t, 8> scene = { stats.instances, stats.clustersInBvh, stats.clasBuildsTotal, stats.clasFreedTotal,
                context->materialTextureStreamingStats.totalResidentBytes, (uint64_t(frame.maxBounces) << 32u) | frame.flags,
                camera, settings.getSettingGetter<uint64_t>("pathTracingCameraModelHash")() };
            // The lens's focus is per frame, outside the lens hash: the GPU restarts the frame it moves (LensFocusRestart), and this
            // word (the manual focus, or the autofocus epoch last read back) keeps accumulatedFrames from counting past that.
            if (m_lens && frame.lens && frame.lens->Active()) scene[7] ^= m_lens->FocusRestartKey() * 0x9E3779B97F4A7C15ull;
            std::scoped_lock lock(m_state->mutex);
            scene[7] ^= m_state->tableCodeHash; // CurrentTable writes it under the same mutex.
            if (scene != m_state->accumulationScene) {
                m_state->accumulationScene = scene;
                m_state->accumulatedFrames = 0u;
                ++m_state->accumulationRestarts;
                frame.flags |= CLOD_RT_PT_FLAG_RESTART_ACCUMULATION;
            }
            ++m_state->accumulatedFrames;
        }
        return frame;
    }

    static uint64_t HashCameraMatrices(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& projection) {
        // FNV-1a over the matrices' bits: any change at all restarts, and an unchanged camera rebuilds the same bits.
        uint64_t hash = 14695981039346656037ull;
        for (const DirectX::XMMATRIX* matrix : { &view, &projection }) {
            const auto* bytes = reinterpret_cast<const unsigned char*>(matrix);
            for (size_t index = 0; index < sizeof(DirectX::XMMATRIX); ++index) {
                hash = (hash ^ bytes[index]) * 1099511628211ull;
            }
        }
        return hash;
    }

    static void Record(const PathTraceBindings& bindings, const PathTraceFrameData& frame, org::PassRecordContext& recording) {
        if (!frame.system || !frame.table || !frame.hasTlas) return;
        std::scoped_lock lock(frame.system->FrameOperationMutex()); // The pixel watch's begin and end.
        br::render::MiscConstants constants{};
        constants[CLOD_RT_SEC_TLAS_SRV] = frame.tlasSrv;
        constants[CLOD_RT_SEC_CLAS_INFO_SRV] = frame.clasInfoSrv;
        constants[CLOD_RT_SEC_INSTANCE_ROWS_SRV] = frame.instanceRowsSrv;
        constants[CLOD_RT_SEC_OUTPUT_UAV] = recording.Index(bindings.output);
        constants[CLOD_RT_PT_MAX_BOUNCES] = frame.maxBounces;
        uint32_t flags = frame.flags;
        // A reference image can't lean on these, and a denoiser handles what they'd clean up.
        if (bindings.hasAccumulation) flags |= CLOD_RT_PT_FLAG_ACCUMULATE;
        if (!bindings.hasRayReconstructionGuides && !bindings.hasAccumulation) flags |= CLOD_RT_PT_FLAG_FIREFLY_CLAMP | CLOD_RT_PT_FLAG_ROULETTE;
        constants[CLOD_RT_PT_FLAGS] = flags;
        constants[CLOD_RT_PT_ACCUMULATION_UAV] = bindings.hasAccumulation ? recording.Index(bindings.accumulation) : CLOD_RT_PT_NO_DESCRIPTOR;
        constants[CLOD_RT_PT_BASE_COLOR_UAV] = recording.Index(bindings.baseColor);
        constants[CLOD_RT_PT_NORMAL_ROUGHNESS_UAV] = recording.Index(bindings.normalRoughness);
        constants[CLOD_RT_PT_MOTION_UAV] = recording.Index(bindings.motion);
        constants[CLOD_RT_PT_PROJECTED_DEPTH_UAV] = recording.Index(bindings.projectedDepth);        constants[CLOD_RT_PT_SPECULAR_ALBEDO_UAV] = bindings.hasRayReconstructionGuides ? recording.Index(bindings.specularAlbedo) : CLOD_RT_PT_NO_DESCRIPTOR;
        constants[CLOD_RT_PT_SPECULAR_HIT_UAV] = bindings.hasRayReconstructionGuides ? recording.Index(bindings.specularHitDistance) : CLOD_RT_PT_NO_DESCRIPTOR;
        constants[CLOD_RT_PT_WATCH_UAV] = frame.watchUav;
        constants[CLOD_RT_PT_WATCH_BRIGHT] = frame.watchBright;
        frame.system->RecordPixelWatchBegin(recording, bindings.clearPipeline);
        recording.Retain(frame.table);
        static constexpr std::string_view kRvtPrefixes[] = { "Builtin::Terrain::Rvt" };
        org::PassRecordContext::BindOptions options{};
        if (!bindings.terrainRvt) options.absentPrefixes = kRvtPrefixes;
        recording.BindRayTracingPipeline(frame.table->Payload(), options);
        br::render::PushMiscConstants(recording, constants);
        recording.TraceRaysIndirect(frame.table->IndirectDesc(), bindings.arguments);
        frame.system->RecordPixelWatchEnd(recording);
    }

private:
    std::shared_ptr<PathTraceState> m_state;
    std::shared_ptr<br::render::CLodRayTracingSystem> m_system; // For its pixel watch's declaration; recording uses the frame's.
    std::shared_ptr<org::ResourceGroup> m_slabs;
    bool m_terrainRvt = false;
    bool m_rayReconstruction = false;
    std::shared_ptr<org::PixelBuffer> m_accumulation;
    std::shared_ptr<br::lens::LensManager> m_lens;
    std::shared_ptr<PathTraceRun> m_run = std::make_shared<PathTraceRun>();
};
