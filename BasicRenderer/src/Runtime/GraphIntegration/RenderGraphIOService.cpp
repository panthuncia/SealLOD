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
    std::vector<org::RenderGraph::ExternalPassDesc>& out) {
    if (auto pass = m_uploads->GetUploadPass())
        out.push_back(org::RenderGraph::ExternalPassDesc::Render("Builtin::Uploads", pass)
            .At(org::RenderGraph::ExternalInsertPoint::Begin(0)));
    if (auto pass = m_textures->GetMipmappingPass())
        out.push_back(org::RenderGraph::ExternalPassDesc::Compute("Builtin::Mipmapping", pass)
            .At(org::RenderGraph::ExternalInsertPoint::Begin(1)));
    if (auto pass = m_textures->GetBC7CompressionPass())
        out.push_back(org::RenderGraph::ExternalPassDesc::Compute("Builtin::BC7Compression", pass)
            .At(org::RenderGraph::ExternalInsertPoint::Begin(2)));
    if (auto pass = m_textures->GetBC7CompressionCopyPass())
        out.push_back(org::RenderGraph::ExternalPassDesc::Render("Builtin::BC7CompressionCopy", pass)
            .At(org::RenderGraph::ExternalInsertPoint::Begin(3)));
    if (auto pass = m_textures->GetBC7CompressionReadbackPass())
        out.push_back(org::RenderGraph::ExternalPassDesc::Copy("Builtin::BC7CompressionReadback", pass)
            .At(org::RenderGraph::ExternalInsertPoint::Begin(4))
            .PinToQueue(static_cast<org::QueueSlotIndex>(2)));
    if (auto pass = m_readbacks->GetReadbackPass())
        out.push_back(org::RenderGraph::ExternalPassDesc::Render("Builtin::Readbacks", pass)
            .At(org::RenderGraph::ExternalInsertPoint::End(0))
            .PinToQueue(static_cast<org::QueueSlotIndex>(0)));
}

void RenderGraphIOService::GatherFramePasses(
    std::vector<org::RenderGraph::ExternalPassDesc>& out) {
    if (auto pass = m_textureStreamingFeedback->CreateTextureStreamingFeedbackReadbackPass())
        out.push_back(org::RenderGraph::ExternalPassDesc::Copy("Material::TextureStreamingReadback", pass)
            .At(org::RenderGraph::ExternalInsertPoint::After("MenuRenderPass"))
            .PreferQueue(org::QueueKind::Copy));
}

} // namespace br::render
