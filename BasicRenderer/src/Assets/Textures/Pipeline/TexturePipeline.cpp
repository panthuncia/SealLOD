#include <BasicRenderer/Assets/TexturePipeline.h>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

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
		const auto& decoded = std::get<DecodedTextureSource>(source.data);
		if (!decoded.data) throw std::runtime_error("texture source '" + source.identity + "' has no pixels");
		return std::make_shared<TextureSourceData>(*decoded.data);
	}
}
