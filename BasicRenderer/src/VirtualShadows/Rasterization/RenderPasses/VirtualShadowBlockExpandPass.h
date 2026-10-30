#pragma once

#include <algorithm>
#include <memory>
#include <vector>

#include <rhi.h>

#include "BuiltinResources.h"
#include "Materials/MaterialManager.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "Runtime/GraphIntegration/Resolvers/ResourceGroupResolver.h"
#include "../shaders/PerPassRootConstants/clodClearUintBufferRootConstants.h"
#include "../shaders/PerPassRootConstants/clodVirtualShadowBlockExpandRootConstants.h"

namespace org { class Buffer; }
namespace org { class ResourceGroup; }

enum class VirtualShadowBlockExpandMode : uint8_t
{
    Histogram,
    Emit,
};

struct VirtualShadowBlockFrameData {
    std::vector<uint8_t> bucketSkinned; // one entry per raster bucket; empty records nothing
};

struct VirtualShadowBlockBindings {
    org::PipelineState rigidPipeline, skinnedPipeline, clearPipeline;
    std::shared_ptr<const rhi::CommandSignaturePtr> commandSignature;
    VirtualShadowBlockExpandMode mode = VirtualShadowBlockExpandMode::Histogram;
    uint32_t expandedRecordCapacity = 0;
    org::DeclaredViewToken sourceVisible, sourceTransforms, sourceHistogram;
    org::ResourceBindingToken sourceArgs;
    org::DeclaredViewToken expandedHistogram, expandedOffsets, expandedCursor, expandedVisible, expandedTransforms;
    org::DeclaredViewToken clipmapInfo, rigidMetadata, skinnedMetadata, coverage, stats;
};

class VirtualShadowBlockExpandPass : public org::TypedRenderGraphPass<VirtualShadowBlockExpandPass,
    VirtualShadowBlockFrameData, VirtualShadowBlockBindings> {
public:
    VirtualShadowBlockExpandPass(
        VirtualShadowBlockExpandMode mode,
        std::shared_ptr<org::Buffer> sourceVisibleClustersBuffer,
        std::shared_ptr<org::Buffer> sourceVisibleClusterTransformIndicesBuffer,
        std::shared_ptr<org::Buffer> sourceHistogramBuffer,
        std::shared_ptr<org::Buffer> sourceIndirectArgsBuffer,
        std::shared_ptr<org::Buffer> expandedHistogramBuffer,
        std::shared_ptr<org::Buffer> expandedOffsetsBuffer,
        std::shared_ptr<org::Buffer> expandedWriteCursorBuffer,
        std::shared_ptr<org::Buffer> expandedVisibleClustersBuffer,
        std::shared_ptr<org::Buffer> expandedVisibleClusterTransformIndicesBuffer,
        std::shared_ptr<org::Buffer> virtualShadowClipmapInfoBuffer,
        std::shared_ptr<org::Buffer> virtualShadowActiveBlockMetadataBuffer,
        std::shared_ptr<org::Buffer> virtualShadowDynamicActiveBlockMetadataBuffer,
        std::shared_ptr<org::Buffer> virtualShadowBlockClusterCoverageBuffer,
        std::shared_ptr<org::Buffer> virtualShadowStatsBuffer,
        uint32_t expandedRecordCapacity,
        std::shared_ptr<org::ResourceGroup> slabResourceGroup = nullptr,
        bool runWhenComputeSWRasterEnabledOnly = false)
        : m_mode(mode)
        , m_sourceVisibleClustersBuffer(std::move(sourceVisibleClustersBuffer))
        , m_sourceVisibleClusterTransformIndicesBuffer(std::move(sourceVisibleClusterTransformIndicesBuffer))
        , m_sourceHistogramBuffer(std::move(sourceHistogramBuffer))
        , m_sourceIndirectArgsBuffer(std::move(sourceIndirectArgsBuffer))
        , m_expandedHistogramBuffer(std::move(expandedHistogramBuffer))
        , m_expandedOffsetsBuffer(std::move(expandedOffsetsBuffer))
        , m_expandedWriteCursorBuffer(std::move(expandedWriteCursorBuffer))
        , m_expandedVisibleClustersBuffer(std::move(expandedVisibleClustersBuffer))
        , m_expandedVisibleClusterTransformIndicesBuffer(std::move(expandedVisibleClusterTransformIndicesBuffer))
        , m_virtualShadowClipmapInfoBuffer(std::move(virtualShadowClipmapInfoBuffer))
        , m_virtualShadowActiveBlockMetadataBuffer(std::move(virtualShadowActiveBlockMetadataBuffer))
        , m_virtualShadowDynamicActiveBlockMetadataBuffer(std::move(virtualShadowDynamicActiveBlockMetadataBuffer))
        , m_virtualShadowBlockClusterCoverageBuffer(std::move(virtualShadowBlockClusterCoverageBuffer))
        , m_virtualShadowStatsBuffer(std::move(virtualShadowStatsBuffer))
        , m_expandedRecordCapacity(expandedRecordCapacity)
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

        const wchar_t* entryPoint = mode == VirtualShadowBlockExpandMode::Histogram
            ? L"CLodVirtualShadowBlockHistogramCSMain"
            : L"CLodVirtualShadowBlockEmitCSMain";
        const char* rigidName = mode == VirtualShadowBlockExpandMode::Histogram
            ? "CLod_VirtualShadowBlockHistogramPSO"
            : "CLod_VirtualShadowBlockEmitPSO";
        const char* skinnedName = mode == VirtualShadowBlockExpandMode::Histogram
            ? "CLod_VirtualShadowBlockHistogramSkinnedPSO"
            : "CLod_VirtualShadowBlockEmitSkinnedPSO";

        m_rigidPso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"Shaders/ClusterLOD/virtualShadowBlockExpansion.hlsl",
            entryPoint,
            {},
            rigidName);
        std::vector<DxcDefine> skinnedDefines = { DxcDefine{ L"PSO_SKINNED", L"1" } };
        m_skinnedPso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"Shaders/ClusterLOD/virtualShadowBlockExpansion.hlsl",
            entryPoint,
            skinnedDefines,
            skinnedName);
        m_clearPso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"Shaders/ClusterLOD/clodUtil.hlsl",
            L"ClearUintStructuredBufferCSMain",
            {},
            "CLod_VirtualShadowBlockExpandClearUintPSO");
    }

    VirtualShadowBlockBindings Declare(org::PassBuilder& declaration)
    {
        declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* builder = &declaration;
        builder->ShaderResource(
                Builtin::PerMeshInstanceBuffer,
                Builtin::InstanceDrawRecordBuffer,
                Builtin::PerInstanceTransformBuffer,
                Builtin::PerObjectBuffer,
                Builtin::CLod::Offsets,
                Builtin::CLod::MeshMetadata,
                Builtin::CLod::AssemblyTransforms,
                Builtin::CLod::AssemblyBoneRemaps,
                Builtin::CLod::AssemblyBoneRemapIndices,
                Builtin::CullingCameraBuffer,
                Builtin::SkeletonResources::InverseBindMatrices,
                Builtin::SkeletonResources::BoneTransforms,
                Builtin::SkeletonResources::SkinningInstanceInfo);
        builder->ConstantBuffer(Builtin::PerFrameBuffer);

        if (m_slabResourceGroup) {
            builder->ShaderResource(ResourceGroupResolver(m_slabResourceGroup));
        }
        VirtualShadowBlockBindings bindings{
            m_rigidPso, m_skinnedPso, m_clearPso, m_commandSignature, m_mode, m_expandedRecordCapacity,
            builder->ShaderResource(m_sourceVisibleClustersBuffer),
            builder->ShaderResource(m_sourceVisibleClusterTransformIndicesBuffer),
            builder->ShaderResource(m_sourceHistogramBuffer),
            builder->IndirectArguments(m_sourceIndirectArgsBuffer),
            builder->UnorderedAccess(m_expandedHistogramBuffer),
            {}, {}, {}, {},
            builder->ShaderResource(m_virtualShadowClipmapInfoBuffer),
            builder->ShaderResource(m_virtualShadowActiveBlockMetadataBuffer),
            builder->ShaderResource(m_virtualShadowDynamicActiveBlockMetadataBuffer),
            m_mode == VirtualShadowBlockExpandMode::Histogram
                ? builder->UnorderedAccess(m_virtualShadowBlockClusterCoverageBuffer).View()
                : builder->ShaderResource(m_virtualShadowBlockClusterCoverageBuffer).View(),
            builder->UnorderedAccess(m_virtualShadowStatsBuffer)};
        if (m_mode == VirtualShadowBlockExpandMode::Emit) {
            bindings.expandedOffsets = builder->ShaderResource(m_expandedOffsetsBuffer);
            bindings.expandedCursor = builder->UnorderedAccess(m_expandedWriteCursorBuffer);
            bindings.expandedVisible = builder->UnorderedAccess(m_expandedVisibleClustersBuffer);
            bindings.expandedTransforms = builder->UnorderedAccess(m_expandedVisibleClusterTransformIndicesBuffer);
        }
        return bindings;
    }

    void Update(const org::UpdateExecutionContext& executionContext) override
    {
        if (m_runWhenComputeSWRasterEnabledOnly &&
            !CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)())) {
            return;
        }

        auto* updateContext = executionContext.hostData->Get<UpdateContext>();
        auto& context = *updateContext;
        const uint32_t numBuckets = context.preparedRasterBucketCount;

        if (m_mode == VirtualShadowBlockExpandMode::Histogram) {
            if (m_expandedHistogramBuffer->GetSize() < static_cast<size_t>(numBuckets) * sizeof(uint32_t)) {
                m_expandedHistogramBuffer->ResizeStructured(numBuckets);
            }
        }

        if (m_mode == VirtualShadowBlockExpandMode::Emit) {
            if (m_expandedWriteCursorBuffer->GetSize() < static_cast<size_t>(numBuckets) * sizeof(uint32_t)) {
                m_expandedWriteCursorBuffer->ResizeStructured(numBuckets);
            }
        }
    }

    VirtualShadowBlockFrameData Prepare(const VirtualShadowBlockBindings&,
        const org::PassPrepareContext& preparation) const {
        VirtualShadowBlockFrameData data{};
        if (m_runWhenComputeSWRasterEnabledOnly &&
            !CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)())) return data;
        const auto& context = *preparation.preparationData->Get<UpdateContext>();
        const auto numBuckets = context.preparedRasterBucketCount;
        data.bucketSkinned.reserve(numBuckets);
        for (uint32_t bucketIndex = 0; bucketIndex < numBuckets; ++bucketIndex) {
            const MaterialRasterFlags flags = context.preparedRasterBucketFlags.at(bucketIndex);
            data.bucketSkinned.push_back((flags & MaterialRasterFlagsSkinned) != 0 ? 1u : 0u);
        }
        return data;
    }

    // Clears the per-bucket counters (histogram or write cursor), then one indirect dispatch per raster bucket.
    static void Record(const VirtualShadowBlockBindings& bindings, const VirtualShadowBlockFrameData& data,
        org::PassRecordContext& recording) {
        const auto numBuckets = static_cast<uint32_t>(data.bucketSkinned.size());
        if (numBuckets == 0u) return;
        const bool histogram = bindings.mode == VirtualShadowBlockExpandMode::Histogram;
        const auto clearToken = histogram ? bindings.expandedHistogram : bindings.expandedCursor;
        br::render::MiscConstants clear{};
        clear[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = recording.Index(clearToken);
        clear[CLOD_CLEAR_UINT_BUFFER_VALUE] = 0u;
        clear[CLOD_CLEAR_UINT_BUFFER_COUNT] = numBuckets;
        br::render::DispatchCompute(recording, bindings.clearPipeline, clear, (numBuckets + 63u) / 64u);
        recording.BufferBarriers({clearToken.Resource()},
            rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceAccessType::UnorderedAccess);

        br::render::MiscConstants misc{};
        misc[CLOD_VSM_BLOCK_EXPAND_SOURCE_HISTOGRAM_DESCRIPTOR_INDEX] = recording.Index(bindings.sourceHistogram);
        misc[CLOD_VSM_BLOCK_EXPAND_SOURCE_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = recording.Index(bindings.sourceVisible);
        misc[CLOD_VSM_BLOCK_EXPAND_SOURCE_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = recording.Index(bindings.sourceTransforms);
        misc[CLOD_VSM_BLOCK_EXPAND_EXPANDED_HISTOGRAM_DESCRIPTOR_INDEX] = recording.Index(bindings.expandedHistogram);
        misc[CLOD_VSM_BLOCK_EXPAND_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX] = recording.Index(bindings.clipmapInfo);
        misc[CLOD_VSM_BLOCK_EXPAND_CLUSTER_COVERAGE_DESCRIPTOR_INDEX] = recording.Index(bindings.coverage);
        misc[CLOD_VSM_BLOCK_EXPAND_RECORD_CAPACITY] = bindings.expandedRecordCapacity;
        misc[CLOD_VSM_BLOCK_EXPAND_STATS_DESCRIPTOR_INDEX] = recording.Index(bindings.stats);
        if (!histogram) {
            misc[CLOD_VSM_BLOCK_EXPAND_EXPANDED_OFFSETS_DESCRIPTOR_INDEX] = recording.Index(bindings.expandedOffsets);
            misc[CLOD_VSM_BLOCK_EXPAND_EXPANDED_WRITE_CURSOR_DESCRIPTOR_INDEX] = recording.Index(bindings.expandedCursor);
            misc[CLOD_VSM_BLOCK_EXPAND_EXPANDED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = recording.Index(bindings.expandedVisible);
            misc[CLOD_VSM_BLOCK_EXPAND_EXPANDED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = recording.Index(bindings.expandedTransforms);
        }
        const uint32_t rigidMetadata = recording.Index(bindings.rigidMetadata);
        const uint32_t skinnedMetadata = recording.Index(bindings.skinnedMetadata);
        for (uint32_t bucketIndex = 0; bucketIndex < numBuckets; ++bucketIndex) {
            const bool skinned = data.bucketSkinned[bucketIndex] != 0;
            misc[CLOD_VSM_BLOCK_EXPAND_ACTIVE_BLOCK_METADATA_DESCRIPTOR_INDEX] = skinned ? skinnedMetadata : rigidMetadata;
            br::render::DispatchComputeIndirect(recording, skinned ? bindings.skinnedPipeline : bindings.rigidPipeline, misc,
                bindings.commandSignature, bindings.sourceArgs,
                static_cast<uint64_t>(bucketIndex) * sizeof(RasterizeClustersCommand));
        }
    }

private:
    VirtualShadowBlockExpandMode m_mode = VirtualShadowBlockExpandMode::Histogram;
    org::PipelineState m_rigidPso;
    org::PipelineState m_skinnedPso;
    org::PipelineState m_clearPso;
    std::shared_ptr<rhi::CommandSignaturePtr> m_commandSignature;
    std::shared_ptr<org::Buffer> m_sourceVisibleClustersBuffer;
    std::shared_ptr<org::Buffer> m_sourceVisibleClusterTransformIndicesBuffer;
    std::shared_ptr<org::Buffer> m_sourceHistogramBuffer;
    std::shared_ptr<org::Buffer> m_sourceIndirectArgsBuffer;
    std::shared_ptr<org::Buffer> m_expandedHistogramBuffer;
    std::shared_ptr<org::Buffer> m_expandedOffsetsBuffer;
    std::shared_ptr<org::Buffer> m_expandedWriteCursorBuffer;
    std::shared_ptr<org::Buffer> m_expandedVisibleClustersBuffer;
    std::shared_ptr<org::Buffer> m_expandedVisibleClusterTransformIndicesBuffer;
    std::shared_ptr<org::Buffer> m_virtualShadowClipmapInfoBuffer;
    std::shared_ptr<org::Buffer> m_virtualShadowActiveBlockMetadataBuffer;
    std::shared_ptr<org::Buffer> m_virtualShadowDynamicActiveBlockMetadataBuffer;
    std::shared_ptr<org::Buffer> m_virtualShadowBlockClusterCoverageBuffer;
    std::shared_ptr<org::Buffer> m_virtualShadowStatsBuffer;
    uint32_t m_expandedRecordCapacity = 0u;
    std::shared_ptr<org::ResourceGroup> m_slabResourceGroup;
    bool m_runWhenComputeSWRasterEnabledOnly = false;
};
