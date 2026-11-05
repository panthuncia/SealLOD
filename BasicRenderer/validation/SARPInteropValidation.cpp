#include "Validation/SARPInteropValidation.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

#include <spdlog/spdlog.h>
#include <Render/PassBuilders.h>
#include <Render/RenderGraph/RenderGraph.h>

namespace br::validation {
namespace {

struct Configuration {
	bool enabled = false;
	bool exposure = false;
	bool bloom = false;
};

const Configuration& GetConfiguration() {
	static const Configuration configuration = [] {
		Configuration result;
		char* raw = nullptr;
		size_t length = 0;
		if (_dupenv_s(&raw, &length, "SARP_INTEROP_VALIDATION") != 0 || !raw) return result;
		std::string value(raw);
		std::free(raw);
		std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		result.enabled = !value.empty() && value != "0" && value != "false" && value != "off";
		result.exposure = result.enabled && (value == "1" || value == "all" || value.find("exposure") != std::string::npos);
		result.bloom = result.enabled && (value == "1" || value == "all" || value.find("bloom") != std::string::npos);
		if (result.enabled) {
			spdlog::info("SARP interop validation enabled: exposure={} bloom={}", result.exposure, result.bloom);
		}
		return result;
	}();
	return configuration;
}

bool PlaceOnPeer(std::string_view passName) {
	const auto& configuration = GetConfiguration();
	return (configuration.exposure && passName == "luminanceHistogramPass") ||
		(configuration.bloom && passName == "BloomDownsamplePass2");
}

} // namespace

bool SARPInteropValidation::Enabled() noexcept { return GetConfiguration().enabled; }
void SARPInteropValidation::ApplyPassPolicy(std::string_view passName, org::RenderPassBuilder& builder, rhi::Backend peerApi) {
	if (!PlaceOnPeer(passName)) return;
	if (peerApi == rhi::Backend::Null)
		throw std::runtime_error("SARP interop validation requested peer placement without an initialized peer API");
	builder.RequireAPI(peerApi);
}

void SARPInteropValidation::ApplyNamedPasses(org::RenderGraph& graph, rhi::Backend peerApi) {
	char* raw = nullptr;
	size_t length = 0;
	if (_dupenv_s(&raw, &length, "SARP_INTEROP_VALIDATION_PASSES") != 0 || !raw) return;
	const std::string list(raw);
	std::free(raw);
	if (list.empty()) return;
	if (peerApi == rhi::Backend::Null)
		throw std::runtime_error("SARP_INTEROP_VALIDATION_PASSES needs BASICRENDERER_MULTI_RHI=1");
	size_t start = 0;
	while (start <= list.size()) {
		const auto end = std::min(list.find(',', start), list.size());
		const auto name = list.substr(start, end - start);
		start = end + 1;
		if (name.empty()) continue;
		auto* builder = graph.FindPassBuilder(name);
		if (!builder) {
			// Graphs differ with the features enabled (and the graph is rebuilt), so a pass missing from one is not an error.
			spdlog::warn("SARP interop validation: this graph has no pass '{}' to require on the peer", name);
			continue;
		}
		builder->RequireAPI(peerApi);
		spdlog::info("SARP interop validation: pass '{}' required on the peer API", name);
	}
}

} // namespace br::validation
