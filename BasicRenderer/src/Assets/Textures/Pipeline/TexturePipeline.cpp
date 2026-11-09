#include <BasicRenderer/Assets/TexturePipeline.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

#include <objbase.h>

#include <spdlog/spdlog.h>

#include "Assets/Textures/Pipeline/TexturePipelineStages.h"
#include "Assets/Textures/Processing/TextureProcessingManager.h"

namespace br::textures
{
	namespace
	{
		std::mutex g_requestedMutex;
		std::unordered_map<std::string, std::weak_ptr<TextureAsset>> g_requested;
		std::mutex g_loaderMutex;
		TextureLoader g_loader;

		std::vector<std::uint8_t> ReadFileRange(const FileTextureSource& file)
		{
			std::ifstream stream(file.path, std::ios::binary | std::ios::ate);
			if (!stream) throw std::runtime_error("cannot open texture source '" + file.path.string() + "'");
			const auto fileSize = static_cast<std::uint64_t>(stream.tellg());
			if (file.offset > fileSize) throw std::runtime_error("texture source range past the end of '" + file.path.string() + "'");
			const auto size = file.size == 0 ? fileSize - file.offset : file.size;
			if (file.offset + size > fileSize) throw std::runtime_error("texture source range past the end of '" + file.path.string() + "'");
			std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
			stream.seekg(static_cast<std::streamoff>(file.offset));
			stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
			if (!stream) throw std::runtime_error("cannot read texture source '" + file.path.string() + "'");
			return bytes;
		}

		// DirectXTex filters mips and converts formats through WIC, which needs COM on the calling thread. The renderer's
		// workers run in the process's implicit MTA; an offline tool's own threads may have no COM at all.
		// DirectXTex keeps one WIC factory for the process. Should every thread in the MTA leave it, COM tears the apartment
		// down and unloads the codecs that factory lives in, and the next mip generation calls through a dangling vtable: hold
		// the MTA for the process's lifetime.
		void HoldMultithreadedApartment()
		{
			static std::once_flag once;
			std::call_once(once, [] {
				CO_MTA_USAGE_COOKIE cookie{};
				(void)CoIncrementMTAUsage(&cookie);
			});
		}

		struct ScopedCom {
			HRESULT hr = [] {
				HoldMultithreadedApartment();
				return CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			}();
			~ScopedCom() { if (SUCCEEDED(hr)) CoUninitialize(); }
		};

		TextureFileMeta CacheMeta(const TextureRequest& request)
		{
			TextureFileMeta meta{};
			meta.filePath = request.source.identity;
			meta.preferSRGB = request.preferSRGB;
			meta.processing = request.processing;
			// A source the file system cannot version (a part of a file, bytes in memory)
			// carries its version in its identity.
			if (!request.source.version.empty()) {
				meta.processing.sourceIdentity = request.source.identity + "@" + request.source.version;
			}
			return meta;
		}
	}

	TextureSource MakeFileTextureSource(const std::filesystem::path& path)
	{
		TextureSource source;
		// The processed cache versions a file identity by its write time itself.
		source.identity = path.string();
		source.formatHint = path.extension().string();
		source.data = FileTextureSource{ .path = path };
		return source;
	}

	std::string TextureCacheKey(const TextureRequest& request)
	{
		if (request.preparation == TexturePreparation::LoadOnly) {
			// Not a processed-cache key (nothing is cached); it only identifies the load.
			return "load#" + request.source.identity + "@" + request.source.version +
				(request.preferSRGB ? "#srgb" : "#linear");
		}
		return TextureProcessingManager::GetInstance().CacheKey(CacheMeta(request));
	}

	void SetTextureLoader(TextureLoader loader)
	{
		std::lock_guard lock(g_loaderMutex);
		g_loader = std::move(loader);
	}

	void LoadTexture(const std::shared_ptr<TextureAsset>& texture, std::function<void(bool loaded)> ready)
	{
		if (!texture) {
			if (ready) ready(false);
			return;
		}
		TextureLoader loader;
		{
			std::lock_guard lock(g_loaderMutex);
			loader = g_loader;
		}
		if (!loader) throw std::runtime_error("LoadTexture: the texture pipeline is not running");
		loader(texture, std::move(ready));
	}

	bool WaitForTexture(const std::shared_ptr<TextureAsset>& texture, std::chrono::milliseconds timeout)
	{
		struct Wait {
			std::mutex mutex;
			std::condition_variable done;
			bool finished = false;
			bool loaded = false;
		};
		auto wait = std::make_shared<Wait>();
		LoadTexture(texture, [wait](bool loaded) {
			{
				std::lock_guard lock(wait->mutex);
				wait->finished = true;
				wait->loaded = loaded;
			}
			wait->done.notify_all();
		});
		std::unique_lock lock(wait->mutex);
		wait->done.wait_for(lock, timeout, [&] { return wait->finished; });
		return wait->finished && wait->loaded;
	}

	TextureFileMeta TextureProcessingMeta(const TextureRequest& request)
	{
		return CacheMeta(request);
	}

	std::shared_ptr<TextureAsset> RequestTexture(TextureRequest request)
	{
		auto key = TextureCacheKey(request);
		// Textures share their processed form by cache key, but each sampler is its own texture.
		const auto textureKey = key + "|sampler:" + std::to_string(reinterpret_cast<std::uintptr_t>(request.sampler.get())) +
			(request.mipStreaming ? "|stream" : "|resident") + (request.generateMipMaps ? "|gpumips" : "");
		std::lock_guard lock(g_requestedMutex);
		auto& slot = g_requested[textureKey];
		if (auto existing = slot.lock()) return existing;
		auto texture = TextureAsset::CreateForPipeline(
			std::make_shared<const TextureRequest>(std::move(request)), key);
		slot = texture;
		return texture;
	}

	TextureConditionResult ConditionTextureOnCpu(const std::shared_ptr<TextureAsset>& texture)
	{
		if (!texture || !texture->IsPipelineTexture()) throw std::runtime_error("ConditionTextureOnCpu: not a pipeline texture");
		const TextureRequest& request = *texture->PipelineRequest();
		if (request.preparation == TexturePreparation::LoadOnly) return TextureConditionResult::NotProcessed;
		// Streamed from its provider as it is: nothing to cache.
		if (ProvidedPassthroughShape(request)) return TextureConditionResult::NotProcessed;

		const std::string& cacheKey = texture->PipelineCacheKey();
		if (ReadConditionedCacheDescription(TextureProcessingManager::ConditionedCachePath(cacheKey))) {
			return TextureConditionResult::AlreadyCached;
		}
		const ScopedCom com;
		const TextureFileMeta meta = TextureProcessingMeta(request);
		using Clock = std::chrono::steady_clock;
		const auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
		const auto t0 = Clock::now();
		const auto source = DecodeTextureSource(request);
		const auto t1 = Clock::now();
		auto processed = TextureProcessingManager::GetInstance().Process(source, meta);
		const auto t2 = Clock::now();
		const auto result = processed.gpuCompression
			? TextureProcessingManager::FinalizeOnCpu(processed.prepared, meta)
			: processed.result;
		const auto t3 = Clock::now();
		if (!result || TextureProcessingManager::WriteCache(cacheKey, *result).empty()) {
			throw std::runtime_error("ConditionTextureOnCpu: could not write the cache for '" + request.source.identity + "'");
		}
		const auto t4 = Clock::now();
		static const bool timing = [] {
			char* value = nullptr;
			std::size_t length = 0;
			const bool on = _dupenv_s(&value, &length, "SARP_TEXTURE_TIMING") == 0 && value && value[0] == '1';
			std::free(value);
			return on;
		}();
		if (timing) {
			const auto dims = [](const std::shared_ptr<TextureSourceData>& a_data) {
				if (!a_data || a_data->desc.imageDimensions.empty()) return std::string("?");
				return std::to_string(a_data->desc.imageDimensions[0].width) + "x" + std::to_string(a_data->desc.imageDimensions[0].height) + " fmt " +
					std::to_string(static_cast<int>(a_data->desc.format)) + " subresources " + std::to_string(a_data->subresources.size());
			};
			spdlog::warn("Texture timing '{}': source {} -> {}; decode {:.0f} ms, process {:.0f} ms, {} {:.0f} ms, write {:.0f} ms", request.source.identity,
				dims(source), dims(result), ms(t0, t1), ms(t1, t2), processed.gpuCompression ? "CPU stand-in for GPU BC7" : "finalize (in process)", ms(t2, t3),
				ms(t3, t4));
		}
		return TextureConditionResult::Built;
	}

	std::shared_ptr<TextureSourceData> LoadPreparedTextureOnCpu(const std::shared_ptr<TextureAsset>& texture)
	{
		if (!texture || !texture->IsPipelineTexture()) throw std::runtime_error("LoadPreparedTextureOnCpu: not a pipeline texture");
		const TextureRequest& request = *texture->PipelineRequest();
		if (request.preparation == TexturePreparation::LoadOnly) return DecodeTextureSource(request);
		if (ProvidedPassthroughShape(request)) return ReadProvidedTextureMips(request, 0);
		ConditionTextureOnCpu(texture);
		const auto path = std::filesystem::path(TextureProcessingManager::ConditionedCachePath(texture->PipelineCacheKey()));
		auto data = LoadTextureSourceDataFromConditionedCacheFilePath(path.string(), "prepared on the CPU");
		if (!data) throw std::runtime_error("LoadPreparedTextureOnCpu: could not read the cache for '" + request.source.identity + "'");
		return data;
	}

	std::shared_ptr<TextureSourceData> DecodeTextureSource(const TextureRequest& request)
	{
		const auto& source = request.source;
		if (const auto* file = std::get_if<FileTextureSource>(&source.data)) {
			auto extension = file->path.extension().string();
			std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			const bool wicCannotRead = extension == ".hdr" || extension == ".tga";
			if (file->offset == 0 && file->size == 0 && !wicCannotRead) {
				return LoadTextureSourceDataFromFilePath(file->path.string(), request.preferSRGB, "texture pipeline");
			}
			const auto bytes = ReadFileRange(*file);
			return LoadTextureSourceDataFromImageBytes(bytes.data(), bytes.size(), request.preferSRGB, source.formatHint, source.identity);
		}
		if (const auto* memory = std::get_if<MemoryTextureSource>(&source.data)) {
			if (!memory->bytes) throw std::runtime_error("texture source '" + source.identity + "' has no bytes");
			return LoadTextureSourceDataFromImageBytes(memory->bytes->data(), memory->bytes->size(), request.preferSRGB, source.formatHint, source.identity);
		}
		if (std::holds_alternative<ProvidedTextureSource>(source.data)) {
			// Every mip, for processing or a LoadOnly upload.
			return ReadProvidedTextureMips(request, 0);
		}
		const auto& decoded = std::get<DecodedTextureSource>(source.data);
		if (!decoded.data) throw std::runtime_error("texture source '" + source.identity + "' has no pixels");
		return std::make_shared<TextureSourceData>(*decoded.data);
	}
}
