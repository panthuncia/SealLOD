#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Runtime/Detail/DepthHistoryService.h"
#include <vector>

class LinearDepthHistoryCopyPass
    : public org::TypedRenderGraphPass<LinearDepthHistoryCopyPass> {
public:
    explicit LinearDepthHistoryCopyPass(br::render::IDepthHistoryService* historyService)
        : m_historyService(historyService) {
    }

    void Declare(org::PassBuilder& builder) {
        // The current depth pyramid remains intact until the next frame's
        // phase-1 cull consumes it. Declaring the read keeps this marker after
        // the final phase-2 depth writes without copying the texture.
        builder.ShaderResource(Builtin::LinearDepthMaps);
    }

    // The frame's depth maps become the history in Update (M6 step 4), not by a lifecycle effect reserved in Prepare: a kept
    // recording of Post's epoch prepares nothing, and a pass with a lifecycle effect cannot be kept. Post runs after the epoch
    // that writes the depth maps, so they are submitted by then.
    void Update(const org::UpdateExecutionContext& execution) override {
        const auto* frame = execution.hostData ? execution.hostData->Get<UpdateContext>() : nullptr;
        if (m_historyService && frame) m_historyService->PublishDepthHistory(frame->viewFamily, frame->frameNumber);
    }
    // It records nothing.
    void Revision(std::vector<uint64_t>&) const {}
    static void Record(const org::EmptyPassFrameData&, org::PassRecordContext&) {}

private:
    br::render::IDepthHistoryService* m_historyService = nullptr;
};
