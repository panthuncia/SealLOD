#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Render/Runtime/IDescriptorService.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

struct GTAODenoiseFrame {
    uint32_t groupsX = 0, groupsY = 0;
};
struct GTAODenoiseBindings {
    org::PipelineState pipeline;
    uint32_t samplerIndex = 0;
    org::DeclaredViewToken workingAO, workingEdges, outputAO;
};

class GTAODenoisePass : public org::TypedRenderGraphPass<GTAODenoisePass,
    GTAODenoiseFrame, GTAODenoiseBindings> {
public:
    GTAODenoisePass() {
        CreatePointClampSampler();
        CreateXeGTAOComputePSO();
    }

    GTAODenoiseBindings Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        builder.ConstantBuffer("Builtin::GTAO::ConstantsBuffer");
		builder.ConstantBuffer(Builtin::PerFrameBuffer);
        return {
            DenoiseLastPassPSO,
            m_samplerIndex,
            builder.ShaderResource(Builtin::GTAO::WorkingAOTerm1),
            builder.ShaderResource(Builtin::GTAO::WorkingEdges),
            builder.UnorderedAccess(Builtin::GTAO::OutputAOTerm) };
    }

    void Initialize() {
        // Removed redundant Register calls now covered by declared-resource auto descriptor registration
    }



    GTAODenoiseFrame Prepare(const GTAODenoiseBindings&,
        const org::PassPrepareContext& preparation) const {
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        return {(context->renderResolution.x + XE_GTAO_NUMTHREADS_X * 2u - 1u) / (XE_GTAO_NUMTHREADS_X * 2u),
            (context->renderResolution.y + XE_GTAO_NUMTHREADS_Y - 1u) / XE_GTAO_NUMTHREADS_Y};
    }

    static void Record(const GTAODenoiseBindings& bindings, const GTAODenoiseFrame& frame,
        org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[UintRootConstant0] = context.Index(bindings.workingAO);
        constants[UintRootConstant1] = context.Index(bindings.workingEdges);
        constants[UintRootConstant2] = bindings.samplerIndex;
        constants[UintRootConstant3] = context.Index(bindings.outputAO);
        br::render::DispatchCompute(context, bindings.pipeline, constants, frame.groupsX, frame.groupsY);
    }

    void ShutdownPass() {
        // Cleanup if necessary
    }

private:
    org::PipelineState DenoisePassPSO;
    org::PipelineState DenoiseLastPassPSO;
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
        m_samplerIndex = DescriptorService().CreateIndexedSampler(samplerDesc);
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
