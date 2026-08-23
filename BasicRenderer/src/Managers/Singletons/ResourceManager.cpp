#include "Managers/Singletons/ResourceManager.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <rhi_helpers.h>
#include <string_view>
#include <OpenRenderGraph/OpenRenderGraph.h>
#include <spdlog/spdlog.h>

#include "Utilities/Utilities.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "Render/TerrainRvtTelemetry.h"

namespace
{
	void UpdateDirectionalShadowConstants(PerFrameCB& perFrameCBData)
	{
		perFrameCBData.numDirectionalClipmaps = SettingsManager::GetInstance().getSettingGetter<uint8_t>("numDirectionalLightCascades")();
		auto shadowCascadeSplits = SettingsManager::GetInstance().getSettingGetter<std::vector<float>>("directionalLightCascadeSplits")();
		switch (perFrameCBData.numDirectionalClipmaps) {
		case 1:
			perFrameCBData.shadowCascadeSplits = DirectX::XMVectorSet(shadowCascadeSplits[0], 0, 0, 0);
			break;
		case 2:
			perFrameCBData.shadowCascadeSplits = DirectX::XMVectorSet(shadowCascadeSplits[0], shadowCascadeSplits[1], 0, 0);
			break;
		case 3:
			perFrameCBData.shadowCascadeSplits = DirectX::XMVectorSet(shadowCascadeSplits[0], shadowCascadeSplits[1], shadowCascadeSplits[2], 0);
			break;
		default:
			perFrameCBData.shadowCascadeSplits = DirectX::XMVectorSet(
				shadowCascadeSplits.size() > 0 ? shadowCascadeSplits[0] : 0.0f,
				shadowCascadeSplits.size() > 1 ? shadowCascadeSplits[1] : 0.0f,
				shadowCascadeSplits.size() > 2 ? shadowCascadeSplits[2] : 0.0f,
				shadowCascadeSplits.size() > 3 ? shadowCascadeSplits[3] : 0.0f);
			break;
		}
	}

	bool TerrainParallaxDiagnosticsEnabled()
	{
		static const bool enabled = [] {
			const auto envEnabled = [](const char* name) {
				char* rawValue = nullptr;
				std::size_t valueLength = 0;
				if (_dupenv_s(&rawValue, &valueLength, name) != 0 || rawValue == nullptr) {
					return false;
				}
				const std::unique_ptr<char, decltype(&std::free)> valueStorage{ rawValue, &std::free };
				const std::string_view value{ valueStorage.get() };
				return !(value == "0" || value == "false" || value == "FALSE" || value == "off" || value == "OFF");
			};
			return envEnabled("SARP_TERRAIN_TEXTURE_DIAGNOSTICS") ||
				envEnabled("SARP_TERRAIN_LAYER_DIAGNOSTICS") ||
				envEnabled("SARP_TERRAIN_PARALLAX_DIAGNOSTICS");
		}();
		return enabled;
	}

    uint32_t PackDirectionalVirtualShadowSmrtCounts(uint32_t rayCount, uint32_t samplesPerRay)
    {
        const uint32_t clampedRayCount = (std::min)(rayCount, 0xFFFFu);
        const uint32_t clampedSamplesPerRay = (std::min)(samplesPerRay, 0xFFFFu);
        return clampedRayCount | (clampedSamplesPerRay << 16u);
    }
}

void ::ResourceManager::Initialize() {

	auto device = DeviceManager::GetInstance().GetDevice();

	m_perFrameBuffer = CreateIndexedConstantBuffer(sizeof(PerFrameCB), "PerFrameCB");

	perFrameCBData.ambientLighting = DirectX::XMVectorSet(0.1f, 0.1f, 0.1f, 1.0f);
	UpdateDirectionalShadowConstants(perFrameCBData);

	auto result = device.CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(sizeof(UINT), rhi::HeapType::Upload), m_uavCounterReset);

	void* pMappedCounterReset = nullptr;
	
    m_uavCounterReset->Map(&pMappedCounterReset, 0, sizeof(UINT));
	ZeroMemory(pMappedCounterReset, sizeof(UINT));
	m_uavCounterReset->Unmap(0, 0);
}

void ::ResourceManager::UpdatePerFrameBuffer(UINT cameraIndex, UINT numLights, DirectX::XMUINT2 screenRes, DirectX::XMUINT3 clusterSizes, unsigned int frameIndex) {
	UpdateDirectionalShadowConstants(perFrameCBData);
	perFrameCBData.mainCameraIndex = cameraIndex;
	perFrameCBData.numLights = numLights;
	perFrameCBData.screenResX = screenRes.x;
	perFrameCBData.screenResY = screenRes.y;
	perFrameCBData.lightClusterGridSizeX = clusterSizes.x;
	perFrameCBData.lightClusterGridSizeY = clusterSizes.y;
	perFrameCBData.lightClusterGridSizeZ = clusterSizes.z;
	perFrameCBData.nearClusterCount = 4;
	perFrameCBData.clusterZSplitDepth = 6.0f;
	perFrameCBData.frameIndex = frameIndex;
    perFrameCBData.shadowVirtualSmrtDirectionalCountsPacked = PackDirectionalVirtualShadowSmrtCounts(
        SettingsManager::GetInstance().getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowSmrtRayCountDirectionalSettingName)(),
        SettingsManager::GetInstance().getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowSmrtSamplesPerRayDirectionalSettingName)());
    perFrameCBData.shadowVirtualSmrtMaxRayAngleFromLightDegrees =
        SettingsManager::GetInstance().getSettingGetter<float>(CLodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegreesSettingName)();
    perFrameCBData.shadowVirtualSmrtRayLengthScaleDirectional =
        SettingsManager::GetInstance().getSettingGetter<float>(CLodDirectionalVirtualShadowSmrtRayLengthScaleDirectionalSettingName)();
	perFrameCBData.shadowVirtualSmrtMaxTraceDistanceWorld =
		SettingsManager::GetInstance().getSettingGetter<float>(CLodDirectionalVirtualShadowSmrtMaxTraceDistanceWorldSettingName)();
    perFrameCBData.shadowVirtualReceiverTraceEnabled =
        SettingsManager::GetInstance().getSettingGetter<bool>(CLodDirectionalVirtualShadowReceiverTraceEnabledSettingName)() ? 1u : 0u;
    perFrameCBData.shadowVirtualReceiverTraceSampleCount =
        std::clamp(
            SettingsManager::GetInstance().getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowReceiverTraceSampleCountSettingName)(),
            1u,
            32u);
    perFrameCBData.shadowVirtualReceiverTraceMaxDistanceWorld =
        std::max(
            SettingsManager::GetInstance().getSettingGetter<float>(CLodDirectionalVirtualShadowReceiverTraceMaxDistanceWorldSettingName)(),
            0.0f);
    perFrameCBData.shadowVirtualReceiverTraceUncertaintyScale =
        std::max(
            SettingsManager::GetInstance().getSettingGetter<float>(CLodDirectionalVirtualShadowReceiverTraceUncertaintyScaleSettingName)(),
            0.0f);
    perFrameCBData.shadowVirtualReceiverTraceDepthSafetyScale =
        std::max(
            SettingsManager::GetInstance().getSettingGetter<float>(CLodDirectionalVirtualShadowReceiverTraceDepthSafetyScaleSettingName)(),
            0.0f);
	perFrameCBData.terrainStochasticSamplingEnabled =
		SettingsManager::GetInstance().getSettingGetter<bool>("enableTerrainStochasticSampling")() ? 1u : 0u;
	perFrameCBData.terrainStochasticDiffuseEnabled =
		SettingsManager::GetInstance().getSettingGetter<bool>("enableTerrainStochasticDiffuseSampling")() ? 1u : 0u;
	perFrameCBData.terrainStochasticNormalEnabled =
		SettingsManager::GetInstance().getSettingGetter<bool>("enableTerrainStochasticNormalSampling")() ? 1u : 0u;
	perFrameCBData.terrainStochasticDerivativeNormalsEnabled =
		SettingsManager::GetInstance().getSettingGetter<bool>("enableTerrainStochasticDerivativeNormalSampling")() ? 1u : 0u;
	perFrameCBData.terrainStochasticBlendCurve =
		SettingsManager::GetInstance().getSettingGetter<float>("terrainStochasticBlendCurve")();
	perFrameCBData.terrainGaussianStochasticEnabled =
		SettingsManager::GetInstance().getSettingGetter<bool>("enableTerrainGaussianStochasticSampling")() ? 1u : 0u;
	perFrameCBData.parallaxOcclusionMappingEnabled =
		SettingsManager::GetInstance().getSettingGetter<bool>("enableParallaxOcclusionMapping")() ? 1u : 0u;
	perFrameCBData.terrainParallaxOcclusionMappingEnabled =
		SettingsManager::GetInstance().getSettingGetter<bool>("enableTerrainParallaxOcclusionMapping")() ? 1u : 0u;
	perFrameCBData.terrainParallaxHeightScale =
		SettingsManager::GetInstance().getSettingGetter<float>("terrainParallaxHeightScale")();
	perFrameCBData.terrainParallaxMaxSteps =
		SettingsManager::GetInstance().getSettingGetter<uint32_t>("terrainParallaxMaxSteps")();
	perFrameCBData.heightFadeStartDistance =
		SettingsManager::GetInstance().getSettingGetter<float>("terrainParallaxFadeStartDistance")();
	perFrameCBData.heightFadeEndDistance =
		SettingsManager::GetInstance().getSettingGetter<float>("terrainParallaxFadeEndDistance")();
	perFrameCBData.terrainRvtEnabled =
		SettingsManager::GetInstance().getSettingGetter<bool>("enableTerrainRvt")() ? 1u : 0u;
	perFrameCBData.terrainRvtForceDirectFallback =
		SettingsManager::GetInstance().getSettingGetter<bool>("forceDirectTerrainRvtFallback")() ? 1u : 0u;
	perFrameCBData.terrainRvtDebugView =
		SettingsManager::GetInstance().getSettingGetter<uint32_t>("terrainRvtDebugView")();
	perFrameCBData.terrainRvtTelemetryEnabled = IsTerrainRvtTelemetryDebugEnabled() ? 1u : 0u;
	perFrameCBData.terrainReyesDisplacementScale =
		SettingsManager::GetInstance().getSettingGetter<float>("terrainReyesDisplacementGlobalScale")();
	perFrameCBData.objectReyesDisplacementScale =
		SettingsManager::GetInstance().getSettingGetter<float>("objectReyesDisplacementScale")();
	perFrameCBData.objectParallaxHeightScale =
		SettingsManager::GetInstance().getSettingGetter<float>("objectParallaxHeightScale")();
	if (TerrainParallaxDiagnosticsEnabled()) {
		static std::atomic_bool logged{ false };
		bool expected = false;
		if (logged.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
			spdlog::info(
				"SARP terrain: per-frame height constants pom={} terrainPom={} heightScale={} maxSteps={} fadeStart={} fadeEnd={}",
				perFrameCBData.parallaxOcclusionMappingEnabled,
				perFrameCBData.terrainParallaxOcclusionMappingEnabled,
				perFrameCBData.terrainParallaxHeightScale,
				perFrameCBData.terrainParallaxMaxSteps,
				perFrameCBData.heightFadeStartDistance,
				perFrameCBData.heightFadeEndDistance);
		}
	}

	BUFFER_UPLOAD(&perFrameCBData, sizeof(PerFrameCB), org::runtime::UploadTarget::FromShared(m_perFrameBuffer), 0);
}
void ::ResourceManager::Cleanup()
{
	m_perFrameBuffer.reset();
	m_uavCounterReset.Reset();
}
