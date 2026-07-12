#include "Render/SceneRenderBridge.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

#include <tracy/Tracy.hpp>

#include "Managers/LightManager.h"
#include "Managers/ManagerInterface.h"
#include "Managers/ObjectManager.h"
#include "Managers/ViewManager.h"
#include "Managers/Singletons/RendererECSManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Materials/Material.h"
#include "Render/DrawWorkload.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Mesh/MeshInstance.h"
#include "Resources/Sampler.h"
#include "Scene/Components.h"
#include "Resources/components.h"
#include "Utilities/Utilities.h"

namespace {

struct BridgedSceneEntity {};
struct CameraResourceSignature {
    uint32_t depthResX = 0;
    uint32_t depthResY = 0;
    bool primary = false;
};
struct LightResourceSignature {
    Components::LightType type = Components::LightType::Directional;
    bool shadowCaster = false;
    uint16_t shadowResolution = 0;
    uint8_t directionalCascadeCount = 0;
    bool hasPrimaryCamera = false;
};
struct RenderableSignature {
    std::vector<uint64_t> meshInstanceKeys;
};

bool operator==(const CameraResourceSignature& lhs, const CameraResourceSignature& rhs) {
    return lhs.depthResX == rhs.depthResX
        && lhs.depthResY == rhs.depthResY
        && lhs.primary == rhs.primary;
}

bool operator==(const LightResourceSignature& lhs, const LightResourceSignature& rhs) {
    return lhs.type == rhs.type
        && lhs.shadowCaster == rhs.shadowCaster
        && lhs.shadowResolution == rhs.shadowResolution
        && lhs.directionalCascadeCount == rhs.directionalCascadeCount
        && lhs.hasPrimaryCamera == rhs.hasPrimaryCamera;
}

uint64_t BuildMeshInstanceKey(const std::shared_ptr<MeshInstance>& meshInstance) {
    const auto mesh = meshInstance->GetMesh();
    const auto material = meshInstance->GetEffectiveMaterial();
    const auto instanceKey = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(meshInstance.get()));
    const auto meshKey = mesh ? mesh->GetGlobalID() : 0ull;
    const auto materialKey = material ? static_cast<uint64_t>(material->GetMaterialID()) : 0ull;
    const auto perMeshKey = static_cast<uint64_t>(meshInstance->GetPerMeshBufferIndex());
    return instanceKey ^ (meshKey << 1) ^ (materialKey << 33) ^ (perMeshKey << 49);
}

RenderableSignature BuildRenderableSignature(const Components::MeshInstances* meshInstances) {
    RenderableSignature signature;
    if (!meshInstances) {
        return signature;
    }

    signature.meshInstanceKeys.reserve(meshInstances->meshInstances.size());
    for (const auto& meshInstance : meshInstances->meshInstances) {
        signature.meshInstanceKeys.push_back(BuildMeshInstanceKey(meshInstance));
    }
    return signature;
}

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

Components::PerPassMeshes BuildPerPassMeshes(const Components::MeshInstances* meshInstances) {
    Components::PerPassMeshes perPassMeshes;
    if (!meshInstances) {
        return perPassMeshes;
    }

    for (const auto& meshInstance : meshInstances->meshInstances) {
        const auto mesh = meshInstance->GetMesh();
        const auto material = meshInstance->GetEffectiveMaterial();
        ForEachMeshRenderPhase(*mesh, *(material ? material : mesh->material), [&](const RenderPhase& pass) {
            perPassMeshes.meshesByPass[pass.hash].push_back(meshInstance);
        });
    }

    return perPassMeshes;
}

void DestroyRendererObject(flecs::entity entity, ObjectManager& objectManager) {
    if (const auto* drawInfo = entity.try_get<Components::ObjectDrawInfo>()) {
        objectManager.RemoveObject(drawInfo);
        entity.remove<Components::ObjectDrawInfo>();
    }
}

void DestroyRendererCamera(flecs::entity entity, ViewManager& viewManager) {
    if (const auto* renderView = entity.try_get<Components::RenderViewRef>()) {
        viewManager.DestroyView(renderView->viewID);
        entity.remove<Components::RenderViewRef>();
    }
    entity.remove<Components::DepthMap>();
}

void DestroyRendererLight(flecs::entity entity, LightManager& lightManager) {
    if (entity.has<Components::LightViewInfo>()) {
        lightManager.RemoveLight(entity);
    }
    entity.remove<Components::DepthMap>();
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

void SyncPassMembership(flecs::entity dst, const Components::MeshInstances* meshInstances) {
    dst.remove<Components::ParticipatesInPass>(flecs::Wildcard);

    if (!meshInstances) {
        return;
    }

    const auto& renderPhaseEntities = RendererECSManager::GetInstance().GetRenderPhaseEntities();
    std::unordered_set<uint64_t> passHashes;
    for (const auto& meshInstance : meshInstances->meshInstances) {
        const auto mesh = meshInstance->GetMesh();
        const auto material = meshInstance->GetEffectiveMaterial();
        ForEachMeshRenderPhase(*mesh, *(material ? material : mesh->material), [&](const RenderPhase& pass) {
            passHashes.insert(pass.hash);
        });
    }

    for (const auto& [phase, phaseEntity] : renderPhaseEntities) {
        if (passHashes.contains(phase.hash)) {
            dst.add<Components::ParticipatesInPass>(phaseEntity);
        }
    }
}

bool SyncRenderableDerivedStateForBulk(
    flecs::entity dst,
    const Components::MeshInstances* meshInstances,
    const Components::InstanceTransforms* instanceTransforms,
    ObjectManager& objectManager,
    ObjectManager::ObjectBuildInfo& objectBuildInfo) {
    const auto newSignature = BuildRenderableSignature(meshInstances);
    const auto perPassMeshes = BuildPerPassMeshes(meshInstances);
    const auto* oldSignature = dst.try_get<RenderableSignature>();
    const bool signatureChanged = oldSignature == nullptr || oldSignature->meshInstanceKeys != newSignature.meshInstanceKeys;
    const auto* matrix = dst.try_get<Components::Matrix>();

    if (meshInstances) {
        dst.set<Components::MeshInstances>(*meshInstances);
        dst.set<Components::PerPassMeshes>(perPassMeshes);
    } else {
        dst.remove<Components::MeshInstances>();
        dst.remove<Components::PerPassMeshes>();
    }
    if (instanceTransforms) {
        dst.set<Components::InstanceTransforms>(*instanceTransforms);
    } else {
        dst.remove<Components::InstanceTransforms>();
    }

    if (!dst.has<Components::RenderableObject>()) {
        Components::RenderableObject renderable{};
        if (matrix) {
            renderable.perObjectCB.modelMatrix = matrix->matrix;
            renderable.perObjectCB.prevModelMatrix = matrix->matrix;
            renderable.perObjectCB.modelInverseMatrix = DirectX::XMMatrixInverse(nullptr, matrix->matrix);
        }
        dst.set<Components::RenderableObject>(renderable);
    }

    if (signatureChanged) {
        DestroyRendererObject(dst, objectManager);
        auto renderable = dst.get<Components::RenderableObject>();
        objectBuildInfo = { renderable.perObjectCB, meshInstances, instanceTransforms };
        dst.set<RenderableSignature>(newSignature);
    }

    SyncPassMembership(dst, meshInstances);
    return signatureChanged;
}

Components::Camera BuildRendererCamera(
    const Components::Camera& sceneCamera,
    const Components::DepthMap& depthMap,
    uint32_t width,
    uint32_t height,
    uint32_t lodHeight) {
    auto rendererCamera = sceneCamera;
    rendererCamera.info.numDepthMips = NumMips(width, height);
    rendererCamera.info.depthResX = width;
    rendererCamera.info.depthResY = height;
    rendererCamera.info.lodResY = lodHeight != 0u ? lodHeight : height;
    const auto paddedLinearDepthX = depthMap.linearDepthMap->GetInternalWidth();
    const auto paddedLinearDepthY = depthMap.linearDepthMap->GetInternalHeight();
    rendererCamera.info.uvScaleToNextPowerOfTwo = {
        static_cast<float>(width) / static_cast<float>(paddedLinearDepthX),
        static_cast<float>(height) / static_cast<float>(paddedLinearDepthY)
    };
    return rendererCamera;
}

void SyncCameraDerivedState(
    flecs::entity dst,
    const Components::Camera& sceneCamera,
    bool isPrimary,
    ViewManager& viewManager,
    uint32_t renderWidth,
    uint32_t renderHeight,
    uint32_t lodHeight) {
    const CameraResourceSignature newSignature{ renderWidth, renderHeight, isPrimary };
    const auto* oldSignature = dst.try_get<CameraResourceSignature>();
    const bool signatureChanged = oldSignature == nullptr || !(*oldSignature == newSignature);

    if (signatureChanged) {
        DestroyRendererCamera(dst, viewManager);

        auto depthMap = CreateDepthMapComponent(renderWidth, renderHeight, 1, false);
        auto rendererCamera = BuildRendererCamera(sceneCamera, depthMap, renderWidth, renderHeight, lodHeight);
        const auto viewFlags = isPrimary ? ViewFlags::PrimaryCamera() : ViewFlags::Generic();
        const auto viewID = viewManager.CreateView(rendererCamera.info, viewFlags);
        viewManager.AttachDepth(viewID, depthMap.depthMap, depthMap.linearDepthMap);

        dst.set<Components::Camera>(rendererCamera);
        dst.set<Components::RenderViewRef>({ viewID });
        dst.set<Components::DepthMap>(depthMap);
        dst.set<CameraResourceSignature>(newSignature);
    } else {
        const auto existing = dst.get<Components::Camera>();
        const auto depthMap = dst.get<Components::DepthMap>();
        auto rendererCamera = BuildRendererCamera(sceneCamera, depthMap, renderWidth, renderHeight, lodHeight);
        // Preserve view/projection history maintained by RunRenderResourceSyncStage.
        // The scene camera does not maintain these — its view stays at identity.
        rendererCamera.info.view = existing.info.view;
        rendererCamera.info.viewInverse = existing.info.viewInverse;
        rendererCamera.info.prevView = existing.info.prevView;
        rendererCamera.info.jitteredProjection = existing.info.jitteredProjection;
        rendererCamera.info.prevJitteredProjection = existing.info.prevJitteredProjection;
        rendererCamera.info.prevUnjitteredProjection = existing.info.prevUnjitteredProjection;
        rendererCamera.info.viewProjection = existing.info.viewProjection;
        rendererCamera.info.projectionInverse = existing.info.projectionInverse;
        rendererCamera.info.positionWorldSpace = existing.info.positionWorldSpace;
        rendererCamera.jitterPixelSpace = existing.jitterPixelSpace;
        rendererCamera.jitterNDC = existing.jitterNDC;
        dst.set<Components::Camera>(rendererCamera);
    }
}

LightResourceSignature BuildLightSignature(const Components::Light& light, uint16_t shadowResolution, uint8_t directionalCascadeCount, bool hasPrimaryCamera) {
    return LightResourceSignature{
        light.type,
        light.lightInfo.shadowCaster,
        shadowResolution,
        directionalCascadeCount,
        hasPrimaryCamera
    };
}

void ApplyLightRendererBindings(Components::Light& light, flecs::entity dst) {
    light.lightInfo.shadowViewInfoIndex = -1;
    light.lightInfo.shadowMapIndex = -1;
    light.lightInfo.shadowSamplerIndex = -1;

    if (const auto* viewInfo = dst.try_get<Components::LightViewInfo>()) {
        light.lightInfo.shadowViewInfoIndex = viewInfo->viewInfoBufferIndex;
    }
}

void SyncLightDerivedState(
    flecs::entity dst,
    const Components::Light& sceneLight,
    const Components::FrustumPlanes* sceneFrustumPlanes,
    LightManager& lightManager,
    uint16_t shadowResolution,
    uint8_t directionalCascadeCount,
    bool hasPrimaryCamera) {
    const auto newSignature = BuildLightSignature(sceneLight, shadowResolution, directionalCascadeCount, hasPrimaryCamera);
    const auto* oldSignature = dst.try_get<LightResourceSignature>();
    const bool signatureChanged = oldSignature == nullptr || !(*oldSignature == newSignature);

    Components::Light rendererLight = sceneLight;

    if (signatureChanged) {
        DestroyRendererLight(dst, lightManager);

        AddLightReturn addInfo = lightManager.AddLight(&rendererLight.lightInfo, dst.id());
        dst.set<Components::LightViewInfo>(addInfo.lightViewInfo);

        if (sceneFrustumPlanes) {
            dst.set<Components::FrustumPlanes>(*sceneFrustumPlanes);
        } else {
            dst.remove<Components::FrustumPlanes>();
        }
        if (addInfo.frustumPlanes.has_value()) {
            dst.set<Components::FrustumPlanes>(*addInfo.frustumPlanes);
        }

        dst.set<LightResourceSignature>(newSignature);
    } else if (sceneFrustumPlanes) {
        dst.set<Components::FrustumPlanes>(*sceneFrustumPlanes);
    }

	dst.remove<Components::DepthMap>();

    ApplyLightRendererBindings(rendererLight, dst);
    if (const auto* viewInfo = dst.try_get<Components::LightViewInfo>()) {
        lightManager.UpdateLightBufferView(viewInfo->lightBufferView.get(), rendererLight.lightInfo);
    }
    dst.set<Components::Light>(rendererLight);
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
    const ManagerInterface& managerInterface) {
    flecs::entity entity{ renderWorld, renderEntityId };
    if (!entity.is_alive()) {
        return;
    }

    if (auto* objectManager = managerInterface.GetObjectManager()) {
        DestroyRendererObject(entity, *objectManager);
    }
    if (auto* viewManager = managerInterface.GetViewManager()) {
        DestroyRendererCamera(entity, *viewManager);
    }
    if (auto* lightManager = managerInterface.GetLightManager()) {
        DestroyRendererLight(entity, *lightManager);
    }
    entity.destruct();
}

bool MatricesEqual(const DirectX::XMMATRIX& a, const DirectX::XMMATRIX& b) {
    for (int i = 0; i < 4; ++i) {
        if (DirectX::XMVector4NotEqual(a.r[i], b.r[i]))
            return false;
    }
    return true;
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
                renderable.meshInstances = meshInstances;
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

void SceneRenderBridge::Clear(const ManagerInterface& managerInterface) {
    if (!RendererECSManager::GetInstance().IsAlive()) {
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

    auto& renderWorld = RendererECSManager::GetInstance().GetWorld();

    for (const auto& [stableSceneID, state] : m_bridgedEntities) {
        DestroyBridgedEntity(renderWorld, state.renderEntityId, managerInterface);
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

void SceneRenderBridge::IngestSnapshot(const SceneFrameSnapshot& snapshot, const ManagerInterface& managerInterface) {
    ZoneScopedN("SceneRenderBridge::IngestSnapshot");

    auto& renderWorld = RendererECSManager::GetInstance().GetWorld();
    auto* objectManager = managerInterface.GetObjectManager();
    auto* viewManager = managerInterface.GetViewManager();
    auto* lightManager = managerInterface.GetLightManager();

    if (!objectManager || !viewManager || !lightManager) {
        throw std::runtime_error("SceneRenderBridge requires object, view, and light managers");
    }

    DirectX::XMUINT2 renderResolution{};
    DirectX::XMUINT2 outputResolution{};
    uint16_t shadowResolution = 0;
    uint8_t directionalCascadeCount = 0;
    float maxShadowDistance = 0.0f;
    float directionalShadowVerticalExtent = 0.0f;
    CLodLodHeightMode clodLodHeightMode = CLodLodHeightMode::OutputHeight;
    {
        ZoneScopedN("SceneRenderBridge::IngestSnapshot::ReadSettings");
        renderResolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
        outputResolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("outputResolution")();
        shadowResolution = SettingsManager::GetInstance().getSettingGetter<uint16_t>("shadowResolution")();
        directionalCascadeCount = SettingsManager::GetInstance().getSettingGetter<uint8_t>("numDirectionalLightCascades")();
        maxShadowDistance = SettingsManager::GetInstance().getSettingGetter<float>("maxShadowDistance")();
        directionalShadowVerticalExtent = SettingsManager::GetInstance().getSettingGetter<float>("directionalShadowVerticalExtent")();
        clodLodHeightMode = SettingsManager::GetInstance().getSettingGetter<CLodLodHeightMode>(CLodLodHeightModeSettingName)();
    }
    const bool lightResourceSettingsChanged =
        !m_hasLightResourceSettings ||
        m_lastRenderWidth != renderResolution.x ||
        m_lastRenderHeight != renderResolution.y ||
        m_lastShadowResolution != shadowResolution ||
        m_lastDirectionalCascadeCount != directionalCascadeCount ||
        m_lastMaxShadowDistance != maxShadowDistance ||
        m_lastDirectionalShadowVerticalExtent != directionalShadowVerticalExtent ||
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
        }
    }

    // Process only renderables that actually changed (transform, mesh, or new)
    {
        ZoneScopedN("SceneRenderBridge::IngestSnapshot::ChangedRenderables");
        struct PendingObjectDraw {
            flecs::entity entity;
        };
        std::vector<ObjectManager::ObjectBuildInfo> objectBuildInfos;
        std::vector<PendingObjectDraw> pendingObjectDraws;
        objectBuildInfos.reserve(snapshot.changedRenderables.size());
        pendingObjectDraws.reserve(snapshot.changedRenderables.size());

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
                ObjectManager::ObjectBuildInfo objectBuildInfo;
                const auto* instanceTransforms = renderable.hasInstanceTransforms ? &renderable.instanceTransforms : nullptr;
                if (SyncRenderableDerivedStateForBulk(dst, &renderable.meshInstances, instanceTransforms, *objectManager, objectBuildInfo)) {
                    objectBuildInfos.push_back(objectBuildInfo);
                    pendingObjectDraws.push_back({ dst });
                }
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

        if (!objectBuildInfos.empty()) {
            ZoneScopedN("SceneRenderBridge::IngestSnapshot::ChangedRenderables::AddObjectsBulk");
            auto drawInfos = objectManager->AddObjectsBulk(objectBuildInfos);
            const auto count = std::min(drawInfos.size(), pendingObjectDraws.size());
            for (size_t i = 0; i < count; ++i) {
                auto dst = pendingObjectDraws[i].entity;
                if (!dst.is_alive()) {
                    continue;
                }
                auto renderable = dst.get<Components::RenderableObject>();
                renderable.perObjectCB.normalMatrixBufferIndex = drawInfos[i].normalMatrixIndex;
                dst.set<Components::RenderableObject>(renderable);
                dst.set<Components::ObjectDrawInfo>(drawInfos[i]);
            }
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
                clodLodHeightMode == CLodLodHeightMode::OutputHeight &&
                outputResolution.y != 0u;
            const uint32_t lodHeight = useOutputHeight ? outputResolution.y : renderResolution.y;
            SyncCameraDerivedState(dst, camera.camera, camera.primary, *viewManager, renderResolution.x, renderResolution.y, lodHeight);
            if (camera.useExternalMatrices) {
                dst.set<Components::ExternalCameraMatrices>(camera.externalMatrices);
            } else {
                dst.remove<Components::ExternalCameraMatrices>();
            }
            if (camera.primary) {
                dst.add<Components::PrimaryCamera>();
                m_primaryCameraEntityId = dst.id();
                lightManager->SetCurrentCamera(dst);
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
            SyncLightDerivedState(dst, light.light, frustumPlanes, *lightManager, shadowResolution, directionalCascadeCount, m_primaryCameraEntityId != 0);
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
            SyncLightDerivedState(
                entity,
                light,
                frustumPlanes,
                *lightManager,
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
    m_lastDirectionalShadowVerticalExtent = directionalShadowVerticalExtent;
    m_lastHasPrimaryCamera = snapshot.hasPrimaryCamera;
    m_hasLightResourceSettings = true;

    if (!snapshot.removedRenderableIDs.empty() || !snapshot.removedCameraIDs.empty() || !snapshot.removedLightIDs.empty()) {
        ZoneScopedN("SceneRenderBridge::IngestSnapshot::RemovedEntityDiffs");
        auto destroyByStableID = [&](uint64_t stableSceneID) {
            auto it = m_bridgedEntities.find(stableSceneID);
            if (it == m_bridgedEntities.end()) {
                return;
            }
            DestroyBridgedEntity(renderWorld, it->second.renderEntityId, managerInterface);
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
                DestroyBridgedEntity(renderWorld, state.renderEntityId, managerInterface);
                staleStableSceneIDs.push_back(stableSceneID);
            }
        }

        for (const auto stableSceneID : staleStableSceneIDs) {
            m_bridgedEntities.erase(stableSceneID);
        }
    }
}

void SceneRenderBridge::Sync(Scene& scene, const ManagerInterface& managerInterface) {
    IngestSnapshot(ExportSnapshot(scene, 0, 0), managerInterface);
}

bool SceneRenderBridge::HasPrimaryCamera() const {
    if (m_primaryCameraEntityId == 0) {
        return false;
    }

    auto& renderWorld = RendererECSManager::GetInstance().GetWorld();
    flecs::entity entity{ renderWorld, m_primaryCameraEntityId };
    return entity.is_alive();
}

flecs::entity SceneRenderBridge::GetSceneRoot() const {
    if (m_sceneRootEntityId == 0 || !RendererECSManager::GetInstance().IsAlive()) {
        return {};
    }

    auto& renderWorld = RendererECSManager::GetInstance().GetWorld();
    flecs::entity root{ renderWorld, m_sceneRootEntityId };
    return root.is_alive() ? root : flecs::entity{};
}

flecs::entity SceneRenderBridge::GetPrimaryCameraEntity() const {
    auto& renderWorld = RendererECSManager::GetInstance().GetWorld();
    return flecs::entity{ renderWorld, m_primaryCameraEntityId };
}

void SceneRenderBridge::ResyncPrimaryCameraDepth(ViewManager& viewManager, uint32_t renderWidth, uint32_t renderHeight) {
    if (m_primaryCameraEntityId == 0) return;
    auto& renderWorld = RendererECSManager::GetInstance().GetWorld();
    auto entity = flecs::entity{ renderWorld, m_primaryCameraEntityId };
    if (!entity.is_alive() || !entity.has<Components::Camera>()) return;
    const auto camera = entity.get<Components::Camera>();
    uint32_t lodHeight = renderHeight;
    if (entity.has<Components::PrimaryCamera>()) {
        const auto lodHeightMode = SettingsManager::GetInstance().getSettingGetter<CLodLodHeightMode>(CLodLodHeightModeSettingName)();
        const auto outputResolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("outputResolution")();
        if (lodHeightMode == CLodLodHeightMode::OutputHeight && outputResolution.y != 0u) {
            lodHeight = outputResolution.y;
        }
    }
    SyncCameraDerivedState(entity, camera, entity.has<Components::PrimaryCamera>(), viewManager, renderWidth, renderHeight, lodHeight);
}

} // namespace br::render
