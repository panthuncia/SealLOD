#pragma once

#include <cstdint>

#include "Mesh/VertexFlags.h"

namespace MeshVertexLayout {

inline constexpr uint32_t PositionOffset = 0u;
inline constexpr uint32_t PositionSize = 12u;
inline constexpr uint32_t NormalOffset = PositionOffset + PositionSize;
inline constexpr uint32_t NormalSize = 12u;
inline constexpr uint32_t BaseVertexSize = NormalOffset + NormalSize;
inline constexpr uint32_t TangentSize = 16u;
inline constexpr uint32_t TexcoordSize = 8u;
inline constexpr uint32_t ColorSize = 12u;
inline constexpr uint32_t MaxVertexSize = BaseVertexSize + TangentSize + TexcoordSize + ColorSize;

inline constexpr bool HasTexcoords(uint32_t flags) noexcept {
    return (flags & VertexFlags::VERTEX_TEXCOORDS) != 0u;
}

inline constexpr bool HasColors(uint32_t flags) noexcept {
    return (flags & VertexFlags::VERTEX_COLORS) != 0u;
}

inline constexpr bool HasTangents(uint32_t flags) noexcept {
    return (flags & VertexFlags::VERTEX_TANGENTS) != 0u;
}

inline constexpr uint32_t TangentOffset(uint32_t) noexcept {
    return BaseVertexSize;
}

inline constexpr uint32_t TexcoordOffset(uint32_t flags) noexcept {
    return BaseVertexSize + (HasTangents(flags) ? TangentSize : 0u);
}

inline constexpr uint32_t ColorOffset(uint32_t flags) noexcept {
    return TexcoordOffset(flags) + (HasTexcoords(flags) ? TexcoordSize : 0u);
}

inline constexpr uint32_t VertexSize(uint32_t flags) noexcept {
    return BaseVertexSize
        + (HasTangents(flags) ? TangentSize : 0u)
        + (HasTexcoords(flags) ? TexcoordSize : 0u)
        + (HasColors(flags) ? ColorSize : 0u);
}

} // namespace MeshVertexLayout
