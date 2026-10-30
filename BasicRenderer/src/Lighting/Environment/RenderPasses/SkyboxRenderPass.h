#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"

struct SkyboxFrame {
    uint32_t groupsX = 0, groupsY = 0;
};
struct SkyboxBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken depth, camera, environment, hdr, motion;
};

class SkyboxRenderPass : public org::TypedRenderGraphPass<SkyboxRenderPass,
    SkyboxFrame, SkyboxBindings> {
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
            m_pso,
            builder->ShaderResource(Subresources(Builtin::PrimaryCamera::LinearDepthMap, org::Mip{ 0, 1 })),
            builder->ShaderResource(Builtin::CameraBuffer),
            builder->ShaderResource(Builtin::Environment::InfoBuffer),
            builder->UnorderedAccess(Builtin::Color::HDRColorTarget),
            builder->UnorderedAccess(Builtin::Surface::Motion) };
    }

    SkyboxFrame Prepare(const SkyboxBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        const auto& target = preparation.Describe(bindings.hdr);
        return {(target.texture.width + 7u) / 8u, (target.texture.height + 7u) / 8u};
    }

    static void Record(const SkyboxBindings& bindings, const SkyboxFrame& frame,
        org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[0] = context.Index(bindings.depth);
        constants[1] = context.Index(bindings.camera);
        constants[2] = context.Index(bindings.environment);
        constants[3] = context.Index(bindings.hdr);
        constants[4] = context.Index(bindings.motion);
        br::render::DispatchCompute(context, bindings.pipeline, constants, frame.groupsX, frame.groupsY);
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
