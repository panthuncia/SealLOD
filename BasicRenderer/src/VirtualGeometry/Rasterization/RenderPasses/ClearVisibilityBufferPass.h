#pragma once

#include <array>
#include <functional>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include <BasicRenderer/Diagnostics/OutputTypes.h>
#include "Resources/PixelBuffer.h"

struct ClearVisibilityBindings {
	struct Clear { org::DeclaredViewToken gpu, cpu; };
	std::array<Clear, 10> clears;
	org::DeclaredViewToken depth;
};

class ClearVisibilityBufferPass final
	: public org::TypedRenderGraphPass<ClearVisibilityBufferPass,
		org::EmptyPassFrameData, ClearVisibilityBindings> {
public:
	ClearVisibilityBufferPass() = default;

	ClearVisibilityBindings Declare(org::PassBuilder& builder) {
		ClearVisibilityBindings bindings{};
		const std::array resources{
			Builtin::PrimaryCamera::VisibilityTexture,
			Builtin::Surface::BaseColorOpacity,
			Builtin::Surface::NormalRoughness,
			Builtin::Surface::SpecularAo,
			Builtin::Surface::Emissive,
			Builtin::Surface::Motion,
			Builtin::Surface::Payload0,
			Builtin::Surface::Payload1,
			Builtin::Surface::Identity,
			Builtin::DebugVisualization };
		for (size_t i = 0; i < resources.size(); ++i) {
			auto clear = builder.UnorderedAccessClear(resources[i]);
			bindings.clears[i] = {clear.View(0), clear.View(1)};
		}
		bindings.depth = builder.DepthStencilClear(Builtin::PrimaryCamera::DepthTexture);
		return bindings;
	}

	void ShutdownPass() {
		// Cleanup the render pass
	}
	static void Record(const ClearVisibilityBindings& bindings, org::PassRecordContext& context) {
		auto& commands = context.Commands();
		auto clear = [&](const ClearVisibilityBindings::Clear& token, bool integer) {
			rhi::UavClearInfo info{};
			info.resource = context.Resolve(token.gpu);
			info.cpuVisible = context.View(token.cpu);
			info.shaderVisible = context.View(token.gpu);
			if (integer) {
				rhi::UavClearUint value{};
				value.v[0] = value.v[1] = 0xFFFFFFFF;
				commands.ClearUavUint(info, value);
			} else {
				commands.ClearUavFloat(info, {});
			}
		};
		clear(bindings.clears[0], true);
		clear(bindings.clears[8], true);
		for (size_t i = 1; i < 8; ++i) clear(bindings.clears[i], false);
		clear(bindings.clears[9], true);
		commands.ClearDepthStencilView(context.View(bindings.depth), true, false, 1.0f, 0);
	}

};
