#pragma once

#include <unordered_map>
#include <functional>
#include <mutex>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Assets/Geometry/Mesh.h"
#include "BasicRenderer/Scene/Scene.h"
#include "Diagnostics/Scene/DebugSceneSnapshotService.h"

struct DebugSphereFrameData {
    struct Sphere {
        DirectX::XMFLOAT4 bounds{};
        uint32_t perObjectIndex = 0;
    };
    std::vector<Sphere> spheres;
};

struct DebugSphereBindings {
	// The mesh pipeline, on its small push-constant layout.
	org::PipelineState pipeline;
	org::DeclaredViewToken cameraBuffer, objectBuffer;
};

class DebugSpherePass
	: public org::TypedRenderGraphPass<DebugSpherePass, DebugSphereFrameData, DebugSphereBindings> {
public:
	explicit DebugSpherePass(std::shared_ptr<br::render::DebugSceneSnapshotService> snapshots)
		: m_snapshots(std::move(snapshots)) {
	}
	~DebugSpherePass() {
	}

	DebugSphereBindings Declare(org::PassBuilder& declaration) {
		auto* builder = &declaration;
		builder->ShaderResource(Builtin::PerMeshBuffer);
		builder->DepthReadWrite(Builtin::PrimaryCamera::DepthTexture);
		builder->IsGeometryPass();
		builder->ConstantBuffer(Builtin::PerFrameBuffer);
		return {builder->Pipeline(PipelineDesc()), builder->ShaderResource(Builtin::CameraBuffer),
			builder->ShaderResource(Builtin::PerObjectBuffer)};
	}

	void Update(const org::UpdateExecutionContext&) override {
		std::scoped_lock lock(m_spheresMutex);
		m_spheres.clear();
		for (const auto& sphere : m_snapshots->CaptureSpheres())
			m_spheres.push_back({sphere.bounds, sphere.perObjectIndex});
	}

	DebugSphereFrameData Prepare(const DebugSphereBindings&, const org::PassPrepareContext&) const {
		DebugSphereFrameData data{};
		{
			std::scoped_lock lock(m_spheresMutex);
			data.spheres = m_spheres;
		}
		return data;
	}
	static void Record(const DebugSphereBindings& bindings, const DebugSphereFrameData& data, org::PassRecordContext& recording) {
		if (data.spheres.empty()) return;
		auto& commandList = recording.Commands();
		commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
		recording.BindGraphicsPipeline(bindings.pipeline);
		struct Constants {
			float center[3]; float padding; float radius;
			uint32_t perObjectIndex, cameraBufferIndex, objectBufferIndex;
		};
		const uint32_t cameraBufferIndex = recording.Index(bindings.cameraBuffer);
		const uint32_t objectBufferIndex = recording.Index(bindings.objectBuffer);
		for (const auto& sphere : data.spheres) {
			Constants constants{{sphere.bounds.x, sphere.bounds.y, sphere.bounds.z}, 0.0f,
				sphere.bounds.w, sphere.perObjectIndex, cameraBufferIndex, objectBufferIndex};
			recording.PushConstants(0, {reinterpret_cast<const uint32_t*>(&constants), 8});
			commandList.DispatchMesh(1, 1, 1);
		}
	}

private:
	std::shared_ptr<br::render::DebugSceneSnapshotService> m_snapshots;

	static org::GraphicsPipelineDesc PipelineDesc() {
		org::PipelineLayoutSpec layout;
		layout.flags = rhi::PipelineLayoutFlags::PF_AllowInputAssembler;
		rhi::LayoutBindingRange binding = {};
		binding.set = 0;
		binding.binding = 0;
		binding.count = 1;
		binding.readOnly = true;
		binding.visibility = rhi::ShaderStage::AllGraphics;
		layout.ranges = {binding};
		layout.pushConstants = {rhi::PushConstantRangeDesc{ rhi::ShaderStage::Mesh, 8, 0, 0 }};
		layout.name = "Debug.Mesh.Wireframe.Layout";

		org::GraphicsPipelineDesc desc;
		desc.name = "Debug.Mesh.Wireframe";
		desc.layout = org::PipelineLayoutRef::Custom(std::move(layout));
		desc.mesh = {L"shaders/sphere.hlsl", L"MSMain",        L"ms_6_6"};
		desc.pixel = {L"shaders/sphere.hlsl", L"SpherePSMain",  L"ps_6_6"};

		rhi::RasterState rs{};
		rs.fill = rhi::FillMode::Wireframe;
		rs.cull = rhi::CullMode::None;
		rs.frontCCW = true;
		desc.raster = rs;

		rhi::BlendState bs{};
		bs.alphaToCoverage = false;
		bs.independentBlend = false;
		bs.numAttachments = 1;
		bs.attachments[0].enable = false;                     // no blending
		bs.attachments[0].writeMask = rhi::ColorWriteEnable::All;
		desc.blend = bs;

		rhi::DepthStencilState ds{};
		ds.depthEnable = true;
		ds.depthWrite = false;                                  // D3D12_DEPTH_WRITE_MASK_ZERO
		ds.depthFunc = rhi::CompareOp::Less;                   // default in your DX path
		desc.depth = ds;

		rhi::RenderTargets rts{};
		rts.count = 1;
		rts.formats[0] = rhi::Format::R8G8B8A8_UNorm;
		desc.renderTargets = rts;

		// Your original used D24_UNORM_S8_UINT. If your RHI format enum doesn�t carry D24,
		// you can either set Unknown (let backend infer) or use D32_Float consistently.
		desc.depthFormat = rhi::Format::D32_Float;
		desc.sample = rhi::SampleDesc{1, 0};
		return desc;
	}

	std::vector<DebugSphereFrameData::Sphere> m_spheres;
	mutable std::mutex m_spheresMutex;
	bool m_wireframe;

};
