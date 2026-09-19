#include "Render/GraphExtensions/ClusterLOD/ReyesVirtualShadowHardwareRasterPass.h"

#include "Managers/MaterialManager.h"
#include "Managers/ViewManager.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadTypes.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodRasterizationRootConstants.h"

ReyesVirtualShadowHardwareRasterPass::ReyesVirtualShadowHardwareRasterPass(
    std::shared_ptr<org::Buffer> visibleClustersBuffer,
    std::shared_ptr<org::Buffer> rasterBucketsHistogramBuffer,
    std::shared_ptr<org::Buffer> rasterBucketsIndirectArgsBuffer,
    std::shared_ptr<org::Buffer> packedRasterWorkGroupsBuffer,
    std::shared_ptr<org::Buffer> compactedRasterWorkIndicesBuffer,
    std::shared_ptr<org::Buffer> rasterWorkBuffer,
    std::shared_ptr<org::Buffer> diceQueueBuffer,
    std::shared_ptr<org::Buffer> tessTableConfigsBuffer,
    std::shared_ptr<org::Buffer> tessTableVerticesBuffer,
    std::shared_ptr<org::Buffer> tessTableTrianglesBuffer,
    std::shared_ptr<org::PixelBuffer> virtualShadowPageTableTexture,
    std::shared_ptr<org::PixelBuffer> virtualShadowPhysicalPagesTexture,
    std::shared_ptr<org::PixelBuffer> virtualShadowDynamicPagesTexture,
    std::shared_ptr<org::Buffer> virtualShadowClipmapInfoBuffer,
    std::shared_ptr<org::Buffer> telemetryBuffer,
    std::shared_ptr<org::ResourceGroup> slabResourceGroup)
    : m_visibleClustersBuffer(std::move(visibleClustersBuffer))
    , m_rasterBucketsHistogramBuffer(std::move(rasterBucketsHistogramBuffer))
    , m_rasterBucketsIndirectArgsBuffer(std::move(rasterBucketsIndirectArgsBuffer))
    , m_packedRasterWorkGroupsBuffer(std::move(packedRasterWorkGroupsBuffer))
    , m_compactedRasterWorkIndicesBuffer(std::move(compactedRasterWorkIndicesBuffer))
    , m_rasterWorkBuffer(std::move(rasterWorkBuffer))
    , m_diceQueueBuffer(std::move(diceQueueBuffer))
    , m_tessTableConfigsBuffer(std::move(tessTableConfigsBuffer))
    , m_tessTableVerticesBuffer(std::move(tessTableVerticesBuffer))
    , m_tessTableTrianglesBuffer(std::move(tessTableTrianglesBuffer))
    , m_virtualShadowPageTableTexture(std::move(virtualShadowPageTableTexture))
    , m_virtualShadowPhysicalPagesTexture(std::move(virtualShadowPhysicalPagesTexture))
    , m_virtualShadowDynamicPagesTexture(std::move(virtualShadowDynamicPagesTexture))
    , m_virtualShadowClipmapInfoBuffer(std::move(virtualShadowClipmapInfoBuffer))
    , m_telemetryBuffer(std::move(telemetryBuffer))
    , m_slabResourceGroup(std::move(slabResourceGroup)) {
    m_viewRasterInfoBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodViewRasterInfo), false, false, false, false);
    m_viewRasterInfoBuffer->SetName("CLodReyesVirtualShadowHardwareViewRasterInfo");
    org::memory::SetResourceUsageHint(*m_viewRasterInfoBuffer, "Cluster LOD Reyes virtual shadow hardware");

    rhi::IndirectArg args[] = {
        {.kind = rhi::IndirectArgKind::Constant, .u = {.rootConstants = { IndirectCommandSignatureRootSignatureIndex, 0, 3 } } },
        {.kind = rhi::IndirectArgKind::DispatchMesh }
    };

    auto device = DeviceManager::GetInstance().GetDevice();
    m_rasterizationCommandSignature = std::make_shared<rhi::CommandSignaturePtr>();
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, 2), sizeof(RasterizeClustersCommand) },
        PSOManager::GetInstance().GetRootSignature().GetHandle(),
        *m_rasterizationCommandSignature);
}

ReyesVirtualShadowHardwareRasterPass::~ReyesVirtualShadowHardwareRasterPass() = default;

ReyesShadowHardwareBindings ReyesVirtualShadowHardwareRasterPass::Declare(org::PassBuilder& declaration) {
    declaration.PreferQueue(org::QueueKind::Graphics);
    auto* builder = &declaration;
    builder->WithShaderResource(
            Builtin::PerObjectBuffer,
            Builtin::PerMeshBuffer,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerMaterialDataBuffer,
            Builtin::PerMaterialOpenPBRDataBuffer,
            Builtin::Material::TextureStreamingMetadataBuffer,
            Builtin::CameraBuffer,
            Builtin::Shadows::CLodDirectionalPageViewInfo,
            Builtin::SkeletonResources::InverseBindMatrices,
            Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo)
		.WithUnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer)
        .WithUnorderedAccess(
            Builtin::Shadows::CLodStats)
        .IsGeometryPass();

    if (m_slabResourceGroup) {
        builder->WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
    }

    builder->WithConstantBuffer(Builtin::PerFrameBuffer);
    ReyesShadowHardwareBindings bindings{
        builder->BindShaderResource(m_visibleClustersBuffer), builder->BindShaderResource(m_rasterBucketsHistogramBuffer),
        builder->BindIndirectArguments(m_rasterBucketsIndirectArgsBuffer), builder->BindShaderResource(m_packedRasterWorkGroupsBuffer),
        builder->BindShaderResource(m_compactedRasterWorkIndicesBuffer), builder->BindShaderResource(m_rasterWorkBuffer),
        builder->BindShaderResource(m_diceQueueBuffer), builder->BindShaderResource(m_tessTableConfigsBuffer),
        builder->BindShaderResource(m_tessTableVerticesBuffer), builder->BindShaderResource(m_tessTableTrianglesBuffer),
        builder->BindUnorderedAccess(m_virtualShadowPageTableTexture), builder->BindUnorderedAccess(m_virtualShadowPhysicalPagesTexture),
        builder->BindUnorderedAccess(m_virtualShadowDynamicPagesTexture), builder->BindShaderResource(m_virtualShadowClipmapInfoBuffer),
        builder->BindUnorderedAccess(m_telemetryBuffer), builder->BindShaderResource(m_viewRasterInfoBuffer)};
    bindings.width = m_passWidth; bindings.height = m_passHeight;
    bindings.pageTableResolution = m_shadowConfig.pageTableResolution;
    bindings.virtualResolution = m_shadowConfig.virtualResolution;
    bindings.bucketFlags = m_bucketFlags;
    return bindings;
}

void ReyesVirtualShadowHardwareRasterPass::Update(const org::UpdateExecutionContext& executionContext) {
    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;
    const CLodVirtualShadowResolutionConfig virtualShadowConfig = CLodVirtualShadowBuildRuntimeResolutionConfig();
    m_shadowConfig = virtualShadowConfig;
    m_bucketFlags.clear();
    m_bucketFlags.reserve(context.preparedRasterBucketFlags.size());
    for (const auto flags : context.preparedRasterBucketFlags) m_bucketFlags.push_back(static_cast<uint32_t>(flags));

    const auto numViews = context.ViewCameraBufferSize();
    std::vector<CLodViewRasterInfo> nextViewRasterInfos(numViews);

    for (const auto& viewInfo : context.Views()) {
        const auto cameraIndex = viewInfo.cameraBufferIndex;
        CLodViewRasterInfo info{};
        if (viewInfo.shadow && viewInfo.lightType == Components::LightType::Directional) {
            info.scissorMinX = 0u;
            info.scissorMinY = 0u;
            info.scissorMaxX = virtualShadowConfig.virtualResolution;
            info.scissorMaxY = virtualShadowConfig.virtualResolution;
            info.viewportScaleX = 1.0f;
            info.viewportScaleY = 1.0f;
        }
        nextViewRasterInfos[cameraIndex] = info;
    }

    m_passWidth = virtualShadowConfig.virtualResolution;
    m_passHeight = virtualShadowConfig.virtualResolution;

    if (m_viewRasterInfos != nextViewRasterInfos) {
        m_viewRasterInfos = std::move(nextViewRasterInfos);
        m_viewRasterInfoBuffer->ResizeStructured(static_cast<uint32_t>(m_viewRasterInfos.size()));
        UploadBufferData(
            m_viewRasterInfos.data(),
            static_cast<uint32_t>(m_viewRasterInfos.size() * sizeof(CLodViewRasterInfo)),
            org::runtime::UploadTarget::FromShared(m_viewRasterInfoBuffer),
            0);
        m_declaredResourcesChanged = true;
    }
    else {
        m_declaredResourcesChanged = false;
    }
}

bool ReyesVirtualShadowHardwareRasterPass::DeclaredResourcesChanged() const {
    return m_declaredResourcesChanged;
}

ReyesShadowHardwareFrameData ReyesVirtualShadowHardwareRasterPass::Prepare(
    const ReyesShadowHardwareBindings& bindings, const org::PassPrepareContext& preparation) const {
    const auto& context = *preparation.preparationData->Get<UpdateContext>();
    ReyesShadowHardwareFrameData data{};
    data.width = bindings.width;
    data.height = bindings.height;
    data.resourceHeap = context.textureDescriptorHeap.GetHandle();
    data.samplerHeap = context.samplerDescriptorHeap.GetHandle();
    data.signature = preparation.CaptureCommandSignature(m_rasterizationCommandSignature);
    data.arguments = preparation.CaptureResource(bindings.indirectArgs);
    const auto srv = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource}).index; };
    const auto uav = [&](org::ResourceBindingToken token, uint32_t variant = UINT32_MAX) { return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess, variant}).index; };
    data.constants[CLOD_RASTER_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX] = srv(bindings.histogram);
    data.constants[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = srv(bindings.visible);
    data.constants[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX] = srv(bindings.viewInfo);
    data.constants[CLOD_RASTER_REYES_TELEMETRY_DESCRIPTOR_INDEX] = uav(bindings.telemetry);
    data.constants[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX] =
        uav(bindings.pageTable, static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull));
    data.constants[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX] = srv(bindings.clipmapInfo);
    data.constants[CLOD_RASTER_VIRTUAL_SHADOW_PHYSICAL_PAGES_DESCRIPTOR_INDEX] =
        uav(bindings.physicalPages);
    data.constants[CLOD_RASTER_VIRTUAL_SHADOW_DYNAMIC_PAGES_DESCRIPTOR_INDEX] =
        uav(bindings.dynamicPages);
    data.constants[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_RESOLUTION] = bindings.pageTableResolution;
    data.constants[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
    data.constants[CLOD_RASTER_VIRTUAL_SHADOW_VIRTUAL_RESOLUTION] = bindings.virtualResolution;
    data.constants[CLOD_RASTER_REYES_PACKED_RASTER_WORK_GROUPS_DESCRIPTOR_INDEX] = srv(bindings.packedWork);
    data.constants[CLOD_RASTER_REYES_COMPACTED_RASTER_WORK_INDICES_DESCRIPTOR_INDEX] = srv(bindings.compactedIndices);
    data.constants[CLOD_RASTER_REYES_RASTER_WORK_BUFFER_DESCRIPTOR_INDEX] = srv(bindings.work);
    data.constants[CLOD_RASTER_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] = srv(bindings.diceQueue);
    data.constants[CLOD_RASTER_REYES_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = srv(bindings.tessConfigs);
    data.constants[CLOD_RASTER_REYES_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX] = srv(bindings.tessVertices);
    data.constants[CLOD_RASTER_REYES_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX] = srv(bindings.tessTriangles);
    for (uint32_t i = 0; i < bindings.bucketFlags.size(); ++i) {
        const auto flags = static_cast<MaterialRasterFlags>(bindings.bucketFlags[i]);
        const auto* pso = PSOManager::GetInstance().TryGetClusterLODVirtualShadowReyesRasterPSO(flags);
        if (!pso) continue;
        auto binding = preparation.CaptureProgramBinding(*pso);
        data.buckets.push_back({binding.program, std::move(binding.descriptorIndices),
            static_cast<uint64_t>(i) * sizeof(RasterizeClustersCommand)});
    }
    return data;
}

void ReyesVirtualShadowHardwareRasterPass::Record(const ReyesShadowHardwareBindings&,
    const ReyesShadowHardwareFrameData& data, org::PassRecordContext& recording) {
    auto& commands = recording.Commands();
    rhi::PassBeginInfo pass{};
    pass.width = data.width;
    pass.height = data.height;
    pass.debugName = "CLod Reyes virtual shadow hardware raster pass";
    commands.BeginPass(pass);
    br::render::BindPreparedDescriptorHeaps(commands, data.resourceHeap, data.samplerHeap);
    commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
    const auto arguments = recording.Resolve(data.arguments).GetHandle();
    for (const auto& bucket : data.buckets) {
        commands.BindLayout(recording.ResolveLayout(bucket.program));
        commands.BindPipeline(recording.Resolve(bucket.program));
        if (!bucket.descriptorIndices.empty())
            commands.PushConstants(rhi::ShaderStage::AllGraphics, 0,
                org::shaderapi::kResourceDescriptorIndicesRootParameter, 0,
                static_cast<uint32_t>(bucket.descriptorIndices.size()), bucket.descriptorIndices.data());
        commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, MiscUintRootSignatureIndex, 0,
            NumMiscUintRootConstants, data.constants.data());
        commands.ExecuteIndirect(data.signature, arguments, bucket.argumentsOffset, {}, 0, 1);
    }
    commands.EndPass();
}
