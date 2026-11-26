#pragma once

#include <memory>

#include <rhi.h>

#include "BasicRenderer/Scene/Components.h"
#include "PostProcessing/FidelityFX/FFXManager.h"

namespace org { class PixelBuffer; }

namespace br::render {

// Retained graph-generation endpoint for the ordered SSSR backend operation. It holds the SSSR context for the render resolution
// its graph was built for (M5 step 3), made by that build if no live graph has one of that size.
class ScreenSpaceReflectionsGenerationService final {
public:
    static std::shared_ptr<ScreenSpaceReflectionsGenerationService> Create();

    void Evaluate(rhi::CommandList& commands, const Components::Camera& camera,
        org::PixelBuffer* hdr, org::PixelBuffer* depth, org::PixelBuffer* normals,
        org::PixelBuffer* motion, org::PixelBuffer* environment,
        org::PixelBuffer* brdf, org::PixelBuffer* output) const;

private:
    std::shared_ptr<FFXManager::SssrContext> m_context;
};

} // namespace br::render
