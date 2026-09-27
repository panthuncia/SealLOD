#pragma once

#include <memory>
#include <vector>

#include <rhi.h>

#include "RenderPasses/Base/TypedRenderGraphPass.h"

namespace org { class Buffer; }

struct RasterBucketHistogramPreparedData {
    rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
    rhi::PipelineLayoutHandle layout{};
    org::PreparedProgramReference clearProgram{}, histogramProgram{};
    rhi::CommandSignatureHandle commandSignature{};
    org::PreparedResourceReference indirectArguments{}, histogramResource{};
    std::vector<unsigned int> clearDescriptorIndices, histogramDescriptorIndices;
    std::vector<uint32_t> clearConstants, histogramConstants;
    uint32_t clearGroups = 0;
    bool enabled = false;
};

struct RasterBucketHistogramBindings {
    org::ResourceBindingToken visibleClusters, visibleCount, indirectArguments, histogram;
    org::ResourceBindingToken reyesOwnership, telemetry, readBaseCounter;
    uint32_t numBuckets = 0;
    uint32_t visibleCapacity = 0;
    bool enabled = false;
    bool telemetryEnabled = false;
    bool readReverse = false;
    bool hasReyesOwnership = false;
    bool hasTelemetry = false;
    bool hasReadBaseCounter = false;
};

class RasterBucketHistogramPass : public org::TypedRenderGraphPass<RasterBucketHistogramPass,
    RasterBucketHistogramPreparedData, RasterBucketHistogramBindings> {
public:
    RasterBucketHistogramPass(
        std::shared_ptr<org::Buffer> visibleClustersBuffer,
        std::shared_ptr<org::Buffer> visibleClustersCounterBuffer,
        std::shared_ptr<org::Buffer> histogramIndirectCommand,
        std::shared_ptr<org::Buffer> histogramBuffer,
        std::shared_ptr<org::Buffer> reyesOwnershipBitsetBuffer = nullptr,
        std::shared_ptr<org::Buffer> telemetryBuffer = nullptr,
        std::shared_ptr<org::Buffer> readBaseCounterBuffer = nullptr,
        bool readReverse = false,
        uint32_t visibleClustersCapacity = 0u,
        bool runWhenComputeSWRasterEnabledOnly = false);
    ~RasterBucketHistogramPass();

    RasterBucketHistogramBindings Declare(org::PassBuilder& builder);
    RasterBucketHistogramPreparedData Prepare(const RasterBucketHistogramBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const RasterBucketHistogramBindings&,
        const RasterBucketHistogramPreparedData&, org::PassRecordContext&);
    void Update(const org::UpdateExecutionContext& executionContext) override;

private:
    using PreparedData = RasterBucketHistogramPreparedData;
    void CreatePipelines(
        rhi::Device device,
        rhi::PipelineLayoutHandle globalRootSignature,
        org::PipelineState& outHistogramPipeline,
        org::PipelineState& outClearPipeline);

    org::PipelineState m_histogramPipeline;
    org::PipelineState m_clearPipeline;
    std::shared_ptr<rhi::CommandSignaturePtr> m_histogramCommandSignature;
    std::shared_ptr<org::Buffer> m_visibleClustersBuffer;
    std::shared_ptr<org::Buffer> m_visibleClustersCounterBuffer;
    std::shared_ptr<org::Buffer> m_histogramIndirectCommand;
    std::shared_ptr<org::Buffer> m_histogramBuffer;
    std::shared_ptr<org::Buffer> m_reyesOwnershipBitsetBuffer;
    std::shared_ptr<org::Buffer> m_telemetryBuffer;
    std::shared_ptr<org::Buffer> m_readBaseCounterBuffer;
    bool m_readReverse = false;
    uint32_t m_visibleClustersCapacity = 0u;
    bool m_runWhenComputeSWRasterEnabledOnly = false;
    uint32_t m_numBuckets = 0u;
    bool m_enabled = false;
};
