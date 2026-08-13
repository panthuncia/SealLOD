#pragma once

#include "RenderPasses/Base/ComputePass.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"

class SkyboxRenderPass : public ComputePass {
public:
    SkyboxRenderPass() {
        CreatePSO();
    }

    void DeclareResourceUsages(ComputePassBuilder* builder) override {
        builder->WithShaderResource(Builtin::Environment::CurrentCubemap, Builtin::Environment::InfoBuffer)
            .WithShaderResource(Subresources(Builtin::PrimaryCamera::LinearDepthMap, Mip{ 0, 1 }), Builtin::CameraBuffer)
			.WithUnorderedAccess(Builtin::Color::HDRColorTarget, Builtin::Surface::Motion);
		builder->WithConstantBuffer(Builtin::PerFrameBuffer);
    }

    void Setup() override {
        m_pHDRTarget = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::Color::HDRColorTarget);
    }

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& context = *renderContext;

        auto& commandList = executionContext.commandList;

		commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());

		commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
		commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());

        BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());

        uint32_t rootConstants[NumMiscUintRootConstants] = {};
        rootConstants[0] = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::PrimaryCamera::LinearDepthMap)->GetSRVInfo(0).slot.index;
        rootConstants[1] = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::CameraBuffer)->GetSRVInfo(0).slot.index;
        rootConstants[2] = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::Environment::InfoBuffer)->GetSRVInfo(0).slot.index;
        rootConstants[3] = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::Color::HDRColorTarget)->GetUAVShaderVisibleInfo(0).slot.index;
		rootConstants[4] = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::Surface::Motion)->GetUAVShaderVisibleInfo(0).slot.index;
        commandList.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, rootConstants);

        const uint32_t w = m_pHDRTarget->GetWidth();
        const uint32_t h = m_pHDRTarget->GetHeight();
        constexpr uint32_t groupSizeX = 8;
        constexpr uint32_t groupSizeY = 8;
        const uint32_t groupsX = (w + groupSizeX - 1) / groupSizeX;
        const uint32_t groupsY = (h + groupSizeY - 1) / groupSizeY;

        commandList.Dispatch(groupsX, groupsY, 1);

        return {};
    }

    void Cleanup() override {
        // Cleanup if necessary
    }

private:
    PipelineState m_pso;

    PixelBuffer* m_pHDRTarget = nullptr;
    void CreatePSO() {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/skybox.hlsl",
            L"SkyboxCSMain",
            {},
            "SkyboxComputePSO"
        );
    }
};
