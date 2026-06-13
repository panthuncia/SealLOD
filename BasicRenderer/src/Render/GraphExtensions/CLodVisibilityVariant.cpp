#include "Render/GraphExtensions/CLodVisibilityVariant.h"

#include <memory>
#include <stdexcept>

#include "Managers/Singletons/SettingsManager.h"
#include "Render/GraphExtensions/CLodExtension.h"
#include "Render/GraphExtensions/CLodExtensionShared.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesBuildRasterWorkPass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesCreateDispatchArgsPass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesPatchRasterizationPass.h"
#include "RenderPasses/TerrainRvtPasses.h"

void CLodVisibilityVariant::AppendPhase1ReyesRasterPasses(
    CLodExtension& extension,
    const CLodVariantTraits& traits,
    const std::shared_ptr<ResourceGroup>& slabGroup,
    std::vector<RenderGraph::ExternalPassDesc>& outPasses)
{
    AppendReyesRasterPassesForPhase(extension, traits, slabGroup, outPasses, 1u);
}

void CLodVisibilityVariant::AppendPhase2ReyesRasterPasses(
    CLodExtension& extension,
    const CLodVariantTraits& traits,
    const std::shared_ptr<ResourceGroup>& slabGroup,
    std::vector<RenderGraph::ExternalPassDesc>& outPasses)
{
    AppendReyesRasterPassesForPhase(extension, traits, slabGroup, outPasses, 2u);
}

std::string CLodVisibilityVariant::AppendPhase1FineRasterPass(
    CLodExtension& extension,
    const CLodVariantTraits& traits,
    const std::shared_ptr<ResourceGroup>& slabGroup,
    std::vector<RenderGraph::ExternalPassDesc>& outPasses)
{
    return AppendFineRasterPassForPhase(extension, traits, slabGroup, outPasses, 1u);
}

std::string CLodVisibilityVariant::AppendPhase2FineRasterPass(
    CLodExtension& extension,
    const CLodVariantTraits& traits,
    const std::shared_ptr<ResourceGroup>& slabGroup,
    std::vector<RenderGraph::ExternalPassDesc>& outPasses)
{
    return AppendFineRasterPassForPhase(extension, traits, slabGroup, outPasses, 2u);
}

void CLodVisibilityVariant::AppendReyesRasterPassesForPhase(
    CLodExtension& extension,
    const CLodVariantTraits& traits,
    const std::shared_ptr<ResourceGroup>& slabGroup,
    std::vector<RenderGraph::ExternalPassDesc>& outPasses,
    uint32_t phaseIndex)
{
    if (traits.type != CLodExtensionType::VisiblityBuffer) {
        return;
    }

    const auto phaseSuffix = std::to_string(phaseIndex);
    auto diceQueuePhase1CountBuffer = std::shared_ptr<Buffer>{};
    auto diceIndirectArgsBuffer = extension.m_reyesDiceIndirectArgsBuffer;
    auto rasterWorkBuffer = extension.m_reyesRasterWorkBuffer;
    auto rasterWorkCounterBuffer = extension.m_reyesRasterWorkCounterBuffer;
    auto rasterWorkIndirectArgsBuffer = extension.m_reyesRasterWorkIndirectArgsBuffer;
    auto telemetryBuffer = extension.m_reyesTelemetryBufferPhase1;

    if (phaseIndex == 2u) {
        diceQueuePhase1CountBuffer = extension.m_reyesDiceQueuePhase1CountBuffer;
        diceIndirectArgsBuffer = extension.m_reyesDiceIndirectArgsBufferPhase2;
        rasterWorkBuffer = extension.m_reyesRasterWorkBufferPhase2;
        rasterWorkCounterBuffer = extension.m_reyesRasterWorkCounterBufferPhase2;
        rasterWorkIndirectArgsBuffer = extension.m_reyesRasterWorkIndirectArgsBufferPhase2;
        telemetryBuffer = extension.m_reyesTelemetryBufferPhase2;
    }
    else if (phaseIndex != 1u) {
        throw std::runtime_error("Unsupported CLod visibility Reyes raster phase.");
    }

    const bool enablePatchOcclusion =
        traits.usesPhase2OcclusionReplay &&
        SettingsManager::GetInstance().getSettingGetter<bool>("enableOcclusionCulling")() &&
        (phaseIndex == 1u || phaseIndex == 2u);
    const auto viewDepthSrvIndicesBuffer = enablePatchOcclusion
        ? (phaseIndex == 1u ? extension.m_viewDepthSrvIndicesBuffer : extension.m_viewDepthSrvIndicesBufferPhase2)
        : nullptr;
    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, std::string("ReyesBuildRasterWorkPass") + phaseSuffix),
            std::make_shared<ReyesBuildRasterWorkPass>(
                extension.m_reyesDiceQueueBuffer,
                extension.m_reyesDiceQueueCounterBuffer,
                diceQueuePhase1CountBuffer,
                extension.m_reyesTessTableConfigsBuffer,
                rasterWorkBuffer,
                rasterWorkCounterBuffer,
                diceIndirectArgsBuffer,
                telemetryBuffer,
                extension.m_reyesRasterWorkCapacity,
                phaseIndex,
                enablePatchOcclusion ? extension.m_visibleClustersBuffer : nullptr,
                viewDepthSrvIndicesBuffer,
                enablePatchOcclusion ? extension.m_reyesReplayDiceQueueBuffer : nullptr,
                enablePatchOcclusion ? extension.m_reyesReplayDiceQueueCounterBuffer : nullptr,
                enablePatchOcclusion ? extension.m_reyesReplayDiceQueueOverflowBuffer : nullptr,
                extension.m_reyesDiceQueueCapacity,
                slabGroup)));

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, std::string("ReyesCreateRasterWorkDispatchArgsPass") + phaseSuffix),
            std::make_shared<ReyesCreateDispatchArgsPass>(
                rasterWorkCounterBuffer,
                rasterWorkIndirectArgsBuffer)));

    AppendFineRasterPassForPhase(extension, traits, slabGroup, outPasses, phaseIndex);
}

std::string CLodVisibilityVariant::AppendFineRasterPassForPhase(
    CLodExtension& extension,
    const CLodVariantTraits& traits,
    const std::shared_ptr<ResourceGroup>& slabGroup,
    std::vector<RenderGraph::ExternalPassDesc>& outPasses,
    uint32_t phaseIndex)
{
    if (traits.type != CLodExtensionType::VisiblityBuffer) {
        return {};
    }

    const auto phaseSuffix = std::to_string(phaseIndex);
    const auto passName = MakeVariantPassName(traits, std::string("ReyesPatchRasterPass") + phaseSuffix);

    auto rasterWorkBuffer = extension.m_reyesRasterWorkBuffer;
    auto rasterWorkCounterBuffer = extension.m_reyesRasterWorkCounterBuffer;
    auto rasterWorkIndirectArgsBuffer = extension.m_reyesRasterWorkIndirectArgsBuffer;
    auto telemetryBuffer = extension.m_reyesTelemetryBufferPhase1;

    if (phaseIndex == 2u) {
        rasterWorkBuffer = extension.m_reyesRasterWorkBufferPhase2;
        rasterWorkCounterBuffer = extension.m_reyesRasterWorkCounterBufferPhase2;
        rasterWorkIndirectArgsBuffer = extension.m_reyesRasterWorkIndirectArgsBufferPhase2;
        telemetryBuffer = extension.m_reyesTelemetryBufferPhase2;
    }
    else if (phaseIndex != 1u) {
        throw std::runtime_error("Unsupported CLod visibility fine raster phase.");
    }

    auto rasterPassDesc = RenderGraph::ExternalPassDesc::Compute(
        passName,
        std::make_shared<ReyesPatchRasterizationPass>(
                extension.m_visibleClustersBuffer,
                extension.m_reyesDiceQueueBuffer,
                extension.m_reyesDiceQueueCounterBuffer,
                rasterWorkBuffer,
                rasterWorkCounterBuffer,
                extension.m_reyesTessTableConfigsBuffer,
                extension.m_reyesTessTableVerticesBuffer,
                extension.m_reyesTessTableTrianglesBuffer,
                extension.m_viewRasterInfoBuffer,
                rasterWorkIndirectArgsBuffer,
                telemetryBuffer,
                slabGroup,
                extension.m_visibleClusterCapacity,
                phaseIndex,
                CLodReyesPatchVisibilityIndexBase(extension.m_visibleClusterCapacity)));
    rasterPassDesc.At(RenderGraph::ExternalInsertPoint::Before("MaterialHistogramPass"));
    outPasses.push_back(std::move(rasterPassDesc));

    return passName;
}
