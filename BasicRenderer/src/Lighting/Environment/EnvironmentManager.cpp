#include "Lighting/Environment/EnvironmentManager.h"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <DirectXTex.h>
#include <DirectXPackedVector.h>
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
// (RGB only; alpha is unused). The GPU encodes it once, and the next launch loads the result. The prefiltered cubemap
// is cached the same way (PrefilteredCubemapCachePath). Written under a temporary name, so a cache is either complete or
// absent.
bool WriteCompressedCubemap(const std::shared_ptr<TextureSourceData>& data, const std::filesystem::path& destination,
	std::string_view label) {
	if (!data || data->subresources.empty()) {
		spdlog::warn("EnvironmentManager: BC6H compression of the {} failed", label);
		return false;
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
	if (FAILED(image.Initialize(metadata))) return false;
	for (size_t slice = 0; slice < metadata.arraySize; ++slice) {
		for (size_t mip = 0; mip < metadata.mipLevels; ++mip) {
			const DirectX::Image* target = image.GetImage(mip, slice, 0);
			const auto& bytes = data->subresources[slice * metadata.mipLevels + mip];
			if (!target || !bytes || bytes->size() != target->slicePitch) {
				spdlog::warn("EnvironmentManager: BC6H {} subresource {} has the wrong size", label, slice * metadata.mipLevels + mip);
				return false;
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
		return false;
	}
	std::filesystem::rename(temporary, destination, error);
	if (error) {
		spdlog::warn("EnvironmentManager: could not move '{}' into place: {}", temporary.string(), error.message());
		std::filesystem::remove(temporary, error);
		return false;
	}
	spdlog::info("EnvironmentManager: wrote the BC6H {} cache '{}'", label, destination.filename().string());
	return true;
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
		WriteCompressedCubemap(blocks, destination, "skybox");
		spdlog::info("EnvironmentManager: skybox BC6H compression took {:.2f} s",
			std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
	});
	if (!submitted) spdlog::info("EnvironmentManager: GPU BC6H compression unavailable; the skybox stays RGBA16F");
}

// The BC6H encoder reads float RGB. The prefilter writes RGBA8 UNorm, so the values are already in [0, 1] and convert
// exactly.
std::shared_ptr<TextureSourceData> ToHalfFloat(const std::shared_ptr<TextureSourceData>& data) {
	if (!data || data->isBlockCompressed || data->desc.format != rhi::Format::R8G8B8A8_UNorm ||
		data->desc.imageDimensions.size() != data->subresources.size()) return nullptr;
	auto converted = std::make_shared<TextureSourceData>(*data);
	converted->desc.format = rhi::Format::R16G16B16A16_Float;
	converted->desc.srvFormat = rhi::Format::R16G16B16A16_Float;
	converted->desc.uavFormat = rhi::Format::Unknown;
	converted->desc.hasUAV = false;
	converted->desc.generateMipMaps = false;
	std::array<uint16_t, 256> halfOf{};
	for (uint32_t value = 0; value < 256; ++value)
		halfOf[value] = DirectX::PackedVector::XMConvertFloatToHalf(static_cast<float>(value) / 255.0f);
	for (size_t index = 0; index < data->subresources.size(); ++index) {
		const auto& source = data->subresources[index];
		auto& dims = converted->desc.imageDimensions[index];
		const uint64_t sourcePitch = dims.rowPitch;
		if (!source || sourcePitch < uint64_t{ dims.width } * 4u || source->size() < sourcePitch * dims.height) return nullptr;
		dims.rowPitch = uint64_t{ dims.width } * 8u;
		dims.slicePitch = dims.rowPitch * dims.height;
		auto bytes = std::make_shared<std::vector<uint8_t>>(dims.slicePitch);
		auto* out = reinterpret_cast<uint16_t*>(bytes->data());
		for (uint32_t y = 0; y < dims.height; ++y) {
			const uint8_t* row = source->data() + y * sourcePitch;
			for (uint32_t x = 0; x < dims.width * 4u; ++x) *out++ = halfOf[row[x]];
		}
		converted->subresources[index] = std::move(bytes);
	}
	return converted;
}

// Reads back the freshly prefiltered cubemap (RGBA8, every mip), encodes it as BC6H and writes destination. then runs
// once the cache is written; nothing runs on failure, and the RGBA8 cubemap stays in use for the session.
void CachePrefilteredCubemap(const EnvironmentManager::RequestReadbackFn& readback, const EnvironmentManager::Bc6hCompressorFn& compressor,
	std::shared_ptr<org::PixelBuffer> cubemap, std::filesystem::path destination, std::function<void()> then) {
	if (!readback || !compressor) return;
	auto intermediate = destination;
	intermediate.replace_extension(L".rgba8.dds");
	readback(std::move(cubemap), intermediate.wstring(), [compressor, intermediate, destination, then = std::move(then)]() mutable {
		const auto started = std::chrono::steady_clock::now();
		auto data = ToHalfFloat(LoadTextureSourceDataFromDDSFilePath(intermediate.string(), false, "prefiltered cubemap BC6H compression"));
		std::error_code error;
		std::filesystem::remove(intermediate, error);
		if (!data) {
			spdlog::warn("EnvironmentManager: could not read the prefiltered cubemap '{}' back to compress it", intermediate.string());
			return;
		}
		const bool submitted = compressor(std::move(data), "Environment prefiltered cubemap",
			[destination, started, then = std::move(then)](std::shared_ptr<TextureSourceData> blocks) {
				if (!WriteCompressedCubemap(blocks, destination, "prefiltered cubemap")) return;
				spdlog::info("EnvironmentManager: prefiltered cubemap BC6H compression took {:.2f} s",
					std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
				if (then) then();
			});
		if (!submitted) spdlog::info("EnvironmentManager: GPU BC6H compression unavailable; the prefiltered cubemap stays RGBA8");
	}, true);
}

// Per environment and resolution; the skybox cache it was filtered from is per environment too.
std::filesystem::path PrefilteredCubemapCachePath(const std::wstring& name, unsigned int resolution) {
	return GetCacheFilePath(name + L"_prefiltered_" + std::to_wstring(resolution) + L"_bc6h.dds", L"environments");
}

} // namespace

// Under *m_environmentUpdateMutex: the BC6H cache just written for a cubemap prefiltered this session. The load is started
// from ReleaseUploadedWork, outside the lock; once it is resident, the next ReleaseUploadedWork puts it in place of the
// environment's prefiltered cubemap. (A later launch loads the cache with the cubemap instead, SetFromHDRI.) The frame
// graph registered the RGBA8 cubemap it replaces when it was built: m_prefilteredCubemapReplaced has the renderer rebuild
// it, once, so it lets go.
void EnvironmentManager::LoadCompressedPrefilteredCubemap(Environment* e, const std::filesystem::path& path) {
	const auto found = m_environmentAlive.find(e);
	if (found == m_environmentAlive.end()) return;
	m_workParkedForUpload.push_back([this, e, path, alive = found->second, pending = m_pendingSourceLoads,
		mutex = m_environmentUpdateMutex] {
		br::textures::TextureRequest request;
		request.preparation = br::textures::TexturePreparation::LoadOnly;
		request.source = br::textures::MakeFileTextureSource(path);
		request.debugName = "Environment prefiltered cubemap";
		auto texture = br::textures::RequestTexture(std::move(request));
		br::textures::LoadTexture(texture, [this, e, path, texture, alive, pending, mutex](bool loaded) {
			std::lock_guard<std::mutex> lock(*mutex);
			if (!pending->alive || !*alive) return;
			if (!loaded) {
				spdlog::warn("EnvironmentManager: the BC6H prefiltered cubemap '{}' failed to load", path.string());
				return;
			}
			m_workParkedForUpload.push_back([this, e, texture, alive, mutex] {
				std::lock_guard<std::mutex> lock(*mutex);
				if (!*alive) return;
				texture->SetName("Environment prefiltered cubemap");
				org::memory::SetResourceUsageHint(*texture->ImagePtr(), "Environment lighting");
				m_environmentPrefilteredCubemapGroup->RemoveResource(e->GetEnvironmentPrefilteredCubemap().get());
				e->SetEnvironmentPrefilteredCubemap(texture);
				m_environmentPrefilteredCubemapGroup->AddResource(texture->ImagePtr());
				if (m_prefilteredCubemapReplaced) m_prefilteredCubemapReplaced(e);
			});
		});
	});
}

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
	// A cached cubemap's prefiltered cubemap may be cached too (as BC6H, PrefilteredCubemapCachePath): it loads beside the
	// cubemap, so the environment is complete when it applies and nothing prefilters it.
	std::shared_ptr<TextureAsset> prefiltered;
	if (cachedCubemap) {
		DirectX::TexMetadata metadata{};
		const std::filesystem::path cubemapPath = compressedCubemap ? compressedSkyboxPath : skyboxPath;
		if (SUCCEEDED(DirectX::GetMetadataFromDDSFile(cubemapPath.c_str(), DirectX::DDS_FLAGS_NONE, metadata))) {
			const auto prefilteredPath = PrefilteredCubemapCachePath(e->GetName(), static_cast<unsigned int>(metadata.width));
			std::error_code error;
			if (std::filesystem::exists(prefilteredPath, error)) {
				br::textures::TextureRequest prefilteredRequest;
				prefilteredRequest.preparation = br::textures::TexturePreparation::LoadOnly;
				prefilteredRequest.source = br::textures::MakeFileTextureSource(prefilteredPath);
				prefilteredRequest.debugName = "Environment prefiltered cubemap";
				prefiltered = br::textures::RequestTexture(std::move(prefilteredRequest));
			}
		}
	}
	std::uint64_t generation = 0;
	{
		std::lock_guard<std::mutex> lock(*m_environmentUpdateMutex);
		generation = ++m_pendingSourceLoads->nextGeneration;
		m_pendingSourceLoads->generations[e] = generation;
	}
	// The environment's own processing starts once its sources are resident.
	struct SourceLoads {
		std::atomic<int> remaining = 0;
		std::atomic_bool sourceLoaded = false, prefilteredLoaded = false;
	};
	auto loads = std::make_shared<SourceLoads>();
	loads->remaining = prefiltered ? 2 : 1;
	auto finish = [this, e, generation, cachedCubemap, source, prefiltered, loads,
		pending = m_pendingSourceLoads, mutex = m_environmentUpdateMutex] {
		std::lock_guard<std::mutex> lock(*mutex);
		const auto it = pending->generations.find(e);
		if (!pending->alive || it == pending->generations.end() || it->second != generation) return;
		pending->generations.erase(it);
		if (!loads->sourceLoaded) {
			spdlog::error("EnvironmentManager: environment source '{}' failed to load", source->Meta().filePath);
			return;
		}
		if (prefiltered && !loads->prefilteredLoaded)
			spdlog::warn("EnvironmentManager: the BC6H prefiltered cubemap '{}' failed to load; prefiltering again", prefiltered->Meta().filePath);
		ApplyEnvironmentSource(e, source, cachedCubemap, loads->prefilteredLoaded ? prefiltered : nullptr);
	};
	br::textures::LoadTexture(source, [loads, finish](bool loaded) {
		loads->sourceLoaded = loaded;
		if (--loads->remaining == 0) finish();
	});
	if (prefiltered) {
		br::textures::LoadTexture(prefiltered, [loads, finish](bool loaded) {
			loads->prefilteredLoaded = loaded;
			if (--loads->remaining == 0) finish();
		});
	}
}

void EnvironmentManager::ApplyEnvironmentSource(Environment* e, const std::shared_ptr<TextureAsset>& source, bool cachedCubemap,
	const std::shared_ptr<TextureAsset>& cachedPrefiltered) {
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

	// The prefiltered cubemap is cached as BC6H (128 MiB of RGBA8 at 2048 a face; 32 MiB as BC6H). A converted HDRI is a
	// new skybox, so what was filtered from the old one is stale.
	const auto prefilteredCachePath = PrefilteredCubemapCachePath(name, res);
	std::error_code cacheError;
	if (!cachedCubemap) std::filesystem::remove(prefilteredCachePath, cacheError);

	auto& alive = m_environmentAlive[e];
	if (!alive) alive = std::make_shared<bool>(true);
	std::optional<br::render::EnvironmentPrefilterWork> prefilter;
	if (cachedPrefiltered) {
		m_environmentPrefilteredCubemapGroup->RemoveResource(e->GetEnvironmentPrefilteredCubemap().get());
		cachedPrefiltered->SetName("Environment prefiltered cubemap");
		org::memory::SetResourceUsageHint(*cachedPrefiltered->ImagePtr(), "Environment lighting");
		e->SetEnvironmentPrefilteredCubemap(cachedPrefiltered);
		m_environmentPrefilteredCubemapGroup->AddResource(cachedPrefiltered->ImagePtr());
	}
	else {
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
		prefilter = br::render::EnvironmentPrefilterWork{skybox->ImagePtr(), prefilteredEnvironmentCubemap,
			e->GetReflectionCubemapResolution(), e->GetEnvironmentIndex(), m_workingEnvironmentCubemapGroup, m_environmentUpdateMutex};
		// Once the GPU has filtered it: read it back, encode it, and swap the BC6H copy in (it also serves later launches).
		prefilter->then = [this, e, alive, pending = m_pendingSourceLoads, mutex = m_environmentUpdateMutex,
			cubemap = prefilteredEnvironmentCubemap, path = prefilteredCachePath] {
			if (!pending->alive || !*alive) return;
			CachePrefilteredCubemap(m_requestReadback, m_bc6hCompressor, cubemap, path, [this, e, alive, pending, mutex, path] {
				std::lock_guard<std::mutex> lock(*mutex);
				if (pending->alive && *alive) LoadCompressedPrefilteredCubemap(e, path);
			});
		};
	}

	// SH and prefiltering read the environment's cubemap: queued now for a cached one, after its conversion otherwise.
	auto filter = [work = m_workServices, alive,
		sh = br::render::EnvironmentSHWork{skybox->ImagePtr(), e->GetEnvironmentIndex(), e->GetReflectionCubemapResolution()},
		prefilter = std::move(prefilter)]() mutable {
		if (!*alive) return;
		work.sphericalHarmonics.Enqueue(sh);
		if (prefilter) work.prefilter.Enqueue(*prefilter);
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

void br::render::EnvironmentPrefilterWork::Complete() const {
    std::lock_guard lock(*publicationMutex);
    if (then) then();
}
