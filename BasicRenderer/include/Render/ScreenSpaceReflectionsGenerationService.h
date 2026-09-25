#pragma once

#include <memory>

#include <rhi.h>

#include "Scene/Components.h"

namespace org { class PixelBuffer; }

namespace br::render {

// Retained graph-generation endpoint for the ordered SSSR backend operation.
class ScreenSpaceReflectionsGenerationService final {
public:
    static std::shared_ptr<ScreenSpaceReflectionsGenerationService> Create();

    void Evaluate(rhi::CommandList& commands, const Components::Camera& camera,
        org::PixelBuffer* hdr, org::PixelBuffer* depth, org::PixelBuffer* normals,
        org::PixelBuffer* motion, org::PixelBuffer* environment,
        org::PixelBuffer* brdf, org::PixelBuffer* output) const;
};

} // namespace br::render
