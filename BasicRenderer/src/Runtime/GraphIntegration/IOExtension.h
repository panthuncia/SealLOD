#pragma once

#include "Render/RenderGraph/RenderGraphExtension.h"
namespace org { class RenderGraph; }
#include "Runtime/GraphIntegration/RenderGraphIOService.h"

#include <memory>

// Adapts the renderer-owned IO service to graph extension insertion points.
class RenderGraphIOExtension final : public org::IRenderGraphExtension {
public:
    explicit RenderGraphIOExtension(std::shared_ptr<br::render::RenderGraphIOService> service)
        : m_service(std::move(service)) {}

    // No structural passes: the upload pass comes from the graph's host (RenderGraphIOService).
    void GatherStructuralPasses(org::RenderGraph&, std::vector<org::ExternalPassDesc>&) override {}

    void AfterExecute(org::RenderGraph& graph) override {
        m_service->AfterExecute(graph);
    }

private:
    std::shared_ptr<br::render::RenderGraphIOService> m_service;
};
