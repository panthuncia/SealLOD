#pragma once

#include <array>
#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct VirtualShadowMapExpandPredictedPagesBindings {
    org::PipelineState stampContentGenerationPipeline, expandPipeline, resetCandidateCountPipeline;
    org::DeclaredViewToken candidates, candidateCount, rawPages, rawCount, clipmapInfo;
    org::DeclaredViewToken scratch, stats, pageTable, pageMetadata, pageViewInfo, replayState;
    std::array<org::DeclaredViewToken, 3> reyesOverflows{};
    bool hasReyesOverflows = false;
    uint32_t physicalPageCount = 0;
};

class VirtualShadowMapExpandPredictedPagesPass final : public org::TypedRenderGraphPass<VirtualShadowMapExpandPredictedPagesPass,
    org::EmptyPassFrameData, VirtualShadowMapExpandPredictedPagesBindings> {
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
        uint32_t physicalPageCount,
        // Which tell whether this frame's shadow work all ran (CLodVirtualShadowFrameShadowWorkComplete). The Reyes
        // split A, split B and dice queue overflow counters are null where the variant does not reset them each frame.
        std::shared_ptr<org::Buffer> replayStateBuffer,
        std::array<std::shared_ptr<org::Buffer>, 3> reyesOverflowBuffers);

    VirtualShadowMapExpandPredictedPagesBindings Declare(org::PassBuilder& builder);
    static void Record(const VirtualShadowMapExpandPredictedPagesBindings&, org::PassRecordContext&);

private:
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
    std::shared_ptr<org::Buffer> m_replayStateBuffer;
    std::array<std::shared_ptr<org::Buffer>, 3> m_reyesOverflowBuffers;
};
