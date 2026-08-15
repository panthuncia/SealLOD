#pragma once

#include <unordered_map>
#include <string>
#include <memory>
#include <ctime>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <flecs.h>
#include <atomic>
#include "BasicScene/EcsEntityPool.h"
#include "Animation/Skeleton.h"
#include "Managers/LightManager.h"
#include "Import/MeshData.h"
#include "Managers/ManagerInterface.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Render/RasterBucketFlags.h"

namespace org { class DynamicGloballyIndexedResource; }
using org::DynamicGloballyIndexedResource;
class Material;

class SkeletonVariantSet {
public:
    explicit SkeletonVariantSet(std::uint32_t variantCount = 1);

    std::uint32_t GetVariantCount() const noexcept { return m_variantCount; }
    std::shared_ptr<Skeleton> GetVariant(const std::shared_ptr<Skeleton>& skeleton, std::uint32_t variantIndex);

private:
    struct VariantRecord {
        std::shared_ptr<Skeleton> baseSkeleton;
        std::vector<std::shared_ptr<Skeleton>> variants;
    };

    std::uint32_t m_variantCount = 1;
    std::unordered_map<const Skeleton*, VariantRecord> m_variantsByBase;
};

struct SkeletonVariantAssignmentOptions {
    std::uint32_t seed = 1337;
};

class Scene {
public:
    Scene();
    ~Scene();
    flecs::entity CreateDirectionalLightECS(std::wstring name, DirectX::XMFLOAT3 color, float intensity, DirectX::XMFLOAT3 direction, bool shadowCasting = true);
    flecs::entity CreatePointLightECS(std::wstring name, DirectX::XMFLOAT3 position, DirectX::XMFLOAT3 color, float intensity, float constantAttenuation = 0, float linearAttenuation = 0, float quadraticAttenuation = 0, bool shadowCasting = true);
    flecs::entity CreateSpotLightECS(std::wstring name, DirectX::XMFLOAT3 position, DirectX::XMFLOAT3 color, float intensity, DirectX::XMFLOAT3 direction, float innerConeAngle, float outerConeAngle, float constantAttenuation = 0, float linearAttenuation = 0, float quadraticAttenuation = 0, bool shadowCasting = true);
    flecs::entity CreateNodeECS(std::wstring name = L"");
    flecs::entity CreateRenderableEntityECS(const std::vector<std::shared_ptr<Mesh>>& meshes, std::wstring name);
    flecs::entity CreateRenderableEntityECS(
        const std::vector<std::shared_ptr<Mesh>>& meshes,
        std::wstring name,
        std::optional<std::uint64_t> stableSceneID,
        DirectX::XMMATRIX initialMatrix,
        bool assignNames = true);
    flecs::entity CreateInstancedRenderableEntityECS(
        const std::vector<std::shared_ptr<Mesh>>& meshes,
        std::wstring name,
        std::optional<std::uint64_t> stableSceneID,
        const std::vector<DirectX::XMMATRIX>& instanceMatrices,
        bool assignNames = true);
    void ReserveRenderableEntityPool(std::size_t count);
    void ReleaseRenderableEntityECS(flecs::entity entity);
    void BeginRenderableActivationBatch();
    void EndRenderableActivationBatch();
    struct RenderableEntityPoolStats {
        br::ecs::EcsEntityPoolStats pool;
        std::uint64_t entityAcquireUs = 0;
        std::uint64_t entitySetupUs = 0;
        std::uint64_t entityActivateUs = 0;
        std::uint64_t activationSkinUs = 0;
        std::uint64_t activationMaterialUs = 0;
        std::uint64_t activationGlobalMeshUs = 0;
        std::uint64_t activationMeshInstanceUs = 0;
        std::uint64_t activationWorkloadUs = 0;
        std::uint64_t activationIndirectFlushUs = 0;
        std::uint64_t meshInstanceCreateUs = 0;
        std::uint64_t entityReleaseUs = 0;
    };
    RenderableEntityPoolStats GetRenderableEntityPoolStats() const;
    flecs::entity GetRoot() const;
    uint64_t GetSceneID() const;
    void Update(float elapsedSeconds);
    void SetCamera(XMFLOAT3 pos, XMFLOAT3 lookAt, DirectX::XMFLOAT3 up, float fov, float aspect, float zNear, float zFar);
    flecs::entity& GetPrimaryCamera();
    bool HasUsablePrimaryCamera() const;
    Components::Position& GetPrimaryCameraPosition();
    Components::Rotation& GetPrimaryCameraRotation();
    void PropagateTransforms();
    void PostUpdate();
    std::shared_ptr<Scene> AppendScene(std::shared_ptr<Scene> scene);
    void Activate(ManagerInterface managerInterface);
    void Deactivate();
    bool SetMeshInstanceMaterialOverride(flecs::entity entity, std::size_t meshInstanceIndex, std::shared_ptr<Material> material);
    void AssignSkeletonVariants(SkeletonVariantSet& variantSet, const SkeletonVariantAssignmentOptions& options = {});
    bool AssignSkeletonVariant(flecs::entity entity, SkeletonVariantSet& variantSet, std::uint32_t variantIndex);

    void ProcessEntitySkins(bool overrideExistingSkins = false);
    std::shared_ptr<Scene> Clone() const;
    void DisableShadows();

private:
    static std::atomic<uint64_t> globalSceneCount;
    uint64_t m_sceneID = 0;
    std::vector<std::shared_ptr<Scene>> m_childScenes;
    flecs::entity m_primaryCamera;

    std::unordered_map<uint64_t, flecs::entity> animatedEntitiesByID;
    UINT numObjects = 0;

    flecs::entity ECSSceneRoot;
    br::ecs::EcsEntityPool m_renderableEntityPool;
    bool m_renderableActivationBatchActive = false;
    std::unordered_set<DrawWorkloadKey, DrawWorkloadKey::Hasher> m_batchedActivationWorkloads;
    struct BatchedMaterialUsage {
        Material* material = nullptr;
        unsigned int slot = 0;
        unsigned int deferredCount = 0;
    };
    struct BatchedRasterBucketUsage {
        MaterialRasterFlags flags = MaterialRasterFlagsNone;
        unsigned int slot = 0;
        unsigned int deferredCount = 0;
    };
    struct BatchedCompileFlagsUsage {
        MaterialCompileFlags flags = MaterialCompileNone;
        unsigned int slot = 0;
        unsigned int deferredCount = 0;
    };
    std::unordered_map<uint32_t, BatchedMaterialUsage> m_batchedMaterialUsages;
    std::unordered_map<uint32_t, BatchedRasterBucketUsage> m_batchedRasterBucketUsages;
    std::unordered_map<uint64_t, BatchedCompileFlagsUsage> m_batchedCompileFlagsUsages;
    std::uint64_t m_renderableEntityAcquireUs = 0;
    std::uint64_t m_renderableEntitySetupUs = 0;
    std::uint64_t m_renderableEntityActivateUs = 0;
    std::uint64_t m_renderableActivationSkinUs = 0;
    std::uint64_t m_renderableActivationMaterialUs = 0;
    std::uint64_t m_renderableActivationGlobalMeshUs = 0;
    std::uint64_t m_renderableActivationMeshInstanceUs = 0;
    std::uint64_t m_renderableActivationWorkloadUs = 0;
    std::uint64_t m_renderableActivationIndirectFlushUs = 0;
    std::uint64_t m_renderableMeshInstanceCreateUs = 0;
    std::uint64_t m_renderableEntityReleaseUs = 0;

    ManagerInterface m_managerInterface;

    std::function<void(std::vector<float>)> setDirectionalLightCascadeSplits;
    std::function<uint8_t()> getNumDirectionalLightCascades;
    std::function<float()> getMaxShadowDistance;
    std::function<bool()> getMeshShadersEnabled;

    SettingsManager::Subscription m_renderResSubscription;

    void MakeResident();
    void MakeNonResident();
    flecs::entity CreateLightECS(std::wstring name, Components::LightType type, DirectX::XMFLOAT3 position, DirectX::XMFLOAT3 color, float intensity, DirectX::XMFLOAT3 attenuation = { 0, 0, 0 }, DirectX::XMFLOAT3 direction = { 0, 0, 0 }, float innerConeAngle = 0, float outerConeAngle = 0, bool shadowCasting = false);
    void ActivateRenderable(flecs::entity& entity);
    void ActivateLight(flecs::entity& entity);
    void ActivateCamera(flecs::entity& entity);

    // Cached queries for PropagateTransforms (must be destroyed before the world)
    flecs::query<> m_updatedCleanupQuery;
    flecs::query<> m_dirtyQuery;
    bool m_propagateQueriesBuilt = false;

    void ActivateAllAnimatedEntities();
    bool IsActive() const;
};
