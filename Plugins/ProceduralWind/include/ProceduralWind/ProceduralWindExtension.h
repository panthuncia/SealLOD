#pragma once

#include "ProceduralWind/ProceduralWindRuntime.h"
#include <BasicRenderer/Extensions/RenderGraphExtensionRegistration.h>

namespace br::wind {

struct WindTypeBlockCache;

class ProceduralWindExtension final : public org::IRenderGraphExtension {
public:
    explicit ProceduralWindExtension(std::shared_ptr<ProceduralWindRuntime> runtime);
    void GatherStructuralPasses(org::RenderGraph& rg, std::vector<org::ExternalPassDesc>& out) override;

private:
    std::shared_ptr<ProceduralWindRuntime> m_runtime;
    // The wind skeletons' type blocks, shared by the graphs this extension builds passes for.
    std::shared_ptr<WindTypeBlockCache> m_typeBlocks;
};

} // namespace br::wind
