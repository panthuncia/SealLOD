#include "VirtualShadows/Rasterization/RenderPasses/ReyesVirtualShadowRasterizationPass.h"

#include <string_view>

#include "Scene/Views/ViewManager.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Render/Runtime/UploadTypes.h"
#include "BasicRenderer/Diagnostics/TerrainRvtTelemetry.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "Runtime/GraphIntegration/Resolvers/ResourceGroupResolver.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodRasterizationRootConstants.h"
#include "../shaders/PerPassRootConstants/clodReyesPatchRasterRootConstants.h"

ReyesVirtualShadowRasterizationPass::ReyesVirtualShadowRasterizationPass(
    std::shared_ptr<org::Buffer> visibleClustersBuffer,
    std::shared_ptr<org::Buffer> visibleClusterTransformIndicesBuffer,
    std::shared_ptr<org::Buffer> diceQueueBuffer,
    std::shared_ptr<org::Buffer> diceQueueCounterBuffer,
    std::shared_ptr<org::Buffer> rasterWorkBuffer,
    std::shared_ptr<org::Buffer> rasterWorkCounterBuffer,
    std::shared_ptr<org::Buffer> tessTableConfigsBuffer,
    std::shared_ptr<org::Buffer> tessTableVerticesBuffer,
    std::shared_ptr<org::Buffer> tessTableTrianglesBuffer,
    std::shared_ptr<org::Buffer> indirectArgsBuffer,
    std::shared_ptr<org::Buffer> telemetryBuffer,
    std::shared_ptr<org::PixelBuffer> virtualShadowPageTableTexture,
    std::shared_ptr<org::PixelBuffer> virtualShadowPhysicalPagesTexture,
    std::shared_ptr<org::PixelBuffer> virtualShadowDynamicPagesTexture,
    std::shared_ptr<org::Buffer> virtualShadowClipmapInfoBuffer,
    std::shared_ptr<org::ResourceGroup> slabResourceGroup,
    std::string_view resourceName,
    uint32_t phaseIndex)
    : m_visibleClustersBuffer(std::move(visibleClustersBuffer))
    , m_visibleClusterTransformIndicesBuffer(std::move(visibleClusterTransformIndicesBuffer))
    , m_diceQueueBuffer(std::move(diceQueueBuffer))
    , m_diceQueueCounterBuffer(std::move(diceQueueCounterBuffer))
    , m_rasterWorkBuffer(std::move(rasterWorkBuffer))
    , m_rasterWorkCounterBuffer(std::move(rasterWorkCounterBuffer))
    , m_tessTableConfigsBuffer(std::move(tessTableConfigsBuffer))
    , m_tessTableVerticesBuffer(std::move(tessTableVerticesBuffer))
    , m_tessTableTrianglesBuffer(std::move(tessTableTrianglesBuffer))
    , m_indirectArgsBuffer(std::move(indirectArgsBuffer))
    , m_telemetryBuffer(std::move(telemetryBuffer))
    , m_virtualShadowPageTableTexture(std::move(virtualShadowPageTableTexture))
    , m_virtualShadowPhysicalPagesTexture(std::move(virtualShadowPhysicalPagesTexture))
    , m_virtualShadowDynamicPagesTexture(std::move(virtualShadowDynamicPagesTexture))
    , m_virtualShadowClipmapInfoBuffer(std::move(virtualShadowClipmapInfoBuffer))
    , m_slabResourceGroup(std::move(slabResourceGroup))
    , m_phaseIndex(phaseIndex)
{
    m_viewRasterInfoBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodViewRasterInfo), false, false, false, false);
    m_viewRasterInfoBuffer->SetName(std::string(resourceName));
    org::memory::SetResourceUsageHint(*m_viewRasterInfoBuffer, "Cluster LOD Reyes virtual shadow");

    auto defines = IsTerrainRvtTelemetryDebugEnabled()
        ? std::vector<DxcDefine>{ DxcDefine{ L"TERRAIN_RVT_TELEMETRY", L"1" } }
        : std::vector<DxcDefine>{};
    defines.push_back({ L"CLOD_VSM_TWO_LAYER_REYES_VERSION", L"2" });
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/reyesPatchVirtualShadowRaster.hlsl",
        L"ReyesPatchVirtualShadowRasterCS",
        defines,
        "CLod.ReyesPatchVirtualShadowRaster.PSO");

    rhi::IndirectArg dispatchArgs[] = {
        {.kind = rhi::IndirectArgKind::Dispatch }
    };

    auto device = DeviceManager::GetInstance().GetDevice();
    m_commandSignature = std::make_shared<rhi::CommandSignaturePtr>();
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(dispatchArgs, 1), sizeof(CLodReyesDispatchIndirectCommand) },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        *m_commandSignature);
}

ReyesVirtualShadowRasterBindings ReyesVirtualShadowRasterizationPass::Declare(org::PassBuilder& declaration)
{
    declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    auto* builder = &declaration;
    builder->WithShaderResource(
            Builtin::PerMeshBuffer,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerObjectBuffer,
            Builtin::CullingCameraBuffer,
            Builtin::CameraBuffer,
            Builtin::Shadows::CLodDirectionalPageViewInfo,
            Builtin::PerMaterialDataBuffer,
            Builtin::CLod::Offsets,
            Builtin::CLod::MeshMetadata,
            Builtin::PerMaterialOpenPBRDataBuffer,
            Builtin::Terrain::Sets,
            Builtin::Terrain::Layers,
            Builtin::Terrain::StochasticLayers,
            Builtin::Terrain::LayerRefs,
            Builtin::Terrain::Regions,
            Builtin::Terrain::WeightBlocks,
            Builtin::Terrain::TextureGroup,
            Builtin::Terrain::RvtInfo,
            Builtin::Terrain::RvtClipInfos,
            Builtin::Terrain::RvtPageTable,
            Builtin::Terrain::RvtPageKeys,
            Builtin::Terrain::RvtPhysicalPageOwner,
            Builtin::Terrain::RvtPhysicalPageAtlas,
            Builtin::Terrain::RvtHeightResidentCache,
            Builtin::Terrain::RvtHeightAtlas,
            Builtin::Terrain::RvtAlbedoAtlas,
            Builtin::Terrain::RvtNormalAtlas,
            Builtin::Terrain::RvtMaterialAtlas,
            Builtin::Material::TextureStreamingMetadataBuffer,
            Builtin::SkeletonResources::InverseBindMatrices,
            Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo)
		.WithUnorderedAccess(
            Builtin::Material::TextureStreamingFeedbackBuffer,
            Builtin::Terrain::RvtRequestMasks,
            Builtin::Terrain::RvtRequestList,
            Builtin::Terrain::RvtCounters,
            Builtin::Terrain::RvtStats)
        .WithConstantBuffer(Builtin::PerFrameBuffer);

    if (m_slabResourceGroup) {
        builder->WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
    }
    return {builder->BindShaderResource(m_visibleClustersBuffer), builder->BindShaderResource(m_visibleClusterTransformIndicesBuffer),
        builder->BindShaderResource(m_diceQueueBuffer), builder->BindShaderResource(m_diceQueueCounterBuffer),
        builder->BindShaderResource(m_rasterWorkBuffer), builder->BindShaderResource(m_rasterWorkCounterBuffer),
        builder->BindShaderResource(m_tessTableConfigsBuffer), builder->BindShaderResource(m_tessTableVerticesBuffer),
        builder->BindShaderResource(m_tessTableTrianglesBuffer), builder->BindIndirectArguments(m_indirectArgsBuffer),
        builder->BindUnorderedAccess(m_telemetryBuffer), builder->BindShaderResource(m_viewRasterInfoBuffer),
        builder->BindUnorderedAccess(m_virtualShadowPageTableTexture), builder->BindUnorderedAccess(m_virtualShadowPhysicalPagesTexture),
        builder->BindUnorderedAccess(m_virtualShadowDynamicPagesTexture), builder->BindShaderResource(m_virtualShadowClipmapInfoBuffer),
        m_phaseIndex, m_shadowConfig.pageTableResolution, m_shadowConfig.virtualResolution};
}

void ReyesVirtualShadowRasterizationPass::Update(const org::UpdateExecutionContext& executionContext)
{
    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;
    const CLodVirtualShadowResolutionConfig virtualShadowConfig = CLodVirtualShadowBuildRuntimeResolutionConfig();
    m_shadowConfig = virtualShadowConfig;

    const auto numViews = context.ViewCameraBufferSize();
    std::vector<CLodViewRasterInfo> nextViewRasterInfos(numViews);

    for (const auto& viewInfo : context.Views()) {
        const auto cameraIndex = viewInfo.cameraBufferIndex;
        CLodViewRasterInfo info{};
        if (viewInfo.shadow && viewInfo.lightType == Components::LightType::Directional) {
            info.scissorMinX = 0u;
            info.scissorMinY = 0u;
            info.scissorMaxX = virtualShadowConfig.virtualResolution;
            info.scissorMaxY = virtualShadowConfig.virtualResolution;
            info.viewportScaleX = 1.0f;
            info.viewportScaleY = 1.0f;
        }
        nextViewRasterInfos[cameraIndex] = info;
    }

    if (m_viewRasterInfos != nextViewRasterInfos) {
        m_viewRasterInfos = std::move(nextViewRasterInfos);
        m_viewRasterInfoBuffer->ResizeStructured(static_cast<uint32_t>(m_viewRasterInfos.size()));
        UploadBufferData(
            m_viewRasterInfos.data(),
            static_cast<uint32_t>(m_viewRasterInfos.size() * sizeof(CLodViewRasterInfo)),
            org::runtime::UploadTarget::FromShared(m_viewRasterInfoBuffer),
            0);
        m_declaredResourcesChanged = true;
    }
    else {
        m_declaredResourcesChanged = false;
    }
}

bool ReyesVirtualShadowRasterizationPass::DeclaredResourcesChanged() const
{
    return m_declaredResourcesChanged;
}

br::render::PreparedComputeIndirect ReyesVirtualShadowRasterizationPass::Prepare(
    const ReyesVirtualShadowRasterBindings& bindings, const org::PassPrepareContext& preparation) const
{
    br::render::PreparedComputeIndirect data{};
    const auto& context = *preparation.preparationData->Get<UpdateContext>();
    data.resourceHeap = context.textureDescriptorHeap.GetHandle();
    data.samplerHeap = context.samplerDescriptorHeap.GetHandle();
    auto binding = preparation.CaptureProgramBinding(m_pso);
    data.program = binding.program;
    data.descriptorIndices = std::move(binding.descriptorIndices);
    const auto srv = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource}).index; };
    const auto uav = [&](org::ResourceBindingToken token, uint32_t variant = UINT32_MAX) { return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess, variant}).index; };
    data.constants[CLOD_REYES_PATCH_RASTER_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = srv(bindings.visible);
    data.constants[CLOD_REYES_PATCH_RASTER_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = srv(bindings.transforms);
    data.constants[CLOD_REYES_PATCH_RASTER_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.diceCounter);
    data.constants[CLOD_REYES_PATCH_RASTER_WORK_BUFFER_DESCRIPTOR_INDEX] = srv(bindings.work);
    data.constants[CLOD_REYES_PATCH_RASTER_DICE_QUEUE_DESCRIPTOR_INDEX] = srv(bindings.diceQueue);
    data.constants[CLOD_REYES_PATCH_RASTER_VIEW_RASTER_INFO_DESCRIPTOR_INDEX] = srv(bindings.viewInfo);
    data.constants[CLOD_REYES_PATCH_RASTER_TELEMETRY_DESCRIPTOR_INDEX] = uav(bindings.telemetry);
    data.constants[CLOD_REYES_PATCH_RASTER_PHASE_INDEX] = bindings.phase;
    data.constants[CLOD_REYES_PATCH_RASTER_WORK_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.workCounter);
    data.constants[CLOD_REYES_PATCH_RASTER_PATCH_INDEX_BASE] = 0u;
    data.constants[CLOD_REYES_PATCH_RASTER_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = srv(bindings.tessConfigs);
    data.constants[CLOD_REYES_PATCH_RASTER_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX] = srv(bindings.tessVertices);
    data.constants[CLOD_REYES_PATCH_RASTER_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX] = srv(bindings.tessTriangles);

    data.constants[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX] =
        uav(bindings.pageTable, static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull));
    data.constants[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX] = srv(bindings.clipmapInfo);
    data.constants[CLOD_RASTER_VIRTUAL_SHADOW_PHYSICAL_PAGES_DESCRIPTOR_INDEX] =
        uav(bindings.physicalPages);
    data.constants[CLOD_RASTER_VIRTUAL_SHADOW_DYNAMIC_PAGES_DESCRIPTOR_INDEX] =
        uav(bindings.dynamicPages);
    data.constants[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_RESOLUTION] = bindings.pageTableResolution;
    data.constants[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
    data.constants[CLOD_RASTER_VIRTUAL_SHADOW_VIRTUAL_RESOLUTION] = bindings.virtualResolution;

    data.commandSignature = preparation.CaptureCommandSignature(m_commandSignature);
    data.argumentsReference = preparation.CaptureResource(bindings.indirectArgs);
    return data;
}

void ReyesVirtualShadowRasterizationPass::Record(const ReyesVirtualShadowRasterBindings&,
    const br::render::PreparedComputeIndirect& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeIndirect(data, recording);
}
