#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/GraphExtensions/ClusterLOD/CLodStreamingSystem.h"
#include "Render/PipelineState.h"
#include "RenderPasses/Base/ComputePass.h"

class Buffer;
class PixelBuffer;

class VirtualShadowMapApplyUpgradesPass final : public ComputePass {
public:
    using DrainEventsFn = std::function<std::vector<CLodVirtualShadowUpgradeInvalidationInput>(uint32_t)>;
    using QueueStatsFn = std::function<CLodVirtualShadowUpgradeQueueStats()>;

    VirtualShadowMapApplyUpgradesPass(
        std::shared_ptr<Buffer> inputsBuffer,
        std::shared_ptr<Buffer> inputCountBuffer,
        std::shared_ptr<PixelBuffer> pageTableTexture,
        std::shared_ptr<Buffer> pageMetadataBuffer,
        std::shared_ptr<Buffer> dirtyPageFlagsBuffer,
        std::shared_ptr<Buffer> statsBuffer,
        DrainEventsFn drainEvents,
        QueueStatsFn queueStats);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    void Update(const UpdateExecutionContext& executionContext) override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    PipelineState m_pso;
    std::shared_ptr<Buffer> m_inputsBuffer;
    std::shared_ptr<Buffer> m_inputCountBuffer;
    std::shared_ptr<PixelBuffer> m_pageTableTexture;
    std::shared_ptr<Buffer> m_pageMetadataBuffer;
    std::shared_ptr<Buffer> m_dirtyPageFlagsBuffer;
    std::shared_ptr<Buffer> m_statsBuffer;
    DrainEventsFn m_drainEvents;
    QueueStatsFn m_queueStats;
    uint32_t m_pendingInputCount = 0u;
    bool m_pendingBatchDispatched = false;
    uint32_t m_telemetryFrameCounter = 0u;
};
