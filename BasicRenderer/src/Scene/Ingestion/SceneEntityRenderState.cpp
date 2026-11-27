#include <BasicRenderer/Extensions/SceneEntityRenderState.h>

#include <unordered_map>

#include <flecs.h>

#include "BasicRenderer/Assets/Geometry/MeshInstance.h"
#include "BasicRenderer/Scene/Components.h"
#include "BasicRenderer/Scene/RendererComponents.h"
#include "Scene/ECS/RendererECSManager.h"

namespace br::extensions {

namespace {

bool IsRenderableWithID(flecs::entity entity, std::uint64_t stableSceneID)
{
    if (!entity.is_alive() || !entity.has<Components::ObjectDrawInfo>()) {
        return false;
    }
    const auto* id = entity.try_get<Components::StableSceneID>();
    return id && id->value == stableSceneID;
}

} // namespace

std::optional<SceneEntityRenderState> FindSceneEntityRenderState(std::uint64_t stableSceneID)
{
    auto& manager = RendererECSManager::GetInstance();
    if (!manager.IsAlive()) {
        return std::nullopt;
    }
    auto& world = manager.GetWorld();

    // Render thread only (see the header), so the cache needs no lock. Render entities are pooled: an entry is checked
    // against the entity's current id before it is used.
    static std::unordered_map<std::uint64_t, flecs::entity_t> cache;
    flecs::entity entity;
    if (const auto cached = cache.find(stableSceneID); cached != cache.end()) {
        entity = flecs::entity(world, cached->second);
        if (!IsRenderableWithID(entity, stableSceneID)) {
            cache.erase(cached);
            entity = flecs::entity();
        }
    }
    if (!entity.is_valid()) {
        world.each([&](flecs::entity candidate, const Components::StableSceneID& id) {
            if (!entity.is_valid() && id.value == stableSceneID && IsRenderableWithID(candidate, stableSceneID)) {
                entity = candidate;
            }
        });
        if (!entity.is_valid()) {
            return std::nullopt;
        }
        cache[stableSceneID] = entity.id();
    }

    SceneEntityRenderState state;
    const auto& drawInfo = entity.get<Components::ObjectDrawInfo>();
    state.perObjectBufferIndex = drawInfo.perObjectCBIndex;
    if (const auto* meshInstances = entity.try_get<Components::MeshInstances>()) {
        for (const auto& instance : meshInstances->meshInstances) {
            // The renderer's copy of a mesh instance keeps the slot in its per-instance row, not in its skeleton copy.
            if (instance && instance->HasSkin()) {
                state.skinningInstanceSlot = instance->GetPerMeshInstanceBufferData().skinningInstanceSlot;
                break;
            }
        }
    }
    return state;
}

} // namespace br::extensions
