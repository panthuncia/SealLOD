#pragma once

#include <string_view>

#include <rhi.h>

namespace org {
class RenderGraph;
class RenderPassBuilder;
}

namespace br::validation {

class SARPInteropValidation {
public:
	static bool Enabled() noexcept;
	static void ApplyPassPolicy(std::string_view passName, org::RenderPassBuilder& builder, rhi::Backend peerApi);
	// SARP_INTEROP_VALIDATION_PASSES: a comma-separated list of passes to require on the peer, by name. A name the graph does
	// not have is logged and skipped. Call after the passes are built, before the graph compiles.
	static void ApplyNamedPasses(org::RenderGraph& graph, rhi::Backend peerApi);
};

} // namespace br::validation
