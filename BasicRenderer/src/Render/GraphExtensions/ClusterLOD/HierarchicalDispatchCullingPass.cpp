#include "Render/GraphExtensions/ClusterLOD/CapturedCullingCommandSink.h"
#include "Render/GraphExtensions/ClusterLOD/HierarchicalDispatchCullingPass.h"
#include "Render/GraphExtensions/ClusterLOD/PreparedCullingWorkloads.h"
#include "RenderPasses/PreparedComputeCommands.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>
#include <BasicTelemetry/Tracy.h>

#include "BuiltinResources.h"
#include "Managers/MaterialManager.h"
#include "Managers/MeshManager.h"
#include "Managers/IndirectCommandBufferManager.h"
#include "Managers/ObjectManager.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/RendererECSManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Managers/ViewManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/GraphExtensions/CLodTelemetry.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Render/RenderContext.h"
#include "Render/IndirectStateArtifacts.h"
#include "Render/ObjectBufferStateArtifacts.h"
#include "Render/GeometryResidencyStateArtifacts.h"
#include "Render/Runtime/UploadTypes.h"
#include "Resources/components.h"
#include "Resources/Resolvers/ECSResourceResolver.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "Resources/Resolvers/PublishedStateResourceResolver.h"
#include "ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodClearUintBufferRootConstants.h"
#include "../shaders/PerPassRootConstants/clodCreateCommandRootConstants.h"
#include "../shaders/PerPassRootConstants/clodPureComputeCullingRootConstants.h"
#include "../shaders/PerPassRootConstants/clodWorkGraphRootConstants.h"

namespace {

static_assert(
    CLOD_PC_OBJECT_CULL_SHADOW_CASTER_CLASS >
        CLOD_WG_VIRTUAL_SHADOW_DYNAMIC_ACTIVE_BLOCK_METADATA_DESCRIPTOR_INDEX &&
    CLOD_PC_OBJECT_CULL_INVALIDATION_COUNT_SRV_INDEX >
        CLOD_WG_VIRTUAL_SHADOW_DYNAMIC_ACTIVE_BLOCK_METADATA_DESCRIPTOR_INDEX &&
    CLOD_WG_VIRTUAL_SHADOW_RECEIVER_MASK_DESCRIPTOR_INDEX >
        CLOD_WG_VIRTUAL_SHADOW_DYNAMIC_ACTIVE_BLOCK_METADATA_DESCRIPTOR_INDEX &&
    CLOD_PC_OBJECT_CULL_SHADOW_CASTER_CLASS !=
        CLOD_PC_OBJECT_CULL_INVALIDATION_COUNT_SRV_INDEX &&
    CLOD_PC_OBJECT_CULL_SHADOW_CASTER_CLASS !=
        CLOD_WG_VIRTUAL_SHADOW_RECEIVER_MASK_DESCRIPTOR_INDEX &&
    CLOD_PC_OBJECT_CULL_INVALIDATION_COUNT_SRV_INDEX !=
        CLOD_WG_VIRTUAL_SHADOW_RECEIVER_MASK_DESCRIPTOR_INDEX,
    "Object-cull-only constants must not overwrite shared CLod traversal state");

constexpr uint32_t kPureComputeObjectCullThreadsPerGroup = 64u;
constexpr uint32_t kPureComputeTraverseThreadsPerGroup = 64u;
constexpr uint32_t kPureComputeClusterThreadsPerGroup = 32u;
constexpr uint32_t kPureComputeMaxTraversalLevels = 64u;
constexpr bool kDisableVirtualShadowDirtyPageCulling = false; 

class PreparedComputeCommandSink {
public:
    using Constant = uint32_t;
    PreparedComputeCommandSink(
        br::render::PreparedComputeCommandBuilder& builder,
        const org::PassPrepareContext& preparation)
        : m_builder(builder), m_preparation(preparation),
          m_bindings(*preparation.bindings)
    {
        // Handles are looked up on demand (FindByHandle): eagerly resolving
        // every declared slot would make the recipe depend on all of them.
    }

    void Register(const PipelineState& pipeline)
    {
        const auto payload = pipeline.GetPayload();
        if (!payload || !payload->pso)
            throw std::invalid_argument("Cannot register an empty culling pipeline");
        const auto handle = payload->pso.Get().GetHandle();
        auto captured = m_builder.CaptureProgramBinding(payload);
        m_programs.emplace(handle, std::move(captured));
    }

    uint32_t SRVIndex(const std::shared_ptr<Resource>& resource,
        uint32_t variant = UINT32_MAX, uint32_t mip = 0,
        uint32_t slice = 0) const
    {
        return ViewIndex(resource, {org::BindlessViewKind::ShaderResource,
            variant, mip, slice});
    }

    uint32_t SRVIndex(Resource* resource,
        uint32_t variant = UINT32_MAX, uint32_t mip = 0,
        uint32_t slice = 0) const
    {
        return ViewIndex(resource, {org::BindlessViewKind::ShaderResource,
            variant, mip, slice});
    }

    uint32_t UAVIndex(const std::shared_ptr<Resource>& resource,
        uint32_t variant = UINT32_MAX, uint32_t mip = 0,
        uint32_t slice = 0) const
    {
        return ViewIndex(resource, {org::BindlessViewKind::UnorderedAccess,
            variant, mip, slice});
    }

    void SetDescriptorHeaps(
        rhi::DescriptorHeapHandle,
        std::optional<rhi::DescriptorHeapHandle>)
    {
        // Admission installs the execution-slot descriptor snapshot.
    }

    void BindLayout(rhi::PipelineLayoutHandle layout)
    {
        m_builder.Sequence().layout = layout;
    }

    void BindPipeline(rhi::PipelineHandle pipeline)
    {
        const auto found = m_programs.find(pipeline);
        if (found == m_programs.end())
            throw std::invalid_argument("Unregistered pipeline used by prepared culling commands");
        m_builder.Commands().emplace_back(br::render::PreparedBindComputeProgram{
            found->second.program, found->second.descriptorIndices});
    }

    void BindDescriptorIndices(const PipelineResources& resources)
    {
        m_builder.Commands().emplace_back(br::render::PreparedComputeDescriptorIndices{
            m_preparation.captureDescriptorIndices(resources)});
    }

    void PushConstants(rhi::ShaderStage, uint32_t, uint32_t binding,
        uint32_t destinationOffset, uint32_t count, const uint32_t* values)
    {
        m_builder.Constants(values, count, binding, destinationOffset);
    }

    void PushConstants(rhi::ShaderStage stages, uint32_t set, uint32_t binding,
        uint32_t destinationOffset, uint32_t count, const float* values)
    {
        std::vector<uint32_t> bits(count);
        if (count) std::memcpy(bits.data(), values, count * sizeof(uint32_t));
        PushConstants(stages, set, binding, destinationOffset, count, bits.data());
    }

    void Dispatch(uint32_t x, uint32_t y, uint32_t z)
    {
        m_builder.Dispatch(x, y, z);
    }

    void PushSharedConstants(rhi::ShaderStage stages, uint32_t set, uint32_t binding,
        uint32_t offset, uint32_t count, const uint32_t* values)
    {
        PushConstants(stages, set, binding, offset, count, values);
        auto& constants = std::get<br::render::PreparedComputeConstants>(m_builder.Commands().back());
        constants.invocationConstantMask = (uint64_t{1} << CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_GENERATION)
            | (uint64_t{1} << CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_ENTRY_COUNT);
    }

    void ObjectCull(const uint32_t* constants)
    {
        m_objectCullOffset = m_builder.Commands().size();
        std::copy_n(constants, m_objectCullConstants.size(), m_objectCullConstants.begin());
    }

    HierarchicalDispatchCullingRecipe FinishRecipe()
    {
        HierarchicalDispatchCullingRecipe recipe;
        recipe.prefix = std::move(m_builder).FinishData();
        if (m_objectCullOffset) {
            recipe.hasObjectCull = true;
            recipe.objectCullConstants = m_objectCullConstants;
            auto& commands = recipe.prefix.commands;
            recipe.suffix.commands.assign(std::make_move_iterator(commands.begin() + *m_objectCullOffset),
                std::make_move_iterator(commands.end()));
            commands.resize(*m_objectCullOffset);
        }
        return recipe;
    }

    void Barriers(const rhi::BarrierBatch& source)
    {
        br::render::PreparedBufferBarrierBatch batch;
        batch.barriers.reserve(source.buffers.size);
        for (const auto& barrier : source.buffers) {
            const auto found = m_bindings.FindByHandle(barrier.buffer);
            if (!found)
                throw std::invalid_argument("Undeclared buffer used by prepared culling barrier");
            batch.barriers.push_back({*found, barrier.beforeAccess, barrier.afterAccess,
                barrier.beforeSync, barrier.afterSync});
        }
        if (!batch.barriers.empty()) m_builder.Commands().emplace_back(std::move(batch));
    }

    void ExecuteIndirect(rhi::CommandSignatureHandle signature,
        rhi::ResourceHandle arguments, uint64_t argumentOffset,
        rhi::ResourceHandle countBuffer, uint64_t, uint32_t maxCount)
    {
        if (countBuffer.valid())
            throw std::invalid_argument("Prepared culling does not support count-buffer indirect execution");
        const auto found = m_bindings.FindByHandle(arguments);
        if (!found)
            throw std::invalid_argument("Undeclared indirect argument buffer in prepared culling");
        m_builder.Commands().emplace_back(br::render::PreparedExecuteIndirectCommand{
            signature, *found, argumentOffset, maxCount});
    }

private:
    uint32_t ViewIndex(const std::shared_ptr<Resource>& resource,
        org::BindlessViewRequest request) const
    {
        return ViewIndex(resource.get(), request);
    }

    uint32_t ViewIndex(Resource* resource,
        org::BindlessViewRequest request) const
    {
        if (!resource) throw std::invalid_argument(
            "Cannot resolve a null prepared culling resource");
        return m_bindings.Views(m_preparation.CaptureResource(
            resource->GetGlobalResourceID())).Resolve(request).index;
    }

    br::render::PreparedComputeCommandBuilder& m_builder;
    const org::PassPrepareContext& m_preparation;
    const org::FrozenExecutionBindings& m_bindings;
    std::unordered_map<rhi::PipelineHandle, org::PreparedProgramBinding,
        rhi::HandleHash<rhi::PipelineHandle>, rhi::HandleEqual<rhi::PipelineHandle>> m_programs;
    std::optional<size_t> m_objectCullOffset;
    std::array<uint32_t, NumMiscUintRootConstants> m_objectCullConstants{};
};

bool UsesVisibilityBufferOutput(CLodRasterOutputKind outputKind)
{
    return outputKind == CLodRasterOutputKind::VisibilityBuffer;
}

bool UsesDeepVisibilityOutput(CLodRasterOutputKind outputKind)
{
    return outputKind == CLodRasterOutputKind::DeepVisibility;
}

bool UsesVirtualShadowOutput(CLodRasterOutputKind outputKind)
{
    return outputKind == CLodRasterOutputKind::VirtualShadow;
}

bool UsesSWClassification(HierarchicalCullingWorkGraphMode mode)
{
    return mode == HierarchicalCullingWorkGraphMode::SoftwareRasterCompute
        || mode == HierarchicalCullingWorkGraphMode::SoftwareRasterWorkGraph;
}

bool UsesPerViewDepthMapOcclusion(CLodRasterOutputKind outputKind)
{
    return !UsesVirtualShadowOutput(outputKind);
}

template <typename T>
bool BytesEqual(const T& left, const T& right)
{
    return std::memcmp(&left, &right, sizeof(T)) == 0;
}

std::vector<uint64_t> CollectDeclaredDrawSetResourceIds(
    const std::shared_ptr<const br::render::PublishedRendererState>& state,
    RenderPhase renderPhase,
    bool clodOnlyWorkloads)
{
    std::vector<uint64_t> resourceIds;
    br::render::PublishedResourceQuery query{};
    query.owner = br::render::PublishedFragmentKind::ActiveDrawLists;
    query.usage = br::render::PublishedResourceUsage::ActiveDrawList;
    query.renderPhaseHash = renderPhase.hash;
    constexpr std::uint64_t clodBit = 1ull << 63u;
    if (clodOnlyWorkloads) query.requiredVariantMask = clodBit;
    else query.forbiddenVariantMask = clodBit;
    const auto resources = state && state->resourceCatalog ? state->resourceCatalog->FindAll(query)
                                                           : br::render::PublishedResourceCatalog::ResourceList{};
    for (const auto& resource : resources) if (resource) resourceIds.push_back(resource->GetGlobalResourceID());

    std::sort(resourceIds.begin(), resourceIds.end());
    resourceIds.erase(std::unique(resourceIds.begin(), resourceIds.end()), resourceIds.end());
    return resourceIds;
}

constexpr uint32_t kReplaySourceNodes = 0u;
constexpr uint32_t kReplaySourceClusters = 1u;

}

HierarchicalDispatchCullingPass::HierarchicalDispatchCullingPass(
    std::string stablePassIdentifier,
    HierarchicalCullingPassInputs inputs,
    std::shared_ptr<Buffer> visibleClustersBuffer,
    std::shared_ptr<Buffer> visibleClusterTransformIndicesBuffer,
    std::shared_ptr<Buffer> visibleClustersCounterBuffer,
    std::shared_ptr<Buffer> swVisibleClustersCounterBuffer,
    std::shared_ptr<Buffer> voxelRasterWorkBuffer,
    std::shared_ptr<Buffer> voxelRasterWorkCounterBuffer,
    std::shared_ptr<Buffer> skinnedVoxelRasterWorkBuffer,
    std::shared_ptr<Buffer> skinnedVoxelRasterWorkCounterBuffer,
    uint32_t voxelRasterWorkCapacity,
    std::shared_ptr<Buffer> pageJobVisibleClustersBuffer,
    std::shared_ptr<Buffer> pageJobVisibleClusterTransformIndicesBuffer,
    std::shared_ptr<Buffer> pageJobVisibleClustersCounterBuffer,
    std::shared_ptr<Buffer> histogramIndirectCommand,
    std::shared_ptr<Buffer> workGraphTelemetryBuffer,
    std::shared_ptr<Buffer> occlusionReplayBuffer,
    std::shared_ptr<Buffer> occlusionReplayStateBuffer,
    std::shared_ptr<Buffer> occlusionNodeGpuInputsBuffer,
    std::shared_ptr<Buffer> viewDepthSrvIndicesBuffer,
    std::shared_ptr<Buffer> viewRasterInfoBuffer,
    std::shared_ptr<PixelBuffer> shadowDirtyHierarchyTexture,
    std::shared_ptr<ResourceGroup> slabResourceGroup,
    std::shared_ptr<Buffer> phase1VisibleClustersCounterBuffer,
    std::shared_ptr<Buffer> swWriteBaseCounterBuffer,
    std::shared_ptr<Buffer> shadowPredictiveInvalidationCandidatesBuffer,
    std::shared_ptr<Buffer> shadowPredictiveInvalidationCandidateCountBuffer,
    std::shared_ptr<Buffer> shadowInvalidationCountBuffer,
    std::shared_ptr<Buffer> shadowInvalidatedInstancesBitsetBuffer,
    std::shared_ptr<PixelBuffer> shadowPageTableTexture,
    std::shared_ptr<PixelBuffer> shadowPhysicalPagesTexture,
    std::shared_ptr<Buffer> shadowActiveBlockMetadataBuffer,
    std::shared_ptr<Buffer> shadowReceiverSubpageMaskBuffer,
    std::shared_ptr<PixelBuffer> shadowDynamicPhysicalPagesTexture,
    std::shared_ptr<Buffer> shadowDynamicActiveBlockMetadataBuffer)
    : m_visibleClustersBuffer(std::move(visibleClustersBuffer))
    , m_visibleClusterTransformIndicesBuffer(std::move(visibleClusterTransformIndicesBuffer))
    , m_visibleClustersCounterBuffer(std::move(visibleClustersCounterBuffer))
    , m_swVisibleClustersCounterBuffer(std::move(swVisibleClustersCounterBuffer))
    , m_voxelRasterWorkBuffer(std::move(voxelRasterWorkBuffer))
    , m_voxelRasterWorkCounterBuffer(std::move(voxelRasterWorkCounterBuffer))
    , m_skinnedVoxelRasterWorkBuffer(std::move(skinnedVoxelRasterWorkBuffer))
    , m_skinnedVoxelRasterWorkCounterBuffer(std::move(skinnedVoxelRasterWorkCounterBuffer))
    , m_voxelRasterWorkCapacity(voxelRasterWorkCapacity)
    , m_pageJobVisibleClustersBuffer(std::move(pageJobVisibleClustersBuffer))
    , m_pageJobVisibleClusterTransformIndicesBuffer(std::move(pageJobVisibleClusterTransformIndicesBuffer))
    , m_pageJobVisibleClustersCounterBuffer(std::move(pageJobVisibleClustersCounterBuffer))
    , m_histogramIndirectCommand(std::move(histogramIndirectCommand))
    , m_workGraphTelemetryBuffer(std::move(workGraphTelemetryBuffer))
    , m_occlusionReplayBuffer(std::move(occlusionReplayBuffer))
    , m_occlusionReplayStateBuffer(std::move(occlusionReplayStateBuffer))
    , m_occlusionNodeGpuInputsBuffer(std::move(occlusionNodeGpuInputsBuffer))
    , m_viewDepthSrvIndicesBuffer(std::move(viewDepthSrvIndicesBuffer))
    , m_viewRasterInfoBuffer(std::move(viewRasterInfoBuffer))
    , m_phase1VisibleClustersCounterBuffer(std::move(phase1VisibleClustersCounterBuffer))
    , m_swWriteBaseCounterBuffer(std::move(swWriteBaseCounterBuffer))
    , m_shadowDirtyHierarchyTexture(std::move(shadowDirtyHierarchyTexture))
    , m_shadowPredictiveInvalidationCandidatesBuffer(std::move(shadowPredictiveInvalidationCandidatesBuffer))
    , m_shadowPredictiveInvalidationCandidateCountBuffer(std::move(shadowPredictiveInvalidationCandidateCountBuffer))
    , m_shadowInvalidationCountBuffer(std::move(shadowInvalidationCountBuffer))
    , m_shadowInvalidatedInstancesBitsetBuffer(std::move(shadowInvalidatedInstancesBitsetBuffer))
    , m_shadowPageTableTexture(std::move(shadowPageTableTexture))
    , m_shadowPhysicalPagesTexture(std::move(shadowPhysicalPagesTexture))
    , m_shadowActiveBlockMetadataBuffer(std::move(shadowActiveBlockMetadataBuffer))
    , m_shadowReceiverSubpageMaskBuffer(std::move(shadowReceiverSubpageMaskBuffer))
    , m_shadowDynamicPhysicalPagesTexture(
        std::move(shadowDynamicPhysicalPagesTexture))
    , m_shadowDynamicActiveBlockMetadataBuffer(
        std::move(shadowDynamicActiveBlockMetadataBuffer))
    , m_slabResourceGroup(std::move(slabResourceGroup))
{
    m_isFirstPass = inputs.isFirstPass;
    m_maxVisibleClusters = inputs.maxVisibleClusters;
    m_workGraphMode = inputs.workGraphMode;
    m_rasterOutputKind = inputs.rasterOutputKind;
    const std::string passIdentifierForLog = stablePassIdentifier;
    m_workGraphComputePageJobDescriptorResourceId =
        std::string(CLodWorkGraphComputePageJobDescriptorBufferId) + "." + std::move(stablePassIdentifier);
    m_voxelRasterQueueDescriptorResourceId =
        std::string(CLodVoxelRasterQueueDescriptorBufferId) + "." + m_workGraphComputePageJobDescriptorResourceId;
    m_renderPhase = std::move(inputs.renderPhase);
    m_clodOnlyWorkloads = inputs.clodOnlyWorkloads;
    m_useShadowCascadeViews = inputs.useShadowCascadeViews;

    if (m_isFirstPass && UsesVirtualShadowOutput(m_rasterOutputKind) &&
        SettingsManager::GetInstance().getSettingGetter<bool>(
            CLodDynamicWindBoundsCacheEnabledSettingName)()) {
        constexpr uint64_t entryStride = 48u;
        const uint64_t budgetMiB = std::min<uint32_t>(
            SettingsManager::GetInstance().getSettingGetter<uint32_t>(
                CLodDynamicWindBoundsCacheMiBSettingName)(),
            256u);
        const uint64_t budgetBytes = budgetMiB * 1024u * 1024u;
        m_dynamicWindBoundsCacheEntryCount = static_cast<uint32_t>(budgetBytes / entryStride);
        if (m_dynamicWindBoundsCacheEntryCount != 0u) {
            m_dynamicWindBoundsCacheBuffer = CreateAliasedUnmaterializedRawBuffer(
                static_cast<uint64_t>(m_dynamicWindBoundsCacheEntryCount) * entryStride,
                true,
                false,
                true);
            m_dynamicWindBoundsCacheBuffer->SetName("CLod DynamicWind Meshlet Bounds Cache");
            org::memory::SetResourceUsageHint(*m_dynamicWindBoundsCacheBuffer, "DynamicWind VSM bounds cache");
        }
    }

    if (m_pageJobVisibleClustersBuffer && m_pageJobVisibleClusterTransformIndicesBuffer && m_pageJobVisibleClustersCounterBuffer) {
        m_workGraphComputePageJobDescriptorsBuffer = CreateAliasedUnmaterializedStructuredBuffer(
            1u,
            sizeof(CLodWorkGraphComputePageJobDescriptors),
            false,
            false,
            false,
            false);
        m_workGraphComputePageJobDescriptorsBuffer->SetName("CLod Pure Compute Page Job Descriptors");
        org::memory::SetResourceUsageHint(*m_workGraphComputePageJobDescriptorsBuffer, "Cluster LOD pure compute");
    }

    if (m_voxelRasterWorkCapacity != 0u) {
        m_voxelRasterQueueDescriptorsBuffer = CreateAliasedUnmaterializedStructuredBuffer(
            1u,
            sizeof(CLodVoxelRasterQueueDescriptors),
            false,
            false,
            false,
            false);
        m_voxelRasterQueueDescriptorsBuffer->SetName("CLod Pure Compute Voxel Raster Queue Descriptors");
        org::memory::SetResourceUsageHint(*m_voxelRasterQueueDescriptorsBuffer, "Cluster LOD voxel rasterization");
    }

    const uint32_t frontierCapacity = std::max(1u, m_maxVisibleClusters);
    const uint64_t nodeFrontierBytes = static_cast<uint64_t>(frontierCapacity) * CLodPureComputeNodeFrontierStrideBytes;
    const uint64_t clusterFrontierBytes = static_cast<uint64_t>(frontierCapacity) * CLodPureComputeClusterFrontierStrideBytes;
    spdlog::info(
        "CLod pure compute frontier allocation '{}': capacity={} nodeFrontier={} MiB each clusterFrontier={} MiB",
        passIdentifierForLog,
        frontierCapacity,
        static_cast<double>(nodeFrontierBytes) / (1024.0 * 1024.0),
        static_cast<double>(clusterFrontierBytes) / (1024.0 * 1024.0));
    m_pureComputeCurrentNodeFrontierBuffer = CreateAliasedUnmaterializedStructuredBuffer(frontierCapacity, CLodPureComputeNodeFrontierStrideBytes, true, false, false, true);
    m_pureComputeCurrentNodeFrontierBuffer->SetName("CLod Pure Compute Current Node Frontier");
    org::memory::SetResourceUsageHint(*m_pureComputeCurrentNodeFrontierBuffer, "Cluster LOD pure compute frontiers");
    m_pureComputeNextNodeFrontierBuffer = CreateAliasedUnmaterializedStructuredBuffer(frontierCapacity, CLodPureComputeNodeFrontierStrideBytes, true, false, false, true);
    m_pureComputeNextNodeFrontierBuffer->SetName("CLod Pure Compute Next Node Frontier");
    org::memory::SetResourceUsageHint(*m_pureComputeNextNodeFrontierBuffer, "Cluster LOD pure compute frontiers");
    m_pureComputeCurrentLeafFrontierBuffer = CreateAliasedUnmaterializedStructuredBuffer(frontierCapacity, CLodPureComputeNodeFrontierStrideBytes, true, false, false, true);
    m_pureComputeCurrentLeafFrontierBuffer->SetName("CLod Pure Compute Current Leaf Frontier");
    org::memory::SetResourceUsageHint(*m_pureComputeCurrentLeafFrontierBuffer, "Cluster LOD pure compute frontiers");
    m_pureComputeNextLeafFrontierBuffer = CreateAliasedUnmaterializedStructuredBuffer(frontierCapacity, CLodPureComputeNodeFrontierStrideBytes, true, false, false, true);
    m_pureComputeNextLeafFrontierBuffer->SetName("CLod Pure Compute Next Leaf Frontier");
    org::memory::SetResourceUsageHint(*m_pureComputeNextLeafFrontierBuffer, "Cluster LOD pure compute frontiers");
    m_pureComputeClusterFrontierBuffer = CreateAliasedUnmaterializedStructuredBuffer(frontierCapacity, CLodPureComputeClusterFrontierStrideBytes, true, false, false, true);
    m_pureComputeClusterFrontierBuffer->SetName("CLod Pure Compute Cluster Frontier");
    org::memory::SetResourceUsageHint(*m_pureComputeClusterFrontierBuffer, "Cluster LOD pure compute frontiers");
    m_pureComputeCurrentNodeCounterBuffer = CreateAliasedUnmaterializedStructuredBuffer(1u, sizeof(uint32_t), true, false, false, false);
    m_pureComputeCurrentNodeCounterBuffer->SetName("CLod Pure Compute Current Node Counter");
    org::memory::SetResourceUsageHint(*m_pureComputeCurrentNodeCounterBuffer, "Cluster LOD pure compute");
    m_pureComputeNextNodeCounterBuffer = CreateAliasedUnmaterializedStructuredBuffer(1u, sizeof(uint32_t), true, false, false, false);
    m_pureComputeNextNodeCounterBuffer->SetName("CLod Pure Compute Next Node Counter");
    org::memory::SetResourceUsageHint(*m_pureComputeNextNodeCounterBuffer, "Cluster LOD pure compute");
    m_pureComputeCurrentLeafCounterBuffer = CreateAliasedUnmaterializedStructuredBuffer(1u, sizeof(uint32_t), true, false, false, false);
    m_pureComputeCurrentLeafCounterBuffer->SetName("CLod Pure Compute Current Leaf Counter");
    org::memory::SetResourceUsageHint(*m_pureComputeCurrentLeafCounterBuffer, "Cluster LOD pure compute");
    m_pureComputeNextLeafCounterBuffer = CreateAliasedUnmaterializedStructuredBuffer(1u, sizeof(uint32_t), true, false, false, false);
    m_pureComputeNextLeafCounterBuffer->SetName("CLod Pure Compute Next Leaf Counter");
    org::memory::SetResourceUsageHint(*m_pureComputeNextLeafCounterBuffer, "Cluster LOD pure compute");
    m_pureComputeClusterCounterBuffer = CreateAliasedUnmaterializedStructuredBuffer(1u, sizeof(uint32_t), true, false, false, false);
    m_pureComputeClusterCounterBuffer->SetName("CLod Pure Compute Cluster Counter");
    org::memory::SetResourceUsageHint(*m_pureComputeClusterCounterBuffer, "Cluster LOD pure compute");
    m_pureComputeNodeDispatchArgsBuffer = CreateAliasedUnmaterializedStructuredBuffer(1u, sizeof(PureComputeDispatchCommand), true, false, false, false);
    m_pureComputeNodeDispatchArgsBuffer->SetName("CLod Pure Compute Node Dispatch Args");
    org::memory::SetResourceUsageHint(*m_pureComputeNodeDispatchArgsBuffer, "Cluster LOD pure compute");
    m_pureComputeLeafDispatchArgsBuffer = CreateAliasedUnmaterializedStructuredBuffer(1u, sizeof(PureComputeDispatchCommand), true, false, false, false);
    m_pureComputeLeafDispatchArgsBuffer->SetName("CLod Pure Compute Leaf Dispatch Args");
    org::memory::SetResourceUsageHint(*m_pureComputeLeafDispatchArgsBuffer, "Cluster LOD pure compute");
    m_pureComputeClusterDispatchArgsBuffer = CreateAliasedUnmaterializedStructuredBuffer(1u, sizeof(PureComputeDispatchCommand), true, false, false, false);
    m_pureComputeClusterDispatchArgsBuffer->SetName("CLod Pure Compute Cluster Dispatch Args");
    org::memory::SetResourceUsageHint(*m_pureComputeClusterDispatchArgsBuffer, "Cluster LOD pure compute");

    const bool enableSharedSWClassificationPath =
        UsesSWClassification(m_workGraphMode) || m_workGraphComputePageJobDescriptorsBuffer != nullptr;
    const bool enableComputePageJobDescriptorBuffer =
        m_workGraphComputePageJobDescriptorsBuffer != nullptr;
    std::wstring pageJobDescriptorResourceIdWide(
        m_workGraphComputePageJobDescriptorResourceId.begin(),
        m_workGraphComputePageJobDescriptorResourceId.end());
    std::wstring pageJobDescriptorResourceIdDefine = L"\"" + pageJobDescriptorResourceIdWide + L"\"";
    std::wstring voxelQueueDescriptorResourceIdWide(
        m_voxelRasterQueueDescriptorResourceId.begin(),
        m_voxelRasterQueueDescriptorResourceId.end());
    std::wstring voxelQueueDescriptorResourceIdDefine = L"\"" + voxelQueueDescriptorResourceIdWide + L"\"";
    std::vector<DxcDefine> pureComputeDefines = {
        { L"CLOD_WG_ENABLE_SW_CLASSIFICATION", enableSharedSWClassificationPath ? L"1" : L"0" },
        { L"CLOD_WG_ENABLE_SW_NODE_OUTPUT", L"0" },
        { L"CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW", UsesVirtualShadowOutput(m_rasterOutputKind) ? L"1" : L"0" },
        { L"CLOD_VSM_TWO_LAYER_COMPUTE_CULL_VERSION", UsesVirtualShadowOutput(m_rasterOutputKind) ? L"2" : L"0" },
        { L"CLOD_WG_ENABLE_COMPUTE_PAGE_JOB_DESCRIPTOR_BUFFER", enableComputePageJobDescriptorBuffer ? L"1" : L"0" },
        { L"CLOD_WG_ENABLE_VOXEL_OUTPUT", m_voxelRasterWorkCapacity != 0u ? L"1" : L"0" },
        { L"CLOD_COMPUTE_INCLUDE_ONLY", L"1" },
        { L"CLOD_WG_VOXEL_RASTER_QUEUE_DESCRIPTOR_BUFFER_ID", voxelQueueDescriptorResourceIdDefine.c_str() },
    };
    if (enableComputePageJobDescriptorBuffer) {
        pureComputeDefines.push_back({ L"CLOD_WG_COMPUTE_PAGE_JOB_DESCRIPTOR_BUFFER_ID", pageJobDescriptorResourceIdDefine.c_str() });
    }
    std::vector<DxcDefine> pureComputeLeafDefines = pureComputeDefines;
    pureComputeLeafDefines.push_back({ L"CLOD_PC_LEAF_ONLY", L"1" });

    auto& psoManager = PSOManager::GetInstance();
    const auto computeLayout = psoManager.GetComputeRootSignature().GetHandle();
    m_clearPipelineState = psoManager.MakeComputePipeline(
        computeLayout,
        L"shaders/ClusterLOD/clodUtil.hlsl",
        L"ClearUintStructuredBufferCSMain",
        {},
        "HierarchicalDispatchCulling.ClearUint");
    m_createCommandPipelineState = psoManager.MakeComputePipeline(
        computeLayout,
        L"shaders/ClusterLOD/clodUtil.hlsl",
        L"CreateRasterBucketsHistogramCommandCSMain",
        {},
        "HierarchicalDispatchCulling.CreateRasterBucketsHistogramCommand");
    m_pureComputeBuildDispatchArgsPipelineState = psoManager.MakeComputePipeline(
        computeLayout,
        L"shaders/ClusterLOD/computeCulling.hlsl",
        L"BuildPureComputeDispatchArgsCS",
        pureComputeDefines,
        "CLod.PureCompute.BuildDispatchArgs");
    m_pureComputeBuildDualDispatchArgsPipelineState = psoManager.MakeComputePipeline(
        computeLayout,
        L"shaders/ClusterLOD/computeCulling.hlsl",
        L"BuildPureComputeDualDispatchArgsCS",
        pureComputeDefines,
        "CLod.PureCompute.BuildDualDispatchArgs");
    m_pureComputeClearTraversalCountersPipelineState = psoManager.MakeComputePipeline(
        computeLayout,
        L"shaders/ClusterLOD/computeCulling.hlsl",
        L"ClearPureComputeTraversalCountersCS",
        pureComputeDefines,
        "CLod.PureCompute.ClearTraversalCounters");
    m_pureComputeBuildReplayDispatchArgsPipelineState = psoManager.MakeComputePipeline(
        computeLayout,
        L"shaders/ClusterLOD/computeCulling.hlsl",
        L"BuildPureComputeReplayDispatchArgsCS",
        pureComputeDefines,
        "CLod.PureCompute.BuildReplayDispatchArgs");
    m_pureComputeObjectCullPipelineState = psoManager.MakeComputePipeline(
        computeLayout,
        L"shaders/ClusterLOD/computeCulling.hlsl",
        L"PureComputeObjectCullCS",
        pureComputeDefines,
        "CLod.PureCompute.ObjectCull");
    m_pureComputeReplayNodesPipelineState = psoManager.MakeComputePipeline(
        computeLayout,
        L"shaders/ClusterLOD/computeCulling.hlsl",
        L"SeedPureComputeReplayNodesCS",
        pureComputeDefines,
        "CLod.PureCompute.SeedReplayNodes");
    m_pureComputeReplayClustersPipelineState = psoManager.MakeComputePipeline(
        computeLayout,
        L"shaders/ClusterLOD/computeCulling.hlsl",
        L"SeedPureComputeReplayClustersCS",
        pureComputeDefines,
        "CLod.PureCompute.SeedReplayClusters");
    m_pureComputeTraversePipelineState = psoManager.MakeComputePipeline(
        computeLayout,
        L"shaders/ClusterLOD/computeCulling.hlsl",
        L"PureComputeTraverseFrontierCS",
        pureComputeDefines,
        "CLod.PureCompute.Traverse");
    m_pureComputeLeafPipelineState = psoManager.MakeComputePipeline(
        computeLayout,
        L"shaders/ClusterLOD/computeCulling.hlsl",
        L"PureComputeTraverseFrontierCS",
        pureComputeLeafDefines,
        "CLod.PureCompute.Leaf");
    m_pureComputeClusterPipelineState = psoManager.MakeComputePipeline(
        computeLayout,
        L"shaders/ClusterLOD/computeCulling.hlsl",
        L"PureComputeClusterFrontierCS",
        pureComputeDefines,
        "CLod.PureCompute.ClusterCull");
    m_pureComputeDenseClusterPipelineState = psoManager.MakeComputePipeline(
        computeLayout,
        L"shaders/ClusterLOD/computeCulling.hlsl",
        L"PureComputeDenseClusterWorkCS",
        pureComputeDefines,
        "CLod.PureCompute.DenseClusterCull");

    const ResourceIdentifier voxelDescriptorId{ m_voxelRasterQueueDescriptorResourceId };
    auto validateVoxelVariant = [&](const char* pipelineName, const PipelineState& pipeline, bool expectsVoxelDescriptor) {
        const auto& resources = pipeline.GetResourceDescriptorSlots();
        const bool requestsVoxelDescriptor = std::ranges::find(
            resources.mandatoryResourceDescriptorSlots,
            voxelDescriptorId) != resources.mandatoryResourceDescriptorSlots.end();
        if (requestsVoxelDescriptor != expectsVoxelDescriptor) {
            throw std::runtime_error(
                std::string("CLod pure-compute shader voxel-output variant '") + pipelineName +
                "' has an inconsistent resource interface (expected=" +
                (expectsVoxelDescriptor ? "true" : "false") + ", reflected=" +
                (requestsVoxelDescriptor ? "true" : "false") + ")");
        }
    };
    validateVoxelVariant("object-cull", m_pureComputeObjectCullPipelineState, false);
    validateVoxelVariant("traverse", m_pureComputeTraversePipelineState, m_voxelRasterWorkCapacity != 0u);
    validateVoxelVariant("leaf", m_pureComputeLeafPipelineState, m_voxelRasterWorkCapacity != 0u);
    validateVoxelVariant("cluster", m_pureComputeClusterPipelineState, false);
    validateVoxelVariant("dense-cluster", m_pureComputeDenseClusterPipelineState, false);

    rhi::IndirectArg dispatchArg[] = {
        {.kind = rhi::IndirectArgKind::Dispatch }
    };
    rhi::CommandSignaturePtr dispatchSignature;
    DeviceManager::GetInstance().GetDevice().CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(dispatchArg, 1), sizeof(PureComputeDispatchCommand) },
        computeLayout,
        dispatchSignature);
    m_pureComputeDispatchCommandSignature =
        std::make_shared<rhi::CommandSignaturePtr>(std::move(dispatchSignature));
}

HierarchicalDispatchCullingPass::~HierarchicalDispatchCullingPass() = default;

void HierarchicalDispatchCullingPass::Declare(org::PassBuilder& builder)
{
    const ResourceState computeReadState{
        rhi::ResourceAccessType::ShaderResource,
        rhi::ResourceLayout::ShaderResource,
        rhi::ResourceSyncState::ComputeShading
    };
    const ResourceState indirectState{
        rhi::ResourceAccessType::IndirectArgument,
        rhi::ResourceLayout::GenericRead,
        rhi::ResourceSyncState::ExecuteIndirect
    };

    br::render::PublishedResourceQuery drawSetIndicesQuery{};
    drawSetIndicesQuery.owner = br::render::PublishedFragmentKind::ActiveDrawLists;
    drawSetIndicesQuery.usage = br::render::PublishedResourceUsage::ActiveDrawList;
    drawSetIndicesQuery.renderPhaseHash = m_renderPhase.hash;
    constexpr std::uint64_t clodBit = 1ull << 63u;
    if (m_clodOnlyWorkloads) drawSetIndicesQuery.requiredVariantMask = clodBit;
    else drawSetIndicesQuery.forbiddenVariantMask = clodBit;

    br::render::PublishedResourceQuery visibilityGenerationQuery{};
    visibilityGenerationQuery.owner = br::render::PublishedFragmentKind::DrawRecords;
    visibilityGenerationQuery.usage = br::render::PublishedResourceUsage::ShaderResource;
    visibilityGenerationQuery.requiredVariantMask =
        br::render::kObjectVisibilityGenerationVariant;

    builder.WithUnorderedAccess(
            m_visibleClustersBuffer,
            m_visibleClusterTransformIndicesBuffer,
            m_visibleClustersCounterBuffer,
            m_histogramIndirectCommand,
            m_workGraphTelemetryBuffer,
            m_occlusionReplayBuffer,
            m_occlusionReplayStateBuffer,
            m_pureComputeCurrentNodeFrontierBuffer,
            m_pureComputeNextNodeFrontierBuffer,
            m_pureComputeCurrentLeafFrontierBuffer,
            m_pureComputeNextLeafFrontierBuffer,
            m_pureComputeClusterFrontierBuffer,
            m_pureComputeCurrentNodeCounterBuffer,
            m_pureComputeNextNodeCounterBuffer,
            m_pureComputeCurrentLeafCounterBuffer,
            m_pureComputeNextLeafCounterBuffer,
            m_pureComputeClusterCounterBuffer,
            m_pureComputeNodeDispatchArgsBuffer,
            m_pureComputeLeafDispatchArgsBuffer,
            m_pureComputeClusterDispatchArgsBuffer,
            Builtin::CLod::StreamingLoadRequestKeys,
            Builtin::CLod::StreamingLoadRequests,
            Builtin::CLod::StreamingLoadCounter,
            Builtin::CLod::StreamingTouchedGroupsCounter,
            Builtin::CLod::StreamingTouchedGroups)
        .WithShaderResource(
            Builtin::IndirectCommandBuffers::Master,
            Builtin::CLod::Offsets,
            Builtin::CLod::Groups,
            Builtin::CLod::Segments,
            Builtin::CLod::Nodes,
			Builtin::CLod::NodeSkinningInfos,
			Builtin::CLod::NodeBoneIndices,
            Builtin::CLod::AssemblyInstances,
            Builtin::CLod::AssemblyTransforms,
            Builtin::CLod::AssemblyBoneRemaps,
            Builtin::CLod::AssemblyBoneRemapIndices,
            Builtin::CLod::StreamingNonResidentBits,
            Builtin::CLod::MeshMetadata,
            CLodLevelInfosBufferId,
            Builtin::CLod::GroupPageMap,
            Builtin::CLod::StreamingRuntimeState,
            Builtin::CullingCameraBuffer,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::SkinnedAssemblyPlacements,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerObjectBuffer,
            Builtin::CameraBuffer,
            Builtin::PerMeshBuffer,
            Builtin::PerMaterialDataBuffer,
            Builtin::Material::TextureStreamingMetadataBuffer,
            Builtin::SkeletonResources::InverseBindMatrices,
            Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo,
            m_viewRasterInfoBuffer)
        .WithUnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer)
        .WithShaderResource(PublishedStateResourceResolver(
            br::render::PublishedStateSource::ProcessSource(), drawSetIndicesQuery))
        .WithShaderResource(PublishedStateResourceResolver(
            br::render::PublishedStateSource::ProcessSource(), visibilityGenerationQuery))
        .WithInternalTransition(m_visibleClustersCounterBuffer, computeReadState)
        .WithInternalTransition(m_occlusionReplayStateBuffer, computeReadState)
        .WithInternalTransition(m_pureComputeCurrentNodeFrontierBuffer, computeReadState)
        .WithInternalTransition(m_pureComputeCurrentNodeCounterBuffer, computeReadState)
        .WithInternalTransition(m_pureComputeCurrentLeafFrontierBuffer, computeReadState)
        .WithInternalTransition(m_pureComputeCurrentLeafCounterBuffer, computeReadState);

    if (m_voxelRasterWorkCapacity != 0u) {
        builder.WithUnorderedAccess(
                m_voxelRasterWorkBuffer,
                m_voxelRasterWorkCounterBuffer,
                m_skinnedVoxelRasterWorkBuffer,
                m_skinnedVoxelRasterWorkCounterBuffer)
            .WithShaderResource(m_voxelRasterQueueDescriptorResourceId.c_str());
    }

    const uint32_t traversalLevelCount = std::min(m_activeTraversalDepth, kPureComputeMaxTraversalLevels);
    if (!m_isFirstPass || traversalLevelCount > 0u) {
        builder.WithInternalTransition(m_pureComputeNodeDispatchArgsBuffer, indirectState)
            .WithInternalTransition(m_pureComputeLeafDispatchArgsBuffer, indirectState)
            .WithInternalTransition(m_pureComputeClusterDispatchArgsBuffer, indirectState);
    }
    if (traversalLevelCount > 0u) {
        builder.WithInternalTransition(m_pureComputeNextNodeFrontierBuffer, computeReadState)
            .WithInternalTransition(m_pureComputeNextNodeCounterBuffer, computeReadState)
            .WithInternalTransition(m_pureComputeNextLeafFrontierBuffer, computeReadState)
            .WithInternalTransition(m_pureComputeNextLeafCounterBuffer, computeReadState);
    }

    if (UsesSWClassification(m_workGraphMode) && m_swVisibleClustersCounterBuffer) {
        builder.WithUnorderedAccess(m_swVisibleClustersCounterBuffer);
    }

    if (m_workGraphComputePageJobDescriptorsBuffer) {
        builder.WithShaderResource(m_workGraphComputePageJobDescriptorResourceId.c_str());
    }

    if (m_pageJobVisibleClustersBuffer && m_pageJobVisibleClusterTransformIndicesBuffer && m_pageJobVisibleClustersCounterBuffer) {
        builder.WithUnorderedAccess(
            m_pageJobVisibleClustersBuffer,
            m_pageJobVisibleClusterTransformIndicesBuffer,
            m_pageJobVisibleClustersCounterBuffer);
    }

    if (m_slabResourceGroup) {
        builder.WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
    }

    if (m_dynamicWindBoundsCacheBuffer) {
        builder.WithUnorderedAccess(m_dynamicWindBoundsCacheBuffer);
        // This read both enforces SimulateInstancesPhase2 -> shadow traversal
        // ordering and limits caching to placements accepted by DynamicWind.
        m_dynamicWindVisibleMembershipBuffer = m_resourceRegistryView
            ->RequestPtr<GloballyIndexedResource>("Builtin::DynamicWind::VisibleSkeletonMembership");
        builder.WithShaderResource("Builtin::DynamicWind::VisibleSkeletonMembership");
    }

    if (UsesVirtualShadowOutput(m_rasterOutputKind)) {
        builder.WithShaderResource(
            Builtin::Shadows::CLodClipmapInfo,
            Builtin::Shadows::CLodDirectionalPageViewInfo,
            Builtin::Shadows::CLodCompactShadowCameras);
        if (m_shadowDirtyHierarchyTexture) {
            builder.WithShaderResource(m_shadowDirtyHierarchyTexture);
        }
        if (m_shadowInvalidatedInstancesBitsetBuffer) {
            builder.WithShaderResource(m_shadowInvalidatedInstancesBitsetBuffer);
        }
        if (m_shadowInvalidationCountBuffer) {
            builder.WithShaderResource(m_shadowInvalidationCountBuffer);
        }
        if (m_shadowPredictiveInvalidationCandidatesBuffer) {
            builder.WithUnorderedAccess(m_shadowPredictiveInvalidationCandidatesBuffer);
        }
        if (m_shadowPredictiveInvalidationCandidateCountBuffer) {
            builder.WithUnorderedAccess(m_shadowPredictiveInvalidationCandidateCountBuffer);
        }
        if (m_shadowPageTableTexture) {
            builder.WithUnorderedAccess(m_shadowPageTableTexture);
        }
        if (m_shadowPhysicalPagesTexture) {
            builder.WithUnorderedAccess(m_shadowPhysicalPagesTexture);
        }
        if (m_shadowDynamicPhysicalPagesTexture) {
            builder.WithUnorderedAccess(m_shadowDynamicPhysicalPagesTexture);
        }
        if (m_shadowActiveBlockMetadataBuffer) {
            builder.WithShaderResource(m_shadowActiveBlockMetadataBuffer);
        }
        if (m_shadowReceiverSubpageMaskBuffer) {
            builder.WithShaderResource(m_shadowReceiverSubpageMaskBuffer);
        }
        if (m_shadowDynamicActiveBlockMetadataBuffer) {
            builder.WithShaderResource(
                m_shadowDynamicActiveBlockMetadataBuffer);
        }
    }

    if (UsesPerViewDepthMapOcclusion(m_rasterOutputKind)) {
        builder.WithUnorderedAccess(m_viewDepthSrvIndicesBuffer)
            .WithShaderResource(Builtin::PrimaryCamera::LinearDepthMap);
    }

    if (m_phase1VisibleClustersCounterBuffer && !m_isFirstPass) {
        builder.WithShaderResource(m_phase1VisibleClustersCounterBuffer);
    }
    if (m_swWriteBaseCounterBuffer && !m_isFirstPass) {
        builder.WithShaderResource(m_swWriteBaseCounterBuffer);
    }

    const auto uavIndex = [&](const auto& resource) {
        return builder.DeclaredBindlessIndex(resource,
            {org::BindlessViewKind::UnorderedAccess});
    };
    if (m_workGraphComputePageJobDescriptorsBuffer) {
        CLodWorkGraphComputePageJobDescriptors descriptors{};
        descriptors.visibleClustersUAVDescriptorIndex = uavIndex(m_pageJobVisibleClustersBuffer);
        descriptors.visibleClustersCounterUAVDescriptorIndex = uavIndex(m_pageJobVisibleClustersCounterBuffer);
        descriptors.visibleClusterTransformIndicesUAVDescriptorIndex =
            uavIndex(m_pageJobVisibleClusterTransformIndicesBuffer);
        m_cachedPageJobDescriptors = descriptors;
        m_hasCachedPageJobDescriptors = true;
    }
    if (m_voxelRasterWorkCapacity != 0u) {
        CLodVoxelRasterQueueDescriptors descriptors{};
        descriptors.rigidWorkRecordsUAVDescriptorIndex = uavIndex(m_voxelRasterWorkBuffer);
        descriptors.rigidWorkRecordCounterUAVDescriptorIndex = uavIndex(m_voxelRasterWorkCounterBuffer);
        descriptors.skinnedWorkRecordsUAVDescriptorIndex = uavIndex(m_skinnedVoxelRasterWorkBuffer);
        descriptors.skinnedWorkRecordCounterUAVDescriptorIndex = uavIndex(m_skinnedVoxelRasterWorkCounterBuffer);
        descriptors.workRecordCapacity = m_voxelRasterWorkCapacity;
        m_cachedVoxelQueueDescriptors = descriptors;
        m_hasCachedVoxelQueueDescriptors = true;
    }

    builder.WithConstantBuffer(Builtin::PerFrameBuffer);
}

void HierarchicalDispatchCullingPass::Initialize()
{
	// Pure-compute traversal uses the same bindless node-bounds sidecars as the
	// work graph. Keep explicit registrations in addition to graph declarations.
	RegisterSRV(Builtin::CLod::NodeSkinningInfos);
	RegisterSRV(Builtin::CLod::NodeBoneIndices);
}

template<class EmissionData, class CommandSink>
PassReturn HierarchicalDispatchCullingPass::EmitCommands(
    const EmissionData& inputs, CommandSink& commandList, const HierarchicalDispatchCullingCommandConfiguration& configuration)
{
    commandList.BindLayout(configuration.layout);

    if (inputs.m_isFirstPass && configuration.telemetry) {
        commandList.BindDescriptorIndices(inputs.m_clearPipelineState.GetResourceDescriptorSlots());
        commandList.BindPipeline(inputs.m_clearPipelineState.GetAPIPipelineState().GetHandle());

        typename CommandSink::Constant clearRootConstants[NumMiscUintRootConstants] = {};
        clearRootConstants[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] =
            commandList.UAVIndex(inputs.m_workGraphTelemetryBuffer);
        clearRootConstants[CLOD_CLEAR_UINT_BUFFER_VALUE] = 0u;
        clearRootConstants[CLOD_CLEAR_UINT_BUFFER_COUNT] = CLodWorkGraphTelemetryBufferCount;
        commandList.PushConstants(
            rhi::ShaderStage::Compute,
            0,
            MiscUintRootSignatureIndex,
            0,
            NumMiscUintRootConstants,
            clearRootConstants);
        commandList.Dispatch((CLodWorkGraphTelemetryBufferCount + 63u) / 64u, 1u, 1u);

        rhi::BufferBarrier telemetryBarrier{};
        telemetryBarrier.buffer = inputs.m_workGraphTelemetryBuffer->GetAPIResource().GetHandle();
        telemetryBarrier.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
        telemetryBarrier.afterAccess = rhi::ResourceAccessType::UnorderedAccess;
        telemetryBarrier.beforeSync = rhi::ResourceSyncState::ComputeShading;
        telemetryBarrier.afterSync = rhi::ResourceSyncState::ComputeShading;
        rhi::BarrierBatch telemetryBarrierBatch{};
        telemetryBarrierBatch.buffers = { &telemetryBarrier };
        commandList.Barriers(telemetryBarrierBatch);
    }

    if (inputs.m_pageJobVisibleClustersCounterBuffer) {
        commandList.BindDescriptorIndices(inputs.m_clearPipelineState.GetResourceDescriptorSlots());
        commandList.BindPipeline(inputs.m_clearPipelineState.GetAPIPipelineState().GetHandle());

        typename CommandSink::Constant clearRootConstants[NumMiscUintRootConstants] = {};
        clearRootConstants[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] =
            commandList.UAVIndex(inputs.m_pageJobVisibleClustersCounterBuffer);
        clearRootConstants[CLOD_CLEAR_UINT_BUFFER_VALUE] = 0u;
        clearRootConstants[CLOD_CLEAR_UINT_BUFFER_COUNT] = 1u;
        commandList.PushConstants(
            rhi::ShaderStage::Compute,
            0,
            MiscUintRootSignatureIndex,
            0,
            NumMiscUintRootConstants,
            clearRootConstants);
        commandList.Dispatch(1u, 1u, 1u);

        rhi::BufferBarrier pageJobCounterBarrier{};
        pageJobCounterBarrier.buffer = inputs.m_pageJobVisibleClustersCounterBuffer->GetAPIResource().GetHandle();
        pageJobCounterBarrier.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
        pageJobCounterBarrier.afterAccess = rhi::ResourceAccessType::UnorderedAccess;
        pageJobCounterBarrier.beforeSync = rhi::ResourceSyncState::ComputeShading;
        pageJobCounterBarrier.afterSync = rhi::ResourceSyncState::ComputeShading;

        rhi::BarrierBatch clearBarrierBatch{};
        clearBarrierBatch.buffers = { &pageJobCounterBarrier };
        commandList.Barriers(clearBarrierBatch);
    }

    const uint32_t phase2ExpansionFactor = configuration.expansionFactor;
    const uint32_t phase2RecordsPerGroup = 64u / phase2ExpansionFactor;

    typename CommandSink::Constant sharedRootConstants[NumMiscUintRootConstants] = {};
    sharedRootConstants[CLOD_WG_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = commandList.UAVIndex(inputs.m_visibleClustersBuffer);
    sharedRootConstants[CLOD_WG_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = commandList.UAVIndex(inputs.m_visibleClusterTransformIndicesBuffer);
    sharedRootConstants[CLOD_WG_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = commandList.UAVIndex(inputs.m_visibleClustersCounterBuffer);
    sharedRootConstants[CLOD_WG_FORCED_TRAVERSAL_DEPTH_ROOT] =
        configuration.forcedTraversalDepth;
    sharedRootConstants[CLOD_WG_SW_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] =
        inputs.m_swVisibleClustersCounterBuffer
            ? commandList.UAVIndex(inputs.m_swVisibleClustersCounterBuffer)
            : 0u;
    sharedRootConstants[CLOD_WG_TELEMETRY_DESCRIPTOR_INDEX] = commandList.UAVIndex(inputs.m_workGraphTelemetryBuffer);
    sharedRootConstants[CLOD_WG_OCCLUSION_REPLAY_BUFFER_DESCRIPTOR_INDEX] = commandList.UAVIndex(inputs.m_occlusionReplayBuffer);
    sharedRootConstants[CLOD_WG_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX] = commandList.UAVIndex(inputs.m_occlusionReplayStateBuffer);
    sharedRootConstants[CLOD_WG_WORKGRAPH_NODE_INPUTS_DESCRIPTOR_INDEX] = 0u;
    sharedRootConstants[CLOD_WG_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX] = commandList.SRVIndex(inputs.m_viewRasterInfoBuffer);
    sharedRootConstants[CLOD_WG_VIEW_DEPTH_SRV_INDICES_DESCRIPTOR_INDEX] =
        UsesPerViewDepthMapOcclusion(inputs.m_rasterOutputKind)
            ? commandList.SRVIndex(inputs.m_viewDepthSrvIndicesBuffer)
            : 0u;
    sharedRootConstants[CLOD_WG_VISIBLE_CLUSTERS_CAPACITY] = static_cast<uint32_t>(inputs.m_maxVisibleClusters);
    sharedRootConstants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_DESCRIPTOR_INDEX] =
        inputs.m_dynamicWindBoundsCacheBuffer
            ? commandList.UAVIndex(inputs.m_dynamicWindBoundsCacheBuffer)
            : 0u;
    sharedRootConstants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_ENTRY_COUNT] = inputs.m_dynamicWindBoundsCacheEntryCount;
    sharedRootConstants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_GENERATION] = inputs.m_dynamicWindBoundsCacheGeneration;
    sharedRootConstants[CLOD_WG_DYNAMIC_WIND_VISIBLE_MEMBERSHIP_DESCRIPTOR_INDEX] =
        inputs.m_dynamicWindBoundsCacheBuffer
            ? commandList.SRVIndex(inputs.m_dynamicWindVisibleMembershipBuffer)
            : 0u;
    if constexpr (std::is_same_v<typename CommandSink::Constant,uint32_t>) {
    if (inputs.m_dynamicWindBoundsCacheBuffer && inputs.m_dynamicWindBoundsCacheGeneration == 1u) {
        spdlog::info(
            "CLod DynamicWind bounds cache bindings: entries={} cacheDescriptor={} membershipDescriptor={}",
            inputs.m_dynamicWindBoundsCacheEntryCount,
            sharedRootConstants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_DESCRIPTOR_INDEX],
            sharedRootConstants[CLOD_WG_DYNAMIC_WIND_VISIBLE_MEMBERSHIP_DESCRIPTOR_INDEX]);
    }
    }
    sharedRootConstants[CLOD_WG_SHADOW_DIRTY_HIERARCHY_DESCRIPTOR_INDEX] =
        inputs.m_shadowDirtyHierarchyTexture
            ? commandList.SRVIndex(inputs.m_shadowDirtyHierarchyTexture, static_cast<uint32_t>(SRVViewType::Texture2DArrayFull))
            : 0u;
    sharedRootConstants[CLOD_WG_SHADOW_INVALIDATED_INSTANCES_DESCRIPTOR_INDEX] =
        inputs.m_shadowInvalidatedInstancesBitsetBuffer
            ? commandList.SRVIndex(inputs.m_shadowInvalidatedInstancesBitsetBuffer)
            : 0u;
    sharedRootConstants[CLOD_WG_SHADOW_PREDICTIVE_INVALIDATION_CANDIDATES_DESCRIPTOR_INDEX] =
        inputs.m_shadowPredictiveInvalidationCandidatesBuffer
            ? commandList.UAVIndex(inputs.m_shadowPredictiveInvalidationCandidatesBuffer)
            : 0u;
    sharedRootConstants[CLOD_WG_SHADOW_PREDICTIVE_INVALIDATION_CANDIDATE_COUNT_DESCRIPTOR_INDEX] =
        inputs.m_shadowPredictiveInvalidationCandidateCountBuffer
            ? commandList.UAVIndex(inputs.m_shadowPredictiveInvalidationCandidateCountBuffer)
            : 0u;
    sharedRootConstants[CLOD_WG_PAGE_JOB_FLAGS] = configuration.pageJobFlags;
    sharedRootConstants[CLOD_WG_VIRTUAL_SHADOW_PAGE_TABLE_UAV_DESCRIPTOR_INDEX] =
        inputs.m_shadowPageTableTexture
            ? commandList.UAVIndex(inputs.m_shadowPageTableTexture, static_cast<uint32_t>(UAVViewType::Texture2DArrayFull))
            : 0u;
    sharedRootConstants[CLOD_WG_VIRTUAL_SHADOW_PHYSICAL_PAGES_UAV_DESCRIPTOR_INDEX] =
        inputs.m_shadowPhysicalPagesTexture
            ? commandList.UAVIndex(inputs.m_shadowPhysicalPagesTexture)
            : 0u;
    sharedRootConstants[CLOD_WG_VIRTUAL_SHADOW_ACTIVE_BLOCK_METADATA_DESCRIPTOR_INDEX] =
        inputs.m_shadowActiveBlockMetadataBuffer
            ? commandList.SRVIndex(inputs.m_shadowActiveBlockMetadataBuffer)
            : 0u;
    sharedRootConstants[CLOD_WG_VIRTUAL_SHADOW_RECEIVER_MASK_DESCRIPTOR_INDEX] =
        inputs.m_shadowReceiverSubpageMaskBuffer
            ? commandList.SRVIndex(inputs.m_shadowReceiverSubpageMaskBuffer)
            : 0u;
    const uint32_t receiverSubpageMode = configuration.receiverSubpageMode;
    sharedRootConstants[CLOD_WG_VIRTUAL_SHADOW_RECEIVER_MASK_MODE] = receiverSubpageMode;
    sharedRootConstants[
        CLOD_WG_VIRTUAL_SHADOW_DYNAMIC_PAGES_UAV_DESCRIPTOR_INDEX] =
        inputs.m_shadowDynamicPhysicalPagesTexture
            ? commandList.UAVIndex(inputs.m_shadowDynamicPhysicalPagesTexture)
            : 0u;
    sharedRootConstants[
        CLOD_WG_VIRTUAL_SHADOW_DYNAMIC_ACTIVE_BLOCK_METADATA_DESCRIPTOR_INDEX] =
        inputs.m_shadowDynamicActiveBlockMetadataBuffer
            ? commandList.SRVIndex(inputs.m_shadowDynamicActiveBlockMetadataBuffer)
            : 0u;
    sharedRootConstants[CLOD_WG_HW_WRITE_BASE_COUNTER_DESCRIPTOR_INDEX] =
        commandList.SRVIndex(inputs.m_phase1VisibleClustersCounterBuffer
            ? inputs.m_phase1VisibleClustersCounterBuffer : inputs.m_visibleClustersCounterBuffer);
    sharedRootConstants[CLOD_WG_SW_WRITE_BASE_COUNTER_DESCRIPTOR_INDEX] =
        commandList.SRVIndex(inputs.m_swWriteBaseCounterBuffer
            ? inputs.m_swWriteBaseCounterBuffer : inputs.m_swVisibleClustersCounterBuffer);
    sharedRootConstants[CLOD_PC_PHASE2_EXPANSION_FACTOR] = phase2ExpansionFactor;

    uint32_t workGraphFlags = 0u;
    if (configuration.telemetry) {
        workGraphFlags |= CLOD_WG_FLAG_TELEMETRY_ENABLED;
    }
    if (UsesPerViewDepthMapOcclusion(inputs.m_rasterOutputKind) &&
        configuration.occlusion) {
        workGraphFlags |= CLOD_WG_FLAG_OCCLUSION_ENABLED;
    }
    const bool enableSoftwareRaster = UsesSWClassification(inputs.m_workGraphMode);
    if (enableSoftwareRaster) {
        workGraphFlags |= CLOD_WG_FLAG_SW_RASTER_ENABLED;
    }
    if (inputs.m_workGraphMode == HierarchicalCullingWorkGraphMode::SoftwareRasterCompute) {
        workGraphFlags |= CLOD_WG_FLAG_COMPUTE_SW_RASTER;
    }
    if (kDisableVirtualShadowDirtyPageCulling && UsesVirtualShadowOutput(inputs.m_rasterOutputKind)) {
        workGraphFlags |= CLOD_WG_FLAG_DISABLE_SHADOW_DIRTY_PAGE_CULLING;
    }
    if (UsesVirtualShadowOutput(inputs.m_rasterOutputKind) &&
        configuration.predictiveInvalidation) {
        workGraphFlags |= CLOD_WG_FLAG_VSM_PREDICTIVE_LOD_INVALIDATION;
    }
    if (UsesVirtualShadowOutput(inputs.m_rasterOutputKind) &&
        receiverSubpageMode != CLodVirtualShadowReceiverSubpageModeOff) {
        workGraphFlags |= CLOD_WG_FLAG_VSM_RECEIVER_SUBPAGE_MASK;
    }
    if (!configuration.frustumCulling) {
        workGraphFlags |= CLOD_WG_FLAG_DISABLE_FRUSTUM_CULLING;
    }
    const uint32_t swRasterThreshold = configuration.softwareRasterThreshold;
    workGraphFlags |= (swRasterThreshold << CLOD_WG_SW_RASTER_THRESHOLD_SHIFT);
    if (!inputs.m_isFirstPass) {
        workGraphFlags |= CLOD_WG_FLAG_PHASE2;
    }
    sharedRootConstants[CLOD_WG_FLAGS] = workGraphFlags;

    auto clearCounter = [&](const typename EmissionData::EmissionBuffer& buffer) {
        commandList.BindDescriptorIndices(inputs.m_clearPipelineState.GetResourceDescriptorSlots());
        commandList.BindPipeline(inputs.m_clearPipelineState.GetAPIPipelineState().GetHandle());

        typename CommandSink::Constant clearRootConstants[NumMiscUintRootConstants] = {};
        clearRootConstants[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = commandList.UAVIndex(buffer);
        clearRootConstants[CLOD_CLEAR_UINT_BUFFER_VALUE] = 0u;
        clearRootConstants[CLOD_CLEAR_UINT_BUFFER_COUNT] = 1u;
        commandList.PushConstants(
            rhi::ShaderStage::Compute,
            0,
            MiscUintRootSignatureIndex,
            0,
            NumMiscUintRootConstants,
            clearRootConstants);
        commandList.Dispatch(1u, 1u, 1u);
    };

    if (inputs.m_voxelRasterWorkCapacity != 0u) {
        clearCounter(inputs.m_voxelRasterWorkCounterBuffer);
        clearCounter(inputs.m_skinnedVoxelRasterWorkCounterBuffer);
    }

    auto bufferBarrier = [&](std::initializer_list<typename EmissionData::EmissionBuffer> buffers,
                             rhi::ResourceAccessType beforeAccess,
                             rhi::ResourceAccessType afterAccess,
                             rhi::ResourceSyncState beforeSync,
                             rhi::ResourceSyncState afterSync) {
        std::vector<rhi::BufferBarrier> barriers;
        barriers.reserve(buffers.size());
        for (const auto& buffer : buffers) {
            if (!buffer) {
                continue;
            }

            rhi::BufferBarrier barrier{};
            barrier.buffer = buffer->GetAPIResource().GetHandle();
            barrier.beforeAccess = beforeAccess;
            barrier.afterAccess = afterAccess;
            barrier.beforeSync = beforeSync;
            barrier.afterSync = afterSync;
            barriers.push_back(barrier);
        }

        if (!barriers.empty()) {
            rhi::BarrierBatch batch{};
            batch.buffers = rhi::Span<rhi::BufferBarrier>(barriers.data(), static_cast<uint32_t>(barriers.size()));
            commandList.Barriers(batch);
        }
    };

    auto uavBarrier = [&](std::initializer_list<typename EmissionData::EmissionBuffer> buffers) {
        bufferBarrier(
            buffers,
            rhi::ResourceAccessType::UnorderedAccess,
            rhi::ResourceAccessType::UnorderedAccess,
            rhi::ResourceSyncState::ComputeShading,
            rhi::ResourceSyncState::ComputeShading);
    };

    auto uavToComputeReadBarrier = [&](std::initializer_list<typename EmissionData::EmissionBuffer> buffers) {
        bufferBarrier(
            buffers,
            rhi::ResourceAccessType::UnorderedAccess,
            rhi::ResourceAccessType::ShaderResource,
            rhi::ResourceSyncState::ComputeShading,
            rhi::ResourceSyncState::ComputeShading);
    };

    auto computeReadToUavBarrier = [&](std::initializer_list<typename EmissionData::EmissionBuffer> buffers) {
        bufferBarrier(
            buffers,
            rhi::ResourceAccessType::ShaderResource,
            rhi::ResourceAccessType::UnorderedAccess,
            rhi::ResourceSyncState::ComputeShading,
            rhi::ResourceSyncState::ComputeShading);
    };

    auto uavToIndirectArgsBarrier = [&](std::initializer_list<typename EmissionData::EmissionBuffer> buffers) {
        bufferBarrier(
            buffers,
            rhi::ResourceAccessType::UnorderedAccess,
            rhi::ResourceAccessType::IndirectArgument,
            rhi::ResourceSyncState::ComputeShading,
            rhi::ResourceSyncState::ExecuteIndirect);
    };

    auto indirectArgsToUavBarrier = [&](std::initializer_list<typename EmissionData::EmissionBuffer> buffers) {
        bufferBarrier(
            buffers,
            rhi::ResourceAccessType::IndirectArgument,
            rhi::ResourceAccessType::UnorderedAccess,
            rhi::ResourceSyncState::ExecuteIndirect,
            rhi::ResourceSyncState::ComputeShading);
    };

    if (inputs.m_voxelRasterWorkCapacity != 0u) {
        uavBarrier({ inputs.m_voxelRasterWorkCounterBuffer, inputs.m_skinnedVoxelRasterWorkCounterBuffer });
    }

    bool nodeDispatchArgsNeedReuseBarrier = false;
    bool leafDispatchArgsNeedReuseBarrier = false;
    bool clusterDispatchArgsNeedReuseBarrier = false;

    auto buildDispatchArgs = [&](const typename EmissionData::EmissionBuffer& counterBuffer, const typename EmissionData::EmissionBuffer& argsBuffer, uint32_t threadsPerGroup) {
        commandList.BindDescriptorIndices(inputs.m_pureComputeBuildDispatchArgsPipelineState.GetResourceDescriptorSlots());
        commandList.BindPipeline(inputs.m_pureComputeBuildDispatchArgsPipelineState.GetAPIPipelineState().GetHandle());

        typename CommandSink::Constant dispatchRootConstants[NumMiscUintRootConstants] = {};
        std::copy(std::begin(sharedRootConstants), std::end(sharedRootConstants), std::begin(dispatchRootConstants));
        dispatchRootConstants[CLOD_WG_VISIBLE_CLUSTERS_CAPACITY] = static_cast<uint32_t>(inputs.m_maxVisibleClusters);
        dispatchRootConstants[CLOD_PC_DISPATCH_COUNTER_DESCRIPTOR_INDEX] = commandList.SRVIndex(counterBuffer);
        dispatchRootConstants[CLOD_PC_DISPATCH_ARGS_DESCRIPTOR_INDEX] = commandList.UAVIndex(argsBuffer);
        dispatchRootConstants[CLOD_PC_DISPATCH_THREADS_PER_GROUP] = threadsPerGroup;
        dispatchRootConstants[CLOD_PC_DISPATCH_COUNT_LIMIT] = static_cast<uint32_t>(inputs.m_maxVisibleClusters);
        commandList.PushSharedConstants(
            rhi::ShaderStage::Compute,
            0,
            MiscUintRootSignatureIndex,
            0,
            NumMiscUintRootConstants,
            dispatchRootConstants);
        commandList.Dispatch(1u, 1u, 1u);
    };

    auto buildDualDispatchArgs = [&](const typename EmissionData::EmissionBuffer& firstCounterBuffer,
                                     const typename EmissionData::EmissionBuffer& firstArgsBuffer,
                                     const typename EmissionData::EmissionBuffer& secondCounterBuffer,
                                     const typename EmissionData::EmissionBuffer& secondArgsBuffer,
                                     uint32_t threadsPerGroup) {
        commandList.BindDescriptorIndices(inputs.m_pureComputeBuildDualDispatchArgsPipelineState.GetResourceDescriptorSlots());
        commandList.BindPipeline(inputs.m_pureComputeBuildDualDispatchArgsPipelineState.GetAPIPipelineState().GetHandle());
        typename CommandSink::Constant dispatchRootConstants[NumMiscUintRootConstants] = {};
        std::copy(std::begin(sharedRootConstants), std::end(sharedRootConstants), std::begin(dispatchRootConstants));
        dispatchRootConstants[CLOD_PC_DISPATCH_COUNTER_DESCRIPTOR_INDEX] = commandList.SRVIndex(firstCounterBuffer);
        dispatchRootConstants[CLOD_PC_DISPATCH_ARGS_DESCRIPTOR_INDEX] = commandList.UAVIndex(firstArgsBuffer);
        dispatchRootConstants[CLOD_PC_DISPATCH_THREADS_PER_GROUP] = threadsPerGroup;
        dispatchRootConstants[CLOD_PC_DISPATCH_COUNT_LIMIT] = static_cast<uint32_t>(inputs.m_maxVisibleClusters);
        dispatchRootConstants[CLOD_PC_SECOND_DISPATCH_COUNTER_DESCRIPTOR_INDEX] = commandList.SRVIndex(secondCounterBuffer);
        dispatchRootConstants[CLOD_PC_SECOND_DISPATCH_ARGS_DESCRIPTOR_INDEX] = commandList.UAVIndex(secondArgsBuffer);
        commandList.PushSharedConstants(
            rhi::ShaderStage::Compute,
            0,
            MiscUintRootSignatureIndex,
            0,
            NumMiscUintRootConstants,
            dispatchRootConstants);
        commandList.Dispatch(1u, 1u, 1u);
    };

    auto clearTraversalCounters = [&](const typename EmissionData::EmissionBuffer& nodeCounter,
                                      const typename EmissionData::EmissionBuffer& leafCounter,
                                      const typename EmissionData::EmissionBuffer& clusterCounter) {
        commandList.BindDescriptorIndices(inputs.m_pureComputeClearTraversalCountersPipelineState.GetResourceDescriptorSlots());
        commandList.BindPipeline(inputs.m_pureComputeClearTraversalCountersPipelineState.GetAPIPipelineState().GetHandle());
        typename CommandSink::Constant clearRootConstants[NumMiscUintRootConstants] = {};
        std::copy(std::begin(sharedRootConstants), std::end(sharedRootConstants), std::begin(clearRootConstants));
        clearRootConstants[CLOD_PC_FRONTIER_OUTPUT_COUNT_DESCRIPTOR_INDEX] = commandList.UAVIndex(nodeCounter);
        clearRootConstants[CLOD_PC_LEAF_OUTPUT_COUNT_DESCRIPTOR_INDEX] = commandList.UAVIndex(leafCounter);
        clearRootConstants[CLOD_PC_CLUSTER_OUTPUT_COUNT_DESCRIPTOR_INDEX] = commandList.UAVIndex(clusterCounter);
        commandList.PushSharedConstants(
            rhi::ShaderStage::Compute,
            0,
            MiscUintRootSignatureIndex,
            0,
            NumMiscUintRootConstants,
            clearRootConstants);
        commandList.Dispatch(1u, 1u, 1u);
    };

    auto buildDispatchArgsWithLimit = [&](const typename EmissionData::EmissionBuffer& counterBuffer,
                                          const typename EmissionData::EmissionBuffer& argsBuffer,
                                          uint32_t threadsPerGroup,
                                          uint32_t countLimit) {
        commandList.BindDescriptorIndices(inputs.m_pureComputeBuildDispatchArgsPipelineState.GetResourceDescriptorSlots());
        commandList.BindPipeline(inputs.m_pureComputeBuildDispatchArgsPipelineState.GetAPIPipelineState().GetHandle());

        typename CommandSink::Constant dispatchRootConstants[NumMiscUintRootConstants] = {};
        std::copy(std::begin(sharedRootConstants), std::end(sharedRootConstants), std::begin(dispatchRootConstants));
        dispatchRootConstants[CLOD_WG_VISIBLE_CLUSTERS_CAPACITY] = static_cast<uint32_t>(inputs.m_maxVisibleClusters);
        dispatchRootConstants[CLOD_PC_DISPATCH_COUNTER_DESCRIPTOR_INDEX] = commandList.SRVIndex(counterBuffer);
        dispatchRootConstants[CLOD_PC_DISPATCH_ARGS_DESCRIPTOR_INDEX] = commandList.UAVIndex(argsBuffer);
        dispatchRootConstants[CLOD_PC_DISPATCH_THREADS_PER_GROUP] = threadsPerGroup;
        dispatchRootConstants[CLOD_PC_DISPATCH_COUNT_LIMIT] = countLimit;
        commandList.PushSharedConstants(
            rhi::ShaderStage::Compute,
            0,
            MiscUintRootSignatureIndex,
            0,
            NumMiscUintRootConstants,
            dispatchRootConstants);
        commandList.Dispatch(1u, 1u, 1u);
    };

    auto buildReplayDispatchArgs = [&](const typename EmissionData::EmissionBuffer& argsBuffer, uint32_t replaySourceKind, uint32_t threadsPerGroup) {
        commandList.BindDescriptorIndices(inputs.m_pureComputeBuildReplayDispatchArgsPipelineState.GetResourceDescriptorSlots());
        commandList.BindPipeline(inputs.m_pureComputeBuildReplayDispatchArgsPipelineState.GetAPIPipelineState().GetHandle());

        typename CommandSink::Constant dispatchRootConstants[NumMiscUintRootConstants] = {};
        dispatchRootConstants[CLOD_WG_VISIBLE_CLUSTERS_CAPACITY] = static_cast<uint32_t>(inputs.m_maxVisibleClusters);
        dispatchRootConstants[CLOD_WG_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX] =
            commandList.UAVIndex(inputs.m_occlusionReplayStateBuffer);
        dispatchRootConstants[CLOD_PC_DISPATCH_ARGS_DESCRIPTOR_INDEX] = commandList.UAVIndex(argsBuffer);
        dispatchRootConstants[CLOD_PC_DISPATCH_THREADS_PER_GROUP] = threadsPerGroup;
        dispatchRootConstants[CLOD_PC_REPLAY_SOURCE_INDEX] = replaySourceKind;
        commandList.PushConstants(
            rhi::ShaderStage::Compute,
            0,
            MiscUintRootSignatureIndex,
            0,
            NumMiscUintRootConstants,
            dispatchRootConstants);
        commandList.Dispatch(1u, 1u, 1u);
    };

    const bool useDenseClusterPath = true;

    auto dispatchClusterCull = [&](const typename EmissionData::EmissionBuffer& frontierBuffer, const typename EmissionData::EmissionBuffer& frontierCounterBuffer) {
        if (!useDenseClusterPath) {
            if (clusterDispatchArgsNeedReuseBarrier) {
                indirectArgsToUavBarrier({ inputs.m_pureComputeClusterDispatchArgsBuffer });
                clusterDispatchArgsNeedReuseBarrier = false;
            }
            buildDispatchArgs(frontierCounterBuffer, inputs.m_pureComputeClusterDispatchArgsBuffer, kPureComputeClusterThreadsPerGroup);
            uavToIndirectArgsBarrier({ inputs.m_pureComputeClusterDispatchArgsBuffer });

            commandList.BindDescriptorIndices(inputs.m_pureComputeClusterPipelineState.GetResourceDescriptorSlots());
            commandList.BindPipeline(inputs.m_pureComputeClusterPipelineState.GetAPIPipelineState().GetHandle());
            typename CommandSink::Constant clusterRootConstants[NumMiscUintRootConstants] = {};
            std::copy(std::begin(sharedRootConstants), std::end(sharedRootConstants), std::begin(clusterRootConstants));
            clusterRootConstants[CLOD_PC_FRONTIER_INPUT_DESCRIPTOR_INDEX] = commandList.SRVIndex(frontierBuffer);
            clusterRootConstants[CLOD_PC_FRONTIER_INPUT_COUNT_DESCRIPTOR_INDEX] = commandList.SRVIndex(frontierCounterBuffer);
            commandList.PushSharedConstants(
                rhi::ShaderStage::Compute,
                0,
                MiscUintRootSignatureIndex,
                0,
                NumMiscUintRootConstants,
                clusterRootConstants);
            commandList.ExecuteIndirect(
                (*inputs.m_pureComputeDispatchCommandSignature)->GetHandle(),
                inputs.m_pureComputeClusterDispatchArgsBuffer->GetAPIResource().GetHandle(),
                0,
                {},
                0,
                1);
            clusterDispatchArgsNeedReuseBarrier = true;
            return;
        }

        if (clusterDispatchArgsNeedReuseBarrier) {
            indirectArgsToUavBarrier({ inputs.m_pureComputeClusterDispatchArgsBuffer });
            clusterDispatchArgsNeedReuseBarrier = false;
        }
        // For dense cluster culling this root constant names records per shader group,
        // not thread lanes: the shader expands each record across phase2ExpansionFactor lanes.
        buildDispatchArgsWithLimit(
            frontierCounterBuffer,
            inputs.m_pureComputeClusterDispatchArgsBuffer,
            phase2RecordsPerGroup,
            static_cast<uint32_t>(inputs.m_maxVisibleClusters));
        uavToIndirectArgsBarrier({ inputs.m_pureComputeClusterDispatchArgsBuffer });

        commandList.BindDescriptorIndices(inputs.m_pureComputeDenseClusterPipelineState.GetResourceDescriptorSlots());
        commandList.BindPipeline(inputs.m_pureComputeDenseClusterPipelineState.GetAPIPipelineState().GetHandle());
        typename CommandSink::Constant denseClusterRootConstants[NumMiscUintRootConstants] = {};
        std::copy(std::begin(sharedRootConstants), std::end(sharedRootConstants), std::begin(denseClusterRootConstants));
        denseClusterRootConstants[CLOD_PC_FRONTIER_INPUT_DESCRIPTOR_INDEX] = commandList.SRVIndex(frontierBuffer);
        denseClusterRootConstants[CLOD_PC_FRONTIER_INPUT_COUNT_DESCRIPTOR_INDEX] = commandList.SRVIndex(frontierCounterBuffer);
        commandList.PushSharedConstants(
            rhi::ShaderStage::Compute,
            0,
            MiscUintRootSignatureIndex,
            0,
            NumMiscUintRootConstants,
            denseClusterRootConstants);
        commandList.ExecuteIndirect(
            (*inputs.m_pureComputeDispatchCommandSignature)->GetHandle(),
            inputs.m_pureComputeClusterDispatchArgsBuffer->GetAPIResource().GetHandle(),
            0,
            {},
            0,
            1);
        clusterDispatchArgsNeedReuseBarrier = true;
    };

    clearCounter(inputs.m_pureComputeCurrentNodeCounterBuffer);
    clearCounter(inputs.m_pureComputeNextNodeCounterBuffer);
    clearCounter(inputs.m_pureComputeCurrentLeafCounterBuffer);
    clearCounter(inputs.m_pureComputeNextLeafCounterBuffer);
    clearCounter(inputs.m_pureComputeClusterCounterBuffer);
    uavBarrier({
        inputs.m_pureComputeCurrentNodeCounterBuffer,
        inputs.m_pureComputeNextNodeCounterBuffer,
        inputs.m_pureComputeCurrentLeafCounterBuffer,
        inputs.m_pureComputeNextLeafCounterBuffer,
        inputs.m_pureComputeClusterCounterBuffer,
    });

    if (inputs.m_isFirstPass) {
        commandList.BindDescriptorIndices(inputs.m_pureComputeObjectCullPipelineState.GetResourceDescriptorSlots());
        commandList.BindPipeline(inputs.m_pureComputeObjectCullPipelineState.GetAPIPipelineState().GetHandle());
        typename CommandSink::Constant objectCullRootConstants[NumMiscUintRootConstants]{};
        std::copy(std::begin(sharedRootConstants), std::end(sharedRootConstants), std::begin(objectCullRootConstants));
        objectCullRootConstants[CLOD_PC_FRONTIER_OUTPUT_DESCRIPTOR_INDEX] = commandList.UAVIndex(inputs.m_pureComputeCurrentNodeFrontierBuffer);
        objectCullRootConstants[CLOD_PC_FRONTIER_OUTPUT_COUNT_DESCRIPTOR_INDEX] = commandList.UAVIndex(inputs.m_pureComputeCurrentNodeCounterBuffer);
        objectCullRootConstants[CLOD_PC_OBJECT_CULL_INVALIDATION_COUNT_SRV_INDEX] =
            inputs.m_shadowInvalidationCountBuffer ? commandList.SRVIndex(inputs.m_shadowInvalidationCountBuffer) : 0u;
        commandList.ObjectCull(objectCullRootConstants);

        uavToComputeReadBarrier({ inputs.m_pureComputeCurrentNodeFrontierBuffer, inputs.m_pureComputeCurrentNodeCounterBuffer });
    }
    else {
        sharedRootConstants[CLOD_WG_HW_WRITE_BASE_COUNTER_DESCRIPTOR_INDEX] =
            commandList.SRVIndex(inputs.m_phase1VisibleClustersCounterBuffer
                ? inputs.m_phase1VisibleClustersCounterBuffer : inputs.m_visibleClustersCounterBuffer);
        sharedRootConstants[CLOD_WG_SW_WRITE_BASE_COUNTER_DESCRIPTOR_INDEX] =
            commandList.SRVIndex(inputs.m_swWriteBaseCounterBuffer
                ? inputs.m_swWriteBaseCounterBuffer : inputs.m_swVisibleClustersCounterBuffer);

        buildReplayDispatchArgs(inputs.m_pureComputeNodeDispatchArgsBuffer, kReplaySourceNodes, kPureComputeTraverseThreadsPerGroup);
        uavToIndirectArgsBarrier({ inputs.m_pureComputeNodeDispatchArgsBuffer });

        commandList.BindDescriptorIndices(inputs.m_pureComputeReplayNodesPipelineState.GetResourceDescriptorSlots());
        commandList.BindPipeline(inputs.m_pureComputeReplayNodesPipelineState.GetAPIPipelineState().GetHandle());
        typename CommandSink::Constant replayNodeRootConstants[NumMiscUintRootConstants] = {};
        std::copy(std::begin(sharedRootConstants), std::end(sharedRootConstants), std::begin(replayNodeRootConstants));
        replayNodeRootConstants[CLOD_PC_FRONTIER_OUTPUT_DESCRIPTOR_INDEX] = commandList.UAVIndex(inputs.m_pureComputeCurrentNodeFrontierBuffer);
        replayNodeRootConstants[CLOD_PC_FRONTIER_OUTPUT_COUNT_DESCRIPTOR_INDEX] = commandList.UAVIndex(inputs.m_pureComputeCurrentNodeCounterBuffer);
        commandList.PushSharedConstants(
            rhi::ShaderStage::Compute,
            0,
            MiscUintRootSignatureIndex,
            0,
            NumMiscUintRootConstants,
            replayNodeRootConstants);
        commandList.ExecuteIndirect(
            (*inputs.m_pureComputeDispatchCommandSignature)->GetHandle(),
            inputs.m_pureComputeNodeDispatchArgsBuffer->GetAPIResource().GetHandle(),
            0,
            {},
            0,
            1);
        nodeDispatchArgsNeedReuseBarrier = true;

        buildReplayDispatchArgs(inputs.m_pureComputeClusterDispatchArgsBuffer, kReplaySourceClusters, kPureComputeClusterThreadsPerGroup);
        uavToIndirectArgsBarrier({ inputs.m_pureComputeClusterDispatchArgsBuffer });

        commandList.BindDescriptorIndices(inputs.m_pureComputeReplayClustersPipelineState.GetResourceDescriptorSlots());
        commandList.BindPipeline(inputs.m_pureComputeReplayClustersPipelineState.GetAPIPipelineState().GetHandle());
        typename CommandSink::Constant replayClusterRootConstants[NumMiscUintRootConstants] = {};
        std::copy(std::begin(sharedRootConstants), std::end(sharedRootConstants), std::begin(replayClusterRootConstants));
        replayClusterRootConstants[CLOD_PC_CLUSTER_OUTPUT_DESCRIPTOR_INDEX] = commandList.UAVIndex(inputs.m_pureComputeClusterFrontierBuffer);
        replayClusterRootConstants[CLOD_PC_CLUSTER_OUTPUT_COUNT_DESCRIPTOR_INDEX] = commandList.UAVIndex(inputs.m_pureComputeClusterCounterBuffer);
        commandList.PushSharedConstants(
            rhi::ShaderStage::Compute,
            0,
            MiscUintRootSignatureIndex,
            0,
            NumMiscUintRootConstants,
            replayClusterRootConstants);
        commandList.ExecuteIndirect(
            (*inputs.m_pureComputeDispatchCommandSignature)->GetHandle(),
            inputs.m_pureComputeClusterDispatchArgsBuffer->GetAPIResource().GetHandle(),
            0,
            {},
            0,
            1);
        clusterDispatchArgsNeedReuseBarrier = true;

        uavToComputeReadBarrier({
            inputs.m_pureComputeCurrentNodeFrontierBuffer,
            inputs.m_pureComputeCurrentNodeCounterBuffer,
            inputs.m_pureComputeClusterFrontierBuffer,
            inputs.m_pureComputeClusterCounterBuffer,
        });

        dispatchClusterCull(inputs.m_pureComputeClusterFrontierBuffer, inputs.m_pureComputeClusterCounterBuffer);
        computeReadToUavBarrier({
            inputs.m_pureComputeClusterFrontierBuffer,
            inputs.m_pureComputeClusterCounterBuffer,
        });
    }

    uavToComputeReadBarrier({
        inputs.m_pureComputeCurrentNodeFrontierBuffer,
        inputs.m_pureComputeCurrentNodeCounterBuffer,
        inputs.m_pureComputeCurrentLeafFrontierBuffer,
        inputs.m_pureComputeCurrentLeafCounterBuffer,
    });
    uavBarrier({
        inputs.m_visibleClustersCounterBuffer,
        inputs.m_occlusionReplayBuffer,
        inputs.m_occlusionReplayStateBuffer,
    });

    auto currentNodeFrontier = inputs.m_pureComputeCurrentNodeFrontierBuffer;
    auto currentNodeCounter = inputs.m_pureComputeCurrentNodeCounterBuffer;
    auto nextNodeFrontier = inputs.m_pureComputeNextNodeFrontierBuffer;
    auto nextNodeCounter = inputs.m_pureComputeNextNodeCounterBuffer;
    auto currentLeafFrontier = inputs.m_pureComputeCurrentLeafFrontierBuffer;
    auto currentLeafCounter = inputs.m_pureComputeCurrentLeafCounterBuffer;
    auto nextLeafFrontier = inputs.m_pureComputeNextLeafFrontierBuffer;
    auto nextLeafCounter = inputs.m_pureComputeNextLeafCounterBuffer;

    const uint32_t traversalLevelCount = std::min(inputs.m_activeTraversalDepth, kPureComputeMaxTraversalLevels);
    for (uint32_t level = 0; level < traversalLevelCount; ++level) {
        if (level > 0u) {
            computeReadToUavBarrier({
                nextNodeFrontier,
                nextNodeCounter,
                nextLeafFrontier,
                nextLeafCounter,
            });
        }
        clearTraversalCounters(nextNodeCounter, nextLeafCounter, inputs.m_pureComputeClusterCounterBuffer);
        uavBarrier({ nextNodeCounter, nextLeafCounter, inputs.m_pureComputeClusterCounterBuffer });

        if (nodeDispatchArgsNeedReuseBarrier) {
            indirectArgsToUavBarrier({ inputs.m_pureComputeNodeDispatchArgsBuffer });
            nodeDispatchArgsNeedReuseBarrier = false;
        }
        if (leafDispatchArgsNeedReuseBarrier) {
            indirectArgsToUavBarrier({ inputs.m_pureComputeLeafDispatchArgsBuffer });
            leafDispatchArgsNeedReuseBarrier = false;
        }
        buildDualDispatchArgs(
            currentNodeCounter,
            inputs.m_pureComputeNodeDispatchArgsBuffer,
            currentLeafCounter,
            inputs.m_pureComputeLeafDispatchArgsBuffer,
            kPureComputeTraverseThreadsPerGroup);
        uavToIndirectArgsBarrier({
            inputs.m_pureComputeNodeDispatchArgsBuffer,
            inputs.m_pureComputeLeafDispatchArgsBuffer,
        });

        commandList.BindDescriptorIndices(inputs.m_pureComputeTraversePipelineState.GetResourceDescriptorSlots());
        commandList.BindPipeline(inputs.m_pureComputeTraversePipelineState.GetAPIPipelineState().GetHandle());
        auto dispatchNodeFrontier = [&](const typename EmissionData::EmissionBuffer& inputFrontier,
                                       const typename EmissionData::EmissionBuffer& inputCounter,
                                       const typename EmissionData::EmissionBuffer& dispatchArgs) {
            typename CommandSink::Constant traverseRootConstants[NumMiscUintRootConstants] = {};
            std::copy(std::begin(sharedRootConstants), std::end(sharedRootConstants), std::begin(traverseRootConstants));
            traverseRootConstants[CLOD_PC_FRONTIER_INPUT_DESCRIPTOR_INDEX] = commandList.SRVIndex(inputFrontier);
            traverseRootConstants[CLOD_PC_FRONTIER_INPUT_COUNT_DESCRIPTOR_INDEX] = commandList.SRVIndex(inputCounter);
            traverseRootConstants[CLOD_PC_FRONTIER_OUTPUT_DESCRIPTOR_INDEX] = commandList.UAVIndex(nextNodeFrontier);
            traverseRootConstants[CLOD_PC_FRONTIER_OUTPUT_COUNT_DESCRIPTOR_INDEX] = commandList.UAVIndex(nextNodeCounter);
            traverseRootConstants[CLOD_PC_CLUSTER_OUTPUT_DESCRIPTOR_INDEX] = commandList.UAVIndex(inputs.m_pureComputeClusterFrontierBuffer);
            traverseRootConstants[CLOD_PC_CLUSTER_OUTPUT_COUNT_DESCRIPTOR_INDEX] = commandList.UAVIndex(inputs.m_pureComputeClusterCounterBuffer);
            traverseRootConstants[CLOD_PC_LEAF_OUTPUT_DESCRIPTOR_INDEX] = commandList.UAVIndex(nextLeafFrontier);
            traverseRootConstants[CLOD_PC_LEAF_OUTPUT_COUNT_DESCRIPTOR_INDEX] = commandList.UAVIndex(nextLeafCounter);
            commandList.PushSharedConstants(
                rhi::ShaderStage::Compute,
                0,
                MiscUintRootSignatureIndex,
                0,
                NumMiscUintRootConstants,
                traverseRootConstants);
            commandList.ExecuteIndirect(
                (*inputs.m_pureComputeDispatchCommandSignature)->GetHandle(),
                dispatchArgs->GetAPIResource().GetHandle(),
                0,
                {},
                0,
                1);
        };
        dispatchNodeFrontier(currentNodeFrontier, currentNodeCounter, inputs.m_pureComputeNodeDispatchArgsBuffer);
        uavBarrier({
            nextNodeFrontier,
            nextNodeCounter,
            nextLeafFrontier,
            nextLeafCounter,
            inputs.m_pureComputeClusterFrontierBuffer,
            inputs.m_pureComputeClusterCounterBuffer,
        });
        commandList.BindDescriptorIndices(inputs.m_pureComputeLeafPipelineState.GetResourceDescriptorSlots());
        commandList.BindPipeline(inputs.m_pureComputeLeafPipelineState.GetAPIPipelineState().GetHandle());
        dispatchNodeFrontier(currentLeafFrontier, currentLeafCounter, inputs.m_pureComputeLeafDispatchArgsBuffer);
        nodeDispatchArgsNeedReuseBarrier = true;
        leafDispatchArgsNeedReuseBarrier = true;

        uavToComputeReadBarrier({
            nextNodeFrontier,
            nextNodeCounter,
            nextLeafFrontier,
            nextLeafCounter,
            inputs.m_pureComputeClusterFrontierBuffer,
            inputs.m_pureComputeClusterCounterBuffer,
        });
        uavBarrier({
            inputs.m_occlusionReplayBuffer,
            inputs.m_occlusionReplayStateBuffer,
        });

        dispatchClusterCull(inputs.m_pureComputeClusterFrontierBuffer, inputs.m_pureComputeClusterCounterBuffer);

        computeReadToUavBarrier({
            inputs.m_pureComputeClusterFrontierBuffer,
            inputs.m_pureComputeClusterCounterBuffer,
        });
        uavBarrier({
            inputs.m_visibleClustersBuffer,
            inputs.m_visibleClustersCounterBuffer,
            inputs.m_occlusionReplayBuffer,
            inputs.m_occlusionReplayStateBuffer,
        });

        std::swap(currentNodeFrontier, nextNodeFrontier);
        std::swap(currentNodeCounter, nextNodeCounter);
        std::swap(currentLeafFrontier, nextLeafFrontier);
        std::swap(currentLeafCounter, nextLeafCounter);
    }

    uavToComputeReadBarrier({ inputs.m_visibleClustersCounterBuffer, inputs.m_occlusionReplayStateBuffer });

    commandList.BindDescriptorIndices(inputs.m_createCommandPipelineState.GetResourceDescriptorSlots());
    commandList.BindPipeline(inputs.m_createCommandPipelineState.GetAPIPipelineState().GetHandle());

    typename CommandSink::Constant createRootConstants[NumMiscUintRootConstants] = {};
    std::copy(std::begin(sharedRootConstants), std::end(sharedRootConstants), std::begin(createRootConstants));
    createRootConstants[CLOD_CREATE_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = commandList.SRVIndex(inputs.m_visibleClustersCounterBuffer);
    createRootConstants[CLOD_CREATE_RASTER_BUCKET_HISTOGRAM_COMMAND_DESCRIPTOR_INDEX] = commandList.UAVIndex(inputs.m_histogramIndirectCommand);
    createRootConstants[CLOD_CREATE_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX] = commandList.SRVIndex(inputs.m_occlusionReplayStateBuffer);
    // Pure-compute replay seeds descriptor-backed frontiers directly; it does
    // not consume D3D12 work-graph node-input records.
    createRootConstants[CLOD_CREATE_WORKGRAPH_NODE_INPUTS_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    createRootConstants[CLOD_CREATE_NUM_RASTER_BUCKETS] = configuration.rasterBucketCount;
    createRootConstants[CLOD_CREATE_VISIBLE_CLUSTERS_CAPACITY] = static_cast<uint32_t>(inputs.m_maxVisibleClusters);
    commandList.PushSharedConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        createRootConstants);
    commandList.Dispatch(1u, 1u, 1u);

    return {};
}

br::render::CapturedHierarchicalDispatchCullingInputs HierarchicalDispatchCullingPass::CaptureCommandInputs(
    const org::PublicationBindingBundle& bundle) const
{
    BT_ZONE_SCOPE("BR.CullingPublication.CaptureInputs");
    br::render::CapturedHierarchicalDispatchCullingInputs result;
    std::unordered_map<uint64_t,br::render::CapturedHierarchicalDispatchCullingInputs::EmissionBuffer> resources;
    const auto capture = [&](const Resource* resource) -> br::render::CapturedHierarchicalDispatchCullingInputs::EmissionBuffer {
        if (!resource) return {};
        const auto id = resource->GetGlobalResourceID();
        if (const auto found = resources.find(id); found != resources.end()) return found->second;
        const auto* snapshot = bundle.Find(id);
        if (!snapshot || !*snapshot) throw std::invalid_argument("Culling resource missing from publication bundle");
        auto captured = std::make_shared<const br::render::CapturedCullingResource>(br::render::CapturedCullingResource{*snapshot});
        resources.emplace(id,captured);
        return captured;
    };
    result.m_dynamicWindBoundsCacheBuffer = capture(m_dynamicWindBoundsCacheBuffer.get());
    result.m_dynamicWindVisibleMembershipBuffer = capture(m_dynamicWindVisibleMembershipBuffer);
    result.m_histogramIndirectCommand = capture(m_histogramIndirectCommand.get());
    result.m_occlusionReplayBuffer = capture(m_occlusionReplayBuffer.get());
    result.m_occlusionReplayStateBuffer = capture(m_occlusionReplayStateBuffer.get());
    result.m_pageJobVisibleClustersCounterBuffer = capture(m_pageJobVisibleClustersCounterBuffer.get());
    result.m_phase1VisibleClustersCounterBuffer = capture(m_phase1VisibleClustersCounterBuffer.get());
    result.m_pureComputeClusterCounterBuffer = capture(m_pureComputeClusterCounterBuffer.get());
    result.m_pureComputeClusterDispatchArgsBuffer = capture(m_pureComputeClusterDispatchArgsBuffer.get());
    result.m_pureComputeClusterFrontierBuffer = capture(m_pureComputeClusterFrontierBuffer.get());
    result.m_pureComputeCurrentLeafCounterBuffer = capture(m_pureComputeCurrentLeafCounterBuffer.get());
    result.m_pureComputeCurrentLeafFrontierBuffer = capture(m_pureComputeCurrentLeafFrontierBuffer.get());
    result.m_pureComputeCurrentNodeCounterBuffer = capture(m_pureComputeCurrentNodeCounterBuffer.get());
    result.m_pureComputeCurrentNodeFrontierBuffer = capture(m_pureComputeCurrentNodeFrontierBuffer.get());
    result.m_pureComputeLeafDispatchArgsBuffer = capture(m_pureComputeLeafDispatchArgsBuffer.get());
    result.m_pureComputeNextLeafCounterBuffer = capture(m_pureComputeNextLeafCounterBuffer.get());
    result.m_pureComputeNextLeafFrontierBuffer = capture(m_pureComputeNextLeafFrontierBuffer.get());
    result.m_pureComputeNextNodeCounterBuffer = capture(m_pureComputeNextNodeCounterBuffer.get());
    result.m_pureComputeNextNodeFrontierBuffer = capture(m_pureComputeNextNodeFrontierBuffer.get());
    result.m_pureComputeNodeDispatchArgsBuffer = capture(m_pureComputeNodeDispatchArgsBuffer.get());
    result.m_shadowActiveBlockMetadataBuffer = capture(m_shadowActiveBlockMetadataBuffer.get());
    result.m_shadowDirtyHierarchyTexture = capture(m_shadowDirtyHierarchyTexture.get());
    result.m_shadowDynamicActiveBlockMetadataBuffer = capture(m_shadowDynamicActiveBlockMetadataBuffer.get());
    result.m_shadowDynamicPhysicalPagesTexture = capture(m_shadowDynamicPhysicalPagesTexture.get());
    result.m_shadowInvalidatedInstancesBitsetBuffer = capture(m_shadowInvalidatedInstancesBitsetBuffer.get());
    result.m_shadowInvalidationCountBuffer = capture(m_shadowInvalidationCountBuffer.get());
    result.m_shadowPageTableTexture = capture(m_shadowPageTableTexture.get());
    result.m_shadowPhysicalPagesTexture = capture(m_shadowPhysicalPagesTexture.get());
    result.m_shadowPredictiveInvalidationCandidateCountBuffer = capture(m_shadowPredictiveInvalidationCandidateCountBuffer.get());
    result.m_shadowPredictiveInvalidationCandidatesBuffer = capture(m_shadowPredictiveInvalidationCandidatesBuffer.get());
    result.m_shadowReceiverSubpageMaskBuffer = capture(m_shadowReceiverSubpageMaskBuffer.get());
    result.m_skinnedVoxelRasterWorkCounterBuffer = capture(m_skinnedVoxelRasterWorkCounterBuffer.get());
    result.m_swVisibleClustersCounterBuffer = capture(m_swVisibleClustersCounterBuffer.get());
    result.m_swWriteBaseCounterBuffer = capture(m_swWriteBaseCounterBuffer.get());
    result.m_viewDepthSrvIndicesBuffer = capture(m_viewDepthSrvIndicesBuffer.get());
    result.m_viewRasterInfoBuffer = capture(m_viewRasterInfoBuffer.get());
    result.m_visibleClusterTransformIndicesBuffer = capture(m_visibleClusterTransformIndicesBuffer.get());
    result.m_visibleClustersBuffer = capture(m_visibleClustersBuffer.get());
    result.m_visibleClustersCounterBuffer = capture(m_visibleClustersCounterBuffer.get());
    result.m_voxelRasterWorkCounterBuffer = capture(m_voxelRasterWorkCounterBuffer.get());
    result.m_workGraphTelemetryBuffer = capture(m_workGraphTelemetryBuffer.get());
    result.m_clearPipelineState.payload = m_clearPipelineState.GetPayload();
    result.m_createCommandPipelineState.payload = m_createCommandPipelineState.GetPayload();
    result.m_pureComputeBuildDispatchArgsPipelineState.payload = m_pureComputeBuildDispatchArgsPipelineState.GetPayload();
    result.m_pureComputeBuildDualDispatchArgsPipelineState.payload = m_pureComputeBuildDualDispatchArgsPipelineState.GetPayload();
    result.m_pureComputeBuildReplayDispatchArgsPipelineState.payload = m_pureComputeBuildReplayDispatchArgsPipelineState.GetPayload();
    result.m_pureComputeClearTraversalCountersPipelineState.payload = m_pureComputeClearTraversalCountersPipelineState.GetPayload();
    result.m_pureComputeClusterPipelineState.payload = m_pureComputeClusterPipelineState.GetPayload();
    result.m_pureComputeDenseClusterPipelineState.payload = m_pureComputeDenseClusterPipelineState.GetPayload();
    result.m_pureComputeLeafPipelineState.payload = m_pureComputeLeafPipelineState.GetPayload();
    result.m_pureComputeObjectCullPipelineState.payload = m_pureComputeObjectCullPipelineState.GetPayload();
    result.m_pureComputeReplayClustersPipelineState.payload = m_pureComputeReplayClustersPipelineState.GetPayload();
    result.m_pureComputeReplayNodesPipelineState.payload = m_pureComputeReplayNodesPipelineState.GetPayload();
    result.m_pureComputeTraversePipelineState.payload = m_pureComputeTraversePipelineState.GetPayload();
    result.m_activeTraversalDepth = m_activeTraversalDepth;
    result.m_dynamicWindBoundsCacheEntryCount = m_dynamicWindBoundsCacheEntryCount;
    result.m_dynamicWindBoundsCacheGeneration = m_dynamicWindBoundsCacheGeneration;
    result.m_isFirstPass = m_isFirstPass;
    result.m_maxVisibleClusters = m_maxVisibleClusters;
    result.m_pureComputeDispatchCommandSignature = m_pureComputeDispatchCommandSignature;
    result.m_rasterOutputKind = m_rasterOutputKind;
    result.m_voxelRasterWorkCapacity = m_voxelRasterWorkCapacity;
    result.m_workGraphMode = m_workGraphMode;
    return result;
}

HierarchicalDispatchCullingRecipe HierarchicalDispatchCullingPass::BuildCapturedRecipe(
    const br::render::CapturedHierarchicalDispatchCullingInputs& inputs,
    const HierarchicalDispatchCullingCommandConfiguration& configuration, const br::render::CapturedCullingBindings& bindings)
{
    if (configuration.expansionFactor != CLodNormalizePureComputePhase2ExpansionFactor(configuration.expansionFactor))
        throw std::invalid_argument("Captured culling expansion factor is not normalized");
    br::render::CapturedCullingCommandSink sink(bindings);
    EmitCommands(inputs,sink,configuration);
    return sink.Finish();
}

HierarchicalDispatchCullingCommandConfiguration HierarchicalDispatchCullingPass::CaptureCommandConfiguration(
    rhi::PipelineLayoutHandle layout, uint32_t rasterBucketCount) const
{
    BT_ZONE_SCOPE("BR.CullingPublication.CaptureConfiguration");
    HierarchicalDispatchCullingCommandConfiguration configuration;
    configuration.layout = layout;
    configuration.rasterBucketCount = rasterBucketCount;
    configuration.settingsRevision = SettingsManager::GetInstance().Revision();
    auto& settings = SettingsManager::GetInstance();
    configuration.telemetry = IsCLodWorkGraphTelemetryEnabled();
    configuration.expansionFactor = CLodNormalizePureComputePhase2ExpansionFactor(settings.getSettingGetter<uint32_t>(
        m_isFirstPass ? CLodPureComputePhase2ExpansionFactorSettingName : CLodPureComputeReplayExpansionFactorSettingName)());
    configuration.forcedTraversalDepth = settings.getSettingGetter<uint32_t>(CLodForceTraversalDepthRootSettingName)();
    configuration.receiverSubpageMode = m_shadowReceiverSubpageMaskBuffer
        ? settings.getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowReceiverSubpageModeSettingName)()
        : CLodVirtualShadowReceiverSubpageModeOff;
    configuration.occlusion = settings.getSettingGetter<bool>("enableOcclusionCulling")();
    configuration.predictiveInvalidation = settings.getSettingGetter<bool>(CLodDirectionalVirtualShadowPredictiveLodInvalidationSettingName)();
    configuration.frustumCulling = settings.getSettingGetter<bool>(CLodFrustumCullingSettingName)();
    configuration.softwareRasterThreshold = std::min(settings.getSettingGetter<uint32_t>(UsesVirtualShadowOutput(m_rasterOutputKind)
        ? CLodVirtualShadowSoftwareRasterDiameterThresholdSettingName : CLodSoftwareRasterDiameterThresholdSettingName)(),0xFFFFu);
    uint32_t pageJobFlags = 0u;
    {
        auto& settings = SettingsManager::GetInstance();
        const bool pageJobEnabled =
            UsesVirtualShadowOutput(m_rasterOutputKind)
            && CLodVSMRasterModeUsesLargeClusterShadowRouting(
                settings.getSettingGetter<CLodVSMRasterMode>(CLodVSMRasterModeSettingName)());
        if (pageJobEnabled) {
            pageJobFlags |= CLOD_WG_PAGE_JOB_FLAG_ENABLED;
        }
        const bool pageJobForceAll = settings.getSettingGetter<bool>(CLodPageJobForceAllSettingName)();
        if (pageJobForceAll) {
            pageJobFlags |= CLOD_WG_PAGE_JOB_FLAG_FORCE_ALL;
        }
        const uint32_t diameterThreshold = std::min(settings.getSettingGetter<uint32_t>(CLodPageJobDiameterThresholdSettingName)(), 255u);
        pageJobFlags |= (diameterThreshold << CLOD_WG_PAGE_JOB_DIAMETER_THRESHOLD_SHIFT);
        const float sparseRatio = settings.getSettingGetter<float>(CLodPageJobSparseRatioSettingName)();
        const uint32_t sparseRatioEncoded = std::min(static_cast<uint32_t>(sparseRatio * 255.0f + 0.5f), 255u);
        pageJobFlags |= (sparseRatioEncoded << CLOD_WG_PAGE_JOB_SPARSE_RATIO_SHIFT);
        const uint32_t maxPages = std::min(settings.getSettingGetter<uint32_t>(CLodPageJobMaxPagesPerClusterSettingName)(), 255u);
        pageJobFlags |= (maxPages << CLOD_WG_PAGE_JOB_MAX_PAGES_SHIFT);
    }
    configuration.pageJobFlags = pageJobFlags;
    if (configuration.settingsRevision != settings.Revision()) throw std::runtime_error("Culling settings changed during command preparation");
    return configuration;
}

HierarchicalDispatchCullingRecipe HierarchicalDispatchCullingPass::BuildRecipe(
    const org::PassPrepareContext& preparation) const
{
    auto* renderContext = preparation.preparationData
        ? preparation.preparationData->Get<RenderContext>() : nullptr;
    if (!renderContext || !preparation.bindings) return {};

    br::render::PreparedComputeCommandBuilder commands(
        preparation,
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    const auto configuration = CaptureCommandConfiguration(commands.Sequence().layout,renderContext->preparedRasterBucketCount);
    PreparedComputeCommandSink sink(commands, preparation);
    {
    BT_ZONE_SCOPE("BR.CullingRecipe.CapturePrograms");
    sink.Register(m_clearPipelineState);
    sink.Register(m_createCommandPipelineState);
    sink.Register(m_pureComputeBuildDispatchArgsPipelineState);
    sink.Register(m_pureComputeBuildDualDispatchArgsPipelineState);
    sink.Register(m_pureComputeClearTraversalCountersPipelineState);
    sink.Register(m_pureComputeBuildReplayDispatchArgsPipelineState);
    sink.Register(m_pureComputeObjectCullPipelineState);
    sink.Register(m_pureComputeReplayNodesPipelineState);
    sink.Register(m_pureComputeReplayClustersPipelineState);
    sink.Register(m_pureComputeTraversePipelineState);
    sink.Register(m_pureComputeLeafPipelineState);
    sink.Register(m_pureComputeClusterPipelineState);
    sink.Register(m_pureComputeDenseClusterPipelineState);
    commands.Retain(m_pureComputeDispatchCommandSignature);
    }

    {
    BT_ZONE_SCOPE("BR.CullingRecipe.EmitCommands");
    EmitCommands(*this, sink, configuration);
    }
    BT_ZONE_SCOPE("BR.CullingRecipe.Finalize");
    return sink.FinishRecipe();
}

std::vector<uint64_t> HierarchicalDispatchCullingPass::RecipeRevision(const org::PassPrepareContext& preparation) const
{
    const auto* context = preparation.preparationData ? preparation.preparationData->Get<RenderContext>() : nullptr;
    std::vector<uint64_t> revision{SettingsManager::GetInstance().Revision(), m_activeTraversalDepth,
        m_maxVisibleClusters, m_voxelRasterWorkCapacity, context ? context->preparedRasterBucketCount : 0u,
        reinterpret_cast<uintptr_t>(m_pureComputeDispatchCommandSignature.get())};
    const auto layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
    revision.push_back((uint64_t{layout.generation} << 32) | layout.index);
    for (const auto* pipeline : {&m_clearPipelineState, &m_createCommandPipelineState,
        &m_pureComputeBuildDispatchArgsPipelineState, &m_pureComputeBuildDualDispatchArgsPipelineState,
        &m_pureComputeClearTraversalCountersPipelineState, &m_pureComputeBuildReplayDispatchArgsPipelineState,
        &m_pureComputeObjectCullPipelineState, &m_pureComputeReplayNodesPipelineState,
        &m_pureComputeReplayClustersPipelineState, &m_pureComputeTraversePipelineState,
        &m_pureComputeLeafPipelineState, &m_pureComputeClusterPipelineState, &m_pureComputeDenseClusterPipelineState})
        revision.push_back(reinterpret_cast<uintptr_t>(pipeline->GetPayload().get()));
    return revision;
}

HierarchicalDispatchCullingInvocation HierarchicalDispatchCullingPass::PrepareInvocation(
    const HierarchicalDispatchCullingRecipe& recipe, const org::PassPrepareContext& preparation) const
{
    const auto* context = preparation.preparationData ? preparation.preparationData->Get<RenderContext>() : nullptr;
    return PrepareInvocation(recipe,HierarchicalDispatchCullingPreparation{
        .views = context ? std::span<const PreparedViewFrameData>{context->Views()} : std::span<const PreparedViewFrameData>{},
        .publication = context ? context->publishedRendererState : nullptr,
        .renderPhase = m_renderPhase, .clodOnlyWorkloads = m_clodOnlyWorkloads,
        .useShadowCascadeViews = m_useShadowCascadeViews, .rasterOutputKind = m_rasterOutputKind,
        .windCacheGeneration = m_dynamicWindBoundsCacheGeneration, .windCacheEntryCount = m_dynamicWindBoundsCacheEntryCount});
}
HierarchicalDispatchCullingInvocation HierarchicalDispatchCullingPass::PrepareInvocation(
    const HierarchicalDispatchCullingRecipe& recipe, const HierarchicalDispatchCullingPreparation& preparation)
{
    HierarchicalDispatchCullingInvocation invocation;
    invocation.publication = preparation.publication;
    invocation.constants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_GENERATION] = preparation.windCacheGeneration;
    invocation.constants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_ENTRY_COUNT] = preparation.windCacheEntryCount;
    if (recipe.hasObjectCull) {
        invocation.workloads = br::render::PrepareCullingWorkloads(preparation.views, preparation.publication,
            preparation.renderPhase, preparation.clodOnlyWorkloads, preparation.useShadowCascadeViews, preparation.rasterOutputKind,
            kPureComputeObjectCullThreadsPerGroup, "HierarchicalDispatchCullingPass");
        basic_telemetry::SetGauge("SARP.Culling.GraphWorkloadRecords.Dispatch",
            static_cast<std::int64_t>(invocation.workloads.size()));
    }
    return invocation;
}

void HierarchicalDispatchCullingPass::Record(const HierarchicalDispatchCullingRecipe& recipe,
    const HierarchicalDispatchCullingInvocation& invocation, org::PassRecordContext& recording)
{
    RecordWithObjectConstants(recipe,invocation,recording,{});
}
void HierarchicalDispatchCullingPass::RecordWithObjectConstants(const HierarchicalDispatchCullingRecipe& recipe,
    const HierarchicalDispatchCullingInvocation& invocation, org::PassRecordContext& recording, std::span<const uint32_t> selectedConstants)
{
    br::render::RecordPreparedComputeCommands(recipe.prefix, recording, nullptr, {}, invocation.constants);
    auto constants = recipe.objectCullConstants;
    if (!selectedConstants.empty()) {
        if (selectedConstants.size() != constants.size()) throw std::invalid_argument("Culling object constant layout mismatch");
        std::copy(selectedConstants.begin(),selectedConstants.end(),constants.begin());
    }
    for (const auto& patch : recipe.objectCullBindingViews) {
        if (patch.index >= constants.size()) throw std::out_of_range("Culling descriptor constant is out of bounds");
        constants[patch.index] = patch.view ? recording.Resolve(*patch.view).index : UINT32_MAX;
    }
    constants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_GENERATION] =
        invocation.constants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_GENERATION];
    constants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_ENTRY_COUNT] =
        invocation.constants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_ENTRY_COUNT];
    for (const auto& workload : invocation.workloads) {
        constants[CLOD_PC_OBJECT_CULL_ACTIVE_DRAW_COUNT] = workload.activeDrawCount;
        constants[CLOD_PC_OBJECT_CULL_VIEW_DATA_INDEX] = workload.viewDataIndex;
        constants[CLOD_PC_OBJECT_CULL_ACTIVE_DRAW_SET_SRV_INDEX] = workload.activeDrawSetIndicesSRVIndex;
        constants[CLOD_PC_OBJECT_CULL_VISIBILITY_GENERATION_SRV_INDEX] = workload.drawRecordVisibilityGenerationSRVIndex;
        constants[CLOD_PC_OBJECT_CULL_SHADOW_CASTER_CLASS] = workload.shadowCasterClass;
        recording.Commands().PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0,
            NumMiscUintRootConstants, constants.data());
        recording.Commands().Dispatch(workload.dispatchGridX, workload.dispatchGridY, workload.dispatchGridZ);
    }
    br::render::RecordPreparedComputeCommands(recipe.suffix, recording, nullptr, {}, invocation.constants);
}

void HierarchicalDispatchCullingPass::Update(const UpdateExecutionContext& executionContext)
{
    ZoneScopedN("HierarchicalDispatchCullingPass::Update");

    auto* updateContext = executionContext.hostData ? executionContext.hostData->Get<UpdateContext>() : nullptr;
    if (!updateContext) {
        return;
    }
	if (m_hasCachedPageJobDescriptors && m_workGraphComputePageJobDescriptorsBuffer)
		UploadBufferData(&m_cachedPageJobDescriptors, sizeof(m_cachedPageJobDescriptors),
			org::runtime::UploadTarget::FromShared(m_workGraphComputePageJobDescriptorsBuffer), 0);
	if (m_hasCachedVoxelQueueDescriptors && m_voxelRasterQueueDescriptorsBuffer)
		UploadBufferData(&m_cachedVoxelQueueDescriptors, sizeof(m_cachedVoxelQueueDescriptors),
			org::runtime::UploadTarget::FromShared(m_voxelRasterQueueDescriptorsBuffer), 0);

    auto& context = *updateContext;
    // This is a logical-frame cache tag, not a recording side effect. Advance
    // it on the preparation owner so delayed/concurrent recording consumes the
    // generation captured for that frame and never mutates pass state.
    if (m_dynamicWindBoundsCacheBuffer) {
        ++m_dynamicWindBoundsCacheGeneration;
        if (m_dynamicWindBoundsCacheGeneration == 0u ||
            m_dynamicWindBoundsCacheGeneration >= 0x7FFFFFFFu) {
            m_dynamicWindBoundsCacheGeneration = 1u;
        }
    }
    m_declaredResourcesChanged = false;
    {
        ZoneScopedN("HierarchicalDispatchCullingPass::CheckDeclaredDrawSetRevision");
        const uint64_t drawSetRevision = context.publishedRendererState
            ? context.publishedRendererState->activeDrawLists.revision : 0u;
        if (drawSetRevision != m_lastDrawSetDeclarationRevision) {
            ZoneScopedN("HierarchicalDispatchCullingPass::CollectDeclaredDrawSets");
            m_lastDrawSetDeclarationRevision = drawSetRevision;
            const std::vector<uint64_t> currentDrawSetResourceIds = CollectDeclaredDrawSetResourceIds(
                context.publishedRendererState, m_renderPhase, m_clodOnlyWorkloads);
            if (currentDrawSetResourceIds != m_declaredDrawSetResourceIds) {
                m_declaredDrawSetResourceIds = currentDrawSetResourceIds;
                m_declaredResourcesChanged = true;
            }
        }
    }
    {
        ZoneScopedN("HierarchicalDispatchCullingPass::CheckTraversalDepth");
        const uint32_t previousActiveTraversalDepth = m_activeTraversalDepth;
		const auto publishedResidency = context.publishedRendererState
			? context.publishedRendererState->geometryResidency.payload
				.Get<br::render::PublishedGeometryResidencyState>()
			: nullptr;
		const uint32_t publishedDepth = publishedResidency
			? publishedResidency->maxTraversalDepth : 0u;
		m_activeTraversalDepth = publishedDepth;
		if (!publishedResidency) {
			basic_telemetry::AddCounter(
				"BasicRenderer.GeometryResidency.MissingAcceptedPublication");
		}
		basic_telemetry::SetGauge("BasicRenderer.GeometryResidency.PublishedDepth",
			static_cast<std::int64_t>(publishedDepth));
		basic_telemetry::SetGauge("BasicRenderer.GeometryResidency.SelectedDepth",
			static_cast<std::int64_t>(m_activeTraversalDepth));
        if (m_activeTraversalDepth != previousActiveTraversalDepth) {
            m_declaredResourcesChanged = true;
        }
    }
    uint32_t zero = 0u;
    {
        ZoneScopedN("HierarchicalDispatchCullingPass::UploadCounterResets");
        UploadBufferData(&zero, sizeof(uint32_t), org::runtime::UploadTarget::FromShared(m_visibleClustersCounterBuffer), 0);
        if (m_swVisibleClustersCounterBuffer) {
            UploadBufferData(&zero, sizeof(uint32_t), org::runtime::UploadTarget::FromShared(m_swVisibleClustersCounterBuffer), 0);
        }
        if (m_pageJobVisibleClustersCounterBuffer) {
            UploadBufferData(&zero, sizeof(uint32_t), org::runtime::UploadTarget::FromShared(m_pageJobVisibleClustersCounterBuffer), 0);
        }
    }

    const uint32_t frontierCapacity = std::max(1u, m_maxVisibleClusters);
    if (frontierCapacity != m_sizedPureComputeFrontierCapacity) {
        ZoneScopedN("HierarchicalDispatchCullingPass::ResizePureComputeBuffers");
        m_sizedPureComputeFrontierCapacity = frontierCapacity;
        m_pureComputeCurrentNodeFrontierBuffer->ResizeStructured(frontierCapacity);
        m_pureComputeNextNodeFrontierBuffer->ResizeStructured(frontierCapacity);
        m_pureComputeCurrentLeafFrontierBuffer->ResizeStructured(frontierCapacity);
        m_pureComputeNextLeafFrontierBuffer->ResizeStructured(frontierCapacity);
        m_pureComputeClusterFrontierBuffer->ResizeStructured(frontierCapacity);
        m_pureComputeCurrentNodeCounterBuffer->ResizeStructured(1u);
        m_pureComputeNextNodeCounterBuffer->ResizeStructured(1u);
        m_pureComputeCurrentLeafCounterBuffer->ResizeStructured(1u);
        m_pureComputeNextLeafCounterBuffer->ResizeStructured(1u);
        m_pureComputeClusterCounterBuffer->ResizeStructured(1u);
        m_pureComputeNodeDispatchArgsBuffer->ResizeStructured(1u);
        m_pureComputeLeafDispatchArgsBuffer->ResizeStructured(1u);
        m_pureComputeClusterDispatchArgsBuffer->ResizeStructured(1u);
    }

    {
        ZoneScopedN("HierarchicalDispatchCullingPass::UploadPureComputeCounterResets");
        UploadBufferData(&zero, sizeof(uint32_t), org::runtime::UploadTarget::FromShared(m_pureComputeCurrentNodeCounterBuffer), 0);
        UploadBufferData(&zero, sizeof(uint32_t), org::runtime::UploadTarget::FromShared(m_pureComputeNextNodeCounterBuffer), 0);
        UploadBufferData(&zero, sizeof(uint32_t), org::runtime::UploadTarget::FromShared(m_pureComputeCurrentLeafCounterBuffer), 0);
        UploadBufferData(&zero, sizeof(uint32_t), org::runtime::UploadTarget::FromShared(m_pureComputeNextLeafCounterBuffer), 0);
        UploadBufferData(&zero, sizeof(uint32_t), org::runtime::UploadTarget::FromShared(m_pureComputeClusterCounterBuffer), 0);
    }

    bool rebuildViewTables = false;
    {
        ZoneScopedN("HierarchicalDispatchCullingPass::CheckViewResourceRevision");
        const uint64_t viewResourceRevision = context.ViewResourceLayoutRevision();
        rebuildViewTables = viewResourceRevision != m_lastViewResourceLayoutRevision;
        if (rebuildViewTables) {
            m_lastViewResourceLayoutRevision = viewResourceRevision;
        }
    }

    if (rebuildViewTables || m_cachedViewRasterInfo.empty()) {
        ZoneScopedN("HierarchicalDispatchCullingPass::RebuildViewRasterInfo");
        const auto numViews = context.ViewCameraBufferSize();
        std::vector<CLodViewRasterInfo> viewRasterInfo(numViews);
        for (const auto& viewInfo : context.Views()) {
            const auto cameraIndex = viewInfo.cameraBufferIndex;
            if (cameraIndex >= viewRasterInfo.size()) {
                continue;
            }

            CLodViewRasterInfo info{};
            info.scissorMinX = 0;
            info.scissorMinY = 0;

            if (UsesVirtualShadowOutput(m_rasterOutputKind)) {
                const CLodVirtualShadowResolutionConfig virtualShadowConfig = CLodVirtualShadowBuildRuntimeResolutionConfig();
                if (viewInfo.shadow && viewInfo.lightType == Components::LightType::Directional) {
                    info.scissorMaxX = virtualShadowConfig.virtualResolution;
                    info.scissorMaxY = virtualShadowConfig.virtualResolution;
                    info.viewportScaleX = 1.0f;
                    info.viewportScaleY = 1.0f;
                }
                viewRasterInfo[cameraIndex] = info;
                continue;
            }

            if (viewInfo.visibilityBuffer != nullptr) {
                info.visibilityUAVDescriptorIndex = viewInfo.visibilityUAVIndex;
                info.scissorMaxX = viewInfo.visibilityBuffer->GetWidth();
                info.scissorMaxY = viewInfo.visibilityBuffer->GetHeight();
                info.viewportScaleX = 1.0f;
                info.viewportScaleY = 1.0f;
            }

            viewRasterInfo[cameraIndex] = info;
        }

        const bool sizeChanged = m_cachedViewRasterInfo.size() != viewRasterInfo.size();
        m_cachedViewRasterInfo = std::move(viewRasterInfo);
        if (sizeChanged) {
            m_viewRasterInfoBuffer->ResizeStructured(static_cast<uint32_t>(m_cachedViewRasterInfo.size()));
        }
        if (!m_cachedViewRasterInfo.empty()) {
            UploadBufferData(
                m_cachedViewRasterInfo.data(),
                static_cast<uint32_t>(m_cachedViewRasterInfo.size() * sizeof(CLodViewRasterInfo)),
                org::runtime::UploadTarget::FromShared(m_viewRasterInfoBuffer),
                0);
        }
    }

    if (UsesPerViewDepthMapOcclusion(m_rasterOutputKind)) {
        if (rebuildViewTables || !m_hasUploadedViewDepthSrvIndices) {
            ZoneScopedN("HierarchicalDispatchCullingPass::RebuildViewDepthSrvIndices");
            std::vector<CLodViewDepthSRVIndex> viewDepthSrvIndices(CLodMaxViewDepthIndices);
            const bool useHistoryDepth = m_isFirstPass;
            for (uint32_t i = 0; i < CLodMaxViewDepthIndices; ++i) {
                viewDepthSrvIndices[i].cameraBufferIndex = i;
                viewDepthSrvIndices[i].linearDepthSRVIndex = 0;
            }

            for (const auto& view : context.Views()) {
                const uint32_t cameraBufferIndex = view.cameraBufferIndex;
                if (cameraBufferIndex >= CLodMaxViewDepthIndices) {
                    continue;
                }

                const auto linearDepthMap =
                    !useHistoryDepth || static_cast<bool>(view.depthHistory)
                    ? view.linearDepthMap
                    : nullptr;
                if (!linearDepthMap) {
                    continue;
                }

                uint32_t slice = 0;
                if (view.depthBufferArrayIndex >= 0) {
                    slice = static_cast<uint32_t>(view.depthBufferArrayIndex);
                }

                const uint32_t maxSlices = linearDepthMap->GetNumSRVSlices();
                if (maxSlices == 0) {
                    continue;
                }

                slice = (std::min)(slice, maxSlices - 1);
                viewDepthSrvIndices[cameraBufferIndex].cameraBufferIndex = cameraBufferIndex;
                if (slice < view.linearDepthSRVIndices.size())
                    viewDepthSrvIndices[cameraBufferIndex].linearDepthSRVIndex =
                        view.linearDepthSRVIndices[slice];
            }

            m_cachedViewDepthSrvIndices = std::move(viewDepthSrvIndices);
            m_hasUploadedViewDepthSrvIndices = true;
            UploadBufferData(
                m_cachedViewDepthSrvIndices.data(),
                static_cast<uint32_t>(m_cachedViewDepthSrvIndices.size() * sizeof(CLodViewDepthSRVIndex)),
                org::runtime::UploadTarget::FromShared(m_viewDepthSrvIndicesBuffer),
                0);
        }
    }

    {
        ZoneScopedN("HierarchicalDispatchCullingPass::CheckFirstPassWork");
        if (!m_isFirstPass) {
            return;
        }
    }

    {
        ZoneScopedN("HierarchicalDispatchCullingPass::UploadReplayStateReset");
        CLodReplayBufferState replayState{};
        replayState.nodeWriteCount = 0;
        replayState.meshletWriteCount = 0;
        replayState.nodeDropped = 0;
        replayState.meshletDropped = 0;
        replayState.visibleClusterCombinedCount = 0;
        UploadBufferData(
            &replayState,
            sizeof(CLodReplayBufferState),
            org::runtime::UploadTarget::FromShared(m_occlusionReplayStateBuffer),
            0);
    }

    if (IsCLodWorkGraphTelemetryEnabled()) {
        ZoneScopedN("HierarchicalDispatchCullingPass::UploadTelemetryReset");
        m_zeroTelemetryScratch.assign(CLodWorkGraphTelemetryBufferCount, 0u);
        UploadBufferData(
            m_zeroTelemetryScratch.data(),
            static_cast<uint32_t>(m_zeroTelemetryScratch.size() * sizeof(uint32_t)),
            org::runtime::UploadTarget::FromShared(m_workGraphTelemetryBuffer),
            0);
    }
}

bool HierarchicalDispatchCullingPass::DeclaredResourcesChanged() const
{
    return m_declaredResourcesChanged;
}

std::shared_ptr<Resource> HierarchicalDispatchCullingPass::ProvideResource(ResourceIdentifier const& key)
{
    if (key == m_workGraphComputePageJobDescriptorResourceId) {
        return m_workGraphComputePageJobDescriptorsBuffer;
    }

    if (key == m_voxelRasterQueueDescriptorResourceId) {
        return m_voxelRasterQueueDescriptorsBuffer;
    }

    return nullptr;
}

std::vector<ResourceIdentifier> HierarchicalDispatchCullingPass::GetSupportedKeys()
{
    std::vector<ResourceIdentifier> resources;
    if (m_voxelRasterQueueDescriptorsBuffer) {
        resources.emplace_back(m_voxelRasterQueueDescriptorResourceId);
    }
    if (m_workGraphComputePageJobDescriptorsBuffer) {
        resources.emplace_back(m_workGraphComputePageJobDescriptorResourceId);
    }
    return resources;
}
