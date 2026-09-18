#pragma once

#include <memory>
#include <vector>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "Render/ShaderAPI.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"

namespace org { class Buffer; }
using org::Buffer;
namespace org { class PixelBuffer; }
using org::PixelBuffer;

struct ReyesSplitFrameData {
    br::render::PreparedComputeDispatch clear;
    br::render::PreparedComputeIndirect split;
    std::array<org::PreparedResourceReference, 2> outputCounters;
};

struct ReyesSplitBindings {
    org::ResourceBindingToken visible, inputQueue, inputCounter, outputQueue, outputCounter, outputOverflow;
    org::ResourceBindingToken diceQueue, diceCounter, diceOverflow, tessConfigs, tessVertices, tessTriangles;
    org::ResourceBindingToken shadowClipmap, shadowDirty, shadowNonRasterable, indirectArgs, telemetry, viewDepthIndices;
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
        std::shared_ptr<Buffer> visibleClustersBuffer,
        std::shared_ptr<Buffer> inputSplitQueueBuffer,
        std::shared_ptr<Buffer> inputSplitQueueCounterBuffer,
        std::shared_ptr<Buffer> outputSplitQueueBuffer,
        std::shared_ptr<Buffer> outputSplitQueueCounterBuffer,
        std::shared_ptr<Buffer> outputSplitQueueOverflowBuffer,
        std::shared_ptr<Buffer> diceQueueBuffer,
        std::shared_ptr<Buffer> diceQueueCounterBuffer,
        std::shared_ptr<Buffer> diceQueueOverflowBuffer,
        std::shared_ptr<Buffer> tessTableConfigsBuffer,
        std::shared_ptr<Buffer> tessTableVerticesBuffer,
        std::shared_ptr<Buffer> tessTableTrianglesBuffer,
        std::shared_ptr<Buffer> shadowClipmapInfoBuffer,
        std::shared_ptr<PixelBuffer> shadowDirtyHierarchyTexture,
        std::shared_ptr<PixelBuffer> shadowNonRasterableHierarchyTexture,
        std::shared_ptr<Buffer> indirectArgsBuffer,
        std::shared_ptr<Buffer> telemetryBuffer,
        uint32_t maxSplitQueueEntries,
        uint32_t splitPassIndex,
        uint32_t maxSplitPassCount,
        uint32_t phaseIndex,
        std::shared_ptr<Buffer> viewDepthSrvIndicesBuffer = nullptr,
        std::shared_ptr<Buffer> replaySplitQueueBuffer = nullptr,
        std::shared_ptr<Buffer> replaySplitQueueCounterBuffer = nullptr,
        std::shared_ptr<Buffer> replaySplitQueueOverflowBuffer = nullptr);

    ReyesSplitBindings Declare(org::PassBuilder& builder);
    void InvocationRevision(const org::PassPrepareContext&, std::vector<uint64_t>&) const;
    ReyesSplitFrameData Prepare(const ReyesSplitBindings&, const org::PassPrepareContext& preparation) const;
    static void Record(const ReyesSplitBindings&, const ReyesSplitFrameData&, org::PassRecordContext&);

private:
    std::shared_ptr<Buffer> m_visibleClustersBuffer;
    std::shared_ptr<Buffer> m_inputSplitQueueBuffer;
    std::shared_ptr<Buffer> m_inputSplitQueueCounterBuffer;
    std::shared_ptr<Buffer> m_outputSplitQueueBuffer;
    std::shared_ptr<Buffer> m_outputSplitQueueCounterBuffer;
    std::shared_ptr<Buffer> m_outputSplitQueueOverflowBuffer;
    std::shared_ptr<Buffer> m_diceQueueBuffer;
    std::shared_ptr<Buffer> m_diceQueueCounterBuffer;
    std::shared_ptr<Buffer> m_diceQueueOverflowBuffer;
    std::shared_ptr<Buffer> m_indirectArgsBuffer;
    std::shared_ptr<Buffer> m_tessTableConfigsBuffer;
    std::shared_ptr<Buffer> m_tessTableVerticesBuffer;
    std::shared_ptr<Buffer> m_tessTableTrianglesBuffer;
    std::shared_ptr<Buffer> m_shadowClipmapInfoBuffer;
    std::shared_ptr<PixelBuffer> m_shadowDirtyHierarchyTexture;
    std::shared_ptr<PixelBuffer> m_shadowNonRasterableHierarchyTexture;
    std::shared_ptr<Buffer> m_telemetryBuffer;
    std::shared_ptr<Buffer> m_viewDepthSrvIndicesBuffer;
    std::shared_ptr<Buffer> m_replaySplitQueueBuffer;
    std::shared_ptr<Buffer> m_replaySplitQueueCounterBuffer;
    std::shared_ptr<Buffer> m_replaySplitQueueOverflowBuffer;
    uint32_t m_maxSplitQueueEntries = 0u;
    uint32_t m_splitPassIndex = 0u;
    uint32_t m_maxSplitPassCount = 0u;
    uint32_t m_phaseIndex = 0u;
    PipelineState m_clearCountersPso;
    PipelineState m_pso;
    std::shared_ptr<rhi::CommandSignaturePtr> m_commandSignature;
};
