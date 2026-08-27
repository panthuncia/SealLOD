#pragma once

#include <rhi.h>
#include <OpenRenderGraph/OpenRenderGraph.h>

#include "Scene/Components.h"
#include "Render/SceneFrameSnapshot.h"
#include "Render/PublishedRendererState.h"

class Scene;
class ObjectManager;
class MeshManager;
class IndirectCommandBufferManager;
class ViewManager;
class LightManager;
class EnvironmentManager;
class MaterialManager;
class SkeletonManager;
namespace org { class PixelBuffer; }
using org::PixelBuffer;

namespace br::render {
class CLodRayTracingSystem;
}

struct RenderContext {
	std::shared_ptr<const br::render::PublishedRendererState> publishedRendererState;
	std::shared_ptr<const br::render::PublishedManifestLease> publishedManifestLease;
	Components::DrawStats drawStats;
	ObjectManager* objectManager;
	MeshManager* meshManager;
	IndirectCommandBufferManager* indirectCommandBufferManager;
	ViewManager* viewManager;
    LightManager* lightManager;
	EnvironmentManager* environmentManager;
	MaterialManager* materialManager;
	br::render::CLodRayTracingSystem* clodRayTracingSystem = nullptr;

    Scene* currentScene;
	Components::Camera primaryCamera;
	Components::DepthMap primaryDepthMap;
	uint64_t primaryViewID = 0;
	bool hasPrimaryCamera = false;
    rhi::DescriptorHeap textureDescriptorHeap;
	rhi::DescriptorHeap samplerDescriptorHeap;
	rhi::DescriptorHeap rtvHeap;
    UINT rtvDescriptorSize;
	UINT dsvDescriptorSize;
    UINT frameIndex;
	uint64_t frameNumber = 0;
	UINT64 frameFenceValue;
    DirectX::XMUINT2 renderResolution;
	DirectX::XMUINT2 outputResolution;
    unsigned int globalPSOFlags;
	bool rayTracedReflectionsEnabled = false;
	bool clodRayTracingSupported = false;
	float deltaTime;
	br::render::SceneOverlapStatus sceneOverlapStatus;
};

struct UpdateContext {
	std::shared_ptr<const br::render::PublishedRendererState> publishedRendererState;
	std::shared_ptr<const br::render::PublishedManifestLease> publishedManifestLease;
	Components::DrawStats drawStats;
	ObjectManager* objectManager = nullptr;
	MeshManager* meshManager = nullptr;
	IndirectCommandBufferManager* indirectCommandBufferManager = nullptr;
	ViewManager* viewManager = nullptr;
	LightManager* lightManager = nullptr;
	EnvironmentManager* environmentManager = nullptr;
	MaterialManager* materialManager = nullptr;
	SkeletonManager* skeletonManager = nullptr;

	Scene* currentScene = nullptr;
	Components::Camera primaryCamera;
	bool hasPrimaryCamera = false;
	UINT frameIndex = 0;
	UINT64 frameFenceValue = 0;
	uint64_t frameNumber = 0;
	DirectX::XMUINT2 renderResolution{};
	DirectX::XMUINT2 outputResolution{};
	float deltaTime = 0.0f;
};
