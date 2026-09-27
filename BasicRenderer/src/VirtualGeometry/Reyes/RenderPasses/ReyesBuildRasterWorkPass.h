#pragma once

#include <memory>

#include <rhi.h>

#include "VirtualGeometry/GraphIntegration/CLodViewTables.h"
#include "Render/PreparedTablePublisher.h"
#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

namespace org { class Buffer; }
namespace org { class ResourceGroup; }

struct ReyesBuildRasterWorkBindings {
    org::ResourceBindingToken diceQueue, diceCounter, readOffset, tessConfigs, output, outputCounter, indirectArgs, telemetry;
    org::ResourceBindingToken visibleClusters, visibleTransforms, replayQueue, replayCounter, replayOverflow;
    uint32_t capacity = 0, phase = 0, replayCapacity = 0;
    bool hasReadOffset = false, hasVisibleClusters = false, hasVisibleTransforms = false, hasViewDepthIndices = false;
    bool hasReplayQueue = false, hasReplayCounter = false, hasReplayOverflow = false, useAabbOcclusion = false;
};

class ReyesBuildRasterWorkPass final : public org::TypedRenderGraphPass<ReyesBuildRasterWorkPass,
    br::render::PreparedComputeIndirect, ReyesBuildRasterWorkBindings> {
public:
    ReyesBuildRasterWorkPass(
        std::shared_ptr<org::Buffer> diceQueueBuffer,
        std::shared_ptr<org::Buffer> diceQueueCounterBuffer,
        std::shared_ptr<org::Buffer> diceQueueReadOffsetBuffer,
        std::shared_ptr<org::Buffer> tessTableConfigsBuffer,
        std::shared_ptr<org::Buffer> rasterWorkBuffer,
        std::shared_ptr<org::Buffer> rasterWorkCounterBuffer,
        std::shared_ptr<org::Buffer> indirectArgsBuffer,
        std::shared_ptr<org::Buffer> telemetryBuffer,
        uint32_t rasterWorkCapacity,
        uint32_t phaseIndex = 0u,
        std::shared_ptr<org::Buffer> visibleClustersBuffer = nullptr,
        std::shared_ptr<org::Buffer> visibleClusterTransformIndicesBuffer = nullptr,
        bool enableViewDepthOcclusion = false,
        std::shared_ptr<org::Buffer> replayDiceQueueBuffer = nullptr,
        std::shared_ptr<org::Buffer> replayDiceQueueCounterBuffer = nullptr,
        std::shared_ptr<org::Buffer> replayDiceQueueOverflowBuffer = nullptr,
        uint32_t replayDiceQueueCapacity = 0u,
        std::shared_ptr<org::ResourceGroup> slabResourceGroup = nullptr);

    ReyesBuildRasterWorkBindings Declare(org::PassBuilder& builder);
    void Update(const org::UpdateExecutionContext& executionContext) override;
    void InvocationRevision(const org::PassPrepareContext&, std::vector<uint64_t>&) const;
    br::render::PreparedComputeIndirect Prepare(const ReyesBuildRasterWorkBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const ReyesBuildRasterWorkBindings&, const br::render::PreparedComputeIndirect&, org::PassRecordContext&);

private:
    std::shared_ptr<org::Buffer> m_diceQueueBuffer;
    std::shared_ptr<org::Buffer> m_diceQueueCounterBuffer;
    std::shared_ptr<org::Buffer> m_diceQueueReadOffsetBuffer;
    std::shared_ptr<org::Buffer> m_tessTableConfigsBuffer;
    std::shared_ptr<org::Buffer> m_rasterWorkBuffer;
    std::shared_ptr<org::Buffer> m_rasterWorkCounterBuffer;
    std::shared_ptr<org::Buffer> m_indirectArgsBuffer;
    std::shared_ptr<org::Buffer> m_telemetryBuffer;
    std::shared_ptr<org::Buffer> m_visibleClustersBuffer;
    std::shared_ptr<org::Buffer> m_visibleClusterTransformIndicesBuffer;
    bool m_enableViewDepthOcclusion = false;
    // Linear-depth SRV per view for patch occlusion; it embeds descriptors, so
    // it is published during preparation from the frame's bindings. Phase 1
    // tests against history depth, as the phase-1 culling pass does.
    org::PreparedTablePublisher m_viewDepthPublisher{"CLod Reyes Build Raster Work View Depth SRV Indices"};
    std::shared_ptr<org::Buffer> m_replayDiceQueueBuffer;
    std::shared_ptr<org::Buffer> m_replayDiceQueueCounterBuffer;
    std::shared_ptr<org::Buffer> m_replayDiceQueueOverflowBuffer;
    std::shared_ptr<org::ResourceGroup> m_slabResourceGroup;
    uint32_t m_rasterWorkCapacity = 0u;
    uint32_t m_phaseIndex = 0u;
    uint32_t m_replayDiceQueueCapacity = 0u;
    org::PipelineState m_pso;
    std::shared_ptr<rhi::CommandSignaturePtr> m_commandSignature;
};
