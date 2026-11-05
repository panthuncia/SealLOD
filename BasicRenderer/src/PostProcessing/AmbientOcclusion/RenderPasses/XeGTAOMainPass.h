#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Resources/PixelBuffer.h"
#include "ThirdParty/XeGTAO.h"
#include "Render/Runtime/IDescriptorService.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

struct GTAOMainFrame {
    uint32_t noiseIndex = 0;
    uint32_t groupsX = 0, groupsY = 0;
};
struct GTAOMainBindings {
    org::PipelineState pipeline;
    uint32_t samplerIndex = 0;
    org::DeclaredViewToken workingDepths, normals, workingAO, workingEdges;
};

class GTAOMainPass : public org::TypedRenderGraphPass<GTAOMainPass,
    GTAOMainFrame, GTAOMainBindings> {
public:
    GTAOMainPass() {
        CreatePointClampSampler();
    }

    GTAOMainBindings Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        builder.ShaderResource(Builtin::CameraBuffer);
        builder.ConstantBuffer("Builtin::GTAO::ConstantsBuffer");
		builder.ConstantBuffer(Builtin::PerFrameBuffer);
        return {
            builder.Pipeline(org::ComputePipelineDesc{
                .shader = {L"shaders/GTAO.hlsl", L"CSGTAOHigh"},
                .name = "GTAO High Quality",
                .layout = org::PipelineLayoutRef::StandardGraphics()}),
            m_samplerIndex,
            builder.ShaderResource(Builtin::GTAO::WorkingDepths),
            builder.ShaderResource(Builtin::Surface::NormalRoughness),
            builder.UnorderedAccess(Builtin::GTAO::WorkingAOTerm1),
            builder.UnorderedAccess(Builtin::GTAO::WorkingEdges) };
    }

    void Initialize() {
        // Removed redundant Register calls now covered by declared-resource auto descriptor registration
    }



    GTAOMainFrame Prepare(const GTAOMainBindings&,
        const org::PassPrepareContext& preparation) const {
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        GTAOMainFrame frame{};
        frame.noiseIndex = static_cast<uint32_t>(context->frameNumber % 64);
        frame.groupsX = (context->renderResolution.x + XE_GTAO_NUMTHREADS_X - 1u) / XE_GTAO_NUMTHREADS_X;
        frame.groupsY = (context->renderResolution.y + XE_GTAO_NUMTHREADS_Y - 1u) / XE_GTAO_NUMTHREADS_Y;
        return frame;
    }

    static void Record(const GTAOMainBindings& bindings, const GTAOMainFrame& frame,
        org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[UintRootConstant0] = frame.noiseIndex;
        constants[UintRootConstant1] = bindings.samplerIndex;
        constants[UintRootConstant2] = context.Index(bindings.workingDepths);
        constants[UintRootConstant3] = context.Index(bindings.normals);
        constants[UintRootConstant4] = context.Index(bindings.workingAO);
        constants[UintRootConstant5] = context.Index(bindings.workingEdges);
        br::render::DispatchCompute(context, bindings.pipeline, constants, frame.groupsX, frame.groupsY);
    }

    void ShutdownPass() {
        // Cleanup if necessary
    }

private:

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
};
