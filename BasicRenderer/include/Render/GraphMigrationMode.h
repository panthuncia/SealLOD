#pragma once

#include <cstdint>

namespace br::render {

enum class GraphMigrationMode : std::uint8_t {
    Disabled,
    Shadow,
    Active,
};

// Foundation cutovers stay observational until their parity gates pass.
inline constexpr GraphMigrationMode kMaterialGraphMigrationMode = GraphMigrationMode::Shadow;
inline constexpr GraphMigrationMode kObjectBufferGraphMigrationMode = GraphMigrationMode::Shadow;
inline constexpr GraphMigrationMode kStaticGraphMigrationMode = GraphMigrationMode::Shadow;

[[nodiscard]] constexpr bool GraphEnabled(GraphMigrationMode mode) noexcept {
    return mode != GraphMigrationMode::Disabled;
}

[[nodiscard]] constexpr bool GraphActive(GraphMigrationMode mode) noexcept {
    return mode == GraphMigrationMode::Active;
}

} // namespace br::render
