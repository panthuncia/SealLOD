#pragma once

#include <memory>

#include "RenderPasses/Base/ComputePass.h"

class Buffer;

class RasterBucketCreateCommandPass : public ComputePass {
public:
    RasterBucketCreateCommandPass(
        std::shared_ptr<Buffer> visibleClustersCounterBuffer,
        std::shared_ptr<Buffer> histogramIndirectCommand,
        std::shared_ptr<Buffer> occlusionReplayStateBuffer,
        std::shared_ptr<Buffer> occlusionNodeGpuInputsBuffer,
        uint32_t visibleClustersCapacity,
        bool runWhenComputeSWRasterEnabledOnly = false,
        bool patchReplayNodeInputs = false);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Update(const UpdateExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    PipelineState m_pso;
    std::shared_ptr<Buffer> m_visibleClustersCounterBuffer;
    std::shared_ptr<Buffer> m_histogramIndirectCommand;
    std::shared_ptr<Buffer> m_occlusionReplayStateBuffer;
    std::shared_ptr<Buffer> m_occlusionNodeGpuInputsBuffer;
    uint32_t m_visibleClustersCapacity = 0;
    bool m_runWhenComputeSWRasterEnabledOnly = false;
    bool m_patchReplayNodeInputs = false;
};
