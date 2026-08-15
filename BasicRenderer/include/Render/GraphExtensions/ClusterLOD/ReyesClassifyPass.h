#pragma once

#include <memory>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/ComputePass.h"

namespace org { class Buffer; }
using org::Buffer;

enum class ReyesClassifyMode : uint32_t
{
    Default = 0u,
    ShadowFineDisplacedOnly = 1u,
    ShadowCoarseLargeOnly = 2u,
};

class ReyesClassifyPass final : public ComputePass {
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

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Update(const UpdateExecutionContext& executionContext) override;
    void Cleanup() override;

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
    rhi::CommandSignaturePtr m_commandSignature;
};
