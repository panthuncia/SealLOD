#pragma once
#include "RenderPasses/Base/ComputePass.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Render/MaterialStateArtifacts.h"

// Pass B: scan block sums, add block prefixes to per-element offsets, and write total pixel count.
// Dispatch dimension: x = 1 (single group), unless we implement recursive scan for very large numBlocks.
class MaterialBlockOffsetsPass : public ComputePass {
public:
    MaterialBlockOffsetsPass() {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/materialPrefixSum.hlsl",
            L"BlockOffsetsCS",
            {},
            "VisUtil_BlockOffsetsPSO");
    }

    void DeclareResourceUsages(ComputePassBuilder* b) override {
        b->WithShaderResource("Builtin::VisUtil::MaterialPixelCountBuffer",
                              "Builtin::VisUtil::BlockSumsBuffer")
         .WithUnorderedAccess("Builtin::VisUtil::MaterialOffsetBuffer",
                              "Builtin::VisUtil::ScannedBlockSumsBuffer",
                              "Builtin::VisUtil::TotalPixelCountBuffer");
    }

    void Setup() override {
    }

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& ctx = *renderContext;
        auto& pm = PSOManager::GetInstance();
        auto& cl = executionContext.commandList;

		const auto materialState = ctx.publishedRendererState
			? ctx.publishedRendererState->materials.payload.Get<br::render::PublishedMaterialState>()
			: nullptr;
		const auto numMaterials = materialState ? materialState->compileFlagSlotsUsed : 0u;
        // numBlocks must match prior pass
        const uint32_t numBlocks = (numMaterials + m_blockSize - 1) / m_blockSize;

        cl.SetDescriptorHeaps(ctx.textureDescriptorHeap.GetHandle(), ctx.samplerDescriptorHeap.GetHandle());
        cl.BindLayout(pm.GetComputeRootSignature().GetHandle());
        cl.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
        BindResourceDescriptorIndices(cl, m_pso.GetResourceDescriptorSlots());

        // Root constants:
        // UintRootConstant0 = NumMaterials
        // UintRootConstant1 = NumBlocks
        unsigned int rc[NumMiscUintRootConstants] = {};
        rc[0] = numMaterials;
        rc[1] = numBlocks;
        cl.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, rc);

        // Single-group: the shader loops across blocks and elements.
        cl.Dispatch(1, 1, 1);
        return {};
    }

    void Cleanup() override {}

private:
    PipelineState m_pso;
    uint32_t m_blockSize = 1024;
};
