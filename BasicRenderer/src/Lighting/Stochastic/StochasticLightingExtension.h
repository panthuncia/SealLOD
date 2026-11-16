#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "Render/RenderGraph/RenderGraphExtension.h"

namespace org { class ResourceGroup; }

namespace br::render {

class CLodRayTracingSystem;

struct StochasticLightingOptions {
	// Brute force over every light in the cluster, averaged over frames: the ground truth the estimator is checked against.
	bool reference = false;
	// The estimator keeps a running average while nothing changes, to be compared with the reference.
	bool accumulate = false;
	// The temporal and spatial denoiser after the estimator (not with the reference or accumulation).
	bool denoise = true;
	// The estimator samples one pixel of each 2x2 block a frame with 2 (docs/stochastic_lighting.md "Reduced resolution");
	// 1 samples every pixel. The reference always runs at full resolution.
	uint32_t downsample = 2u;
};

// Stochastic direct lighting's passes (docs/stochastic_lighting.md), placed before DeferredShadingPass. They write
// Builtin::StochasticLighting::Diffuse and ::Specular, which the renderer registers, and the deferred pass adds them in
// ShadowingMode::StochasticRayTraced.
class StochasticLightingExtension final : public org::IRenderGraphExtension {
public:
	StochasticLightingExtension(std::shared_ptr<CLodRayTracingSystem> rayTracingSystem, std::shared_ptr<org::ResourceGroup> slabs,
		StochasticLightingOptions options);

	void GatherStructuralPasses(org::RenderGraph& graph, std::vector<org::ExternalPassDesc>& passes) override;

private:
	std::shared_ptr<CLodRayTracingSystem> m_rayTracingSystem;
	std::shared_ptr<org::ResourceGroup> m_slabs;
	StochasticLightingOptions m_options;
};

} // namespace br::render
