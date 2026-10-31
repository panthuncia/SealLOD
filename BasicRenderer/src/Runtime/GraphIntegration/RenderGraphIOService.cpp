#include "Runtime/GraphIntegration/RenderGraphIOService.h"

#include "Assets/Textures/TextureFactory.h"
#include "Materials/TextureStreaming/ITextureStreamingFeedbackService.h"
#include "Render/Runtime/IUploadService.h"
#include "Render/RenderGraph/RenderGraph.h"

namespace br::render {

RenderGraphIOService::RenderGraphIOService(TextureFactory& textures,
    std::shared_ptr<org::runtime::IUploadService> uploads, ITextureStreamingFeedbackService& feedback) noexcept
    : m_textures(&textures), m_uploads(std::move(uploads)), m_textureStreamingFeedback(&feedback) {}

void RenderGraphIOService::SetRegistry(org::ResourceRegistry& registry) {
    org::runtime::UploadResolveContext context;
    context.registry = &registry;
    context.epoch = 0;
    m_uploads->SetUploadResolveContext(context);
}

void RenderGraphIOService::GatherStructuralPasses(
    std::vector<org::ExternalPassDesc>& out) {
    if (auto pass = m_uploads->GetUploadPass())
        out.push_back(org::ExternalPassDesc::Render("Builtin::Uploads", pass)
            .At(org::ExternalInsertPoint::Begin(0)));
    // Mip generation and BC7 compression run in the texture job graph (TextureJobGraph), not here.
    // Texture readbacks (ReadbackManager) are submitted through the graph's external client after each frame.
}

void RenderGraphIOService::AfterExecute(org::RenderGraph& graph) {
    // The feedback copy runs on the copy queue when there is one.
    const auto& queues = graph.GetQueueRegistry();
    auto slot = queues.FindSlot({org::QueueKind::Copy, 0});
    if (org::ToUnderlying(slot) == 0xFF) slot = queues.FindGraphicsSlot();
    m_textureStreamingFeedback->SubmitTextureStreamingFeedbackReadback(graph.GetExternalClient(), org::ToUnderlying(slot));
}

} // namespace br::render
