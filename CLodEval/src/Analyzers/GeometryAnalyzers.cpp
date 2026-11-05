// Checks a hierarchy against its own finest triangles: is a simplified group as close to the surface as its
// declared error says, and is each voxel cell actually near the surface? Everything comes from the cache, so
// no source asset is needed.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <atomic>
#include <limits>
#include <thread>
#include <vector>

#include <DirectXMath.h>

#include "Analyzers/Analyzers.h"
#include "Core/Analyzer.h"

namespace clodeval {
namespace {
	using DirectX::XMFLOAT3;

	struct Vec {
		double x = 0.0, y = 0.0, z = 0.0;
	};
	Vec ToVec(const XMFLOAT3& value) { return { value.x, value.y, value.z }; }
	Vec operator-(const Vec& a, const Vec& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	Vec operator+(const Vec& a, const Vec& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
	Vec operator*(const Vec& a, double s) { return { a.x * s, a.y * s, a.z * s }; }
	double Dot(const Vec& a, const Vec& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

	// Closest point on a triangle (Ericson, Real-Time Collision Detection 5.1.5).
	double SquaredDistanceToTriangle(const Vec& p, const Vec& a, const Vec& b, const Vec& c)
	{
		const Vec ab = b - a, ac = c - a, ap = p - a;
		const double d1 = Dot(ab, ap), d2 = Dot(ac, ap);
		const auto squared = [&](const Vec& q) { const Vec d = p - q; return Dot(d, d); };
		if (d1 <= 0.0 && d2 <= 0.0) {
			return squared(a);
		}
		const Vec bp = p - b;
		const double d3 = Dot(ab, bp), d4 = Dot(ac, bp);
		if (d3 >= 0.0 && d4 <= d3) {
			return squared(b);
		}
		const double vc = d1 * d4 - d3 * d2;
		if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
			return squared(a + ab * (d1 / (d1 - d3)));
		}
		const Vec cp = p - c;
		const double d5 = Dot(ab, cp), d6 = Dot(ac, cp);
		if (d6 >= 0.0 && d5 <= d6) {
			return squared(c);
		}
		const double vb = d5 * d2 - d1 * d6;
		if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
			return squared(a + ac * (d2 / (d2 - d6)));
		}
		const double va = d3 * d6 - d5 * d4;
		if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
			return squared(b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6))));
		}
		const double denominator = 1.0 / (va + vb + vc);
		return squared(a + ab * (vb * denominator) + ac * (vc * denominator));
	}

	// A uniform grid over triangles. Nearest-surface queries grow outward in shells of cells and stop once no
	// unvisited cell can hold anything closer.
	class TriangleGrid {
	public:
		explicit TriangleGrid(std::vector<std::array<XMFLOAT3, 3>> triangles)
			: m_triangles(std::move(triangles))
		{
			m_min = { std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max() };
			Vec max{ -m_min.x, -m_min.y, -m_min.z };
			for (const auto& triangle : m_triangles) {
				for (const XMFLOAT3& vertex : triangle) {
					m_min = { std::min<double>(m_min.x, vertex.x), std::min<double>(m_min.y, vertex.y), std::min<double>(m_min.z, vertex.z) };
					max = { std::max<double>(max.x, vertex.x), std::max<double>(max.y, vertex.y), std::max<double>(max.z, vertex.z) };
				}
			}
			const Vec size = max - m_min;
			const double volume = std::max(size.x, 1.0e-6) * std::max(size.y, 1.0e-6) * std::max(size.z, 1.0e-6);
			m_cell = std::max(std::cbrt(volume / std::max<double>(static_cast<double>(m_triangles.size()), 1.0)), 1.0e-6);
			// Thin shapes would otherwise ask for too many cells.
			m_cell = std::max(m_cell, std::max({ size.x, size.y, size.z }) / 512.0);
			for (int axis = 0; axis < 3; ++axis) {
				m_counts[axis] = 1;
			}
			m_counts[0] = static_cast<int>(size.x / m_cell) + 1;
			m_counts[1] = static_cast<int>(size.y / m_cell) + 1;
			m_counts[2] = static_cast<int>(size.z / m_cell) + 1;

			std::vector<uint32_t> counts(CellCount() + 1u, 0u);
			const auto forEachCell = [&](const std::array<XMFLOAT3, 3>& triangle, auto&& visit) {
				Vec low = ToVec(triangle[0]), high = low;
				for (const XMFLOAT3& vertex : triangle) {
					low = { std::min<double>(low.x, vertex.x), std::min<double>(low.y, vertex.y), std::min<double>(low.z, vertex.z) };
					high = { std::max<double>(high.x, vertex.x), std::max<double>(high.y, vertex.y), std::max<double>(high.z, vertex.z) };
				}
				const auto [x0, y0, z0] = CellOf(low);
				const auto [x1, y1, z1] = CellOf(high);
				for (int z = z0; z <= z1; ++z) {
					for (int y = y0; y <= y1; ++y) {
						for (int x = x0; x <= x1; ++x) {
							visit(Index(x, y, z));
						}
					}
				}
			};
			for (const auto& triangle : m_triangles) {
				forEachCell(triangle, [&](size_t cell) { ++counts[cell + 1u]; });
			}
			for (size_t index = 1; index < counts.size(); ++index) {
				counts[index] += counts[index - 1u];
			}
			m_cellStart = counts;
			m_cellTriangles.resize(counts.back());
			for (uint32_t index = 0; index < m_triangles.size(); ++index) {
				forEachCell(m_triangles[index], [&](size_t cell) { m_cellTriangles[counts[cell]++] = index; });
			}
		}

		bool Empty() const { return m_triangles.empty(); }

		double Nearest(const Vec& point) const
		{
			const auto [cx, cy, cz] = CellOf(point);
			double best = std::numeric_limits<double>::max();
			const int maxRing = std::max({ m_counts[0], m_counts[1], m_counts[2] }) + 1;
			for (int ring = 0; ring <= maxRing; ++ring) {
				// Everything in this ring or beyond is at least (ring - 1) cells away, plus whatever the query point
				// sits outside the grid by. Being conservative here only costs an extra ring.
				if (best != std::numeric_limits<double>::max()) {
					const double reach = static_cast<double>(ring - 1) * m_cell;
					if (reach > 0.0 && reach * reach > best) {
						break;
					}
				}
				for (int z = cz - ring; z <= cz + ring; ++z) {
					if (z < 0 || z >= m_counts[2]) {
						continue;
					}
					for (int y = cy - ring; y <= cy + ring; ++y) {
						if (y < 0 || y >= m_counts[1]) {
							continue;
						}
						const bool onShellYZ = std::abs(z - cz) == ring || std::abs(y - cy) == ring;
						const int step = onShellYZ || ring == 0 ? 1 : 2 * ring;
						for (int x = cx - ring; x <= cx + ring; x += step) {
							if (x < 0 || x >= m_counts[0]) {
								continue;
							}
							const size_t cell = Index(x, y, z);
							for (uint32_t entry = m_cellStart[cell]; entry < m_cellStart[cell + 1u]; ++entry) {
								const auto& triangle = m_triangles[m_cellTriangles[entry]];
								best = std::min(best, SquaredDistanceToTriangle(point, ToVec(triangle[0]), ToVec(triangle[1]), ToVec(triangle[2])));
							}
						}
					}
				}
			}
			return std::sqrt(best);
		}

	private:
		size_t CellCount() const { return static_cast<size_t>(m_counts[0]) * m_counts[1] * m_counts[2]; }
		size_t Index(int x, int y, int z) const { return (static_cast<size_t>(z) * m_counts[1] + y) * m_counts[0] + x; }
		std::array<int, 3> CellOf(const Vec& point) const
		{
			const auto clampAxis = [&](double value, double origin, int count) {
				return std::clamp(static_cast<int>(std::floor((value - origin) / m_cell)), 0, count - 1);
			};
			return { clampAxis(point.x, m_min.x, m_counts[0]), clampAxis(point.y, m_min.y, m_counts[1]), clampAxis(point.z, m_min.z, m_counts[2]) };
		}

		std::vector<std::array<XMFLOAT3, 3>> m_triangles;
		Vec m_min;
		double m_cell = 1.0;
		std::array<int, 3> m_counts{ 1, 1, 1 };
		std::vector<uint32_t> m_cellStart;
		std::vector<uint32_t> m_cellTriangles;
	};

	// Runs body(index) for every index on all hardware threads. The body may only write to its own index's data.
	template <typename Body>
	void ParallelFor(size_t count, const Body& body)
	{
		std::atomic<size_t> next{ 0 };
		const auto worker = [&] {
			for (size_t index = next.fetch_add(1u); index < count; index = next.fetch_add(1u)) {
				body(index);
			}
		};
		std::vector<std::jthread> threads;
		for (unsigned thread = 1; thread < std::max(std::thread::hardware_concurrency(), 1u); ++thread) {
			threads.emplace_back(worker);
		}
		worker();
	}

	bool IsVoxel(const ClusterLODGroup& group) { return (group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u; }
	bool HasMeasuredError(const ClusterLODGroup& group) { return std::isfinite(group.bounds.error) && group.bounds.error > 0.0f && group.bounds.error < 1.0e19f; }

	// Terminal triangles only. Assemblies hold no leaves of their own, and the result is then empty.
	std::vector<std::array<XMFLOAT3, 3>> CollectLeafTriangles(const CachedMesh& mesh)
	{
		std::vector<std::array<XMFLOAT3, 3>> triangles;
		const ClusterLODPrebuiltData& prebuilt = mesh.Prebuilt();
		for (uint32_t groupIndex = 0; groupIndex < prebuilt.groups.size(); ++groupIndex) {
			const ClusterLODGroup& group = prebuilt.groups[groupIndex];
			if (IsVoxel(group)) {
				continue;
			}
			for (uint32_t segmentIndex = group.firstSegment; segmentIndex < group.firstSegment + group.segmentCount; ++segmentIndex) {
				const ClusterLODGroupSegment& segment = prebuilt.segments[segmentIndex];
				if (segment.refinedGroup != -1) {
					continue;
				}
				mesh.VisitSegmentMeshlets(groupIndex, segment, [&](const CLodCache::TrianglePage& page, const CLodCache::TriangleMeshlet& meshlet) {
					for (uint32_t triangle = 0; triangle < meshlet.triangleCount; ++triangle) {
						const auto vertices = page.Triangle(meshlet, triangle);
						triangles.push_back({ page.Position(meshlet, vertices[0]), page.Position(meshlet, vertices[1]), page.Position(meshlet, vertices[2]) });
					}
				});
			}
		}
		return triangles;
	}

	class GeometryTruthAnalyzer final : public IAnalyzer {
	public:
		const AnalyzerInfo& Info() const override
		{
			static const AnalyzerInfo info{ "geometry.truth", 2, "Distance of simplified triangles and of voxel cells from the leaf surface, against what each claims" };
			return info;
		}

		void Run(const AnalysisContext& context, MetricSink& sink) const override
		{
			constexpr uint32_t kSamplesPerGroup = 32u;
			constexpr uint64_t kCellsPerMesh = 20000u;
			const CachedMesh& mesh = context.mesh;
			const ClusterLODPrebuiltData& prebuilt = mesh.Prebuilt();
			const TriangleGrid grid(CollectLeafTriangles(mesh));
			if (grid.Empty()) {
				return;
			}

			std::vector<double> distanceOverError;
			std::vector<double> groupDistance;
			uint64_t groupsMeasured = 0;
			uint64_t groupsOverError = 0;
			for (uint32_t groupIndex = 0; groupIndex < prebuilt.groups.size(); ++groupIndex) {
				const ClusterLODGroup& group = prebuilt.groups[groupIndex];
				if (IsVoxel(group) || !HasMeasuredError(group)) {
					continue;
				}
				// One-sided: this finds simplified geometry that moved away from the surface, not surface that lost
				// its geometry. It samples triangle centres because simplification keeps original vertices, which
				// are at distance zero whatever it did between them. Voxel builds rewrite the error of a voxel group's
				// children to a cut boundary, larger than the simplification error, so a ratio there flatters.
				std::vector<Vec> vertices;
				for (uint32_t segmentIndex = group.firstSegment; segmentIndex < group.firstSegment + group.segmentCount; ++segmentIndex) {
					mesh.VisitSegmentMeshlets(groupIndex, prebuilt.segments[segmentIndex], [&](const CLodCache::TrianglePage& page, const CLodCache::TriangleMeshlet& meshlet) {
						for (uint32_t triangle = 0; triangle < meshlet.triangleCount; ++triangle) {
							const auto corners = page.Triangle(meshlet, triangle);
							const Vec sum = ToVec(page.Position(meshlet, corners[0])) + ToVec(page.Position(meshlet, corners[1])) + ToVec(page.Position(meshlet, corners[2]));
							vertices.push_back(sum * (1.0 / 3.0));
						}
					});
				}
				if (vertices.empty()) {
					continue;
				}
				const size_t stride = std::max<size_t>(vertices.size() / kSamplesPerGroup, 1u);
				std::vector<double> distances((vertices.size() + stride - 1u) / stride);
				ParallelFor(distances.size(), [&](size_t index) { distances[index] = grid.Nearest(vertices[index * stride]); });
				const double farthest = *std::max_element(distances.begin(), distances.end());
				++groupsMeasured;
				groupDistance.push_back(farthest);
				const double ratio = farthest / group.bounds.error;
				distanceOverError.push_back(ratio);
				if (ratio > 1.0) {
					++groupsOverError;
				}
			}
			sink.Emit("geometry.groups_measured", static_cast<double>(groupsMeasured), Unit::Count, Better::Neutral, {}, Rollup::Sum);
			sink.Emit("geometry.groups_over_declared_error", static_cast<double>(groupsOverError), Unit::Count, Better::Lower, {}, Rollup::Sum);
			if (!distanceOverError.empty()) {
				EmitDistribution(sink, "geometry.distance_over_error", distanceOverError, Unit::Ratio, Better::Lower);
				EmitDistribution(sink, "geometry.group_distance", groupDistance, Unit::MeshUnits, Better::Lower);
			}

			// An assembly's voxels cover its instanced parts, whose triangles are not among its own leaves, so
			// distances from them would be meaningless. Its cells are not measured here.
			if (mesh.Name().find("CLodAssetAssembly") != std::string::npos) {
				return;
			}

			// Cell centres can sit up to half a diagonal from the surface they cover, about 0.87 widths.
			std::vector<Vec> cellCentres;
			std::vector<double> cellWidths;
			uint64_t occupied = 0;
			for (uint32_t pageIndex = prebuilt.voxelPageBase; pageIndex < prebuilt.voxelPageBase + prebuilt.voxelPageCount; ++pageIndex) {
				CLodCache::VoxelPage page;
				if (CLodCache::VoxelPage::Parse(mesh.Page(pageIndex), page)) {
					for (uint32_t cube = 0; cube < page.CubeCount(); ++cube) {
						const CLodVoxelCubeRecord record = page.Cube(cube);
						occupied += static_cast<uint64_t>(std::popcount(record.occupancyMask));
					}
				}
			}
			const uint64_t cellStride = std::max<uint64_t>(occupied / kCellsPerMesh, 1u);
			uint64_t cellCursor = 0;
			for (uint32_t pageIndex = prebuilt.voxelPageBase; pageIndex < prebuilt.voxelPageBase + prebuilt.voxelPageCount; ++pageIndex) {
				CLodCache::VoxelPage page;
				if (!CLodCache::VoxelPage::Parse(mesh.Page(pageIndex), page)) {
					continue;
				}
				for (uint32_t clusterIndex = 0; clusterIndex < page.ClusterCount(); ++clusterIndex) {
					const CLodVoxelClusterRecord cluster = page.Cluster(clusterIndex);
					const double width = cluster.aabbMinAndVoxelWidth.w;
					if (width <= 0.0) {
						continue;
					}
					const Vec origin{ cluster.aabbMinAndVoxelWidth.x, cluster.aabbMinAndVoxelWidth.y, cluster.aabbMinAndVoxelWidth.z };
					for (uint32_t cubeIndex = cluster.firstCube; cubeIndex < cluster.firstCube + cluster.cubeCount && cubeIndex < page.CubeCount(); ++cubeIndex) {
						const CLodVoxelCubeRecord cube = page.Cube(cubeIndex);
						const double cubeX = cube.cubeCoord & 0x3FFu, cubeY = (cube.cubeCoord >> 10u) & 0x3FFu, cubeZ = (cube.cubeCoord >> 20u) & 0x3FFu;
						for (uint32_t cell = 0; cell < 64u; ++cell) {
							if (((cube.occupancyMask >> cell) & 1u) == 0u || (cellCursor++ % cellStride) != 0u) {
								continue;
							}
							// Local cell index is x + 4y + 16z, as ResolveVoxelMaskTest reads it.
							const Vec centre = origin + Vec{ (cubeX * 4.0 + (cell & 3u) + 0.5), (cubeY * 4.0 + ((cell >> 2u) & 3u) + 0.5), (cubeZ * 4.0 + ((cell >> 4u) & 3u) + 0.5) } * width;
							cellCentres.push_back(centre);
							cellWidths.push_back(width);
						}
					}
				}
			}
			if (!cellCentres.empty()) {
				std::vector<double> cellDistanceOverWidth(cellCentres.size());
				ParallelFor(cellCentres.size(), [&](size_t index) { cellDistanceOverWidth[index] = grid.Nearest(cellCentres[index]) / cellWidths[index]; });
				const auto farCells = std::ranges::count_if(cellDistanceOverWidth, [](double ratio) { return ratio > 1.5; });
				sink.Emit("voxel.truth.cells_sampled", static_cast<double>(cellCentres.size()), Unit::Count, Better::Neutral, {}, Rollup::Sum);
				sink.Emit("voxel.truth.cells_far_from_surface", static_cast<double>(farCells), Unit::Count, Better::Lower, {}, Rollup::Sum);
				EmitDistribution(sink, "voxel.truth.cell_distance_over_width", cellDistanceOverWidth, Unit::Ratio, Better::Lower);
			}
		}
	};

	// The groups on either side of the handover: what triangles a first voxel group replaces, how its cells compare
	// with them, and how evenly the mesh crosses over.
	class TransitionHierarchyAnalyzer final : public IAnalyzer {
	public:
		const AnalyzerInfo& Info() const override
		{
			static const AnalyzerInfo info{ "transition.hierarchy", 1, "First voxel groups and the triangle groups they replace" };
			return info;
		}

		void Run(const AnalysisContext& context, MetricSink& sink) const override
		{
			const CachedMesh& mesh = context.mesh;
			const ClusterLODPrebuiltData& prebuilt = mesh.Prebuilt();

			std::vector<double> boundaries, width, widthOverBoundary, edgeOverWidth, cellsPerTriangle, triangleCountOverLeaf, childDepth, voxelLevelWidthRatio, parentBoundaryRatio;
			uint64_t firstVoxelGroups = 0, assemblyVoxelGroups = 0, mixedChildren = 0;
			double replacedTriangles = 0.0, firstCells = 0.0, firstVoxelBytes = 0.0;
			double assemblyMaxWidth = 0.0;

			const auto groupVoxelStats = [&](uint32_t groupIndex, double& outWidth, double& outCells, double& outBytes) {
				const ClusterLODGroup& group = prebuilt.groups[groupIndex];
				outWidth = 0.0;
				outCells = 0.0;
				outBytes = 0.0;
				std::vector<uint32_t> seenPages;
				for (uint32_t segmentIndex = group.firstSegment; segmentIndex < group.firstSegment + group.segmentCount; ++segmentIndex) {
					const ClusterLODGroupSegment& segment = prebuilt.segments[segmentIndex];
					uint32_t meshPage = 0;
					CLodCache::VoxelPage page;
					if (segment.meshletCount == 0u || !CLodCache::ResolveSegmentMeshPage(prebuilt, groupIndex, segment, meshPage) || !CLodCache::VoxelPage::Parse(mesh.Page(meshPage), page)) {
						continue;
					}
					if (std::ranges::find(seenPages, meshPage) == seenPages.end()) {
						seenPages.push_back(meshPage);
						outBytes += static_cast<double>(mesh.Page(meshPage).size());
					}
					for (uint32_t cluster = segment.firstMeshletInPage; cluster < segment.firstMeshletInPage + segment.meshletCount && cluster < page.ClusterCount(); ++cluster) {
						const CLodVoxelClusterRecord record = page.Cluster(cluster);
						outWidth = std::max<double>(outWidth, record.aabbMinAndVoxelWidth.w);
						for (uint32_t cube = record.firstCube; cube < record.firstCube + record.cubeCount && cube < page.CubeCount(); ++cube) {
							outCells += std::popcount(page.Cube(cube).occupancyMask);
						}
					}
				}
			};

			double leafTrianglesPerGroup = 0.0;
			{
				uint64_t leafTriangles = 0, leafGroups = 0;
				for (uint32_t groupIndex = 0; groupIndex < prebuilt.groups.size(); ++groupIndex) {
					const ClusterLODGroup& group = prebuilt.groups[groupIndex];
					if (IsVoxel(group)) {
						continue;
					}
					bool terminal = false;
					for (uint32_t segmentIndex = group.firstSegment; segmentIndex < group.firstSegment + group.segmentCount; ++segmentIndex) {
						const ClusterLODGroupSegment& segment = prebuilt.segments[segmentIndex];
						if (segment.refinedGroup != -1) {
							continue;
						}
						terminal = true;
						mesh.VisitSegmentMeshlets(groupIndex, segment, [&](const CLodCache::TrianglePage&, const CLodCache::TriangleMeshlet& meshlet) { leafTriangles += meshlet.triangleCount; });
					}
					leafGroups += terminal ? 1u : 0u;
				}
				leafTrianglesPerGroup = leafGroups != 0u ? static_cast<double>(leafTriangles) / static_cast<double>(leafGroups) : 0.0;
			}

			for (uint32_t groupIndex = 0; groupIndex < prebuilt.groups.size(); ++groupIndex) {
				const ClusterLODGroup& group = prebuilt.groups[groupIndex];
				if (!IsVoxel(group)) {
					continue;
				}
				double groupWidth = 0.0, groupCells = 0.0, groupBytes = 0.0;
				groupVoxelStats(groupIndex, groupWidth, groupCells, groupBytes);
				if ((group.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) != 0u) {
					++assemblyVoxelGroups;
					assemblyMaxWidth = std::max(assemblyMaxWidth, groupWidth);
				}

				// One step up the voxel chain: how quickly do cells grow?
				bool hasTriangleChild = false, hasVoxelChild = false;
				double triangles = 0.0, edges = 0.0, edgeCount = 0.0;
				double boundary = 0.0;
				for (uint32_t segmentIndex = group.firstSegment; segmentIndex < group.firstSegment + group.segmentCount; ++segmentIndex) {
					const int32_t refined = prebuilt.segments[segmentIndex].refinedGroup;
					if (refined < 0 || static_cast<size_t>(refined) >= prebuilt.groups.size()) {
						continue;
					}
					const ClusterLODGroup& child = prebuilt.groups[static_cast<size_t>(refined)];
					if (IsVoxel(child)) {
						hasVoxelChild = true;
						double childWidth = 0.0, childCells = 0.0, ignoredBytes = 0.0;
						groupVoxelStats(static_cast<uint32_t>(refined), childWidth, childCells, ignoredBytes);
						if (childWidth > 0.0 && groupWidth > 0.0) {
							voxelLevelWidthRatio.push_back(groupWidth / childWidth);
						}
						continue;
					}
					hasTriangleChild = true;
					if (HasMeasuredError(child)) {
						boundary = std::max<double>(boundary, child.bounds.error);
					}
					childDepth.push_back(child.depth);
					for (uint32_t childSegment = child.firstSegment; childSegment < child.firstSegment + child.segmentCount; ++childSegment) {
						mesh.VisitSegmentMeshlets(static_cast<uint32_t>(refined), prebuilt.segments[childSegment], [&](const CLodCache::TrianglePage& page, const CLodCache::TriangleMeshlet& meshlet) {
							triangles += meshlet.triangleCount;
							for (uint32_t triangle = 0; triangle < meshlet.triangleCount; ++triangle) {
								const auto corners = page.Triangle(meshlet, triangle);
								const Vec a = ToVec(page.Position(meshlet, corners[0])), b = ToVec(page.Position(meshlet, corners[1])), c = ToVec(page.Position(meshlet, corners[2]));
								const Vec ab = b - a, bc = c - b, ca = a - c;
								edges += std::sqrt(Dot(ab, ab)) + std::sqrt(Dot(bc, bc)) + std::sqrt(Dot(ca, ca));
								edgeCount += 3.0;
							}
						});
					}
				}
				if (hasTriangleChild && hasVoxelChild) {
					++mixedChildren;
				}
				if (!hasTriangleChild) {
					continue;
				}
				++firstVoxelGroups;
				if (boundary > 0.0) {
					boundaries.push_back(boundary);
					if (groupWidth > 0.0) {
						widthOverBoundary.push_back(groupWidth / boundary);
					}
				}
				width.push_back(groupWidth);
				if (edgeCount > 0.0 && groupWidth > 0.0) {
					edgeOverWidth.push_back((edges / edgeCount) / groupWidth);
				}
				if (triangles > 0.0) {
					cellsPerTriangle.push_back(groupCells / triangles);
					if (leafTrianglesPerGroup > 0.0) {
						triangleCountOverLeaf.push_back(triangles / leafTrianglesPerGroup);
					}
				}
				replacedTriangles += triangles;
				firstCells += groupCells;
				firstVoxelBytes += groupBytes;
			}

			if (firstVoxelGroups == 0u && assemblyVoxelGroups == 0u) {
				return;
			}
			sink.Emit("transition.first_voxel_groups", static_cast<double>(firstVoxelGroups), Unit::Count, Better::Neutral, {}, Rollup::Sum);
			sink.Emit("transition.groups_with_mixed_children", static_cast<double>(mixedChildren), Unit::Count, Better::Neutral, {}, Rollup::Sum);
			sink.Emit("transition.assembly_voxel_groups", static_cast<double>(assemblyVoxelGroups), Unit::Count, Better::Neutral, {}, Rollup::Sum);
			sink.Emit("transition.replaced_triangles", replacedTriangles, Unit::Count, Better::Neutral, {}, Rollup::Sum);
			sink.Emit("transition.first_level_cells", firstCells, Unit::Count, Better::Neutral, {}, Rollup::Sum);
			sink.Emit("transition.first_level_voxel_bytes", firstVoxelBytes, Unit::Bytes, Better::Lower, {}, Rollup::Sum);
			if (assemblyMaxWidth > 0.0) {
				sink.Emit("transition.assembly_max_voxel_width", assemblyMaxWidth, Unit::MeshUnits, Better::Neutral, {}, Rollup::Max);
			}
			const auto emit = [&](const char* id, std::vector<double>& samples, Unit unit) {
				if (!samples.empty()) {
					EmitDistribution(sink, id, samples, unit, Better::Neutral);
				}
			};
			emit("transition.boundary_error", boundaries, Unit::MeshUnits);
			emit("transition.first_voxel_width", width, Unit::MeshUnits);
			emit("transition.width_over_boundary", widthOverBoundary, Unit::Ratio);
			emit("transition.triangle_edge_over_width", edgeOverWidth, Unit::Ratio);
			emit("transition.cells_per_triangle", cellsPerTriangle, Unit::Ratio);
			emit("transition.child_triangles_over_leaf_group", triangleCountOverLeaf, Unit::Ratio);
			emit("transition.child_depth", childDepth, Unit::Count);
			emit("transition.voxel_level_width_ratio", voxelLevelWidthRatio, Unit::Ratio);
		}
	};
}

std::unique_ptr<IAnalyzer> MakeGeometryTruthAnalyzer() { return std::make_unique<GeometryTruthAnalyzer>(); }
std::unique_ptr<IAnalyzer> MakeTransitionHierarchyAnalyzer() { return std::make_unique<TransitionHierarchyAnalyzer>(); }

}
