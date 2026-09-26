#pragma once

#include <string_view>

#include <rhi.h>

namespace org {
class ComputePassBuilder;
class RenderPassBuilder;
}

namespace br::validation {

class SARPInteropValidation {
public:
	static bool Enabled() noexcept;
	static void ApplyPassPolicy(std::string_view passName, org::ComputePassBuilder& builder, rhi::Backend peerApi);
	static void ApplyPassPolicy(std::string_view passName, org::RenderPassBuilder& builder, rhi::Backend peerApi);
};

} // namespace br::validation
