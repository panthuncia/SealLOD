#include "Render/UpscalingGenerationService.h"

#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/SettingsManager.h"

namespace br::render {

UpscalingGenerationService::UpscalingGenerationService(UpscalingMode mode,
    rhi::Backend backend, bool dilatedMotion,
    DirectX::XMUINT2 renderResolution,
    DirectX::XMUINT2 outputResolution) noexcept
    : m_mode(mode), m_backend(backend), m_dilatedMotion(dilatedMotion),
      m_renderResolution(renderResolution), m_outputResolution(outputResolution) {}

std::shared_ptr<UpscalingGenerationService> UpscalingGenerationService::CaptureCurrent() {
    const auto mode = UpscalingManager::GetInstance().GetCurrentUpscalingMode();
    auto& settings = SettingsManager::GetInstance();
    const bool dilated = mode == UpscalingMode::DLSS
        && settings.getSettingGetter<bool>("enableDilatedMotionVectors")();
    return std::shared_ptr<UpscalingGenerationService>(new UpscalingGenerationService(
        mode, DeviceManager::GetInstance().GetBackend(), dilated,
        settings.getSettingGetter<DirectX::XMUINT2>("renderResolution")(),
        settings.getSettingGetter<DirectX::XMUINT2>("outputResolution")()));
}

void UpscalingGenerationService::Evaluate(rhi::CommandList& commands,
    const Components::Camera& camera, std::uint64_t frameNumber,
    double deltaTime, org::PixelBuffer* hdr, org::PixelBuffer* output,
    org::PixelBuffer* depth, org::PixelBuffer* motion) const {
    UpscalingManager::GetInstance().EvaluateCaptured(m_mode, commands, &camera, frameNumber,
        deltaTime, hdr, output, depth, motion);
}

} // namespace br::render
