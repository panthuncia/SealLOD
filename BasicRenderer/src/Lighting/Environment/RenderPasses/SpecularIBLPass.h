#pragma once

#include <functional>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedFullscreenDraw.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Scene/Scene.h"

struct SpecularIBLBindings { org::ResourceBindingToken target; };

class SpecularIBLPass : public org::TypedRenderGraphPass<SpecularIBLPass,
    br::render::PreparedFullscreenDraw, SpecularIBLBindings> {
public:
    SpecularIBLPass() {
        CreatePSO();
        auto& settingsManager = SettingsManager::GetInstance();
        m_gtaoEnabled = settingsManager.getSettingGetter<bool>("enableGTAO")();
    }

    SpecularIBLBindings Declare(org::PassBuilder& builder) {
        builder.WithShaderResource(Builtin::PostProcessing::ScreenSpaceReflections,
            Builtin::Environment::InfoBuffer,
            Builtin::PerMaterialOpenPBRDataBuffer,
            Builtin::Surface::BaseColorOpacity,
            Builtin::Surface::NormalRoughness,
            Builtin::Surface::SpecularAo,
            Builtin::Surface::Emissive,
            Builtin::Surface::Payload0,
            Builtin::Surface::Payload1,
            Builtin::Surface::Identity,
            Builtin::Surface::Records,
            Builtin::PrimaryCamera::DepthTexture,
			Builtin::OpenPBR::FuzzLTC,
			Builtin::OpenPBR::IdealMetalEnergyComplement,
			Builtin::OpenPBR::OpaqueDielectricEnergyComplement,
			Builtin::OpenPBR::OpaqueDielectricAverageEnergyComplement,
            Builtin::CameraBuffer).WithConstantBuffer(Builtin::PerFrameBuffer);

        builder.WithUnorderedAccess(Builtin::DebugVisualization);

        if (m_gtaoEnabled) {
            builder.WithShaderResource(Builtin::GTAO::OutputAOTerm);
        }
        return {builder.BindRenderTarget(org::ResourceIdentifier{Builtin::Color::HDRColorTarget})};
    }

    void Initialize() {
		RegisterSRV(org::SRVViewType::Texture2DArrayFull, Builtin::OpenPBR::OpaqueDielectricEnergyComplement);
    }

    br::render::PreparedFullscreenDraw Prepare(const SpecularIBLBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        br::render::PreparedFullscreenDraw data{};
        data.resourceHeap = context->textureDescriptorHeap.GetHandle();
        data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
        data.renderTargetReference = preparation.CaptureView(bindings.target,
            {org::BindlessViewKind::RenderTarget});
        const auto& desc = preparation.Describe(bindings.target);
        data.width = desc.texture.width;
        data.height = desc.texture.height;
        data.constantStage = rhi::ShaderStage::AllGraphics;
        br::render::BindPreparedProgram(data, preparation, m_pso);
        data.constants[MiscEnableGTAO] = m_gtaoEnabled;
        return data;
    }

    static void Record(const SpecularIBLBindings&, const br::render::PreparedFullscreenDraw& data,
        org::PassRecordContext& recording) {
        br::render::RecordPreparedFullscreenDraw(data, recording);
    }

    void ShutdownPass() {
        // Cleanup the render pass
    }

private:

    org::PipelineState m_pso;

    bool m_gtaoEnabled = true;

    void CreatePSO() {
        auto dev = DeviceManager::GetInstance().GetDevice();

        // Compile shaders
        ShaderInfoBundle sib;
        sib.vertexShader = { L"shaders/fullscreenVS.hlsli", L"FullscreenVSMain", L"vs_6_6" };
        sib.pixelShader = { L"shaders/specularIBL.hlsl",   L"PSMain",           L"ps_6_6" };
        auto compiled = PSOManager::GetInstance().CompileShaders(sib);

        // Subobjects
        auto& layout = PSOManager::GetInstance().GetRootSignature(); // rhi::PipelineLayout&
        rhi::SubobjLayout soLayout{ layout.GetHandle() };
        rhi::SubobjShader soVS{ rhi::ShaderStage::Vertex, rhi::DXIL(compiled.vertexShader.Get()), "FullscreenVSMain" };
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
        {
            auto& a0 = bs.attachments[0];
            a0.enable = true;
            a0.srcColor = rhi::BlendFactor::One;
            a0.dstColor = rhi::BlendFactor::One;
            a0.colorOp = rhi::BlendOp::Add;

            a0.srcAlpha = rhi::BlendFactor::Zero;
            a0.dstAlpha = rhi::BlendFactor::One;
            a0.alphaOp = rhi::BlendOp::Add;

            a0.writeMask = rhi::ColorWriteEnable::All;
        }
        rhi::SubobjBlend soBlend{ bs };

        //rhi::DepthStencilState ds{};
        //ds.depthEnable = false;
        //ds.depthWrite = false;
        //ds.depthFunc = rhi::CompareOp::Greater; // kept for parity; ignored when depth off
        //rhi::SubobjDepth soDepth{ ds };

        rhi::RenderTargets rts{};
        rts.count = 1;
        rts.formats[0] = rhi::Format::R16G16B16A16_Float;
        rhi::SubobjRTVs soRTVs{ rts };

        //rhi::SubobjDSV    soDSV{ rhi::Format::Unknown }; // no DSV
        rhi::SubobjSample soSmp{ rhi::SampleDesc{1, 0} };
        rhi::SubobjPrimitiveTopology soTopo{ rhi::PrimitiveTopology::TriangleStrip };

        const rhi::PipelineStreamItem items[] = {
            rhi::Make(soLayout),
            rhi::Make(soVS),
            rhi::Make(soPS),
            rhi::Make(soRaster),
            rhi::Make(soBlend),
            //rhi::Make(soDepth),
            rhi::Make(soRTVs),
            //rhi::Make(soDSV),
            rhi::Make(soSmp),
			rhi::Make(soTopo)
        };

        rhi::PipelinePtr pipeline;
        auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), pipeline);
        if (Failed(result)) {
            throw std::runtime_error("Failed to create SpecularIBL PSO (RHI)");
        }
        pipeline->SetName("SpecularIBL.PSO");
        m_pso = org::PipelineState(std::move(pipeline), compiled.resourceIDsHash,
            compiled.resourceDescriptorSlots, PSOManager::GetInstance().CaptureLayoutOwner(soLayout.layout),
            soLayout.layout);
    }
};
