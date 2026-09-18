#pragma once

#include <memory>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"

namespace org { class Buffer; }
using org::Buffer;

struct ReyesHistogramFrameData {
    br::render::PreparedComputeDispatch clear;
    br::render::PreparedComputeIndirect histogram;
    org::PreparedResourceReference histogramBarrier;
};

struct ReyesRasterWorkHistogramBindings {
    org::ResourceBindingToken work, counter, indirectArgs, histogram;
    uint32_t numBuckets = 0;
};

class ReyesRasterWorkHistogramPass final : public org::TypedRenderGraphPass<ReyesRasterWorkHistogramPass,
    ReyesHistogramFrameData, ReyesRasterWorkHistogramBindings> {
public:
    ReyesRasterWorkHistogramPass(
        std::shared_ptr<Buffer> rasterWorkBuffer,
        std::shared_ptr<Buffer> rasterWorkCounterBuffer,
        std::shared_ptr<Buffer> histogramIndirectCommand,
        std::shared_ptr<Buffer> histogramBuffer);

    ReyesRasterWorkHistogramBindings Declare(org::PassBuilder& builder);
    void InvocationRevision(const org::PassPrepareContext&, std::vector<uint64_t>&) const;
    ReyesHistogramFrameData Prepare(const ReyesRasterWorkHistogramBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const ReyesRasterWorkHistogramBindings&, const ReyesHistogramFrameData& data, org::PassRecordContext& recording);
    void Update(const UpdateExecutionContext& executionContext) override;

private:
    void CreatePipelines(
        rhi::Device device,
        rhi::PipelineLayoutHandle globalRootSignature,
        PipelineState& outHistogramPipeline,
        PipelineState& outClearPipeline);

    PipelineState m_histogramPipeline;
    PipelineState m_clearPipeline;
    std::shared_ptr<rhi::CommandSignaturePtr> m_histogramCommandSignature;
    std::shared_ptr<Buffer> m_rasterWorkBuffer;
    std::shared_ptr<Buffer> m_rasterWorkCounterBuffer;
    std::shared_ptr<Buffer> m_histogramIndirectCommand;
    std::shared_ptr<Buffer> m_histogramBuffer;
    uint32_t m_numBuckets = 0;
};
