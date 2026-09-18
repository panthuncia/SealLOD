#pragma once

#include <memory>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"

namespace org { class Buffer; }
using org::Buffer;
namespace org { class ResourceGroup; }
using org::ResourceGroup;

struct ReyesSeedPatchesBindings {
    org::ResourceBindingToken visible, owned, ownedCounter, splitQueue, splitCounter, splitOverflow, indirectArgs;
    uint32_t capacity = 0, phase = 0;
};

class ReyesSeedPatchesPass final : public org::TypedRenderGraphPass<ReyesSeedPatchesPass,
    br::render::PreparedComputeIndirect, ReyesSeedPatchesBindings> {
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

    ReyesSeedPatchesBindings Declare(org::PassBuilder& builder);
    void InvocationRevision(const org::PassPrepareContext&, std::vector<uint64_t>&) const;
    br::render::PreparedComputeIndirect Prepare(const ReyesSeedPatchesBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const ReyesSeedPatchesBindings&, const br::render::PreparedComputeIndirect&, org::PassRecordContext&);
    void Update(const UpdateExecutionContext& executionContext) override;

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
    std::shared_ptr<rhi::CommandSignaturePtr> m_commandSignature;
};
