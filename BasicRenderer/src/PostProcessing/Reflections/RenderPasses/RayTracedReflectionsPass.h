#pragma once

#include <bit>
#include <mutex>

#include "BuiltinResources.h"
#include "Render/PassBuilders.h"
#include "Resources/Buffers/Buffer.h"
#include "Runtime/GraphIntegration/Resolvers/ResourceGroupResolver.h"
#include "Runtime/Device/DeviceManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include <BasicRenderer/Extensions/PipelineDescs.h>
#include "VirtualGeometry/RayTracing/CLodRayTracingSystem.h"
#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Resources/PixelBuffer.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "../shaders/PerPassRootConstants/clodRayTracingSetupRootConstants.h"

struct RayTracedReflectionsFrameData {
    std::shared_ptr<br::render::CLodRayTracingSystem> service;
    uint32_t width = 0u;
    uint32_t height = 0u;
    float maxDistance = 0.0f;
    float roughnessCutoff = 0.0f;
    uint32_t frame = 0u;
};

struct RayTracedReflectionsBindings {
    org::PipelineState tracePipeline, shadePipeline, shadeTerrainPipeline;
    org::DeclaredViewToken outputShader, outputCpu, hits;
};

class RayTracedReflectionsPass
    : public org::TypedRenderGraphPass<RayTracedReflectionsPass,
          RayTracedReflectionsFrameData, RayTracedReflectionsBindings> {
public:
    RayTracedReflectionsPass(std::shared_ptr<org::ResourceGroup> slabs, bool terrainRvt) : m_slabs(std::move(slabs)), m_terrainRvt(terrainRvt) {
        const auto resolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
        m_hits = CreateAliasedUnmaterializedStructuredBuffer(uint64_t(resolution.x) * resolution.y, 64u, true, false, false, false);
        m_hits->SetName("CLod RT reflection hits");
    }

    RayTracedReflectionsBindings Declare(org::PassBuilder& builder) {
        builder.ShaderResource(Builtin::Color::HDRColorTarget,
            Builtin::PrimaryCamera::DepthTexture, Builtin::Surface::NormalRoughness,
            Builtin::Surface::SpecularAo, Builtin::CameraBuffer,
            Builtin::Environment::CurrentPrefilteredCubemap);
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        builder.ShaderResource(Builtin::PrimaryCamera::VisibilityTexture, Builtin::Light::InfoBuffer, Builtin::Light::ActiveLightIndices,
            Builtin::Environment::InfoBuffer, Builtin::PerMeshBuffer, Builtin::PerMaterialDataBuffer, "Builtin::PerMaterialEvalDataBuffer",
            Builtin::PerMeshInstanceBuffer, Builtin::InstanceDrawRecordBuffer, Builtin::PerInstanceTransformBuffer, Builtin::PerObjectBuffer,
            Builtin::NormalMatrixBuffer, Builtin::CullingCameraBuffer, Builtin::CLod::Offsets, Builtin::CLod::MeshMetadata, Builtin::CLod::Groups,
            Builtin::CLod::GroupChunks, Builtin::CLod::GroupPageMap, Builtin::CLod::AssemblyTransforms, Builtin::CLod::AssemblyBoneRemaps,
            Builtin::CLod::AssemblyBoneRemapIndices, Builtin::SkeletonResources::InverseBindMatrices, Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo, Builtin::Material::TextureStreamingMetadataBuffer,
            Builtin::Terrain::Sets, Builtin::Terrain::Layers, Builtin::Terrain::StochasticLayers, Builtin::Terrain::LayerRefs,
            Builtin::Terrain::Regions, Builtin::Terrain::WeightBlocks,
            Builtin::Light::PointLightCubemapBuffer, Builtin::Light::DirectionalLightCascadeBuffer, Builtin::Light::SpotLightMatrixBuffer,
            Builtin::PerMaterialOpenPBRDataBuffer, Builtin::OpenPBR::FuzzLTC, Builtin::OpenPBR::IdealMetalEnergyComplement,
            Builtin::OpenPBR::IdealMetalAverageEnergyComplement, Builtin::OpenPBR::OpaqueDielectricAverageEnergyComplement, Builtin::Noise::BlueNoise2D);
        builder.ShaderResource(Builtin::OpenPBR::OpaqueDielectricEnergyComplement,
            org::SrvView{static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull)});
        if (m_slabs) builder.ShaderResource(ResourceGroupResolver(m_slabs));
        // Materials seen in reflections ask for their mips like any others.
        builder.UnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer);
        if (m_terrainRvt) {
            builder.ShaderResource(Builtin::Terrain::RvtInfo, Builtin::Terrain::RvtClipInfos, Builtin::Terrain::RvtPageTable,
                Builtin::Terrain::RvtPageKeys, Builtin::Terrain::RvtPhysicalPageOwner, Builtin::Terrain::RvtPhysicalPageAtlas,
                Builtin::Terrain::RvtHeightResidentCache, Builtin::Terrain::RvtHeightAtlas, Builtin::Terrain::RvtAlbedoAtlas,
                Builtin::Terrain::RvtNormalAtlas, Builtin::Terrain::RvtMaterialAtlas);
            builder.UnorderedAccess(Builtin::Terrain::RvtRequestMasks, Builtin::Terrain::RvtRequestList,
                Builtin::Terrain::RvtCounters, Builtin::Terrain::RvtStats);
        }
        RayTracedReflectionsBindings bindings{};
        // What every material could need. Unlike primary evaluation there's one program per surface kind rather than one per
        // material combination, because material flags still decide at run time what is sampled.
        const std::vector<org::PipelineShaderDefine> materialDefines = {
            { L"PSO_BASE_COLOR_TEXTURE", L"1" }, { L"PSO_NORMAL_MAP", L"1" }, { L"PSO_ROUGHNESS_TEXTURE", L"1" },
            { L"PSO_METALLIC_TEXTURE", L"1" }, { L"PSO_AO_TEXTURE", L"1" }, { L"PSO_EMISSIVE_TEXTURE", L"1" }, { L"PSO_OPACITY_TEXTURE", L"1" },
            { L"PSO_IMAGE_BASED_LIGHTING", L"1" }, { L"PSO_DEFERRED", L"1" }, { L"VISUTIL_COLOR_ONLY_GBUFFER_EVAL", L"1" },
        };
        std::vector<org::PipelineShaderDefine> terrainDefines = materialDefines;
        terrainDefines.push_back({ L"PSO_TERRAIN", L"1" });
        // Ray queries against clusters need shader model 6.10.
        const auto sm610 = [](org::ComputePipelineDesc desc) {
            desc.shader.target = L"cs_6_10";
            return desc;
        };
        bindings.tracePipeline = builder.Pipeline(sm610(br::render::ComputePipeline(L"Shaders/ClusterLOD/rayTracedReflections.hlsl",
            L"CLodRtReflectionTraceCS", "CLod.RayTracing.ReflectionTrace.PSO")));
        bindings.shadePipeline = builder.Pipeline(sm610(br::render::ComputePipeline(L"Shaders/ClusterLOD/rayTracedReflectionShade.hlsl",
            L"CLodRtReflectionShadeCS", "CLod.RayTracing.ReflectionShade.PSO", materialDefines)));
        bindings.shadeTerrainPipeline = builder.Pipeline(sm610(br::render::ComputePipeline(L"Shaders/ClusterLOD/rayTracedReflectionShade.hlsl",
            L"CLodRtReflectionShadeCS", "CLod.RayTracing.ReflectionShadeTerrain.PSO", terrainDefines)));
        bindings.hits = builder.UnorderedAccess(m_hits).View();
        const auto output = builder.UnorderedAccessClear(Builtin::PostProcessing::ScreenSpaceReflections);
        bindings.outputShader = output.View(0);
        bindings.outputCpu = output.View(1);
        return bindings;
    }

    RayTracedReflectionsFrameData Prepare(const RayTracedReflectionsBindings&, const org::PassPrepareContext& preparation) const {
        RayTracedReflectionsFrameData frame{};
        const auto* context = preparation.preparationData ? preparation.preparationData->Get<UpdateContext>() : nullptr;
        if (!context) return frame;
        frame.service = context->clodRayTracingSystem;
        frame.width = context->renderResolution.x;
        frame.height = context->renderResolution.y;
        auto& settings = SettingsManager::GetInstance();
        frame.maxDistance = settings.getSettingGetter<float>("rayTracedReflectionMaxDistance")();
        frame.roughnessCutoff = settings.getSettingGetter<float>("rayTracedReflectionRoughnessCutoff")();
        frame.frame = static_cast<uint32_t>(context->frameIndex);
        return frame;
    }

    static void Record(const RayTracedReflectionsBindings& bindings, const RayTracedReflectionsFrameData& frame, org::PassRecordContext& recording) {
        bool traced = false;
        if (frame.service) {
            std::scoped_lock lock(frame.service->FrameOperationMutex());
            if (frame.service->HasTlas()) {
                br::render::MiscConstants constants{};
                constants[CLOD_RT_SEC_TLAS_SRV] = frame.service->GetTlasSrv();
                constants[CLOD_RT_SEC_CLAS_INFO_SRV] = frame.service->GetClasInfoSrv();
                constants[CLOD_RT_SEC_OUTPUT_UAV] = recording.Index(bindings.outputShader);
                constants[CLOD_RT_SEC_HITS_UAV] = recording.Index(bindings.hits);
                constants[CLOD_RT_SEC_FRAME] = frame.frame;
                constants[CLOD_RT_SEC_MAX_DISTANCE] = std::bit_cast<uint32_t>(frame.maxDistance);
                constants[CLOD_RT_SEC_ROUGHNESS_CUTOFF] = std::bit_cast<uint32_t>(frame.roughnessCutoff);
                recording.BindComputePipeline(bindings.tracePipeline);
                br::render::PushMiscConstants(recording, constants);
                recording.Commands().Dispatch((frame.width + 7u) / 8u, (frame.height + 7u) / 8u, 1u);
                rhi::GlobalBarrier uav{ rhi::ResourceSyncState::ComputeShading, rhi::ResourceSyncState::ComputeShading,
                    rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceAccessType::UnorderedAccess };
                rhi::BarrierBatch barrier{};
                barrier.globals = { &uav, 1u };
                recording.Commands().Barriers(barrier);
                recording.BindComputePipeline(bindings.shadePipeline);
                br::render::PushMiscConstants(recording, constants);
                recording.Commands().Dispatch((frame.width + 7u) / 8u, (frame.height + 7u) / 8u, 1u);
                recording.BindComputePipeline(bindings.shadeTerrainPipeline);
                br::render::PushMiscConstants(recording, constants);
                recording.Commands().Dispatch((frame.width + 7u) / 8u, (frame.height + 7u) / 8u, 1u);
                traced = true;
            }
        }
        if (!traced) br::render::ClearUavFloat(recording, bindings.outputShader, bindings.outputCpu, 0.0f);
    }

private:
    std::shared_ptr<org::Buffer> m_hits;
    std::shared_ptr<org::ResourceGroup> m_slabs;
    bool m_terrainRvt = false;
};
