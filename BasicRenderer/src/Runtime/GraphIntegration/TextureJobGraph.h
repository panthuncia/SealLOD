#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <rhi.h>

class TextureFactory;
namespace org { class Runtime; class RenderGraph; }
namespace org::runtime { class IUploadService; }

namespace br::render {

// The graph BasicRenderer's texture jobs run in: GPU mip generation (and the handoff of each mipped texture to bindless
// sampling) and BC7 compression. It is a graph of its own on the runtime, next to the frame graph, so that new work changes
// its declarations instead of the frame graph's (ORG docs/job-graphs.md). It has its own upload queue, which the textures it
// mips upload their sources through, and leaves the runtime's frame services (statistics, captures, readback polling) to the
// frame graph.
//
// For now it executes once per frame on the render thread, ahead of the frame graph. Its results are not yet gated: a
// texture is published before its mips are generated, as it was when the passes ran in the frame graph.
class TextureJobGraph {
public:
    TextureJobGraph(std::shared_ptr<org::Runtime> runtime, TextureFactory& textures, uint32_t slots);
    ~TextureJobGraph();
    TextureJobGraph(const TextureJobGraph&) = delete;
    TextureJobGraph& operator=(const TextureJobGraph&) = delete;

    // The graph's upload queue (TextureFactory::SetJobUploadService).
    const std::shared_ptr<org::runtime::IUploadService>& Uploads() const noexcept { return m_uploads; }

    // One execution in a slot, when a texture job has work left: the slot's previous one has completed first (its upload pages
    // are reused). Otherwise only the completed executions retire (their jobs learn of completion).
    void Execute(uint32_t slot, uint64_t frameFenceValue, rhi::Device device);

private:
    class Extension;
    TextureFactory* m_textures;
    std::shared_ptr<org::Runtime> m_runtime;
    std::shared_ptr<org::runtime::IUploadService> m_uploads;
    std::unique_ptr<org::RenderGraph> m_graph;
    // Per slot: each queue's timeline and the value its last execution in the slot submitted up to.
    std::vector<std::vector<std::pair<std::shared_ptr<rhi::TimelinePtr>, uint64_t>>> m_slotPoints;
    uint64_t m_executionsWithWork = 0, m_batches = 0;  // logged when it goes
};

} // namespace br::render
