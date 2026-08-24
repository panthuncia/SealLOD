#pragma once
#include "RenderPasses/Base/ComputePass.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/CommandSignatureManager.h"
#include "Render/RenderContext.h"
#include "Render/MaterialStateArtifacts.h"

// Runs after histogram + prefix sum + pixel list build.
// Fills a single indirect arguments buffer with one entry per material.
// Each entry encodes 4 root constants and a 2D dispatch sized to process all pixels of that material.
class BuildMaterialIndirectCommandBufferPass : public ComputePass {
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

    void DeclareResourceUsages(ComputePassBuilder* b) override {
        b->WithShaderResource(
            "Builtin::VisUtil::MaterialPixelCountBuffer",
            "Builtin::VisUtil::MaterialOffsetBuffer")
            .WithUnorderedAccess(
                "Builtin::IndirectCommandBuffers::MaterialEvaluationCommandBuffer");
		b->WithConstantBuffer(Builtin::PerFrameBuffer);
    }

    void Setup() override {
    }

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& ctx = *renderContext;
        auto& pm = PSOManager::GetInstance();
        auto& cl = executionContext.commandList;

        cl.SetDescriptorHeaps(ctx.textureDescriptorHeap.GetHandle(), ctx.samplerDescriptorHeap.GetHandle());
        cl.BindLayout(pm.GetComputeRootSignature().GetHandle());
        cl.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
        BindResourceDescriptorIndices(cl, m_pso.GetResourceDescriptorSlots());

        // Push constants:
        // UintRootConstant0 = NumMaterials
        unsigned int rc[NumMiscUintRootConstants] = {};
        const auto materialState = ctx.publishedRendererState
            ? ctx.publishedRendererState->materials.payload.Get<br::render::PublishedMaterialState>()
            : nullptr;
        rc[0] = materialState ? materialState->compileFlagSlotsUsed : 0u;
        cl.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, rc);

        // Dispatch: one thread per material, rounded up by 64
        const uint32_t kThreads = 64;
        const uint32_t groups = (rc[0] + kThreads - 1u) / kThreads;
        cl.Dispatch(groups, 1, 1);

        return {};
    }

    void Cleanup() override {}

private:
    PipelineState m_pso;
};
