#pragma once

#include <memory>
#include <vector>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct VirtualShadowMapDeduplicatePredictedPagesBindings {
    org::ResourceBindingToken rawPages, rawCount, scratch, pages, pageCount;
    org::ResourceBindingToken stats, pageTable, pageMetadata, dirtyFlags;
    uint32_t physicalPageCount = 0;
};

class VirtualShadowMapDeduplicatePredictedPagesPass final : public org::TypedRenderGraphPass<VirtualShadowMapDeduplicatePredictedPagesPass,
    br::render::PreparedComputePipelineSequence, VirtualShadowMapDeduplicatePredictedPagesBindings> {
public:
    VirtualShadowMapDeduplicatePredictedPagesPass(
        std::shared_ptr<org::Buffer> predictiveRawPagesBuffer,
        std::shared_ptr<org::Buffer> predictiveRawPageCountBuffer,
        std::shared_ptr<org::Buffer> predictedScratchBitsetBuffer,
        std::shared_ptr<org::Buffer> predictedPagesBuffer,
        std::shared_ptr<org::Buffer> predictedPageCountBuffer,
        std::shared_ptr<org::Buffer> statsBuffer,
        std::shared_ptr<org::PixelBuffer> pageTableTexture,
        std::shared_ptr<org::Buffer> pageMetadataBuffer,
        std::shared_ptr<org::Buffer> dirtyFlagsBuffer,
        uint32_t physicalPageCount);

    VirtualShadowMapDeduplicatePredictedPagesBindings Declare(org::PassBuilder& builder);
    br::render::PreparedComputePipelineSequence Prepare(const VirtualShadowMapDeduplicatePredictedPagesBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const VirtualShadowMapDeduplicatePredictedPagesBindings&,
        const br::render::PreparedComputePipelineSequence&, org::PassRecordContext&);

private:

    org::PipelineState m_clearStatePso;
    org::PipelineState m_deduplicatePso;
    std::shared_ptr<org::Buffer> m_predictiveRawPagesBuffer;
    std::shared_ptr<org::Buffer> m_predictiveRawPageCountBuffer;
    std::shared_ptr<org::Buffer> m_predictedScratchBitsetBuffer;
    std::shared_ptr<org::Buffer> m_predictedPagesBuffer;
    std::shared_ptr<org::Buffer> m_predictedPageCountBuffer;
    std::shared_ptr<org::Buffer> m_statsBuffer;
    std::shared_ptr<org::PixelBuffer> m_pageTableTexture;
    std::shared_ptr<org::Buffer> m_pageMetadataBuffer;
    std::shared_ptr<org::Buffer> m_dirtyFlagsBuffer;
    uint32_t m_physicalPageCount = 0u;
};
