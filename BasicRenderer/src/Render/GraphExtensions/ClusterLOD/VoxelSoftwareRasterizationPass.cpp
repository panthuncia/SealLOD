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
    std::shared_ptr<Buffer> voxelWorkRecordsBuffer,
    std::shared_ptr<Buffer> voxelWorkCounterBuffer,
    std::shared_ptr<Buffer> voxelIndirectArgsBuffer,
    std::shared_ptr<Buffer> telemetryBuffer,
    std::shared_ptr<Buffer> viewRasterInfoBuffer,
    CLodRasterOutputKind outputKind,
    std::shared_ptr<PixelBuffer> virtualShadowPageTableTexture,
    std::shared_ptr<PixelBuffer> virtualShadowPhysicalPagesTexture,
    std::shared_ptr<Buffer> virtualShadowClipmapInfoBuffer,
    std::shared_ptr<ResourceGroup> slabResourceGroup,
    uint32_t voxelWorkCapacity)
    : m_visibleClustersBuffer(std::move(visibleClustersBuffer))
    , m_voxelWorkRecordsBuffer(std::move(voxelWorkRecordsBuffer))
    , m_voxelWorkCounterBuffer(std::move(voxelWorkCounterBuffer))
    , m_voxelIndirectArgsBuffer(std::move(voxelIndirectArgsBuffer))
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
    std::vector<DxcDefine> defines = {
        { L"CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW", outputKind == CLodRasterOutputKind::VirtualShadow ? L"1" : L"0" },
    };
    m_buildArgsPso = psoManager.MakeComputePipeline(
        computeLayout,
        L"shaders/ClusterLOD/voxelSoftwareRaster.hlsl",
        L"VoxelRasterBuildDispatchArgsCS",
        defines,
        "CLod.VoxelRaster.BuildDispatchArgs");
    m_rasterPso = psoManager.MakeComputePipeline(
        computeLayout,
        L"shaders/ClusterLOD/voxelSoftwareRaster.hlsl",
        L"VoxelRasterCS",
        defines,
        "CLod.VoxelRaster.Rasterize");

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
            Builtin::SkeletonResources::InverseBindMatrices,
            Builtin::SkeletonResources::InverseSkinMatrices,
            Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo,
            m_voxelWorkRecordsBuffer,
            m_visibleClustersBuffer,
            m_voxelWorkCounterBuffer,
            m_viewRasterInfoBuffer)
        .WithUnorderedAccess(m_voxelIndirectArgsBuffer, m_telemetryBuffer, Builtin::DebugVisualization)
        .WithInternalTransition(m_voxelIndirectArgsBuffer, indirectState)
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
    misc[CLOD_RASTER_VOXEL_WORK_RECORDS_DESCRIPTOR_INDEX] = m_voxelWorkRecordsBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_RASTER_VOXEL_WORK_COUNTER_DESCRIPTOR_INDEX] = m_voxelWorkCounterBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_RASTER_VOXEL_INDIRECT_ARGS_DESCRIPTOR_INDEX] = m_voxelIndirectArgsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    misc[CLOD_RASTER_VOXEL_WORK_CAPACITY] = m_voxelWorkCapacity;
    misc[CLOD_RASTER_VOXEL_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = m_visibleClustersBuffer->GetSRVInfo(0).slot.index;
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

    commandList.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, misc);
    BindResourceDescriptorIndices(commandList, m_buildArgsPso.GetResourceDescriptorSlots());
    commandList.BindPipeline(m_buildArgsPso.GetAPIPipelineState().GetHandle());
    commandList.Dispatch(1u, 1u, 1u);

    rhi::BufferBarrier argsBarrier{};
    argsBarrier.buffer = m_voxelIndirectArgsBuffer->GetAPIResource().GetHandle();
    argsBarrier.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
    argsBarrier.afterAccess = rhi::ResourceAccessType::IndirectArgument;
    argsBarrier.beforeSync = rhi::ResourceSyncState::ComputeShading;
    argsBarrier.afterSync = rhi::ResourceSyncState::ExecuteIndirect;
    rhi::BarrierBatch barrierBatch{};
    barrierBatch.buffers = { &argsBarrier };
    commandList.Barriers(barrierBatch);

    BindResourceDescriptorIndices(commandList, m_rasterPso.GetResourceDescriptorSlots());
    commandList.BindPipeline(m_rasterPso.GetAPIPipelineState().GetHandle());
    commandList.ExecuteIndirect(
        m_dispatchCommandSignature->GetHandle(),
        m_voxelIndirectArgsBuffer->GetAPIResource().GetHandle(),
        0,
        {},
        0,
        1);

    return {};
}

void VoxelSoftwareRasterizationPass::Cleanup() {}
