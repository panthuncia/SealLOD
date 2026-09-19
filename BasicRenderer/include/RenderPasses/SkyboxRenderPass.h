#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"

struct SkyboxBindings {
    org::ResourceBindingToken depth, camera, environment, hdr, motion;
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
		builder->WithShaderResource(Builtin::Environment::CurrentCubemap);
		builder->WithConstantBuffer(Builtin::PerFrameBuffer);
        return {
            builder->BindShaderResource(Subresources(Builtin::PrimaryCamera::LinearDepthMap, org::Mip{ 0, 1 })),
            builder->BindShaderResource(Builtin::CameraBuffer),
            builder->BindShaderResource(Builtin::Environment::InfoBuffer),
            builder->BindUnorderedAccess(Builtin::Color::HDRColorTarget),
            builder->BindUnorderedAccess(Builtin::Surface::Motion) };
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
        data.constants[0] = preparation.ResolveView(bindings.depth, {org::BindlessViewKind::ShaderResource}).index;
        data.constants[1] = preparation.ResolveView(bindings.camera, {org::BindlessViewKind::ShaderResource}).index;
        data.constants[2] = preparation.ResolveView(bindings.environment, {org::BindlessViewKind::ShaderResource}).index;
        data.constants[3] = preparation.ResolveView(bindings.hdr, {org::BindlessViewKind::UnorderedAccess}).index;
		data.constants[4] = preparation.ResolveView(bindings.motion, {org::BindlessViewKind::UnorderedAccess}).index;
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
