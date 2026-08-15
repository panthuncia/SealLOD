#pragma once

#include <memory>

#include <rhi.h>

#include "RenderPasses/Base/ComputePass.h"

namespace org { class Buffer; }
using org::Buffer;

class RasterBucketHistogramPass : public ComputePass {
public:
    RasterBucketHistogramPass(
        std::shared_ptr<Buffer> visibleClustersBuffer,
        std::shared_ptr<Buffer> visibleClustersCounterBuffer,
        std::shared_ptr<Buffer> histogramIndirectCommand,
        std::shared_ptr<Buffer> histogramBuffer,
        std::shared_ptr<Buffer> reyesOwnershipBitsetBuffer = nullptr,
        std::shared_ptr<Buffer> telemetryBuffer = nullptr,
        std::shared_ptr<Buffer> readBaseCounterBuffer = nullptr,
        bool readReverse = false,
        uint32_t visibleClustersCapacity = 0u,
        bool runWhenComputeSWRasterEnabledOnly = false);
    ~RasterBucketHistogramPass();

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Update(const UpdateExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    void CreatePipelines(
        rhi::Device device,
        rhi::PipelineLayoutHandle globalRootSignature,
        PipelineState& outHistogramPipeline,
        PipelineState& outClearPipeline);

    PipelineState m_histogramPipeline;
    PipelineState m_clearPipeline;
    rhi::CommandSignaturePtr m_histogramCommandSignature;
    std::shared_ptr<Buffer> m_visibleClustersBuffer;
    std::shared_ptr<Buffer> m_visibleClustersCounterBuffer;
    std::shared_ptr<Buffer> m_histogramIndirectCommand;
    std::shared_ptr<Buffer> m_histogramBuffer;
    std::shared_ptr<Buffer> m_reyesOwnershipBitsetBuffer;
    std::shared_ptr<Buffer> m_telemetryBuffer;
    std::shared_ptr<Buffer> m_readBaseCounterBuffer; // Phase 2 only: Phase 1's HW counter for read offset
    bool m_readReverse = false;
    uint32_t m_visibleClustersCapacity = 0u;
    bool m_runWhenComputeSWRasterEnabledOnly = false;
};
