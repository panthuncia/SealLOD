#include "Render/DebugSceneSnapshotService.h"

#include <unordered_set>

#include "Animation/Skeleton.h"
#include "Mesh/Mesh.h"
#include "Mesh/MeshInstance.h"

namespace br::render {

DebugSceneSnapshotService::DebugSceneSnapshotService(flecs::world& world)
    : m_spheres(world.query_builder<Components::ObjectDrawInfo,
          Components::MeshInstances>().cached().cache_kind(flecs::QueryCacheAll).build()),
      m_skeletons(world.query_builder<Components::Matrix,
          Components::ObjectDrawInfo, Components::MeshInstances>()
          .cached().cache_kind(flecs::QueryCacheAll).build()) {}

std::shared_ptr<DebugSceneSnapshotService>
DebugSceneSnapshotService::Create(flecs::world& world) {
    return std::shared_ptr<DebugSceneSnapshotService>(new DebugSceneSnapshotService(world));
}

std::vector<DebugSphereSource> DebugSceneSnapshotService::CaptureSpheres() const {
    std::vector<DebugSphereSource> result;
    m_spheres.each([&](flecs::entity, Components::ObjectDrawInfo drawInfo,
        Components::MeshInstances meshInstances) {
        for (const auto& instance : meshInstances.meshInstances) {
            if (!instance || !instance->GetMesh()) continue;
            result.push_back({
                instance->GetMesh()->GetPerMeshCBData().boundingSphere.sphere,
                drawInfo.perObjectCBIndex});
        }
    });
    return result;
}

std::vector<DebugSkeletonSource> DebugSceneSnapshotService::CaptureSkeletons() const {
    std::vector<DebugSkeletonSource> result;
    std::unordered_set<const Skeleton*> captured;
    m_skeletons.each([&](flecs::entity, Components::Matrix matrix,
        Components::ObjectDrawInfo, Components::MeshInstances meshInstances) {
        for (const auto& instance : meshInstances.meshInstances) {
            if (!instance || !instance->HasSkin()) continue;
            const auto skin = instance->GetSkin();
            if (!skin || !captured.insert(skin.get()).second) continue;
            const auto boneMatrices = skin->GetBoneMatrices();
            const auto parents = skin->GetParentIndices();
            const auto roots = skin->GetRootParentGlobals();
            const auto names = skin->GetBoneNames();
            DebugSkeletonSource source{};
            source.objectMatrix = matrix.matrix;
            source.boneMatrices.assign(boneMatrices.begin(), boneMatrices.end());
            source.parentIndices.assign(parents.begin(), parents.end());
            source.rootParentGlobals.assign(roots.begin(), roots.end());
            source.boneNames.reserve(names.size());
            for (const auto name : names) source.boneNames.emplace_back(name);
            result.push_back(std::move(source));
        }
    });
    return result;
}

} // namespace br::render
