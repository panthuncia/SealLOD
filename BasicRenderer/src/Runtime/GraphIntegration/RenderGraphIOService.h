#pragma once

#include "Render/RenderGraph/RenderGraphExtension.h"
namespace org { class RenderGraph; }

#include <memory>
#include <vector>

class TextureFactory;
class ITextureStreamingFeedbackService;
namespace org::runtime { class IUploadService; }

namespace br::render {

// Device-scoped owner for operations imported into each graph generation.
class RenderGraphIOService final {
public:
    RenderGraphIOService(TextureFactory&, std::shared_ptr<org::runtime::IUploadService>,
        ITextureStreamingFeedbackService&) noexcept;

    void SetRegistry(org::ResourceRegistry&);
    // A graph with no host (GraphInstance::Adopt) takes the upload service's pass from here; a GraphHost adds its own.
    void SetAddsUploadPass(bool adds) noexcept { m_addsUploadPass = adds; }
    void GatherStructuralPasses(std::vector<org::ExternalPassDesc>&);
    // After each frame graph execution: submits the texture-streaming feedback readback.
    void AfterExecute(org::RenderGraph&);

private:
    TextureFactory* m_textures;
    std::shared_ptr<org::runtime::IUploadService> m_uploads;
    ITextureStreamingFeedbackService* m_textureStreamingFeedback;
    bool m_addsUploadPass = false;
};

} // namespace br::render
