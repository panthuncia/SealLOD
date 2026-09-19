#include "Render/GraphExtensions/ClusterLOD/VoxelSoftwareRasterizationPass.h"

#include <algorithm>

#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/ViewManager.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadTypes.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/components.h"
#include "BuiltinResources.h"
#include "ShaderBuffers.h"
#include "Render/GraphExtensions/CLodTelemetry.h"
#include "../shaders/PerPassRootConstants/clodRasterizationRootConstants.h"

void VoxelSoftwareRasterizationPass::Record(const VoxelRasterBindings&, const VoxelRasterFrameData& data,
    org::PassRecordContext& recording) {
    auto& commands = recording.Commands();
    commands.SetDescriptorHeaps(data.resourceHeap, data.samplerHeap);
    for (const auto& step : data.steps) {
        commands.BindLayout(recording.ResolveLayout(step.buildProgram.program));
        commands.BindPipeline(recording.Resolve(step.buildProgram.program));
        if (!step.buildProgram.descriptorIndices.empty()) commands.PushConstants(rhi::ShaderStage::Compute, 0,
            org::shaderapi::kResourceDescriptorIndicesRootParameter, 0,
            static_cast<uint32_t>(step.buildProgram.descriptorIndices.size()), step.buildProgram.descriptorIndices.data());
        commands.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0,
            NumMiscUintRootConstants, step.constants.data());
        commands.Dispatch(1u, 1u, 1u);
        const auto arguments = recording.Resolve(step.arguments).GetHandle();
        rhi::BufferBarrier barrier{};
        barrier.buffer = arguments;
        barrier.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
        barrier.afterAccess = rhi::ResourceAccessType::IndirectArgument;
        barrier.beforeSync = rhi::ResourceSyncState::ComputeShading;
        barrier.afterSync = rhi::ResourceSyncState::ExecuteIndirect;
        rhi::BarrierBatch barriers{}; barriers.buffers = {&barrier, 1}; commands.Barriers(barriers);
        commands.BindLayout(recording.ResolveLayout(step.rasterProgram.program));
        commands.BindPipeline(recording.Resolve(step.rasterProgram.program));
        if (!step.rasterProgram.descriptorIndices.empty()) commands.PushConstants(rhi::ShaderStage::Compute, 0,
            org::shaderapi::kResourceDescriptorIndicesRootParameter, 0,
            static_cast<uint32_t>(step.rasterProgram.descriptorIndices.size()), step.rasterProgram.descriptorIndices.data());
        commands.ExecuteIndirect(data.commandSignature, arguments, 0, {}, 0, 1);
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

    builder->WithShaderResource(
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
            Builtin::SkeletonResources::SkinningInstanceInfo,
            m_voxelWorkRecordsBuffers[0],
            m_voxelWorkRecordsBuffers[1],
            m_visibleClustersBuffer,
            m_voxelWorkCounterBuffers[0],
            m_voxelWorkCounterBuffers[1])
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
        builder->WithShaderResource(
                m_virtualShadowClipmapInfoBuffer,
                Builtin::Shadows::CLodDirectionalPageViewInfo)
            .WithUnorderedAccess(
                m_virtualShadowPageTableTexture,
                m_virtualShadowPhysicalPagesTexture,
                m_virtualShadowDynamicPagesTexture);
    }

    if (m_slabResourceGroup) {
        builder->WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
    }
    VoxelRasterBindings bindings{
        builder->BindShaderResource(m_visibleClustersBuffer),
        builder->BindShaderResource(m_visibleClusterTransformIndicesBuffer),
        {}};
    bindings.hasTelemetry = static_cast<bool>(m_telemetryBuffer);
    if (bindings.hasTelemetry) bindings.telemetry = builder->BindUnorderedAccess(m_telemetryBuffer);
    for (uint32_t i = 0; i < 2; ++i) {
        bindings.workRecords[i] = builder->BindShaderResource(m_voxelWorkRecordsBuffers[i]);
        bindings.workCounters[i] = builder->BindShaderResource(m_voxelWorkCounterBuffers[i]);
        bindings.indirectArgs[i] = builder->BindUnorderedAccess(m_voxelIndirectArgsBuffers[i]);
    }
    if (m_outputKind == CLodRasterOutputKind::VirtualShadow) {
        bindings.pageTable = builder->BindUnorderedAccess(m_virtualShadowPageTableTexture);
        bindings.clipmapInfo = builder->BindShaderResource(m_virtualShadowClipmapInfoBuffer);
        bindings.physicalPages = builder->BindUnorderedAccess(m_virtualShadowPhysicalPagesTexture);
        bindings.dynamicPages = builder->BindUnorderedAccess(m_virtualShadowDynamicPagesTexture);
        bindings.virtualShadow = true;
    }
    return bindings;
}

void VoxelSoftwareRasterizationPass::Update(const org::UpdateExecutionContext& executionContext)
{
    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;
    // Only the declarations are maintained here: the view table the shader
    // reads is published during preparation.
    std::vector<std::shared_ptr<org::PixelBuffer>> nextVisibilityBuffers;
    if (m_outputKind != CLodRasterOutputKind::VirtualShadow)
        for (const auto& viewInfo : context.Views())
            if (viewInfo.visibilityBuffer && viewInfo.cameraBufferIndex < context.ViewCameraBufferSize())
                nextVisibilityBuffers.push_back(viewInfo.visibilityBuffer);

    m_declaredResourcesChanged = (nextVisibilityBuffers != m_visibilityBuffers);
    m_visibilityBuffers = std::move(nextVisibilityBuffers);
}

bool VoxelSoftwareRasterizationPass::DeclaredResourcesChanged() const
{
    return m_declaredResourcesChanged;
}

VoxelRasterFrameData VoxelSoftwareRasterizationPass::Prepare(const VoxelRasterBindings& bindings,
    const org::PassPrepareContext& preparation) const
{
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    VoxelRasterFrameData data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle(); data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.commandSignature = preparation.CaptureCommandSignature(m_dispatchCommandSignature);
    const auto srv = [&](org::ResourceBindingToken token) {
        return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource}).index;
    };
    const auto uav = [&](org::ResourceBindingToken token, uint32_t variant = UINT32_MAX) {
        return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess, variant}).index;
    };
    std::array<uint32_t, NumMiscUintRootConstants> misc{};
    misc[CLOD_RASTER_VOXEL_WORK_CAPACITY] = m_voxelWorkCapacity;
    misc[CLOD_RASTER_VOXEL_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = srv(bindings.visible);
    misc[CLOD_RASTER_VOXEL_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = srv(bindings.transforms);
    misc[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX] = bindings.hasTelemetry && IsCLodWorkGraphTelemetryEnabled()
        ? uav(bindings.telemetry) : 0xFFFFFFFFu;
    misc[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX] = BuildCLodVisibilityViewRasterInfo(
        context->Views(), context->ViewCameraBufferSize(), m_outputKind).Publish(preparation, m_viewRasterInfoPublisher);
    if (bindings.virtualShadow) {
        const auto config = CLodVirtualShadowBuildRuntimeResolutionConfig();
        misc[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX] =
            uav(bindings.pageTable, static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull));
        misc[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX] = srv(bindings.clipmapInfo);
        misc[CLOD_RASTER_VIRTUAL_SHADOW_PHYSICAL_PAGES_DESCRIPTOR_INDEX] = uav(bindings.physicalPages);
        misc[CLOD_RASTER_VIRTUAL_SHADOW_DYNAMIC_PAGES_DESCRIPTOR_INDEX] = uav(bindings.dynamicPages);
        misc[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_RESOLUTION] = config.pageTableResolution;
        misc[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
        misc[CLOD_RASTER_VIRTUAL_SHADOW_VIRTUAL_RESOLUTION] = config.virtualResolution;
    }
    const bool telemetry = bindings.hasTelemetry && IsCLodWorkGraphTelemetryEnabled();
    for (uint32_t i = 0; i < data.steps.size(); ++i) {
        auto& step = data.steps[i]; step.constants = misc;
        step.constants[CLOD_RASTER_VOXEL_WORK_RECORDS_DESCRIPTOR_INDEX] = srv(bindings.workRecords[i]);
        step.constants[CLOD_RASTER_VOXEL_WORK_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.workCounters[i]);
        step.constants[CLOD_RASTER_VOXEL_INDIRECT_ARGS_DESCRIPTOR_INDEX] = uav(bindings.indirectArgs[i]);
        step.buildProgram = preparation.CaptureProgramBinding(m_buildArgsPso);
        const org::PipelineState& raster = telemetry ? (i == 0 ? m_rigidTelemetryRasterPso : m_skinnedTelemetryRasterPso)
            : (i == 0 ? m_rigidRasterPso : m_skinnedRasterPso);
        step.rasterProgram = preparation.CaptureProgramBinding(raster);
        step.arguments = preparation.CaptureResource(bindings.indirectArgs[i]);
    }
    return data;
}
