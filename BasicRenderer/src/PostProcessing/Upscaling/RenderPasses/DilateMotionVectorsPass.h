#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

struct DilateMotionVectorsBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken source, depth, destination;
};
struct DilateMotionVectorsFrame {
    uint32_t width = 0, height = 0;
};

class DilateMotionVectorsPass : public org::TypedRenderGraphPass<DilateMotionVectorsPass,
    DilateMotionVectorsFrame, DilateMotionVectorsBindings> {
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
            m_pso,
            builder.ShaderResource(Builtin::Surface::Motion),
            builder.ShaderResource(Builtin::PrimaryCamera::ProjectedDepthTexture),
            builder.UnorderedAccess(Builtin::Surface::DilatedMotion) };
    }



    DilateMotionVectorsFrame Prepare(const DilateMotionVectorsBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        const auto& destination = preparation.Describe(bindings.destination);
        return {destination.texture.width, destination.texture.height};
    }

    static void Record(const DilateMotionVectorsBindings& bindings, const DilateMotionVectorsFrame& frame,
        org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[0] = context.Index(bindings.source);
        constants[1] = context.Index(bindings.depth);
        constants[2] = context.Index(bindings.destination);
        constants[3] = frame.width; constants[4] = frame.height;
        br::render::DispatchCompute(context, bindings.pipeline, constants, (frame.width + 7u) / 8u, (frame.height + 7u) / 8u);
    }

    void ShutdownPass() {}

private:
    org::PipelineState m_pso;
};
