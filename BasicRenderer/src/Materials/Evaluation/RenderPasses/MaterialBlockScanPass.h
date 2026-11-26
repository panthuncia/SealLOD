#pragma once
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include <BasicRenderer/Extensions/PipelineDescs.h>
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Materials/Publication/MaterialStateArtifacts.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "BasicRenderer/Extensions/PassRevision.h"

struct MaterialBlockScanBindings {
    br::PipelineState pipeline;
    uint32_t blockSize = 0;
};
struct MaterialBlockScanFrame {
    uint32_t slotsUsed = 0;
    bool operator==(const MaterialBlockScanFrame&) const = default;
};

// Pass A: per-block exclusive scan producing per-element local offsets and per-block totals.
// Dispatch dimension: x = numBlocks, where numBlocks = ceil(NumMaterials / blockSize).
class MaterialBlockScanPass : public org::TypedRenderGraphPass<MaterialBlockScanPass, MaterialBlockScanFrame, MaterialBlockScanBindings> {
public:
    MaterialBlockScanBindings Declare(org::PassBuilder& b) {
        b.ShaderResource("Builtin::VisUtil::MaterialPixelCountBuffer");
        b.UnorderedAccess("Builtin::VisUtil::MaterialOffsetBuffer", "Builtin::VisUtil::BlockSumsBuffer");
        b.PreferQueue(org::QueueKind::Compute);
        m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(L"shaders/materialPrefixSum.hlsl", L"BlockScanCS",
            "VisUtil_BlockScanPSO"));
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

    MaterialBlockScanFrame Prepare(const MaterialBlockScanBindings&, const org::PassPrepareContext& preparation) const {
        const auto* update = preparation.preparationData->Get<UpdateContext>();
        const auto* render = preparation.preparationData->Get<RenderContext>();
        if (!update && !render) throw std::logic_error("MaterialBlockScanPass requires frame context");
        const auto& published = update ? update->publishedRendererState : render->publishedRendererState;
        const auto materialState = published
            ? published->materials.payload.Get<br::render::PublishedMaterialState>()
            : nullptr;
        return {materialState ? materialState->compileFlagSlotsUsed : 0u};
    }

    static void Record(const MaterialBlockScanBindings& bindings, const MaterialBlockScanFrame& frame,
        org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[0] = frame.slotsUsed;
        br::render::DispatchCompute(context, bindings.pipeline, constants,
            (frame.slotsUsed + bindings.blockSize - 1u) / bindings.blockSize);
    }

private:
        // block size used by the shader (materialPrefixSum.hlsl). Keep in sync.
        uint32_t m_blockSize = 1024;
        br::PipelineState m_pipeline;
        uint32_t m_slotsUsed = 0;
};
