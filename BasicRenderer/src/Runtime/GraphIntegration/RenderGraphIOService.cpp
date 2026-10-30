#include "Runtime/GraphIntegration/RenderGraphIOService.h"

#include "Assets/Textures/TextureFactory.h"
#include "Runtime/IO/ReadbackManager.h"
#include "Materials/TextureStreaming/ITextureStreamingFeedbackService.h"
#include "Render/Runtime/IUploadService.h"

namespace br::render {

RenderGraphIOService::RenderGraphIOService(TextureFactory& textures,
    std::shared_ptr<org::runtime::IUploadService> uploads, br::ReadbackManager& readbacks,
    ITextureStreamingFeedbackService& feedback) noexcept
    : m_textures(&textures), m_uploads(std::move(uploads)), m_readbacks(&readbacks),
      m_textureStreamingFeedback(&feedback) {}

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
    if (auto pass = m_readbacks->GetReadbackPass())
        out.push_back(org::ExternalPassDesc::Render("Builtin::Readbacks", pass)
            .At(org::ExternalInsertPoint::End(0))
            .PinToQueue(static_cast<org::QueueSlotIndex>(0)));
}

void RenderGraphIOService::GatherFramePasses(
    std::vector<org::ExternalPassDesc>& out) {
    if (auto pass = m_textureStreamingFeedback->CreateTextureStreamingFeedbackReadbackPass())
        out.push_back(org::ExternalPassDesc::Copy("Material::TextureStreamingReadback", pass)
            .At(org::ExternalInsertPoint::After("MenuRenderPass"))
            .PreferQueue(org::QueueKind::Copy));
}

} // namespace br::render
