#pragma once

#include <span>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Scene/Scene.h"
#include "Utilities/Utilities.h"
#include "../shaders/PerPassRootConstants/bloomBlendRootConstants.h"
#include "Resources/PixelBuffer.h"

struct BloomBlendBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken bloomMip1, bloomMip2, target;
};
struct BloomBlendFrame {
    uint32_t width = 0, height = 0;
};

class BloomBlendPass : public org::TypedRenderGraphPass<BloomBlendPass,
    BloomBlendFrame, BloomBlendBindings> {
public:

    BloomBlendPass() {
        CreatePSO();
    }

    BloomBlendBindings Declare(org::PassBuilder& builder) {
        const org::SrvView bloomViews[] = {{UINT32_MAX, 1}, {UINT32_MAX, 2}};
        auto bloom = builder.ShaderResource(Subresources(Builtin::PostProcessing::BloomTexture, org::Mip{1, 2}), std::span<const org::SrvView>{bloomViews});
        return {m_pso, bloom.View(0), bloom.View(1), builder.RenderTarget(Subresources(Builtin::PostProcessing::UpscaledHDR, org::Mip{0, 1}))};
    }

    BloomBlendFrame Prepare(const BloomBlendBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        const auto& targetDesc = preparation.Describe(bindings.target);
        return {targetDesc.texture.width, targetDesc.texture.height};
    }

    static void Record(const BloomBlendBindings& bindings, const BloomBlendFrame& frame,
        org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[BLOOM_LOW_SOURCE_SRV_DESCRIPTOR_INDEX] = context.Index(bindings.bloomMip2);
        constants[BLOOM_SOURCE_SRV_DESCRIPTOR_INDEX] = context.Index(bindings.bloomMip1);
        constants[DST_WIDTH] = frame.width;
        constants[DST_HEIGHT] = frame.height;
        constants[BLOOM_BLEND_FILTER_RADIUS] = as_uint(0.001f);
        constants[BLOOM_BLEND_ASPECT_RATIO] = as_uint(frame.width / static_cast<float>(frame.height));
        br::render::DrawFullscreen(context, bindings.pipeline, constants, br::render::ColorTarget(context, bindings.target),
            frame.width, frame.height);
    }

    void ShutdownPass() {
        // Cleanup the render pass
    }

private:

    unsigned int m_mipIndex;
    bool m_isUpsample = false;

    org::PipelineState m_pso;

    void CreatePSO() {
        auto dev = DeviceManager::GetInstance().GetDevice();

        // 1) Compile shaders (same as before)
        ShaderInfoBundle sib;
        sib.vertexShader = { L"shaders/fullscreenVS.hlsli", L"FullscreenVSNoViewRayMain", L"vs_6_6" };
        sib.pixelShader = { L"shaders/PostProcessing/bloomBlend.hlsl", L"blend", L"ps_6_6" };

        auto compiled = PSOManager::GetInstance().CompileShaders(sib);

        auto& layout = PSOManager::GetInstance().GetRootSignature(); // rhi::PipelineLayout&
        rhi::SubobjLayout soLayout{ layout.GetHandle() };
        rhi::SubobjShader soVS{ rhi::ShaderStage::Vertex, rhi::DXIL(compiled.vertexShader.Get()), "FullscreenVSNoViewRayMain" };
        rhi::SubobjShader soPS{ rhi::ShaderStage::Pixel,  rhi::DXIL(compiled.pixelShader.Get()), "blend" };

        rhi::RasterState rs{};
        rs.fill = rhi::FillMode::Solid;
        rs.cull = rhi::CullMode::None; // fullscreen triangle: no culling
        rs.frontCCW = false;
        rhi::SubobjRaster soRaster{ rs };

        rhi::BlendState bs{};
        bs.alphaToCoverage = false;
        bs.independentBlend = false;
        bs.numAttachments = 1;
        auto& blend = bs.attachments[0];
        blend.enable = true;
        blend.srcColor = rhi::BlendFactor::SrcAlpha;
        blend.dstColor = rhi::BlendFactor::InvSrcAlpha;
        blend.colorOp = rhi::BlendOp::Add;
        blend.srcAlpha = rhi::BlendFactor::Zero;
        blend.dstAlpha = rhi::BlendFactor::One;
        blend.alphaOp = rhi::BlendOp::Add;
        blend.writeMask = rhi::ColorWriteEnable::All;
        rhi::SubobjBlend soBlend{ bs };

        rhi::RenderTargets rts{};
        rts.count = 1;
        rts.formats[0] = rhi::Format::R16G16B16A16_Float;
        rhi::SubobjRTVs soRTVs{ rts };

        rhi::SubobjSample soSample{ rhi::SampleDesc{1, 0} };

        rhi::SubobjPrimitiveTopology soTopo{ rhi::PrimitiveTopology::TriangleStrip };

        const rhi::PipelineStreamItem items[] = {
            rhi::Make(soLayout),
            rhi::Make(soVS),
            rhi::Make(soPS),
            rhi::Make(soRaster),
            rhi::Make(soBlend),
            rhi::Make(soRTVs),
            rhi::Make(soSample),
			rhi::Make(soTopo)
        };

        // 3) Create PSO
        rhi::PipelinePtr pipeline;
        auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), pipeline);
        if (Failed(result)) {
            throw std::runtime_error("Failed to create upsample PSO (RHI)");
        }
        pipeline->SetName("BloomBlend (RHI)");
        m_pso = org::PipelineState(std::move(pipeline), compiled.resourceIDsHash,
            compiled.resourceDescriptorSlots, PSOManager::GetInstance().CaptureLayoutOwner(soLayout.layout),
            soLayout.layout);
    }
};
