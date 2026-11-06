#include "Lighting/Environment/EnvironmentManager.h"

#include <chrono>
#include <filesystem>
#include <DirectXTex.h>
#include <BasicTelemetry/Tracy.h>
#include <spdlog/spdlog.h>
#include <rhi.h>

#include "Runtime/Resources/ResourceManager.h"
#include "BasicRenderer/Scene/Environment.h"
#include "BasicRenderer/Extensions/Buffers/LazyDynamicStructuredBuffer.h"
#include "Runtime/Settings/SettingsManager.h"
#include "Resources/PixelBuffer.h"
#include "Resources/Sampler.h"
#include "Resources/ResourceGroup.h"
#include "BasicRenderer/Assets/Texture.h"
#include "Assets/Textures/TextureFactory.h"
#include <BasicRenderer/Assets/TexturePipeline.h>
#include <BasicRenderer/Assets/CachePaths.h>
#include "../../../generated/BuiltinResources.h"
#include "Runtime/GraphIntegration/Resolvers/ResourceGroupResolver.h"
#include "Render/MemoryIntrospectionAPI.h"

EnvironmentManager::EnvironmentManager(std::shared_ptr<org::runtime::IUploadService> uploadService)
	: m_uploadService(std::move(uploadService)) {
	auto& resourceManager = ::ResourceManager::GetInstance();
	m_skyboxResolution = SettingsManager::GetInstance().getSettingGetter<uint16_t>("skyboxResolution")();
	m_reflectionCubemapResolution = SettingsManager::GetInstance().getSettingGetter<uint16_t>("reflectionCubemapResolution")();
	m_environmentInfoBuffer = org::LazyDynamicStructuredBuffer<EnvironmentInfo>::CreateShared(1, "environmentsBuffer", 0, true);
	org::memory::SetResourceUsageHint(*m_environmentInfoBuffer, "Environment Info");

	m_workingEnvironmentCubemapGroup = std::make_shared<org::ResourceGroup>("EnvironmentCubemapGroup");
	m_workingHDRIGroup = std::make_shared<org::ResourceGroup>("WorkingHDRIGroup");
	m_environmentPrefilteredCubemapGroup = std::make_shared<org::ResourceGroup>("EnvironmentPrefilteredCubemapGroup");

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

	org::ImageDimensions dims;
	dims.height = m_reflectionCubemapResolution;
	dims.width = m_reflectionCubemapResolution;
	dims.rowPitch = m_reflectionCubemapResolution * 4;
	dims.slicePitch = m_reflectionCubemapResolution * m_reflectionCubemapResolution * 4;

	org::TextureDescription prefilteredDesc;
	for (int i = 0; i < 6; i++) {
		prefilteredDesc.imageDimensions.push_back(dims);
	}
	prefilteredDesc.channels = 3;
	prefilteredDesc.isCubemap = true;
	//prefilteredDesc.hasRTV = true;
	prefilteredDesc.format = rhi::Format::R8G8B8A8_UNorm;
	prefilteredDesc.generateMipMaps = true;
	prefilteredDesc.hasUAV = true;

	auto prefilteredEnvironmentCubemap = org::PixelBuffer::CreateShared(prefilteredDesc);
	org::memory::SetResourceUsageHint(*prefilteredEnvironmentCubemap, "Environment lighting");
	auto sampler = org::Sampler::GetDefaultSampler();
	auto prefilteredEnvironment = TextureAsset::CreateFromImage(prefilteredEnvironmentCubemap, sampler, "Environment prefiltered cubemap");

	env->SetEnvironmentPrefilteredCubemap(prefilteredEnvironment);
	env->SetReflectionCubemapResolution(m_reflectionCubemapResolution);

	m_environmentPrefilteredCubemapGroup->AddResource(prefilteredEnvironment->ImagePtr());

	return std::move(env);
}

namespace {

// The skybox as BC6H: 32 MiB at 2048 a face instead of 192 MiB of RGBA16F, for the sky raster and path tracing read
// (RGB only; alpha is unused). The GPU encodes it once, and the next launch loads the result. Written under a temporary
// name, so a cache is either complete or absent.
void WriteCompressedSkybox(const std::shared_ptr<TextureSourceData>& data, const std::filesystem::path& destination) {
	if (!data || data->subresources.empty()) {
		spdlog::warn("EnvironmentManager: BC6H compression of the skybox failed");
		return;
	}
	const auto& desc = data->desc;
	DirectX::TexMetadata metadata{};
	metadata.width = desc.imageDimensions[0].width;
	metadata.height = desc.imageDimensions[0].height;
	metadata.depth = 1;
	metadata.arraySize = (std::max)(1u, desc.arraySize) * (desc.isCubemap ? 6u : 1u);
	metadata.mipLevels = data->subresources.size() / metadata.arraySize;
	metadata.miscFlags = desc.isCubemap ? DirectX::TEX_MISC_TEXTURECUBE : 0u;
	metadata.format = DXGI_FORMAT_BC6H_UF16;
	metadata.dimension = DirectX::TEX_DIMENSION_TEXTURE2D;
	DirectX::ScratchImage image;
	if (FAILED(image.Initialize(metadata))) return;
	for (size_t slice = 0; slice < metadata.arraySize; ++slice) {
		for (size_t mip = 0; mip < metadata.mipLevels; ++mip) {
			const DirectX::Image* target = image.GetImage(mip, slice, 0);
			const auto& bytes = data->subresources[slice * metadata.mipLevels + mip];
			if (!target || !bytes || bytes->size() != target->slicePitch) {
				spdlog::warn("EnvironmentManager: BC6H skybox subresource {} has the wrong size", slice * metadata.mipLevels + mip);
				return;
			}
			std::memcpy(target->pixels, bytes->data(), bytes->size());
		}
	}
	auto temporary = destination;
	temporary += L".tmp";
	std::error_code error;
	if (FAILED(DirectX::SaveToDDSFile(image.GetImages(), image.GetImageCount(), metadata, DirectX::DDS_FLAGS_NONE, temporary.c_str()))) {
		spdlog::warn("EnvironmentManager: could not write '{}'", temporary.string());
		std::filesystem::remove(temporary, error);
		return;
	}
	std::filesystem::rename(temporary, destination, error);
	if (error) {
		spdlog::warn("EnvironmentManager: could not move '{}' into place: {}", temporary.string(), error.message());
		std::filesystem::remove(temporary, error);
		return;
	}
	spdlog::info("EnvironmentManager: wrote the BC6H skybox cache '{}'", destination.filename().string());
}

void SubmitSkyboxCompression(const EnvironmentManager::Bc6hCompressorFn& compressor,
	const std::filesystem::path& source, const std::filesystem::path& destination) {
	const auto started = std::chrono::steady_clock::now();
	auto data = LoadTextureSourceDataFromDDSFilePath(source.string(), false, "skybox BC6H compression");
	if (!data) {
		spdlog::warn("EnvironmentManager: could not read '{}' to compress it", source.string());
		return;
	}
	const bool submitted = compressor(std::move(data), "Skybox", [destination, started](std::shared_ptr<TextureSourceData> blocks) {
		WriteCompressedSkybox(blocks, destination);
		spdlog::info("EnvironmentManager: skybox BC6H compression took {:.2f} s",
			std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
	});
	if (!submitted) spdlog::info("EnvironmentManager: GPU BC6H compression unavailable; the skybox stays RGBA16F");
}

} // namespace

EnvironmentManager::~EnvironmentManager() {
	if (m_skyboxCompression.valid()) m_skyboxCompression.wait();
	std::lock_guard<std::mutex> lock(*m_environmentUpdateMutex);
	m_pendingSourceLoads->alive = false;
}

void EnvironmentManager::SetFromHDRI(Environment* e, std::string hdriPath) {
	// A processed environment is cached as its cubemap; otherwise its HDRI is converted.
	const auto skyboxPath = GetCacheFilePath(e->GetName() + L"_environment.dds", L"environments");
	const auto compressedSkyboxPath = GetCacheFilePath(e->GetName() + L"_environment_bc6h.dds", L"environments");
	const bool compressedCubemap = std::filesystem::exists(compressedSkyboxPath);
	const bool cachedCubemap = compressedCubemap || std::filesystem::exists(skyboxPath);
	br::textures::TextureRequest request;
	request.preparation = br::textures::TexturePreparation::LoadOnly;
	if (compressedCubemap) {
		request.source = br::textures::MakeFileTextureSource(compressedSkyboxPath);
		request.debugName = "Skybox cubemap";
	}
	else if (cachedCubemap) {
		request.source = br::textures::MakeFileTextureSource(skyboxPath);
		request.allowRTV = true;
		request.allowUAV = true;
		request.debugName = "Skybox cubemap";
		if (m_bc6hCompressor && !m_skyboxCompression.valid()) {
			m_skyboxCompression = std::async(std::launch::async, [compressor = m_bc6hCompressor,
				source = std::filesystem::path(skyboxPath), destination = std::filesystem::path(compressedSkyboxPath)] {
				SubmitSkyboxCompression(compressor, source, destination);
			});
		}
	}
	else {
		request.source = br::textures::MakeFileTextureSource(hdriPath);
		request.debugName = "Environment HDRI";
	}
	auto source = br::textures::RequestTexture(std::move(request));
	std::uint64_t generation = 0;
	{
		std::lock_guard<std::mutex> lock(*m_environmentUpdateMutex);
		generation = ++m_pendingSourceLoads->nextGeneration;
		m_pendingSourceLoads->generations[e] = generation;
	}
	// The environment's own processing starts once its source is resident.
	br::textures::LoadTexture(source, [this, e, generation, cachedCubemap, source,
		pending = m_pendingSourceLoads, mutex = m_environmentUpdateMutex](bool loaded) {
		std::lock_guard<std::mutex> lock(*mutex);
		const auto it = pending->generations.find(e);
		if (!pending->alive || it == pending->generations.end() || it->second != generation) return;
		pending->generations.erase(it);
		if (!loaded) {
			spdlog::error("EnvironmentManager: environment source '{}' failed to load", source->Meta().filePath);
			return;
		}
		ApplyEnvironmentSource(e, source, cachedCubemap);
	});
}

void EnvironmentManager::ApplyEnvironmentSource(Environment* e, const std::shared_ptr<TextureAsset>& source, bool cachedCubemap) {
	auto& name = e->GetName();
	std::shared_ptr<TextureAsset> skybox;
	unsigned int res = m_reflectionCubemapResolution;
	// An HDRI environment is converted to a cubemap first.
	std::optional<br::render::EnvironmentConversionWork> conversion;
	std::function<void()> cacheConvertedCubemap;
	if (cachedCubemap) {
		skybox = source;
		skybox->SetName("Skybox cubemap");
		org::memory::SetResourceUsageHint(*skybox->ImagePtr(), "Environment lighting");
		res = skybox->GetWidth();
		e->SetReflectionCubemapResolution(res);
		e->SetEnvironmentCubemap(skybox);
	}
	else {
		const auto& skyHDR = source;

		org::TextureDescription skyboxDesc;
		org::ImageDimensions dims;
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

		auto envCubemap = org::PixelBuffer::CreateShared(skyboxDesc);
		org::memory::SetResourceUsageHint(*envCubemap, "Environment lighting");
		auto sampler = org::Sampler::GetDefaultSampler();
		skybox = TextureAsset::CreateFromImage(envCubemap, sampler, "Environment cubemap");

		e->SetHDRI(skyHDR);
		e->SetEnvironmentCubemap(skybox);
		e->SetReflectionCubemapResolution(m_skyboxResolution); // For HDRI environments, use the same resolution as the skybox

		conversion = br::render::EnvironmentConversionWork{skyHDR->ImagePtr(), envCubemap, e->GetEnvironmentIndex(),
			m_workingHDRIGroup, m_environmentUpdateMutex};
		m_workingHDRIGroup->AddResource(skyHDR->ImagePtr());
		cacheConvertedCubemap = [readback = m_requestReadback, envCubemap,
			path = GetCacheFilePath(name + L"_environment.dds", L"environments")] {
			if (readback) readback(envCubemap, path, nullptr, true);
			else spdlog::warn("EnvironmentManager: readback request callback is not configured.");
		};
	}

	//Re-create environment cubemap at full res
	m_environmentPrefilteredCubemapGroup->RemoveResource(e->GetEnvironmentPrefilteredCubemap().get());
	org::ImageDimensions dims;
	dims.height = res;
	dims.width = res;
	dims.rowPitch = res * 4;
	dims.slicePitch = res * res * 4;

	org::TextureDescription prefilteredDesc;
	for (int i = 0; i < 6; i++) {
		prefilteredDesc.imageDimensions.push_back(dims);
	}
	prefilteredDesc.channels = 3;
	prefilteredDesc.isCubemap = true;
	prefilteredDesc.format = rhi::Format::R8G8B8A8_UNorm;
	prefilteredDesc.generateMipMaps = true;
	prefilteredDesc.hasUAV = true;

	auto prefilteredEnvironmentCubemap = org::PixelBuffer::CreateShared(prefilteredDesc);
	org::memory::SetResourceUsageHint(*prefilteredEnvironmentCubemap, "Environment lighting");
	auto sampler = org::Sampler::GetDefaultSampler();
	auto prefilteredEnvironment = TextureAsset::CreateFromImage(prefilteredEnvironmentCubemap, sampler, "Environment prefiltered cubemap");
	e->SetEnvironmentPrefilteredCubemap(prefilteredEnvironment);
	m_environmentPrefilteredCubemapGroup->AddResource(prefilteredEnvironment->ImagePtr());


	m_workingEnvironmentCubemapGroup->AddResource(skybox->ImagePtr());
	// SH and prefiltering read the environment's cubemap: queued now for a cached one, after its conversion otherwise.
	auto& alive = m_environmentAlive[e];
	if (!alive) alive = std::make_shared<bool>(true);
	auto filter = [work = m_workServices, alive,
		sh = br::render::EnvironmentSHWork{skybox->ImagePtr(), e->GetEnvironmentIndex(), e->GetReflectionCubemapResolution()},
		prefilter = br::render::EnvironmentPrefilterWork{skybox->ImagePtr(), prefilteredEnvironmentCubemap,
			e->GetReflectionCubemapResolution(), e->GetEnvironmentIndex(), m_workingEnvironmentCubemapGroup, m_environmentUpdateMutex}]() mutable {
		if (!*alive) return;
		work.sphericalHarmonics.Enqueue(sh);
		work.prefilter.Enqueue(prefilter);
	};
	if (conversion) {
		// The cache copy must follow the conversion: a readback queued sooner saves an unwritten cubemap, and later
		// launches prefer that file over the HDRI.
		conversion->then = [filter = std::move(filter), cacheConvertedCubemap = std::move(cacheConvertedCubemap)]() mutable {
			cacheConvertedCubemap();
			filter();
		};
		m_workParkedForUpload.push_back([work = m_workServices, job = std::move(*conversion), alive]() mutable {
			if (*alive) work.conversion.Enqueue(std::move(job));
			else job.Discard();
		});
	}
	else m_workParkedForUpload.push_back(std::move(filter));
}

void EnvironmentManager::ReleaseUploadedWork() {
	std::vector<std::function<void()>> ready;
	{
		std::lock_guard lock(*m_environmentUpdateMutex);
		ready.swap(m_workAwaitingFrameUpload);
		m_workAwaitingFrameUpload.swap(m_workParkedForUpload);
	}
	for (auto& work : ready) work();
}

void EnvironmentManager::RemoveEnvironment(Environment* e) {
	{
		// A conversion submitted from here on queues nothing for it (EnvironmentConversionWork::then).
		std::lock_guard<std::mutex> lock(*m_environmentUpdateMutex);
		if (const auto found = m_environmentAlive.find(e); found != m_environmentAlive.end()) {
			*found->second = false;
			m_environmentAlive.erase(found);
		}
	}
    m_workServices.conversion.DiscardUnsubmitted([index = e->GetEnvironmentIndex()](const br::render::EnvironmentConversionWork& work) { return work.environmentIndex == index; });
    m_workServices.prefilter.DiscardUnsubmitted([index = e->GetEnvironmentIndex()](const br::render::EnvironmentPrefilterWork& work) { return work.environmentIndex == index; });
    m_workServices.sphericalHarmonics.DiscardUnsubmitted([index = e->GetEnvironmentIndex()](const br::render::EnvironmentSHWork& work) { return work.environmentIndex == index; });
	std::lock_guard<std::mutex> lock(*m_environmentUpdateMutex);
	m_pendingSourceLoads->generations.erase(e);
	m_environmentInfoBuffer->Remove(e->GetEnvironmentBufferView());
	m_environmentPrefilteredCubemapGroup->RemoveResource(e->GetEnvironmentPrefilteredCubemap().get());
	m_workingEnvironmentCubemapGroup->RemoveResource(e->GetEnvironmentCubemap()->ImagePtr().get());
}

std::shared_ptr<org::Resource> EnvironmentManager::ProvideResource(org::ResourceIdentifier const& key) {
	return m_resources[key];
}

std::vector<org::ResourceIdentifier> EnvironmentManager::GetSupportedKeys() {
	std::vector<org::ResourceIdentifier> keys;
	keys.reserve(m_resources.size());
	for (auto const& [key, _] : m_resources)
		keys.push_back(key);

	return keys;
}

std::vector<org::ResourceIdentifier> EnvironmentManager::GetSupportedResolverKeys() {
	std::vector<org::ResourceIdentifier> keys;
	keys.reserve(m_resolvers.size());
	for (auto const& [k, _] : m_resolvers)
		keys.push_back(k);
	return keys;
}
std::shared_ptr<org::IResourceResolver> EnvironmentManager::ProvideResolver(org::ResourceIdentifier const& key) {
	auto it = m_resolvers.find(key);
	if (it == m_resolvers.end()) return nullptr;
	return it->second;
}

void br::render::EnvironmentWorkServices::PublishTelemetry() const {
    const auto conversion = this->conversion.ReadCounters();
    BT_PLOT("Environment.Conversion.AcceptedJobs", static_cast<int64_t>(conversion.accepted));
    BT_PLOT("Environment.Conversion.PendingJobs", static_cast<int64_t>(conversion.pending));
    BT_PLOT("Environment.Conversion.ReservedJobs", static_cast<int64_t>(conversion.reserved));
    BT_PLOT("Environment.Conversion.SubmittedJobs", static_cast<int64_t>(conversion.submitted));
    BT_PLOT("Environment.Conversion.ReturnedJobs", static_cast<int64_t>(conversion.returned));
    BT_PLOT("Environment.Conversion.DiscardedJobs", static_cast<int64_t>(conversion.discarded));
    const auto prefilter = this->prefilter.ReadCounters();
    BT_PLOT("Environment.Prefilter.AcceptedJobs", static_cast<int64_t>(prefilter.accepted));
    BT_PLOT("Environment.Prefilter.PendingJobs", static_cast<int64_t>(prefilter.pending));
    BT_PLOT("Environment.Prefilter.ReservedJobs", static_cast<int64_t>(prefilter.reserved));
    BT_PLOT("Environment.Prefilter.SubmittedJobs", static_cast<int64_t>(prefilter.submitted));
    BT_PLOT("Environment.Prefilter.ReturnedJobs", static_cast<int64_t>(prefilter.returned));
    BT_PLOT("Environment.Prefilter.DiscardedJobs", static_cast<int64_t>(prefilter.discarded));
    const auto sh = sphericalHarmonics.ReadCounters();
    BT_PLOT("Environment.SH.AcceptedJobs", static_cast<int64_t>(sh.accepted));
    BT_PLOT("Environment.SH.PendingJobs", static_cast<int64_t>(sh.pending));
    BT_PLOT("Environment.SH.ReservedJobs", static_cast<int64_t>(sh.reserved));
    BT_PLOT("Environment.SH.SubmittedJobs", static_cast<int64_t>(sh.submitted));
    BT_PLOT("Environment.SH.ReturnedJobs", static_cast<int64_t>(sh.returned));
    BT_PLOT("Environment.SH.DiscardedJobs", static_cast<int64_t>(sh.discarded));
}
void br::render::EnvironmentConversionWork::Commit() const {
    std::lock_guard lock(*publicationMutex);
    sourceGroup->RemoveResource(srcTexture.get());
    if (then) then();
}

void br::render::EnvironmentConversionWork::Discard() const {
    std::lock_guard lock(*publicationMutex);
    sourceGroup->RemoveResource(srcTexture.get());
}

void br::render::EnvironmentPrefilterWork::Commit() const {
    std::lock_guard lock(*publicationMutex);
    sourceGroup->RemoveResource(srcCubemap.get());
}
