#pragma once

#include <cstdint>

namespace br::extensions {

enum class PipelineKind : std::uint8_t { Compute, Graphics, Mesh, RayTracing };

} // namespace br::extensions
