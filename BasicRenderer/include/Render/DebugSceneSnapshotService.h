#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <DirectXMath.h>
#include <flecs.h>

#include "Scene/Components.h"

namespace br::render {

struct DebugSphereSource {
    DirectX::XMFLOAT4 bounds{};
    std::uint32_t perObjectIndex = 0;
};

struct DebugSkeletonSource {
    DirectX::XMMATRIX objectMatrix{};
    std::vector<DirectX::XMMATRIX> boneMatrices;
    std::vector<std::int32_t> parentIndices;
    std::vector<DirectX::XMMATRIX> rootParentGlobals;
    std::vector<std::string> boneNames;
};

// Generation-owned source adapter. It is invoked during the serialized update
// stage and returns owned values; preparation and recording never touch ECS or
// mutable mesh/skeleton instances.
class DebugSceneSnapshotService final {
public:
    static std::shared_ptr<DebugSceneSnapshotService> Create(flecs::world& world);

    std::vector<DebugSphereSource> CaptureSpheres() const;
    std::vector<DebugSkeletonSource> CaptureSkeletons() const;

private:
    explicit DebugSceneSnapshotService(flecs::world& world);

    flecs::query<Components::ObjectDrawInfo, Components::MeshInstances> m_spheres;
    flecs::query<Components::Matrix, Components::ObjectDrawInfo,
        Components::MeshInstances> m_skeletons;
};

} // namespace br::render
