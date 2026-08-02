#pragma once

#include <algorithm>
#include <array>
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
#include "RenderPasses/Base/ComputePass.h"
#include "Resources/PixelBuffer.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "ShaderBuffers.h"
#include "../../../../shaders/PerPassRootConstants/clodClearUintBufferRootConstants.h"
#include "../../../../shaders/PerPassRootConstants/clodWorkGraphRootConstants.h"
#include "../../../../shaders/PerPassRootConstants/clodRasterizationRootConstants.h"

class Buffer;
class ResourceGroup;

class ClusterSoftwareRasterPageJobExpandPass : public ComputePass {
public:
    ClusterSoftwareRasterPageJobExpandPass(
        std::shared_ptr<Buffer> compactedVisibleClustersBuffer,
        std::shared_ptr<Buffer> compactedVisibleClusterTransformIndicesBuffer,
        std::shared_ptr<Buffer> rasterBucketsHistogramBuffer,
        std::shared_ptr<Buffer> rasterBucketsIndirectArgsBuffer,
        std::shared_ptr<Buffer> viewRasterInfoBuffer,
        std::shared_ptr<PixelBuffer> virtualShadowPageTableTexture,
        std::shared_ptr<Buffer> virtualShadowClipmapInfoBuffer,
        std::shared_ptr<Buffer> rigidPageJobRecordsBuffer,
        std::shared_ptr<Buffer> rigidPageJobCountBuffer,
        std::shared_ptr<Buffer> skinnedPageJobRecordsBuffer,
        std::shared_ptr<Buffer> skinnedPageJobCountBuffer,
        std::shared_ptr<Buffer> pageJobClusterTagsBuffer,
        std::shared_ptr<Buffer> virtualShadowStatsBuffer,
        uint32_t pageJobRecordCapacity,
        std::shared_ptr<ResourceGroup> slabResourceGroup = nullptr,
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
        device.CreateCommandSignature(
            rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, 2), sizeof(RasterizeClustersCommand) },
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            m_commandSignature);

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

    void DeclareResourceUsages(ComputePassBuilder* builder) override
    {
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
    }

    void Setup() override {}

    void Update(const UpdateExecutionContext&) override {}

    PassReturn Execute(PassExecutionContext& executionContext) override
    {
        if (m_runWhenComputeSWRasterEnabledOnly &&
            !CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)())) {
            return {};
        }

        auto& settings = SettingsManager::GetInstance();
        if (!CLodVSMRasterModeUsesLargeClusterPageJob(
                settings.getSettingGetter<CLodVSMRasterMode>(CLodVSMRasterModeSettingName)())) {
            return {};
        }

        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& context = *renderContext;
        auto& commandList = executionContext.commandList;

        commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
        commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());

        BindResourceDescriptorIndices(commandList, m_clearPso.GetResourceDescriptorSlots());
        commandList.BindPipeline(m_clearPso.GetAPIPipelineState().GetHandle());

        uint32_t clearRootConstants[NumMiscUintRootConstants] = {};
        for (const auto& pageJobCountBuffer : m_pageJobCountBuffers) {
            clearRootConstants[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = pageJobCountBuffer->GetUAVShaderVisibleInfo(0).slot.index;
            clearRootConstants[CLOD_CLEAR_UINT_BUFFER_VALUE] = 0u;
            clearRootConstants[CLOD_CLEAR_UINT_BUFFER_COUNT] = 1u;
            commandList.PushConstants(
                rhi::ShaderStage::Compute,
                0,
                MiscUintRootSignatureIndex,
                0,
                NumMiscUintRootConstants,
                clearRootConstants);
            commandList.Dispatch(1u, 1u, 1u);
        }

        const uint32_t tagCount = static_cast<uint32_t>(m_pageJobClusterTagsBuffer->GetSize() / sizeof(uint32_t));
        clearRootConstants[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = m_pageJobClusterTagsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
        clearRootConstants[CLOD_CLEAR_UINT_BUFFER_VALUE] = 0xFFFFFFFFu;
        clearRootConstants[CLOD_CLEAR_UINT_BUFFER_COUNT] = tagCount;
        if (tagCount > 0u) {
            commandList.PushConstants(
                rhi::ShaderStage::Compute,
                0,
                MiscUintRootSignatureIndex,
                0,
                NumMiscUintRootConstants,
                clearRootConstants);
            commandList.Dispatch((tagCount + 63u) / 64u, 1u, 1u);
        }

        std::array<rhi::BufferBarrier, 3> clearBarriers{};
        for (uint32_t variantIndex = 0u; variantIndex < m_pageJobCountBuffers.size(); ++variantIndex) {
            clearBarriers[variantIndex].buffer = m_pageJobCountBuffers[variantIndex]->GetAPIResource().GetHandle();
            clearBarriers[variantIndex].beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
            clearBarriers[variantIndex].afterAccess = rhi::ResourceAccessType::UnorderedAccess;
            clearBarriers[variantIndex].beforeSync = rhi::ResourceSyncState::ComputeShading;
            clearBarriers[variantIndex].afterSync = rhi::ResourceSyncState::ComputeShading;
        }
        clearBarriers[2].buffer = m_pageJobClusterTagsBuffer->GetAPIResource().GetHandle();
        clearBarriers[2].beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
        clearBarriers[2].afterAccess = rhi::ResourceAccessType::UnorderedAccess;
        clearBarriers[2].beforeSync = rhi::ResourceSyncState::ComputeShading;
        clearBarriers[2].afterSync = rhi::ResourceSyncState::ComputeShading;

        rhi::BarrierBatch clearBarrierBatch{};
        clearBarrierBatch.buffers = rhi::Span<rhi::BufferBarrier>(clearBarriers.data(), static_cast<uint32_t>(clearBarriers.size()));
        commandList.Barriers(clearBarrierBatch);

        uint32_t misc[NumMiscUintRootConstants] = {};
        misc[CLOD_RASTER_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX] = m_rasterBucketsHistogramBuffer->GetSRVInfo(0).slot.index;
        misc[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = m_compactedVisibleClustersBuffer->GetSRVInfo(0).slot.index;
        misc[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] =
            m_compactedVisibleClusterTransformIndicesBuffer->GetSRVInfo(0).slot.index;
        misc[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX] = m_viewRasterInfoBuffer->GetSRVInfo(0).slot.index;
        misc[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX] = m_virtualShadowPageTableTexture->GetUAVShaderVisibleInfo(UAVViewType::Texture2DArrayFull, 0).slot.index;
        misc[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX] = m_virtualShadowClipmapInfoBuffer->GetSRVInfo(0).slot.index;
        misc[CLOD_RASTER_PAGE_JOB_CLUSTER_TAGS_DESCRIPTOR_INDEX] = m_pageJobClusterTagsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
        misc[CLOD_RASTER_PAGE_JOB_RECORD_CAPACITY] = m_pageJobRecordCapacity;
        misc[CLOD_RASTER_VIRTUAL_SHADOW_STATS_DESCRIPTOR_INDEX] =
            m_virtualShadowStatsBuffer->GetUAVShaderVisibleInfo(0).slot.index;

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

        const uint32_t numBuckets = context.materialManager->GetRasterBucketCount();
        if (numBuckets == 0) {
            return {};
        }

        auto apiResource = m_rasterBucketsIndirectArgsBuffer->GetAPIResource();
        const uint64_t stride = sizeof(RasterizeClustersCommand);
        for (uint32_t i = 0; i < numBuckets; ++i) {
            const MaterialRasterFlags flags = context.materialManager->GetRasterFlagsForBucket(i);
            const uint32_t variantIndex = (flags & MaterialRasterFlagsSkinned) ? 1u : 0u;
            const bool doubleSided =
                (flags & MaterialRasterFlags::MaterialRasterFlagsDoubleSided) != 0;
            const PipelineState& pso =
                variantIndex != 0u
                    ? (doubleSided ? m_skinnedDoubleSidedPso : m_skinnedPso)
                    : (doubleSided ? m_doubleSidedPso : m_rigidPso);

            misc[CLOD_RASTER_PAGE_JOB_RECORDS_DESCRIPTOR_INDEX] =
                m_pageJobRecordsBuffers[variantIndex]->GetUAVShaderVisibleInfo(0).slot.index;
            misc[CLOD_RASTER_PAGE_JOB_COUNT_DESCRIPTOR_INDEX] =
                m_pageJobCountBuffers[variantIndex]->GetUAVShaderVisibleInfo(0).slot.index;
            commandList.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, misc);
            BindResourceDescriptorIndices(commandList, pso.GetResourceDescriptorSlots());
            commandList.BindPipeline(pso.GetAPIPipelineState().GetHandle());

            const uint64_t argOffset = static_cast<uint64_t>(i) * stride;
            commandList.ExecuteIndirect(
                m_commandSignature->GetHandle(),
                apiResource.GetHandle(),
                argOffset,
                {},
                0,
                1);
        }

        return {};
    }

    void Cleanup() override {}

private:
    PipelineState m_rigidPso;
    PipelineState m_skinnedPso;
    PipelineState m_doubleSidedPso;
    PipelineState m_skinnedDoubleSidedPso;
    PipelineState m_clearPso;
    rhi::CommandSignaturePtr m_commandSignature;
    std::shared_ptr<Buffer> m_compactedVisibleClustersBuffer;
    std::shared_ptr<Buffer> m_compactedVisibleClusterTransformIndicesBuffer;
    std::shared_ptr<Buffer> m_rasterBucketsHistogramBuffer;
    std::shared_ptr<Buffer> m_rasterBucketsIndirectArgsBuffer;
    std::shared_ptr<Buffer> m_viewRasterInfoBuffer;
    std::shared_ptr<PixelBuffer> m_virtualShadowPageTableTexture;
    std::shared_ptr<Buffer> m_virtualShadowClipmapInfoBuffer;
    std::array<std::shared_ptr<Buffer>, 2> m_pageJobRecordsBuffers;
    std::array<std::shared_ptr<Buffer>, 2> m_pageJobCountBuffers;
    std::shared_ptr<Buffer> m_pageJobClusterTagsBuffer;
    std::shared_ptr<Buffer> m_virtualShadowStatsBuffer;
    uint32_t m_pageJobRecordCapacity = 0u;
    std::shared_ptr<ResourceGroup> m_slabResourceGroup;
    bool m_runWhenComputeSWRasterEnabledOnly = false;
};
