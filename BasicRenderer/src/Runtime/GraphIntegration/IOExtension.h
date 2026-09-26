#pragma once

#include "Render/RenderGraph/RenderGraph.h"
#include "Runtime/GraphIntegration/RenderGraphIOService.h"

#include <memory>

// Adapts the renderer-owned IO service to graph extension insertion points.
class RenderGraphIOExtension final : public org::RenderGraph::IRenderGraphExtension {
public:
    explicit RenderGraphIOExtension(std::shared_ptr<br::render::RenderGraphIOService> service)
        : m_service(std::move(service)) {}

    void OnRegistryReset(org::ResourceRegistry* registry) override {
        m_service->SetRegistry(*registry);
    }

    void GatherStructuralPasses(org::RenderGraph&,
        std::vector<org::RenderGraph::ExternalPassDesc>& passes) override {
        m_service->GatherStructuralPasses(passes);
    }

    void GatherFramePasses(org::RenderGraph&,
        std::vector<org::RenderGraph::ExternalPassDesc>& passes) override {
        m_service->GatherFramePasses(passes);
    }

private:
    std::shared_ptr<br::render::RenderGraphIOService> m_service;
};
