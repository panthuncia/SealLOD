#include <BasicRenderer/Scene/Scene.h>
#include "Diagnostics/Menu/Menu.h"

void Menu::DrawPassTimingWindow() {
    if (!m_renderGraph) {
        return;
    }

    auto* statisticsService = m_renderGraph->GetStatisticsService();
    if (!statisticsService) {
        return;
    }

    auto& names = statisticsService->GetPassNames();
    auto& techniquePaths = statisticsService->GetPassTechniquePaths();
    auto& stats = statisticsService->GetPassStats();
    auto& meshStats = statisticsService->GetMeshStats();
    auto& isGeom = statisticsService->GetIsGeometryPassVector();
    static int maxStaleFrames = 240;
    static int viewMode = 0;

    if (names.empty()) {
        return;
    }

    ImGui::Begin("Pass Timings");
    ImGui::SliderInt("Max Stale Frames", &maxStaleFrames, 0, 2000);
    constexpr const char* kPassTimingViewNames[] = { "Flat", "Techniques" };
    ImGui::SetNextItemWidth(160.0f);
    ImGui::Combo("View", &viewMode, kPassTimingViewNames, IM_ARRAYSIZE(kPassTimingViewNames));

    const auto& visible = statisticsService->GetVisiblePassIndices(static_cast<uint64_t>(maxStaleFrames));
    if (visible.empty()) {
        ImGui::TextUnformatted("No pass timings within selected staleness window.");
        ImGui::End();
        return;
    }

    static std::vector<bool> pinned;
    if (pinned.size() != names.size()) {
        pinned.assign(names.size(), false);
    }

    enum class PassTimingViewMode : int {
        Flat = 0,
        Techniques = 1,
    };

    enum class PassTimingColumn : ImGuiID {
        Pass = 1,
        Gpu = 2,
        Cpu = 3,
        CpuUpdate = 4,
        CpuExecute = 5,
    };

    auto compareDoubles = [](double lhs, double rhs) {
        if (lhs < rhs) {
            return -1;
        }
        if (lhs > rhs) {
            return 1;
        }
        return 0;
    };

    auto compareNames = [&](int lhs, int rhs) {
        const int cmp = std::strcmp(names[lhs].c_str(), names[rhs].c_str());
        if (cmp < 0) {
            return -1;
        }
        if (cmp > 0) {
            return 1;
        }
        return 0;
    };

    auto compareRows = [&](int lhs, int rhs, const ImGuiTableColumnSortSpecs* sortSpec) {
        const ImGuiID sortColumn = sortSpec != nullptr
            ? sortSpec->ColumnUserID
            : static_cast<ImGuiID>(PassTimingColumn::Gpu);
        const ImGuiSortDirection direction = sortSpec != nullptr
            ? sortSpec->SortDirection
            : ImGuiSortDirection_Descending;

        int result = 0;
        switch (static_cast<PassTimingColumn>(sortColumn)) {
        case PassTimingColumn::Pass:
            result = compareNames(lhs, rhs);
            break;
        case PassTimingColumn::Gpu:
            result = compareDoubles(stats[lhs].gpuTimeEma, stats[rhs].gpuTimeEma);
            break;
        case PassTimingColumn::Cpu:
            result = compareDoubles(stats[lhs].GetCpuTimeEma(), stats[rhs].GetCpuTimeEma());
            break;
        case PassTimingColumn::CpuUpdate:
            result = compareDoubles(stats[lhs].cpuUpdateTimeEma, stats[rhs].cpuUpdateTimeEma);
            break;
        case PassTimingColumn::CpuExecute:
            result = compareDoubles(stats[lhs].cpuExecuteTimeEma, stats[rhs].cpuExecuteTimeEma);
            break;
        default:
            result = compareDoubles(stats[lhs].gpuTimeEma, stats[rhs].gpuTimeEma);
            break;
        }

        if (result == 0) {
            result = compareNames(lhs, rhs);
        }

        return direction == ImGuiSortDirection_Ascending ? (result < 0) : (result > 0);
    };

    std::vector<int> pinnedRows;
    std::vector<int> unpinnedRows;
    pinnedRows.reserve(visible.size());
    unpinnedRows.reserve(visible.size());
    for (unsigned rawIdx : visible) {
        if (rawIdx >= names.size() || rawIdx >= stats.size()) {
            continue;
        }

        const int idx = static_cast<int>(rawIdx);
        if (pinned[idx]) {
            pinnedRows.push_back(idx);
        }
        else {
            unpinnedRows.push_back(idx);
        }
    }

    constexpr ImGuiTableFlags tableFlags =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_Hideable |
        ImGuiTableFlags_Sortable |
        ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("PassTimingsTable", 6, tableFlags)) {
        const bool techniquesView = viewMode == static_cast<int>(PassTimingViewMode::Techniques);
        ImGui::TableSetupColumn(techniquesView ? "Passes" : "Pin", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch, 0.0f, static_cast<ImGuiID>(PassTimingColumn::Pass));
        ImGui::TableSetupColumn("GPU (ms)", ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_DefaultSort, 0.0f, static_cast<ImGuiID>(PassTimingColumn::Gpu));
        ImGui::TableSetupColumn("CPU (ms)", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, static_cast<ImGuiID>(PassTimingColumn::Cpu));
        ImGui::TableSetupColumn("Update (ms)", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, static_cast<ImGuiID>(PassTimingColumn::CpuUpdate));
        ImGui::TableSetupColumn("Execute (ms)", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, static_cast<ImGuiID>(PassTimingColumn::CpuExecute));
        ImGui::TableHeadersRow();

        const ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
        const ImGuiTableColumnSortSpecs* primarySort =
            (sortSpecs != nullptr && sortSpecs->SpecsCount > 0) ? &sortSpecs->Specs[0] : nullptr;

        auto drawRow = [&](int idx) {
            const bool hasMeshDetails = idx < static_cast<int>(isGeom.size()) && idx < static_cast<int>(meshStats.size()) && isGeom[idx];

            ImGui::PushID(idx);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            if (ImGui::SmallButton(pinned[idx] ? "Unpin" : "Pin")) {
                pinned[idx] = !pinned[idx];
            }

            ImGui::TableSetColumnIndex(1);
            ImGuiTreeNodeFlags treeFlags = ImGuiTreeNodeFlags_SpanFullWidth;
            if (!hasMeshDetails) {
                treeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            }
            const bool open = ImGui::TreeNodeEx("PassRow", treeFlags, "%s", names[idx].c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.3f", stats[idx].gpuTimeEma);

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.3f", stats[idx].GetCpuTimeEma());

            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.3f", stats[idx].cpuUpdateTimeEma);

            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%.3f", stats[idx].cpuExecuteTimeEma);

            if (hasMeshDetails && open) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(1);
                ImGui::Indent();
                ImGui::Text("Mesh Invocations: %.0f", meshStats[idx].invocationsEma);
                ImGui::Text("Mesh Primitives:  %.0f", meshStats[idx].primitivesEma);
                ImGui::Unindent();
                ImGui::TreePop();
            }

            ImGui::PopID();
        };

        if (viewMode == static_cast<int>(PassTimingViewMode::Flat)) {
            auto sortRows = [&](std::vector<int>& rows) {
                std::stable_sort(rows.begin(), rows.end(), [&](int lhs, int rhs) {
                    return compareRows(lhs, rhs, primarySort);
                });
            };

            sortRows(pinnedRows);
            sortRows(unpinnedRows);

            for (int idx : pinnedRows) {
                drawRow(idx);
            }
            for (int idx : unpinnedRows) {
                drawRow(idx);
            }
        }
        else {
            struct TechniqueTreeNode {
                std::string label;
                int parentIndex = -1;
                std::vector<int> childNodeIndices;
                std::vector<int> passIndices;
                double gpuTimeMs = 0.0;
                double cpuTimeMs = 0.0;
                double updateTimeMs = 0.0;
                double executeTimeMs = 0.0;
                uint32_t totalPassCount = 0;
            };

            std::vector<TechniqueTreeNode> techniqueNodes;
            techniqueNodes.reserve(visible.size() + 1);
            techniqueNodes.push_back({ "All Techniques", -1 });

            std::unordered_map<std::string, int> techniquePathToNodeIndex;
            techniquePathToNodeIndex.reserve(visible.size() + 1);
            techniquePathToNodeIndex.emplace("", 0);

            auto ensureTechniqueNode = [&](int parentIndex, const std::string& techniquePath, std::string_view label) {
                auto it = techniquePathToNodeIndex.find(techniquePath);
                if (it != techniquePathToNodeIndex.end()) {
                    return it->second;
                }

                const int nodeIndex = static_cast<int>(techniqueNodes.size());
                techniqueNodes.push_back({ std::string(label), parentIndex });
                techniquePathToNodeIndex.emplace(techniquePath, nodeIndex);
                techniqueNodes[parentIndex].childNodeIndices.push_back(nodeIndex);
                return nodeIndex;
            };

            for (unsigned rawIdx : visible) {
                if (rawIdx >= names.size() || rawIdx >= stats.size()) {
                    continue;
                }

                const int passIndex = static_cast<int>(rawIdx);
                std::string techniquePath =
                    rawIdx < techniquePaths.size() && !techniquePaths[rawIdx].empty()
                    ? techniquePaths[rawIdx]
                    : "Ungrouped";

                int currentNodeIndex = 0;
                std::string currentPath;
                size_t segmentStart = 0;
                while (segmentStart <= techniquePath.size()) {
                    const size_t separator = techniquePath.find("::", segmentStart);
                    const size_t segmentEnd = separator == std::string::npos ? techniquePath.size() : separator;
                    const std::string_view segment(techniquePath.data() + segmentStart, segmentEnd - segmentStart);
                    if (!segment.empty()) {
                        if (!currentPath.empty()) {
                            currentPath += "::";
                        }
                        currentPath.append(segment);
                        currentNodeIndex = ensureTechniqueNode(currentNodeIndex, currentPath, segment);
                    }

                    if (separator == std::string::npos) {
                        break;
                    }
                    segmentStart = separator + 2;
                }

                techniqueNodes[currentNodeIndex].passIndices.push_back(passIndex);
                for (int aggregateNodeIndex = currentNodeIndex; aggregateNodeIndex >= 0; aggregateNodeIndex = techniqueNodes[aggregateNodeIndex].parentIndex) {
                    auto& aggregateNode = techniqueNodes[aggregateNodeIndex];
                    aggregateNode.gpuTimeMs += stats[passIndex].gpuTimeEma;
                    aggregateNode.cpuTimeMs += stats[passIndex].GetCpuTimeEma();
                    aggregateNode.updateTimeMs += stats[passIndex].cpuUpdateTimeEma;
                    aggregateNode.executeTimeMs += stats[passIndex].cpuExecuteTimeEma;
                    aggregateNode.totalPassCount += 1;
                }
            }

            auto compareNodeValues = [&](const TechniqueTreeNode& lhs, const TechniqueTreeNode& rhs) {
                int result = 0;
                switch (static_cast<PassTimingColumn>(primarySort != nullptr ? primarySort->ColumnUserID : static_cast<ImGuiID>(PassTimingColumn::Gpu))) {
                case PassTimingColumn::Pass:
                    result = std::strcmp(lhs.label.c_str(), rhs.label.c_str());
                    break;
                case PassTimingColumn::Gpu:
                    result = compareDoubles(lhs.gpuTimeMs, rhs.gpuTimeMs);
                    break;
                case PassTimingColumn::Cpu:
                    result = compareDoubles(lhs.cpuTimeMs, rhs.cpuTimeMs);
                    break;
                case PassTimingColumn::CpuUpdate:
                    result = compareDoubles(lhs.updateTimeMs, rhs.updateTimeMs);
                    break;
                case PassTimingColumn::CpuExecute:
                    result = compareDoubles(lhs.executeTimeMs, rhs.executeTimeMs);
                    break;
                default:
                    result = compareDoubles(lhs.gpuTimeMs, rhs.gpuTimeMs);
                    break;
                }

                if (result == 0) {
                    result = std::strcmp(lhs.label.c_str(), rhs.label.c_str());
                }

                const ImGuiSortDirection direction = primarySort != nullptr
                    ? primarySort->SortDirection
                    : ImGuiSortDirection_Descending;
                return direction == ImGuiSortDirection_Ascending ? (result < 0) : (result > 0);
            };

            auto compareTechniquePasses = [&](int lhs, int rhs) {
                if (pinned[lhs] != pinned[rhs]) {
                    return pinned[lhs] && !pinned[rhs];
                }
                return compareRows(lhs, rhs, primarySort);
            };

            std::function<void(int)> sortTechniqueTree = [&](int nodeIndex) {
                auto& node = techniqueNodes[nodeIndex];
                std::stable_sort(node.childNodeIndices.begin(), node.childNodeIndices.end(), [&](int lhs, int rhs) {
                    return compareNodeValues(techniqueNodes[lhs], techniqueNodes[rhs]);
                });
                std::stable_sort(node.passIndices.begin(), node.passIndices.end(), compareTechniquePasses);
                for (int childNodeIndex : node.childNodeIndices) {
                    sortTechniqueTree(childNodeIndex);
                }
            };
            sortTechniqueTree(0);

            std::function<void(int)> drawTechniqueNode = [&](int nodeIndex) {
                auto& node = techniqueNodes[nodeIndex];
                if (nodeIndex != 0) {
                    ImGui::PushID(nodeIndex + 100000);
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%u", node.totalPassCount);

                    ImGui::TableSetColumnIndex(1);
                    ImGuiTreeNodeFlags treeFlags = ImGuiTreeNodeFlags_SpanFullWidth;
                    if (node.childNodeIndices.empty() && node.passIndices.empty()) {
                        treeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                    }
                    const bool open = ImGui::TreeNodeEx("TechniqueRow", treeFlags, "%s", node.label.c_str());

                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%.3f", node.gpuTimeMs);

                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%.3f", node.cpuTimeMs);

                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%.3f", node.updateTimeMs);

                    ImGui::TableSetColumnIndex(5);
                    ImGui::Text("%.3f", node.executeTimeMs);

                    if (!open) {
                        ImGui::PopID();
                        return;
                    }

                    for (int childNodeIndex : node.childNodeIndices) {
                        drawTechniqueNode(childNodeIndex);
                    }
                    for (int passIndex : node.passIndices) {
                        drawRow(passIndex);
                    }
                    ImGui::TreePop();
                    ImGui::PopID();
                    return;
                }

                for (int childNodeIndex : node.childNodeIndices) {
                    drawTechniqueNode(childNodeIndex);
                }
                for (int passIndex : node.passIndices) {
                    drawRow(passIndex);
                }
            };

            drawTechniqueNode(0);
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

