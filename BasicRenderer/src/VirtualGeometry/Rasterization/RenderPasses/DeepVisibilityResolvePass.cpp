#include "VirtualGeometry/Rasterization/RenderPasses/DeepVisibilityResolvePass.h"

#include <bit>

#include "Pipeline/PipelineState/PSOManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "Scene/Views/ViewManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "../shaders/PerPassRootConstants/clodDeepVisibilityResolveRootConstants.h"
#include "../shaders/PerPassRootConstants/visUtilRootConstants.h"

DeepVisibilityResolvePass::DeepVisibilityResolvePass(
    std::shared_ptr<org::Buffer> visibleClustersBuffer,
    std::shared_ptr<org::Buffer> reyesDiceQueueBuffer,
    std::shared_ptr<org::Buffer> reyesTessTableConfigsBuffer,
    std::shared_ptr<org::Buffer> reyesTessTableVerticesBuffer,
    std::shared_ptr<org::Buffer> reyesTessTableTrianglesBuffer,
    std::shared_ptr<org::Buffer> deepVisibilityNodesBuffer,
    std::shared_ptr<org::Buffer> deepVisibilityCounterBuffer,
    std::shared_ptr<org::Buffer> deepVisibilityOverflowCounterBuffer,
    std::shared_ptr<org::Buffer> deepVisibilityStatsBuffer,
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

DeepVisibilityResolveBindings DeepVisibilityResolvePass::Declare(org::PassBuilder& declaration)
{
    declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    auto* builder = &declaration;
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
            Builtin::OpenPBR::FuzzLTC,
            Builtin::OpenPBR::IdealMetalEnergyComplement,
            Builtin::OpenPBR::IdealMetalAverageEnergyComplement,
            Builtin::OpenPBR::OpaqueDielectricEnergyComplement,
            Builtin::OpenPBR::OpaqueDielectricAverageEnergyComplement,
            Builtin::CLod::Offsets,
            Builtin::CLod::GroupChunks,
            Builtin::CLod::Groups,
            Builtin::CLod::MeshMetadata,
            Builtin::CLod::AssemblyTransforms,
            Builtin::CLod::AssemblyBoneRemaps,
            Builtin::CLod::AssemblyBoneRemapIndices,
            Builtin::SkeletonResources::InverseBindMatrices,
            Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo,
            Builtin::Noise::BlueNoise2D)
        .WithUnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer)
        .WithUnorderedAccess(Builtin::Color::HDRColorTarget)
        .WithUnorderedAccess(Builtin::DebugVisualization)
        ;

    DeepVisibilityResolveBindings bindings;
    bindings.visibleClusters = builder->BindShaderResource(m_visibleClustersBuffer);
    bindings.nodes = builder->BindShaderResource(m_deepVisibilityNodesBuffer);
    bindings.counter = builder->BindShaderResource(m_deepVisibilityCounterBuffer);
    bindings.overflow = builder->BindShaderResource(m_deepVisibilityOverflowCounterBuffer);
    bindings.stats = builder->BindUnorderedAccess(m_deepVisibilityStatsBuffer);

    if (shadowsEnabled) {
        builder->WithShaderResource(
            Builtin::Shadows::CLodClipmapInfo,
            Builtin::Shadows::CLodDirectionalPageViewInfo,
            Builtin::Shadows::CLodPageMetadata,
            Builtin::Shadows::CLodPageTable,
            Builtin::Shadows::CLodPhysicalPages,
		    Builtin::Shadows::CLodCompactMainCamera,
            Builtin::Shadows::CLodCompactShadowCameras);
    }

    if (m_reyesDiceQueueBuffer) {
        bindings.diceQueue = builder->BindShaderResource(m_reyesDiceQueueBuffer);
        bindings.hasDiceQueue = true;
    }

    if (m_reyesTessTableConfigsBuffer && m_reyesTessTableVerticesBuffer && m_reyesTessTableTrianglesBuffer) {
        bindings.tessConfigs = builder->BindShaderResource(m_reyesTessTableConfigsBuffer);
        bindings.tessVertices = builder->BindShaderResource(m_reyesTessTableVerticesBuffer);
        bindings.tessTriangles = builder->BindShaderResource(m_reyesTessTableTrianglesBuffer);
        bindings.hasTessTables = true;
    }

    if (m_primaryHeadPointerTexture) {
        bindings.headPointers = builder->BindShaderResource(m_primaryHeadPointerTexture);
    }

    builder->WithConstantBuffer(Builtin::PerFrameBuffer);
    bindings.patchIndexBase = m_patchVisibilityIndexBase;
    bindings.width = m_renderWidth;
    bindings.height = m_renderHeight;
    bindings.globalPsoFlags = m_globalPsoFlags;
    bindings.ready = m_primaryHeadPointerTexture && m_pHDRTarget;
    bindings.shadows = shadowsEnabled;
    bindings.punctualLights = m_getPunctualLightingEnabled ? m_getPunctualLightingEnabled() : false;
    bindings.gtao = m_gtaoEnabled;
    bindings.useNormalMaps = CLodReyesUseNormalMaps();
    bindings.terrainNormalBlend = CLodReyesTerrainNormalBlend();
    bindings.terrainNormalMipBias = CLodReyesTerrainNormalMipBias();
    bindings.objectNormalMapBlend = CLodReyesObjectNormalMapBlend();
    return bindings;
}

void DeepVisibilityResolvePass::Initialize()
{
    RegisterSRV(org::SRVViewType::Texture2DArrayFull, Builtin::OpenPBR::OpaqueDielectricEnergyComplement);
    if (m_getShadowsEnabled && m_getShadowsEnabled()) {
        RegisterSRV(org::SRVViewType::Texture2DArrayFull, Builtin::Shadows::CLodPageTable);
    }
    m_pHDRTarget = m_resourceRegistryView->RequestPtr<org::PixelBuffer>(Builtin::Color::HDRColorTarget);
}

void DeepVisibilityResolvePass::Update(const org::UpdateExecutionContext& executionContext)
{
    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;
    m_renderWidth = context.renderResolution.x;
    m_renderHeight = context.renderResolution.y;
    m_globalPsoFlags = context.globalPSOFlags;

    std::shared_ptr<org::PixelBuffer> primaryHeadPointers;
    for (const auto& view : context.Views()) if (view.primary) {
        primaryHeadPointers = view.deepVisibilityHeadPointers;
        break;
    }

    m_declaredResourcesChanged = m_primaryHeadPointerTexture != primaryHeadPointers;
    m_primaryHeadPointerTexture = std::move(primaryHeadPointers);
}

bool DeepVisibilityResolvePass::DeclaredResourcesChanged() const
{
    return m_declaredResourcesChanged;
}

br::render::PreparedComputeDispatch DeepVisibilityResolvePass::Prepare(
    const DeepVisibilityResolveBindings& bindings, const org::PassPrepareContext& preparation) const
{
    br::render::PreparedComputeDispatch data{};
    if (!bindings.ready) return data;
    const auto& context = *preparation.preparationData->Get<UpdateContext>();
    data.resourceHeap = context.textureDescriptorHeap.GetHandle();
    data.samplerHeap = context.samplerDescriptorHeap.GetHandle();
    const auto& pso = PSOManager::GetInstance().GetClusterLODDeepVisibilityResolvePSO(bindings.globalPsoFlags);
    auto binding = preparation.CaptureProgramBinding(pso);
    data.program = binding.program;
    data.descriptorIndices = std::move(binding.descriptorIndices);
    const auto srv = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource}).index; };
    data.constants[MiscEnableShadows] = bindings.shadows;
    data.constants[MiscEnablePunctualLights] = bindings.punctualLights;
    data.constants[MiscEnableGTAO] = bindings.gtao;
    data.constants[CLOD_DEEP_VISIBILITY_RESOLVE_HEAD_POINTER_DESCRIPTOR_INDEX] = srv(bindings.headPointers);
    data.constants[CLOD_DEEP_VISIBILITY_RESOLVE_NODE_BUFFER_DESCRIPTOR_INDEX] = srv(bindings.nodes);
    data.constants[CLOD_DEEP_VISIBILITY_RESOLVE_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.counter);
    data.constants[CLOD_DEEP_VISIBILITY_RESOLVE_OVERFLOW_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.overflow);
    data.constants[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = srv(bindings.visibleClusters);
    data.constants[CLOD_DEEP_VISIBILITY_RESOLVE_STATS_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.stats, {org::BindlessViewKind::UnorderedAccess}).index;
    data.constants[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] = bindings.hasDiceQueue ? srv(bindings.diceQueue) : 0xFFFFFFFFu;
    data.constants[VISBUF_REYES_PATCH_INDEX_BASE] = bindings.patchIndexBase;
    data.constants[VISBUF_REYES_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = bindings.hasTessTables ? srv(bindings.tessConfigs) : 0xFFFFFFFFu;
    data.constants[VISBUF_REYES_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX] = bindings.hasTessTables ? srv(bindings.tessVertices) : 0xFFFFFFFFu;
    data.constants[VISBUF_REYES_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX] = bindings.hasTessTables ? srv(bindings.tessTriangles) : 0xFFFFFFFFu;
    data.constants[VISBUF_REYES_USE_NORMAL_MAPS] = bindings.useNormalMaps ? 1u : 0u;
    data.constants[VISBUF_REYES_TERRAIN_NORMAL_BLEND_AS_UINT] = std::bit_cast<uint32_t>(bindings.terrainNormalBlend);
    data.constants[VISBUF_REYES_TERRAIN_NORMAL_MIP_BIAS] = bindings.terrainNormalMipBias;
    data.constants[VISBUF_REYES_OBJECT_NORMAL_MAP_BLEND_AS_UINT] = std::bit_cast<uint32_t>(bindings.objectNormalMapBlend);
    data.groupsX = (bindings.width + 7u) / 8u;
    data.groupsY = (bindings.height + 7u) / 8u;
    return data;
}

void DeepVisibilityResolvePass::Record(const DeepVisibilityResolveBindings&,
    const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeDispatch(data, recording);
}
