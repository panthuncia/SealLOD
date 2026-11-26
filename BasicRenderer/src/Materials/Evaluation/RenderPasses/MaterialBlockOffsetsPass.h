#pragma once
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include <BasicRenderer/Extensions/PipelineDescs.h>
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Materials/Publication/MaterialStateArtifacts.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "BasicRenderer/Extensions/PassRevision.h"

struct MaterialBlockOffsetsBindings {
    br::PipelineState pipeline;
    uint32_t blockSize = 0;
};
struct MaterialBlockOffsetsFrame {
    uint32_t slotsUsed = 0;
    bool operator==(const MaterialBlockOffsetsFrame&) const = default;
};

// Pass B: scan block sums, add block prefixes to per-element offsets, and write total pixel count.
// Dispatch dimension: x = 1 (single group), unless we implement recursive scan for very large numBlocks.
class MaterialBlockOffsetsPass : public org::TypedRenderGraphPass<MaterialBlockOffsetsPass, MaterialBlockOffsetsFrame, MaterialBlockOffsetsBindings> {
public:
    MaterialBlockOffsetsBindings Declare(org::PassBuilder& b) {
        b.ShaderResource("Builtin::VisUtil::MaterialPixelCountBuffer", "Builtin::VisUtil::BlockSumsBuffer");
        b.UnorderedAccess("Builtin::VisUtil::MaterialOffsetBuffer", "Builtin::VisUtil::ScannedBlockSumsBuffer",
            "Builtin::VisUtil::TotalPixelCountBuffer");
        b.PreferQueue(org::QueueKind::Compute);
        m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(L"shaders/materialPrefixSum.hlsl", L"BlockOffsetsCS",
            "VisUtil_BlockOffsetsPSO"));
        return {m_pipeline, m_blockSize};
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
    uint32_t m_blockSize = 1024;
    br::PipelineState m_pipeline;
    uint32_t m_slotsUsed = 0;
};
