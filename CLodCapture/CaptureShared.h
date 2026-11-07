#pragma once

// What a capture job and the viewer both need to put assets on screen with the same renderer setup.

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <DirectXMath.h>
#include <nlohmann/json.hpp>

#include <BasicRenderer/Assets/Geometry/MeshInstance.h>
#include <BasicRenderer/Scene/Components.h>
#include <BasicRenderer/Extensions/BuiltinResources.h>
#include <BasicRenderer/Extensions/SettingAccess.h>
#include <BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h>
#include <BasicRenderer/Pipeline/PipelineRecipe.h>
#include <BasicRenderer/Pipeline/UpscalingTypes.h>
#include <BasicRenderer/Renderer.h>
#include <BasicRenderer/Scene/Scene.h>
#include <BasicRenderer/StaticImport/ImportedAssetLibrary.h>
#include <BasicRenderer/StaticImport/StaticImportRuntime.h>

#include "CacheRoots.h"

namespace clodcapture {
	namespace fs = std::filesystem;
	using namespace DirectX;

struct JobAsset {
	std::string path;
	float scale = 1.0f;
};

struct JobView {
	std::string name;
	XMFLOAT3 position{};
	XMFLOAT3 lookAt{};
	XMFLOAT3 up{ 0.0f, 1.0f, 0.0f };
	float fovDegrees = 60.0f;
	float zNear = 0.1f;
	float zFar = 1000.0f;
};

// Everything the render needs and nothing it doesn't. Upscaling, tonemapping and present stay because
// Render returns early without a present dependency, and the validator requires the chain.
inline br::pipeline::PipelineRecipe MakeCapturePipeline(bool interactive = false)
{
	using namespace br::pipeline;
	PipelineRecipe recipe;
	recipe.Add<FrameResourcesTechnique>()
		.Add<BrdfIntegrationTechnique>()
		.Add<EnvironmentTechnique>()
		.Add<ClusterLodTechnique>(ClusterLodOptions{ .reyes = ReyesMode::Disabled })
		// Alpha-masked materials only draw through this one, and foliage is what voxels are mostly for.
		.Add<ClusterLodAlphaTechnique>(ClusterLodOptions{ .reyes = ReyesMode::Disabled })
		.Add<ClusterLodVoxelTechnique>(ClusterLodVoxelOptions{ .workRecordCapacity = 1u << 20 })
		.Add<CanonicalSurfaceResourcesTechnique>()
		.Add<VisibilityMaterialBinningTechnique>()
		.Add<MaterialEvaluationTechnique>()
		.Add<ClusteredLightingTechnique>()
		.Add<PrimaryLightingTechnique>();
	// A window needs exposure, or the lit image tonemaps to black, plus the debug views and the panels. A capture reads
	// targets before any of them would draw, and leaves them out.
	if (interactive) {
		recipe.Add<ExposureTechnique>();
	}
	recipe.Add<UpscalingTechnique>()
		.Add<TonemappingTechnique>();
	if (interactive) {
		recipe.Add<DebugOutputTechnique>().Add<DebugUiTechnique>();
	}
	recipe.Add<PresentTechnique>();
	return recipe;
}

struct LayerDefinition {
	std::string_view name;
	std::string_view resource;
	std::string_view anchorPass;
};

// Each is read from inside a pass that binds it. These targets may alias, so reading them after the
// graph would be wrong.
constexpr std::string_view kSurfaceAnchor = "EvaluateMaterialGroupsPass";
constexpr std::string_view kLightingAnchor = "DeferredShadingPass";
constexpr std::array kLayers{
	LayerDefinition{ "hdr", Builtin::Color::HDRColorTarget, kLightingAnchor },
	LayerDefinition{ "visibility", Builtin::PrimaryCamera::VisibilityTexture, kSurfaceAnchor },
	LayerDefinition{ "base-color-opacity", Builtin::Surface::BaseColorOpacity, kSurfaceAnchor },
	LayerDefinition{ "normal-roughness", Builtin::Surface::NormalRoughness, kSurfaceAnchor },
	// What the outputType setting wrote. Packed as debugPayload.hlsli describes.
	LayerDefinition{ "debug", Builtin::DebugVisualization, kSurfaceAnchor },
};

inline const LayerDefinition& FindLayer(std::string_view name)
{
	for (const LayerDefinition& layer : kLayers) {
		if (layer.name == name) {
			return layer;
		}
	}
	throw std::runtime_error(std::format("unknown layer '{}'", name));
}

inline float HalfToFloat(uint16_t half)
{
	const uint32_t sign = (half & 0x8000u) << 16u;
	const uint32_t exponent = (half >> 10u) & 0x1Fu;
	const uint32_t mantissa = half & 0x3FFu;
	uint32_t bits = 0;
	if (exponent == 0u) {
		if (mantissa != 0u) {
			uint32_t shift = 0u;
			uint32_t normalized = mantissa;
			while ((normalized & 0x400u) == 0u) {
				normalized <<= 1u;
				++shift;
			}
			bits = sign | ((127u - 15u + 1u - shift) << 23u) | ((normalized & 0x3FFu) << 13u);
		}
		else {
			bits = sign;
		}
	}
	else if (exponent == 31u) {
		bits = sign | 0x7F800000u | (mantissa << 13u);
	}
	else {
		bits = sign | ((exponent - 15u + 127u) << 23u) | (mantissa << 13u);
	}
	float value = 0.0f;
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}

// The assets. Static parts go through the static import runtime as SARP does. Skinned parts never reach
// that pipeline, so they become ordinary scene entities. Scene activation binds a skeleton's first clip,
// so each is given an external pose equal to its bind pose, which also stops the animation ticking.
class CaptureScene {
public:
	~CaptureScene()
	{
		if (m_runtime) {
			m_runtime->Shutdown();
		}
		m_runtime.reset();
		m_assets.Shutdown();
	}

	void SetHoldBindPose(bool hold) { m_holdBindPose = hold; }
	// Only parts whose name contains this are placed, so one part of an asset can be looked at alone.
	void SetPartFilter(std::string filter) { m_partFilter = std::move(filter); }

	void AddAsset(const JobAsset& asset)
	{
		Model model{ asset.path };
		XMStoreFloat4x4(&model.world, XMMatrixScaling(asset.scale, asset.scale, asset.scale));
		model.key = br::staticimport::recipe::HashString(std::to_string(++m_modelsAdded), br::staticimport::recipe::HashString("clod-capture-model"));
		(void)m_assets.TryGetLoadedParts(asset.path);
		m_unplaced.push_back(std::move(model));
	}

	// A merge-hierarchy root and its parts, from the cache tree. Placed at the origin, as the root's space is.
	void AddCacheRoot(const fs::path& rootMetadata)
	{
		std::string error;
		if (!m_rootSource.Load(rootMetadata, error)) {
			throw std::runtime_error(error);
		}
		m_unplacedRoots.push_back(CacheRootSource::KeyFor(rootMetadata));
	}

	void Tick(Renderer& renderer)
	{
		if (!m_runtime) {
			m_runtime = std::make_unique<br::staticimport::StaticImportRuntime>(
				m_shared, br::staticimport::StaticImportSettings{}, m_rootSource, br::staticimport::StaticImportRuntime::Hooks{});
			m_runtime->AttachServices(renderer.GetSceneIngestionServices().StaticScene());
		}
		if (!m_unplacedRoots.empty()) {
			std::vector<br::staticimport::StaticImportGroup> groups;
			for (const std::string& key : m_unplacedRoots) {
				auto& group = groups.emplace_back();
				group.assetKey = key;
				group.groupID = br::staticimport::recipe::HashString(key, br::staticimport::recipe::HashString("clod-capture-root"));
				group.batchKey = group.groupID;
				group.name = key;
				XMStoreFloat4x4(&group.instanceWorldMatrices.emplace_back(), XMMatrixIdentity());
			}
			m_placedParts += groups.size();
			m_runtime->AddOrUpdateGroups(std::move(groups));
			m_unplacedRoots.clear();
		}
		std::erase_if(m_unplaced, [&](const Model& model) {
			const auto parts = m_assets.TryGetLoadedParts(model.path);
			if (!parts) {
				return false;
			}
			// Parts are listed before the asset publishes, and skinned parts are only available after.
			const auto* skinnedParts = m_assets.TryGetSkinnedParts(model.path);
			if (skinnedParts == nullptr && std::ranges::any_of(*parts, [](const auto& part) { return part.skinned; })) {
				return false;
			}
			// A skinned entity registers its materials as it is created. Before the first frame that can stall
			// for good: frames wait for material rows, the rows wait for texture jobs, and the jobs only run
			// in a frame. So wait until frames are flowing.
			if (skinnedParts != nullptr && renderer.GetTotalFramesRendered() == 0u) {
				return false;
			}
			std::vector<br::staticimport::StaticImportGroup> groups;
			for (const auto& part : *parts) {
				spdlog::info("part {}: name='{}' skinned={}", part.index, part.name, part.skinned);
				if (part.skinned || (!m_partFilter.empty() && part.name.find(m_partFilter) == std::string::npos)) {
					continue;
				}
				auto& group = groups.emplace_back();
				group.assetKey = br::staticimport::ImportedAssetLibrary::MakePartAssetKey(model.path, part.index);
				group.groupID = br::staticimport::recipe::HashString(group.assetKey, model.key);
				group.batchKey = model.key;
				group.name = part.name.empty() ? fs::path(model.path).filename().string() : part.name;
				XMStoreFloat4x4(&group.instanceWorldMatrices.emplace_back(), XMLoadFloat4x4(&part.localMatrix) * XMLoadFloat4x4(&model.world));
			}
			m_placedParts += groups.size();
			m_runtime->AddOrUpdateGroups(std::move(groups));
			if (skinnedParts != nullptr) {
				org::BufferBase::ScopedBackingMutation backingMutation;
				for (const auto& part : *skinnedParts) {
					const XMMATRIX world = part.localMatrix * XMLoadFloat4x4(&model.world);
					m_skinnedEntities.push_back(renderer.GetCurrentScene()->CreateRenderableEntityECS(
						part.meshes, L"capture-skinned-" + std::to_wstring(m_skinnedEntities.size()), std::nullopt, world));
					if (m_holdBindPose) {
						HoldBindPose(m_skinnedEntities.back());
					}
				}
			}
			return true;
		});
		m_runtime->Tick();
	}

	bool AllPlaced() const { return m_unplaced.empty() && m_unplacedRoots.empty(); }
	std::string DescribePlacement() const
	{
		return std::format("unplaced_models={} static_parts={} skinned_parts={} pending_groups={} live={}",
			m_unplaced.size(), m_placedParts, m_skinnedEntities.size(), m_runtime ? m_runtime->GetStatsSnapshot().pendingGroups : 0u, LiveObjects());
	}
	// A joint's pose at bind is the inverse of its inverse bind matrix, which makes every skin matrix identity.
	static void HoldBindPose(const flecs::entity& entity)
	{
		const auto* instances = entity.try_get<Components::MeshInstances>();
		if (instances == nullptr) {
			return;
		}
		for (const auto& instance : instances->meshInstances) {
			if (!instance || !instance->HasSkin()) {
				continue;
			}
			const auto skin = instance->GetSkin();
			const auto inverseBinds = skin->GetInverseBindMatrices();
			std::vector<XMMATRIX> pose;
			pose.reserve(inverseBinds.size());
			for (const auto& inverseBind : inverseBinds) {
				pose.push_back(XMMatrixInverse(nullptr, inverseBind));
			}
			skin->SetExternalPose(pose);
			instance->SyncSkinningStateFromSkeleton();
		}
	}
	size_t PlacedParts() const { return m_placedParts; }
	size_t SkinnedParts() const { return m_skinnedEntities.size(); }
	bool Published() const
	{
		if (!m_runtime || !AllPlaced() || m_runtime->GetStatsSnapshot().pendingGroups != 0u) {
			return false;
		}
		return m_runtime->LiveStaticObjectCount() == m_placedParts && (m_placedParts != 0u || !m_skinnedEntities.empty());
	}
	size_t LiveObjects() const { return m_runtime ? m_runtime->LiveStaticObjectCount() : 0u; }

private:
	struct Model {
		std::string path;
		XMFLOAT4X4 world{};
		uint64_t key = 0;
	};
	br::staticimport::StaticImportShared m_shared;
	br::staticimport::ImportedAssetLibrary m_assets;
	CacheRootSource m_rootSource{ m_assets };
	std::vector<std::string> m_unplacedRoots;
	std::unique_ptr<br::staticimport::StaticImportRuntime> m_runtime;
	std::vector<Model> m_unplaced;
	uint32_t m_modelsAdded = 0;
	size_t m_placedParts = 0;
	std::vector<flecs::entity> m_skinnedEntities;
	bool m_holdBindPose = true;
	std::string m_partFilter;
};

inline nlohmann::json ReadinessJson(const Renderer::SamplingReadinessSnapshot& snapshot)
{
	return {
		{ "resident_clod_groups", snapshot.residentClodGroups },
		{ "resident_clod_allocation_bytes", snapshot.residentClodAllocationBytes },
		{ "total_clod_streamed_bytes", snapshot.totalClodStreamedBytes },
		{ "material_textures", snapshot.materialTextures },
		{ "full_resolution_textures", snapshot.fullResolutionTextures },
		{ "pending_texture_reloads", snapshot.pendingTextureReloads },
		{ "material_texture_resident_bytes", snapshot.materialTextureResidentBytes },
	};
}

inline std::string DescribeReadiness(const Renderer::SamplingReadinessSnapshot& snapshot)
{
	return std::format(
		"scene_task={} texture_reloads={} resident_groups={} resident_bytes={} queued={} in_flight={} completed={} ds_launches={} ds_uploads={} io={} background={} shader_tasks={}",
		snapshot.sceneTaskInFlight, snapshot.pendingTextureReloads, snapshot.residentClodGroups, snapshot.residentClodAllocationBytes,
		snapshot.queuedClodRequests, snapshot.inFlightClodGroups, snapshot.completedClodResults, snapshot.pendingDirectStorageLaunches,
		snapshot.pendingDirectStorageUploads, snapshot.ioTasks, snapshot.backgroundTasks, snapshot.shaderCompileTasks);
}

// True once nothing is still loading. This proves stability, not that the view has every page it
// wants: CLodEval checks that from the page-miss counters. Pending texture reloads are left out because
// that count is a published snapshot that stops refreshing while feedback is suppressed, so it can stay
// nonzero forever. The bundle records the texture state instead.
inline bool IsIdle(const Renderer::SamplingReadinessSnapshot& snapshot)
{
	return !snapshot.sceneTaskInFlight && snapshot.queuedClodRequests == 0u && snapshot.inFlightClodGroups == 0u &&
		snapshot.completedClodResults == 0u && snapshot.pendingDirectStorageLaunches == 0u && snapshot.pendingDirectStorageUploads == 0u &&
		snapshot.ioTasks == 0u && snapshot.backgroundTasks == 0u && snapshot.shaderCompileTasks == 0u;
}

// Scene::SetCamera replaces the view, which a running scene cannot survive, so views after the first
// move the existing camera. That leaves its projection alone, which is why a job has one field of view.
inline void MoveCamera(Renderer& renderer, const JobView& view)
{
	const XMMATRIX viewMatrix = XMMatrixLookAtRH(XMLoadFloat3(&view.position), XMLoadFloat3(&view.lookAt), XMLoadFloat3(&view.up));
	const XMMATRIX model = XMMatrixInverse(nullptr, viewMatrix);
	renderer.GetCurrentScene()->GetPrimaryCamera()
		.set<Components::Position>({ view.position.x, view.position.y, view.position.z })
		.set<Components::Rotation>(XMQuaternionNormalize(XMQuaternionRotationMatrix(model)))
		.set<Components::Matrix>(model);
}

inline void ApplySettings(const nlohmann::json& settings)
{
	for (const auto& [name, value] : settings.items()) {
		if (value.is_boolean()) {
			br::extensions::MakeSettingSetter<bool>(name)(value.get<bool>());
		}
		else if (value.is_number_float()) {
			br::extensions::MakeSettingSetter<float>(name)(value.get<float>());
		}
		else if (value.is_number_integer()) {
			br::extensions::MakeSettingSetter<uint32_t>(name)(value.get<uint32_t>());
		}
		else {
			throw std::runtime_error(std::format("setting '{}' has an unsupported type", name));
		}
	}
}

}
