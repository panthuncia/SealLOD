#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedFullscreenDraw.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Scene/Scene.h"
#include "Utilities/Utilities.h"
#include "../shaders/PerPassRootConstants/bloomSampleRootConstants.h"
#include <array>

struct BloomSamplePassInputs {
    unsigned int mipIndex;
    bool isUpsample;

    RG_DEFINE_PASS_INPUTS(BloomSamplePassInputs, &BloomSamplePassInputs::mipIndex, &BloomSamplePassInputs::isUpsample);
};

struct BloomSampleBindings {
    org::ResourceBindingToken source, target;
    uint32_t sourceMip = 0, targetMip = 0;
    bool upsample = false;
};

class BloomSamplePass : public org::TypedRenderGraphPass<BloomSamplePass,
    br::render::PreparedFullscreenDraw, BloomSampleBindings> {
public:
    // mipIndex selects which mip is used as render target, and which is used as shader resource.
    // E.g. DownsamplePassIndex 0 will downsample from mip 0 to mip 1, and use mip 1 as the render target.
    // If isUpsample is true, it will upsample from mip 1 to mip 0.
    BloomSamplePass() {
        CreatePSO();
    }

    BloomSampleBindings Declare(org::PassBuilder& builder) {
		auto inputs = Inputs<BloomSamplePassInputs>();
        const auto targetMip = inputs.mipIndex + (inputs.isUpsample ? 0u : 1u);
        const auto sourceMip = !inputs.isUpsample && inputs.mipIndex == 0
            ? 0u : inputs.mipIndex + (inputs.isUpsample ? 1u : 0u);
        if (!inputs.isUpsample) {
            const auto source = inputs.mipIndex == 0
                ? Subresources(Builtin::PostProcessing::UpscaledHDR, org::Mip{ 0, 1 })
                : Subresources(Builtin::PostProcessing::BloomTexture, org::Mip{ inputs.mipIndex, 1 });
            return {builder.BindShaderResource(source),
                builder.BindRenderTarget(Subresources(Builtin::PostProcessing::BloomTexture, org::Mip{targetMip, 1})),
                sourceMip, targetMip, false};
        }
        return {builder.BindShaderResource(Subresources(Builtin::PostProcessing::BloomTexture, org::Mip{sourceMip, 1})),
            builder.BindRenderTarget(Subresources(Builtin::PostProcessing::BloomTexture, org::Mip{targetMip, 1})),
            sourceMip, targetMip, true};
    }



    br::render::PreparedFullscreenDraw Prepare(const BloomSampleBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        br::render::PreparedFullscreenDraw data{};
        data.resourceHeap = context->textureDescriptorHeap.GetHandle();
        data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
        data.targetMip = bindings.targetMip;
        data.targetResource = preparation.CaptureResource(bindings.target);
        data.renderTargetReference = preparation.CaptureView(bindings.target,
            {org::BindlessViewKind::RenderTarget, UINT32_MAX, bindings.targetMip});
        br::render::BindPreparedProgram(data, preparation, bindings.upsample ? m_upsamplePso : m_downsamplePso);
        data.constantStage = rhi::ShaderStage::AllGraphics;
        const auto& targetDesc = preparation.Describe(bindings.target);
        const auto& sourceDesc = preparation.Describe(bindings.source);
        data.width = targetDesc.texture.width >> bindings.targetMip;
        data.height = targetDesc.texture.height >> bindings.targetMip;
        data.loadOp = bindings.upsample ? rhi::LoadOp::Load : rhi::LoadOp::DontCare;
        data.constants[SOURCE_TEXTURE_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.source,
            {org::BindlessViewKind::ShaderResource, UINT32_MAX, bindings.sourceMip}).index;
        data.constants[MIP_WIDTH] = sourceDesc.texture.width >> bindings.sourceMip;
        data.constants[MIP_HEIGHT] = sourceDesc.texture.height >> bindings.sourceMip;
        if (bindings.upsample) {
            data.constants[BLOOM_SAMPLE_FILTER_RADIUS] = as_uint(0.001f);
            data.constants[BLOOM_SAMPLE_ASPECT_RATIO] = as_uint(
                data.constants[MIP_WIDTH] / static_cast<float>(data.constants[MIP_HEIGHT]));
        } else {
            data.constants[SRC_TEXEL_SIZE_X] = as_uint(1.0f / data.constants[MIP_WIDTH]);
            data.constants[SRC_TEXEL_SIZE_Y] = as_uint(1.0f / data.constants[MIP_HEIGHT]);
        }
        return data;
    }

    void ShutdownPass() {
        // Cleanup the render pass
    }

    static void Record(const BloomSampleBindings&, const br::render::PreparedFullscreenDraw& data,
        org::PassRecordContext& recording) {
        br::render::RecordPreparedFullscreenDraw(data, recording);
    }

private:
    org::PipelineState m_downsamplePso;
    org::PipelineState m_upsamplePso;


    void CreatePSOForBackend(org::BackendInstanceId backendInstance) {
        auto& deviceManager = DeviceManager::GetInstance();
        auto dev = backendInstance == org::BackendInstanceId::Primary
            ? deviceManager.GetDevice() : deviceManager.GetPeerDevice();
        if (!dev) return;

        ShaderInfoBundle sib;
        sib.vertexShader = { L"shaders/fullscreenVS.hlsli", L"FullscreenVSNoViewRayMain", L"vs_6_6" };
        sib.pixelShader = { L"shaders/PostProcessing/bloomDownsample.hlsl", L"downsample", L"ps_6_6" };
        auto compiled = PSOManager::GetInstance().CompileShaders(sib, backendInstance);


        auto& layout = PSOManager::GetInstance().GetRootSignature(backendInstance);
        rhi::SubobjLayout soLayout{ layout.GetHandle() };
        rhi::SubobjShader soVS{ rhi::ShaderStage::Vertex, rhi::DXIL(compiled.vertexShader.Get()), "FullscreenVSNoViewRayMain" };

        rhi::RasterState rs{};
        rs.fill = rhi::FillMode::Solid;
        rs.cull = rhi::CullMode::None;    // fullscreen triangle
        rs.frontCCW = false;
        rhi::SubobjRaster soRaster{ rs };

        rhi::DepthStencilState ds{};
        ds.depthEnable = false;
        ds.depthWrite = false;
        rhi::SubobjDepth soDepth{ ds };

        rhi::RenderTargets rts{};
        rts.count = 1;
        rts.formats[0] = rhi::Format::R16G16B16A16_Float; // RTVFormats[0]
        rhi::SubobjRTVs soRTVs{ rts };

        rhi::SubobjDSV soDSV{ rhi::Format::D32_Float };   // matches DX psoDesc.DSVFormat
        rhi::SubobjSample soSample{ rhi::SampleDesc{1, 0} };

        rhi::SubobjShader soPS_down{ rhi::ShaderStage::Pixel, rhi::DXIL(compiled.pixelShader.Get()), "downsample" };

        rhi::BlendState bsDown{};
        bsDown.alphaToCoverage = false;
        bsDown.independentBlend = false;
        bsDown.numAttachments = 1;
        bsDown.attachments[0].enable = false; // disabled
        rhi::SubobjBlend soBlendDown{ bsDown };
        rhi::SubobjPrimitiveTopology soTopo{ rhi::PrimitiveTopology::TriangleStrip };

        {
            const rhi::PipelineStreamItem items[] = {
                rhi::Make(soLayout),
                rhi::Make(soVS),
                rhi::Make(soPS_down),
                rhi::Make(soRaster),
                rhi::Make(soBlendDown),
                rhi::Make(soDepth),
                rhi::Make(soRTVs),
                rhi::Make(soDSV),
                rhi::Make(soSample),
				rhi::Make(soTopo),
            };

            rhi::PipelinePtr pipeline;
            auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), pipeline);
            if (Failed(result)) {
                throw std::runtime_error("Failed to create bloom downsample PSO (RHI)");
            }
            pipeline->SetName("Bloom.Downsample");
            m_downsamplePso.AttachBackendPipeline(
                backendInstance, std::move(pipeline), compiled.resourceIDsHash,
                compiled.resourceDescriptorSlots, PSOManager::GetInstance().CaptureLayoutOwner(soLayout.layout), soLayout.layout);
        }


        sib.pixelShader = { L"shaders/PostProcessing/bloomUpsample.hlsl", L"upsample", L"ps_6_6" };
        auto compiledUp = PSOManager::GetInstance().CompileShaders(sib, backendInstance);
        rhi::SubobjShader soPS_up{ rhi::ShaderStage::Pixel, rhi::DXIL(compiledUp.pixelShader.Get()), "upsample" };

        rhi::BlendState bsUp{};
        bsUp.alphaToCoverage = false;
        bsUp.independentBlend = false;
        bsUp.numAttachments = 1;
        {
            auto& a0 = bsUp.attachments[0];
            a0.enable = true;
            a0.srcColor = rhi::BlendFactor::One;
            a0.dstColor = rhi::BlendFactor::One;
            a0.colorOp = rhi::BlendOp::Add;

            a0.srcAlpha = rhi::BlendFactor::One;
            a0.dstAlpha = rhi::BlendFactor::Zero;
            a0.alphaOp = rhi::BlendOp::Add;

            a0.writeMask = rhi::ColorWriteEnable::All;
        }
        rhi::SubobjBlend soBlendUp{ bsUp };

        {
            const rhi::PipelineStreamItem items[] = {
                rhi::Make(soLayout),
                rhi::Make(soVS),
                rhi::Make(soPS_up),
                rhi::Make(soRaster),
                rhi::Make(soBlendUp),
                rhi::Make(soDepth),
                rhi::Make(soRTVs),
                rhi::Make(soDSV),
                rhi::Make(soSample),
				rhi::Make(soTopo),
            };

            rhi::PipelinePtr pipeline;
            auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), pipeline);
            if (Failed(result)) {
                throw std::runtime_error("Failed to create bloom upsample PSO (RHI)");
            }
            pipeline->SetName("Bloom.Upsample");
            m_upsamplePso.AttachBackendPipeline(
                backendInstance, std::move(pipeline), compiledUp.resourceIDsHash,
                compiledUp.resourceDescriptorSlots, PSOManager::GetInstance().CaptureLayoutOwner(soLayout.layout), soLayout.layout);
        }
    }

    void CreatePSO() {
        CreatePSOForBackend(org::BackendInstanceId::Primary);
        if (DeviceManager::GetInstance().IsMultiRHIEnabled()) {
            CreatePSOForBackend(org::BackendInstanceId::Peer);
        }
    }
};
