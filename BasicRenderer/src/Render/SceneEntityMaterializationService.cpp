#include "Render/SceneEntityMaterializationService.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include "Managers/LightManager.h"
#include "Managers/ObjectManager.h"
#include "Managers/ViewManager.h"
#include "Materials/Material.h"
#include "Mesh/MeshInstance.h"
#include "Render/DrawWorkload.h"
#include "Render/SceneRenderableResidencyService.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Resources/Sampler.h"
#include "Resources/components.h"
#include "Utilities/Utilities.h"

namespace {

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

void SyncPassMembership(flecs::entity dst, const Components::MeshInstances* meshInstances,
    const std::unordered_map<RenderPhase, flecs::entity, RenderPhase::Hasher>& renderPhaseEntities) {
    dst.remove<Components::ParticipatesInPass>(flecs::Wildcard);

    if (!meshInstances) {
        return;
    }

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
    ObjectManager::ObjectBuildInfo& objectBuildInfo,
    const std::unordered_map<RenderPhase, flecs::entity, RenderPhase::Hasher>& renderPhaseEntities) {
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

    if (const auto* stableId = dst.try_get<Components::StableSceneID>()) {
        auto renderable = dst.get<Components::RenderableObject>();
        renderable.perObjectCB.stableSceneIdLo = static_cast<uint32_t>(stableId->value);
        renderable.perObjectCB.stableSceneIdHi = static_cast<uint32_t>(stableId->value >> 32u);
        dst.set<Components::RenderableObject>(renderable);
    }

    if (signatureChanged) {
        DestroyRendererObject(dst, objectManager);
        auto renderable = dst.get<Components::RenderableObject>();
        objectBuildInfo = { renderable.perObjectCB, meshInstances, instanceTransforms };
        dst.set<RenderableSignature>(newSignature);
    }

    SyncPassMembership(dst, meshInstances, renderPhaseEntities);
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


} // namespace

namespace br::render {

void SceneEntityMaterializationService::Configure(
    ObjectManager* objects, ViewManager* views, LightManager* lights,
    SceneRenderableResidencyService* renderables) noexcept {
    m_objects = objects;
    m_views = views;
    m_lights = lights;
    m_renderables = renderables;
}

bool SceneEntityMaterializationService::Available() const noexcept {
    return m_objects && m_views && m_lights && m_renderables && m_renderables->Available();
}

void SceneEntityMaterializationService::Destroy(flecs::entity entity) const {
    if (!entity.is_alive()) return;
    DestroyRendererObject(entity, *m_objects);
    if (const auto* instances = entity.try_get<Components::MeshInstances>()) {
        for (const auto& instance : instances->meshInstances) {
            if (instance && instance->GetPerMeshInstanceBufferView())
                m_renderables->ReleaseInstance(*instance);
        }
    }
    DestroyRendererCamera(entity, *m_views);
    DestroyRendererLight(entity, *m_lights);
}

void SceneEntityMaterializationService::MaterializeRenderables(
    std::span<const RenderableRequest> requests,
    const SceneSourceStateStore::PhaseMap& phases) const {
    std::vector<ObjectManager::ObjectBuildInfo> builds;
    std::vector<flecs::entity> entities;
    builds.reserve(requests.size());
    entities.reserve(requests.size());
    for (const auto& request : requests) {
        const auto incomingSignature = BuildRenderableSignature(request.meshes);
        const auto* priorSignature = request.entity.try_get<RenderableSignature>();
        const bool replacingInstances = priorSignature == nullptr ||
            priorSignature->meshInstanceKeys != incomingSignature.meshInstanceKeys;
        if (replacingInstances) {
            if (const auto* oldInstances = request.entity.try_get<Components::MeshInstances>()) {
                for (const auto& instance : oldInstances->meshInstances) {
                    if (instance && instance->GetPerMeshInstanceBufferView())
                        m_renderables->ReleaseInstance(*instance);
                }
            }
            if (request.meshes) {
                for (const auto& instance : request.meshes->meshInstances) {
                    if (!instance) continue;
                    // Exported mesh instances intentionally own no mutable
                    // manager allocation. Materialize a fresh renderer-owned
                    // instance row before ObjectManager creates draw records.
                    if (!m_renderables->MaterializeInstance(*instance, false)) {
                        throw std::runtime_error(
                            "SceneEntityMaterializationService failed to materialize mesh instance");
                    }
                }
            }
        }
        ObjectManager::ObjectBuildInfo build;
        if (SyncRenderableDerivedStateForBulk(request.entity, request.meshes,
            request.instanceTransforms, *m_objects, build, phases)) {
            builds.push_back(std::move(build));
            entities.push_back(request.entity);
        }
    }
    if (builds.empty()) return;

    auto drawInfos = m_objects->AddObjectsBulk(builds);
    const auto count = std::min(drawInfos.size(), entities.size());
    for (std::size_t i = 0; i < count; ++i) {
        auto entity = entities[i];
        if (!entity.is_alive()) continue;
        auto renderable = entity.get<Components::RenderableObject>();
        renderable.perObjectCB.normalMatrixBufferIndex = drawInfos[i].normalMatrixIndex;
        entity.set<Components::RenderableObject>(renderable);
        entity.set<Components::ObjectDrawInfo>(drawInfos[i]);
    }
}

void SceneEntityMaterializationService::MaterializeCamera(flecs::entity entity,
    const Components::Camera& camera, bool primary, std::uint32_t width,
    std::uint32_t height, std::uint32_t lodHeight) const {
    SyncCameraDerivedState(entity, camera, primary, *m_views, width, height, lodHeight);
}

void SceneEntityMaterializationService::SelectCurrentCamera(flecs::entity entity) const {
    m_lights->SetCurrentCamera(entity);
}

void SceneEntityMaterializationService::MaterializeLight(flecs::entity entity,
    const Components::Light& light, const Components::FrustumPlanes* frustumPlanes,
    std::uint16_t shadowResolution, std::uint8_t directionalCascadeCount,
    bool hasPrimaryCamera) const {
    SyncLightDerivedState(entity, light, frustumPlanes, *m_lights,
        shadowResolution, directionalCascadeCount, hasPrimaryCamera);
}

unsigned int SceneEntityMaterializationService::ResolveWorkloadCount(
    const DrawWorkloadKey& workload, unsigned int sceneCount) const {
    if (m_objects == nullptr) return sceneCount;
    const auto activeDrawSet = m_objects->TryGetActiveDrawSetIndices(workload);
    if (!activeDrawSet) return sceneCount;
    const auto activeCount = static_cast<unsigned int>((std::min<std::uint64_t>)(
        activeDrawSet->Size(), std::numeric_limits<unsigned int>::max()));
    return (std::max)(sceneCount, activeCount);
}


} // namespace br::render
