// Appearance of simplified geometry against the original leaves, measured pointwise from decoded pages, and the same
// channels over the triangles the runtime would draw at uniform cuts. quality.normals asks whether shading is flipped,
// after averaging over each triangle; this asks how wrong it is, point by point, with no averaging that could cancel a
// residual, and how much geometry and coverage the representation gives up for its triangle count.
//
// Channels (Core/Appearance.h):
//  - normals: |n_coarse - n_source|^2 integrated over area (Q_N), its area-normalised value, and area-weighted angle and
//    chord distributions. The reference is the nearest leaf point facing the coarse triangle's way when one is about as
//    near as the nearest; otherwise the nearest, and the point counts as ambiguous.
//  - geometry: coarse to source and source to coarse distance, sampled. Sampled maxima are not Hausdorff bounds.
//  - coverage: coarse surface farther from the source than the allowed error (spurious) and source surface farther from
//    what is drawn than it (missing); points with no correspondence at all (unmatched) are counted, never dropped.
//
// The cut sweep is the decisive number: triangles drawn against each channel, at fixed cuts in mesh units, whole-mesh.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Analyzers/Analyzers.h"
#include "Core/Analyzer.h"
#include "Core/Appearance.h"
#include "Core/ErrorChannels.h"

namespace clodeval {
namespace {
	// Fixed cuts in mesh units, 2^kMinCutLog2 to 2^kMaxCutLog2, so two builds compare cut by cut.
	constexpr int kMinCutLog2 = -14;
	constexpr int kMaxCutLog2 = 8;
	// Simplified triangles measured per mesh, each at kTriangleSamplePoints; past this they are taken at an even stride.
	constexpr uint64_t kMaxCoarseTriangles = 250000u;
	// Leaf triangles measured against what a cut draws, per distinct cut selection, at their centroids.
	constexpr uint64_t kMaxSourceTrianglesPerCut = 65536u;
	// A group error at or over this is a sentinel (roots, terminal groups), not an error.
	constexpr float kSentinelError = 1.0e19f;

	const char* MethodName(uint32_t method)
	{
		switch (method) {
		case 1u: return "regular";
		case 2u: return "permissive";
		case 3u: return "sloppy";
		default: return "unknown";
		}
	}

	float ErrorOf(const ClusterLODGroup& group)
	{
		return std::isfinite(group.bounds.error) && group.bounds.error < kSentinelError ? group.bounds.error : std::numeric_limits<float>::infinity();
	}

	struct Segment {
		uint32_t group = 0;
		int32_t refined = -1;
		int32_t depth = 0;
		uint32_t method = 0;
		bool leaf = false;
		// It draws while enter <= cut < leave: its own group's error is over the cut and the group it refines has an
		// error at or under it (the runtime's two conditions; ProjectedGeometricError in a uniform, telephoto cut).
		float enter = 0.0f;
		float leave = 0.0f;
		uint32_t firstTriangle = 0;
		uint32_t triangleCount = 0;
		double area = 0.0;
		bool DrawsAt(double cut) const { return enter <= cut && cut < leave; }
	};

	Vec At(const ShadedTriangle& triangle, const std::array<double, 3>& weights)
	{
		return triangle.position[0] * weights[0] + triangle.position[1] * weights[1] + triangle.position[2] * weights[2];
	}

	class AppearanceAnalyzer final : public IAnalyzer {
	public:
		const AnalyzerInfo& Info() const override
		{
			static const AnalyzerInfo info{ "quality.appearance", 1,
				"Pointwise shading-normal residual, bidirectional distance and coverage of simplified geometry against the leaves, stored and at runtime cuts" };
			return info;
		}

		void Run(const AnalysisContext& context, MetricSink& sink) const override
		{
			const CachedMesh& mesh = context.mesh;
			// A forked mesh's branches share their lower levels; measuring both would count those twice. The triangle
			// branch is what raster without voxels and ray tracing draw.
			if (mesh.Branch() == CLOD_BRANCH_VOXEL) {
				return;
			}
			const ClusterLODPrebuiltData& prebuilt = mesh.Prebuilt();

			std::vector<ShadedTriangle> triangles;
			std::vector<Segment> segments;
			bool missingNormals = false;
			// A cut is only modelled for plain triangle hierarchies: voxel groups draw something this cannot measure, and
			// merge-hierarchy proxies draw parts from other caches.
			bool cutsModelled = true;
			for (uint32_t groupIndex = 0; groupIndex < prebuilt.groups.size(); ++groupIndex) {
				const ClusterLODGroup& group = prebuilt.groups[groupIndex];
				if ((group.flags & (CLOD_GROUP_FLAG_IS_VOXEL | CLOD_GROUP_FLAG_IS_ASSEMBLY_PROXY | CLOD_GROUP_FLAG_MERGE_CHILD)) != 0u) {
					cutsModelled = false;
					continue;
				}
				for (uint32_t segmentIndex = group.firstSegment; segmentIndex < group.firstSegment + group.segmentCount; ++segmentIndex) {
					const ClusterLODGroupSegment& stored = prebuilt.segments[segmentIndex];
					Segment segment;
					segment.group = groupIndex;
					segment.refined = stored.refinedGroup;
					segment.depth = group.depth;
					segment.leaf = stored.refinedGroup < 0;
					segment.leave = ErrorOf(group);
					if (!segment.leaf) {
						if (static_cast<size_t>(stored.refinedGroup) >= prebuilt.groups.size()) {
							cutsModelled = false;
							continue;
						}
						const ClusterLODGroup& refined = prebuilt.groups[static_cast<size_t>(stored.refinedGroup)];
						segment.enter = ErrorOf(refined);
						segment.method = ClusterLODGroupSimplifyMethod(refined.flags);
						if ((refined.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u) {
							cutsModelled = false;
						}
					}
					segment.firstTriangle = static_cast<uint32_t>(triangles.size());
					mesh.VisitSegmentMeshlets(groupIndex, stored, [&](const CLodCache::TrianglePage& page, const CLodCache::TriangleMeshlet& meshlet) {
						if (!page.HasNormals()) {
							missingNormals = true;
							return;
						}
						for (uint32_t triangleIndex = 0; triangleIndex < meshlet.triangleCount; ++triangleIndex) {
							const auto corners = page.Triangle(meshlet, triangleIndex);
							ShadedTriangle triangle;
							for (int corner = 0; corner < 3; ++corner) {
								triangle.position[corner] = ToVec(page.Position(meshlet, corners[corner]));
								const Vec normal = ToVec(page.Normal(meshlet, corners[corner]));
								const double length = std::sqrt(Dot(normal, normal));
								triangle.normal[corner] = length > 0.0 ? normal * (1.0 / length) : Vec{};
							}
							segment.area += TriangleArea(triangle);
							triangles.push_back(triangle);
						}
					});
					segment.triangleCount = static_cast<uint32_t>(triangles.size()) - segment.firstTriangle;
					segments.push_back(segment);
				}
			}
			if (missingNormals) {
				sink.Emit("appearance.meshes_without_normals", 1.0, Unit::Count, Better::Neutral, {}, Rollup::Sum);
				return;
			}

			std::vector<ShadedTriangle> leaves;
			uint64_t simplifiedTriangles = 0;
			for (const Segment& segment : segments) {
				if (segment.leaf) {
					leaves.insert(leaves.end(), triangles.begin() + segment.firstTriangle, triangles.begin() + segment.firstTriangle + segment.triangleCount);
				}
				else {
					simplifiedTriangles += segment.triangleCount;
				}
			}
			if (leaves.empty()) {
				return;
			}
			const double size = SurfaceSize(leaves);
			const double leafArea = [&] {
				double total = 0.0;
				for (const Segment& segment : segments) {
					total += segment.leaf ? segment.area : 0.0;
				}
				return total;
			}();
			const SurfaceIndex source(std::move(leaves));

			// Every sampled point of every simplified triangle, measured once against the leaves. Which reference counts
			// and what is spurious depend on the allowed error, so they are decided per use.
			std::vector<AppearanceSample> samples;
			std::vector<uint32_t> sampleSegment;
			std::vector<double> sampledArea(segments.size(), 0.0);
			const uint64_t coarseStride = std::max<uint64_t>((simplifiedTriangles + kMaxCoarseTriangles - 1u) / kMaxCoarseTriangles, 1u);
			uint64_t coarseCursor = 0;
			for (uint32_t segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex) {
				const Segment& segment = segments[segmentIndex];
				if (segment.leaf) {
					continue;
				}
				for (uint32_t triangleIndex = segment.firstTriangle; triangleIndex < segment.firstTriangle + segment.triangleCount; ++triangleIndex) {
					if ((coarseCursor++ % coarseStride) != 0u) {
						continue;
					}
					const ShadedTriangle& triangle = triangles[triangleIndex];
					const double area = TriangleArea(triangle);
					if (!(area > 0.0)) {
						continue;
					}
					const Vec face = FaceNormal(triangle);
					const double reach = MakeAppearanceTolerance(0.0, size).FacingReach();
					for (const auto& weights : kTriangleSamplePoints) {
						const SurfaceMatch match = source.Match(At(triangle, weights), face, reach);
						samples.push_back(ResolveSample(match, ShadingNormal(triangle, weights), true, area / kTriangleSamplePoints.size()));
						sampleSegment.push_back(segmentIndex);
					}
					sampledArea[segmentIndex] += area;
				}
			}

			EmitStored(sink, segments, samples, sampleSegment, sampledArea, size, simplifiedTriangles);
			if (!cutsModelled) {
				sink.Emit("appearance.cut_excluded_meshes", 1.0, Unit::Count, Better::Neutral, {}, Rollup::Sum);
				return;
			}
			SweepCuts(sink, triangles, segments, samples, sampleSegment, sampledArea, size, leafArea, {});

			// Error channels (docs/clod-simplification-validation.md, "Stage 3"): what the builder measured, whether it is
			// monotone along every segment (enter <= leave under any rule), and the sweep again under each selection rule.
			const GroupErrorChannels channels = ReadGroupErrorChannels(prebuilt);
			if (channels.present) {
				double violations = 0.0, measuredAboveQuadric = 0.0, simplifiedGroups = 0.0;
				std::vector<bool> refinedInto(prebuilt.groups.size(), false);
				for (const Segment& segment : segments) {
					if (segment.leaf) {
						continue;
					}
					const uint32_t child = static_cast<uint32_t>(segment.refined);
					refinedInto[child] = true;
					violations += channels.measured[segment.group] < channels.measured[child] || channels.appearance[segment.group] < channels.appearance[child] ? 1.0 : 0.0;
				}
				for (size_t group = 0; group < prebuilt.groups.size(); ++group) {
					if (refinedInto[group]) {
						simplifiedGroups += 1.0;
						measuredAboveQuadric += channels.measured[group] > prebuilt.groups[group].bounds.error ? 1.0 : 0.0;
					}
				}
				sink.Emit("appearance.channels.meshes", 1.0, Unit::Count, Better::Neutral, {}, Rollup::Sum);
				sink.Emit("appearance.channels.monotonic_violations", violations, Unit::Count, Better::Lower, {}, Rollup::Sum);
				sink.EmitRatio("appearance.channels.measured_above_quadric_group_fraction", measuredAboveQuadric, simplifiedGroups, Unit::Ratio, Better::Neutral);
			}
			if (context.cutRules == nullptr) {
				return;
			}
			for (const CutRule& rule : *context.cutRules) {
				const auto selection = [&](uint32_t group) {
					const float error = SelectionError(prebuilt.groups[group].bounds.error, channels.measured[group], channels.appearance[group], rule);
					return std::isfinite(error) && error < kSentinelError ? error : std::numeric_limits<float>::infinity();
				};
				std::vector<Segment> ruled = segments;
				for (Segment& segment : ruled) {
					segment.leave = selection(segment.group);
					segment.enter = segment.leaf ? 0.0f : selection(static_cast<uint32_t>(segment.refined));
				}
				SweepCuts(sink, triangles, ruled, samples, sampleSegment, sampledArea, size, leafArea, { { "rule", rule.name } });
			}
		}

	private:
		// Stored simplified geometry, whatever cut draws it: each point against its own declared error, the error of
		// the group it was simplified from.
		static void EmitStored(
			MetricSink& sink,
			const std::vector<Segment>& segments,
			const std::vector<AppearanceSample>& samples,
			const std::vector<uint32_t>& sampleSegment,
			const std::vector<double>& sampledArea,
			double size,
			uint64_t simplifiedTriangles)
		{
			sink.Emit("appearance.simplified_triangles", static_cast<double>(simplifiedTriangles), Unit::Count, Better::Neutral, {}, Rollup::Sum);
			if (samples.empty()) {
				return;
			}
			struct Category {
				AppearanceTotals totals;
				double exactArea = 0.0;
				double sampledArea = 0.0;
				double triangles = 0.0;
			};
			Category total;
			std::map<int32_t, Category> byDepth;
			std::map<uint32_t, Category> byMethod;
			for (uint32_t segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex) {
				const Segment& segment = segments[segmentIndex];
				if (segment.leaf) {
					continue;
				}
				for (Category* category : { &total, &byDepth[segment.depth], &byMethod[segment.method] }) {
					category->exactArea += segment.area;
					category->sampledArea += sampledArea[segmentIndex];
					category->triangles += segment.triangleCount;
				}
			}
			std::vector<AppearanceTolerance> tolerances;
			tolerances.reserve(segments.size());
			for (const Segment& segment : segments) {
				tolerances.push_back(MakeAppearanceTolerance(segment.enter, size));
			}
			for (size_t index = 0; index < samples.size(); ++index) {
				const Segment& segment = segments[sampleSegment[index]];
				const AppearanceTolerance& tolerance = tolerances[sampleSegment[index]];
				for (Category* category : { &total, &byDepth[segment.depth], &byMethod[segment.method] }) {
					category->totals.Add(samples[index], tolerance);
				}
			}
			const auto emit = [&](Category& category, const Tags& tags) {
				// Sampled points stand for the category's whole area.
				if (category.sampledArea > 0.0) {
					category.totals.Scale(category.exactArea / category.sampledArea);
				}
				const AppearanceTotals& t = category.totals;
				const bool whole = tags.empty();
				const auto ratio = [&](const char* id, double numerator, double denominator, Unit unit) {
					if (whole) {
						sink.EmitRatio(id, numerator, denominator, unit, Better::Lower, tags);
					}
					else {
						sink.EmitRatioRollupOnly(id, numerator, denominator, unit, Better::Lower, tags);
					}
				};
				if (!whole) {
					sink.EmitRollupOnly("appearance.simplified_triangles", category.triangles, Unit::Count, Better::Neutral, tags, Rollup::Sum);
				}
				ratio("appearance.normal_q_per_area", t.normalQ, t.measuredArea, Unit::Ratio);
				sink.EmitQuantiles("appearance.angle", t.angle, Unit::Radians, Better::Lower, tags, whole);
				for (size_t threshold = 0; threshold < AppearanceTotals::kAngleThresholdsDegrees.size(); ++threshold) {
					Tags overTags = tags;
					overTags["over"] = std::format("{:g}deg", AppearanceTotals::kAngleThresholdsDegrees[threshold]);
					if (whole) {
						sink.EmitRatio("appearance.angle_over_area_fraction", t.angleOverArea[threshold], t.measuredArea, Unit::Ratio, Better::Lower, overTags);
					}
					else {
						sink.EmitRatioRollupOnly("appearance.angle_over_area_fraction", t.angleOverArea[threshold], t.measuredArea, Unit::Ratio, Better::Lower, overTags);
					}
				}
				ratio("appearance.spurious_area_fraction", t.beyondArea, t.area, Unit::Ratio);
				ratio("appearance.ambiguous_area_fraction", t.ambiguousArea, t.area, Unit::Ratio);
				ratio("appearance.unmatched_area_fraction", t.unmatchedArea, t.area, Unit::Ratio);
				if (!whole) {
					return;
				}
				sink.Emit("appearance.normal_q", t.normalQ, Unit::MeshArea, Better::Lower, {}, Rollup::Sum);
				sink.Emit("appearance.angle_sampled_max", t.maxAngle, Unit::Radians, Better::Lower, {}, Rollup::Max);
				sink.EmitQuantiles("appearance.chord", t.chord, Unit::Ratio, Better::Lower);
				sink.EmitQuantiles("appearance.distance_over_error", t.distanceOverTolerance, Unit::Ratio, Better::Lower);
				sink.Emit("appearance.distance_over_error_sampled_max", t.maxDistanceOverTolerance, Unit::Ratio, Better::Lower, {}, Rollup::Max);
				sink.EmitRatio("appearance.coarse_zero_normal_area_fraction", t.coarseZeroNormalArea, t.area, Unit::Ratio, Better::Lower);
				sink.EmitRatio("appearance.source_zero_normal_area_fraction", t.sourceZeroNormalArea, t.area, Unit::Ratio, Better::Lower);
				sink.Emit("appearance.sampled_points", static_cast<double>(t.samples), Unit::Count, Better::Neutral, {}, Rollup::Sum);
				sink.Emit("appearance.unmatched_samples", static_cast<double>(t.unmatchedSamples), Unit::Count, Better::Lower, {}, Rollup::Sum);
				sink.Emit("appearance.ambiguous_samples", static_cast<double>(t.ambiguousSamples), Unit::Count, Better::Lower, {}, Rollup::Sum);
				sink.Emit("appearance.coarse_zero_normal_samples", static_cast<double>(t.coarseZeroNormalSamples), Unit::Count, Better::Lower, {}, Rollup::Sum);
				sink.Emit("appearance.source_zero_normal_samples", static_cast<double>(t.sourceZeroNormalSamples), Unit::Count, Better::Neutral, {}, Rollup::Sum);
			};
			emit(total, {});
			for (auto& [depth, category] : byDepth) {
				emit(category, { { "depth", std::to_string(depth) } });
			}
			for (auto& [method, category] : byMethod) {
				emit(category, { { "method", MethodName(method) } });
			}
		}

		// The triangles a uniform cut draws, as the runtime selects them, measured whole-mesh: leaves drawn as
		// themselves are exactly on the source, simplified triangles by their samples, and the leaves they replace
		// against everything drawn.
		static void SweepCuts(
			MetricSink& sink,
			const std::vector<ShadedTriangle>& triangles,
			const std::vector<Segment>& segments,
			const std::vector<AppearanceSample>& samples,
			const std::vector<uint32_t>& sampleSegment,
			const std::vector<double>& sampledArea,
			double size,
			double leafArea,
			const Tags& ruleTags)
		{
			std::vector<bool> previousDrawn;
			std::vector<AppearanceSample> sourceSamples;
			double sourceSampledArea = 0.0;
			for (int exponent = kMinCutLog2; exponent <= kMaxCutLog2; ++exponent) {
				const double cut = std::ldexp(1.0, exponent);
				const AppearanceTolerance tolerance = MakeAppearanceTolerance(cut, size);
				std::vector<bool> drawn(segments.size(), false);
				double drawnTriangles = 0.0, drawnSimplified = 0.0, leafDrawnArea = 0.0, coarseArea = 0.0, coarseSampledArea = 0.0;
				for (size_t index = 0; index < segments.size(); ++index) {
					const Segment& segment = segments[index];
					if (!segment.DrawsAt(cut)) {
						continue;
					}
					drawn[index] = true;
					drawnTriangles += segment.triangleCount;
					if (segment.leaf) {
						leafDrawnArea += segment.area;
					}
					else {
						drawnSimplified += segment.triangleCount;
						coarseArea += segment.area;
						coarseSampledArea += sampledArea[index];
					}
				}

				AppearanceTotals coarse;
				for (size_t index = 0; index < samples.size(); ++index) {
					if (drawn[sampleSegment[index]]) {
						coarse.Add(samples[index], tolerance);
					}
				}
				if (coarseSampledArea > 0.0) {
					coarse.Scale(coarseArea / coarseSampledArea);
				}
				AppearanceTotals drawnToSource;
				drawnToSource.AddExact(leafDrawnArea);
				drawnToSource.Merge(coarse);

				// The leaves not drawn as themselves, against everything drawn. What they find does not depend on the
				// cut, only on what is drawn, so a selection repeated from the last cut reuses its points.
				const double replacedLeafArea = leafArea - leafDrawnArea;
				if (drawn != previousDrawn) {
					previousDrawn = drawn;
					sourceSamples.clear();
					sourceSampledArea = 0.0;
					std::vector<ShadedTriangle> drawnTriangleList;
					uint64_t replacedLeaves = 0;
					for (size_t index = 0; index < segments.size(); ++index) {
						const Segment& segment = segments[index];
						if (drawn[index]) {
							drawnTriangleList.insert(drawnTriangleList.end(), triangles.begin() + segment.firstTriangle,
								triangles.begin() + segment.firstTriangle + segment.triangleCount);
						}
						else if (segment.leaf) {
							replacedLeaves += segment.triangleCount;
						}
					}
					const SurfaceIndex drawnIndex(std::move(drawnTriangleList));
					const uint64_t stride = std::max<uint64_t>((replacedLeaves + kMaxSourceTrianglesPerCut - 1u) / kMaxSourceTrianglesPerCut, 1u);
					uint64_t cursor = 0;
					const std::array<double, 3> centroid{ 1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0 };
					for (size_t index = 0; index < segments.size(); ++index) {
						const Segment& segment = segments[index];
						if (drawn[index] || !segment.leaf) {
							continue;
						}
						for (uint32_t triangleIndex = segment.firstTriangle; triangleIndex < segment.firstTriangle + segment.triangleCount; ++triangleIndex) {
							if ((cursor++ % stride) != 0u) {
								continue;
							}
							const ShadedTriangle& triangle = triangles[triangleIndex];
							const double area = TriangleArea(triangle);
							if (!(area > 0.0)) {
								continue;
							}
							// Nothing drawn at all leaves a default match: infinitely far, so missing and unmatched.
							const SurfaceMatch match = drawnIndex.Empty() ? SurfaceMatch{} : drawnIndex.Match(At(triangle, centroid), FaceNormal(triangle), tolerance.FacingReach());
							sourceSamples.push_back(ResolveSample(match, ShadingNormal(triangle, centroid), false, area));
							sourceSampledArea += area;
						}
					}
				}
				AppearanceTotals replaced;
				for (const AppearanceSample& sample : sourceSamples) {
					replaced.Add(sample, tolerance);
				}
				if (sourceSampledArea > 0.0) {
					replaced.Scale(replacedLeafArea / sourceSampledArea);
				}
				AppearanceTotals sourceToDrawn;
				sourceToDrawn.AddExact(leafDrawnArea);
				sourceToDrawn.Merge(replaced);

				EmitCut(sink, cut, drawnTriangles, drawnSimplified, coarseArea, drawnToSource, sourceToDrawn, ruleTags);
			}
		}

		static void EmitCut(
			MetricSink& sink,
			double cut,
			double drawnTriangles,
			double drawnSimplified,
			double coarseArea,
			const AppearanceTotals& drawn,
			const AppearanceTotals& source,
			const Tags& ruleTags)
		{
			Tags tags = ruleTags;
			tags["cut"] = std::format("{:g}", cut);
			// A mesh drawing only its leaves at this cut has nothing to say for itself; its share still rolls up.
			const bool meshRecords = drawnSimplified > 0.0;
			const auto value = [&](const char* id, double v, Unit unit, Better better, Rollup rollup, bool record, const Tags& t) {
				if (record) {
					sink.Emit(id, v, unit, better, t, rollup);
				}
				else {
					sink.EmitRollupOnly(id, v, unit, better, t, rollup);
				}
			};
			const auto ratio = [&](const char* id, double numerator, double denominator, bool record, const Tags& t) {
				if (record) {
					sink.EmitRatio(id, numerator, denominator, Unit::Ratio, Better::Lower, t);
				}
				else {
					sink.EmitRatioRollupOnly(id, numerator, denominator, Unit::Ratio, Better::Lower, t);
				}
			};
			value("appearance.cut.triangles", drawnTriangles, Unit::Count, Better::Lower, Rollup::Sum, meshRecords, tags);
			value("appearance.cut.simplified_triangles", drawnSimplified, Unit::Count, Better::Neutral, Rollup::Sum, false, tags);
			ratio("appearance.cut.simplified_area_fraction", coarseArea, drawn.area, false, tags);
			value("appearance.cut.normal_q", drawn.normalQ, Unit::MeshArea, Better::Lower, Rollup::Sum, false, tags);
			ratio("appearance.cut.normal_q_per_area", drawn.normalQ, drawn.measuredArea, meshRecords, tags);
			sink.EmitQuantiles("appearance.cut.angle", drawn.angle, Unit::Radians, Better::Lower, tags, false);
			value("appearance.cut.angle_sampled_max", drawn.maxAngle, Unit::Radians, Better::Lower, Rollup::Max, false, tags);
			for (size_t threshold = 0; threshold < AppearanceTotals::kAngleThresholdsDegrees.size(); ++threshold) {
				Tags overTags = tags;
				overTags["over"] = std::format("{:g}deg", AppearanceTotals::kAngleThresholdsDegrees[threshold]);
				ratio("appearance.cut.angle_over_area_fraction", drawn.angleOverArea[threshold], drawn.measuredArea,
					meshRecords && threshold + 1u == AppearanceTotals::kAngleThresholdsDegrees.size(), overTags);
			}
			ratio("appearance.cut.spurious_area_fraction", drawn.beyondArea, drawn.area, false, tags);
			sink.EmitQuantiles("appearance.cut.coarse_to_source_over_cut", drawn.distanceOverTolerance, Unit::Ratio, Better::Lower, tags, false);
			value("appearance.cut.coarse_to_source_over_cut_sampled_max", drawn.maxDistanceOverTolerance, Unit::Ratio, Better::Lower, Rollup::Max, false, tags);
			ratio("appearance.cut.unmatched_area_fraction", drawn.unmatchedArea, drawn.area, false, tags);
			ratio("appearance.cut.ambiguous_area_fraction", drawn.ambiguousArea, drawn.area, false, tags);
			ratio("appearance.cut.coarse_zero_normal_area_fraction", drawn.coarseZeroNormalArea, drawn.area, false, tags);
			ratio("appearance.cut.missing_area_fraction", source.beyondArea, source.area, meshRecords, tags);
			sink.EmitQuantiles("appearance.cut.source_to_drawn_over_cut", source.distanceOverTolerance, Unit::Ratio, Better::Lower, tags, false);
			value("appearance.cut.source_to_drawn_over_cut_sampled_max", source.maxDistanceOverTolerance, Unit::Ratio, Better::Lower, Rollup::Max, false, tags);
			ratio("appearance.cut.source_normal_q_per_area", source.normalQ, source.measuredArea, false, tags);
			ratio("appearance.cut.source_unmatched_area_fraction", source.unmatchedArea, source.area, false, tags);
			ratio("appearance.cut.source_ambiguous_area_fraction", source.ambiguousArea, source.area, false, tags);
		}
	};
}

std::unique_ptr<IAnalyzer> MakeAppearanceAnalyzer() { return std::make_unique<AppearanceAnalyzer>(); }

}
