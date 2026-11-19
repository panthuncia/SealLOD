#include <BasicRenderer/Assets/ClusterLODTextureDecode.h>

#include <algorithm>
#include <array>
#include <cstring>

#include <DirectXTex.h>
#include <ktx.h>

// libktx reads the container (level index, Zstandard/zlib inflation, Basis Universal transcoding) and DirectXTex decodes the
// block-compressed levels, both as the renderer's KTX2 provider and texture conditioning use them (Ktx2Textures.cpp).

namespace clod_texture {

	namespace {

		constexpr std::array<uint8_t, 12> kKtx2Identifier = { 0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n' };
		constexpr size_t kKtx2HeaderBytes = 80u;

		uint32_t ReadU32(const uint8_t* at)
		{
			uint32_t value;
			std::memcpy(&value, at, sizeof(value));
			return value;
		}

		// How a supported VkFormat's levels become RGBA8. Values are VkFormat's.
		enum class LevelLayout { Unsupported, Rgba8, Bgra8, Block };

		LevelLayout LayoutOf(uint32_t vkFormat, DXGI_FORMAT& blockFormat)
		{
			blockFormat = DXGI_FORMAT_UNKNOWN;
			switch (vkFormat)
			{
			case 37: case 43: return LevelLayout::Rgba8;  // R8G8B8A8_UNORM, _SRGB
			case 44: case 50: return LevelLayout::Bgra8;  // B8G8R8A8_UNORM, _SRGB
			case 131: case 133: blockFormat = DXGI_FORMAT_BC1_UNORM; return LevelLayout::Block;
			case 132: case 134: blockFormat = DXGI_FORMAT_BC1_UNORM_SRGB; return LevelLayout::Block;
			case 135: blockFormat = DXGI_FORMAT_BC2_UNORM; return LevelLayout::Block;
			case 136: blockFormat = DXGI_FORMAT_BC2_UNORM_SRGB; return LevelLayout::Block;
			case 137: blockFormat = DXGI_FORMAT_BC3_UNORM; return LevelLayout::Block;
			case 138: blockFormat = DXGI_FORMAT_BC3_UNORM_SRGB; return LevelLayout::Block;
			case 145: blockFormat = DXGI_FORMAT_BC7_UNORM; return LevelLayout::Block;
			case 146: blockFormat = DXGI_FORMAT_BC7_UNORM_SRGB; return LevelLayout::Block;
			default: return LevelLayout::Unsupported;
			}
		}

		bool Fail(std::string* error, std::string message)
		{
			if (error != nullptr)
			{
				*error = std::move(message);
			}
			return false;
		}

		// One level of the texture as RGBA8, from its stored image. Block formats decode to the RGBA8 of the same sRGB-ness, so
		// DirectXTex passes the bytes through without a transfer conversion.
		bool DecodeLevel(LevelLayout layout, DXGI_FORMAT blockFormat, const uint8_t* stored, size_t storedBytes, uint32_t w, uint32_t h,
			std::vector<uint8_t>& out, std::string* error)
		{
			const size_t texels = static_cast<size_t>(w) * h;
			out.resize(texels * 4u);
			if (layout == LevelLayout::Rgba8 || layout == LevelLayout::Bgra8)
			{
				if (storedBytes < texels * 4u)
				{
					return Fail(error, "a level is shorter than its texels");
				}
				std::memcpy(out.data(), stored, texels * 4u);
				if (layout == LevelLayout::Bgra8)
				{
					for (size_t i = 0; i < texels; ++i)
					{
						std::swap(out[i * 4u], out[i * 4u + 2u]);
					}
				}
				return true;
			}
			size_t rowPitch = 0, slicePitch = 0;
			if (FAILED(DirectX::ComputePitch(blockFormat, w, h, rowPitch, slicePitch)) || storedBytes < slicePitch)
			{
				return Fail(error, "a level is shorter than its blocks");
			}
			DirectX::Image image{};
			image.width = w;
			image.height = h;
			image.format = blockFormat;
			image.rowPitch = rowPitch;
			image.slicePitch = slicePitch;
			image.pixels = const_cast<uint8_t*>(stored);
			const DXGI_FORMAT target = DirectX::IsSRGB(blockFormat) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
			DirectX::ScratchImage decoded;
			if (FAILED(DirectX::Decompress(image, target, decoded)))
			{
				return Fail(error, "DirectXTex cannot decode a level");
			}
			const DirectX::Image* result = decoded.GetImage(0, 0, 0);
			if (result == nullptr || result->format != target || result->width != w || result->height != h || result->rowPitch < size_t{ w } * 4u)
			{
				return Fail(error, "DirectXTex decoded a level to an unexpected image");
			}
			for (uint32_t y = 0; y < h; ++y)
			{
				std::memcpy(out.data() + static_cast<size_t>(y) * w * 4u, result->pixels + static_cast<size_t>(y) * result->rowPitch, size_t{ w } * 4u);
			}
			return true;
		}

	}

	bool IsKtx2(std::span<const uint8_t> bytes)
	{
		return bytes.size() >= kKtx2Identifier.size() && std::equal(kKtx2Identifier.begin(), kKtx2Identifier.end(), bytes.begin());
	}

	uint32_t Ktx2DecoderIdentity(std::span<const uint8_t> bytes)
	{
		if (bytes.size() < kKtx2HeaderBytes || !IsKtx2(bytes))
		{
			return 0u;
		}
		const uint32_t vkFormat = ReadU32(bytes.data() + 12);
		const uint32_t width = ReadU32(bytes.data() + 20);
		const uint32_t height = ReadU32(bytes.data() + 24);
		const uint32_t depth = ReadU32(bytes.data() + 28);
		const uint32_t layers = ReadU32(bytes.data() + 32);
		const uint32_t faces = ReadU32(bytes.data() + 36);
		const uint32_t supercompression = ReadU32(bytes.data() + 44);
		DXGI_FORMAT blockFormat;
		const bool format = vkFormat == 0u || LayoutOf(vkFormat, blockFormat) != LevelLayout::Unsupported; // 0: Basis Universal
		const bool shape = width > 0u && height > 0u && depth <= 1u && layers <= 1u && faces == 1u;
		return format && shape && supercompression <= 3u ? kKtx2DecoderRevision : 0u;
	}

	std::shared_ptr<Texture> DecodeKtx2(std::span<const uint8_t> bytes, std::string* error)
	{
		if (Ktx2DecoderIdentity(bytes) == 0u)
		{
			Fail(error, IsKtx2(bytes) ? "unsupported KTX2 format or layout" : "not a KTX2 file");
			return nullptr;
		}
		ktxTexture2* source = nullptr;
		if (ktxTexture2_CreateFromMemory(bytes.data(), bytes.size(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &source) != KTX_SUCCESS || source == nullptr)
		{
			Fail(error, "libktx cannot read it");
			return nullptr;
		}
		struct Destroy
		{
			ktxTexture2* texture;
			~Destroy() { ktxTexture_Destroy(ktxTexture(texture)); }
		} destroy{ source };
		if (ktxTexture2_NeedsTranscoding(source) && ktxTexture2_TranscodeBasis(source, KTX_TTF_RGBA32, 0) != KTX_SUCCESS)
		{
			Fail(error, "libktx cannot transcode its Basis Universal payload");
			return nullptr;
		}
		DXGI_FORMAT blockFormat;
		const LevelLayout layout = LayoutOf(source->vkFormat, blockFormat);
		if (layout == LevelLayout::Unsupported || source->baseDepth > 1u || source->numFaces != 1u || source->isArray || source->baseWidth == 0u ||
			source->baseHeight == 0u)
		{
			Fail(error, "unsupported KTX2 format or layout");
			return nullptr;
		}
		auto texture = std::make_shared<Texture>();
		texture->width = source->baseWidth;
		texture->height = source->baseHeight;
		const uint8_t* data = ktxTexture_GetData(ktxTexture(source));
		const size_t dataSize = ktxTexture_GetDataSize(ktxTexture(source));
		const uint32_t levels = std::max(source->numLevels, 1u);
		for (uint32_t level = 0; level < levels; ++level)
		{
			const uint32_t w = texture->MipWidth(level), h = texture->MipHeight(level);
			ktx_size_t offset = 0;
			if (ktxTexture_GetImageOffset(ktxTexture(source), level, 0, 0, &offset) != KTX_SUCCESS || offset > dataSize)
			{
				Fail(error, "libktx has no image for a level");
				return nullptr;
			}
			const size_t storedBytes = std::min<size_t>(ktxTexture_GetImageSize(ktxTexture(source), level), dataSize - offset);
			std::vector<uint8_t> texels;
			if (!DecodeLevel(layout, blockFormat, data + offset, storedBytes, w, h, texels, error))
			{
				return nullptr;
			}
			texture->mips.push_back(std::move(texels));
			if (w == 1u && h == 1u)
			{
				break;
			}
		}
		BuildMips(*texture);
		return texture;
	}

}
