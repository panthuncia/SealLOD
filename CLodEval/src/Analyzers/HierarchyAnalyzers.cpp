#include "Analyzers/Analyzers.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

namespace clodeval {

namespace {

	bool IsVoxel(const ClusterLODGroup& group) { return (group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u; }

	// Terminal and root groups carry a sentinel error, which is not a measurement.
	bool HasMeasuredError(const ClusterLODGroup& group) { return std::isfinite(group.bounds.error) && group.bounds.error < 1.0e19f; }

	class HierarchyStructureAnalyzer final : public IAnalyzer {
	public:
		const AnalyzerInfo& Info() const override
		{
			static const AnalyzerInfo info{ "hierarchy.structure", 1, "LOD levels, triangle reduction per level and cluster fill" };
			return info;
		}

		void Run(const AnalysisContext& context, MetricSink& sink) const override
		{
			const ClusterLODPrebuiltData& prebuilt = context.mesh.Prebuilt();
			struct DepthTotals {
				uint64_t triangles = 0;
				uint64_t clusters = 0;
				uint64_t groups = 0;
			};
			std::map<int32_t, DepthTotals> depths;
			uint64_t leafTriangles = 0;
			uint64_t triangleClusters = 0;
			uint64_t triangleGroups = 0;
			uint64_t voxelGroups = 0;
			std::vector<double> clusterTriangles;
			std::vector<double> clusterVertices;
			std::vector<double> clustersPerGroup;

			for (uint32_t groupIndex = 0; groupIndex < prebuilt.groups.size(); ++groupIndex) {
				const ClusterLODGroup& group = prebuilt.groups[groupIndex];
				if (IsVoxel(group)) {
					++voxelGroups;
					continue;
				}
				++triangleGroups;
				DepthTotals& totals = depths[group.depth];
				++totals.groups;
				uint64_t groupClusters = 0;
				for (uint32_t segmentIndex = group.firstSegment; segmentIndex < group.firstSegment + group.segmentCount; ++segmentIndex) {
					const ClusterLODGroupSegment& segment = prebuilt.segments[segmentIndex];
					context.mesh.VisitSegmentMeshlets(groupIndex, segment, [&](const CLodCache::TrianglePage&, const CLodCache::TriangleMeshlet& meshlet) {
						totals.triangles += meshlet.triangleCount;
						++totals.clusters;
						++groupClusters;
						++triangleClusters;
						clusterTriangles.push_back(meshlet.triangleCount);
						clusterVertices.push_back(meshlet.vertexCount);
						if (segment.refinedGroup == -1) {
							leafTriangles += meshlet.triangleCount;
						}
					});
				}
				clustersPerGroup.push_back(static_cast<double>(groupClusters));
			}

			sink.Emit("hierarchy.groups", static_cast<double>(prebuilt.groups.size()), Unit::Count, Better::Neutral, {}, Rollup::Sum);
			sink.Emit("hierarchy.triangle_groups", static_cast<double>(triangleGroups), Unit::Count, Better::Neutral, {}, Rollup::Sum);
			sink.Emit("hierarchy.voxel_groups", static_cast<double>(voxelGroups), Unit::Count, Better::Neutral, {}, Rollup::Sum);
			sink.Emit("hierarchy.segments", static_cast<double>(prebuilt.segments.size()), Unit::Count, Better::Neutral, {}, Rollup::Sum);
			sink.Emit("hierarchy.nodes", static_cast<double>(prebuilt.nodes.size()), Unit::Count, Better::Neutral, {}, Rollup::Sum);
			sink.Emit("hierarchy.max_depth", static_cast<double>(prebuilt.maxDepth), Unit::Count, Better::Neutral, {}, Rollup::Max);
			sink.Emit("hierarchy.max_traversal_depth", static_cast<double>(prebuilt.maxTraversalDepth), Unit::Count, Better::Neutral, {}, Rollup::Max);
			sink.Emit("hierarchy.leaf_triangles", static_cast<double>(leafTriangles), Unit::Count, Better::Neutral, {}, Rollup::Sum);
			sink.Emit("hierarchy.triangle_clusters", static_cast<double>(triangleClusters), Unit::Count, Better::Neutral, {}, Rollup::Sum);

			// A level that barely shrinks costs memory and traversal without giving a useful coarser LOD.
			std::vector<double> reductionRatios;
			const DepthTotals* previous = nullptr;
			for (const auto& [depth, totals] : depths) {
				const Tags tags{ { "depth", std::to_string(depth) } };
				sink.Emit("hierarchy.depth.triangles", static_cast<double>(totals.triangles), Unit::Count, Better::Neutral, tags);
				sink.Emit("hierarchy.depth.clusters", static_cast<double>(totals.clusters), Unit::Count, Better::Neutral, tags);
				sink.Emit("hierarchy.depth.groups", static_cast<double>(totals.groups), Unit::Count, Better::Neutral, tags);
				if (previous != nullptr && previous->triangles != 0u) {
					reductionRatios.push_back(static_cast<double>(totals.triangles) / static_cast<double>(previous->triangles));
				}
				previous = &totals;
			}
			EmitDistribution(sink, "hierarchy.level_triangle_ratio", reductionRatios, Unit::Ratio, Better::Lower);
			// With voxel groups the coarse end of the hierarchy is not triangles, so there is nothing to report.
			if (!depths.empty() && voxelGroups == 0u) {
				sink.Emit("hierarchy.coarsest_triangles", static_cast<double>(depths.rbegin()->second.triangles), Unit::Count, Better::Lower, {}, Rollup::Sum);
			}
			EmitDistribution(sink, "hierarchy.cluster_triangles", clusterTriangles, Unit::Count, Better::Higher);
			EmitDistribution(sink, "hierarchy.cluster_vertices", clusterVertices, Unit::Count, Better::Neutral);
			EmitDistribution(sink, "hierarchy.clusters_per_group", clustersPerGroup, Unit::Count, Better::Neutral);
		}
	};

	// Everything counted here should be zero. A nonzero count means the numbers from other analyzers
	// and any rendering of this cache are suspect, so it is worth failing a budget on.
	class HierarchyValidityAnalyzer final : public IAnalyzer {
	public:
		const AnalyzerInfo& Info() const override
		{
			static const AnalyzerInfo info{ "hierarchy.validity", 2, "Consistency of error, bounds, traversal nodes and pages" };
			return info;
		}

		void Run(const AnalysisContext& context, MetricSink& sink) const override
		{
			const ClusterLODPrebuiltData& prebuilt = context.mesh.Prebuilt();
			const auto emitProblem = [&](const char* id, uint64_t count) {
				sink.Emit(id, static_cast<double>(count), Unit::Count, Better::Lower, {}, Rollup::Sum);
			};

			uint64_t nonFiniteError = 0;
			uint64_t badRefinedGroup = 0;
			uint64_t errorNotMonotonic = 0;
			uint64_t childSphereEscapes = 0;
			uint64_t childSphereEscapesAcrossKinds = 0;
			double maxSphereEscape = 0.0;
			uint64_t badSegmentPage = 0;
			uint64_t pageKindMismatch = 0;
			double minParentOverChildError = std::numeric_limits<double>::infinity();
			for (uint32_t groupIndex = 0; groupIndex < prebuilt.groups.size(); ++groupIndex) {
				const ClusterLODGroup& group = prebuilt.groups[groupIndex];
				if (std::isnan(group.bounds.error)) {
					++nonFiniteError;
				}
				for (uint32_t segmentIndex = group.firstSegment; segmentIndex < group.firstSegment + group.segmentCount; ++segmentIndex) {
					if (segmentIndex >= prebuilt.segments.size()) {
						++badSegmentPage;
						continue;
					}
					const ClusterLODGroupSegment& segment = prebuilt.segments[segmentIndex];
					if (segment.meshletCount != 0u) {
						uint32_t meshPage = 0;
						if (!CLodCache::ResolveSegmentMeshPage(prebuilt, groupIndex, segment, meshPage) || meshPage >= context.mesh.PageCount()) {
							++badSegmentPage;
						}
						else if (context.mesh.IsVoxelPage(meshPage) != IsVoxel(group)) {
							++pageKindMismatch;
						}
					}
					if (segment.refinedGroup < 0) {
						continue;
					}
					if (static_cast<size_t>(segment.refinedGroup) >= prebuilt.groups.size()) {
						++badRefinedGroup;
						continue;
					}
					const ClusterLODGroup& child = prebuilt.groups[static_cast<size_t>(segment.refinedGroup)];
					if (HasMeasuredError(group) && HasMeasuredError(child)) {
						if (group.bounds.error < child.bounds.error) {
							++errorNotMonotonic;
						}
						if (child.bounds.error > 0.0f) {
							minParentOverChildError = std::min(minParentOverChildError, static_cast<double>(group.bounds.error) / child.bounds.error);
						}
					}
					const double dx = static_cast<double>(group.bounds.center[0]) - child.bounds.center[0];
					const double dy = static_cast<double>(group.bounds.center[1]) - child.bounds.center[1];
					const double dz = static_cast<double>(group.bounds.center[2]) - child.bounds.center[2];
					const double reach = std::sqrt(dx * dx + dy * dy + dz * dz) + child.bounds.radius;
					if (reach > static_cast<double>(group.bounds.radius) * 1.001 + 1.0e-6) {
						++childSphereEscapes;
						const bool parentIsVoxel = (group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u;
						const bool childIsVoxel = (child.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u;
						if (parentIsVoxel != childIsVoxel) {
							++childSphereEscapesAcrossKinds;
						}
						if (group.bounds.radius > 0.0f) {
							maxSphereEscape = std::max(maxSphereEscape, reach / group.bounds.radius - 1.0);
						}
					}
				}
			}
			emitProblem("validity.nonfinite_group_error", nonFiniteError);
			emitProblem("validity.bad_refined_group", badRefinedGroup);
			emitProblem("validity.error_not_monotonic", errorNotMonotonic);
			emitProblem("validity.child_sphere_escapes", childSphereEscapes);
			emitProblem("validity.child_sphere_escapes_voxel_to_triangle", childSphereEscapesAcrossKinds);
			if (maxSphereEscape > 0.0) {
				sink.Emit("validity.child_sphere_escape_max", maxSphereEscape, Unit::Ratio, Better::Lower, {}, Rollup::Max);
			}
			emitProblem("validity.bad_segment_page", badSegmentPage);
			emitProblem("validity.page_kind_mismatch", pageKindMismatch);
			if (std::isfinite(minParentOverChildError)) {
				sink.Emit("validity.min_parent_over_child_error", minParentOverChildError, Unit::Ratio, Better::Higher);
			}

			// Walk the traversal tree the way the shaders do. A group it never reaches can never be drawn.
			std::vector<bool> reachedGroups(prebuilt.groups.size(), false);
			std::vector<bool> visitedNodes(prebuilt.nodes.size(), false);
			uint64_t badNodeRange = 0;
			std::vector<uint32_t> stack;
			if (!prebuilt.nodes.empty()) {
				stack.push_back(0u);
			}
			for (const ClusterLODPartRecord& part : prebuilt.partRecords) {
				if (part.rootNode < prebuilt.nodes.size()) {
					stack.push_back(part.rootNode);
				}
			}
			while (!stack.empty()) {
				const uint32_t nodeIndex = stack.back();
				stack.pop_back();
				if (visitedNodes[nodeIndex]) {
					continue;
				}
				visitedNodes[nodeIndex] = true;
				const ClusterLODNodeRange& range = prebuilt.nodes[nodeIndex].range;
				if (range.isGroup == CLOD_NODE_INTERNAL) {
					const uint64_t end = static_cast<uint64_t>(range.indexOrOffset) + range.countMinusOne + 1u;
					if (end > prebuilt.nodes.size()) {
						++badNodeRange;
						continue;
					}
					for (uint32_t child = range.indexOrOffset; child < end; ++child) {
						stack.push_back(child);
					}
				}
				else if (range.isGroup == CLOD_NODE_VOXEL_LEAF || range.isGroup == CLOD_NODE_SEGMENT_LEAF) {
					if (range.ownerGroupId >= prebuilt.groups.size()) {
						++badNodeRange;
					}
					else {
						reachedGroups[range.ownerGroupId] = true;
					}
				}
			}
			uint64_t unreachableGroups = 0;
			for (uint32_t groupIndex = 0; groupIndex < prebuilt.groups.size(); ++groupIndex) {
				if (!reachedGroups[groupIndex] && prebuilt.groups[groupIndex].segmentCount != 0u) {
					++unreachableGroups;
				}
			}
			emitProblem("validity.bad_node_range", badNodeRange);
			emitProblem("validity.unreachable_groups", unreachableGroups);

			// Every page decodes.
			uint64_t badPages = 0;
			for (uint32_t pageIndex = 0; pageIndex < context.mesh.PageCount(); ++pageIndex) {
				const auto page = context.mesh.Page(pageIndex);
				bool parsed = false;
				if (context.mesh.IsVoxelPage(pageIndex)) {
					CLodCache::VoxelPage voxelPage;
					parsed = CLodCache::VoxelPage::Parse(page, voxelPage);
				}
				else {
					CLodCache::TrianglePage trianglePage;
					parsed = CLodCache::TrianglePage::Parse(page, trianglePage);
				}
				if (!parsed) {
					++badPages;
				}
			}
			emitProblem("validity.bad_pages", badPages);
		}
	};
}

std::unique_ptr<IAnalyzer> MakeHierarchyStructureAnalyzer() { return std::make_unique<HierarchyStructureAnalyzer>(); }
std::unique_ptr<IAnalyzer> MakeHierarchyValidityAnalyzer() { return std::make_unique<HierarchyValidityAnalyzer>(); }

}
