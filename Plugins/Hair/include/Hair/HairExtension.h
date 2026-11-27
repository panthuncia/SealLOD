#pragma once

#include "Hair/HairRuntime.h"

#include <BasicRenderer/Extensions/RenderGraphExtensionRegistration.h>

#include <memory>
#include <vector>

namespace br::hair {

// The hair passes of a frame graph. Register it in the Visibility epoch (PipelineRecipe::AddExtension). It adds
// Hair::Select, Hair::RootCheck, Hair::Simulate, Hair::GuideLevels and Hair::Evaluate before culling, Hair::Raster once
// the opaque visibility buffer is complete, Hair::Resolve and Hair::Probe after material evaluation, and the debug view
// Hair::DebugOverlay, which it gives to the Overlay epoch.
class HairExtension final : public org::IRenderGraphExtension {
public:
    explicit HairExtension(std::shared_ptr<HairRuntime> runtime);
    void GatherStructuralPasses(org::RenderGraph& rg, std::vector<org::ExternalPassDesc>& out) override;

private:
    std::shared_ptr<HairRuntime> m_runtime;
};

} // namespace br::hair
