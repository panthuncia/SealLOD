#pragma once

#include <algorithm>
#include <array>
#include <algorithm>
#include <memory>
#include <vector>

#include <rhi.h>

#include "BuiltinResources.h"
#include "Interfaces/IDynamicDeclaredResources.h"
#include "Managers/MaterialManager.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"
#include "Resources/PixelBuffer.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "ShaderBuffers.h"
#include "../../../../shaders/PerPassRootConstants/clodClearUintBufferRootConstants.h"
#include "../../../../shaders/PerPassRootConstants/clodWorkGraphRootConstants.h"
#include "../../../../shaders/PerPassRootConstants/clodRasterizationRootConstants.h"

namespace org { class Buffer; }
namespace org { class ResourceGroup; }

struct ClusterPageJobExpandFrameData {
    std::vector<br::render::PreparedComputeIndirect> dispatches;
    std::vector<br::render::PreparedComputeDispatch> clears;
    std::array<org::PreparedResourceReference, 3> barriers;
};

struct ClusterPageJobExpandBindings {
    org::ResourceBindingToken compactedVisibleClusters, compactedVisibleClusterTransformIndices;
    org::ResourceBindingToken histogram, indirectArgs, viewRasterInfo, pageTable, clipmapInfo;
    std::array<org::ResourceBindingToken, 2> pageJobRecords, pageJobCounts;
    org::ResourceBindingToken clusterTags, stats;
};

class ClusterSoftwareRasterPageJobExpandPass : public org::TypedRenderGraphPass<ClusterSoftwareRasterPageJobExpandPass,
    ClusterPageJobExpandFrameData, ClusterPageJobExpandBindings> {
public:
    ClusterSoftwareRasterPageJobExpandPass(
        std::shared_ptr<org::Buffer> compactedVisibleClustersBuffer,
        std::shared_ptr<org::Buffer> compactedVisibleClusterTransformIndicesBuffer,
        std::shared_ptr<org::Buffer> rasterBucketsHistogramBuffer,
        std::shared_ptr<org::Buffer> rasterBucketsIndirectArgsBuffer,
        std::shared_ptr<org::Buffer> viewRasterInfoBuffer,
        std::shared_ptr<org::PixelBuffer> virtualShadowPageTableTexture,
        std::shared_ptr<org::Buffer> virtualShadowClipmapInfoBuffer,
        std::shared_ptr<org::Buffer> rigidPageJobRecordsBuffer,
        std::shared_ptr<org::Buffer> rigidPageJobCountBuffer,
        std::shared_ptr<org::Buffer> skinnedPageJobRecordsBuffer,
        std::shared_ptr<org::Buffer> skinnedPageJobCountBuffer,
        std::shared_ptr<org::Buffer> pageJobClusterTagsBuffer,
        std::shared_ptr<org::Buffer> virtualShadowStatsBuffer,
        uint32_t pageJobRecordCapacity,
        std::shared_ptr<org::ResourceGroup> slabResourceGroup = nullptr,
        bool runWhenComputeSWRasterEnabledOnly = false)
        : m_compactedVisibleClustersBuffer(std::move(compactedVisibleClustersBuffer))
        , m_compactedVisibleClusterTransformIndicesBuffer(std::move(compactedVisibleClusterTransformIndicesBuffer))
        , m_rasterBucketsHistogramBuffer(std::move(rasterBucketsHistogramBuffer))
        , m_rasterBucketsIndirectArgsBuffer(std::move(rasterBucketsIndirectArgsBuffer))
        , m_viewRasterInfoBuffer(std::move(viewRasterInfoBuffer))
        , m_virtualShadowPageTableTexture(std::move(virtualShadowPageTableTexture))
        , m_virtualShadowClipmapInfoBuffer(std::move(virtualShadowClipmapInfoBuffer))
        , m_pageJobRecordsBuffers{ std::move(rigidPageJobRecordsBuffer), std::move(skinnedPageJobRecordsBuffer) }
        , m_pageJobCountBuffers{ std::move(rigidPageJobCountBuffer), std::move(skinnedPageJobCountBuffer) }
        , m_pageJobClusterTagsBuffer(std::move(pageJobClusterTagsBuffer))
        , m_virtualShadowStatsBuffer(std::move(virtualShadowStatsBuffer))
        , m_pageJobRecordCapacity(pageJobRecordCapacity)
        , m_slabResourceGroup(std::move(slabResourceGroup))
        , m_runWhenComputeSWRasterEnabledOnly(runWhenComputeSWRasterEnabledOnly)
    {
        rhi::IndirectArg args[] = {
            {.kind = rhi::IndirectArgKind::Constant, .u = {.rootConstants = { IndirectCommandSignatureRootSignatureIndex, 0, 3 } } },
            {.kind = rhi::IndirectArgKind::Dispatch }
        };

        auto device = DeviceManager::GetInstance().GetDevice();
        m_commandSignature = std::make_shared<rhi::CommandSignaturePtr>();
        device.CreateCommandSignature(
            rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, 2), sizeof(RasterizeClustersCommand) },
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            *m_commandSignature);

        m_rigidPso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"Shaders/ClusterLOD/softwareRasterPageJobs.hlsl",
            L"SWPageJobExpandCSMain",
            {},
            "CLod_SoftwarePageJobExpandPSO");
        std::vector<DxcDefine> skinnedDefines = { DxcDefine{ L"PSO_SKINNED", L"1" } };
        m_skinnedPso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"Shaders/ClusterLOD/softwareRasterPageJobs.hlsl",
            L"SWPageJobExpandCSMain",
            skinnedDefines,
            "CLod_SoftwarePageJobExpandSkinnedPSO");
        std::vector<DxcDefine> doubleSidedDefines = { DxcDefine{ L"PSO_DOUBLE_SIDED", L"1" } };
        m_doubleSidedPso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"Shaders/ClusterLOD/softwareRasterPageJobs.hlsl",
            L"SWPageJobExpandCSMain",
            doubleSidedDefines,
            "CLod_SoftwarePageJobExpandDoubleSidedPSO");
        std::vector<DxcDefine> skinnedDoubleSidedDefines = {
            DxcDefine{ L"PSO_SKINNED", L"1" },
            DxcDefine{ L"PSO_DOUBLE_SIDED", L"1" } };
        m_skinnedDoubleSidedPso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"Shaders/ClusterLOD/softwareRasterPageJobs.hlsl",
            L"SWPageJobExpandCSMain",
            skinnedDoubleSidedDefines,
            "CLod_SoftwarePageJobExpandSkinnedDoubleSidedPSO");
        m_clearPso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"Shaders/ClusterLOD/clodUtil.hlsl",
            L"ClearUintStructuredBufferCSMain",
            {},
            "CLod_SoftwarePageJobExpandClearUintPSO");
    }

    ClusterPageJobExpandBindings Declare(org::PassBuilder& declaration)
    {
        declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* builder = &declaration;
        builder->WithShaderResource(
                Builtin::PerMeshBuffer,
                Builtin::PerMaterialDataBuffer,
                Builtin::PerMeshInstanceBuffer,
                Builtin::InstanceDrawRecordBuffer,
                Builtin::PerInstanceTransformBuffer,
                Builtin::PerObjectBuffer,
                Builtin::CLod::Offsets,
                Builtin::CLod::MeshMetadata,
                Builtin::CLod::Groups,
                Builtin::CullingCameraBuffer,
                Builtin::SkeletonResources::InverseBindMatrices,
                Builtin::SkeletonResources::BoneTransforms,
                Builtin::SkeletonResources::SkinningInstanceInfo,
                Builtin::CLod::AssemblyTransforms,
                Builtin::CLod::AssemblyBoneRemaps,
                Builtin::CLod::AssemblyBoneRemapIndices,
                m_compactedVisibleClustersBuffer,
                m_compactedVisibleClusterTransformIndicesBuffer,
                m_rasterBucketsHistogramBuffer,
                m_viewRasterInfoBuffer,
                m_virtualShadowClipmapInfoBuffer)
            .WithUnorderedAccess(
                m_virtualShadowPageTableTexture,
                m_pageJobRecordsBuffers[0],
                m_pageJobCountBuffers[0],
                m_pageJobRecordsBuffers[1],
                m_pageJobCountBuffers[1],
                m_pageJobClusterTagsBuffer,
                m_virtualShadowStatsBuffer)
            .WithIndirectArguments(m_rasterBucketsIndirectArgsBuffer)
            .WithConstantBuffer(Builtin::PerFrameBuffer);

        if (m_slabResourceGroup) {
            builder->WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
        }
        ClusterPageJobExpandBindings bindings{
            builder->BindShaderResource(m_compactedVisibleClustersBuffer),
            builder->BindShaderResource(m_compactedVisibleClusterTransformIndicesBuffer),
            builder->BindShaderResource(m_rasterBucketsHistogramBuffer),
            builder->BindIndirectArguments(m_rasterBucketsIndirectArgsBuffer),
            builder->BindShaderResource(m_viewRasterInfoBuffer),
            builder->BindUnorderedAccess(m_virtualShadowPageTableTexture),
            builder->BindShaderResource(m_virtualShadowClipmapInfoBuffer)};
        for (uint32_t i = 0; i < 2; ++i) {
            bindings.pageJobRecords[i] = builder->BindUnorderedAccess(m_pageJobRecordsBuffers[i]);
            bindings.pageJobCounts[i] = builder->BindUnorderedAccess(m_pageJobCountBuffers[i]);
        }
        bindings.clusterTags = builder->BindUnorderedAccess(m_pageJobClusterTagsBuffer);
        bindings.stats = builder->BindUnorderedAccess(m_virtualShadowStatsBuffer);
        return bindings;
    }

    ClusterPageJobExpandFrameData Prepare(const ClusterPageJobExpandBindings& declared,
        const org::PassPrepareContext& preparation) const {
        ClusterPageJobExpandFrameData data{};
        if (m_runWhenComputeSWRasterEnabledOnly &&
            !CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)())) {
            return data;
        }

        auto& settings = SettingsManager::GetInstance();
        if (!CLodVSMRasterModeUsesLargeClusterPageJob(
                settings.getSettingGetter<CLodVSMRasterMode>(CLodVSMRasterModeSettingName)())) {
            return data;
        }

        const auto& context = *preparation.preparationData->Get<UpdateContext>();
        const auto signature = preparation.CaptureCommandSignature(m_commandSignature);
        const auto clear = preparation.CaptureProgramBinding(m_clearPso);
        const auto srv = [&](org::ResourceBindingToken token) {
            return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource}).index;
        };
        const auto uav = [&](org::ResourceBindingToken token, uint32_t variant = UINT32_MAX) {
            return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess, variant}).index;
        };
        const auto appendClear = [&](org::ResourceBindingToken token, uint32_t value, uint32_t count) {
            br::render::PreparedComputeDispatch dispatch{};
            dispatch.resourceHeap = context.textureDescriptorHeap.GetHandle();
            dispatch.samplerHeap = context.samplerDescriptorHeap.GetHandle();
            dispatch.program = clear.program;
            dispatch.descriptorIndices = clear.descriptorIndices;
            dispatch.constants[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = uav(token);
            dispatch.constants[CLOD_CLEAR_UINT_BUFFER_VALUE] = value;
            dispatch.constants[CLOD_CLEAR_UINT_BUFFER_COUNT] = count;
            dispatch.groupsX = (count + 63u) / 64u;
            data.clears.push_back(std::move(dispatch));
        };
        for (uint32_t i = 0; i < m_pageJobCountBuffers.size(); ++i) {
            appendClear(declared.pageJobCounts[i], 0u, 1u);
            data.barriers[i] = preparation.CaptureResource(declared.pageJobCounts[i]);
        }
        appendClear(declared.clusterTags, 0xFFFFFFFFu,
            static_cast<uint32_t>(m_pageJobClusterTagsBuffer->GetSize() / sizeof(uint32_t)));
        data.barriers[2] = preparation.CaptureResource(declared.clusterTags);
        uint32_t misc[NumMiscUintRootConstants] = {};
        misc[CLOD_RASTER_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX] = srv(declared.histogram);
        misc[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = srv(declared.compactedVisibleClusters);
        misc[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] =
            srv(declared.compactedVisibleClusterTransformIndices);
        misc[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX] = srv(declared.viewRasterInfo);
        misc[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX] =
            uav(declared.pageTable, static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull));
        misc[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX] = srv(declared.clipmapInfo);
        misc[CLOD_RASTER_PAGE_JOB_CLUSTER_TAGS_DESCRIPTOR_INDEX] = uav(declared.clusterTags);
        misc[CLOD_RASTER_PAGE_JOB_RECORD_CAPACITY] = m_pageJobRecordCapacity;
        misc[CLOD_RASTER_VIRTUAL_SHADOW_STATS_DESCRIPTOR_INDEX] =
            uav(declared.stats);

        uint32_t pageJobFlags = 0u;
        pageJobFlags |= CLOD_WG_PAGE_JOB_FLAG_ENABLED;
        if (settings.getSettingGetter<bool>(CLodPageJobForceAllSettingName)()) {
            pageJobFlags |= CLOD_WG_PAGE_JOB_FLAG_FORCE_ALL;
        }
        const uint32_t diameterThreshold = std::min(settings.getSettingGetter<uint32_t>(CLodPageJobDiameterThresholdSettingName)(), 255u);
        pageJobFlags |= (diameterThreshold << CLOD_WG_PAGE_JOB_DIAMETER_THRESHOLD_SHIFT);
        const uint32_t maxPages = std::min(settings.getSettingGetter<uint32_t>(CLodPageJobMaxPagesPerClusterSettingName)(), 255u);
        pageJobFlags |= (maxPages << CLOD_WG_PAGE_JOB_MAX_PAGES_SHIFT);
        misc[CLOD_RASTER_PAGE_JOB_FLAGS] = pageJobFlags;

        const uint32_t numBuckets = context.preparedRasterBucketCount;
        if (numBuckets == 0) {
            return data;
        }

        const auto arguments = preparation.CaptureResource(declared.indirectArgs);
        const std::array bindings{
            preparation.CaptureProgramBinding(m_rigidPso),
            preparation.CaptureProgramBinding(m_doubleSidedPso),
            preparation.CaptureProgramBinding(m_skinnedPso),
            preparation.CaptureProgramBinding(m_skinnedDoubleSidedPso)};
        data.dispatches.reserve(numBuckets);
        for (uint32_t i = 0; i < numBuckets; ++i) {
            const auto flags = context.preparedRasterBucketFlags.at(i);
            const uint32_t variantIndex = (flags & MaterialRasterFlagsSkinned) ? 1u : 0u;
            const bool doubleSided = (flags & MaterialRasterFlagsDoubleSided) != 0;
            misc[CLOD_RASTER_PAGE_JOB_RECORDS_DESCRIPTOR_INDEX] = uav(declared.pageJobRecords[variantIndex]);
            misc[CLOD_RASTER_PAGE_JOB_COUNT_DESCRIPTOR_INDEX] = uav(declared.pageJobCounts[variantIndex]);
            const auto& binding = bindings[variantIndex * 2u + (doubleSided ? 1u : 0u)];
            br::render::PreparedComputeIndirect dispatch{};
            dispatch.resourceHeap = context.textureDescriptorHeap.GetHandle();
            dispatch.samplerHeap = context.samplerDescriptorHeap.GetHandle();
            dispatch.program = binding.program;
            dispatch.descriptorIndices = binding.descriptorIndices;
            std::copy(std::begin(misc), std::end(misc), dispatch.constants.begin());
            dispatch.commandSignature = signature;
            dispatch.argumentsReference = arguments;
            dispatch.argumentsOffset = static_cast<uint64_t>(i) * sizeof(RasterizeClustersCommand);
            data.dispatches.push_back(std::move(dispatch));
        }
        return data;
    }

    static void Record(const ClusterPageJobExpandBindings&, const ClusterPageJobExpandFrameData& data,
        org::PassRecordContext& recording) {
        if (data.clears.empty()) return;
        for (const auto& clear : data.clears)
            br::render::RecordPreparedComputeDispatch(clear, recording);
        std::array<rhi::BufferBarrier, 3> barriers{};
        for (size_t i = 0; i < barriers.size(); ++i) {
            barriers[i].buffer = recording.Resolve(data.barriers[i]).GetHandle();
            barriers[i].beforeAccess = barriers[i].afterAccess = rhi::ResourceAccessType::UnorderedAccess;
            barriers[i].beforeSync = barriers[i].afterSync = rhi::ResourceSyncState::ComputeShading;
        }
        rhi::BarrierBatch batch{};
        batch.buffers = {barriers.data(), static_cast<uint32_t>(barriers.size())};
        recording.Commands().Barriers(batch);
        for (const auto& dispatch : data.dispatches)
            br::render::RecordPreparedComputeIndirect(dispatch, recording);
    }

private:
    org::PipelineState m_rigidPso;
    org::PipelineState m_skinnedPso;
    org::PipelineState m_doubleSidedPso;
    org::PipelineState m_skinnedDoubleSidedPso;
    org::PipelineState m_clearPso;
    std::shared_ptr<rhi::CommandSignaturePtr> m_commandSignature;
    std::shared_ptr<org::Buffer> m_compactedVisibleClustersBuffer;
    std::shared_ptr<org::Buffer> m_compactedVisibleClusterTransformIndicesBuffer;
    std::shared_ptr<org::Buffer> m_rasterBucketsHistogramBuffer;
    std::shared_ptr<org::Buffer> m_rasterBucketsIndirectArgsBuffer;
    std::shared_ptr<org::Buffer> m_viewRasterInfoBuffer;
    std::shared_ptr<org::PixelBuffer> m_virtualShadowPageTableTexture;
    std::shared_ptr<org::Buffer> m_virtualShadowClipmapInfoBuffer;
    std::array<std::shared_ptr<org::Buffer>, 2> m_pageJobRecordsBuffers;
    std::array<std::shared_ptr<org::Buffer>, 2> m_pageJobCountBuffers;
    std::shared_ptr<org::Buffer> m_pageJobClusterTagsBuffer;
    std::shared_ptr<org::Buffer> m_virtualShadowStatsBuffer;
    uint32_t m_pageJobRecordCapacity = 0u;
    std::shared_ptr<org::ResourceGroup> m_slabResourceGroup;
    bool m_runWhenComputeSWRasterEnabledOnly = false;
};
