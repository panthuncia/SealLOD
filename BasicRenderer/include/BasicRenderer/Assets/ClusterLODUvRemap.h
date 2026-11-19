#pragma once

// UV remap blocks (stage 6 step 2, docs/clod-appearance-stage6.md): per coarse CLod cluster whose triangles cross UV charts, a block
// of source UVs on a regular barycentric grid of each damaged triangle, so material evaluation samples the original material
// textures where the original surface had them instead of interpolating the coarse corners' UVs across charts.
//
// A block lives in the cluster's own triangle page, after the triangle stream (so it streams and stays resident with the cluster).
// The page's CLOD_PAGE_ATTRIBUTE_UV_REMAP bit says some cluster of it has one, and the cluster's UV set 0 descriptor holds the
// block's page-relative byte offset in reserved0 (0: none). There is one layout (kVersion), whichever kinds of triangle entry a block
// holds. Little-endian 32-bit words:
//
//   [0] kBlockMagic                    [1] triangleCount | kVersion << 16
//   [2..3] uvMin (float2)              [4..5] uvScale (float2): uv = uvMin + unorm16 * uvScale
//   [6] uvStep (float): a typical UV step between neighbouring grid nodes, the derivative of nearest-node lookups
//   [7] byte size of the block
//   [8 .. 8 + triangleCount) per triangle: firstWord (word offset from the block's start, 24 bits) | n << 24 (7 bits) | kRegionEntryFlag
//                            n = nodes per edge (2..kMaxNodesPerEdge), 0 = not remapped (interpolate the corners);
//                            kRegionEntryFlag (bit 31): a region triangle (below), else a grid triangle
//   A grid triangle, at firstWord: NodeCount(n) node words (u16 | v16 << 16), then MaskWords(n) words of one bit per sub-triangle (1: interpolate
//   its three nodes; 0: they straddle a chart boundary), then StepWords(n) words of a 4-bit step code per node (8 per word): bits 0-1
//   along i, bits 2-3 along j, each 0 none, 1 towards the next node, 2 towards the previous one; the grid neighbour in its own chart
//   that gives the node's local UV gradient. A straddling sub-triangle extrapolates from the node of largest weight along those
//   gradients, so the texture continues up to where the nearest node changes instead of freezing at one texel.
//
// Node (i, j), i + j < n, sits at barycentrics (b1, b2) = (i, j) / (n - 1) of the triangle's stored corners (b0 = 1 - b1 - b2).
// Sub-triangles: the lower (i, j) (i+1, j) (i, j+1) for i + j <= n - 2 and the upper (i+1, j+1) (i, j+1) (i+1, j) for
// i + j <= n - 3. Evaluate() is the reference the shaders (clodUvRemap.hlsli) and CLodEval follow.
//
// A region triangle (the step 2 refinement, chart regions; the builder writes them only with remap layout 2), at firstWord:
//   [0] R, the region count (1..kMaxRegions)
//   R records of kRegionWords floats: origin.uv, dUv/db1, dUv/db2, so the region's map is uv = origin + b1 dUv/db1 + b2 dUv/db2,
//   exact for a planar source chart (each region is the part of the triangle a run of source triangles of one chart and one
//   affine mapping covers, seen along the coarse normal);
//   when R > 1, SubTriangleCount(n) cell words, one per sub-triangle of the n grid (indexed as above): region A (6 bits) | region B
//   (6 bits) << 6 | angle (8 bits) << 12 | offset (12 bits) << 20. A pixel in the cell takes B where the cell's line puts it on the
//   positive side, else A (A == B: one region). The line, in the cell's own grid units (fx, fy) = ((b1, b2) (n - 1)) - (i, j):
//   s = (fx - 1/2) cos(theta) + (fy - 1/2) sin(theta) - d, theta = angle 2 pi / 256, d = (offset / 4095 * 2 - 1) * kCellOffsetRange.
// So a chart boundary is a straight line inside each cell and the texture on either side is the source's own mapping, with no
// straddling sub-triangles and no grid interpolation.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace clod_uv_remap {

	inline constexpr uint32_t kBlockMagic = 0x504D5655u; // UVMP
	// The one block layout (grid and region triangles). 1 and 2 were the step 2 grid-only and refinement layouts, retired with
	// CLodCache schema 84; ValidateBlock rejects them.
	inline constexpr uint32_t kVersion = 3u;
	inline constexpr uint32_t kRegionEntryFlag = 0x80000000u;
	inline constexpr uint32_t kMaxRegions = 64u;
	inline constexpr uint32_t kRegionWords = 6u;
	inline constexpr float kCellOffsetRange = 0.75f; // the line's offset from the cell centre, at most (the cell's half diagonal is 0.707)
	inline constexpr uint32_t kStepNone = 0u, kStepNext = 1u, kStepPrevious = 2u;
	inline constexpr uint32_t kHeaderWords = 8u;
	inline constexpr uint32_t kMaxNodesPerEdge = 64u;

	inline constexpr uint32_t NodeCount(uint32_t n) { return n * (n + 1u) / 2u; }
	inline constexpr uint32_t SubTriangleCount(uint32_t n) { return n >= 2u ? (n - 1u) * (n - 1u) : 0u; }
	inline constexpr uint32_t MaskWords(uint32_t n) { return (SubTriangleCount(n) + 31u) / 32u; }
	inline constexpr uint32_t StepWords(uint32_t n) { return (NodeCount(n) + 7u) / 8u; }
	inline constexpr uint32_t TriangleWords(uint32_t n) { return NodeCount(n) + MaskWords(n) + StepWords(n); }
	inline constexpr uint32_t NodeIndex(uint32_t n, uint32_t i, uint32_t j) { return j * n - (j * (j - (j > 0u ? 1u : 0u))) / 2u + i; }
	// Row j holds 2(n - 1 - j) - 1 sub-triangles: lower ones at even, upper ones at odd positions.
	inline constexpr uint32_t SubTriangleIndex(uint32_t n, uint32_t i, uint32_t j, bool upper) { return j * (2u * n - 2u - j) + 2u * i + (upper ? 1u : 0u); }
	// A region triangle's words: the count, its records, and its cells when it has more than one region.
	// Region triangle word 0: the region count in bits 0-7, kRegionHalfCells when its cells are half words (at most 16 regions): region A
	// (4 bits) | B (4 bits) << 4 | angle / 16 (4 bits) << 8 | offset / 273 (4 bits) << 12, two per word (the even cell in the low half),
	// which ExpandHalfCell turns into the full cell word (the same line exactly).
	inline constexpr uint32_t kRegionHalfCells = 0x100u;
	inline constexpr uint32_t kMaxHalfCellRegions = 16u;
	inline constexpr uint32_t CellWords(uint32_t n, uint32_t regions, bool half) { return regions > 1u ? (half ? (SubTriangleCount(n) + 1u) / 2u : SubTriangleCount(n)) : 0u; }
	inline constexpr uint32_t RegionTriangleWords(uint32_t n, uint32_t regions, bool half = false) { return 1u + regions * kRegionWords + CellWords(n, regions, half); }
	inline constexpr uint32_t EncodeCell(uint32_t a, uint32_t b, uint32_t angle, uint32_t offset)
	{
		return (a & 63u) | ((b & 63u) << 6u) | ((angle & 255u) << 12u) | ((offset & 4095u) << 20u);
	}
	// Whether a full cell word is exactly representable as a half cell, and the two conversions.
	inline constexpr bool IsHalfCell(uint32_t cell)
	{
		return (cell & 63u) < 16u && ((cell >> 6u) & 63u) < 16u && (((cell >> 12u) & 255u) % 16u) == 0u && ((cell >> 20u) % 273u) == 0u;
	}
	inline constexpr uint32_t PackHalfCell(uint32_t cell)
	{
		return (cell & 15u) | (((cell >> 6u) & 15u) << 4u) | ((((cell >> 12u) & 255u) / 16u) << 8u) | (((cell >> 20u) / 273u) << 12u);
	}
	inline constexpr uint32_t ExpandHalfCell(uint32_t half)
	{
		return EncodeCell(half & 15u, (half >> 4u) & 15u, ((half >> 8u) & 15u) * 16u, ((half >> 12u) & 15u) * 273u);
	}
	// The region a cell gives at (fx, fy) of its own grid units (the reference for the shaders' CLodUvRemapCellRegion).
	uint32_t CellRegion(uint32_t cell, float fx, float fy);
	// The cell's signed line distance at (fx, fy) (positive: region B).
	float CellLineDistance(uint32_t cell, float fx, float fy);

	// One region's map, before encoding: uv = origin + b1 dUvDb1 + b2 dUvDb2.
	struct RegionMap
	{
		float origin[2] = {};
		float dUvDb1[2] = {};
		float dUvDb2[2] = {};
	};

	// One triangle's remap as the builder computes it, before quantization.
	struct TriangleRemap
	{
		uint32_t n = 0;                                // nodes per edge; 0 = not remapped
		std::vector<std::array<float, 2>> uvs;         // NodeCount(n), indexed by NodeIndex
		std::vector<uint8_t> interpolate;              // SubTriangleCount(n), indexed by SubTriangleIndex
		std::vector<uint8_t> steps;                    // NodeCount(n) step codes (kStep* along i | along j << 2); empty: none
		// Region triangle (layout 2) when not empty: its maps, and SubTriangleCount(n) cell words (EncodeCell) when more than one.
		// uvs, interpolate and steps are then unused.
		std::vector<RegionMap> regions;
		std::vector<uint32_t> cells;
		bool halfCells = false; // store the cells as half words (every cell IsHalfCell, at most kMaxHalfCellRegions regions)
	};

	// Encodes one cluster's block (triangles in the cluster's stored order). Empty when no triangle is remapped.
	std::vector<std::byte> EncodeBlock(std::span<const TriangleRemap> triangles, float uvStep);

	// Checks a block's structure against the cluster's triangle count and the bytes available from its start.
	bool ValidateBlock(std::span<const std::byte> block, uint32_t triangleCount, std::string* outError = nullptr);

	// The block's size in bytes from its header (0 when it is not a block).
	uint32_t BlockSize(std::span<const std::byte> block);

	struct Sample
	{
		bool remapped = false;      // false: the triangle has no remap; use the interpolated corner UVs
		bool interpolated = false;  // the sub-triangle's nodes were interpolated (else extrapolated from the nearest one)
		float uv[2] = {};
		float dUvDb1[2] = {};       // d(uv) / d(b1) and d(uv) / d(b2), for derivatives and footprints
		float dUvDb2[2] = {};
		uint32_t nodes[3] = {};     // the sub-triangle's node indices, and the one of largest weight
		uint32_t nearestNode = 0;
		uint32_t n = 0;
		bool region = false;        // a region triangle's map (interpolated is then true)
		uint32_t regionIndex = 0;
	};

	// The remapped UV of the cluster's triangle at barycentrics (b1, b2) of its stored corners. block starts at the block's first
	// byte (validated or produced by EncodeBlock).
	Sample Evaluate(std::span<const std::byte> block, uint32_t triangle, float b1, float b2);

}
