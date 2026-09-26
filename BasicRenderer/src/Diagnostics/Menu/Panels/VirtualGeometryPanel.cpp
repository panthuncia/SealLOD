#include <BasicRenderer/Scene/Scene.h>
#include "Diagnostics/Menu/Menu.h"

void Menu::TryFinalizeCLodCaptureStats(CLodWorkGraphCaptureState& captureState, uint64_t captureId, const char* captureLabel) {
    if (!captureState.captureStatsPending || captureState.captureStatsId != captureId) {
        return;
    }

    if (!captureState.captureHasPendingCounter || !captureState.captureHasPendingClusters) {
        return;
    }

    const uint32_t requestedCount = captureState.capturePendingVisibleCount;
    const uint32_t availableCount = static_cast<uint32_t>(captureState.capturePendingClusters.size());
    const uint32_t decodeCount = (std::min)(requestedCount, availableCount);

    std::unordered_map<uint32_t, uint32_t> viewHistogram;
    std::unordered_map<uint32_t, uint32_t> instanceHistogram;
    std::unordered_set<uint64_t> uniqueMeshlets;

    viewHistogram.reserve(16);
    instanceHistogram.reserve(512);
    uniqueMeshlets.reserve(decodeCount > 0 ? decodeCount : 1);

    for (uint32_t i = 0; i < decodeCount; ++i) {
        const VisibleCluster& cluster = captureState.capturePendingClusters[i];
        viewHistogram[cluster.viewID]++;
        instanceHistogram[cluster.instanceID]++;
        const uint64_t key = (static_cast<uint64_t>(cluster.instanceID) << 32ull) | (static_cast<uint64_t>(cluster.groupID) << 16ull) | static_cast<uint64_t>(cluster.localMeshletIndex);
        uniqueMeshlets.insert(key);
    }

    CLodCaptureStats stats{};
    stats.visibleClusterCount = decodeCount;
    stats.uniqueViews = static_cast<uint32_t>(viewHistogram.size());
    stats.uniqueInstances = static_cast<uint32_t>(instanceHistogram.size());
    stats.uniqueMeshlets = static_cast<uint32_t>(uniqueMeshlets.size());

    for (const auto& [_, count] : viewHistogram) {
        stats.maxClustersPerView = (std::max)(stats.maxClustersPerView, count);
    }
    for (const auto& [_, count] : instanceHistogram) {
        stats.maxClustersPerInstance = (std::max)(stats.maxClustersPerInstance, count);
    }

    if (stats.uniqueViews > 0) {
        stats.avgClustersPerView = static_cast<float>(decodeCount) / static_cast<float>(stats.uniqueViews);
    }
    if (stats.uniqueInstances > 0) {
        stats.avgClustersPerInstance = static_cast<float>(decodeCount) / static_cast<float>(stats.uniqueInstances);
    }
    if (decodeCount > 0) {
        stats.dominantViewPercent = 100.0f * static_cast<float>(stats.maxClustersPerView) / static_cast<float>(decodeCount);
        stats.dominantInstancePercent = 100.0f * static_cast<float>(stats.maxClustersPerInstance) / static_cast<float>(decodeCount);
    }

    captureState.captureStats = stats;
    captureState.captureStatsAvailable = true;
    captureState.captureStatsPending = false;

    spdlog::info(
        "{} stats capture: visible={}, views={}, instances={}, uniqueMeshlets={}, maxPerView={}, maxPerInstance={}",
        captureLabel,
        stats.visibleClusterCount,
        stats.uniqueViews,
        stats.uniqueInstances,
        stats.uniqueMeshlets,
        stats.maxClustersPerView,
        stats.maxClustersPerInstance);
}

void Menu::TryFinalizeCLodAlphaTelemetryCapture(uint64_t captureId) {
    if (!m_clodAlphaTelemetryCapturePending || m_clodAlphaTelemetryCaptureId != captureId) {
        return;
    }

    if (!m_clodAlphaTelemetryHasPendingNodeCount ||
        !m_clodAlphaTelemetryHasPendingOverflow ||
        !m_clodAlphaTelemetryHasPendingStats) {
        return;
    }

    m_clodAlphaNodeCount = m_clodAlphaTelemetryPendingNodeCount;
    m_clodAlphaOverflowCount = m_clodAlphaTelemetryPendingOverflow;
    m_clodAlphaStats = m_clodAlphaTelemetryPendingStats;
    m_clodAlphaTelemetryHasData = true;
    m_clodAlphaTelemetryCapturePending = false;
    m_clodAlphaTelemetryStatus = "Alpha capture completed.";

    spdlog::info(
        "CLod alpha telemetry: nodes={}, overflow={}, truncatedPixels={}, truncatedNodes={}, resolvedSamples={}, maxRaw={}, maxResolved={}",
        m_clodAlphaNodeCount,
        m_clodAlphaOverflowCount,
        m_clodAlphaStats.truncatedPixelCount,
        m_clodAlphaStats.truncatedNodeCount,
        m_clodAlphaStats.totalResolvedSamples,
        m_clodAlphaStats.maxRawNodeCount,
        m_clodAlphaStats.maxResolvedSamples);
}

void Menu::TryFinalizeCLodReyesTelemetryCapture(uint64_t captureId) {
    if (!m_clodReyesTelemetryCapturePending || m_clodReyesTelemetryCaptureId != captureId) {
        return;
    }

    if (!m_clodReyesTelemetryHasPendingPhase1 || !m_clodReyesTelemetryHasPendingPhase2) {
        return;
    }

    m_clodReyesTelemetryPhase1 = m_clodReyesTelemetryPendingPhase1;
    m_clodReyesTelemetryPhase2 = m_clodReyesTelemetryPendingPhase2;
    m_clodReyesTelemetryHasData = true;
    m_clodReyesTelemetryCapturePending = false;
    m_clodReyesTelemetryCaptureCount++;
    m_clodReyesTelemetryStatus = "Reyes capture completed.";

    spdlog::info(
        "Reyes telemetry capture: phase1 input={} owned={} bypass={} totalDice={} splitDepth={} rasterizedPatches={} rasterizedMicros={} | phase2 input={} owned={} bypass={} totalDice={} splitDepth={} rasterizedPatches={} rasterizedMicros={}",
        m_clodReyesTelemetryPhase1.visibleClusterInputCount,
        m_clodReyesTelemetryPhase1.ownedClusterOutputCount,
        m_clodReyesTelemetryPhase1.fullClusterOutputCount,
        m_clodReyesTelemetryPhase1.immediateDiceQueueEntryCount + m_clodReyesTelemetryPhase1.finalDiceQueueEntryCount,
        m_clodReyesTelemetryPhase1.deepestSplitLevelReached,
        m_clodReyesTelemetryPhase1.patchRasterizedPatchCount,
        m_clodReyesTelemetryPhase1.patchRasterizedMicroTriangleCount,
        m_clodReyesTelemetryPhase2.visibleClusterInputCount,
        m_clodReyesTelemetryPhase2.ownedClusterOutputCount,
        m_clodReyesTelemetryPhase2.fullClusterOutputCount,
        m_clodReyesTelemetryPhase2.immediateDiceQueueEntryCount + m_clodReyesTelemetryPhase2.finalDiceQueueEntryCount,
        m_clodReyesTelemetryPhase2.deepestSplitLevelReached,
        m_clodReyesTelemetryPhase2.patchRasterizedPatchCount,
        m_clodReyesTelemetryPhase2.patchRasterizedMicroTriangleCount);
}

void Menu::TryFinalizeCLodVirtualShadowCapture(uint64_t captureId) {
    if (!m_shadowVirtualShadowTelemetry.capturePending || m_shadowVirtualShadowTelemetry.captureId != captureId) {
        return;
    }

    if (!m_shadowVirtualShadowTelemetry.captureHasPendingStats || !m_shadowVirtualShadowTelemetry.captureHasPendingRuntimeState) {
        return;
    }

    m_shadowVirtualShadowTelemetry.capturePending = false;
    m_shadowVirtualShadowTelemetry.hasData = true;
    m_shadowVirtualShadowTelemetry.captureCount++;
    m_shadowVirtualShadowTelemetry.status = "Capture completed.";
}

void Menu::DrawCLodTelemetryWindow() {
    ImGui::Begin("CLod Work Graph Telemetry", nullptr);

    org::Resource* clodTelemetryResource = nullptr;
    org::Resource* shadowClodTelemetryResource = nullptr;
    org::Resource* reyesTelemetryPhase1Resource = nullptr;
    org::Resource* reyesTelemetryPhase2Resource = nullptr;
    org::Resource* shadowReyesTelemetryPhase1Resource = nullptr;
    org::Resource* clodVisibleClustersResource = nullptr;
    org::Resource* clodVisibleCounterResource = nullptr;
    org::Resource* shadowClodVisibleClustersResource = nullptr;
    org::Resource* shadowClodVisibleCounterResource = nullptr;
    org::Resource* shadowVirtualShadowStatsResource = nullptr;
    org::Resource* shadowVirtualShadowRuntimeStateResource = nullptr;
    org::Resource* alphaNodeCounterResource = nullptr;
    org::Resource* alphaOverflowCounterResource = nullptr;
    org::Resource* alphaStatsResource = nullptr;
    {
        m_telemetryQuery.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (clodTelemetryResource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    clodTelemetryResource = resource.get();
                }
            }
            });

        m_shadowTelemetryQuery.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (shadowClodTelemetryResource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    shadowClodTelemetryResource = resource.get();
                }
            }
            });

        m_reyesTelemetryPhase1Query.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (reyesTelemetryPhase1Resource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    reyesTelemetryPhase1Resource = resource.get();
                }
            }
            });

        m_reyesTelemetryPhase2Query.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (reyesTelemetryPhase2Resource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    reyesTelemetryPhase2Resource = resource.get();
                }
            }
            });

        m_shadowReyesTelemetryPhase1Query.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (shadowReyesTelemetryPhase1Resource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    shadowReyesTelemetryPhase1Resource = resource.get();
                }
            }
            });

        m_visibleClustersQuery.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (clodVisibleClustersResource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    clodVisibleClustersResource = resource.get();
                }
            }
            });

        m_visibleCounterQuery.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (clodVisibleCounterResource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    clodVisibleCounterResource = resource.get();
                }
            }
            });

        m_shadowVisibleClustersQuery.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (shadowClodVisibleClustersResource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    shadowClodVisibleClustersResource = resource.get();
                }
            }
            });

        m_shadowVisibleCounterQuery.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (shadowClodVisibleCounterResource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    shadowClodVisibleCounterResource = resource.get();
                }
            }
            });

        m_shadowVirtualShadowStatsQuery.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (shadowVirtualShadowStatsResource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    shadowVirtualShadowStatsResource = resource.get();
                }
            }
            });

        m_shadowVirtualShadowRuntimeStateQuery.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (shadowVirtualShadowRuntimeStateResource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    shadowVirtualShadowRuntimeStateResource = resource.get();
                }
            }
            });

        m_alphaDeepVisibilityCounterQuery.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (alphaNodeCounterResource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    alphaNodeCounterResource = resource.get();
                }
            }
            });

        m_alphaDeepVisibilityOverflowQuery.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (alphaOverflowCounterResource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    alphaOverflowCounterResource = resource.get();
                }
            }
            });

        m_alphaDeepVisibilityStatsQuery.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (alphaStatsResource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    alphaStatsResource = resource.get();
                }
            }
            });
    }

    const bool captureStatsResourcesReady = (clodVisibleClustersResource != nullptr) && (clodVisibleCounterResource != nullptr);
    const bool shadowCaptureStatsResourcesReady = (shadowClodVisibleClustersResource != nullptr) && (shadowClodVisibleCounterResource != nullptr);
    const bool alphaCaptureResourcesReady =
        (alphaNodeCounterResource != nullptr) &&
        (alphaOverflowCounterResource != nullptr) &&
        (alphaStatsResource != nullptr);
    const bool reyesCaptureResourcesReady =
        (reyesTelemetryPhase1Resource != nullptr) &&
        (reyesTelemetryPhase2Resource != nullptr);
    const bool shadowReyesCaptureResourcesReady =
        (shadowReyesTelemetryPhase1Resource != nullptr);
    auto* readbackService = m_renderGraph ? m_renderGraph->GetReadbackService() : nullptr;
    const bool canCapture =
        (clodTelemetryResource != nullptr) &&
        (readbackService != nullptr) &&
        (!m_clodTelemetry.capturePending) &&
        (!m_clodTelemetry.captureStatsPending);
    const bool canCaptureShadow =
        (shadowClodTelemetryResource != nullptr) &&
        (readbackService != nullptr) &&
        (!m_shadowClodTelemetry.capturePending) &&
        (!m_shadowClodTelemetry.captureStatsPending);
    const bool canCaptureVirtualShadow =
        (shadowVirtualShadowStatsResource != nullptr) &&
        (shadowVirtualShadowRuntimeStateResource != nullptr) &&
        (readbackService != nullptr) &&
        (!m_shadowVirtualShadowTelemetry.capturePending);
    const bool canCaptureReyes = reyesCaptureResourcesReady && (readbackService != nullptr) && (!m_clodReyesTelemetryCapturePending);
    const bool canCaptureShadowReyes = shadowReyesCaptureResourcesReady && (readbackService != nullptr) && (!m_shadowClodReyesTelemetryCapturePending);
    const bool canCaptureAlpha = alphaCaptureResourcesReady && (readbackService != nullptr) && (!m_clodAlphaTelemetryCapturePending);

    if (!captureStatsResourcesReady) {
        ImGui::TextDisabled("Primary extended stats unavailable: visible cluster resources not found.");
    }
    if (!shadowCaptureStatsResourcesReady) {
        ImGui::TextDisabled("Shadow extended stats unavailable: visible cluster resources not found.");
    }

    if (!canCapture) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Capture CLod Primary Metrics")) {
        m_clodTelemetry.capturePending = true;
        m_clodTelemetry.status = "Capture requested.";

        const bool requestCaptureStats = captureStatsResourcesReady;
        if (requestCaptureStats) {
            m_clodTelemetry.captureStatsPending = true;
            m_clodTelemetry.captureStatsId++;
            m_clodTelemetry.captureHasPendingCounter = false;
            m_clodTelemetry.captureHasPendingClusters = false;
            m_clodTelemetry.capturePendingVisibleCount = 0;
            m_clodTelemetry.capturePendingClusters.clear();
        }

        if (readbackService) {
            readbackService->RequestReadbackCapture(
                "CLodOpaque::RasterizeClustersPass2",
                clodTelemetryResource,
                org::RangeSpec{},
                [this](org::ReadbackCaptureResult&& result) {
                m_clodTelemetry.capturePending = false;

                constexpr size_t telemetryBytes = sizeof(uint32_t) * static_cast<size_t>(CLodWorkGraphCounterCount);
                if (result.data.size() < telemetryBytes) {
                    m_clodTelemetry.status = "Capture failed: telemetry payload too small.";
                    spdlog::warn("CLod telemetry capture payload too small ({} bytes).", result.data.size());
                    return;
                }

                CLodWorkGraphTelemetryCounters decoded{};
                std::memcpy(decoded.counters.data(), result.data.data(), telemetryBytes);

                m_clodTelemetry.counters = decoded;
                m_clodTelemetry.hasData = true;
                m_clodTelemetry.captureCount++;
                m_clodTelemetry.status = "Capture completed.";

                auto counter = [&](CLodWorkGraphCounterIndex idx) -> uint32_t {
                    return decoded.counters[static_cast<size_t>(idx)];
                    };

                const uint32_t objectThreads = counter(CLodWorkGraphCounterIndex::ObjectCullThreads);
                const uint32_t objectActive = counter(CLodWorkGraphCounterIndex::ObjectCullInRangeThreads);
                const uint32_t traverseThreads = counter(CLodWorkGraphCounterIndex::TraverseNodesThreads);
                const uint32_t traverseActive = counter(CLodWorkGraphCounterIndex::TraverseNodesActiveChildThreads);
                const uint32_t clusterThreads = counter(CLodWorkGraphCounterIndex::ClusterCullThreads);
                const uint32_t clusterActive = counter(CLodWorkGraphCounterIndex::ClusterCullInRangeThreads);
                const uint32_t visibleWrites = counter(CLodWorkGraphCounterIndex::ClusterCullVisibleClusterWrites);
                const uint32_t bucketDispatchRecords = counter(CLodWorkGraphCounterIndex::ClusterCullBucketRecordsDispatched);
                const uint32_t denseExpansionBuckets = counter(CLodWorkGraphCounterIndex::ClusterCullDenseExpansionBuckets);
                const uint32_t denseClustersDispatched = counter(CLodWorkGraphCounterIndex::ClusterCullDenseClustersDispatched);
                const uint32_t replayNodeInput = counter(CLodWorkGraphCounterIndex::Phase2ReplayNodeInputRecords);
                const uint32_t replayMeshletInput = counter(CLodWorkGraphCounterIndex::Phase2ReplayMeshletInputRecords);
                const uint32_t voxelLeaves = counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelLeafRecords);
                const uint32_t voxelRejected = counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelRejectedByErrorRecords);
                const uint32_t voxelHits = counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelSegmentPageHits);
                const uint32_t voxelMisses = counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelSegmentPageMisses);
                const uint32_t voxelRasterWork = counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelRasterWorkRecords);
                const uint32_t voxelRasterDropped = counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelRasterWorkDropped);
                const uint32_t sortHistInputs = counter(CLodWorkGraphCounterIndex::RasterSortHistogramInputs);
                const uint32_t sortHistVoxels = counter(CLodWorkGraphCounterIndex::RasterSortHistogramVoxelSkipped);
                const uint32_t sortHistTriangles = counter(CLodWorkGraphCounterIndex::RasterSortHistogramTriangleContributors);
                const uint32_t sortCompactInputs = counter(CLodWorkGraphCounterIndex::RasterSortCompactionInputs);
                const uint32_t sortCompactVoxels = counter(CLodWorkGraphCounterIndex::RasterSortCompactionVoxelSkipped);
                const uint32_t sortCompactTriangles = counter(CLodWorkGraphCounterIndex::RasterSortCompactionTriangleEmitted);
                const uint32_t rasterGroups = counter(CLodWorkGraphCounterIndex::RasterMeshShaderGroups);
                const uint32_t rasterInRange = counter(CLodWorkGraphCounterIndex::RasterMeshShaderInRange);
                const uint32_t rasterInitFailed = counter(CLodWorkGraphCounterIndex::RasterMeshShaderInitFailed);
                const uint32_t rasterOutputTris = counter(CLodWorkGraphCounterIndex::RasterMeshShaderOutputTriangles);
                const char* clusterDispatchMode = (denseExpansionBuckets > 0u || denseClustersDispatched > 0u)
                    ? ((bucketDispatchRecords > 0u) ? "mixed" : "dense")
                    : "bucketed";

                spdlog::info(
                    "CLod WG telemetry: ObjectCull {}/{} active, Traverse {}/{} active-child, voxel(leaves={}, rejected={}, descHit={}, descMiss={}, rasterWork={}, rasterDrop={}), ClusterCull[{}] {}/{} in-range, visible writes {}, dispatch(bucket={}, denseBuckets={}, denseClusters={}), replay(node={}, meshlet={}), sort(hist input={}, hist voxels={}, hist tris={}, compact input={}, compact voxels={}, compact tris={}), raster(groups={}, inRange={}, initFail={}, outTris={})",
                    objectActive,
                    objectThreads,
                    traverseActive,
                    traverseThreads,
                    voxelLeaves,
                    voxelRejected,
                    voxelHits,
                    voxelMisses,
                    voxelRasterWork,
                    voxelRasterDropped,
                    clusterDispatchMode,
                    clusterActive,
                    clusterThreads,
                    visibleWrites,
                    bucketDispatchRecords,
                    denseExpansionBuckets,
                    denseClustersDispatched,
                    replayNodeInput,
                    replayMeshletInput,
                    sortHistInputs,
                    sortHistVoxels,
                    sortHistTriangles,
                    sortCompactInputs,
                    sortCompactVoxels,
                    sortCompactTriangles,
                    rasterGroups,
                    rasterInRange,
                    rasterInitFailed,
                    rasterOutputTris);
                });

        }

        if (requestCaptureStats) {
            const uint64_t captureId = m_clodTelemetry.captureStatsId;

            if (readbackService) {
                readbackService->RequestReadbackCapture(
                    "CLodOpaque::HierarchicalCullingPass2",
                    clodVisibleCounterResource,
                    org::RangeSpec{},
                    [this, captureId](org::ReadbackCaptureResult&& result) {
                    if (!m_clodTelemetry.captureStatsPending || m_clodTelemetry.captureStatsId != captureId) {
                        return;
                    }

                    if (result.data.size() < sizeof(uint32_t)) {
                        m_clodTelemetry.status = "Capture failed: visible counter payload too small.";
                        m_clodTelemetry.captureStatsPending = false;
                        return;
                    }

                    std::memcpy(&m_clodTelemetry.capturePendingVisibleCount, result.data.data(), sizeof(uint32_t));
                    m_clodTelemetry.captureHasPendingCounter = true;
                    TryFinalizeCLodCaptureStats(m_clodTelemetry, captureId, "CLod primary WG");
                    });

                readbackService->RequestReadbackCapture(
                    "CLodOpaque::HierarchicalCullingPass2",
                    clodVisibleClustersResource,
                    org::RangeSpec{},
                    [this, captureId](org::ReadbackCaptureResult&& result) {
                    if (!m_clodTelemetry.captureStatsPending || m_clodTelemetry.captureStatsId != captureId) {
                        return;
                    }

                    const size_t clusterBytes = PackedVisibleClusterStrideBytes;
                    const size_t count = result.data.size() / clusterBytes;
                    m_clodTelemetry.capturePendingClusters.resize(count);
                    if (count > 0) {
                        const std::byte* rawClusters = result.data.data();
                        for (size_t i = 0; i < count; ++i) {
                            m_clodTelemetry.capturePendingClusters[i] = DecodePackedVisibleCluster(rawClusters + i * clusterBytes);
                        }
                    }

                    m_clodTelemetry.captureHasPendingClusters = true;
                    TryFinalizeCLodCaptureStats(m_clodTelemetry, captureId, "CLod primary WG");
                    });
            }

            m_clodTelemetry.status = "Capture requested (extended stats).";
        }
    }
    if (!canCapture) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::Text("Primary Status: %s", m_clodTelemetry.status.c_str());

    if (!canCaptureShadow) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Capture CLod Shadow Metrics")) {
        m_shadowClodTelemetry.capturePending = true;
        m_shadowClodTelemetry.status = "Capture requested.";

        const bool requestCaptureStats = shadowCaptureStatsResourcesReady;
        if (requestCaptureStats) {
            m_shadowClodTelemetry.captureStatsPending = true;
            m_shadowClodTelemetry.captureStatsId++;
            m_shadowClodTelemetry.captureHasPendingCounter = false;
            m_shadowClodTelemetry.captureHasPendingClusters = false;
            m_shadowClodTelemetry.capturePendingVisibleCount = 0;
            m_shadowClodTelemetry.capturePendingClusters.clear();
        }

        if (readbackService) {
            readbackService->RequestReadbackCapture(
                "CLodShadow::RasterizeClustersPass1",
                shadowClodTelemetryResource,
                org::RangeSpec{},
                [this](org::ReadbackCaptureResult&& result) {
                m_shadowClodTelemetry.capturePending = false;

                constexpr size_t telemetryBytes = sizeof(uint32_t) * static_cast<size_t>(CLodWorkGraphCounterCount);
                if (result.data.size() < telemetryBytes) {
                    m_shadowClodTelemetry.status = "Capture failed: telemetry payload too small.";
                    spdlog::warn("CLod shadow telemetry capture payload too small ({} bytes).", result.data.size());
                    return;
                }

                CLodWorkGraphTelemetryCounters decoded{};
                std::memcpy(decoded.counters.data(), result.data.data(), telemetryBytes);

                m_shadowClodTelemetry.counters = decoded;
                m_shadowClodTelemetry.hasData = true;
                m_shadowClodTelemetry.captureCount++;
                m_shadowClodTelemetry.status = "Capture completed.";

                auto counter = [&](CLodWorkGraphCounterIndex idx) -> uint32_t {
                    return decoded.counters[static_cast<size_t>(idx)];
                    };

                const uint32_t objectThreads = counter(CLodWorkGraphCounterIndex::ObjectCullThreads);
                const uint32_t objectActive = counter(CLodWorkGraphCounterIndex::ObjectCullInRangeThreads);
                const uint32_t traverseThreads = counter(CLodWorkGraphCounterIndex::TraverseNodesThreads);
                const uint32_t traverseActive = counter(CLodWorkGraphCounterIndex::TraverseNodesActiveChildThreads);
                const uint32_t clusterThreads = counter(CLodWorkGraphCounterIndex::ClusterCullThreads);
                const uint32_t clusterActive = counter(CLodWorkGraphCounterIndex::ClusterCullInRangeThreads);
                const uint32_t visibleWrites = counter(CLodWorkGraphCounterIndex::ClusterCullVisibleClusterWrites);
                const uint32_t bucketDispatchRecords = counter(CLodWorkGraphCounterIndex::ClusterCullBucketRecordsDispatched);
                const uint32_t denseExpansionBuckets = counter(CLodWorkGraphCounterIndex::ClusterCullDenseExpansionBuckets);
                const uint32_t denseClustersDispatched = counter(CLodWorkGraphCounterIndex::ClusterCullDenseClustersDispatched);
                const uint32_t replayNodeInput = counter(CLodWorkGraphCounterIndex::Phase2ReplayNodeInputRecords);
                const uint32_t replayMeshletInput = counter(CLodWorkGraphCounterIndex::Phase2ReplayMeshletInputRecords);
                const uint32_t voxelLeaves = counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelLeafRecords);
                const uint32_t voxelRejected = counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelRejectedByErrorRecords);
                const uint32_t voxelHits = counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelSegmentPageHits);
                const uint32_t voxelMisses = counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelSegmentPageMisses);
                const uint32_t voxelRasterWork = counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelRasterWorkRecords);
                const uint32_t voxelRasterDropped = counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelRasterWorkDropped);
                const uint32_t sortHistInputs = counter(CLodWorkGraphCounterIndex::RasterSortHistogramInputs);
                const uint32_t sortHistVoxels = counter(CLodWorkGraphCounterIndex::RasterSortHistogramVoxelSkipped);
                const uint32_t sortHistTriangles = counter(CLodWorkGraphCounterIndex::RasterSortHistogramTriangleContributors);
                const uint32_t sortCompactInputs = counter(CLodWorkGraphCounterIndex::RasterSortCompactionInputs);
                const uint32_t sortCompactVoxels = counter(CLodWorkGraphCounterIndex::RasterSortCompactionVoxelSkipped);
                const uint32_t sortCompactTriangles = counter(CLodWorkGraphCounterIndex::RasterSortCompactionTriangleEmitted);
                const uint32_t rasterGroups = counter(CLodWorkGraphCounterIndex::RasterMeshShaderGroups);
                const uint32_t rasterInRange = counter(CLodWorkGraphCounterIndex::RasterMeshShaderInRange);
                const uint32_t rasterInitFailed = counter(CLodWorkGraphCounterIndex::RasterMeshShaderInitFailed);
                const uint32_t rasterOutputTris = counter(CLodWorkGraphCounterIndex::RasterMeshShaderOutputTriangles);
                const char* clusterDispatchMode = (denseExpansionBuckets > 0u || denseClustersDispatched > 0u)
                    ? ((bucketDispatchRecords > 0u) ? "mixed" : "dense")
                    : "bucketed";

                spdlog::info(
                    "CLod shadow WG telemetry: ObjectCull {}/{} active, Traverse {}/{} active-child, voxel(leaves={}, rejected={}, descHit={}, descMiss={}, rasterWork={}, rasterDrop={}), ClusterCull[{}] {}/{} in-range, visible writes {}, dispatch(bucket={}, denseBuckets={}, denseClusters={}), replay(node={}, meshlet={}), sort(hist input={}, hist voxels={}, hist tris={}, compact input={}, compact voxels={}, compact tris={}), raster(groups={}, inRange={}, initFail={}, outTris={})",
                    objectActive,
                    objectThreads,
                    traverseActive,
                    traverseThreads,
                    voxelLeaves,
                    voxelRejected,
                    voxelHits,
                    voxelMisses,
                    voxelRasterWork,
                    voxelRasterDropped,
                    clusterDispatchMode,
                    clusterActive,
                    clusterThreads,
                    visibleWrites,
                    bucketDispatchRecords,
                    denseExpansionBuckets,
                    denseClustersDispatched,
                    replayNodeInput,
                    replayMeshletInput,
                    sortHistInputs,
                    sortHistVoxels,
                    sortHistTriangles,
                    sortCompactInputs,
                    sortCompactVoxels,
                    sortCompactTriangles,
                    rasterGroups,
                    rasterInRange,
                    rasterInitFailed,
                    rasterOutputTris);
                });

        }

        if (requestCaptureStats) {
            const uint64_t captureId = m_shadowClodTelemetry.captureStatsId;

            if (readbackService) {
                readbackService->RequestReadbackCapture(
                    "CLodShadow::HierarchicalCullingPass1",
                    shadowClodVisibleCounterResource,
                    org::RangeSpec{},
                    [this, captureId](org::ReadbackCaptureResult&& result) {
                    if (!m_shadowClodTelemetry.captureStatsPending || m_shadowClodTelemetry.captureStatsId != captureId) {
                        return;
                    }

                    if (result.data.size() < sizeof(uint32_t)) {
                        m_shadowClodTelemetry.status = "Capture failed: visible counter payload too small.";
                        m_shadowClodTelemetry.captureStatsPending = false;
                        return;
                    }

                    std::memcpy(&m_shadowClodTelemetry.capturePendingVisibleCount, result.data.data(), sizeof(uint32_t));
                    m_shadowClodTelemetry.captureHasPendingCounter = true;
                    TryFinalizeCLodCaptureStats(m_shadowClodTelemetry, captureId, "CLod shadow WG");
                    });

                readbackService->RequestReadbackCapture(
                    "CLodShadow::HierarchicalCullingPass1",
                    shadowClodVisibleClustersResource,
                    org::RangeSpec{},
                    [this, captureId](org::ReadbackCaptureResult&& result) {
                    if (!m_shadowClodTelemetry.captureStatsPending || m_shadowClodTelemetry.captureStatsId != captureId) {
                        return;
                    }

                    const size_t clusterBytes = PackedVisibleClusterStrideBytes;
                    const size_t count = result.data.size() / clusterBytes;
                    m_shadowClodTelemetry.capturePendingClusters.resize(count);
                    if (count > 0) {
                        const std::byte* rawClusters = result.data.data();
                        for (size_t i = 0; i < count; ++i) {
                            m_shadowClodTelemetry.capturePendingClusters[i] = DecodePackedVisibleCluster(rawClusters + i * clusterBytes);
                        }
                    }

                    m_shadowClodTelemetry.captureHasPendingClusters = true;
                    TryFinalizeCLodCaptureStats(m_shadowClodTelemetry, captureId, "CLod shadow WG");
                    });
            }

            m_shadowClodTelemetry.status = "Capture requested (extended stats).";
        }
    }
    if (!canCaptureShadow) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::Text("Shadow Status: %s", m_shadowClodTelemetry.status.c_str());

    if (!shadowVirtualShadowStatsResource || !shadowVirtualShadowRuntimeStateResource) {
        ImGui::TextDisabled("VSM stats unavailable: required stats/runtime-state resources not found.");
    }

    if (!canCaptureVirtualShadow) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Capture Virtual Shadow Metrics")) {
        m_shadowVirtualShadowTelemetry.capturePending = true;
        m_shadowVirtualShadowTelemetry.captureHasPendingStats = false;
        m_shadowVirtualShadowTelemetry.captureHasPendingRuntimeState = false;
        m_shadowVirtualShadowTelemetry.captureId++;
        m_shadowVirtualShadowTelemetry.status = "Capture requested.";

        const uint64_t captureId = m_shadowVirtualShadowTelemetry.captureId;

        readbackService->RequestReadbackCapture(
            "CLodShadow::VirtualShadowClearDirtyBitsPass",
            shadowVirtualShadowStatsResource,
            org::RangeSpec{},
            [this, captureId](org::ReadbackCaptureResult&& result) {
                if (!m_shadowVirtualShadowTelemetry.capturePending || m_shadowVirtualShadowTelemetry.captureId != captureId) {
                    return;
                }

                if (result.data.size() < sizeof(CLodVirtualShadowStats)) {
                    m_shadowVirtualShadowTelemetry.capturePending = false;
                    m_shadowVirtualShadowTelemetry.status = "Capture failed: VSM stats payload too small.";
                    return;
                }

                std::memcpy(&m_shadowVirtualShadowTelemetry.stats, result.data.data(), sizeof(CLodVirtualShadowStats));
                m_shadowVirtualShadowTelemetry.captureHasPendingStats = true;
                TryFinalizeCLodVirtualShadowCapture(captureId);
            });

        readbackService->RequestReadbackCapture(
            "CLodShadow::VirtualShadowSetupPass",
            shadowVirtualShadowRuntimeStateResource,
            org::RangeSpec{},
            [this, captureId](org::ReadbackCaptureResult&& result) {
                if (!m_shadowVirtualShadowTelemetry.capturePending || m_shadowVirtualShadowTelemetry.captureId != captureId) {
                    return;
                }

                if (result.data.size() < sizeof(CLodVirtualShadowRuntimeState)) {
                    m_shadowVirtualShadowTelemetry.capturePending = false;
                    m_shadowVirtualShadowTelemetry.status = "Capture failed: VSM runtime-state payload too small.";
                    return;
                }

                std::memcpy(&m_shadowVirtualShadowTelemetry.runtimeState, result.data.data(), sizeof(CLodVirtualShadowRuntimeState));
                m_shadowVirtualShadowTelemetry.captureHasPendingRuntimeState = true;
                TryFinalizeCLodVirtualShadowCapture(captureId);
            });
    }
    if (!canCaptureVirtualShadow) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::Text("VSM Status: %s", m_shadowVirtualShadowTelemetry.status.c_str());

    if (!reyesCaptureResourcesReady) {
        ImGui::TextDisabled("Reyes metrics unavailable: phase telemetry resources not found.");
    }

    if (!canCaptureReyes) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Capture Reyes Metrics")) {
        m_clodReyesTelemetryCapturePending = true;
        m_clodReyesTelemetryCaptureId++;
        m_clodReyesTelemetryHasPendingPhase1 = false;
        m_clodReyesTelemetryHasPendingPhase2 = false;
        m_clodReyesTelemetryPendingPhase1 = {};
        m_clodReyesTelemetryPendingPhase2 = {};
        m_clodReyesTelemetryStatus = "Reyes capture requested.";

        const uint64_t captureId = m_clodReyesTelemetryCaptureId;
        readbackService->RequestReadbackCapture(
            "CLodOpaque::ReyesPatchRasterPass1",
            reyesTelemetryPhase1Resource,
            org::RangeSpec{},
            [this, captureId](org::ReadbackCaptureResult&& result) {
                if (!m_clodReyesTelemetryCapturePending || m_clodReyesTelemetryCaptureId != captureId) {
                    return;
                }

                if (result.data.size() < sizeof(CLodReyesTelemetry)) {
                    m_clodReyesTelemetryStatus = "Reyes capture failed: phase 1 payload too small.";
                    m_clodReyesTelemetryCapturePending = false;
                    return;
                }

                std::memcpy(&m_clodReyesTelemetryPendingPhase1, result.data.data(), sizeof(CLodReyesTelemetry));
                m_clodReyesTelemetryHasPendingPhase1 = true;
                TryFinalizeCLodReyesTelemetryCapture(captureId);
            });

        readbackService->RequestReadbackCapture(
            "CLodOpaque::ReyesPatchRasterPass2",
            reyesTelemetryPhase2Resource,
            org::RangeSpec{},
            [this, captureId](org::ReadbackCaptureResult&& result) {
                if (!m_clodReyesTelemetryCapturePending || m_clodReyesTelemetryCaptureId != captureId) {
                    return;
                }

                if (result.data.size() < sizeof(CLodReyesTelemetry)) {
                    m_clodReyesTelemetryStatus = "Reyes capture failed: phase 2 payload too small.";
                    m_clodReyesTelemetryCapturePending = false;
                    return;
                }

                std::memcpy(&m_clodReyesTelemetryPendingPhase2, result.data.data(), sizeof(CLodReyesTelemetry));
                m_clodReyesTelemetryHasPendingPhase2 = true;
                TryFinalizeCLodReyesTelemetryCapture(captureId);
            });
    }
    if (!canCaptureReyes) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::Text("Reyes Status: %s", m_clodReyesTelemetryStatus.c_str());

    if (!shadowReyesCaptureResourcesReady) {
        ImGui::TextDisabled("Shadow Reyes metrics unavailable: phase telemetry resource not found.");
    }

    if (!canCaptureShadowReyes) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Capture Shadow Reyes Metrics")) {
        m_shadowClodReyesTelemetryCapturePending = true;
        m_shadowClodReyesTelemetryCaptureId++;
        m_shadowClodReyesTelemetryPhase1 = {};
        m_shadowClodReyesTelemetryStatus = "Shadow Reyes capture requested.";

        const uint64_t captureId = m_shadowClodReyesTelemetryCaptureId;
        readbackService->RequestReadbackCapture(
            "CLodShadow::VirtualShadowClearDirtyBitsPass",
            shadowReyesTelemetryPhase1Resource,
            org::RangeSpec{},
            [this, captureId](org::ReadbackCaptureResult&& result) {
                if (!m_shadowClodReyesTelemetryCapturePending || m_shadowClodReyesTelemetryCaptureId != captureId) {
                    return;
                }

                if (result.data.size() < sizeof(CLodReyesTelemetry)) {
                    m_shadowClodReyesTelemetryStatus = "Shadow Reyes capture failed: phase 1 payload too small.";
                    m_shadowClodReyesTelemetryCapturePending = false;
                    return;
                }

                std::memcpy(&m_shadowClodReyesTelemetryPhase1, result.data.data(), sizeof(CLodReyesTelemetry));
                m_shadowClodReyesTelemetryHasData = true;
                m_shadowClodReyesTelemetryCapturePending = false;
                m_shadowClodReyesTelemetryCaptureCount++;
                m_shadowClodReyesTelemetryStatus = "Shadow Reyes capture completed.";

                spdlog::info(
                    "Shadow Reyes telemetry capture: phase1 input={} owned={} bypass={} totalDice={} splitDepth={} rasterizedPatches={} rasterizedMicros={}",
                    m_shadowClodReyesTelemetryPhase1.visibleClusterInputCount,
                    m_shadowClodReyesTelemetryPhase1.ownedClusterOutputCount,
                    m_shadowClodReyesTelemetryPhase1.fullClusterOutputCount,
                    m_shadowClodReyesTelemetryPhase1.immediateDiceQueueEntryCount + m_shadowClodReyesTelemetryPhase1.finalDiceQueueEntryCount,
                    m_shadowClodReyesTelemetryPhase1.deepestSplitLevelReached,
                    m_shadowClodReyesTelemetryPhase1.patchRasterizedPatchCount,
                    m_shadowClodReyesTelemetryPhase1.patchRasterizedMicroTriangleCount);
            });
    }
    if (!canCaptureShadowReyes) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::Text("Shadow Reyes Status: %s", m_shadowClodReyesTelemetryStatus.c_str());

    if (!alphaCaptureResourcesReady) {
        ImGui::TextDisabled("Alpha deep-visibility metrics unavailable: required resources not found.");
    }

    if (!canCaptureAlpha) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Capture CLod Alpha Metrics")) {
        m_clodAlphaTelemetryCapturePending = true;
        m_clodAlphaTelemetryCaptureId++;
        m_clodAlphaTelemetryHasPendingNodeCount = false;
        m_clodAlphaTelemetryHasPendingOverflow = false;
        m_clodAlphaTelemetryHasPendingStats = false;
        m_clodAlphaTelemetryPendingNodeCount = 0;
        m_clodAlphaTelemetryPendingOverflow = 0;
        m_clodAlphaTelemetryPendingStats = {};
        m_clodAlphaTelemetryStatus = "Alpha capture requested.";

        const uint64_t captureId = m_clodAlphaTelemetryCaptureId;
        readbackService->RequestReadbackCapture(
            "CLodAlpha::DeepVisibilityResolvePass",
            alphaNodeCounterResource,
            org::RangeSpec{},
            [this, captureId](org::ReadbackCaptureResult&& result) {
                if (!m_clodAlphaTelemetryCapturePending || m_clodAlphaTelemetryCaptureId != captureId) {
                    return;
                }

                if (result.data.size() < sizeof(uint32_t)) {
                    m_clodAlphaTelemetryStatus = "Alpha capture failed: node counter payload too small.";
                    m_clodAlphaTelemetryCapturePending = false;
                    return;
                }

                std::memcpy(&m_clodAlphaTelemetryPendingNodeCount, result.data.data(), sizeof(uint32_t));
                m_clodAlphaTelemetryHasPendingNodeCount = true;
                TryFinalizeCLodAlphaTelemetryCapture(captureId);
            });

        readbackService->RequestReadbackCapture(
            "CLodAlpha::DeepVisibilityResolvePass",
            alphaOverflowCounterResource,
            org::RangeSpec{},
            [this, captureId](org::ReadbackCaptureResult&& result) {
                if (!m_clodAlphaTelemetryCapturePending || m_clodAlphaTelemetryCaptureId != captureId) {
                    return;
                }

                if (result.data.size() < sizeof(uint32_t)) {
                    m_clodAlphaTelemetryStatus = "Alpha capture failed: overflow payload too small.";
                    m_clodAlphaTelemetryCapturePending = false;
                    return;
                }

                std::memcpy(&m_clodAlphaTelemetryPendingOverflow, result.data.data(), sizeof(uint32_t));
                m_clodAlphaTelemetryHasPendingOverflow = true;
                TryFinalizeCLodAlphaTelemetryCapture(captureId);
            });

        readbackService->RequestReadbackCapture(
            "CLodAlpha::DeepVisibilityResolvePass",
            alphaStatsResource,
            org::RangeSpec{},
            [this, captureId](org::ReadbackCaptureResult&& result) {
                if (!m_clodAlphaTelemetryCapturePending || m_clodAlphaTelemetryCaptureId != captureId) {
                    return;
                }

                if (result.data.size() < sizeof(CLodDeepVisibilityStats)) {
                    m_clodAlphaTelemetryStatus = "Alpha capture failed: stats payload too small.";
                    m_clodAlphaTelemetryCapturePending = false;
                    return;
                }

                std::memcpy(&m_clodAlphaTelemetryPendingStats, result.data.data(), sizeof(CLodDeepVisibilityStats));
                m_clodAlphaTelemetryHasPendingStats = true;
                TryFinalizeCLodAlphaTelemetryCapture(captureId);
            });
    }
    if (!canCaptureAlpha) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::Text("Alpha Status: %s", m_clodAlphaTelemetryStatus.c_str());

    {
        CLodStreamingOperationStats latestOps{};
        if (TryReadCLodStreamingOperationStats(m_clodStreamingOpsLastSequence, latestOps)) {
            m_clodStreamingOpsLatest = latestOps;
            m_clodStreamingOpsHistory.push_back({ std::chrono::steady_clock::now(), latestOps });
        }

        CLodDirectionalShadowDebugSnapshot latestShadowDebug{};
        if (TryReadCLodDirectionalShadowDebugSnapshot(m_directionalShadowDebugLastSequence, latestShadowDebug)) {
            m_directionalShadowDebugLatest = latestShadowDebug;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto horizon = std::chrono::seconds(5);
        m_clodStreamingOpsHistory.erase(
            std::remove_if(
                m_clodStreamingOpsHistory.begin(),
                m_clodStreamingOpsHistory.end(),
                [&](const CLodStreamingOpsHistorySample& sample) {
                    return (now - sample.timestamp) > horizon;
                }),
            m_clodStreamingOpsHistory.end());

        CLodStreamingOperationStats max5s{};
        for (const auto& sample : m_clodStreamingOpsHistory) {
            max5s.loadRequested = std::max(max5s.loadRequested, sample.stats.loadRequested);
            max5s.loadUnique = std::max(max5s.loadUnique, sample.stats.loadUnique);
            max5s.loadApplied = std::max(max5s.loadApplied, sample.stats.loadApplied);
            max5s.loadFailed = std::max(max5s.loadFailed, sample.stats.loadFailed);
            max5s.decodedRequests = std::max(max5s.decodedRequests, sample.stats.decodedRequests);
            max5s.queuedLoadRequests = std::max(max5s.queuedLoadRequests, sample.stats.queuedLoadRequests);
            max5s.duplicateRequests = std::max(max5s.duplicateRequests, sample.stats.duplicateRequests);

            max5s.unloadRequested = std::max(max5s.unloadRequested, sample.stats.unloadRequested);
            max5s.unloadUnique = std::max(max5s.unloadUnique, sample.stats.unloadUnique);
            max5s.unloadApplied = std::max(max5s.unloadApplied, sample.stats.unloadApplied);
            max5s.unloadFailed = std::max(max5s.unloadFailed, sample.stats.unloadFailed);

            max5s.pendingCpuRequests = std::max(max5s.pendingCpuRequests, sample.stats.pendingCpuRequests);
            max5s.waitingForPagesRequests = std::max(max5s.waitingForPagesRequests, sample.stats.waitingForPagesRequests);
            max5s.inProgressRequests = std::max(max5s.inProgressRequests, sample.stats.inProgressRequests);
            max5s.diskIoRequests = std::max(max5s.diskIoRequests, sample.stats.diskIoRequests);
            max5s.pendingCommitGroups = std::max(max5s.pendingCommitGroups, sample.stats.pendingCommitGroups);
            max5s.readyCompletions = std::max(max5s.readyCompletions, sample.stats.readyCompletions);
            max5s.preallocationDeferrals = std::max(max5s.preallocationDeferrals, sample.stats.preallocationDeferrals);
            max5s.promotionDeferrals = std::max(max5s.promotionDeferrals, sample.stats.promotionDeferrals);
            max5s.completionSuccess = std::max(max5s.completionSuccess, sample.stats.completionSuccess);
            max5s.completionFailed = std::max(max5s.completionFailed, sample.stats.completionFailed);
            max5s.uploadQueuedGroups = std::max(max5s.uploadQueuedGroups, sample.stats.uploadQueuedGroups);
            max5s.residentGroups = std::max(max5s.residentGroups, sample.stats.residentGroups);
            max5s.residentAllocations = std::max(max5s.residentAllocations, sample.stats.residentAllocations);
            max5s.queuedRequests = std::max(max5s.queuedRequests, sample.stats.queuedRequests);
            max5s.completedResults = std::max(max5s.completedResults, sample.stats.completedResults);
            max5s.residentAllocationBytes = std::max(max5s.residentAllocationBytes, sample.stats.residentAllocationBytes);
            max5s.completedResultBytes = std::max(max5s.completedResultBytes, sample.stats.completedResultBytes);
            max5s.streamedBytesThisFrame = std::max(max5s.streamedBytesThisFrame, sample.stats.streamedBytesThisFrame);
            max5s.uploadQueuedBytes = std::max(max5s.uploadQueuedBytes, sample.stats.uploadQueuedBytes);
            max5s.requestToUploadSamples = std::max(max5s.requestToUploadSamples, sample.stats.requestToUploadSamples);
            max5s.requestToUploadAvgTicks = std::max(max5s.requestToUploadAvgTicks, sample.stats.requestToUploadAvgTicks);
            max5s.requestToUploadWorstTicks = std::max(max5s.requestToUploadWorstTicks, sample.stats.requestToUploadWorstTicks);
            max5s.requestToUploadWorstGroup = sample.stats.requestToUploadWorstTicks >= max5s.requestToUploadWorstTicks
                ? sample.stats.requestToUploadWorstGroup
                : max5s.requestToUploadWorstGroup;
            max5s.requestToResidentSamples = std::max(max5s.requestToResidentSamples, sample.stats.requestToResidentSamples);
            max5s.requestToResidentAvgTicks = std::max(max5s.requestToResidentAvgTicks, sample.stats.requestToResidentAvgTicks);
            max5s.requestToResidentWorstTicks = std::max(max5s.requestToResidentWorstTicks, sample.stats.requestToResidentWorstTicks);
            max5s.requestToResidentWorstGroup = sample.stats.requestToResidentWorstTicks >= max5s.requestToResidentWorstTicks
                ? sample.stats.requestToResidentWorstGroup
                : max5s.requestToResidentWorstGroup;
            max5s.diskQueueToCompleteAvgTicks = std::max(max5s.diskQueueToCompleteAvgTicks, sample.stats.diskQueueToCompleteAvgTicks);
            max5s.diskQueueToCompleteWorstTicks = std::max(max5s.diskQueueToCompleteWorstTicks, sample.stats.diskQueueToCompleteWorstTicks);
            max5s.uploadToResidentAvgTicks = std::max(max5s.uploadToResidentAvgTicks, sample.stats.uploadToResidentAvgTicks);
            max5s.uploadToResidentWorstTicks = std::max(max5s.uploadToResidentWorstTicks, sample.stats.uploadToResidentWorstTicks);
            max5s.commitToResidentAvgTicks = std::max(max5s.commitToResidentAvgTicks, sample.stats.commitToResidentAvgTicks);
            max5s.commitToResidentWorstTicks = std::max(max5s.commitToResidentWorstTicks, sample.stats.commitToResidentWorstTicks);
            max5s.pendingCpuMaxAgeTicks = std::max(max5s.pendingCpuMaxAgeTicks, sample.stats.pendingCpuMaxAgeTicks);
            max5s.pendingCpuMaxAgeGroup = sample.stats.pendingCpuMaxAgeTicks >= max5s.pendingCpuMaxAgeTicks
                ? sample.stats.pendingCpuMaxAgeGroup
                : max5s.pendingCpuMaxAgeGroup;
            max5s.diskIoMaxAgeTicks = std::max(max5s.diskIoMaxAgeTicks, sample.stats.diskIoMaxAgeTicks);
            max5s.diskIoMaxAgeGroup = sample.stats.diskIoMaxAgeTicks >= max5s.diskIoMaxAgeTicks
                ? sample.stats.diskIoMaxAgeGroup
                : max5s.diskIoMaxAgeGroup;
            max5s.pendingCommitMaxAgeTicks = std::max(max5s.pendingCommitMaxAgeTicks, sample.stats.pendingCommitMaxAgeTicks);
            max5s.pendingCommitMaxAgeGroup = sample.stats.pendingCommitMaxAgeTicks >= max5s.pendingCommitMaxAgeTicks
                ? sample.stats.pendingCommitMaxAgeGroup
                : max5s.pendingCommitMaxAgeGroup;
        }

        auto formatBytes = [](uint64_t bytes) {
            const double kib = 1024.0;
            const double mib = kib * 1024.0;
            const double gib = mib * 1024.0;

            if (bytes >= static_cast<uint64_t>(gib)) {
                return std::format("{:.2f} GiB", static_cast<double>(bytes) / gib);
            }
            if (bytes >= static_cast<uint64_t>(mib)) {
                return std::format("{:.2f} MiB", static_cast<double>(bytes) / mib);
            }
            if (bytes >= static_cast<uint64_t>(kib)) {
                return std::format("{:.2f} KiB", static_cast<double>(bytes) / kib);
            }

            return std::format("{} B", bytes);
        };

        ImGui::Separator();
        ImGui::TextUnformatted("Streaming operations (per frame)");
        ImGui::Text("Load: requested=%u unique=%u applied=%u failed=%u",
            m_clodStreamingOpsLatest.loadRequested,
            m_clodStreamingOpsLatest.loadUnique,
            m_clodStreamingOpsLatest.loadApplied,
            m_clodStreamingOpsLatest.loadFailed);
        ImGui::Text("Feedback: decoded=%u queued=%u duplicates=%u",
            m_clodStreamingOpsLatest.decodedRequests,
            m_clodStreamingOpsLatest.queuedLoadRequests,
            m_clodStreamingOpsLatest.duplicateRequests);
        ImGui::Text("Unload: requested=%u unique=%u applied=%u failed=%u",
            m_clodStreamingOpsLatest.unloadRequested,
            m_clodStreamingOpsLatest.unloadUnique,
            m_clodStreamingOpsLatest.unloadApplied,
            m_clodStreamingOpsLatest.unloadFailed);
        ImGui::Text("CPU backlog: pending=%u waitingPages=%u inProgress=%u diskIo=%u readyCompletions=%u commitPending=%u",
            m_clodStreamingOpsLatest.pendingCpuRequests,
            m_clodStreamingOpsLatest.waitingForPagesRequests,
            m_clodStreamingOpsLatest.inProgressRequests,
            m_clodStreamingOpsLatest.diskIoRequests,
            m_clodStreamingOpsLatest.readyCompletions,
            m_clodStreamingOpsLatest.pendingCommitGroups);
        ImGui::Text("Deferrals: prealloc=%u promotion=%u completions ok=%u failed=%u",
            m_clodStreamingOpsLatest.preallocationDeferrals,
            m_clodStreamingOpsLatest.promotionDeferrals,
            m_clodStreamingOpsLatest.completionSuccess,
            m_clodStreamingOpsLatest.completionFailed);
        ImGui::Text("Resident: groups=%u allocations=%u bytes=%s",
            m_clodStreamingOpsLatest.residentGroups,
            m_clodStreamingOpsLatest.residentAllocations,
            formatBytes(m_clodStreamingOpsLatest.residentAllocationBytes).c_str());
        ImGui::Text("Backlog: queued=%u completed=%u completedBytes=%s",
            m_clodStreamingOpsLatest.queuedRequests,
            m_clodStreamingOpsLatest.completedResults,
            formatBytes(m_clodStreamingOpsLatest.completedResultBytes).c_str());
        {
            const double kbPerFrame = static_cast<double>(m_clodStreamingOpsLatest.streamedBytesThisFrame) / 1024.0;
            const float fps = ImGui::GetIO().Framerate;
            const double gbPerSec = (fps > 0.0f)
                ? (static_cast<double>(m_clodStreamingOpsLatest.streamedBytesThisFrame) * static_cast<double>(fps)) / (1024.0 * 1024.0 * 1024.0)
                : 0.0;
            ImGui::Text("Throughput: %.1f KB/frame  %.3f GB/s uploadQueued=%s groups=%u",
                kbPerFrame,
                gbPerSec,
                formatBytes(m_clodStreamingOpsLatest.uploadQueuedBytes).c_str(),
                m_clodStreamingOpsLatest.uploadQueuedGroups);
        }
        ImGui::Text("Latency ticks: req->upload avg=%u worst=%u group=%u samples=%u",
            m_clodStreamingOpsLatest.requestToUploadAvgTicks,
            m_clodStreamingOpsLatest.requestToUploadWorstTicks,
            m_clodStreamingOpsLatest.requestToUploadWorstGroup,
            m_clodStreamingOpsLatest.requestToUploadSamples);
        ImGui::Text("Latency ticks: req->resident avg=%u worst=%u group=%u samples=%u",
            m_clodStreamingOpsLatest.requestToResidentAvgTicks,
            m_clodStreamingOpsLatest.requestToResidentWorstTicks,
            m_clodStreamingOpsLatest.requestToResidentWorstGroup,
            m_clodStreamingOpsLatest.requestToResidentSamples);
        ImGui::Text("Stage ticks: disk avg=%u worst=%u upload->resident avg=%u worst=%u commit->resident avg=%u worst=%u",
            m_clodStreamingOpsLatest.diskQueueToCompleteAvgTicks,
            m_clodStreamingOpsLatest.diskQueueToCompleteWorstTicks,
            m_clodStreamingOpsLatest.uploadToResidentAvgTicks,
            m_clodStreamingOpsLatest.uploadToResidentWorstTicks,
            m_clodStreamingOpsLatest.commitToResidentAvgTicks,
            m_clodStreamingOpsLatest.commitToResidentWorstTicks);
        ImGui::Text("Oldest active: pendingCpu=%u group=%u diskIo=%u group=%u commit=%u group=%u",
            m_clodStreamingOpsLatest.pendingCpuMaxAgeTicks,
            m_clodStreamingOpsLatest.pendingCpuMaxAgeGroup,
            m_clodStreamingOpsLatest.diskIoMaxAgeTicks,
            m_clodStreamingOpsLatest.diskIoMaxAgeGroup,
            m_clodStreamingOpsLatest.pendingCommitMaxAgeTicks,
            m_clodStreamingOpsLatest.pendingCommitMaxAgeGroup);

        ImGui::TextUnformatted("Max in last 5 seconds");
        ImGui::Text("Load max: requested=%u unique=%u applied=%u failed=%u",
            max5s.loadRequested,
            max5s.loadUnique,
            max5s.loadApplied,
            max5s.loadFailed);
        ImGui::Text("Feedback max: decoded=%u queued=%u duplicates=%u",
            max5s.decodedRequests,
            max5s.queuedLoadRequests,
            max5s.duplicateRequests);
        ImGui::Text("Unload max: requested=%u unique=%u applied=%u failed=%u",
            max5s.unloadRequested,
            max5s.unloadUnique,
            max5s.unloadApplied,
            max5s.unloadFailed);
        ImGui::Text("CPU backlog max: pending=%u waitingPages=%u inProgress=%u diskIo=%u readyCompletions=%u commitPending=%u",
            max5s.pendingCpuRequests,
            max5s.waitingForPagesRequests,
            max5s.inProgressRequests,
            max5s.diskIoRequests,
            max5s.readyCompletions,
            max5s.pendingCommitGroups);
        ImGui::Text("Deferrals max: prealloc=%u promotion=%u completions ok=%u failed=%u",
            max5s.preallocationDeferrals,
            max5s.promotionDeferrals,
            max5s.completionSuccess,
            max5s.completionFailed);
        ImGui::Text("Resident max: groups=%u allocations=%u bytes=%s",
            max5s.residentGroups,
            max5s.residentAllocations,
            formatBytes(max5s.residentAllocationBytes).c_str());
        ImGui::Text("Backlog max: queued=%u completed=%u completedBytes=%s",
            max5s.queuedRequests,
            max5s.completedResults,
            formatBytes(max5s.completedResultBytes).c_str());
        {
            const double kbPerFrame = static_cast<double>(max5s.streamedBytesThisFrame) / 1024.0;
            const float fps = ImGui::GetIO().Framerate;
            const double gbPerSec = (fps > 0.0f)
                ? (static_cast<double>(max5s.streamedBytesThisFrame) * static_cast<double>(fps)) / (1024.0 * 1024.0 * 1024.0)
                : 0.0;
            ImGui::Text("Throughput max: %.1f KB/frame  %.3f GB/s uploadQueued=%s groups=%u",
                kbPerFrame,
                gbPerSec,
                formatBytes(max5s.uploadQueuedBytes).c_str(),
                max5s.uploadQueuedGroups);
        }
        ImGui::Text("Latency max ticks: req->upload avg=%u worst=%u group=%u samples=%u",
            max5s.requestToUploadAvgTicks,
            max5s.requestToUploadWorstTicks,
            max5s.requestToUploadWorstGroup,
            max5s.requestToUploadSamples);
        ImGui::Text("Latency max ticks: req->resident avg=%u worst=%u group=%u samples=%u",
            max5s.requestToResidentAvgTicks,
            max5s.requestToResidentWorstTicks,
            max5s.requestToResidentWorstGroup,
            max5s.requestToResidentSamples);
        ImGui::Text("Stage max ticks: disk avg=%u worst=%u upload->resident avg=%u worst=%u commit->resident avg=%u worst=%u",
            max5s.diskQueueToCompleteAvgTicks,
            max5s.diskQueueToCompleteWorstTicks,
            max5s.uploadToResidentAvgTicks,
            max5s.uploadToResidentWorstTicks,
            max5s.commitToResidentAvgTicks,
            max5s.commitToResidentWorstTicks);
        ImGui::Text("Oldest active max: pendingCpu=%u group=%u diskIo=%u group=%u commit=%u group=%u",
            max5s.pendingCpuMaxAgeTicks,
            max5s.pendingCpuMaxAgeGroup,
            max5s.diskIoMaxAgeTicks,
            max5s.diskIoMaxAgeGroup,
            max5s.pendingCommitMaxAgeTicks,
            max5s.pendingCommitMaxAgeGroup);
    }

    const auto drawWorkGraphCaptureSection = [&](const char* title, const CLodWorkGraphCaptureState& captureState) {
        ImGui::Separator();
        ImGui::TextUnformatted(title);

        if (captureState.capturePending) {
            ImGui::Text("Telemetry capture status: pending...");
        }
        else if (!captureState.hasData) {
            ImGui::TextDisabled("No telemetry capture results yet.");
        }
        else {
            auto counter = [&](CLodWorkGraphCounterIndex idx) -> uint32_t {
                return captureState.counters.counters[static_cast<size_t>(idx)];
            };

            auto drawUtilizationRow = [&](const char* label, uint32_t active, uint32_t total) {
                const float efficiency = (total > 0)
                    ? (100.0f * static_cast<float>(active) / static_cast<float>(total))
                    : 0.0f;
                ImGui::Text("%s: %u / %u (%.1f%%)", label, active, total, efficiency);
            };

            ImGui::Text("Telemetry captures: %llu", static_cast<unsigned long long>(captureState.captureCount));
            const uint32_t sourceGroupMismatchCount = counter(CLodWorkGraphCounterIndex::RasterMeshShaderSourceGroupMismatch);
            if (sourceGroupMismatchCount != 0u) {
                ImGui::TextColored(
                    ImVec4(1.0f, 0.15f, 0.10f, 1.0f),
                    "Source group mismatches: %u",
                    sourceGroupMismatchCount);
            }
            else {
                ImGui::Text("Source group mismatches: %u", sourceGroupMismatchCount);
            }
            const uint32_t zeroPageSlabCount = counter(CLodWorkGraphCounterIndex::RasterMeshShaderInitFailedZeroPageSlab);
            if (zeroPageSlabCount != 0u) {
                ImGui::TextColored(
                    ImVec4(1.0f, 0.15f, 0.10f, 1.0f),
                    "Zero page slab: %u",
                    zeroPageSlabCount);
            }
            else {
                ImGui::Text("Zero page slab: %u", zeroPageSlabCount);
            }
            drawUtilizationRow(
                "ObjectCull active draw threads",
                counter(CLodWorkGraphCounterIndex::ObjectCullInRangeThreads),
                counter(CLodWorkGraphCounterIndex::ObjectCullThreads));
            drawUtilizationRow(
                "ObjectCull visible threads",
                counter(CLodWorkGraphCounterIndex::ObjectCullVisibleThreads),
                counter(CLodWorkGraphCounterIndex::ObjectCullInRangeThreads));

            const uint32_t objectCullRejectedFrustum = counter(CLodWorkGraphCounterIndex::ObjectCullRejectedFrustum);
            const uint32_t objectCullInvalidBounds = counter(CLodWorkGraphCounterIndex::ObjectCullInvalidBounds);
            const uint32_t objectCullRejectedTotal = objectCullRejectedFrustum + objectCullInvalidBounds;
            ImGui::Text("ObjectCull rejected: %u", objectCullRejectedTotal);
            if (objectCullRejectedTotal > 0u) {
                auto rejectionRow = [](const char* label, uint32_t count, uint32_t total) {
                    const float pct = (total > 0)
                        ? (100.0f * static_cast<float>(count) / static_cast<float>(total))
                        : 0.0f;
                    ImGui::Text("  %s: %u (%.1f%%)", label, count, pct);
                };
                rejectionRow("Invalid bounds", objectCullInvalidBounds, objectCullRejectedTotal);
                rejectionRow("Frustum reject", objectCullRejectedFrustum, objectCullRejectedTotal);
            }
            drawUtilizationRow(
                "TraverseNodes active child threads",
                counter(CLodWorkGraphCounterIndex::TraverseNodesActiveChildThreads),
                counter(CLodWorkGraphCounterIndex::TraverseNodesThreads));
            drawUtilizationRow(
                "ClusterCull in-range threads",
                counter(CLodWorkGraphCounterIndex::ClusterCullInRangeThreads),
                counter(CLodWorkGraphCounterIndex::ClusterCullThreads));

            const uint32_t bucketDispatchRecords = counter(CLodWorkGraphCounterIndex::ClusterCullBucketRecordsDispatched);
            const uint32_t phase2Records = counter(CLodWorkGraphCounterIndex::ClusterCullDenseExpansionBuckets);
            const uint32_t denseClustersDispatched = counter(CLodWorkGraphCounterIndex::ClusterCullDenseClustersDispatched);
            const bool denseDispatchActive = (phase2Records > 0u || denseClustersDispatched > 0u);
            const char* clusterDispatchMode = denseDispatchActive
                ? ((bucketDispatchRecords > 0u) ? "mixed" : "phase2 records")
                : "bucketed";
            ImGui::Text(
                "ClusterCull dispatch mode: %s | bucket records=%u | phase2 records=%u | phase2 clusters=%u",
                clusterDispatchMode,
                bucketDispatchRecords,
                phase2Records,
                denseClustersDispatched);

            const uint32_t clusterActiveLanes = counter(CLodWorkGraphCounterIndex::ClusterCullActiveLanes);
            const uint32_t clusterSurvivingLanes = counter(CLodWorkGraphCounterIndex::ClusterCullSurvivingLanes);
            drawUtilizationRow("ClusterCull surviving lanes", clusterSurvivingLanes, clusterActiveLanes);

            ImGui::Text("Traverse node records: internal=%u leaf=%u culled=%u rejectedByError=%u",
                counter(CLodWorkGraphCounterIndex::TraverseNodesInternalNodeRecords),
                counter(CLodWorkGraphCounterIndex::TraverseNodesLeafNodeRecords),
                counter(CLodWorkGraphCounterIndex::TraverseNodesCulledNodeRecords),
                counter(CLodWorkGraphCounterIndex::TraverseNodesRejectedByErrorRecords));
			ImGui::Text("Animated node bounds: explicit=%u explicitFrustumReject=%u overflowFallback=%u assemblyFallback=%u invalidFallback=%u",
				counter(CLodWorkGraphCounterIndex::NodeBoundsExplicitEvaluations),
				counter(CLodWorkGraphCounterIndex::NodeBoundsExplicitFrustumRejected),
				counter(CLodWorkGraphCounterIndex::NodeBoundsOverflowFallbacks),
				counter(CLodWorkGraphCounterIndex::NodeBoundsAssemblyFallbacks),
				counter(CLodWorkGraphCounterIndex::NodeBoundsInvalidFallbacks));

            ImGui::Text("Voxel leaves: reached=%u rejectedByError=%u segmentPageHit=%u segmentPageMiss=%u rasterWork=%u rasterDrop=%u",
                counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelLeafRecords),
                counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelRejectedByErrorRecords),
                counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelSegmentPageHits),
                counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelSegmentPageMisses),
                counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelRasterWorkRecords),
                counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelRasterWorkDropped));

            const uint32_t traverseCoalescedLaunches = counter(CLodWorkGraphCounterIndex::TraverseNodesCoalescedLaunches);
            const uint32_t traverseCoalescedInputRecords = counter(CLodWorkGraphCounterIndex::TraverseNodesCoalescedInputRecords);
            const float avgRecordsPerLaunch = (traverseCoalescedLaunches > 0)
                ? (static_cast<float>(traverseCoalescedInputRecords) / static_cast<float>(traverseCoalescedLaunches))
                : 0.0f;
            const float packingPercent = 100.0f * avgRecordsPerLaunch / 8.0f;

            ImGui::Text("Traverse coalesced launches: %u | input records: %u | avg records/launch: %.2f (%.1f%% of 8)",
                traverseCoalescedLaunches,
                traverseCoalescedInputRecords,
                avgRecordsPerLaunch,
                packingPercent);

            std::array<uint32_t, 8> traverseInputHistogram = {
                counter(CLodWorkGraphCounterIndex::TraverseNodesCoalescedInputCount1),
                counter(CLodWorkGraphCounterIndex::TraverseNodesCoalescedInputCount2),
                counter(CLodWorkGraphCounterIndex::TraverseNodesCoalescedInputCount3),
                counter(CLodWorkGraphCounterIndex::TraverseNodesCoalescedInputCount4),
                counter(CLodWorkGraphCounterIndex::TraverseNodesCoalescedInputCount5),
                counter(CLodWorkGraphCounterIndex::TraverseNodesCoalescedInputCount6),
                counter(CLodWorkGraphCounterIndex::TraverseNodesCoalescedInputCount7),
                counter(CLodWorkGraphCounterIndex::TraverseNodesCoalescedInputCount8)
            };

            ImGui::TextUnformatted("Traverse coalesced input histogram (records per launch):");
            float histogramValues[8] = {};
            for (size_t i = 0; i < traverseInputHistogram.size(); ++i) {
                histogramValues[i] = static_cast<float>(traverseInputHistogram[i]);
            }

            static const char* kHistogramLabels[8] = { "1", "2", "3", "4", "5", "6", "7", "8" };
            const std::string histogramId = std::string("##TraverseCoalescedInputHistogram") + title;
            if (ImPlot::BeginPlot(histogramId.c_str(), ImVec2(-1.0f, 150.0f), ImPlotFlags_NoLegend)) {
                ImPlot::SetupAxes("Records", "Launches", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
                ImPlot::SetupAxisTicks(ImAxis_X1, 0.0, 7.0, 8, kHistogramLabels);
                ImPlot::PlotBars("Launches", histogramValues, 8, 0.6f, 0.0f);
                ImPlot::EndPlot();
            }

            const uint32_t clusterWaves = counter(CLodWorkGraphCounterIndex::ClusterCullWaves);
            const uint32_t zeroSurvivorWaves = counter(CLodWorkGraphCounterIndex::ClusterCullZeroSurvivorWaves);
            const uint32_t survivingWaves = (clusterWaves > zeroSurvivorWaves)
                ? (clusterWaves - zeroSurvivorWaves)
                : 0u;
            drawUtilizationRow("ClusterCull waves with survivors", survivingWaves, clusterWaves);

            ImGui::Text("Visible cluster writes: %u", counter(CLodWorkGraphCounterIndex::ClusterCullVisibleClusterWrites));

            ImGui::Separator();
            ImGui::TextUnformatted("Raster bucket sort/compaction");
            {
                const uint32_t histInputs = counter(CLodWorkGraphCounterIndex::RasterSortHistogramInputs);
                const uint32_t histVoxels = counter(CLodWorkGraphCounterIndex::RasterSortHistogramVoxelSkipped);
                const uint32_t histReyes = counter(CLodWorkGraphCounterIndex::RasterSortHistogramReyesSkipped);
                const uint32_t histTriangles = counter(CLodWorkGraphCounterIndex::RasterSortHistogramTriangleContributors);
                const uint32_t compactInputs = counter(CLodWorkGraphCounterIndex::RasterSortCompactionInputs);
                const uint32_t compactVoxels = counter(CLodWorkGraphCounterIndex::RasterSortCompactionVoxelSkipped);
                const uint32_t compactReyes = counter(CLodWorkGraphCounterIndex::RasterSortCompactionReyesSkipped);
                const uint32_t compactTriangles = counter(CLodWorkGraphCounterIndex::RasterSortCompactionTriangleEmitted);
                const uint32_t rasterGroups = counter(CLodWorkGraphCounterIndex::RasterMeshShaderGroups);
                const uint32_t rasterInRange = counter(CLodWorkGraphCounterIndex::RasterMeshShaderInRange);
                const uint32_t rasterInitFailed = counter(CLodWorkGraphCounterIndex::RasterMeshShaderInitFailed);
                const uint32_t rasterOutputTriangles = counter(CLodWorkGraphCounterIndex::RasterMeshShaderOutputTriangles);
                const uint32_t rasterZeroTriangleOutputs = counter(CLodWorkGraphCounterIndex::RasterMeshShaderZeroTriangleOutputs);
                const uint32_t rasterInitZeroPage = counter(CLodWorkGraphCounterIndex::RasterMeshShaderInitFailedZeroPageSlab);
                const uint32_t rasterInitMeshletOob = counter(CLodWorkGraphCounterIndex::RasterMeshShaderInitFailedMeshletOutOfBounds);
                const uint32_t rasterInitInvalidOutput = counter(CLodWorkGraphCounterIndex::RasterMeshShaderInitFailedInvalidOutputCounts);
                const uint32_t rasterSourceGroupMismatch = counter(CLodWorkGraphCounterIndex::RasterMeshShaderSourceGroupMismatch);
                const uint32_t pixelInvocations = counter(CLodWorkGraphCounterIndex::RasterPixelShaderInvocations);
                const uint32_t pixelScissorRejected = counter(CLodWorkGraphCounterIndex::RasterPixelScissorRejected);
                const uint32_t pixelBoundsRejected = counter(CLodWorkGraphCounterIndex::RasterPixelTargetBoundsRejected);
                const uint32_t pixelVisibilityWrites = counter(CLodWorkGraphCounterIndex::RasterPixelVisibilityWrites);
                const uint32_t pixelVsmClipmapRejected = counter(CLodWorkGraphCounterIndex::RasterPixelVirtualShadowClipmapRejected);
                const uint32_t pixelVsmPageRejected = counter(CLodWorkGraphCounterIndex::RasterPixelVirtualShadowPageRejected);
                const uint32_t pixelVsmWrites = counter(CLodWorkGraphCounterIndex::RasterPixelVirtualShadowWrites);
                ImGui::Text("Histogram: input=%u triangles=%u voxelsSkipped=%u reyesSkipped=%u",
                    histInputs,
                    histTriangles,
                    histVoxels,
                    histReyes);
                ImGui::Text("Compaction: input=%u emittedTriangles=%u voxelsSkipped=%u reyesSkipped=%u",
                    compactInputs,
                    compactTriangles,
                    compactVoxels,
                    compactReyes);
                ImGui::Text("Raster MS: groups=%u inRange=%u initFailed=%u outputTriangles=%u zeroTriOutputs=%u",
                    rasterGroups,
                    rasterInRange,
                    rasterInitFailed,
                    rasterOutputTriangles,
                    rasterZeroTriangleOutputs);
                ImGui::Text("Raster MS init failures: zeroPageSlab=%u meshletOOB=%u invalidOutputCounts=%u",
                    rasterInitZeroPage,
                    rasterInitMeshletOob,
                    rasterInitInvalidOutput);
                ImGui::Text("Raster MS diagnostics: sourceGroupMismatch=%u", rasterSourceGroupMismatch);
                ImGui::Text("Raster PS: invocations=%u scissorRejected=%u boundsRejected=%u visibilityWrites=%u",
                    pixelInvocations,
                    pixelScissorRejected,
                    pixelBoundsRejected,
                    pixelVisibilityWrites);
                ImGui::Text("Raster PS VSM: clipmapRejected=%u pageRejected=%u writes=%u",
                    pixelVsmClipmapRejected,
                    pixelVsmPageRejected,
                    pixelVsmWrites);
            }

            ImGui::Separator();
            ImGui::TextUnformatted("ClusterCull meshlet rejection breakdown");
            {
                const uint32_t rejFrustum = counter(CLodWorkGraphCounterIndex::ClusterCullRejectedFrustum);
                const uint32_t rejCond2 = counter(CLodWorkGraphCounterIndex::ClusterCullRejectedCondition2);
                const uint32_t rejOccl = counter(CLodWorkGraphCounterIndex::ClusterCullRejectedOcclusion);
                const uint32_t rejOOR = counter(CLodWorkGraphCounterIndex::ClusterCullRejectedOutOfRange);
                const uint32_t rejPageBounds = counter(CLodWorkGraphCounterIndex::ClusterCullRejectedPageBounds);
                const uint32_t rejCleanPages = counter(CLodWorkGraphCounterIndex::ClusterCullRejectedCleanPages);
                const uint32_t shadowClipmapMisses = counter(CLodWorkGraphCounterIndex::ClusterCullShadowClipmapMisses);
                const uint32_t shadowDirtyRegionHits = counter(CLodWorkGraphCounterIndex::ClusterCullShadowDirtyRegionHits);
                const uint32_t shadowDirtyQueries = counter(CLodWorkGraphCounterIndex::ClusterCullShadowDirtyQueries);
                const uint32_t shadowDirtyClipped = counter(CLodWorkGraphCounterIndex::ClusterCullShadowDirtyQueriesClipped);
                const uint32_t shadowDirtyCoarseMipChecks = counter(CLodWorkGraphCounterIndex::ClusterCullShadowDirtyRegionCoarseMipChecks);
                const uint32_t survived = counter(CLodWorkGraphCounterIndex::ClusterCullSurvivingLanes);
                const uint32_t totalRejected = rejFrustum + rejCond2 + rejOccl + rejOOR + rejPageBounds + rejCleanPages;

                ImGui::Text("Survived: %u", survived);
                ImGui::Text("Rejected total: %u", totalRejected);

                auto rejectionRow = [](const char* label, uint32_t count, uint32_t total) {
                    const float pct = (total > 0)
                        ? (100.0f * static_cast<float>(count) / static_cast<float>(total))
                        : 0.0f;
                    ImGui::Text("  %s: %u (%.1f%%)", label, count, pct);
                };
                rejectionRow("Frustum cull", rejFrustum, totalRejected);
                rejectionRow("Condition 2 (child group refinement)", rejCond2, totalRejected);
                rejectionRow("Occlusion cull", rejOccl, totalRejected);
                rejectionRow(
                    denseDispatchActive ? "Inactive iterations / tail lanes" : "WaveActiveMax padding (inactive iterations)",
                    rejOOR,
                    totalRejected);
                rejectionRow("Page bounds overflow", rejPageBounds, totalRejected);
                rejectionRow("Clean shadow pages", rejCleanPages, totalRejected);
                ImGui::Text("  Shadow clipmap misses: %u", shadowClipmapMisses);
                ImGui::Text("  Shadow dirty queries: %u | clipped: %u | coarse-mip checks: %u",
                    shadowDirtyQueries,
                    shadowDirtyClipped,
                    shadowDirtyCoarseMipChecks);
                ImGui::Text("  Shadow dirty hits: %u | clean-page rejects: %u",
                    shadowDirtyRegionHits,
                    rejCleanPages);
            }

            ImGui::Separator();
            ImGui::TextUnformatted("SW/HW/PageJob classification breakdown");
            {
                const uint32_t contributing = counter(CLodWorkGraphCounterIndex::ClassifyContributing);
                const uint32_t routedHW = counter(CLodWorkGraphCounterIndex::ClassifyRoutedHW);
                const uint32_t routedSW = counter(CLodWorkGraphCounterIndex::ClassifyRoutedSW);
                const uint32_t routedPJ = counter(CLodWorkGraphCounterIndex::ClassifyRoutedPageJob);
                ImGui::Text("Contributing (survived cull): %u", contributing);
                ImGui::Text("Routed -> HW: %u | SW: %u | PageJob: %u", routedHW, routedSW, routedPJ);
                if (contributing > 0) {
                    ImGui::Text("  HW: %.1f%% | SW: %.1f%% | PJ: %.1f%%",
                        100.0f * routedHW / (float)contributing,
                        100.0f * routedSW / (float)contributing,
                        100.0f * routedPJ / (float)contributing);
                }

                const uint32_t pjRejectReyes = counter(CLodWorkGraphCounterIndex::ClassifyPJRejectReyesDisplacement);
                const uint32_t pjRejectAlpha = counter(CLodWorkGraphCounterIndex::ClassifyPJRejectAlphaTested);
                const uint32_t pjRejectNoClipmap = counter(CLodWorkGraphCounterIndex::ClassifyPJRejectNoClipmapIndex);
                const uint32_t pjRejectThreshold = counter(CLodWorkGraphCounterIndex::ClassifyPJRejectBelowThreshold);
                const uint32_t pjRejectDisabled = counter(CLodWorkGraphCounterIndex::ClassifyPJRejectDisabled);
                const uint32_t pjRejectAlreadySW = counter(CLodWorkGraphCounterIndex::ClassifyPJRejectAlreadySW);
                const uint32_t swDisabled = counter(CLodWorkGraphCounterIndex::ClassifySwDisabled);
                const uint32_t totalPJRejects = pjRejectReyes + pjRejectAlpha + pjRejectNoClipmap
                    + pjRejectThreshold + pjRejectDisabled + pjRejectAlreadySW;

                ImGui::Text("PageJob rejections (total: %u):", totalPJRejects);
                ImGui::Text("  Disabled: %u | AlreadySW: %u | ReyesDisplacement: %u",
                    pjRejectDisabled, pjRejectAlreadySW, pjRejectReyes);
                ImGui::Text("  AlphaTested: %u | NoClipmapIdx: %u | BelowThreshold: %u",
                    pjRejectAlpha, pjRejectNoClipmap, pjRejectThreshold);
                ImGui::Text("  SW classification disabled (contributes but !swRasterEnabled): %u", swDisabled);
            }

            ImGui::Separator();
            ImGui::TextUnformatted("PageJob WG pipeline");
            {
                const uint32_t buildProcessed = counter(CLodWorkGraphCounterIndex::PageJobBuildClustersProcessed);
                const uint32_t buildEmitted = counter(CLodWorkGraphCounterIndex::PageJobBuildPagesEmitted);
                ImGui::Text("Build: processed=%u emitted=%u", buildProcessed, buildEmitted);

                const uint32_t rasterJobs = counter(CLodWorkGraphCounterIndex::PageJobRasterJobsLaunched);
                const uint32_t pixWritten = counter(CLodWorkGraphCounterIndex::PageJobRasterPixelsWritten);
                const uint32_t flagWrites = counter(CLodWorkGraphCounterIndex::PageJobRasterFlagWrites);
                ImGui::Text("Raster: jobs=%u pixWritten=%u flagWrites=%u", rasterJobs, pixWritten, flagWrites);
            }

            ImGui::Separator();
            ImGui::TextUnformatted("Occlusion -> Phase 2 enqueue attempts");
            ImGui::Text("Node attempts: %u | Cluster attempts: %u",
                counter(CLodWorkGraphCounterIndex::Phase1OcclusionNodeReplayEnqueueAttempts),
                counter(CLodWorkGraphCounterIndex::Phase1OcclusionClusterReplayEnqueueAttempts));

            ImGui::TextUnformatted("Phase 2 replay launch validation");
            ImGui::Text("ReplayNode launches: %u | input records: %u",
                counter(CLodWorkGraphCounterIndex::Phase2ReplayNodeLaunches),
                counter(CLodWorkGraphCounterIndex::Phase2ReplayNodeInputRecords));
            ImGui::Text("ReplayNode emitted traverse records: %u",
                counter(CLodWorkGraphCounterIndex::Phase2ReplayNodeRecordsEmitted));
            ImGui::Text("ReplayMeshlet launches: %u | input records: %u | emitted bucket records: %u",
                counter(CLodWorkGraphCounterIndex::Phase2ReplayMeshletLaunches),
                counter(CLodWorkGraphCounterIndex::Phase2ReplayMeshletInputRecords),
                counter(CLodWorkGraphCounterIndex::Phase2ReplayMeshletBucketRecordsEmitted));

            ImGui::TextUnformatted("Phase 2 downstream consumption");
            ImGui::Text("Replay Traverse records consumed: %u",
                counter(CLodWorkGraphCounterIndex::Phase2ReplayTraverseRecordsConsumed));
            ImGui::Text("Replay ClusterCull bucket records consumed: %u",
                counter(CLodWorkGraphCounterIndex::Phase2ReplayClusterBucketRecordsConsumed));
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Extended capture statistics");
        if (captureState.captureStatsPending) {
            ImGui::Text("Capture stats status: pending...");
        }
        else if (!captureState.captureStatsAvailable) {
            ImGui::TextDisabled("No extended capture results yet.");
        }
        else {
            ImGui::Text("Visible clusters: %u", captureState.captureStats.visibleClusterCount);
            ImGui::Text("Unique views: %u | Unique instances: %u | Unique meshlets: %u",
                captureState.captureStats.uniqueViews,
                captureState.captureStats.uniqueInstances,
                captureState.captureStats.uniqueMeshlets);
            ImGui::Text("Avg clusters/view: %.2f | Avg clusters/instance: %.2f",
                captureState.captureStats.avgClustersPerView,
                captureState.captureStats.avgClustersPerInstance);
            ImGui::Text("Max clusters/view: %u (%.1f%% of total)",
                captureState.captureStats.maxClustersPerView,
                captureState.captureStats.dominantViewPercent);
            ImGui::Text("Max clusters/instance: %u (%.1f%% of total)",
                captureState.captureStats.maxClustersPerInstance,
                captureState.captureStats.dominantInstancePercent);
        }
    };

    drawWorkGraphCaptureSection("Primary CLod WG", m_clodTelemetry);
    drawWorkGraphCaptureSection("Shadow CLod WG", m_shadowClodTelemetry);

    ImGui::Separator();
    ImGui::TextUnformatted("Directional shadow clipmap snapshot");
    ImGui::Text("Clipmaps published: %u", m_directionalShadowDebugLatest.clipmapCount);
    for (uint32_t clipmapIndex = 0; clipmapIndex < m_directionalShadowDebugLatest.clipmapCount; ++clipmapIndex) {
        const auto& clipmap = m_directionalShadowDebugLatest.clipmaps[clipmapIndex];
        if (clipmap.valid == 0u) {
            continue;
        }

        ImGui::Text(
            "Clip %u: pos=(%.2f, %.2f, %.2f) size=%.2f near=%.3f far=%.3f pageOffset=(%lld, %lld)",
            clipmapIndex,
            clipmap.positionWorldSpace[0],
            clipmap.positionWorldSpace[1],
            clipmap.positionWorldSpace[2],
            clipmap.clipDiameter,
            clipmap.nearPlane,
            clipmap.farPlane,
            static_cast<long long>(clipmap.pageOffsetX),
            static_cast<long long>(clipmap.pageOffsetY));
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Virtual Shadow Map");
    if (m_shadowVirtualShadowTelemetry.capturePending) {
        ImGui::Text("VSM capture status: pending...");
    }
    else if (!m_shadowVirtualShadowTelemetry.hasData) {
        ImGui::TextDisabled("No VSM capture results yet.");
    }
    else {
        const CLodVirtualShadowStats& stats = m_shadowVirtualShadowTelemetry.stats;
        const CLodVirtualShadowRuntimeState& runtimeState = m_shadowVirtualShadowTelemetry.runtimeState;
        const uint32_t displayedClipmapCount = (std::min)(
            (std::max)((std::max)(stats.activeClipmapCount, stats.validClipmapCount), runtimeState.clipmapCount),
            CLodVirtualShadowMaxSupportedClipmapCount);
        ImGui::Text("Captures: %llu", static_cast<unsigned long long>(m_shadowVirtualShadowTelemetry.captureCount));
        ImGui::Text("Clipmaps: active=%u valid=%u supportedMax=%u", stats.activeClipmapCount, stats.validClipmapCount, CLodVirtualShadowMaxSupportedClipmapCount);
        ImGui::Text(
            "Runtime: publishedActive=%u supported=%u virtualRes=%u pageTable=%u physicalAtlas=%ux%u maxPhysicalPages=%u maxRequests=%u lodBias=%.2f",
            runtimeState.clipmapCount,
            runtimeState.supportedClipmapCount,
            runtimeState.virtualResolution,
            runtimeState.pageTableResolution,
            runtimeState.physicalAtlasPagesWide,
            runtimeState.physicalAtlasPagesHigh,
            runtimeState.maxPhysicalPages,
            runtimeState.maxAllocationRequests,
            runtimeState.directionalLodBias);
        const uint32_t allocatablePhysicalPages = std::min(
            stats.freePhysicalPageCount + stats.reusablePhysicalPageCount,
            runtimeState.maxPhysicalPages);
        const uint32_t unbackedAllocationRequests =
            stats.allocationRequestCount > allocatablePhysicalPages
            ? stats.allocationRequestCount - allocatablePhysicalPages
            : 0u;
        ImGui::Text("Allocator: requests=%u dispatchGroups=%u freePages=%u reusablePages=%u allocatable=%u unbacked=%u",
            stats.allocationRequestCount,
            stats.allocationDispatchGroupCount,
            stats.freePhysicalPageCount,
            stats.reusablePhysicalPageCount,
            allocatablePhysicalPages,
            unbackedAllocationRequests);
        ImGui::Text(
            "Page budgets: total=%s%u upgrade=%s%u | admittedTotal=%u | normal eligible=%u admitted=%u deferred=%u | upgrade eligible=%u admitted=%u deferred=%u",
            stats.configuredPageRenderBudget == 0u ? "unlimited/" : "",
            stats.configuredPageRenderBudget,
            stats.configuredUpgradePageRenderBudget == 0u ? "unlimited/" : "",
            stats.configuredUpgradePageRenderBudget,
            stats.admittedPageCount,
            stats.normalEligiblePageCount,
            stats.normalAdmittedPageCount,
            stats.normalDeferredPageCount,
            stats.upgradeEligiblePageCount,
            stats.upgradeAdmittedPageCount,
            stats.upgradeDeferredPageCount);
        ImGui::Text(
            "Streaming upgrades: invalidDependencies=%u",
            stats.invalidUpgradeDependencyCount);
        ImGui::Text(
            "Rendered pages: normal=%u upgrade=%u | candidates input=%u",
            stats.normalRenderedPageCount,
            stats.upgradeRenderedPageCount,
            stats.upgradeCandidateInputCount);
        ImGui::Text(
            "Upgrade queues: raw=%u rawOverflow=%u ready=%u readyOverflow=%u",
            stats.upgradeRawPageCount,
            stats.upgradeRawPageOverflowCount,
            stats.readyUpgradePageCount,
            stats.readyUpgradePageOverflowCount);
        ImGui::Text(
            "Controller: requestAllocation=%.1f%% targetBias=%.2f smoothedBias=%.2f recoveryStableFrames=%u",
            stats.currentAllocationPercentage * 100.0f,
            stats.targetPressureLodBias,
            stats.smoothedPressureLodBias,
            stats.framesSinceOverBudget);
        ImGui::Text("Lifecycle diagnostics: setupReset=%u requestOverflow=%u unwrittenClears=%u",
            stats.setupResetApplied,
            stats.markRequestOverflowCount,
            std::accumulate(
                std::begin(stats.clearedUnwrittenDirtyPages),
                std::end(stats.clearedUnwrittenDirtyPages),
                0u));
        ImGui::Text("Setup reset reasons: forced=%u noPrev=%u structureMismatch=%u lightDirChanged=%u",
            stats.setupResetForced,
            stats.setupResetNoPreviousState,
            stats.setupResetStructureMismatch,
            stats.setupResetLightDirectionChanged);
        uint32_t totalVisitedPageTableEntries = 0u;
        uint32_t totalVisitedDirtyPageTableEntries = 0u;
        uint32_t totalResidentCleanHits = 0u;
        uint32_t totalResidentDirtyHits = 0u;
        uint32_t totalRequestedPages = 0u;
        uint32_t totalDirtyPageTableEntries = 0u;
        uint32_t totalPredictiveInvalidatedPageTableEntries = 0u;
        uint32_t totalInvalidatedCurrentBoundsPageTableEntries = 0u;
        uint32_t totalInvalidatedPreviousBoundsPageTableEntries = 0u;
        for (uint32_t clipmapIndex = 0u; clipmapIndex < displayedClipmapCount; ++clipmapIndex) {
            totalVisitedPageTableEntries += stats.visitedPageTableEntries[clipmapIndex];
            totalVisitedDirtyPageTableEntries += stats.visitedDirtyPageTableEntries[clipmapIndex];
            totalResidentCleanHits += stats.markResidentCleanHits[clipmapIndex];
            totalResidentDirtyHits += stats.markResidentDirtyHits[clipmapIndex];
            totalRequestedPages += stats.requestedPages[clipmapIndex];
            totalDirtyPageTableEntries += stats.dirtyPageTableEntries[clipmapIndex];
            totalPredictiveInvalidatedPageTableEntries += stats.predictiveInvalidatedPageTableEntries[clipmapIndex];
            totalInvalidatedCurrentBoundsPageTableEntries += stats.invalidatedCurrentBoundsPageTableEntries[clipmapIndex];
            totalInvalidatedPreviousBoundsPageTableEntries += stats.invalidatedPreviousBoundsPageTableEntries[clipmapIndex];
        }
        ImGui::Text("Cache lifecycle: cleanHits=%u dirtyHits=%u requests=%u visitedPT=%u visitedDirtyPT=%u dirtyPT=%u",
            totalResidentCleanHits,
            totalResidentDirtyHits,
            totalRequestedPages,
            totalVisitedPageTableEntries,
            totalVisitedDirtyPageTableEntries,
            totalDirtyPageTableEntries);
        ImGui::Text("Request creators: predictiveInv=%u invalidateCurr=%u invalidatePrev=%u wrapClr=%u staleClr=%u",
            totalPredictiveInvalidatedPageTableEntries,
            totalInvalidatedCurrentBoundsPageTableEntries,
            totalInvalidatedPreviousBoundsPageTableEntries,
            std::accumulate(
                std::begin(stats.setupWrappedClearedPageTableEntries),
                std::end(stats.setupWrappedClearedPageTableEntries),
                0u),
            std::accumulate(
                std::begin(stats.setupStaleDirtyClearedPageTableEntries),
                std::end(stats.setupStaleDirtyClearedPageTableEntries),
                0u));
        ImGui::TextDisabled("Dirty PT is sampled at GatherStatsPass before ClearPages re-marks cleared pages dirty for the hierarchy build.");
        if (ImGui::BeginTable("##VirtualShadowStatsTable", 19, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Clip");
            ImGui::TableSetupColumn("Selected");
            ImGui::TableSetupColumn("Proj Reject");
            ImGui::TableSetupColumn("Requests");
            ImGui::TableSetupColumn("Clean Hits");
            ImGui::TableSetupColumn("Dirty Hits");
            ImGui::TableSetupColumn("Wrap Clr");
            ImGui::TableSetupColumn("Stale Clr");
            ImGui::TableSetupColumn("Pre NZ PT");
            ImGui::TableSetupColumn("Pre Dirty PT");
            ImGui::TableSetupColumn("NonZero PT");
            ImGui::TableSetupColumn("Allocated PT");
            ImGui::TableSetupColumn("Visited PT");
            ImGui::TableSetupColumn("Visited Dirty");
            ImGui::TableSetupColumn("Dirty PT");
            ImGui::TableSetupColumn("NoWrite Clr");
            ImGui::TableSetupColumn("Pred Inv");
            ImGui::TableSetupColumn("Inv Curr");
            ImGui::TableSetupColumn("Inv Prev");
            ImGui::TableHeadersRow();

            for (uint32_t clipmapIndex = 0u; clipmapIndex < displayedClipmapCount; ++clipmapIndex) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%u", clipmapIndex);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%u", stats.selectedPixels[clipmapIndex]);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%u", stats.projectionRejectedPixels[clipmapIndex]);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%u", stats.requestedPages[clipmapIndex]);
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%u", stats.markResidentCleanHits[clipmapIndex]);
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%u", stats.markResidentDirtyHits[clipmapIndex]);
                ImGui::TableSetColumnIndex(6);
                ImGui::Text("%u", stats.setupWrappedClearedPageTableEntries[clipmapIndex]);
                ImGui::TableSetColumnIndex(7);
                ImGui::Text("%u", stats.setupStaleDirtyClearedPageTableEntries[clipmapIndex]);
                ImGui::TableSetColumnIndex(8);
                ImGui::Text("%u", stats.preAllocateNonZeroPageTableEntries[clipmapIndex]);
                ImGui::TableSetColumnIndex(9);
                ImGui::Text("%u", stats.preAllocateDirtyPageTableEntries[clipmapIndex]);
                ImGui::TableSetColumnIndex(10);
                ImGui::Text("%u", stats.nonZeroPageTableEntries[clipmapIndex]);
                ImGui::TableSetColumnIndex(11);
                ImGui::Text("%u", stats.allocatedPageTableEntries[clipmapIndex]);
                ImGui::TableSetColumnIndex(12);
                ImGui::Text("%u", stats.visitedPageTableEntries[clipmapIndex]);
                ImGui::TableSetColumnIndex(13);
                ImGui::Text("%u", stats.visitedDirtyPageTableEntries[clipmapIndex]);
                ImGui::TableSetColumnIndex(14);
                ImGui::Text("%u", stats.dirtyPageTableEntries[clipmapIndex]);
                ImGui::TableSetColumnIndex(15);
                ImGui::Text("%u", stats.clearedUnwrittenDirtyPages[clipmapIndex]);
                ImGui::TableSetColumnIndex(16);
                ImGui::Text("%u", stats.predictiveInvalidatedPageTableEntries[clipmapIndex]);
                ImGui::TableSetColumnIndex(17);
                ImGui::Text("%u", stats.invalidatedCurrentBoundsPageTableEntries[clipmapIndex]);
                ImGui::TableSetColumnIndex(18);
                ImGui::Text("%u", stats.invalidatedPreviousBoundsPageTableEntries[clipmapIndex]);
            }

            ImGui::EndTable();
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Reyes Pipeline");
    const auto drawReyesPhase = [](const char* label, const CLodReyesTelemetry& telemetry) {
        if (!ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen)) {
            return;
        }

        const uint32_t totalDiceInputs = telemetry.immediateDiceQueueEntryCount + telemetry.finalDiceQueueEntryCount;
        ImGui::Text("Phase index: %u", telemetry.phaseIndex);
        ImGui::Text("Classify: visible input=%u reyes-owned=%u bypass-full=%u immediate dice=%u",
            telemetry.visibleClusterInputCount,
            telemetry.ownedClusterOutputCount,
            telemetry.fullClusterOutputCount,
            telemetry.immediateDiceQueueEntryCount);
        ImGui::Text("Split: deepest level=%u max configured=%u split-routed dice=%u",
            telemetry.deepestSplitLevelReached,
            telemetry.configuredMaxSplitPassCount,
            telemetry.finalDiceQueueEntryCount);
        ImGui::Text("Split rejects: invalid domains=%u fallback-to-dice=%u frustum=%u shadow-dirty=%u child=%u",
            telemetry.invalidSplitPatchDomainCount,
            telemetry.splitCollapseFallbackDiceCount,
            telemetry.splitFrustumCullCount,
            telemetry.splitShadowDirtyCullCount,
            telemetry.splitChildCullCount);
        ImGui::Text("Coarse dirty-only: eligible=%u rejected=%u leaf outputs=%u",
            telemetry.splitCoarseOnlyDirtyEligibleCount,
            telemetry.splitCoarseOnlyDirtyRejectedCount,
            telemetry.splitCoarseOnlyDirtyLeafOutputCount);
        ImGui::Text("Dice: total queue inputs=%u valid patches=%u invalid domains=%u est triangles=%u est vertices=%u",
            totalDiceInputs,
            telemetry.dicedPatchCount,
            telemetry.invalidDicePatchDomainCount,
            telemetry.dicedTriangleEstimateCount,
            telemetry.dicedVertexEstimateCount);
        ImGui::Text("Raster work: entries=%u emitted patches=%u emitted microtriangles=%u overflow patches=%u overflow batches=%u",
            telemetry.rasterWorkEntryCount,
            telemetry.patchRasterizedPatchCount,
            telemetry.patchRasterizedMicroTriangleCount,
            telemetry.rasterWorkOverflowPatchCount,
            telemetry.rasterWorkOverflowBatchCount);
        ImGui::Text(
            "Hardware Reyes: meshGroups=%u packed entries=%u emitted triangles=%u avg entries/group=%.2f avg emitted/group=%.2f avg requested/group=%.2f",
            telemetry.hardwareRasterMeshGroupCount,
            telemetry.hardwareRasterPackedWorkEntryCount,
            telemetry.hardwareRasterMicroTriangleCount,
            telemetry.hardwareRasterMeshGroupCount > 0u
                ? static_cast<float>(telemetry.hardwareRasterPackedWorkEntryCount) / static_cast<float>(telemetry.hardwareRasterMeshGroupCount)
                : 0.0f,
            telemetry.hardwareRasterMeshGroupCount > 0u
                ? static_cast<float>(telemetry.hardwareRasterMicroTriangleCount) / static_cast<float>(telemetry.hardwareRasterMeshGroupCount)
                : 0.0f,
            telemetry.hardwareRasterMeshGroupCount > 0u
                ? static_cast<float>(telemetry.hardwareRasterRequestedMicroTriangleCount) / static_cast<float>(telemetry.hardwareRasterMeshGroupCount)
                : 0.0f);
        ImGui::Text("Patch raster rejects: zeroCount=%u overflow=%u clip=%u area=%u bounds=%u clippedQuad=%u",
            telemetry.rasterZeroMicroTriangleCount,
            telemetry.rasterMicroTriangleOverflowCount,
            telemetry.rasterClipCullCount,
            telemetry.rasterPreAreaCullCount,
            telemetry.rasterEmptyBoundsCullCount,
            telemetry.rasterNearPlaneClippedQuadCount);
        ImGui::Text("Patch raster projected-triangles: windingSwaps=%u postSwapDegenerate=%u",
            telemetry.rasterWindingSwapCount,
            telemetry.rasterPostSwapNonNegativeAreaCount);
        ImGui::Text("Patch raster tiny-triangle fallback=%u",
            telemetry.rasterTinyTriangleFallbackCount);

        ImGui::TextUnformatted("Per split pass");
        for (uint32_t splitPassIndex = 0; splitPassIndex < CLodReyesMaxSplitPassCount; ++splitPassIndex) {
            ImGui::Text(
                "  Split %u: input=%u children=%u diced=%u splitOverflow=%u diceOverflow=%u",
                splitPassIndex,
                telemetry.splitInputCounts[splitPassIndex],
                telemetry.splitChildOutputCounts[splitPassIndex],
                telemetry.splitDiceOutputCounts[splitPassIndex],
                telemetry.splitQueueOverflowCounts[splitPassIndex],
                telemetry.diceQueueOverflowCounts[splitPassIndex]);
        }
    };

    if (m_clodReyesTelemetryCapturePending) {
        ImGui::Text("Reyes capture status: pending...");
    }
    else if (!m_clodReyesTelemetryHasData) {
        ImGui::TextDisabled("No Reyes telemetry capture results yet.");
    }
    else {
        ImGui::Text("Reyes telemetry captures: %llu", static_cast<unsigned long long>(m_clodReyesTelemetryCaptureCount));
        drawReyesPhase("Reyes Phase 1", m_clodReyesTelemetryPhase1);
        drawReyesPhase("Reyes Phase 2", m_clodReyesTelemetryPhase2);
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Shadow Reyes Pipeline");
    if (m_shadowClodReyesTelemetryCapturePending) {
        ImGui::Text("Shadow Reyes capture status: pending...");
    }
    else if (!m_shadowClodReyesTelemetryHasData) {
        ImGui::TextDisabled("No shadow Reyes telemetry capture results yet.");
    }
    else {
        ImGui::Text("Shadow Reyes telemetry captures: %llu", static_cast<unsigned long long>(m_shadowClodReyesTelemetryCaptureCount));
        drawReyesPhase("Shadow Reyes Phase 1", m_shadowClodReyesTelemetryPhase1);
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Alpha Deep Visibility");
    if (m_clodAlphaTelemetryCapturePending) {
        ImGui::Text("Alpha capture status: pending...");
    }
    else if (!m_clodAlphaTelemetryHasData) {
        ImGui::TextDisabled("No alpha deep-visibility capture results yet.");
    }
    else {
        ImGui::Text("Allocated nodes: %u", m_clodAlphaNodeCount);
        ImGui::Text("Overflowed allocations: %u", m_clodAlphaOverflowCount);
        ImGui::Text("Truncated pixels: %u | Truncated nodes: %u",
            m_clodAlphaStats.truncatedPixelCount,
            m_clodAlphaStats.truncatedNodeCount);
        ImGui::Text("Resolved samples: %u", m_clodAlphaStats.totalResolvedSamples);
        ImGui::Text("Max raw node count/pixel: %u", m_clodAlphaStats.maxRawNodeCount);
        ImGui::Text("Max resolved samples/pixel: %u", m_clodAlphaStats.maxResolvedSamples);
    }

    ImGui::End();
}

void Menu::DrawCLodLodHeightModeCombo()
{
    int modeIdx = static_cast<int>(m_currentCLodLodHeightMode);

    if (ImGui::Combo("CLod LOD Height", &modeIdx, CLodLodHeightModeNames, CLodLodHeightModeCount))
    {
        m_currentCLodLodHeightMode = static_cast<CLodLodHeightMode>(modeIdx);
        setCLodLodHeightMode(m_currentCLodLodHeightMode);
    }
}

