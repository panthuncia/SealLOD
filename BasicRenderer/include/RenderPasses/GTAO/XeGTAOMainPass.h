#pragma once

#include "RenderPasses/Base/ComputePass.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Resources/PixelBuffer.h"
#include "ThirdParty/XeGTAO.h"
#include "Render/Runtime/DescriptorServiceAccess.h"

class GTAOMainPass : public ComputePass {
public:
    GTAOMainPass() {
        CreatePointClampSampler();
        CreateXeGTAOComputePSO();
    }

    void DeclareResourceUsages(ComputePassBuilder* builder) override {
        builder->WithShaderResource(Builtin::Surface::NormalRoughness, Builtin::GTAO::WorkingDepths, Builtin::CameraBuffer)
            .WithUnorderedAccess(Builtin::GTAO::WorkingEdges, Builtin::GTAO::WorkingAOTerm1)
            .WithConstantBuffer("Builtin::GTAO::ConstantsBuffer");
		builder->WithConstantBuffer(Builtin::PerFrameBuffer);
    }

    void Setup() override {
        // Removed redundant Register calls now covered by declared-resource auto descriptor registration
    }

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& context = *renderContext;
        frameIndex++;
        auto& psoManager = PSOManager::GetInstance();
        auto& commandList = executionContext.commandList;
        auto workingDepths = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::GTAO::WorkingDepths);
        auto workingAOTerm = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::GTAO::WorkingAOTerm1);
        auto workingEdges = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::GTAO::WorkingEdges);
        auto normals = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::Surface::NormalRoughness);

		commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());

		commandList.BindLayout(psoManager.GetRootSignature().GetHandle());
		commandList.BindPipeline(GTAOHighPSO.GetAPIPipelineState().GetHandle());

        BindResourceDescriptorIndices(commandList, GTAOHighPSO.GetResourceDescriptorSlots());

        unsigned int passConstants[NumMiscUintRootConstants] = {};
		passConstants[UintRootConstant0] = frameIndex % 64; // For spatiotemporal denoising
        passConstants[UintRootConstant1] = m_samplerIndex;
        passConstants[UintRootConstant2] = workingDepths->GetSRVInfo(0).slot.index;
        passConstants[UintRootConstant3] = normals->GetSRVInfo(0).slot.index;
        passConstants[UintRootConstant4] = workingAOTerm->GetUAVShaderVisibleInfo(0).slot.index;
        passConstants[UintRootConstant5] = workingEdges->GetUAVShaderVisibleInfo(0).slot.index;

		commandList.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, passConstants);

        commandList.Dispatch((context.renderResolution.x + XE_GTAO_NUMTHREADS_X - 1) / XE_GTAO_NUMTHREADS_X, (context.renderResolution.y + XE_GTAO_NUMTHREADS_Y - 1) / XE_GTAO_NUMTHREADS_Y, 1);
        return {};
    }

    void Cleanup() override {
        // Cleanup if necessary
    }

private:

    PipelineState PrefilterDepths16x16PSO;
    PipelineState GTAOLowPSO;
    PipelineState GTAOMediumPSO;
    PipelineState GTAOHighPSO;
    PipelineState GTAOUltraPSO;
    PipelineState DenoisePassPSO;
    PipelineState DenoiseLastPassPSO;
    PipelineState GenerateNormalsPSO;

    uint64_t frameIndex = 0;
    uint32_t m_samplerIndex = 0;

    void CreatePointClampSampler()
    {
        rhi::SamplerDesc samplerDesc;
        samplerDesc.minFilter = rhi::Filter::Nearest;
        samplerDesc.magFilter = rhi::Filter::Nearest;
        samplerDesc.mipFilter = rhi::MipFilter::Nearest;
        samplerDesc.addressU = rhi::AddressMode::Clamp;
        samplerDesc.addressV = rhi::AddressMode::Clamp;
        samplerDesc.addressW = rhi::AddressMode::Clamp;
        samplerDesc.mipLodBias = 0.0f;
        samplerDesc.maxAnisotropy = 1;
        samplerDesc.compareEnable = false;
        samplerDesc.borderPreset = rhi::BorderPreset::TransparentBlack;
        samplerDesc.minLod = 0.0f;
        samplerDesc.maxLod = 0.0f;
        m_samplerIndex = rg::runtime::CreateIndexedSamplerFromActiveDescriptorService(samplerDesc);
    }

    void CreateXeGTAOComputePSO() {

		GTAOUltraPSO = PSOManager::GetInstance().MakeComputePipeline(
			PSOManager::GetInstance().GetRootSignature().GetHandle(),
			L"shaders/GTAO.hlsl",
			L"CSGTAOUltra",
			{},
			"GTAO Ultra Quality");

		GTAOHighPSO = PSOManager::GetInstance().MakeComputePipeline(
			PSOManager::GetInstance().GetRootSignature().GetHandle(),
			L"shaders/GTAO.hlsl",
			L"CSGTAOHigh",
			{},
			"GTAO High Quality");

		GTAOMediumPSO = PSOManager::GetInstance().MakeComputePipeline(
			PSOManager::GetInstance().GetRootSignature().GetHandle(),
			L"shaders/GTAO.hlsl",
			L"CSGTAOMedium",
			{},
			"GTAO Medium Quality");

		GTAOLowPSO = PSOManager::GetInstance().MakeComputePipeline(
			PSOManager::GetInstance().GetRootSignature().GetHandle(),
			L"shaders/GTAO.hlsl",
			L"CSGTAOLow",
			{},
			"GTAO Low Quality");
		
    }
};
