#pragma once

#include "RenderPasses/Base/RenderPass.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Scene/Scene.h"
#include "Utilities/Utilities.h"
#include "../shaders/PerPassRootConstants/bloomBlendRootConstants.h"
#include "Resources/PixelBuffer.h"

class BloomBlendPass : public RenderPass {
public:

    BloomBlendPass() {
        CreatePSO();
    }

    void DeclareResourceUsages(RenderPassBuilder* builder) override {
        builder->WithShaderResource(Subresources(Builtin::PostProcessing::BloomTexture, Mip{ 1, 1 }))
            .WithUnorderedAccess(Subresources(Builtin::PostProcessing::UpscaledHDR, Mip{ 0, 1 }));
    }

    void Setup() override {
        m_pHDRTarget = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::PostProcessing::UpscaledHDR);
        m_pBloomTarget = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::PostProcessing::BloomTexture);
    }

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& context = *renderContext;
        auto& psoManager = PSOManager::GetInstance();
        auto& commandList = executionContext.commandList;

		commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());

		rhi::PassBeginInfo passInfo{};
		passInfo.height = m_pHDRTarget->GetHeight();
		passInfo.width = m_pHDRTarget->GetWidth();
		commandList.BeginPass(passInfo);

        commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleStrip);

		commandList.BindLayout(psoManager.GetRootSignature().GetHandle());
		commandList.BindPipeline(m_pso->GetHandle());

        BindResourceDescriptorIndices(commandList, m_resourceDescriptorBindings);

        unsigned int misc[NumMiscUintRootConstants] = {};
		misc[HDR_TARGET_UAV_DESCRIPTOR_INDEX] = m_pHDRTarget->GetUAVShaderVisibleInfo(0).slot.index; // HDR target index
		misc[BLOOM_SOURCE_SRV_DESCRIPTOR_INDEX] = m_pBloomTarget->GetSRVInfo(1).slot.index; // Bloom texture index
        misc[DST_WIDTH] = m_pHDRTarget->GetWidth();
        misc[DST_HEIGHT] = m_pHDRTarget->GetHeight();
        misc[BLOOM_BLEND_FILTER_RADIUS] = as_uint(0.001f); // Kernel size
        misc[BLOOM_BLEND_ASPECT_RATIO] = as_uint(misc[DST_WIDTH] / static_cast<float>(misc[DST_HEIGHT]));
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

    rhi::PipelinePtr m_pso;

	PixelBuffer* m_pHDRTarget;
	PixelBuffer* m_pBloomTarget;

	PipelineResources m_resourceDescriptorBindings;

    void CreatePSO() {
        auto dev = DeviceManager::GetInstance().GetDevice();

        // 1) Compile shaders (same as before)
        ShaderInfoBundle sib;
        sib.vertexShader = { L"shaders/fullscreenVS.hlsli", L"FullscreenVSNoViewRayMain", L"vs_6_6" };
        sib.pixelShader = { L"shaders/PostProcessing/bloomBlend.hlsl", L"blend", L"ps_6_6" };

        auto compiled = PSOManager::GetInstance().CompileShaders(sib);
        m_resourceDescriptorBindings = compiled.resourceDescriptorSlots;

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
        bs.numAttachments = 0;
        rhi::SubobjBlend soBlend{ bs };

        rhi::RenderTargets rts{};
        rts.count = 0;
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
        auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), m_pso);
        if (Failed(result)) {
            throw std::runtime_error("Failed to create upsample PSO (RHI)");
        }
        m_pso->SetName("BloomBlend (RHI)");
    }
};
