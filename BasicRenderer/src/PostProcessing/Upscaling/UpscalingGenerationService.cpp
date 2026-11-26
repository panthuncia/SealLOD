#include "PostProcessing/Upscaling/UpscalingGenerationService.h"

#include "Runtime/Device/DeviceManager.h"
#include "PostProcessing/Upscaling/UpscalingManager.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Render/RenderGraph/RenderGraph.h"
#include "Resources/PixelBuffer.h"
#include "Runtime/Settings/SettingsManager.h"
#include "../../../generated/BuiltinResources.h"

namespace br::render {

UpscalingGenerationService::UpscalingGenerationService(std::shared_ptr<const UpscalerConfiguration> configuration, UpscalingMode mode,
    rhi::Backend backend, bool dilatedMotion,
    DirectX::XMUINT2 renderResolution,
    DirectX::XMUINT2 outputResolution) noexcept
    : m_configuration(std::move(configuration)), m_mode(mode), m_backend(backend), m_dilatedMotion(dilatedMotion),
      m_renderResolution(renderResolution), m_outputResolution(outputResolution) {}

std::shared_ptr<UpscalingGenerationService> UpscalingGenerationService::CaptureCurrent() {
    auto configuration = UpscalingManager::GetInstance().Latest();
    if (!configuration) return nullptr; // Prepare runs at the renderer's initialization, before any build.
    // The build that uses it is starting: its feature is prepared meanwhile.
    UpscalingManager::GetInstance().RequestPreparation(*configuration);
    const auto mode = configuration->Mode();
    auto& settings = SettingsManager::GetInstance();
    const bool dilated = mode == UpscalingMode::DLSS
        && settings.getSettingGetter<bool>("enableDilatedMotionVectors")();
    const auto render = configuration->RenderResolution();
    const auto output = configuration->OutputResolution();
    return std::shared_ptr<UpscalingGenerationService>(new UpscalingGenerationService(
        std::move(configuration), mode, DeviceManager::GetInstance().GetBackend(), dilated, render, output));
}

void UpscalingGenerationService::RegisterRayReconstructionGuides(org::RenderGraph& graph) {
    if (UpscalingManager::GetInstance().GetCurrentUpscalingMode() != UpscalingMode::DLSSRayReconstruction) {
        return;
    }
    MakeRayReconstructionGuides([&](std::string_view id, std::shared_ptr<org::Resource> texture) {
        graph.RegisterResource(id, std::move(texture));
    });
}

void UpscalingGenerationService::MakeRayReconstructionGuides(
    const std::function<void(std::string_view, std::shared_ptr<org::Resource>)>& sink) {
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
        sink(id, std::move(texture));
    };
    registerGuide(Builtin::RayTracing::SpecularAlbedo, "RR Specular Albedo", rhi::Format::R8G8B8A8_UNorm, 4);
    registerGuide(Builtin::RayTracing::SpecularHitDistance, "RR Specular Hit Distance", rhi::Format::R32_Float, 1);
}

bool UpscalingGenerationService::Evaluate(rhi::CommandList& commands,
    const Components::Camera& camera, std::uint64_t frameNumber,
    double deltaTime, const UpscalingInputs& inputs) const {
    return UpscalingManager::GetInstance().Evaluate(*m_configuration, commands, &camera, frameNumber,
        deltaTime, inputs);
}

} // namespace br::render
