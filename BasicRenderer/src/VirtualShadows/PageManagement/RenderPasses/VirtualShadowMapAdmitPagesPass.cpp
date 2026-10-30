#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapAdmitPagesPass.h"
#include "Render/PassBuilders.h"

#include "Pipeline/PipelineState/PSOManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include <algorithm>
#include <tracy/Tracy.hpp>

#include "../shaders/PerPassRootConstants/clodVirtualShadowAdmitPagesRootConstants.h"
#include "../shaders/PerPassRootConstants/clodVirtualShadowApplyUpgradesRootConstants.h"

VirtualShadowMapAdmitPagesPass::VirtualShadowMapAdmitPagesPass(
    std::shared_ptr<org::PixelBuffer> pageTableTexture,
    std::shared_ptr<org::Buffer> dirtyPageFlagsBuffer,
    std::vector<std::shared_ptr<org::Buffer>> upgradeInputBuffers,
    std::shared_ptr<org::Buffer> pageMetadataBuffer,
    std::shared_ptr<org::Buffer> clipmapInfoBuffer,
    std::shared_ptr<org::Buffer> compactShadowCamerasBuffer,
    std::shared_ptr<org::Buffer> statsBuffer,
    VirtualShadowUpgradeQueue upgradeQueue)
    : m_pageTableTexture(std::move(pageTableTexture))
    , m_dirtyPageFlagsBuffer(std::move(dirtyPageFlagsBuffer))
    , m_upgradeInputBuffers(std::move(upgradeInputBuffers))
    , m_pageMetadataBuffer(std::move(pageMetadataBuffer))
    , m_clipmapInfoBuffer(std::move(clipmapInfoBuffer))
    , m_compactShadowCamerasBuffer(std::move(compactShadowCamerasBuffer))
    , m_statsBuffer(std::move(statsBuffer))
    , m_upgradeQueue(std::move(upgradeQueue))
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowAdmitPagesCSMain",
        {},
        "CLod.VirtualShadow.AdmitPages.PSO");
    m_applyUpgradesPso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowApplyExactUpgradesCSMain",
        {},
        "CLod.VirtualShadow.ApplyUpgrades.PSO");
}

VirtualShadowMapAdmitPagesBindings VirtualShadowMapAdmitPagesPass::Declare(org::PassBuilder& declaration)
{
    declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    VirtualShadowMapAdmitPagesBindings bindings{
        m_pso,
        m_applyUpgradesPso,
        declaration.UnorderedAccess(m_pageTableTexture, {static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull)}),
        declaration.UnorderedAccess(m_dirtyPageFlagsBuffer),
        declaration.UnorderedAccess(m_pageMetadataBuffer),
        declaration.ShaderResource(m_clipmapInfoBuffer),
        declaration.ShaderResource(m_compactShadowCamerasBuffer),
        declaration.UnorderedAccess(m_statsBuffer)};
    bindings.normalBudget = SettingsManager::GetInstance().getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowPageRenderBudgetSettingName)();
    bindings.upgradeBudget = SettingsManager::GetInstance().getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowUpgradePageRenderBudgetSettingName)();
    bindings.upgradeInputs.reserve(m_upgradeInputBuffers.size());
    for (const auto& buffer : m_upgradeInputBuffers) {
        bindings.upgradeInputs.push_back(declaration.ShaderResource(buffer));
    }
    return bindings;
}

VirtualShadowMapAdmitPagesFrame VirtualShadowMapAdmitPagesPass::Prepare(
    const VirtualShadowMapAdmitPagesBindings&, const org::PassPrepareContext& preparation) const
{
    VirtualShadowMapAdmitPagesFrame frame{};
    const auto upgrades = m_upgradeQueue.Pending();
    frame.pageTableResolution = CLodVirtualShadowBuildRuntimeResolutionConfig().pageTableResolution;
    frame.upgrades.reserve(upgrades.size());
    for (const auto& entry : upgrades) {
        const auto& work = entry->work;
        if (!work.buffer || !work.inputCount) throw std::logic_error("Invalid shadow upgrade publication");
        const auto found = std::find(m_upgradeInputBuffers.begin(), m_upgradeInputBuffers.end(), work.buffer);
        if (found == m_upgradeInputBuffers.end()) throw std::logic_error("Undeclared shadow upgrade input buffer");
        frame.upgrades.push_back({static_cast<uint32_t>(std::distance(m_upgradeInputBuffers.begin(), found)), work.inputCount});
    }
    m_upgradeQueue.Reserve(upgrades, preparation);
    return frame;
}

void VirtualShadowMapAdmitPagesPass::Record(const VirtualShadowMapAdmitPagesBindings& bindings,
    const VirtualShadowMapAdmitPagesFrame& frame, org::PassRecordContext& recording) {
    const uint32_t pageTable = recording.Index(bindings.pageTable);
    const uint32_t dirtyPageFlags = recording.Index(bindings.dirtyPageFlags);
    const uint32_t stats = recording.Index(bindings.stats);
    const uint32_t pageMetadata = recording.Index(bindings.pageMetadata);
    // A step with zero groups still binds and pushes; only its dispatch is skipped. Every step ends in a UAV barrier.
    const auto step = [&](const org::PipelineState& pipeline, const br::render::MiscConstants& constants, uint32_t groups) {
        recording.BindComputePipeline(pipeline);
        br::render::PushMiscConstants(recording, constants);
        if (groups != 0) recording.Commands().Dispatch(groups, 1, 1);
        recording.UavBarrier();
    };
    for (const auto& upgrade : frame.upgrades) {
        br::render::MiscConstants c{};
        c[CLOD_VIRTUAL_SHADOW_APPLY_UPGRADES_INPUTS_DESCRIPTOR_INDEX] = recording.Index(bindings.upgradeInputs[upgrade.inputIndex]);
        c[CLOD_VIRTUAL_SHADOW_APPLY_UPGRADES_INPUT_COUNT] = upgrade.inputCount;
        c[CLOD_VIRTUAL_SHADOW_APPLY_UPGRADES_PAGE_TABLE_DESCRIPTOR_INDEX] = pageTable;
        c[CLOD_VIRTUAL_SHADOW_APPLY_UPGRADES_DIRTY_FLAGS_DESCRIPTOR_INDEX] = dirtyPageFlags;
        c[CLOD_VIRTUAL_SHADOW_APPLY_UPGRADES_STATS_DESCRIPTOR_INDEX] = stats;
        c[CLOD_VIRTUAL_SHADOW_APPLY_UPGRADES_PAGE_METADATA_DESCRIPTOR_INDEX] = pageMetadata;
        c[CLOD_VIRTUAL_SHADOW_APPLY_UPGRADES_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
        step(bindings.applyUpgradesPipeline, c, (upgrade.inputCount + 63u) / 64u);
    }
    const uint32_t pageGroups = (frame.pageTableResolution * frame.pageTableResolution + 63u) / 64u;
    for (uint32_t phaseIteration = 0; phaseIteration < 2; ++phaseIteration) {
        for (uint32_t clipmapIndex = 0; clipmapIndex < CLodVirtualShadowMaxSupportedClipmapCount; ++clipmapIndex) {
            br::render::MiscConstants c{};
            c[CLOD_VIRTUAL_SHADOW_ADMIT_PAGE_TABLE_DESCRIPTOR_INDEX] = pageTable;
            c[CLOD_VIRTUAL_SHADOW_ADMIT_DIRTY_FLAGS_DESCRIPTOR_INDEX] = dirtyPageFlags;
            c[CLOD_VIRTUAL_SHADOW_ADMIT_STATS_DESCRIPTOR_INDEX] = stats;
            c[CLOD_VIRTUAL_SHADOW_ADMIT_PAGE_TABLE_RESOLUTION] = frame.pageTableResolution;
            c[CLOD_VIRTUAL_SHADOW_ADMIT_CLIPMAP_INDEX] = clipmapIndex;
            c[CLOD_VIRTUAL_SHADOW_ADMIT_NORMAL_BUDGET] = bindings.normalBudget;
            c[CLOD_VIRTUAL_SHADOW_ADMIT_UPGRADE_BUDGET] = bindings.upgradeBudget;
            c[CLOD_VIRTUAL_SHADOW_ADMIT_UPGRADE_PHASE] = 1u - phaseIteration;
            step(bindings.admitPipeline, c, pageGroups);
        }
    }
}
