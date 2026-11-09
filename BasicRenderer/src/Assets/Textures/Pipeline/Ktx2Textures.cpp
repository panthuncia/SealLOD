#include <BasicRenderer/Assets/TexturePipeline.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>

#include <DirectXTex.h>
#include <spdlog/spdlog.h>
#include <ktx.h>
#include <zstd.h>

#include <rhi_dx12.h>
#include <rhi_helpers.h>

// KTX2 textures (https://registry.khronos.org/KTX/specs/2.0/ktxspec.v2.html). The header and level index are a fixed
// layout, read here directly so a mip window costs exactly its levels' bytes. libktx (KTX-Software) is used for what it
// alone does: Basis Universal transcoding.

namespace br::textures
{
	namespace
	{
		constexpr std::array<std::uint8_t, 12> kKtx2Identifier = { 0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n' };
		constexpr std::size_t kHeaderBytes = 80u;
		constexpr std::size_t kLevelIndexEntryBytes = 24u;

		enum : std::uint32_t
		{
			kSupercompressionNone = 0,
			kSupercompressionBasisLZ = 1,
			kSupercompressionZstd = 2,
			kSupercompressionZlib = 3,
		};

		std::uint32_t ReadU32(const std::uint8_t* at) { std::uint32_t value; std::memcpy(&value, at, sizeof(value)); return value; }
		std::uint64_t ReadU64(const std::uint8_t* at) { std::uint64_t value; std::memcpy(&value, at, sizeof(value)); return value; }

		std::uint32_t FullMipCount(std::uint32_t width, std::uint32_t height)
		{
			std::uint32_t count = 1;
			for (std::uint32_t size = (std::max)(width, height); size > 1u; size >>= 1u) ++count;
			return count;
		}

		// The VkFormats a KTX2 stores that D3D12 samples as they are. Values are VkFormat's.
		DXGI_FORMAT DxgiFromVkFormat(std::uint32_t vkFormat)
		{
			switch (vkFormat) {
			case 9: return DXGI_FORMAT_R8_UNORM;
			case 10: return DXGI_FORMAT_R8_SNORM;
			case 16: return DXGI_FORMAT_R8G8_UNORM;
			case 17: return DXGI_FORMAT_R8G8_SNORM;
			case 37: return DXGI_FORMAT_R8G8B8A8_UNORM;
			case 38: return DXGI_FORMAT_R8G8B8A8_SNORM;
			case 43: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
			case 44: return DXGI_FORMAT_B8G8R8A8_UNORM;
			case 50: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
			case 64: return DXGI_FORMAT_R10G10B10A2_UNORM;  // A2B10G10R10_UNORM_PACK32
			case 70: return DXGI_FORMAT_R16_UNORM;
			case 76: return DXGI_FORMAT_R16_FLOAT;
			case 77: return DXGI_FORMAT_R16G16_UNORM;
			case 83: return DXGI_FORMAT_R16G16_FLOAT;
			case 91: return DXGI_FORMAT_R16G16B16A16_UNORM;
			case 97: return DXGI_FORMAT_R16G16B16A16_FLOAT;
			case 100: return DXGI_FORMAT_R32_FLOAT;
			case 103: return DXGI_FORMAT_R32G32_FLOAT;
			case 109: return DXGI_FORMAT_R32G32B32A32_FLOAT;
			case 122: return DXGI_FORMAT_R11G11B10_FLOAT;  // B10G11R11_UFLOAT_PACK32
			case 123: return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;  // E5B9G9R9_UFLOAT_PACK32
			case 131: case 133: return DXGI_FORMAT_BC1_UNORM;
			case 132: case 134: return DXGI_FORMAT_BC1_UNORM_SRGB;
			case 135: return DXGI_FORMAT_BC2_UNORM;
			case 136: return DXGI_FORMAT_BC2_UNORM_SRGB;
			case 137: return DXGI_FORMAT_BC3_UNORM;
			case 138: return DXGI_FORMAT_BC3_UNORM_SRGB;
			case 139: return DXGI_FORMAT_BC4_UNORM;
			case 140: return DXGI_FORMAT_BC4_SNORM;
			case 141: return DXGI_FORMAT_BC5_UNORM;
			case 142: return DXGI_FORMAT_BC5_SNORM;
			case 143: return DXGI_FORMAT_BC6H_UF16;
			case 144: return DXGI_FORMAT_BC6H_SF16;
			case 145: return DXGI_FORMAT_BC7_UNORM;
			case 146: return DXGI_FORMAT_BC7_UNORM_SRGB;
			default: return DXGI_FORMAT_UNKNOWN;
			}
		}

		// Lays out every subresource of a 2D texture, item-major as TextureSourceData orders them.
		bool BuildDimensions(DXGI_FORMAT format, std::uint32_t width, std::uint32_t height, std::uint32_t mips, std::uint32_t items,
			std::vector<org::ImageDimensions>& out)
		{
			out.clear();
			out.reserve(static_cast<std::size_t>(mips) * items);
			for (std::uint32_t item = 0; item < items; ++item) {
				std::size_t w = width;
				std::size_t h = height;
				for (std::uint32_t mip = 0; mip < mips; ++mip) {
					std::size_t rowPitch = 0;
					std::size_t slicePitch = 0;
					if (FAILED(DirectX::ComputePitch(format, w, h, rowPitch, slicePitch))) return false;
					org::ImageDimensions dims{};
					dims.width = static_cast<std::uint32_t>(w);
					dims.height = static_cast<std::uint32_t>(h);
					dims.rowPitch = rowPitch;
					dims.slicePitch = slicePitch;
					out.push_back(dims);
					w = (std::max)(std::size_t(1), w / 2u);
					h = (std::max)(std::size_t(1), h / 2u);
				}
			}
			return true;
		}

		class Ktx2TextureMipProvider final : public ITextureMipProvider
		{
		public:
			Ktx2TextureMipProvider(std::shared_ptr<ITextureByteSource> bytes, std::string debugName)
				: _bytes(std::move(bytes)), _debugName(std::move(debugName)) {}

			std::optional<TextureShape> DescribeTexture() override
			{
				std::call_once(_describeOnce, [this] { Describe(); });
				return _layout ? std::optional<TextureShape>(_layout->shape) : std::nullopt;
			}

			std::shared_ptr<TextureSourceData> ReadTextureMips(std::uint32_t topMip) override
			{
				if (!DescribeTexture()) throw std::runtime_error("KTX2 texture '" + _debugName + "' has no readable header");
				const Layout& layout = *_layout;
				if (layout.transcoded) return layout.transcoded;

				const std::uint32_t mips = layout.mipCount;
				const std::uint32_t items = layout.itemCount;
				// One item streams from its top mip; arrays and cubes only ever load whole.
				const std::uint32_t first = items == 1u ? (std::min)(topMip, mips - 1u) : 0u;

				// The levels asked for are one span of the file whichever order they are stored in (smallest first, as
				// the spec recommends, or largest first): read it once and cut it up.
				std::uint64_t begin = UINT64_MAX;
				std::uint64_t end = 0;
				for (std::uint32_t level = first; level < mips; ++level) {
					begin = (std::min)(begin, layout.levels[level].byteOffset);
					end = (std::max)(end, layout.levels[level].byteOffset + layout.levels[level].byteLength);
				}
				const auto span = _bytes->Read(begin, end - begin);
				if (span.size() < end - begin) {
					throw std::runtime_error("KTX2 texture '" + _debugName + "' is shorter than its level index says");
				}

				auto data = std::make_shared<TextureSourceData>();
				data->desc = layout.shape.desc;
				data->hasFullMipChain = layout.shape.hasFullMipChain;
				data->isBlockCompressed = layout.shape.isBlockCompressed;
				data->subresources.resize(static_cast<std::size_t>(items) * mips);
				for (std::uint32_t level = first; level < mips; ++level) {
					const LevelEntry& entry = layout.levels[level];
					const std::uint8_t* stored = span.data() + (entry.byteOffset - begin);
					std::vector<std::uint8_t> inflated;
					const std::uint8_t* levelBytes = stored;
					if (layout.supercompression == kSupercompressionZstd) {
						inflated.resize(static_cast<std::size_t>(entry.uncompressedByteLength));
						const std::size_t written = ZSTD_decompress(inflated.data(), inflated.size(), stored, static_cast<std::size_t>(entry.byteLength));
						if (ZSTD_isError(written) || written != inflated.size()) {
							throw std::runtime_error("KTX2 texture '" + _debugName + "': level " + std::to_string(level) + " does not inflate: " +
								(ZSTD_isError(written) ? ZSTD_getErrorName(written) : "short"));
						}
						levelBytes = inflated.data();
					}
					// A level holds every item's image of that level, item after item (layers, then faces), tightly packed.
					std::uint64_t within = 0;
					for (std::uint32_t item = 0; item < items; ++item) {
						const std::size_t index = static_cast<std::size_t>(item) * mips + level;
						const auto length = static_cast<std::size_t>(layout.shape.desc.imageDimensions[index].slicePitch);
						if (within + length > layout.levelBytes(level)) {
							throw std::runtime_error("KTX2 texture '" + _debugName + "': level " + std::to_string(level) + " is smaller than its images");
						}
						data->subresources[index] = std::make_shared<std::vector<std::uint8_t>>(
							levelBytes + within, levelBytes + within + length);
						within += length;
					}
				}
				return data;
			}

		private:
			struct LevelEntry
			{
				std::uint64_t byteOffset = 0;
				std::uint64_t byteLength = 0;
				std::uint64_t uncompressedByteLength = 0;
			};

			struct Layout
			{
				TextureShape shape;
				std::uint32_t mipCount = 0;
				std::uint32_t itemCount = 0;
				std::uint32_t supercompression = kSupercompressionNone;
				std::vector<LevelEntry> levels;
				// A Basis payload, transcoded once whole.
				std::shared_ptr<TextureSourceData> transcoded;

				std::uint64_t levelBytes(std::uint32_t level) const
				{
					return supercompression == kSupercompressionZstd ? levels[level].uncompressedByteLength : levels[level].byteLength;
				}
			};

			void Describe()
			{
				try {
					const auto fileSize = _bytes->Size();
					const auto header = _bytes->Read(0, (std::min<std::uint64_t>)(fileSize, kHeaderBytes));
					if (header.size() < kHeaderBytes || !LooksLikeKtx2(header)) {
						spdlog::warn("Provided texture '{}': not a KTX2 file", _debugName);
						return;
					}
					const std::uint32_t vkFormat = ReadU32(header.data() + 12);
					const std::uint32_t width = ReadU32(header.data() + 20);
					const std::uint32_t height = ReadU32(header.data() + 24);
					const std::uint32_t depth = ReadU32(header.data() + 28);
					const std::uint32_t layers = ReadU32(header.data() + 32);
					const std::uint32_t faces = ReadU32(header.data() + 36);
					const std::uint32_t levelCount = (std::max)(1u, ReadU32(header.data() + 40));
					const std::uint32_t supercompression = ReadU32(header.data() + 44);
					if (depth > 1u || height == 0u || width == 0u || (faces != 1u && faces != 6u)) {
						spdlog::warn("Provided texture '{}': KTX2 {}x{}x{} with {} faces is not a 2D texture, array or cube", _debugName, width, height, depth, faces);
						return;
					}
					if (vkFormat == 0u || supercompression == kSupercompressionBasisLZ || supercompression == kSupercompressionZlib) {
						// Basis Universal (vkFormat undefined) or zlib: libktx loads and, for Basis, transcodes it whole.
						DescribeThroughLibktx(fileSize);
						return;
					}
					if (supercompression != kSupercompressionNone && supercompression != kSupercompressionZstd) {
						spdlog::warn("Provided texture '{}': KTX2 supercompression scheme {} is not supported", _debugName, supercompression);
						return;
					}
					const DXGI_FORMAT format = DxgiFromVkFormat(vkFormat);
					if (format == DXGI_FORMAT_UNKNOWN) {
						spdlog::warn("Provided texture '{}': KTX2 VkFormat {} has no D3D12 equivalent here", _debugName, vkFormat);
						return;
					}

					Layout layout;
					layout.mipCount = levelCount;
					layout.itemCount = (std::max)(1u, layers) * faces;
					layout.supercompression = supercompression;
					const auto index = _bytes->Read(kHeaderBytes, static_cast<std::uint64_t>(levelCount) * kLevelIndexEntryBytes);
					if (index.size() < static_cast<std::size_t>(levelCount) * kLevelIndexEntryBytes) {
						spdlog::warn("Provided texture '{}': KTX2 level index is truncated", _debugName);
						return;
					}
					layout.levels.resize(levelCount);
					for (std::uint32_t level = 0; level < levelCount; ++level) {
						const std::uint8_t* entry = index.data() + static_cast<std::size_t>(level) * kLevelIndexEntryBytes;
						auto& out = layout.levels[level];
						out.byteOffset = ReadU64(entry);
						out.byteLength = ReadU64(entry + 8);
						out.uncompressedByteLength = ReadU64(entry + 16);
						if (out.byteOffset + out.byteLength > fileSize) {
							spdlog::warn("Provided texture '{}': KTX2 level {} lies past the end of the file", _debugName, level);
							return;
						}
					}

					auto& desc = layout.shape.desc;
					if (!BuildDimensions(format, width, height, levelCount, layout.itemCount, desc.imageDimensions)) {
						spdlog::warn("Provided texture '{}': cannot lay out KTX2 mips", _debugName);
						return;
					}
					for (std::uint32_t level = 0; level < levelCount; ++level) {
						std::uint64_t needed = 0;
						for (std::uint32_t item = 0; item < layout.itemCount; ++item) {
							needed += desc.imageDimensions[static_cast<std::size_t>(item) * levelCount + level].slicePitch;
						}
						if (layout.levelBytes(level) < needed) {
							spdlog::warn("Provided texture '{}': KTX2 level {} holds {} bytes, its images need {}", _debugName, level, layout.levelBytes(level), needed);
							return;
						}
					}
					FinishShape(layout, format, width, height, layers, faces);
					_layout = std::move(layout);
				}
				catch (const std::exception& error) {
					spdlog::warn("Provided texture '{}': KTX2 header unreadable: {}", _debugName, error.what());
				}
			}

			void DescribeThroughLibktx(std::uint64_t fileSize)
			{
				const auto file = _bytes->Read(0, fileSize);
				ktxTexture2* texture = nullptr;
				if (ktxTexture2_CreateFromMemory(file.data(), file.size(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &texture) != KTX_SUCCESS || !texture) {
					spdlog::warn("Provided texture '{}': libktx cannot read this KTX2", _debugName);
					return;
				}
				struct Destroy { ktxTexture2* t; ~Destroy() { ktxTexture_Destroy(ktxTexture(t)); } } destroy{ texture };

				if (ktxTexture2_NeedsTranscoding(texture)) {
					const std::uint32_t components = ktxTexture2_GetNumComponents(texture);
					const ktx_transcode_fmt_e target = components == 1u ? KTX_TTF_BC4_R : components == 2u ? KTX_TTF_BC5_RG : KTX_TTF_BC7_RGBA;
					if (ktxTexture2_TranscodeBasis(texture, target, 0) != KTX_SUCCESS) {
						spdlog::warn("Provided texture '{}': Basis Universal transcode failed", _debugName);
						return;
					}
				}
				DXGI_FORMAT format = DxgiFromVkFormat(texture->vkFormat);
				if (format == DXGI_FORMAT_UNKNOWN) {
					spdlog::warn("Provided texture '{}': KTX2 VkFormat {} has no D3D12 equivalent here", _debugName, texture->vkFormat);
					return;
				}
				// A transcoded Basis texture's colour space comes from its data format descriptor, not its VkFormat.
				if (ktxTexture2_GetTransferFunction_e(texture) == KHR_DF_TRANSFER_SRGB) format = DirectX::MakeSRGB(format);

				Layout layout;
				layout.mipCount = texture->numLevels;
				layout.itemCount = (std::max)(1u, texture->numLayers) * texture->numFaces;
				auto& desc = layout.shape.desc;
				if (!BuildDimensions(format, texture->baseWidth, texture->baseHeight, layout.mipCount, layout.itemCount, desc.imageDimensions)) {
					spdlog::warn("Provided texture '{}': cannot lay out KTX2 mips", _debugName);
					return;
				}
				FinishShape(layout, format, texture->baseWidth, texture->baseHeight, texture->numLayers, texture->numFaces);

				auto data = std::make_shared<TextureSourceData>();
				data->desc = desc;
				data->hasFullMipChain = layout.shape.hasFullMipChain;
				data->isBlockCompressed = layout.shape.isBlockCompressed;
				data->subresources.resize(desc.imageDimensions.size());
				const std::uint8_t* base = ktxTexture_GetData(ktxTexture(texture));
				const std::size_t dataSize = ktxTexture_GetDataSize(ktxTexture(texture));
				for (std::uint32_t layer = 0; layer < (std::max)(1u, texture->numLayers); ++layer) {
					for (std::uint32_t face = 0; face < texture->numFaces; ++face) {
						const std::uint32_t item = layer * texture->numFaces + face;
						for (std::uint32_t level = 0; level < layout.mipCount; ++level) {
							ktx_size_t offset = 0;
							if (ktxTexture_GetImageOffset(ktxTexture(texture), level, layer, face, &offset) != KTX_SUCCESS) {
								spdlog::warn("Provided texture '{}': libktx has no image for level {}", _debugName, level);
								return;
							}
							const std::size_t index = static_cast<std::size_t>(item) * layout.mipCount + level;
							const auto length = static_cast<std::size_t>(desc.imageDimensions[index].slicePitch);
							if (offset + length > dataSize) {
								spdlog::warn("Provided texture '{}': libktx image for level {} is short", _debugName, level);
								return;
							}
							data->subresources[index] = std::make_shared<std::vector<std::uint8_t>>(base + offset, base + offset + length);
						}
					}
				}
				layout.transcoded = std::move(data);
				_layout = std::move(layout);
			}

			static void FinishShape(Layout& layout, DXGI_FORMAT format, std::uint32_t width, std::uint32_t height, std::uint32_t layers, std::uint32_t faces)
			{
				auto& desc = layout.shape.desc;
				desc.format = rhi::helpers::ToRHI(format);
				desc.channels = static_cast<unsigned short>(rhi::helpers::FormatChannelCount(desc.format));
				desc.isCubemap = faces == 6u;
				desc.isArray = layers > 1u && !desc.isCubemap;
				desc.arraySize = (std::max)(1u, layers);
				layout.shape.isBlockCompressed = DirectX::IsCompressed(format);
				layout.shape.hasFullMipChain = layout.mipCount == FullMipCount(width, height);
			}

			std::shared_ptr<ITextureByteSource> _bytes;
			std::string _debugName;
			std::once_flag _describeOnce;
			std::optional<Layout> _layout;
		};

		class FileTextureByteSource final : public ITextureByteSource
		{
		public:
			FileTextureByteSource(std::filesystem::path path, std::uint64_t offset, std::uint64_t size)
				: _path(std::move(path)), _offset(offset), _size(size) {}

			std::uint64_t Size() override
			{
				std::call_once(_sizeOnce, [this] {
					if (_size != 0) return;
					const auto fileSize = std::filesystem::file_size(_path);
					_size = fileSize > _offset ? fileSize - _offset : 0;
				});
				return _size;
			}

			std::vector<std::uint8_t> Read(std::uint64_t offset, std::uint64_t length) override
			{
				const auto size = Size();
				if (offset >= size) return {};
				length = (std::min)(length, size - offset);
				// A stream per read: positional, so threads never share a file position, and no handle outlives the read.
				std::ifstream stream(_path, std::ios::binary);
				if (!stream) throw std::runtime_error("cannot open '" + _path.string() + "'");
				stream.seekg(static_cast<std::streamoff>(_offset + offset));
				std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
				stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length));
				bytes.resize(static_cast<std::size_t>(stream.gcount()));
				return bytes;
			}

		private:
			std::filesystem::path _path;
			std::uint64_t _offset = 0;
			std::uint64_t _size = 0;
			std::once_flag _sizeOnce;
		};

		class MemoryTextureByteSource final : public ITextureByteSource
		{
		public:
			explicit MemoryTextureByteSource(std::shared_ptr<const std::vector<std::uint8_t>> bytes) : _bytes(std::move(bytes)) {}

			std::uint64_t Size() override { return _bytes->size(); }

			std::vector<std::uint8_t> Read(std::uint64_t offset, std::uint64_t length) override
			{
				if (offset >= _bytes->size()) return {};
				length = (std::min<std::uint64_t>)(length, _bytes->size() - offset);
				return { _bytes->begin() + static_cast<std::ptrdiff_t>(offset), _bytes->begin() + static_cast<std::ptrdiff_t>(offset + length) };
			}

		private:
			std::shared_ptr<const std::vector<std::uint8_t>> _bytes;
		};
	}

	bool LooksLikeKtx2(std::span<const std::uint8_t> leadingBytes)
	{
		return leadingBytes.size() >= kKtx2Identifier.size() &&
			std::equal(kKtx2Identifier.begin(), kKtx2Identifier.end(), leadingBytes.begin());
	}

	bool FileLooksLikeKtx2(const std::filesystem::path& path, std::uint64_t offset)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream) return false;
		stream.seekg(static_cast<std::streamoff>(offset));
		std::array<std::uint8_t, 12> leading{};
		stream.read(reinterpret_cast<char*>(leading.data()), static_cast<std::streamsize>(leading.size()));
		return stream.gcount() == static_cast<std::streamsize>(leading.size()) && LooksLikeKtx2(leading);
	}

	std::shared_ptr<ITextureMipProvider> MakeKtx2TextureMipProvider(std::shared_ptr<ITextureByteSource> bytes, std::string debugName)
	{
		if (!bytes) throw std::invalid_argument("MakeKtx2TextureMipProvider: no byte source");
		return std::make_shared<Ktx2TextureMipProvider>(std::move(bytes), std::move(debugName));
	}

	std::shared_ptr<ITextureByteSource> MakeFileTextureByteSource(std::filesystem::path path, std::uint64_t offset, std::uint64_t size)
	{
		return std::make_shared<FileTextureByteSource>(std::move(path), offset, size);
	}

	std::shared_ptr<ITextureByteSource> MakeMemoryTextureByteSource(std::shared_ptr<const std::vector<std::uint8_t>> bytes)
	{
		if (!bytes) throw std::invalid_argument("MakeMemoryTextureByteSource: no bytes");
		return std::make_shared<MemoryTextureByteSource>(std::move(bytes));
	}
}
