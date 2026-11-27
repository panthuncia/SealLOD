#include "Runtime/GraphIntegration/RenderGraphIOService.h"

#include "Assets/Textures/TextureFactory.h"
#include "Materials/TextureStreaming/ITextureStreamingFeedbackService.h"
#include "Render/RenderGraph/RenderGraph.h"

namespace br::render {

RenderGraphIOService::RenderGraphIOService(TextureFactory& textures, ITextureStreamingFeedbackService& feedback) noexcept
    : m_textures(&textures), m_textureStreamingFeedback(&feedback) {}

void RenderGraphIOService::AfterExecute(org::RenderGraph& graph) {
    // The feedback copy runs on the copy queue when there is one.
    const auto& queues = graph.GetQueueRegistry();
    auto slot = queues.FindSlot({org::QueueKind::Copy, 0});
    if (org::ToUnderlying(slot) == 0xFF) slot = queues.FindGraphicsSlot();
    m_textureStreamingFeedback->SubmitTextureStreamingFeedbackReadback(graph.GetExternalClient(), org::ToUnderlying(slot));
}

} // namespace br::render
