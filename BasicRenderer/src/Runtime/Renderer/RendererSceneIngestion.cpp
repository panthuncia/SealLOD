#include <BasicRenderer/Renderer.h>
#include <BasicRenderer/Scene/Scene.h>
#include <BasicRenderer/Scene/SceneFrameSnapshot.h>
#include <BasicRenderer/Scene/SceneIngestionServices.h>
#include <BasicRenderer/Assets/Geometry/MeshInstance.h>
#include <BasicRenderer/Pipeline/MeshDrawWorkload.h>
#include <BasicRenderer/Streaming/TaskScheduler.h>
#include <BasicTelemetry/Tracy.h>
#include "Animation/Skeletons/SkeletonManager.h"
#include "Materials/MaterialManager.h"
#include "VirtualGeometry/GeometryStorage/MeshManager.h"
#include "Scene/Objects/IndirectCommandBufferManager.h"
#include "BasicRenderer/Streaming/TaskScheduler.h"
#include "Runtime/Settings/RendererSettingsHelpers.h"
#include "Scene/ECS/RendererECSManager.h"
#include "Utilities/MathUtils.h"
#include "Utilities/Utilities.h"
#include <algorithm>
#include <mutex>

namespace {
flecs::entity FindSceneEntityByStableSceneID(flecs::entity node, uint64_t stableSceneID) {
    if (!node.is_alive()) {
        return {};
    }

    if (const auto* currentStableSceneID = node.try_get<Components::StableSceneID>()) {
        if (currentStableSceneID->value == stableSceneID) {
            return node;
        }
    }

    flecs::entity found;
    node.children([&](flecs::entity child) {
        if (found.is_alive()) {
            return;
        }

        auto candidate = FindSceneEntityByStableSceneID(child, stableSceneID);
        if (candidate.is_alive()) {
            found = candidate;
        }
    });
    return found;
}
}

void Renderer::RunGameUpdateStage(float elapsedSeconds) {
    BT_ZONE_SCOPE("Renderer::Update::GameUpdate");
    currentScene->Update(elapsedSeconds);
}

void Renderer::RunAnimationUpdateStage(float elapsedSeconds) {
    BT_ZONE_SCOPE("Renderer::Update::AnimationUpdate");
    m_pSkeletonManager->TickAnimations(elapsedSeconds);
    m_pSkeletonManager->UpdateAllDirtyInstances();
}

void Renderer::RunTransformPropagationStage() {
    BT_ZONE_SCOPE("Renderer::Update::TransformPropagation");
    currentScene->PropagateTransforms();
}

void Renderer::RunSceneBridgeSyncStage() {
    BT_ZONE_SCOPE("Renderer::Update::SceneBridgeSync");
    if (!currentScene) {
        return;
    }
    m_sceneRenderBridge.Sync(*currentScene, GetSceneIngestionServices(),
        CaptureSceneIngestionConfiguration());
}

void Renderer::SetExternalSceneMode(bool enabled) {
    if (m_externalSceneMode == enabled) {
        return;
    }

    m_externalSceneMode = enabled;
    if (enabled) {
        SetSceneRenderOverlapEnabled(false);
        InvalidateSceneOverlapState();
        m_warnedNullScene = false;
        m_warnedMissingPrimaryCamera = false;
    }
}

void Renderer::IngestExternalSnapshot(const br::render::SceneFrameSnapshot& snapshot) {
    SetExternalSceneMode(true);
    RegisterExternalSnapshotMeshes(snapshot);
    m_sceneRenderBridge.IngestSnapshot(snapshot, GetSceneIngestionServices(),
        CaptureSceneIngestionConfiguration());
    m_hasCommittedSceneSnapshot = true;
    m_lastCommittedSceneSnapshotSequence = snapshot.snapshotSequence;
    m_lastCommittedSceneSourceFrame = snapshot.sourceFrameNumber;
}

void Renderer::SetDeterministicSamplingMode(bool enabled) {
    m_deterministicSamplingMode = enabled;
    if (m_pMaterialManager) {
        m_pMaterialManager->SetTextureStreamingFeedbackSuppressed(enabled);
    }
    if (enabled) {
        m_jitter = false;
        movementState = {};
    }
}

void Renderer::RegisterExternalSnapshotMeshes(const br::render::SceneFrameSnapshot& snapshot) {
    if (!m_pMeshManager || !m_pMaterialManager || !m_pIndirectCommandBufferManager) {
        return;
    }

    const bool useMeshletReorderedVertices = getMeshShadersEnabled ? getMeshShadersEnabled() : m_useMeshShaders;

    for (const auto& renderable : snapshot.changedRenderables) {
        for (const auto& meshInstance : renderable.meshInstances.meshInstances) {
            if (!meshInstance) {
                continue;
            }

            auto mesh = meshInstance->GetMesh();
            auto material = meshInstance->GetEffectiveMaterial();
            if (!mesh || !material) {
                continue;
            }

            const auto instanceKey = reinterpret_cast<uint64_t>(meshInstance.get());
            const bool externalInstanceKnown = m_externalRegisteredMeshInstances.contains(instanceKey);
            const bool needsExternalInstanceRegistration =
                !externalInstanceKnown || meshInstance->GetPerMeshInstanceBufferView() == nullptr;
            if (needsExternalInstanceRegistration && meshInstance->HasSkin() && m_pSkeletonManager) {
                meshInstance->SetPoseRegistrationService(std::addressof(m_poseInstanceRegistrationService));
                auto skinInst = meshInstance->GetSkin();
                m_poseInstanceRegistrationService.Acquire(skinInst);
                meshInstance->SetSkinningInstanceSlot(skinInst->GetSkinningInstanceSlot());
                meshInstance->SyncSkinningStateFromSkeleton();
            }

            const bool externalMeshKnown = m_externalRegisteredMeshes.contains(mesh->GetGlobalID());
            const bool needsExternalMeshRegistration =
                !externalMeshKnown || mesh->GetPerMeshBufferView() == nullptr;
            if (needsExternalMeshRegistration) {
                if (!externalMeshKnown) {
                    m_pMaterialManager->IncrementMaterialUsageCount(*material);
                }
                const auto materialDataIndex = m_pMaterialManager->GetMaterialSlot(material->GetMaterialID());
                mesh->SetMaterialDataIndex(materialDataIndex);
                const auto materialEvalVariants = ComposeMaterialEvalVariantSet(*mesh, *material);
                const auto materialEvalCompileFlagsID =
                    m_pMaterialManager->AcquireCompileFlagsSlot(materialEvalVariants.regular);
                mesh->SetMaterialEvalCompileFlagsID(materialEvalCompileFlagsID);
                const auto materialReyesEvalCompileFlagsID =
                    materialEvalVariants.hasDistinctReyes
                        ? m_pMaterialManager->AcquireCompileFlagsSlot(materialEvalVariants.reyes)
                        : materialEvalCompileFlagsID;
                mesh->SetMaterialReyesEvalCompileFlagsID(materialReyesEvalCompileFlagsID);

                auto rasterFlags = material->Technique().rasterFlags;
                if ((mesh->GetPerMeshCBData().vertexFlags & VERTEX_SKINNED) != 0u) {
                    rasterFlags |= MaterialRasterFlagsSkinned;
                }
                const auto rasterBucketIndex = m_pMaterialManager->AcquireRasterBucket(rasterFlags);
                mesh->SetRasterBucketIndex(rasterBucketIndex);

                if (!m_pMeshManager->AddMesh(mesh, useMeshletReorderedVertices)) {
                    m_pMaterialManager->ReleaseRasterBucket(rasterFlags);
                    m_pMaterialManager->ReleaseCompileFlagsSlot(materialEvalVariants.regular);
                    if (materialEvalVariants.hasDistinctReyes) {
                        m_pMaterialManager->ReleaseCompileFlagsSlot(materialEvalVariants.reyes);
                    }
                    if (!externalMeshKnown) {
                        m_pMaterialManager->DecrementMaterialUsageCount(*material);
                    }
                    m_externalRegisteredMeshes.erase(mesh->GetGlobalID());
                    continue;
                }
                m_externalRegisteredMeshes.insert(mesh->GetGlobalID());
                m_externalMeshRegistrations[mesh->GetGlobalID()] = ExternalMeshRegistration{
                    .material = material,
                    .regularEvalFlags = materialEvalVariants.regular,
                    .reyesEvalFlags = materialEvalVariants.reyes,
                    .rasterFlags = rasterFlags,
                    .hasDistinctReyes = materialEvalVariants.hasDistinctReyes,
                };

                ForEachMeshDrawWorkload(*mesh, *material, [&](const DrawWorkloadKey& workload) {
                    m_pIndirectCommandBufferManager->RegisterWorkload(workload);
                });
            }

            if (needsExternalInstanceRegistration) {
                if (m_pMeshManager->AddMeshInstance(meshInstance.get(), useMeshletReorderedVertices)) {
                    m_externalRegisteredMeshInstances.insert(instanceKey);
                } else {
                    m_externalRegisteredMeshInstances.erase(instanceKey);
                }
            }
        }
    }
}

void Renderer::ClearExternalSnapshotMeshRegistrations() {
    if (m_pMaterialManager) {
        for (const auto& [_, registration] : m_externalMeshRegistrations) {
            m_pMaterialManager->ReleaseCompileFlagsSlot(registration.regularEvalFlags);
            if (registration.hasDistinctReyes) {
                m_pMaterialManager->ReleaseCompileFlagsSlot(registration.reyesEvalFlags);
            }
            m_pMaterialManager->ReleaseRasterBucket(registration.rasterFlags);
            if (registration.material) {
                m_pMaterialManager->DecrementMaterialUsageCount(*registration.material);
            }
        }
    }
    m_externalMeshRegistrations.clear();
    m_externalRegisteredMeshes.clear();
    m_externalRegisteredMeshInstances.clear();
}

void Renderer::ApplyPrimaryCameraInput(float elapsedSeconds) {
    if (m_deterministicSamplingMode) {
        return;
    }
    if (!currentScene || !currentScene->HasUsablePrimaryCamera()) {
        return;
    }

    Components::Position& cameraPosition = currentScene->GetPrimaryCameraPosition();
    Components::Rotation& cameraRotation = currentScene->GetPrimaryCameraRotation();
    ApplyMovement(cameraPosition, cameraRotation, movementState, elapsedSeconds);
    RotatePitchYaw(cameraRotation, verticalAngle, horizontalAngle);
    currentScene->GetPrimaryCamera().modified<Components::Position>();
    currentScene->GetPrimaryCamera().modified<Components::Rotation>();
    verticalAngle = 0.0f;
    horizontalAngle = 0.0f;
}

void Renderer::InvalidateSceneOverlapState() {
    m_sceneOverlapEpoch.fetch_add(1, std::memory_order_relaxed);
    m_sceneTaskCompleted.store(false);
    {
        std::scoped_lock lock(m_sceneSnapshotMutex);
        m_hasCommittedSceneSnapshot = false;
        m_completedSceneSnapshot.reset();
    }
}

void Renderer::SetSceneRenderOverlapEnabled(bool enabled) {
    if (m_sceneRenderOverlapEnabled == enabled) {
        return;
    }

    if (m_sceneTaskInFlight.load()) {
        spdlog::info("Renderer: scene overlap mode changed while async update was running. Dropping stale async snapshot work.");
    }

    m_sceneRenderOverlapEnabled = enabled;
    InvalidateSceneOverlapState();

    if (!currentScene) {
        return;
    }

    if (enabled) {
        BootstrapCommittedSceneSnapshot();
        return;
    }

    RunSceneBridgeSyncStage();
}

void Renderer::QueueSceneNodePositionEdit(uint64_t stableSceneID, DirectX::XMFLOAT3 position) {
    std::scoped_lock lock(m_pendingSceneExplorerEditsMutex);
    auto& edit = m_pendingSceneExplorerEdits[stableSceneID];
    edit.hasPosition = true;
    edit.position = position;
}

void Renderer::QueueSceneNodeUniformScaleEdit(uint64_t stableSceneID, float uniformScale) {
    std::scoped_lock lock(m_pendingSceneExplorerEditsMutex);
    auto& edit = m_pendingSceneExplorerEdits[stableSceneID];
    edit.hasUniformScale = true;
    edit.uniformScale = uniformScale;
}

void Renderer::FlushPendingSceneExplorerEdits() {
    if (!currentScene || m_sceneTaskInFlight.load()) {
        return;
    }

    std::unordered_map<uint64_t, PendingSceneExplorerEdit> pendingEdits;
    {
        std::scoped_lock lock(m_pendingSceneExplorerEditsMutex);
        if (m_pendingSceneExplorerEdits.empty()) {
            return;
        }
        pendingEdits.swap(m_pendingSceneExplorerEdits);
    }

    bool anyApplied = false;
    auto root = currentScene->GetRoot();
    for (const auto& [stableSceneID, edit] : pendingEdits) {
        auto entity = FindSceneEntityByStableSceneID(root, stableSceneID);
        if (!entity.is_alive()) {
            continue;
        }

        if (edit.hasPosition && entity.has<Components::Position>()) {
            entity.set<Components::Position>(edit.position);
            anyApplied = true;
        }

        if (edit.hasUniformScale && entity.has<Components::Scale>()) {
            entity.set<Components::Scale>({ edit.uniformScale, edit.uniformScale, edit.uniformScale });
            anyApplied = true;
        }
    }

    if (anyApplied) {
        currentScene->PropagateTransforms();

        InvalidateSceneOverlapState();
        if (m_sceneRenderOverlapEnabled) {
            BootstrapCommittedSceneSnapshot();
        } else {
            RunSceneBridgeSyncStage();
        }
    }
}

void Renderer::BootstrapCommittedSceneSnapshot() {
    if (!currentScene) {
        std::scoped_lock lock(m_sceneSnapshotMutex);
        m_hasCommittedSceneSnapshot = false;
        return;
    }

    auto snapshot = std::make_shared<br::render::SceneFrameSnapshot>(
        m_sceneRenderBridge.ExportSnapshot(*currentScene, m_nextSceneSnapshotSequence++, m_totalFramesRendered));
    m_sceneRenderBridge.IngestSnapshot(*snapshot, GetSceneIngestionServices(),
        CaptureSceneIngestionConfiguration());

    std::scoped_lock lock(m_sceneSnapshotMutex);
    m_hasCommittedSceneSnapshot = true;
    m_lastCommittedSceneSnapshotSequence = snapshot->snapshotSequence;
    m_lastCommittedSceneSourceFrame = snapshot->sourceFrameNumber;
}

void Renderer::CommitCompletedSceneSnapshot() {
    BT_ZONE_SCOPE("Renderer::CommitCompletedSceneSnapshot");

    if (!m_sceneTaskCompleted.exchange(false)) {
        return;
    }

    std::shared_ptr<br::render::SceneFrameSnapshot> completedSnapshot;
    {
        BT_ZONE_SCOPE("Renderer::CommitCompletedSceneSnapshot::TakeCompletedSnapshot");
        std::scoped_lock lock(m_sceneSnapshotMutex);
        completedSnapshot = std::exchange(m_completedSceneSnapshot, nullptr);
    }

    if (!completedSnapshot) {
        return;
    }

    if (!currentScene || completedSnapshot->sceneID != currentScene->GetSceneID()) {
        return;
    }

    {
        BT_ZONE_SCOPE("Renderer::CommitCompletedSceneSnapshot::IngestSnapshot");
        m_sceneRenderBridge.IngestSnapshot(*completedSnapshot, GetSceneIngestionServices(),
            CaptureSceneIngestionConfiguration());
    }
    {
        BT_ZONE_SCOPE("Renderer::CommitCompletedSceneSnapshot::PublishCommittedSnapshot");
        std::scoped_lock lock(m_sceneSnapshotMutex);
        m_hasCommittedSceneSnapshot = true;
        m_lastCommittedSceneSnapshotSequence = completedSnapshot->snapshotSequence;
        m_lastCommittedSceneSourceFrame = completedSnapshot->sourceFrameNumber;
    }
}

void Renderer::ScheduleSceneUpdateTask(float elapsedSeconds) {
    ZoneScopedN("Renderer::ScheduleSceneUpdateTask");
    if (!m_sceneRenderOverlapEnabled || !currentScene || m_sceneTaskInFlight.exchange(true)) {
        return;
    }

    auto scene = currentScene;
    const auto movementSnapshot = movementState;
    const float verticalAngleSnapshot = verticalAngle;
    const float horizontalAngleSnapshot = horizontalAngle;
    const uint64_t overlapEpoch = m_sceneOverlapEpoch.load(std::memory_order_relaxed);
    const uint64_t snapshotSequence = m_nextSceneSnapshotSequence++;
    const uint64_t sourceFrameNumber = m_totalFramesRendered + 1;

    verticalAngle = 0.0f;
    horizontalAngle = 0.0f;

    TaskSchedulerManager::GetInstance().Submit(TaskLane::Streaming, TaskDomain::General, "SceneUpdateOverlap", [this, scene, elapsedSeconds, movementSnapshot, verticalAngleSnapshot, horizontalAngleSnapshot, overlapEpoch, snapshotSequence, sourceFrameNumber]() mutable {
        ZoneScopedN("Renderer::SceneUpdateOverlap");
        const auto taskStart = std::chrono::steady_clock::now();

        if (!scene) {
            m_sceneTaskInFlight.store(false);
            return;
        }

        {
            ZoneScopedN("Renderer::SceneUpdateOverlap::ApplyCameraInput");
            if (scene->HasUsablePrimaryCamera()) {
                Components::Position& cameraPosition = scene->GetPrimaryCameraPosition();
                Components::Rotation& cameraRotation = scene->GetPrimaryCameraRotation();
                ApplyMovement(cameraPosition, cameraRotation, movementSnapshot, elapsedSeconds);
                RotatePitchYaw(cameraRotation, verticalAngleSnapshot, horizontalAngleSnapshot);
                scene->GetPrimaryCamera().modified<Components::Position>();
                scene->GetPrimaryCamera().modified<Components::Rotation>();
            }
        }

        {
            ZoneScopedN("Renderer::SceneUpdateOverlap::SceneUpdate");
            scene->Update(elapsedSeconds);
        }
        {
            ZoneScopedN("Renderer::SceneUpdateOverlap::PropagateTransforms");
            scene->PropagateTransforms();
        }

        std::shared_ptr<br::render::SceneFrameSnapshot> snapshot;
        {
            ZoneScopedN("Renderer::SceneUpdateOverlap::ExportSnapshot");
            snapshot = std::make_shared<br::render::SceneFrameSnapshot>(
                m_sceneRenderBridge.ExportSnapshot(*scene, snapshotSequence, sourceFrameNumber));
        }

        if (overlapEpoch != m_sceneOverlapEpoch.load(std::memory_order_relaxed)) {
            m_sceneTaskInFlight.store(false);
            return;
        }

        const auto taskEnd = std::chrono::steady_clock::now();
        const auto durationMs = std::chrono::duration<double, std::milli>(taskEnd - taskStart).count();

        {
            ZoneScopedN("Renderer::SceneUpdateOverlap::PublishSnapshot");
            std::scoped_lock lock(m_sceneSnapshotMutex);
            m_completedSceneSnapshot = std::move(snapshot);
            m_lastCompletedSceneSnapshotSequence = snapshotSequence;
            m_lastSceneTaskDurationMs = durationMs;
        }

        m_sceneTaskCompleted.store(true);
        m_sceneTaskInFlight.store(false);
    });
}

bool Renderer::HasCommittedSceneSnapshot() const {
    std::scoped_lock lock(m_sceneSnapshotMutex);
    return m_hasCommittedSceneSnapshot;
}

bool Renderer::NeedsSceneSnapshotBootstrap() const {
    if (!m_sceneRenderOverlapEnabled || !currentScene || m_sceneTaskInFlight.load()) {
        return false;
    }

    if (!currentScene->HasUsablePrimaryCamera()) {
        return false;
    }

    return !HasCommittedSceneSnapshot() || !m_sceneRenderBridge.HasPrimaryCamera();
}

br::render::SceneOverlapStatus Renderer::GetSceneOverlapStatus() const {
    br::render::SceneOverlapStatus status;
    status.enabled = m_sceneRenderOverlapEnabled;
    status.taskInFlight = m_sceneTaskInFlight.load();

    std::scoped_lock lock(m_sceneSnapshotMutex);
    status.hasCommittedSnapshot = m_hasCommittedSceneSnapshot;
    status.committedSnapshotSequence = m_lastCommittedSceneSnapshotSequence;
    status.lastCompletedSnapshotSequence = m_lastCompletedSceneSnapshotSequence;
    status.lastCommittedSourceFrame = m_lastCommittedSceneSourceFrame;
    status.lastTaskDurationMs = m_lastSceneTaskDurationMs;
    if (m_completedSceneSnapshot) {
        status.pendingSnapshotSequence = m_completedSceneSnapshot->snapshotSequence;
    }

    return status;
}

bool Renderer::IsSceneReadyForFrame(bool logWarnings) {
    if (m_externalSceneMode) {
        if (!m_sceneRenderBridge.HasPrimaryCamera()) {
            if (logWarnings && !m_warnedMissingPrimaryCamera) {
                spdlog::warn("Renderer: external scene snapshot has no primary camera. Skipping frame update work.");
            }
            m_warnedMissingPrimaryCamera = true;
            return false;
        }

        m_warnedNullScene = false;
        m_warnedMissingPrimaryCamera = false;
        return true;
    }

    if (!currentScene) {
        if (logWarnings && !m_warnedNullScene) {
            spdlog::warn("Renderer: current scene is null. Skipping scene update/render work until a valid scene is set.");
        }
        m_warnedNullScene = true;
        m_warnedMissingPrimaryCamera = false;
        return false;
    }

    m_warnedNullScene = false;

    const bool hasPrimaryCamera = m_sceneRenderOverlapEnabled
        ? (NeedsSceneSnapshotBootstrap() || (HasCommittedSceneSnapshot() && m_sceneRenderBridge.HasPrimaryCamera()))
        : currentScene->HasUsablePrimaryCamera();

    if (!hasPrimaryCamera) {
        if (logWarnings && !m_warnedMissingPrimaryCamera) {
            spdlog::warn("Renderer: primary camera is missing or invalid. Skipping scene update/render work until a valid camera is available.");
        }
        m_warnedMissingPrimaryCamera = true;
        return false;
    }

    m_warnedMissingPrimaryCamera = false;
    return true;
}

flecs::entity Renderer::GetValidatedPrimaryRenderCamera(bool attemptResync) {
    if (!RendererECSManager::GetInstance().IsAlive()) {
        return {};
    }

    if (!m_externalSceneMode && !currentScene) {
        return {};
    }

    if (!m_externalSceneMode && m_sceneRenderOverlapEnabled && !HasCommittedSceneSnapshot()) {
        return {};
    }

    if (!m_externalSceneMode && !m_sceneRenderOverlapEnabled && !currentScene->HasUsablePrimaryCamera()) {
        return {};
    }

    if (!m_externalSceneMode && m_sceneRenderOverlapEnabled && NeedsSceneSnapshotBootstrap()) {
        if (attemptResync) {
            BootstrapCommittedSceneSnapshot();
        }

        if (!HasCommittedSceneSnapshot()) {
            return {};
        }
    }

    auto validateCamera = [](flecs::entity camera) {
        return camera
            && camera.is_alive()
            && camera.has<Components::Camera>()
            && camera.has<Components::RenderViewRef>()
            && camera.has<Components::DepthMap>();
    };

    auto primaryCamera = m_sceneRenderBridge.GetPrimaryCameraEntity();
    if (!m_externalSceneMode && !validateCamera(primaryCamera) && attemptResync && !m_sceneRenderOverlapEnabled) {
        m_sceneRenderBridge.Sync(*currentScene, GetSceneIngestionServices(),
            CaptureSceneIngestionConfiguration());
        primaryCamera = m_sceneRenderBridge.GetPrimaryCameraEntity();
    }

    if (!validateCamera(primaryCamera)) {
        return {};
    }

    return primaryCamera;
}


void Renderer::PostUpdate() {
    BT_ZONE_SCOPE("Renderer::PostUpdate");
	if (!currentScene) {
        return;
    }
	currentScene->PostUpdate();
}

