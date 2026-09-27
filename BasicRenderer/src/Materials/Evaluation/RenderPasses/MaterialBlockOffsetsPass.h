#pragma once
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Materials/Publication/MaterialStateArtifacts.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

// Pass B: scan block sums, add block prefixes to per-element offsets, and write total pixel count.
// Dispatch dimension: x = 1 (single group), unless we implement recursive scan for very large numBlocks.
class MaterialBlockOffsetsPass : public org::TypedRenderGraphPass<MaterialBlockOffsetsPass, org::EmptyPassFrameData, org::LegacyPassBindings, br::render::PreparedComputeDispatch> {
public:
    MaterialBlockOffsetsPass() {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/materialPrefixSum.hlsl",
            L"BlockOffsetsCS",
            {},
            "VisUtil_BlockOffsetsPSO");
    }

    void Declare(org::PassBuilder& b) {
        b.WithShaderResource("Builtin::VisUtil::MaterialPixelCountBuffer",
                              "Builtin::VisUtil::BlockSumsBuffer")
         .WithUnorderedAccess("Builtin::VisUtil::MaterialOffsetBuffer",
                              "Builtin::VisUtil::ScannedBlockSumsBuffer",
                              "Builtin::VisUtil::TotalPixelCountBuffer")
         .PreferQueue(org::QueueKind::Compute);
    }

    br::render::PreparedComputeDispatch BuildRecipe(const org::PassPrepareContext& preparation) const {
        const auto* update = preparation.preparationData->Get<UpdateContext>();
        const auto* render = preparation.preparationData->Get<RenderContext>();
        if (!update && !render) throw std::logic_error("MaterialBlockOffsetsPass requires frame context");
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
        data.constants[1] = (data.constants[0] + m_blockSize - 1u) / m_blockSize;
        data.groupsX = 1;
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
    uint32_t m_blockSize = 1024;
};
