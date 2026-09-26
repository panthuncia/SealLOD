#pragma once

#include "ProceduralWind/ProceduralWindRuntime.h"
#include <BasicRenderer/Extensions/RenderGraphExtensionRegistration.h>

namespace br::wind {

class ProceduralWindExtension final : public org::RenderGraph::IRenderGraphExtension {
public:
    explicit ProceduralWindExtension(std::shared_ptr<ProceduralWindRuntime> runtime);
    void GatherStructuralPasses(org::RenderGraph& rg, std::vector<org::RenderGraph::ExternalPassDesc>& out) override;

private:
    std::shared_ptr<ProceduralWindRuntime> m_runtime;
};

} // namespace br::wind
