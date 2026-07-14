#include "Render/GraphExtensions/ClusterLOD/ReyesVirtualShadowRasterizationPass.h"

#include <string_view>

#include "Managers/ViewManager.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "Render/TerrainRvtTelemetry.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodRasterizationRootConstants.h"
#include "../shaders/PerPassRootConstants/clodReyesPatchRasterRootConstants.h"

ReyesVirtualShadowRasterizationPass::ReyesVirtualShadowRasterizationPass(
    std::shared_ptr<Buffer> visibleClustersBuffer,
    std::shared_ptr<Buffer> visibleClusterTransformIndicesBuffer,
    std::shared_ptr<Buffer> diceQueueBuffer,
    std::shared_ptr<Buffer> diceQueueCounterBuffer,
    std::shared_ptr<Buffer> rasterWorkBuffer,
    std::shared_ptr<Buffer> rasterWorkCounterBuffer,
    std::shared_ptr<Buffer> tessTableConfigsBuffer,
    std::shared_ptr<Buffer> tessTableVerticesBuffer,
    std::shared_ptr<Buffer> tessTableTrianglesBuffer,
    std::shared_ptr<Buffer> indirectArgsBuffer,
    std::shared_ptr<Buffer> telemetryBuffer,
    std::shared_ptr<PixelBuffer> virtualShadowPageTableTexture,
    std::shared_ptr<PixelBuffer> virtualShadowPhysicalPagesTexture,
    std::shared_ptr<Buffer> virtualShadowClipmapInfoBuffer,
    std::shared_ptr<ResourceGroup> slabResourceGroup,
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
    , m_virtualShadowClipmapInfoBuffer(std::move(virtualShadowClipmapInfoBuffer))
    , m_slabResourceGroup(std::move(slabResourceGroup))
    , m_phaseIndex(phaseIndex)
{
    m_viewRasterInfoBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodViewRasterInfo), false, false, false, false);
    m_viewRasterInfoBuffer->SetName(std::string(resourceName));
    rg::memory::SetResourceUsageHint(*m_viewRasterInfoBuffer, "Cluster LOD Reyes virtual shadow");

    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/reyesPatchVirtualShadowRaster.hlsl",
        L"ReyesPatchVirtualShadowRasterCS",
        IsTerrainRvtTelemetryDebugEnabled() ? std::vector<DxcDefine>{ DxcDefine{ L"TERRAIN_RVT_TELEMETRY", L"1" } } : std::vector<DxcDefine>{},
        "CLod.ReyesPatchVirtualShadowRaster.PSO");

    rhi::IndirectArg dispatchArgs[] = {
        {.kind = rhi::IndirectArgKind::Dispatch }
    };

    auto device = DeviceManager::GetInstance().GetDevice();
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(dispatchArgs, 1), sizeof(CLodReyesDispatchIndirectCommand) },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        m_commandSignature);
}

void ReyesVirtualShadowRasterizationPass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    builder->WithShaderResource(
            m_visibleClustersBuffer,
            m_visibleClusterTransformIndicesBuffer,
            m_diceQueueBuffer,
            m_diceQueueCounterBuffer,
            m_rasterWorkBuffer,
            m_rasterWorkCounterBuffer,
            m_tessTableConfigsBuffer,
            m_tessTableVerticesBuffer,
            m_tessTableTrianglesBuffer,
            m_viewRasterInfoBuffer,
            m_virtualShadowClipmapInfoBuffer,
            Builtin::PerMeshBuffer,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerObjectBuffer,
            Builtin::CullingCameraBuffer,
            Builtin::PerMaterialDataBuffer,
            Builtin::CLod::Offsets,
            Builtin::CLod::MeshMetadata,
            Builtin::CLod::AssemblyTransforms,
            Builtin::CLod::AssemblyBoneRemaps,
            Builtin::CLod::AssemblyBoneRemapIndices,
            Builtin::PerMaterialOpenPBRDataBuffer,
            Builtin::Material::TextureGroup,
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
        .WithIndirectArguments(m_indirectArgsBuffer)
        .WithUnorderedAccess(
            m_telemetryBuffer,
            m_virtualShadowPageTableTexture,
            m_virtualShadowPhysicalPagesTexture)
        .WithConstantBuffer(Builtin::PerFrameBuffer);

    if (m_slabResourceGroup) {
        builder->WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
    }
}

void ReyesVirtualShadowRasterizationPass::Setup()
{
}

void ReyesVirtualShadowRasterizationPass::Update(const UpdateExecutionContext& executionContext)
{
    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;
    const CLodVirtualShadowResolutionConfig virtualShadowConfig = CLodVirtualShadowBuildRuntimeResolutionConfig();

    const auto numViews = context.viewManager->GetCameraBufferSize();
    std::vector<CLodViewRasterInfo> nextViewRasterInfos(numViews);

    context.viewManager->ForEachView([&](uint64_t viewID) {
        const auto* viewInfo = context.viewManager->Get(viewID);
        if (!viewInfo) {
            return;
        }

        const auto cameraIndex = viewInfo->gpu.cameraBufferIndex;
        CLodViewRasterInfo info{};
        if (viewInfo->flags.shadow && viewInfo->lightType == Components::LightType::Directional) {
            info.scissorMinX = 0u;
            info.scissorMinY = 0u;
            info.scissorMaxX = virtualShadowConfig.virtualResolution;
            info.scissorMaxY = virtualShadowConfig.virtualResolution;
            info.viewportScaleX = 1.0f;
            info.viewportScaleY = 1.0f;
        }
        nextViewRasterInfos[cameraIndex] = info;
    });

    if (m_viewRasterInfos != nextViewRasterInfos) {
        m_viewRasterInfos = std::move(nextViewRasterInfos);
        m_viewRasterInfoBuffer->ResizeStructured(static_cast<uint32_t>(m_viewRasterInfos.size()));
        BUFFER_UPLOAD(
            m_viewRasterInfos.data(),
            static_cast<uint32_t>(m_viewRasterInfos.size() * sizeof(CLodViewRasterInfo)),
            rg::runtime::UploadTarget::FromShared(m_viewRasterInfoBuffer),
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

PassReturn ReyesVirtualShadowRasterizationPass::Execute(PassExecutionContext& executionContext)
{
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;
    const CLodVirtualShadowResolutionConfig virtualShadowConfig = CLodVirtualShadowBuildRuntimeResolutionConfig();

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());

    uint32_t uintRootConstants[NumMiscUintRootConstants] = {};
    uintRootConstants[CLOD_REYES_PATCH_RASTER_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = m_visibleClustersBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_PATCH_RASTER_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] =
        m_visibleClusterTransformIndicesBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_PATCH_RASTER_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] = m_diceQueueCounterBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_PATCH_RASTER_WORK_BUFFER_DESCRIPTOR_INDEX] = m_rasterWorkBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_PATCH_RASTER_DICE_QUEUE_DESCRIPTOR_INDEX] = m_diceQueueBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_PATCH_RASTER_VIEW_RASTER_INFO_DESCRIPTOR_INDEX] = m_viewRasterInfoBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_PATCH_RASTER_TELEMETRY_DESCRIPTOR_INDEX] = m_telemetryBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_PATCH_RASTER_PHASE_INDEX] = m_phaseIndex;
    uintRootConstants[CLOD_REYES_PATCH_RASTER_WORK_COUNTER_DESCRIPTOR_INDEX] = m_rasterWorkCounterBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_PATCH_RASTER_PATCH_INDEX_BASE] = 0u;
    uintRootConstants[CLOD_REYES_PATCH_RASTER_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = m_tessTableConfigsBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_PATCH_RASTER_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX] = m_tessTableVerticesBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_PATCH_RASTER_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX] = m_tessTableTrianglesBuffer->GetSRVInfo(0).slot.index;

    uintRootConstants[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX] =
        m_virtualShadowPageTableTexture->GetUAVShaderVisibleInfo(UAVViewType::Texture2DArrayFull, 0).slot.index;
    uintRootConstants[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX] = m_virtualShadowClipmapInfoBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_RASTER_VIRTUAL_SHADOW_PHYSICAL_PAGES_DESCRIPTOR_INDEX] =
        m_virtualShadowPhysicalPagesTexture->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_RESOLUTION] = virtualShadowConfig.pageTableResolution;
    uintRootConstants[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
    uintRootConstants[CLOD_RASTER_VIRTUAL_SHADOW_VIRTUAL_RESOLUTION] = virtualShadowConfig.virtualResolution;

    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        uintRootConstants);

    commandList.ExecuteIndirect(m_commandSignature->GetHandle(), m_indirectArgsBuffer->GetAPIResource().GetHandle(), 0, {}, 0, 1);
    return {};
}

void ReyesVirtualShadowRasterizationPass::Cleanup()
{
}
