#include "Render/GraphExtensions/ClusterLOD/ClusterSoftwareRasterizationPass.h"

#include <algorithm>

#include "Managers/MaterialManager.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Managers/ViewManager.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "Resources/components.h"
#include "BuiltinResources.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "../shaders/PerPassRootConstants/clodRasterizationRootConstants.h"

ClusterSoftwareRasterizationPass::ClusterSoftwareRasterizationPass(
    std::shared_ptr<Buffer> compactedVisibleClustersBuffer,
    std::shared_ptr<Buffer> compactedVisibleClusterTransformIndicesBuffer,
    std::shared_ptr<Buffer> rasterBucketsHistogramBuffer,
    std::shared_ptr<Buffer> rasterBucketsIndirectArgsBuffer,
    std::shared_ptr<Buffer> sortedToUnsortedMappingBuffer,
    std::shared_ptr<Buffer> viewRasterInfoBuffer,
    CLodRasterOutputKind outputKind,
    std::shared_ptr<PixelBuffer> virtualShadowPageTableTexture,
    std::shared_ptr<PixelBuffer> virtualShadowPhysicalPagesTexture,
    std::shared_ptr<PixelBuffer> virtualShadowDynamicPagesTexture,
    std::shared_ptr<Buffer> virtualShadowClipmapInfoBuffer,
    std::shared_ptr<Buffer> telemetryBuffer,
    std::shared_ptr<ResourceGroup> slabResourceGroup,
    bool runWhenComputeSWRasterEnabledOnly)
    : m_compactedVisibleClustersBuffer(std::move(compactedVisibleClustersBuffer))
    , m_compactedVisibleClusterTransformIndicesBuffer(std::move(compactedVisibleClusterTransformIndicesBuffer))
    , m_rasterBucketsHistogramBuffer(std::move(rasterBucketsHistogramBuffer))
    , m_rasterBucketsIndirectArgsBuffer(std::move(rasterBucketsIndirectArgsBuffer))
    , m_sortedToUnsortedMappingBuffer(std::move(sortedToUnsortedMappingBuffer))
    , m_viewRasterInfoBuffer(std::move(viewRasterInfoBuffer))
    , m_virtualShadowPageTableTexture(std::move(virtualShadowPageTableTexture))
    , m_virtualShadowPhysicalPagesTexture(std::move(virtualShadowPhysicalPagesTexture))
    , m_virtualShadowDynamicPagesTexture(std::move(virtualShadowDynamicPagesTexture))
    , m_virtualShadowClipmapInfoBuffer(std::move(virtualShadowClipmapInfoBuffer))
    , m_telemetryBuffer(std::move(telemetryBuffer))
    , m_slabResourceGroup(std::move(slabResourceGroup))
    , m_outputKind(outputKind)
    , m_runWhenComputeSWRasterEnabledOnly(runWhenComputeSWRasterEnabledOnly) {
    rhi::IndirectArg args[] = {
        {.kind = rhi::IndirectArgKind::Constant, .u = {.rootConstants = { IndirectCommandSignatureRootSignatureIndex, 0, 3 } } },
        {.kind = rhi::IndirectArgKind::Dispatch }
    };

    auto device = DeviceManager::GetInstance().GetDevice();
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, 2), sizeof(RasterizeClustersCommand) },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        m_rasterizationCommandSignature);
}

ClusterSoftwareRasterizationPass::~ClusterSoftwareRasterizationPass() = default;

void ClusterSoftwareRasterizationPass::DeclareResourceUsages(ComputePassBuilder* builder) {
    builder->WithShaderResource(
            Builtin::PerMeshBuffer,
            Builtin::PerMaterialDataBuffer,
            Builtin::Material::TextureGroup,
            Builtin::Material::TextureStreamingMetadataBuffer,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerObjectBuffer,
            Builtin::CLod::Offsets,
            Builtin::CLod::MeshMetadata,
            Builtin::CLod::Groups,
            Builtin::CLod::AssemblyTransforms,
            Builtin::CLod::AssemblyBoneRemaps,
            Builtin::CLod::AssemblyBoneRemapIndices,
            Builtin::CullingCameraBuffer,
            Builtin::SkeletonResources::InverseBindMatrices,
            Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo,
            m_compactedVisibleClustersBuffer,
            m_compactedVisibleClusterTransformIndicesBuffer,
            m_rasterBucketsHistogramBuffer,
            m_sortedToUnsortedMappingBuffer,
            m_viewRasterInfoBuffer)
        .WithUnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer)
        .WithIndirectArguments(m_rasterBucketsIndirectArgsBuffer)
        .WithUnorderedAccess(Builtin::DebugVisualization);

    if (m_outputKind == CLodRasterOutputKind::VisibilityBuffer) {
        for (auto& vb : m_visibilityBuffers) {
            builder->WithUnorderedAccess(vb);
        }
    }
    else if (m_outputKind == CLodRasterOutputKind::VirtualShadow) {
        builder->WithShaderResource(m_virtualShadowClipmapInfoBuffer)
            .WithUnorderedAccess(
                m_virtualShadowPageTableTexture,
                m_virtualShadowPhysicalPagesTexture,
                m_virtualShadowDynamicPagesTexture);
        if (m_telemetryBuffer) {
            builder->WithUnorderedAccess(m_telemetryBuffer);
        }
    }

    if (m_slabResourceGroup) {
        builder->WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
    }

    builder->WithConstantBuffer(Builtin::PerFrameBuffer);
}

void ClusterSoftwareRasterizationPass::Setup() {
}

void ClusterSoftwareRasterizationPass::Update(const UpdateExecutionContext& executionContext) {
    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;
    const CLodVirtualShadowResolutionConfig virtualShadowConfig = CLodVirtualShadowBuildRuntimeResolutionConfig();

    std::vector<std::shared_ptr<PixelBuffer>> nextVisibilityBuffers;
    auto numViews = context.viewManager->GetCameraBufferSize();
    std::vector<CLodViewRasterInfo> viewRasterInfo(numViews);

    context.viewManager->ForEachView([&](uint64_t v) {
        auto viewInfo = context.viewManager->Get(v);
        if (!viewInfo) {
            return;
        }

        auto cameraIndex = viewInfo->gpu.cameraBufferIndex;
        CLodViewRasterInfo info{};
        info.scissorMinX = 0;
        info.scissorMinY = 0;

        if (m_outputKind == CLodRasterOutputKind::VirtualShadow) {
            if (viewInfo->flags.shadow && viewInfo->lightType == Components::LightType::Directional) {
                info.scissorMaxX = virtualShadowConfig.virtualResolution;
                info.scissorMaxY = virtualShadowConfig.virtualResolution;
                info.viewportScaleX = 1.0f;
                info.viewportScaleY = 1.0f;
            }
            viewRasterInfo[cameraIndex] = info;
            return;
        }

        if (viewInfo->gpu.visibilityBuffer == nullptr) {
            return;
        }

        info.visibilityUAVDescriptorIndex = viewInfo->gpu.visibilityBuffer->GetUAVShaderVisibleInfo(0).slot.index;
        info.scissorMaxX = viewInfo->gpu.visibilityBuffer->GetWidth();
        info.scissorMaxY = viewInfo->gpu.visibilityBuffer->GetHeight();
        info.viewportScaleX = 1.0f;
        info.viewportScaleY = 1.0f;
        viewRasterInfo[cameraIndex] = info;
        nextVisibilityBuffers.push_back(viewInfo->gpu.visibilityBuffer);
    });

    m_viewRasterInfoBuffer->ResizeStructured(static_cast<uint32_t>(viewRasterInfo.size()));
    BUFFER_UPLOAD(
        viewRasterInfo.data(),
        static_cast<uint32_t>(viewRasterInfo.size() * sizeof(CLodViewRasterInfo)),
        rg::runtime::UploadTarget::FromShared(m_viewRasterInfoBuffer),
        0);

    m_declaredResourcesChanged = (nextVisibilityBuffers != m_visibilityBuffers);
    m_visibilityBuffers = std::move(nextVisibilityBuffers);
}

bool ClusterSoftwareRasterizationPass::DeclaredResourcesChanged() const {
    return m_declaredResourcesChanged;
}

PassReturn ClusterSoftwareRasterizationPass::Execute(PassExecutionContext& executionContext) {
    if (m_runWhenComputeSWRasterEnabledOnly && !CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)())) {
        return {};
    }
    if (m_outputKind == CLodRasterOutputKind::VisibilityBuffer &&
        SettingsManager::GetInstance().getSettingGetter<bool>(CLodDisableNonVoxelVisibilitySettingName)()) {
        return {};
    }

    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());

    uint32_t misc[NumMiscUintRootConstants] = {};
    misc[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    misc[CLOD_RASTER_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX] = m_rasterBucketsHistogramBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = m_compactedVisibleClustersBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = m_compactedVisibleClusterTransformIndicesBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX] = m_viewRasterInfoBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_RASTER_SORTED_TO_UNSORTED_MAPPING_DESCRIPTOR_INDEX] = m_sortedToUnsortedMappingBuffer->GetSRVInfo(0).slot.index;
    if (m_outputKind == CLodRasterOutputKind::VirtualShadow) {
        const CLodVirtualShadowResolutionConfig virtualShadowConfig = CLodVirtualShadowBuildRuntimeResolutionConfig();
        misc[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX] = m_virtualShadowPageTableTexture->GetUAVShaderVisibleInfo(UAVViewType::Texture2DArrayFull, 0).slot.index;
        misc[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX] = m_virtualShadowClipmapInfoBuffer->GetSRVInfo(0).slot.index;
        misc[CLOD_RASTER_VIRTUAL_SHADOW_PHYSICAL_PAGES_DESCRIPTOR_INDEX] = m_virtualShadowPhysicalPagesTexture->GetUAVShaderVisibleInfo(0).slot.index;
        misc[CLOD_RASTER_VIRTUAL_SHADOW_DYNAMIC_PAGES_DESCRIPTOR_INDEX] =
            m_virtualShadowDynamicPagesTexture->GetUAVShaderVisibleInfo(0).slot.index;
        misc[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_RESOLUTION] = virtualShadowConfig.pageTableResolution;
        misc[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
        misc[CLOD_RASTER_VIRTUAL_SHADOW_VIRTUAL_RESOLUTION] = virtualShadowConfig.virtualResolution;
        if (m_telemetryBuffer) {
            misc[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX] =
                m_telemetryBuffer->GetUAVShaderVisibleInfo(0).slot.index;
        }
    }
    commandList.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, misc);

    auto numBuckets = context.materialManager->GetRasterBucketCount();
    if (numBuckets == 0) {
        return {};
    }

    auto apiResource = m_rasterBucketsIndirectArgsBuffer->GetAPIResource();
    auto stride = sizeof(RasterizeClustersCommand);
    for (uint32_t i = 0; i < numBuckets; ++i) {
        auto flags = context.materialManager->GetRasterFlagsForBucket(i);
        const PipelineState* pso = PSOManager::GetInstance().TryGetClusterLODSoftwareRasterPSO(flags, m_outputKind);
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

void ClusterSoftwareRasterizationPass::Cleanup() {}
