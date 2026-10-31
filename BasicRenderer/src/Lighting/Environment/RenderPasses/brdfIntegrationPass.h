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
    explicit BRDFIntegrationPass(std::shared_ptr<org::PixelBuffer> target) : m_target(std::move(target)) {
        CreatePSO();
    }

    BRDFIntegrationBindings Declare(org::PassBuilder& builder) {
        return {PSO, builder.RenderTarget(m_target).View()};
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
    org::PipelineState PSO;

    void CreatePSO() {
        auto dev = DeviceManager::GetInstance().GetDevice();

        // Compile shaders
        ShaderInfoBundle sib;
        sib.vertexShader = { L"shaders/fullscreenVS.hlsli", L"FullscreenVSNoViewRayMain", L"vs_6_6" };
        sib.pixelShader = { L"shaders/brdfIntegration.hlsl", L"PSMain", L"ps_6_6" };
        auto compiled = PSOManager::GetInstance().CompileShaders(sib);

        // Subobjects
        auto& layout = PSOManager::GetInstance().GetRootSignature(); // rhi::PipelineLayout&
        rhi::SubobjLayout soLayout{ layout.GetHandle() };
        rhi::SubobjShader soVS{ rhi::ShaderStage::Vertex, rhi::DXIL(compiled.vertexShader.Get()), "FullscreenVSNoViewRayMain" };
        rhi::SubobjShader soPS{ rhi::ShaderStage::Pixel,  rhi::DXIL(compiled.pixelShader.Get()), "PSMain" };

        rhi::RasterState rs{};
        rs.fill = rhi::FillMode::Solid;
        rs.cull = rhi::CullMode::None;
        rs.frontCCW = false;
        rhi::SubobjRaster soRaster{ rs };

        rhi::BlendState bs{};
        bs.alphaToCoverage = false;
        bs.independentBlend = false;
        bs.numAttachments = 1;
        bs.attachments[0].enable = false;                    // no blending
        bs.attachments[0].writeMask = rhi::ColorWriteEnable::All;
        rhi::SubobjBlend soBlend{ bs };

        rhi::DepthStencilState ds{};
        ds.depthEnable = false;      // depth disabled (write mask ignored)
        ds.depthWrite = false;
        ds.depthFunc = rhi::CompareOp::Less;
        rhi::SubobjDepth soDepth{ ds };

        rhi::RenderTargets rts{};
        rts.count = 1;
        rts.formats[0] = rhi::Format::R16G16_Float;
        rhi::SubobjRTVs soRTVs{ rts };

        rhi::SubobjDSV soDSV{ rhi::Format::D32_Float };
        rhi::SubobjSample soSmp{ rhi::SampleDesc{1, 0} };
		rhi::SubobjPrimitiveTopology soTopo{ rhi::PrimitiveTopology::TriangleStrip };

        const rhi::PipelineStreamItem items[] = {
            rhi::Make(soLayout),
            rhi::Make(soVS),
            rhi::Make(soPS),
			rhi::Make(soTopo),
            rhi::Make(soRaster),
            rhi::Make(soBlend),
            rhi::Make(soDepth),
            rhi::Make(soRTVs),
            rhi::Make(soDSV),
            rhi::Make(soSmp),
        };

        rhi::PipelinePtr pipeline;
        auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), pipeline);
        if (Failed(result)) {
            throw std::runtime_error(
                std::string("Failed to create BRDF integration PSO (RHI): ") +
                rhi::ResultName(result) +
                " (" +
                std::to_string(static_cast<uint32_t>(result)) +
                ")");
        }
        pipeline->SetName("BRDFIntegration.PSO");
        PSO = org::PipelineState(std::move(pipeline), compiled.resourceIDsHash,
            compiled.resourceDescriptorSlots, PSOManager::GetInstance().CaptureLayoutOwner(soLayout.layout),
            soLayout.layout);
    }
};
