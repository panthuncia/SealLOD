#pragma once
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Render/MaterialStateArtifacts.h"
#include "RenderPasses/PreparedComputeDispatch.h"

// Pass A: per-block exclusive scan producing per-element local offsets and per-block totals.
// Dispatch dimension: x = numBlocks, where numBlocks = ceil(NumMaterials / blockSize).
class MaterialBlockScanPass : public org::TypedRenderGraphPass<MaterialBlockScanPass, org::EmptyPassFrameData, org::LegacyPassBindings, br::render::PreparedComputeDispatch> {
public:
    explicit MaterialBlockScanPass() {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/materialPrefixSum.hlsl",
            L"BlockScanCS",
            {},
            "VisUtil_BlockScanPSO");
    }

    void Declare(org::PassBuilder& b) {
        b.WithShaderResource("Builtin::VisUtil::MaterialPixelCountBuffer")
         .WithUnorderedAccess("Builtin::VisUtil::MaterialOffsetBuffer",
                              "Builtin::VisUtil::BlockSumsBuffer")
         .PreferQueue(org::QueueKind::Compute);
    }

    br::render::PreparedComputeDispatch BuildRecipe(const org::PassPrepareContext& preparation) const {
        const auto* update = preparation.preparationData->Get<UpdateContext>();
        const auto* render = preparation.preparationData->Get<RenderContext>();
        if (!update && !render) throw std::logic_error("MaterialBlockScanPass requires frame context");
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
        data.groupsX = (data.constants[0] + m_blockSize - 1u) / m_blockSize;
        return data;
    }

    std::vector<uint64_t> RecipeRevision(const org::PassPrepareContext& preparation) const {
        const auto* update = preparation.preparationData->Get<UpdateContext>();
        const auto* render = preparation.preparationData->Get<RenderContext>();
        const auto& published = update ? update->publishedRendererState : render->publishedRendererState;
        const auto resolution = update ? update->renderResolution : render->renderResolution;
        return {reinterpret_cast<uintptr_t>(m_pso.GetPayload().get()),
            published ? published->materials.revision : 0u, resolution.x, resolution.y};
    }
    org::EmptyPassFrameData PrepareInvocation(const br::render::PreparedComputeDispatch&,
        const org::PassPrepareContext&) const { return {}; }
    static void Record(const br::render::PreparedComputeDispatch& data, const org::EmptyPassFrameData&,
        org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatch(data, recording);
    }

private:
        PipelineState m_pso;
        // block size used by the shader (materialPrefixSum.hlsl). Keep in sync.
        uint32_t m_blockSize = 1024;
};
