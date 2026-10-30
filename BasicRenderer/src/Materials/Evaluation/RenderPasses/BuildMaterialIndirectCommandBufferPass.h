#pragma once
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Pipeline/PipelineState/CommandSignatureManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Materials/Publication/MaterialStateArtifacts.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

struct BuildMaterialIndirectCommandBufferBindings {
    org::PipelineState pipeline;
};
struct BuildMaterialIndirectCommandBufferFrame {
    uint32_t slotsUsed = 0;
};

// Runs after histogram + prefix sum + pixel list build.
// Fills a single indirect arguments buffer with one entry per material.
// Each entry encodes 4 root constants and a 2D dispatch sized to process all pixels of that material.
class BuildMaterialIndirectCommandBufferPass : public org::TypedRenderGraphPass<BuildMaterialIndirectCommandBufferPass,
    BuildMaterialIndirectCommandBufferFrame, BuildMaterialIndirectCommandBufferBindings> {
public:
    BuildMaterialIndirectCommandBufferPass() {
        // Build PSO for the args builder kernel
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/VisUtil.hlsl",
            L"BuildEvaluateIndirectArgsCS",
            {},
            "VisUtil_BuildEvaluateIndirectArgsPSO");
    }

    BuildMaterialIndirectCommandBufferBindings Declare(org::PassBuilder& b) {
        b.ShaderResource(
            "Builtin::VisUtil::MaterialPixelCountBuffer",
            "Builtin::VisUtil::MaterialOffsetBuffer");
        b.UnorderedAccess("Builtin::IndirectCommandBuffers::MaterialEvaluationCommandBuffer");
		b.ConstantBuffer(Builtin::PerFrameBuffer);
        b.PreferQueue(org::QueueKind::Compute);
        return {m_pso};
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
    org::PipelineState m_pso;
};
