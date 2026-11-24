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
    // The host's upload extension points the upload service at the graph's registry; without a host, this does.
    if (!m_addsUploadPass) return;
    org::runtime::UploadResolveContext context;
    context.registry = &registry;
    context.epoch = 0;
    m_uploads->SetUploadResolveContext(context);
}

void RenderGraphIOService::GatherStructuralPasses(
    std::vector<org::ExternalPassDesc>& out) {
    if (m_addsUploadPass)
        if (auto pass = m_uploads->GetUploadPass())
            out.push_back(org::ExternalPassDesc::Render("Builtin::Uploads", pass).At(org::ExternalInsertPoint::Begin(0)));
    // The upload service's pass comes from the graph's host (org::GraphHost adds it to every graph it builds). Adding the
    // same pass object again would run it twice and point the upload service at this graph's registry a second time.
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
