#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/ComputePass.h"

class Buffer;
class PixelBuffer;

class VirtualShadowMapDeduplicatePredictedPagesPass final : public ComputePass {
public:
    VirtualShadowMapDeduplicatePredictedPagesPass(
        std::shared_ptr<Buffer> predictiveRawPagesBuffer,
        std::shared_ptr<Buffer> predictiveRawPageCountBuffer,
        std::shared_ptr<Buffer> predictedScratchBitsetBuffer,
        std::shared_ptr<Buffer> predictedPagesBuffer,
        std::shared_ptr<Buffer> predictedPageCountBuffer,
        std::shared_ptr<Buffer> statsBuffer,
        std::shared_ptr<PixelBuffer> pageTableTexture,
        std::shared_ptr<Buffer> pageMetadataBuffer,
        std::shared_ptr<Buffer> dirtyFlagsBuffer,
        uint32_t physicalPageCount);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    PipelineState m_clearStatePso;
    PipelineState m_deduplicatePso;
    std::shared_ptr<Buffer> m_predictiveRawPagesBuffer;
    std::shared_ptr<Buffer> m_predictiveRawPageCountBuffer;
    std::shared_ptr<Buffer> m_predictedScratchBitsetBuffer;
    std::shared_ptr<Buffer> m_predictedPagesBuffer;
    std::shared_ptr<Buffer> m_predictedPageCountBuffer;
    std::shared_ptr<Buffer> m_statsBuffer;
    std::shared_ptr<PixelBuffer> m_pageTableTexture;
    std::shared_ptr<Buffer> m_pageMetadataBuffer;
    std::shared_ptr<Buffer> m_dirtyFlagsBuffer;
    uint32_t m_physicalPageCount = 0u;
};
