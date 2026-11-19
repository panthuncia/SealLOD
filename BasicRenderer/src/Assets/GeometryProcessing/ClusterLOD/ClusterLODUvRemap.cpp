#include <BasicRenderer/Assets/ClusterLODUvRemap.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>

namespace clod_uv_remap {

	namespace {

		uint32_t LoadWord(std::span<const std::byte> block, size_t word)
		{
			uint32_t value = 0u;
			std::memcpy(&value, block.data() + word * 4u, 4u);
			return value;
		}

		float LoadFloat(std::span<const std::byte> block, size_t word)
		{
			float value = 0.0f;
			std::memcpy(&value, block.data() + word * 4u, 4u);
			return value;
		}

		bool Fail(std::string* outError, std::string message)
		{
			if (outError != nullptr)
			{
				*outError = std::move(message);
			}
			return false;
		}

		bool IsGridTriangle(const TriangleRemap& triangle)
		{
			return triangle.regions.empty() && triangle.n >= 2u && triangle.n <= kMaxNodesPerEdge && triangle.uvs.size() == NodeCount(triangle.n);
		}

		bool IsRegionTriangle(const TriangleRemap& triangle)
		{
			return !triangle.regions.empty() && triangle.regions.size() <= kMaxRegions && triangle.n >= 2u && triangle.n <= kMaxNodesPerEdge &&
				(triangle.regions.size() == 1u || triangle.cells.size() == SubTriangleCount(triangle.n));
		}

		bool HalfCells(const TriangleRemap& triangle)
		{
			if (!triangle.halfCells || triangle.regions.size() <= 1u || triangle.regions.size() > kMaxHalfCellRegions)
			{
				return false;
			}
			for (const uint32_t cell : triangle.cells)
			{
				if (!IsHalfCell(cell))
				{
					return false;
				}
			}
			return true;
		}

		uint32_t LoadCell(std::span<const std::byte> block, size_t cells, uint32_t index, bool half)
		{
			if (!half)
			{
				return LoadWord(block, cells + index);
			}
			return ExpandHalfCell((LoadWord(block, cells + index / 2u) >> (16u * (index & 1u))) & 0xFFFFu);
		}

	}

	float CellLineDistance(uint32_t cell, float fx, float fy)
	{
		const float theta = static_cast<float>((cell >> 12u) & 255u) * (6.28318530718f / 256.0f);
		const float d = (static_cast<float>(cell >> 20u) * (2.0f / 4095.0f) - 1.0f) * kCellOffsetRange;
		return (fx - 0.5f) * std::cos(theta) + (fy - 0.5f) * std::sin(theta) - d;
	}

	uint32_t CellRegion(uint32_t cell, float fx, float fy)
	{
		const uint32_t a = cell & 63u;
		const uint32_t b = (cell >> 6u) & 63u;
		return a == b || !(CellLineDistance(cell, fx, fy) > 0.0f) ? a : b;
	}

	std::vector<std::byte> EncodeBlock(std::span<const TriangleRemap> triangles, float uvStep)
	{
		float low[2] = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
		float high[2] = { -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };
		uint32_t words = kHeaderWords + static_cast<uint32_t>(triangles.size());
		bool any = false;
		for (const TriangleRemap& triangle : triangles)
		{
			if (IsRegionTriangle(triangle))
			{
				any = true;
				words += RegionTriangleWords(triangle.n, static_cast<uint32_t>(triangle.regions.size()), HalfCells(triangle));
				continue;
			}
			if (!IsGridTriangle(triangle))
			{
				continue;
			}
			any = true;
			words += TriangleWords(triangle.n);
			for (const auto& uv : triangle.uvs)
			{
				for (int axis = 0; axis < 2; ++axis)
				{
					low[axis] = std::min(low[axis], uv[axis]);
					high[axis] = std::max(high[axis], uv[axis]);
				}
			}
		}
		if (!any || words >= (1u << 24u))
		{
			return {};
		}

		std::vector<uint32_t> out(words, 0u);
		float scale[2];
		for (int axis = 0; axis < 2; ++axis)
		{
			if (!(high[axis] >= low[axis]))
			{
				low[axis] = high[axis] = 0.0f; // region triangles only: no nodes
			}
			scale[axis] = high[axis] > low[axis] ? (high[axis] - low[axis]) / 65535.0f : 0.0f;
		}
		out[0] = kBlockMagic;
		out[1] = (static_cast<uint32_t>(triangles.size()) & 0xFFFFu) | (kVersion << 16u);
		std::memcpy(&out[2], &low[0], 4u);
		std::memcpy(&out[3], &low[1], 4u);
		std::memcpy(&out[4], &scale[0], 4u);
		std::memcpy(&out[5], &scale[1], 4u);
		std::memcpy(&out[6], &uvStep, 4u);
		out[7] = words * 4u;

		uint32_t cursor = kHeaderWords + static_cast<uint32_t>(triangles.size());
		for (size_t t = 0; t < triangles.size(); ++t)
		{
			const TriangleRemap& triangle = triangles[t];
			if (IsRegionTriangle(triangle))
			{
				const uint32_t regions = static_cast<uint32_t>(triangle.regions.size());
				out[kHeaderWords + t] = cursor | (triangle.n << 24u) | kRegionEntryFlag;
				const bool half = HalfCells(triangle);
				out[cursor++] = regions | (half ? kRegionHalfCells : 0u);
				for (const RegionMap& region : triangle.regions)
				{
					const float values[kRegionWords] = { region.origin[0], region.origin[1], region.dUvDb1[0], region.dUvDb1[1], region.dUvDb2[0], region.dUvDb2[1] };
					std::memcpy(&out[cursor], values, sizeof(values));
					cursor += kRegionWords;
				}
				if (regions > 1u)
				{
					if (half)
					{
						for (size_t c = 0; c < triangle.cells.size(); ++c)
						{
							out[cursor + c / 2u] |= PackHalfCell(triangle.cells[c]) << (16u * (c & 1u));
						}
						cursor += CellWords(triangle.n, regions, true);
					}
					else
					{
						for (const uint32_t cell : triangle.cells)
						{
							out[cursor++] = cell;
						}
					}
				}
				continue;
			}
			if (!IsGridTriangle(triangle))
			{
				out[kHeaderWords + t] = 0u;
				continue;
			}
			out[kHeaderWords + t] = cursor | (triangle.n << 24u);
			for (const auto& uv : triangle.uvs)
			{
				uint32_t q[2];
				for (int axis = 0; axis < 2; ++axis)
				{
					const float normalized = scale[axis] > 0.0f ? (uv[axis] - low[axis]) / scale[axis] : 0.0f;
					q[axis] = static_cast<uint32_t>(std::clamp(std::lround(normalized), 0l, 65535l));
				}
				out[cursor++] = q[0] | (q[1] << 16u);
			}
			const uint32_t subTriangles = SubTriangleCount(triangle.n);
			for (uint32_t s = 0; s < subTriangles; ++s)
			{
				if (s < triangle.interpolate.size() && triangle.interpolate[s] != 0u)
				{
					out[cursor + s / 32u] |= 1u << (s % 32u);
				}
			}
			cursor += MaskWords(triangle.n);
			for (uint32_t node = 0; node < NodeCount(triangle.n); ++node)
			{
				const uint32_t code = node < triangle.steps.size() ? (triangle.steps[node] & 0xFu) : 0u;
				out[cursor + node / 8u] |= code << (4u * (node % 8u));
			}
			cursor += StepWords(triangle.n);
		}

		std::vector<std::byte> bytes(out.size() * 4u);
		std::memcpy(bytes.data(), out.data(), bytes.size());
		return bytes;
	}

	uint32_t BlockSize(std::span<const std::byte> block)
	{
		if (block.size() < kHeaderWords * 4u || LoadWord(block, 0) != kBlockMagic)
		{
			return 0u;
		}
		return LoadWord(block, 7);
	}

	bool ValidateBlock(std::span<const std::byte> block, uint32_t triangleCount, std::string* outError)
	{
		if (block.size() < kHeaderWords * 4u)
		{
			return Fail(outError, "UV remap block shorter than its header");
		}
		if (LoadWord(block, 0) != kBlockMagic)
		{
			return Fail(outError, "UV remap block without its magic");
		}
		const uint32_t word1 = LoadWord(block, 1);
		const uint32_t version = word1 >> 16u;
		if (version != kVersion)
		{
			return Fail(outError, std::format("UV remap block version {} (expected {})", version, kVersion));
		}
		if ((word1 & 0xFFFFu) != triangleCount)
		{
			return Fail(outError, std::format("UV remap block for {} triangles on a cluster of {}", word1 & 0xFFFFu, triangleCount));
		}
		const uint32_t size = LoadWord(block, 7);
		if (size > block.size() || (size % 4u) != 0u || size < (kHeaderWords + triangleCount) * 4u)
		{
			return Fail(outError, std::format("UV remap block of {} bytes does not fit in {}", size, block.size()));
		}
		for (uint32_t t = 0; t < triangleCount; ++t)
		{
			const uint32_t entry = LoadWord(block, kHeaderWords + t);
			if ((entry & kRegionEntryFlag) != 0u)
			{
				const uint32_t n = (entry >> 24u) & 0x7Fu;
				const uint64_t first = entry & 0xFFFFFFu;
				if (n < 2u || n > kMaxNodesPerEdge || first < kHeaderWords + triangleCount || (first + 1u) * 4u > size)
				{
					return Fail(outError, std::format("UV remap region triangle {} (n {}, word {}) outside its block", t, n, first));
				}
				const uint32_t word0 = LoadWord(block, static_cast<size_t>(first));
				const uint32_t regions = word0 & 0xFFu;
				const bool half = (word0 & kRegionHalfCells) != 0u;
				if (regions < 1u || regions > kMaxRegions || (word0 & ~(0xFFu | kRegionHalfCells)) != 0u || (half && regions > kMaxHalfCellRegions) ||
					(first + RegionTriangleWords(n, regions, half)) * 4u > size)
				{
					return Fail(outError, std::format("UV remap region triangle {} ({} regions) outside its block", t, regions));
				}
				if (regions > 1u)
				{
					const uint64_t cells = first + 1u + regions * kRegionWords;
					for (uint32_t c = 0; c < SubTriangleCount(n); ++c)
					{
						const uint32_t cell = LoadCell(block, static_cast<size_t>(cells), c, half);
						if ((cell & 63u) >= regions || ((cell >> 6u) & 63u) >= regions)
						{
							return Fail(outError, std::format("UV remap region triangle {} cell {} names a region past {}", t, c, regions));
						}
					}
				}
				continue;
			}
			const uint32_t n = entry >> 24u;
			if (n == 0u)
			{
				continue;
			}
			const uint64_t first = entry & 0xFFFFFFu;
			if (n < 2u || n > kMaxNodesPerEdge || first < kHeaderWords + triangleCount ||
				(first + TriangleWords(n)) * 4u > size)
			{
				return Fail(outError, std::format("UV remap triangle {} (n {}, word {}) outside its block", t, n, first));
			}
		}
		return true;
	}

	Sample Evaluate(std::span<const std::byte> block, uint32_t triangle, float b1, float b2)
	{
		Sample sample;
		const uint32_t triangleCount = LoadWord(block, 1) & 0xFFFFu;
		if (triangle >= triangleCount)
		{
			return sample;
		}
		const uint32_t entry = LoadWord(block, kHeaderWords + triangle);
		const bool regionTriangle = (entry & kRegionEntryFlag) != 0u;
		const uint32_t n = (entry >> 24u) & 0x7Fu;
		if (n < 2u)
		{
			return sample;
		}
		const uint32_t first = entry & 0xFFFFFFu;
		const float uvMin[2] = { LoadFloat(block, 2), LoadFloat(block, 3) };
		const float uvScale[2] = { LoadFloat(block, 4), LoadFloat(block, 5) };
		const float uvStep = LoadFloat(block, 6);
		const auto node = [&](uint32_t index, float* out) {
			const uint32_t word = LoadWord(block, first + index);
			out[0] = uvMin[0] + static_cast<float>(word & 0xFFFFu) * uvScale[0];
			out[1] = uvMin[1] + static_cast<float>(word >> 16u) * uvScale[1];
		};

		// The grid cell, clamped into the triangle (b1 + b2 can exceed 1 by rounding).
		const float span = static_cast<float>(n - 1u);
		const float x = std::clamp(b1, 0.0f, 1.0f) * span;
		const float y = std::clamp(b2, 0.0f, 1.0f) * span;
		int j = std::clamp(static_cast<int>(std::floor(y)), 0, static_cast<int>(n) - 2);
		int i = std::clamp(static_cast<int>(std::floor(x)), 0, static_cast<int>(n) - 2 - j);
		const float fx = x - static_cast<float>(i);
		const float fy = y - static_cast<float>(j);
		const bool upper = i + j <= static_cast<int>(n) - 3 && fx + fy > 1.0f;
		uint32_t a, b, c;
		float wa, wb, wc;
		const uint32_t ui = static_cast<uint32_t>(i), uj = static_cast<uint32_t>(j);
		uint32_t gridA[2], gridB[2], gridC[2]; // the nodes' (i, j)
		if (!upper)
		{
			a = NodeIndex(n, ui, uj);
			b = NodeIndex(n, ui + 1u, uj);
			c = NodeIndex(n, ui, uj + 1u);
			gridA[0] = ui; gridA[1] = uj;
			gridB[0] = ui + 1u; gridB[1] = uj;
			gridC[0] = ui; gridC[1] = uj + 1u;
			wa = 1.0f - fx - fy;
			wb = fx;
			wc = fy;
		}
		else
		{
			a = NodeIndex(n, ui + 1u, uj + 1u);
			b = NodeIndex(n, ui, uj + 1u);
			c = NodeIndex(n, ui + 1u, uj);
			gridA[0] = ui + 1u; gridA[1] = uj + 1u;
			gridB[0] = ui; gridB[1] = uj + 1u;
			gridC[0] = ui + 1u; gridC[1] = uj;
			wa = fx + fy - 1.0f;
			wb = 1.0f - fx;
			wc = 1.0f - fy;
		}
		const uint32_t subTriangle = SubTriangleIndex(n, ui, uj, upper);
		if (regionTriangle)
		{
			// The cell's region, then its exact map (at the unclamped barycentrics: the map continues past the triangle's edges).
			const uint32_t word0 = LoadWord(block, first);
			const uint32_t regions = word0 & 0xFFu;
			const uint32_t region = regions > 1u
				? CellRegion(LoadCell(block, first + 1u + regions * kRegionWords, subTriangle, (word0 & kRegionHalfCells) != 0u), fx, fy)
				: 0u;
			const size_t record = first + 1u + region * kRegionWords;
			for (int axis = 0; axis < 2; ++axis)
			{
				const float origin = LoadFloat(block, record + axis);
				sample.dUvDb1[axis] = LoadFloat(block, record + 2u + axis);
				sample.dUvDb2[axis] = LoadFloat(block, record + 4u + axis);
				sample.uv[axis] = origin + b1 * sample.dUvDb1[axis] + b2 * sample.dUvDb2[axis];
			}
			sample.remapped = true;
			sample.interpolated = true;
			sample.region = true;
			sample.regionIndex = region;
			sample.n = n;
			return sample;
		}
		const bool interpolate = ((LoadWord(block, first + NodeCount(n) + subTriangle / 32u) >> (subTriangle % 32u)) & 1u) != 0u;

		float uva[2], uvb[2], uvc[2];
		node(a, uva);
		node(b, uvb);
		node(c, uvc);
		sample.remapped = true;
		sample.interpolated = interpolate;
		sample.n = n;
		sample.nodes[0] = a;
		sample.nodes[1] = b;
		sample.nodes[2] = c;
		sample.nearestNode = wa >= wb && wa >= wc ? a : (wb >= wc ? b : c);
		if (interpolate)
		{
			for (int axis = 0; axis < 2; ++axis)
			{
				sample.uv[axis] = wa * uva[axis] + wb * uvb[axis] + wc * uvc[axis];
				// Grid steps along +i and +j: lower (b - a, c - a); upper (a - b, a - c).
				const float alongI = upper ? uva[axis] - uvb[axis] : uvb[axis] - uva[axis];
				const float alongJ = upper ? uva[axis] - uvc[axis] : uvc[axis] - uva[axis];
				sample.dUvDb1[axis] = alongI * span;
				sample.dUvDb2[axis] = alongJ * span;
			}
		}
		else
		{
			// From the nearest node along its own chart's grid steps (its step code); an axis without one keeps the node's UV along
			// it, with a footprint of one grid step (uvStep).
			const float* nearest = sample.nearestNode == a ? uva : (sample.nearestNode == b ? uvb : uvc);
			const uint32_t* grid = sample.nearestNode == a ? gridA : (sample.nearestNode == b ? gridB : gridC);
			const uint32_t code = (LoadWord(block, first + NodeCount(n) + MaskWords(n) + sample.nearestNode / 8u) >> (4u * (sample.nearestNode % 8u))) & 0xFu;
			float gradient[2][2] = { { 0.0f, 0.0f }, { 0.0f, 0.0f } }; // per grid step along i, along j
			bool has[2] = { false, false };
			for (int axis = 0; axis < 2; ++axis)
			{
				const uint32_t step = (code >> (2 * axis)) & 3u;
				if (step != kStepNext && step != kStepPrevious)
				{
					continue;
				}
				const int sign = step == kStepNext ? 1 : -1;
				const int ni = static_cast<int>(grid[0]) + (axis == 0 ? sign : 0);
				const int nj = static_cast<int>(grid[1]) + (axis == 1 ? sign : 0);
				if (ni < 0 || nj < 0 || ni + nj > static_cast<int>(n) - 1)
				{
					continue;
				}
				float other[2];
				node(NodeIndex(n, static_cast<uint32_t>(ni), static_cast<uint32_t>(nj)), other);
				gradient[axis][0] = (other[0] - nearest[0]) * static_cast<float>(sign);
				gradient[axis][1] = (other[1] - nearest[1]) * static_cast<float>(sign);
				has[axis] = true;
			}
			const float offset[2] = { x - static_cast<float>(grid[0]), y - static_cast<float>(grid[1]) };
			for (int component = 0; component < 2; ++component)
			{
				sample.uv[component] = nearest[component] + gradient[0][component] * offset[0] + gradient[1][component] * offset[1];
			}
			sample.dUvDb1[0] = has[0] ? gradient[0][0] * span : uvStep * span;
			sample.dUvDb1[1] = has[0] ? gradient[0][1] * span : 0.0f;
			sample.dUvDb2[0] = has[1] ? gradient[1][0] * span : 0.0f;
			sample.dUvDb2[1] = has[1] ? gradient[1][1] * span : uvStep * span;
		}
		return sample;
	}

}
