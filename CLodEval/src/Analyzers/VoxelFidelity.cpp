#include "Analyzers/VoxelFidelity.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <map>
#include <thread>
#include <unordered_map>

#include <DirectXMath.h>

#include <BasicRenderer/Assets/VoxelSurfaceTrace.h>

#include "Analyzers/Analyzers.h"
#include "Core/Analyzer.h"

namespace clodeval {
namespace {
	struct V3 {
		double x = 0.0, y = 0.0, z = 0.0;
	};
	V3 operator-(const V3& a, const V3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	V3 operator+(const V3& a, const V3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
	V3 operator*(const V3& a, double s) { return { a.x * s, a.y * s, a.z * s }; }
	double Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	V3 Cross(const V3& a, const V3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
	V3 ToV3(const DirectX::XMFLOAT3& value) { return { value.x, value.y, value.z }; }
	V3 Normalized(const V3& value)
	{
		const double length = std::sqrt(Dot(value, value));
		return length > 0.0 ? value * (1.0 / length) : V3{ 0.0, 0.0, 1.0 };
	}

	bool IsVoxel(const ClusterLODGroup& group) { return (group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u; }

	using Triangle = std::array<V3, 3>;

	// Directions a surface might be seen from: the axes, where the coverage rays are aimed, and between them, where they are not.
	const std::array<V3, 7> kDirections{
		V3{ 1.0, 0.0, 0.0 },
		V3{ 0.0, 1.0, 0.0 },
		V3{ 0.0, 0.0, 1.0 },
		Normalized(V3{ 1.0, 1.0, 1.0 }),
		Normalized(V3{ 1.0, -1.0, 1.0 }),
		Normalized(V3{ 1.0, 1.0, -1.0 }),
		Normalized(V3{ -1.0, 1.0, 1.0 }),
	};

	struct Cell {
		int32_t x = 0, y = 0, z = 0;
		bool operator==(const Cell&) const = default;
	};
	struct CellHash {
		size_t operator()(const Cell& cell) const
		{
			return (static_cast<size_t>(static_cast<uint32_t>(cell.x)) * 73856093u) ^ (static_cast<size_t>(static_cast<uint32_t>(cell.y)) * 19349663u) ^ (static_cast<size_t>(static_cast<uint32_t>(cell.z)) * 83492791u);
		}
	};

	// The sum of area * n n^T, whose largest eigenvalue over its trace says how much normals agree, whichever side they face.
	struct NormalMoment {
		double xx = 0.0, xy = 0.0, xz = 0.0, yy = 0.0, yz = 0.0, zz = 0.0;
		double area = 0.0;
		void Add(const V3& n, double weight)
		{
			xx += n.x * n.x * weight;
			xy += n.x * n.y * weight;
			xz += n.x * n.z * weight;
			yy += n.y * n.y * weight;
			yz += n.y * n.z * weight;
			zz += n.z * n.z * weight;
			area += weight;
		}
		void Add(const NormalMoment& other)
		{
			xx += other.xx;
			xy += other.xy;
			xz += other.xz;
			yy += other.yy;
			yz += other.yz;
			zz += other.zz;
			area += other.area;
		}
		double Coherence() const
		{
			if (area <= 0.0) {
				return 0.0;
			}
			// Power iteration is plenty for a 3x3 symmetric matrix that is only compared to a threshold.
			V3 v = Normalized(V3{ 1.0, 0.7, 0.3 });
			double eigen = 0.0;
			for (int iteration = 0; iteration < 12; ++iteration) {
				const V3 next{ xx * v.x + xy * v.y + xz * v.z, xy * v.x + yy * v.y + yz * v.z, xz * v.x + yz * v.y + zz * v.z };
				eigen = std::sqrt(Dot(next, next));
				if (eigen <= 0.0) {
					return 0.0;
				}
				v = next * (1.0 / eigen);
			}
			return eigen / area;
		}
	};

	struct Mask {
		int32_t width = 0, height = 0;
		std::vector<uint8_t> bits;
		void Reset(int32_t w, int32_t h)
		{
			width = w;
			height = h;
			bits.assign(static_cast<size_t>(w) * h, 0u);
		}
	};

	struct P2 {
		double x = 0.0, y = 0.0;
	};

	// Fills a convex polygon, sampling at pixel centres. Thin triangles do miss pixels, as they would on screen.
	void FillConvex(const P2* points, int count, uint8_t bit, Mask& mask)
	{
		double minY = points[0].y, maxY = points[0].y;
		for (int index = 1; index < count; ++index) {
			minY = std::min(minY, points[index].y);
			maxY = std::max(maxY, points[index].y);
		}
		const int32_t firstRow = std::max<int32_t>(static_cast<int32_t>(std::ceil(minY - 0.5)), 0);
		const int32_t lastRow = std::min<int32_t>(static_cast<int32_t>(std::floor(maxY - 0.5)), mask.height - 1);
		for (int32_t row = firstRow; row <= lastRow; ++row) {
			const double y = row + 0.5;
			double left = std::numeric_limits<double>::max(), right = std::numeric_limits<double>::lowest();
			for (int index = 0; index < count; ++index) {
				const P2& a = points[index];
				const P2& b = points[(index + 1) % count];
				if ((a.y <= y && b.y > y) || (b.y <= y && a.y > y)) {
					const double x = a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y);
					left = std::min(left, x);
					right = std::max(right, x);
				}
			}
			if (left > right) {
				continue;
			}
			const int32_t first = std::max<int32_t>(static_cast<int32_t>(std::ceil(left - 0.5)), 0);
			const int32_t last = std::min<int32_t>(static_cast<int32_t>(std::floor(right - 0.5)), mask.width - 1);
			uint8_t* line = mask.bits.data() + static_cast<size_t>(row) * mask.width;
			for (int32_t column = first; column <= last; ++column) {
				line[column] |= bit;
			}
		}
	}

	// Gift-wrapped outline of a projected cube.
	int ConvexHull(std::array<P2, 8>& points, std::array<P2, 8>& hull)
	{
		std::sort(points.begin(), points.end(), [](const P2& a, const P2& b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
		std::array<P2, 16> chain;
		int size = 0;
		const auto turn = [](const P2& o, const P2& a, const P2& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
		for (int index = 0; index < 8; ++index) {
			while (size >= 2 && turn(chain[size - 2], chain[size - 1], points[index]) <= 0.0) {
				--size;
			}
			chain[size++] = points[index];
		}
		const int lower = size + 1;
		for (int index = 6; index >= 0; --index) {
			while (size >= lower && turn(chain[size - 2], chain[size - 1], points[index]) <= 0.0) {
				--size;
			}
			chain[size++] = points[index];
		}
		const int count = std::min(size - 1, 8);
		for (int index = 0; index < count; ++index) {
			hull[index] = chain[index];
		}
		return count;
	}

	struct GroupCells {
		std::vector<Cell> cells;
		V3 origin;
		double width = 0.0;
	};

	GroupCells ReadGroupCells(const CachedMesh& mesh, uint32_t groupIndex)
	{
		GroupCells out;
		const ClusterLODPrebuiltData& prebuilt = mesh.Prebuilt();
		const ClusterLODGroup& group = prebuilt.groups[groupIndex];
		bool hasOrigin = false;
		for (uint32_t segmentIndex = group.firstSegment; segmentIndex < group.firstSegment + group.segmentCount; ++segmentIndex) {
			const ClusterLODGroupSegment& segment = prebuilt.segments[segmentIndex];
			uint32_t meshPage = 0;
			CLodCache::VoxelPage page;
			if (segment.meshletCount == 0u || !CLodCache::ResolveSegmentMeshPage(prebuilt, groupIndex, segment, meshPage) || !CLodCache::VoxelPage::Parse(mesh.Page(meshPage), page)) {
				continue;
			}
			for (uint32_t clusterIndex = segment.firstMeshletInPage; clusterIndex < segment.firstMeshletInPage + segment.meshletCount && clusterIndex < page.ClusterCount(); ++clusterIndex) {
				const CLodVoxelClusterRecord cluster = page.Cluster(clusterIndex);
				const double width = cluster.aabbMinAndVoxelWidth.w;
				if (width <= 0.0) {
					continue;
				}
				const V3 clusterOrigin{ cluster.aabbMinAndVoxelWidth.x, cluster.aabbMinAndVoxelWidth.y, cluster.aabbMinAndVoxelWidth.z };
				if (!hasOrigin) {
					out.origin = clusterOrigin;
					out.width = width;
					hasOrigin = true;
				}
				// Clusters of a group share a grid, but each keeps its own origin, so cells are put on the first one's.
				const V3 offset = (clusterOrigin - out.origin) * (1.0 / out.width);
				const int32_t offsetX = static_cast<int32_t>(std::lround(offset.x)), offsetY = static_cast<int32_t>(std::lround(offset.y)), offsetZ = static_cast<int32_t>(std::lround(offset.z));
				for (uint32_t cubeIndex = cluster.firstCube; cubeIndex < cluster.firstCube + cluster.cubeCount && cubeIndex < page.CubeCount(); ++cubeIndex) {
					const CLodVoxelCubeRecord cube = page.Cube(cubeIndex);
					const int32_t cubeX = static_cast<int32_t>(cube.cubeCoord & 0x3FFu) * 4, cubeY = static_cast<int32_t>((cube.cubeCoord >> 10u) & 0x3FFu) * 4, cubeZ = static_cast<int32_t>((cube.cubeCoord >> 20u) & 0x3FFu) * 4;
					for (uint32_t cell = 0; cell < 64u; ++cell) {
						if (((cube.occupancyMask >> cell) & 1u) != 0u) {
							out.cells.push_back({ offsetX + cubeX + static_cast<int32_t>(cell & 3u), offsetY + cubeY + static_cast<int32_t>((cell >> 2u) & 3u), offsetZ + cubeZ + static_cast<int32_t>((cell >> 4u) & 3u) });
						}
					}
				}
			}
		}
		return out;
	}

	VoxelGroupFidelity MeasureGroup(const CachedMesh& mesh, uint32_t groupIndex, const std::vector<int32_t>& levels, const std::vector<std::vector<uint32_t>>& children, const std::vector<std::vector<Triangle>>& leaves)
	{
		const ClusterLODPrebuiltData& prebuilt = mesh.Prebuilt();
		const ClusterLODGroup& group = prebuilt.groups[groupIndex];
		VoxelGroupFidelity out;
		out.group = groupIndex;
		out.depth = group.depth;
		out.voxelLevel = levels[groupIndex];
		out.leaveError = group.bounds.error;
		float enter = std::numeric_limits<float>::max();
		for (const uint32_t child : children[groupIndex]) {
			enter = std::min(enter, prebuilt.groups[child].bounds.error);
		}
		out.enterError = children[groupIndex].empty() ? 0.0f : enter;

		const GroupCells cells = ReadGroupCells(mesh, groupIndex);
		out.cells = static_cast<uint32_t>(cells.cells.size());
		out.width = static_cast<float>(cells.width);
		if (cells.cells.empty()) {
			return out;
		}

		// The triangles this group stands for: every leaf below it.
		std::vector<uint8_t> visited(prebuilt.groups.size(), 0u);
		std::vector<uint32_t> stack{ groupIndex };
		std::vector<const std::vector<Triangle>*> sources;
		while (!stack.empty()) {
			const uint32_t current = stack.back();
			stack.pop_back();
			if (visited[current] != 0u) {
				continue;
			}
			visited[current] = 1u;
			if (!leaves[current].empty()) {
				sources.push_back(&leaves[current]);
				out.sourceTriangles += leaves[current].size();
			}
			for (const uint32_t child : children[current]) {
				stack.push_back(child);
			}
		}
		if (sources.empty()) {
			return out;
		}

		// Source surface per cell, sampled at half a cell so that a large triangle reaches every cell it crosses.
		std::unordered_map<Cell, NormalMoment, CellHash> touched;
		touched.reserve(cells.cells.size() * 3u);
		const double width = cells.width;
		const auto cellOf = [&](const V3& point) {
			const V3 local = (point - cells.origin) * (1.0 / width);
			return Cell{ static_cast<int32_t>(std::floor(local.x)), static_cast<int32_t>(std::floor(local.y)), static_cast<int32_t>(std::floor(local.z)) };
		};
		V3 boundsMin{ 1.0e30, 1.0e30, 1.0e30 }, boundsMax{ -1.0e30, -1.0e30, -1.0e30 };
		for (const std::vector<Triangle>* source : sources) {
			for (const Triangle& triangle : *source) {
				for (const V3& corner : triangle) {
					boundsMin = { std::min(boundsMin.x, corner.x), std::min(boundsMin.y, corner.y), std::min(boundsMin.z, corner.z) };
					boundsMax = { std::max(boundsMax.x, corner.x), std::max(boundsMax.y, corner.y), std::max(boundsMax.z, corner.z) };
				}
				const V3 edgeA = triangle[1] - triangle[0], edgeB = triangle[2] - triangle[0];
				const V3 cross = Cross(edgeA, edgeB);
				const double area = 0.5 * std::sqrt(Dot(cross, cross));
				if (area <= 0.0) {
					continue;
				}
				const V3 normal = cross * (0.5 / area);
				const double longest = std::sqrt(std::max({ Dot(edgeA, edgeA), Dot(edgeB, edgeB), Dot(edgeA - edgeB, edgeA - edgeB) }));
				const int32_t steps = std::clamp<int32_t>(static_cast<int32_t>(std::ceil(longest / (width * 0.5))), 1, 256);
				const double sampleWeight = area / (static_cast<double>(steps) * (steps + 1) * 0.5);
				for (int32_t a = 0; a < steps; ++a) {
					for (int32_t b = 0; b < steps - a; ++b) {
						const double u = (a + 1.0 / 3.0) / steps, v = (b + 1.0 / 3.0) / steps;
						touched[cellOf(triangle[0] + edgeA * u + edgeB * v)].Add(normal, sampleWeight);
					}
				}
			}
		}
		out.touchedCells = static_cast<uint32_t>(touched.size());
		out.keptFraction = touched.empty() ? 0.0f : static_cast<float>(cells.cells.size()) / static_cast<float>(touched.size());

		double totalArea = 0.0, cellCoherence = 0.0, neighbourhoodCoherence = 0.0, neighbourhoodFill = 0.0, neighbourhoodWeight = 0.0;
		const size_t stride = std::max<size_t>(touched.size() / 4000u, 1u);
		size_t cursor = 0;
		for (const auto& [cell, moment] : touched) {
			totalArea += moment.area;
			cellCoherence += moment.Coherence() * moment.area;
			if ((cursor++ % stride) != 0u) {
				continue;
			}
			NormalMoment around;
			uint32_t touchedAround = 0;
			for (int32_t z = -2; z <= 2; ++z) {
				for (int32_t y = -2; y <= 2; ++y) {
					for (int32_t x = -2; x <= 2; ++x) {
						if (const auto found = touched.find({ cell.x + x, cell.y + y, cell.z + z }); found != touched.end()) {
							around.Add(found->second);
							++touchedAround;
						}
					}
				}
			}
			neighbourhoodCoherence += around.Coherence() * moment.area;
			neighbourhoodFill += std::min(touchedAround / 25.0, 1.0) * moment.area;
			neighbourhoodWeight += moment.area;
		}
		out.areaPerCell = static_cast<float>(totalArea / (static_cast<double>(touched.size()) * width * width));
		out.cellCoherence = static_cast<float>(cellCoherence / std::max(totalArea, 1.0e-30));
		out.neighbourhoodCoherence = static_cast<float>(neighbourhoodCoherence / std::max(neighbourhoodWeight, 1.0e-30));
		out.neighbourhoodFill = static_cast<float>(neighbourhoodFill / std::max(neighbourhoodWeight, 1.0e-30));

		{
			std::vector<DirectX::XMFLOAT3> positions;
			positions.reserve(static_cast<size_t>(out.sourceTriangles) * 3u);
			for (const std::vector<Triangle>* source : sources) {
				for (const Triangle& triangle : *source) {
					for (const V3& corner : triangle) {
						positions.push_back({ static_cast<float>(corner.x), static_cast<float>(corner.y), static_cast<float>(corner.z) });
					}
				}
			}
			std::vector<std::array<int32_t, 3>> keys;
			keys.reserve(cells.cells.size());
			for (const Cell& cell : cells.cells) {
				keys.push_back({ cell.x, cell.y, cell.z });
			}
			const DirectX::XMFLOAT3 origin{ static_cast<float>(cells.origin.x), static_cast<float>(cells.origin.y), static_cast<float>(cells.origin.z) };
			const std::span<const DirectX::XMFLOAT3> list(positions);
			const CLodCache::VoxelSurfaceTrace trace = CLodCache::TraceVoxelSurface({ &list, 1u }, keys, origin, static_cast<float>(width));
			out.blockSource = trace.meanSource;
			out.blockVoxel = trace.meanVoxel;
			out.blockDeficit = trace.deficit;
			out.wallDeficit = trace.wallDeficit;
			out.wallShare = trace.wallShare;
			out.wallMissed = trace.wallMissed;
		}

		// Project both along each direction, a quarter of a cell to the pixel, so thin geometry is measured by its area.
		const V3 extent = boundsMax - boundsMin;
		const double diagonal = std::sqrt(Dot(extent, extent)) + width * 4.0;
		constexpr int32_t kMaxResolution = 4096;
		const double pixel = std::max(width * 0.25, diagonal / kMaxResolution);
		const int32_t resolution = std::clamp<int32_t>(static_cast<int32_t>(std::ceil(diagonal / pixel)), 8, kMaxResolution);
		const V3 middle = (boundsMin + boundsMax) * 0.5;
		Mask mask;
		double holeSum = 0.0, ratioSum = 0.0, excessSum = 0.0;
		double deficitSum[3]{}, solidDeficitSum = 0.0, solidShareSum = 0.0;
		for (const V3& direction : kDirections) {
			const V3 helper = std::abs(direction.y) < 0.9 ? V3{ 0.0, 1.0, 0.0 } : V3{ 1.0, 0.0, 0.0 };
			const V3 axisU = Normalized(Cross(helper, direction));
			const V3 axisV = Cross(direction, axisU);
			const auto project = [&](const V3& point) {
				const V3 local = point - middle;
				return P2{ Dot(local, axisU) / pixel + resolution * 0.5, Dot(local, axisV) / pixel + resolution * 0.5 };
			};
			mask.Reset(resolution, resolution);
			for (const std::vector<Triangle>* source : sources) {
				for (const Triangle& triangle : *source) {
					const std::array<P2, 3> points{ project(triangle[0]), project(triangle[1]), project(triangle[2]) };
					FillConvex(points.data(), 3, 1u, mask);
				}
			}
			for (const Cell& cell : cells.cells) {
				std::array<P2, 8> corners;
				for (int corner = 0; corner < 8; ++corner) {
					corners[corner] = project(cells.origin + V3{ (cell.x + (corner & 1)) * width, (cell.y + ((corner >> 1) & 1)) * width, (cell.z + ((corner >> 2) & 1)) * width });
				}
				std::array<P2, 8> hull;
				const int count = ConvexHull(corners, hull);
				FillConvex(hull.data(), count, 2u, mask);
			}
			uint64_t source = 0, voxel = 0, hole = 0, extra = 0;
			for (const uint8_t bits : mask.bits) {
				source += (bits & 1u) != 0u;
				voxel += (bits & 2u) != 0u;
				hole += bits == 1u;
				extra += bits == 2u;
			}
			if (source != 0u) {
				const double holeFraction = static_cast<double>(hole) / static_cast<double>(source);
				holeSum += holeFraction;
				ratioSum += static_cast<double>(voxel) / static_cast<double>(source);
				excessSum += static_cast<double>(extra) / static_cast<double>(source);
				out.worstHoles = std::max(out.worstHoles, static_cast<float>(holeFraction));
				for (int scale = 0; scale < 3; ++scale) {
					const int32_t cellsWide = 2 << scale;
					const int32_t tile = std::max<int32_t>(static_cast<int32_t>(std::lround(cellsWide * width / pixel)), 2);
					const int32_t tiles = (resolution + tile - 1) / tile;
					std::vector<uint32_t> sourceCount(static_cast<size_t>(tiles) * tiles, 0u), voxelCount(static_cast<size_t>(tiles) * tiles, 0u);
					for (int32_t y = 0; y < resolution; ++y) {
						const uint8_t* line = mask.bits.data() + static_cast<size_t>(y) * resolution;
						const size_t tileRow = static_cast<size_t>(y / tile) * tiles;
						for (int32_t x = 0; x < resolution; ++x) {
							sourceCount[tileRow + x / tile] += line[x] & 1u;
							voxelCount[tileRow + x / tile] += (line[x] >> 1u) & 1u;
						}
					}
					uint64_t lost = 0u, solidLost = 0u, solidSource = 0u;
					const uint32_t full = static_cast<uint32_t>(tile) * tile;
					for (size_t index = 0; index < sourceCount.size(); ++index) {
						const uint32_t deficit = sourceCount[index] > voxelCount[index] ? sourceCount[index] - voxelCount[index] : 0u;
						lost += deficit;
						if (scale == 1 && sourceCount[index] >= full * 9u / 10u) {
							solidLost += deficit;
							solidSource += sourceCount[index];
						}
					}
					deficitSum[scale] += static_cast<double>(lost) / static_cast<double>(source);
					if (scale == 1) {
						solidDeficitSum += solidSource != 0u ? static_cast<double>(solidLost) / static_cast<double>(solidSource) : 0.0;
						solidShareSum += static_cast<double>(solidSource) / static_cast<double>(source);
					}
				}
			}
		}
		const double directions = static_cast<double>(kDirections.size());
		out.holes = static_cast<float>(holeSum / directions);
		out.coverageRatio = static_cast<float>(ratioSum / directions);
		out.excess = static_cast<float>(excessSum / directions);
		out.deficit2 = static_cast<float>(deficitSum[0] / directions);
		out.deficit4 = static_cast<float>(deficitSum[1] / directions);
		out.deficit8 = static_cast<float>(deficitSum[2] / directions);
		out.solidDeficit4 = static_cast<float>(solidDeficitSum / directions);
		out.solidShare4 = static_cast<float>(solidShareSum / directions);
		return out;
	}

	class VoxelFidelityAnalyzer final : public IAnalyzer {
	public:
		const AnalyzerInfo& Info() const override
		{
			static const AnalyzerInfo info{ "voxel.fidelity", 1, "How much of the source surface each voxel group actually covers, and how surface-like that source is at the group's cell size" };
			return info;
		}

		void Run(const AnalysisContext& context, MetricSink& sink) const override
		{
			const std::vector<VoxelGroupFidelity> groups = MeasureVoxelFidelity(context.mesh);
			std::map<std::string, std::vector<double>> firstLevel;
			for (const VoxelGroupFidelity& group : groups) {
				if (group.voxelLevel == 0 && group.cells != 0u) {
					firstLevel["voxel.fidelity.first_holes"].push_back(group.holes);
					firstLevel["voxel.fidelity.first_coverage_ratio"].push_back(group.coverageRatio);
					firstLevel["voxel.fidelity.first_kept_fraction"].push_back(group.keptFraction);
				}
			}
			for (auto& [id, samples] : firstLevel) {
				EmitDistribution(sink, id, samples, Unit::Ratio, Better::Neutral);
			}
		}
	};
}

std::vector<VoxelGroupFidelity> MeasureVoxelFidelity(const CachedMesh& mesh)
{
	const ClusterLODPrebuiltData& prebuilt = mesh.Prebuilt();
	const size_t groupCount = prebuilt.groups.size();
	std::vector<std::vector<uint32_t>> children(groupCount);
	std::vector<std::vector<Triangle>> leaves(groupCount);
	for (uint32_t groupIndex = 0; groupIndex < groupCount; ++groupIndex) {
		const ClusterLODGroup& group = prebuilt.groups[groupIndex];
		for (uint32_t segmentIndex = group.firstSegment; segmentIndex < group.firstSegment + group.segmentCount; ++segmentIndex) {
			const ClusterLODGroupSegment& segment = prebuilt.segments[segmentIndex];
			if (segment.refinedGroup >= 0 && static_cast<size_t>(segment.refinedGroup) < groupCount) {
				const auto child = static_cast<uint32_t>(segment.refinedGroup);
				if (std::ranges::find(children[groupIndex], child) == children[groupIndex].end()) {
					children[groupIndex].push_back(child);
				}
			}
			else if (!IsVoxel(group)) {
				mesh.VisitSegmentMeshlets(groupIndex, segment, [&](const CLodCache::TrianglePage& page, const CLodCache::TriangleMeshlet& meshlet) {
					for (uint32_t triangle = 0; triangle < meshlet.triangleCount; ++triangle) {
						const auto vertices = page.Triangle(meshlet, triangle);
						leaves[groupIndex].push_back({ ToV3(page.Position(meshlet, vertices[0])), ToV3(page.Position(meshlet, vertices[1])), ToV3(page.Position(meshlet, vertices[2])) });
					}
				});
			}
		}
	}

	// Steps above triangles. Children have lower depth, so one pass in depth order settles it.
	std::vector<uint32_t> order;
	for (uint32_t groupIndex = 0; groupIndex < groupCount; ++groupIndex) {
		if (IsVoxel(prebuilt.groups[groupIndex])) {
			order.push_back(groupIndex);
		}
	}
	std::ranges::sort(order, {}, [&](uint32_t groupIndex) { return prebuilt.groups[groupIndex].depth; });
	std::vector<int32_t> levels(groupCount, -1);
	for (const uint32_t groupIndex : order) {
		int32_t level = 0;
		for (const uint32_t child : children[groupIndex]) {
			if (IsVoxel(prebuilt.groups[child])) {
				level = std::max(level, levels[child] + 1);
			}
		}
		levels[groupIndex] = level;
	}

	std::vector<VoxelGroupFidelity> results(order.size());
	std::atomic<size_t> next{ 0u };
	std::vector<std::thread> workers;
	const uint32_t workerCount = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
	for (uint32_t worker = 0; worker < workerCount; ++worker) {
		workers.emplace_back([&]() {
			for (size_t index = next.fetch_add(1u); index < order.size(); index = next.fetch_add(1u)) {
				results[index] = MeasureGroup(mesh, order[index], levels, children, leaves);
			}
		});
	}
	for (std::thread& worker : workers) {
		worker.join();
	}
	return results;
}

std::vector<VoxelSegmentCells> ReadVoxelSegmentCells(const CachedMesh& mesh, uint32_t groupIndex)
{
	std::vector<VoxelSegmentCells> out;
	const ClusterLODPrebuiltData& prebuilt = mesh.Prebuilt();
	const ClusterLODGroup& group = prebuilt.groups[groupIndex];
	for (uint32_t segmentIndex = group.firstSegment; segmentIndex < group.firstSegment + group.segmentCount; ++segmentIndex) {
		const ClusterLODGroupSegment& segment = prebuilt.segments[segmentIndex];
		VoxelSegmentCells cells;
		cells.refinedGroup = segment.refinedGroup;
		uint32_t meshPage = 0;
		CLodCache::VoxelPage page;
		if (segment.meshletCount != 0u && CLodCache::ResolveSegmentMeshPage(prebuilt, groupIndex, segment, meshPage) && CLodCache::VoxelPage::Parse(mesh.Page(meshPage), page)) {
			for (uint32_t clusterIndex = segment.firstMeshletInPage; clusterIndex < segment.firstMeshletInPage + segment.meshletCount && clusterIndex < page.ClusterCount(); ++clusterIndex) {
				const CLodVoxelClusterRecord cluster = page.Cluster(clusterIndex);
				const float width = cluster.aabbMinAndVoxelWidth.w;
				cells.width = width;
				for (uint32_t cubeIndex = cluster.firstCube; cubeIndex < cluster.firstCube + cluster.cubeCount && cubeIndex < page.CubeCount(); ++cubeIndex) {
					const CLodVoxelCubeRecord cube = page.Cube(cubeIndex);
					const uint32_t cubeX = (cube.cubeCoord & 0x3FFu) * 4u, cubeY = ((cube.cubeCoord >> 10u) & 0x3FFu) * 4u, cubeZ = ((cube.cubeCoord >> 20u) & 0x3FFu) * 4u;
					for (uint32_t cell = 0; cell < 64u; ++cell) {
						if (((cube.occupancyMask >> cell) & 1u) != 0u) {
							cells.cellMins.push_back({ cluster.aabbMinAndVoxelWidth.x + (cubeX + (cell & 3u)) * width, cluster.aabbMinAndVoxelWidth.y + (cubeY + ((cell >> 2u) & 3u)) * width, cluster.aabbMinAndVoxelWidth.z + (cubeZ + ((cell >> 4u) & 3u)) * width });
						}
					}
				}
			}
		}
		out.push_back(std::move(cells));
	}
	return out;
}

std::unique_ptr<IAnalyzer> MakeVoxelFidelityAnalyzer() { return std::make_unique<VoxelFidelityAnalyzer>(); }

}
