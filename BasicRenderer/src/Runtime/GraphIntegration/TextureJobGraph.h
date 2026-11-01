#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <rhi.h>

class TextureFactory;
namespace org { class Runtime; class GraphInstance; class PixelBuffer; }
namespace org::runtime { class IUploadService; }

namespace br::render {

// The graph BasicRenderer's texture jobs run in: GPU mip generation (and the handoff of each mipped texture to bindless
// sampling) and BC7 compression. It is a graph instance of its own on the runtime (org::GraphInstance), next to the frame
// graph, so that new work changes its declarations instead of the frame graph's (ORG docs/job-graphs.md). It has its own
// upload queue, which the textures it mips upload their sources through, and leaves the runtime's frame services
// (statistics, captures, readback polling) to the frame graph.
//
// It executes on the render thread, ahead of the frame graph, while a texture job has work. Its results are not yet
// gated: a texture is published before its mips are generated, as it was when the passes ran in the frame graph.
class TextureJobGraph {
public:
    // framesInFlight: executions that may run on the GPU at once. One more waits on the CPU for the oldest.
    TextureJobGraph(std::shared_ptr<org::Runtime> runtime, TextureFactory& textures, uint32_t framesInFlight);
    ~TextureJobGraph();
    TextureJobGraph(const TextureJobGraph&) = delete;
    TextureJobGraph& operator=(const TextureJobGraph&) = delete;

    // The graph's upload queue (TextureFactory::SetJobUploadService).
    const std::shared_ptr<org::runtime::IUploadService>& Uploads() const noexcept;

    // BR_TEXTURE_JOB_SELF_TEST=1: mips a 64x64 checkerboard on the GPU through TextureFactory, and once its mips are ready
    // (TextureFactory::WhenMipsReady) reads back its 1x1 mip, which must be mid-grey. Logs the result. Call once the factory
    // uploads through this graph (TextureFactory::SetJobUploadService).
    void StartSelfTestIfRequested();

    // One execution when a texture job has work left. Otherwise only the completed executions retire (their jobs learn of
    // completion).
    void Execute();

private:
    TextureFactory* m_textures;
    std::shared_ptr<org::Runtime> m_runtime;
    std::unique_ptr<org::GraphInstance> m_instance;
    uint64_t m_executionsWithWork = 0, m_batches = 0;  // logged when it goes
    std::shared_ptr<org::PixelBuffer> m_selfTestTexture;
};

} // namespace br::render
