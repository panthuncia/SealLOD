#include <BasicRenderer/Scene/Scene.h>
#include "Diagnostics/Menu/Menu.h"

Menu::SceneExplorerNodeSnapshot Menu::BuildSceneExplorerSnapshot(flecs::entity node, size_t& remainingNodes, bool& truncated) {
    SceneExplorerNodeSnapshot snapshot;
    if (!node.is_alive()) {
        return snapshot;
    }

    if (remainingNodes == 0) {
        truncated = true;
        return snapshot;
    }
    --remainingNodes;

    if (const auto* stableSceneID = node.try_get<Components::StableSceneID>()) {
        snapshot.stableId = stableSceneID->value;
    } else {
        snapshot.stableId = static_cast<uint64_t>(node.id());
    }

    if (const auto* nameComponent = node.try_get<Components::Name>()) {
        snapshot.name = nameComponent->name;
    } else {
        snapshot.name = "Unnamed Node";
    }

    if (const auto* position = node.try_get<Components::Position>()) {
        snapshot.hasPosition = true;
        XMStoreFloat3(&snapshot.position, position->pos);
    }

    if (const auto* scale = node.try_get<Components::Scale>()) {
        DirectX::XMFLOAT3 scaleValue{};
        snapshot.hasScale = true;
        XMStoreFloat3(&scaleValue, scale->scale);
        snapshot.uniformScale = scaleValue.x;
    }

    if (const auto* rotation = node.try_get<Components::Rotation>()) {
        snapshot.hasRotation = true;
        XMStoreFloat4(&snapshot.rotation, rotation->rot);
    }

    snapshot.isRenderable = node.has<Components::RenderableObject>();
    if (snapshot.isRenderable) {
        if (const auto* meshInstances = node.try_get<Components::MeshInstances>()) {
            snapshot.meshCount = meshInstances->meshInstances.size();
        }
        snapshot.skinned = node.has<Components::Skinned>();
    }

    auto* world = node.world().c_ptr();
    ecs_iter_t it = ecs_children(world, node.id());
    while (ecs_children_next(&it)) {
        for (int32_t i = 0; i < it.count; ++i) {
            if (remainingNodes == 0) {
                truncated = true;
                ecs_iter_fini(&it);
                return snapshot;
            }

            snapshot.children.push_back(BuildSceneExplorerSnapshot(flecs::entity(world, it.entities[i]), remainingNodes, truncated));
            if (remainingNodes == 0) {
                truncated = true;
                ecs_iter_fini(&it);
                return snapshot;
            }
        }
    }

    return snapshot;
}

const Menu::SceneExplorerNodeSnapshot* Menu::FindSceneExplorerSnapshotNode(const SceneExplorerNodeSnapshot& node, uint64_t stableId) const {
    if (node.stableId == stableId) {
        return &node;
    }

    for (const auto& child : node.children) {
        if (const auto* found = FindSceneExplorerSnapshotNode(child, stableId)) {
            return found;
        }
    }

    return nullptr;
}

Menu::SceneExplorerNodeSnapshot* Menu::FindSceneExplorerSnapshotNode(SceneExplorerNodeSnapshot& node, uint64_t stableId) {
    if (node.stableId == stableId) {
        return &node;
    }

    for (auto& child : node.children) {
        if (auto* found = FindSceneExplorerSnapshotNode(child, stableId)) {
            return found;
        }
    }

    return nullptr;
}

void Menu::OverlayPendingSceneExplorerEdits() {
    if (!m_sceneExplorerSnapshotAvailable) {
        return;
    }

    constexpr float kFloatEpsilon = 1e-4f;
    for (auto it = m_sceneExplorerPendingEdits.begin(); it != m_sceneExplorerPendingEdits.end();) {
        auto* node = FindSceneExplorerSnapshotNode(m_sceneExplorerRootSnapshot, it->first);
        if (!node) {
            it = m_sceneExplorerPendingEdits.erase(it);
            continue;
        }

        bool appliedToScene = true;
        if (it->second.hasPosition) {
            appliedToScene = appliedToScene
                && node->hasPosition
                && std::fabs(node->position.x - it->second.position.x) <= kFloatEpsilon
                && std::fabs(node->position.y - it->second.position.y) <= kFloatEpsilon
                && std::fabs(node->position.z - it->second.position.z) <= kFloatEpsilon;
            node->hasPosition = true;
            node->position = it->second.position;
        }

        if (it->second.hasUniformScale) {
            appliedToScene = appliedToScene
                && node->hasScale
                && std::fabs(node->uniformScale - it->second.uniformScale) <= kFloatEpsilon;
            node->hasScale = true;
            node->uniformScale = it->second.uniformScale;
        }

        if (appliedToScene) {
            it = m_sceneExplorerPendingEdits.erase(it);
        } else {
            ++it;
        }
    }
}

void Menu::RefreshSceneExplorerSnapshot(size_t maxNodes) {
    if (m_sceneOverlapStatus.taskInFlight) {
        return;
    }

    auto root = getSceneRoot();
    if (!root) {
        m_sceneExplorerSnapshotAvailable = false;
        m_sceneExplorerSnapshotTruncated = false;
        m_sceneExplorerSnapshotNodeBudget = 0;
        m_selectedSceneNodeStableId = 0;
        m_sceneExplorerPendingEdits.clear();
        return;
    }

    size_t remainingNodes = std::max<size_t>(1, maxNodes);
    m_sceneExplorerSnapshotTruncated = false;
    m_sceneExplorerSnapshotNodeBudget = remainingNodes;
    m_sceneExplorerRootSnapshot = BuildSceneExplorerSnapshot(root, remainingNodes, m_sceneExplorerSnapshotTruncated);
    m_sceneExplorerSnapshotAvailable = true;
    OverlayPendingSceneExplorerEdits();

    if (m_selectedSceneNodeStableId != 0
        && FindSceneExplorerSnapshotNode(m_sceneExplorerRootSnapshot, m_selectedSceneNodeStableId) == nullptr) {
        m_selectedSceneNodeStableId = 0;
    }
}

void Menu::QueueSceneNodePositionChange(uint64_t stableId, const DirectX::XMFLOAT3& position) {
    auto& pendingEdit = m_sceneExplorerPendingEdits[stableId];
    pendingEdit.hasPosition = true;
    pendingEdit.position = position;
    if (queueSceneNodePositionEdit) {
        queueSceneNodePositionEdit(stableId, position);
    }
}

void Menu::QueueSceneNodeUniformScaleChange(uint64_t stableId, float uniformScale) {
    auto& pendingEdit = m_sceneExplorerPendingEdits[stableId];
    pendingEdit.hasUniformScale = true;
    pendingEdit.uniformScale = uniformScale;
    if (queueSceneNodeUniformScaleEdit) {
        queueSceneNodeUniformScaleEdit(stableId, uniformScale);
    }
}

void Menu::DisplaySceneNode(const SceneExplorerNodeSnapshot& node, bool isOnlyChild) {
    ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;

    if (node.stableId == m_selectedSceneNodeStableId) {
        nodeFlags |= ImGuiTreeNodeFlags_Selected;
    }

    if (isOnlyChild) {
        nodeFlags |= ImGuiTreeNodeFlags_DefaultOpen;
    }

    if (node.children.empty()) {
        nodeFlags |= ImGuiTreeNodeFlags_Leaf;
    }

    void* uniqueId = reinterpret_cast<void*>(static_cast<intptr_t>(node.stableId));
    if (ImGui::TreeNodeEx(uniqueId, nodeFlags, "%s", node.name.c_str())) {
        if (ImGui::IsItemClicked()) {
            m_selectedSceneNodeStableId = node.stableId;
        }

        if (node.isRenderable) {
            ImGui::Text("Meshes: %llu", static_cast<unsigned long long>(node.meshCount));
            ImGui::Text("Has Skinned: %s", node.skinned ? "Yes" : "No");
        }

        const bool childIsOnly = node.children.size() <= 1;
        for (const auto& child : node.children) {
            DisplaySceneNode(child, childIsOnly);
        }

        ImGui::TreePop();
    } else if (ImGui::IsItemClicked()) {
        m_selectedSceneNodeStableId = node.stableId;
    }
}

void Menu::DisplaySceneGraph() {
    const float lineHeight = std::max(1.0f, ImGui::GetTextLineHeightWithSpacing());
    const float availableHeight = std::max(0.0f, ImGui::GetContentRegionAvail().y);
    const size_t visibleRows = static_cast<size_t>(std::ceil(availableHeight / lineHeight));
    RefreshSceneExplorerSnapshot(std::max<size_t>(1, visibleRows + 8));

    if (!m_sceneExplorerSnapshotAvailable) {
        ImGui::TextDisabled("No scene snapshot available.");
        return;
    }

    if (m_sceneExplorerSnapshotTruncated) {
        ImGui::TextDisabled(
            "Scene graph limited to %llu visible nodes.",
            static_cast<unsigned long long>(m_sceneExplorerSnapshotNodeBudget));
    }

    DisplaySceneNode(m_sceneExplorerRootSnapshot, true);
}

void Menu::DisplaySelectedNode() {
    if (m_selectedSceneNodeStableId == 0 || !m_sceneExplorerSnapshotAvailable) {
        return;
    }

    auto* selectedNode = FindSceneExplorerSnapshotNode(m_sceneExplorerRootSnapshot, m_selectedSceneNodeStableId);
    if (!selectedNode) {
        m_selectedSceneNodeStableId = 0;
        return;
    }

    ImGui::Begin("Selected Node Transform", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::Text("Position:");
    if (selectedNode->hasPosition) {
        DirectX::XMFLOAT3 pos = selectedNode->position;
        if (ImGui::InputFloat3("Position", &pos.x)) {
            selectedNode->position = pos;
            QueueSceneNodePositionChange(selectedNode->stableId, pos);
        }
    } else {
        ImGui::TextDisabled("Position unavailable.");
    }

    ImGui::Text("Scale:");
    if (selectedNode->hasScale) {
        float uniformScale = selectedNode->uniformScale;
        if (ImGui::InputFloat("Scale", &uniformScale)) {
            selectedNode->uniformScale = uniformScale;
            QueueSceneNodeUniformScaleChange(selectedNode->stableId, uniformScale);
        }
    } else {
        ImGui::TextDisabled("Scale unavailable.");
    }

    if (selectedNode->hasRotation) {
        ImGui::Text(
            "Rotation (quaternion): (%.3f, %.3f, %.3f, %.3f)",
            selectedNode->rotation.x,
            selectedNode->rotation.y,
            selectedNode->rotation.z,
            selectedNode->rotation.w);
    } else {
        ImGui::TextDisabled("Rotation unavailable.");
    }

    ImGui::End();
}
void Menu::DrawLoadModelButton() {
    if (ImGui::Button("Load Model"))
    {
        std::wstring selectedFile;
        std::wstring customFilter = L"Scene Files\0*.glb;*.gltf;*.usd;*.usda;*.usdc;*.usdz;*.nif\0All Files\0*.*\0";
        if (OpenFileDialog(selectedFile, customFilter))
        {
			//auto exePath = GetExePath();
			//// Strip EXE path from selectedFile
			//if (selectedFile.find(exePath) == 0) {
			//	selectedFile.erase(0, exePath.length());
			//}
			//// Strip filename from selectedFile
   //         auto pathCopy = selectedFile;
			//auto lastSlash = selectedFile.find_last_of(L"\\/");
			//if (lastSlash != std::wstring::npos) {
			//	selectedFile.erase(lastSlash, selectedFile.size()-1);
			//}

            spdlog::info("Selected file: {}", ws2s(selectedFile));
			auto scene = LoadModel(ws2s(selectedFile));
			scene->GetRoot().set<Components::Name>(ws2s(getFileNameFromPath(selectedFile)));
			appendScene(scene->Clone());
        }
        else
        {
            spdlog::warn("No file selected.");
        }
    }
}

