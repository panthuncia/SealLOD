#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/ComputePass.h"

class Buffer;
class PixelBuffer;

class VirtualShadowMapExpandPredictedPagesPass final : public ComputePass {
public:
    VirtualShadowMapExpandPredictedPagesPass(
        std::shared_ptr<Buffer> predictiveCandidatesBuffer,
        std::shared_ptr<Buffer> predictiveCandidateCountBuffer,
        std::shared_ptr<Buffer> predictiveRawPagesBuffer,
        std::shared_ptr<Buffer> predictiveRawPageCountBuffer,
        std::shared_ptr<Buffer> clipmapInfoBuffer,
        std::shared_ptr<Buffer> scratchBitsetBuffer,
        std::shared_ptr<Buffer> statsBuffer,
        std::shared_ptr<PixelBuffer> pageTableTexture,
        std::shared_ptr<Buffer> pageMetadataBuffer,
        uint32_t physicalPageCount);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    PipelineState m_stampContentGenerationPso;
    PipelineState m_pso;
    PipelineState m_resetCandidateCountPso;
    std::shared_ptr<Buffer> m_predictiveCandidatesBuffer;
    std::shared_ptr<Buffer> m_predictiveCandidateCountBuffer;
    std::shared_ptr<Buffer> m_predictiveRawPagesBuffer;
    std::shared_ptr<Buffer> m_predictiveRawPageCountBuffer;
    std::shared_ptr<Buffer> m_clipmapInfoBuffer;
    std::shared_ptr<Buffer> m_scratchBitsetBuffer;
    std::shared_ptr<Buffer> m_statsBuffer;
    std::shared_ptr<PixelBuffer> m_pageTableTexture;
    std::shared_ptr<Buffer> m_pageMetadataBuffer;
    uint32_t m_physicalPageCount = 0u;
};
