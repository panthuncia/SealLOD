#pragma once

// Base colour textures on the CPU, for measuring what CLod simplification does to texture appearance: 8-bit sRGB mip chains
// box-filtered in linear light, wrapping trilinear sampling, the mip level for a footprint, and the CIE76 colour difference.
// Shared by the generator (the stage 6 colour-weighted texture term, docs/clod-appearance-stage6.md) and CLodEval
// (quality.texture), so both measure colour the same way. No decoding: importers decode (the builder never depends on one).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace clod_texture {

	struct Texture
	{
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<std::vector<uint8_t>> mips; // RGBA8, sRGB colour and linear alpha, level 0 first, down to 1x1

		uint32_t MipWidth(uint32_t level) const { return std::max(width >> level, 1u); }
		uint32_t MipHeight(uint32_t level) const { return std::max(height >> level, 1u); }
		uint32_t Levels() const { return static_cast<uint32_t>(mips.size()); }
	};

	// Appends the box-filtered mip chain (linear light, alpha linear, as GPU sRGB mip generation does) below mips[0] down to 1x1.
	// Odd sizes clamp the last texel.
	void BuildMips(Texture& texture);

	// Linear RGBA at (u, v) (wrapping, as glTF's default sampler), trilinear between mip levels at lod (0: full resolution).
	std::array<float, 4> SampleLinear(const Texture& texture, double u, double v, double lod);

	// The level of detail at which a footprint of `footprintUv` UV units (the size of a pixel or texel, in UV units) is one texel.
	double TextureLod(const Texture& texture, double footprintUv);

	// CIE L*a*b* (D65) of linear sRGB, and the CIE76 difference of two colours given as linear RGB.
	std::array<double, 3> LinearToLab(const std::array<float, 4>& linear);
	double DeltaE(const std::array<float, 4>& a, const std::array<float, 4>& b);

	// FNV-1a over bytes, continuing from seed: content hashes for cache keys.
	inline constexpr uint64_t kHashSeed = 1469598103934665603ull;
	inline uint64_t HashBytes(const void* data, size_t size, uint64_t seed = kHashSeed)
	{
		const auto* bytes = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < size; ++i)
		{
			seed ^= bytes[i];
			seed *= 1099511628211ull;
		}
		return seed;
	}

}
