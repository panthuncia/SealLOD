#include "Render/GraphExtensions/ClusterLOD/ReyesVirtualShadowHardwareRasterPass.h"

#include "Managers/MaterialManager.h"
#include "Managers/ViewManager.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodRasterizationRootConstants.h"

ReyesVirtualShadowHardwareRasterPass::ReyesVirtualShadowHardwareRasterPass(
    std::shared_ptr<Buffer> visibleClustersBuffer,
    std::shared_ptr<Buffer> rasterBucketsHistogramBuffer,
    std::shared_ptr<Buffer> rasterBucketsIndirectArgsBuffer,
    std::shared_ptr<Buffer> packedRasterWorkGroupsBuffer,
    std::shared_ptr<Buffer> compactedRasterWorkIndicesBuffer,
    std::shared_ptr<Buffer> rasterWorkBuffer,
    std::shared_ptr<Buffer> diceQueueBuffer,
    std::shared_ptr<Buffer> tessTableConfigsBuffer,
    std::shared_ptr<Buffer> tessTableVerticesBuffer,
    std::shared_ptr<Buffer> tessTableTrianglesBuffer,
    std::shared_ptr<PixelBuffer> virtualShadowPageTableTexture,
    std::shared_ptr<PixelBuffer> virtualShadowPhysicalPagesTexture,
    std::shared_ptr<PixelBuffer> virtualShadowDynamicPagesTexture,
    std::shared_ptr<Buffer> virtualShadowClipmapInfoBuffer,
    std::shared_ptr<Buffer> telemetryBuffer,
    std::shared_ptr<ResourceGroup> slabResourceGroup)
    : m_visibleClustersBuffer(std::move(visibleClustersBuffer))
    , m_rasterBucketsHistogramBuffer(std::move(rasterBucketsHistogramBuffer))
    , m_rasterBucketsIndirectArgsBuffer(std::move(rasterBucketsIndirectArgsBuffer))
    , m_packedRasterWorkGroupsBuffer(std::move(packedRasterWorkGroupsBuffer))
    , m_compactedRasterWorkIndicesBuffer(std::move(compactedRasterWorkIndicesBuffer))
    , m_rasterWorkBuffer(std::move(rasterWorkBuffer))
    , m_diceQueueBuffer(std::move(diceQueueBuffer))
    , m_tessTableConfigsBuffer(std::move(tessTableConfigsBuffer))
    , m_tessTableVerticesBuffer(std::move(tessTableVerticesBuffer))
    , m_tessTableTrianglesBuffer(std::move(tessTableTrianglesBuffer))
    , m_virtualShadowPageTableTexture(std::move(virtualShadowPageTableTexture))
    , m_virtualShadowPhysicalPagesTexture(std::move(virtualShadowPhysicalPagesTexture))
    , m_virtualShadowDynamicPagesTexture(std::move(virtualShadowDynamicPagesTexture))
    , m_virtualShadowClipmapInfoBuffer(std::move(virtualShadowClipmapInfoBuffer))
    , m_telemetryBuffer(std::move(telemetryBuffer))
    , m_slabResourceGroup(std::move(slabResourceGroup)) {
    m_viewRasterInfoBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodViewRasterInfo), false, false, false, false);
    m_viewRasterInfoBuffer->SetName("CLodReyesVirtualShadowHardwareViewRasterInfo");
    rg::memory::SetResourceUsageHint(*m_viewRasterInfoBuffer, "Cluster LOD Reyes virtual shadow hardware");

    rhi::IndirectArg args[] = {
        {.kind = rhi::IndirectArgKind::Constant, .u = {.rootConstants = { IndirectCommandSignatureRootSignatureIndex, 0, 3 } } },
        {.kind = rhi::IndirectArgKind::DispatchMesh }
    };

    auto device = DeviceManager::GetInstance().GetDevice();
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, 2), sizeof(RasterizeClustersCommand) },
        PSOManager::GetInstance().GetRootSignature().GetHandle(),
        m_rasterizationCommandSignature);
}

ReyesVirtualShadowHardwareRasterPass::~ReyesVirtualShadowHardwareRasterPass() = default;

void ReyesVirtualShadowHardwareRasterPass::DeclareResourceUsages(RenderPassBuilder* builder) {
    builder->WithShaderResource(
            Builtin::PerObjectBuffer,
            Builtin::PerMeshBuffer,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerMaterialDataBuffer,
            Builtin::PerMaterialOpenPBRDataBuffer,
            Builtin::Material::TextureGroup,
            Builtin::Material::TextureStreamingMetadataBuffer,
            Builtin::CameraBuffer,
            Builtin::SkeletonResources::InverseBindMatrices,
            Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo,
            m_visibleClustersBuffer,
            m_rasterBucketsHistogramBuffer,
            m_packedRasterWorkGroupsBuffer,
            m_compactedRasterWorkIndicesBuffer,
            m_rasterWorkBuffer,
            m_diceQueueBuffer,
            m_tessTableConfigsBuffer,
            m_tessTableVerticesBuffer,
            m_tessTableTrianglesBuffer,
            m_viewRasterInfoBuffer,
            m_virtualShadowClipmapInfoBuffer)
		.WithUnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer)
        .WithIndirectArguments(m_rasterBucketsIndirectArgsBuffer)
        .WithUnorderedAccess(
            m_virtualShadowPageTableTexture,
            m_virtualShadowPhysicalPagesTexture,
            m_virtualShadowDynamicPagesTexture,
            Builtin::Shadows::CLodStats,
            m_telemetryBuffer)
        .IsGeometryPass();

    if (m_slabResourceGroup) {
        builder->WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
    }

    builder->WithConstantBuffer(Builtin::PerFrameBuffer);
}

void ReyesVirtualShadowHardwareRasterPass::Setup() {}

void ReyesVirtualShadowHardwareRasterPass::Update(const UpdateExecutionContext& executionContext) {
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

    m_passWidth = virtualShadowConfig.virtualResolution;
    m_passHeight = virtualShadowConfig.virtualResolution;

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

bool ReyesVirtualShadowHardwareRasterPass::DeclaredResourcesChanged() const {
    return m_declaredResourcesChanged;
}

PassReturn ReyesVirtualShadowHardwareRasterPass::Execute(PassExecutionContext& executionContext) {
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;
    const CLodVirtualShadowResolutionConfig virtualShadowConfig = CLodVirtualShadowBuildRuntimeResolutionConfig();

    rhi::PassBeginInfo p{};
    p.width = m_passWidth;
    p.height = m_passHeight;
    p.debugName = "CLod Reyes virtual shadow hardware raster pass";

    executionContext.commandList.BeginPass(p);

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
    commandList.BindLayout(PSOManager::GetInstance().GetRootSignature().GetHandle());

    uint32_t misc[NumMiscUintRootConstants] = {};
    misc[CLOD_RASTER_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX] = m_rasterBucketsHistogramBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = m_visibleClustersBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX] = m_viewRasterInfoBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_RASTER_REYES_TELEMETRY_DESCRIPTOR_INDEX] = m_telemetryBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    misc[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX] =
        m_virtualShadowPageTableTexture->GetUAVShaderVisibleInfo(UAVViewType::Texture2DArrayFull, 0).slot.index;
    misc[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX] = m_virtualShadowClipmapInfoBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_RASTER_VIRTUAL_SHADOW_PHYSICAL_PAGES_DESCRIPTOR_INDEX] =
        m_virtualShadowPhysicalPagesTexture->GetUAVShaderVisibleInfo(0).slot.index;
    misc[CLOD_RASTER_VIRTUAL_SHADOW_DYNAMIC_PAGES_DESCRIPTOR_INDEX] =
        m_virtualShadowDynamicPagesTexture->GetUAVShaderVisibleInfo(0).slot.index;
    misc[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_RESOLUTION] = virtualShadowConfig.pageTableResolution;
    misc[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
    misc[CLOD_RASTER_VIRTUAL_SHADOW_VIRTUAL_RESOLUTION] = virtualShadowConfig.virtualResolution;
    misc[CLOD_RASTER_REYES_PACKED_RASTER_WORK_GROUPS_DESCRIPTOR_INDEX] = m_packedRasterWorkGroupsBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_RASTER_REYES_COMPACTED_RASTER_WORK_INDICES_DESCRIPTOR_INDEX] = m_compactedRasterWorkIndicesBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_RASTER_REYES_RASTER_WORK_BUFFER_DESCRIPTOR_INDEX] = m_rasterWorkBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_RASTER_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] = m_diceQueueBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_RASTER_REYES_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = m_tessTableConfigsBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_RASTER_REYES_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX] = m_tessTableVerticesBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_RASTER_REYES_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX] = m_tessTableTrianglesBuffer->GetSRVInfo(0).slot.index;
    commandList.PushConstants(rhi::ShaderStage::AllGraphics, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, misc);

    auto& psoManager = PSOManager::GetInstance();
    const auto numBuckets = context.materialManager->GetRasterBucketCount();
    if (numBuckets == 0u) {
        return {};
    }

    auto apiResource = m_rasterBucketsIndirectArgsBuffer->GetAPIResource();
    const auto stride = sizeof(RasterizeClustersCommand);
    for (uint32_t i = 0; i < numBuckets; ++i) {
        const auto flags = context.materialManager->GetRasterFlagsForBucket(i);
        const PipelineState* pso = psoManager.TryGetClusterLODVirtualShadowReyesRasterPSO(flags);
        if (!pso) {
            continue;
        }
        BindResourceDescriptorIndices(commandList, pso->GetResourceDescriptorSlots());
        commandList.BindPipeline(pso->GetAPIPipelineState().GetHandle());

        const uint64_t argOffset = static_cast<uint64_t>(i) * stride;
        commandList.ExecuteIndirect(
            m_rasterizationCommandSignature->GetHandle(),
            apiResource.GetHandle(),
            argOffset,
            {},
            0,
            1);
    }

    return {};
}

void ReyesVirtualShadowHardwareRasterPass::Cleanup() {}
