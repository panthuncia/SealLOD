#pragma once
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include <BasicRenderer/Extensions/PipelineDescs.h>
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Materials/Publication/MaterialStateArtifacts.h"
#include "BasicRenderer/Assets/TechniqueDescriptor.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "BasicRenderer/Extensions/PassRevision.h"

struct MaterialUAVResetBindings {
    br::PipelineState pipeline;
};
struct MaterialUAVResetFrame {
    uint32_t slotsUsed = 0;
    bool operator==(const MaterialUAVResetFrame&) const = default;
};

class MaterialUAVResetPass : public org::TypedRenderGraphPass<MaterialUAVResetPass, MaterialUAVResetFrame, MaterialUAVResetBindings> {
public:
    MaterialUAVResetBindings Declare(org::PassBuilder& builder) {
        builder.UnorderedAccess("Builtin::VisUtil::MaterialPixelCountBuffer",
            "Builtin::VisUtil::MaterialWriteCursorBuffer");
		builder.ConstantBuffer(Builtin::PerFrameBuffer);
        builder.PreferQueue(org::QueueKind::Compute);
        m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(L"shaders/VisUtil.hlsl", L"ClearMaterialCountersCS",
            "ClearMaterialCountersPSO"));
        return {m_pipeline};
    }

    // Kept recordings (M6 step 8): the slot count follows the published material state, taken before Update
    // (RenderGraphPass::TakeFrameValues) and returned by Prepare.
    void TakeFrameValues(const org::UpdateExecutionContext& execution) override {
        const auto* update = execution.hostData ? execution.hostData->Get<UpdateContext>() : nullptr;
        const auto* render = execution.hostData ? execution.hostData->Get<RenderContext>() : nullptr;
        m_taken = {};
        if (!update && !render) return;
        const auto& published = update ? update->publishedRendererState : render->publishedRendererState;
        const auto materialState = published
            ? published->materials.payload.Get<br::render::PublishedMaterialState>()
            : nullptr;
        m_taken = {materialState ? materialState->compileFlagSlotsUsed : 0u};
    }
    void Revision(std::vector<uint64_t>& out) const {
        br::render::AppendPipelineRevision(out, m_pipeline);
        out.push_back(m_taken.slotsUsed);
    }

    MaterialUAVResetFrame Prepare(const MaterialUAVResetBindings&, const org::PassPrepareContext& preparation) const {
        const auto* update = preparation.preparationData->Get<UpdateContext>();
        const auto* render = preparation.preparationData->Get<RenderContext>();
        if (!update && !render) throw std::logic_error("MaterialUAVResetPass requires frame context");
        return m_taken;
    }

    static void Record(const MaterialUAVResetBindings& bindings, const MaterialUAVResetFrame& frame,
        org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[0] = frame.slotsUsed;
        br::render::DispatchCompute(context, bindings.pipeline, constants, (frame.slotsUsed + 63u) / 64u);
    }

private:
    br::PipelineState m_pipeline;
    MaterialUAVResetFrame m_taken{};
};
