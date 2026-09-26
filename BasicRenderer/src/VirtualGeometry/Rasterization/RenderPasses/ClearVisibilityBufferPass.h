#pragma once

#include <array>
#include "Runtime/StateGraph/InvocationRevision.h"
#include <functional>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include <BasicRenderer/Diagnostics/OutputTypes.h>
#include "Resources/PixelBuffer.h"

struct ClearVisibilityFrameData {
	rhi::DescriptorHeapHandle textureHeap{}, samplerHeap{};
	struct Clear {
		org::PreparedResourceReference resource;
		org::PreparedDescriptorReference cpu, gpu;
		bool integer = false;
	};
	std::vector<Clear> clears;
	org::PreparedDescriptorReference depth;
};

struct ClearVisibilityBindings {
	std::array<org::ResourceBindingToken, 10> clears;
	org::ResourceBindingToken depth;
};

class ClearVisibilityBufferPass final
	: public org::TypedRenderGraphPass<ClearVisibilityBufferPass,
		ClearVisibilityFrameData, ClearVisibilityBindings> {
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
		for (size_t i = 0; i < resources.size(); ++i)
			bindings.clears[i] = builder.BindUnorderedAccessClear(resources[i]);
		bindings.depth = builder.BindDepthStencilClear(Builtin::PrimaryCamera::DepthTexture);
		return bindings;
	}

	void InvocationRevision(const org::PassPrepareContext& preparation, std::vector<uint64_t>& out) const {
		br::render::AppendFrameHeapRevision(preparation, out);
	}
	ClearVisibilityFrameData Prepare(const ClearVisibilityBindings& bindings,
		const org::PassPrepareContext& preparation) const {
		ClearVisibilityFrameData data{};
		const auto* context = preparation.preparationData->Get<UpdateContext>();
		data.textureHeap = context->textureDescriptorHeap.GetHandle();
		data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
		auto append = [&](org::ResourceBindingToken token, bool integer) {
			data.clears.push_back({
				preparation.CaptureResource(token),
				preparation.CaptureView(token, {org::BindlessViewKind::NonShaderVisibleUnorderedAccess}),
				preparation.CaptureView(token, {org::BindlessViewKind::UnorderedAccess}),
				integer });
		};
		append(bindings.clears[0], true);
		append(bindings.clears[8], true);
		for (size_t i = 1; i < 8; ++i) append(bindings.clears[i], false);
		append(bindings.clears[9], true);
		data.depth = preparation.CaptureView(bindings.depth,
			{org::BindlessViewKind::DepthStencil});
		return data;
	}

	void ShutdownPass() {
		// Cleanup the render pass
	}
	static void Record(const ClearVisibilityBindings&, const ClearVisibilityFrameData& data,
		org::PassRecordContext& recording) {
		auto& commands = recording.Commands();
		commands.SetDescriptorHeaps(data.textureHeap, data.samplerHeap);
		for (const auto& clear : data.clears) {
			rhi::UavClearInfo info{};
			info.resource = recording.Resolve(clear.resource);
			info.cpuVisible = recording.Resolve(clear.cpu);
			info.shaderVisible = recording.Resolve(clear.gpu);
			if (clear.integer) {
				rhi::UavClearUint value{};
				value.v[0] = value.v[1] = 0xFFFFFFFF;
				commands.ClearUavUint(info, value);
			} else {
				commands.ClearUavFloat(info, {});
			}
		}
		commands.ClearDepthStencilView(recording.Resolve(data.depth), true, false, 1.0f, 0);
	}

};
