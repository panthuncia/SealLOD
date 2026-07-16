#include "Render/GraphExtensions/ClusterLOD/VoxelSoftwareRasterizationPass.h"

#include <algorithm>

#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/ViewManager.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/components.h"
#include "BuiltinResources.h"
#include "ShaderBuffers.h"
#include "Render/GraphExtensions/CLodTelemetry.h"
#include "../shaders/PerPassRootConstants/clodRasterizationRootConstants.h"

VoxelSoftwareRasterizationPass::VoxelSoftwareRasterizationPass(
    std::shared_ptr<Buffer> visibleClustersBuffer,
    std::shared_ptr<Buffer> visibleClusterTransformIndicesBuffer,
    std::shared_ptr<Buffer> rigidVoxelWorkRecordsBuffer,
    std::shared_ptr<Buffer> rigidVoxelWorkCounterBuffer,
    std::shared_ptr<Buffer> skinnedVoxelWorkRecordsBuffer,
    std::shared_ptr<Buffer> skinnedVoxelWorkCounterBuffer,
    std::shared_ptr<Buffer> rigidVoxelIndirectArgsBuffer,
    std::shared_ptr<Buffer> skinnedVoxelIndirectArgsBuffer,
    std::shared_ptr<Buffer> telemetryBuffer,
    std::shared_ptr<Buffer> viewRasterInfoBuffer,
    CLodRasterOutputKind outputKind,
    std::shared_ptr<PixelBuffer> virtualShadowPageTableTexture,
    std::shared_ptr<PixelBuffer> virtualShadowPhysicalPagesTexture,
    std::shared_ptr<Buffer> virtualShadowClipmapInfoBuffer,
    std::shared_ptr<ResourceGroup> slabResourceGroup,
    uint32_t voxelWorkCapacity)
    : m_visibleClustersBuffer(std::move(visibleClustersBuffer))
    , m_visibleClusterTransformIndicesBuffer(std::move(visibleClusterTransformIndicesBuffer))
    , m_voxelWorkRecordsBuffers{ std::move(rigidVoxelWorkRecordsBuffer), std::move(skinnedVoxelWorkRecordsBuffer) }
    , m_voxelWorkCounterBuffers{ std::move(rigidVoxelWorkCounterBuffer), std::move(skinnedVoxelWorkCounterBuffer) }
    , m_voxelIndirectArgsBuffers{ std::move(rigidVoxelIndirectArgsBuffer), std::move(skinnedVoxelIndirectArgsBuffer) }
    , m_telemetryBuffer(std::move(telemetryBuffer))
    , m_viewRasterInfoBuffer(std::move(viewRasterInfoBuffer))
    , m_virtualShadowPageTableTexture(std::move(virtualShadowPageTableTexture))
    , m_virtualShadowPhysicalPagesTexture(std::move(virtualShadowPhysicalPagesTexture))
    , m_virtualShadowClipmapInfoBuffer(std::move(virtualShadowClipmapInfoBuffer))
    , m_slabResourceGroup(std::move(slabResourceGroup))
    , m_outputKind(outputKind)
    , m_voxelWorkCapacity(voxelWorkCapacity)
{
    auto& psoManager = PSOManager::GetInstance();
    const auto computeLayout = psoManager.GetComputeRootSignature().GetHandle();
    const std::string pipelineSuffix =
        outputKind == CLodRasterOutputKind::VirtualShadow ? ".VirtualShadow" : "";
    const auto pipelineId = [&pipelineSuffix](std::string_view base) {
        return std::string(base) + pipelineSuffix;
    };
    std::vector<DxcDefine> defines = {
        { L"CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW", outputKind == CLodRasterOutputKind::VirtualShadow ? L"1" : L"0" },
        { L"CLOD_VOXEL_RASTER_FAST_SPHERE_PROJECT", L"1" },
        { L"CLOD_VOXEL_RASTER_CUBE_BATCH_SIZE", L"8" },
    };
    const std::string buildArgsPipelineId = pipelineId("CLod.VoxelRaster.BuildDispatchArgs");
    m_buildArgsPso = psoManager.MakeComputePipeline(
        computeLayout,
        L"shaders/ClusterLOD/voxelSoftwareRaster.hlsl",
        L"VoxelRasterBuildDispatchArgsCS",
        defines,
        buildArgsPipelineId.c_str());

    auto skinnedDefines = defines;
    skinnedDefines.push_back({ L"PSO_SKINNED", L"1" });
    auto telemetryDefines = defines;
    telemetryDefines.push_back({ L"CLOD_VOXEL_RASTER_TELEMETRY", L"1" });
    auto skinnedTelemetryDefines = skinnedDefines;
    skinnedTelemetryDefines.push_back({ L"CLOD_VOXEL_RASTER_TELEMETRY", L"1" });

    const std::string rigidPipelineId = pipelineId("CLod.VoxelRaster.Rasterize.Rigid");
    const std::string skinnedPipelineId = pipelineId("CLod.VoxelRaster.Rasterize.Skinned");
    const std::string rigidTelemetryPipelineId = pipelineId("CLod.VoxelRaster.Rasterize.Rigid.Telemetry");
    const std::string skinnedTelemetryPipelineId = pipelineId("CLod.VoxelRaster.Rasterize.Skinned.Telemetry");
    m_rigidRasterPso = psoManager.MakeComputePipeline(
        computeLayout,
        L"shaders/ClusterLOD/voxelSoftwareRaster.hlsl",
        L"VoxelRasterCS",
        defines,
        rigidPipelineId.c_str());
    m_skinnedRasterPso = psoManager.MakeComputePipeline(
        computeLayout,
        L"shaders/ClusterLOD/voxelSoftwareRaster.hlsl",
        L"VoxelRasterCS",
        skinnedDefines,
        skinnedPipelineId.c_str());
    m_rigidTelemetryRasterPso = psoManager.MakeComputePipeline(
        computeLayout,
        L"shaders/ClusterLOD/voxelSoftwareRaster.hlsl",
        L"VoxelRasterCS",
        telemetryDefines,
        rigidTelemetryPipelineId.c_str());
    m_skinnedTelemetryRasterPso = psoManager.MakeComputePipeline(
        computeLayout,
        L"shaders/ClusterLOD/voxelSoftwareRaster.hlsl",
        L"VoxelRasterCS",
        skinnedTelemetryDefines,
        skinnedTelemetryPipelineId.c_str());

    rhi::IndirectArg args[] = {
        {.kind = rhi::IndirectArgKind::Dispatch }
    };
    DeviceManager::GetInstance().GetDevice().CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, 1), sizeof(CLodVoxelRasterDispatchCommand) },
        computeLayout,
        m_dispatchCommandSignature);
}

VoxelSoftwareRasterizationPass::~VoxelSoftwareRasterizationPass() = default;

void VoxelSoftwareRasterizationPass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    const ResourceState indirectState{
        rhi::ResourceAccessType::IndirectArgument,
        rhi::ResourceLayout::GenericRead,
        rhi::ResourceSyncState::ExecuteIndirect
    };

    builder->WithShaderResource(
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerObjectBuffer,
            Builtin::CullingCameraBuffer,
            Builtin::PerMeshBuffer,
			Builtin::CLod::Offsets,
            Builtin::CLod::MeshMetadata,
            Builtin::CLod::Groups,
            Builtin::CLod::GroupPageMap,
            Builtin::CLod::AssemblyTransforms,
            Builtin::CLod::AssemblyBoneRemaps,
            Builtin::CLod::AssemblyBoneRemapIndices,
            Builtin::SkeletonResources::InverseBindMatrices,
            Builtin::SkeletonResources::InverseSkinMatrices,
            Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo,
            m_voxelWorkRecordsBuffers[0],
            m_voxelWorkRecordsBuffers[1],
            m_visibleClustersBuffer,
            m_voxelWorkCounterBuffers[0],
            m_voxelWorkCounterBuffers[1],
            m_viewRasterInfoBuffer)
        .WithShaderResource(
            Builtin::CLod::AssemblyTransforms,
            m_visibleClusterTransformIndicesBuffer)
        .WithUnorderedAccess(
            m_voxelIndirectArgsBuffers[0],
            m_voxelIndirectArgsBuffers[1],
            m_telemetryBuffer,
            Builtin::DebugVisualization)
        .WithInternalTransition(m_voxelIndirectArgsBuffers[0], indirectState)
        .WithInternalTransition(m_voxelIndirectArgsBuffers[1], indirectState)
        .WithConstantBuffer(Builtin::PerFrameBuffer);

    if (m_outputKind == CLodRasterOutputKind::VisibilityBuffer) {
        for (auto& vb : m_visibilityBuffers) {
            builder->WithUnorderedAccess(vb);
        }
    }
    else if (m_outputKind == CLodRasterOutputKind::VirtualShadow) {
        builder->WithShaderResource(m_virtualShadowClipmapInfoBuffer)
            .WithUnorderedAccess(m_virtualShadowPageTableTexture, m_virtualShadowPhysicalPagesTexture);
    }

    if (m_slabResourceGroup) {
        builder->WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
    }
}

void VoxelSoftwareRasterizationPass::Setup() {}

void VoxelSoftwareRasterizationPass::Update(const UpdateExecutionContext& executionContext)
{
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

bool VoxelSoftwareRasterizationPass::DeclaredResourcesChanged() const
{
    return m_declaredResourcesChanged;
}

PassReturn VoxelSoftwareRasterizationPass::Execute(PassExecutionContext& executionContext)
{
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());

    uint32_t misc[NumMiscUintRootConstants] = {};
    misc[CLOD_RASTER_VOXEL_WORK_CAPACITY] = m_voxelWorkCapacity;
    misc[CLOD_RASTER_VOXEL_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = m_visibleClustersBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_RASTER_VOXEL_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] =
        m_visibleClusterTransformIndicesBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    if (m_telemetryBuffer && IsCLodWorkGraphTelemetryEnabled()) {
        misc[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX] = m_telemetryBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    }
    misc[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX] = m_viewRasterInfoBuffer->GetSRVInfo(0).slot.index;
    if (m_outputKind == CLodRasterOutputKind::VirtualShadow) {
        const CLodVirtualShadowResolutionConfig virtualShadowConfig = CLodVirtualShadowBuildRuntimeResolutionConfig();
        misc[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX] = m_virtualShadowPageTableTexture->GetUAVShaderVisibleInfo(UAVViewType::Texture2DArrayFull, 0).slot.index;
        misc[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX] = m_virtualShadowClipmapInfoBuffer->GetSRVInfo(0).slot.index;
        misc[CLOD_RASTER_VIRTUAL_SHADOW_PHYSICAL_PAGES_DESCRIPTOR_INDEX] = m_virtualShadowPhysicalPagesTexture->GetUAVShaderVisibleInfo(0).slot.index;
        misc[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_RESOLUTION] = virtualShadowConfig.pageTableResolution;
        misc[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
        misc[CLOD_RASTER_VIRTUAL_SHADOW_VIRTUAL_RESOLUTION] = virtualShadowConfig.virtualResolution;
    }

    const bool telemetryEnabled = m_telemetryBuffer && IsCLodWorkGraphTelemetryEnabled();
    for (uint32_t variantIndex = 0u; variantIndex < m_voxelWorkRecordsBuffers.size(); ++variantIndex) {
        misc[CLOD_RASTER_VOXEL_WORK_RECORDS_DESCRIPTOR_INDEX] = m_voxelWorkRecordsBuffers[variantIndex]->GetSRVInfo(0).slot.index;
        misc[CLOD_RASTER_VOXEL_WORK_COUNTER_DESCRIPTOR_INDEX] = m_voxelWorkCounterBuffers[variantIndex]->GetSRVInfo(0).slot.index;
        misc[CLOD_RASTER_VOXEL_INDIRECT_ARGS_DESCRIPTOR_INDEX] = m_voxelIndirectArgsBuffers[variantIndex]->GetUAVShaderVisibleInfo(0).slot.index;

        commandList.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, misc);
        BindResourceDescriptorIndices(commandList, m_buildArgsPso.GetResourceDescriptorSlots());
        commandList.BindPipeline(m_buildArgsPso.GetAPIPipelineState().GetHandle());
        commandList.Dispatch(1u, 1u, 1u);

        rhi::BufferBarrier argsBarrier{};
        argsBarrier.buffer = m_voxelIndirectArgsBuffers[variantIndex]->GetAPIResource().GetHandle();
        argsBarrier.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
        argsBarrier.afterAccess = rhi::ResourceAccessType::IndirectArgument;
        argsBarrier.beforeSync = rhi::ResourceSyncState::ComputeShading;
        argsBarrier.afterSync = rhi::ResourceSyncState::ExecuteIndirect;
        rhi::BarrierBatch barrierBatch{};
        barrierBatch.buffers = { &argsBarrier };
        commandList.Barriers(barrierBatch);

        const PipelineState& pso = telemetryEnabled
            ? (variantIndex == 0u ? m_rigidTelemetryRasterPso : m_skinnedTelemetryRasterPso)
            : (variantIndex == 0u ? m_rigidRasterPso : m_skinnedRasterPso);
        BindResourceDescriptorIndices(commandList, pso.GetResourceDescriptorSlots());
        commandList.BindPipeline(pso.GetAPIPipelineState().GetHandle());
        commandList.ExecuteIndirect(
            m_dispatchCommandSignature->GetHandle(),
            m_voxelIndirectArgsBuffers[variantIndex]->GetAPIResource().GetHandle(),
            0,
            {},
            0,
            1);
    }

    return {};
}

void VoxelSoftwareRasterizationPass::Cleanup() {}
