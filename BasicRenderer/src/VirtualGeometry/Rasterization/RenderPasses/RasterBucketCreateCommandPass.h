#pragma once

#include <memory>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }

struct RasterBucketCreateCommandBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken visibleCount, indirectCommand, replayState, nodeInputs;
    uint32_t visibleCapacity = 0;
    bool patchReplay = false;
};
struct RasterBucketCreateCommandFrame {
    uint32_t numBuckets = 0;
    uint32_t groups = 0;
};

class RasterBucketCreateCommandPass : public org::TypedRenderGraphPass<RasterBucketCreateCommandPass,
    RasterBucketCreateCommandFrame, RasterBucketCreateCommandBindings> {
public:
    RasterBucketCreateCommandPass(
        std::shared_ptr<org::Buffer> visibleClustersCounterBuffer,
        std::shared_ptr<org::Buffer> histogramIndirectCommand,
        std::shared_ptr<org::Buffer> occlusionReplayStateBuffer,
        std::shared_ptr<org::Buffer> occlusionNodeGpuInputsBuffer,
        uint32_t visibleClustersCapacity,
        bool runWhenComputeSWRasterEnabledOnly = false,
        bool patchReplayNodeInputs = false);

    RasterBucketCreateCommandBindings Declare(org::PassBuilder& builder);
    RasterBucketCreateCommandFrame Prepare(const RasterBucketCreateCommandBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const RasterBucketCreateCommandBindings& bindings, const RasterBucketCreateCommandFrame& frame,
        org::PassRecordContext& context);

private:
    org::PipelineState m_pso;
    std::shared_ptr<org::Buffer> m_visibleClustersCounterBuffer;
    std::shared_ptr<org::Buffer> m_histogramIndirectCommand;
    std::shared_ptr<org::Buffer> m_occlusionReplayStateBuffer;
    std::shared_ptr<org::Buffer> m_occlusionNodeGpuInputsBuffer;
    uint32_t m_visibleClustersCapacity = 0;
    bool m_runWhenComputeSWRasterEnabledOnly = false;
    bool m_patchReplayNodeInputs = false;
};
