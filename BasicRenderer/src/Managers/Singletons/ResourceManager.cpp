#include "Managers/Singletons/ResourceManager.h"

#include <rhi_helpers.h>
#include <OpenRenderGraph/OpenRenderGraph.h>

#include "Utilities/Utilities.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/Runtime/UploadServiceAccess.h"

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

    uint32_t PackDirectionalVirtualShadowSmrtCounts(uint32_t rayCount, uint32_t samplesPerRay)
    {
        const uint32_t clampedRayCount = (std::min)(rayCount, 0xFFFFu);
        const uint32_t clampedSamplesPerRay = (std::min)(samplesPerRay, 0xFFFFu);
        return clampedRayCount | (clampedSamplesPerRay << 16u);
    }
}

void ResourceManager::Initialize() {

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

void ResourceManager::UpdatePerFrameBuffer(UINT cameraIndex, UINT numLights, DirectX::XMUINT2 screenRes, DirectX::XMUINT3 clusterSizes, unsigned int frameIndex) {
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
	perFrameCBData.terrainStochasticSamplingEnabled =
		SettingsManager::GetInstance().getSettingGetter<bool>("enableTerrainStochasticSampling")() ? 1u : 0u;
	perFrameCBData.terrainStochasticDiffuseEnabled =
		SettingsManager::GetInstance().getSettingGetter<bool>("enableTerrainStochasticDiffuseSampling")() ? 1u : 0u;
	perFrameCBData.terrainStochasticNormalEnabled =
		SettingsManager::GetInstance().getSettingGetter<bool>("enableTerrainStochasticNormalSampling")() ? 1u : 0u;

	BUFFER_UPLOAD(&perFrameCBData, sizeof(PerFrameCB), rg::runtime::UploadTarget::FromShared(m_perFrameBuffer), 0);
}
void ResourceManager::Cleanup()
{
	m_perFrameBuffer.reset();
	m_uavCounterReset.Reset();
}
