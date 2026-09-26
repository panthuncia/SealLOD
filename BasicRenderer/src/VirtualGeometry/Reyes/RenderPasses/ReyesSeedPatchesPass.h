#pragma once

#include <memory>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

namespace org { class Buffer; }
namespace org { class ResourceGroup; }

struct ReyesSeedPatchesBindings {
    org::ResourceBindingToken visible, owned, ownedCounter, splitQueue, splitCounter, splitOverflow, indirectArgs;
    uint32_t capacity = 0, phase = 0;
};

class ReyesSeedPatchesPass final : public org::TypedRenderGraphPass<ReyesSeedPatchesPass,
    br::render::PreparedComputeIndirect, ReyesSeedPatchesBindings> {
public:
    ReyesSeedPatchesPass(
        std::shared_ptr<org::Buffer> visibleClustersBuffer,
        std::shared_ptr<org::Buffer> ownedClustersBuffer,
        std::shared_ptr<org::Buffer> ownedClustersCounterBuffer,
        std::shared_ptr<org::Buffer> splitQueueBuffer,
        std::shared_ptr<org::Buffer> splitQueueCounterBuffer,
        std::shared_ptr<org::Buffer> splitQueueOverflowBuffer,
        std::shared_ptr<org::Buffer> indirectArgsBuffer,
        std::shared_ptr<org::ResourceGroup> slabResourceGroup,
        uint32_t maxSplitQueueEntries,
        uint32_t phaseIndex);

    ReyesSeedPatchesBindings Declare(org::PassBuilder& builder);
    void InvocationRevision(const org::PassPrepareContext&, std::vector<uint64_t>&) const;
    br::render::PreparedComputeIndirect Prepare(const ReyesSeedPatchesBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const ReyesSeedPatchesBindings&, const br::render::PreparedComputeIndirect&, org::PassRecordContext&);
    void Update(const org::UpdateExecutionContext& executionContext) override;

private:
    std::shared_ptr<org::Buffer> m_visibleClustersBuffer;
    std::shared_ptr<org::Buffer> m_ownedClustersBuffer;
    std::shared_ptr<org::Buffer> m_ownedClustersCounterBuffer;
    std::shared_ptr<org::Buffer> m_splitQueueBuffer;
    std::shared_ptr<org::Buffer> m_splitQueueCounterBuffer;
    std::shared_ptr<org::Buffer> m_splitQueueOverflowBuffer;
    std::shared_ptr<org::Buffer> m_indirectArgsBuffer;
    std::shared_ptr<org::ResourceGroup> m_slabResourceGroup;
    uint32_t m_maxSplitQueueEntries = 0u;
    uint32_t m_phaseIndex = 0u;
    org::PipelineState m_pso;
    std::shared_ptr<rhi::CommandSignaturePtr> m_commandSignature;
};
