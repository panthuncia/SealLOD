#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapAdmitPagesPass.h"

#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
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
        declaration.BindUnorderedAccess(m_pageTableTexture),
        declaration.BindUnorderedAccess(m_dirtyPageFlagsBuffer),
        declaration.BindUnorderedAccess(m_pageMetadataBuffer),
        declaration.BindShaderResource(m_clipmapInfoBuffer),
        declaration.BindShaderResource(m_compactShadowCamerasBuffer),
        declaration.BindUnorderedAccess(m_statsBuffer)};
    bindings.normalBudget = SettingsManager::GetInstance().getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowPageRenderBudgetSettingName)();
    bindings.upgradeBudget = SettingsManager::GetInstance().getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowUpgradePageRenderBudgetSettingName)();
    bindings.upgradeInputs.reserve(m_upgradeInputBuffers.size());
    for (const auto& buffer : m_upgradeInputBuffers) {
        bindings.upgradeInputs.push_back(declaration.BindShaderResource(buffer));
    }
    return bindings;
}

br::render::PreparedComputePipelineSequence VirtualShadowMapAdmitPagesPass::Prepare(
    const VirtualShadowMapAdmitPagesBindings& bindings, const org::PassPrepareContext& preparation) const
{
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    br::render::PreparedComputePipelineSequence data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle();
    data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    const auto upgrades = m_upgradeQueue.Pending();
    const auto config = CLodVirtualShadowBuildRuntimeResolutionConfig();
    const auto srv = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource}).index; };
    const auto uav = [&](org::ResourceBindingToken token, uint32_t variant = UINT32_MAX) { return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess, variant}).index; };
    const auto admitProgram = preparation.CaptureProgramBinding(m_pso);
    const auto append = [&](const org::PreparedProgramBinding& program, std::array<unsigned int, NumMiscUintRootConstants> constants,
        uint32_t groups, bool barrierAfter) {
        br::render::PreparedComputePipelineSequence::Step step{};
        step.program = program.program;
        step.descriptorIndices = program.descriptorIndices;
        step.constants = constants;
        step.groupsX = groups;
        step.uavBarrierAfter = barrierAfter;
        data.steps.push_back(std::move(step));
    };
    std::optional<org::PreparedProgramBinding> upgradeProgram;
    if (!upgrades.empty()) upgradeProgram = preparation.CaptureProgramBinding(m_applyUpgradesPso);
    for (const auto& entry : upgrades) {
        const auto& work = entry->work;
        if (!work.buffer || !work.inputCount) throw std::logic_error("Invalid shadow upgrade publication");
        const auto found = std::find(m_upgradeInputBuffers.begin(), m_upgradeInputBuffers.end(), work.buffer);
        if (found == m_upgradeInputBuffers.end()) throw std::logic_error("Undeclared shadow upgrade input buffer");
        const auto inputIndex = static_cast<size_t>(std::distance(m_upgradeInputBuffers.begin(), found));
        std::array<unsigned int, NumMiscUintRootConstants> c{};
        c[CLOD_VIRTUAL_SHADOW_APPLY_UPGRADES_INPUTS_DESCRIPTOR_INDEX] = srv(bindings.upgradeInputs[inputIndex]);
        c[CLOD_VIRTUAL_SHADOW_APPLY_UPGRADES_INPUT_COUNT] = work.inputCount;
        c[CLOD_VIRTUAL_SHADOW_APPLY_UPGRADES_PAGE_TABLE_DESCRIPTOR_INDEX] = uav(bindings.pageTable, static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull));
        c[CLOD_VIRTUAL_SHADOW_APPLY_UPGRADES_DIRTY_FLAGS_DESCRIPTOR_INDEX] = uav(bindings.dirtyPageFlags);
        c[CLOD_VIRTUAL_SHADOW_APPLY_UPGRADES_STATS_DESCRIPTOR_INDEX] = uav(bindings.stats);
        c[CLOD_VIRTUAL_SHADOW_APPLY_UPGRADES_PAGE_METADATA_DESCRIPTOR_INDEX] = uav(bindings.pageMetadata);
        c[CLOD_VIRTUAL_SHADOW_APPLY_UPGRADES_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
        append(*upgradeProgram, c, (work.inputCount + 63u) / 64u, true);
    }
    const uint32_t pageGroups = (config.pageTableResolution * config.pageTableResolution + 63u) / 64u;
    for (uint32_t phaseIteration = 0; phaseIteration < 2; ++phaseIteration) {
        for (uint32_t clipmapIndex = 0; clipmapIndex < CLodVirtualShadowMaxSupportedClipmapCount; ++clipmapIndex) {
            std::array<unsigned int, NumMiscUintRootConstants> c{};
            c[CLOD_VIRTUAL_SHADOW_ADMIT_PAGE_TABLE_DESCRIPTOR_INDEX] = uav(bindings.pageTable, static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull));
            c[CLOD_VIRTUAL_SHADOW_ADMIT_DIRTY_FLAGS_DESCRIPTOR_INDEX] = uav(bindings.dirtyPageFlags);
            c[CLOD_VIRTUAL_SHADOW_ADMIT_STATS_DESCRIPTOR_INDEX] = uav(bindings.stats);
            c[CLOD_VIRTUAL_SHADOW_ADMIT_PAGE_TABLE_RESOLUTION] = config.pageTableResolution;
            c[CLOD_VIRTUAL_SHADOW_ADMIT_CLIPMAP_INDEX] = clipmapIndex;
            c[CLOD_VIRTUAL_SHADOW_ADMIT_NORMAL_BUDGET] = bindings.normalBudget;
            c[CLOD_VIRTUAL_SHADOW_ADMIT_UPGRADE_BUDGET] = bindings.upgradeBudget;
            c[CLOD_VIRTUAL_SHADOW_ADMIT_UPGRADE_PHASE] = 1u - phaseIteration;
            append(admitProgram, c, pageGroups, true);
        }
    }
    m_upgradeQueue.Reserve(upgrades, preparation);
    return data;
}

void VirtualShadowMapAdmitPagesPass::Record(const VirtualShadowMapAdmitPagesBindings&,
    const br::render::PreparedComputePipelineSequence& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputePipelineSequence(data, recording);
}
