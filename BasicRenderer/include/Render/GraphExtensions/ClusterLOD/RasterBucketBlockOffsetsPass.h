#pragma once

#include <memory>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"

namespace org { class Buffer; }

struct RasterBucketBlockOffsetsBindings {
    org::ResourceBindingToken offsets, blockSums, scannedBlockSums, totalCount;
    uint32_t numBuckets = 0;
    bool enabled = false;
};

class RasterBucketBlockOffsetsPass : public org::TypedRenderGraphPass<RasterBucketBlockOffsetsPass,
    br::render::PreparedComputeDispatch, RasterBucketBlockOffsetsBindings> {
public:
    RasterBucketBlockOffsetsPass(
        std::shared_ptr<org::Buffer> offsetsBuffer,
        std::shared_ptr<org::Buffer> blockSumsBuffer,
        std::shared_ptr<org::Buffer> scannedBlockSumsBuffer,
        std::shared_ptr<org::Buffer> totalCountBuffer,
        bool runWhenComputeSWRasterEnabledOnly = false);

    RasterBucketBlockOffsetsBindings Declare(org::PassBuilder& builder);
    br::render::PreparedComputeDispatch Prepare(const RasterBucketBlockOffsetsBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const RasterBucketBlockOffsetsBindings&, const br::render::PreparedComputeDispatch& data, org::PassRecordContext& context) {
        br::render::RecordPreparedComputeDispatch(data, context);
    }
    void Update(const org::UpdateExecutionContext& executionContext) override;

private:
    org::PipelineState m_pso;
    uint32_t m_blockSize = 1024;
    std::shared_ptr<org::Buffer> m_offsetsBuffer;
    std::shared_ptr<org::Buffer> m_blockSumsBuffer;
    std::shared_ptr<org::Buffer> m_scannedBlockSumsBuffer;
    std::shared_ptr<org::Buffer> m_totalCountBuffer;
    bool m_runWhenComputeSWRasterEnabledOnly = false;
    uint32_t m_numBuckets = 0;
    bool m_enabled = false;
};
