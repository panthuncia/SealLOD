#pragma once

#include <memory>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/ComputePass.h"

namespace org { class Buffer; }
using org::Buffer;
namespace org { class ResourceGroup; }
using org::ResourceGroup;

class ReyesSeedPatchesPass final : public ComputePass {
public:
    ReyesSeedPatchesPass(
        std::shared_ptr<Buffer> visibleClustersBuffer,
        std::shared_ptr<Buffer> ownedClustersBuffer,
        std::shared_ptr<Buffer> ownedClustersCounterBuffer,
        std::shared_ptr<Buffer> splitQueueBuffer,
        std::shared_ptr<Buffer> splitQueueCounterBuffer,
        std::shared_ptr<Buffer> splitQueueOverflowBuffer,
        std::shared_ptr<Buffer> indirectArgsBuffer,
        std::shared_ptr<ResourceGroup> slabResourceGroup,
        uint32_t maxSplitQueueEntries,
        uint32_t phaseIndex);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Update(const UpdateExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    std::shared_ptr<Buffer> m_visibleClustersBuffer;
    std::shared_ptr<Buffer> m_ownedClustersBuffer;
    std::shared_ptr<Buffer> m_ownedClustersCounterBuffer;
    std::shared_ptr<Buffer> m_splitQueueBuffer;
    std::shared_ptr<Buffer> m_splitQueueCounterBuffer;
    std::shared_ptr<Buffer> m_splitQueueOverflowBuffer;
    std::shared_ptr<Buffer> m_indirectArgsBuffer;
    std::shared_ptr<ResourceGroup> m_slabResourceGroup;
    uint32_t m_maxSplitQueueEntries = 0u;
    uint32_t m_phaseIndex = 0u;
    PipelineState m_pso;
    rhi::CommandSignaturePtr m_commandSignature;
};
