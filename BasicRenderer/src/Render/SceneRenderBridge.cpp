#include "Render/SceneRenderBridge.h"

#include <algorithm>
#include <exception>
#include <unordered_set>
#include <vector>

#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

#include "Render/SceneIngestionServices.h"
#include "Render/SceneEntityMaterializationService.h"
#include "Render/SceneSourceStateStore.h"
#include "Materials/Material.h"
#include "Mesh/MeshInstance.h"
#include "Scene/Components.h"
#include "Resources/components.h"

namespace {

struct BridgedSceneEntity {};

// Ingestion mutates the renderer ECS and legacy allocation services as one
// ordered operation. Until those stores support rollback, an exception after
// mutation begins leaves their relationship indeterminate. Continuing would
// allow a later source revision to publish from partially materialized state.
class CriticalIngestionGuard {
public:
    explicit CriticalIngestionGuard(std::uint64_t sourceRevision) noexcept
        : m_sourceRevision(sourceRevision), m_uncaughtOnEntry(std::uncaught_exceptions()) {}

    CriticalIngestionGuard(const CriticalIngestionGuard&) = delete;
    CriticalIngestionGuard& operator=(const CriticalIngestionGuard&) = delete;

    ~CriticalIngestionGuard() noexcept {
        if (!m_committed && std::uncaught_exceptions() > m_uncaughtOnEntry) {
            try {
                spdlog::critical(
                    "Scene ingestion revision {} failed after renderer-state mutation began; terminating because rollback is unavailable",
                    m_sourceRevision);
            } catch (...) {
            }
            std::terminate();
        }
    }

    void Commit() noexcept { m_committed = true; }

private:
    std::uint64_t m_sourceRevision = 0;
    int m_uncaughtOnEntry = 0;
    bool m_committed = false;
};

bool HasSkinningPassEligibleMeshes(const Components::MeshInstances* meshInstances) {
    if (!meshInstances) {
        return false;
    }

    for (const auto& meshInstance : meshInstances->meshInstances) {
        if (!meshInstance || !meshInstance->HasSkin()) {
            continue;
        }

        const auto mesh = meshInstance->GetMesh();
        if (mesh && !mesh->IsCLodMesh()) {
            return true;
        }
    }

    return false;
}

void CopyCommonComponents(flecs::entity dst, flecs::entity src) {
    dst.add<BridgedSceneEntity>();
    dst.add<Components::Active>();

    if (auto name = src.try_get<Components::Name>()) {
        dst.set<Components::Name>(*name);
    }

    if (auto matrix = src.try_get<Components::Matrix>()) {
        dst.set<Components::Matrix>(*matrix);
    }

    if (auto stableSceneID = src.try_get<Components::StableSceneID>()) {
        dst.set<Components::StableSceneID>(*stableSceneID);
    }
}

void CopyCommonComponents(flecs::entity dst, uint64_t stableSceneID, const std::string& name, const Components::Matrix& matrix) {
    dst.add<BridgedSceneEntity>();
    dst.add<Components::Active>();
    dst.set<Components::StableSceneID>({ stableSceneID });
    dst.set<Components::Matrix>(matrix);

    if (!name.empty()) {
        dst.set<Components::Name>(name);
    } else {
        dst.remove<Components::Name>();
    }
}

flecs::entity GetOrCreateBridgedEntity(
    flecs::world& renderWorld,
    std::unordered_map<uint64_t, br::render::SceneRenderBridge::BridgedEntityState>& bridgedEntities,
    uint64_t stableSceneID,
    uint64_t currentFrame,
    flecs::entity sceneRoot) {
    if (auto it = bridgedEntities.find(stableSceneID); it != bridgedEntities.end()) {
        it->second.lastSeenFrame = currentFrame;
        flecs::entity existing{ renderWorld, it->second.renderEntityId };
        if (existing.is_alive()) {
            if (sceneRoot.is_alive()) {
                existing.child_of(sceneRoot);
            }
            return existing;
        }
    }

    auto dst = renderWorld.entity();
    if (sceneRoot.is_alive()) {
        dst.child_of(sceneRoot);
    }
    bridgedEntities[stableSceneID] = { dst.id(), currentFrame, 0, 0, DirectX::XMMatrixIdentity() };
    return dst;
}

flecs::entity EnsureExternalSceneRoot(flecs::world& renderWorld, uint64_t& sceneRootEntityId) {
    if (sceneRootEntityId != 0) {
        flecs::entity root{ renderWorld, sceneRootEntityId };
        if (root.is_alive()) {
            return root;
        }
    }

    auto root = renderWorld.entity("SARP External Scene")
        .add<Components::SceneRoot>()
        .add<Components::ActiveScene>()
        .add<Components::Active>()
        .set<Components::StableSceneID>({ 0x53415250524F4F54ULL })
        .set<Components::Name>("SARP External Scene")
        .set<Components::Matrix>(DirectX::XMMatrixIdentity());
    sceneRootEntityId = root.id();
    return root;
}

uint64_t GetStableSceneID(flecs::entity entity) {
    const auto* stableSceneID = entity.try_get<Components::StableSceneID>();
    if (!stableSceneID) {
        throw std::runtime_error("SceneRenderBridge requires Components::StableSceneID on exported entities");
    }

    return stableSceneID->value;
}

void DestroyBridgedEntity(
    flecs::world& renderWorld,
    uint64_t renderEntityId,
    const br::render::SceneIngestionServices& services) {
    flecs::entity entity{ renderWorld, renderEntityId };
    if (!entity.is_alive()) {
        return;
    }

    if (services.sceneEntities) services.sceneEntities->Destroy(entity);
    entity.destruct();
}

bool MatricesEqual(const DirectX::XMMATRIX& a, const DirectX::XMMATRIX& b) {
    for (int i = 0; i < 4; ++i) {
        if (DirectX::XMVector4NotEqual(a.r[i], b.r[i]))
            return false;
    }
    return true;
}

Components::MeshInstances FreezeMeshInstances(const Components::MeshInstances& source) {
    Components::MeshInstances frozen;
    frozen.generation = source.generation;
    frozen.meshInstances.reserve(source.meshInstances.size());
    for (const auto& instance : source.meshInstances) {
        frozen.meshInstances.push_back(instance
            ? MeshInstance::CreateFrozenCopy(*instance)
            : nullptr);
    }
    return frozen;
}

} // namespace

namespace br::render {

void SceneRenderBridge::EnsureExportQueries(flecs::world& sceneWorld) const {
    const auto sceneID = reinterpret_cast<uint64_t>(sceneWorld.c_ptr()); // use world pointer as cache key
    if (m_cachedExportSceneID == sceneID && m_exportRenderableQuery) {
        return;
    }

    m_exportRenderableQuery = sceneWorld.query_builder<Components::StableSceneID, Components::Matrix, Components::MeshInstances>()
        .with<Components::Active>()
        .build();
    m_exportDirtyRenderableQuery = sceneWorld.query_builder<Components::StableSceneID, Components::Matrix, Components::MeshInstances>()
        .with<Components::Active>()
        .with<Components::RenderBridgeContentDirty>()
        .build();
    m_exportTransformUpdatedRenderableQuery = sceneWorld.query_builder<Components::StableSceneID, Components::Matrix, Components::MeshInstances>()
        .with<Components::Active>()
        .with<Components::TransformUpdatedThisFrame>()
        .build();
    m_exportCameraQuery = sceneWorld.query_builder<Components::StableSceneID, Components::Matrix, Components::Camera>()
        .with<Components::Active>()
        .build();
    m_exportDirtyCameraQuery = sceneWorld.query_builder<Components::StableSceneID, Components::Matrix, Components::Camera>()
        .with<Components::Active>()
        .with<Components::RenderBridgeContentDirty>()
        .build();
    m_exportTransformUpdatedCameraQuery = sceneWorld.query_builder<Components::StableSceneID, Components::Matrix, Components::Camera>()
        .with<Components::Active>()
        .with<Components::TransformUpdatedThisFrame>()
        .build();
    m_exportLightQuery = sceneWorld.query_builder<Components::StableSceneID, Components::Matrix, Components::Light>()
        .with<Components::Active>()
        .build();
    m_exportDirtyLightQuery = sceneWorld.query_builder<Components::StableSceneID, Components::Matrix, Components::Light>()
        .with<Components::Active>()
        .with<Components::RenderBridgeContentDirty>()
        .build();
    m_exportTransformUpdatedLightQuery = sceneWorld.query_builder<Components::StableSceneID, Components::Matrix, Components::Light>()
        .with<Components::Active>()
        .with<Components::TransformUpdatedThisFrame>()
        .build();
    m_cachedExportSceneID = sceneID;
}

void SceneRenderBridge::InvalidateExportQueries() {
    m_exportRenderableQuery = {};
    m_exportDirtyRenderableQuery = {};
    m_exportTransformUpdatedRenderableQuery = {};
    m_exportCameraQuery = {};
    m_exportDirtyCameraQuery = {};
    m_exportTransformUpdatedCameraQuery = {};
    m_exportLightQuery = {};
    m_exportDirtyLightQuery = {};
    m_exportTransformUpdatedLightQuery = {};
    m_cachedExportSceneID = 0;
    m_needsFullRenderableExport = true;
}

SceneFrameSnapshot SceneRenderBridge::ExportSnapshot(Scene& scene, uint64_t snapshotSequence, uint64_t sourceFrameNumber) const {
    ZoneScopedN("SceneRenderBridge::ExportSnapshot");
    bool fullRenderableExport = false;

    SceneFrameSnapshot snapshot;
    snapshot.sceneID = scene.GetSceneID();
    snapshot.snapshotSequence = snapshotSequence;
    snapshot.sourceFrameNumber = sourceFrameNumber;

    auto sceneWorld = scene.GetRoot().world();

    if (const auto* drawStats = sceneWorld.try_get<Components::DrawStats>()) {
        ZoneScopedN("SceneRenderBridge::ExportSnapshot::DrawStats");
        snapshot.drawStatsChanged = !m_hasLastExportedDrawStats
            || m_lastExportedDrawStats.numDrawsInScene != drawStats->numDrawsInScene
            || m_lastExportedDrawStats.numDrawsPerTechnique != drawStats->numDrawsPerTechnique;
        if (snapshot.drawStatsChanged) {
            snapshot.drawStats = *drawStats;
            m_lastExportedDrawStats = *drawStats;
            m_hasLastExportedDrawStats = true;
        }
    } else {
        snapshot.drawStatsChanged = m_hasLastExportedDrawStats;
        m_lastExportedDrawStats = {};
        m_hasLastExportedDrawStats = false;
    }
    if (const auto* meshLibrary = sceneWorld.try_get<Components::GlobalMeshLibrary>()) {
        ZoneScopedN("SceneRenderBridge::ExportSnapshot::MeshLibrary");
        snapshot.meshLibraryChanged = !m_hasLastExportedMeshLibrary
            || meshLibrary->generation != m_lastExportedMeshLibraryGeneration;
        if (snapshot.meshLibraryChanged) {
            snapshot.meshLibrary = *meshLibrary;
            snapshot.retainedMeshArtifacts.reserve(meshLibrary->meshes.size());
            for (const auto& [_, weakMesh] : meshLibrary->meshes) {
                if (auto mesh = weakMesh.lock()) {
                    snapshot.retainedMeshArtifacts.push_back(std::move(mesh));
                }
            }
            m_lastExportedMeshLibraryGeneration = meshLibrary->generation;
            m_hasLastExportedMeshLibrary = true;
        }
    } else {
        snapshot.meshLibraryChanged = m_hasLastExportedMeshLibrary;
        m_lastExportedMeshLibraryGeneration = 0;
        m_hasLastExportedMeshLibrary = false;
    }

    EnsureExportQueries(sceneWorld);

    if (auto* sceneDiff = sceneWorld.try_get_mut<Components::RenderBridgeSceneDiff>()) {
        ZoneScopedN("SceneRenderBridge::ExportSnapshot::SceneDiff");
        snapshot.removedRenderableIDs = std::move(sceneDiff->removedRenderableIDs);
        snapshot.removedCameraIDs = std::move(sceneDiff->removedCameraIDs);
        snapshot.removedLightIDs = std::move(sceneDiff->removedLightIDs);
        sceneDiff->removedRenderableIDs.clear();
        sceneDiff->removedCameraIDs.clear();
        sceneDiff->removedLightIDs.clear();
    }

    auto* dirtyState = sceneWorld.try_get_mut<Components::RenderBridgeDirtyState>();
    fullRenderableExport = m_needsFullRenderableExport;
    if (!fullRenderableExport) {
        snapshot.aliveSetsComplete = false;
        snapshot.aliveSetsChanged = false;
    }
    const bool renderableChangesPending = fullRenderableExport || dirtyState == nullptr || dirtyState->renderables;
    const bool lightChangesPending = fullRenderableExport || dirtyState == nullptr || dirtyState->lights;

    {
        ZoneScopedN("SceneRenderBridge::ExportSnapshot::Renderables");
        snapshot.aliveRenderableIDs.reserve(m_lastRenderableCount);
        snapshot.changedRenderables.reserve(m_lastChangedRenderableCount);
        std::unordered_set<uint64_t> emittedRenderableIDs;
        emittedRenderableIDs.reserve(fullRenderableExport ? m_lastChangedRenderableCount : 256u);
        std::vector<flecs::entity> dirtyRenderableEntities;

        auto emitRenderable = [&](flecs::entity src, const Components::StableSceneID& stableSceneID, const Components::Matrix& matrix, const Components::MeshInstances& meshInstances) {
            const bool transformChanged = src.has<Components::TransformUpdatedThisFrame>();
            const bool wasAlive = m_lastExportedAliveRenderableIDs.contains(stableSceneID.value);
            auto genIt = m_lastExportedMeshGeneration.find(stableSceneID.value);
            const auto* instanceTransforms = src.try_get<Components::InstanceTransforms>();
            const uint64_t combinedGeneration =
                meshInstances.generation ^
                (instanceTransforms ? (instanceTransforms->generation + 0x9E3779B97F4A7C15ull) : 0ull);
            const bool meshChanged = !wasAlive || genIt == m_lastExportedMeshGeneration.end() || genIt->second != combinedGeneration;
            const bool isNew = !wasAlive;

            if (!emittedRenderableIDs.insert(stableSceneID.value).second) {
                return;
            }

            if (isNew && !fullRenderableExport) {
                snapshot.aliveSetsChanged = true;
                m_lastExportedAliveRenderableIDs.insert(stableSceneID.value);
            }

            if (transformChanged || meshChanged || isNew) {
                SnapshotRenderable renderable;
                renderable.stableID = stableSceneID.value;
                renderable.matrix = matrix;
                renderable.meshInstances = FreezeMeshInstances(meshInstances);
                if (instanceTransforms) {
                    renderable.instanceTransforms = *instanceTransforms;
                    renderable.hasInstanceTransforms = true;
                }
                renderable.transformChanged = transformChanged;
                if (const auto* name = src.try_get<Components::Name>()) {
                    renderable.name = name->name;
                }
                renderable.skinned = src.has<Components::Skinned>();
                renderable.skipShadowPass = src.has<Components::SkipShadowPass>();
                snapshot.changedRenderables.push_back(std::move(renderable));
                m_lastExportedMeshGeneration[stableSceneID.value] = combinedGeneration;
            }
        };

        if (renderableChangesPending) {
        if (fullRenderableExport) {
            m_exportRenderableQuery.each([&](flecs::entity src, const Components::StableSceneID& stableSceneID, const Components::Matrix& matrix, const Components::MeshInstances& meshInstances) {
                if (src.has<Components::RenderBridgeContentDirty>()) {
                    dirtyRenderableEntities.push_back(src);
                }
                snapshot.aliveRenderableIDs.insert(stableSceneID.value);
                emitRenderable(src, stableSceneID, matrix, meshInstances);
            });
            m_lastRenderableCount = snapshot.aliveRenderableIDs.size();
        } else {
            snapshot.aliveSetsComplete = false;
            snapshot.aliveSetsChanged = false;
            m_exportDirtyRenderableQuery.each([&](flecs::entity src, const Components::StableSceneID& stableSceneID, const Components::Matrix& matrix, const Components::MeshInstances& meshInstances) {
                dirtyRenderableEntities.push_back(src);
                emitRenderable(src, stableSceneID, matrix, meshInstances);
            });
            m_exportTransformUpdatedRenderableQuery.each([&](flecs::entity src, const Components::StableSceneID& stableSceneID, const Components::Matrix& matrix, const Components::MeshInstances& meshInstances) {
                emitRenderable(src, stableSceneID, matrix, meshInstances);
            });
            m_lastRenderableCount = m_lastExportedAliveRenderableIDs.size();
        }
        }
        else {
            m_lastRenderableCount = m_lastExportedAliveRenderableIDs.size();
        }

        if (!dirtyRenderableEntities.empty()) {
            sceneWorld.defer_begin();
            for (auto entity : dirtyRenderableEntities) {
                entity.remove<Components::RenderBridgeContentDirty>();
            }
            sceneWorld.defer_end();
        }

        m_lastChangedRenderableCount = snapshot.changedRenderables.size();
        if (dirtyState && renderableChangesPending) {
            dirtyState->renderables = false;
        }
    }

    {
        ZoneScopedN("SceneRenderBridge::ExportSnapshot::Cameras");
        snapshot.aliveCameraIDs.reserve(m_lastCameraCount);
        std::vector<flecs::entity> dirtyCameraEntities;
        std::unordered_set<uint64_t> emittedCameraIDs;
        emittedCameraIDs.reserve(4);

        auto emitCamera = [&](flecs::entity src, const Components::StableSceneID& stableSceneID, const Components::Matrix& matrix, const Components::Camera& camera) {
            if (!emittedCameraIDs.insert(stableSceneID.value).second) {
                return;
            }
            const bool isNew = !m_lastExportedAliveCameraIDs.contains(stableSceneID.value);
            if (isNew) {
                snapshot.aliveSetsChanged = true;
                m_lastExportedAliveCameraIDs.insert(stableSceneID.value);
            }

            SnapshotCamera snapshotCamera;
            snapshotCamera.stableID = stableSceneID.value;
            snapshotCamera.matrix = matrix;
            snapshotCamera.camera = camera;
            snapshotCamera.primary = src.has<Components::PrimaryCamera>();
            if (const auto* externalMatrices = src.try_get<Components::ExternalCameraMatrices>()) {
                snapshotCamera.useExternalMatrices = true;
                snapshotCamera.externalMatrices = *externalMatrices;
            }
            if (const auto* name = src.try_get<Components::Name>()) {
                snapshotCamera.name = name->name;
            }
            if (snapshotCamera.primary) {
                snapshot.hasPrimaryCamera = true;
                snapshot.primaryCameraStableID = snapshotCamera.stableID;
            }
            snapshot.changedCameras.push_back(std::move(snapshotCamera));
        };

        if (fullRenderableExport) {
        m_exportCameraQuery.each([&](flecs::entity src, const Components::StableSceneID& stableSceneID, const Components::Matrix& matrix, const Components::Camera& camera) {
        if (src.has<Components::RenderBridgeContentDirty>()) {
            dirtyCameraEntities.push_back(src);
        }
        snapshot.aliveCameraIDs.insert(stableSceneID.value);

        // Always export cameras — they're few and often change (jitter, movement)
        SnapshotCamera snapshotCamera;
        snapshotCamera.stableID = stableSceneID.value;
        snapshotCamera.matrix = matrix;
        snapshotCamera.camera = camera;
        snapshotCamera.primary = src.has<Components::PrimaryCamera>();
        if (const auto* externalMatrices = src.try_get<Components::ExternalCameraMatrices>()) {
            snapshotCamera.useExternalMatrices = true;
            snapshotCamera.externalMatrices = *externalMatrices;
        }
        if (const auto* name = src.try_get<Components::Name>()) {
            snapshotCamera.name = name->name;
        }
        if (snapshotCamera.primary) {
            snapshot.hasPrimaryCamera = true;
            snapshot.primaryCameraStableID = snapshotCamera.stableID;
        }
        snapshot.changedCameras.push_back(std::move(snapshotCamera));
        });
        m_lastCameraCount = snapshot.aliveCameraIDs.size();
        }
        else {
            if (scene.HasUsablePrimaryCamera()) {
                auto primaryCamera = scene.GetPrimaryCamera();
                const auto* stableSceneID = primaryCamera.try_get<Components::StableSceneID>();
                const auto* matrix = primaryCamera.try_get<Components::Matrix>();
                const auto* camera = primaryCamera.try_get<Components::Camera>();
                if (stableSceneID && matrix && camera) {
                    if (primaryCamera.has<Components::RenderBridgeContentDirty>()) {
                        dirtyCameraEntities.push_back(primaryCamera);
                    }
                    emitCamera(primaryCamera, *stableSceneID, *matrix, *camera);
                }
            }
            else if (dirtyState == nullptr || dirtyState->cameras) {
                m_exportDirtyCameraQuery.each([&](flecs::entity src, const Components::StableSceneID& stableSceneID, const Components::Matrix& matrix, const Components::Camera& camera) {
                    dirtyCameraEntities.push_back(src);
                    emitCamera(src, stableSceneID, matrix, camera);
                });
                m_exportTransformUpdatedCameraQuery.each([&](flecs::entity src, const Components::StableSceneID& stableSceneID, const Components::Matrix& matrix, const Components::Camera& camera) {
                    emitCamera(src, stableSceneID, matrix, camera);
                });
            }
            m_lastCameraCount = m_lastExportedAliveCameraIDs.size();
        }

        if (!dirtyCameraEntities.empty()) {
            sceneWorld.defer_begin();
            for (auto entity : dirtyCameraEntities) {
                entity.remove<Components::RenderBridgeContentDirty>();
            }
            sceneWorld.defer_end();
        }
        if (dirtyState) {
            dirtyState->cameras = false;
        }
    }

    {
        ZoneScopedN("SceneRenderBridge::ExportSnapshot::Lights");
        snapshot.aliveLightIDs.reserve(m_lastLightCount);
        std::vector<flecs::entity> dirtyLightEntities;
        std::unordered_set<uint64_t> emittedLightIDs;
        emittedLightIDs.reserve(4);

        auto emitLight = [&](flecs::entity src, const Components::StableSceneID& stableSceneID, const Components::Matrix& matrix, const Components::Light& light) {
            if (!emittedLightIDs.insert(stableSceneID.value).second) {
                return;
            }
            const bool isNew = !m_lastExportedAliveLightIDs.contains(stableSceneID.value);
            if (isNew) {
                snapshot.aliveSetsChanged = true;
                m_lastExportedAliveLightIDs.insert(stableSceneID.value);
            }

            SnapshotLight snapshotLight;
            snapshotLight.stableID = stableSceneID.value;
            snapshotLight.matrix = matrix;
            snapshotLight.light = light;
            snapshotLight.skipShadowPass = src.has<Components::SkipShadowPass>();
            if (const auto* frustumPlanes = src.try_get<Components::FrustumPlanes>()) {
                snapshotLight.frustumPlanes = *frustumPlanes;
            }
            if (const auto* name = src.try_get<Components::Name>()) {
                snapshotLight.name = name->name;
            }
            snapshot.changedLights.push_back(std::move(snapshotLight));
        };

        if (lightChangesPending) {
        if (fullRenderableExport) {
        m_exportLightQuery.each([&](flecs::entity src, const Components::StableSceneID& stableSceneID, const Components::Matrix& matrix, const Components::Light& light) {
        if (src.has<Components::RenderBridgeContentDirty>()) {
            dirtyLightEntities.push_back(src);
        }
        snapshot.aliveLightIDs.insert(stableSceneID.value);

        const bool transformChanged = src.has<Components::TransformUpdatedThisFrame>();
        const bool isNew = !m_lastExportedAliveLightIDs.contains(stableSceneID.value);

        if (transformChanged || isNew) {
            SnapshotLight snapshotLight;
            snapshotLight.stableID = stableSceneID.value;
            snapshotLight.matrix = matrix;
            snapshotLight.light = light;
            snapshotLight.skipShadowPass = src.has<Components::SkipShadowPass>();
            if (const auto* frustumPlanes = src.try_get<Components::FrustumPlanes>()) {
                snapshotLight.frustumPlanes = *frustumPlanes;
            }
            if (const auto* name = src.try_get<Components::Name>()) {
                snapshotLight.name = name->name;
            }
            snapshot.changedLights.push_back(std::move(snapshotLight));
        }
        });
        m_lastLightCount = snapshot.aliveLightIDs.size();
        }
        else {
            m_exportDirtyLightQuery.each([&](flecs::entity src, const Components::StableSceneID& stableSceneID, const Components::Matrix& matrix, const Components::Light& light) {
                dirtyLightEntities.push_back(src);
                emitLight(src, stableSceneID, matrix, light);
            });
            m_exportTransformUpdatedLightQuery.each([&](flecs::entity src, const Components::StableSceneID& stableSceneID, const Components::Matrix& matrix, const Components::Light& light) {
                emitLight(src, stableSceneID, matrix, light);
            });
            m_lastLightCount = m_lastExportedAliveLightIDs.size();
        }
        }
        else {
            m_lastLightCount = m_lastExportedAliveLightIDs.size();
        }

        if (!dirtyLightEntities.empty()) {
            sceneWorld.defer_begin();
            for (auto entity : dirtyLightEntities) {
                entity.remove<Components::RenderBridgeContentDirty>();
            }
            sceneWorld.defer_end();
        }
        if (dirtyState && lightChangesPending) {
            dirtyState->lights = false;
        }
    }

    if (!snapshot.removedRenderableIDs.empty() || !snapshot.removedCameraIDs.empty() || !snapshot.removedLightIDs.empty()) {
        ZoneScopedN("SceneRenderBridge::ExportSnapshot::ApplyRemovedDiffs");
        snapshot.aliveSetsChanged = true;
        for (const auto stableSceneID : snapshot.removedRenderableIDs) {
            m_lastExportedAliveRenderableIDs.erase(stableSceneID);
            m_lastExportedMeshGeneration.erase(stableSceneID);
        }
        for (const auto stableSceneID : snapshot.removedCameraIDs) {
            m_lastExportedAliveCameraIDs.erase(stableSceneID);
        }
        for (const auto stableSceneID : snapshot.removedLightIDs) {
            m_lastExportedAliveLightIDs.erase(stableSceneID);
        }
        m_lastRenderableCount = m_lastExportedAliveRenderableIDs.size();
        m_lastCameraCount = m_lastExportedAliveCameraIDs.size();
        m_lastLightCount = m_lastExportedAliveLightIDs.size();
    }

    if (snapshot.aliveSetsComplete) {
        snapshot.aliveSetsChanged =
            snapshot.aliveRenderableIDs != m_lastExportedAliveRenderableIDs ||
            snapshot.aliveCameraIDs != m_lastExportedAliveCameraIDs ||
            snapshot.aliveLightIDs != m_lastExportedAliveLightIDs;
    }
    if (snapshot.aliveSetsChanged && snapshot.aliveSetsComplete) {
        ZoneScopedN("SceneRenderBridge::ExportSnapshot::UpdateAliveCaches");
        for (auto it = m_lastExportedMeshGeneration.begin(); it != m_lastExportedMeshGeneration.end();) {
            if (!snapshot.aliveRenderableIDs.contains(it->first)) {
                it = m_lastExportedMeshGeneration.erase(it);
            } else {
                ++it;
            }
        }
        m_lastExportedAliveRenderableIDs = snapshot.aliveRenderableIDs;
        m_lastExportedAliveCameraIDs = snapshot.aliveCameraIDs;
        m_lastExportedAliveLightIDs = snapshot.aliveLightIDs;
    }
    m_needsFullRenderableExport = false;

    return snapshot;
}

void SceneRenderBridge::Clear(const SceneIngestionServices& services) {
    m_sourceStore = nullptr;
    m_retainedSourceMeshes.clear();
    m_retainedSourceMeshes.clear();
    if (!services.source || !services.source->Available()) {
        m_bridgedEntities.clear();
        m_sceneRootEntityId = 0;
        m_primaryCameraEntityId = 0;
        m_lastExportedMeshGeneration.clear();
        m_lastExportedAliveRenderableIDs.clear();
        m_lastExportedAliveCameraIDs.clear();
        m_lastExportedAliveLightIDs.clear();
        m_lastExportedMeshLibraryGeneration = 0;
        m_hasLastExportedDrawStats = false;
        m_hasLastExportedMeshLibrary = false;
        InvalidateExportQueries();
        return;
    }

    auto source = services.source->AcquireWrite();
    auto& renderWorld = source.World();

    for (const auto& [stableSceneID, state] : m_bridgedEntities) {
        DestroyBridgedEntity(renderWorld, state.renderEntityId, services);
    }

    if (m_sceneRootEntityId != 0) {
        flecs::entity root{ renderWorld, m_sceneRootEntityId };
        if (root.is_alive()) {
            root.destruct();
        }
    }

    m_bridgedEntities.clear();
    m_sceneRootEntityId = 0;
    m_primaryCameraEntityId = 0;
    m_lastExportedMeshGeneration.clear();
    m_lastExportedAliveRenderableIDs.clear();
    m_lastExportedAliveCameraIDs.clear();
    m_lastExportedAliveLightIDs.clear();
    m_lastExportedMeshLibraryGeneration = 0;
    m_hasLastExportedDrawStats = false;
    m_hasLastExportedMeshLibrary = false;
    InvalidateExportQueries();
}

void SceneRenderBridge::IngestSnapshot(const SceneFrameSnapshot& snapshot,
    const SceneIngestionServices& services, const SceneIngestionConfiguration& configuration) {
    ZoneScopedN("SceneRenderBridge::IngestSnapshot");

    if (!services.source || !services.source->Available()) {
        throw std::runtime_error("SceneRenderBridge requires renderer source-state storage");
    }
    // The snapshot sequence is externally assigned. Include its immutable
    // envelope in replay identity so duplicate delivery is idempotent while a
    // conflicting reuse of the same sequence is rejected.
    std::uint64_t fingerprint = 1469598103934665603ull;
    const auto mix = [&fingerprint](std::uint64_t value) {
        fingerprint ^= value;
        fingerprint *= 1099511628211ull;
    };
    const auto mixBytes = [&fingerprint](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::byte*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            fingerprint ^= std::to_integer<std::uint8_t>(bytes[i]);
            fingerprint *= 1099511628211ull;
        }
    };
    const auto mixString = [&mixBytes](std::string_view value) {
        mixBytes(value.data(), value.size());
    };
    const auto mixIDs = [&mix](const auto& values) {
        std::vector<std::uint64_t> ordered(values.begin(), values.end());
        std::ranges::sort(ordered);
        for (const auto value : ordered) mix(value);
    };
    mix(snapshot.sceneID);
    mix(snapshot.sourceFrameNumber);
    mix(snapshot.changedRenderables.size());
    mix(snapshot.changedCameras.size());
    mix(snapshot.changedLights.size());
    mix(snapshot.removedRenderableIDs.size());
    mix(snapshot.removedCameraIDs.size());
    mix(snapshot.removedLightIDs.size());
    mix(snapshot.aliveRenderableIDs.size());
    mix(snapshot.aliveCameraIDs.size());
    mix(snapshot.aliveLightIDs.size());
    for (const auto& value : snapshot.changedRenderables) {
        mix(value.stableID);
        mix(value.meshInstances.generation);
        mix(value.instanceTransforms.generation);
        mix(value.meshInstances.meshInstances.size());
        for (const auto& instance : value.meshInstances.meshInstances) {
            if (!instance) {
                mix(0);
                continue;
            }
            const auto mesh = instance->GetMesh();
            const auto material = instance->GetEffectiveMaterial();
            mix(mesh ? mesh->GetGlobalID() : 0);
            mix(material ? material->GetMaterialID() : 0);
            const auto& instanceData = instance->GetPerMeshInstanceBufferData();
            mixBytes(&instanceData, sizeof(instanceData));
            const auto skeleton = instance->GetSkin();
            if (skeleton) {
                mix(skeleton->GetBoneCount());
                mixBytes(skeleton->GetBoneMatrices().data(),
                    skeleton->GetBoneMatrices().size_bytes());
            } else {
                mix(0);
            }
        }
        mix(value.instanceTransforms.transforms.size());
        for (const auto& transform : value.instanceTransforms.transforms) {
            mixBytes(&transform.matrix, sizeof(transform.matrix));
        }
        mix(value.instanceTransforms.meshInstanceTransformIndices.size());
        for (const auto index : value.instanceTransforms.meshInstanceTransformIndices) mix(index);
        mixBytes(&value.matrix.matrix, sizeof(value.matrix.matrix));
        mixString(value.name);
        mix(value.skinned);
        mix(value.skipShadowPass);
    }
    for (const auto& value : snapshot.changedCameras) {
        mix(value.stableID);
        mixBytes(&value.matrix.matrix, sizeof(value.matrix.matrix));
        mixBytes(&value.camera, sizeof(value.camera));
        mixString(value.name);
    }
    for (const auto& value : snapshot.changedLights) {
        mix(value.stableID);
        mixBytes(&value.matrix.matrix, sizeof(value.matrix.matrix));
        mixBytes(&value.light, sizeof(value.light));
        mixString(value.name);
    }
    mixIDs(snapshot.removedRenderableIDs);
    mixIDs(snapshot.removedCameraIDs);
    mixIDs(snapshot.removedLightIDs);
    mixIDs(snapshot.aliveRenderableIDs);
    mixIDs(snapshot.aliveCameraIDs);
    mixIDs(snapshot.aliveLightIDs);
    auto source = services.source->BeginBatch(snapshot.snapshotSequence, fingerprint);
    if (source.IsReplay()) return;
    auto& renderWorld = source.World();
    m_sourceStore = services.source;
    auto* sceneEntities = services.sceneEntities;
    if (!sceneEntities || !sceneEntities->Available()) {
        throw std::runtime_error("SceneRenderBridge requires scene-entity materialization");
    }

    // Validation above remains recoverable and cannot have changed renderer
    // state. From this point onward, any exception is process-fatal until the
    // source store and all materialization services gain atomic rollback.
    CriticalIngestionGuard criticalIngestion(snapshot.snapshotSequence);

    const auto renderResolution = configuration.renderResolution;
    const auto outputResolution = configuration.outputResolution;
    const auto shadowResolution = configuration.shadowResolution;
    const auto directionalCascadeCount = configuration.directionalCascadeCount;
    const auto maxShadowDistance = configuration.maxShadowDistance;
    const auto directionalShadowDistanceLowerBound = configuration.directionalShadowDistanceLowerBound;
    const auto directionalShadowSceneExtent = configuration.directionalShadowSceneExtent;
    const bool lightResourceSettingsChanged =
        !m_hasLightResourceSettings ||
        m_lastRenderWidth != renderResolution.x ||
        m_lastRenderHeight != renderResolution.y ||
        m_lastShadowResolution != shadowResolution ||
        m_lastDirectionalCascadeCount != directionalCascadeCount ||
        m_lastMaxShadowDistance != maxShadowDistance ||
        m_lastDirectionalShadowDistanceLowerBound != directionalShadowDistanceLowerBound ||
        m_lastDirectionalShadowSceneExtent != directionalShadowSceneExtent ||
        m_lastHasPrimaryCamera != snapshot.hasPrimaryCamera;

    ++m_currentIngestionFrame;
    m_primaryCameraEntityId = 0;
    auto sceneRoot = EnsureExternalSceneRoot(renderWorld, m_sceneRootEntityId);

    {
        ZoneScopedN("SceneRenderBridge::IngestSnapshot::GlobalComponents");
        if (snapshot.drawStatsChanged) {
            renderWorld.set<Components::DrawStats>(snapshot.drawStats);
        }
        if (snapshot.meshLibraryChanged) {
            renderWorld.set<Components::GlobalMeshLibrary>(snapshot.meshLibrary);
            m_retainedSourceMeshes = snapshot.retainedMeshArtifacts;
        }
    }

    // Process only renderables that actually changed (transform, mesh, or new)
    {
        ZoneScopedN("SceneRenderBridge::IngestSnapshot::ChangedRenderables");
        std::vector<SceneEntityMaterializationService::RenderableRequest> materializationRequests;
        materializationRequests.reserve(snapshot.changedRenderables.size());

        for (const auto& renderable : snapshot.changedRenderables) {
            auto dst = GetOrCreateBridgedEntity(renderWorld, m_bridgedEntities, renderable.stableID, m_currentIngestionFrame, sceneRoot);

            const bool isNew = !dst.has<BridgedSceneEntity>();
            auto& entityState = m_bridgedEntities[renderable.stableID];
            const bool meshChanged =
                entityState.meshGeneration != renderable.meshInstances.generation ||
                entityState.instanceTransformGeneration != renderable.instanceTransforms.generation;

            // Entity is in the changed list, so always update common components
            CopyCommonComponents(dst, renderable.stableID, renderable.name, renderable.matrix);
            entityState.lastMatrix = renderable.matrix.matrix;
            if (renderable.transformChanged || isNew || meshChanged) {
                // This is a fresh transform update, not the convergence upload
                // scheduled by the renderer after the preceding update.
                dst.remove<Components::RenderTransformNeedsConvergence>();
                dst.add<Components::RenderTransformUpdated>();
            } else if (!dst.has<Components::RenderTransformNeedsConvergence>()) {
                dst.remove<Components::RenderTransformUpdated>();
            }

            if (isNew || meshChanged) {
                const auto* instanceTransforms = renderable.hasInstanceTransforms ? &renderable.instanceTransforms : nullptr;
                materializationRequests.push_back({ dst, &renderable.meshInstances, instanceTransforms });
                entityState.meshGeneration = renderable.meshInstances.generation;
                entityState.instanceTransformGeneration = renderable.instanceTransforms.generation;
            }

            if (isNew || meshChanged) {
                if (renderable.skinned) {
                    dst.add<Components::Skinned>();
                } else {
                    dst.remove<Components::Skinned>();
                }
                if (HasSkinningPassEligibleMeshes(&renderable.meshInstances)) {
                    dst.add<Components::SkinningPassEligible>();
                } else {
                    dst.remove<Components::SkinningPassEligible>();
                }
                if (renderable.skipShadowPass) {
                    dst.add<Components::SkipShadowPass>();
                } else {
                    dst.remove<Components::SkipShadowPass>();
                }
            }
        }

        if (!materializationRequests.empty()) {
            ZoneScopedN("SceneRenderBridge::IngestSnapshot::ChangedRenderables::AddObjectsBulk");
            sceneEntities->MaterializeRenderables(materializationRequests, source.Phases());
        }
    }

    // Cameras are always exported (few entities, frequently change)
    {
        ZoneScopedN("SceneRenderBridge::IngestSnapshot::ChangedCameras");
        for (const auto& camera : snapshot.changedCameras) {
            auto dst = GetOrCreateBridgedEntity(renderWorld, m_bridgedEntities, camera.stableID, m_currentIngestionFrame, sceneRoot);
            CopyCommonComponents(dst, camera.stableID, camera.name, camera.matrix);
            dst.add<Components::RenderTransformUpdated>();
            const bool useOutputHeight =
                camera.primary &&
                configuration.primaryCameraUsesOutputHeight &&
                outputResolution.y != 0u;
            const uint32_t lodHeight = useOutputHeight ? outputResolution.y : renderResolution.y;
            sceneEntities->MaterializeCamera(dst, camera.camera, camera.primary,
                renderResolution.x, renderResolution.y, lodHeight);
            if (camera.useExternalMatrices) {
                dst.set<Components::ExternalCameraMatrices>(camera.externalMatrices);
            } else {
                dst.remove<Components::ExternalCameraMatrices>();
            }
            if (camera.primary) {
                dst.add<Components::PrimaryCamera>();
                m_primaryCameraEntityId = dst.id();
                sceneEntities->SelectCurrentCamera(dst);
            } else {
                dst.remove<Components::PrimaryCamera>();
            }
        }
    }

    // Process only lights that actually changed
    {
        ZoneScopedN("SceneRenderBridge::IngestSnapshot::ChangedLights");
        for (const auto& light : snapshot.changedLights) {
            auto dst = GetOrCreateBridgedEntity(renderWorld, m_bridgedEntities, light.stableID, m_currentIngestionFrame, sceneRoot);
            CopyCommonComponents(dst, light.stableID, light.name, light.matrix);
            dst.add<Components::RenderTransformUpdated>();
            const auto* frustumPlanes = light.frustumPlanes ? &light.frustumPlanes.value() : nullptr;
            sceneEntities->MaterializeLight(dst, light.light, frustumPlanes,
                shadowResolution, directionalCascadeCount, m_primaryCameraEntityId != 0);
            if (light.skipShadowPass) {
                dst.add<Components::SkipShadowPass>();
            } else {
                dst.remove<Components::SkipShadowPass>();
            }
        }
    }

    if (lightResourceSettingsChanged) {
        ZoneScopedN("SceneRenderBridge::IngestSnapshot::ResyncLightsForResourceSettings");
        auto resyncQuery = renderWorld.query_builder<Components::Light>()
            .with<Components::LightViewInfo>()
            .with<BridgedSceneEntity>()
            .with<Components::Active>()
            .build();
        resyncQuery.each([&](flecs::entity entity, Components::Light& light) {
            const auto* frustumPlanes = entity.try_get<Components::FrustumPlanes>();
            sceneEntities->MaterializeLight(
                entity,
                light,
                frustumPlanes,
                shadowResolution,
                directionalCascadeCount,
                m_primaryCameraEntityId != 0);
        });
    }

    m_lastRenderWidth = renderResolution.x;
    m_lastRenderHeight = renderResolution.y;
    m_lastShadowResolution = shadowResolution;
    m_lastDirectionalCascadeCount = directionalCascadeCount;
    m_lastMaxShadowDistance = maxShadowDistance;
    m_lastDirectionalShadowDistanceLowerBound = directionalShadowDistanceLowerBound;
    m_lastDirectionalShadowSceneExtent = directionalShadowSceneExtent;
    m_lastHasPrimaryCamera = snapshot.hasPrimaryCamera;
    m_hasLightResourceSettings = true;

    if (!snapshot.removedRenderableIDs.empty() || !snapshot.removedCameraIDs.empty() || !snapshot.removedLightIDs.empty()) {
        ZoneScopedN("SceneRenderBridge::IngestSnapshot::RemovedEntityDiffs");
        auto destroyByStableID = [&](uint64_t stableSceneID) {
            auto it = m_bridgedEntities.find(stableSceneID);
            if (it == m_bridgedEntities.end()) {
                return;
            }
            DestroyBridgedEntity(renderWorld, it->second.renderEntityId, services);
            m_bridgedEntities.erase(it);
        };
        for (const auto stableSceneID : snapshot.removedRenderableIDs) {
            destroyByStableID(stableSceneID);
        }
        for (const auto stableSceneID : snapshot.removedCameraIDs) {
            destroyByStableID(stableSceneID);
        }
        for (const auto stableSceneID : snapshot.removedLightIDs) {
            destroyByStableID(stableSceneID);
        }
    }

    if (snapshot.aliveSetsChanged && snapshot.aliveSetsComplete) {
        ZoneScopedN("SceneRenderBridge::IngestSnapshot::RemoveStaleEntities");
        // Remove stale entities using alive sets from the snapshot.
        std::vector<uint64_t> staleStableSceneIDs;
        for (const auto& [stableSceneID, state] : m_bridgedEntities) {
            if (!snapshot.aliveRenderableIDs.contains(stableSceneID) &&
                !snapshot.aliveCameraIDs.contains(stableSceneID) &&
                !snapshot.aliveLightIDs.contains(stableSceneID)) {
                DestroyBridgedEntity(renderWorld, state.renderEntityId, services);
                staleStableSceneIDs.push_back(stableSceneID);
            }
        }

        for (const auto stableSceneID : staleStableSceneIDs) {
            m_bridgedEntities.erase(stableSceneID);
        }
    }
    source.Commit();
    criticalIngestion.Commit();
}

void SceneRenderBridge::Sync(Scene& scene, const SceneIngestionServices& services,
    const SceneIngestionConfiguration& configuration) {
    IngestSnapshot(ExportSnapshot(scene, 0, 0), services, configuration);
}

bool SceneRenderBridge::HasPrimaryCamera() const {
    if (m_primaryCameraEntityId == 0) {
        return false;
    }

    if (!m_sourceStore || !m_sourceStore->Available()) return false;
    auto source = m_sourceStore->AcquireWrite();
    auto& renderWorld = source.World();
    flecs::entity entity{ renderWorld, m_primaryCameraEntityId };
    return entity.is_alive();
}

flecs::entity SceneRenderBridge::GetSceneRoot() const {
    if (m_sceneRootEntityId == 0 || !m_sourceStore || !m_sourceStore->Available()) {
        return {};
    }

    auto source = m_sourceStore->AcquireWrite();
    auto& renderWorld = source.World();
    flecs::entity root{ renderWorld, m_sceneRootEntityId };
    return root.is_alive() ? root : flecs::entity{};
}

flecs::entity SceneRenderBridge::GetPrimaryCameraEntity() const {
    if (!m_sourceStore || !m_sourceStore->Available()) return {};
    auto source = m_sourceStore->AcquireWrite();
    auto& renderWorld = source.World();
    return flecs::entity{ renderWorld, m_primaryCameraEntityId };
}

void SceneRenderBridge::ResyncPrimaryCameraDepth(const SceneIngestionServices& services,
    uint32_t renderWidth, uint32_t renderHeight, uint32_t primaryLodHeight) {
    if (m_primaryCameraEntityId == 0) return;
    if (!m_sourceStore || !m_sourceStore->Available()) return;
    auto source = m_sourceStore->AcquireWrite();
    auto& renderWorld = source.World();
    auto entity = flecs::entity{ renderWorld, m_primaryCameraEntityId };
    if (!entity.is_alive() || !entity.has<Components::Camera>()) return;
    const auto camera = entity.get<Components::Camera>();
    const uint32_t lodHeight = entity.has<Components::PrimaryCamera>() && primaryLodHeight != 0u
        ? primaryLodHeight : renderHeight;
    if (!services.sceneEntities || !services.sceneEntities->Available()) return;
    services.sceneEntities->MaterializeCamera(entity, camera,
        entity.has<Components::PrimaryCamera>(), renderWidth, renderHeight, lodHeight);
}

} // namespace br::render
