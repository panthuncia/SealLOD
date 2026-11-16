#include "Lighting/Stochastic/StochasticLightingExtension.h"

#include <algorithm>
#include <utility>

#include "Lighting/Stochastic/RenderPasses/StochasticDenoisePass.h"
#include "Lighting/Stochastic/RenderPasses/StochasticGuidePass.h"
#include "Lighting/Stochastic/RenderPasses/StochasticSamplePass.h"
#include "Render/RenderGraph/RenderGraph.h"
#include "Runtime/Settings/SettingsManager.h"

namespace br::render {

StochasticLightingExtension::StochasticLightingExtension(std::shared_ptr<CLodRayTracingSystem> rayTracingSystem,
	std::shared_ptr<org::ResourceGroup> slabs, StochasticLightingOptions options)
	: m_rayTracingSystem(std::move(rayTracingSystem)), m_slabs(std::move(slabs)), m_options(options) {
}

void StochasticLightingExtension::GatherStructuralPasses(org::RenderGraph& graph, std::vector<org::ExternalPassDesc>& passes) {
	if (!m_rayTracingSystem) {
		return;
	}
	// The reference and the estimator's running average are compared without the denoiser.
	const bool denoise = m_options.denoise && !m_options.reference && !m_options.accumulate;
	const auto resolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
	// The reference is the full-resolution ground truth. The estimator's running average keeps the sampling resolution: each
	// pixel averages the frames that sampled it, so it is compared with the reference pixel by pixel.
	const uint32_t downsample = m_options.reference ? 1u : std::clamp(m_options.downsample, 1u, 2u);
	// The estimator's slots (docs/stochastic_lighting.md "Kernel split"): the most samples a sampled pixel takes. Reduced
	// resolution takes twice the setting, at most STOCHASTIC_MAX_SAMPLE_COUNT; full resolution the setting, which rebuilds the
	// graph when it changes (RendererSettings).
	const bool split = !m_options.reference;
	const uint32_t samples = std::clamp(SettingsManager::GetInstance().getSettingGetter<uint32_t>("stochasticLightingSamplesPerPixel")(),
		1u, STOCHASTIC_MAX_SAMPLE_COUNT);
	const uint32_t slotCapacity = !split ? 0u : downsample > 1u ? STOCHASTIC_MAX_SAMPLE_COUNT : samples;
	auto resources = StochasticLightingResources::Create(resolution.x, resolution.y, denoise, downsample, slotCapacity);

	const auto add = [&](const char* name, std::shared_ptr<org::RenderPass> pass) {
		auto desc = org::ExternalPassDesc::Compute(name, std::move(pass));
		desc.At(org::ExternalInsertPoint::Before("DeferredShadingPass"));
		desc.Technique("Lighting::Stochastic");
		passes.push_back(std::move(desc));
	};
	const auto mode = m_options.reference ? StochasticLightingPassMode::Reference : StochasticLightingPassMode::Estimator;
	if (denoise) {
		// The guide for guide-normalized accumulation (docs/stochastic_lighting.md "Guide-normalized accumulation"), before shading reads it.
		// Named in the graph for capture.hdr's stochastic_guide target.
		graph.RegisterResource(org::ResourceIdentifier{ StochasticLightingResources::kGuideResourceName }, resources->guide);
		add("StochasticGuidePass", std::make_shared<StochasticGuidePass>(resources));
	}
	if (split) {
		// Selection, the trace and shading. One state for the three, so they agree on each frame's flags.
		auto state = std::make_shared<StochasticSampleState>();
		for (const auto& [name, kernel] : { std::pair{ "StochasticSelectPass", StochasticSampleKernel::Select },
				std::pair{ "StochasticTracePass", StochasticSampleKernel::Trace }, std::pair{ "StochasticShadePass", StochasticSampleKernel::Shade } }) {
			add(name, std::make_shared<StochasticSamplePass>(m_rayTracingSystem, m_slabs, resources, mode, m_options.accumulate, kernel, state));
		}
	}
	else {
		add("StochasticReferencePass", std::make_shared<StochasticSamplePass>(m_rayTracingSystem, m_slabs, resources, mode, m_options.accumulate));
	}
	if (denoise) {
		add("StochasticDenoiseTemporalPass", std::make_shared<StochasticDenoisePass>(resources, StochasticDenoiseStage::Temporal));
		add("StochasticDenoiseSpatialPass", std::make_shared<StochasticDenoisePass>(resources, StochasticDenoiseStage::Spatial));
	}
}

} // namespace br::render
