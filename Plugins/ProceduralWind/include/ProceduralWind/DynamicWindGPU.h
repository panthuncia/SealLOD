#pragma once

#include <cstdint>

namespace br::wind {

// Consumer-neutral GPU view of the DynamicWind forcing field.  Tree skinning,
// analytic foliage and future particle consumers should branch after this
// resource rather than depending on one another's response representation.
struct DynamicWindFrameGPU {
    std::uint32_t fieldSlice0 = 0u;
    std::uint32_t fieldSlice1 = 0u;
    std::uint32_t fieldDimensions = 0u;
    std::uint32_t fieldValid = 0u;
    float fieldCellSize = 0.0f;
    float fieldOriginX = 0.0f;
    float fieldOriginY = 0.0f;
    float fieldInterpolation = 0.0f;
    float currentTime = 0.0f;
    float previousTime = 0.0f;
    float deltaTime = 0.0f;
    float strength = 0.0f;
    float windX = 1.0f;
    float windY = 0.0f;
    float gustStrength = 0.0f;
    float treeDisplacementScale = 0.0f;
    // This frame's half of the transient wind palette (SkeletonManager flips the halves every frame). The wind passes read
    // it here rather than from root constants, so a kept recording of them stays current (M6 step 9).
    std::uint32_t transformBaseMatrices = 0u;
    std::uint32_t previousTransformBaseMatrices = 0u;
    std::uint32_t pad0 = 0u;
    std::uint32_t pad1 = 0u;
};

static_assert(sizeof(DynamicWindFrameGPU) == 80u);

inline constexpr const char* DynamicWindFieldSlice0ResourceName =
    "Builtin::DynamicWind::FieldSlice0";
inline constexpr const char* DynamicWindFieldSlice1ResourceName =
    "Builtin::DynamicWind::FieldSlice1";
inline constexpr const char* DynamicWindFrameStateResourceName =
    "Builtin::DynamicWind::FrameState";

} // namespace br::wind
