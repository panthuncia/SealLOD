#include "PostProcessing/Upscaling/UpscalingGenerationService.h"

#include "Runtime/Device/DeviceManager.h"
#include "PostProcessing/Upscaling/UpscalingManager.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Render/RenderGraph/RenderGraph.h"
#include "Resources/PixelBuffer.h"
#include "Runtime/Settings/SettingsManager.h"
#include "../../../generated/BuiltinResources.h"

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

void UpscalingGenerationService::RegisterRayReconstructionGuides(org::RenderGraph& graph) {
    if (UpscalingManager::GetInstance().GetCurrentUpscalingMode() != UpscalingMode::DLSSRayReconstruction) {
        return;
    }
    const auto resolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
    const auto registerGuide = [&](std::string_view id, const char* name, rhi::Format format, uint16_t channels) {
        org::TextureDescription desc;
        desc.channels = channels;
        desc.format = format;
        desc.hasSRV = desc.hasUAV = desc.hasNonShaderVisibleUAV = true;
        desc.allowAlias = true;
        desc.imageDimensions.emplace_back(resolution.x, resolution.y, 0, 0);
        auto texture = org::PixelBuffer::CreateSharedUnmaterialized(desc);
        texture->SetName(name);
        org::memory::SetResourceUsageHint(*texture, "DLSS Ray Reconstruction guides");
        graph.RegisterResource(id, std::move(texture));
    };
    registerGuide(Builtin::RayTracing::SpecularAlbedo, "RR Specular Albedo", rhi::Format::R8G8B8A8_UNorm, 4);
    registerGuide(Builtin::RayTracing::SpecularHitDistance, "RR Specular Hit Distance", rhi::Format::R32_Float, 1);
}

void UpscalingGenerationService::Evaluate(rhi::CommandList& commands,
    const Components::Camera& camera, std::uint64_t frameNumber,
    double deltaTime, const UpscalingInputs& inputs) const {
    UpscalingManager::GetInstance().EvaluateCaptured(m_mode, commands, &camera, frameNumber,
        deltaTime, inputs);
}

} // namespace br::render
