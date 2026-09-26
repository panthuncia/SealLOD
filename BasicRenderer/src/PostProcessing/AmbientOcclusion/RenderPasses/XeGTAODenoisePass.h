#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Render/Runtime/IDescriptorService.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

struct GTAODenoiseBindings {
    org::ResourceBindingToken workingAO, workingEdges, outputAO;
};

class GTAODenoisePass : public org::TypedRenderGraphPass<GTAODenoisePass,
    br::render::PreparedComputeDispatch, GTAODenoiseBindings> {
public:
    GTAODenoisePass() {
        CreatePointClampSampler();
        CreateXeGTAOComputePSO();
    }

    GTAODenoiseBindings Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        builder.WithConstantBuffer("Builtin::GTAO::ConstantsBuffer");
		builder.WithConstantBuffer(Builtin::PerFrameBuffer);
        return {
            builder.BindShaderResource(Builtin::GTAO::WorkingAOTerm1),
            builder.BindShaderResource(Builtin::GTAO::WorkingEdges),
            builder.BindUnorderedAccess(Builtin::GTAO::OutputAOTerm) };
    }

    void Initialize() {
        // Removed redundant Register calls now covered by declared-resource auto descriptor registration
    }



    br::render::PreparedComputeDispatch Prepare(const GTAODenoiseBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        auto payload = DenoiseLastPassPSO.GetPayload();
        br::render::PreparedComputeDispatch data{};
        data.resourceHeap = context->textureDescriptorHeap.GetHandle();
        data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
        data.layout = PSOManager::GetInstance().GetRootSignature().GetHandle();
        auto program = preparation.CaptureProgramBinding(std::move(payload));
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);


        data.constants[UintRootConstant0] = preparation.ResolveView(bindings.workingAO,
            {org::BindlessViewKind::ShaderResource}).index;
        data.constants[UintRootConstant1] = preparation.ResolveView(bindings.workingEdges,
            {org::BindlessViewKind::ShaderResource}).index;
        data.constants[UintRootConstant2] = m_samplerIndex;
        data.constants[UintRootConstant3] = preparation.ResolveView(bindings.outputAO,
            {org::BindlessViewKind::UnorderedAccess}).index;
        data.groupsX = (context->renderResolution.x + XE_GTAO_NUMTHREADS_X * 2u - 1u) / (XE_GTAO_NUMTHREADS_X * 2u);
        data.groupsY = (context->renderResolution.y + XE_GTAO_NUMTHREADS_Y - 1u) / XE_GTAO_NUMTHREADS_Y;
        return data;
    }

    static void Record(const GTAODenoiseBindings&, const br::render::PreparedComputeDispatch& data,
        org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatch(data, recording);
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
