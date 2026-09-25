#pragma once

#include <array>
#include <algorithm>
#include <memory>
#include <vector>

#include <rhi.h>

#include "BuiltinResources.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/GraphExtensions/CLodTelemetry.h"
#include "Render/RenderContext.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"
#include "Resources/PixelBuffer.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "../../../../shaders/PerPassRootConstants/clodRasterizationRootConstants.h"

namespace org { class Buffer; }
namespace org { class ResourceGroup; }

struct ClusterPageJobRasterFrameData {
    std::vector<br::render::PreparedComputeIndirect> dispatches;
};

struct ClusterPageJobRasterBindings {
    org::ResourceBindingToken compactedVisibleClusters, compactedVisibleClusterTransformIndices;
    org::ResourceBindingToken viewRasterInfo, pageTable, clipmapInfo, physicalPages, dynamicPages, stats;
    std::array<org::ResourceBindingToken, 2> pageJobCounts, pageJobRecords, indirectArgs;
};

class ClusterSoftwareRasterPageJobRasterPass : public org::TypedRenderGraphPass<ClusterSoftwareRasterPageJobRasterPass,
    ClusterPageJobRasterFrameData, ClusterPageJobRasterBindings> {
public:
    ClusterSoftwareRasterPageJobRasterPass(
        std::shared_ptr<org::Buffer> compactedVisibleClustersBuffer,
        std::shared_ptr<org::Buffer> compactedVisibleClusterTransformIndicesBuffer,
        std::shared_ptr<org::Buffer> viewRasterInfoBuffer,
        std::shared_ptr<org::PixelBuffer> virtualShadowPageTableTexture,
        std::shared_ptr<org::PixelBuffer> virtualShadowPhysicalPagesTexture,
        std::shared_ptr<org::PixelBuffer> virtualShadowDynamicPagesTexture,
        std::shared_ptr<org::Buffer> virtualShadowClipmapInfoBuffer,
        std::shared_ptr<org::Buffer> rigidPageJobCountBuffer,
        std::shared_ptr<org::Buffer> rigidPageJobRecordsBuffer,
        std::shared_ptr<org::Buffer> rigidPageJobIndirectArgsBuffer,
        std::shared_ptr<org::Buffer> skinnedPageJobCountBuffer,
        std::shared_ptr<org::Buffer> skinnedPageJobRecordsBuffer,
        std::shared_ptr<org::Buffer> skinnedPageJobIndirectArgsBuffer,
        std::shared_ptr<org::Buffer> virtualShadowStatsBuffer,
        std::shared_ptr<org::ResourceGroup> slabResourceGroup = nullptr,
        bool runWhenComputeSWRasterEnabledOnly = false)
        : m_compactedVisibleClustersBuffer(std::move(compactedVisibleClustersBuffer))
        , m_compactedVisibleClusterTransformIndicesBuffer(std::move(compactedVisibleClusterTransformIndicesBuffer))
        , m_viewRasterInfoBuffer(std::move(viewRasterInfoBuffer))
        , m_virtualShadowPageTableTexture(std::move(virtualShadowPageTableTexture))
        , m_virtualShadowPhysicalPagesTexture(std::move(virtualShadowPhysicalPagesTexture))
        , m_virtualShadowDynamicPagesTexture(std::move(virtualShadowDynamicPagesTexture))
        , m_virtualShadowClipmapInfoBuffer(std::move(virtualShadowClipmapInfoBuffer))
        , m_pageJobCountBuffers{ std::move(rigidPageJobCountBuffer), std::move(skinnedPageJobCountBuffer) }
        , m_pageJobRecordsBuffers{ std::move(rigidPageJobRecordsBuffer), std::move(skinnedPageJobRecordsBuffer) }
        , m_pageJobIndirectArgsBuffers{ std::move(rigidPageJobIndirectArgsBuffer), std::move(skinnedPageJobIndirectArgsBuffer) }
        , m_virtualShadowStatsBuffer(std::move(virtualShadowStatsBuffer))
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
            L"SWPageJobRasterPageCSMain",
            {},
            "CLod_SoftwarePageJobRasterPSO");
        std::vector<DxcDefine> skinnedDefines = { DxcDefine{ L"PSO_SKINNED", L"1" } };
        m_skinnedPso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"Shaders/ClusterLOD/softwareRasterPageJobs.hlsl",
            L"SWPageJobRasterPageCSMain",
            skinnedDefines,
            "CLod_SoftwarePageJobRasterSkinnedPSO");
    }

    ClusterPageJobRasterBindings Declare(org::PassBuilder& declaration)
    {
        declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* builder = &declaration;
        builder->WithShaderResource(
                Builtin::PerMeshBuffer,
                Builtin::PerMeshInstanceBuffer,
                Builtin::InstanceDrawRecordBuffer,
                Builtin::PerInstanceTransformBuffer,
                Builtin::PerObjectBuffer,
                Builtin::CLod::Offsets,
                Builtin::CLod::MeshMetadata,
                Builtin::CLod::Groups,
                Builtin::CullingCameraBuffer,
                Builtin::CameraBuffer,
                Builtin::Shadows::CLodDirectionalPageViewInfo,
                Builtin::SkeletonResources::InverseBindMatrices,
                Builtin::SkeletonResources::BoneTransforms,
                Builtin::SkeletonResources::SkinningInstanceInfo,
                Builtin::CLod::AssemblyTransforms,
                Builtin::CLod::AssemblyBoneRemaps,
                Builtin::CLod::AssemblyBoneRemapIndices,
                m_compactedVisibleClustersBuffer,
                m_compactedVisibleClusterTransformIndicesBuffer,
                m_viewRasterInfoBuffer,
                m_virtualShadowClipmapInfoBuffer,
                m_pageJobCountBuffers[0],
                m_pageJobRecordsBuffers[0],
                m_pageJobCountBuffers[1],
                m_pageJobRecordsBuffers[1])
            .WithUnorderedAccess(
                m_virtualShadowPageTableTexture,
                m_virtualShadowPhysicalPagesTexture,
                m_virtualShadowDynamicPagesTexture,
                m_virtualShadowStatsBuffer)
            .WithIndirectArguments(m_pageJobIndirectArgsBuffers[0], m_pageJobIndirectArgsBuffers[1])
            .WithConstantBuffer(Builtin::PerFrameBuffer);

        if (m_slabResourceGroup) {
            builder->WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
        }
        ClusterPageJobRasterBindings bindings{
            builder->BindShaderResource(m_compactedVisibleClustersBuffer),
            builder->BindShaderResource(m_compactedVisibleClusterTransformIndicesBuffer),
            builder->BindShaderResource(m_viewRasterInfoBuffer),
            builder->BindUnorderedAccess(m_virtualShadowPageTableTexture),
            builder->BindShaderResource(m_virtualShadowClipmapInfoBuffer),
            builder->BindUnorderedAccess(m_virtualShadowPhysicalPagesTexture),
            builder->BindUnorderedAccess(m_virtualShadowDynamicPagesTexture),
            builder->BindUnorderedAccess(m_virtualShadowStatsBuffer)};
        for (uint32_t i = 0; i < 2; ++i) {
            bindings.pageJobCounts[i] = builder->BindShaderResource(m_pageJobCountBuffers[i]);
            bindings.pageJobRecords[i] = builder->BindShaderResource(m_pageJobRecordsBuffers[i]);
            bindings.indirectArgs[i] = builder->BindIndirectArguments(m_pageJobIndirectArgsBuffers[i]);
        }
        return bindings;
    }

    ClusterPageJobRasterFrameData Prepare(const ClusterPageJobRasterBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        ClusterPageJobRasterFrameData data{};
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
        const auto srv = [&](org::ResourceBindingToken token, uint32_t variant = UINT32_MAX) {
            return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource, variant}).index;
        };
        const auto uav = [&](org::ResourceBindingToken token, uint32_t variant = UINT32_MAX) {
            return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess, variant}).index;
        };
        uint32_t misc[NumMiscUintRootConstants] = {};
        misc[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = srv(bindings.compactedVisibleClusters);
        misc[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] =
            srv(bindings.compactedVisibleClusterTransformIndices);
        misc[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX] = srv(bindings.viewRasterInfo);
        misc[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX] =
            uav(bindings.pageTable, static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull));
        misc[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX] = srv(bindings.clipmapInfo);
        misc[CLOD_RASTER_VIRTUAL_SHADOW_PHYSICAL_PAGES_DESCRIPTOR_INDEX] = uav(bindings.physicalPages);
        misc[CLOD_RASTER_VIRTUAL_SHADOW_DYNAMIC_PAGES_DESCRIPTOR_INDEX] =
            uav(bindings.dynamicPages);
        misc[CLOD_RASTER_VIRTUAL_SHADOW_STATS_DESCRIPTOR_INDEX] =
            uav(bindings.stats);
        misc[CLOD_RASTER_VIRTUAL_SHADOW_TELEMETRY_ENABLED] =
            IsCLodWorkGraphTelemetryEnabled() ? 1u : 0u;
        for (uint32_t variantIndex = 0; variantIndex < m_pageJobCountBuffers.size(); ++variantIndex) {
            const auto binding = preparation.CaptureProgramBinding(variantIndex ? m_skinnedPso : m_rigidPso);
            misc[CLOD_RASTER_PAGE_JOB_COUNT_DESCRIPTOR_INDEX] = srv(bindings.pageJobCounts[variantIndex]);
            misc[CLOD_RASTER_PAGE_JOB_RECORDS_DESCRIPTOR_INDEX] = srv(bindings.pageJobRecords[variantIndex]);
            br::render::PreparedComputeIndirect dispatch{};
            dispatch.resourceHeap = context.textureDescriptorHeap.GetHandle();
            dispatch.samplerHeap = context.samplerDescriptorHeap.GetHandle();
            dispatch.program = binding.program;
            dispatch.descriptorIndices = binding.descriptorIndices;
            std::copy(std::begin(misc), std::end(misc), dispatch.constants.begin());
            dispatch.commandSignature = signature;
            dispatch.argumentsReference = preparation.CaptureResource(bindings.indirectArgs[variantIndex]);
            data.dispatches.push_back(std::move(dispatch));
        }
        return data;
    }

    static void Record(const ClusterPageJobRasterBindings&, const ClusterPageJobRasterFrameData& data,
        org::PassRecordContext& recording) {
        for (const auto& dispatch : data.dispatches)
            br::render::RecordPreparedComputeIndirect(dispatch, recording);
    }

private:
    org::PipelineState m_rigidPso;
    org::PipelineState m_skinnedPso;
    std::shared_ptr<rhi::CommandSignaturePtr> m_commandSignature;
    std::shared_ptr<org::Buffer> m_compactedVisibleClustersBuffer;
    std::shared_ptr<org::Buffer> m_compactedVisibleClusterTransformIndicesBuffer;
    std::shared_ptr<org::Buffer> m_viewRasterInfoBuffer;
    std::shared_ptr<org::PixelBuffer> m_virtualShadowPageTableTexture;
    std::shared_ptr<org::PixelBuffer> m_virtualShadowPhysicalPagesTexture;
    std::shared_ptr<org::PixelBuffer> m_virtualShadowDynamicPagesTexture;
    std::shared_ptr<org::Buffer> m_virtualShadowClipmapInfoBuffer;
    std::array<std::shared_ptr<org::Buffer>, 2> m_pageJobCountBuffers;
    std::array<std::shared_ptr<org::Buffer>, 2> m_pageJobRecordsBuffers;
    std::array<std::shared_ptr<org::Buffer>, 2> m_pageJobIndirectArgsBuffers;
    std::shared_ptr<org::Buffer> m_virtualShadowStatsBuffer;
    std::shared_ptr<org::ResourceGroup> m_slabResourceGroup;
    bool m_runWhenComputeSWRasterEnabledOnly = false;
};
