#pragma once

#include "Hair/HairRuntime.h"

#include <BasicRenderer/Extensions/RenderGraphExtensionRegistration.h>
#include <BasicRenderer/Extensions/VirtualShadowCasterProvider.h>

#include <memory>
#include <vector>

namespace br::hair {

// The hair passes of a frame graph. Register it in the Visibility epoch (PipelineRecipe::AddExtension). It adds
// Hair::Select, Hair::RootCheck, Hair::Simulate, Hair::GuideLevels and Hair::Evaluate before culling, Hair::Raster once
// the opaque visibility buffer is complete, Hair::Resolve and Hair::Probe after material evaluation, and the debug view
// Hair::DebugOverlay, which it gives to the Overlay epoch.
//
// It is also a caster of the sun's virtual shadow map (IVirtualShadowCasterProvider): in a graph that has one it gives
// the shadow map Hair::ShadowSelect, Hair::ShadowEvaluate and Hair::ShadowRaster, which draw hair into the pages the
// frame draws (hairShadow.hlsl). The provider reaches the sun's clipmaps only: hair is in no spot or point light's faces.
class HairExtension final : public org::IRenderGraphExtension, public IVirtualShadowCasterProvider {
public:
    explicit HairExtension(std::shared_ptr<HairRuntime> runtime);
    // What the shade pass needs of the shadow map that no name reaches: its cached pages (src/HairExtension.cpp).
    // Filled when the shadow map gathers hair's passes; the shade pass reads it when it declares.
    struct ShadowMap;
    void GatherStructuralPasses(org::RenderGraph& rg, std::vector<org::ExternalPassDesc>& out) override;

    std::string_view GetVirtualShadowCasterProviderId() const noexcept override { return "Hair"; }
    float GetRequestedDynamicShadowRadius() const override;
    void OnVirtualShadowCasterRegistered(const std::shared_ptr<VirtualShadowInvalidationQueue>& queue) override;
    void GatherVirtualShadowPreparationPasses(const VirtualShadowCasterBuildContext& context, VirtualShadowPassBuilder& builder) override;
    void GatherVirtualShadowRasterPasses(const VirtualShadowCasterBuildContext& context, VirtualShadowPassBuilder& builder) override;

private:
    std::shared_ptr<HairRuntime> m_runtime;
    std::shared_ptr<ShadowMap> m_shadowMap;
};

} // namespace br::hair
