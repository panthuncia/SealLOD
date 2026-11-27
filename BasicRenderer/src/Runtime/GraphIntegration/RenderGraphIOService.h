#pragma once

#include "Render/RenderGraph/RenderGraphExtension.h"
namespace org { class RenderGraph; }

#include <memory>
#include <vector>

class TextureFactory;
class ITextureStreamingFeedbackService;

namespace br::render {

// Device-scoped owner for operations imported into each graph generation.
class RenderGraphIOService final {
public:
    // The upload service's pass comes from the graph's host (org::GraphHost adds it to every graph it builds), which also points
    // the upload service at the graph's registry. Mip generation and BC7 compression run in the texture job graph
    // (TextureJobGraph). Texture readbacks (ReadbackManager) are submitted through the graph's external client after each frame.
    RenderGraphIOService(TextureFactory&, ITextureStreamingFeedbackService&) noexcept;

    // After each frame graph execution: submits the texture-streaming feedback readback.
    void AfterExecute(org::RenderGraph&);

private:
    TextureFactory* m_textures;
    ITextureStreamingFeedbackService* m_textureStreamingFeedback;
};

} // namespace br::render
