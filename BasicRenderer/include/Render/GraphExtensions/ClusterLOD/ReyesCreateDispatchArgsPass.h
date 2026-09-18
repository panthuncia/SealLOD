#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"

namespace org { class Buffer; }
using org::Buffer;

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
        std::shared_ptr<Buffer> sourceCounterBuffer,
        std::shared_ptr<Buffer> indirectArgsBuffer,
        std::shared_ptr<Buffer> sourceBaseCounterBuffer = nullptr,
        uint32_t threadsPerGroup = 64u,
        uint32_t maxWorkItemCount = 0xFFFFFFFFu);

    ReyesCreateDispatchArgsBindings Declare(org::PassBuilder& builder);
    void Initialize();
    void InvocationRevision(const org::PassPrepareContext&, std::vector<uint64_t>&) const;
    br::render::PreparedComputeDispatch Prepare(const ReyesCreateDispatchArgsBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const ReyesCreateDispatchArgsBindings&,
        const br::render::PreparedComputeDispatch&, org::PassRecordContext&);
    void Update(const UpdateExecutionContext& executionContext) override;
    void ShutdownPass();

private:
    PipelineState m_pso;
    std::shared_ptr<Buffer> m_sourceCounterBuffer;
    std::shared_ptr<Buffer> m_indirectArgsBuffer;
    std::shared_ptr<Buffer> m_sourceBaseCounterBuffer;
    uint32_t m_threadsPerGroup = 64u;
    uint32_t m_maxWorkItemCount = 0xFFFFFFFFu;
};
