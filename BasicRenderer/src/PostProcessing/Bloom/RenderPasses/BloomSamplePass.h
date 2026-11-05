#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Scene/Scene.h"
#include "Utilities/Utilities.h"
#include "../shaders/PerPassRootConstants/bloomSampleRootConstants.h"
#include <array>

struct BloomSamplePassInputs {
    unsigned int mipIndex;
    bool isUpsample;

    RG_DEFINE_PASS_INPUTS(BloomSamplePassInputs, &BloomSamplePassInputs::mipIndex, &BloomSamplePassInputs::isUpsample);
};

struct BloomSampleBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken source, target;
    uint32_t sourceMip = 0, targetMip = 0;
    bool upsample = false;
};
struct BloomSampleFrame {
    uint32_t width = 0, height = 0;
    uint32_t sourceWidth = 0, sourceHeight = 0;
};

class BloomSamplePass : public org::TypedRenderGraphPass<BloomSamplePass,
    BloomSampleFrame, BloomSampleBindings> {
public:
    // mipIndex selects which mip is used as render target, and which is used as shader resource.
    // E.g. DownsamplePassIndex 0 will downsample from mip 0 to mip 1, and use mip 1 as the render target.
    // If isUpsample is true, it will upsample from mip 1 to mip 0.
    BloomSampleBindings Declare(org::PassBuilder& builder) {
		auto inputs = Inputs<BloomSamplePassInputs>();
        const auto targetMip = inputs.mipIndex + (inputs.isUpsample ? 0u : 1u);
        const auto sourceMip = !inputs.isUpsample && inputs.mipIndex == 0
            ? 0u : inputs.mipIndex + (inputs.isUpsample ? 1u : 0u);
        if (!inputs.isUpsample) {
            const auto source = inputs.mipIndex == 0
                ? Subresources(Builtin::PostProcessing::UpscaledHDR, org::Mip{ 0, 1 })
                : Subresources(Builtin::PostProcessing::BloomTexture, org::Mip{ inputs.mipIndex, 1 });
            return {builder.Pipeline(PipelineDesc(false)), builder.ShaderResource(source, {UINT32_MAX, sourceMip}),
                builder.RenderTarget(Subresources(Builtin::PostProcessing::BloomTexture, org::Mip{targetMip, 1}), {UINT32_MAX, targetMip}),
                sourceMip, targetMip, false};
        }
        return {builder.Pipeline(PipelineDesc(true)), builder.ShaderResource(Subresources(Builtin::PostProcessing::BloomTexture, org::Mip{sourceMip, 1}), {UINT32_MAX, sourceMip}),
            builder.RenderTarget(Subresources(Builtin::PostProcessing::BloomTexture, org::Mip{targetMip, 1}), {UINT32_MAX, targetMip}),
            sourceMip, targetMip, true};
    }



    BloomSampleFrame Prepare(const BloomSampleBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        const auto& targetDesc = preparation.Describe(bindings.target);
        const auto& sourceDesc = preparation.Describe(bindings.source);
        return {targetDesc.texture.width >> bindings.targetMip, targetDesc.texture.height >> bindings.targetMip,
            sourceDesc.texture.width >> bindings.sourceMip, sourceDesc.texture.height >> bindings.sourceMip};
    }

    void ShutdownPass() {
        // Cleanup the render pass
    }

    static void Record(const BloomSampleBindings& bindings, const BloomSampleFrame& frame,
        org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[SOURCE_TEXTURE_DESCRIPTOR_INDEX] = context.Index(bindings.source);
        constants[MIP_WIDTH] = frame.sourceWidth;
        constants[MIP_HEIGHT] = frame.sourceHeight;
        if (bindings.upsample) {
            constants[BLOOM_SAMPLE_FILTER_RADIUS] = as_uint(0.001f);
            constants[BLOOM_SAMPLE_ASPECT_RATIO] = as_uint(
                constants[MIP_WIDTH] / static_cast<float>(constants[MIP_HEIGHT]));
        } else {
            constants[SRC_TEXEL_SIZE_X] = as_uint(1.0f / constants[MIP_WIDTH]);
            constants[SRC_TEXEL_SIZE_Y] = as_uint(1.0f / constants[MIP_HEIGHT]);
        }
        const auto color = br::render::ColorTarget(context, bindings.target,
            bindings.upsample ? rhi::LoadOp::Load : rhi::LoadOp::DontCare, bindings.targetMip);
        br::render::DrawFullscreen(context, bindings.pipeline, constants, color, frame.width, frame.height);
    }

private:
    static org::GraphicsPipelineDesc PipelineDesc(bool upsample) {
        org::GraphicsPipelineDesc desc;
        desc.name = upsample ? "Bloom.Upsample" : "Bloom.Downsample";
        desc.vertex = {L"shaders/fullscreenVS.hlsli", L"FullscreenVSNoViewRayMain", L"vs_6_6"};
        desc.pixel = upsample
            ? org::PipelineShaderSource{L"shaders/PostProcessing/bloomUpsample.hlsl", L"upsample", L"ps_6_6"}
            : org::PipelineShaderSource{L"shaders/PostProcessing/bloomDownsample.hlsl", L"downsample", L"ps_6_6"};

        rhi::RasterState raster{};
        raster.fill = rhi::FillMode::Solid;
        raster.cull = rhi::CullMode::None; // fullscreen triangle
        raster.frontCCW = false;
        desc.raster = raster;

        rhi::BlendState blend{};
        blend.alphaToCoverage = false;
        blend.independentBlend = false;
        blend.numAttachments = 1;
        if (upsample) {
            auto& a0 = blend.attachments[0];
            a0.enable = true;
            a0.srcColor = rhi::BlendFactor::One;
            a0.dstColor = rhi::BlendFactor::One;
            a0.colorOp = rhi::BlendOp::Add;
            a0.srcAlpha = rhi::BlendFactor::One;
            a0.dstAlpha = rhi::BlendFactor::Zero;
            a0.alphaOp = rhi::BlendOp::Add;
            a0.writeMask = rhi::ColorWriteEnable::All;
        } else {
            blend.attachments[0].enable = false;
        }
        desc.blend = blend;

        rhi::DepthStencilState depth{};
        depth.depthEnable = false;
        depth.depthWrite = false;
        desc.depth = depth;

        rhi::RenderTargets targets{};
        targets.count = 1;
        targets.formats[0] = rhi::Format::R16G16B16A16_Float;
        desc.renderTargets = targets;

        desc.depthFormat = rhi::Format::D32_Float;
        desc.sample = rhi::SampleDesc{1, 0};
        desc.topology = rhi::PrimitiveTopology::TriangleStrip;
        return desc;
    }
};
