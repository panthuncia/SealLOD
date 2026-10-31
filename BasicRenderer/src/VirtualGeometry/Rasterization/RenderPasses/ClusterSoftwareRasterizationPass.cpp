#include "VirtualGeometry/Rasterization/RenderPasses/ClusterSoftwareRasterizationPass.h"
#include "Render/PassBuilders.h"

#include <algorithm>
#include <stdexcept>

#include <BasicTelemetry/Tracy.h>

#include "Materials/MaterialManager.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "Scene/Views/ViewManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Render/Runtime/UploadTypes.h"
#include "Runtime/GraphIntegration/Resolvers/ResourceGroupResolver.h"
#include "Render/MemoryIntrospectionAPI.h"
#include <BasicRenderer/Extensions/ResourceComponent.h>
#include "BuiltinResources.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "../shaders/PerPassRootConstants/clodClearUintBufferRootConstants.h"
#include "../shaders/PerPassRootConstants/clodRasterizationRootConstants.h"

namespace {

// Constants shared by the software raster steps and the skin-cache dispatches.
br::render::MiscConstants BuildRasterConstants(const ClusterSoftwareRasterBindings& bindings,
    const ClusterSoftwareRasterFrame& frame, org::PassRecordContext& context) {
    br::render::MiscConstants constants{};
    for (const auto index : {CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX,CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_MAPPING_DESCRIPTOR_INDEX,
        CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_HASH_DESCRIPTOR_INDEX,CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_POSITIONS_DESCRIPTOR_INDEX,
        CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_ALLOCATOR_DESCRIPTOR_INDEX,CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_WORK_RECORDS_DESCRIPTOR_INDEX,
        CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_INDIRECT_ARGS_DESCRIPTOR_INDEX,CLOD_RASTER_DYNAMIC_WIND_VISIBLE_MEMBERSHIP_DESCRIPTOR_INDEX})
        constants[index] = UINT32_MAX;
    constants[CLOD_RASTER_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX] = context.Index(bindings.histogram);
    constants[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = context.Index(bindings.visible);
    constants[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = context.Index(bindings.transforms);
    constants[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX] = frame.viewRasterInfoTable;
    constants[CLOD_RASTER_SORTED_TO_UNSORTED_MAPPING_DESCRIPTOR_INDEX] = context.Index(bindings.mapping);
    if (!bindings.virtualShadow) return constants;
    constants[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX] = context.Index(bindings.pageTable);
    constants[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX] = context.Index(bindings.clipmapInfo);
    constants[CLOD_RASTER_VIRTUAL_SHADOW_PHYSICAL_PAGES_DESCRIPTOR_INDEX] = context.Index(bindings.physicalPages);
    constants[CLOD_RASTER_VIRTUAL_SHADOW_DYNAMIC_PAGES_DESCRIPTOR_INDEX] = context.Index(bindings.dynamicPages);
    constants[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_RESOLUTION] = frame.pageTableResolution;
    constants[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
    constants[CLOD_RASTER_VIRTUAL_SHADOW_VIRTUAL_RESOLUTION] = frame.virtualResolution;
    if (bindings.hasTelemetry) constants[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX] = context.Index(bindings.telemetry);
    if (!bindings.hasSkinCache) return constants;
    constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_MAPPING_DESCRIPTOR_INDEX] = context.Index(bindings.skinMapping);
    constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_HASH_DESCRIPTOR_INDEX] = context.Index(bindings.skinHash);
    constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_HASH_ENTRY_COUNT] = bindings.skinCacheHashEntries;
    constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_GENERATION] = 0u;
    constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_POSITIONS_DESCRIPTOR_INDEX] = context.Index(bindings.skinPositions);
    constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_POSITION_CAPACITY] = bindings.skinCachePositionCapacity;
    constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_ALLOCATOR_DESCRIPTOR_INDEX] = context.Index(bindings.skinAllocator);
    constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_WORK_RECORDS_DESCRIPTOR_INDEX] = context.Index(bindings.skinWork);
    constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_INDIRECT_ARGS_DESCRIPTOR_INDEX] = context.Index(bindings.skinArgs);
    constants[CLOD_RASTER_DYNAMIC_WIND_VISIBLE_MEMBERSHIP_DESCRIPTOR_INDEX] = context.Index(bindings.skinMembership);
    return constants;
}

// Clear the allocator, build the cache over every bucket, finalize, skin the
// cached vertices and resolve them back over every bucket.
void RecordSkinCache(const ClusterSoftwareRasterBindings& bindings, const ClusterSoftwareRasterFrame& frame,
    br::render::MiscConstants constants, org::PassRecordContext& context) {
    const auto generation = frame.skinCacheGeneration;
    if (!generation || generation >= 0x7FFFFFFFu)
        throw std::invalid_argument("Software skin-cache invocation has a reserved generation");
    constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_GENERATION] = generation;
    const auto uavBarrier = [&](const org::DeclaredViewToken& token) {
        context.BufferBarriers({token.Resource()},
            rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceAccessType::UnorderedAccess);
    };
    const auto buckets = [&](const org::PipelineState& pipeline) {
        context.BindComputePipeline(pipeline);
        for (uint32_t bucket = 0; bucket < frame.bucketCount; ++bucket)
            context.ExecuteIndirect(bindings.rasterSignature, bindings.indirectArgs,
                uint64_t{bucket} * sizeof(RasterizeClustersCommand));
    };

    br::render::MiscConstants clear{};
    clear[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = context.Index(bindings.skinAllocator);
    clear[CLOD_CLEAR_UINT_BUFFER_COUNT] = 2u;
    context.BindComputePipeline(bindings.cacheClear);
    br::render::PushMiscConstants(context, clear);
    context.Commands().Dispatch(1, 1, 1);
    br::render::PushMiscConstants(context, constants);
    uavBarrier(bindings.skinAllocator);
    buckets(bindings.cacheBuild);
    uavBarrier(bindings.skinHash); uavBarrier(bindings.skinAllocator); uavBarrier(bindings.skinWork);
    context.BindComputePipeline(bindings.cacheFinalize);
    context.Commands().Dispatch(1, 1, 1);
    context.BufferBarriers({bindings.skinArgs.Resource()},
        rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceAccessType::IndirectArgument,
        rhi::ResourceSyncState::ComputeShading, rhi::ResourceSyncState::ExecuteIndirect);
    context.BindComputePipeline(bindings.cacheSkin);
    context.ExecuteIndirect(bindings.cacheDispatchSignature, bindings.skinArgs.Resource());
    uavBarrier(bindings.skinPositions);
    buckets(bindings.cacheResolve);
    uavBarrier(bindings.skinMapping);
}

} // namespace

void ClusterSoftwareRasterizationPass::Record(const ClusterSoftwareRasterBindings& bindings,
    const ClusterSoftwareRasterFrame& frame, org::PassRecordContext& context) {
    if (!frame.enabled || frame.bucketCount == 0u) return;

    auto constants = BuildRasterConstants(bindings, frame, context);
    if (frame.hasSkinCache) RecordSkinCache(bindings, frame, constants, context);

    constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_GENERATION] = frame.skinCacheGeneration;
    for (const auto& step : frame.steps) {
        context.BindComputePipeline(step.pipeline);
        br::render::PushMiscConstants(context, constants);
        context.ExecuteIndirect(bindings.rasterSignature, bindings.indirectArgs, step.argumentsOffset);
    }
}

ClusterSoftwareRasterizationPass::ClusterSoftwareRasterizationPass(
    std::shared_ptr<org::Buffer> compactedVisibleClustersBuffer,
    std::shared_ptr<org::Buffer> compactedVisibleClusterTransformIndicesBuffer,
    std::shared_ptr<org::Buffer> rasterBucketsHistogramBuffer,
    std::shared_ptr<org::Buffer> rasterBucketsIndirectArgsBuffer,
    std::shared_ptr<org::Buffer> sortedToUnsortedMappingBuffer,
    CLodRasterOutputKind outputKind,
    std::shared_ptr<org::PixelBuffer> virtualShadowPageTableTexture,
    std::shared_ptr<org::PixelBuffer> virtualShadowPhysicalPagesTexture,
    std::shared_ptr<org::PixelBuffer> virtualShadowDynamicPagesTexture,
    std::shared_ptr<org::Buffer> virtualShadowClipmapInfoBuffer,
    std::shared_ptr<org::Buffer> telemetryBuffer,
    std::shared_ptr<org::ResourceGroup> slabResourceGroup,
    bool runWhenComputeSWRasterEnabledOnly)
    : m_compactedVisibleClustersBuffer(std::move(compactedVisibleClustersBuffer))
    , m_compactedVisibleClusterTransformIndicesBuffer(std::move(compactedVisibleClusterTransformIndicesBuffer))
    , m_rasterBucketsHistogramBuffer(std::move(rasterBucketsHistogramBuffer))
    , m_rasterBucketsIndirectArgsBuffer(std::move(rasterBucketsIndirectArgsBuffer))
    , m_sortedToUnsortedMappingBuffer(std::move(sortedToUnsortedMappingBuffer))
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
    builder->ShaderResource(
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
            Builtin::SkeletonResources::SkinningInstanceInfo);
    builder->UnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer);
    builder->UnorderedAccess(Builtin::DebugVisualization);
    ClusterSoftwareRasterBindings bindings{m_rasterizationCommandSignature,
        m_dynamicWindSkinCacheDispatchCommandSignature,
        m_dynamicWindSkinCacheClearPipeline, m_dynamicWindSkinCacheBuildPipeline,
        m_dynamicWindSkinCacheFinalizePipeline, m_dynamicWindSkinCacheSkinPipeline,
        m_dynamicWindSkinCacheResolvePipeline,
        builder->ShaderResource(m_rasterBucketsHistogramBuffer).View(),
        builder->ShaderResource(m_compactedVisibleClustersBuffer).View(),
        builder->ShaderResource(m_compactedVisibleClusterTransformIndicesBuffer).View(),
        builder->ShaderResource(m_sortedToUnsortedMappingBuffer).View(),
        builder->IndirectArguments(m_rasterBucketsIndirectArgsBuffer)};

    m_viewRasterInfoTable.Declare(*builder);
    bindings.viewRasterInfoLayout = m_viewRasterInfoTable.Declaration();
    if (m_outputKind == CLodRasterOutputKind::VirtualShadow) {
        bindings.virtualShadow = true;
        builder->ShaderResource(Builtin::Shadows::CLodDirectionalPageViewInfo);
        if (m_telemetryBuffer) {
            bindings.telemetry = builder->UnorderedAccess(m_telemetryBuffer).View();
            bindings.hasTelemetry = true;
        }
        if (m_dynamicWindSkinCacheHashBuffer) {
            bindings.skinMapping = builder->UnorderedAccess(m_dynamicWindSkinCacheMappingBuffer).View();
            bindings.skinHash = builder->UnorderedAccess(m_dynamicWindSkinCacheHashBuffer).View();
            bindings.skinPositions = builder->UnorderedAccess(m_dynamicWindSkinCachePositionsBuffer).View();
            bindings.skinAllocator = builder->UnorderedAccess(m_dynamicWindSkinCacheAllocatorBuffer).View();
            bindings.skinWork = builder->UnorderedAccess(m_dynamicWindSkinCacheWorkRecordsBuffer).View();
            bindings.skinArgs = builder->UnorderedAccess(m_dynamicWindSkinCacheIndirectArgsBuffer).View();
            builder->IndirectArguments(m_dynamicWindSkinCacheIndirectArgsBuffer);
            bindings.skinMembership = builder->ShaderResource("Builtin::DynamicWind::VisibleSkeletonMembership").View();
            bindings.skinCacheHashEntries = m_dynamicWindSkinCacheHashEntryCount;
            bindings.skinCachePositionCapacity = m_dynamicWindSkinCachePositionCapacity;
            bindings.hasSkinCache = true;
        }
        bindings.pageTable = builder->UnorderedAccess(m_virtualShadowPageTableTexture,
            org::UavView{static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull)}).View();
        bindings.clipmapInfo = builder->ShaderResource(m_virtualShadowClipmapInfoBuffer).View();
        bindings.physicalPages = builder->UnorderedAccess(m_virtualShadowPhysicalPagesTexture).View();
        bindings.dynamicPages = builder->UnorderedAccess(m_virtualShadowDynamicPagesTexture).View();
    }

    if (m_slabResourceGroup) {
        builder->ShaderResource(ResourceGroupResolver(m_slabResourceGroup));
    }

    builder->ConstantBuffer(Builtin::PerFrameBuffer);
    return bindings;
}

void ClusterSoftwareRasterizationPass::Update(const org::UpdateExecutionContext& executionContext) {
    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;
    m_declaredResourcesChanged = m_viewRasterInfoTable.Update(context.Views(), context.ViewCameraBufferSize(), m_outputKind);
}

bool ClusterSoftwareRasterizationPass::DeclaredResourcesChanged() const {
    return m_declaredResourcesChanged;
}

ClusterSoftwareRasterFrame ClusterSoftwareRasterizationPass::Prepare(
    const ClusterSoftwareRasterBindings& bindings, const org::PassPrepareContext& preparation) const {
    ClusterSoftwareRasterFrame frame{};
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
    frame.viewRasterInfoTable =
        m_viewRasterInfoTable.Publish(preparation, m_viewRasterInfoPublisher, bindings.viewRasterInfoLayout);
    if (bindings.virtualShadow) {
        const auto config = CLodVirtualShadowBuildRuntimeResolutionConfig();
        frame.pageTableResolution = config.pageTableResolution;
        frame.virtualResolution = config.virtualResolution;
    }
    const auto numBuckets = context->preparedRasterBucketCount;
    frame.bucketCount = numBuckets;
    BT_PLOT("CLod.RasterArgs.PreparedSoftwareBucketCount", static_cast<int64_t>(numBuckets));
    BT_PLOT("CLod.RasterArgs.PreparedSoftwareBackingBytes", static_cast<int64_t>(m_rasterBucketsIndirectArgsBuffer->GetSize()));
    // Validate the whole input before selecting any pipeline.
    constexpr uint64_t stride = sizeof(RasterizeClustersCommand);
    if (numBuckets > context->preparedRasterBucketFlags.size())
        throw std::out_of_range("Software raster bucket flags are incomplete");
    if (numBuckets > m_rasterBucketsIndirectArgsBuffer->GetSize() / stride)
        throw std::out_of_range("Software raster arguments are incomplete");
    frame.steps.reserve(numBuckets);
    for (uint32_t bucket = 0; bucket < numBuckets; ++bucket) {
        const auto* pso = PSOManager::GetInstance().TryGetClusterLODSoftwareRasterPSO(
            context->preparedRasterBucketFlags[bucket], m_outputKind);
        if (pso) frame.steps.push_back({*pso, uint64_t{bucket} * stride});
    }
    frame.hasSkinCache = bindings.hasSkinCache && numBuckets != 0u;
    if (frame.hasSkinCache) {
        if (++m_dynamicWindSkinCacheGeneration == 0 || m_dynamicWindSkinCacheGeneration >= 0x7FFFFFFFu)
            m_dynamicWindSkinCacheGeneration = 1;
        frame.skinCacheGeneration = m_dynamicWindSkinCacheGeneration;
    }
    return frame;
}
