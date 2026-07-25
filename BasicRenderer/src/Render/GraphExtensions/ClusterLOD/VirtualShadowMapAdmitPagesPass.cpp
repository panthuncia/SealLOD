#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapAdmitPagesPass.h"

#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"

#include "../shaders/PerPassRootConstants/clodVirtualShadowAdmitPagesRootConstants.h"

VirtualShadowMapAdmitPagesPass::VirtualShadowMapAdmitPagesPass(
    std::shared_ptr<PixelBuffer> pageTableTexture,
    std::shared_ptr<Buffer> dirtyPageFlagsBuffer,
    std::shared_ptr<Buffer> upgradeInputsBuffer,
    std::shared_ptr<Buffer> upgradeInputCountBuffer,
    std::shared_ptr<Buffer> pageMetadataBuffer,
    std::shared_ptr<Buffer> clipmapInfoBuffer,
    std::shared_ptr<Buffer> compactShadowCamerasBuffer,
    std::shared_ptr<Buffer> directionalPageViewInfoBuffer,
    std::shared_ptr<Buffer> statsBuffer)
    : m_pageTableTexture(std::move(pageTableTexture))
    , m_dirtyPageFlagsBuffer(std::move(dirtyPageFlagsBuffer))
    , m_upgradeInputsBuffer(std::move(upgradeInputsBuffer))
    , m_upgradeInputCountBuffer(std::move(upgradeInputCountBuffer))
    , m_pageMetadataBuffer(std::move(pageMetadataBuffer))
    , m_clipmapInfoBuffer(std::move(clipmapInfoBuffer))
    , m_compactShadowCamerasBuffer(std::move(compactShadowCamerasBuffer))
    , m_directionalPageViewInfoBuffer(std::move(directionalPageViewInfoBuffer))
    , m_statsBuffer(std::move(statsBuffer))
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowAdmitPagesCSMain",
        {},
        "CLod.VirtualShadow.AdmitPages.PSO");
}

void VirtualShadowMapAdmitPagesPass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    builder->WithShaderResource(
            m_upgradeInputsBuffer,
            m_upgradeInputCountBuffer,
            m_clipmapInfoBuffer,
            m_compactShadowCamerasBuffer,
            Builtin::CameraBuffer)
        .WithUnorderedAccess(
            m_pageTableTexture,
            m_dirtyPageFlagsBuffer,
            m_pageMetadataBuffer,
            m_statsBuffer,
            m_directionalPageViewInfoBuffer);
}

void VirtualShadowMapAdmitPagesPass::Setup() {}

PassReturn VirtualShadowMapAdmitPagesPass::Execute(PassExecutionContext& executionContext)
{
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& commandList = executionContext.commandList;
    commandList.SetDescriptorHeaps(
        renderContext->textureDescriptorHeap.GetHandle(),
        renderContext->samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());

    const auto config = CLodVirtualShadowBuildRuntimeResolutionConfig();
    const uint32_t normalBudget =
        SettingsManager::GetInstance().getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowPageRenderBudgetSettingName)();
    const uint32_t upgradeBudget =
        SettingsManager::GetInstance().getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowUpgradePageRenderBudgetSettingName)();
    constexpr uint32_t threadsPerGroup = 64u;
    const uint32_t pageCount = config.pageTableResolution * config.pageTableResolution;

    rhi::GlobalBarrier globalBarrier{};
    globalBarrier.beforeSync = rhi::ResourceSyncState::ComputeShading;
    globalBarrier.afterSync = rhi::ResourceSyncState::ComputeShading;
    globalBarrier.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
    globalBarrier.afterAccess = rhi::ResourceAccessType::UnorderedAccess;
    rhi::BarrierBatch barrierBatch{};
    barrierBatch.globals = rhi::Span<rhi::GlobalBarrier>(&globalBarrier, 1);

    // Apply exact CPU upgrade tokens in this known-always-executed pass before
    // admission. The standalone upload pass owns CPU queue draining; this
    // phase consumes its persistent GPU batch and closes the scheduling gap
    // that previously allowed uploads without page-table application.
    {
        uint32_t rootConstants[NumMiscUintRootConstants] = {};
        rootConstants[CLOD_VIRTUAL_SHADOW_ADMIT_PAGE_TABLE_DESCRIPTOR_INDEX] =
            m_pageTableTexture->GetUAVShaderVisibleInfo(UAVViewType::Texture2DArrayFull, 0).slot.index;
        rootConstants[CLOD_VIRTUAL_SHADOW_ADMIT_DIRTY_FLAGS_DESCRIPTOR_INDEX] =
            m_dirtyPageFlagsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
        rootConstants[CLOD_VIRTUAL_SHADOW_ADMIT_STATS_DESCRIPTOR_INDEX] =
            m_statsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
        rootConstants[CLOD_VIRTUAL_SHADOW_ADMIT_PAGE_TABLE_RESOLUTION] = config.pageTableResolution;
        rootConstants[CLOD_VIRTUAL_SHADOW_ADMIT_UPGRADE_INPUTS_DESCRIPTOR_INDEX] =
            m_upgradeInputsBuffer->GetSRVInfo(0).slot.index;
        rootConstants[CLOD_VIRTUAL_SHADOW_ADMIT_UPGRADE_INPUT_COUNT_DESCRIPTOR_INDEX] =
            m_upgradeInputCountBuffer->GetSRVInfo(0).slot.index;
            rootConstants[CLOD_VIRTUAL_SHADOW_ADMIT_PAGE_METADATA_DESCRIPTOR_INDEX] =
                m_pageMetadataBuffer->GetUAVShaderVisibleInfo(0).slot.index;
            rootConstants[CLOD_VIRTUAL_SHADOW_ADMIT_CLIPMAP_INFO_DESCRIPTOR_INDEX] =
                m_clipmapInfoBuffer->GetSRVInfo(0).slot.index;
            rootConstants[CLOD_VIRTUAL_SHADOW_ADMIT_SHADOW_CAMERAS_DESCRIPTOR_INDEX] =
                m_compactShadowCamerasBuffer->GetSRVInfo(0).slot.index;
            rootConstants[CLOD_VIRTUAL_SHADOW_ADMIT_PAGE_VIEW_INFO_DESCRIPTOR_INDEX] =
                m_directionalPageViewInfoBuffer->GetUAVShaderVisibleInfo(0).slot.index;
        rootConstants[CLOD_VIRTUAL_SHADOW_ADMIT_APPLY_UPGRADES_ONLY] = 1u;
        commandList.PushConstants(
            rhi::ShaderStage::Compute,
            0,
            MiscUintRootSignatureIndex,
            0,
            NumMiscUintRootConstants,
            rootConstants);
        commandList.Dispatch((pageCount + threadsPerGroup - 1u) / threadsPerGroup, 1u, 1u);
        commandList.Barriers(barrierBatch);
    }

    // Upgrade work receives its reserved share first. Dispatching clipmaps
    // individually makes near-clipmap priority deterministic across classes.
    for (uint32_t phaseIteration = 0u; phaseIteration < 2u; ++phaseIteration) {
        const uint32_t upgradePhase = 1u - phaseIteration;
        for (uint32_t clipmapIndex = 0u; clipmapIndex < CLodVirtualShadowMaxSupportedClipmapCount; ++clipmapIndex) {
            uint32_t rootConstants[NumMiscUintRootConstants] = {};
            rootConstants[CLOD_VIRTUAL_SHADOW_ADMIT_PAGE_TABLE_DESCRIPTOR_INDEX] =
                m_pageTableTexture->GetUAVShaderVisibleInfo(UAVViewType::Texture2DArrayFull, 0).slot.index;
            rootConstants[CLOD_VIRTUAL_SHADOW_ADMIT_DIRTY_FLAGS_DESCRIPTOR_INDEX] =
                m_dirtyPageFlagsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
            rootConstants[CLOD_VIRTUAL_SHADOW_ADMIT_STATS_DESCRIPTOR_INDEX] =
                m_statsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
            rootConstants[CLOD_VIRTUAL_SHADOW_ADMIT_PAGE_TABLE_RESOLUTION] = config.pageTableResolution;
            rootConstants[CLOD_VIRTUAL_SHADOW_ADMIT_CLIPMAP_INDEX] = clipmapIndex;
            rootConstants[CLOD_VIRTUAL_SHADOW_ADMIT_NORMAL_BUDGET] = normalBudget;
            rootConstants[CLOD_VIRTUAL_SHADOW_ADMIT_UPGRADE_BUDGET] = upgradeBudget;
            rootConstants[CLOD_VIRTUAL_SHADOW_ADMIT_UPGRADE_PHASE] = upgradePhase;
            rootConstants[CLOD_VIRTUAL_SHADOW_ADMIT_APPLY_UPGRADES_ONLY] = 0u;
            commandList.PushConstants(
                rhi::ShaderStage::Compute,
                0,
                MiscUintRootSignatureIndex,
                0,
                NumMiscUintRootConstants,
                rootConstants);
            commandList.Dispatch((pageCount + threadsPerGroup - 1u) / threadsPerGroup, 1u, 1u);
            commandList.Barriers(barrierBatch);
        }
    }
    return {};
}

void VirtualShadowMapAdmitPagesPass::Cleanup() {}
