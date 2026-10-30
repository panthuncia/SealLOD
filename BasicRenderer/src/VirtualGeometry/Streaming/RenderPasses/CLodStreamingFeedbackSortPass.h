#pragma once

#include <array>
#include <memory>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }

struct StreamingFeedbackSortFrameData {
    std::shared_ptr<const rhi::CommandSignaturePtr> signature;
};

struct StreamingFeedbackSortBindings {
    // Setup, count, reduce, scan, scan-add, scatter.
    std::array<org::PipelineState, 6> pipelines;
    org::DeclaredViewToken requestCounter;
    std::array<org::DeclaredViewToken, 7> uavs;
    org::DeclaredViewToken countScatterUav, reduceScanUav;
    org::ResourceBindingToken countScatterIndirect, reduceScanIndirect;
};

class CLodStreamingFeedbackSortPass final : public org::TypedRenderGraphPass<CLodStreamingFeedbackSortPass,
    StreamingFeedbackSortFrameData, StreamingFeedbackSortBindings> {
public:
    CLodStreamingFeedbackSortPass(
        std::shared_ptr<org::Buffer> requestKeys,
        std::shared_ptr<org::Buffer> requests,
        std::shared_ptr<org::Buffer> requestCounter,
        std::shared_ptr<org::Buffer> keyScratch,
        std::shared_ptr<org::Buffer> payloadScratch,
        std::shared_ptr<org::Buffer> sumTable,
        std::shared_ptr<org::Buffer> reduceTable,
        std::shared_ptr<org::Buffer> constants,
        std::shared_ptr<org::Buffer> countScatterArgs,
        std::shared_ptr<org::Buffer> reduceScanArgs);

    StreamingFeedbackSortBindings Declare(org::PassBuilder& builder);
    StreamingFeedbackSortFrameData Prepare(const StreamingFeedbackSortBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const StreamingFeedbackSortBindings& bindings,
        const StreamingFeedbackSortFrameData& frame, org::PassRecordContext& context);

private:
    org::PipelineState m_setupPso;
    org::PipelineState m_countPso;
    org::PipelineState m_reducePso;
    org::PipelineState m_scanPso;
    org::PipelineState m_scanAddPso;
    org::PipelineState m_scatterPso;

    std::shared_ptr<org::Buffer> m_requestKeys;
    std::shared_ptr<org::Buffer> m_requests;
    std::shared_ptr<org::Buffer> m_requestCounter;
    std::shared_ptr<org::Buffer> m_keyScratch;
    std::shared_ptr<org::Buffer> m_payloadScratch;
    std::shared_ptr<org::Buffer> m_sumTable;
    std::shared_ptr<org::Buffer> m_reduceTable;
    std::shared_ptr<org::Buffer> m_constants;
    std::shared_ptr<org::Buffer> m_countScatterArgs;
    std::shared_ptr<org::Buffer> m_reduceScanArgs;
};
