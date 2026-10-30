#include "VirtualGeometry/Culling/RenderPasses/CullingPassCommon.h"
#include "Render/PassBuilders.h"

#include <algorithm>
#include <stdexcept>

#include <tracy/Tracy.hpp>

#include "BasicRenderer/Diagnostics/CLodTelemetry.h"
#include <BasicRenderer/Extensions/Resources/PublishedStateResourceResolver.h>
#include "Render/MemoryIntrospectionAPI.h"
#include "Render/PipelineState.h"
#include "Resources/Buffers/Buffer.h"
#include "Runtime/Settings/SettingsManager.h"
#include "Scene/Objects/IndirectStateArtifacts.h"
#include "../shaders/PerPassRootConstants/clodWorkGraphRootConstants.h"

namespace br::render {

namespace {

// Debug switch: cull against every virtual shadow page, not just dirty ones.
constexpr bool kDisableVirtualShadowDirtyPageCulling = false;

PublishedResourceQuery ActiveDrawListQuery(const CullingPassConfig& config)
{
    PublishedResourceQuery query{};
    query.owner = PublishedFragmentKind::ActiveDrawLists;
    query.usage = PublishedResourceUsage::ActiveDrawList;
    query.renderPhaseHash = config.renderPhase.hash;
    constexpr std::uint64_t clodBit = 1ull << 63u;
    if (config.clodOnlyWorkloads) query.requiredVariantMask = clodBit;
    else query.forbiddenVariantMask = clodBit;
    return query;
}

}

CullingSettings ReadCullingSettings(const CullingPassConfig& config, bool hasReceiverSubpageMask)
{
    auto& settings = SettingsManager::GetInstance();
    CullingSettings result;
    result.telemetry = IsCLodWorkGraphTelemetryEnabled();
    result.occlusion = settings.getSettingGetter<bool>("enableOcclusionCulling")();
    result.predictiveInvalidation =
        settings.getSettingGetter<bool>(CLodDirectionalVirtualShadowPredictiveLodInvalidationSettingName)();
    result.frustumCulling = settings.getSettingGetter<bool>(CLodFrustumCullingSettingName)();
    result.softwareRasterThreshold = std::min(settings.getSettingGetter<uint32_t>(config.UsesVirtualShadowOutput()
        ? CLodVirtualShadowSoftwareRasterDiameterThresholdSettingName
        : CLodSoftwareRasterDiameterThresholdSettingName)(), 0xFFFFu);
    result.forcedTraversalDepth = settings.getSettingGetter<uint32_t>(CLodForceTraversalDepthRootSettingName)();
    result.receiverSubpageMode = hasReceiverSubpageMask
        ? settings.getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowReceiverSubpageModeSettingName)()
        : CLodVirtualShadowReceiverSubpageModeOff;
    return result;
}

uint32_t BuildCullingFlags(const CullingPassConfig& config, const CullingSettings& settings)
{
    uint32_t flags = 0u;
    if (settings.telemetry) flags |= CLOD_WG_FLAG_TELEMETRY_ENABLED;
    if (config.UsesPerViewDepthOcclusion() && settings.occlusion) flags |= CLOD_WG_FLAG_OCCLUSION_ENABLED;
    if (config.UsesSWClassification()) flags |= CLOD_WG_FLAG_SW_RASTER_ENABLED;
    if (config.softwareRaster == CLodSoftwareRasterMode::Compute) flags |= CLOD_WG_FLAG_COMPUTE_SW_RASTER;
    if (config.UsesVirtualShadowOutput()) {
        if (kDisableVirtualShadowDirtyPageCulling) flags |= CLOD_WG_FLAG_DISABLE_SHADOW_DIRTY_PAGE_CULLING;
        if (settings.predictiveInvalidation) flags |= CLOD_WG_FLAG_VSM_PREDICTIVE_LOD_INVALIDATION;
        if (settings.receiverSubpageMode != CLodVirtualShadowReceiverSubpageModeOff)
            flags |= CLOD_WG_FLAG_VSM_RECEIVER_SUBPAGE_MASK;
    }
    if (!settings.frustumCulling) flags |= CLOD_WG_FLAG_DISABLE_FRUSTUM_CULLING;
    flags |= settings.softwareRasterThreshold << CLOD_WG_SW_RASTER_THRESHOLD_SHIFT;
    if (!config.isFirstPass) flags |= CLOD_WG_FLAG_PHASE2;
    return flags;
}

void DeclareCullingWorkloadInputs(org::RenderPassBuilder& builder, const CullingPassConfig& config)
{
    PublishedResourceQuery visibilityGenerationQuery{};
    visibilityGenerationQuery.owner = PublishedFragmentKind::DrawRecords;
    visibilityGenerationQuery.usage = PublishedResourceUsage::ShaderResource;
    visibilityGenerationQuery.requiredVariantMask = kObjectVisibilityGenerationVariant;
    builder.ShaderResource(PublishedStateResourceResolver(
        PublishedStateSource::ProcessSource(), ActiveDrawListQuery(config)));
    builder.ShaderResource(PublishedStateResourceResolver(
        PublishedStateSource::ProcessSource(), visibilityGenerationQuery));
}

bool CullingDrawSetTracker::Update(const std::shared_ptr<const PublishedRendererState>& state,
    const CullingPassConfig& config)
{
    ZoneScopedN("Culling::CheckDeclaredDrawSetRevision");
    const uint64_t revision = state ? state->activeDrawLists.revision : 0u;
    if (revision == m_revision) return false;
    m_revision = revision;

    std::vector<uint64_t> resourceIds;
    {
        ZoneScopedN("Culling::CollectDeclaredDrawSets");
        const auto resources = state && state->resourceCatalog
            ? state->resourceCatalog->FindAll(ActiveDrawListQuery(config))
            : PublishedResourceCatalog::ResourceList{};
        for (const auto& resource : resources) if (resource) resourceIds.push_back(resource->GetGlobalResourceID());
        std::sort(resourceIds.begin(), resourceIds.end());
        resourceIds.erase(std::unique(resourceIds.begin(), resourceIds.end()), resourceIds.end());
    }
    if (resourceIds == m_resourceIds) return false;
    m_resourceIds = std::move(resourceIds);
    return true;
}

void ResetCullingCounter(const std::shared_ptr<org::Buffer>& counter, const CullingUploader& upload)
{
    const uint32_t zero = 0u;
    upload(&zero, sizeof(zero), counter);
}

void ResetOcclusionReplayState(const std::shared_ptr<org::Buffer>& state, const CullingUploader& upload)
{
    const CLodReplayBufferState replayState{};
    upload(&replayState, sizeof(replayState), state);
}

void ResetCullingTelemetry(const std::shared_ptr<org::Buffer>& telemetry, std::vector<uint32_t>& scratch,
    const CullingUploader& upload)
{
    scratch.assign(CLodWorkGraphTelemetryBufferCount, 0u);
    upload(scratch.data(), scratch.size() * sizeof(uint32_t), telemetry);
}

CullingVoxelQueueTable::CullingVoxelQueueTable(const std::string& stablePassIdentifier, const char* debugName,
    bool enabled)
    : m_id(std::string(CLodVoxelRasterQueueDescriptorBufferId) + "." + stablePassIdentifier)
{
    if (!enabled) return;
    m_buffer = CreateAliasedUnmaterializedStructuredBuffer(1u, sizeof(CLodVoxelRasterQueueDescriptors),
        false, false, false, false);
    m_buffer->SetName(debugName);
    org::memory::SetResourceUsageHint(*m_buffer, "Cluster LOD voxel rasterization");
}

std::wstring CullingVoxelQueueTable::ShaderDefine() const
{
    return L"\"" + std::wstring(m_id.begin(), m_id.end()) + L"\"";
}

void CullingVoxelQueueTable::Upload(const CLodVoxelRasterQueueDescriptors& table, const CullingUploader& upload) const
{
    if (m_buffer) upload(&table, sizeof(table), m_buffer);
}

void CullingVoxelQueueTable::ValidateInterface(const org::PipelineResources& resources, bool expectsTable,
    const char* program) const
{
    const org::ResourceIdentifier id{m_id};
    const bool readsTable = std::ranges::find(resources.mandatoryResourceDescriptorSlots, id)
        != resources.mandatoryResourceDescriptorSlots.end();
    if (readsTable != expectsTable) {
        throw std::runtime_error(std::string("CLod culling shader '") + program
            + "' has an inconsistent voxel-output resource interface (expected="
            + (expectsTable ? "true" : "false") + ", reflected=" + (readsTable ? "true" : "false") + ")");
    }
}

}
