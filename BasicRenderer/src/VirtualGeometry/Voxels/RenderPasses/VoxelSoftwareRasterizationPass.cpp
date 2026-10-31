#include "VirtualGeometry/Voxels/RenderPasses/VoxelSoftwareRasterizationPass.h"
#include "Render/PassBuilders.h"

#include <algorithm>

#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Scene/Views/ViewManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Render/Runtime/UploadTypes.h"
#include "Runtime/GraphIntegration/Resolvers/ResourceGroupResolver.h"
#include "Resources/Buffers/Buffer.h"
#include <BasicRenderer/Extensions/ResourceComponent.h>
#include "BuiltinResources.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "BasicRenderer/Diagnostics/CLodTelemetry.h"
#include "../shaders/PerPassRootConstants/clodRasterizationRootConstants.h"

void VoxelSoftwareRasterizationPass::Record(const VoxelRasterBindings& bindings, const VoxelRasterFrame& frame,
    org::PassRecordContext& context) {
    br::render::MiscConstants misc{};
    misc[CLOD_RASTER_VOXEL_WORK_CAPACITY] = bindings.voxelWorkCapacity;
    misc[CLOD_RASTER_VOXEL_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = context.Index(bindings.visible);
    misc[CLOD_RASTER_VOXEL_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = context.Index(bindings.transforms);
    misc[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX] = frame.telemetry ? context.Index(bindings.telemetry) : 0xFFFFFFFFu;
    misc[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX] = frame.viewRasterInfoTable;
    if (bindings.virtualShadow) {
        misc[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX] = context.Index(bindings.pageTable);
        misc[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX] = context.Index(bindings.clipmapInfo);
        misc[CLOD_RASTER_VIRTUAL_SHADOW_PHYSICAL_PAGES_DESCRIPTOR_INDEX] = context.Index(bindings.physicalPages);
        misc[CLOD_RASTER_VIRTUAL_SHADOW_DYNAMIC_PAGES_DESCRIPTOR_INDEX] = context.Index(bindings.dynamicPages);
        misc[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_RESOLUTION] = frame.pageTableResolution;
        misc[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
        misc[CLOD_RASTER_VIRTUAL_SHADOW_VIRTUAL_RESOLUTION] = frame.virtualResolution;
    }
    for (uint32_t i = 0; i < 2; ++i) {
        auto constants = misc;
        constants[CLOD_RASTER_VOXEL_WORK_RECORDS_DESCRIPTOR_INDEX] = context.Index(bindings.workRecords[i]);
        constants[CLOD_RASTER_VOXEL_WORK_COUNTER_DESCRIPTOR_INDEX] = context.Index(bindings.workCounters[i]);
        constants[CLOD_RASTER_VOXEL_INDIRECT_ARGS_DESCRIPTOR_INDEX] = context.Index(bindings.indirectArgs[i]);
        br::render::DispatchCompute(context, bindings.buildArgs, constants, 1u);
        const auto arguments = bindings.indirectArgs[i].Resource();
        context.BufferBarriers({arguments},
            rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceAccessType::IndirectArgument,
            rhi::ResourceSyncState::ComputeShading, rhi::ResourceSyncState::ExecuteIndirect);
        // The raster pipeline shares the layout, so the constants pushed for the build stay bound.
        context.BindComputePipeline(frame.telemetry ? bindings.telemetryRaster[i] : bindings.raster[i]);
        context.ExecuteIndirect(bindings.commandSignature, arguments);
    }
}

VoxelSoftwareRasterizationPass::VoxelSoftwareRasterizationPass(
    std::shared_ptr<org::Buffer> visibleClustersBuffer,
    std::shared_ptr<org::Buffer> visibleClusterTransformIndicesBuffer,
    std::shared_ptr<org::Buffer> rigidVoxelWorkRecordsBuffer,
    std::shared_ptr<org::Buffer> rigidVoxelWorkCounterBuffer,
    std::shared_ptr<org::Buffer> skinnedVoxelWorkRecordsBuffer,
    std::shared_ptr<org::Buffer> skinnedVoxelWorkCounterBuffer,
    std::shared_ptr<org::Buffer> rigidVoxelIndirectArgsBuffer,
    std::shared_ptr<org::Buffer> skinnedVoxelIndirectArgsBuffer,
    std::shared_ptr<org::Buffer> telemetryBuffer,
    CLodRasterOutputKind outputKind,
    std::shared_ptr<org::PixelBuffer> virtualShadowPageTableTexture,
    std::shared_ptr<org::PixelBuffer> virtualShadowPhysicalPagesTexture,
    std::shared_ptr<org::PixelBuffer> virtualShadowDynamicPagesTexture,
    std::shared_ptr<org::Buffer> virtualShadowClipmapInfoBuffer,
    std::shared_ptr<org::ResourceGroup> slabResourceGroup,
    uint32_t voxelWorkCapacity)
    : m_visibleClustersBuffer(std::move(visibleClustersBuffer))
    , m_visibleClusterTransformIndicesBuffer(std::move(visibleClusterTransformIndicesBuffer))
    , m_voxelWorkRecordsBuffers{ std::move(rigidVoxelWorkRecordsBuffer), std::move(skinnedVoxelWorkRecordsBuffer) }
    , m_voxelWorkCounterBuffers{ std::move(rigidVoxelWorkCounterBuffer), std::move(skinnedVoxelWorkCounterBuffer) }
    , m_voxelIndirectArgsBuffers{ std::move(rigidVoxelIndirectArgsBuffer), std::move(skinnedVoxelIndirectArgsBuffer) }
    , m_telemetryBuffer(std::move(telemetryBuffer))
    , m_virtualShadowPageTableTexture(std::move(virtualShadowPageTableTexture))
    , m_virtualShadowPhysicalPagesTexture(std::move(virtualShadowPhysicalPagesTexture))
    , m_virtualShadowDynamicPagesTexture(std::move(virtualShadowDynamicPagesTexture))
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
        { L"CLOD_VSM_TWO_LAYER_VOXEL_VERSION", outputKind == CLodRasterOutputKind::VirtualShadow ? L"2" : L"0" },
        { L"CLOD_VOXEL_RASTER_FAST_SPHERE_PROJECT", L"1" },
        { L"CLOD_VOXEL_RASTER_CUBE_BATCH_SIZE", L"16" },
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
    rhi::CommandSignaturePtr commandSignature;
    DeviceManager::GetInstance().GetDevice().CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, 1), sizeof(CLodVoxelRasterDispatchCommand) },
        computeLayout,
        commandSignature);
    m_dispatchCommandSignature = std::make_shared<rhi::CommandSignaturePtr>(std::move(commandSignature));
}

VoxelSoftwareRasterizationPass::~VoxelSoftwareRasterizationPass() = default;

VoxelRasterBindings VoxelSoftwareRasterizationPass::Declare(org::PassBuilder& declaration)
{
    auto* builder = &declaration;
    builder->PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    const org::ResourceState indirectState{
        rhi::ResourceAccessType::IndirectArgument,
        rhi::ResourceLayout::GenericRead,
        rhi::ResourceSyncState::ExecuteIndirect
    };

    builder->ShaderResource(
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerObjectBuffer,
            Builtin::CullingCameraBuffer,
            Builtin::CameraBuffer,
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
            Builtin::SkeletonResources::SkinningInstanceInfo);
    builder->ShaderResource(Builtin::CLod::AssemblyTransforms);
    builder->UnorderedAccess(Builtin::DebugVisualization);
    builder->InternalTransition(m_voxelIndirectArgsBuffers[0], indirectState)
        .InternalTransition(m_voxelIndirectArgsBuffers[1], indirectState);
    builder->ConstantBuffer(Builtin::PerFrameBuffer);

    m_viewRasterInfoTable.Declare(*builder);
    if (m_outputKind == CLodRasterOutputKind::VirtualShadow) {
        builder->ShaderResource(Builtin::Shadows::CLodDirectionalPageViewInfo);
    }

    if (m_slabResourceGroup) {
        builder->ShaderResource(ResourceGroupResolver(m_slabResourceGroup));
    }
    VoxelRasterBindings bindings{m_buildArgsPso, {m_rigidRasterPso, m_skinnedRasterPso},
        {m_rigidTelemetryRasterPso, m_skinnedTelemetryRasterPso}, m_dispatchCommandSignature,
        builder->ShaderResource(m_visibleClustersBuffer).View(),
        builder->ShaderResource(m_visibleClusterTransformIndicesBuffer).View(),
        {}};
    bindings.viewRasterInfoLayout = m_viewRasterInfoTable.Declaration();
    bindings.voxelWorkCapacity = m_voxelWorkCapacity;
    bindings.hasTelemetry = static_cast<bool>(m_telemetryBuffer);
    if (bindings.hasTelemetry) bindings.telemetry = builder->UnorderedAccess(m_telemetryBuffer).View();
    for (uint32_t i = 0; i < 2; ++i) {
        bindings.workRecords[i] = builder->ShaderResource(m_voxelWorkRecordsBuffers[i]).View();
        bindings.workCounters[i] = builder->ShaderResource(m_voxelWorkCounterBuffers[i]).View();
        bindings.indirectArgs[i] = builder->UnorderedAccess(m_voxelIndirectArgsBuffers[i]).View();
    }
    if (m_outputKind == CLodRasterOutputKind::VirtualShadow) {
        bindings.pageTable = builder->UnorderedAccess(m_virtualShadowPageTableTexture,
            org::UavView{static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull)}).View();
        bindings.clipmapInfo = builder->ShaderResource(m_virtualShadowClipmapInfoBuffer).View();
        bindings.physicalPages = builder->UnorderedAccess(m_virtualShadowPhysicalPagesTexture).View();
        bindings.dynamicPages = builder->UnorderedAccess(m_virtualShadowDynamicPagesTexture).View();
        bindings.virtualShadow = true;
    }
    return bindings;
}

void VoxelSoftwareRasterizationPass::Update(const org::UpdateExecutionContext& executionContext)
{
    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;
    m_declaredResourcesChanged = m_viewRasterInfoTable.Update(context.Views(), context.ViewCameraBufferSize(), m_outputKind);
}

bool VoxelSoftwareRasterizationPass::DeclaredResourcesChanged() const
{
    return m_declaredResourcesChanged;
}

VoxelRasterFrame VoxelSoftwareRasterizationPass::Prepare(const VoxelRasterBindings& bindings,
    const org::PassPrepareContext& preparation) const
{
    VoxelRasterFrame frame{};
    frame.telemetry = bindings.hasTelemetry && IsCLodWorkGraphTelemetryEnabled();
    frame.viewRasterInfoTable =
        m_viewRasterInfoTable.Publish(preparation, m_viewRasterInfoPublisher, bindings.viewRasterInfoLayout);
    if (bindings.virtualShadow) {
        const auto config = CLodVirtualShadowBuildRuntimeResolutionConfig();
        frame.pageTableResolution = config.pageTableResolution;
        frame.virtualResolution = config.virtualResolution;
    }
    return frame;
}
