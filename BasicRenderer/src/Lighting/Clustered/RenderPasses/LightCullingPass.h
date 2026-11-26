#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include <BasicRenderer/Extensions/PipelineDescs.h>
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Runtime/Device/DeviceManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "../shaders/PerPassRootConstants/lightCullingRootConstants.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "BasicRenderer/Extensions/PassRevision.h"

// The light grid's three passes (shaders/lightCulling.hlsl; docs/lighting.md "The light grid"). The graph builder
// (BuildLightClusteringPipeline) registers their buffers at the capacities it hands them.

struct LightGridCapacities {
	uint32_t indices = 0;
	uint32_t records = 0;
	bool operator==(const LightGridCapacities&) const = default;
};

namespace light_grid_detail {
inline DirectX::XMUINT3 GridSize(const org::PassPrepareContext& preparation) {
	const auto* update = preparation.preparationData->Get<UpdateContext>();
	const auto* render = preparation.preparationData->Get<RenderContext>();
	if (!update && !render) throw std::logic_error("light grid passes require frame context");
	return update ? update->lightClusterSize : render->lightClusterSize;
}
inline DirectX::XMUINT2 RenderResolution(const org::PassPrepareContext& preparation) {
	const auto* update = preparation.preparationData->Get<UpdateContext>();
	const auto* render = preparation.preparationData->Get<RenderContext>();
	if (!update && !render) throw std::logic_error("light grid passes require frame context");
	return update ? update->renderResolution : render->renderResolution;
}
// Kept recordings (M6 step 5): what the three passes' dispatches follow, taken by each pass's Update and named by its
// revision with its pipeline.
struct FrameShape {
	DirectX::XMUINT3 grid{};
	DirectX::XMUINT2 resolution{};
	void Take(const org::UpdateExecutionContext& execution) {
		const auto* update = execution.hostData ? execution.hostData->Get<UpdateContext>() : nullptr;
		const auto* render = execution.hostData ? execution.hostData->Get<RenderContext>() : nullptr;
		if (update) { grid = update->lightClusterSize; resolution = update->renderResolution; }
		else if (render) { grid = render->lightClusterSize; resolution = render->renderResolution; }
	}
	void Append(std::vector<uint64_t>& out, const br::PipelineState& pipeline) const {
		br::render::AppendPipelineRevision(out, pipeline);
		out.push_back((uint64_t(grid.x) << 40u) ^ (uint64_t(grid.y) << 20u) ^ grid.z);
		out.push_back((uint64_t(resolution.x) << 32u) | resolution.y);
	}
};
}

struct LightGridComputeBindings {
	br::PipelineState pipeline;
};
struct LightGridDispatchFrame {
	uint32_t groupsX = 0, groupsY = 0, groupsZ = 0;
	LightGridCapacities capacities;
	bool forwardLists = true;
	bool operator==(const LightGridDispatchFrame&) const = default;
};

// One culling record per active light, and every froxel's depth bounds reset.
class LightGridPreparePass : public org::TypedRenderGraphPass<LightGridPreparePass, LightGridDispatchFrame, LightGridComputeBindings> {
public:
	explicit LightGridPreparePass(LightGridCapacities capacities) : m_capacities(capacities) {}

	LightGridComputeBindings Declare(org::PassBuilder& builder) {
		builder.ShaderResource(Builtin::CameraBuffer, Builtin::Light::ActiveLightIndices, Builtin::Light::InfoBuffer);
		builder.UnorderedAccess(Builtin::Light::CullingRecords, Builtin::Light::GridDepthBounds, Builtin::Light::CullingStats);
		builder.ConstantBuffer(Builtin::PerFrameBuffer);
		builder.PreferQueue(org::QueueKind::Compute);
		m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(L"shaders/lightCulling.hlsl", L"LightGridPrepareCS", "Light Grid Prepare CS"));
		return {m_pipeline};
	}
	void Revision(std::vector<uint64_t>& out) const { m_shape.Append(out, m_pipeline); }

	void Initialize() {
		m_cullingStatsHandle = m_resourceRegistryView->RequestHandle(Builtin::Light::CullingStats);
	}

	LightGridDispatchFrame Prepare(const LightGridComputeBindings&, const org::PassPrepareContext& preparation) const {
		const auto grid = light_grid_detail::GridSize(preparation);
		const uint32_t threads = (std::max)(grid.x * grid.y * grid.z + grid.x * grid.y, m_capacities.records);
		LightGridDispatchFrame frame{};
		frame.groupsX = (threads + LIGHT_GRID_PREPARE_THREADS - 1u) / LIGHT_GRID_PREPARE_THREADS;
		frame.groupsY = frame.groupsZ = 1u;
		frame.capacities = m_capacities;
		return frame;
	}
	static void Record(const LightGridComputeBindings& bindings, const LightGridDispatchFrame& frame, org::PassRecordContext& context) {
		br::render::MiscConstants constants{};
		constants[LIGHT_GRID_INDEX_CAPACITY] = frame.capacities.indices;
		constants[LIGHT_GRID_RECORD_CAPACITY] = frame.capacities.records;
		br::render::DispatchCompute(context, bindings.pipeline, constants, frame.groupsX);
	}

	// Staged recordings (M6 step 6): the values the revision names, taken without side effects (RenderGraphPass::TakeFrameValues).
	void TakeFrameValues(const org::UpdateExecutionContext& execution) override { m_shape.Take(execution); }
	void Update(const org::UpdateExecutionContext&) override {
		// The overflow counters and the index allocator (LightManager::PollCullingStats reads them back after the graph).
		const uint32_t stats[LIGHT_CULLING_STATS_WORDS] = {};
		UploadBufferData(stats, sizeof(stats), org::runtime::UploadTarget::FromHandle(m_cullingStatsHandle), 0);
	}

private:
	LightGridCapacities m_capacities;
	org::ResourceRegistry::RegistryHandle m_cullingStatsHandle;
	br::PipelineState m_pipeline;
	light_grid_detail::FrameShape m_shape;
};

// Each opaque pixel widens its froxel's depth bounds and its tile's farthest depth.
class LightGridDepthBoundsPass : public org::TypedRenderGraphPass<LightGridDepthBoundsPass, LightGridDispatchFrame, LightGridComputeBindings> {
public:
	LightGridComputeBindings Declare(org::PassBuilder& builder) {
		builder.ShaderResource(Builtin::CameraBuffer);
		builder.ShaderResource(Subresources(Builtin::PrimaryCamera::LinearDepthMap, org::Mip{0, 1}));
		builder.UnorderedAccess(Builtin::Light::GridDepthBounds);
		builder.ConstantBuffer(Builtin::PerFrameBuffer);
		builder.PreferQueue(org::QueueKind::Compute);
		m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(L"shaders/lightCulling.hlsl", L"LightGridDepthBoundsCS", "Light Grid Depth Bounds CS"));
		return {m_pipeline};
	}
	void TakeFrameValues(const org::UpdateExecutionContext& execution) override { m_shape.Take(execution); }
	void Revision(std::vector<uint64_t>& out) const { m_shape.Append(out, m_pipeline); }

	LightGridDispatchFrame Prepare(const LightGridComputeBindings&, const org::PassPrepareContext& preparation) const {
		const auto resolution = light_grid_detail::RenderResolution(preparation);
		LightGridDispatchFrame frame{};
		frame.groupsX = (resolution.x + LIGHT_GRID_DEPTH_BOUNDS_TILE - 1u) / LIGHT_GRID_DEPTH_BOUNDS_TILE;
		frame.groupsY = (resolution.y + LIGHT_GRID_DEPTH_BOUNDS_TILE - 1u) / LIGHT_GRID_DEPTH_BOUNDS_TILE;
		frame.groupsZ = 1u;
		return frame;
	}
	static void Record(const LightGridComputeBindings& bindings, const LightGridDispatchFrame& frame, org::PassRecordContext& context) {
		br::render::DispatchCompute(context, bindings.pipeline, {}, frame.groupsX, frame.groupsY);
	}

private:
	br::PipelineState m_pipeline;
	light_grid_detail::FrameShape m_shape;
};

// Culls every active light into the froxels: counts, packs each froxel's run into GridIndices, fills it.
class LightCullingPass : public org::TypedRenderGraphPass<LightCullingPass, LightGridDispatchFrame, LightGridComputeBindings> {
public:
	LightCullingPass(LightGridCapacities capacities, bool forwardLists) : m_capacities(capacities), m_forwardLists(forwardLists) {}

	LightGridComputeBindings Declare(org::PassBuilder& builder) {
		builder.ShaderResource(Builtin::CameraBuffer, Builtin::Light::CullingRecords, Builtin::Light::GridDepthBounds);
		builder.UnorderedAccess(Builtin::Light::GridCells, Builtin::Light::GridIndices, Builtin::Light::CullingStats);
		builder.ConstantBuffer(Builtin::PerFrameBuffer);
		builder.PreferQueue(org::QueueKind::Compute);
		m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(L"shaders/lightCulling.hlsl", L"LightCullingCS", "Light Culling CS"));
		return {m_pipeline};
	}
	void TakeFrameValues(const org::UpdateExecutionContext& execution) override { m_shape.Take(execution); }
	void Revision(std::vector<uint64_t>& out) const { m_shape.Append(out, m_pipeline); }

	LightGridDispatchFrame Prepare(const LightGridComputeBindings&, const org::PassPrepareContext& preparation) const {
		const auto grid = light_grid_detail::GridSize(preparation);
		LightGridDispatchFrame frame{};
		frame.groupsX = (grid.x + LIGHT_GRID_COARSE_X - 1u) / LIGHT_GRID_COARSE_X;
		frame.groupsY = (grid.y + LIGHT_GRID_COARSE_Y - 1u) / LIGHT_GRID_COARSE_Y;
		frame.groupsZ = (grid.z + LIGHT_GRID_COARSE_Z - 1u) / LIGHT_GRID_COARSE_Z;
		frame.capacities = m_capacities;
		frame.forwardLists = m_forwardLists;
		return frame;
	}
	static void Record(const LightGridComputeBindings& bindings, const LightGridDispatchFrame& frame, org::PassRecordContext& context) {
		br::render::MiscConstants constants{};
		constants[LIGHT_GRID_INDEX_CAPACITY] = frame.capacities.indices;
		constants[LIGHT_GRID_RECORD_CAPACITY] = frame.capacities.records;
		constants[LIGHT_GRID_FLAGS] = frame.forwardLists ? LIGHT_GRID_FLAG_FORWARD_LISTS : 0u;
		br::render::DispatchCompute(context, bindings.pipeline, constants, frame.groupsX, frame.groupsY, frame.groupsZ);
	}

private:
	LightGridCapacities m_capacities;
	bool m_forwardLists = true;
	br::PipelineState m_pipeline;
	light_grid_detail::FrameShape m_shape;
};
