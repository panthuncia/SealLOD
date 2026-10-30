#pragma once
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Materials/Publication/MaterialStateArtifacts.h"
#include "BasicRenderer/Assets/TechniqueDescriptor.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

struct MaterialUAVResetBindings {
    org::PipelineState pipeline;
};
struct MaterialUAVResetFrame {
    uint32_t slotsUsed = 0;
};

class MaterialUAVResetPass : public org::TypedRenderGraphPass<MaterialUAVResetPass, MaterialUAVResetFrame, MaterialUAVResetBindings> {
public:
    MaterialUAVResetPass() {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/VisUtil.hlsl",
            L"ClearMaterialCountersCS",
            {},
            "ClearMaterialCountersPSO");
    }

    MaterialUAVResetBindings Declare(org::PassBuilder& builder) {
        builder.UnorderedAccess("Builtin::VisUtil::MaterialPixelCountBuffer",
            "Builtin::VisUtil::MaterialWriteCursorBuffer");
		builder.ConstantBuffer(Builtin::PerFrameBuffer);
        builder.PreferQueue(org::QueueKind::Compute);
        return {m_pso};
    }

    MaterialUAVResetFrame Prepare(const MaterialUAVResetBindings&, const org::PassPrepareContext& preparation) const {
        const auto* update = preparation.preparationData->Get<UpdateContext>();
        const auto* render = preparation.preparationData->Get<RenderContext>();
        if (!update && !render) throw std::logic_error("MaterialUAVResetPass requires frame context");
        const auto& published = update ? update->publishedRendererState : render->publishedRendererState;
        const auto materialState = published
            ? published->materials.payload.Get<br::render::PublishedMaterialState>()
            : nullptr;
        return {materialState ? materialState->compileFlagSlotsUsed : 0u};
    }

    static void Record(const MaterialUAVResetBindings& bindings, const MaterialUAVResetFrame& frame,
        org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[0] = frame.slotsUsed;
        br::render::DispatchCompute(context, bindings.pipeline, constants, (frame.slotsUsed + 63u) / 64u);
    }

private:
    org::PipelineState m_pso;
};
