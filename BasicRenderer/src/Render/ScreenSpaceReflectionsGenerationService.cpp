#include "Render/ScreenSpaceReflectionsGenerationService.h"

#include "Managers/Singletons/FFXManager.h"

namespace br::render {

std::shared_ptr<ScreenSpaceReflectionsGenerationService>
ScreenSpaceReflectionsGenerationService::Create() {
    return std::make_shared<ScreenSpaceReflectionsGenerationService>();
}

void ScreenSpaceReflectionsGenerationService::Evaluate(rhi::CommandList& commands,
    const Components::Camera& camera, org::PixelBuffer* hdr,
    org::PixelBuffer* depth, org::PixelBuffer* normals, org::PixelBuffer* motion,
    org::PixelBuffer* environment, org::PixelBuffer* brdf,
    org::PixelBuffer* output) const {
    FFXManager::GetInstance().EvaluateSSSR(commands, &camera, hdr, depth,
        normals, normals, motion, environment, brdf, output);
}

} // namespace br::render
