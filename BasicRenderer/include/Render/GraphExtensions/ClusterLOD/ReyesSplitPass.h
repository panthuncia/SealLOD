#pragma once

#include <memory>
#include <vector>

#include <rhi.h>

#include "Render/GraphExtensions/ClusterLOD/CLodViewTables.h"
#include "Render/PreparedTablePublisher.h"
#include "Render/PipelineState.h"
#include "Render/ShaderAPI.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct ReyesSplitFrameData {
    br::render::PreparedComputeDispatch clear;
    br::render::PreparedComputeIndirect split;
    std::array<org::PreparedResourceReference, 2> outputCounters;
};

struct ReyesSplitBindings {
    org::ResourceBindingToken visible, inputQueue, inputCounter, outputQueue, outputCounter, outputOverflow;
    org::ResourceBindingToken diceQueue, diceCounter, diceOverflow, tessConfigs, tessVertices, tessTriangles;
    org::ResourceBindingToken shadowClipmap, shadowDirty, shadowNonRasterable, indirectArgs, telemetry;
    org::ResourceBindingToken replayQueue, replayCounter, replayOverflow;
    uint32_t capacity = 0, maxPassCount = 0, phase = 0, coarseTargetBits = 0;
    bool hasShadowClipmap = false, hasShadowDirty = false, hasShadowNonRasterable = false;
    bool hasViewDepth = false, hasReplayQueue = false, hasReplayCounter = false, hasReplayOverflow = false;
    bool useAabbOcclusion = false;
};

class ReyesSplitPass final : public org::TypedRenderGraphPass<ReyesSplitPass,
    ReyesSplitFrameData, ReyesSplitBindings> {
public:
    ReyesSplitPass(
        std::shared_ptr<org::Buffer> visibleClustersBuffer,
        std::shared_ptr<org::Buffer> inputSplitQueueBuffer,
        std::shared_ptr<org::Buffer> inputSplitQueueCounterBuffer,
        std::shared_ptr<org::Buffer> outputSplitQueueBuffer,
        std::shared_ptr<org::Buffer> outputSplitQueueCounterBuffer,
        std::shared_ptr<org::Buffer> outputSplitQueueOverflowBuffer,
        std::shared_ptr<org::Buffer> diceQueueBuffer,
        std::shared_ptr<org::Buffer> diceQueueCounterBuffer,
        std::shared_ptr<org::Buffer> diceQueueOverflowBuffer,
        std::shared_ptr<org::Buffer> tessTableConfigsBuffer,
        std::shared_ptr<org::Buffer> tessTableVerticesBuffer,
        std::shared_ptr<org::Buffer> tessTableTrianglesBuffer,
        std::shared_ptr<org::Buffer> shadowClipmapInfoBuffer,
        std::shared_ptr<org::PixelBuffer> shadowDirtyHierarchyTexture,
        std::shared_ptr<org::PixelBuffer> shadowNonRasterableHierarchyTexture,
        std::shared_ptr<org::Buffer> indirectArgsBuffer,
        std::shared_ptr<org::Buffer> telemetryBuffer,
        uint32_t maxSplitQueueEntries,
        uint32_t splitPassIndex,
        uint32_t maxSplitPassCount,
        uint32_t phaseIndex,
        bool enableViewDepthOcclusion = false,
        std::shared_ptr<org::Buffer> replaySplitQueueBuffer = nullptr,
        std::shared_ptr<org::Buffer> replaySplitQueueCounterBuffer = nullptr,
        std::shared_ptr<org::Buffer> replaySplitQueueOverflowBuffer = nullptr);

    ReyesSplitBindings Declare(org::PassBuilder& builder);
    void InvocationRevision(const org::PassPrepareContext&, std::vector<uint64_t>&) const;
    ReyesSplitFrameData Prepare(const ReyesSplitBindings&, const org::PassPrepareContext& preparation) const;
    static void Record(const ReyesSplitBindings&, const ReyesSplitFrameData&, org::PassRecordContext&);

private:
    std::shared_ptr<org::Buffer> m_visibleClustersBuffer;
    std::shared_ptr<org::Buffer> m_inputSplitQueueBuffer;
    std::shared_ptr<org::Buffer> m_inputSplitQueueCounterBuffer;
    std::shared_ptr<org::Buffer> m_outputSplitQueueBuffer;
    std::shared_ptr<org::Buffer> m_outputSplitQueueCounterBuffer;
    std::shared_ptr<org::Buffer> m_outputSplitQueueOverflowBuffer;
    std::shared_ptr<org::Buffer> m_diceQueueBuffer;
    std::shared_ptr<org::Buffer> m_diceQueueCounterBuffer;
    std::shared_ptr<org::Buffer> m_diceQueueOverflowBuffer;
    std::shared_ptr<org::Buffer> m_indirectArgsBuffer;
    std::shared_ptr<org::Buffer> m_tessTableConfigsBuffer;
    std::shared_ptr<org::Buffer> m_tessTableVerticesBuffer;
    std::shared_ptr<org::Buffer> m_tessTableTrianglesBuffer;
    std::shared_ptr<org::Buffer> m_shadowClipmapInfoBuffer;
    std::shared_ptr<org::PixelBuffer> m_shadowDirtyHierarchyTexture;
    std::shared_ptr<org::PixelBuffer> m_shadowNonRasterableHierarchyTexture;
    std::shared_ptr<org::Buffer> m_telemetryBuffer;
    bool m_enableViewDepthOcclusion = false;
    // Linear-depth SRV per view for patch occlusion; it embeds descriptors, so
    // it is published during preparation from the frame's bindings. Phase 1
    // tests against history depth, as the phase-1 culling pass does.
    org::PreparedTablePublisher m_viewDepthPublisher{"CLod Reyes Split View Depth SRV Indices"};
    std::shared_ptr<org::Buffer> m_replaySplitQueueBuffer;
    std::shared_ptr<org::Buffer> m_replaySplitQueueCounterBuffer;
    std::shared_ptr<org::Buffer> m_replaySplitQueueOverflowBuffer;
    uint32_t m_maxSplitQueueEntries = 0u;
    uint32_t m_splitPassIndex = 0u;
    uint32_t m_maxSplitPassCount = 0u;
    uint32_t m_phaseIndex = 0u;
    org::PipelineState m_clearCountersPso;
    org::PipelineState m_pso;
    std::shared_ptr<rhi::CommandSignaturePtr> m_commandSignature;
};
