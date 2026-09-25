#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Render/RenderContext.h"
#include "Render/DepthHistoryService.h"
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
        builder.WithShaderResource(Builtin::LinearDepthMaps);
    }

    org::EmptyPassFrameData Prepare(const org::PassPrepareContext& preparation) {
        const auto* frame = preparation.preparationData->Get<UpdateContext>();
        if (m_historyService) preparation.Reserve(
            m_historyService->ReserveDepthHistoryPublication(
                frame ? frame->viewFamily : nullptr,
                frame ? frame->frameNumber : 0));
        return {};
    }
    static void Record(const org::EmptyPassFrameData&, org::PassRecordContext&) {}

private:
    br::render::IDepthHistoryService* m_historyService = nullptr;
};
