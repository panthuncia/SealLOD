#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct VirtualShadowMapExpandPredictedPagesBindings {
    org::ResourceBindingToken candidates, candidateCount, rawPages, rawCount, clipmapInfo;
    org::ResourceBindingToken scratch, stats, pageTable, pageMetadata, pageViewInfo;
    uint32_t physicalPageCount = 0;
};

class VirtualShadowMapExpandPredictedPagesPass final : public org::TypedRenderGraphPass<VirtualShadowMapExpandPredictedPagesPass,
    br::render::PreparedComputePipelineSequence, VirtualShadowMapExpandPredictedPagesBindings> {
public:
    VirtualShadowMapExpandPredictedPagesPass(
        std::shared_ptr<org::Buffer> predictiveCandidatesBuffer,
        std::shared_ptr<org::Buffer> predictiveCandidateCountBuffer,
        std::shared_ptr<org::Buffer> predictiveRawPagesBuffer,
        std::shared_ptr<org::Buffer> predictiveRawPageCountBuffer,
        std::shared_ptr<org::Buffer> clipmapInfoBuffer,
        std::shared_ptr<org::Buffer> scratchBitsetBuffer,
        std::shared_ptr<org::Buffer> statsBuffer,
        std::shared_ptr<org::PixelBuffer> pageTableTexture,
        std::shared_ptr<org::Buffer> pageMetadataBuffer,
        std::shared_ptr<org::Buffer> pageViewInfoBuffer,
        uint32_t physicalPageCount);

    VirtualShadowMapExpandPredictedPagesBindings Declare(org::PassBuilder& builder);
    br::render::PreparedComputePipelineSequence Prepare(const VirtualShadowMapExpandPredictedPagesBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const VirtualShadowMapExpandPredictedPagesBindings&,
        const br::render::PreparedComputePipelineSequence&, org::PassRecordContext&);

private:
    org::PipelineState m_stampContentGenerationPso;
    org::PipelineState m_pso;
    org::PipelineState m_resetCandidateCountPso;
    std::shared_ptr<org::Buffer> m_predictiveCandidatesBuffer;
    std::shared_ptr<org::Buffer> m_predictiveCandidateCountBuffer;
    std::shared_ptr<org::Buffer> m_predictiveRawPagesBuffer;
    std::shared_ptr<org::Buffer> m_predictiveRawPageCountBuffer;
    std::shared_ptr<org::Buffer> m_clipmapInfoBuffer;
    std::shared_ptr<org::Buffer> m_scratchBitsetBuffer;
    std::shared_ptr<org::Buffer> m_statsBuffer;
    std::shared_ptr<org::PixelBuffer> m_pageTableTexture;
    std::shared_ptr<org::Buffer> m_pageMetadataBuffer;
    std::shared_ptr<org::Buffer> m_pageViewInfoBuffer;
    uint32_t m_physicalPageCount = 0u;
};
