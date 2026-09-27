#pragma once

#include <memory>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

namespace org { class Buffer; }

struct RasterBucketBlockScanBindings {
    org::ResourceBindingToken histogram, offsets, blockSums;
    uint32_t numBuckets = 0;
    bool enabled = false;
};

class RasterBucketBlockScanPass : public org::TypedRenderGraphPass<RasterBucketBlockScanPass,
    br::render::PreparedComputeDispatch, RasterBucketBlockScanBindings> {
public:
    RasterBucketBlockScanPass(
        std::shared_ptr<org::Buffer> histogramBuffer,
        std::shared_ptr<org::Buffer> offsetsBuffer,
        std::shared_ptr<org::Buffer> blockSumsBuffer,
        bool runWhenComputeSWRasterEnabledOnly = false);

    RasterBucketBlockScanBindings Declare(org::PassBuilder& builder);
    br::render::PreparedComputeDispatch Prepare(const RasterBucketBlockScanBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const RasterBucketBlockScanBindings&, const br::render::PreparedComputeDispatch& data, org::PassRecordContext& context) {
        br::render::RecordPreparedComputeDispatch(data, context);
    }
    void Update(const org::UpdateExecutionContext& executionContext) override;

private:
    org::PipelineState m_pso;
    uint32_t m_blockSize = 1024;
    std::shared_ptr<org::Buffer> m_histogramBuffer;
    std::shared_ptr<org::Buffer> m_offsetsBuffer;
    std::shared_ptr<org::Buffer> m_blockSumsBuffer;
    bool m_runWhenComputeSWRasterEnabledOnly = false;
    uint32_t m_numBuckets = 0;
    bool m_enabled = false;
};
