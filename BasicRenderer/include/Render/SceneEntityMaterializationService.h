#pragma once

#include <cstdint>
#include <span>

#include <flecs.h>

#include "Render/SceneSourceStateStore.h"
#include "Scene/Components.h"

class LightManager;
class ObjectManager;
class ViewManager;
struct DrawWorkloadKey;
namespace br::render { class SceneRenderableResidencyService; }

namespace br::render {

// Narrow, serialized compatibility boundary that materializes renderer storage
// from source-state entities. Scene ingestion owns ordering and source values;
// this service is the only bridge-facing owner of the legacy manager backends.
class SceneEntityMaterializationService {
public:
    struct RenderableRequest {
        flecs::entity entity;
        const Components::MeshInstances* meshes = nullptr;
        const Components::InstanceTransforms* instanceTransforms = nullptr;
    };

    void Configure(ObjectManager* objects, ViewManager* views, LightManager* lights,
        SceneRenderableResidencyService* renderables) noexcept;
    [[nodiscard]] bool Available() const noexcept;

    void Destroy(flecs::entity entity) const;
    void MaterializeRenderables(std::span<const RenderableRequest> requests,
        const SceneSourceStateStore::PhaseMap& phases) const;
    void MaterializeCamera(flecs::entity entity, const Components::Camera& camera,
        bool primary, std::uint32_t width, std::uint32_t height,
        std::uint32_t lodHeight) const;
    void SelectCurrentCamera(flecs::entity entity) const;
    void MaterializeLight(flecs::entity entity, const Components::Light& light,
        const Components::FrustumPlanes* frustumPlanes,
        std::uint16_t shadowResolution, std::uint8_t directionalCascadeCount,
        bool hasPrimaryCamera) const;
    [[nodiscard]] unsigned int ResolveWorkloadCount(
        const DrawWorkloadKey& workload, unsigned int sceneCount) const;

private:
    ObjectManager* m_objects = nullptr;
    ViewManager* m_views = nullptr;
    LightManager* m_lights = nullptr;
    SceneRenderableResidencyService* m_renderables = nullptr;
};

} // namespace br::render
