#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedFullscreenDraw.h"

#include <string>

struct BRDFIntegrationBindings { org::ResourceBindingToken target; };

class BRDFIntegrationPass
    : public org::TypedRenderGraphPass<BRDFIntegrationPass,
          br::render::PreparedFullscreenDraw, BRDFIntegrationBindings> {
public:
    BRDFIntegrationPass() {
        CreatePSO();
    }

    BRDFIntegrationBindings Declare(org::PassBuilder& builder) {
        return {builder.BindRenderTarget(org::ResourceIdentifier{Builtin::BRDFLUT})};
    }

    br::render::PreparedFullscreenDraw Prepare(const BRDFIntegrationBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        br::render::PreparedFullscreenDraw data{};
        data.renderTargetReference = preparation.CaptureView(bindings.target,
            {org::BindlessViewKind::RenderTarget});
        data.loadOp = rhi::LoadOp::Clear;
        data.clear = preparation.ClearValue(bindings.target);
        const auto& desc = preparation.Describe(bindings.target);
        data.width = desc.texture.width; data.height = desc.texture.height;
        data.debugName = "BRDF Integration Pass";
        br::render::BindPreparedProgram(
            data, preparation, PSO);
        return data;
    }

    static void Record(const BRDFIntegrationBindings&, const br::render::PreparedFullscreenDraw& data,
        org::PassRecordContext& recording) {
        br::render::RecordPreparedFullscreenDraw(data, recording);
    }

private:
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
