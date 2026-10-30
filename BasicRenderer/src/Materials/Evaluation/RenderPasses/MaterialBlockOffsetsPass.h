#pragma once
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Materials/Publication/MaterialStateArtifacts.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

struct MaterialBlockOffsetsBindings {
    org::PipelineState pipeline;
    uint32_t blockSize = 0;
};
struct MaterialBlockOffsetsFrame {
    uint32_t slotsUsed = 0;
};

// Pass B: scan block sums, add block prefixes to per-element offsets, and write total pixel count.
// Dispatch dimension: x = 1 (single group), unless we implement recursive scan for very large numBlocks.
class MaterialBlockOffsetsPass : public org::TypedRenderGraphPass<MaterialBlockOffsetsPass, MaterialBlockOffsetsFrame, MaterialBlockOffsetsBindings> {
public:
    MaterialBlockOffsetsPass() {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/materialPrefixSum.hlsl",
            L"BlockOffsetsCS",
            {},
            "VisUtil_BlockOffsetsPSO");
    }

    MaterialBlockOffsetsBindings Declare(org::PassBuilder& b) {
        b.ShaderResource("Builtin::VisUtil::MaterialPixelCountBuffer", "Builtin::VisUtil::BlockSumsBuffer");
        b.UnorderedAccess("Builtin::VisUtil::MaterialOffsetBuffer", "Builtin::VisUtil::ScannedBlockSumsBuffer",
            "Builtin::VisUtil::TotalPixelCountBuffer");
        b.PreferQueue(org::QueueKind::Compute);
        return {m_pso, m_blockSize};
    }

    MaterialBlockOffsetsFrame Prepare(const MaterialBlockOffsetsBindings&, const org::PassPrepareContext& preparation) const {
        const auto* update = preparation.preparationData->Get<UpdateContext>();
        const auto* render = preparation.preparationData->Get<RenderContext>();
        if (!update && !render) throw std::logic_error("MaterialBlockOffsetsPass requires frame context");
        const auto& published = update ? update->publishedRendererState : render->publishedRendererState;
        const auto materialState = published
            ? published->materials.payload.Get<br::render::PublishedMaterialState>()
            : nullptr;
        return {materialState ? materialState->compileFlagSlotsUsed : 0u};
    }

    static void Record(const MaterialBlockOffsetsBindings& bindings, const MaterialBlockOffsetsFrame& frame,
        org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[0] = frame.slotsUsed;
        constants[1] = (frame.slotsUsed + bindings.blockSize - 1u) / bindings.blockSize;
        br::render::DispatchCompute(context, bindings.pipeline, constants, 1);
    }

private:
    org::PipelineState m_pso;
    uint32_t m_blockSize = 1024;
};
