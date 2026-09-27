#include <BasicRenderer/Renderer.h>
#include <BasicRenderer/Pipeline/PipelineRecipe.h>
#include <spdlog/spdlog.h>
#include "Runtime/Settings/SettingsManager.h"
#include "Runtime/Settings/RendererSettingsHelpers.h"
#include <exception>
#include <optional>
#include <utility>

bool Renderer::RequestPipelineReplacement(br::pipeline::PipelineRecipe recipe) {
    const auto validation = recipe.Validate();
    if (!validation.valid) {
        for (const auto& error : validation.errors) {
            spdlog::error("Renderer rejected pipeline replacement: {}", error);
        }
        return false;
    }

    std::scoped_lock lock(m_pipelineRecipeMutex);
    m_pendingPipelineRecipe = std::move(recipe);
    return true;
}

br::pipeline::PipelineRecipe Renderer::GetPipelineRecipeForMutation() const {
    std::scoped_lock lock(m_pipelineRecipeMutex);
    return m_pendingPipelineRecipe ? *m_pendingPipelineRecipe : m_pipelineRecipe;
}

void Renderer::ApplyPendingPipelineReplacement() {
    std::optional<br::pipeline::PipelineRecipe> pending;
    {
        std::scoped_lock lock(m_pipelineRecipeMutex);
        pending.swap(m_pendingPipelineRecipe);
    }
    if (!pending) {
        return;
    }

    m_pipelineRollbackRecipe = m_pipelineRecipe;
    m_pipelineRecipe = std::move(*pending);
    m_pipelineExtensionsDirty = true;
    rebuildRenderGraph = true;

    m_syncingPipelineTopologySettings = true;
    auto& settings = SettingsManager::GetInstance();
    settings.getSettingSetter<bool>("enableTerrainRvt")(
        m_pipelineRecipe.Contains<br::pipeline::TerrainRvtTechnique>());
    settings.getSettingSetter<bool>(CLodDisableReyesRasterizationSettingName)(
        m_pipelineRecipe.Options<br::pipeline::ClusterLodTechnique>().reyes == br::pipeline::ReyesMode::Disabled);
    settings.getSettingSetter<bool>("enableGTAO")(
        m_pipelineRecipe.Contains<br::pipeline::GtaoTechnique>());
    settings.getSettingSetter<bool>("enableClusteredLighting")(
        m_pipelineRecipe.Contains<br::pipeline::ClusteredLightingTechnique>());
    settings.getSettingSetter<bool>("enableBloom")(
        m_pipelineRecipe.Contains<br::pipeline::BloomTechnique>());
    settings.getSettingSetter<bool>("enableTerrainRegionMaterialEvaluation")(
        m_pipelineRecipe.Contains<br::pipeline::TerrainRegionMaterialEvaluationTechnique>());
    settings.getSettingSetter<bool>("enableShadows")(
        m_pipelineRecipe.Contains<br::pipeline::ClusterLodShadowTechnique>());
    m_syncingPipelineTopologySettings = false;
}

void Renderer::HandlePipelineReplacementFailure(const std::exception& error) {
    spdlog::error("Renderer pipeline replacement failed: {}", error.what());
    if (m_pipelineReplacementDebugBreakHandler) {
        m_pipelineReplacementDebugBreakHandler();
    }
    if (!m_pipelineRollbackRecipe) {
        throw;
    }

    m_pipelineRecipe = std::move(*m_pipelineRollbackRecipe);
    m_pipelineRollbackRecipe.reset();
    m_pipelineExtensionsDirty = true;
    rebuildRenderGraph = true;
    m_syncingPipelineTopologySettings = true;
    auto& settings = SettingsManager::GetInstance();
    settings.getSettingSetter<bool>("enableTerrainRvt")(
        m_pipelineRecipe.Contains<br::pipeline::TerrainRvtTechnique>());
    settings.getSettingSetter<bool>(CLodDisableReyesRasterizationSettingName)(
        m_pipelineRecipe.Options<br::pipeline::ClusterLodTechnique>().reyes == br::pipeline::ReyesMode::Disabled);
    settings.getSettingSetter<bool>("enableGTAO")(
        m_pipelineRecipe.Contains<br::pipeline::GtaoTechnique>());
    settings.getSettingSetter<bool>("enableClusteredLighting")(
        m_pipelineRecipe.Contains<br::pipeline::ClusteredLightingTechnique>());
    settings.getSettingSetter<bool>("enableBloom")(
        m_pipelineRecipe.Contains<br::pipeline::BloomTechnique>());
    settings.getSettingSetter<bool>("enableTerrainRegionMaterialEvaluation")(
        m_pipelineRecipe.Contains<br::pipeline::TerrainRegionMaterialEvaluationTechnique>());
    settings.getSettingSetter<bool>("enableShadows")(
        m_pipelineRecipe.Contains<br::pipeline::ClusterLodShadowTechnique>());
    m_syncingPipelineTopologySettings = false;
}

