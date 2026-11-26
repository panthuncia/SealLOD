#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

#include <DirectXMath.h>
#include <rhi.h>

#include <BasicRenderer/Pipeline/UpscalingTypes.h>

namespace Components { struct Camera; }
class UpscalerConfiguration;

namespace org { class RenderGraph; class Resource; }

namespace br::render {

// Immutable graph-generation selection plus the ordered vendor operation.
// Pass declaration and recording use this retained object and never query the
// process-wide manager or settings registry. It holds the upscaler configuration
// the graph was built for (M5 step 3), so the configuration lives as long as the
// graph and evaluation uses its viewport or context.
class UpscalingGenerationService final {
public:
    // The latest upscaler configuration (UpscalingManager::Prepare), with the dilated motion vector setting.
    static std::shared_ptr<UpscalingGenerationService> CaptureCurrent();

    // Creates the guides only Ray Reconstruction reads, if the current mode is it.
    static void RegisterRayReconstructionGuides(org::RenderGraph& graph);
    // The guides, made now (render resolution) and handed to `sink` by id: the renderer keeps them in its frame interface.
    static void MakeRayReconstructionGuides(const std::function<void(std::string_view, std::shared_ptr<org::Resource>)>& sink);

    UpscalingMode Mode() const noexcept { return m_mode; }
    const std::shared_ptr<const UpscalerConfiguration>& Configuration() const noexcept { return m_configuration; }
    rhi::Backend Backend() const noexcept { return m_backend; }
    bool UsesDilatedMotionVectors() const noexcept { return m_dilatedMotion; }
    DirectX::XMUINT2 RenderResolution() const noexcept { return m_renderResolution; }
    DirectX::XMUINT2 OutputResolution() const noexcept { return m_outputResolution; }

    bool Evaluate(rhi::CommandList& commands, const Components::Camera& camera,
        std::uint64_t frameNumber, double deltaTime, const UpscalingInputs& inputs) const;

private:
    UpscalingGenerationService(std::shared_ptr<const UpscalerConfiguration> configuration, UpscalingMode mode, rhi::Backend backend,
        bool dilatedMotion, DirectX::XMUINT2 renderResolution,
        DirectX::XMUINT2 outputResolution) noexcept;

    std::shared_ptr<const UpscalerConfiguration> m_configuration;
    UpscalingMode m_mode;
    rhi::Backend m_backend;
    bool m_dilatedMotion;
    DirectX::XMUINT2 m_renderResolution;
    DirectX::XMUINT2 m_outputResolution;
};

} // namespace br::render
