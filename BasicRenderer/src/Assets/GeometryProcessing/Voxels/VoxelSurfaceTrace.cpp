#include <BasicRenderer/Assets/VoxelSurfaceTrace.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace CLodCache {
	namespace {
		constexpr int32_t kBlockCells = 4;
		constexpr int32_t kRaysPerAxis = 6;
		constexpr uint32_t kMaxBlocks = 400u;
		constexpr double kWallFraction = 0.9;
		// A ray clipping a corner of the block says little, so only those with a real length through it count.
		constexpr double kMinRayLengthInBlocks = 0.5;
		constexpr uint32_t kMinRays = 4u;

		struct V3 {
			double x = 0.0, y = 0.0, z = 0.0;
		};
		V3 operator-(const V3& a, const V3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
		V3 operator+(const V3& a, const V3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
		V3 operator*(const V3& a, double s) { return { a.x * s, a.y * s, a.z * s }; }
		double Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		V3 Cross(const V3& a, const V3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
		V3 Normalized(const V3& value) { return value * (1.0 / std::sqrt(Dot(value, value))); }
		V3 ToV3(const DirectX::XMFLOAT3& value) { return { value.x, value.y, value.z }; }

		using Key = std::array<int32_t, 3>;

		// The axes, and the diagonals between them where axis-aligned coverage rays never look.
		const V3 kDirections[7]{
			{ 1.0, 0.0, 0.0 },
			{ 0.0, 1.0, 0.0 },
			{ 0.0, 0.0, 1.0 },
			Normalized({ 1.0, 1.0, 1.0 }),
			Normalized({ 1.0, -1.0, 1.0 }),
			Normalized({ 1.0, 1.0, -1.0 }),
			Normalized({ -1.0, 1.0, 1.0 }),
		};

		Key CellOf(const V3& point, const V3& origin, double width)
		{
			const V3 local = (point - origin) * (1.0 / width);
			return { static_cast<int32_t>(std::floor(local.x)), static_cast<int32_t>(std::floor(local.y)), static_cast<int32_t>(std::floor(local.z)) };
		}

		// Moller-Trumbore, for any hit between tMin and tMax.
		bool RayHits(const V3& origin, const V3& direction, double tMin, double tMax, const V3& a, const V3& b, const V3& c)
		{
			const V3 edgeA = b - a, edgeB = c - a;
			const V3 p = Cross(direction, edgeB);
			const double determinant = Dot(edgeA, p);
			if (std::abs(determinant) < 1.0e-20) {
				return false;
			}
			const double inverse = 1.0 / determinant;
			const V3 s = origin - a;
			const double u = Dot(s, p) * inverse;
			if (u < 0.0 || u > 1.0) {
				return false;
			}
			const V3 q = Cross(s, edgeA);
			const double v = Dot(direction, q) * inverse;
			if (v < 0.0 || u + v > 1.0) {
				return false;
			}
			const double t = Dot(edgeB, q) * inverse;
			return t >= tMin && t <= tMax;
		}
	}

	VoxelSurfaceTrace TraceVoxelSurface(
		std::span<const std::span<const DirectX::XMFLOAT3>> triangleLists,
		std::span<const std::array<int32_t, 3>> cells,
		const DirectX::XMFLOAT3& originValue,
		float cellWidth)
	{
		VoxelSurfaceTrace result;
		size_t triangleCount = 0u;
		for (const std::span<const DirectX::XMFLOAT3> list : triangleLists) {
			triangleCount += list.size() / 3u;
		}
		if (triangleCount == 0u || !(cellWidth > 0.0f)) {
			return result;
		}
		const double width = cellWidth;
		const double blockWidth = width * kBlockCells;
		const V3 origin = ToV3(originValue);

		// Which triangles reach which block, as sorted pairs: a build has to come out the same every time, so nothing here
		// may depend on hash order, and a sorted list is also the cheapest thing to look blocks up in.
		struct Entry {
			Key block;
			const DirectX::XMFLOAT3* triangle;
		};
		// Dense geometry has hundreds of triangles a block and a coarse group holds the whole mesh, so triangles are only gathered
		// for the blocks that will be traced. Those are picked by hash, which needs the count first, estimated from triangle centres.
		const auto hashBlock = [](const Key& block) {
			uint32_t hash = static_cast<uint32_t>(block[0]) * 73856093u ^ static_cast<uint32_t>(block[1]) * 19349663u ^ static_cast<uint32_t>(block[2]) * 83492791u;
			hash ^= hash >> 13u;
			return hash * 0x5BD1E995u;
		};
		std::vector<Key> centres;
		centres.reserve(triangleCount);
		for (const std::span<const DirectX::XMFLOAT3> list : triangleLists) {
			for (size_t corner = 0; corner + 2u < list.size(); corner += 3u) {
				const V3 sum = ToV3(list[corner]) + ToV3(list[corner + 1u]) + ToV3(list[corner + 2u]);
				centres.push_back(CellOf(sum * (1.0 / 3.0), origin, blockWidth));
			}
		}
		std::ranges::sort(centres);
		const size_t estimatedBlocks = static_cast<size_t>(std::distance(centres.begin(), std::unique(centres.begin(), centres.end())));
		const uint32_t keepOneIn = static_cast<uint32_t>(std::max<size_t>(estimatedBlocks / kMaxBlocks, 1u));

		std::vector<Entry> entries;
		for (const std::span<const DirectX::XMFLOAT3> list : triangleLists) {
		for (size_t corner = 0; corner + 2u < list.size(); corner += 3u) {
			const DirectX::XMFLOAT3* triangle = list.data() + corner;
			const V3 a = ToV3(triangle[0]), b = ToV3(triangle[1]), c = ToV3(triangle[2]);
			const V3 edgeA = b - a, edgeB = c - a;
			const double longest = std::sqrt(std::max({ Dot(edgeA, edgeA), Dot(edgeB, edgeB), Dot(edgeA - edgeB, edgeA - edgeB) }));
			const int32_t steps = std::clamp<int32_t>(static_cast<int32_t>(std::ceil(longest / (blockWidth * 0.25))), 1, 128);
			Key last{ INT32_MAX, 0, 0 };
			for (int32_t i = 0; i <= steps; ++i) {
				for (int32_t j = 0; j <= steps - i; ++j) {
					const Key block = CellOf(a + edgeA * (static_cast<double>(i) / steps) + edgeB * (static_cast<double>(j) / steps), origin, blockWidth);
					if (block != last) {
						last = block;
						if (keepOneIn == 1u || (hashBlock(block) >> 8u) % keepOneIn == 0u) {
							entries.push_back({ block, triangle });
						}
					}
				}
			}
		}
		}
		// Whether a ray hits does not depend on the order within a block, so ordering by address cannot change a result.
		std::ranges::sort(entries, [](const Entry& lhs, const Entry& rhs) { return lhs.block != rhs.block ? lhs.block < rhs.block : lhs.triangle < rhs.triangle; });
		entries.erase(std::unique(entries.begin(), entries.end(), [](const Entry& lhs, const Entry& rhs) { return lhs.block == rhs.block && lhs.triangle == rhs.triangle; }), entries.end());

		std::vector<Key> occupied(cells.begin(), cells.end());
		std::ranges::sort(occupied);
		const auto isOccupied = [&](const Key& cell) { return std::binary_search(occupied.begin(), occupied.end(), cell); };

		std::vector<size_t> blockStarts;
		for (size_t index = 0; index < entries.size(); ++index) {
			if (index == 0u || entries[index].block != entries[index - 1u].block) {
				blockStarts.push_back(index);
			}
		}

		uint64_t pairs = 0u, wallPairs = 0u, wallsMissed = 0u;
		double sourceSum = 0.0, voxelSum = 0.0, deficitSum = 0.0, wallSource = 0.0, wallDeficit = 0.0;
		for (size_t blockIndex = 0; blockIndex < blockStarts.size(); ++blockIndex) {
			const size_t first = blockStarts[blockIndex];
			const size_t last = blockIndex + 1u < blockStarts.size() ? blockStarts[blockIndex + 1u] : entries.size();
			const Key& block = entries[first].block;
			const V3 boxMin = origin + V3{ block[0] * blockWidth, block[1] * blockWidth, block[2] * blockWidth };
			const V3 centre = boxMin + V3{ blockWidth * 0.5, blockWidth * 0.5, blockWidth * 0.5 };
			const double minAxis[3]{ boxMin.x, boxMin.y, boxMin.z };
			for (const V3& direction : kDirections) {
				const V3 helper = std::abs(direction.y) < 0.9 ? V3{ 0.0, 1.0, 0.0 } : V3{ 1.0, 0.0, 0.0 };
				const V3 axisU = Normalized(Cross(helper, direction));
				const V3 axisV = Cross(direction, axisU);
				const double directionAxis[3]{ direction.x, direction.y, direction.z };
				uint32_t rays = 0u, sourceHits = 0u, voxelHits = 0u;
				for (int32_t row = 0; row < kRaysPerAxis; ++row) {
					for (int32_t column = 0; column < kRaysPerAxis; ++column) {
						// A grid wide enough for any direction. Rays that miss the block are not counted.
						const double spread = blockWidth * 0.8;
						const V3 rayOrigin = centre + axisU * (((column + 0.5) / kRaysPerAxis - 0.5) * 2.0 * spread) + axisV * (((row + 0.5) / kRaysPerAxis - 0.5) * 2.0 * spread);
						const double originAxis[3]{ rayOrigin.x, rayOrigin.y, rayOrigin.z };
						double tEnter = -1.0e30, tExit = 1.0e30;
						bool inside = true;
						for (int axis = 0; axis < 3 && inside; ++axis) {
							if (std::abs(directionAxis[axis]) < 1.0e-12) {
								inside = originAxis[axis] >= minAxis[axis] && originAxis[axis] <= minAxis[axis] + blockWidth;
							}
							else {
								const double t0 = (minAxis[axis] - originAxis[axis]) / directionAxis[axis];
								const double t1 = (minAxis[axis] + blockWidth - originAxis[axis]) / directionAxis[axis];
								tEnter = std::max(tEnter, std::min(t0, t1));
								tExit = std::min(tExit, std::max(t0, t1));
							}
						}
						if (!inside || tExit - tEnter < blockWidth * kMinRayLengthInBlocks) {
							continue;
						}
						++rays;
						for (size_t entry = first; entry < last; ++entry) {
							const DirectX::XMFLOAT3* triangle = entries[entry].triangle;
							if (RayHits(rayOrigin, direction, tEnter, tExit, ToV3(triangle[0]), ToV3(triangle[1]), ToV3(triangle[2]))) {
								++sourceHits;
								break;
							}
						}
						const double step = width * 0.25;
						for (double t = tEnter + step * 0.5; t < tExit && !occupied.empty(); t += step) {
							if (isOccupied(CellOf(rayOrigin + direction * t, origin, width))) {
								++voxelHits;
								break;
							}
						}
					}
				}
				if (rays < kMinRays) {
					continue;
				}
				const double source = static_cast<double>(sourceHits) / rays;
				const double voxel = static_cast<double>(voxelHits) / rays;
				const double deficit = std::max(0.0, source - voxel);
				++pairs;
				sourceSum += source;
				voxelSum += voxel;
				deficitSum += deficit;
				if (source >= kWallFraction) {
					++wallPairs;
					wallSource += source;
					wallDeficit += deficit;
					wallsMissed += voxel < 0.5 * source ? 1u : 0u;
				}
			}
		}
		if (pairs != 0u) {
			result.meanSource = static_cast<float>(sourceSum / static_cast<double>(pairs));
			result.meanVoxel = static_cast<float>(voxelSum / static_cast<double>(pairs));
		}
		if (sourceSum > 0.0) {
			result.deficit = static_cast<float>(deficitSum / sourceSum);
			result.wallShare = static_cast<float>(wallSource / sourceSum);
		}
		if (wallSource > 0.0) {
			result.wallDeficit = static_cast<float>(wallDeficit / wallSource);
			result.wallMissed = static_cast<float>(static_cast<double>(wallsMissed) / static_cast<double>(wallPairs));
		}
		return result;
	}

}
