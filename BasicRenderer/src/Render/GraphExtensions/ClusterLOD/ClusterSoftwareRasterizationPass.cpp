#include "Render/GraphExtensions/ClusterLOD/ClusterSoftwareRasterizationPass.h"

#include <algorithm>

#include "Managers/MaterialManager.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Managers/ViewManager.h"
#include "Render/RenderContext.h"
#include "RenderPasses/PreparedComputeDispatch.h"
#include "RenderPasses/PreparedComputeBarrier.h"
#include "Render/Runtime/UploadTypes.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Resources/components.h"
#include "BuiltinResources.h"
#include "ShaderBuffers.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "../shaders/PerPassRootConstants/clodClearUintBufferRootConstants.h"
#include "../shaders/PerPassRootConstants/clodRasterizationRootConstants.h"

void ClusterSoftwareRasterizationPass::Record(
    const ClusterSoftwareRasterFrameData& frame, const uint32_t& generation,
    org::PassRecordContext& recording) {
    if (!frame.enabled || frame.bucketCount == 0u) return;

    auto& commands = recording.Commands();
    commands.SetDescriptorHeaps(frame.raster.resourceHeap, frame.raster.samplerHeap);
    auto bindProgram = [&](const org::PreparedProgramBinding& binding) {
        commands.BindLayout(recording.ResolveLayout(binding.program));
        commands.BindPipeline(recording.Resolve(binding.program));
        if (!binding.descriptorIndices.empty()) {
            commands.PushConstants(rhi::ShaderStage::Compute, 0,
                org::shaderapi::kResourceDescriptorIndicesRootParameter, 0,
                static_cast<uint32_t>(binding.descriptorIndices.size()),
                binding.descriptorIndices.data());
        }
    };

    auto cacheConstants = frame.cacheConstants;
    cacheConstants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_GENERATION] = generation;
    if (frame.hasSkinCache) {
        bindProgram(frame.clearProgram);
        commands.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0,
            NumMiscUintRootConstants, frame.clearConstants.data());
        commands.Dispatch(1u, 1u, 1u);
        commands.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0,
            NumMiscUintRootConstants, cacheConstants.data());

        const auto rasterArguments = recording.Resolve(*frame.raster.argumentsReference).GetHandle();
        auto dispatchBuckets = [&](const org::PreparedProgramBinding& binding) {
            bindProgram(binding);
            for (uint32_t bucket = 0; bucket < frame.bucketCount; ++bucket) {
                commands.ExecuteIndirect(frame.raster.commandSignature, rasterArguments,
                    static_cast<uint64_t>(bucket) * sizeof(RasterizeClustersCommand),
                    {}, 0u, 1u);
            }
        };

        br::render::RecordPreparedComputeUavBarrier(frame.cacheAllocator, recording);
        dispatchBuckets(frame.buildProgram);
        br::render::RecordPreparedComputeUavBarrier(frame.cacheHash, recording);
        br::render::RecordPreparedComputeUavBarrier(frame.cacheAllocator, recording);
        br::render::RecordPreparedComputeUavBarrier(frame.cacheWorkRecords, recording);
        bindProgram(frame.finalizeProgram);
        commands.Dispatch(1u, 1u, 1u);

        rhi::BufferBarrier indirectBarrier{};
        indirectBarrier.buffer = recording.Resolve(frame.cacheIndirectArgs).GetHandle();
        indirectBarrier.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
        indirectBarrier.afterAccess = rhi::ResourceAccessType::IndirectArgument;
        indirectBarrier.beforeSync = rhi::ResourceSyncState::ComputeShading;
        indirectBarrier.afterSync = rhi::ResourceSyncState::ExecuteIndirect;
        rhi::BarrierBatch indirectBatch{};
        indirectBatch.buffers = {&indirectBarrier};
        commands.Barriers(indirectBatch);

        bindProgram(frame.skinProgram);
        commands.ExecuteIndirect(frame.cacheDispatchSignature,
            recording.Resolve(frame.cacheIndirectArgs).GetHandle(), 0u, {}, 0u, 1u);
        br::render::RecordPreparedComputeUavBarrier(frame.cachePositions, recording);
        dispatchBuckets(frame.resolveProgram);
        br::render::RecordPreparedComputeUavBarrier(frame.cacheMapping, recording);
    }

    br::render::RecordPreparedComputeIndirectSequence(frame.raster, recording,
        std::pair<uint32_t, uint32_t>{CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_GENERATION, generation});
}

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
    m_rasterizationCommandSignature = std::make_shared<rhi::CommandSignaturePtr>();
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, 2), sizeof(RasterizeClustersCommand) },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        *m_rasterizationCommandSignature);

    rhi::IndirectArg dispatchArg[] = {
        {.kind = rhi::IndirectArgKind::Dispatch }
    };
    m_dynamicWindSkinCacheDispatchCommandSignature = std::make_shared<rhi::CommandSignaturePtr>();
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(dispatchArg, 1), 12u },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        *m_dynamicWindSkinCacheDispatchCommandSignature);

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
                org::memory::SetResourceUsageHint(*m_dynamicWindSkinCacheMappingBuffer, "DynamicWind VSM vertex cache");
                org::memory::SetResourceUsageHint(*m_dynamicWindSkinCacheHashBuffer, "DynamicWind VSM vertex cache");
                org::memory::SetResourceUsageHint(*m_dynamicWindSkinCachePositionsBuffer, "DynamicWind VSM vertex cache");
                org::memory::SetResourceUsageHint(*m_dynamicWindSkinCacheAllocatorBuffer, "DynamicWind VSM vertex cache");
                org::memory::SetResourceUsageHint(*m_dynamicWindSkinCacheWorkRecordsBuffer, "DynamicWind VSM vertex cache");
                org::memory::SetResourceUsageHint(*m_dynamicWindSkinCacheIndirectArgsBuffer, "DynamicWind VSM vertex cache");

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

ClusterSoftwareRasterBindings ClusterSoftwareRasterizationPass::Declare(org::PassBuilder& declaration) {
    auto* builder = &declaration;
    builder->PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
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
        .WithUnorderedAccess(Builtin::DebugVisualization);
    ClusterSoftwareRasterBindings bindings{
        builder->BindShaderResource(m_rasterBucketsHistogramBuffer),
        builder->BindShaderResource(m_compactedVisibleClustersBuffer),
        builder->BindShaderResource(m_compactedVisibleClusterTransformIndicesBuffer),
        builder->BindShaderResource(m_sortedToUnsortedMappingBuffer),
        builder->BindShaderResource(m_viewRasterInfoBuffer),
        builder->BindIndirectArguments(m_rasterBucketsIndirectArgsBuffer)};

    if (m_outputKind == CLodRasterOutputKind::VisibilityBuffer) {
        for (auto& vb : m_visibilityBuffers) {
            builder->WithUnorderedAccess(vb);
        }
    }
    else if (m_outputKind == CLodRasterOutputKind::VirtualShadow) {
        bindings.virtualShadow = true;
        builder->WithShaderResource(
                m_virtualShadowClipmapInfoBuffer,
                Builtin::Shadows::CLodDirectionalPageViewInfo)
            .WithUnorderedAccess(
                m_virtualShadowPageTableTexture,
                m_virtualShadowPhysicalPagesTexture,
                m_virtualShadowDynamicPagesTexture);
        if (m_telemetryBuffer) {
            builder->WithUnorderedAccess(m_telemetryBuffer);
            bindings.telemetry = builder->BindUnorderedAccess(m_telemetryBuffer);
            bindings.hasTelemetry = true;
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
            bindings.skinMapping = builder->BindUnorderedAccess(m_dynamicWindSkinCacheMappingBuffer);
            bindings.skinHash = builder->BindUnorderedAccess(m_dynamicWindSkinCacheHashBuffer);
            bindings.skinPositions = builder->BindUnorderedAccess(m_dynamicWindSkinCachePositionsBuffer);
            bindings.skinAllocator = builder->BindUnorderedAccess(m_dynamicWindSkinCacheAllocatorBuffer);
            bindings.skinWork = builder->BindUnorderedAccess(m_dynamicWindSkinCacheWorkRecordsBuffer);
            bindings.skinArgs = builder->BindUnorderedAccess(m_dynamicWindSkinCacheIndirectArgsBuffer);
            bindings.skinMembership = builder->BindShaderResource("Builtin::DynamicWind::VisibleSkeletonMembership");
            bindings.hasSkinCache = true;
        }
        bindings.pageTable = builder->BindUnorderedAccess(m_virtualShadowPageTableTexture);
        bindings.clipmapInfo = builder->BindShaderResource(m_virtualShadowClipmapInfoBuffer);
        bindings.physicalPages = builder->BindUnorderedAccess(m_virtualShadowPhysicalPagesTexture);
        bindings.dynamicPages = builder->BindUnorderedAccess(m_virtualShadowDynamicPagesTexture);
    }

    if (m_slabResourceGroup) {
        builder->WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
    }

    builder->WithConstantBuffer(Builtin::PerFrameBuffer);
    return bindings;
}

void ClusterSoftwareRasterizationPass::Update(const UpdateExecutionContext& executionContext) {
    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;
    const CLodVirtualShadowResolutionConfig virtualShadowConfig = CLodVirtualShadowBuildRuntimeResolutionConfig();

    std::vector<std::shared_ptr<PixelBuffer>> nextVisibilityBuffers;
    auto numViews = context.ViewCameraBufferSize();
    std::vector<CLodViewRasterInfo> viewRasterInfo(numViews);

    for (const auto& viewInfo : context.Views()) {
        auto cameraIndex = viewInfo.cameraBufferIndex;
        if (cameraIndex >= viewRasterInfo.size()) continue;
        CLodViewRasterInfo info{};
        info.scissorMinX = 0;
        info.scissorMinY = 0;

        if (m_outputKind == CLodRasterOutputKind::VirtualShadow) {
            if (viewInfo.shadow && viewInfo.lightType == Components::LightType::Directional) {
                info.scissorMaxX = virtualShadowConfig.virtualResolution;
                info.scissorMaxY = virtualShadowConfig.virtualResolution;
                info.viewportScaleX = 1.0f;
                info.viewportScaleY = 1.0f;
            }
            viewRasterInfo[cameraIndex] = info;
            continue;
        }

        if (viewInfo.visibilityBuffer == nullptr) {
            continue;
        }

        info.visibilityUAVDescriptorIndex = viewInfo.visibilityUAVIndex;
        info.scissorMaxX = viewInfo.visibilityBuffer->GetWidth();
        info.scissorMaxY = viewInfo.visibilityBuffer->GetHeight();
        info.viewportScaleX = 1.0f;
        info.viewportScaleY = 1.0f;
        viewRasterInfo[cameraIndex] = info;
        nextVisibilityBuffers.push_back(viewInfo.visibilityBuffer);
    }

    m_viewRasterInfoBuffer->ResizeStructured(static_cast<uint32_t>(viewRasterInfo.size()));
    UploadBufferData(
        viewRasterInfo.data(),
        static_cast<uint32_t>(viewRasterInfo.size() * sizeof(CLodViewRasterInfo)),
        org::runtime::UploadTarget::FromShared(m_viewRasterInfoBuffer),
        0);

    m_declaredResourcesChanged = (nextVisibilityBuffers != m_visibilityBuffers);
    m_visibilityBuffers = std::move(nextVisibilityBuffers);
}

bool ClusterSoftwareRasterizationPass::DeclaredResourcesChanged() const {
    return m_declaredResourcesChanged;
}

ClusterSoftwareRasterFrameData ClusterSoftwareRasterizationPass::BuildRecipe(
    const ClusterSoftwareRasterBindings& bindings, const org::PassPrepareContext& preparation) const {
    ClusterSoftwareRasterFrameData frame{};
    if (m_runWhenComputeSWRasterEnabledOnly && !CLodSoftwareRasterUsesCompute(
        SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)()))
        return frame;
    if (m_outputKind == CLodRasterOutputKind::VisibilityBuffer &&
        SettingsManager::GetInstance().getSettingGetter<bool>(CLodDisableNonVoxelVisibilitySettingName)())
        return frame;
    const auto* context = preparation.preparationData
        ? preparation.preparationData->Get<UpdateContext>() : nullptr;
    if (!context) throw std::logic_error("CLod software-raster preparation requires the owned render snapshot");
    frame.enabled = true;
    auto& data = frame.raster;
    data.resourceHeap = context->textureDescriptorHeap.GetHandle(); data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.commandSignature = preparation.CaptureCommandSignature(m_rasterizationCommandSignature);
    data.argumentsReference = preparation.CaptureResource(bindings.indirectArgs);
    const auto srv = [&](org::ResourceBindingToken token) {
        return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource}).index;
    };
    const auto uav = [&](org::ResourceBindingToken token, uint32_t variant = UINT32_MAX) {
        return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess, variant}).index;
    };
    std::array<unsigned int, NumMiscUintRootConstants> constants{};
    constants[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_MAPPING_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_HASH_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_POSITIONS_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_ALLOCATOR_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_WORK_RECORDS_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_INDIRECT_ARGS_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    constants[CLOD_RASTER_DYNAMIC_WIND_VISIBLE_MEMBERSHIP_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    // The view-info buffer embeds bindless output indices; resolve the complete
    // raster table from the same immutable shared-heap publication.
    constants[CLOD_RASTER_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX] =
        m_rasterBucketsHistogramBuffer->GetSRVInfo(0).slot.index;
    constants[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] =
        m_compactedVisibleClustersBuffer->GetSRVInfo(0).slot.index;
    constants[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] =
        m_compactedVisibleClusterTransformIndicesBuffer->GetSRVInfo(0).slot.index;
    constants[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX] =
        m_viewRasterInfoBuffer->GetSRVInfo(0).slot.index;
    constants[CLOD_RASTER_SORTED_TO_UNSORTED_MAPPING_DESCRIPTOR_INDEX] =
        m_sortedToUnsortedMappingBuffer->GetSRVInfo(0).slot.index;
    if (bindings.virtualShadow) {
        const auto config = CLodVirtualShadowBuildRuntimeResolutionConfig();
        constants[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX] =
            uav(bindings.pageTable, static_cast<uint32_t>(UAVViewType::Texture2DArrayFull));
        constants[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX] = srv(bindings.clipmapInfo);
        constants[CLOD_RASTER_VIRTUAL_SHADOW_PHYSICAL_PAGES_DESCRIPTOR_INDEX] = uav(bindings.physicalPages);
        constants[CLOD_RASTER_VIRTUAL_SHADOW_DYNAMIC_PAGES_DESCRIPTOR_INDEX] = uav(bindings.dynamicPages);
        constants[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_RESOLUTION] = config.pageTableResolution;
        constants[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
        constants[CLOD_RASTER_VIRTUAL_SHADOW_VIRTUAL_RESOLUTION] = config.virtualResolution;
        if (bindings.hasTelemetry) constants[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX] = uav(bindings.telemetry);
        if (bindings.hasSkinCache) {
            constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_MAPPING_DESCRIPTOR_INDEX] = uav(bindings.skinMapping);
            constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_HASH_DESCRIPTOR_INDEX] = uav(bindings.skinHash);
            constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_HASH_ENTRY_COUNT] = m_dynamicWindSkinCacheHashEntryCount;
            constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_GENERATION] = 0u;
            constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_POSITIONS_DESCRIPTOR_INDEX] = uav(bindings.skinPositions);
            constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_POSITION_CAPACITY] = m_dynamicWindSkinCachePositionCapacity;
            constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_ALLOCATOR_DESCRIPTOR_INDEX] = uav(bindings.skinAllocator);
            constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_WORK_RECORDS_DESCRIPTOR_INDEX] = uav(bindings.skinWork);
            constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_INDIRECT_ARGS_DESCRIPTOR_INDEX] = uav(bindings.skinArgs);
            constants[CLOD_RASTER_DYNAMIC_WIND_VISIBLE_MEMBERSHIP_DESCRIPTOR_INDEX] = srv(bindings.skinMembership);
        }
    }
    const auto numBuckets = context->preparedRasterBucketCount;
    frame.bucketCount = numBuckets;
    BT_PLOT("CLod.RasterArgs.PreparedSoftwareBucketCount", static_cast<int64_t>(numBuckets));
    BT_PLOT("CLod.RasterArgs.PreparedSoftwareBackingBytes", static_cast<int64_t>(m_rasterBucketsIndirectArgsBuffer->GetSize()));
    data.steps.reserve(numBuckets);
    for (uint32_t bucket = 0; bucket < numBuckets; ++bucket) {
        const auto flags = context->preparedRasterBucketFlags.at(bucket);
        const auto* pso = PSOManager::GetInstance().TryGetClusterLODSoftwareRasterPSO(flags, m_outputKind);
        if (!pso) continue;
        br::render::PreparedComputeIndirectSequence::Step step{};
        const auto binding = preparation.CaptureProgramBinding(*pso);
        step.program = binding.program;
        step.descriptorIndices = binding.descriptorIndices;
        step.constants = constants;
        step.argumentsOffset = static_cast<uint64_t>(bucket) * sizeof(RasterizeClustersCommand);
        data.steps.push_back(std::move(step));
    }
    if (bindings.hasSkinCache && numBuckets != 0u) {
        frame.hasSkinCache = true;
        frame.cacheConstants = constants;
        frame.clearConstants[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = uav(bindings.skinAllocator);
        frame.clearConstants[CLOD_CLEAR_UINT_BUFFER_COUNT] = 2u;
        frame.clearProgram = preparation.CaptureProgramBinding(m_dynamicWindSkinCacheClearPipeline);
        frame.buildProgram = preparation.CaptureProgramBinding(m_dynamicWindSkinCacheBuildPipeline);
        frame.finalizeProgram = preparation.CaptureProgramBinding(m_dynamicWindSkinCacheFinalizePipeline);
        frame.skinProgram = preparation.CaptureProgramBinding(m_dynamicWindSkinCacheSkinPipeline);
        frame.resolveProgram = preparation.CaptureProgramBinding(m_dynamicWindSkinCacheResolvePipeline);
        frame.cacheDispatchSignature = preparation.CaptureCommandSignature(m_dynamicWindSkinCacheDispatchCommandSignature);
        frame.cacheIndirectArgs = preparation.CaptureResource(bindings.skinArgs);
        frame.cacheAllocator = preparation.CaptureResource(bindings.skinAllocator);
        frame.cacheHash = preparation.CaptureResource(bindings.skinHash);
        frame.cacheWorkRecords = preparation.CaptureResource(bindings.skinWork);
        frame.cachePositions = preparation.CaptureResource(bindings.skinPositions);
        frame.cacheMapping = preparation.CaptureResource(bindings.skinMapping);
    }
    return frame;
}

uint32_t ClusterSoftwareRasterizationPass::PrepareInvocation(const ClusterSoftwareRasterFrameData& recipe,
    const ClusterSoftwareRasterBindings&, const org::PassPrepareContext&) const {
    if (!recipe.enabled || !recipe.hasSkinCache || recipe.bucketCount == 0u) return 0u;
    if (++m_dynamicWindSkinCacheGeneration == 0u || m_dynamicWindSkinCacheGeneration >= 0x7FFFFFFFu)
        m_dynamicWindSkinCacheGeneration = 1u;
    return m_dynamicWindSkinCacheGeneration;
}
std::vector<uint64_t> ClusterSoftwareRasterizationPass::RecipeRevision(const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    std::vector<uint64_t> revision{SettingsManager::GetInstance().Revision(), context->preparedRasterBucketCount,
        reinterpret_cast<uintptr_t>(m_rasterizationCommandSignature.get()),
        reinterpret_cast<uintptr_t>(m_dynamicWindSkinCacheClearPipeline.GetPayload().get()),
        reinterpret_cast<uintptr_t>(m_dynamicWindSkinCacheBuildPipeline.GetPayload().get()),
        reinterpret_cast<uintptr_t>(m_dynamicWindSkinCacheFinalizePipeline.GetPayload().get()),
        reinterpret_cast<uintptr_t>(m_dynamicWindSkinCacheSkinPipeline.GetPayload().get()),
        reinterpret_cast<uintptr_t>(m_dynamicWindSkinCacheResolvePipeline.GetPayload().get())};
    for (uint32_t i = 0; i < context->preparedRasterBucketCount; ++i) {
        const auto flags = context->preparedRasterBucketFlags.at(i);
        const auto* pso = PSOManager::GetInstance().TryGetClusterLODSoftwareRasterPSO(flags, m_outputKind);
        revision.push_back(static_cast<uint64_t>(flags));
        revision.push_back(reinterpret_cast<uintptr_t>(pso ? pso->GetPayload().get() : nullptr));
    }
    return revision;
}
