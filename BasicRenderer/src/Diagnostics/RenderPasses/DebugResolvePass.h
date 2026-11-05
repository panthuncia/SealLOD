#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

struct DebugResolveBindings {
	org::PipelineState pipeline;
	org::DeclaredViewToken target;
};
struct DebugResolveFrame {
	uint32_t width = 0, height = 0;
};

class DebugResolvePass
    : public org::TypedRenderGraphPass<DebugResolvePass,
          DebugResolveFrame, DebugResolveBindings> {
public:
	DebugResolveBindings Declare(org::PassBuilder& builder) {
		builder.ShaderResource(Builtin::DebugVisualization, Builtin::CameraBuffer);
		builder.ConstantBuffer(Builtin::PerFrameBuffer);
		return {builder.Pipeline(PipelineDesc()), builder.RenderTarget(org::ResourceIdentifier{Builtin::PresentationColor})};
	}

	DebugResolveFrame Prepare(const DebugResolveBindings&,
		const org::PassPrepareContext& preparation) const {
		const auto* context = preparation.preparationData->Get<UpdateContext>();
		return {context->outputResolution.x, context->outputResolution.y};
	}

	static void Record(const DebugResolveBindings& bindings, const DebugResolveFrame& frame,
		org::PassRecordContext& context) {
		br::render::DrawFullscreen(context, bindings.pipeline, {}, br::render::ColorTarget(context, bindings.target),
			frame.width, frame.height);
	}

private:
	static org::GraphicsPipelineDesc PipelineDesc() {
		org::GraphicsPipelineDesc desc;
		desc.name = "DebugResolve.PSO";
		desc.vertex = {L"shaders/fullscreenVS.hlsli", L"FullscreenVSNoViewRayMain", L"vs_6_6"};
		desc.pixel = {L"shaders/PostProcessing/debugResolve.hlsl", L"PSMain", L"ps_6_6"};

		rhi::RasterState rs{};
		rs.fill = rhi::FillMode::Solid;
		rs.cull = rhi::CullMode::None;
		rs.frontCCW = false;
		desc.raster = rs;

		rhi::BlendState bs{};
		bs.alphaToCoverage = false;
		bs.independentBlend = false;
		bs.numAttachments = 1;
		{
			auto& a0 = bs.attachments[0];
			a0.enable = true;
			a0.srcColor = rhi::BlendFactor::SrcAlpha;
			a0.dstColor = rhi::BlendFactor::InvSrcAlpha;
			a0.colorOp = rhi::BlendOp::Add;
			a0.srcAlpha = rhi::BlendFactor::One;
			a0.dstAlpha = rhi::BlendFactor::InvSrcAlpha;
			a0.alphaOp = rhi::BlendOp::Add;
			a0.writeMask = rhi::ColorWriteEnable::All;
		}
		desc.blend = bs;

		rhi::DepthStencilState ds{};
		ds.depthEnable = false;
		ds.depthWrite = false;
		ds.depthFunc = rhi::CompareOp::Greater;
		desc.depth = ds;

		rhi::RenderTargets rts{};
		rts.count = 1;
		rts.formats[0] = rhi::Format::R8G8B8A8_UNorm;
		desc.renderTargets = rts;

		desc.depthFormat = rhi::Format::D32_Float;
		desc.sample = rhi::SampleDesc{1, 0};
		desc.topology = rhi::PrimitiveTopology::TriangleStrip;
		return desc;
	}
};
