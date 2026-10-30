#pragma once

#include <memory>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }

struct RasterBucketBlockOffsetsBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken offsets, blockSums, scannedBlockSums, totalCount;
};
struct RasterBucketBlockOffsetsFrame {
    uint32_t numBuckets = 0;
    uint32_t numBlocks = 0;
    uint32_t groups = 0;
};

class RasterBucketBlockOffsetsPass : public org::TypedRenderGraphPass<RasterBucketBlockOffsetsPass,
    RasterBucketBlockOffsetsFrame, RasterBucketBlockOffsetsBindings> {
public:
    RasterBucketBlockOffsetsPass(
        std::shared_ptr<org::Buffer> offsetsBuffer,
        std::shared_ptr<org::Buffer> blockSumsBuffer,
        std::shared_ptr<org::Buffer> scannedBlockSumsBuffer,
        std::shared_ptr<org::Buffer> totalCountBuffer,
        bool runWhenComputeSWRasterEnabledOnly = false);

    RasterBucketBlockOffsetsBindings Declare(org::PassBuilder& builder);
    RasterBucketBlockOffsetsFrame Prepare(const RasterBucketBlockOffsetsBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const RasterBucketBlockOffsetsBindings& bindings, const RasterBucketBlockOffsetsFrame& frame,
        org::PassRecordContext& context);
    void Update(const org::UpdateExecutionContext& executionContext) override;

private:
    org::PipelineState m_pso;
    uint32_t m_blockSize = 1024;
    std::shared_ptr<org::Buffer> m_offsetsBuffer;
    std::shared_ptr<org::Buffer> m_blockSumsBuffer;
    std::shared_ptr<org::Buffer> m_scannedBlockSumsBuffer;
    std::shared_ptr<org::Buffer> m_totalCountBuffer;
    bool m_runWhenComputeSWRasterEnabledOnly = false;
    bool m_enabled = false;
};
