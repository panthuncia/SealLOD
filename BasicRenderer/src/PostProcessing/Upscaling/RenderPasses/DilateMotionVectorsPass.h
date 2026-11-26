#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include <BasicRenderer/Extensions/PipelineDescs.h>
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "BasicRenderer/Extensions/PassRevision.h"

struct DilateMotionVectorsBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken source, depth, destination;
};
struct DilateMotionVectorsFrame {
    uint32_t width = 0, height = 0;
    bool operator==(const DilateMotionVectorsFrame&) const = default;
};

class DilateMotionVectorsPass : public org::TypedRenderGraphPass<DilateMotionVectorsPass,
    DilateMotionVectorsFrame, DilateMotionVectorsBindings> {
public:
    DilateMotionVectorsBindings Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"shaders/PostProcessing/dilateMotionVectors.hlsl", L"DilateMotionVectorsCS", "DilateMotionVectorsCS"));
        return {
            m_pipeline,
            builder.ShaderResource(Builtin::Surface::Motion),
            builder.ShaderResource(Builtin::PrimaryCamera::ProjectedDepthTexture),
            builder.UnorderedAccess(Builtin::Surface::DilatedMotion) };
    }

    // Kept recordings (M6 step 4): the dispatch follows the destination's size, which only a new resource changes (a binding).
    void Revision(std::vector<uint64_t>& out) const { br::render::AppendPipelineRevision(out, m_pipeline); }



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
    br::PipelineState m_pipeline;
};
