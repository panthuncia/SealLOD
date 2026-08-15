#pragma once

#include <memory>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/ComputePass.h"

namespace org { class Buffer; }
using org::Buffer;

class CLodStreamingFeedbackSortPass final : public ComputePass {
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

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override {}
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override {}

private:
    void PushRootConstants(
        rhi::CommandList& commandList,
        const std::shared_ptr<Buffer>& sourceKeys,
        const std::shared_ptr<Buffer>& destKeys,
        const std::shared_ptr<Buffer>& sourcePayloads,
        const std::shared_ptr<Buffer>& destPayloads,
        uint32_t iterationIndex) const;
    void UavBarrier(rhi::CommandList& commandList) const;
    void TransitionIndirectArgsForExecute(rhi::CommandList& commandList) const;

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
