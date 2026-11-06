#pragma once

#include <algorithm>
#include <memory>
#include <mutex>

#include "BuiltinResources.h"
#include "Render/PassBuilders.h"
#include "BasicRenderer/Extensions/PipelineDescs.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "Runtime/GraphIntegration/Resolvers/ResourceGroupResolver.h"
#include "VirtualGeometry/RayTracing/CLodRayTracingSystem.h"
#include "../shaders/PerPassRootConstants/clodRayTracingSetupRootConstants.h"

// A compute pipeline with ray queries against clusters, which need shader model 6.10.
inline org::ComputePipelineDesc CLodRtComputePipeline(std::wstring path, std::wstring entry, std::string name) {
    org::ComputePipelineDesc desc = br::render::ComputePipeline(std::move(path), std::move(entry), std::move(name));
    desc.shader.target = L"cs_6_10";
    return desc;
}

// Builds the ray tracing scene from the RT view's culling output. Raytracing passes that run after it in the frame can
// use the TLAS.
struct CLodRayTracingSceneFrame {
    std::shared_ptr<br::render::CLodRayTracingSystem> system;
};

struct CLodRayTracingSceneBindings {
    br::render::CLodRayTracingSystem::ScenePipelines pipelines;
    org::DeclaredViewToken clusters, sideBand, counter;
    uint32_t clusterCapacity = 0u;
};

class CLodRayTracingScenePass
    : public org::TypedRenderGraphPass<CLodRayTracingScenePass, CLodRayTracingSceneFrame, CLodRayTracingSceneBindings> {
public:
    CLodRayTracingScenePass(std::shared_ptr<br::render::CLodRayTracingSystem> system, std::shared_ptr<org::Buffer> clusters,
        std::shared_ptr<org::Buffer> sideBand, std::shared_ptr<org::Buffer> counter, std::shared_ptr<org::ResourceGroup> slabs, uint32_t clusterCapacity)
        : m_system(std::move(system)), m_clusters(std::move(clusters)), m_sideBand(std::move(sideBand)), m_counter(std::move(counter)),
          m_slabs(std::move(slabs)), m_clusterCapacity(clusterCapacity) {
    }

    CLodRayTracingSceneBindings Declare(org::PassBuilder& builder) {
        CLodRayTracingSceneBindings bindings{};
        const auto make = [&](const wchar_t* entry, const char* name) {
            return builder.Pipeline(br::render::ComputePipeline(L"Shaders/ClusterLOD/rayTracingSceneBuild.hlsl", entry, name));
        };
        auto& pipelines = bindings.pipelines;
        pipelines.clear = make(L"CLodRtClearCS", "CLod.RayTracing.Clear.PSO");
        pipelines.buildClasArgs = make(L"CLodRtBuildClasArgsCS", "CLod.RayTracing.BuildClasArgs.PSO");
        pipelines.resetDraws = make(L"CLodRtResetDrawsCS", "CLod.RayTracing.ResetDraws.PSO");
        pipelines.count = make(L"CLodRtCountCS", "CLod.RayTracing.Count.PSO");
        pipelines.allocate = make(L"CLodRtAllocateCS", "CLod.RayTracing.Allocate.PSO");
        pipelines.addCoarseConsumers = make(L"CLodRtAddCoarseConsumersCS", "CLod.RayTracing.AddCoarseConsumers.PSO");
        pipelines.allocateFinish = make(L"CLodRtAllocateFinishCS", "CLod.RayTracing.AllocateFinish.PSO");
        pipelines.place = make(L"CLodRtPlaceCS", "CLod.RayTracing.Place.PSO");
        pipelines.writeInstances = make(L"CLodRtWriteInstancesCS", "CLod.RayTracing.WriteInstances.PSO");
        bindings.clusterCapacity = m_clusterCapacity;
        bindings.clusters = builder.ShaderResource(m_clusters).View();
        bindings.sideBand = builder.ShaderResource(m_sideBand).View(); // Which part portal occurrence reached each cluster.
        bindings.counter = builder.ShaderResource(m_counter).View();
        if (m_slabs) builder.ShaderResource(ResourceGroupResolver(m_slabs));
        builder.ShaderResource(Builtin::PerMeshBuffer, Builtin::CLod::MaterialPalettes, Builtin::CLod::AssemblyMaterialBases, Builtin::PerMaterialDataBuffer, Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer, Builtin::PerInstanceTransformBuffer, Builtin::PerObjectBuffer,
            Builtin::CLod::Offsets, Builtin::CLod::MeshMetadata, Builtin::CLod::Groups, Builtin::CLod::AssemblyTransforms,
            Builtin::Material::TextureStreamingMetadataBuffer); // Micromap bakes and the instance check read alpha textures' residency.
        builder.UnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer); // And keep them resident at their test mip.
        return bindings;
    }

    CLodRayTracingSceneFrame Prepare(const CLodRayTracingSceneBindings&, const org::PassPrepareContext&) const {
        return { m_system };
    }

    static void Record(const CLodRayTracingSceneBindings& bindings, const CLodRayTracingSceneFrame& frame, org::PassRecordContext& recording) {
        std::scoped_lock lock(frame.system->FrameOperationMutex());
        frame.system->RecordSceneBuild(recording, bindings.pipelines,
            recording.Index(bindings.clusters), recording.Index(bindings.sideBand), recording.Index(bindings.counter), bindings.clusterCapacity);
    }

private:
    std::shared_ptr<br::render::CLodRayTracingSystem> m_system;
    std::shared_ptr<org::Buffer> m_clusters, m_sideBand, m_counter;
    std::shared_ptr<org::ResourceGroup> m_slabs;
    uint32_t m_clusterCapacity = 0u;
};

// Primary visibility by ray, or (compare) counting how raster's result agrees with the rays.
struct RayTracedVisibilityFrame {
    std::shared_ptr<br::render::CLodRayTracingSystem> system;
    uint32_t mainCameraIndex = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;
};

struct RayTracedVisibilityBindings {
    org::PipelineState pipeline, clearPipeline;
    org::DeclaredViewToken visibility, visibleClusters, transformIndices;
    uint32_t mode = CLOD_RT_VIS_MODE_WRITE;
    uint32_t recordBase = 0u;
    uint32_t capacity = 0u;
};

class RayTracedVisibilityPass
    : public org::TypedRenderGraphPass<RayTracedVisibilityPass, RayTracedVisibilityFrame, RayTracedVisibilityBindings> {
public:
    RayTracedVisibilityPass(std::shared_ptr<br::render::CLodRayTracingSystem> system, std::shared_ptr<org::ResourceGroup> slabs, uint32_t mode)
        : m_system(std::move(system)), m_slabs(std::move(slabs)), m_mode(mode) {
    }

    RayTracedVisibilityBindings Declare(org::PassBuilder& builder) {
        RayTracedVisibilityBindings bindings{};
        bindings.pipeline = builder.Pipeline(CLodRtComputePipeline(L"Shaders/ClusterLOD/rayTracedVisibility.hlsl", L"CLodRtVisibilityCS",
            "CLod.RayTracing.Visibility.PSO"));
        bindings.clearPipeline = builder.Pipeline(br::render::ComputePipeline(L"Shaders/ClusterLOD/rayTracingSceneBuild.hlsl", L"CLodRtClearCS",
            "CLod.RayTracing.ClearVisibility.PSO"));
        bindings.mode = m_mode;
        bindings.visibility = builder.UnorderedAccess(Builtin::PrimaryCamera::VisibilityTexture).View();
        bindings.visibleClusters = builder.UnorderedAccess(m_system->GetPrimaryVisibleClusters()).View();
        bindings.transformIndices = builder.UnorderedAccess(m_system->GetPrimaryTransformIndices()).View();
        bindings.capacity = m_system->GetPrimaryVisibleClusterCapacity();
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
};

// Sun shadows by ray. Trace writes a working texture, filter writes the mask the deferred pass reads.
struct RayTracedShadowFrame {
    std::shared_ptr<br::render::CLodRayTracingSystem> system;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t frame = 0u;
};

struct RayTracedShadowBindings {
    org::PipelineState pipeline;
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
        bindings.pipeline = builder.Pipeline(CLodRtComputePipeline(L"Shaders/ClusterLOD/rayTracedShadows.hlsl",
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
