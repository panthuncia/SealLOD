#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"

struct SkyboxBindings {
    org::DeclaredViewToken depth, camera, environment, hdr, motion;
};

class SkyboxRenderPass : public org::TypedRenderGraphPass<SkyboxRenderPass,
    br::render::PreparedComputeDispatch, SkyboxBindings> {
public:
    SkyboxRenderPass() {
        CreatePSO();
    }

    SkyboxBindings Declare(org::PassBuilder& declaration) {
        declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* builder = &declaration;
		builder->ShaderResource(Builtin::Environment::CurrentCubemap);
		builder->ConstantBuffer(Builtin::PerFrameBuffer);
        return {
            builder->ShaderResource(Subresources(Builtin::PrimaryCamera::LinearDepthMap, org::Mip{ 0, 1 })),
            builder->ShaderResource(Builtin::CameraBuffer),
            builder->ShaderResource(Builtin::Environment::InfoBuffer),
            builder->UnorderedAccess(Builtin::Color::HDRColorTarget),
            builder->UnorderedAccess(Builtin::Surface::Motion) };
    }

    br::render::PreparedComputeDispatch Prepare(const SkyboxBindings& bindings,
        const org::PassPrepareContext& preparation) const {

        const auto& context = *preparation.preparationData->Get<UpdateContext>();
        br::render::PreparedComputeDispatch data{};
        data.resourceHeap = context.textureDescriptorHeap.GetHandle();
        data.samplerHeap = context.samplerDescriptorHeap.GetHandle();
        auto program = preparation.CaptureProgramBinding(m_pso);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);
        data.constants[0] = preparation.Resolve(bindings.depth).index;
        data.constants[1] = preparation.Resolve(bindings.camera).index;
        data.constants[2] = preparation.Resolve(bindings.environment).index;
        data.constants[3] = preparation.Resolve(bindings.hdr).index;
		data.constants[4] = preparation.Resolve(bindings.motion).index;
        const auto& target = preparation.Describe(bindings.hdr);
        data.groupsX = (target.texture.width + 7u) / 8u;
        data.groupsY = (target.texture.height + 7u) / 8u;
        return data;
    }

    static void Record(const SkyboxBindings&, const br::render::PreparedComputeDispatch& data,
        org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatch(data, recording);
    }

private:
    org::PipelineState m_pso;

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
