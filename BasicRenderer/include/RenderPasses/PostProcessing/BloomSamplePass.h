#pragma once

#include "RenderPasses/Base/RenderPass.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Scene/Scene.h"
#include "Utilities/Utilities.h"
#include "../shaders/PerPassRootConstants/bloomSampleRootConstants.h"

struct BloomSamplePassInputs {
    unsigned int mipIndex;
    bool isUpsample;

    RG_DEFINE_PASS_INPUTS(BloomSamplePassInputs, &BloomSamplePassInputs::mipIndex, &BloomSamplePassInputs::isUpsample);
};

class BloomSamplePass : public RenderPass {
public:
    // mipIndex selects which mip is used as render target, and which is used as shader resource.
    // E.g. DownsamplePassIndex 0 will downsample from mip 0 to mip 1, and use mip 1 as the render target.
    // If isUpsample is true, it will upsample from mip 1 to mip 0.
    BloomSamplePass() {
        CreatePSO();
    }

    void DeclareResourceUsages(RenderPassBuilder* builder) override {
		auto inputs = Inputs<BloomSamplePassInputs>();
		m_mipIndex = inputs.mipIndex;
        m_isUpsample = inputs.isUpsample;

        if (!m_isUpsample) {
            const auto source = m_mipIndex == 0
                ? Subresources(Builtin::PostProcessing::UpscaledHDR, Mip{ 0, 1 })
                : Subresources(Builtin::PostProcessing::BloomTexture, Mip{ m_mipIndex, 1 });
            builder->WithShaderResource(source)
                .WithRenderTarget(Subresources(Builtin::PostProcessing::BloomTexture, Mip{ m_mipIndex + 1, 1 }));
        }
        else {
            builder->WithShaderResource(Subresources(Builtin::PostProcessing::BloomTexture, Mip{ m_mipIndex + 1, 1 }))
                .WithRenderTarget(Subresources(Builtin::PostProcessing::BloomTexture, Mip{ m_mipIndex, 1 }));
        }
    }

    void Setup() override {
        m_pBloomTarget = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::PostProcessing::BloomTexture);
        if (!m_isUpsample && m_mipIndex == 0) {
            m_pUpscaledHDRTarget = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::PostProcessing::UpscaledHDR);
        }
    }

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto& psoManager = PSOManager::GetInstance();
        auto& commandList = executionContext.commandList;

		commandList.SetDescriptorHeaps(
            executionContext.GetResourceDescriptorHeap().GetHandle(),
            executionContext.GetSamplerDescriptorHeap().GetHandle());

        unsigned int mipOffset = m_isUpsample ? 0 : 1;

		rhi::PassBeginInfo passInfo{};
		rhi::ColorAttachment colorAttachment{};
		colorAttachment.rtv = executionContext.ResolveRTV(*m_pBloomTarget, m_mipIndex + mipOffset);
		colorAttachment.loadOp = m_isUpsample ? rhi::LoadOp::Load : rhi::LoadOp::DontCare;
		colorAttachment.mipSlice = m_mipIndex + mipOffset;
		colorAttachment.storeOp = rhi::StoreOp::Store;
		colorAttachment.resource = executionContext.Resolve(*m_pBloomTarget).GetHandle();
		passInfo.colors = { &colorAttachment };
        passInfo.width = m_pBloomTarget->GetWidth() >> (m_mipIndex + mipOffset);
        passInfo.height = m_pBloomTarget->GetHeight() >> (m_mipIndex + mipOffset);
		commandList.BeginPass(passInfo);

        commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleStrip);

        if (m_isUpsample) {
			commandList.BindPipeline(m_upsamplePso.GetAPIPipelineState(executionContext.backendInstance).GetHandle());
        }
        else {
			commandList.BindPipeline(m_downsamplePso.GetAPIPipelineState(executionContext.backendInstance).GetHandle());
        }

        auto rootSignature = psoManager.GetRootSignature(executionContext.backendInstance);
		commandList.BindLayout(rootSignature.GetHandle());

		BindResourceDescriptorIndices(commandList, m_resourceDescriptorBindings);

        unsigned int misc[NumMiscUintRootConstants] = {};
        const bool readsUpscaledHDR = !m_isUpsample && m_mipIndex == 0;
        PixelBuffer* source = readsUpscaledHDR ? m_pUpscaledHDRTarget : m_pBloomTarget;
        const unsigned int sourceMip = readsUpscaledHDR ? 0 : m_mipIndex + (m_isUpsample ? 1 : 0);
        misc[SOURCE_TEXTURE_DESCRIPTOR_INDEX] = source->GetSRVInfo(sourceMip).slot.index;
        misc[MIP_WIDTH] = source->GetWidth() >> sourceMip;
        misc[MIP_HEIGHT] = source->GetHeight() >> sourceMip;
        if (m_isUpsample) {
            misc[BLOOM_SAMPLE_FILTER_RADIUS] = as_uint(0.001f); // Kernel size
            misc[BLOOM_SAMPLE_ASPECT_RATIO] = as_uint(misc[MIP_WIDTH] / static_cast<float>(misc[MIP_HEIGHT])); // Aspect ratio
        }
        else {
            misc[SRC_TEXEL_SIZE_X] = as_uint(1.0f / misc[MIP_WIDTH]); // Texel size X
            misc[SRC_TEXEL_SIZE_Y] = as_uint(1.0f / misc[MIP_HEIGHT]); // Texel size Y
        }
		commandList.PushConstants(rhi::ShaderStage::AllGraphics, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, misc);

        commandList.Draw(3, 1, 0, 0); // Fullscreen triangle
        return {};
    }

    void Cleanup() override {
        // Cleanup the render pass
    }

private:

    unsigned int m_mipIndex;
    bool m_isUpsample = false;

    PipelineState m_downsamplePso;
    PipelineState m_upsamplePso;

	PixelBuffer* m_pBloomTarget = nullptr;
	PixelBuffer* m_pUpscaledHDRTarget = nullptr;

	PipelineResources m_resourceDescriptorBindings;

    void CreatePSOForBackend(BackendInstanceId backendInstance) {
        auto& deviceManager = DeviceManager::GetInstance();
        auto dev = backendInstance == BackendInstanceId::Primary
            ? deviceManager.GetDevice() : deviceManager.GetPeerDevice();
        if (!dev) return;

        ShaderInfoBundle sib;
        sib.vertexShader = { L"shaders/fullscreenVS.hlsli", L"FullscreenVSNoViewRayMain", L"vs_6_6" };
        sib.pixelShader = { L"shaders/PostProcessing/bloomDownsample.hlsl", L"downsample", L"ps_6_6" };
        auto compiled = PSOManager::GetInstance().CompileShaders(sib, backendInstance);

        m_resourceDescriptorBindings = compiled.resourceDescriptorSlots;

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
                compiled.resourceDescriptorSlots);
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
                compiledUp.resourceDescriptorSlots);
        }
    }

    void CreatePSO() {
        CreatePSOForBackend(BackendInstanceId::Primary);
        if (DeviceManager::GetInstance().IsMultiRHIEnabled()) {
            CreatePSOForBackend(BackendInstanceId::Peer);
        }
    }
};
