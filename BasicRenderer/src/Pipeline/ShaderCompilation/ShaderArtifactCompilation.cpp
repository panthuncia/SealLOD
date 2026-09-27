#include <BasicRenderer/Assets/ShaderArtifactCompilation.h>

#include "Pipeline/PipelineState/PSOManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Pipeline/ShaderVariantRequestService.h"

void br::assets::RegisterHeadlessStaticImportSettings()
{
	auto& settings = SettingsManager::GetInstance();
	const auto clipmapCount = static_cast<std::uint8_t>(CLodVirtualShadowDefaultClipmapCount);
	settings.registerSetting<std::uint8_t>("numDirectionalLightCascades", clipmapCount);
	settings.registerSetting<std::vector<float>>(
		"directionalLightCascadeSplits",
		std::vector<float>(clipmapCount, 1.0f));
	settings.registerSetting<float>("directionalShadowSceneExtent", 0.0f);
	settings.registerSetting<bool>("enableMeshShader", false);
	settings.registerSetting<CLodSoftwareRasterMode>(
		CLodSoftwareRasterModeSettingName, CLodSoftwareRasterMode::Compute);
	settings.registerSetting<CLodVSMRasterMode>(
		CLodVSMRasterModeSettingName, CLodVSMRasterMode::Standard);
	settings.registerSetting<CLodTransparencyMode>(
		CLodTransparencyModeSettingName, CLodTransparencyMode::Disabled);
	settings.registerSetting<bool>(CLodDisableReyesRasterizationSettingName, false);
}

void br::assets::InitializeShaderArtifactCompiler()
{
	PSOManager::GetInstance().initializeShaderCompiler();
}

void br::assets::PrecompileShaderArtifact(const ShaderVariantRequest& request)
{
	PSOManager::GetInstance().PrecompileShaderArtifact(request);
}

void br::assets::PrecompileShaderBundleArtifact(const ShaderInfoBundle& shaderInfoBundle)
{
	PSOManager::GetInstance().PrecompileShaderBundleArtifact(shaderInfoBundle);
}
