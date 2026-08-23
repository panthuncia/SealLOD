#pragma once

#include "ProceduralWind/ProceduralWindRuntime.h"
#include "Render/GraphExtensions/RenderGraphExtensionRegistration.h"

namespace br::wind {

class ProceduralWindExtension final : public RenderGraph::IRenderGraphExtension {
public:
    explicit ProceduralWindExtension(std::shared_ptr<ProceduralWindRuntime> runtime);
    void GatherStructuralPasses(RenderGraph& rg, std::vector<RenderGraph::ExternalPassDesc>& out) override;

private:
    std::shared_ptr<ProceduralWindRuntime> m_runtime;
};

} // namespace br::wind
