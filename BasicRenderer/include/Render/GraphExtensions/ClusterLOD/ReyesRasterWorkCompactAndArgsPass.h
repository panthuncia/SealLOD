#pragma once

#include <memory>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"

namespace org { class Buffer; }

struct ReyesCompactFrameData {
    br::render::PreparedComputeDispatch clear, finalize;
    br::render::PreparedComputeIndirect compact, pack;
    org::PreparedResourceReference cursorBarrier, compactedBarrier, packedBarrier;
};

struct ReyesRasterWorkCompactBindings {
    org::ResourceBindingToken work, counter, indirectCommand, histogram, offsets, cursor, compacted, packed, indirectArgs;
    uint32_t numBuckets = 0;
};

class ReyesRasterWorkCompactAndArgsPass final : public org::TypedRenderGraphPass<ReyesRasterWorkCompactAndArgsPass,
    ReyesCompactFrameData, ReyesRasterWorkCompactBindings> {
public:
    ReyesRasterWorkCompactAndArgsPass(
        std::shared_ptr<org::Buffer> rasterWorkBuffer,
        std::shared_ptr<org::Buffer> rasterWorkCounterBuffer,
        std::shared_ptr<org::Buffer> indirectCommand,
        std::shared_ptr<org::Buffer> histogramBuffer,
        std::shared_ptr<org::Buffer> offsetsBuffer,
        std::shared_ptr<org::Buffer> writeCursorBuffer,
        std::shared_ptr<org::Buffer> compactedRasterWorkIndicesBuffer,
        std::shared_ptr<org::Buffer> packedRasterWorkGroupsBuffer,
        std::shared_ptr<org::Buffer> indirectArgsBuffer);

    ReyesRasterWorkCompactBindings Declare(org::PassBuilder& builder);
    void InvocationRevision(const org::PassPrepareContext&, std::vector<uint64_t>&) const;
    ReyesCompactFrameData Prepare(const ReyesRasterWorkCompactBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const ReyesRasterWorkCompactBindings&, const ReyesCompactFrameData& data, org::PassRecordContext& recording);
    void Update(const org::UpdateExecutionContext& executionContext) override;

private:
    org::PipelineState m_pso;
    org::PipelineState m_packPipeline;
    org::PipelineState m_finalizePackPipeline;
    org::PipelineState m_clearPipeline;
    std::shared_ptr<rhi::CommandSignaturePtr> m_compactionCommandSignature;

    std::shared_ptr<org::Buffer> m_rasterWorkBuffer;
    std::shared_ptr<org::Buffer> m_rasterWorkCounterBuffer;
    std::shared_ptr<org::Buffer> m_indirectCommand;
    std::shared_ptr<org::Buffer> m_histogramBuffer;
    std::shared_ptr<org::Buffer> m_offsetsBuffer;
    std::shared_ptr<org::Buffer> m_writeCursorBuffer;
    std::shared_ptr<org::Buffer> m_compactedRasterWorkIndicesBuffer;
    std::shared_ptr<org::Buffer> m_packedRasterWorkGroupsBuffer;
    std::shared_ptr<org::Buffer> m_indirectArgsBuffer;
    uint32_t m_numBuckets = 0;
};
