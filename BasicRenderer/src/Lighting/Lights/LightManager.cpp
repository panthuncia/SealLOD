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
    m_spotViewInfo = DynamicStructuredBuffer<unsigned int>::CreateShared(1, "spotViewInfo<uint>");
    m_pointViewInfo = DynamicStructuredBuffer<unsigned int>::CreateShared(1, "pointViewInfo<uint>");
    m_directionalViewInfo = DynamicStructuredBuffer<unsigned int>::CreateShared(1, "direcitonalViewInfo<uint>");

	org::memory::SetResourceUsageHint(*m_activeLightIndices, "Lighting buffers");
	org::memory::SetResourceUsageHint(*m_lightBuffer, "Lighting buffers");
	org::memory::SetResourceUsageHint(*m_spotViewInfo, "Lighting buffers");
	org::memory::SetResourceUsageHint(*m_pointViewInfo, "Lighting buffers");
	org::memory::SetResourceUsageHint(*m_directionalViewInfo, "Lighting buffers");

	getNumDirectionalLightCascades = SettingsManager::GetInstance().getSettingGetter<uint8_t>("numDirectionalLightCascades");
	getShadowResolution = SettingsManager::GetInstance().getSettingGetter<uint16_t>("shadowResolution");
	getMaxShadowDistance = SettingsManager::GetInstance().getSettingGetter<float>("maxShadowDistance");
	getDirectionalShadowSceneExtent = SettingsManager::GetInstance().getSettingGetter<float>("directionalShadowSceneExtent");
	getDirectionalVirtualShadowSourceAngleDegrees = SettingsManager::GetInstance().getSettingGetter<float>(
		CLodDirectionalVirtualShadowSourceAngleDegreesSettingName);

	m_pLightViewInfoResourceGroup = std::make_shared<org::ResourceGroup>("LightViewInfo");
	m_pLightViewInfoResourceGroup->AddResource(m_spotViewInfo);
	m_pLightViewInfoResourceGroup->AddResource(m_pointViewInfo);

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
	m_resources[Builtin::Light::PointLightCubemapBuffer] = m_pointViewInfo;
	m_resources[Builtin::Light::SpotLightMatrixBuffer] = m_spotViewInfo;
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
    addPublished(Builtin::Light::SpotLightMatrixBuffer, m_spotViewInfo,
        br::render::LightSpotViewTableVariant);
    addPublished(Builtin::Light::PointLightCubemapBuffer, m_pointViewInfo,
        br::render::LightPointViewTableVariant);
    addPublished(Builtin::Light::DirectionalLightCascadeBuffer, m_directionalViewInfo,
        br::render::LightDirectionalViewTableVariant);
    addPublished(Builtin::Light::ActiveLightIndices, m_activeLightIndices,
        br::render::LightActiveIndexTableVariant);

	m_resolvers[Builtin::Light::ViewResourceGroup] = 
		std::make_shared<ResourceGroupResolver>(m_pLightViewInfoResourceGroup);
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

    Components::LightViewInfo viewInfo;
    std::optional<Components::FrustumPlanes> planes = std::nullopt;

    if (lightInfo->shadowCaster) {
        switch (lightInfo->type) {
            case Components::LightType::Point:
                std::tie(viewInfo, planes) = CreatePointLightViewInfo(*lightInfo, entityId);
                break;
            case Components::LightType::Spot:
                std::tie(viewInfo, planes) = CreateSpotLightViewInfo(*lightInfo, entityId);
                break;
            case Components::LightType::Directional:
                std::tie(viewInfo, planes) = CreateDirectionalLightViewInfo(*lightInfo, entityId);
                break;
            default:
                spdlog::warn("Unhandled light type");
                break;
        }

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
	}

	RemoveLightViewInfo(light);
	light.remove<Components::LightViewInfo>();
	m_publicationRevision.fetch_add(1, std::memory_order_release);
}

unsigned int LightManager::GetNumLights() {
	return m_activeLightIndices->Size();
}

std::pair<Components::LightViewInfo, std::optional<Components::FrustumPlanes>>
LightManager::CreatePointLightViewInfo(const LightInfo& info, uint64_t entityId) {
	Components::LightViewInfo viewInfo = {};
	// Assume each cubemap face uses the same projection but different view matrices.
	auto cubeViewIndex = m_pointViewInfo->Size() / 6;
	viewInfo.viewInfoBufferIndex = cubeViewIndex;
	DirectX::XMFLOAT3 pos;
	DirectX::XMStoreFloat3(&pos, info.posWorldSpace);
	auto cubemapMatrices = GetCubemapViewMatrices(pos);

	auto projection = GetProjectionMatrixForLight(info);

	// For each face of the cubemap, create a camera view
	for (int i = 0; i < 6; i++) {
		CameraInfo camera = {};
		camera.positionWorldSpace = { pos.x, pos.y, pos.z, 1.0f };
		camera.view = cubemapMatrices[i];
		camera.unjitteredProjection = projection;
		camera.jitteredProjection = projection; // lights don't use jittering.
		camera.prevView = camera.view;
		camera.prevJitteredProjection = camera.jitteredProjection;
		camera.prevUnjitteredProjection = camera.unjitteredProjection;
		camera.viewProjection = XMMatrixMultiply(cubemapMatrices[i], camera.unjitteredProjection);

		ViewCreationParams viewParams{};
		viewParams.parentEntityID = entityId;
		viewParams.lightType = Components::LightType::Point;
		auto renderView = m_shadowViews->CreateShadowView(camera, ViewFlags::ShadowFace(), viewParams);
		m_pointViewInfo->Add(m_shadowViews->ShadowViewCameraBufferIndex(renderView));
		viewInfo.viewIDs.push_back(renderView);
	}	
	
	viewInfo.projectionMatrix = Components::Matrix(projection);

	return { viewInfo, std::nullopt }; // Point lights don't need extra frustum data.
}

std::pair<Components::LightViewInfo, std::optional<Components::FrustumPlanes>>
LightManager::CreateSpotLightViewInfo(const LightInfo& info, uint64_t entityId) {
	Components::LightViewInfo viewInfo = {};
	viewInfo.viewInfoBufferIndex = m_spotViewInfo->Size();

	DirectX::XMFLOAT3 pos;
	DirectX::XMStoreFloat3(&pos, info.posWorldSpace);

	CameraInfo camera = {};
	camera.positionWorldSpace = { pos.x, pos.y, pos.z, 1.0f };
	DirectX::XMFLOAT3 up = { 0, 1, 0 };
	camera.view = DirectX::XMMatrixLookToRH(info.posWorldSpace, info.dirWorldSpace, DirectX::XMLoadFloat3(&up));
	camera.unjitteredProjection = GetProjectionMatrixForLight(info);
	camera.jitteredProjection = camera.unjitteredProjection; // lights don't use jittering.
	camera.prevView = camera.view;
	camera.prevJitteredProjection = camera.jitteredProjection;
	camera.prevUnjitteredProjection = camera.unjitteredProjection;
	camera.viewProjection = DirectX::XMMatrixMultiply(camera.view, camera.unjitteredProjection);

	ViewCreationParams viewParams{};
	viewParams.parentEntityID = entityId;
	viewParams.lightType = Components::LightType::Spot;
	auto renderView = m_shadowViews->CreateShadowView(camera, ViewFlags::ShadowFace(), viewParams);
	m_spotViewInfo->Add(m_shadowViews->ShadowViewCameraBufferIndex(renderView));
	viewInfo.viewIDs.push_back(renderView);

	viewInfo.projectionMatrix = Components::Matrix(camera.unjitteredProjection);
	return { viewInfo, std::nullopt };
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
	viewInfo.virtualShadowUnwrappedPageOffsetX.resize(numCascades);
	viewInfo.virtualShadowUnwrappedPageOffsetY.resize(numCascades);
	for (int i = 0; i < numCascades; i++) {
		viewInfo.virtualShadowUnwrappedPageOffsetX[i] = cascades[i].pageOffsetX;
		viewInfo.virtualShadowUnwrappedPageOffsetY[i] = cascades[i].pageOffsetY;
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
	m_publicationRevision.fetch_add(1, std::memory_order_release);
	//auto projectionMatrix = light.get<Components::ProjectionMatrix>();
	auto viewInfo = light.get<Components::LightViewInfo>();
	auto& renderViewIds = viewInfo.viewIDs;
	bool lightViewInfoChanged = false;
	auto& lightInfo = light.get_mut<Components::Light>();
	auto& lightMatrix = light.get<Components::Matrix>();
	auto& planes = light.get<Components::FrustumPlanes>().frustumPlanes;
	auto globalPos = GetGlobalPositionFromMatrix(lightMatrix.matrix);
	if (lightInfo.type == Components::LightType::Directional) {
		lightInfo.lightInfo.shadowSourceAngleDegrees = getDirectionalVirtualShadowSourceAngleDegrees();
	}
	switch (lightInfo.type) {
	case Components::LightType::Point: {
		auto cubemapMatrices = GetCubemapViewMatrices(globalPos);
		for (int i = 0; i < 6; i++) {
			//const CameraInfo* oldInfo = light.get<CameraInfo>();
			CameraInfo info = {};
			info.positionWorldSpace = { globalPos.x, globalPos.y, globalPos.z, 1.0 };
			info.view = cubemapMatrices[i];
			info.unjitteredProjection = viewInfo.projectionMatrix.matrix;
			info.jitteredProjection = info.unjitteredProjection; // lights don't use jittering.
			info.prevView = info.view;
			info.prevJitteredProjection = info.jitteredProjection;
			info.prevUnjitteredProjection = info.unjitteredProjection;
			info.viewProjection = XMMatrixMultiply(cubemapMatrices[i], viewInfo.projectionMatrix.matrix);
			info.clippingPlanes[0] = planes[i][0];
			info.clippingPlanes[1] = planes[i][1];
			info.clippingPlanes[2] = planes[i][2];
			info.clippingPlanes[3] = planes[i][3];
			info.clippingPlanes[4] = planes[i][4];
			info.clippingPlanes[5] = planes[i][5];
			info.depthBufferArrayIndex = i;
			info.depthResX = viewInfo.depthResX;
			info.depthResY = viewInfo.depthResY;
			info.uvScaleToNextPowerOfTwo = {
				static_cast<float>(viewInfo.depthResX) / static_cast<float>(GetNextPowerOfTwo(viewInfo.depthResX)),
				static_cast<float>(viewInfo.depthResY) / static_cast<float>(GetNextPowerOfTwo(viewInfo.depthResY))
			};
			info.numDepthMips = CalculateMipLevels(static_cast<uint16_t>(info.depthResX), static_cast<uint16_t>(info.depthResY));
			m_shadowViews->UpdateShadowView(renderViewIds[i], info);
		}
		break;
	}
	case Components::LightType::Spot: {
		CameraInfo camera = {};
		camera.positionWorldSpace = { globalPos.x, globalPos.y, globalPos.z, 1.0 };
		auto up = DirectX::XMFLOAT3(0, 1, 0);
		camera.view = DirectX::XMMatrixLookToRH(DirectX::XMLoadFloat3(&globalPos), DirectX::XMVector3Normalize(lightMatrix.matrix.r[2]), XMLoadFloat3(&up));
		camera.unjitteredProjection = viewInfo.projectionMatrix.matrix;
		camera.jitteredProjection = camera.unjitteredProjection; // lights don't use jittering.
		camera.prevView = camera.view;
		camera.prevJitteredProjection = camera.jitteredProjection;
		camera.prevUnjitteredProjection = camera.unjitteredProjection;
		camera.viewProjection = DirectX::XMMatrixMultiply(camera.view, viewInfo.projectionMatrix.matrix);
		camera.clippingPlanes[0] = planes[0][0];
		camera.clippingPlanes[1] = planes[0][1];
		camera.clippingPlanes[2] = planes[0][2];
		camera.clippingPlanes[3] = planes[0][3];
		camera.clippingPlanes[4] = planes[0][4];
		camera.clippingPlanes[5] = planes[0][5];
		camera.depthBufferArrayIndex = 0;
		camera.depthResX = viewInfo.depthResX;
		camera.depthResY = viewInfo.depthResY;
		camera.uvScaleToNextPowerOfTwo = {
			static_cast<float>(viewInfo.depthResX) / static_cast<float>(GetNextPowerOfTwo(viewInfo.depthResX)),
			static_cast<float>(viewInfo.depthResY) / static_cast<float>(GetNextPowerOfTwo(viewInfo.depthResY))
		};
		camera.numDepthMips = CalculateMipLevels(static_cast<uint16_t>(camera.depthResX), static_cast<uint16_t>(camera.depthResY));

		m_shadowViews->UpdateShadowView(renderViewIds[0], camera);
		break;
	}
	case Components::LightType::Directional: {
		if (!m_currentCamera.is_valid()) {
			spdlog::warn("Camera must be provided for directional light shadow mapping");
			return;
		}
		auto numCascades = getNumDirectionalLightCascades();
		if (renderViewIds.size() != static_cast<size_t>(numCascades)) {
			numCascades = static_cast<uint8_t>((std::min)(renderViewIds.size(), static_cast<size_t>(numCascades)));
			viewInfo.virtualShadowUnwrappedPageOffsetX.resize(numCascades);
			viewInfo.virtualShadowUnwrappedPageOffsetY.resize(numCascades);
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
		viewInfo.virtualShadowUnwrappedPageOffsetX.resize(numCascades);
		viewInfo.virtualShadowUnwrappedPageOffsetY.resize(numCascades);
		for (int i = 0; i < numCascades; i++) {
			viewInfo.virtualShadowUnwrappedPageOffsetX[i] = cascades[i].pageOffsetX;
			viewInfo.virtualShadowUnwrappedPageOffsetY[i] = cascades[i].pageOffsetY;
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
			m_shadowViews->UpdateShadowView(renderViewIds[i], info);
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

	switch (lightInfo->type) {
	case Components::LightType::Point: {
		const auto& views = viewInfo->viewIDs;
		for (size_t i = 0; i < views.size(); i++) {
			m_shadowViews->DestroyShadowView(views[i]);
		}
		break;
	}
	case Components::LightType::Spot: {
		for (const auto viewID : viewInfo->viewIDs) {
			m_shadowViews->DestroyShadowView(viewID);
		}
		break;
	}
	case Components::LightType::Directional: {
		const auto& views = viewInfo->viewIDs;
		for (size_t i = 0; i < views.size(); i++) {
			m_shadowViews->DestroyShadowView(views[i]);
		}
		RebuildDirectionalLightViewInfoBuffer(light.id());
		break;
	}
	default:
		spdlog::warn("Light type not recognized");
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
    result.reserve(5);
    const auto capture = [&](const auto& buffer) {
        result.push_back(std::make_shared<const std::vector<std::byte>>(
            buffer->CaptureCpuShadowBytes()));
    };
    capture(m_lightBuffer);
    capture(m_spotViewInfo);
    capture(m_pointViewInfo);
    capture(m_directionalViewInfo);
    capture(m_activeLightIndices);
    return result;
}

void LightManager::UpdateLightBufferView(org::BufferView* view, const LightInfo& data) {
	std::lock_guard<std::mutex> lock(m_lightUpdateMutex);
	m_lightBuffer->UpdateView(view, &data);
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
