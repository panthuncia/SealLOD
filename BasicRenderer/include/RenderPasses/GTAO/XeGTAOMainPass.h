#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Resources/PixelBuffer.h"
#include "ThirdParty/XeGTAO.h"
#include "Render/Runtime/IDescriptorService.h"
#include "RenderPasses/PreparedComputeDispatch.h"

struct GTAOMainBindings {
    org::ResourceBindingToken workingDepths, normals, workingAO, workingEdges;
};

class GTAOMainPass : public org::TypedRenderGraphPass<GTAOMainPass,
    br::render::PreparedComputeDispatch, GTAOMainBindings> {
public:
    GTAOMainPass() {
        CreatePointClampSampler();
        CreateXeGTAOComputePSO();
    }

    GTAOMainBindings Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        builder.WithShaderResource(Builtin::CameraBuffer)
            .WithConstantBuffer("Builtin::GTAO::ConstantsBuffer");
		builder.WithConstantBuffer(Builtin::PerFrameBuffer);
        return {
            builder.BindShaderResource(Builtin::GTAO::WorkingDepths),
            builder.BindShaderResource(Builtin::Surface::NormalRoughness),
            builder.BindUnorderedAccess(Builtin::GTAO::WorkingAOTerm1),
            builder.BindUnorderedAccess(Builtin::GTAO::WorkingEdges) };
    }

    void Initialize() {
        // Removed redundant Register calls now covered by declared-resource auto descriptor registration
    }



    br::render::PreparedComputeDispatch Prepare(const GTAOMainBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        auto payload = GTAOHighPSO.GetPayload(); br::render::PreparedComputeDispatch data{};
        data.resourceHeap = context->textureDescriptorHeap.GetHandle(); data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
        data.layout = PSOManager::GetInstance().GetRootSignature().GetHandle(); auto program = preparation.CaptureProgramBinding(std::move(payload));
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);

        data.constants[UintRootConstant0] = static_cast<uint32_t>(context->frameNumber % 64);
        data.constants[UintRootConstant1] = m_samplerIndex;
        data.constants[UintRootConstant2] = preparation.ResolveView(bindings.workingDepths,
            {org::BindlessViewKind::ShaderResource}).index;
        data.constants[UintRootConstant3] = preparation.ResolveView(bindings.normals,
            {org::BindlessViewKind::ShaderResource}).index;
        data.constants[UintRootConstant4] = preparation.ResolveView(bindings.workingAO,
            {org::BindlessViewKind::UnorderedAccess}).index;
        data.constants[UintRootConstant5] = preparation.ResolveView(bindings.workingEdges,
            {org::BindlessViewKind::UnorderedAccess}).index;
        data.groupsX = (context->renderResolution.x + XE_GTAO_NUMTHREADS_X - 1u) / XE_GTAO_NUMTHREADS_X;
        data.groupsY = (context->renderResolution.y + XE_GTAO_NUMTHREADS_Y - 1u) / XE_GTAO_NUMTHREADS_Y;
        return data;
    }

    static void Record(const GTAOMainBindings&, const br::render::PreparedComputeDispatch& data,
        org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatch(data, recording);
    }

    void ShutdownPass() {
        // Cleanup if necessary
    }

private:

    org::PipelineState PrefilterDepths16x16PSO;
    org::PipelineState GTAOLowPSO;
    org::PipelineState GTAOMediumPSO;
    org::PipelineState GTAOHighPSO;
    org::PipelineState GTAOUltraPSO;
    org::PipelineState DenoisePassPSO;
    org::PipelineState DenoiseLastPassPSO;
    org::PipelineState GenerateNormalsPSO;

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
