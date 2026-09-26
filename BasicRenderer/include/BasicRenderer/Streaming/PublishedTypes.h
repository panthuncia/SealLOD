#pragma once

#include <cstddef>
#include <cstdint>

namespace br::render {

enum class PublishedFragmentKind : std::uint8_t {
    Materials, TextureImages, Terrain, Geometry, GeometryResidency, DrawRecords, ActiveDrawLists, IndirectWorkloads,
    Grass, Views, Poses, Lights, Count
};

inline constexpr std::size_t kPublishedFragmentCount =
    static_cast<std::size_t>(PublishedFragmentKind::Count);

enum class PublishedResourceUsage : std::uint8_t {
    ShaderResource, UnorderedAccess, IndirectArguments, CopySource, CopyDestination, ActiveDrawList
};

} // namespace br::render
