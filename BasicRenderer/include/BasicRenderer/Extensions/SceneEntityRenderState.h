#pragma once

#include <cstdint>
#include <optional>

namespace br::extensions {

// Where the renderer keeps what it draws a scene entity with: for a plugin that attaches its own data to an entity
// (strand hair) and has its shaders follow the entity's transform and pose.
struct SceneEntityRenderState {
    // The entity's row of Builtin::PerObjectBuffer (PerObjectCB: model and previous model matrix). For an instanced
    // entity, its first instance's row. The buffer is versioned: a frame binds the version of its published state
    // (PublishedObjectBufferState, kObjectPerObjectVariant), which can be a few frames behind the scene and may not
    // hold the row yet (compare the version's elementCount). That version's image holds the row's bytes on the CPU.
    std::uint32_t perObjectBufferIndex = 0;
    // The row of Builtin::SkeletonResources::SkinningInstanceInfo the entity's first skinned mesh instance is drawn
    // with: where its bone palettes (current and previous) are in Builtin::SkeletonResources::BoneTransforms.
    // 0xFFFFFFFF for an entity without a skin. The pose itself is the scene entity's: its mesh instance's skeleton
    // (MeshInstance::GetSkin), whose palette the renderer uploads in every frame the pose changed.
    std::uint32_t skinningInstanceSlot = 0xFFFFFFFFu;
};

// The render state of the scene entity with this Components::StableSceneID, or nothing while the renderer has no
// drawable for it (not ingested yet, or destroyed).
//
// Render thread only, after the frame's scene synchronization: call it from a pass's Update. The first call for an id
// (and every call for an id the renderer does not have) scans the render world; later calls are a lookup.
std::optional<SceneEntityRenderState> FindSceneEntityRenderState(std::uint64_t stableSceneID);

} // namespace br::extensions
