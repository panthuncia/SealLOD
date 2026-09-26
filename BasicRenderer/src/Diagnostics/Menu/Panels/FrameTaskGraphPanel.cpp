#include <BasicRenderer/Scene/Scene.h>
#include "Diagnostics/Menu/Menu.h"

void Menu::DrawFrameTaskGraphWindow() {
    ImGui::Begin("CPU Frame Task Graph", nullptr);

    ImGui::Text(
        "Scene overlap: %s | committed=%llu | completed=%llu | source frame=%llu | last task=%.2f ms",
        m_sceneOverlapStatus.taskInFlight ? "running" : (m_sceneOverlapStatus.enabled ? "idle" : "disabled"),
        static_cast<unsigned long long>(m_sceneOverlapStatus.committedSnapshotSequence),
        static_cast<unsigned long long>(m_sceneOverlapStatus.lastCompletedSnapshotSequence),
        static_cast<unsigned long long>(m_sceneOverlapStatus.lastCommittedSourceFrame),
        m_sceneOverlapStatus.lastTaskDurationMs);
    if (!m_sceneOverlapStatus.hasCommittedSnapshot) {
        ImGui::TextDisabled("No committed scene snapshot is available yet.");
    }
    ImGui::Separator();

    ImGui::Checkbox("Pause", &m_frameTaskGraphPaused);

    if (!m_frameTaskGraphPaused) {
        br::telemetry::FrameTaskGraphSnapshot latestSnapshot{};
        if (br::telemetry::TryReadFrameTaskGraphSnapshot(m_frameTaskGraphLastSequence, latestSnapshot)) {
            m_frameTaskGraphLatest = latestSnapshot;
            m_frameTaskGraphHasData = true;
            if (m_frameTaskGraphHistory.empty() || m_frameTaskGraphHistory.back().frameNumber != latestSnapshot.frameNumber) {
                m_frameTaskGraphHistory.push_back(latestSnapshot);
                constexpr size_t kMaxHistoryFrames = 240;
                if (m_frameTaskGraphHistory.size() > kMaxHistoryFrames) {
                    m_frameTaskGraphHistory.erase(m_frameTaskGraphHistory.begin());
                }
            }
        }
    }

    if (!m_frameTaskGraphHasData || m_frameTaskGraphLatest.nodeCount == 0) {
        ImGui::TextDisabled("No CPU frame task graph snapshots published yet.");
        ImGui::End();
        return;
    }

    const auto domainName = [](br::telemetry::CpuTaskDomain domain) {
        switch (domain) {
        case br::telemetry::CpuTaskDomain::MainThread:
            return "Main";
        case br::telemetry::CpuTaskDomain::Worker:
            return "Worker";
        case br::telemetry::CpuTaskDomain::IOService:
            return "IO";
        case br::telemetry::CpuTaskDomain::BackgroundService:
            return "Background";
        default:
            return "Unknown";
        }
    };

    const auto domainColor = [](br::telemetry::CpuTaskDomain domain) {
        switch (domain) {
        case br::telemetry::CpuTaskDomain::MainThread:
            return IM_COL32(74, 144, 226, 255);
        case br::telemetry::CpuTaskDomain::Worker:
            return IM_COL32(91, 192, 120, 255);
        case br::telemetry::CpuTaskDomain::IOService:
            return IM_COL32(245, 166, 35, 255);
        case br::telemetry::CpuTaskDomain::BackgroundService:
            return IM_COL32(214, 93, 177, 255);
        default:
            return IM_COL32(160, 160, 160, 255);
        }
    };

    const auto isWaitForFrame = [](const char* name) {
        return std::strcmp(name, "WaitForFrame") == 0;
    };

    struct StageAggregate {
        char name[64]{};
        br::telemetry::CpuTaskDomain domain = br::telemetry::CpuTaskDomain::MainThread;
        int32_t dependencyNodeIndex = -1;
        uint64_t avgStartUs = 0;
        uint64_t avgSpanUs = 0;
        uint64_t avgTotalDurationUs = 0;
        uint64_t minTotalDurationUs = 0;
        uint64_t maxTotalDurationUs = 0;
        uint64_t latestStartUs = 0;
        uint64_t latestSpanUs = 0;
        uint64_t latestTotalDurationUs = 0;
        uint32_t avgDispatchCount = 0;
        uint32_t latestDispatchCount = 0;
        uint32_t sampleCount = 0;
    };

    const int clampedAverageWindow = (std::max)(1, (std::min)(m_frameTaskGraphAverageWindow, 120));
    m_frameTaskGraphAverageWindow = clampedAverageWindow;

    const size_t historyCount = m_frameTaskGraphHistory.size();
    const size_t windowCount = (std::min)(historyCount, static_cast<size_t>(m_frameTaskGraphAverageWindow));
    const size_t windowStart = historyCount > windowCount ? (historyCount - windowCount) : 0;

    const auto buildStageGroups = [](const br::telemetry::FrameTaskGraphSnapshot& snapshot) {
        std::vector<StageAggregate> groups;
        groups.reserve(snapshot.nodeCount);

        for (uint32_t nodeIndex = 0; nodeIndex < snapshot.nodeCount; ++nodeIndex) {
            const auto& node = snapshot.nodes[nodeIndex];
            auto existingGroup = std::find_if(groups.begin(), groups.end(), [&](const StageAggregate& group) {
                return group.domain == node.domain && std::strcmp(group.name, node.name) == 0;
            });

            if (existingGroup == groups.end()) {
                StageAggregate group{};
                std::snprintf(group.name, sizeof(group.name), "%s", node.name);
                group.domain = node.domain;
                group.dependencyNodeIndex = node.dependencyNodeIndex;
                group.latestStartUs = node.startTimeUs;
                group.latestSpanUs = node.durationUs;
                group.latestTotalDurationUs = node.durationUs;
                group.latestDispatchCount = 1;
                groups.push_back(group);
                continue;
            }

            const uint64_t currentEndUs = existingGroup->latestStartUs + existingGroup->latestSpanUs;
            existingGroup->latestStartUs = (std::min)(existingGroup->latestStartUs, node.startTimeUs);
            const uint64_t nodeEndUs = node.startTimeUs + node.durationUs;
            const uint64_t updatedEndUs = (std::max)(currentEndUs, nodeEndUs);
            existingGroup->latestSpanUs = updatedEndUs - existingGroup->latestStartUs;
            existingGroup->latestTotalDurationUs += node.durationUs;
            ++existingGroup->latestDispatchCount;
        }

        return groups;
    };

    std::vector<StageAggregate> stageAggregates = buildStageGroups(m_frameTaskGraphLatest);

    uint64_t latestFrameEndUs = 0;
    for (uint32_t nodeIndex = 0; nodeIndex < m_frameTaskGraphLatest.nodeCount; ++nodeIndex) {
        const auto& node = m_frameTaskGraphLatest.nodes[nodeIndex];
        latestFrameEndUs = (std::max)(latestFrameEndUs, node.startTimeUs + node.durationUs);
    }

    for (auto& aggregate : stageAggregates) {
        uint64_t totalStartUs = 0;
        uint64_t totalSpanUs = 0;
        uint64_t totalBusyUs = 0;
        uint64_t totalDispatches = 0;

        for (size_t historyIndex = windowStart; historyIndex < historyCount; ++historyIndex) {
            const auto groupedSnapshot = buildStageGroups(m_frameTaskGraphHistory[historyIndex]);
            const auto match = std::find_if(groupedSnapshot.begin(), groupedSnapshot.end(), [&](const StageAggregate& snapshotGroup) {
                return snapshotGroup.domain == aggregate.domain && std::strcmp(snapshotGroup.name, aggregate.name) == 0;
            });
            if (match == groupedSnapshot.end()) {
                continue;
            }

            totalStartUs += match->latestStartUs;
            totalSpanUs += match->latestSpanUs;
            totalBusyUs += match->latestTotalDurationUs;
            totalDispatches += match->latestDispatchCount;
            if (aggregate.sampleCount == 0) {
                aggregate.minTotalDurationUs = match->latestTotalDurationUs;
                aggregate.maxTotalDurationUs = match->latestTotalDurationUs;
            }
            else {
                aggregate.minTotalDurationUs = (std::min)(aggregate.minTotalDurationUs, match->latestTotalDurationUs);
                aggregate.maxTotalDurationUs = (std::max)(aggregate.maxTotalDurationUs, match->latestTotalDurationUs);
            }
            ++aggregate.sampleCount;
        }

        if (aggregate.sampleCount > 0) {
            aggregate.avgStartUs = totalStartUs / aggregate.sampleCount;
            aggregate.avgSpanUs = totalSpanUs / aggregate.sampleCount;
            aggregate.avgTotalDurationUs = totalBusyUs / aggregate.sampleCount;
            aggregate.avgDispatchCount = static_cast<uint32_t>(totalDispatches / aggregate.sampleCount);
        }
        else {
            aggregate.avgStartUs = aggregate.latestStartUs;
            aggregate.avgSpanUs = aggregate.latestSpanUs;
            aggregate.avgTotalDurationUs = aggregate.latestTotalDurationUs;
            aggregate.minTotalDurationUs = aggregate.latestTotalDurationUs;
            aggregate.maxTotalDurationUs = aggregate.latestTotalDurationUs;
            aggregate.avgDispatchCount = aggregate.latestDispatchCount;
        }
    }

    std::vector<float> frameHistoryMs;
    frameHistoryMs.reserve(windowCount);
    uint64_t avgFrameEndUs = 0;
    uint64_t minFrameEndUs = 0;
    uint64_t maxFrameEndUs = 0;
    uint32_t frameSamples = 0;
    for (size_t historyIndex = windowStart; historyIndex < historyCount; ++historyIndex) {
        const auto& snapshot = m_frameTaskGraphHistory[historyIndex];
        uint64_t frameEndUs = 0;
        for (uint32_t nodeIndex = 0; nodeIndex < snapshot.nodeCount; ++nodeIndex) {
            const auto& node = snapshot.nodes[nodeIndex];
            frameEndUs = (std::max)(frameEndUs, node.startTimeUs + node.durationUs);
        }
        frameHistoryMs.push_back(static_cast<float>(frameEndUs) / 1000.0f);
        avgFrameEndUs += frameEndUs;
        if (frameSamples == 0) {
            minFrameEndUs = frameEndUs;
            maxFrameEndUs = frameEndUs;
        }
        else {
            minFrameEndUs = (std::min)(minFrameEndUs, frameEndUs);
            maxFrameEndUs = (std::max)(maxFrameEndUs, frameEndUs);
        }
        ++frameSamples;
    }
    if (frameSamples > 0) {
        avgFrameEndUs /= frameSamples;
    }

    ImGui::Text(
        "Frame %llu | swap index %u | nodes %u | grouped stages %zu",
        static_cast<unsigned long long>(m_frameTaskGraphLatest.frameNumber),
        static_cast<unsigned int>(m_frameTaskGraphLatest.frameIndex),
        m_frameTaskGraphLatest.nodeCount,
        stageAggregates.size());
    ImGui::Text(
        "Frame total: latest %.3f ms | avg(%u) %.3f ms | min/max %.3f / %.3f ms",
        static_cast<double>(latestFrameEndUs) / 1000.0,
        frameSamples,
        static_cast<double>(avgFrameEndUs) / 1000.0,
        static_cast<double>(minFrameEndUs) / 1000.0,
        static_cast<double>(maxFrameEndUs) / 1000.0);
    if (m_frameTaskGraphLatest.droppedNodeCount > 0) {
        ImGui::TextColored(
            ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
            "Dropped %u task nodes in the latest snapshot because the capture buffer filled.",
            m_frameTaskGraphLatest.droppedNodeCount);
    }

    {
        uint64_t waitUs = 0;
        bool hasWait = false;
        for (uint32_t i = 0; i < m_frameTaskGraphLatest.nodeCount; ++i) {
            if (isWaitForFrame(m_frameTaskGraphLatest.nodes[i].name)) {
                waitUs += m_frameTaskGraphLatest.nodes[i].durationUs;
                hasWait = true;
            }
        }
        if (hasWait) {
            ImGui::TextDisabled("WaitForFrame: %.3f ms (hidden from graph, GPU throughput proxy)",
                static_cast<double>(waitUs) / 1000.0);
        }
    }

    ImGui::SliderInt("Average window (frames)", &m_frameTaskGraphAverageWindow, 1, 120);

    if (!frameHistoryMs.empty() && ImPlot::BeginPlot("##CpuFrameTaskFrameHistory", ImVec2(-1.0f, 150.0f), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("Recent frames", "ms", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        std::vector<float> xValues(frameHistoryMs.size());
        for (size_t i = 0; i < frameHistoryMs.size(); ++i) {
            xValues[i] = static_cast<float>(i);
        }
        ImPlot::PlotLine("Frame total", xValues.data(), frameHistoryMs.data(), static_cast<int>(frameHistoryMs.size()));
        if (frameSamples > 0) {
            const double avgLine[2] = { static_cast<double>(avgFrameEndUs) / 1000.0, static_cast<double>(avgFrameEndUs) / 1000.0 };
            const double avgX[2] = { 0.0, static_cast<double>((std::max)(size_t{ 1 }, frameHistoryMs.size())) - 1.0 };
            ImPlotSpec avgLineSpec;
            avgLineSpec.LineColor = ImVec4(0.95f, 0.8f, 0.2f, 1.0f);
            avgLineSpec.LineWeight = 1.5f;
            ImPlot::PlotLine("Average", avgX, avgLine, 2, avgLineSpec);
        }
        ImPlot::EndPlot();
    }

    std::vector<size_t> bottleneckOrder(stageAggregates.size());
    for (size_t i = 0; i < bottleneckOrder.size(); ++i) {
        bottleneckOrder[i] = i;
    }
    std::sort(bottleneckOrder.begin(), bottleneckOrder.end(), [&](size_t lhs, size_t rhs) {
        return stageAggregates[lhs].avgTotalDurationUs > stageAggregates[rhs].avgTotalDurationUs;
    });

    ImGui::SeparatorText("Bottlenecks");
    size_t bottleneckRank = 0;
    for (size_t i = 0; i < bottleneckOrder.size() && bottleneckRank < 5; ++i) {
        const auto& aggregate = stageAggregates[bottleneckOrder[i]];
        if (isWaitForFrame(aggregate.name)) continue;
        ++bottleneckRank;
        ImGui::Text(
            "%zu. %s [%s] avg busy %.3f ms | latest busy %.3f ms | avg dispatches %u",
            bottleneckRank,
            aggregate.name,
            domainName(aggregate.domain),
            static_cast<double>(aggregate.avgTotalDurationUs) / 1000.0,
            static_cast<double>(aggregate.latestTotalDurationUs) / 1000.0,
            aggregate.avgDispatchCount);
    }

    ImGui::SeparatorText("Task Graph");

    // Layout: assign Y position per individual node, grouped into domain swim lanes.
    // Concurrent tasks within a domain are stacked into sub-lanes.
    struct NodeLayout {
        float yCenter;
        float startMs;
        float endMs;
    };

    std::vector<NodeLayout> nodeLayouts(m_frameTaskGraphLatest.nodeCount);

    // Compute the earliest start time among visible (non-WaitForFrame) nodes
    // so we can subtract it from all displayed times, eliminating the gap.
    uint64_t displayTimeBaseUs = UINT64_MAX;
    for (uint32_t i = 0; i < m_frameTaskGraphLatest.nodeCount; ++i) {
        const auto& n = m_frameTaskGraphLatest.nodes[i];
        if (!isWaitForFrame(n.name))
            displayTimeBaseUs = (std::min)(displayTimeBaseUs, n.startTimeUs);
    }
    if (displayTimeBaseUs == UINT64_MAX) displayTimeBaseUs = 0;

    constexpr float kBarHeight = 0.6f;
    constexpr float kSubLaneHeight = 0.85f;
    constexpr float kDomainGap = 0.6f;
    constexpr int kDomainOrder[] = { 0, 1, 2, 3 }; // Main, Worker, IO, Background

    float currentY = 0.0f;
    float domainLabelY[4] = {};
    bool domainHasNodes[4] = {};

    for (int di = 0; di < 4; ++di) {
        auto domain = static_cast<br::telemetry::CpuTaskDomain>(kDomainOrder[di]);

        std::vector<uint32_t> domainNodes;
        for (uint32_t i = 0; i < m_frameTaskGraphLatest.nodeCount; ++i) {
            if (m_frameTaskGraphLatest.nodes[i].domain == domain && !isWaitForFrame(m_frameTaskGraphLatest.nodes[i].name))
                domainNodes.push_back(i);
        }

        if (domainNodes.empty()) {
            domainLabelY[di] = currentY;
            continue;
        }
        domainHasNodes[di] = true;

        std::sort(domainNodes.begin(), domainNodes.end(), [&](uint32_t a, uint32_t b) {
            return m_frameTaskGraphLatest.nodes[a].startTimeUs < m_frameTaskGraphLatest.nodes[b].startTimeUs;
        });

        struct SubLane { float endTimeMs; };
        std::vector<SubLane> subLanes;
        float domainBaseY = currentY;

        for (uint32_t idx : domainNodes) {
            const auto& node = m_frameTaskGraphLatest.nodes[idx];
            float startMs = static_cast<float>(node.startTimeUs - displayTimeBaseUs) / 1000.0f;
            float endMs = static_cast<float>(node.startTimeUs + node.durationUs - displayTimeBaseUs) / 1000.0f;

            int subLane = -1;
            for (int s = 0; s < static_cast<int>(subLanes.size()); ++s) {
                if (subLanes[s].endTimeMs <= startMs) {
                    subLane = s;
                    break;
                }
            }
            if (subLane < 0) {
                subLane = static_cast<int>(subLanes.size());
                subLanes.push_back({});
            }
            subLanes[subLane].endTimeMs = endMs;

            float y = domainBaseY + subLane * kSubLaneHeight + kSubLaneHeight * 0.5f;
            nodeLayouts[idx] = { y, startMs, endMs };
        }

        float domainHeight = (std::max)(1.0f, static_cast<float>(subLanes.size())) * kSubLaneHeight;
        domainLabelY[di] = domainBaseY + domainHeight * 0.5f;
        currentY = domainBaseY + domainHeight + kDomainGap;
    }

    float totalYRange = (std::max)(currentY, 1.0f);
    float ganttPlotHeight = (std::max)(200.0f, (std::min)(totalYRange * 35.0f, 500.0f));

    uint64_t timelineFrameEndUs = 0;
    for (uint32_t i = 0; i < m_frameTaskGraphLatest.nodeCount; ++i) {
        const auto& n = m_frameTaskGraphLatest.nodes[i];
        if (!isWaitForFrame(n.name))
            timelineFrameEndUs = (std::max)(timelineFrameEndUs, n.startTimeUs + n.durationUs - displayTimeBaseUs);
    }

    if (ImPlot::BeginPlot("##CpuTaskGantt", ImVec2(-1.0f, ganttPlotHeight), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("Time (ms)", nullptr, 0, ImPlotAxisFlags_Invert | ImPlotAxisFlags_NoGridLines);
        const double maxTimeMs = static_cast<double>(timelineFrameEndUs) / 1000.0 * 1.05;
        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, (std::max)(maxTimeMs, 0.1), ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -0.5, static_cast<double>(totalYRange) + 0.5, ImPlotCond_Always);

        std::vector<double> yTicks;
        std::vector<const char*> yLabels;
        for (int di = 0; di < 4; ++di) {
            if (domainHasNodes[di]) {
                yTicks.push_back(static_cast<double>(domainLabelY[di]));
                yLabels.push_back(domainName(static_cast<br::telemetry::CpuTaskDomain>(kDomainOrder[di])));
            }
        }
        if (!yTicks.empty()) {
            ImPlot::SetupAxisTicks(ImAxis_Y1, yTicks.data(), static_cast<int>(yTicks.size()), yLabels.data());
        }

        ImDrawList* drawList = ImPlot::GetPlotDrawList();

        // Draw domain swim-lane backgrounds
        for (int di = 0; di < 4; ++di) {
            if (!domainHasNodes[di]) continue;
            float dMinY = 1e9f, dMaxY = -1e9f;
            for (uint32_t i = 0; i < m_frameTaskGraphLatest.nodeCount; ++i) {
                if (m_frameTaskGraphLatest.nodes[i].domain == static_cast<br::telemetry::CpuTaskDomain>(kDomainOrder[di])) {
                    dMinY = (std::min)(dMinY, nodeLayouts[i].yCenter - kBarHeight * 0.5f);
                    dMaxY = (std::max)(dMaxY, nodeLayouts[i].yCenter + kBarHeight * 0.5f);
                }
            }
            ImVec2 bgMin = ImPlot::PlotToPixels(0.0, static_cast<double>(dMinY - 0.15f));
            ImVec2 bgMax = ImPlot::PlotToPixels(maxTimeMs, static_cast<double>(dMaxY + 0.15f));
            drawList->AddRectFilled(bgMin, bgMax, IM_COL32(40, 40, 40, 80), 4.0f);
        }

        // Draw task bars
        for (uint32_t i = 0; i < m_frameTaskGraphLatest.nodeCount; ++i) {
            const auto& node = m_frameTaskGraphLatest.nodes[i];
            if (isWaitForFrame(node.name)) continue;
            const auto& layout = nodeLayouts[i];

            ImVec2 pMin = ImPlot::PlotToPixels(
                static_cast<double>(layout.startMs),
                static_cast<double>(layout.yCenter - kBarHeight * 0.5f));
            ImVec2 pMax = ImPlot::PlotToPixels(
                static_cast<double>(layout.endMs),
                static_cast<double>(layout.yCenter + kBarHeight * 0.5f));

            if (pMax.x - pMin.x < 3.0f) pMax.x = pMin.x + 3.0f;

            ImU32 color = domainColor(node.domain);
            drawList->AddRectFilled(pMin, pMax, color, 3.0f);
            drawList->AddRect(pMin, pMax, IM_COL32(20, 20, 20, 200), 3.0f);

            float barPx = pMax.x - pMin.x;
            if (barPx > 44.0f) {
                drawList->AddText(nullptr, 0.0f,
                    ImVec2(pMin.x + 4.0f, pMin.y + 1.0f),
                    IM_COL32(10, 10, 10, 255), node.name, nullptr, barPx - 6.0f);
            }
        }

        // Draw dependency arrows
        for (uint32_t i = 0; i < m_frameTaskGraphLatest.nodeCount; ++i) {
            const auto& node = m_frameTaskGraphLatest.nodes[i];
            if (isWaitForFrame(node.name)) continue;
            if (node.dependencyNodeIndex < 0 ||
                static_cast<uint32_t>(node.dependencyNodeIndex) >= m_frameTaskGraphLatest.nodeCount)
                continue;
            if (isWaitForFrame(m_frameTaskGraphLatest.nodes[static_cast<uint32_t>(node.dependencyNodeIndex)].name))
                continue;

            const auto& depLayout = nodeLayouts[static_cast<uint32_t>(node.dependencyNodeIndex)];
            const auto& thisLayout = nodeLayouts[i];

            ImVec2 from = ImPlot::PlotToPixels(
                static_cast<double>(depLayout.endMs),
                static_cast<double>(depLayout.yCenter));
            ImVec2 to = ImPlot::PlotToPixels(
                static_cast<double>(thisLayout.startMs),
                static_cast<double>(thisLayout.yCenter));

            const ImU32 arrowColor = IM_COL32(255, 220, 80, 200);
            drawList->AddLine(from, to, arrowColor, 1.5f);

            // Arrowhead
            float dx = to.x - from.x;
            float dy = to.y - from.y;
            float lenSq = dx * dx + dy * dy;
            if (lenSq > 4.0f) {
                float invLen = 1.0f / std::sqrt(lenSq);
                dx *= invLen;
                dy *= invLen;
                constexpr float arrowSz = 7.0f;
                ImVec2 p1(to.x - dx * arrowSz - dy * arrowSz * 0.4f,
                          to.y - dy * arrowSz + dx * arrowSz * 0.4f);
                ImVec2 p2(to.x - dx * arrowSz + dy * arrowSz * 0.4f,
                          to.y - dy * arrowSz - dx * arrowSz * 0.4f);
                drawList->AddTriangleFilled(to, p1, p2, arrowColor);
            }
        }

        // Tooltip on hover
        if (ImPlot::IsPlotHovered()) {
            ImPlotPoint mouse = ImPlot::GetPlotMousePos();
            for (uint32_t i = 0; i < m_frameTaskGraphLatest.nodeCount; ++i) {
                if (isWaitForFrame(m_frameTaskGraphLatest.nodes[i].name)) continue;
                const auto& layout = nodeLayouts[i];
                if (mouse.x >= static_cast<double>(layout.startMs) &&
                    mouse.x <= static_cast<double>(layout.endMs) &&
                    mouse.y >= static_cast<double>(layout.yCenter - kBarHeight * 0.5f) &&
                    mouse.y <= static_cast<double>(layout.yCenter + kBarHeight * 0.5f)) {
                    const auto& node = m_frameTaskGraphLatest.nodes[i];
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(node.name);
                    ImGui::Text("Domain: %s", domainName(node.domain));
                    ImGui::Text("Start: %.3f ms", static_cast<double>(node.startTimeUs - displayTimeBaseUs) / 1000.0);
                    ImGui::Text("Duration: %.3f ms", static_cast<double>(node.durationUs) / 1000.0);
                    if (node.dependencyNodeIndex >= 0 &&
                        static_cast<uint32_t>(node.dependencyNodeIndex) < m_frameTaskGraphLatest.nodeCount) {
                        ImGui::Text("Depends on: %s",
                            m_frameTaskGraphLatest.nodes[node.dependencyNodeIndex].name);
                    }
                    ImGui::EndTooltip();
                    break;
                }
            }
        }

        ImPlot::EndPlot();
    }

    ImGui::SeparatorText("Stages");
    if (ImGui::BeginTable("##CpuFrameTaskStages", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Stage");
        ImGui::TableSetupColumn("Domain");
        ImGui::TableSetupColumn("Avg Start (ms)");
        ImGui::TableSetupColumn("Avg Span (ms)");
        ImGui::TableSetupColumn("Avg Busy (ms)");
        ImGui::TableSetupColumn("Latest Busy (ms)");
        ImGui::TableSetupColumn("Dispatches");
        ImGui::TableSetupColumn("Min/Max Busy (ms)");
        ImGui::TableSetupColumn("Depends On");
        ImGui::TableHeadersRow();

        for (size_t nodeIndex = 0; nodeIndex < stageAggregates.size(); ++nodeIndex) {
            const auto& aggregate = stageAggregates[nodeIndex];
            if (isWaitForFrame(aggregate.name)) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(aggregate.name);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(domainName(aggregate.domain));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.3f", static_cast<double>(aggregate.avgStartUs) / 1000.0);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.3f", static_cast<double>(aggregate.avgSpanUs) / 1000.0);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.3f", static_cast<double>(aggregate.avgTotalDurationUs) / 1000.0);
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%.3f", static_cast<double>(aggregate.latestTotalDurationUs) / 1000.0);
            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%u / %u", aggregate.avgDispatchCount, aggregate.latestDispatchCount);
            ImGui::TableSetColumnIndex(7);
            ImGui::Text("%.3f / %.3f", static_cast<double>(aggregate.minTotalDurationUs) / 1000.0, static_cast<double>(aggregate.maxTotalDurationUs) / 1000.0);
            ImGui::TableSetColumnIndex(8);
            if (aggregate.dependencyNodeIndex >= 0 && static_cast<uint32_t>(aggregate.dependencyNodeIndex) < m_frameTaskGraphLatest.nodeCount) {
                ImGui::TextUnformatted(m_frameTaskGraphLatest.nodes[aggregate.dependencyNodeIndex].name);
            }
            else {
                ImGui::TextDisabled("None");
            }
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

