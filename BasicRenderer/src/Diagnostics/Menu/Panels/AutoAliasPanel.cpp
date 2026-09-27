#include <BasicRenderer/Scene/Scene.h>
#include "Diagnostics/Menu/Menu.h"

void Menu::DrawAutoAliasPlannerWindow() {
    if (!ImGui::Begin("Auto Alias Planner", nullptr)) {
        ImGui::End();
        return;
    }

    constexpr const char* kAutoAliasModeNames[] = {
        "Off",
        "Conservative",
        "Balanced",
        "Aggressive"
    };

    int autoAliasModeIndex = static_cast<int>(m_autoAliasMode);
    if (ImGui::Combo("Mode", &autoAliasModeIndex, kAutoAliasModeNames, IM_ARRAYSIZE(kAutoAliasModeNames))) {
        autoAliasModeIndex = std::clamp(autoAliasModeIndex, 0, static_cast<int>(IM_ARRAYSIZE(kAutoAliasModeNames) - 1));
        m_autoAliasMode = static_cast<org::AutoAliasMode>(autoAliasModeIndex);
        setAutoAliasMode(m_autoAliasMode);
    }

    constexpr const char* kPackingStrategyNames[] = {
        "Greedy Sweep-Line",
        "Beam Search (Near-Optimal)",
    };

    int packingStrategyIndex = static_cast<int>(m_autoAliasPackingStrategy);
    if (ImGui::Combo("Packing Strategy", &packingStrategyIndex, kPackingStrategyNames, IM_ARRAYSIZE(kPackingStrategyNames))) {
        packingStrategyIndex = std::clamp(packingStrategyIndex, 0, static_cast<int>(IM_ARRAYSIZE(kPackingStrategyNames) - 1));
        m_autoAliasPackingStrategy = static_cast<org::AutoAliasPackingStrategy>(packingStrategyIndex);
        setAutoAliasPackingStrategy(m_autoAliasPackingStrategy);
    }

    if (ImGui::Checkbox("Log Exclusions", &m_autoAliasLogExclusionReasons)) {
        setAutoAliasLogExclusionReasons(m_autoAliasLogExclusionReasons);
    }

    int retireIdleFrames = static_cast<int>(m_autoAliasPoolRetireIdleFrames);
    if (ImGui::SliderInt("Pool Retire Idle Frames", &retireIdleFrames, 0, 2000)) {
        retireIdleFrames = std::max(retireIdleFrames, 0);
        m_autoAliasPoolRetireIdleFrames = static_cast<uint32_t>(retireIdleFrames);
        setAutoAliasPoolRetireIdleFrames(m_autoAliasPoolRetireIdleFrames);
    }

    if (ImGui::SliderFloat("Pool Growth Headroom", &m_autoAliasPoolGrowthHeadroom, 1.0f, 3.0f, "%.2fx")) {
        m_autoAliasPoolGrowthHeadroom = std::max(1.0f, m_autoAliasPoolGrowthHeadroom);
        setAutoAliasPoolGrowthHeadroom(m_autoAliasPoolGrowthHeadroom);
    }

    if (m_renderGraph) {
        ImGui::Separator();
        auto formatBytes = [](uint64_t bytes) {
            constexpr double kKB = 1024.0;
            constexpr double kMB = 1024.0 * 1024.0;
            constexpr double kGB = 1024.0 * 1024.0 * 1024.0;

            const double value = static_cast<double>(bytes);
            if (value >= kGB) {
                return std::format("{:.2f} GB", value / kGB);
            }
            if (value >= kMB) {
                return std::format("{:.2f} MB", value / kMB);
            }
            if (value >= kKB) {
                return std::format("{:.2f} KB", value / kKB);
            }
            return std::format("{:.2f} B", value);
        };

        const auto snapshot = m_renderGraph->GetAutoAliasDebugSnapshot();
        constexpr const char* kModeNames[] = { "Off", "Conservative", "Balanced", "Aggressive" };
        constexpr const char* kStrategyNames[] = { "Greedy Sweep-Line", "Beam Search (Near-Optimal)" };
        const int modeIdx = std::clamp(static_cast<int>(snapshot.mode), 0, static_cast<int>(IM_ARRAYSIZE(kModeNames) - 1));
        const int strategyIdx = std::clamp(static_cast<int>(snapshot.packingStrategy), 0, static_cast<int>(IM_ARRAYSIZE(kStrategyNames) - 1));
        ImGui::Text("Active mode: %s", kModeNames[modeIdx]);
        ImGui::Text("Active strategy: %s", kStrategyNames[strategyIdx]);

        ImGui::Text("Candidates: %llu | Manual: %llu | Auto: %llu | Excluded: %llu",
            static_cast<unsigned long long>(snapshot.candidatesSeen),
            static_cast<unsigned long long>(snapshot.manuallyAssigned),
            static_cast<unsigned long long>(snapshot.autoAssigned),
            static_cast<unsigned long long>(snapshot.excluded));

        ImGui::Text("Candidate MB: %.2f | Auto MB: %.2f",
            static_cast<double>(snapshot.candidateBytes) / (1024.0 * 1024.0),
            static_cast<double>(snapshot.autoAssignedBytes) / (1024.0 * 1024.0));

        const double independentMB = static_cast<double>(snapshot.pooledIndependentBytes) / (1024.0 * 1024.0);
        const double pooledMB = static_cast<double>(snapshot.pooledActualBytes) / (1024.0 * 1024.0);
        const double savedMB = static_cast<double>(snapshot.pooledSavedBytes) / (1024.0 * 1024.0);
        const double savedPct = (snapshot.pooledIndependentBytes > 0)
            ? (100.0 * static_cast<double>(snapshot.pooledSavedBytes) / static_cast<double>(snapshot.pooledIndependentBytes))
            : 0.0;

        ImGui::Text("Pooling memory (alias candidates)");
        ImGui::BulletText("Independent: %.2f MB", independentMB);
        ImGui::BulletText("Pooled: %.2f MB", pooledMB);
        ImGui::BulletText("Saved: %.2f MB (%.1f%%)", savedMB, savedPct);

        if (snapshot.planCacheHits > 0 || snapshot.planCacheMisses > 0 || !snapshot.primaryPlanCacheMissReason.empty()) {
            ImGui::Text("Planner cache");
            ImGui::BulletText(
                "Hits: %llu | Misses: %llu",
                static_cast<unsigned long long>(snapshot.planCacheHits),
                static_cast<unsigned long long>(snapshot.planCacheMisses));
            if (!snapshot.primaryPlanCacheMissReason.empty()) {
                ImGui::BulletText("Primary miss reason: %s", snapshot.primaryPlanCacheMissReason.c_str());
            }
        }

        if (!snapshot.poolDebug.empty()) {
            ImGui::Separator();
            ImGui::TextUnformatted("Pool byte overlap view");

            for (const auto& pool : snapshot.poolDebug) {
                ImGui::PushID(static_cast<int>(pool.poolID & 0x7fffffff));

                const double requiredMB = static_cast<double>(pool.requiredBytes) / (1024.0 * 1024.0);
                const double reservedMB = static_cast<double>(pool.reservedBytes) / (1024.0 * 1024.0);
                const std::string header = std::format(
                    "Pool {} (resources={}, required={:.2f} MB, reserved={:.2f} MB)",
                    static_cast<unsigned long long>(pool.poolID),
                    pool.ranges.size(),
                    requiredMB,
                    reservedMB);

                if (ImGui::TreeNode(header.c_str())) {
                    if (pool.ranges.empty()) {
                        ImGui::TextDisabled("No ranges");
                        ImGui::TreePop();
                        ImGui::PopID();
                        continue;
                    }

                    std::vector<org::RenderGraph::AutoAliasPoolRangeDebug> ranges = pool.ranges;
                    std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) {
                        if (a.startByte != b.startByte) {
                            return a.startByte < b.startByte;
                        }
                        return a.resourceID < b.resourceID;
                        });

                    uint64_t maxByte = std::max<uint64_t>(1ull, std::max(pool.requiredBytes, pool.reservedBytes));
                    for (const auto& r : ranges) {
                        maxByte = std::max(maxByte, r.endByte);
                    }

                    const float rowHeight = 18.0f;
                    const float plotHeight = std::max(80.0f, rowHeight * static_cast<float>(ranges.size()) + 28.0f);
                    const float labelWidth = 260.0f;
                    ImVec2 canvasSize(ImGui::GetContentRegionAvail().x, plotHeight);
                    if (canvasSize.x < 320.0f) {
                        canvasSize.x = 320.0f;
                    }

                    const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton("##AliasPoolOverlapPlot", canvasSize);
                    ImDrawList* draw = ImGui::GetWindowDrawList();

                    const float left = canvasPos.x;
                    const float top = canvasPos.y;
                    const float right = canvasPos.x + canvasSize.x;
                    const float bottom = canvasPos.y + canvasSize.y;

                    const float plotLeft = left + labelWidth;
                    const float plotRight = right - 10.0f;
                    const float plotWidth = std::max(1.0f, plotRight - plotLeft);
                    const float plotTop = top + 6.0f;

                    draw->AddRectFilled(ImVec2(left, top), ImVec2(right, bottom), IM_COL32(20, 20, 20, 100));
                    draw->AddRect(ImVec2(left, top), ImVec2(right, bottom), IM_COL32(255, 255, 255, 40));

                    auto toX = [&](uint64_t byteOffset) {
                        const double t = static_cast<double>(byteOffset) / static_cast<double>(maxByte);
                        return plotLeft + static_cast<float>(t) * plotWidth;
                        };

                    draw->AddLine(ImVec2(plotLeft, bottom - 14.0f), ImVec2(plotRight, bottom - 14.0f), IM_COL32(220, 220, 220, 140), 1.0f);
                    draw->AddText(ImVec2(plotLeft, bottom - 13.0f), IM_COL32(220, 220, 220, 200), "0");
                    const std::string maxLabel = std::format("{} B", static_cast<unsigned long long>(maxByte));
                    draw->AddText(ImVec2(plotRight - ImGui::CalcTextSize(maxLabel.c_str()).x, bottom - 13.0f), IM_COL32(220, 220, 220, 200), maxLabel.c_str());

                    if (pool.reservedBytes > 0 && pool.reservedBytes != maxByte) {
                        const float reservedX = toX(pool.reservedBytes);
                        draw->AddLine(ImVec2(reservedX, plotTop), ImVec2(reservedX, bottom - 16.0f), IM_COL32(255, 230, 120, 120), 1.0f);
                    }

                    for (size_t i = 0; i < ranges.size(); ++i) {
                        const auto& r = ranges[i];
                        const float y0 = plotTop + static_cast<float>(i) * rowHeight;
                        const float y1 = y0 + rowHeight - 4.0f;
                        const float x0 = toX(r.startByte);
                        const float x1 = toX(r.endByte);

                        draw->AddRectFilled(ImVec2(x0, y0), ImVec2(std::max(x0 + 1.0f, x1), y1), IM_COL32(90, 170, 250, 180));
                        draw->AddRect(ImVec2(x0, y0), ImVec2(std::max(x0 + 1.0f, x1), y1), IM_COL32(15, 30, 45, 220));

                        std::vector<std::pair<uint64_t, uint64_t>> overlapSegments;
                        overlapSegments.reserve(ranges.size());
                        for (size_t j = 0; j < ranges.size(); ++j) {
                            if (i == j) {
                                continue;
                            }
                            const auto& other = ranges[j];
                            const uint64_t overlapStart = std::max(r.startByte, other.startByte);
                            const uint64_t overlapEnd = std::min(r.endByte, other.endByte);
                            if (overlapStart < overlapEnd) {
                                overlapSegments.emplace_back(overlapStart, overlapEnd);
                            }
                        }

                        if (!overlapSegments.empty()) {
                            std::sort(overlapSegments.begin(), overlapSegments.end());
                            std::vector<std::pair<uint64_t, uint64_t>> merged;
                            for (const auto& seg : overlapSegments) {
                                if (merged.empty() || seg.first > merged.back().second) {
                                    merged.push_back(seg);
                                }
                                else {
                                    merged.back().second = std::max(merged.back().second, seg.second);
                                }
                            }

                            for (const auto& seg : merged) {
                                const float ox0 = toX(seg.first);
                                const float ox1 = toX(seg.second);
                                draw->AddRectFilled(ImVec2(ox0, y0), ImVec2(std::max(ox0 + 1.0f, ox1), y1), IM_COL32(255, 80, 80, 210));
                            }
                        }

                        const std::string label = std::format(
                            "{} ({})",
                            r.resourceName,
                            formatBytes(r.sizeBytes));
                        draw->AddText(ImVec2(left + 6.0f, y0), IM_COL32(230, 230, 230, 230), label.c_str());
                    }

                    ImGui::TreePop();
                }

                ImGui::PopID();
            }
        }

        if (!snapshot.exclusionReasons.empty()) {
            ImGui::Separator();
            ImGui::TextUnformatted("Top exclusion reasons:");
            const size_t maxReasons = std::min<size_t>(snapshot.exclusionReasons.size(), 8);
            for (size_t i = 0; i < maxReasons; ++i) {
                ImGui::BulletText("%s (%llu)",
                    snapshot.exclusionReasons[i].reason.c_str(),
                    static_cast<unsigned long long>(snapshot.exclusionReasons[i].count));
            }
        }

        if (!snapshot.excludedResources.empty()) {
            ImGui::Separator();
            uint64_t excludedBytes = 0;
            for (const auto& excludedResource : snapshot.excludedResources) {
                excludedBytes += excludedResource.sizeBytes;
            }

            ImGui::Text(
                "Non-aliasable resources: %llu | Total bytes: %s",
                static_cast<unsigned long long>(snapshot.excludedResources.size()),
                formatBytes(excludedBytes).c_str());
            ImGui::TextDisabled("Sorted by memory size (largest first)");

            constexpr ImGuiTableFlags excludedTableFlags =
                ImGuiTableFlags_Borders |
                ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingStretchProp |
                ImGuiTableFlags_ScrollY;
            const float listHeight = std::min(320.0f, 22.0f * static_cast<float>(snapshot.excludedResources.size()) + 28.0f);
            if (ImGui::BeginTable("##AutoAliasExcludedResources", 3, excludedTableFlags, ImVec2(0.0f, std::max(140.0f, listHeight)))) {
                ImGui::TableSetupColumn("Resource", ImGuiTableColumnFlags_WidthStretch, 0.45f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthStretch, 0.55f);
                ImGui::TableHeadersRow();
                ImGui::TableSetupScrollFreeze(0, 1);

                for (const auto& excludedResource : snapshot.excludedResources) {
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(excludedResource.resourceName.c_str());

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(formatBytes(excludedResource.sizeBytes).c_str());

                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextWrapped("%s", excludedResource.reason.c_str());
                }

                ImGui::EndTable();
            }
        }
    }
    else {
        ImGui::Separator();
        ImGui::TextDisabled("Render graph not available.");
    }

    ImGui::End();
}
