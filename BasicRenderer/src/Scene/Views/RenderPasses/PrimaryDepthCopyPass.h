#pragma once

#include <unordered_map>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "BasicRenderer/Extensions/PassRevision.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include <BasicRenderer/Extensions/PipelineDescs.h>
#include "BasicRenderer/Extensions/RenderContext.h"

struct PrimaryDepthCopyBindings {
	br::PipelineState pipeline;
};
struct PrimaryDepthCopyFrame {
	uint32_t groupsX = 0, groupsY = 0;
	bool operator==(const PrimaryDepthCopyFrame&) const = default;
};

class PrimaryDepthCopyPass : public org::TypedRenderGraphPass<PrimaryDepthCopyPass, PrimaryDepthCopyFrame, PrimaryDepthCopyBindings> {
public:
	PrimaryDepthCopyBindings Declare(org::PassBuilder& declaration) {
        declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* builder = &declaration;
		builder->ShaderResource(Builtin::PrimaryCamera::VisibilityTexture);
		builder->UnorderedAccess(Builtin::PrimaryCamera::LinearDepthMap);
		builder->ConstantBuffer(Builtin::PerFrameBuffer);
		m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(L"shaders/canonicalSurface.hlsl", L"PrimaryDepthCopyCS",
			"PrimaryDepthCopyPSO"));
		return {m_pipeline};
	}

    // Kept recordings (M6 step 8): the dispatch covers the render resolution, taken before Update
    // (RenderGraphPass::TakeFrameValues) and returned by Prepare.
    void TakeFrameValues(const org::UpdateExecutionContext& execution) override {
        const auto* context = execution.hostData ? execution.hostData->Get<UpdateContext>() : nullptr;
        m_taken = context ? PrimaryDepthCopyFrame{(context->renderResolution.x + 7u) / 8u, (context->renderResolution.y + 7u) / 8u}
                          : PrimaryDepthCopyFrame{};
    }
    void Revision(std::vector<uint64_t>& out) const {
        br::render::AppendPipelineRevision(out, m_pipeline);
        out.push_back(m_taken.groupsX | (uint64_t{m_taken.groupsY} << 32));
    }

    PrimaryDepthCopyFrame Prepare(const PrimaryDepthCopyBindings&, const org::PassPrepareContext&) const {
        return m_taken;
    }

    static void Record(const PrimaryDepthCopyBindings& bindings, const PrimaryDepthCopyFrame& frame, org::PassRecordContext& context) {
        br::render::DispatchCompute(context, bindings.pipeline, {}, frame.groupsX, frame.groupsY);
    }

private:
    br::PipelineState m_pipeline;
    PrimaryDepthCopyFrame m_taken{};
};
