#pragma once

#include <memory>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }

struct RasterBucketBlockScanBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken histogram, offsets, blockSums;
};
struct RasterBucketBlockScanFrame {
    uint32_t numBuckets = 0;
    uint32_t groups = 0;
};

class RasterBucketBlockScanPass : public org::TypedRenderGraphPass<RasterBucketBlockScanPass,
    RasterBucketBlockScanFrame, RasterBucketBlockScanBindings> {
public:
    RasterBucketBlockScanPass(
        std::shared_ptr<org::Buffer> histogramBuffer,
        std::shared_ptr<org::Buffer> offsetsBuffer,
        std::shared_ptr<org::Buffer> blockSumsBuffer,
        bool runWhenComputeSWRasterEnabledOnly = false);

    RasterBucketBlockScanBindings Declare(org::PassBuilder& builder);
    RasterBucketBlockScanFrame Prepare(const RasterBucketBlockScanBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const RasterBucketBlockScanBindings& bindings, const RasterBucketBlockScanFrame& frame,
        org::PassRecordContext& context);
    void Update(const org::UpdateExecutionContext& executionContext) override;

private:
    org::PipelineState m_pso;
    uint32_t m_blockSize = 1024;
    std::shared_ptr<org::Buffer> m_histogramBuffer;
    std::shared_ptr<org::Buffer> m_offsetsBuffer;
    std::shared_ptr<org::Buffer> m_blockSumsBuffer;
    bool m_runWhenComputeSWRasterEnabledOnly = false;
    bool m_enabled = false;
};
