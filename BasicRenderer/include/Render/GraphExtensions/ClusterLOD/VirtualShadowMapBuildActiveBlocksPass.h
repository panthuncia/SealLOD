#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/ComputePass.h"

class Buffer;
class PixelBuffer;

class VirtualShadowMapBuildActiveBlocksPass final : public ComputePass {
public:
    VirtualShadowMapBuildActiveBlocksPass(
        std::shared_ptr<PixelBuffer> pageTableTexture,
        std::shared_ptr<Buffer> clipmapInfoBuffer,
        std::shared_ptr<Buffer> activeBlockMetadataBuffer,
        bool dynamicPages = false);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override {}
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override {}

private:
    PipelineState m_pso;
    std::shared_ptr<PixelBuffer> m_pageTableTexture;
    std::shared_ptr<Buffer> m_clipmapInfoBuffer;
    std::shared_ptr<Buffer> m_activeBlockMetadataBuffer;
    bool m_dynamicPages = false;
};
