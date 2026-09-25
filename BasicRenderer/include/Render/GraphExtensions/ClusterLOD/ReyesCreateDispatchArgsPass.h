#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"

namespace org { class Buffer; }

struct ReyesCreateDispatchArgsBindings {
    org::ResourceBindingToken sourceCounter, indirectArgs, sourceBaseCounter;
    uint32_t threadsPerGroup = 0;
    uint32_t maxWorkItemCount = 0;
    bool hasSourceBaseCounter = false;
};

class ReyesCreateDispatchArgsPass final : public org::TypedRenderGraphPass<ReyesCreateDispatchArgsPass,
    br::render::PreparedComputeDispatch, ReyesCreateDispatchArgsBindings> {
public:
    ReyesCreateDispatchArgsPass(
        std::shared_ptr<org::Buffer> sourceCounterBuffer,
        std::shared_ptr<org::Buffer> indirectArgsBuffer,
        std::shared_ptr<org::Buffer> sourceBaseCounterBuffer = nullptr,
        uint32_t threadsPerGroup = 64u,
        uint32_t maxWorkItemCount = 0xFFFFFFFFu);

    ReyesCreateDispatchArgsBindings Declare(org::PassBuilder& builder);
    void Initialize();
    void InvocationRevision(const org::PassPrepareContext&, std::vector<uint64_t>&) const;
    br::render::PreparedComputeDispatch Prepare(const ReyesCreateDispatchArgsBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const ReyesCreateDispatchArgsBindings&,
        const br::render::PreparedComputeDispatch&, org::PassRecordContext&);
    void Update(const org::UpdateExecutionContext& executionContext) override;
    void ShutdownPass();

private:
    org::PipelineState m_pso;
    std::shared_ptr<org::Buffer> m_sourceCounterBuffer;
    std::shared_ptr<org::Buffer> m_indirectArgsBuffer;
    std::shared_ptr<org::Buffer> m_sourceBaseCounterBuffer;
    uint32_t m_threadsPerGroup = 64u;
    uint32_t m_maxWorkItemCount = 0xFFFFFFFFu;
};
