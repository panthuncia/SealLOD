#pragma once

#include <algorithm>
#include <memory>
#include <vector>

#include <rhi.h>

#include "BuiltinResources.h"
#include "Managers/MaterialManager.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"
#include "RenderPasses/PreparedComputeBarrier.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "../../../../shaders/PerPassRootConstants/clodClearUintBufferRootConstants.h"
#include "../../../../shaders/PerPassRootConstants/clodVirtualShadowBlockExpandRootConstants.h"

namespace org { class Buffer; }
namespace org { class ResourceGroup; }

enum class VirtualShadowBlockExpandMode : uint8_t
{
    Histogram,
    Emit,
};

struct VirtualShadowBlockFrameData {
    br::render::PreparedComputeDispatch clear;
    org::PreparedResourceReference clearBarrier;
    std::vector<br::render::PreparedComputeIndirect> buckets;
};

struct VirtualShadowBlockBindings {
    org::ResourceBindingToken sourceVisible, sourceTransforms, sourceHistogram, sourceArgs;
    org::ResourceBindingToken expandedHistogram, expandedOffsets, expandedCursor, expandedVisible, expandedTransforms;
    org::ResourceBindingToken clipmapInfo, rigidMetadata, skinnedMetadata, coverage, stats;
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
        builder->WithShaderResource(
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
                Builtin::SkeletonResources::SkinningInstanceInfo,
                m_sourceVisibleClustersBuffer,
                m_sourceVisibleClusterTransformIndicesBuffer,
                m_sourceHistogramBuffer,
                m_virtualShadowClipmapInfoBuffer,
                m_virtualShadowActiveBlockMetadataBuffer,
                m_virtualShadowDynamicActiveBlockMetadataBuffer)
            .WithUnorderedAccess(m_expandedHistogramBuffer)
            .WithUnorderedAccess(m_virtualShadowStatsBuffer)
            .WithIndirectArguments(m_sourceIndirectArgsBuffer)
            .WithConstantBuffer(Builtin::PerFrameBuffer);

        if (m_mode == VirtualShadowBlockExpandMode::Emit) {
            builder->WithShaderResource(m_expandedOffsetsBuffer, m_virtualShadowBlockClusterCoverageBuffer)
                .WithUnorderedAccess(
                    m_expandedWriteCursorBuffer,
                    m_expandedVisibleClustersBuffer,
                    m_expandedVisibleClusterTransformIndicesBuffer);
        } else {
            builder->WithUnorderedAccess(m_virtualShadowBlockClusterCoverageBuffer);
        }

        if (m_slabResourceGroup) {
            builder->WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
        }
        VirtualShadowBlockBindings bindings{
            builder->BindShaderResource(m_sourceVisibleClustersBuffer),
            builder->BindShaderResource(m_sourceVisibleClusterTransformIndicesBuffer),
            builder->BindShaderResource(m_sourceHistogramBuffer),
            builder->BindIndirectArguments(m_sourceIndirectArgsBuffer),
            builder->BindUnorderedAccess(m_expandedHistogramBuffer),
            {}, {}, {}, {},
            builder->BindShaderResource(m_virtualShadowClipmapInfoBuffer),
            builder->BindShaderResource(m_virtualShadowActiveBlockMetadataBuffer),
            builder->BindShaderResource(m_virtualShadowDynamicActiveBlockMetadataBuffer),
            m_mode == VirtualShadowBlockExpandMode::Histogram
                ? builder->BindUnorderedAccess(m_virtualShadowBlockClusterCoverageBuffer)
                : builder->BindShaderResource(m_virtualShadowBlockClusterCoverageBuffer),
            builder->BindUnorderedAccess(m_virtualShadowStatsBuffer)};
        if (m_mode == VirtualShadowBlockExpandMode::Emit) {
            bindings.expandedOffsets = builder->BindShaderResource(m_expandedOffsetsBuffer);
            bindings.expandedCursor = builder->BindUnorderedAccess(m_expandedWriteCursorBuffer);
            bindings.expandedVisible = builder->BindUnorderedAccess(m_expandedVisibleClustersBuffer);
            bindings.expandedTransforms = builder->BindUnorderedAccess(m_expandedVisibleClusterTransformIndicesBuffer);
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

    VirtualShadowBlockFrameData Prepare(const VirtualShadowBlockBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        VirtualShadowBlockFrameData data{};
        if (m_runWhenComputeSWRasterEnabledOnly &&
            !CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)())) return data;
        const auto& context = *preparation.preparationData->Get<UpdateContext>();
        const auto numBuckets = context.preparedRasterBucketCount;
        if (numBuckets == 0u) return data;
        const auto clearToken = m_mode == VirtualShadowBlockExpandMode::Histogram
            ? bindings.expandedHistogram : bindings.expandedCursor;
        const auto srv = [&](org::ResourceBindingToken token) {
            return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource}).index;
        };
        const auto uav = [&](org::ResourceBindingToken token) {
            return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess}).index;
        };
        data.clear.resourceHeap = context.textureDescriptorHeap.GetHandle();
        data.clear.samplerHeap = context.samplerDescriptorHeap.GetHandle();
        auto clear = preparation.CaptureProgramBinding(m_clearPso);
        data.clear.program = clear.program;
        data.clear.descriptorIndices = std::move(clear.descriptorIndices);
        data.clear.constants[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = uav(clearToken);
        data.clear.constants[CLOD_CLEAR_UINT_BUFFER_VALUE] = 0u;
        data.clear.constants[CLOD_CLEAR_UINT_BUFFER_COUNT] = numBuckets;
        data.clear.groupsX = (numBuckets + 63u) / 64u;
        data.clearBarrier = preparation.CaptureResource(clearToken);
        const auto arguments = preparation.CaptureResource(bindings.sourceArgs);
        const auto signature = preparation.CaptureCommandSignature(m_commandSignature);
        const auto rigid = preparation.CaptureProgramBinding(m_rigidPso);
        const auto skinnedBinding = preparation.CaptureProgramBinding(m_skinnedPso);
        uint32_t misc[NumMiscUintRootConstants] = {};
        misc[CLOD_VSM_BLOCK_EXPAND_SOURCE_HISTOGRAM_DESCRIPTOR_INDEX] = srv(bindings.sourceHistogram);
        misc[CLOD_VSM_BLOCK_EXPAND_SOURCE_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = srv(bindings.sourceVisible);
        misc[CLOD_VSM_BLOCK_EXPAND_SOURCE_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = srv(bindings.sourceTransforms);
        misc[CLOD_VSM_BLOCK_EXPAND_EXPANDED_HISTOGRAM_DESCRIPTOR_INDEX] = uav(bindings.expandedHistogram);
        misc[CLOD_VSM_BLOCK_EXPAND_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX] = srv(bindings.clipmapInfo);
        misc[CLOD_VSM_BLOCK_EXPAND_CLUSTER_COVERAGE_DESCRIPTOR_INDEX] =
            m_mode == VirtualShadowBlockExpandMode::Histogram
                ? uav(bindings.coverage) : srv(bindings.coverage);
        misc[CLOD_VSM_BLOCK_EXPAND_RECORD_CAPACITY] = m_expandedRecordCapacity;
        misc[CLOD_VSM_BLOCK_EXPAND_STATS_DESCRIPTOR_INDEX] =
            uav(bindings.stats);

        if (m_mode == VirtualShadowBlockExpandMode::Emit) {
            misc[CLOD_VSM_BLOCK_EXPAND_EXPANDED_OFFSETS_DESCRIPTOR_INDEX] = srv(bindings.expandedOffsets);
            misc[CLOD_VSM_BLOCK_EXPAND_EXPANDED_WRITE_CURSOR_DESCRIPTOR_INDEX] = uav(bindings.expandedCursor);
            misc[CLOD_VSM_BLOCK_EXPAND_EXPANDED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = uav(bindings.expandedVisible);
            misc[CLOD_VSM_BLOCK_EXPAND_EXPANDED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = uav(bindings.expandedTransforms);
        }

        data.buckets.reserve(numBuckets);
        for (uint32_t bucketIndex = 0; bucketIndex < numBuckets; ++bucketIndex) {
            const MaterialRasterFlags flags = context.preparedRasterBucketFlags.at(bucketIndex);
            const bool skinned =
                (flags & MaterialRasterFlagsSkinned) != 0;
            misc[CLOD_VSM_BLOCK_EXPAND_ACTIVE_BLOCK_METADATA_DESCRIPTOR_INDEX] =
                srv(skinned ? bindings.skinnedMetadata : bindings.rigidMetadata);

            const auto& binding = skinned ? skinnedBinding : rigid;
            br::render::PreparedComputeIndirect bucket{};
            bucket.resourceHeap = data.clear.resourceHeap;
            bucket.samplerHeap = data.clear.samplerHeap;
            bucket.program = binding.program;
            bucket.descriptorIndices = binding.descriptorIndices;
            std::copy(std::begin(misc), std::end(misc), bucket.constants.begin());
            bucket.commandSignature = signature;
            bucket.argumentsReference = arguments;
            bucket.argumentsOffset = static_cast<uint64_t>(bucketIndex) * sizeof(RasterizeClustersCommand);
            data.buckets.push_back(std::move(bucket));
        }
        return data;
    }

    static void Record(const VirtualShadowBlockBindings&, const VirtualShadowBlockFrameData& data,
        org::PassRecordContext& recording) {
        if (data.clear.groupsX == 0) return;
        br::render::RecordPreparedComputeDispatch(data.clear, recording);
        br::render::RecordPreparedComputeUavBarrier(data.clearBarrier, recording);
        for (const auto& bucket : data.buckets)
            br::render::RecordPreparedComputeIndirect(bucket, recording);
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
