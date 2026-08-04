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
#include "Render/MemoryIntrospectionAPI.h"
#include "Resources/components.h"
#include "BuiltinResources.h"
#include "ShaderBuffers.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "../shaders/PerPassRootConstants/clodClearUintBufferRootConstants.h"
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

    rhi::IndirectArg dispatchArg[] = {
        {.kind = rhi::IndirectArgKind::Dispatch }
    };
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(dispatchArg, 1), 12u },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        m_dynamicWindSkinCacheDispatchCommandSignature);

    if (m_outputKind == CLodRasterOutputKind::VirtualShadow &&
        SettingsManager::GetInstance().getSettingGetter<bool>(
            CLodDynamicWindVertexCacheEnabledSettingName)()) {
        constexpr uint64_t hashEntryStride = 36u;
        constexpr uint64_t workRecordStride = sizeof(uint32_t);
        constexpr uint64_t positionStride = sizeof(float) * 3u;
        const uint64_t budgetMiB = std::min<uint32_t>(
            SettingsManager::GetInstance().getSettingGetter<uint32_t>(
                CLodDynamicWindVertexCacheMiBSettingName)(),
            512u);
        const uint64_t budgetBytes = budgetMiB * 1024u * 1024u;
        const uint64_t metadataBytes = budgetBytes / 4u;
        const uint64_t positionBytes = budgetBytes - metadataBytes;
        const uint64_t visibleClusterCapacity =
            m_compactedVisibleClustersBuffer->GetBufferSize() / PackedVisibleClusterStrideBytes;
        const uint64_t mappingBytes = visibleClusterCapacity * sizeof(uint32_t);

        constexpr uint64_t fixedMetadataBytes = sizeof(uint32_t) * 2u + 12u;
        if (visibleClusterCapacity != 0u &&
            metadataBytes > mappingBytes + fixedMetadataBytes + hashEntryStride + workRecordStride) {
            m_dynamicWindSkinCacheHashEntryCount = static_cast<uint32_t>(
                std::min<uint64_t>(
                    (metadataBytes - mappingBytes - fixedMetadataBytes) /
                        (hashEntryStride + workRecordStride),
                    UINT32_MAX));
            m_dynamicWindSkinCachePositionCapacity = static_cast<uint32_t>(
                std::min<uint64_t>(positionBytes / positionStride, UINT32_MAX));

            if (m_dynamicWindSkinCacheHashEntryCount != 0u &&
                m_dynamicWindSkinCachePositionCapacity != 0u) {
                m_dynamicWindSkinCacheMappingBuffer = CreateAliasedUnmaterializedStructuredBuffer(
                    static_cast<uint32_t>(visibleClusterCapacity), sizeof(uint32_t), true, false, false, true);
                m_dynamicWindSkinCacheHashBuffer = CreateAliasedUnmaterializedRawBuffer(
                    static_cast<uint64_t>(m_dynamicWindSkinCacheHashEntryCount) * hashEntryStride,
                    true,
                    false,
                    true);
                m_dynamicWindSkinCachePositionsBuffer = CreateAliasedUnmaterializedStructuredBuffer(
                    m_dynamicWindSkinCachePositionCapacity,
                    static_cast<uint32_t>(positionStride),
                    true,
                    false,
                    false,
                    true);
                m_dynamicWindSkinCacheAllocatorBuffer = CreateAliasedUnmaterializedStructuredBuffer(
                    2u, sizeof(uint32_t), true, false, false, true);
                m_dynamicWindSkinCacheWorkRecordsBuffer = CreateAliasedUnmaterializedStructuredBuffer(
                    m_dynamicWindSkinCacheHashEntryCount, sizeof(uint32_t), true, false, false, true);
                m_dynamicWindSkinCacheIndirectArgsBuffer = CreateAliasedUnmaterializedRawBuffer(
                    12u, true, true, true);

                m_dynamicWindSkinCacheMappingBuffer->SetName("CLod DynamicWind Skin Cache Mapping");
                m_dynamicWindSkinCacheHashBuffer->SetName("CLod DynamicWind Skin Cache Hash");
                m_dynamicWindSkinCachePositionsBuffer->SetName("CLod DynamicWind Skin Cache Positions");
                m_dynamicWindSkinCacheAllocatorBuffer->SetName("CLod DynamicWind Skin Cache Allocator");
                m_dynamicWindSkinCacheWorkRecordsBuffer->SetName("CLod DynamicWind Skin Cache Work Records");
                m_dynamicWindSkinCacheIndirectArgsBuffer->SetName("CLod DynamicWind Skin Cache Indirect Args");
                rg::memory::SetResourceUsageHint(*m_dynamicWindSkinCacheMappingBuffer, "DynamicWind VSM vertex cache");
                rg::memory::SetResourceUsageHint(*m_dynamicWindSkinCacheHashBuffer, "DynamicWind VSM vertex cache");
                rg::memory::SetResourceUsageHint(*m_dynamicWindSkinCachePositionsBuffer, "DynamicWind VSM vertex cache");
                rg::memory::SetResourceUsageHint(*m_dynamicWindSkinCacheAllocatorBuffer, "DynamicWind VSM vertex cache");
                rg::memory::SetResourceUsageHint(*m_dynamicWindSkinCacheWorkRecordsBuffer, "DynamicWind VSM vertex cache");
                rg::memory::SetResourceUsageHint(*m_dynamicWindSkinCacheIndirectArgsBuffer, "DynamicWind VSM vertex cache");

                std::vector<DxcDefine> defines = {
                    { L"CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW", L"1" },
                    { L"CLOD_VSM_TWO_LAYER_RASTER_VERSION", L"2" }
                };
                auto& psoManager = PSOManager::GetInstance();
                const auto rootSignature = psoManager.GetComputeRootSignature().GetHandle();
                m_dynamicWindSkinCacheBuildPipeline = psoManager.MakeComputePipeline(
                    rootSignature,
                    L"Shaders/ClusterLOD/dynamicWindSkinCache.hlsl",
                    L"DynamicWindSkinCacheBuildCS",
                    defines,
                    "CLod_DynamicWindSkinCacheBuild");
                m_dynamicWindSkinCacheSkinPipeline = psoManager.MakeComputePipeline(
                    rootSignature,
                    L"Shaders/ClusterLOD/dynamicWindSkinCache.hlsl",
                    L"DynamicWindSkinCacheSkinWorkCS",
                    defines,
                    "CLod_DynamicWindSkinCacheSkinWork");
                m_dynamicWindSkinCacheFinalizePipeline = psoManager.MakeComputePipeline(
                    rootSignature,
                    L"Shaders/ClusterLOD/dynamicWindSkinCache.hlsl",
                    L"DynamicWindSkinCacheFinalizeCS",
                    defines,
                    "CLod_DynamicWindSkinCacheFinalize");
                m_dynamicWindSkinCacheResolvePipeline = psoManager.MakeComputePipeline(
                    rootSignature,
                    L"Shaders/ClusterLOD/dynamicWindSkinCache.hlsl",
                    L"DynamicWindSkinCacheResolveCS",
                    defines,
                    "CLod_DynamicWindSkinCacheResolve");
                m_dynamicWindSkinCacheClearPipeline = psoManager.MakeComputePipeline(
                    rootSignature,
                    L"Shaders/ClusterLOD/clodUtil.hlsl",
                    L"ClearUintStructuredBufferCSMain",
                    {},
                    "CLod_DynamicWindSkinCacheClear");
            }
        }
    }
}

ClusterSoftwareRasterizationPass::~ClusterSoftwareRasterizationPass() = default;

void ClusterSoftwareRasterizationPass::DeclareResourceUsages(ComputePassBuilder* builder) {
    builder->WithShaderResource(
            Builtin::PerMeshBuffer,
            Builtin::PerMaterialDataBuffer,
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
            Builtin::CameraBuffer,
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
        builder->WithShaderResource(
                m_virtualShadowClipmapInfoBuffer,
                Builtin::Shadows::CLodDirectionalPageViewInfo)
            .WithUnorderedAccess(
                m_virtualShadowPageTableTexture,
                m_virtualShadowPhysicalPagesTexture,
                m_virtualShadowDynamicPagesTexture);
        if (m_telemetryBuffer) {
            builder->WithUnorderedAccess(m_telemetryBuffer);
        }
        if (m_dynamicWindSkinCacheHashBuffer) {
            builder->WithUnorderedAccess(
                m_dynamicWindSkinCacheMappingBuffer,
                m_dynamicWindSkinCacheHashBuffer,
                m_dynamicWindSkinCachePositionsBuffer,
                m_dynamicWindSkinCacheAllocatorBuffer,
                m_dynamicWindSkinCacheWorkRecordsBuffer,
                m_dynamicWindSkinCacheIndirectArgsBuffer)
                .WithIndirectArguments(m_dynamicWindSkinCacheIndirectArgsBuffer)
                .WithShaderResource("Builtin::DynamicWind::VisibleSkeletonMembership");
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
    misc[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_MAPPING_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    misc[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_HASH_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    misc[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_POSITIONS_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    misc[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_ALLOCATOR_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    misc[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_WORK_RECORDS_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    misc[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_INDIRECT_ARGS_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    misc[CLOD_RASTER_DYNAMIC_WIND_VISIBLE_MEMBERSHIP_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
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
        if (m_dynamicWindSkinCacheHashBuffer) {
            ++m_dynamicWindSkinCacheGeneration;
            if (m_dynamicWindSkinCacheGeneration == 0u ||
                m_dynamicWindSkinCacheGeneration >= 0x7FFFFFFFu) {
                m_dynamicWindSkinCacheGeneration = 1u;
            }
            misc[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_MAPPING_DESCRIPTOR_INDEX] =
                m_dynamicWindSkinCacheMappingBuffer->GetUAVShaderVisibleInfo(0).slot.index;
            misc[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_HASH_DESCRIPTOR_INDEX] =
                m_dynamicWindSkinCacheHashBuffer->GetUAVShaderVisibleInfo(0).slot.index;
            misc[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_HASH_ENTRY_COUNT] =
                m_dynamicWindSkinCacheHashEntryCount;
            misc[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_GENERATION] =
                m_dynamicWindSkinCacheGeneration;
            misc[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_POSITIONS_DESCRIPTOR_INDEX] =
                m_dynamicWindSkinCachePositionsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
            misc[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_POSITION_CAPACITY] =
                m_dynamicWindSkinCachePositionCapacity;
            misc[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_ALLOCATOR_DESCRIPTOR_INDEX] =
                m_dynamicWindSkinCacheAllocatorBuffer->GetUAVShaderVisibleInfo(0).slot.index;
            misc[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_WORK_RECORDS_DESCRIPTOR_INDEX] =
                m_dynamicWindSkinCacheWorkRecordsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
            misc[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_INDIRECT_ARGS_DESCRIPTOR_INDEX] =
                m_dynamicWindSkinCacheIndirectArgsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
            misc[CLOD_RASTER_DYNAMIC_WIND_VISIBLE_MEMBERSHIP_DESCRIPTOR_INDEX] =
                m_resourceRegistryView
                    ->RequestPtr<GloballyIndexedResource>("Builtin::DynamicWind::VisibleSkeletonMembership")
                    ->GetSRVInfo(0).slot.index;
        }
    }
    commandList.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, misc);

    auto numBuckets = context.materialManager->GetRasterBucketCount();
    if (numBuckets == 0) {
        return {};
    }

    auto apiResource = m_rasterBucketsIndirectArgsBuffer->GetAPIResource();
    auto stride = sizeof(RasterizeClustersCommand);

    if (m_dynamicWindSkinCacheHashBuffer) {
        BindResourceDescriptorIndices(commandList, m_dynamicWindSkinCacheClearPipeline.GetResourceDescriptorSlots());
        commandList.BindPipeline(m_dynamicWindSkinCacheClearPipeline.GetAPIPipelineState().GetHandle());
        uint32_t clearConstants[NumMiscUintRootConstants] = {};
        clearConstants[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] =
            m_dynamicWindSkinCacheAllocatorBuffer->GetUAVShaderVisibleInfo(0).slot.index;
        clearConstants[CLOD_CLEAR_UINT_BUFFER_VALUE] = 0u;
        clearConstants[CLOD_CLEAR_UINT_BUFFER_COUNT] = 2u;
        commandList.PushConstants(
            rhi::ShaderStage::Compute,
            0,
            MiscUintRootSignatureIndex,
            0,
            NumMiscUintRootConstants,
            clearConstants);
        commandList.Dispatch(1u, 1u, 1u);
        commandList.PushConstants(
            rhi::ShaderStage::Compute,
            0,
            MiscUintRootSignatureIndex,
            0,
            NumMiscUintRootConstants,
            misc);

        auto dispatchCacheStage = [&](const PipelineState& pipeline) {
            BindResourceDescriptorIndices(commandList, pipeline.GetResourceDescriptorSlots());
            commandList.BindPipeline(pipeline.GetAPIPipelineState().GetHandle());
            for (uint32_t bucket = 0; bucket < numBuckets; ++bucket) {
                const uint64_t argOffset = static_cast<uint64_t>(bucket) * stride;
                commandList.ExecuteIndirect(
                    m_rasterizationCommandSignature->GetHandle(),
                    apiResource.GetHandle(),
                    argOffset,
                    {},
                    0,
                    1);
            }
        };
        auto uavBarrier = [&](const std::shared_ptr<Buffer>& buffer) {
            rhi::BufferBarrier barrier{};
            barrier.buffer = buffer->GetAPIResource().GetHandle();
            barrier.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
            barrier.afterAccess = rhi::ResourceAccessType::UnorderedAccess;
            barrier.beforeSync = rhi::ResourceSyncState::ComputeShading;
            barrier.afterSync = rhi::ResourceSyncState::ComputeShading;
            rhi::BarrierBatch batch{};
            batch.buffers = { &barrier };
            commandList.Barriers(batch);
        };

        uavBarrier(m_dynamicWindSkinCacheAllocatorBuffer);
        dispatchCacheStage(m_dynamicWindSkinCacheBuildPipeline);
        uavBarrier(m_dynamicWindSkinCacheHashBuffer);
        uavBarrier(m_dynamicWindSkinCacheAllocatorBuffer);
        uavBarrier(m_dynamicWindSkinCacheWorkRecordsBuffer);
        BindResourceDescriptorIndices(commandList, m_dynamicWindSkinCacheFinalizePipeline.GetResourceDescriptorSlots());
        commandList.BindPipeline(m_dynamicWindSkinCacheFinalizePipeline.GetAPIPipelineState().GetHandle());
        commandList.Dispatch(1u, 1u, 1u);
        rhi::BufferBarrier indirectArgsBarrier{};
        indirectArgsBarrier.buffer = m_dynamicWindSkinCacheIndirectArgsBuffer->GetAPIResource().GetHandle();
        indirectArgsBarrier.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
        indirectArgsBarrier.afterAccess = rhi::ResourceAccessType::IndirectArgument;
        indirectArgsBarrier.beforeSync = rhi::ResourceSyncState::ComputeShading;
        indirectArgsBarrier.afterSync = rhi::ResourceSyncState::ExecuteIndirect;
        rhi::BarrierBatch indirectArgsBarrierBatch{};
        indirectArgsBarrierBatch.buffers = { &indirectArgsBarrier };
        commandList.Barriers(indirectArgsBarrierBatch);
        BindResourceDescriptorIndices(commandList, m_dynamicWindSkinCacheSkinPipeline.GetResourceDescriptorSlots());
        commandList.BindPipeline(m_dynamicWindSkinCacheSkinPipeline.GetAPIPipelineState().GetHandle());
        commandList.ExecuteIndirect(
            m_dynamicWindSkinCacheDispatchCommandSignature->GetHandle(),
            m_dynamicWindSkinCacheIndirectArgsBuffer->GetAPIResource().GetHandle(),
            0u,
            {},
            0u,
            1u);
        uavBarrier(m_dynamicWindSkinCachePositionsBuffer);
        dispatchCacheStage(m_dynamicWindSkinCacheResolvePipeline);
        uavBarrier(m_dynamicWindSkinCacheMappingBuffer);
    }

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
