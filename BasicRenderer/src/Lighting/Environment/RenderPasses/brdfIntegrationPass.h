#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

#include <string>

struct BRDFIntegrationBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken target;
};
struct BRDFIntegrationFrame {
    rhi::ClearValue clear{};
    uint32_t width = 0, height = 0;
};

class BRDFIntegrationPass
    : public org::TypedRenderGraphPass<BRDFIntegrationPass,
          BRDFIntegrationFrame, BRDFIntegrationBindings> {
public:
    // Renders the LUT into `target` once, in a graph of its own (Renderer::EnsureBrdfLut).
    explicit BRDFIntegrationPass(std::shared_ptr<org::PixelBuffer> target) : m_target(std::move(target)) {}

    BRDFIntegrationBindings Declare(org::PassBuilder& builder) {
        return {builder.Pipeline(PipelineDesc()), builder.RenderTarget(m_target).View()};
    }

    BRDFIntegrationFrame Prepare(const BRDFIntegrationBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        BRDFIntegrationFrame frame{};
        frame.clear = preparation.ClearValue(bindings.target.Resource());
        const auto& desc = preparation.Describe(bindings.target);
        frame.width = desc.texture.width; frame.height = desc.texture.height;
        return frame;
    }

    static void Record(const BRDFIntegrationBindings& bindings, const BRDFIntegrationFrame& frame,
        org::PassRecordContext& context) {
        auto color = br::render::ColorTarget(context, bindings.target, rhi::LoadOp::Clear);
        color.clear = frame.clear;
        br::render::DrawFullscreen(context, bindings.pipeline, {}, color, frame.width, frame.height,
            "BRDF Integration Pass");
    }

private:
    std::shared_ptr<org::PixelBuffer> m_target;

    static org::GraphicsPipelineDesc PipelineDesc() {
        org::GraphicsPipelineDesc desc;
        desc.name = "BRDFIntegration.PSO";
        desc.vertex = {L"shaders/fullscreenVS.hlsli", L"FullscreenVSNoViewRayMain", L"vs_6_6"};
        desc.pixel = {L"shaders/brdfIntegration.hlsl", L"PSMain", L"ps_6_6"};
        desc.topology = rhi::PrimitiveTopology::TriangleStrip;

        rhi::RasterState rs{};
        rs.fill = rhi::FillMode::Solid;
        rs.cull = rhi::CullMode::None;
        rs.frontCCW = false;
        desc.raster = rs;

        rhi::BlendState bs{};
        bs.alphaToCoverage = false;
        bs.independentBlend = false;
        bs.numAttachments = 1;
        bs.attachments[0].enable = false;                    // no blending
        bs.attachments[0].writeMask = rhi::ColorWriteEnable::All;
        desc.blend = bs;

        rhi::DepthStencilState ds{};
        ds.depthEnable = false;      // depth disabled (write mask ignored)
        ds.depthWrite = false;
        ds.depthFunc = rhi::CompareOp::Less;
        desc.depth = ds;

        rhi::RenderTargets rts{};
        rts.count = 1;
        rts.formats[0] = rhi::Format::R16G16_Float;
        desc.renderTargets = rts;

        desc.depthFormat = rhi::Format::D32_Float;
        desc.sample = rhi::SampleDesc{1, 0};
        return desc;
    }
};
