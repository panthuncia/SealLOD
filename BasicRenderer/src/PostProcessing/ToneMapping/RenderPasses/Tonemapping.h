#pragma once

#include <unordered_map>
#include <functional>

#include <spdlog/spdlog.h>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Scene/Scene.h"
#include "Materials/colorspaces.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

#include "../shaders/FidelityFX/ffx_a.h"
A_STATIC AF1 fs2S;
A_STATIC AF1 hdr10S;
A_STATIC AU1 ctl[24 * 4];

A_STATIC void LpmSetupOut(AU1 i, inAU4 v)
{
    for (int j = 0; j < 4; ++j) { ctl[i * 4 + j] = v[j]; }
}
#include "../shaders/FidelityFX/ffx_lpm.h"
#include "../shaders/PerPassRootConstants/tonemapRootConstants.h"

struct TonemappingBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken lpm, bloomMip1, bloomMip2, target;
    bool bloomEnabled = false;
};
struct TonemappingFrame {
    uint32_t tonemapType = 0;
    uint32_t width = 0, height = 0;
};

class TonemappingPass : public org::TypedRenderGraphPass<
    TonemappingPass, TonemappingFrame, TonemappingBindings> {
public:
	explicit TonemappingPass(bool bloomEnabled = false)
        : m_bloomEnabled(bloomEnabled) {
        m_pLPMConstants = org::LazyDynamicStructuredBuffer<LPMConstants>::CreateShared(1, "AMD LPM constants", 1, true);
	}

    std::shared_ptr<org::Resource> ProvideResource(org::ResourceIdentifier const& key) override {
        if (key == m_providedResources[0]) {
			return m_pLPMConstants;
        }
		return nullptr;
    }
    std::vector<org::ResourceIdentifier> GetSupportedKeys() override {
		return m_providedResources;
    }

    TonemappingBindings Declare(org::PassBuilder& builder) {
        builder.ShaderResource(Builtin::PostProcessing::UpscaledHDR, Builtin::CameraBuffer);
        TonemappingBindings bindings{};
        bindings.pipeline = builder.Pipeline(PipelineDesc());
        bindings.bloomEnabled = m_bloomEnabled;
        bindings.target = builder.RenderTarget(org::ResourceIdentifier{Builtin::PresentationColor}).View();
        bindings.lpm = builder.ShaderResource(m_pLPMConstants).View();
        if (m_bloomEnabled) {
            const std::array views{org::SrvView{UINT32_MAX, 1}, org::SrvView{UINT32_MAX, 2}};
            auto bloom = builder.ShaderResource(Subresources(Builtin::PostProcessing::BloomTexture, org::Mip{ 1, 2 }),
                std::span<const org::SrvView>(views));
            bindings.bloomMip1 = bloom.View(0);
            bindings.bloomMip2 = bloom.View(1);
        }
		builder.ConstantBuffer(Builtin::PerFrameBuffer);
        return bindings;
    }

	void Initialize() {
        LPMConstants lpmConstants = {};

        lpmConstants.shoulder = true;
        lpmConstants.con = false;
        lpmConstants.soft = false;
        lpmConstants.con2 = false;
        lpmConstants.clip = true;
        lpmConstants.scaleOnly = false;

        // Rest will be filled in by the luminanceHistogramAverage shader

        UploadBufferData(&lpmConstants, sizeof(LPMConstants),
            org::runtime::UploadTarget::FromShared(m_pLPMConstants), 0);
    }

	TonemappingFrame Prepare(const TonemappingBindings&,
        const org::PassPrepareContext& preparation) const {
		const auto* context = preparation.preparationData->Get<UpdateContext>();
		return {static_cast<uint32_t>(context->tonemapType), context->outputResolution.x, context->outputResolution.y};
	}

    static void Record(const TonemappingBindings& bindings, const TonemappingFrame& frame,
        org::PassRecordContext& context) {
		br::render::MiscConstants constants{};
		constants[LPM_CONSTANTS_BUFFER_SRV_DESCRIPTOR_INDEX] = context.Index(bindings.lpm);
		constants[TONEMAP_TYPE] = frame.tonemapType; constants[TONEMAP_BLOOM_ENABLED] = bindings.bloomEnabled ? 1u : 0u;
		if (bindings.bloomEnabled) {
			constants[TONEMAP_BLOOM_MIP1_SRV_DESCRIPTOR_INDEX] = context.Index(bindings.bloomMip1);
			constants[TONEMAP_BLOOM_MIP2_SRV_DESCRIPTOR_INDEX] = context.Index(bindings.bloomMip2);
			constants[TONEMAP_BLOOM_FILTER_RADIUS] = as_uint(0.001f);
			constants[TONEMAP_BLOOM_ASPECT_RATIO] = as_uint(frame.width / static_cast<float>(frame.height));
		}
		auto color = br::render::ColorTarget(context, bindings.target, rhi::LoadOp::Clear);
		color.clear.rgba[3] = 1.0f;
		br::render::DrawFullscreen(context, bindings.pipeline, constants, color, frame.width, frame.height);
	}

private:

    std::shared_ptr<org::LazyDynamicStructuredBuffer<LPMConstants>> m_pLPMConstants;

    bool m_bloomEnabled = false;

    std::vector<org::ResourceIdentifier> m_providedResources = {
		"FFX::LPMConstants"
	};

    static org::GraphicsPipelineDesc PipelineDesc() {
        org::GraphicsPipelineDesc desc;
        desc.name = "Tonemapping.PSO";
        desc.vertex = {L"shaders/fullscreenVS.hlsli", L"FullscreenVSNoViewRayMain", L"vs_6_6"};
        desc.pixel = {L"shaders/PostProcessing/tonemapping.hlsl", L"PSMain", L"ps_6_6"};

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
			a0.enable = false;
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
