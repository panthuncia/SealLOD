#pragma once
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/CommandSignatureManager.h"
#include "Render/RenderContext.h"
#include "Render/MaterialStateArtifacts.h"
#include "RenderPasses/PreparedComputeDispatch.h"

// Runs after histogram + prefix sum + pixel list build.
// Fills a single indirect arguments buffer with one entry per material.
// Each entry encodes 4 root constants and a 2D dispatch sized to process all pixels of that material.
class BuildMaterialIndirectCommandBufferPass : public org::TypedRenderGraphPass<BuildMaterialIndirectCommandBufferPass, org::EmptyPassFrameData, org::LegacyPassBindings, br::render::PreparedComputeDispatch> {
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

    void Declare(org::PassBuilder& b) {
        b.WithShaderResource(
            "Builtin::VisUtil::MaterialPixelCountBuffer",
            "Builtin::VisUtil::MaterialOffsetBuffer")
            .WithUnorderedAccess(
                "Builtin::IndirectCommandBuffers::MaterialEvaluationCommandBuffer");
		b.WithConstantBuffer(Builtin::PerFrameBuffer)
            .PreferQueue(org::QueueKind::Compute);
    }

    br::render::PreparedComputeDispatch BuildRecipe(const org::PassPrepareContext& preparation) const {
        const auto* update = preparation.preparationData->Get<UpdateContext>();
        const auto* render = preparation.preparationData->Get<RenderContext>();
        if (!update && !render) throw std::logic_error("BuildMaterialIndirectCommandBufferPass requires frame context");
        br::render::PreparedComputeDispatch data{};
        data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
        auto program = CaptureProgramBinding(preparation, m_pso);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);
        const auto& published = update ? update->publishedRendererState : render->publishedRendererState;
        const auto materialState = published
            ? published->materials.payload.Get<br::render::PublishedMaterialState>()
            : nullptr;
        data.constants[0] = materialState ? materialState->compileFlagSlotsUsed : 0u;
        data.groupsX = (data.constants[0] + 63u) / 64u;
        return data;
    }

    std::vector<uint64_t> RecipeRevision(const org::PassPrepareContext& preparation) const {
        const auto* update = preparation.preparationData->Get<UpdateContext>();
        const auto* render = preparation.preparationData->Get<RenderContext>();
        const auto& published = update ? update->publishedRendererState : render->publishedRendererState;
        const auto resolution = update ? update->renderResolution : render->renderResolution;
        return {reinterpret_cast<uintptr_t>(m_pso.PeekPayload()),
            published ? published->materials.revision : 0u, resolution.x, resolution.y};
    }
    org::EmptyPassFrameData PrepareInvocation(const br::render::PreparedComputeDispatch&,
        const org::PassPrepareContext&) const { return {}; }
    static void Record(const br::render::PreparedComputeDispatch& data, const org::EmptyPassFrameData&,
        org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatch(data, recording);
    }

private:
    org::PipelineState m_pso;
};
