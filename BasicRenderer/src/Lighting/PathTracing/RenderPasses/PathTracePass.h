#pragma once

#include <array>
#include <bit>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>

#include "BuiltinResources.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Render/PassBuilders.h"
#include "Render/PipelineState.h"
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
#include "../shaders/PerPassRootConstants/clodRayTracingSetupRootConstants.h"

// The state object, its shader table and the dispatch arguments. The two passes share them: PathTraceArgsPass writes the
// arguments that PathTracePass launches, and the arguments name the table's records.
struct PathTraceState {
    RayTracingPipelineRecipe recipe;
    std::shared_ptr<org::Buffer> arguments;
    std::mutex mutex;
    std::shared_ptr<const br::render::CLodRtShaderTable> table;
    // What accumulated samples depend on besides the camera. Averaging across a change would keep what is no longer there.
    // The settings that change the image are in the last entry.
    std::array<uint64_t, 6> accumulationScene{};
    uint64_t accumulatedFrames = 0u;

    // Null until the library has compiled on its worker.
    std::shared_ptr<const br::render::CLodRtShaderTable> CurrentTable() {
        auto& manager = PSOManager::GetInstance();
        const org::PipelineState* pipeline = manager.TryGetRayTracingPipeline(recipe);
        if (!pipeline) {
            return nullptr;
        }
        std::scoped_lock lock(mutex);
        if (!table || !table->IsCurrent(*pipeline)) {
            auto& device = DeviceManager::GetInstance();
            table = br::render::CLodRtShaderTable::Create(device.GetDevice(), device.GetRayTracingFeatures(), *pipeline,
                { recipe.GroupNames(), "PathTraceRayGen", { "PathTraceRadianceMiss", "PathTraceShadowMiss" },
                    { "PathTraceGeneralRadiance", "PathTraceGeneralShadow", "PathTraceTerrainRadiance", "PathTraceTerrainShadow" } },
                "Path trace shader table");
        }
        return table;
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
};

struct PathTraceArgsBindings {
    org::PipelineState pipeline;
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
        bindings.pipeline = builder.Pipeline(br::render::ComputePipeline(L"Shaders/ClusterLOD/pathTraceDispatchArgs.hlsl",
            L"PathTraceDispatchArgsCS", "PathTrace.DispatchArgs.PSO"));
        bindings.output = builder.UnorderedAccess(m_state->arguments).View();
        return bindings;
    }

    PathTraceArgsFrameData Prepare(const PathTraceArgsBindings&, const org::PassPrepareContext& preparation) const {
        PathTraceArgsFrameData frame{};
        const auto* context = preparation.preparationData ? preparation.preparationData->Get<UpdateContext>() : nullptr;
        if (!context) return frame;
        if (const auto table = m_state->CurrentTable()) {
            frame.arguments = table->DispatchArguments(context->renderResolution.x, context->renderResolution.y);
        }
        return frame;
    }

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
};

struct PathTraceFrameData {
    std::shared_ptr<br::render::CLodRayTracingSystem> system;
    std::shared_ptr<const br::render::CLodRtShaderTable> table;
    uint32_t frame = 0u;
    uint32_t maxBounces = 2u;
    uint32_t flags = 0u;
};

struct PathTraceBindings {
    org::PipelineState clearPipeline; // The pixel watch's.
    org::ResourceBindingToken arguments;
    org::DeclaredViewToken output, baseColor, normalRoughness, motion, projectedDepth, accumulation, specularAlbedo, specularHitDistance;
    bool hasAccumulation = false;
    bool hasRayReconstructionGuides = false;
};

// Replaces raster, material evaluation and deferred lighting. Everything after it reads HDRColorTarget as usual.
class PathTracePass : public org::TypedRenderGraphPass<PathTracePass, PathTraceFrameData, PathTraceBindings> {
public:
    PathTracePass(std::shared_ptr<PathTraceState> state, std::shared_ptr<org::ResourceGroup> slabs, bool terrainRvt, bool rayReconstruction)
        : m_state(std::move(state)), m_slabs(std::move(slabs)), m_terrainRvt(terrainRvt), m_rayReconstruction(rayReconstruction) {
        if (SettingsManager::GetInstance().getSettingGetter<bool>("pathTracingAccumulate")()) {
            const auto resolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
            org::TextureDescription description;
            description.channels = 4;
            description.format = rhi::Format::R32G32B32A32_Float;
            description.hasSRV = description.hasUAV = true;
            description.imageDimensions.emplace_back(resolution.x, resolution.y, 0, 0);
            m_accumulation = org::PixelBuffer::CreateSharedUnmaterialized(description);
            m_accumulation->SetName("Path trace accumulation");
        }
    }

    PathTraceBindings Declare(org::PassBuilder& builder) {
        PathTraceBindings bindings{};
        bindings.clearPipeline = builder.Pipeline(br::render::ComputePipeline(L"Shaders/ClusterLOD/rayTracingSceneBuild.hlsl", L"CLodRtClearCS",
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
        builder.ShaderResource(Builtin::OpenPBR::OpaqueDielectricEnergyComplement,
            org::SrvView{ static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull) });
        if (m_slabs) builder.ShaderResource(ResourceGroupResolver(m_slabs));
        // Primary hits ask for the mips they see. Nothing past them does: see pathTracingHit.hlsli.
        builder.UnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer);
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
        bindings.projectedDepth = builder.UnorderedAccess(Builtin::PrimaryCamera::ProjectedDepthTexture).View();        if (m_accumulation) {
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

    PathTraceFrameData Prepare(const PathTraceBindings&, const org::PassPrepareContext& preparation) const {
        PathTraceFrameData frame{};
        const auto* context = preparation.preparationData ? preparation.preparationData->Get<UpdateContext>() : nullptr;
        if (!context) return frame;
        frame.system = context->clodRayTracingSystem;
        frame.table = m_state->CurrentTable();
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
        if (frame.system) {
            const auto& stats = frame.system->GetStats();
            const std::array<uint64_t, 6> scene = { stats.instances, stats.clustersInBvh, stats.clasBuildsTotal, stats.clasFreedTotal,
                context->materialTextureStreamingStats.totalResidentBytes, (uint64_t(frame.maxBounces) << 32u) | frame.flags };
            std::scoped_lock lock(m_state->mutex);
            if (scene != m_state->accumulationScene) {
                m_state->accumulationScene = scene;
                m_state->accumulatedFrames = 0u;
                frame.flags |= CLOD_RT_PT_FLAG_RESTART_ACCUMULATION;
            }
            ++m_state->accumulatedFrames;
        }
        return frame;
    }

    static void Record(const PathTraceBindings& bindings, const PathTraceFrameData& frame, org::PassRecordContext& recording) {
        if (!frame.system || !frame.table) return;
        std::scoped_lock lock(frame.system->FrameOperationMutex());
        if (!frame.system->HasTlas()) return;
        br::render::MiscConstants constants{};
        constants[CLOD_RT_SEC_TLAS_SRV] = frame.system->GetTlasSrv();
        constants[CLOD_RT_SEC_CLAS_INFO_SRV] = frame.system->GetClasInfoSrv();
        constants[CLOD_RT_SEC_INSTANCE_ROWS_SRV] = frame.system->GetInstanceRowsSrv();
        constants[CLOD_RT_SEC_OUTPUT_UAV] = recording.Index(bindings.output);
        constants[CLOD_RT_SEC_FRAME] = frame.frame;
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
        constants[CLOD_RT_PT_WATCH_UAV] = frame.system->GetPixelWatchUav();
        constants[CLOD_RT_PT_WATCH_BRIGHT] = std::bit_cast<uint32_t>(frame.system->GetPixelWatchBright());
        frame.system->RecordPixelWatchBegin(recording, bindings.clearPipeline);
        recording.Retain(frame.table);
        recording.BindRayTracingPipeline(frame.table->Payload());
        br::render::PushMiscConstants(recording, constants);
        recording.TraceRaysIndirect(frame.table->IndirectDesc(), bindings.arguments);
        frame.system->RecordPixelWatchEnd(recording);
    }

private:
    std::shared_ptr<PathTraceState> m_state;
    std::shared_ptr<org::ResourceGroup> m_slabs;
    bool m_terrainRvt = false;
    bool m_rayReconstruction = false;
    std::shared_ptr<org::PixelBuffer> m_accumulation;
};
