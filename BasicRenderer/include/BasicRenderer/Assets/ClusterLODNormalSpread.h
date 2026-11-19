#pragma once

// Normal spread blocks (stage 6 step 3, docs/clod-appearance-stage6.md): per coarse CLod cluster, one byte per vertex saying how far
// the original surface's shading normals spread around the cluster's own interpolated normal there, so material evaluation can widen
// the material's roughness by it (Toksvig / LEAN-style filtered normal statistics) and a coarse surface that stands for a bumpy or
// folded one does not shade with a sharper highlight than its leaves.
//
// What a byte means. The builder measures, at area samples of every triangle of a simplified cluster, the squared chord between
// the stored (decoded) coarse normal interpolated there and the original's normal at the side-aware match, |n_c - n_s|^2 =
// 2 (1 - cos theta), which is theta^2 for small angles. Each vertex takes the hat-weighted area mean of its triangles' samples:
// v, the second moment of the source normals about the drawn normal (their spread plus any bias), at the scale of the area the
// coarse vertex stands for. For a distribution of normals, v is the mean squared slope, which is what a Beckmann / GGX alpha^2 of
// an equivalent microfacet lobe holds, so convolving the material's lobe with it adds: alpha'^2 = alpha^2 + v.
// The byte stores sqrt(min(v, 1)) as UNORM8 (v = (byte / 255)^2), which keeps resolution where v is small. The shader decodes the
// three corners, interpolates v with the barycentrics (variances add linearly) and widens by at most the lobe's own variance:
// alpha'^2 = alpha^2 + min(scale v, cap alpha^2) (cap 1 by default: at most twice the variance). One lobe can stand for spread about as
// wide as itself; past that the source's normals are several separate highlights, and widening further only dims the drawn one
// (measured on Bistro, docs/clod-appearance-stage6.md step 3).
//
// A block lives in the cluster's own triangle page, after the triangle stream and any UV remap blocks (so it streams and stays
// resident with the cluster). The page's CLOD_PAGE_ATTRIBUTE_NORMAL_SPREAD bit says some cluster of it has one, and the cluster's
// UV set 0 descriptor holds the block's page-relative byte offset in reserved1 (0: none), so pages without UV sets carry none.
// Layout, little-endian:
//
//   word 0: kBlockMagic | vertexCount << 16 | kVersion << 24
//   then vertexCount bytes, one per meshlet-local vertex, padded with zeros to a word multiple.
//
// Clusters whose every byte would be 0 (leaves, flat or exactly represented surface) store no block.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace clod_normal_spread {

	inline constexpr uint32_t kBlockMagic = 0x534Eu; // "NS"
	inline constexpr uint32_t kVersion = 1u;
	inline constexpr uint32_t kHeaderBytes = 4u;

	inline constexpr uint32_t BlockSize(uint32_t vertexCount) { return kHeaderBytes + ((vertexCount + 3u) & ~3u); }

	// v (the second moment, squared chord) to its stored byte.
	inline uint8_t Encode(double v)
	{
		if (!(v > 0.0))
		{
			return 0u;
		}
		const double root = std::sqrt(std::min(v, 1.0));
		return static_cast<uint8_t>(std::clamp(std::lround(root * 255.0), 0l, 255l));
	}

	inline float Decode(uint8_t byte)
	{
		const float root = static_cast<float>(byte) / 255.0f;
		return root * root;
	}

	// The widened roughness (perceptual, alpha = roughness^2, as material evaluation and the BRDF use it):
	// alpha'^2 = alpha^2 + min(scale v, cap alpha^2). clodNormalSpread.hlsli follows it.
	inline float WidenPerceptualRoughness(float perceptualRoughness, float v, float scale, float cap)
	{
		const float alpha = perceptualRoughness * perceptualRoughness;
		const float added = std::min(std::max(scale * v, 0.0f), std::max(cap, 0.0f) * alpha * alpha);
		const float widened = std::sqrt(std::sqrt(alpha * alpha + added));
		return std::min(std::max(widened, perceptualRoughness), 1.0f);
	}

	// One cluster's block, or empty when every byte is 0.
	inline std::vector<std::byte> EncodeBlock(std::span<const uint8_t> vertexBytes)
	{
		const bool any = std::any_of(vertexBytes.begin(), vertexBytes.end(), [](uint8_t b) { return b != 0u; });
		if (!any || vertexBytes.size() > 255u)
		{
			return {};
		}
		const uint32_t vertexCount = static_cast<uint32_t>(vertexBytes.size());
		std::vector<std::byte> block(BlockSize(vertexCount), std::byte{ 0 });
		const uint32_t header = kBlockMagic | (vertexCount << 16u) | (kVersion << 24u);
		std::memcpy(block.data(), &header, sizeof(header));
		std::memcpy(block.data() + kHeaderBytes, vertexBytes.data(), vertexBytes.size());
		return block;
	}

	// Checks a block's header against its cluster's vertex count and the bytes available from its start.
	inline bool ValidateBlock(std::span<const std::byte> block, uint32_t vertexCount)
	{
		if (block.size() < kHeaderBytes)
		{
			return false;
		}
		uint32_t header = 0u;
		std::memcpy(&header, block.data(), sizeof(header));
		return (header & 0xFFFFu) == kBlockMagic && ((header >> 16u) & 0xFFu) == vertexCount && (header >> 24u) == kVersion &&
			block.size() >= BlockSize(vertexCount);
	}

	// The stored byte of a meshlet-local vertex of a validated block.
	inline uint8_t VertexByte(std::span<const std::byte> block, uint32_t vertex)
	{
		const size_t offset = kHeaderBytes + vertex;
		return offset < block.size() ? static_cast<uint8_t>(block[offset]) : 0u;
	}

}
