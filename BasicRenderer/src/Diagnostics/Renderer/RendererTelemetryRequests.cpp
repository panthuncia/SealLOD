#include <BasicRenderer/Renderer.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <utility>

#include <spdlog/spdlog.h>

#include "../../../generated/BuiltinResources.h"
#include "Scene/ECS/RendererECSManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "OpenRenderGraph/OpenRenderGraph.h"
#include "VirtualGeometry/GraphIntegration/CLodExtensionComponents.h"
#include "BasicRenderer/Diagnostics/CLodTelemetry.h"
#include "Assets/GeometryProcessing/Reyes/ObjectReyesAtlasTelemetry.h"
#include "BasicRenderer/Diagnostics/TerrainRvtTelemetry.h"
#include "Terrain/RenderPasses/TerrainRvtPasses.h"
#include <BasicRenderer/Extensions/ResourceComponent.h>

namespace {
constexpr size_t TerrainRvtTelemetryMipBins = 16u;
constexpr const char* ObjectReyesAtlasTelemetryDebugSettingName = "objectReyesAtlasTelemetryDebug";
struct TerrainRvtTelemetryStatsReadback {
    uint32_t heightRequests;
    uint32_t materialRequests;
    uint32_t requestOverflows;
    uint32_t generatedPages;
    uint32_t allocationFailures;
    uint32_t heightFallbacks;
    uint32_t materialFallbacks;
    uint32_t residentHits;
    uint32_t heightSampleAttempts;
    uint32_t materialSampleAttempts;
    uint32_t heightSampleHits;
    uint32_t materialSampleHits;
    uint32_t heightPageTableMisses;
    uint32_t materialPageTableMisses;
    uint32_t heightComputePageFailures;
    uint32_t materialComputePageFailures;
    uint32_t heightDisabledFallbacks;
    uint32_t materialDisabledFallbacks;
    uint32_t heightForcedFallbacks;
    uint32_t materialForcedFallbacks;
    uint32_t markComputePageFailures;
    uint32_t markWorldRectCalls;
    uint32_t markWorldRectPages;
    uint32_t resolveResidentPages;
    uint32_t generationHeightPages;
    uint32_t generationMaterialPages;
    uint32_t generationCombinedPages;
    uint32_t generationTexels;
    uint32_t materialSampleRequestedPageXor;
    uint32_t materialSampleResidentPageXor;
    uint32_t materialSamplePhysicalPageXor;
    uint32_t materialSampleRequestedPageMin;
    uint32_t materialSampleRequestedPageMax;
    uint32_t materialSampleResidentPageMin;
    uint32_t materialSampleResidentPageMax;
    uint32_t materialSamplePhysicalPageMin;
    uint32_t materialSamplePhysicalPageMax;
    uint32_t materialSampleCoarserResidentHits;
    uint32_t materialSampleAtlasPoolMask;
    uint32_t heightOwnerMismatches;
    uint32_t materialOwnerMismatches;
    uint32_t requestPageTableXor;
    uint32_t requestPageTableMin;
    uint32_t requestPageTableMax;
    uint32_t generationPageTableMin;
    uint32_t generationPageTableMax;
    uint32_t materialSampleAttemptedPageXor;
    uint32_t materialSampleAttemptedPageMin;
    uint32_t materialSampleAttemptedPageMax;
    uint32_t materialSamplePageMissRequestedPageXor;
    uint32_t materialSamplePageMissRequestedPageMin;
    uint32_t materialSamplePageMissRequestedPageMax;
    uint32_t heightSampleAttemptedPageXor;
    uint32_t heightSampleAttemptedPageMin;
    uint32_t heightSampleAttemptedPageMax;
    uint32_t heightSamplePageMissRequestedPageXor;
    uint32_t heightSamplePageMissRequestedPageMin;
    uint32_t heightSamplePageMissRequestedPageMax;
    uint32_t heightFastSampleAttempts;
    uint32_t heightFastSampleHits;
    uint32_t heightFastPageMissRequests;
    uint32_t heightFullSampleAttempts;
    uint32_t heightFullSampleHits;
    uint32_t generationPageTableXor;
    uint32_t generationPhysicalPageXor;
    uint32_t physicalPageOwnerCollisions;
    std::array<uint32_t, TerrainRvtTelemetryMipBins> heightRequestMipHistogram;
    std::array<uint32_t, TerrainRvtTelemetryMipBins> materialRequestMipHistogram;
    std::array<uint32_t, TerrainRvtTelemetryMipBins> heightSampleMipHistogram;
    std::array<uint32_t, TerrainRvtTelemetryMipBins> materialSampleMipHistogram;
    std::array<uint32_t, TerrainRvtTelemetryMipBins> generationMipHistogram;
};

static_assert(sizeof(TerrainRvtTelemetryStatsReadback) == sizeof(uint32_t) * (66u + TerrainRvtTelemetryMipBins * 5u));

std::string FormatTerrainRvtMipHistogram(const std::array<uint32_t, TerrainRvtTelemetryMipBins>& histogram)
{
    std::ostringstream output;
    bool any = false;
    for (size_t i = 0; i < histogram.size(); ++i) {
        if (histogram[i] == 0u) {
            continue;
        }
        if (any) {
            output << ",";
        }
        output << i << ":" << histogram[i];
        any = true;
    }
    return any ? output.str() : "none";
}

}

void Renderer::MaybeRequestObjectReyesAtlasTelemetry() {
    if (!currentRenderGraph) {
        return;
    }

    // Shares the environment override with the shader define in
    // ReyesPatchRasterizationPass, so a benchmark run that cannot reach the debug
    // menu still gets both the compiled-in counters and this readback.
    if (!IsObjectReyesAtlasTelemetryDebugEnabled()) {
        m_loggedObjectReyesAtlasTelemetryEnabled = false;
        return;
    }

    if (!m_loggedObjectReyesAtlasTelemetryEnabled) {
        spdlog::info(
            "SARP Object Reyes atlas shader telemetry debug enabled (setting '{}').",
            ObjectReyesAtlasTelemetryDebugSettingName);
        m_loggedObjectReyesAtlasTelemetryEnabled = true;
    }

    // The first sample waits one interval: at frame 0 no Reyes geometry has
    // streamed in, so it only ever reported zeros. Short benchmark runs set
    // SARP_OBJECT_REYES_ATLAS_TELEMETRY_INTERVAL to sample within their window.
    static const uint64_t captureIntervalFrames = [] {
        char* value = nullptr;
        size_t length = 0;
        _dupenv_s(&value, &length, "SARP_OBJECT_REYES_ATLAS_TELEMETRY_INTERVAL");
        const uint64_t frames = value ? std::strtoull(value, nullptr, 10) : 0u;
        std::free(value);
        return frames ? frames : 300u;
    }();
    const uint64_t lastRequestFrame = m_lastObjectReyesAtlasTelemetryRequestFrame == UINT64_MAX
        ? 0u : m_lastObjectReyesAtlasTelemetryRequestFrame;
    if (m_totalFramesRendered - lastRequestFrame < captureIntervalFrames) {
        return;
    }
    if (m_objectReyesAtlasTelemetryPhase1ReadbackPending ||
        m_objectReyesAtlasTelemetryPhase2ReadbackPending) {
        return;
    }

    auto* readbackService = currentRenderGraph->GetReadbackService();
    if (!readbackService) {
        return;
    }

    auto& world = RendererECSManager::GetInstance().GetWorld();
    const auto visibilityTag = world.component<CLodExtensionVisibilityBufferTag>();

    std::shared_ptr<org::Resource> phase1Resource;
    world.query_builder<const Components::Resource>()
        .with<CLodReyesTelemetryBufferPhase1Tag>()
        .with<CLodExtensionTypeTag>(visibilityTag)
        .build()
        .each([&](const Components::Resource& component) {
            if (!phase1Resource) {
                phase1Resource = component.resource.lock();
            }
        });

    std::shared_ptr<org::Resource> phase2Resource;
    world.query_builder<const Components::Resource>()
        .with<CLodReyesTelemetryBufferPhase2Tag>()
        .with<CLodExtensionTypeTag>(visibilityTag)
        .build()
        .each([&](const Components::Resource& component) {
            if (!phase2Resource) {
                phase2Resource = component.resource.lock();
            }
        });

    if (!phase1Resource && !phase2Resource) {
        return;
    }

    const uint64_t requestedFrame = m_totalFramesRendered;
    m_lastObjectReyesAtlasTelemetryRequestFrame = requestedFrame;

    auto logTelemetry = [requestedFrame](const char* phaseLabel, org::ReadbackCaptureResult&& result) {
        if (result.data.size() < sizeof(CLodReyesTelemetry)) {
            spdlog::warn(
                "SARP Object Reyes atlas shader telemetry: frame={} phase={} payload too small ({} bytes).",
                requestedFrame,
                phaseLabel,
                result.data.size());
            return;
        }

        CLodReyesTelemetry telemetry{};
        std::memcpy(&telemetry, result.data.data(), sizeof(CLodReyesTelemetry));
        const auto normalizeSentinel = [](uint32_t value) {
            return value == 0xFFFFFFFFu ? 0u : value;
        };
        spdlog::info(
            "SARP Object Reyes atlas shader telemetry: frame={} phase={} phaseIndex={} atlasMaterials={} displacementEnabled={} zeroDescriptor={} sourceSamples={} materialSlot=[{},{}] heightDescriptor=[{},{}] sampler=[{},{}] sourceHeightU16=[{},{}] patchSamples={} patchHeightU16=[{},{}] patchUvU16=([{},{}],[{},{}]) pageUvSets=[{},{}] heightUvSet=[{},{}] invalidHeightUvSet={} rasterWork={} patches={} microTris={} classify=[in={} full={} owned={} dropped={}] overflow=[split={} dice={} rasterWorkPatch={} rasterWorkBatch={} microTri={}] rasterCull=[clip={} preArea={} emptyBounds={} zeroMicroTri={} tinyFallback={} nearPlaneQuad={} windingSwap={}] occlusion=[splitTest={} splitDrop={} diceTest={} diceDrop={}] patchDomain=[invalidSplit={} invalidDice={} diced={} splitCollapse={} deepestSplit={} maxSplitPasses={} dicedTriEst={}]",
            requestedFrame,
            phaseLabel,
            telemetry.phaseIndex,
            telemetry.objectReyesAtlasDebugMaterialCount,
            telemetry.objectReyesAtlasDebugDisplacementEnabledCount,
            telemetry.objectReyesAtlasDebugZeroHeightDescriptorCount,
            telemetry.objectReyesAtlasDebugSampleCount,
            normalizeSentinel(telemetry.objectReyesAtlasDebugMinMaterialSlot),
            telemetry.objectReyesAtlasDebugMaxMaterialSlot,
            normalizeSentinel(telemetry.objectReyesAtlasDebugMinHeightDescriptor),
            telemetry.objectReyesAtlasDebugMaxHeightDescriptor,
            normalizeSentinel(telemetry.objectReyesAtlasDebugMinSamplerDescriptor),
            telemetry.objectReyesAtlasDebugMaxSamplerDescriptor,
            normalizeSentinel(telemetry.objectReyesAtlasDebugMinHeightValueU16),
            telemetry.objectReyesAtlasDebugMaxHeightValueU16,
            telemetry.objectReyesAtlasDebugPatchSampleCount,
            normalizeSentinel(telemetry.objectReyesAtlasDebugMinPatchHeightValueU16),
            telemetry.objectReyesAtlasDebugMaxPatchHeightValueU16,
            normalizeSentinel(telemetry.objectReyesAtlasDebugMinPatchUvXU16),
            telemetry.objectReyesAtlasDebugMaxPatchUvXU16,
            normalizeSentinel(telemetry.objectReyesAtlasDebugMinPatchUvYU16),
            telemetry.objectReyesAtlasDebugMaxPatchUvYU16,
            normalizeSentinel(telemetry.objectReyesAtlasDebugMinPageUvSetCount),
            telemetry.objectReyesAtlasDebugMaxPageUvSetCount,
            normalizeSentinel(telemetry.objectReyesAtlasDebugMinHeightUvSetIndex),
            telemetry.objectReyesAtlasDebugMaxHeightUvSetIndex,
            telemetry.objectReyesAtlasDebugInvalidHeightUvSetCount,
            telemetry.rasterWorkEntryCount,
            telemetry.patchRasterizedPatchCount,
            telemetry.patchRasterizedMicroTriangleCount,
            // reyesClassify.hlsl routes every visible cluster to exactly one of
            // the full or owned outputs, unless the destination buffer is full,
            // in which case it returns without emitting anything and the cluster
            // is never drawn by either path. "dropped" is that loss.
            telemetry.visibleClusterInputCount,
            telemetry.fullClusterOutputCount,
            telemetry.ownedClusterOutputCount,
            telemetry.visibleClusterInputCount >
                    telemetry.fullClusterOutputCount + telemetry.ownedClusterOutputCount
                ? telemetry.visibleClusterInputCount -
                    (telemetry.fullClusterOutputCount + telemetry.ownedClusterOutputCount)
                : 0u,
            telemetry.splitQueueOverflowCounts[0] + telemetry.splitQueueOverflowCounts[1] +
                telemetry.splitQueueOverflowCounts[2] + telemetry.splitQueueOverflowCounts[3],
            telemetry.diceQueueOverflowCounts[0] + telemetry.diceQueueOverflowCounts[1] +
                telemetry.diceQueueOverflowCounts[2] + telemetry.diceQueueOverflowCounts[3],
            telemetry.rasterWorkOverflowPatchCount,
            telemetry.rasterWorkOverflowBatchCount,
            telemetry.rasterMicroTriangleOverflowCount,
            // patchRasterizedMicroTriangleCount counts micro-triangles handed to
            // the rasterizer, not visibility-buffer writes. These say how many
            // were thrown away inside it, which is the difference between "Reyes
            // ran" and "Reyes produced pixels".
            telemetry.rasterClipCullCount,
            telemetry.rasterPreAreaCullCount,
            telemetry.rasterEmptyBoundsCullCount,
            telemetry.rasterZeroMicroTriangleCount,
            telemetry.rasterTinyTriangleFallbackCount,
            telemetry.rasterNearPlaneClippedQuadCount,
            telemetry.rasterWindingSwapCount,
            telemetry.splitOcclusionTestCount,
            telemetry.splitOcclusionDropCount,
            telemetry.diceOcclusionTestCount,
            telemetry.diceOcclusionDropCount,
            // The patch domain is what maps a diced micro-triangle back onto its
            // source triangle. A degenerate or out-of-range domain extrapolates
            // the interpolated positions away from the source geometry, which is
            // how a micro-triangle ends up projecting off-screen with real area
            // and no near-plane clip.
            telemetry.invalidSplitPatchDomainCount,
            telemetry.invalidDicePatchDomainCount,
            telemetry.dicedPatchCount,
            telemetry.splitCollapseFallbackDiceCount,
            telemetry.deepestSplitLevelReached,
            telemetry.configuredMaxSplitPassCount,
            telemetry.dicedTriangleEstimateCount);
    };

    if (phase1Resource) {
        m_objectReyesAtlasTelemetryPhase1ReadbackPending = true;
        readbackService->RequestReadbackCaptureAfterGraph(
            phase1Resource.get(),
            org::RangeSpec{},
            [this, logTelemetry](org::ReadbackCaptureResult&& result) mutable {
                m_objectReyesAtlasTelemetryPhase1ReadbackPending = false;
                logTelemetry("phase1", std::move(result));
            });
    }

    if (phase2Resource) {
        m_objectReyesAtlasTelemetryPhase2ReadbackPending = true;
        readbackService->RequestReadbackCaptureAfterGraph(
            phase2Resource.get(),
            org::RangeSpec{},
            [this, logTelemetry](org::ReadbackCaptureResult&& result) mutable {
                m_objectReyesAtlasTelemetryPhase2ReadbackPending = false;
                logTelemetry("phase2", std::move(result));
            });
    }
}

void Renderer::MaybeRequestTerrainRvtTelemetry() {
    if (!currentRenderGraph) {
        return;
    }

    if (!IsTerrainRvtTelemetryDebugEnabled()) {
        m_loggedTerrainRvtTelemetryEnabled = false;
        return;
    }

    if (!SettingsManager::GetInstance().getSettingGetter<bool>("enableTerrainRvt")()) {
        return;
    }

    if (!m_loggedTerrainRvtTelemetryEnabled) {
        spdlog::info(
            "SARP terrain RVT telemetry debug enabled (setting '{}' or SARP_TERRAIN_RVT_TELEMETRY).",
            TerrainRvtTelemetryDebugSettingName);
        m_loggedTerrainRvtTelemetryEnabled = true;
    }

    constexpr uint64_t kCaptureIntervalFrames = 30;
    if (m_lastTerrainRvtTelemetryRequestFrame != UINT64_MAX &&
        m_totalFramesRendered - m_lastTerrainRvtTelemetryRequestFrame < kCaptureIntervalFrames) {
        return;
    }
    if (m_terrainRvtStatsReadbackPending || m_terrainRvtCountersReadbackPending) {
        return;
    }

    auto* readbackService = currentRenderGraph->GetReadbackService();
    if (!readbackService) {
        return;
    }

    auto statsResource = currentRenderGraph->RequestResourcePtr(Builtin::Terrain::RvtStats, /*allowFailure=*/true);
    auto countersResource = currentRenderGraph->RequestResourcePtr(Builtin::Terrain::RvtCounters, /*allowFailure=*/true);
    if (!statsResource || !countersResource) {
        return;
    }

    const uint64_t requestedFrame = m_totalFramesRendered;
    m_lastTerrainRvtTelemetryRequestFrame = requestedFrame;
    m_terrainRvtStatsReadbackPending = true;
    m_terrainRvtCountersReadbackPending = true;
    spdlog::info("SARP terrain RVT telemetry: frame={} queued stats/counters readback.", requestedFrame);

    const auto pageSize = SettingsManager::GetInstance().getSettingGetter<uint32_t>("terrainRvtPageSize")();
    const auto borderTexels = SettingsManager::GetInstance().getSettingGetter<uint32_t>("terrainRvtBorderTexels")();
    const auto atlasPagesWide = TerrainRvt::AtlasPagesWide();
    const auto atlasPagesHigh = TerrainRvt::AtlasPagesHigh();
    const auto atlasPoolCount = TerrainRvt::AtlasPoolCount();
    const auto clipPageTableResolution = TerrainRvt::ClipPageTableResolution();
    const auto maxTerrainSets = TerrainRvt::MaxTerrainSets();
    const auto maxClipLevels = TerrainRvt::MaxClipLevels();
    const auto maxGeneratedPagesPerFrame = TerrainRvt::MaxGeneratedPagesPerFrame();
    const auto mipCount = SettingsManager::GetInstance().getSettingGetter<uint32_t>("terrainRvtMipCount")();
    const auto sourceTexelsPerWorld = TerrainRvt::SourceTexelsPerWorld();
    const auto basePageWorldSize = TerrainRvt::BasePageWorldSize();
    const float mip0TexelWorldSize = basePageWorldSize / static_cast<float>((std::max)(pageSize, 1u));
    const auto forcedFallback = SettingsManager::GetInstance().getSettingGetter<bool>("forceDirectTerrainRvtFallback")();

    readbackService->RequestReadbackCaptureAfterGraph(
            statsResource.get(),
        org::RangeSpec{},
        [this, requestedFrame](org::ReadbackCaptureResult&& result) {
            m_terrainRvtStatsReadbackPending = false;

            if (result.data.size() < sizeof(TerrainRvtTelemetryStatsReadback)) {
                spdlog::warn(
                    "SARP terrain RVT telemetry: frame={} stats payload too small ({} bytes, expected {}).",
                    requestedFrame,
                    result.data.size(),
                    sizeof(TerrainRvtTelemetryStatsReadback));
                return;
            }

            TerrainRvtTelemetryStatsReadback stats{};
            std::memcpy(&stats, result.data.data(), sizeof(stats));
            const uint32_t heightMisses = stats.heightSampleAttempts - (std::min)(stats.heightSampleAttempts, stats.heightSampleHits);
            const uint32_t materialMisses = stats.materialSampleAttempts - (std::min)(stats.materialSampleAttempts, stats.materialSampleHits);
            const auto rangeMinOrZero = [](uint32_t value) {
                return value == 0xffffffffu ? 0u : value;
            };

            spdlog::info(
                "SARP terrain RVT telemetry: frame={} requests(height={} material={} resident_hits={} overflows={} mark_fail={} rect_calls={} rect_pages={}) generation(pages={} height={} material={} combined={} texels={} resident_skips={} alloc_fail={}) samples(height_attempts={} height_hits={} height_misses={} material_attempts={} material_hits={} material_misses={})",
                requestedFrame,
                stats.heightRequests,
                stats.materialRequests,
                stats.residentHits,
                stats.requestOverflows,
                stats.markComputePageFailures,
                stats.markWorldRectCalls,
                stats.markWorldRectPages,
                stats.generatedPages,
                stats.generationHeightPages,
                stats.generationMaterialPages,
                stats.generationCombinedPages,
                stats.generationTexels,
                stats.resolveResidentPages,
                stats.allocationFailures,
                stats.heightSampleAttempts,
                stats.heightSampleHits,
                heightMisses,
                stats.materialSampleAttempts,
                stats.materialSampleHits,
                materialMisses);

            spdlog::info(
                "SARP terrain RVT telemetry fallback: frame={} height(total={} disabled={} forced={} compute_fail={} page_miss={} owner_mismatch={}) material(total={} disabled={} forced={} compute_fail={} page_miss={} owner_mismatch={})",
                requestedFrame,
                stats.heightFallbacks,
                stats.heightDisabledFallbacks,
                stats.heightForcedFallbacks,
                stats.heightComputePageFailures,
                stats.heightPageTableMisses,
                stats.heightOwnerMismatches,
                stats.materialFallbacks,
                stats.materialDisabledFallbacks,
                stats.materialForcedFallbacks,
                stats.materialComputePageFailures,
                stats.materialPageTableMisses,
                stats.materialOwnerMismatches);

            spdlog::info(
                "SARP terrain RVT telemetry mips: frame={} request_height=[{}] request_material=[{}] sample_height=[{}] sample_material=[{}] generated=[{}]",
                requestedFrame,
                FormatTerrainRvtMipHistogram(stats.heightRequestMipHistogram),
                FormatTerrainRvtMipHistogram(stats.materialRequestMipHistogram),
                FormatTerrainRvtMipHistogram(stats.heightSampleMipHistogram),
                FormatTerrainRvtMipHistogram(stats.materialSampleMipHistogram),
                FormatTerrainRvtMipHistogram(stats.generationMipHistogram));

            spdlog::info(
                "SARP terrain RVT telemetry pages: frame={} requested_page_range=[{},{}] resident_page_range=[{},{}] physical_page_range=[{},{}] xor(requested=0x{:08X} resident=0x{:08X} physical=0x{:08X}) coarser_resident_hits={} atlas_pool_mask=0x{:08X}",
                requestedFrame,
                rangeMinOrZero(stats.materialSampleRequestedPageMin),
                stats.materialSampleRequestedPageMax,
                rangeMinOrZero(stats.materialSampleResidentPageMin),
                stats.materialSampleResidentPageMax,
                rangeMinOrZero(stats.materialSamplePhysicalPageMin),
                stats.materialSamplePhysicalPageMax,
                stats.materialSampleRequestedPageXor,
                stats.materialSampleResidentPageXor,
                stats.materialSamplePhysicalPageXor,
                stats.materialSampleCoarserResidentHits,
                stats.materialSampleAtlasPoolMask);

            spdlog::info(
                "SARP terrain RVT telemetry page-domains: frame={} requests=[{},{}] generation=[{},{}] sample_attempts=[{},{}] sample_misses=[{},{}] xor(request=0x{:08X} attempt=0x{:08X} miss=0x{:08X})",
                requestedFrame,
                rangeMinOrZero(stats.requestPageTableMin),
                stats.requestPageTableMax,
                rangeMinOrZero(stats.generationPageTableMin),
                stats.generationPageTableMax,
                rangeMinOrZero(stats.materialSampleAttemptedPageMin),
                stats.materialSampleAttemptedPageMax,
                rangeMinOrZero(stats.materialSamplePageMissRequestedPageMin),
                stats.materialSamplePageMissRequestedPageMax,
                stats.requestPageTableXor,
                stats.materialSampleAttemptedPageXor,
                stats.materialSamplePageMissRequestedPageXor);

            spdlog::info(
                "SARP terrain RVT telemetry height-domain: frame={} attempts=[{},{}] misses=[{},{}] xor(attempt=0x{:08X} miss=0x{:08X}) fast(attempts={} hits={} miss_requests={}) full(attempts={} hits={})",
                requestedFrame,
                rangeMinOrZero(stats.heightSampleAttemptedPageMin),
                stats.heightSampleAttemptedPageMax,
                rangeMinOrZero(stats.heightSamplePageMissRequestedPageMin),
                stats.heightSamplePageMissRequestedPageMax,
                stats.heightSampleAttemptedPageXor,
                stats.heightSamplePageMissRequestedPageXor,
                stats.heightFastSampleAttempts,
                stats.heightFastSampleHits,
                stats.heightFastPageMissRequests,
                stats.heightFullSampleAttempts,
                stats.heightFullSampleHits);

            spdlog::info(
                "SARP terrain RVT telemetry atlas-owner: frame={} owner_collisions={} generation_xor(page=0x{:08X} physical=0x{:08X})",
                requestedFrame,
                stats.physicalPageOwnerCollisions,
                stats.generationPageTableXor,
                stats.generationPhysicalPageXor);
        },
        org::QueueKind::Copy);

    readbackService->RequestReadbackCaptureAfterGraph(
            countersResource.get(),
        org::RangeSpec{},
        [this,
         requestedFrame,
         pageSize,
         borderTexels,
         sourceTexelsPerWorld,
         basePageWorldSize,
         mip0TexelWorldSize,
         atlasPagesWide,
         atlasPagesHigh,
         atlasPoolCount,
         clipPageTableResolution,
         maxTerrainSets,
         maxClipLevels,
         maxGeneratedPagesPerFrame,
         mipCount,
         forcedFallback](org::ReadbackCaptureResult&& result) {
            m_terrainRvtCountersReadbackPending = false;

            constexpr size_t counterBytes = sizeof(uint32_t) * 4u;
            if (result.data.size() < counterBytes) {
                spdlog::warn(
                    "SARP terrain RVT telemetry: frame={} counters payload too small ({} bytes).",
                    requestedFrame,
                    result.data.size());
                return;
            }

            uint32_t counters[4] = {};
            std::memcpy(counters, result.data.data(), counterBytes);
            spdlog::info(
                "SARP terrain RVT telemetry counters: frame={} request_count={} generation_count={} allocated_physical_pages={} counter_overflows={} config(page={} border={} source_texels_per_world={:.3f} clip0_page_world={:.3f} mip0_texel_world={:.5f} atlas={}x{}x{} clip_table={} max_sets={} max_clips={} max_gen_pages={} configured_mips={} forced_fallback={})",
                requestedFrame,
                counters[0],
                counters[1],
                counters[2],
                counters[3],
                pageSize,
                borderTexels,
                sourceTexelsPerWorld,
                basePageWorldSize,
                mip0TexelWorldSize,
                atlasPagesWide,
                atlasPagesHigh,
                atlasPoolCount,
                clipPageTableResolution,
                maxTerrainSets,
                maxClipLevels,
                maxGeneratedPagesPerFrame,
                mipCount,
                forcedFallback ? 1 : 0);
        },
        org::QueueKind::Copy);
}
