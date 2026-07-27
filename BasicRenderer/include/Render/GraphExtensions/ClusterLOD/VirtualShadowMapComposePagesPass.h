#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/ComputePass.h"

class Buffer;
class PixelBuffer;

class VirtualShadowMapComposePagesPass final : public ComputePass {
public:
    VirtualShadowMapComposePagesPass(
        std::shared_ptr<PixelBuffer> staticPagesTexture,
        std::shared_ptr<PixelBuffer> dynamicPagesTexture,
        std::shared_ptr<PixelBuffer> pageTableTexture,
        std::shared_ptr<Buffer> pageMetadataBuffer,
        std::shared_ptr<Buffer> statsBuffer);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override {}
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override {}

private:
    PipelineState m_pso;
    std::shared_ptr<PixelBuffer> m_staticPagesTexture;
    std::shared_ptr<PixelBuffer> m_dynamicPagesTexture;
    std::shared_ptr<PixelBuffer> m_pageTableTexture;
    std::shared_ptr<Buffer> m_pageMetadataBuffer;
    std::shared_ptr<Buffer> m_statsBuffer;
};
