#include <BasicRenderer/Assets/VoxelSurfaceTrace.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <vector>

#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_sort.h>
#include <oneapi/tbb/task_arena.h>

namespace CLodCache {
	namespace {
		constexpr int32_t kBlockCells = 4;
		constexpr int32_t kRaysPerAxis = 6;
		constexpr uint32_t kMaxBlocks = 400u;
		constexpr double kWallFraction = 0.9;
		// A ray clipping a corner of the block says little, so only those with a real length through it count.
		constexpr double kMinRayLengthInBlocks = 0.5;
		constexpr uint32_t kMinRays = 4u;
		// Triangles per task when gathering. Every parallel part writes its own slots and is reduced in a fixed order, and the sorts
		// order by a total order, so the result does not depend on the split.
		constexpr size_t kTrianglesPerTask = 8192u;

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

		// Lexicographic, as std::array's operator< (which goes through std::mismatch, slowly).
		bool KeyLess(const Key& lhs, const Key& rhs)
		{
			if (lhs[0] != rhs[0]) {
				return lhs[0] < rhs[0];
			}
			if (lhs[1] != rhs[1]) {
				return lhs[1] < rhs[1];
			}
			return lhs[2] < rhs[2];
		}

		constexpr size_t kDirectionCount = std::size(kDirections);
		constexpr int32_t kRayCount = kRaysPerAxis * kRaysPerAxis;
		static_assert(kRayCount <= 64);

		// One block's rays in one direction: the frame, and each ray's origin and span through the block.
		struct BlockRays {
			V3 axisU, axisV;
			double offsets[kRaysPerAxis]{};  // a ray's offset along axisU (by column) and axisV (by row) from the centre
			V3 rayOrigins[kRayCount];
			double tEnter[kRayCount]{}, tExit[kRayCount]{};
			uint64_t counted = 0u;  // rays with a real length through the block
		};

		void BuildBlockRays(const V3& boxMin, double blockWidth, size_t directionIndex, BlockRays& out)
		{
			const V3 centre = boxMin + V3{ blockWidth * 0.5, blockWidth * 0.5, blockWidth * 0.5 };
			const double minAxis[3]{ boxMin.x, boxMin.y, boxMin.z };
			const V3& direction = kDirections[directionIndex];
			const V3 helper = std::abs(direction.y) < 0.9 ? V3{ 0.0, 1.0, 0.0 } : V3{ 1.0, 0.0, 0.0 };
			out.axisU = Normalized(Cross(helper, direction));
			out.axisV = Cross(direction, out.axisU);
			const double directionAxis[3]{ direction.x, direction.y, direction.z };
			// A grid wide enough for any direction. Rays that miss the block are not counted.
			const double spread = blockWidth * 0.8;
			for (int32_t index = 0; index < kRaysPerAxis; ++index) {
				out.offsets[index] = ((index + 0.5) / kRaysPerAxis - 0.5) * 2.0 * spread;
			}
			out.counted = 0u;
			for (int32_t row = 0; row < kRaysPerAxis; ++row) {
				for (int32_t column = 0; column < kRaysPerAxis; ++column) {
					const int32_t ray = row * kRaysPerAxis + column;
					const V3 rayOrigin = centre + out.axisU * (((column + 0.5) / kRaysPerAxis - 0.5) * 2.0 * spread) + out.axisV * (((row + 0.5) / kRaysPerAxis - 0.5) * 2.0 * spread);
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
					out.rayOrigins[ray] = rayOrigin;
					out.tEnter[ray] = tEnter;
					out.tExit[ray] = tExit;
					if (inside && !(tExit - tEnter < blockWidth * kMinRayLengthInBlocks)) {
						out.counted |= uint64_t{ 1u } << ray;
					}
				}
			}
		}

		uint32_t PopCount(uint64_t value)
		{
			uint32_t count = 0u;
			for (; value != 0u; value &= value - 1u) {
				++count;
			}
			return count;
		}
	}

	VoxelSurfaceSource PrepareVoxelSurfaceSource(
		std::span<const std::span<const DirectX::XMFLOAT3>> triangleLists,
		const DirectX::XMFLOAT3& originValue,
		float cellWidth)
	{
		VoxelSurfaceSource source;
		source.origin = originValue;
		source.cellWidth = cellWidth;
		size_t triangleCount = 0u;
		for (const std::span<const DirectX::XMFLOAT3> list : triangleLists) {
			triangleCount += list.size() / 3u;
		}
		if (triangleCount == 0u || !(cellWidth > 0.0f)) {
			return source;
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
		// Every triangle by its list and its first corner there, so ranges of them can run as tasks.
		std::vector<const DirectX::XMFLOAT3*> triangles;
		triangles.reserve(triangleCount);
		for (const std::span<const DirectX::XMFLOAT3> list : triangleLists) {
			for (size_t corner = 0; corner + 2u < list.size(); corner += 3u) {
				triangles.push_back(list.data() + corner);
			}
		}
		triangleCount = triangles.size();
		const size_t tasks = (triangleCount + kTrianglesPerTask - 1u) / kTrianglesPerTask;
		// Isolated: a waiting thread takes only these tasks.
		const auto parallelTasks = [](size_t count, const auto& body) {
			oneapi::tbb::this_task_arena::isolate([&] {
				oneapi::tbb::parallel_for(size_t(0), count, [&](size_t task) { body(task); });
			});
		};
		std::vector<Key> centres(triangleCount);
		parallelTasks(tasks, [&](size_t task) {
			const size_t end = std::min(triangleCount, (task + 1u) * kTrianglesPerTask);
			for (size_t t = task * kTrianglesPerTask; t < end; ++t) {
				const DirectX::XMFLOAT3* triangle = triangles[t];
				const V3 sum = ToV3(triangle[0]) + ToV3(triangle[1]) + ToV3(triangle[2]);
				centres[t] = CellOf(sum * (1.0 / 3.0), origin, blockWidth);
			}
		});
		oneapi::tbb::parallel_sort(centres.begin(), centres.end(), KeyLess);
		const size_t estimatedBlocks = static_cast<size_t>(std::distance(centres.begin(), std::unique(centres.begin(), centres.end())));
		const uint32_t keepOneIn = static_cast<uint32_t>(std::max<size_t>(estimatedBlocks / kMaxBlocks, 1u));
		std::vector<Key>().swap(centres);

		std::vector<std::vector<Entry>> taskEntries(tasks);
		parallelTasks(tasks, [&](size_t task) {
		std::vector<Entry>& entries = taskEntries[task];
		const size_t end = std::min(triangleCount, (task + 1u) * kTrianglesPerTask);
		for (size_t t = task * kTrianglesPerTask; t < end; ++t) {
			const DirectX::XMFLOAT3* triangle = triangles[t];
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
		});
		size_t entryCount = 0u;
		for (const std::vector<Entry>& part : taskEntries) {
			entryCount += part.size();
		}
		std::vector<Entry> entries;
		entries.reserve(entryCount);
		for (std::vector<Entry>& part : taskEntries) {
			entries.insert(entries.end(), part.begin(), part.end());
			std::vector<Entry>().swap(part);
		}
		// Whether a ray hits does not depend on the order within a block, so ordering by address cannot change a result.
		oneapi::tbb::parallel_sort(entries.begin(), entries.end(),
			[](const Entry& lhs, const Entry& rhs) { return lhs.block != rhs.block ? KeyLess(lhs.block, rhs.block) : lhs.triangle < rhs.triangle; });
		entries.erase(std::unique(entries.begin(), entries.end(), [](const Entry& lhs, const Entry& rhs) { return lhs.block == rhs.block && lhs.triangle == rhs.triangle; }), entries.end());

		std::vector<size_t> blockStarts;
		for (size_t index = 0; index < entries.size(); ++index) {
			if (index == 0u || entries[index].block != entries[index - 1u].block) {
				blockStarts.push_back(index);
			}
		}
		source.blocks.resize(blockStarts.size());
		source.rays.assign(blockStarts.size() * kDirectionCount, 0u);
		source.sourceHits.assign(blockStarts.size() * kDirectionCount, 0u);
		source.rayMasks.assign(blockStarts.size() * kDirectionCount, 0u);

		// Each block as a task. A ray is tested only against the triangles whose extent across the ray grid reaches it, with a
		// margin far above the rounding of the projection and of the hit test, so every triangle the test could accept is tested
		// and the counts are the ones testing every triangle would give. A triangle the test sees too nearly edge-on to bound
		// that way is tested against every ray; one it rejects for every ray (a determinant under its threshold) is skipped.
		parallelTasks(blockStarts.size(), [&](size_t blockIndex) {
			const size_t first = blockStarts[blockIndex];
			const size_t last = blockIndex + 1u < blockStarts.size() ? blockStarts[blockIndex + 1u] : entries.size();
			const Key& block = entries[first].block;
			source.blocks[blockIndex] = block;
			const V3 boxMin = origin + V3{ block[0] * blockWidth, block[1] * blockWidth, block[2] * blockWidth };
			const V3 centre = boxMin + V3{ blockWidth * 0.5, blockWidth * 0.5, blockWidth * 0.5 };
			const double scale = std::max({ std::abs(centre.x), std::abs(centre.y), std::abs(centre.z) });
			BlockRays frame;
			for (size_t directionIndex = 0; directionIndex < kDirectionCount; ++directionIndex) {
				const V3& direction = kDirections[directionIndex];
				BuildBlockRays(boxMin, blockWidth, directionIndex, frame);
				uint64_t hit = 0u;
				for (size_t entry = first; entry < last && (frame.counted & ~hit) != 0u; ++entry) {
					const DirectX::XMFLOAT3* triangle = entries[entry].triangle;
					const V3 a = ToV3(triangle[0]), b = ToV3(triangle[1]), c = ToV3(triangle[2]);
					// RayHits' determinant, which does not depend on the ray.
					const V3 edgeA = b - a, edgeB = c - a;
					const double determinant = Dot(edgeA, Cross(direction, edgeB));
					if (std::abs(determinant) < 1.0e-20) {
						continue;
					}
					const double lengthA = std::sqrt(Dot(edgeA, edgeA)), lengthB = std::sqrt(Dot(edgeB, edgeB));
					uint64_t candidates = frame.counted & ~hit;
					if (std::abs(determinant) >= 1.0e-4 * lengthA * lengthB) {
						const V3 pa = a - centre, pb = b - centre, pc = c - centre;
						const double ua = Dot(pa, frame.axisU), ub = Dot(pb, frame.axisU), uc = Dot(pc, frame.axisU);
						const double va = Dot(pa, frame.axisV), vb = Dot(pb, frame.axisV), vc = Dot(pc, frame.axisV);
						const double margin = 1.0e-6 * (blockWidth + lengthA + lengthB) + 1.0e-9 * scale;
						const double uMin = std::min(std::min(ua, ub), uc) - margin, uMax = std::max(std::max(ua, ub), uc) + margin;
						const double vMin = std::min(std::min(va, vb), vc) - margin, vMax = std::max(std::max(va, vb), vc) + margin;
						uint64_t columns = 0u, rows = 0u;
						for (int32_t index = 0; index < kRaysPerAxis; ++index) {
							if (frame.offsets[index] >= uMin && frame.offsets[index] <= uMax) {
								columns |= uint64_t{ 1u } << index;
							}
							if (frame.offsets[index] >= vMin && frame.offsets[index] <= vMax) {
								rows |= uint64_t{ 1u } << index;
							}
						}
						uint64_t reach = 0u;
						for (int32_t row = 0; row < kRaysPerAxis; ++row) {
							if ((rows >> row) & 1u) {
								reach |= columns << (row * kRaysPerAxis);
							}
						}
						candidates &= reach;
					}
					for (; candidates != 0u; candidates &= candidates - 1u) {
						const int32_t ray = static_cast<int32_t>(std::countr_zero(candidates));
						if (RayHits(frame.rayOrigins[ray], direction, frame.tEnter[ray], frame.tExit[ray], a, b, c)) {
							hit |= uint64_t{ 1u } << ray;
						}
					}
				}
				const size_t slot = blockIndex * kDirectionCount + directionIndex;
				source.rays[slot] = static_cast<uint8_t>(PopCount(frame.counted));
				source.sourceHits[slot] = static_cast<uint8_t>(PopCount(frame.counted & hit));
				source.rayMasks[slot] = frame.counted;
			}
		});
		return source;
	}

	VoxelSurfaceTrace TraceVoxelSurface(const VoxelSurfaceSource& source, std::span<const std::array<int32_t, 3>> cells)
	{
		VoxelSurfaceTrace result;
		if (source.blocks.empty()) {
			return result;
		}
		const double width = source.cellWidth;
		const double blockWidth = width * kBlockCells;
		const V3 origin = ToV3(source.origin);

		std::vector<Key> occupied(cells.begin(), cells.end());
		std::sort(occupied.begin(), occupied.end(), KeyLess);
		const auto isOccupied = [&](const Key& cell) { return std::binary_search(occupied.begin(), occupied.end(), cell, KeyLess); };

		// The cells' side per block and direction; the sums are taken afterwards in block and direction order.
		std::vector<uint8_t> voxelHits(source.rays.size(), 0u);
		if (!occupied.empty()) {
			oneapi::tbb::this_task_arena::isolate([&] {
				oneapi::tbb::parallel_for(size_t(0), source.blocks.size(), [&](size_t blockIndex) {
					const Key& block = source.blocks[blockIndex];
					const V3 boxMin = origin + V3{ block[0] * blockWidth, block[1] * blockWidth, block[2] * blockWidth };
					BlockRays frame;
					for (size_t directionIndex = 0; directionIndex < kDirectionCount; ++directionIndex) {
						const size_t slot = blockIndex * kDirectionCount + directionIndex;
						if (source.rays[slot] < kMinRays) {
							continue;
						}
						const V3& direction = kDirections[directionIndex];
						BuildBlockRays(boxMin, blockWidth, directionIndex, frame);
						uint32_t hits = 0u;
						for (uint64_t rays = frame.counted; rays != 0u; rays &= rays - 1u) {
							const int32_t ray = static_cast<int32_t>(std::countr_zero(rays));
							const double step = width * 0.25;
							for (double t = frame.tEnter[ray] + step * 0.5; t < frame.tExit[ray]; t += step) {
								if (isOccupied(CellOf(frame.rayOrigins[ray] + direction * t, origin, width))) {
									++hits;
									break;
								}
							}
						}
						voxelHits[slot] = static_cast<uint8_t>(hits);
					}
				});
			});
		}

		uint64_t pairs = 0u, wallPairs = 0u, wallsMissed = 0u;
		double sourceSum = 0.0, voxelSum = 0.0, deficitSum = 0.0, wallSource = 0.0, wallDeficit = 0.0;
		for (size_t slot = 0; slot < source.rays.size(); ++slot) {
			const uint32_t rays = source.rays[slot];
			if (rays < kMinRays) {
				continue;
			}
			const double sourceShare = static_cast<double>(source.sourceHits[slot]) / rays;
			const double voxel = static_cast<double>(voxelHits[slot]) / rays;
			const double deficit = std::max(0.0, sourceShare - voxel);
			++pairs;
			sourceSum += sourceShare;
			voxelSum += voxel;
			deficitSum += deficit;
			if (sourceShare >= kWallFraction) {
				++wallPairs;
				wallSource += sourceShare;
				wallDeficit += deficit;
				wallsMissed += voxel < 0.5 * sourceShare ? 1u : 0u;
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

	VoxelSurfaceTrace TraceVoxelSurface(
		std::span<const std::span<const DirectX::XMFLOAT3>> triangleLists,
		std::span<const std::array<int32_t, 3>> cells,
		const DirectX::XMFLOAT3& originValue,
		float cellWidth)
	{
		return TraceVoxelSurface(PrepareVoxelSurfaceSource(triangleLists, originValue, cellWidth), cells);
	}

}
