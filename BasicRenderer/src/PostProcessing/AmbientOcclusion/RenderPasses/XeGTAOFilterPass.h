#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "ThirdParty/XeGTAO.h"
#include "Resources/PixelBuffer.h"
#include <Resources/Buffers/Buffer.h>
#include "Render/Runtime/IDescriptorService.h"
#include "Render/Runtime/UploadTypes.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

struct GTAOFilterBindings {
    org::ResourceBindingToken depth, workingDepths;
};

class GTAOFilterPass : public org::TypedRenderGraphPass<GTAOFilterPass,
    br::render::PreparedComputeDispatch, GTAOFilterBindings> {
public:
    GTAOFilterPass() {
        CreatePointClampSampler();
        CreateXeGTAOComputePSO();
    }

    void Initialize() {
        m_gtaoConstantsHandle = m_resourceRegistryView->RequestHandle("Builtin::GTAO::ConstantsBuffer");
    }

    void Update(const org::UpdateExecutionContext& updateExecutionContext) override {
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

        UploadBufferData(
            &gtaoInfo,
            sizeof(GTAOInfo),
            org::runtime::UploadTarget::FromHandle(m_gtaoConstantsHandle),
            0);
    }

    GTAOFilterBindings Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        builder.WithShaderResource(Builtin::Surface::NormalRoughness)
            .WithConstantBuffer("Builtin::GTAO::ConstantsBuffer");
		builder.WithConstantBuffer(Builtin::PerFrameBuffer);
        return {
            builder.BindShaderResource(Subresources(Builtin::PrimaryCamera::LinearDepthMap, org::Mip{ 0, 1 })),
            builder.BindUnorderedAccess(Builtin::GTAO::WorkingDepths) };
    }



    br::render::PreparedComputeDispatch Prepare(const GTAOFilterBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        auto payload = PrefilterDepths16x16PSO.GetPayload(); br::render::PreparedComputeDispatch data{};
        data.resourceHeap = context->textureDescriptorHeap.GetHandle(); data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
        data.layout = PSOManager::GetInstance().GetRootSignature().GetHandle(); auto program = preparation.CaptureProgramBinding(std::move(payload));
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);

        data.constants[UintRootConstant0] = m_samplerIndex;
        data.constants[UintRootConstant1] = preparation.ResolveView(bindings.depth,
            {org::BindlessViewKind::ShaderResource}).index;
        for (uint32_t mip = 0; mip < 5; ++mip)
            data.constants[UintRootConstant2 + mip] = preparation.ResolveView(bindings.workingDepths,
                {org::BindlessViewKind::UnorderedAccess, UINT32_MAX, mip}).index;
        data.groupsX = (context->renderResolution.x + 15u) / 16u; data.groupsY = (context->renderResolution.y + 15u) / 16u;
        return data;
    }

    static void Record(const GTAOFilterBindings&, const br::render::PreparedComputeDispatch& data,
        org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatch(data, recording);
    }

    void ShutdownPass() {
        // Cleanup if necessary
    }

private:

    org::PipelineState PrefilterDepths16x16PSO;
    uint32_t m_samplerIndex = 0;
    org::ResourceRegistry::RegistryHandle m_gtaoConstantsHandle;

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
        auto& psoManager = PSOManager::GetInstance();
        PrefilterDepths16x16PSO = psoManager.MakeComputePipeline(
            psoManager.GetRootSignature().GetHandle(),
            L"shaders/GTAO.hlsl",
            L"CSPrefilterDepths16x16"
		);
    }
};
