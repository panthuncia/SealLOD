#pragma once

#include <unordered_map>
#include <functional>

#include <spdlog/spdlog.h>

#include "RenderPasses/Base/RenderPass.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Scene/Scene.h"
#include "Materials/colorspaces.h"

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

class TonemappingPass : public RenderPass {
public:
	TonemappingPass() {
		CreatePSO();
		getTonemapType = SettingsManager::GetInstance().getSettingGetter<unsigned int>("tonemapType");
        m_pLPMConstants = LazyDynamicStructuredBuffer<LPMConstants>::CreateShared(1, "AMD LPM constants", 1, true);
	}

    std::shared_ptr<Resource> ProvideResource(ResourceIdentifier const& key) override {
        if (key == m_providedResources[0]) {
			return m_pLPMConstants;
        }
		return nullptr;
    }
    std::vector<ResourceIdentifier> GetSupportedKeys() override {
		return m_providedResources;
    }

    void DeclareResourceUsages(RenderPassBuilder* builder) override {
        builder->WithShaderResource(Builtin::PostProcessing::UpscaledHDR, Builtin::CameraBuffer, "FFX::LPMConstants")
            .WithRenderTarget(Builtin::Backbuffer);
		builder->WithConstantBuffer(Builtin::PerFrameBuffer);
    }

	void Setup() override {

        LPMConstants lpmConstants = {};
        
        lpmConstants.shoulder = true;
        lpmConstants.con = false;
        lpmConstants.soft = false;
        lpmConstants.con2 = false;
        lpmConstants.clip = true;
        lpmConstants.scaleOnly = false;
        
        // Rest will be filled in by the luminanceHistogramAverage shader

        BUFFER_UPLOAD(&lpmConstants, sizeof(LPMConstants), rg::runtime::UploadTarget::FromShared(m_pLPMConstants), 0);
    }

	PassReturn Execute(PassExecutionContext& executionContext) override {
		auto* renderContext = executionContext.hostData->Get<RenderContext>();
		auto& context = *renderContext;
		auto& psoManager = PSOManager::GetInstance();
		auto& commandList = executionContext.commandList;

		commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());

		rhi::PassBeginInfo passInfo{};
		rhi::ColorAttachment colorAttachment{};
		colorAttachment.rtv = { context.rtvHeap.GetHandle(), context.frameIndex };
		colorAttachment.loadOp = rhi::LoadOp::Clear;
		colorAttachment.storeOp = rhi::StoreOp::Store;
		colorAttachment.clear.rgba[0] = 0.0f;
		colorAttachment.clear.rgba[1] = 0.0f;
		colorAttachment.clear.rgba[2] = 0.0f;
		colorAttachment.clear.rgba[3] = 1.0f;
		passInfo.colors = { &colorAttachment };
		passInfo.width = context.outputResolution.x;
		passInfo.height = context.outputResolution.y;
		commandList.BeginPass(passInfo);

		commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleStrip);
		commandList.BindLayout(psoManager.GetRootSignature().GetHandle());
		commandList.BindPipeline(m_pso->GetHandle());

        BindResourceDescriptorIndices(commandList, m_resourceDescriptorBindings);

		unsigned int misc[NumMiscUintRootConstants] = {};
		misc[LPM_CONSTANTS_BUFFER_SRV_DESCRIPTOR_INDEX] = m_pLPMConstants->GetSRVInfo(0).slot.index;
		misc[TONEMAP_TYPE] = getTonemapType();

		commandList.PushConstants(rhi::ShaderStage::Pixel, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, misc);

		commandList.Draw(3, 1, 0, 0); // Fullscreen triangle
		return {};
	}

    void Cleanup() override {
        // Cleanup the render pass
	}

private:

    rhi::PipelinePtr m_pso;
    PipelineResources m_resourceDescriptorBindings;

    std::shared_ptr<LazyDynamicStructuredBuffer<LPMConstants>> m_pLPMConstants;

    std::function<unsigned int()> getTonemapType;

    std::vector<ResourceIdentifier> m_providedResources = {
		"FFX::LPMConstants"
	};

    void CreatePSO() {
        auto dev = DeviceManager::GetInstance().GetDevice();

        // Compile shaders
        ShaderInfoBundle sib;
        sib.vertexShader = { L"shaders/fullscreenVS.hlsli", L"FullscreenVSNoViewRayMain", L"vs_6_6" };
        sib.pixelShader = { L"shaders/PostProcessing/tonemapping.hlsl", L"PSMain", L"ps_6_6" };
        auto compiled = PSOManager::GetInstance().CompileShaders(sib);
        m_resourceDescriptorBindings = compiled.resourceDescriptorSlots;

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
        rhi::SubobjBlend soBlend{ bs };

        rhi::DepthStencilState ds{};
        ds.depthEnable = false;
        ds.depthWrite = false;
        ds.depthFunc = rhi::CompareOp::Greater;
        rhi::SubobjDepth soDepth{ ds };

        rhi::RenderTargets rts{};
        rts.count = 1;
        rts.formats[0] = rhi::Format::R8G8B8A8_UNorm;
        rhi::SubobjRTVs soRTVs{ rts };

        rhi::SubobjDSV    soDSV{ rhi::Format::D32_Float };
        rhi::SubobjSample soSmp{ rhi::SampleDesc{1, 0} };
        rhi::SubobjPrimitiveTopology soTopo{ rhi::PrimitiveTopology::TriangleStrip };

        const rhi::PipelineStreamItem items[] = {
            rhi::Make(soLayout),
            rhi::Make(soVS),
            rhi::Make(soPS),
            rhi::Make(soRaster),
            rhi::Make(soBlend),
            rhi::Make(soDepth),
            rhi::Make(soRTVs),
            rhi::Make(soDSV),
            rhi::Make(soSmp),
			rhi::Make(soTopo)
        };

        auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), m_pso);
        if (Failed(result)) {
            throw std::runtime_error("Failed to create tonemapping PSO (RHI)");
        }
        m_pso->SetName("Tonemapping.PSO");
    }
};