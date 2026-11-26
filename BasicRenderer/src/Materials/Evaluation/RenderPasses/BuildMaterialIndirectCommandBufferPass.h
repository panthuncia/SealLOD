#pragma once
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Pipeline/PipelineState/CommandSignatureManager.h"
#include <BasicRenderer/Extensions/PipelineDescs.h>
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Materials/Publication/MaterialStateArtifacts.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "BasicRenderer/Extensions/PassRevision.h"

struct BuildMaterialIndirectCommandBufferBindings {
    br::PipelineState pipeline;
};
struct BuildMaterialIndirectCommandBufferFrame {
    uint32_t slotsUsed = 0;
    bool operator==(const BuildMaterialIndirectCommandBufferFrame&) const = default;
};

// Runs after histogram + prefix sum + pixel list build.
// Fills a single indirect arguments buffer with one entry per material.
// Each entry encodes 4 root constants and a 2D dispatch sized to process all pixels of that material.
class BuildMaterialIndirectCommandBufferPass : public org::TypedRenderGraphPass<BuildMaterialIndirectCommandBufferPass,
    BuildMaterialIndirectCommandBufferFrame, BuildMaterialIndirectCommandBufferBindings> {
public:
    BuildMaterialIndirectCommandBufferBindings Declare(org::PassBuilder& b) {
        b.ShaderResource(
            "Builtin::VisUtil::MaterialPixelCountBuffer",
            "Builtin::VisUtil::MaterialOffsetBuffer");
        b.UnorderedAccess("Builtin::IndirectCommandBuffers::MaterialEvaluationCommandBuffer");
		b.ConstantBuffer(Builtin::PerFrameBuffer);
        b.PreferQueue(org::QueueKind::Compute);
        m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(L"shaders/VisUtil.hlsl", L"BuildEvaluateIndirectArgsCS",
            "VisUtil_BuildEvaluateIndirectArgsPSO"));
        return {m_pipeline};
    }

    // Kept recordings (M6 step 7): the dispatch follows the published material state's slot count, taken from the frame's host
    // data with no side effect (RenderGraphPass::TakeFrameValues) before Prepare reads the same.
    void TakeFrameValues(const org::UpdateExecutionContext& execution) override {
        m_slotsUsed = 0;
        const auto* update = execution.hostData ? execution.hostData->Get<UpdateContext>() : nullptr;
        const auto* render = execution.hostData ? execution.hostData->Get<RenderContext>() : nullptr;
        if (!update && !render) return;
        const auto& published = update ? update->publishedRendererState : render->publishedRendererState;
        const auto materialState = published ? published->materials.payload.Get<br::render::PublishedMaterialState>() : nullptr;
        m_slotsUsed = materialState ? materialState->compileFlagSlotsUsed : 0u;
    }
    void Revision(std::vector<uint64_t>& out) const {
        br::render::AppendPipelineRevision(out, m_pipeline);
        br::render::AppendRevision(out, m_slotsUsed);
    }

    BuildMaterialIndirectCommandBufferFrame Prepare(const BuildMaterialIndirectCommandBufferBindings&,
        const org::PassPrepareContext& preparation) const {
        const auto* update = preparation.preparationData->Get<UpdateContext>();
        const auto* render = preparation.preparationData->Get<RenderContext>();
        if (!update && !render) throw std::logic_error("BuildMaterialIndirectCommandBufferPass requires frame context");
        const auto& published = update ? update->publishedRendererState : render->publishedRendererState;
        const auto materialState = published
            ? published->materials.payload.Get<br::render::PublishedMaterialState>()
            : nullptr;
        return {materialState ? materialState->compileFlagSlotsUsed : 0u};
    }

    static void Record(const BuildMaterialIndirectCommandBufferBindings& bindings,
        const BuildMaterialIndirectCommandBufferFrame& frame, org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[0] = frame.slotsUsed;
        br::render::DispatchCompute(context, bindings.pipeline, constants, (frame.slotsUsed + 63u) / 64u);
    }

private:
    br::PipelineState m_pipeline;
    uint32_t m_slotsUsed = 0;
};
