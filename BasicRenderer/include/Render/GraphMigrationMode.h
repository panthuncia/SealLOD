#pragma once

#include <cstdint>

namespace br::render {

enum class GraphMigrationMode : std::uint8_t {
    Disabled,
    Shadow,
    Active,
};

// Foundation cutovers stay observational until their parity gates pass.
inline constexpr GraphMigrationMode kMaterialGraphMigrationMode = GraphMigrationMode::Active;
// First authoritative cutover: object/draw-record buffers and their active lists
// are published as one graph closure before static transactions become active.
inline constexpr GraphMigrationMode kObjectBufferGraphMigrationMode = GraphMigrationMode::Active;
inline constexpr GraphMigrationMode kStaticGraphMigrationMode = GraphMigrationMode::Active;

[[nodiscard]] constexpr bool GraphEnabled(GraphMigrationMode mode) noexcept {
    return mode != GraphMigrationMode::Disabled;
}

[[nodiscard]] constexpr bool GraphActive(GraphMigrationMode mode) noexcept {
    return mode == GraphMigrationMode::Active;
}

} // namespace br::render
