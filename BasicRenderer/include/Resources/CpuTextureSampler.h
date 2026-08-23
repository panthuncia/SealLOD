#pragma once

#include "Resources/Texture.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>

#include <DirectXTex.h>

namespace SARP::Resources
{
	struct CpuTextureSample
	{
		float r{ 1.0f };
		float g{ 1.0f };
		float b{ 1.0f };
		float a{ 1.0f };
	};

	[[nodiscard]] inline DXGI_FORMAT CpuTextureToDxgi(rhi::Format format)
	{
		switch (format) {
		case rhi::Format::BC1_UNorm:
			return DXGI_FORMAT_BC1_UNORM;
		case rhi::Format::BC1_UNorm_sRGB:
			return DXGI_FORMAT_BC1_UNORM_SRGB;
		case rhi::Format::BC2_UNorm:
			return DXGI_FORMAT_BC2_UNORM;
		case rhi::Format::BC2_UNorm_sRGB:
			return DXGI_FORMAT_BC2_UNORM_SRGB;
		case rhi::Format::BC3_UNorm:
			return DXGI_FORMAT_BC3_UNORM;
		case rhi::Format::BC3_UNorm_sRGB:
			return DXGI_FORMAT_BC3_UNORM_SRGB;
		case rhi::Format::BC4_UNorm:
		case rhi::Format::BC4_Typeless:
			return DXGI_FORMAT_BC4_UNORM;
		case rhi::Format::BC4_SNorm:
			return DXGI_FORMAT_BC4_SNORM;
		case rhi::Format::BC5_UNorm:
			return DXGI_FORMAT_BC5_UNORM;
		case rhi::Format::BC5_SNorm:
			return DXGI_FORMAT_BC5_SNORM;
		case rhi::Format::BC7_UNorm:
			return DXGI_FORMAT_BC7_UNORM;
		case rhi::Format::BC7_UNorm_sRGB:
			return DXGI_FORMAT_BC7_UNORM_SRGB;
		default:
			return DXGI_FORMAT_UNKNOWN;
		}
	}

	class CpuTextureSampler
	{
	public:
		[[nodiscard]] static std::optional<CpuTextureSampler> Create(
			const std::shared_ptr<TextureAsset>& texture,
			const char* reason)
		{
			if (!texture) {
				return std::nullopt;
			}

			std::shared_ptr<TextureSourceData> source;
			try {
				source = texture->BuildSourceData(reason);
			} catch (...) {
				return std::nullopt;
			}
			return Create(std::move(source));
		}

		[[nodiscard]] static std::optional<CpuTextureSampler> Create(std::shared_ptr<TextureSourceData> source)
		{
			if (!source || source->subresources.empty() || source->desc.imageDimensions.empty()) {
				return std::nullopt;
			}
			const auto format = rhi::helpers::stripSrgb(source->desc.format);
			const auto& dims = source->desc.imageDimensions.front();
			if (dims.width == 0u || dims.height == 0u) {
				return std::nullopt;
			}
			const auto& bytes = source->subresources.front();
			if (!bytes || bytes->size() < dims.slicePitch) {
				return std::nullopt;
			}

			CpuTextureSampler sampler;
			sampler._source = std::move(source);
			if (sampler._source->isBlockCompressed || rhi::helpers::IsBlockCompressed(sampler._source->desc.format)) {
				const DXGI_FORMAT dxgiFormat = CpuTextureToDxgi(sampler._source->desc.format);
				if (dxgiFormat == DXGI_FORMAT_UNKNOWN) {
					return std::nullopt;
				}

				DirectX::Image compressedImage{};
				compressedImage.width = dims.width;
				compressedImage.height = dims.height;
				compressedImage.format = dxgiFormat;
				compressedImage.rowPitch = static_cast<std::size_t>(dims.rowPitch);
				compressedImage.slicePitch = static_cast<std::size_t>(dims.slicePitch);
				compressedImage.pixels = bytes->data();

				DirectX::TexMetadata metadata{};
				metadata.width = dims.width;
				metadata.height = dims.height;
				metadata.depth = 1u;
				metadata.arraySize = 1u;
				metadata.mipLevels = 1u;
				metadata.format = dxgiFormat;
				metadata.dimension = DirectX::TEX_DIMENSION_TEXTURE2D;

				DirectX::ScratchImage decompressed;
				if (FAILED(DirectX::Decompress(
						std::addressof(compressedImage),
						1u,
						metadata,
						DXGI_FORMAT_R8G8B8A8_UNORM,
						decompressed))) {
					return std::nullopt;
				}
				const auto* image = decompressed.GetImage(0u, 0u, 0u);
				if (!image || !image->pixels || image->rowPitch < image->width * 4ull || image->slicePitch < image->rowPitch * image->height) {
					return std::nullopt;
				}

				sampler._ownedBytes = std::make_shared<std::vector<std::uint8_t>>(
					image->pixels,
					image->pixels + image->slicePitch);
				sampler._bytes = sampler._ownedBytes;
				sampler._width = static_cast<std::uint32_t>(image->width);
				sampler._height = static_cast<std::uint32_t>(image->height);
				sampler._rowPitch = static_cast<std::uint32_t>(image->rowPitch);
				sampler._format = rhi::Format::R8G8B8A8_UNorm;
				return sampler;
			}

			switch (format) {
			case rhi::Format::R8_UNorm:
				if (dims.rowPitch < dims.width) {
					return std::nullopt;
				}
				break;
			case rhi::Format::R16_UNorm:
			case rhi::Format::R16_Typeless:
			case rhi::Format::R16_Float:
				if (dims.rowPitch < static_cast<std::uint64_t>(dims.width) * 2ull) {
					return std::nullopt;
				}
				break;
			case rhi::Format::R8G8B8A8_UNorm:
			case rhi::Format::B8G8R8A8_UNorm:
				if (dims.rowPitch < static_cast<std::uint64_t>(dims.width) * 4ull) {
					return std::nullopt;
				}
				break;
			default:
				return std::nullopt;
			}

			sampler._bytes = sampler._source->subresources.front();
			sampler._width = dims.width;
			sampler._height = dims.height;
			sampler._rowPitch = static_cast<std::uint32_t>(dims.rowPitch);
			sampler._format = format;
			return sampler;
		}

		[[nodiscard]] CpuTextureSample Sample(float u, float v) const
		{
			if (!_bytes || _bytes->empty() || _width == 0u || _height == 0u) {
				return {};
			}

			u = u - std::floor(u);
			v = v - std::floor(v);
			const auto x = std::min<std::uint32_t>(
				_width - 1u,
				static_cast<std::uint32_t>(std::floor(u * static_cast<float>(_width))));
			const auto y = std::min<std::uint32_t>(
				_height - 1u,
				static_cast<std::uint32_t>(std::floor(v * static_cast<float>(_height))));
			const auto* pixel = _bytes->data() + static_cast<std::size_t>(y) * _rowPitch;
			constexpr float inv255 = 1.0f / 255.0f;
			switch (_format) {
			case rhi::Format::R8_UNorm: {
				const float r = static_cast<float>(pixel[x]) * inv255;
				return CpuTextureSample{ r, r, r, 1.0f };
			}
			case rhi::Format::R16_UNorm:
			case rhi::Format::R16_Typeless: {
				std::uint16_t value = 0u;
				std::memcpy(&value, pixel + static_cast<std::size_t>(x) * 2ull, sizeof(value));
				const float r = static_cast<float>(value) / 65535.0f;
				return CpuTextureSample{ r, r, r, 1.0f };
			}
			case rhi::Format::R16_Float: {
				std::uint16_t value = 0u;
				std::memcpy(&value, pixel + static_cast<std::size_t>(x) * 2ull, sizeof(value));
				const std::uint32_t sign = (value >> 15u) & 1u;
				const std::uint32_t exponent = (value >> 10u) & 0x1fu;
				const std::uint32_t mantissa = value & 0x3ffu;
				float decoded = 0.0f;
				if (exponent == 0u) {
					decoded = std::ldexp(static_cast<float>(mantissa), -24);
				} else if (exponent != 31u) {
					decoded = std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f, static_cast<int>(exponent) - 15);
				}
				const float r = std::clamp(sign ? -decoded : decoded, 0.0f, 1.0f);
				return CpuTextureSample{ r, r, r, 1.0f };
			}
			case rhi::Format::B8G8R8A8_UNorm: {
				const auto* p = pixel + static_cast<std::size_t>(x) * 4ull;
				return CpuTextureSample{
					static_cast<float>(p[2]) * inv255,
					static_cast<float>(p[1]) * inv255,
					static_cast<float>(p[0]) * inv255,
					static_cast<float>(p[3]) * inv255,
				};
			}
			default: {
				const auto* p = pixel + static_cast<std::size_t>(x) * 4ull;
				return CpuTextureSample{
					static_cast<float>(p[0]) * inv255,
					static_cast<float>(p[1]) * inv255,
					static_cast<float>(p[2]) * inv255,
					static_cast<float>(p[3]) * inv255,
				};
			}
			}
		}

	private:
		std::shared_ptr<TextureSourceData> _source;
		TextureSourceData::BytesPtr _bytes;
		TextureSourceData::BytesPtr _ownedBytes;
		std::uint32_t _width{ 0 };
		std::uint32_t _height{ 0 };
		std::uint32_t _rowPitch{ 0 };
		rhi::Format _format{ rhi::Format::Unknown };
	};
}
