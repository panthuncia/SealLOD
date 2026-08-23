#pragma once

#include "RenderPasses/Base/ComputePass.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/DescriptorServiceAccess.h"

class GTAODenoisePass : public ComputePass {
public:
    GTAODenoisePass() {
        CreatePointClampSampler();
        CreateXeGTAOComputePSO();
    }

    void DeclareResourceUsages(ComputePassBuilder* builder) override {
        builder->WithShaderResource(Builtin::GTAO::WorkingEdges, Builtin::GTAO::WorkingAOTerm1)
            .WithUnorderedAccess(Builtin::GTAO::OutputAOTerm)
            .WithConstantBuffer("Builtin::GTAO::ConstantsBuffer");
		builder->WithConstantBuffer(Builtin::PerFrameBuffer);
    }

    void Setup() override {
        // Removed redundant Register calls now covered by declared-resource auto descriptor registration
    }

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& context = *renderContext;

        auto& psoManager = PSOManager::GetInstance();
        auto& commandList = executionContext.commandList;
        auto workingAOTerm = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::GTAO::WorkingAOTerm1);
        auto workingEdges = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::GTAO::WorkingEdges);
        auto outputAO = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::GTAO::OutputAOTerm);

		commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());

		// Set the root signature
		commandList.BindLayout(psoManager.GetRootSignature().GetHandle());
		commandList.BindPipeline(DenoiseLastPassPSO.GetAPIPipelineState().GetHandle());

		BindResourceDescriptorIndices(commandList, DenoiseLastPassPSO.GetResourceDescriptorSlots());

        unsigned int gtaoConstants[NumMiscUintRootConstants] = {};
        gtaoConstants[UintRootConstant0] = workingAOTerm->GetSRVInfo(0).slot.index;
        gtaoConstants[UintRootConstant1] = workingEdges->GetSRVInfo(0).slot.index;
        gtaoConstants[UintRootConstant2] = m_samplerIndex;
        gtaoConstants[UintRootConstant3] = outputAO->GetUAVShaderVisibleInfo(0).slot.index;
            
		commandList.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, gtaoConstants);

        commandList.Dispatch((context.renderResolution.x + (XE_GTAO_NUMTHREADS_X*2)-1) / (XE_GTAO_NUMTHREADS_X*2), (context.renderResolution.y + XE_GTAO_NUMTHREADS_Y-1) / XE_GTAO_NUMTHREADS_Y, 1 );
    
        return {};
    }

    void Cleanup() override {
        // Cleanup if necessary
    }

private:
    PipelineState DenoisePassPSO;
    PipelineState DenoiseLastPassPSO;
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
        m_samplerIndex = org::runtime::CreateIndexedSamplerFromActiveDescriptorService(samplerDesc);
    }

    void CreateXeGTAOComputePSO()
    {
        auto device = DeviceManager::GetInstance().GetDevice();

		auto& psoManager = PSOManager::GetInstance();
        DenoisePassPSO = psoManager.MakeComputePipeline(
            psoManager.GetRootSignature().GetHandle(),
            L"shaders/GTAO.hlsl",
            L"CSDenoisePass",
            {},
			"GTAO Denoise Pass");

		DenoiseLastPassPSO = psoManager.MakeComputePipeline(
			psoManager.GetRootSignature().GetHandle(),
			L"shaders/GTAO.hlsl",
			L"CSDenoiseLastPass",
			{},
			"GTAO Denoise Last Pass");
    }
};
