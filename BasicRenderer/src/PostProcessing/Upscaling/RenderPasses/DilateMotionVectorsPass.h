#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

struct DilateMotionVectorsBindings {
    org::ResourceBindingToken source, depth, destination;
};

class DilateMotionVectorsPass : public org::TypedRenderGraphPass<DilateMotionVectorsPass,
    br::render::PreparedComputeDispatch, DilateMotionVectorsBindings> {
public:
    DilateMotionVectorsPass() {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/PostProcessing/dilateMotionVectors.hlsl",
            L"DilateMotionVectorsCS",
            {},
            "DilateMotionVectorsCS");
    }

    DilateMotionVectorsBindings Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        return {
            builder.BindShaderResource(Builtin::Surface::Motion),
            builder.BindShaderResource(Builtin::PrimaryCamera::ProjectedDepthTexture),
            builder.BindUnorderedAccess(Builtin::Surface::DilatedMotion) };
    }



    br::render::PreparedComputeDispatch Prepare(const DilateMotionVectorsBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        auto payload = m_pso.GetPayload(); br::render::PreparedComputeDispatch data{};
        data.resourceHeap = context->textureDescriptorHeap.GetHandle(); data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
        data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle(); auto program = preparation.CaptureProgramBinding(std::move(payload));
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);

        data.constants[0] = preparation.ResolveView(bindings.source, {org::BindlessViewKind::ShaderResource}).index;
        data.constants[1] = preparation.ResolveView(bindings.depth, {org::BindlessViewKind::ShaderResource}).index;
        data.constants[2] = preparation.ResolveView(bindings.destination, {org::BindlessViewKind::UnorderedAccess}).index;
        const auto& destination = preparation.Describe(bindings.destination);
        data.constants[3] = destination.texture.width; data.constants[4] = destination.texture.height;
        data.groupsX = (destination.texture.width + 7u) / 8u;
        data.groupsY = (destination.texture.height + 7u) / 8u;
        return data;
    }

    static void Record(const DilateMotionVectorsBindings&, const br::render::PreparedComputeDispatch& data,
        org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatch(data, recording);
    }

    void ShutdownPass() {}

private:
    org::PipelineState m_pso;
};
