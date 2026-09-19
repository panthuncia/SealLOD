#pragma once

#include <unordered_map>
#include <functional>
#include <mutex>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Mesh/Mesh.h"
#include "Scene/Scene.h"
#include "Render/DebugSceneSnapshotService.h"

struct DebugSphereFrameData {
    struct Sphere {
        DirectX::XMFLOAT4 bounds{};
        uint32_t perObjectIndex = 0;
    };
    rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
    rhi::PipelineLayoutHandle layout{};
    org::PreparedProgramReference program{};
    uint32_t cameraBufferIndex = 0;
    uint32_t objectBufferIndex = 0;
    std::vector<Sphere> spheres;
};

struct DebugSphereBindings {
	org::ResourceBindingToken cameraBuffer, objectBuffer;
};

class DebugSpherePass
	: public org::TypedRenderGraphPass<DebugSpherePass, DebugSphereFrameData, DebugSphereBindings> {
public:
	explicit DebugSpherePass(std::shared_ptr<br::render::DebugSceneSnapshotService> snapshots)
		: m_snapshots(std::move(snapshots)) {
		CreateDebugRootSignature();
		CreateDebugMeshPSO();
	}
	~DebugSpherePass() {
	}

	DebugSphereBindings Declare(org::PassBuilder& declaration) {
		auto* builder = &declaration;
		builder->WithShaderResource(Builtin::PerMeshBuffer)
			.WithDepthReadWrite(Builtin::PrimaryCamera::DepthTexture)
			.IsGeometryPass();
		builder->WithConstantBuffer(Builtin::PerFrameBuffer);
		return {builder->BindShaderResource(Builtin::CameraBuffer),
			builder->BindShaderResource(Builtin::PerObjectBuffer)};
	}

	void Update(const org::UpdateExecutionContext&) override {
		std::scoped_lock lock(m_spheresMutex);
		m_spheres.clear();
		for (const auto& sphere : m_snapshots->CaptureSpheres())
			m_spheres.push_back({sphere.bounds, sphere.perObjectIndex});
	}

	DebugSphereFrameData Prepare(const DebugSphereBindings& bindings, const org::PassPrepareContext& preparation) const {
		const auto* context = preparation.preparationData->Get<UpdateContext>();
		DebugSphereFrameData data{};
		data.resourceHeap = context->textureDescriptorHeap.GetHandle();
		data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
		data.layout = (*m_debugLayout)->GetHandle();
		data.program = preparation.CaptureProgram(m_pso);
		preparation.Retain(m_debugLayout);
		data.cameraBufferIndex = preparation.ResolveView(bindings.cameraBuffer, {org::BindlessViewKind::ShaderResource}).index;
		data.objectBufferIndex = preparation.ResolveView(bindings.objectBuffer, {org::BindlessViewKind::ShaderResource}).index;
		{
			std::scoped_lock lock(m_spheresMutex);
			data.spheres = m_spheres;
		}
		return data;
	}
	static void Record(const DebugSphereBindings&, const DebugSphereFrameData& data, org::PassRecordContext& recording) {
		if (data.spheres.empty()) return;
		auto& commandList = recording.Commands();
		commandList.SetDescriptorHeaps(data.resourceHeap, data.samplerHeap);
		commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
		commandList.BindLayout(data.layout);
		commandList.BindPipeline(recording.Resolve(data.program));
		struct Constants {
			float center[3]; float padding; float radius;
			uint32_t perObjectIndex, cameraBufferIndex, objectBufferIndex;
		};
		for (const auto& sphere : data.spheres) {
			Constants constants{{sphere.bounds.x, sphere.bounds.y, sphere.bounds.z}, 0.0f,
				sphere.bounds.w, sphere.perObjectIndex, data.cameraBufferIndex, data.objectBufferIndex};
			commandList.PushConstants(rhi::ShaderStage::AllGraphics, 0, 0, 0, 8,
				reinterpret_cast<const uint32_t*>(&constants));
			commandList.DispatchMesh(1, 1, 1);
		}
	}

private:
	std::shared_ptr<br::render::DebugSceneSnapshotService> m_snapshots;

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
		m_debugLayout = std::make_shared<rhi::PipelineLayoutPtr>();
		auto result = device.CreatePipelineLayout(desc, *m_debugLayout);

	}

	void CreateDebugMeshPSO() {

		auto dev = DeviceManager::GetInstance().GetDevice();

		// Compile shaders
		ShaderInfoBundle sib;
		sib.meshShader = { L"shaders/sphere.hlsl", L"MSMain",        L"ms_6_6" };
		sib.pixelShader = { L"shaders/sphere.hlsl", L"SpherePSMain",  L"ps_6_6" };
		auto compiled = PSOManager::GetInstance().CompileShaders(sib);

		// Subobjects
		rhi::SubobjLayout soLayout{ (*m_debugLayout)->GetHandle() };

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

		m_pso = std::make_shared<rhi::PipelinePtr>();
		auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), *m_pso);
		if (Failed(result)) {
			throw std::runtime_error("Failed to create Debug Mesh PSO (RHI)");
		}
		(*m_pso)->SetName("Debug.Mesh.Wireframe");

	}

	std::vector<DebugSphereFrameData::Sphere> m_spheres;
	mutable std::mutex m_spheresMutex;
	std::shared_ptr<rhi::PipelineLayoutPtr> m_debugLayout;
	std::shared_ptr<rhi::PipelinePtr> m_pso;
	bool m_wireframe;

};
