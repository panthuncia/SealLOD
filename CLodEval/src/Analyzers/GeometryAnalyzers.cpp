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
#include "Core/TriangleGrid.h"

namespace clodeval {
namespace {
	using DirectX::XMFLOAT3;

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
