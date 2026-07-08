#include "Render/GraphExtensions/ClusterLOD/DeepVisibilityResolvePass.h"

#include <bit>

#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Managers/ViewManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "../shaders/PerPassRootConstants/clodDeepVisibilityResolveRootConstants.h"
#include "../shaders/PerPassRootConstants/visUtilRootConstants.h"

DeepVisibilityResolvePass::DeepVisibilityResolvePass(
    std::shared_ptr<Buffer> visibleClustersBuffer,
    std::shared_ptr<Buffer> reyesDiceQueueBuffer,
    std::shared_ptr<Buffer> reyesTessTableConfigsBuffer,
    std::shared_ptr<Buffer> reyesTessTableVerticesBuffer,
    std::shared_ptr<Buffer> reyesTessTableTrianglesBuffer,
    std::shared_ptr<Buffer> deepVisibilityNodesBuffer,
    std::shared_ptr<Buffer> deepVisibilityCounterBuffer,
    std::shared_ptr<Buffer> deepVisibilityOverflowCounterBuffer,
    std::shared_ptr<Buffer> deepVisibilityStatsBuffer,
    uint32_t patchVisibilityIndexBase)
    : m_visibleClustersBuffer(std::move(visibleClustersBuffer))
    , m_reyesDiceQueueBuffer(std::move(reyesDiceQueueBuffer))
    , m_reyesTessTableConfigsBuffer(std::move(reyesTessTableConfigsBuffer))
    , m_reyesTessTableVerticesBuffer(std::move(reyesTessTableVerticesBuffer))
    , m_reyesTessTableTrianglesBuffer(std::move(reyesTessTableTrianglesBuffer))
    , m_deepVisibilityNodesBuffer(std::move(deepVisibilityNodesBuffer))
    , m_deepVisibilityCounterBuffer(std::move(deepVisibilityCounterBuffer))
    , m_deepVisibilityOverflowCounterBuffer(std::move(deepVisibilityOverflowCounterBuffer))
    , m_deepVisibilityStatsBuffer(std::move(deepVisibilityStatsBuffer))
    , m_patchVisibilityIndexBase(patchVisibilityIndexBase)
{
    auto& settingsManager = SettingsManager::GetInstance();
    m_getPunctualLightingEnabled = settingsManager.getSettingGetter<bool>("enablePunctualLighting");
    m_getShadowsEnabled = settingsManager.getSettingGetter<bool>("enableShadows");
    m_gtaoEnabled = settingsManager.getSettingGetter<bool>("enableGTAO")();
}

void DeepVisibilityResolvePass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    const bool shadowsEnabled = m_getShadowsEnabled ? m_getShadowsEnabled() : false;
    builder->WithShaderResource(
            Builtin::Light::BufferGroup,
            Builtin::PerObjectBuffer,
            Builtin::NormalMatrixBuffer,
            Builtin::PerMeshBuffer,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerMaterialDataBuffer,
            Builtin::PerMaterialOpenPBRDataBuffer,
            Builtin::Material::TextureGroup,
            Builtin::Material::TextureStreamingMetadataBuffer,
            Builtin::Environment::PrefilteredCubemapsGroup,
            Builtin::Environment::InfoBuffer,
            Builtin::CameraBuffer,
            Builtin::Light::ActiveLightIndices,
            Builtin::Light::InfoBuffer,
            Builtin::Light::PointLightCubemapBuffer,
            Builtin::Light::SpotLightMatrixBuffer,
            Builtin::Light::DirectionalLightCascadeBuffer,
            Builtin::Light::ClusterBuffer,
            Builtin::Light::PagesBuffer,
            Builtin::CLod::Offsets,
            Builtin::CLod::GroupChunks,
            Builtin::CLod::Groups,
            Builtin::CLod::MeshMetadata,
            Builtin::CLod::AssemblyBoneRemaps,
            Builtin::CLod::AssemblyBoneRemapIndices,
            Builtin::SkeletonResources::InverseBindMatrices,
            Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo,
			Builtin::Noise::BlueNoise2D,
            m_visibleClustersBuffer,
            m_deepVisibilityNodesBuffer,
            m_deepVisibilityCounterBuffer,
            m_deepVisibilityOverflowCounterBuffer)
        .WithUnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer)
        .WithUnorderedAccess(Builtin::Color::HDRColorTarget)
        .WithUnorderedAccess(Builtin::DebugVisualization)
        .WithUnorderedAccess(m_deepVisibilityStatsBuffer);

    if (shadowsEnabled) {
        builder->WithShaderResource(
            Builtin::Shadows::CLodClipmapInfo,
            Builtin::Shadows::CLodDirectionalPageViewInfo,
            Builtin::Shadows::CLodPageTable,
            Builtin::Shadows::CLodPhysicalPages,
		    Builtin::Shadows::CLodCompactMainCamera,
            Builtin::Shadows::CLodCompactShadowCameras);
    }

    if (m_reyesDiceQueueBuffer) {
        builder->WithShaderResource(m_reyesDiceQueueBuffer);
    }

    if (m_reyesTessTableConfigsBuffer && m_reyesTessTableVerticesBuffer && m_reyesTessTableTrianglesBuffer) {
        builder->WithShaderResource(
            m_reyesTessTableConfigsBuffer,
            m_reyesTessTableVerticesBuffer,
            m_reyesTessTableTrianglesBuffer);
    }

    if (m_primaryHeadPointerTexture) {
        builder->WithShaderResource(m_primaryHeadPointerTexture);
    }

    builder->WithConstantBuffer(Builtin::PerFrameBuffer);
}

void DeepVisibilityResolvePass::Setup()
{
    if (m_getShadowsEnabled && m_getShadowsEnabled()) {
        RegisterSRV(SRVViewType::Texture2DArrayFull, Builtin::Shadows::CLodPageTable);
    }
    m_pHDRTarget = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::Color::HDRColorTarget);
}

void DeepVisibilityResolvePass::Update(const UpdateExecutionContext& executionContext)
{
    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;

    std::shared_ptr<PixelBuffer> primaryHeadPointers;
    context.viewManager->ForEachFiltered(ViewFilter::PrimaryCameras(), [&](uint64_t viewID) {
        if (!primaryHeadPointers) {
            primaryHeadPointers = context.viewManager->EnsureCLodDeepVisibilityHeadPointers(viewID);
        }
    });

    m_declaredResourcesChanged = m_primaryHeadPointerTexture != primaryHeadPointers;
    m_primaryHeadPointerTexture = std::move(primaryHeadPointers);
}

bool DeepVisibilityResolvePass::DeclaredResourcesChanged() const
{
    return m_declaredResourcesChanged;
}

PassReturn DeepVisibilityResolvePass::Execute(PassExecutionContext& executionContext)
{
    if (!m_primaryHeadPointerTexture || !m_pHDRTarget) {
        return {};
    }

    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;
    auto& psoManager = PSOManager::GetInstance();

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(psoManager.GetComputeRootSignature().GetHandle());

    const auto& pso = psoManager.GetClusterLODDeepVisibilityResolvePSO(context.globalPSOFlags);
    commandList.BindPipeline(pso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, pso.GetResourceDescriptorSlots());

    uint32_t misc[NumMiscUintRootConstants] = {};
    misc[MiscEnableShadows] = m_getShadowsEnabled();
    misc[MiscEnablePunctualLights] = m_getPunctualLightingEnabled();
    misc[MiscEnableGTAO] = m_gtaoEnabled;
    misc[CLOD_DEEP_VISIBILITY_RESOLVE_HEAD_POINTER_DESCRIPTOR_INDEX] = m_primaryHeadPointerTexture->GetSRVInfo(0).slot.index;
    misc[CLOD_DEEP_VISIBILITY_RESOLVE_NODE_BUFFER_DESCRIPTOR_INDEX] = m_deepVisibilityNodesBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_DEEP_VISIBILITY_RESOLVE_COUNTER_DESCRIPTOR_INDEX] = m_deepVisibilityCounterBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_DEEP_VISIBILITY_RESOLVE_OVERFLOW_COUNTER_DESCRIPTOR_INDEX] = m_deepVisibilityOverflowCounterBuffer->GetSRVInfo(0).slot.index;
    misc[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = m_visibleClustersBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_DEEP_VISIBILITY_RESOLVE_STATS_DESCRIPTOR_INDEX] = m_deepVisibilityStatsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    misc[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] = m_reyesDiceQueueBuffer
        ? m_reyesDiceQueueBuffer->GetSRVInfo(0).slot.index
        : 0xFFFFFFFFu;
    misc[VISBUF_REYES_PATCH_INDEX_BASE] = m_patchVisibilityIndexBase;
    misc[VISBUF_REYES_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = m_reyesTessTableConfigsBuffer
        ? m_reyesTessTableConfigsBuffer->GetSRVInfo(0).slot.index
        : 0xFFFFFFFFu;
    misc[VISBUF_REYES_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX] = m_reyesTessTableVerticesBuffer
        ? m_reyesTessTableVerticesBuffer->GetSRVInfo(0).slot.index
        : 0xFFFFFFFFu;
    misc[VISBUF_REYES_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX] = m_reyesTessTableTrianglesBuffer
        ? m_reyesTessTableTrianglesBuffer->GetSRVInfo(0).slot.index
        : 0xFFFFFFFFu;
    misc[VISBUF_REYES_USE_NORMAL_MAPS] = CLodReyesUseNormalMaps() ? 1u : 0u;
    misc[VISBUF_REYES_TERRAIN_NORMAL_BLEND_AS_UINT] = std::bit_cast<uint32_t>(CLodReyesTerrainNormalBlend());
    misc[VISBUF_REYES_TERRAIN_NORMAL_MIP_BIAS] = CLodReyesTerrainNormalMipBias();
    misc[VISBUF_REYES_OBJECT_NORMAL_MAP_BLEND_AS_UINT] = std::bit_cast<uint32_t>(CLodReyesObjectNormalMapBlend());
    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        misc);

    constexpr uint32_t kThreadGroupSize = 8u;
    const uint32_t groupCountX = (context.renderResolution.x + kThreadGroupSize - 1u) / kThreadGroupSize;
    const uint32_t groupCountY = (context.renderResolution.y + kThreadGroupSize - 1u) / kThreadGroupSize;
    commandList.Dispatch(groupCountX, groupCountY, 1);
    return {};
}

void DeepVisibilityResolvePass::Cleanup()
{
}
