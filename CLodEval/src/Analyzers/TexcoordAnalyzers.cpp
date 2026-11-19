// Texture-coordinate drift of simplified triangles against the leaves. Simplification that moves vertices must give them new
// UVs, and a solve that does not follow the surface slides the texture across it: geometry and shading can be exact while the
// texture is not. Each simplified triangle is sampled at four interior points (a quarter of its area each) and at its corners;
// at each, the leaf triangles facing the same way that are about as near as the nearest (as quality.normals matches) give the
// leaf's UV at their closest points, and the drift is the smallest distance between the sample's UV and theirs, in UV units (UV
// set 0, as the pages store it). The smallest, because on a UV seam triangles of two charts are equally near.
//
// The builder knows no texture sizes and neither does this, so drift is in UV units: 1/1024 of a UV unit is a texel of a
// 1024-texel texture. It also counts drift that matching itself causes (a coarse sample matched to the nearest leaf point
// moves along the surface by up to its distance from it, which a surface-following UV would show too), so compare builds
// against each other, not against zero.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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
	using DirectX::XMFLOAT2;
	using DirectX::XMFLOAT3;

	// Reference lookups per mesh; simplified triangles past this are sampled at an even stride.
	constexpr uint64_t kMaxSampledTriangles = 400000u;
	// Drift thresholds for the area fractions, in UV units, and their tag values.
	constexpr std::array<double, 4> kOver{ 1.0 / 1024.0, 1.0 / 256.0, 1.0 / 64.0, 1.0 / 16.0 };
	constexpr std::array<const char*, 4> kOverName{ "1/1024", "1/256", "1/64", "1/16" };

	struct UvTriangle {
		std::array<Vec, 3> position;
		std::array<XMFLOAT2, 3> uv;
	};

	struct Totals {
		double area = 0.0;        // sampled interior area that matched a leaf
		double driftArea = 0.0;   // sum of area * drift
		std::array<double, 4> overArea{};
		double corners = 0.0;
		double cornerDrift = 0.0;
		double slideArea = 0.0;   // sum of area * drift / leaf UV scale (mesh units)
		double slideMeasuredArea = 0.0;
	};

	const char* MethodName(uint32_t method)
	{
		switch (method) {
		case 1u: return "regular";
		case 2u: return "permissive";
		case 3u: return "sloppy";
		default: return "unknown";
		}
	}

	class TexcoordDriftAnalyzer final : public IAnalyzer {
	public:
		const AnalyzerInfo& Info() const override
		{
			static const AnalyzerInfo info{ "quality.texcoords", 1, "UV drift of simplified triangles against the leaf UVs at matched points" };
			return info;
		}

		void Run(const AnalysisContext& context, MetricSink& sink) const override
		{
			const CachedMesh& mesh = context.mesh;
			const ClusterLODPrebuiltData& prebuilt = mesh.Prebuilt();

			// Every triangle of every triangle group with UVs: (groupIndex, refinedGroup, triangle).
			const auto forEachTriangle = [&](auto&& visit) {
				for (uint32_t groupIndex = 0; groupIndex < prebuilt.groups.size(); ++groupIndex) {
					const ClusterLODGroup& group = prebuilt.groups[groupIndex];
					if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u) {
						continue;
					}
					for (uint32_t segmentIndex = group.firstSegment; segmentIndex < group.firstSegment + group.segmentCount; ++segmentIndex) {
						const ClusterLODGroupSegment& segment = prebuilt.segments[segmentIndex];
						if (segment.meshletCount == 0u) {
							continue;
						}
						uint32_t meshPage = 0;
						CLodCache::TrianglePage page;
						if (!CLodCache::ResolveSegmentMeshPage(prebuilt, groupIndex, segment, meshPage) || !CLodCache::TrianglePage::Parse(mesh.Page(meshPage), page) ||
							page.UvSetCount() == 0u || static_cast<uint64_t>(segment.firstMeshletInPage) + segment.meshletCount > page.MeshletCount()) {
							continue;
						}
						for (uint32_t index = 0; index < segment.meshletCount; ++index) {
							const uint32_t meshletIndex = segment.firstMeshletInPage + index;
							const CLodCache::TriangleMeshlet meshlet = page.Meshlet(meshletIndex);
							for (uint32_t triangleIndex = 0; triangleIndex < meshlet.triangleCount; ++triangleIndex) {
								const auto corners = page.Triangle(meshlet, triangleIndex);
								UvTriangle triangle;
								for (int corner = 0; corner < 3; ++corner) {
									triangle.position[corner] = ToVec(page.Position(meshlet, corners[corner]));
									triangle.uv[corner] = page.Uv(meshletIndex, meshlet, 0u, corners[corner]);
								}
								visit(groupIndex, segment.refinedGroup, triangle);
							}
						}
					}
				}
			};

			std::vector<std::array<XMFLOAT3, 3>> leafPositions;
			std::vector<std::array<XMFLOAT2, 3>> leafUvs;
			uint64_t simplifiedTriangles = 0;
			Vec low{ std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max() };
			Vec high = low * -1.0;
			forEachTriangle([&](uint32_t, int32_t refinedGroup, const UvTriangle& triangle) {
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
				leafUvs.push_back(triangle.uv);
			});
			if (leafPositions.empty() || simplifiedTriangles == 0u) {
				return;
			}
			const Vec extent = high - low;
			const double meshSize = std::sqrt(Dot(extent, extent));
			const TriangleGrid grid(std::move(leafPositions));
			const uint64_t sampleStride = std::max<uint64_t>((simplifiedTriangles + kMaxSampledTriangles - 1u) / kMaxSampledTriangles, 1u);

			Totals all;
			std::map<int32_t, Totals> byDepth;
			std::map<uint32_t, Totals> byMethod;
			LogHistogram drift(1.0e-6, 16.0, 8u), cornerDrift(1.0e-6, 16.0, 8u);
			uint64_t cursor = 0, unmatched = 0;

			// The leaf UV nearest the sample's own among the leaf triangles about as near as the nearest that face the triangle's
			// way (the UV difference at each one's closest point), and that triangle's UV scale (UV units per mesh unit, 0 without UV
			// area). Taking the smallest difference, not the best-facing triangle, keeps a point on a UV seam (where triangles of two
			// charts are equally near) from reading the other chart's UV as drift. False when nothing faces that way.
			const auto leafUvAt = [&](const Vec& point, const Vec& faceNormal, double u, double v, double& distance, double& scale) {
				const double reach = grid.Nearest(point) * 1.1 + meshSize * 1.0e-5;
				distance = std::numeric_limits<double>::max();
				grid.ForEachNear(point, reach, [&](uint32_t index) {
					const auto& leaf = grid.Triangle(index);
					const Vec la = ToVec(leaf[0]), lb = ToVec(leaf[1]), lc = ToVec(leaf[2]);
					const auto weights = ClosestPointBarycentric(point, la, lb, lc);
					const Vec offset = point - (la * weights[0] + lb * weights[1] + lc * weights[2]);
					if (Dot(offset, offset) > reach * reach) {
						return;
					}
					const Vec cross = Cross(lb - la, lc - la);
					const double doubleArea = std::sqrt(Dot(cross, cross));
					if (!(doubleArea > 0.0) || !(Dot(cross, faceNormal) > 0.0)) {
						return;
					}
					const auto& t = leafUvs[index];
					const double du = u - (t[0].x * weights[0] + t[1].x * weights[1] + t[2].x * weights[2]);
					const double dv = v - (t[0].y * weights[0] + t[1].y * weights[1] + t[2].y * weights[2]);
					const double d = std::sqrt(du * du + dv * dv);
					if (d < distance) {
						distance = d;
						const double uvDoubleArea = std::fabs((t[1].x - t[0].x) * (t[2].y - t[0].y) - (t[2].x - t[0].x) * (t[1].y - t[0].y));
						scale = std::sqrt(uvDoubleArea / doubleArea);
					}
				});
				return distance != std::numeric_limits<double>::max();
			};

			forEachTriangle([&](uint32_t groupIndex, int32_t refinedGroup, const UvTriangle& triangle) {
				if (refinedGroup == -1 || (cursor++ % sampleStride) != 0u) {
					return;
				}
				const auto& [a, b, c] = triangle.position;
				const Vec cross = Cross(b - a, c - a);
				const double doubleArea = std::sqrt(Dot(cross, cross));
				if (!(doubleArea > 0.0)) {
					return;
				}
				const double area = 0.5 * doubleArea;
				const Vec faceNormal = cross * (1.0 / doubleArea);
				const int32_t depth = prebuilt.groups[groupIndex].depth;
				const uint32_t method = refinedGroup >= 0 && static_cast<size_t>(refinedGroup) < prebuilt.groups.size()
					? ClusterLODGroupSimplifyMethod(prebuilt.groups[static_cast<size_t>(refinedGroup)].flags)
					: 0u;
				Totals* totals[] = { &all, &byDepth[depth], &byMethod[method] };

				static constexpr std::array<std::array<double, 3>, 7> kSamples{ { { 1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0 }, { 4.0 / 6.0, 1.0 / 6.0, 1.0 / 6.0 },
					{ 1.0 / 6.0, 4.0 / 6.0, 1.0 / 6.0 }, { 1.0 / 6.0, 1.0 / 6.0, 4.0 / 6.0 }, { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 }, { 0.0, 0.0, 1.0 } } };
				for (size_t sample = 0; sample < kSamples.size(); ++sample) {
					const auto& [wa, wb, wc] = kSamples[sample];
					const Vec point = a * wa + b * wb + c * wc;
					const double u = triangle.uv[0].x * wa + triangle.uv[1].x * wb + triangle.uv[2].x * wc;
					const double v = triangle.uv[0].y * wa + triangle.uv[1].y * wb + triangle.uv[2].y * wc;
					double distance = 0.0, scale = 0.0;
					if (!leafUvAt(point, faceNormal, u, v, distance, scale) || !std::isfinite(distance)) {
						++unmatched;
						continue;
					}
					if (sample >= 4u) {
						cornerDrift.Add(distance, 1.0);
						for (Totals* t : totals) {
							t->corners += 1.0;
							t->cornerDrift += distance;
						}
						continue;
					}
					const double weight = area * 0.25;
					drift.Add(distance, weight);
					for (Totals* t : totals) {
						t->area += weight;
						t->driftArea += weight * distance;
						for (size_t level = 0; level < kOver.size(); ++level) {
							t->overArea[level] += distance > kOver[level] ? weight : 0.0;
						}
						if (scale > 0.0) {
							t->slideArea += weight * distance / scale;
							t->slideMeasuredArea += weight;
						}
					}
				}
			});

			const auto emitTotals = [&](const Totals& t, const Tags& tags) {
				sink.EmitRatio("texcoords.drift_mean", t.driftArea, t.area, Unit::None, Better::Lower, tags);
				sink.EmitRatio("texcoords.corner_drift_mean", t.cornerDrift, t.corners, Unit::None, Better::Lower, tags);
				sink.EmitRatio("texcoords.slide_mean", t.slideArea, t.slideMeasuredArea, Unit::MeshUnits, Better::Lower, tags);
				for (size_t level = 0; level < kOver.size(); ++level) {
					Tags overTags = tags;
					overTags["over"] = kOverName[level];
					sink.EmitRatio("texcoords.drift_over_area_fraction", t.overArea[level], t.area, Unit::Ratio, Better::Lower, overTags);
				}
			};
			emitTotals(all, {});
			for (const auto& [depth, t] : byDepth) {
				emitTotals(t, { { "depth", std::to_string(depth) } });
			}
			for (const auto& [method, t] : byMethod) {
				emitTotals(t, { { "method", MethodName(method) } });
			}
			sink.EmitQuantiles("texcoords.drift", drift, Unit::None, Better::Lower);
			sink.EmitQuantiles("texcoords.corner_drift", cornerDrift, Unit::None, Better::Lower);
			sink.Emit("texcoords.unmatched_samples", static_cast<double>(unmatched), Unit::Count, Better::Neutral, {}, Rollup::Sum);
		}
	};
}

std::unique_ptr<IAnalyzer> MakeTexcoordDriftAnalyzer() { return std::make_unique<TexcoordDriftAnalyzer>(); }

}
