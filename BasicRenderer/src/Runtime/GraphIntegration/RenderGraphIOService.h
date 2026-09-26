#pragma once

#include "Render/RenderGraph/RenderGraph.h"

#include <memory>
#include <vector>

class TextureFactory;
class ITextureStreamingFeedbackService;
namespace br { class ReadbackManager; }
namespace org::runtime { class IUploadService; }

namespace br::render {

// Device-scoped owner for operations imported into each graph generation.
class RenderGraphIOService final {
public:
    RenderGraphIOService(TextureFactory&, std::shared_ptr<org::runtime::IUploadService>,
        br::ReadbackManager&, ITextureStreamingFeedbackService&) noexcept;

    void SetRegistry(org::ResourceRegistry&);
    void GatherStructuralPasses(std::vector<org::RenderGraph::ExternalPassDesc>&);
    void GatherFramePasses(std::vector<org::RenderGraph::ExternalPassDesc>&);

private:
    TextureFactory* m_textures;
    std::shared_ptr<org::runtime::IUploadService> m_uploads;
    br::ReadbackManager* m_readbacks;
    ITextureStreamingFeedbackService* m_textureStreamingFeedback;
};

} // namespace br::render
