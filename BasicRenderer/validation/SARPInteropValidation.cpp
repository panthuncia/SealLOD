#include "Validation/SARPInteropValidation.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

#include <spdlog/spdlog.h>
#include <Render/PassBuilders.h>

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

template<class Builder>
void Apply(std::string_view passName, Builder& builder, rhi::Backend peerApi) {
	if (!PlaceOnPeer(passName)) return;
	if (peerApi == rhi::Backend::Null)
		throw std::runtime_error("SARP interop validation requested peer placement without an initialized peer API");
	builder.RequireAPI(peerApi);
}

} // namespace

bool SARPInteropValidation::Enabled() noexcept { return GetConfiguration().enabled; }
void SARPInteropValidation::ApplyPassPolicy(std::string_view name, org::ComputePassBuilder& builder, rhi::Backend peerApi) {
	Apply(name, builder, peerApi);
}
void SARPInteropValidation::ApplyPassPolicy(std::string_view name, org::RenderPassBuilder& builder, rhi::Backend peerApi) {
	Apply(name, builder, peerApi);
}

} // namespace br::validation
