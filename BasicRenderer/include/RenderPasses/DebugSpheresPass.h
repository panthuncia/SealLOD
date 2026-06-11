#pragma once

#include <unordered_map>
#include <functional>

#include "RenderPasses/Base/RenderPass.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Mesh/Mesh.h"
#include "Scene/Scene.h"
#include "Managers/Singletons/RendererECSManager.h"

class DebugSpherePass : public RenderPass {
public:
	DebugSpherePass() {
		CreateDebugRootSignature();
		CreateDebugMeshPSO();
		auto& ecsWorld = RendererECSManager::GetInstance().GetWorld();
		m_meshInstancesQuery = ecsWorld.query_builder<Components::ObjectDrawInfo, Components::MeshInstances>().cached().cache_kind(flecs::QueryCacheAll).build();
	}
	~DebugSpherePass() {
	}

	void DeclareResourceUsages(RenderPassBuilder* builder) override {
		builder->WithShaderResource(Builtin::PerObjectBuffer, Builtin::PerMeshBuffer, Builtin::CameraBuffer)
			.WithDepthReadWrite(Builtin::PrimaryCamera::DepthTexture)
			.IsGeometryPass();
		builder->WithConstantBuffer(Builtin::PerFrameBuffer);
	}

	void Setup() override {
	
		m_pPrimaryDepthBuffer = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::PrimaryCamera::DepthTexture);
	}

	PassReturn Execute(PassExecutionContext& executionContext) override {
	    auto* renderContext = executionContext.hostData->Get<RenderContext>();
	    auto& context = *renderContext;
		auto& commandList = executionContext.commandList;

		commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());

		rhi::PassBeginInfo passInfo{};
		rhi::DepthAttachment depthAttachment{};
		depthAttachment.dsv = m_pPrimaryDepthBuffer->GetDSVInfo(0).slot;

		commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);

		commandList.BindLayout(m_debugLayout->GetHandle());
		commandList.BindPipeline(m_pso->GetHandle());
		
		struct Constants { // TODO: Rework how constants are passed here
			float center[3];
			float padding;
			float radius;
			uint32_t perObjectIndex;
			uint32_t cameraBufferIndex;
			uint32_t objectBufferIndex;
		};
		Constants constants;
		constants.center[0] = 0.0;
		constants.center[1] = 0.0;
		constants.center[2] = 0.0;
		constants.radius = 1.0;
		constants.perObjectIndex = 0;
		constants.cameraBufferIndex = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::CameraBuffer)->GetSRVInfo(0).slot.index;
		constants.objectBufferIndex = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::PerObjectBuffer)->GetSRVInfo(0).slot.index;

		commandList.PushConstants(rhi::ShaderStage::AllGraphics, 0, 0, 0, 8, (uint32_t*)&constants);

		m_meshInstancesQuery.each([&](flecs::entity e, Components::ObjectDrawInfo drawInfo, Components::MeshInstances meshInstances) {
			auto& meshes = meshInstances.meshInstances;

			for (auto& pMesh : meshes) {
				auto meshData = pMesh->GetMesh()->GetPerMeshCBData();
				constants.center[0] = meshData.boundingSphere.sphere.x;
				constants.center[1] = meshData.boundingSphere.sphere.y;
				constants.center[2] = meshData.boundingSphere.sphere.z;
				constants.radius = meshData.boundingSphere.sphere.w;
				constants.perObjectIndex = drawInfo.perObjectCBIndex;
				commandList.PushConstants(rhi::ShaderStage::AllGraphics, 0, 0, 0, 6, (uint32_t*)&constants);
				commandList.DispatchMesh(1, 1, 1);
			}
			});

		return {};
	}

	void Cleanup() override {
		// Cleanup the render pass
	}

private:

	void CreateDebugRootSignature() {
		auto device = DeviceManager::GetInstance().GetDevice();

		rhi::PipelineLayoutDesc desc = {};
		desc.flags = rhi::PipelineLayoutFlags::PF_AllowInputAssembler;
		rhi::PushConstantRangeDesc pushConstant = { rhi::ShaderStage::Mesh, 8, 0, 0 };

		rhi::LayoutBindingRange binding = {};
		binding.set = 0;
		binding.binding = 0;
		binding.count = 1;
		binding.readOnly = true;
		binding.visibility = rhi::ShaderStage::AllGraphics;
		desc.ranges = rhi::Span<rhi::LayoutBindingRange>{ &binding, 1 };
		desc.pushConstants = rhi::Span<rhi::PushConstantRangeDesc>{ &pushConstant };
		desc.staticSamplers = rhi::Span<rhi::StaticSamplerDesc>{};
		auto result = device.CreatePipelineLayout(desc, m_debugLayout);

	}

	void CreateDebugMeshPSO() {

		auto dev = DeviceManager::GetInstance().GetDevice();

		// Compile shaders
		ShaderInfoBundle sib;
		sib.meshShader = { L"shaders/sphere.hlsl", L"MSMain",        L"ms_6_6" };
		sib.pixelShader = { L"shaders/sphere.hlsl", L"SpherePSMain",  L"ps_6_6" };
		auto compiled = PSOManager::GetInstance().CompileShaders(sib);

		// Subobjects
		rhi::SubobjLayout soLayout{ m_debugLayout->GetHandle() };

		rhi::SubobjShader soMS{ rhi::ShaderStage::Mesh,  rhi::DXIL(compiled.meshShader.Get()), "MSMain" };
		rhi::SubobjShader soPS{ rhi::ShaderStage::Pixel, rhi::DXIL(compiled.pixelShader.Get()), "SpherePSMain" };

		rhi::RasterState rs{};
		rs.fill = rhi::FillMode::Wireframe;
		rs.cull = rhi::CullMode::None;
		rs.frontCCW = true;
		rhi::SubobjRaster soRaster{ rs };

		rhi::BlendState bs{};
		bs.alphaToCoverage = false;
		bs.independentBlend = false;
		bs.numAttachments = 1;
		bs.attachments[0].enable = false;                     // no blending
		bs.attachments[0].writeMask = rhi::ColorWriteEnable::All;
		rhi::SubobjBlend soBlend{ bs };

		rhi::DepthStencilState ds{};
		ds.depthEnable = true;
		ds.depthWrite = false;                                  // D3D12_DEPTH_WRITE_MASK_ZERO
		ds.depthFunc = rhi::CompareOp::Less;                   // default in your DX path
		rhi::SubobjDepth soDepth{ ds };

		rhi::RenderTargets rts{};
		rts.count = 1;
		rts.formats[0] = rhi::Format::R8G8B8A8_UNorm;
		rhi::SubobjRTVs soRTVs{ rts };

		// Your original used D24_UNORM_S8_UINT. If your RHI format enum doesn�t carry D24,
		// you can either set Unknown (let backend infer) or use D32_Float consistently.
		rhi::SubobjDSV    soDSV{ rhi::Format::D32_Float };
		rhi::SubobjSample soSmp{ rhi::SampleDesc{1, 0} };

		const rhi::PipelineStreamItem items[] = {
			rhi::Make(soLayout),
			rhi::Make(soMS),
			rhi::Make(soPS),
			rhi::Make(soRaster),
			rhi::Make(soBlend),
			rhi::Make(soDepth),
			rhi::Make(soRTVs),
			rhi::Make(soDSV),
			rhi::Make(soSmp),
		};

		auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), m_pso);
		if (Failed(result)) {
			throw std::runtime_error("Failed to create Debug Mesh PSO (RHI)");
		}
		m_pso->SetName("Debug.Mesh.Wireframe");

	}

	flecs::query<Components::ObjectDrawInfo, Components::MeshInstances> m_meshInstancesQuery;
	rhi::PipelineLayoutPtr m_debugLayout;
	rhi::PipelinePtr m_pso;
	bool m_wireframe;

	PixelBuffer* m_pPrimaryDepthBuffer;

	std::function<bool()> getImageBasedLightingEnabled;
	std::function<bool()> getPunctualLightingEnabled;
	std::function<bool()> getShadowsEnabled;

};
