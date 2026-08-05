#pragma once

#include "RenderPasses/Base/ComputePass.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"

class DilateMotionVectorsPass : public ComputePass {
public:
    DilateMotionVectorsPass() {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/PostProcessing/dilateMotionVectors.hlsl",
            L"DilateMotionVectorsCS",
            {},
            "DilateMotionVectorsCS");
    }

    void DeclareResourceUsages(ComputePassBuilder* builder) override {
        builder->WithShaderResource(
            Builtin::GBuffer::MotionVectors,
            Builtin::PrimaryCamera::ProjectedDepthTexture)
            .WithUnorderedAccess(Builtin::GBuffer::DilatedMotionVectors);
    }

    void Setup() override {
        m_source = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::GBuffer::MotionVectors);
        m_depth = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::PrimaryCamera::ProjectedDepthTexture);
        m_destination = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::GBuffer::DilatedMotionVectors);
    }

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& commandList = executionContext.commandList;

        commandList.SetDescriptorHeaps(
            renderContext->textureDescriptorHeap.GetHandle(),
            renderContext->samplerDescriptorHeap.GetHandle());
        commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
        commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());

        uint32_t constants[NumMiscUintRootConstants] = {};
        constants[0] = m_source->GetSRVInfo(0).slot.index;
        constants[1] = m_depth->GetSRVInfo(0).slot.index;
        constants[2] = m_destination->GetUAVShaderVisibleInfo(0).slot.index;
        constants[3] = m_destination->GetWidth();
        constants[4] = m_destination->GetHeight();
        commandList.PushConstants(
            rhi::ShaderStage::Compute,
            0,
            MiscUintRootSignatureIndex,
            0,
            NumMiscUintRootConstants,
            constants);

        constexpr uint32_t groupSize = 8;
        commandList.Dispatch(
            (m_destination->GetWidth() + groupSize - 1) / groupSize,
            (m_destination->GetHeight() + groupSize - 1) / groupSize,
            1);
        return {};
    }

    void Cleanup() override {}

private:
    PipelineState m_pso;
    PixelBuffer* m_source = nullptr;
    PixelBuffer* m_depth = nullptr;
    PixelBuffer* m_destination = nullptr;
};
