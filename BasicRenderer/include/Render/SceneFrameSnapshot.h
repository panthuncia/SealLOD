#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "Scene/Components.h"

namespace br::render {

using StableSceneID = uint64_t;

struct SnapshotRenderable {
    StableSceneID stableID = 0;
    Components::Matrix matrix;
    Components::MeshInstances meshInstances;
    Components::InstanceTransforms instanceTransforms;
    std::string name;
    bool transformChanged = false;
    bool hasInstanceTransforms = false;
    bool skinned = false;
    bool skipShadowPass = false;
};

struct SnapshotCamera {
    StableSceneID stableID = 0;
    Components::Matrix matrix;
    Components::Camera camera;
    std::string name;
    bool primary = false;
    bool useExternalMatrices = false;
    Components::ExternalCameraMatrices externalMatrices;
};

struct SnapshotLight {
    StableSceneID stableID = 0;
    Components::Matrix matrix;
    Components::Light light;
    std::optional<Components::FrustumPlanes> frustumPlanes;
    std::string name;
    bool skipShadowPass = false;
};

struct SceneFrameSnapshot {
    uint64_t sceneID = 0;
    uint64_t snapshotSequence = 0;
    uint64_t sourceFrameNumber = 0;
    Components::DrawStats drawStats;
    Components::GlobalMeshLibrary meshLibrary;
    bool drawStatsChanged = true;
    bool meshLibraryChanged = true;

    // Complete set of alive entity IDs (for stale detection)
    std::unordered_set<StableSceneID> aliveRenderableIDs;
    std::unordered_set<StableSceneID> aliveCameraIDs;
    std::unordered_set<StableSceneID> aliveLightIDs;

    // Only entities that actually changed this frame
    std::vector<SnapshotRenderable> changedRenderables;
    std::vector<SnapshotCamera> changedCameras;
    std::vector<SnapshotLight> changedLights;
    std::vector<StableSceneID> removedRenderableIDs;
    std::vector<StableSceneID> removedCameraIDs;
    std::vector<StableSceneID> removedLightIDs;

    bool hasPrimaryCamera = false;
    StableSceneID primaryCameraStableID = 0;
    bool aliveSetsChanged = true;
    bool aliveSetsComplete = true;
};

struct SceneOverlapStatus {
    bool enabled = false;
    bool taskInFlight = false;
    bool hasCommittedSnapshot = false;
    uint64_t committedSnapshotSequence = 0;
    uint64_t pendingSnapshotSequence = 0;
    uint64_t lastCompletedSnapshotSequence = 0;
    uint64_t lastCommittedSourceFrame = 0;
    double lastTaskDurationMs = 0.0;
};

} // namespace br::render
