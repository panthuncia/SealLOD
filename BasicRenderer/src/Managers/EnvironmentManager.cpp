#include "Managers/EnvironmentManager.h"

#include <filesystem>
#include <spdlog/spdlog.h>
#include <rhi.h>

#include "Managers/Singletons/ResourceManager.h"
#include "Scene/Environment.h"
#include "Resources/Buffers/LazyDynamicStructuredBuffer.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Resources/PixelBuffer.h"
#include "Resources/Sampler.h"
#include "Resources/ResourceGroup.h"
#include "Resources/Texture.h"
#include "../../generated/BuiltinResources.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "Render/MemoryIntrospectionAPI.h"

EnvironmentManager::EnvironmentManager() {
	auto& resourceManager = ::ResourceManager::GetInstance();
	m_skyboxResolution = SettingsManager::GetInstance().getSettingGetter<uint16_t>("skyboxResolution")();
	m_reflectionCubemapResolution = SettingsManager::GetInstance().getSettingGetter<uint16_t>("reflectionCubemapResolution")();
	m_environmentInfoBuffer = LazyDynamicStructuredBuffer<EnvironmentInfo>::CreateShared(1, "environmentsBuffer", 0, true);
	org::memory::SetResourceUsageHint(*m_environmentInfoBuffer, "Environment Info");

	m_workingEnvironmentCubemapGroup = std::make_shared<ResourceGroup>("EnvironmentCubemapGroup");
	m_workingHDRIGroup = std::make_shared<ResourceGroup>("WorkingHDRIGroup");
	m_environmentPrefilteredCubemapGroup = std::make_shared<ResourceGroup>("EnvironmentPrefilteredCubemapGroup");

	m_resources[Builtin::Environment::InfoBuffer] = m_environmentInfoBuffer;

	m_resolvers[Builtin::Environment::PrefilteredCubemapsGroup] =
		std::make_shared<ResourceGroupResolver>(m_environmentPrefilteredCubemapGroup);
	m_resolvers[Builtin::Environment::WorkingHDRIGroup] = 
		std::make_shared<ResourceGroupResolver>(m_workingHDRIGroup);
	m_resolvers[Builtin::Environment::WorkingCubemapGroup] = 
		std::make_shared<ResourceGroupResolver>(m_workingEnvironmentCubemapGroup);
}

std::unique_ptr<Environment> EnvironmentManager::CreateEnvironment(std::wstring name) {
	auto view = m_environmentInfoBuffer->Add();
	std::unique_ptr<Environment> env = std::make_unique<Environment>(this, name);
	env->SetEnvironmentBufferView(view);

	ImageDimensions dims;
	dims.height = m_reflectionCubemapResolution;
	dims.width = m_reflectionCubemapResolution;
	dims.rowPitch = m_reflectionCubemapResolution * 4;
	dims.slicePitch = m_reflectionCubemapResolution * m_reflectionCubemapResolution * 4;

	TextureDescription prefilteredDesc;
	for (int i = 0; i < 6; i++) {
		prefilteredDesc.imageDimensions.push_back(dims);
	}
	prefilteredDesc.channels = 3;
	prefilteredDesc.isCubemap = true;
	//prefilteredDesc.hasRTV = true;
	prefilteredDesc.format = rhi::Format::R8G8B8A8_UNorm;
	prefilteredDesc.generateMipMaps = true;
	prefilteredDesc.hasUAV = true;

	auto prefilteredEnvironmentCubemap = PixelBuffer::CreateShared(prefilteredDesc);
	org::memory::SetResourceUsageHint(*prefilteredEnvironmentCubemap, "Environment lighting");
	auto sampler = Sampler::GetDefaultSampler();
	auto prefilteredEnvironment = TextureAsset::CreateShared(prefilteredDesc, prefilteredEnvironmentCubemap, sampler, TextureFileMeta());
	prefilteredEnvironment->SetName("Environment prefiltered cubemap");

	env->SetEnvironmentPrefilteredCubemap(prefilteredEnvironment);
	env->SetReflectionCubemapResolution(m_reflectionCubemapResolution);

	m_environmentPrefilteredCubemapGroup->AddResource(prefilteredEnvironment->ImagePtr());

	return std::move(env);
}

void EnvironmentManager::SetFromHDRI(Environment* e, std::string hdriPath) {
	std::lock_guard<std::mutex> lock(m_environmentUpdateMutex);

	// Check if this environment has been processed and cached. If it has, load the cache. If it hasn't, load the environment and process it.
	auto& name = e->GetName();
	auto skyboxPath = GetCacheFilePath(name + L"_environment.dds", L"environments");

	std::shared_ptr<TextureAsset> skybox;
	unsigned int res = m_reflectionCubemapResolution;
	if (std::filesystem::exists(skyboxPath)) {
		skybox = LoadCubemapFromFile(skyboxPath, true, true);
		auto factory = TextureFactory::CreateUnique();
		skybox->EnsureUploaded(*factory);
		skybox->SetName("Skybox cubemap");
		org::memory::SetResourceUsageHint(*skybox->ImagePtr(), "Environment lighting");
		res = skybox->GetWidth();
		e->SetReflectionCubemapResolution(res);
		e->SetEnvironmentCubemap(skybox);
	}
	else {
		auto skyHDR = LoadTextureFromFile(s2ws(hdriPath));
		auto factory = TextureFactory::CreateUnique();
		skyHDR->EnsureUploaded(*factory);

		TextureDescription skyboxDesc;
		ImageDimensions dims;
		res = m_skyboxResolution;
		dims.height = m_skyboxResolution;
		dims.width = m_skyboxResolution;
		dims.rowPitch = m_skyboxResolution * 4;
		dims.slicePitch = m_skyboxResolution * m_skyboxResolution * 4;
		for (int i = 0; i < 6; i++) {
			skyboxDesc.imageDimensions.push_back(dims);
		}
		skyboxDesc.channels = 4;
		skyboxDesc.isCubemap = true;
		skyboxDesc.format = rhi::Format::R16G16B16A16_Float;
		skyboxDesc.hasUAV = true;

		auto envCubemap = PixelBuffer::CreateShared(skyboxDesc);
		org::memory::SetResourceUsageHint(*envCubemap, "Environment lighting");
		auto sampler = Sampler::GetDefaultSampler();
		skybox = TextureAsset::CreateShared(skyboxDesc, envCubemap, sampler, TextureFileMeta());
		skybox->SetName("Environment cubemap");

		e->SetHDRI(skyHDR);
		e->SetEnvironmentCubemap(skybox);
		e->SetReflectionCubemapResolution(m_skyboxResolution); // For HDRI environments, use the same resolution as the skybox

		m_environmentsToConvert.push_back(e);
		m_workingHDRIGroup->AddResource(skyHDR->ImagePtr());
		auto path = GetCacheFilePath(name+L"_environment.dds", L"environments");
		if (m_requestReadback) {
			m_requestReadback(envCubemap, path, nullptr, true);
		}
		else {
			spdlog::warn("EnvironmentManager: readback request callback is not configured.");
		}

		path = GetCacheFilePath(name + L"_prefiltered.dds", L"environments");
		if (m_requestReadback) {
			m_requestReadback(e->GetEnvironmentPrefilteredCubemap(), path, nullptr, true);
		}
		else {
			spdlog::warn("EnvironmentManager: readback request callback is not configured.");
		}
	}

	//Re-create environment cubemap at full res
	m_environmentPrefilteredCubemapGroup->RemoveResource(e->GetEnvironmentPrefilteredCubemap().get());
	ImageDimensions dims;
	dims.height = res;
	dims.width = res;
	dims.rowPitch = res * 4;
	dims.slicePitch = res * res * 4;

	TextureDescription prefilteredDesc;
	for (int i = 0; i < 6; i++) {
		prefilteredDesc.imageDimensions.push_back(dims);
	}
	prefilteredDesc.channels = 3;
	prefilteredDesc.isCubemap = true;
	prefilteredDesc.format = rhi::Format::R8G8B8A8_UNorm;
	prefilteredDesc.generateMipMaps = true;
	prefilteredDesc.hasUAV = true;

	auto prefilteredEnvironmentCubemap = PixelBuffer::CreateShared(prefilteredDesc);
	org::memory::SetResourceUsageHint(*prefilteredEnvironmentCubemap, "Environment lighting");
	auto sampler = Sampler::GetDefaultSampler();
	auto prefilteredEnvironment = TextureAsset::CreateShared(prefilteredDesc, prefilteredEnvironmentCubemap, sampler, TextureFileMeta());
	prefilteredEnvironment->SetName("Environment prefiltered cubemap");
	e->SetEnvironmentPrefilteredCubemap(prefilteredEnvironment);
	m_environmentPrefilteredCubemapGroup->AddResource(prefilteredEnvironment->ImagePtr());


	m_environmentsToComputeSH.push_back(e);
	m_environmentsToPrefilter.push_back(e);
	m_workingEnvironmentCubemapGroup->AddResource(skybox->ImagePtr());
}

void EnvironmentManager::RemoveEnvironment(Environment* e) {
	std::lock_guard<std::mutex> lock(m_environmentUpdateMutex);
	m_environmentInfoBuffer->Remove(e->GetEnvironmentBufferView());
	m_environmentPrefilteredCubemapGroup->RemoveResource(e->GetEnvironmentPrefilteredCubemap().get());
	m_workingEnvironmentCubemapGroup->RemoveResource(e->GetEnvironmentCubemap()->ImagePtr().get());
	m_environmentsToConvert.erase(std::remove(m_environmentsToConvert.begin(), m_environmentsToConvert.end(), e), m_environmentsToConvert.end());
	m_environmentsToPrefilter.erase(std::remove(m_environmentsToPrefilter.begin(), m_environmentsToPrefilter.end(), e), m_environmentsToPrefilter.end());
}

std::shared_ptr<Resource> EnvironmentManager::ProvideResource(ResourceIdentifier const& key) {
	return m_resources[key];
}

std::vector<ResourceIdentifier> EnvironmentManager::GetSupportedKeys() {
	std::vector<ResourceIdentifier> keys;
	keys.reserve(m_resources.size());
	for (auto const& [key, _] : m_resources)
		keys.push_back(key);

	return keys;
}

std::vector<ResourceIdentifier> EnvironmentManager::GetSupportedResolverKeys() {
	std::vector<ResourceIdentifier> keys;
	keys.reserve(m_resolvers.size());
	for (auto const& [k, _] : m_resolvers)
		keys.push_back(k);
	return keys;
}
std::shared_ptr<IResourceResolver> EnvironmentManager::ProvideResolver(ResourceIdentifier const& key) {
	auto it = m_resolvers.find(key);
	if (it == m_resolvers.end()) return nullptr;
	return it->second;
}
