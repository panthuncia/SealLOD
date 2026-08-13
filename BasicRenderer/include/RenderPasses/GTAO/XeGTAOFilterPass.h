#pragma once

#include "RenderPasses/Base/ComputePass.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "ShaderBuffers.h"
#include "ThirdParty/XeGTAO.h"
#include "Resources/PixelBuffer.h"
#include <Resources/Buffers/Buffer.h>
#include "Render/Runtime/DescriptorServiceAccess.h"
#include "Render/Runtime/UploadServiceAccess.h"

class GTAOFilterPass : public ComputePass {
public:
    GTAOFilterPass() {
        CreatePointClampSampler();
        CreateXeGTAOComputePSO();
    }

    void Setup() override {
        m_gtaoConstantsHandle = m_resourceRegistryView->RequestHandle("Builtin::GTAO::ConstantsBuffer");
    }

    void Update(const UpdateExecutionContext& updateExecutionContext) override {
        const auto* updateContext = updateExecutionContext.hostData->Get<UpdateContext>();
        if (updateContext == nullptr || !updateContext->hasPrimaryCamera) {
            return;
        }

        GTAOInfo gtaoInfo{};
        XeGTAO::GTAOSettings gtaoSettings;
        XeGTAO::GTAOUpdateConstants(
            gtaoInfo.g_GTAOConstants,
            updateContext->renderResolution.x,
            updateContext->renderResolution.y,
            gtaoSettings,
            false,
            static_cast<unsigned int>(updateContext->frameNumber),
            updateContext->primaryCamera);

        BUFFER_UPLOAD(
            &gtaoInfo,
            sizeof(GTAOInfo),
            rg::runtime::UploadTarget::FromHandle(m_gtaoConstantsHandle),
            0);
    }

    void DeclareResourceUsages(ComputePassBuilder* builder)override {
        builder->WithShaderResource(Builtin::Surface::NormalRoughness)
            .WithShaderResource(Subresources(Builtin::PrimaryCamera::LinearDepthMap, Mip{ 0, 1 }))
            .WithUnorderedAccess(Builtin::GTAO::WorkingDepths)
            .WithConstantBuffer("Builtin::GTAO::ConstantsBuffer");
		builder->WithConstantBuffer(Builtin::PerFrameBuffer);
    }

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& context = *renderContext;
        auto& psoManager = PSOManager::GetInstance();
        auto& commandList = executionContext.commandList;
        auto depthTexture = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::PrimaryCamera::LinearDepthMap);
        auto workingDepths = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::GTAO::WorkingDepths);

		commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());

		// Set the compute pipeline state
		commandList.BindLayout(psoManager.GetRootSignature().GetHandle());
		commandList.BindPipeline(PrefilterDepths16x16PSO.GetAPIPipelineState().GetHandle());

        BindResourceDescriptorIndices(commandList, PrefilterDepths16x16PSO.GetResourceDescriptorSlots());

        unsigned int passConstants[NumMiscUintRootConstants] = {};
        passConstants[UintRootConstant0] = m_samplerIndex;
        passConstants[UintRootConstant1] = depthTexture->GetSRVInfo(0).slot.index;
        passConstants[UintRootConstant2] = workingDepths->GetUAVShaderVisibleInfo(0).slot.index;
        passConstants[UintRootConstant3] = workingDepths->GetUAVShaderVisibleInfo(1).slot.index;
        passConstants[UintRootConstant4] = workingDepths->GetUAVShaderVisibleInfo(2).slot.index;
        passConstants[UintRootConstant5] = workingDepths->GetUAVShaderVisibleInfo(3).slot.index;
        passConstants[UintRootConstant6] = workingDepths->GetUAVShaderVisibleInfo(4).slot.index;

        commandList.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, passConstants);

        // Dispatch
        // note: in CSPrefilterDepths16x16 each is thread group handles a 16x16 block (with [numthreads(8, 8, 1)] and each logical thread handling a 2x2 block)
		unsigned int x = (context.renderResolution.x + 16 - 1) / 16;
		unsigned int y = (context.renderResolution.y + 16 - 1) / 16;
		commandList.Dispatch(x, y, 1);

        return {};
    }

    void Cleanup() override {
        // Cleanup if necessary
    }

private:

    PipelineState PrefilterDepths16x16PSO;
    uint32_t m_samplerIndex = 0;
    ResourceRegistry::RegistryHandle m_gtaoConstantsHandle;

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

    void CreateXeGTAOComputePSO()
    {
        auto& psoManager = PSOManager::GetInstance();
        PrefilterDepths16x16PSO = psoManager.MakeComputePipeline(
            psoManager.GetRootSignature().GetHandle(),
            L"shaders/GTAO.hlsl",
            L"CSPrefilterDepths16x16"
		);
    }
};
