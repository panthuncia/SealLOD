#include "PostProcessing/Reflections/ScreenSpaceReflectionsGenerationService.h"

#include "Runtime/Settings/SettingsManager.h"

namespace br::render {

std::shared_ptr<ScreenSpaceReflectionsGenerationService>
ScreenSpaceReflectionsGenerationService::Create() {
    auto service = std::make_shared<ScreenSpaceReflectionsGenerationService>();
    // The render resolution the build sizes the reflection target for (BuildSSRPasses).
    service->m_context = FFXManager::GetInstance().AcquireSssrContext(
        SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")());
    return service;
}

void ScreenSpaceReflectionsGenerationService::Evaluate(rhi::CommandList& commands,
    const Components::Camera& camera, org::PixelBuffer* hdr,
    org::PixelBuffer* depth, org::PixelBuffer* normals, org::PixelBuffer* motion,
    org::PixelBuffer* environment, org::PixelBuffer* brdf,
    org::PixelBuffer* output) const {
    if (!m_context) return;
    FFXManager::GetInstance().EvaluateSSSR(*m_context, commands, &camera, hdr, depth,
        normals, normals, motion, environment, brdf, output);
}

} // namespace br::render
