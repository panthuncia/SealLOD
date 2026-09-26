#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedFullscreenDraw.h"

struct DebugResolveBindings { org::ResourceBindingToken target; };

class DebugResolvePass
    : public org::TypedRenderGraphPass<DebugResolvePass,
          br::render::PreparedFullscreenDraw, DebugResolveBindings> {
public:
	DebugResolvePass() {
		CreatePSO();
	}

	DebugResolveBindings Declare(org::PassBuilder& builder) {
		builder.WithShaderResource(Builtin::DebugVisualization, Builtin::CameraBuffer);
		builder.WithConstantBuffer(Builtin::PerFrameBuffer);
		return {builder.BindRenderTarget(org::ResourceIdentifier{Builtin::PresentationColor})};
	}

	br::render::PreparedFullscreenDraw Prepare(const DebugResolveBindings& bindings,
		const org::PassPrepareContext& preparation) const {
		const auto* context = preparation.preparationData->Get<UpdateContext>();
		br::render::PreparedFullscreenDraw data{};
		data.targetResource = preparation.CaptureResource(bindings.target);
		data.renderTargetReference = preparation.CaptureView(
			bindings.target, {org::BindlessViewKind::RenderTarget});
		data.loadOp = rhi::LoadOp::Load;
		data.width = context->outputResolution.x; data.height = context->outputResolution.y;

		br::render::BindPreparedProgram(
			data, preparation, m_pso);
		return data;
	}

	static void Record(const DebugResolveBindings&, const br::render::PreparedFullscreenDraw& data,
		org::PassRecordContext& recording) {
		br::render::RecordPreparedFullscreenDraw(data, recording);
	}

private:
	org::PipelineState m_pso;

	void CreatePSO() {
		auto dev = DeviceManager::GetInstance().GetDevice();

		ShaderInfoBundle sib;
		sib.vertexShader = { L"shaders/fullscreenVS.hlsli", L"FullscreenVSNoViewRayMain", L"vs_6_6" };
		sib.pixelShader = { L"shaders/PostProcessing/debugResolve.hlsl", L"PSMain", L"ps_6_6" };
		auto compiled = PSOManager::GetInstance().CompileShaders(sib);

		auto& layout = PSOManager::GetInstance().GetRootSignature();
		rhi::SubobjLayout soLayout{ layout.GetHandle() };
		rhi::SubobjShader soVS{ rhi::ShaderStage::Vertex, rhi::DXIL(compiled.vertexShader.Get()), "FullscreenVSNoViewRayMain" };
		rhi::SubobjShader soPS{ rhi::ShaderStage::Pixel, rhi::DXIL(compiled.pixelShader.Get()), "PSMain" };

		rhi::RasterState rs{};
		rs.fill = rhi::FillMode::Solid;
		rs.cull = rhi::CullMode::None;
		rs.frontCCW = false;
		rhi::SubobjRaster soRaster{ rs };

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
		rhi::SubobjBlend soBlend{ bs };

		rhi::DepthStencilState ds{};
		ds.depthEnable = false;
		ds.depthWrite = false;
		ds.depthFunc = rhi::CompareOp::Greater;
		rhi::SubobjDepth soDepth{ ds };

		rhi::RenderTargets rts{};
		rts.count = 1;
		rts.formats[0] = rhi::Format::R8G8B8A8_UNorm;
		rhi::SubobjRTVs soRTVs{ rts };

		rhi::SubobjDSV    soDSV{ rhi::Format::D32_Float };
		rhi::SubobjSample soSmp{ rhi::SampleDesc{1, 0} };
		rhi::SubobjPrimitiveTopology soTopo{ rhi::PrimitiveTopology::TriangleStrip };

		const rhi::PipelineStreamItem items[] = {
			rhi::Make(soLayout),
			rhi::Make(soVS),
			rhi::Make(soPS),
			rhi::Make(soRaster),
			rhi::Make(soBlend),
			rhi::Make(soDepth),
			rhi::Make(soRTVs),
			rhi::Make(soDSV),
			rhi::Make(soSmp),
			rhi::Make(soTopo)
		};

		rhi::PipelinePtr pipeline;
		auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), pipeline);
		if (Failed(result)) {
			throw std::runtime_error("Failed to create DebugResolve PSO");
		}
		pipeline->SetName("DebugResolve.PSO");
		m_pso = org::PipelineState(std::move(pipeline), compiled.resourceIDsHash,
            compiled.resourceDescriptorSlots, PSOManager::GetInstance().CaptureLayoutOwner(soLayout.layout),
            soLayout.layout);
	}
};
