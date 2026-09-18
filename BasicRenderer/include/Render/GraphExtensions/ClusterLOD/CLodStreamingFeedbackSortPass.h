#pragma once

#include <memory>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"

namespace org { class Buffer; }
using org::Buffer;

struct StreamingFeedbackSortFrameData {
    rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
    rhi::CommandSignatureHandle signature{};
    std::array<org::PreparedProgramBinding, 6> programs;
    std::array<org::PreparedResourceReference, 7> uavResources;
    std::array<org::PreparedResourceReference, 2> indirectResources;
    std::array<std::array<unsigned int, NumMiscUintRootConstants>, 2> constants;
};

struct StreamingFeedbackSortBindings {
    org::ResourceBindingToken requestCounter;
    std::array<org::ResourceBindingToken, 7> uavs;
    org::ResourceBindingToken countScatterUav, reduceScanUav, countScatterIndirect, reduceScanIndirect;
};

class CLodStreamingFeedbackSortPass final : public org::TypedRenderGraphPass<CLodStreamingFeedbackSortPass,
    StreamingFeedbackSortFrameData, StreamingFeedbackSortBindings> {
public:
    CLodStreamingFeedbackSortPass(
        std::shared_ptr<Buffer> requestKeys,
        std::shared_ptr<Buffer> requests,
        std::shared_ptr<Buffer> requestCounter,
        std::shared_ptr<Buffer> keyScratch,
        std::shared_ptr<Buffer> payloadScratch,
        std::shared_ptr<Buffer> sumTable,
        std::shared_ptr<Buffer> reduceTable,
        std::shared_ptr<Buffer> constants,
        std::shared_ptr<Buffer> countScatterArgs,
        std::shared_ptr<Buffer> reduceScanArgs);

    StreamingFeedbackSortBindings Declare(org::PassBuilder& builder);
    void InvocationRevision(const org::PassPrepareContext&, std::vector<uint64_t>&) const;
    StreamingFeedbackSortFrameData Prepare(const StreamingFeedbackSortBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const StreamingFeedbackSortBindings&,
        const StreamingFeedbackSortFrameData& data, org::PassRecordContext& recording);

private:
    PipelineState m_setupPso;
    PipelineState m_countPso;
    PipelineState m_reducePso;
    PipelineState m_scanPso;
    PipelineState m_scanAddPso;
    PipelineState m_scatterPso;

    std::shared_ptr<Buffer> m_requestKeys;
    std::shared_ptr<Buffer> m_requests;
    std::shared_ptr<Buffer> m_requestCounter;
    std::shared_ptr<Buffer> m_keyScratch;
    std::shared_ptr<Buffer> m_payloadScratch;
    std::shared_ptr<Buffer> m_sumTable;
    std::shared_ptr<Buffer> m_reduceTable;
    std::shared_ptr<Buffer> m_constants;
    std::shared_ptr<Buffer> m_countScatterArgs;
    std::shared_ptr<Buffer> m_reduceScanArgs;
};
