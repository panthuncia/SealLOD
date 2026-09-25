#pragma once

#include <cstdint>
#include <memory>

#include <DirectXMath.h>
#include <rhi.h>

#include "Managers/Singletons/UpscalingManager.h"

namespace org { class PixelBuffer; }

namespace br::render {

// Immutable graph-generation selection plus the ordered vendor operation.
// Pass declaration and recording use this retained object and never query the
// process-wide manager or settings registry.
class UpscalingGenerationService final {
public:
    static std::shared_ptr<UpscalingGenerationService> CaptureCurrent();

    UpscalingMode Mode() const noexcept { return m_mode; }
    rhi::Backend Backend() const noexcept { return m_backend; }
    bool UsesDilatedMotionVectors() const noexcept { return m_dilatedMotion; }
    DirectX::XMUINT2 RenderResolution() const noexcept { return m_renderResolution; }
    DirectX::XMUINT2 OutputResolution() const noexcept { return m_outputResolution; }

    void Evaluate(rhi::CommandList& commands, const Components::Camera& camera,
        std::uint64_t frameNumber, double deltaTime, org::PixelBuffer* hdr,
        org::PixelBuffer* output, org::PixelBuffer* depth,
        org::PixelBuffer* motion) const;

private:
    UpscalingGenerationService(UpscalingMode mode, rhi::Backend backend,
        bool dilatedMotion, DirectX::XMUINT2 renderResolution,
        DirectX::XMUINT2 outputResolution) noexcept;

    UpscalingMode m_mode;
    rhi::Backend m_backend;
    bool m_dilatedMotion;
    DirectX::XMUINT2 m_renderResolution;
    DirectX::XMUINT2 m_outputResolution;
};

} // namespace br::render
