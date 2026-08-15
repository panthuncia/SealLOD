#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/ComputePass.h"

namespace org { class Buffer; }
using org::Buffer;
namespace org { class PixelBuffer; }
using org::PixelBuffer;

class VirtualShadowMapFreeWrappedPagesPass final : public ComputePass {
public:
    VirtualShadowMapFreeWrappedPagesPass(
        std::shared_ptr<PixelBuffer> pageTableTexture,
        std::shared_ptr<Buffer> pageMetadataBuffer,
        std::shared_ptr<Buffer> clipmapInfoBuffer,
        std::shared_ptr<Buffer> statsBuffer);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    PipelineState m_pso;
    std::shared_ptr<PixelBuffer> m_pageTableTexture;
    std::shared_ptr<Buffer> m_pageMetadataBuffer;
    std::shared_ptr<Buffer> m_clipmapInfoBuffer;
    std::shared_ptr<Buffer> m_statsBuffer;
};