#pragma once

// The texture pipeline: the one way textures are loaded.
//
// A texture is requested from a source and a preparation, and loads as graph work
// (src/Assets/Textures/Pipeline). A LoadOnly texture is decoded and uploaded as it is. A
// Processed one is looked up in the processed cache by the request's cache key: on a hit
// its mip window uploads from the cache file; on a miss the source is decoded and processed
// (CPU, GPU mips, BC7), shown from that result, and written to the cache in the background.
// Requesting never blocks or loads anything: the texture loads once something tracks it (a
// material binding, or LoadTexture).

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <BasicRenderer/Assets/Texture.h>

namespace org { class Sampler; }

namespace br::textures
{
	// Encoded image bytes in a file: all of it, or the byte range [offset, offset + size)
	// (a glTF bufferView inside its .glb).
	struct FileTextureSource
	{
		std::filesystem::path path;
		std::uint64_t offset = 0;
		std::uint64_t size = 0;  // 0: to the end of the file
	};

	// Encoded image bytes held in memory (a data URI, a USD in-memory asset).
	struct MemoryTextureSource
	{
		std::shared_ptr<const std::vector<std::uint8_t>> bytes;
	};

	// Pixels produced in code (lookup tables).
	struct DecodedTextureSource
	{
		std::shared_ptr<const TextureSourceData> data;
	};

	struct TextureSource
	{
		// Stable identity of the image: the normalized file path, or "<container>#<part>"
		// for an image inside another file, or "proc:<name>". Keys the processed cache with
		// version and the processing settings.
		std::string identity;
		// Changes whenever the source's pixels can: a file's write time, a content hash.
		std::string version;
		// Container hint for encoded bytes without a telling extension: a MIME type or an
		// extension ("image/png", ".tga").
		std::string formatHint;
		std::variant<FileTextureSource, MemoryTextureSource, DecodedTextureSource> data;
	};

	// How a texture is prepared between its source and the GPU.
	enum class TexturePreparation : std::uint8_t
	{
		// Decoded and uploaded as it is: no processing, no processed cache, no mip
		// streaming (lookup and noise textures; sources a consumer processes itself, like an
		// HDRI converted to a cubemap once loaded).
		LoadOnly,
		// The processing chain the settings ask for (mips, block compression, normal
		// convention), kept in the processed cache, its mip window streamed (material textures).
		Processed,
	};

	struct TextureRequest
	{
		TextureSource source;
		TexturePreparation preparation = TexturePreparation::Processed;
		// Used by Processed (LoadOnly uses only maxMipLevels).
		TextureProcessingSettings processing;
		// Processed: stream the mip window by sampling feedback (off keeps every mip resident).
		bool mipStreaming = true;
		// LoadOnly: a source without a full mip chain gets one generated on the GPU at upload.
		bool generateMipMaps = false;
		bool preferSRGB = false;
		bool allowRTV = false;
		bool allowUAV = false;
		std::shared_ptr<org::Sampler> sampler;
		std::string debugName;
	};

	// A file source whose identity is the normalized path and whose version is its write time.
	[[nodiscard]] TextureSource MakeFileTextureSource(const std::filesystem::path& path);

	// The processed-cache key of a request: identity, version and processing settings.
	[[nodiscard]] std::string TextureCacheKey(const TextureRequest& request);

	// The texture for a request. Never blocks; requests with the same cache key share one
	// texture while it lives.
	[[nodiscard]] std::shared_ptr<TextureAsset> RequestTexture(TextureRequest request);

	// Loads a texture no material binding tracks (lookup textures, an HDRI): ready runs once
	// its image is resident (true) or its load failed for good (false), on a worker thread,
	// or at once when it is already resident. A consumer with its own processing starts it
	// from ready.
	void LoadTexture(const std::shared_ptr<TextureAsset>& texture, std::function<void(bool loaded)> ready);

	// Blocks until a LoadTexture texture is ready; false on failure or timeout. Renderer
	// initialization only.
	bool WaitForTexture(const std::shared_ptr<TextureAsset>& texture, std::chrono::milliseconds timeout);

	// Decodes a request's source to pixels, unprocessed, on the calling thread: for CPU
	// consumers of texture data (preprocessing tools). Throws on failure.
	[[nodiscard]] std::shared_ptr<TextureSourceData> DecodeTextureSource(const TextureRequest& request);
}
