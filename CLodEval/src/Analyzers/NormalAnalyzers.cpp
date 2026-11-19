// Shading-normal integrity of simplified triangles. Simplification keeps original vertices, so a coarse triangle shades
// with normals that were authored for other triangles. When those normals point away from the triangle, it lights as if
// seen from behind. That is invisible to geometric error, since the positions can be exactly on the surface.
//
// Three measurements:
//  - inverted: the triangle's interpolated shading normal (at its centre) points away from its own face normal. This
//    catches folded triangles and wrong normals alike. Leaf triangles give the baseline the source asset already has.
//  - flipped vs source: the triangle's shading normals, averaged over it, point away from the leaf surface's shading
//    normals averaged over the same points. Each point's reference is the nearest leaf triangle facing the same way, so
//    coincident two-sided sheets compare like with like. Averaging keeps normal aliasing (a coarse triangle over fine
//    detail) from counting; what is left is shading that is wrong at the triangle's own scale.
//  - facing flipped vs source: no leaf surface facing the triangle's way lies near its centre, so its winding folded.
//
// Every simplified triangle is attributed to its depth and to the meshoptimizer path that produced it, which the
// builder records on the refined group (CLOD_GROUP_SIMPLIFY_METHOD_*).

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Analyzers/Analyzers.h"
#include "Core/Analyzer.h"
#include "Core/TriangleGrid.h"

namespace clodeval {
namespace {
	using DirectX::XMFLOAT3;

	constexpr double kPi = 3.14159265358979323846;
	// Reference lookups per mesh; triangles past this are sampled at an even stride.
	constexpr uint64_t kMaxReferenceSamples = 400000u;
	// A group whose simplified area is at least this much flipped against the source counts as a flipped group.
	constexpr double kFlippedGroupAreaFraction = 0.01;

	Vec Normalized(const Vec& value)
	{
		const double length = std::sqrt(Dot(value, value));
		return length > 0.0 ? value * (1.0 / length) : Vec{};
	}

	const char* MethodName(uint32_t method)
	{
		switch (method) {
		case 1u: return "regular";
		case 2u: return "permissive";
		case 3u: return "sloppy";
		default: return "unknown";
		}
	}

	struct Totals {
		double triangles = 0.0;
		double area = 0.0;
		double invertedTriangles = 0.0;
		double invertedArea = 0.0;
		double cornerInvertedArea = 0.0;
		// Reference comparisons, over the sampled triangles only.
		double sampledTriangles = 0.0;
		double sampledArea = 0.0;
		double flippedTriangles = 0.0;
		double flippedArea = 0.0;
		double facingFlippedArea = 0.0;
		double deviationArea = 0.0;  // sum of area * angle to the source shading normal
		double severeArea = 0.0;     // over 60 degrees from the source shading normal
	};

	struct Triangle {
		std::array<Vec, 3> position;
		std::array<Vec, 3> normal;
	};

	struct DumpedTriangle {
		Triangle triangle;
		uint32_t group = 0;
		bool leaf = false;
		bool inverted = false;
		bool sampled = false;
		bool flipped = false;
	};

	class NormalQualityAnalyzer final : public IAnalyzer {
	public:
		const AnalyzerInfo& Info() const override
		{
			static const AnalyzerInfo info{ "quality.normals", 1, "Simplified triangles whose shading normals face away from their face or from the source surface" };
			return info;
		}

		void Run(const AnalysisContext& context, MetricSink& sink) const override
		{
			const CachedMesh& mesh = context.mesh;
			const ClusterLODPrebuiltData& prebuilt = mesh.Prebuilt();
			const auto isVoxel = [](const ClusterLODGroup& group) { return (group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u; };

			// Visits every triangle of every triangle group: (groupIndex, refinedGroup, triangle). Pages without normals
			// are skipped, and so is a mesh where none have them.
			bool anyNormals = false;
			const auto forEachTriangle = [&](auto&& visit) {
				for (uint32_t groupIndex = 0; groupIndex < prebuilt.groups.size(); ++groupIndex) {
					const ClusterLODGroup& group = prebuilt.groups[groupIndex];
					if (isVoxel(group)) {
						continue;
					}
					for (uint32_t segmentIndex = group.firstSegment; segmentIndex < group.firstSegment + group.segmentCount; ++segmentIndex) {
						const ClusterLODGroupSegment& segment = prebuilt.segments[segmentIndex];
						mesh.VisitSegmentMeshlets(groupIndex, segment, [&](const CLodCache::TrianglePage& page, const CLodCache::TriangleMeshlet& meshlet) {
							if (!page.HasNormals()) {
								return;
							}
							anyNormals = true;
							for (uint32_t triangleIndex = 0; triangleIndex < meshlet.triangleCount; ++triangleIndex) {
								const auto corners = page.Triangle(meshlet, triangleIndex);
								Triangle triangle;
								for (int corner = 0; corner < 3; ++corner) {
									triangle.position[corner] = ToVec(page.Position(meshlet, corners[corner]));
									triangle.normal[corner] = Normalized(ToVec(page.Normal(meshlet, corners[corner])));
								}
								visit(groupIndex, segment.refinedGroup, triangle);
							}
						});
					}
				}
			};

			std::vector<std::array<XMFLOAT3, 3>> leafPositions;
			std::vector<std::array<Vec, 3>> leafNormals;
			uint64_t simplifiedTriangles = 0;
			Vec low{ std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max() };
			Vec high = low * -1.0;
			forEachTriangle([&](uint32_t, int32_t refinedGroup, const Triangle& triangle) {
				if (refinedGroup != -1) {
					++simplifiedTriangles;
					return;
				}
				std::array<XMFLOAT3, 3> positions;
				for (int corner = 0; corner < 3; ++corner) {
					const Vec& p = triangle.position[corner];
					positions[corner] = XMFLOAT3(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z));
					low = { std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z) };
					high = { std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z) };
				}
				leafPositions.push_back(positions);
				leafNormals.push_back(triangle.normal);
			});
			if (!anyNormals || leafPositions.empty()) {
				return;
			}
			const Vec extent = high - low;
			const double meshSize = std::sqrt(Dot(extent, extent));
			const TriangleGrid grid(std::move(leafPositions));
			const uint64_t sampleStride = std::max<uint64_t>((simplifiedTriangles + kMaxReferenceSamples - 1u) / kMaxReferenceSamples, 1u);

			Totals leaf, simplified;
			std::map<int32_t, Totals> byDepth;
			std::map<uint32_t, Totals> byMethod;
			std::vector<Totals> byGroup(prebuilt.groups.size());
			std::vector<double> deviations;
			std::vector<DumpedTriangle> dumped;
			const bool dump = std::getenv("CLOD_EVAL_NORMALS_DUMP") != nullptr;
			uint64_t simplifiedCursor = 0;

			forEachTriangle([&](uint32_t groupIndex, int32_t refinedGroup, const Triangle& triangle) {
				const auto& [a, b, c] = triangle.position;
				const Vec cross = Cross(b - a, c - a);
				const double doubleArea = std::sqrt(Dot(cross, cross));
				if (!(doubleArea > 0.0)) {
					return;  // degenerate: no face to be inverted against
				}
				const double area = 0.5 * doubleArea;
				const Vec faceNormal = cross * (1.0 / doubleArea);
				// Normals that cancel out are as broken as normals that point backwards.
				const Vec shading = Normalized(triangle.normal[0] + triangle.normal[1] + triangle.normal[2]);
				const bool inverted = Dot(shading, faceNormal) <= 0.0;
				const bool cornerInverted = Dot(triangle.normal[0], faceNormal) < 0.0 || Dot(triangle.normal[1], faceNormal) < 0.0 ||
					Dot(triangle.normal[2], faceNormal) < 0.0;

				const auto addIntrinsic = [&](Totals& totals) {
					totals.triangles += 1.0;
					totals.area += area;
					totals.invertedTriangles += inverted ? 1.0 : 0.0;
					totals.invertedArea += inverted ? area : 0.0;
					totals.cornerInvertedArea += cornerInverted ? area : 0.0;
				};
				if (refinedGroup == -1) {
					addIntrinsic(leaf);
					if (dump) {
						dumped.push_back({ triangle, groupIndex, true, inverted, false, false });
					}
					return;
				}
				const int32_t depth = prebuilt.groups[groupIndex].depth;
				const uint32_t method = refinedGroup >= 0 && static_cast<size_t>(refinedGroup) < prebuilt.groups.size()
					? ClusterLODGroupSimplifyMethod(prebuilt.groups[static_cast<size_t>(refinedGroup)].flags)
					: 0u;
				Totals& depthTotals = byDepth[depth];
				Totals& methodTotals = byMethod[method];
				Totals& groupTotals = byGroup[groupIndex];
				for (Totals* totals : { &simplified, &depthTotals, &methodTotals, &groupTotals }) {
					addIntrinsic(*totals);
				}
				if ((simplifiedCursor++ % sampleStride) != 0u) {
					if (dump) {
						dumped.push_back({ triangle, groupIndex, false, inverted, false, false });
					}
					return;
				}

				// The source surface under the triangle, filtered over it: at each sample point, the nearest leaf triangle
				// that faces the same way (among those about as near as the nearest) gives a shading normal, and their sum
				// is compared with the sum of this triangle's own shading normals at the same points. A coarse triangle
				// over fine detail is then judged against what that detail looks like on average, not against whichever
				// bump its centre lands on. bestFacing is the facing at the centre.
				static constexpr std::array<std::array<double, 3>, 7> kSamples{ { { 1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0 },
					{ 4.0 / 6.0, 1.0 / 6.0, 1.0 / 6.0 }, { 1.0 / 6.0, 4.0 / 6.0, 1.0 / 6.0 }, { 1.0 / 6.0, 1.0 / 6.0, 4.0 / 6.0 },
					{ 1.0 / 6.0, 5.0 / 12.0, 5.0 / 12.0 }, { 5.0 / 12.0, 1.0 / 6.0, 5.0 / 12.0 }, { 5.0 / 12.0, 5.0 / 12.0, 1.0 / 6.0 } } };
				Vec referenceSum{}, shadingSum{};
				double bestFacing = -std::numeric_limits<double>::max();
				for (size_t sample = 0; sample < kSamples.size(); ++sample) {
					const auto& [wa, wb, wc] = kSamples[sample];
					const Vec point = a * wa + b * wb + c * wc;
					const double reach = grid.Nearest(point) * 1.1 + meshSize * 1.0e-5;
					double sampleFacing = -std::numeric_limits<double>::max();
					Vec sampleReference{};
					grid.ForEachNear(point, reach, [&](uint32_t index) {
						const auto& leafTriangle = grid.Triangle(index);
						const Vec la = ToVec(leafTriangle[0]), lb = ToVec(leafTriangle[1]), lc = ToVec(leafTriangle[2]);
						const auto weights = ClosestPointBarycentric(point, la, lb, lc);
						const Vec offset = point - (la * weights[0] + lb * weights[1] + lc * weights[2]);
						if (Dot(offset, offset) > reach * reach) {
							return;
						}
						const double facing = Dot(Normalized(Cross(lb - la, lc - la)), faceNormal);
						if (facing > sampleFacing) {
							sampleFacing = facing;
							const auto& normals = leafNormals[index];
							sampleReference = Normalized(normals[0] * weights[0] + normals[1] * weights[1] + normals[2] * weights[2]);
						}
					});
					if (sampleFacing == -std::numeric_limits<double>::max()) {
						continue;
					}
					if (sample == 0u) {
						bestFacing = sampleFacing;
					}
					referenceSum = referenceSum + sampleReference;
					shadingSum = shadingSum + Normalized(triangle.normal[0] * wa + triangle.normal[1] * wb + triangle.normal[2] * wc);
				}
				const Vec reference = Normalized(referenceSum);
				const Vec filteredShading = Normalized(shadingSum);
				if (bestFacing == -std::numeric_limits<double>::max() || Dot(reference, reference) == 0.0) {
					if (dump) {
						dumped.push_back({ triangle, groupIndex, false, inverted, false, false });
					}
					return;
				}
				// Shading normals that cancel out over the triangle count as flipped, as for `inverted`.
				const double agreement = std::clamp(Dot(filteredShading, reference), -1.0, 1.0);
				const bool flipped = agreement <= 0.0;
				const double angle = std::acos(agreement);
				for (Totals* totals : { &simplified, &depthTotals, &methodTotals, &groupTotals }) {
					totals->sampledTriangles += 1.0;
					totals->sampledArea += area;
					totals->flippedTriangles += flipped ? 1.0 : 0.0;
					totals->flippedArea += flipped ? area : 0.0;
					totals->facingFlippedArea += bestFacing < 0.0 ? area : 0.0;
					totals->deviationArea += area * angle;
					totals->severeArea += angle > kPi / 3.0 ? area : 0.0;
				}
				deviations.push_back(angle);
				if (dump) {
					dumped.push_back({ triangle, groupIndex, false, inverted, true, flipped });
				}
			});

			// Leaves: the asset's own baseline.
			sink.Emit("normals.leaf_triangles", leaf.triangles, Unit::Count, Better::Neutral, {}, Rollup::Sum);
			sink.EmitRatio("normals.leaf_inverted_area_fraction", leaf.invertedArea, leaf.area, Unit::Ratio, Better::Lower);
			sink.Emit("normals.simplified_triangles", simplified.triangles, Unit::Count, Better::Neutral, {}, Rollup::Sum);
			if (simplified.triangles == 0.0) {
				return;
			}

			const auto emitTotals = [&](const Totals& totals, const Tags& tags) {
				sink.EmitRatio("normals.inverted_area_fraction", totals.invertedArea, totals.area, Unit::Ratio, Better::Lower, tags);
				sink.EmitRatio("normals.inverted_triangle_fraction", totals.invertedTriangles, totals.triangles, Unit::Ratio, Better::Lower, tags);
				sink.EmitRatio("normals.flipped_vs_source_area_fraction", totals.flippedArea, totals.sampledArea, Unit::Ratio, Better::Lower, tags);
				sink.EmitRatio("normals.severe_vs_source_area_fraction", totals.severeArea, totals.sampledArea, Unit::Ratio, Better::Lower, tags);
				sink.EmitRatio("normals.mean_deviation_vs_source", totals.deviationArea, totals.sampledArea, Unit::Radians, Better::Lower, tags);
			};
			emitTotals(simplified, {});
			sink.EmitRatio("normals.corner_inverted_area_fraction", simplified.cornerInvertedArea, simplified.area, Unit::Ratio, Better::Lower);
			sink.EmitRatio("normals.facing_flipped_vs_source_area_fraction", simplified.facingFlippedArea, simplified.sampledArea, Unit::Ratio, Better::Lower);
			sink.Emit("normals.inverted_triangles", simplified.invertedTriangles, Unit::Count, Better::Lower, {}, Rollup::Sum);
			sink.Emit("normals.flipped_vs_source_triangles", simplified.flippedTriangles, Unit::Count, Better::Lower, {}, Rollup::Sum);
			sink.Emit("normals.sampled_triangles", simplified.sampledTriangles, Unit::Count, Better::Neutral, {}, Rollup::Sum);
			for (const auto& [depth, totals] : byDepth) {
				emitTotals(totals, { { "depth", std::to_string(depth) } });
			}
			for (const auto& [method, totals] : byMethod) {
				const Tags tags{ { "method", MethodName(method) } };
				sink.Emit("normals.simplified_triangles", totals.triangles, Unit::Count, Better::Neutral, tags, Rollup::Sum);
				emitTotals(totals, tags);
			}

			uint64_t flippedGroups = 0, measuredGroups = 0;
			double worstGroup = 0.0;
			for (const Totals& totals : byGroup) {
				if (totals.sampledArea <= 0.0) {
					continue;
				}
				++measuredGroups;
				const double fraction = totals.flippedArea / totals.sampledArea;
				flippedGroups += fraction >= kFlippedGroupAreaFraction ? 1u : 0u;
				worstGroup = std::max(worstGroup, fraction);
			}
			sink.Emit("normals.measured_groups", static_cast<double>(measuredGroups), Unit::Count, Better::Neutral, {}, Rollup::Sum);
			sink.Emit("normals.flipped_groups", static_cast<double>(flippedGroups), Unit::Count, Better::Lower, {}, Rollup::Sum);
			sink.Emit("normals.worst_group_flipped_area_fraction", worstGroup, Unit::Ratio, Better::Lower, {}, Rollup::Max);
			if (!deviations.empty()) {
				EmitDistribution(sink, "normals.deviation_vs_source", deviations, Unit::Radians, Better::Lower);
			}

			if (dump) {
				WriteDump(std::getenv("CLOD_EVAL_NORMALS_DUMP"), context, prebuilt, byGroup, dumped);
			}
		}

	private:
		// CLOD_EVAL_NORMALS_DUMP=<dir>: per mesh with any inverted or flipped triangle, a CSV of the groups that have them,
		// an OBJ of those triangles (normals as decoded) for a model viewer, and a CSV of every triangle with its status.
		// Meant for one mesh at a time (analyze-cache on its .clodmeta): the triangle CSV is large.
		static void WriteDump(
			const char* directory,
			const AnalysisContext& context,
			const ClusterLODPrebuiltData& prebuilt,
			const std::vector<Totals>& byGroup,
			const std::vector<DumpedTriangle>& triangles)
		{
			if (std::ranges::none_of(triangles, [](const DumpedTriangle& entry) { return entry.inverted || entry.flipped; })) {
				return;
			}
			std::string stem = std::format("{}_{}", context.asset, context.mesh.Name());
			for (char& ch : stem) {
				if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && ch != '-') {
					ch = '_';
				}
			}
			const std::filesystem::path root(directory);
			std::error_code error;
			std::filesystem::create_directories(root, error);
			{
				std::ofstream csv(root / (stem + ".groups.csv"));
				csv << "group,depth,refined_from_method,triangles,area,inverted_area_fraction,flipped_vs_source_area_fraction,severe_area_fraction\n";
				for (uint32_t groupIndex = 0; groupIndex < byGroup.size(); ++groupIndex) {
					const Totals& totals = byGroup[groupIndex];
					if (totals.flippedArea <= 0.0 && totals.invertedArea <= 0.0) {
						continue;
					}
					// The method is per segment; name the methods seen among this group's simplified segments.
					std::string methods;
					const ClusterLODGroup& group = prebuilt.groups[groupIndex];
					for (uint32_t segmentIndex = group.firstSegment; segmentIndex < group.firstSegment + group.segmentCount; ++segmentIndex) {
						const int32_t refined = prebuilt.segments[segmentIndex].refinedGroup;
						if (refined < 0 || static_cast<size_t>(refined) >= prebuilt.groups.size()) {
							continue;
						}
						const std::string name = MethodName(ClusterLODGroupSimplifyMethod(prebuilt.groups[static_cast<size_t>(refined)].flags));
						if (methods.find(name) == std::string::npos) {
							methods += (methods.empty() ? "" : "+") + name;
						}
					}
					csv << groupIndex << "," << group.depth << "," << methods << "," << totals.triangles << "," << totals.area << ","
						<< totals.invertedArea / totals.area << "," << (totals.sampledArea > 0.0 ? totals.flippedArea / totals.sampledArea : 0.0) << ","
						<< (totals.sampledArea > 0.0 ? totals.severeArea / totals.sampledArea : 0.0) << "\n";
				}
			}
			std::ofstream obj(root / (stem + ".flipped.obj"));
			uint32_t currentGroup = std::numeric_limits<uint32_t>::max();
			uint32_t vertex = 1;
			for (const DumpedTriangle& entry : triangles) {
				if (entry.leaf || !(entry.inverted || entry.flipped)) {
					continue;
				}
				if (entry.group != currentGroup) {
					currentGroup = entry.group;
					obj << "g group" << currentGroup << "_depth" << prebuilt.groups[currentGroup].depth << "\n";
				}
				for (int corner = 0; corner < 3; ++corner) {
					const Vec& p = entry.triangle.position[corner];
					const Vec& n = entry.triangle.normal[corner];
					obj << "v " << p.x << " " << p.y << " " << p.z << "\nvn " << n.x << " " << n.y << " " << n.z << "\n";
				}
				obj << "f " << vertex << "//" << vertex << " " << vertex + 1 << "//" << vertex + 1 << " " << vertex + 2 << "//" << vertex + 2 << "\n";
				vertex += 3;
			}

			std::ofstream csv(root / (stem + ".triangles.csv"));
			csv << "group,depth,leaf,inverted,sampled,flipped,ax,ay,az,bx,by,bz,cx,cy,cz,nax,nay,naz,nbx,nby,nbz,ncx,ncy,ncz\n";
			for (const DumpedTriangle& entry : triangles) {
				csv << entry.group << "," << prebuilt.groups[entry.group].depth << "," << entry.leaf << "," << entry.inverted << "," << entry.sampled << ","
					<< entry.flipped;
				for (const auto* corners : { &entry.triangle.position, &entry.triangle.normal }) {
					for (const Vec& value : *corners) {
						csv << "," << value.x << "," << value.y << "," << value.z;
					}
				}
				csv << "\n";
			}
		}
	};
}

std::unique_ptr<IAnalyzer> MakeNormalQualityAnalyzer() { return std::make_unique<NormalQualityAnalyzer>(); }

}
