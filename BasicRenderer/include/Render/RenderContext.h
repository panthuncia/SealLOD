#pragma once

#include <rhi.h>
#include <OpenRenderGraph/OpenRenderGraph.h>
#include <memory>
#include <span>
#include <vector>

#include "Scene/Components.h"
#include "Render/SceneFrameSnapshot.h"
#include "Render/PublishedRendererState.h"
#include "Render/RasterBucketFlags.h"
#include "Render/ObjectBufferStateArtifacts.h"
#include "Render/ViewStateArtifacts.h"
#include "Render/LightStateArtifacts.h"
#include "Render/PoseStateArtifacts.h"
#include "Render/WindPaletteService.h"
#include "Render/EnvironmentWorkService.h"
#include "Managers/TextureStreamingManager.h"
#include "ShaderBuffers.h"

namespace org { class PixelBuffer; }
template<class T> class DynamicStructuredBuffer;
class SortedUnsignedIntBuffer;
struct PreparedImGuiDrawData;

namespace br::render {
class CLodRayTracingSystem;
}

using PreparedViewFrameData = br::render::PreparedViewFrameData;

using PreparedActiveDrawEntry = br::render::PublishedActiveSkinnedPlacement;

struct ProceduralWindFrameSettings {
	float displacementScale = 0.0f;
	float innerRadius = 0.0f;
	float outerRadius = 0.0f;
	std::vector<float> skeletonLodQualityCurve;
	float skeletonLodStaticCutoff = 0.0f;
	float skeletonLodHysteresis = 0.0f;
	int32_t forcedSkeletonLod = -1;
	float skeletonLodCapacityTarget = 0.95f;
	float skeletonLodLateReserve = 0.10f;
	bool occlusionCullingEnabled = true;
};

struct LightingFrameSettings {
	bool imageBasedLightingEnabled = true;
	bool punctualLightingEnabled = true;
	bool shadowsEnabled = true;
	bool gtaoEnabled = true;
	bool clusteredLightingEnabled = true;
};

struct RenderContext {
	std::shared_ptr<const br::render::PublishedRendererState> publishedRendererState;
	std::shared_ptr<const br::render::PublishedManifestLease> publishedManifestLease;
	Components::DrawStats drawStats;
	MaterialTextureStreamingStats materialTextureStreamingStats{};
	br::render::EnvironmentWorkServices environmentWork;
	std::shared_ptr<br::render::CLodRayTracingSystem> clodRayTracingSystem;
	std::shared_ptr<const br::render::PreparedViewFamilyState> viewFamily;
	std::shared_ptr<const br::render::PublishedLightTableState> lightTables;
	std::shared_ptr<const br::render::PublishedPoseState> poses;
	// Deep-copied at frame acceptance. Preparation and recording never consult
	// ImGui's mutable global draw lists.
	std::shared_ptr<const PreparedImGuiDrawData> uiDrawData;
	// Owner-thread snapshot used by delayed typed/transitioning packets. A
	// recording worker must not enumerate the live ViewManager container.
	uint32_t preparedRasterBucketCount = 0;
	std::vector<MaterialRasterFlags> preparedRasterBucketFlags;
	bool terrainRegionMaterialEvaluationEnabled = false;
	unsigned int outputType = 0;
	unsigned int tonemapType = 0;
	DirectX::XMUINT3 lightClusterSize{};
	LightingFrameSettings lighting;
	ProceduralWindFrameSettings proceduralWind;

	Components::Camera primaryCamera;
	Components::DepthMap primaryDepthMap;
	uint64_t primaryViewID = 0;
	bool hasPrimaryCamera = false;
    rhi::DescriptorHeap textureDescriptorHeap;
	rhi::DescriptorHeap samplerDescriptorHeap;
	rhi::DescriptorHeap rtvHeap;
    UINT rtvDescriptorSize;
	UINT dsvDescriptorSize;
	// Stable CPU/GPU ownership slot selected when the logical frame is accepted.
	// frameIndex remains a compatibility alias while passes migrate.
	UINT frameSlot = 0;
	// Selected only for presentation/submission; it is not an ownership slot.
	UINT swapchainImageIndex = UINT_MAX;
    UINT frameIndex = 0;
	uint64_t frameNumber = 0;
	UINT64 frameFenceValue;
    DirectX::XMUINT2 renderResolution;
	DirectX::XMUINT2 outputResolution;
    unsigned int globalPSOFlags;
	bool rayTracedReflectionsEnabled = false;
	bool clodRayTracingSupported = false;
	float deltaTime;
	br::render::SceneOverlapStatus sceneOverlapStatus;

	std::span<const PreparedViewFrameData> Views() const noexcept {
		return viewFamily ? std::span<const PreparedViewFrameData>(viewFamily->views)
			: std::span<const PreparedViewFrameData>{};
	}
	uint32_t LightPagePoolSize() const noexcept {
		return lightTables ? lightTables->lightPagePoolSize : 0u;
	}
	uint32_t ViewCameraBufferSize() const noexcept {
		return viewFamily ? viewFamily->cameraBufferSize : 0u;
	}
	uint64_t ViewResourceLayoutRevision() const noexcept {
		return viewFamily ? viewFamily->resourceLayoutRevision : 0u;
	}
};

struct UpdateContext {
	std::shared_ptr<const br::render::PublishedRendererState> publishedRendererState;
	std::shared_ptr<const br::render::PublishedManifestLease> publishedManifestLease;
	Components::DrawStats drawStats;
	MaterialTextureStreamingStats materialTextureStreamingStats{};
	br::render::EnvironmentWorkServices environmentWork;
	// Ordered ray-tracing build/trace service. The typed reflections pass captures
	// all resource and program ownership before invoking it on a recording worker.
	std::shared_ptr<br::render::CLodRayTracingSystem> clodRayTracingSystem;
	std::shared_ptr<const br::render::PreparedViewFamilyState> viewFamily;
	std::shared_ptr<const br::render::PublishedLightTableState> lightTables;
	std::shared_ptr<const br::render::PublishedPoseState> poses;
	// Immutable logical-frame material-bucket snapshot. Pass Update/Prepare
	// must use this rather than racing the live manager between phases.
	uint32_t preparedRasterBucketCount = 0;
	std::vector<MaterialRasterFlags> preparedRasterBucketFlags;
	// Generation/frame-selected configuration. Worker preparation must not
	// consult SettingsManager after the frame has been accepted.
	bool terrainRegionMaterialEvaluationEnabled = false;
	unsigned int outputType = 0;
	unsigned int tonemapType = 0;
	DirectX::XMUINT3 lightClusterSize{};
	LightingFrameSettings lighting;
	ProceduralWindFrameSettings proceduralWind;
	std::shared_ptr<br::render::IWindPaletteService> windPaletteService;
	rhi::DescriptorHeap textureDescriptorHeap;
	rhi::DescriptorHeap samplerDescriptorHeap;
	rhi::DescriptorHeapHandle rtvHeap{};

	Components::Camera primaryCamera;
	uint64_t primaryViewID = 0;
	bool hasPrimaryCamera = false;
	UINT frameSlot = 0;
	UINT frameIndex = 0;
	UINT64 frameFenceValue = 0;
	uint64_t frameNumber = 0;
	DirectX::XMUINT2 renderResolution{};
	DirectX::XMUINT2 outputResolution{};
	unsigned int globalPSOFlags = 0;
	float deltaTime = 0.0f;

	std::span<const PreparedViewFrameData> Views() const noexcept {
		return viewFamily ? std::span<const PreparedViewFrameData>(viewFamily->views)
			: std::span<const PreparedViewFrameData>{};
	}
	uint32_t LightPagePoolSize() const noexcept {
		return lightTables ? lightTables->lightPagePoolSize : 0u;
	}
	uint32_t ViewCameraBufferSize() const noexcept {
		return viewFamily ? viewFamily->cameraBufferSize : 0u;
	}
	uint64_t ViewResourceLayoutRevision() const noexcept {
		return viewFamily ? viewFamily->resourceLayoutRevision : 0u;
	}
};
