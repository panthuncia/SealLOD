#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/ComputePass.h"

namespace org { class Buffer; }
using org::Buffer;
namespace org { class PixelBuffer; }
using org::PixelBuffer;

class VirtualShadowMapBuildPageListsPass final : public ComputePass {
public:
    VirtualShadowMapBuildPageListsPass(
        std::shared_ptr<PixelBuffer> pageTableTexture,
        std::shared_ptr<Buffer> pageMetadataBuffer,
        std::shared_ptr<Buffer> allocationCountBuffer,
        std::shared_ptr<Buffer> freePhysicalPagesBuffer,
        std::shared_ptr<Buffer> reusablePhysicalPagesBuffer,
        std::shared_ptr<Buffer> pageListHeaderBuffer);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    PipelineState m_pso;
    std::shared_ptr<PixelBuffer> m_pageTableTexture;
    std::shared_ptr<Buffer> m_pageMetadataBuffer;
    std::shared_ptr<Buffer> m_allocationCountBuffer;
    std::shared_ptr<Buffer> m_freePhysicalPagesBuffer;
    std::shared_ptr<Buffer> m_reusablePhysicalPagesBuffer;
    std::shared_ptr<Buffer> m_pageListHeaderBuffer;
};
