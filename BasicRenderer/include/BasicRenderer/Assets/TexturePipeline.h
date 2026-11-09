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
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
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

	// A texture's shape without its pixels: format, size, and every mip's dimensions, slice-major as TextureSourceData
	// lays them out.
	struct TextureShape
	{
		org::TextureDescription desc;
		bool hasFullMipChain = false;
		bool isBlockCompressed = false;
	};

	// An application that has the texture's mips itself (game data in its own files and archives) hands them to the
	// pipeline on demand instead of the pipeline keeping a processed copy. When the request's processing would leave the
	// texture as it is (block-compressed, a full mip chain, nothing to convert), it is never processed or cached: its shape
	// comes from DescribeTexture and every mip window the streaming system asks for from ReadTextureMips. Otherwise it is
	// read once whole (ReadTextureMips(0)), processed, and cached like any other source. Called from worker threads, any
	// number at once; both may block on I/O.
	class ITextureMipProvider
	{
	public:
		virtual ~ITextureMipProvider() = default;
		// The whole texture's shape. Formats as stored; the pipeline applies the request's preferSRGB. nullopt: unreadable.
		[[nodiscard]] virtual std::optional<TextureShape> DescribeTexture() = 0;
		// Every slice's mips from topMip to the smallest, in the whole texture's layout: desc and subresources cover every
		// mip, and subresources above topMip may be null. Tightly packed rows (rowPitch, slicePitch as for a DDS). Throws or
		// returns null on failure.
		[[nodiscard]] virtual std::shared_ptr<TextureSourceData> ReadTextureMips(std::uint32_t topMip) = 0;
	};

	struct ProvidedTextureSource
	{
		std::shared_ptr<ITextureMipProvider> provider;
	};

	// Reads part of a file the application resolves itself (a loose file, an archive entry).
	class ITextureByteSource
	{
	public:
		virtual ~ITextureByteSource() = default;
		[[nodiscard]] virtual std::uint64_t Size() = 0;
		// length bytes from offset (fewer at the end of the file). Throws on failure.
		[[nodiscard]] virtual std::vector<std::uint8_t> Read(std::uint64_t offset, std::uint64_t length) = 0;
	};

	// A provider for a DDS file read through bytes: its header for the shape, then only the byte range of the mips asked
	// for. A DDS the GPU can't take as stored (legacy 24-bit, luminance) is decoded whole, which also routes it to
	// processing.
	[[nodiscard]] std::shared_ptr<ITextureMipProvider> MakeDdsTextureMipProvider(std::shared_ptr<ITextureByteSource> bytes, std::string debugName);

	// A provider for a KTX2 file read through bytes. Its header and level index give the shape; a mip window reads only its
	// levels' byte ranges, inflating zstd-supercompressed levels one by one. A block-compressed KTX2 with a full chain is
	// therefore streamed straight from the file, never processed or cached. Basis Universal payloads (ETC1S, UASTC) are
	// transcoded whole with libktx to BC4/BC5/BC7 by channel count, once, and kept by the provider.
	[[nodiscard]] std::shared_ptr<ITextureMipProvider> MakeKtx2TextureMipProvider(std::shared_ptr<ITextureByteSource> bytes, std::string debugName);
	// Whether bytes start with the KTX2 file identifier. Needs at least 12 bytes; fewer is false.
	[[nodiscard]] bool LooksLikeKtx2(std::span<const std::uint8_t> leadingBytes);
	// Whether a file starts with the KTX2 file identifier.
	[[nodiscard]] bool FileLooksLikeKtx2(const std::filesystem::path& path, std::uint64_t offset = 0);

	// Byte sources over a file range (size 0: to the end of the file) and over bytes in memory. Reads are positional, so any
	// number of threads may read at once.
	[[nodiscard]] std::shared_ptr<ITextureByteSource> MakeFileTextureByteSource(std::filesystem::path path, std::uint64_t offset = 0, std::uint64_t size = 0);
	[[nodiscard]] std::shared_ptr<ITextureByteSource> MakeMemoryTextureByteSource(std::shared_ptr<const std::vector<std::uint8_t>> bytes);

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
		std::variant<FileTextureSource, MemoryTextureSource, DecodedTextureSource, ProvidedTextureSource> data;
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

	// NotProcessed: a LoadOnly texture, or a provided one its processing would leave as it is (it streams from its
	// provider and has no cache entry).
	enum class TextureConditionResult { AlreadyCached, Built, NotProcessed };

	// Whether a Processed request's texture streams straight from its ITextureMipProvider: its processing would leave it as
	// it is. Reads the provider's shape (DescribeTexture). False for any other source.
	[[nodiscard]] bool IsProvidedPassthrough(const TextureRequest& request);

	// Writes a pipeline texture's processed cache entry on the calling thread, with no GPU and no running
	// pipeline, so offline tools can finish a cache the renderer then reads as a hit. Throws on failure.
	TextureConditionResult ConditionTextureOnCpu(const std::shared_ptr<TextureAsset>& texture);
	// A pipeline texture's pixels as the renderer streams them, every mip on the CPU: its processed cache entry,
	// conditioned on the calling thread first when it's missing (ConditionTextureOnCpu). A LoadOnly texture is its
	// decoded source. For offline consumers that must see what the device samples (the opacity micromap bake). Throws
	// on failure.
	[[nodiscard]] std::shared_ptr<TextureSourceData> LoadPreparedTextureOnCpu(const std::shared_ptr<TextureAsset>& texture);

	// One level of an RGBA8 mip chain (sRGB or not, as the BC7 it encodes to; BC4 reads red, BC5 red and green), rows
	// rowPitch apart.
	struct BlockEncodeLevel
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t rowPitch = 0;
		std::span<const std::byte> pixels;
	};
	enum class BlockEncodeFormat : std::uint8_t { BC4, BC5, BC7 };
	// Encodes a chain (one array slice or cube face) to a block format: each level's rows of 4x4 blocks in turn, at least
	// one block a side, as the format lays them out. Called from any thread, and may block. Nothing to leave that chain
	// to DirectXTex.
	using BlockEncoder = std::function<std::optional<std::vector<std::byte>>(BlockEncodeFormat format, std::span<const BlockEncodeLevel> levels)>;
	// For offline tools with a GPU encoder of their own: CPU processing (ConditionTextureOnCpu, and the CPU taking a
	// texture the GPU BC7 job can't) encodes BC7, BC5 (normals) and BC4 (scalars) through it instead of DirectXTex's CPU
	// search, which takes minutes for a large texture. Process-wide; an empty one restores DirectXTex.
	void SetBlockEncoder(BlockEncoder encoder);

	// Decodes a request's source to pixels, unprocessed, on the calling thread: for CPU
	// consumers of texture data (preprocessing tools). Throws on failure.
	[[nodiscard]] std::shared_ptr<TextureSourceData> DecodeTextureSource(const TextureRequest& request);
}
