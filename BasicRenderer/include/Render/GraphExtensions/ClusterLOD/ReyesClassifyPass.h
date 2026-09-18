#pragma once

#include <memory>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"

namespace org { class Buffer; }
using org::Buffer;

enum class ReyesClassifyMode : uint32_t
{
    Default = 0u,
    ShadowFineDisplacedOnly = 1u,
    ShadowCoarseLargeOnly = 2u,
};

struct ReyesClassifyBindings {
    org::ResourceBindingToken visible, visibleCounter, readBaseCounter, fullClusters, fullCounter;
    org::ResourceBindingToken ownedClusters, ownedCounter, ownershipBitset, indirectArgs, telemetry;
    uint32_t fullCapacity = 0, ownedCapacity = 0, phase = 0, mode = 0;
    bool hasReadBaseCounter = false, hasOwnershipBitset = false;
};

class ReyesClassifyPass final : public org::TypedRenderGraphPass<ReyesClassifyPass,
    br::render::PreparedComputeIndirect, ReyesClassifyBindings> {
public:
    ReyesClassifyPass(
        std::shared_ptr<Buffer> visibleClustersBuffer,
        std::shared_ptr<Buffer> visibleClustersCounterBuffer,
        std::shared_ptr<Buffer> visibleClustersReadBaseCounterBuffer,
        std::shared_ptr<Buffer> fullClusterOutputsBuffer,
        std::shared_ptr<Buffer> fullClusterCounterBuffer,
        uint32_t fullClusterOutputCapacity,
        std::shared_ptr<Buffer> ownedClustersBuffer,
        std::shared_ptr<Buffer> ownedClustersCounterBuffer,
        uint32_t ownedClusterCapacity,
        std::shared_ptr<Buffer> ownershipBitsetBuffer,
        std::shared_ptr<Buffer> indirectArgsBuffer,
        std::shared_ptr<Buffer> telemetryBuffer,
        uint32_t phaseIndex,
        ReyesClassifyMode classifyMode = ReyesClassifyMode::Default);

    ReyesClassifyBindings Declare(org::PassBuilder& builder);
    void InvocationRevision(const org::PassPrepareContext&, std::vector<uint64_t>&) const;
    br::render::PreparedComputeIndirect Prepare(const ReyesClassifyBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const ReyesClassifyBindings&, const br::render::PreparedComputeIndirect&, org::PassRecordContext&);

private:
    std::shared_ptr<Buffer> m_visibleClustersBuffer;
    std::shared_ptr<Buffer> m_visibleClustersCounterBuffer;
    std::shared_ptr<Buffer> m_visibleClustersReadBaseCounterBuffer;
    std::shared_ptr<Buffer> m_fullClusterOutputsBuffer;
    std::shared_ptr<Buffer> m_fullClusterCounterBuffer;
    uint32_t m_fullClusterOutputCapacity = 0u;
    std::shared_ptr<Buffer> m_ownedClustersBuffer;
    std::shared_ptr<Buffer> m_ownedClustersCounterBuffer;
    uint32_t m_ownedClusterCapacity = 0u;
    std::shared_ptr<Buffer> m_ownershipBitsetBuffer;
    std::shared_ptr<Buffer> m_indirectArgsBuffer;
    std::shared_ptr<Buffer> m_telemetryBuffer;
    uint32_t m_phaseIndex = 0u;
    ReyesClassifyMode m_classifyMode = ReyesClassifyMode::Default;
    PipelineState m_pso;
    std::shared_ptr<rhi::CommandSignaturePtr> m_commandSignature;
};
