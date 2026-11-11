#include "Lighting/Lights/LightManager.h"

#include "Runtime/Resources/ResourceManager.h"
#include "Utilities/Utilities.h"
#include "Runtime/Settings/SettingsManager.h"
#include "Scene/Objects/IndirectCommandBufferManager.h"
#include "Runtime/Device/DeletionManager.h"
#include "Scene/ECS/RendererECSManager.h"
#include "Scene/Views/ViewManager.h"
#include "VirtualShadows/Views/ShadowViewService.h"
#include "BasicRenderer/Extensions/Buffers/SortedUnsignedIntBuffer.h"
#include "BasicRenderer/Diagnostics/CLodTelemetry.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "Utilities/MathUtils.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "Resources/PixelBuffer.h"
#include <BasicRenderer/Extensions/Resources/PublishedStateResourceResolver.h>
#include "BasicRenderer/Streaming/LightStateArtifacts.h"
#include "../../../generated/BuiltinResources.h"

LightManager::LightManager() {
    auto& resourceManager = ::ResourceManager::GetInstance();

	m_activeLightIndices = SortedUnsignedIntBuffer::CreateShared(1, "activeLightIndices");
    m_lightBuffer = org::LazyDynamicStructuredBuffer<LightInfo>::CreateShared(10, "lightBuffer<LightInfo>");
    m_directionalViewInfo = DynamicStructuredBuffer<unsigned int>::CreateShared(1, "direcitonalViewInfo<uint>");

	org::memory::SetResourceUsageHint(*m_activeLightIndices, "Lighting buffers");
	org::memory::SetResourceUsageHint(*m_lightBuffer, "Lighting buffers");
	org::memory::SetResourceUsageHint(*m_directionalViewInfo, "Lighting buffers");

	getNumDirectionalLightCascades = SettingsManager::GetInstance().getSettingGetter<uint8_t>("numDirectionalLightCascades");
	getShadowResolution = SettingsManager::GetInstance().getSettingGetter<uint16_t>("shadowResolution");
	getMaxShadowDistance = SettingsManager::GetInstance().getSettingGetter<float>("maxShadowDistance");
	getDirectionalShadowSceneExtent = SettingsManager::GetInstance().getSettingGetter<float>("directionalShadowSceneExtent");
	getDirectionalVirtualShadowSourceAngleDegrees = SettingsManager::GetInstance().getSettingGetter<float>(
		CLodDirectionalVirtualShadowSourceAngleDegreesSettingName);

	m_pLightBufferResourceGroup = std::make_shared<org::ResourceGroup>("LightBufferResourceGroup");
	m_pLightBufferResourceGroup->AddResource(m_lightBuffer);
	m_pLightBufferResourceGroup->AddResource(m_activeLightIndices);

	auto getClusterSize = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT3>("lightClusterSize");
	auto lightClusterSize = getClusterSize();

	auto numClusters = lightClusterSize.x * lightClusterSize.y * lightClusterSize.z;
	m_pClusterBuffer = CreateIndexedStructuredBuffer(numClusters, sizeof(Cluster), true, false);
	m_pClusterBuffer->SetName("lightingClusterBuffer");
	org::memory::SetResourceUsageHint(*m_pClusterBuffer, "Lighting buffers");

	static const size_t avgPagesPerCluster = 10;
	m_lightPagePoolSize = numClusters * avgPagesPerCluster;
	m_pLightPagesBuffer = org::Buffer::CreateUnmaterializedStructuredBuffer(
		static_cast<uint32_t>(m_lightPagePoolSize),
		sizeof(LightPage),
		true,
		false,
		false,
		rhi::HeapType::DeviceLocal);
	m_pLightPagesBuffer->SetAllowAlias(true);
	m_pLightPagesBuffer->SetName("lightPagesBuffer");
	org::memory::SetResourceUsageHint(*m_pLightPagesBuffer, "Lighting buffers");

	m_resources[Builtin::Light::ClusterBuffer] = m_pClusterBuffer;
	m_resources[Builtin::Light::PagesBuffer] = m_pLightPagesBuffer;
	m_resources[Builtin::Light::InfoBuffer] = m_lightBuffer;
	m_resources[Builtin::Light::DirectionalLightCascadeBuffer] = m_directionalViewInfo;
	m_resources[Builtin::Light::ActiveLightIndices] = m_activeLightIndices;

    const auto publishedSource = br::render::PublishedStateSource::ProcessSource();
    const auto addPublished = [&](org::ResourceIdentifier key, std::shared_ptr<org::Resource> fallback,
        std::uint64_t variant) {
        m_resolvers[key] = std::make_shared<PublishedStateResourceResolver>(publishedSource,
            br::render::PublishedResourceKey{ br::render::PublishedFragmentKind::Lights,
                br::render::PublishedResourceUsage::ShaderResource, 0, 0, variant },
            std::move(fallback));
    };
    addPublished(Builtin::Light::InfoBuffer, m_lightBuffer, br::render::LightInfoTableVariant);
    addPublished(Builtin::Light::DirectionalLightCascadeBuffer, m_directionalViewInfo,
        br::render::LightDirectionalViewTableVariant);
    addPublished(Builtin::Light::ActiveLightIndices, m_activeLightIndices,
        br::render::LightActiveIndexTableVariant);

	m_resolvers[Builtin::Light::BufferGroup] = 
		std::make_shared<ResourceGroupResolver>(m_pLightBufferResourceGroup);
}

LightManager::~LightManager() {
	auto& deletionManager = org::DeletionManager::GetInstance();
}

namespace {
void PublishDirectionalShadowDebug(const std::vector<Cascade>& cascades)
{
	CLodDirectionalShadowDebugSnapshot snapshot{};
	snapshot.clipmapCount = static_cast<uint32_t>((std::min)(cascades.size(), static_cast<size_t>(CLodDirectionalShadowDebugMaxClipmaps)));
	for (uint32_t clipmapIndex = 0; clipmapIndex < snapshot.clipmapCount; ++clipmapIndex) {
		const Cascade& cascade = cascades[clipmapIndex];
		auto& entry = snapshot.clipmaps[clipmapIndex];
		entry.valid = 1u;
		entry.clipDiameter = cascade.size;
		entry.nearPlane = cascade.nearPlane;
		entry.farPlane = cascade.farPlane;
		entry.pageOffsetX = cascade.pageOffsetX;
		entry.pageOffsetY = cascade.pageOffsetY;
		entry.positionWorldSpace = {
			cascade.worldCenter.x,
			cascade.worldCenter.y,
			cascade.worldCenter.z,
			cascade.worldCenter.w
		};
	}

	PublishCLodDirectionalShadowDebugSnapshot(snapshot);
}
}

AddLightReturn LightManager::AddLight(LightInfo* lightInfo, uint64_t entityId) {
    auto lightBufferView = m_lightBuffer->Add(*lightInfo);
    uint32_t lightIndex = static_cast<uint32_t>(lightBufferView->GetOffset() / sizeof(LightInfo));
    m_activeLightIndices->Insert(lightIndex);
    {
        std::lock_guard<std::mutex> lock(m_lightUpdateMutex);
        if (m_slotGenerations.size() <= lightIndex) {
            m_slotGenerations.resize(lightIndex + 1u, 0u);
        }
        lightInfo->generation = ++m_slotGenerations[lightIndex];
        m_lightBuffer->UpdateView(lightBufferView.get(), lightInfo);
        m_activeIndices.insert(lightIndex);
        if (lightInfo->type == Components::LightType::Directional) {
            m_directionalLights[lightIndex] = lightInfo->shadowCaster != 0u;
        }
    }

    Components::LightViewInfo viewInfo;
    std::optional<Components::FrustumPlanes> planes = std::nullopt;

    // Only the sun is shadowed (by the virtual shadow map), so only directional lights get shadow views. A point or
    // spot light's shadowCaster flag is kept for shadowing modes that trace it.
    if (lightInfo->shadowCaster && lightInfo->type == Components::LightType::Directional) {
        std::tie(viewInfo, planes) = CreateDirectionalLightViewInfo(*lightInfo, entityId);
        viewInfo.depthResX = getShadowResolution();
        viewInfo.depthResY = getShadowResolution();
    }

    viewInfo.lightBufferIndex = lightIndex;
    viewInfo.lightBufferView = lightBufferView;

    m_publicationRevision.fetch_add(1, std::memory_order_release);
    return { viewInfo, planes };
}


void LightManager::RemoveLight(LightInfo* light) {

}

void LightManager::RemoveLight(flecs::entity light) {
	if (!light.is_alive() || !light.has<Components::LightViewInfo>()) {
		return;
	}

	auto& viewInfo = light.get<Components::LightViewInfo>();
	if (viewInfo.lightBufferView) {
		m_activeLightIndices->Remove(viewInfo.lightBufferIndex);
		m_lightBuffer->Remove(viewInfo.lightBufferView.get());
		std::lock_guard<std::mutex> lock(m_lightUpdateMutex);
		m_activeIndices.erase(viewInfo.lightBufferIndex);
		m_directionalLights.erase(viewInfo.lightBufferIndex);
	}

	RemoveLightViewInfo(light);
	light.remove<Components::LightViewInfo>();
	m_publicationRevision.fetch_add(1, std::memory_order_release);
}

unsigned int LightManager::GetNumLights() {
	return m_activeLightIndices->Size();
}

SunLightSelection LightManager::GetSunLight() {
	std::lock_guard<std::mutex> lock(m_lightUpdateMutex);
	SunLightSelection selection;
	for (const auto& [index, shadowCaster] : m_directionalLights) {
		if (shadowCaster) {
			selection.lightIndex = index;
			break;
		}
	}
	if (selection.lightIndex == 0xFFFFFFFFu && !m_directionalLights.empty()) {
		selection.lightIndex = m_directionalLights.begin()->first;
	}
	if (const auto it = m_activeIndices.find(selection.lightIndex); it != m_activeIndices.end()) {
		selection.activeSlot = static_cast<uint32_t>(std::distance(m_activeIndices.begin(), it));
	}
	else {
		selection.lightIndex = 0xFFFFFFFFu;
	}
	return selection;
}

std::pair<Components::LightViewInfo, std::optional<Components::FrustumPlanes>>
LightManager::CreateDirectionalLightViewInfo(const LightInfo& info, uint64_t entityId) {
	Components::LightViewInfo viewInfo = {};
	std::optional<Components::FrustumPlanes> cascadePlanes = std::nullopt;
	auto numCascades = getNumDirectionalLightCascades();
	viewInfo.viewInfoBufferIndex = m_directionalViewInfo->Size() / numCascades;

	if (!m_currentCamera.is_valid()) {
		spdlog::warn("Camera must be provided for directional light shadow mapping");
		return { viewInfo, cascadePlanes };
	}

	auto& camera = m_currentCamera.get<Components::Camera>();
	auto& matrix = m_currentCamera.get<Components::Matrix>().matrix;
	auto posFloats = GetGlobalPositionFromMatrix(matrix);
	const std::vector<float> directionalClipFarPlanes = calculateCascadeSplits(
		numCascades,
		camera.zNear,
		camera.zFar,
		std::max(getMaxShadowDistance(), camera.zNear));

	// Virtual shadow clip levels are nested around the primary camera.
	const float shadowDistanceLowerBound = std::max(
		SettingsManager::GetInstance().getSettingGetter<float>("directionalShadowDistanceLowerBound")(),
		1.0f);
	const float directionalLodBias =
		CLodVirtualShadowBuildRuntimeResolutionConfig().directionalLodBias;
	const float directionalResolutionScale = std::exp2(directionalLodBias);
	auto cascades = setupDirectionalClipmaps(numCascades, info.dirWorldSpace,
		DirectX::XMLoadFloat3(&posFloats), 
		GetForwardFromMatrix(matrix),
		GetUpFromMatrix(matrix),
		camera.zNear, camera.fov, camera.aspect,
		directionalClipFarPlanes,
		shadowDistanceLowerBound,
		std::max(getDirectionalShadowSceneExtent(), 0.0f),
		directionalResolutionScale);

	// Collect the frustum planes from each cascade.
	cascadePlanes = Components::FrustumPlanes();
	for (const auto& cascade : cascades) {
		cascadePlanes->frustumPlanes.push_back(cascade.frustumPlanes);
	}
	PublishDirectionalShadowDebug(cascades);

	// Create a camera and command buffers for each cascade.
	for (int i = 0; i < numCascades; i++) {
		CameraInfo cameraInfo = {};
		cameraInfo.positionWorldSpace = cascades[i].worldCenter;
		cameraInfo.view = cascades[i].viewMatrix;
		cameraInfo.viewInverse = DirectX::XMMatrixInverse(nullptr, cameraInfo.view);
		cameraInfo.unjitteredProjection = cascades[i].orthoMatrix;
		cameraInfo.jitteredProjection = cameraInfo.unjitteredProjection; // lights don't use jittering.
		cameraInfo.projectionInverse = DirectX::XMMatrixInverse(nullptr, cameraInfo.unjitteredProjection);
		cameraInfo.prevView = cameraInfo.view;
		cameraInfo.prevJitteredProjection = cameraInfo.jitteredProjection;
		cameraInfo.prevUnjitteredProjection = cameraInfo.unjitteredProjection;
		cameraInfo.viewProjection = DirectX::XMMatrixMultiply(cascades[i].viewMatrix, cascades[i].orthoMatrix);
		cameraInfo.aspectRatio = camera.aspect;
		cameraInfo.zNear = cascades[i].nearPlane;
		cameraInfo.zFar = cascades[i].farPlane;
		cameraInfo.clippingPlanes[0] = cascades[i].frustumPlanes[0];
		cameraInfo.clippingPlanes[1] = cascades[i].frustumPlanes[1];
		cameraInfo.clippingPlanes[2] = cascades[i].frustumPlanes[2];
		cameraInfo.clippingPlanes[3] = cascades[i].frustumPlanes[3];
		cameraInfo.clippingPlanes[4] = cascades[i].frustumPlanes[4];
		cameraInfo.clippingPlanes[5] = cascades[i].frustumPlanes[5];
		cameraInfo.depthBufferArrayIndex = i;
		cameraInfo.depthResX = getShadowResolution();
		cameraInfo.depthResY = getShadowResolution();
		cameraInfo.uvScaleToNextPowerOfTwo = {
			static_cast<float>(cameraInfo.depthResX) / static_cast<float>(GetNextPowerOfTwo(cameraInfo.depthResX)),
			static_cast<float>(cameraInfo.depthResY) / static_cast<float>(GetNextPowerOfTwo(cameraInfo.depthResY))
		};
		cameraInfo.numDepthMips = CalculateMipLevels(static_cast<uint16_t>(cameraInfo.depthResX), static_cast<uint16_t>(cameraInfo.depthResY));
		cameraInfo.isOrtho = true; // Directional lights use orthographic projection for shadows.
		// TODO: Needs near and far for depth unprojection
		ViewCreationParams viewParams{};
		viewParams.parentEntityID = entityId;
		viewParams.lightType = Components::LightType::Directional;
		viewParams.cascadeIndex = i;
		auto renderView = m_shadowViews->CreateShadowView(cameraInfo, ViewFlags::ShadowCascade(), viewParams);
		m_shadowViews->UpdateDirectionalClipView(renderView, cameraInfo, cascades[i].pageOffsetX, cascades[i].pageOffsetY);
		m_directionalViewInfo->Add(m_shadowViews->ShadowViewCameraBufferIndex(renderView));
		viewInfo.viewIDs.push_back(renderView);
	}
	return { viewInfo, cascadePlanes };
}

void LightManager::RebuildDirectionalLightViewInfoBuffer(std::optional<uint64_t> excludedLightEntityId) {
	while (m_directionalViewInfo->Size() > 0) {
		m_directionalViewInfo->RemoveAt(m_directionalViewInfo->Size() - 1);
	}

	auto& world = RendererECSManager::GetInstance().GetWorld();
	auto query = world.query_builder<Components::Light, Components::LightViewInfo>().build();
	query.each([&](flecs::entity entity, Components::Light& light, Components::LightViewInfo& viewInfo) {
		if (excludedLightEntityId.has_value() && entity.id() == excludedLightEntityId.value()) {
			return;
		}

		if (light.type != Components::LightType::Directional || !light.lightInfo.shadowCaster) {
			return;
		}

		viewInfo.viewInfoBufferIndex = m_directionalViewInfo->Size();
		for (uint64_t viewId : viewInfo.viewIDs) {
			m_directionalViewInfo->Add(m_shadowViews->ShadowViewCameraBufferIndex(viewId));
		}

		light.lightInfo.shadowViewInfoIndex = static_cast<int>(viewInfo.viewInfoBufferIndex);
		UpdateLightBufferView(viewInfo.lightBufferView.get(), light.lightInfo);
	});
}


void LightManager::UpdateLightViewInfo(flecs::entity light) {
	auto& lightInfo = light.get_mut<Components::Light>();
	if (lightInfo.type != Components::LightType::Directional) {
		return; // Only directional lights have shadow views.
	}
	m_publicationRevision.fetch_add(1, std::memory_order_release);
	auto viewInfo = light.get<Components::LightViewInfo>();
	auto& renderViewIds = viewInfo.viewIDs;
	bool lightViewInfoChanged = false;
	// The virtual shadow map's source angle setting is the sun's size for everything that reads the light.
	lightInfo.lightInfo.sourceHalfAngleDegrees = getDirectionalVirtualShadowSourceAngleDegrees();
	switch (lightInfo.type) {
	case Components::LightType::Directional: {
		if (!m_currentCamera.is_valid()) {
			spdlog::warn("Camera must be provided for directional light shadow mapping");
			return;
		}
		auto numCascades = getNumDirectionalLightCascades();
		if (renderViewIds.size() != static_cast<size_t>(numCascades)) {
			numCascades = static_cast<uint8_t>((std::min)(renderViewIds.size(), static_cast<size_t>(numCascades)));
			lightViewInfoChanged = true;
		}
		auto& camera = m_currentCamera.get<Components::Camera>();
		auto& matrix = m_currentCamera.get<Components::Matrix>().matrix;
		auto posFloats = GetGlobalPositionFromMatrix(matrix);
		const std::vector<float> directionalClipFarPlanes = calculateCascadeSplits(
			numCascades,
			camera.zNear,
			camera.zFar,
			std::max(getMaxShadowDistance(), camera.zNear));
		const float shadowDistanceLowerBound = std::max(
			SettingsManager::GetInstance().getSettingGetter<float>("directionalShadowDistanceLowerBound")(),
			1.0f);
		const float directionalLodBias =
			CLodVirtualShadowBuildRuntimeResolutionConfig().directionalLodBias;
		const float directionalResolutionScale = std::exp2(directionalLodBias);
		auto cascades = setupDirectionalClipmaps(numCascades, lightInfo.lightInfo.dirWorldSpace, DirectX::XMLoadFloat3(&posFloats), GetForwardFromMatrix(matrix), GetUpFromMatrix(matrix), camera.zNear, camera.fov, camera.aspect, directionalClipFarPlanes, shadowDistanceLowerBound, std::max(getDirectionalShadowSceneExtent(), 0.0f), directionalResolutionScale);
		PublishDirectionalShadowDebug(cascades);
		for (int i = 0; i < numCascades; i++) {
			CameraInfo info = {};
			// Match Timberdoodle's model: the shadow camera is derived from a page-aligned
			// projection of the primary camera into a fixed light-space basis.
			info.positionWorldSpace = cascades[i].worldCenter;
			info.view = cascades[i].viewMatrix;
			info.viewInverse = DirectX::XMMatrixInverse(nullptr, info.view);
			info.unjitteredProjection = cascades[i].orthoMatrix;
			info.jitteredProjection = info.unjitteredProjection; // lights don't use jittering.
			info.projectionInverse = DirectX::XMMatrixInverse(nullptr, info.unjitteredProjection);
			info.prevView = info.view;
			info.prevJitteredProjection = info.jitteredProjection;
			info.prevUnjitteredProjection = info.unjitteredProjection;
			info.viewProjection = DirectX::XMMatrixMultiply(cascades[i].viewMatrix, cascades[i].orthoMatrix);
			info.aspectRatio = camera.aspect;
			info.zNear = cascades[i].nearPlane;
			info.zFar = cascades[i].farPlane;
			info.clippingPlanes[0] = cascades[i].frustumPlanes[0];
			info.clippingPlanes[1] = cascades[i].frustumPlanes[1];
			info.clippingPlanes[2] = cascades[i].frustumPlanes[2];
			info.clippingPlanes[3] = cascades[i].frustumPlanes[3];
			info.clippingPlanes[4] = cascades[i].frustumPlanes[4];
			info.clippingPlanes[5] = cascades[i].frustumPlanes[5];
			info.depthBufferArrayIndex = i;
			info.depthResX = viewInfo.depthResX;
			info.depthResY = viewInfo.depthResY;
			unsigned int nextPowerOfTwoX = GetNextPowerOfTwo(viewInfo.depthResX);
			unsigned int nextPowerOfTwoY = GetNextPowerOfTwo(viewInfo.depthResY);
			info.uvScaleToNextPowerOfTwo = {
				static_cast<float>(viewInfo.depthResX) / static_cast<float>(nextPowerOfTwoX),
				static_cast<float>(viewInfo.depthResY) / static_cast<float>(nextPowerOfTwoY)
			};
			info.numDepthMips = CalculateMipLevels(static_cast<uint16_t>(info.depthResX), static_cast<uint16_t>(info.depthResY));
			info.isOrtho = true; // Directional lights use orthographic projection for shadows.
			m_shadowViews->UpdateDirectionalClipView(renderViewIds[i], info, cascades[i].pageOffsetX, cascades[i].pageOffsetY);
		}
		lightViewInfoChanged = true;
		UpdateLightBufferView(viewInfo.lightBufferView.get(), lightInfo.lightInfo);
		break;
	}
	default:
		spdlog::warn("Light type not recognized");
	}

	if (lightViewInfoChanged) {
		light.set<Components::LightViewInfo>(viewInfo);
	}
}

void LightManager::RemoveLightViewInfo(flecs::entity light) {

	//m_pCommandBufferManager->UnregisterBuffers(light.id()); // Remove indirect command buffers
	const auto* lightInfo = light.try_get<Components::Light>();
	const auto* viewInfo = light.try_get<Components::LightViewInfo>();
	if (!lightInfo || !viewInfo || !lightInfo->lightInfo.shadowCaster) {
		return;
	}

	for (const auto viewID : viewInfo->viewIDs) {
		m_shadowViews->DestroyShadowView(viewID);
	}
	if (lightInfo->type == Components::LightType::Directional) {
		RebuildDirectionalLightViewInfoBuffer(light.id());
	}
}

void LightManager::SetCurrentCamera(flecs::entity camera) {
	m_currentCamera = camera;
}


void LightManager::SetShadowViewService(br::render::IShadowViewService* service) {
	m_shadowViews = service;
}

std::vector<std::shared_ptr<const std::vector<std::byte>>> LightManager::CaptureTableImages() const {
    std::vector<std::shared_ptr<const std::vector<std::byte>>> result;
    // In LightTableFamilyCount order: LightInfoTableVariant, LightDirectionalViewTableVariant, LightActiveIndexTableVariant.
    result.reserve(br::render::LightTableFamilyCount);
    const auto capture = [&](const auto& buffer) {
        result.push_back(std::make_shared<const std::vector<std::byte>>(
            buffer->CaptureCpuShadowBytes()));
    };
    capture(m_lightBuffer);
    capture(m_directionalViewInfo);
    capture(m_activeLightIndices);
    return result;
}

void LightManager::UpdateLightBufferView(org::BufferView* view, const LightInfo& data) {
	std::lock_guard<std::mutex> lock(m_lightUpdateMutex);
	const auto lightIndex = static_cast<uint32_t>(view->GetOffset() / sizeof(LightInfo));
	LightInfo stamped = data;
	stamped.generation = lightIndex < m_slotGenerations.size() ? m_slotGenerations[lightIndex] : 0u;
	m_lightBuffer->UpdateView(view, &stamped);
	if (const auto it = m_directionalLights.find(lightIndex); it != m_directionalLights.end()) {
		it->second = data.shadowCaster != 0u;
	}
	m_publicationRevision.fetch_add(1, std::memory_order_release);
}

std::shared_ptr<org::Resource> LightManager::ProvideResource(org::ResourceIdentifier const& key) {
	return m_resources[key];
}

std::vector<org::ResourceIdentifier> LightManager::GetSupportedKeys() {
	std::vector<org::ResourceIdentifier> keys;
	keys.reserve(m_resources.size());
	for (auto const& [key, _] : m_resources)
		keys.push_back(key);

	return keys;
}

std::vector<org::ResourceIdentifier> LightManager::GetSupportedResolverKeys() {
	std::vector<org::ResourceIdentifier> keys;
	keys.reserve(m_resolvers.size());
	for (auto const& [k, _] : m_resolvers)
		keys.push_back(k);
	return keys;
}
std::shared_ptr<org::IResourceResolver> LightManager::ProvideResolver(org::ResourceIdentifier const& key) {
	auto it = m_resolvers.find(key);
	if (it == m_resolvers.end()) return nullptr;
	return it->second;
}
