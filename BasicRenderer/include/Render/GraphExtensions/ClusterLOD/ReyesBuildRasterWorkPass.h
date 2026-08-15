#pragma once

#include <memory>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/ComputePass.h"

namespace org { class Buffer; }
using org::Buffer;
namespace org { class ResourceGroup; }
using org::ResourceGroup;

class ReyesBuildRasterWorkPass final : public ComputePass {
public:
    ReyesBuildRasterWorkPass(
        std::shared_ptr<Buffer> diceQueueBuffer,
        std::shared_ptr<Buffer> diceQueueCounterBuffer,
        std::shared_ptr<Buffer> diceQueueReadOffsetBuffer,
        std::shared_ptr<Buffer> tessTableConfigsBuffer,
        std::shared_ptr<Buffer> rasterWorkBuffer,
        std::shared_ptr<Buffer> rasterWorkCounterBuffer,
        std::shared_ptr<Buffer> indirectArgsBuffer,
        std::shared_ptr<Buffer> telemetryBuffer,
        uint32_t rasterWorkCapacity,
        uint32_t phaseIndex = 0u,
        std::shared_ptr<Buffer> visibleClustersBuffer = nullptr,
        std::shared_ptr<Buffer> visibleClusterTransformIndicesBuffer = nullptr,
        std::shared_ptr<Buffer> viewDepthSrvIndicesBuffer = nullptr,
        std::shared_ptr<Buffer> replayDiceQueueBuffer = nullptr,
        std::shared_ptr<Buffer> replayDiceQueueCounterBuffer = nullptr,
        std::shared_ptr<Buffer> replayDiceQueueOverflowBuffer = nullptr,
        uint32_t replayDiceQueueCapacity = 0u,
        std::shared_ptr<ResourceGroup> slabResourceGroup = nullptr);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    void Update(const UpdateExecutionContext& executionContext) override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    std::shared_ptr<Buffer> m_diceQueueBuffer;
    std::shared_ptr<Buffer> m_diceQueueCounterBuffer;
    std::shared_ptr<Buffer> m_diceQueueReadOffsetBuffer;
    std::shared_ptr<Buffer> m_tessTableConfigsBuffer;
    std::shared_ptr<Buffer> m_rasterWorkBuffer;
    std::shared_ptr<Buffer> m_rasterWorkCounterBuffer;
    std::shared_ptr<Buffer> m_indirectArgsBuffer;
    std::shared_ptr<Buffer> m_telemetryBuffer;
    std::shared_ptr<Buffer> m_visibleClustersBuffer;
    std::shared_ptr<Buffer> m_visibleClusterTransformIndicesBuffer;
    std::shared_ptr<Buffer> m_viewDepthSrvIndicesBuffer;
    std::shared_ptr<Buffer> m_replayDiceQueueBuffer;
    std::shared_ptr<Buffer> m_replayDiceQueueCounterBuffer;
    std::shared_ptr<Buffer> m_replayDiceQueueOverflowBuffer;
    std::shared_ptr<ResourceGroup> m_slabResourceGroup;
    uint32_t m_rasterWorkCapacity = 0u;
    uint32_t m_phaseIndex = 0u;
    uint32_t m_replayDiceQueueCapacity = 0u;
    PipelineState m_pso;
    rhi::CommandSignaturePtr m_commandSignature;
};
