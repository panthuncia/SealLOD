#pragma once

#include <functional>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Scene/Scene.h"

struct SpecularIBLBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken target;
    bool gtaoEnabled = false;
};
struct SpecularIBLFrame {
    uint32_t width = 0, height = 0;
};

class SpecularIBLPass : public org::TypedRenderGraphPass<SpecularIBLPass,
    SpecularIBLFrame, SpecularIBLBindings> {
public:
    SpecularIBLPass() {
        auto& settingsManager = SettingsManager::GetInstance();
        m_gtaoEnabled = settingsManager.getSettingGetter<bool>("enableGTAO")();
    }

    SpecularIBLBindings Declare(org::PassBuilder& builder) {
        builder.ShaderResource(Builtin::PostProcessing::ScreenSpaceReflections,
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
			Builtin::OpenPBR::OpaqueDielectricAverageEnergyComplement,
            Builtin::CameraBuffer);
        builder.ShaderResource(Builtin::OpenPBR::OpaqueDielectricEnergyComplement,
            org::SrvView{static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull)});
        builder.ConstantBuffer(Builtin::PerFrameBuffer);

        builder.UnorderedAccess(Builtin::DebugVisualization);

        if (m_gtaoEnabled) {
            builder.ShaderResource(Builtin::GTAO::OutputAOTerm);
        }
        return {builder.Pipeline(PipelineDesc()), builder.RenderTarget(org::ResourceIdentifier{Builtin::Color::HDRColorTarget}), m_gtaoEnabled};
    }

    void Initialize() {}

    SpecularIBLFrame Prepare(const SpecularIBLBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        const auto& desc = preparation.Describe(bindings.target);
        return {desc.texture.width, desc.texture.height};
    }

    static void Record(const SpecularIBLBindings& bindings, const SpecularIBLFrame& frame,
        org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[MiscEnableGTAO] = bindings.gtaoEnabled;
        br::render::DrawFullscreen(context, bindings.pipeline, constants, br::render::ColorTarget(context, bindings.target),
            frame.width, frame.height);
    }

    void ShutdownPass() {
        // Cleanup the render pass
    }

private:

    bool m_gtaoEnabled = true;

    static org::GraphicsPipelineDesc PipelineDesc() {
        org::GraphicsPipelineDesc desc;
        desc.name = "SpecularIBL.PSO";
        desc.vertex = {L"shaders/fullscreenVS.hlsli", L"FullscreenVSMain", L"vs_6_6"};
        desc.pixel = {L"shaders/specularIBL.hlsl",   L"PSMain",           L"ps_6_6"};

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
            a0.srcColor = rhi::BlendFactor::One;
            a0.dstColor = rhi::BlendFactor::One;
            a0.colorOp = rhi::BlendOp::Add;

            a0.srcAlpha = rhi::BlendFactor::Zero;
            a0.dstAlpha = rhi::BlendFactor::One;
            a0.alphaOp = rhi::BlendOp::Add;

            a0.writeMask = rhi::ColorWriteEnable::All;
        }
        desc.blend = bs;

        // No depth-stencil state and no DSV format.

        rhi::RenderTargets rts{};
        rts.count = 1;
        rts.formats[0] = rhi::Format::R16G16B16A16_Float;
        desc.renderTargets = rts;

        desc.sample = rhi::SampleDesc{1, 0};
        desc.topology = rhi::PrimitiveTopology::TriangleStrip;
        return desc;
    }
};
