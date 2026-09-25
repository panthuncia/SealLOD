#pragma once

#include <memory>
#include <vector>

#include <rhi.h>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Interfaces/IDynamicDeclaredResources.h"

namespace org { class Buffer; }

struct RasterBucketCompactAndArgsPreparedData {
    rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
    rhi::PipelineLayoutHandle layout{};
    org::PreparedProgramReference clearProgram{}, compactProgram{};
    rhi::CommandSignatureHandle commandSignature{};
    org::PreparedResourceReference indirectCommand{}, cursorResource{};
    std::vector<unsigned int> clearDescriptorIndices, compactDescriptorIndices;
    std::vector<uint32_t> clearConstants, compactConstants;
    uint32_t clearGroups = 0;
    bool enabled = false;
};

struct RasterBucketCompactAndArgsBindings {
    org::ResourceBindingToken visibleClusters, visibleTransforms, visibleCount, compactedBaseCount, readBaseCount;
    org::ResourceBindingToken indirectCommand, histogram, offsets, writeCursor, compactedClusters;
    org::ResourceBindingToken compactedTransforms, indirectArgs, sortedMapping, reyesOwnership, telemetry;
    uint32_t numBuckets = 0;
    uint32_t maxVisibleClusters = 0;
    bool enabled = false;
    bool appendToExisting = false;
    bool readReverse = false;
    bool buildSoftwareRasterDispatch = false;
    bool hasReadBaseCount = false;
    bool hasReyesOwnership = false;
    bool hasTelemetry = false;
    bool telemetryEnabled = false;
};

class RasterBucketCompactAndArgsPass : public org::TypedRenderGraphPass<RasterBucketCompactAndArgsPass,
    RasterBucketCompactAndArgsPreparedData, RasterBucketCompactAndArgsBindings>,
    public org::IDynamicDeclaredResources {
public:
    RasterBucketCompactAndArgsPass(
        std::shared_ptr<org::Buffer> visibleClustersBuffer,
        std::shared_ptr<org::Buffer> visibleClusterTransformIndicesBuffer,
        std::shared_ptr<org::Buffer> visibleClustersCounterBuffer,
        std::shared_ptr<org::Buffer> compactedBaseCounterBuffer,
        std::shared_ptr<org::Buffer> readBaseCounterBuffer,
        std::shared_ptr<org::Buffer> indirectCommand,
        std::shared_ptr<org::Buffer> histogramBuffer,
        std::shared_ptr<org::Buffer> offsetsBuffer,
        std::shared_ptr<org::Buffer> writeCursorBuffer,
        std::shared_ptr<org::Buffer> compactedClustersBuffer,
        std::shared_ptr<org::Buffer> compactedClusterTransformIndicesBuffer,
        std::shared_ptr<org::Buffer> indirectArgsBuffer,
        std::shared_ptr<org::Buffer> sortedToUnsortedMappingBuffer,
        std::shared_ptr<org::Buffer> reyesOwnershipBitsetBuffer,
        std::shared_ptr<org::Buffer> telemetryBuffer,
        uint64_t maxVisibleClusters,
        bool appendToExisting,
        bool readReverse = false,
        bool buildSoftwareRasterDispatch = false,
        bool runWhenComputeSWRasterEnabledOnly = false);

    RasterBucketCompactAndArgsBindings Declare(org::PassBuilder& builder);
    RasterBucketCompactAndArgsPreparedData Prepare(const RasterBucketCompactAndArgsBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const RasterBucketCompactAndArgsBindings&,
        const RasterBucketCompactAndArgsPreparedData&, org::PassRecordContext&);
    void Update(const org::UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override;

private:
    using PreparedData = RasterBucketCompactAndArgsPreparedData;
    org::PipelineState m_pso;
    org::PipelineState m_clearPipeline;
    std::shared_ptr<rhi::CommandSignaturePtr> m_compactionCommandSignature;

    std::shared_ptr<org::Buffer> m_visibleClustersBuffer;
    std::shared_ptr<org::Buffer> m_visibleClusterTransformIndicesBuffer;
    std::shared_ptr<org::Buffer> m_visibleClustersCounterBuffer;
    std::shared_ptr<org::Buffer> m_compactedBaseCounterBuffer;
    std::shared_ptr<org::Buffer> m_readBaseCounterBuffer;
    std::shared_ptr<org::Buffer> m_indirectCommand;
    std::shared_ptr<org::Buffer> m_histogramBuffer;
    std::shared_ptr<org::Buffer> m_offsetsBuffer;
    std::shared_ptr<org::Buffer> m_writeCursorBuffer;
    std::shared_ptr<org::Buffer> m_compactedClustersBuffer;
    std::shared_ptr<org::Buffer> m_compactedClusterTransformIndicesBuffer;
    std::shared_ptr<org::Buffer> m_indirectArgsBuffer;
    std::shared_ptr<org::Buffer> m_sortedToUnsortedMappingBuffer;
    std::shared_ptr<org::Buffer> m_reyesOwnershipBitsetBuffer;
    std::shared_ptr<org::Buffer> m_telemetryBuffer;

    uint64_t m_maxVisibleClusters = 0;
    bool m_appendToExisting = false;
    bool m_readReverse = false;
    bool m_buildSoftwareRasterDispatch = false;
    bool m_runWhenComputeSWRasterEnabledOnly = false;
    uint32_t m_numBuckets = 0;
    bool m_enabled = false;
    bool m_declaredResourcesChanged = true;
};
