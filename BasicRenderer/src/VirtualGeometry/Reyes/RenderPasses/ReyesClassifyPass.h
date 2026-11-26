#pragma once

#include <memory>

#include <rhi.h>

#include <BasicRenderer/Pipeline/PipelineDesc.h>
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "BasicRenderer/Extensions/PassRevision.h"

namespace org { class Buffer; }

enum class ReyesClassifyMode : uint32_t
{
    Default = 0u,
    ShadowFineDisplacedOnly = 1u,
};

struct ReyesClassifyBindings {
    br::PipelineState pipeline;
    br::CommandSignatureState commandSignature;
    org::DeclaredViewToken visible, visibleCounter, readBaseCounter, fullClusters, fullCounter;
    org::DeclaredViewToken ownedClusters, ownedCounter, ownershipBitset, telemetry;
    org::ResourceBindingToken indirectArgs;
    uint32_t fullCapacity = 0, ownedCapacity = 0, phase = 0, mode = 0;
    bool hasReadBaseCounter = false, hasOwnershipBitset = false;
};

class ReyesClassifyPass final : public org::TypedRenderGraphPass<ReyesClassifyPass,
    org::EmptyPassFrameData, ReyesClassifyBindings> {
public:
    ReyesClassifyPass(
        std::shared_ptr<org::Buffer> visibleClustersBuffer,
        std::shared_ptr<org::Buffer> visibleClustersCounterBuffer,
        std::shared_ptr<org::Buffer> visibleClustersReadBaseCounterBuffer,
        std::shared_ptr<org::Buffer> fullClusterOutputsBuffer,
        std::shared_ptr<org::Buffer> fullClusterCounterBuffer,
        uint32_t fullClusterOutputCapacity,
        std::shared_ptr<org::Buffer> ownedClustersBuffer,
        std::shared_ptr<org::Buffer> ownedClustersCounterBuffer,
        uint32_t ownedClusterCapacity,
        std::shared_ptr<org::Buffer> ownershipBitsetBuffer,
        std::shared_ptr<org::Buffer> indirectArgsBuffer,
        std::shared_ptr<org::Buffer> telemetryBuffer,
        uint32_t phaseIndex,
        ReyesClassifyMode classifyMode = ReyesClassifyMode::Default);

    ReyesClassifyBindings Declare(org::PassBuilder& builder);
    static void Record(const ReyesClassifyBindings& bindings, org::PassRecordContext& recording);
    // Kept recordings (M6 step 8): Record bakes only its bindings and its pipelines' builds.
    void Revision(std::vector<uint64_t>& out) const { m_pipelines.Append(out); }

private:
    br::render::DeclaredPipelines m_pipelines;
    std::shared_ptr<org::Buffer> m_visibleClustersBuffer;
    std::shared_ptr<org::Buffer> m_visibleClustersCounterBuffer;
    std::shared_ptr<org::Buffer> m_visibleClustersReadBaseCounterBuffer;
    std::shared_ptr<org::Buffer> m_fullClusterOutputsBuffer;
    std::shared_ptr<org::Buffer> m_fullClusterCounterBuffer;
    uint32_t m_fullClusterOutputCapacity = 0u;
    std::shared_ptr<org::Buffer> m_ownedClustersBuffer;
    std::shared_ptr<org::Buffer> m_ownedClustersCounterBuffer;
    uint32_t m_ownedClusterCapacity = 0u;
    std::shared_ptr<org::Buffer> m_ownershipBitsetBuffer;
    std::shared_ptr<org::Buffer> m_indirectArgsBuffer;
    std::shared_ptr<org::Buffer> m_telemetryBuffer;
    uint32_t m_phaseIndex = 0u;
    ReyesClassifyMode m_classifyMode = ReyesClassifyMode::Default;
};
