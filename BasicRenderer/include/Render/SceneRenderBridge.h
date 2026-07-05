#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include <flecs.h>

#include "Render/SceneFrameSnapshot.h"
#include "Scene/Scene.h"

class ManagerInterface;
class ViewManager;

namespace br::render {

class SceneRenderBridge {
public:
    struct BridgedEntityState {
        uint64_t renderEntityId = 0;
        uint64_t lastSeenFrame = 0;
        uint64_t meshGeneration = 0;
        uint64_t instanceTransformGeneration = 0;
        DirectX::XMMATRIX lastMatrix = DirectX::XMMatrixIdentity();
    };

    SceneFrameSnapshot ExportSnapshot(Scene& scene, uint64_t snapshotSequence, uint64_t sourceFrameNumber) const;
    void IngestSnapshot(const SceneFrameSnapshot& snapshot, const ManagerInterface& managerInterface);
    void Sync(Scene& scene, const ManagerInterface& managerInterface);
    void Clear(const ManagerInterface& managerInterface);

    bool HasPrimaryCamera() const;
    flecs::entity GetSceneRoot() const;
    flecs::entity GetPrimaryCameraEntity() const;
    void ResyncPrimaryCameraDepth(ViewManager& viewManager, uint32_t renderWidth, uint32_t renderHeight);

private:
    void EnsureExportQueries(flecs::world& sceneWorld) const;
    void InvalidateExportQueries();

    std::unordered_map<uint64_t, BridgedEntityState> m_bridgedEntities;
    uint64_t m_sceneRootEntityId = 0;
    uint64_t m_primaryCameraEntityId = 0;
    uint64_t m_currentIngestionFrame = 0;

    // Cached export queries (mutable because ExportSnapshot is const)
    mutable flecs::query<Components::StableSceneID, Components::Matrix, Components::MeshInstances> m_exportRenderableQuery;
    mutable flecs::query<Components::StableSceneID, Components::Matrix, Components::MeshInstances> m_exportDirtyRenderableQuery;
    mutable flecs::query<Components::StableSceneID, Components::Matrix, Components::MeshInstances> m_exportTransformUpdatedRenderableQuery;
    mutable flecs::query<Components::StableSceneID, Components::Matrix, Components::Camera> m_exportCameraQuery;
    mutable flecs::query<Components::StableSceneID, Components::Matrix, Components::Camera> m_exportDirtyCameraQuery;
    mutable flecs::query<Components::StableSceneID, Components::Matrix, Components::Camera> m_exportTransformUpdatedCameraQuery;
    mutable flecs::query<Components::StableSceneID, Components::Matrix, Components::Light> m_exportLightQuery;
    mutable flecs::query<Components::StableSceneID, Components::Matrix, Components::Light> m_exportDirtyLightQuery;
    mutable flecs::query<Components::StableSceneID, Components::Matrix, Components::Light> m_exportTransformUpdatedLightQuery;
    mutable uint64_t m_cachedExportSceneID = 0;

    // Export-side generation cache for detecting mesh changes without scene-side flags
    mutable std::unordered_map<uint64_t, uint64_t> m_lastExportedMeshGeneration;
    mutable std::unordered_set<uint64_t> m_lastExportedAliveRenderableIDs;
    mutable std::unordered_set<uint64_t> m_lastExportedAliveCameraIDs;
    mutable std::unordered_set<uint64_t> m_lastExportedAliveLightIDs;
    mutable uint64_t m_lastExportedMeshLibraryGeneration = 0;
    mutable Components::DrawStats m_lastExportedDrawStats;
    mutable bool m_hasLastExportedDrawStats = false;
    mutable bool m_hasLastExportedMeshLibrary = false;
    mutable bool m_needsFullRenderableExport = true;

    // Hints for vector pre-reservation
    mutable size_t m_lastRenderableCount = 0;
    mutable size_t m_lastChangedRenderableCount = 0;
    mutable size_t m_lastCameraCount = 0;
    mutable size_t m_lastLightCount = 0;
    uint32_t m_lastRenderWidth = 0;
    uint32_t m_lastRenderHeight = 0;
    uint16_t m_lastShadowResolution = 0;
    uint8_t m_lastDirectionalCascadeCount = 0;
    float m_lastMaxShadowDistance = 0.0f;
    float m_lastDirectionalShadowVerticalExtent = 0.0f;
    bool m_lastHasPrimaryCamera = false;
    bool m_hasLightResourceSettings = false;
};

} // namespace br::render
