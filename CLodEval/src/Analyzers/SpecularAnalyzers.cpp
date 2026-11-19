// Specular response of what runtime cuts draw, against the leaves' normals: stage 6 step 3 of the appearance plan
// (docs/clod-appearance-stage6.md). quality.appearance measures the shading-normal residual |n_coarse - n_source|^2, which says how
// wrong a Lambertian term is; a glossy highlight depends on the normal far more sharply, and a coarse surface that stands for a bumpy
// or folded one draws one sharp highlight where its leaves, seen through a pixel, draw a spread of them. Normal spread blocks
// (ClusterLODNormalSpread.h) widen the material's roughness by the spread the coarse surface hides; this measures whether that
// brings the coarse lobe closer to the leaves'.
//
// What a pixel sees. A segment is drawn from its enter error on, where a pixel of a one-pixel threshold is about that error wide, so
// each simplified sample is compared with the leaves inside a pixel-sized footprint around it: a disc of radius half the segment's
// enter error (times the suite's "specular_footprint", default 1) in the coarse triangle's plane, sampled by the 7-point disc rule
// (the centre and six points at sqrt(2/3) of the radius), each point matched to the nearest same-facing leaf (as quality.texture and
// quality.normals match). The reference is the mixture of the matched leaves' lobes; the coarse surface draws one lobe at its
// interpolated normal, with the base roughness (plain), with the roughness widened by the stored spread as material evaluation
// widens it, alpha'^2 = alpha^2 + min(v, cap alpha^2) (widened; equal to plain without blocks; cap the suite's "specular_spread_cap",
// default 1 as clodNormalSpreadRoughnessCap), or widened the same way by the spread measured right there against the footprint's
// leaves, each point capped at 0.1 as the builder caps it (oracle: what a per-point store could do; the builder stores per vertex,
// measured against the original). Per sample and base roughness:
//  - lobe distance: each lobe a Gaussian in slope space of the coarse triangle (a GGX / Beckmann lobe of roughness alpha has per-axis
//    slope variance alpha^2 / 2), 1 - 2 <R, C> / (<R, R> + <C, C>), closed form: 0 for the same distribution, 1 for disjoint ones.
//    It counts both where the highlight is and how wide and bright it is;
//  - response error: the GGX specular term (Fresnel 1, Smith height-correlated visibility, times N.L) at five view / light pairs set
//    in the coarse triangle's frame (mirror pairs centre the highlight on its face normal), |coarse - reference| relative to the
//    reference, and the drawn-to-reference energy ratio.
// Area-weighted over everything a cut draws (leaves drawn as themselves are exact). Every cut rule of the suite is swept as in
// quality.texture; widening never changes selection, so plain, widened and oracle compare at exactly the same triangles.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include <BasicRenderer/Assets/ClusterLODNormalSpread.h>

#include "Analyzers/Analyzers.h"
#include "Core/Analyzer.h"
#include "Core/ErrorChannels.h"
#include "Core/TriangleGrid.h"

namespace clodeval {
namespace {
	using DirectX::XMFLOAT3;

	constexpr int kMinCutLog2 = -14;
	constexpr int kMaxCutLog2 = 8;
	// Simplified triangles measured per mesh, each at four points; past this they are taken at an even stride.
	constexpr uint64_t kMaxCoarseTriangles = 60000u;
	constexpr float kSentinelError = 1.0e19f;
	// Base perceptual roughness (alpha = roughness^2), as material evaluation takes it.
	constexpr std::array<double, 2> kRoughness{ 0.25, 0.5 };
	constexpr size_t kRough = kRoughness.size();

	constexpr std::array<std::array<double, 3>, 4> kSamplePoints{ {
		{ 1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0 }, { 2.0 / 3.0, 1.0 / 6.0, 1.0 / 6.0 }, { 1.0 / 6.0, 2.0 / 3.0, 1.0 / 6.0 }, { 1.0 / 6.0, 1.0 / 6.0, 2.0 / 3.0 } } };

	// The 7-point disc rule: the centre (weight 1/4) and six points at sqrt(2/3) of the radius (1/8 each).
	constexpr size_t kFootprintPoints = 7u;
	// The oracle caps each point's squared chord as the builder does (ClusterLODBuilderSettings::lodNormalSpreadSampleCap's default).
	constexpr double kOracleSampleCap = 0.1;

	// View and light directions in the coarse triangle's frame (polar angle from the face normal, azimuth), in degrees.
	struct DirectionPair {
		double viewPolar, viewAzimuth, lightPolar, lightAzimuth;
	};
	constexpr std::array<DirectionPair, 5> kDirections{ {
		{ 45.0, 0.0, 45.0, 180.0 },  // mirror: the highlight centred on the face normal
		{ 70.0, 0.0, 70.0, 180.0 },  // grazing mirror
		{ 20.0, 0.0, 20.0, 180.0 },  // near-normal mirror
		{ 45.0, 0.0, 30.0, 150.0 },  // off-specular
		{ 30.0, 90.0, 50.0, 300.0 }, // off-specular, another plane
	} };
	constexpr size_t kPairs = kDirections.size();

	float ErrorOf(const ClusterLODGroup& group)
	{
		return std::isfinite(group.bounds.error) && group.bounds.error < kSentinelError ? group.bounds.error : std::numeric_limits<float>::infinity();
	}

	Vec Normalized(const Vec& v)
	{
		const double length = std::sqrt(Dot(v, v));
		return length > 1.0e-12 ? v * (1.0 / length) : Vec{};
	}

	// GGX specular times N.L, Fresnel 1 (PBR.hlsli's D_GGX and V_SmithGGXCorrelated with alpha = roughness^2).
	double Specular(const Vec& n, const Vec& view, const Vec& light, double alpha)
	{
		const double nv = Dot(n, view), nl = Dot(n, light);
		if (!(nv > 0.0) || !(nl > 0.0)) {
			return 0.0;
		}
		const Vec h = Normalized(view + light);
		const double nh = std::clamp(Dot(n, h), 0.0, 1.0);
		const double a2 = alpha * alpha;
		const double denom = nh * nh * (a2 - 1.0) + 1.0;
		const double d = a2 / (std::numbers::pi * denom * denom);
		const double lambdaV = nl * std::sqrt(nv * nv * (1.0 - a2) + a2);
		const double lambdaL = nv * std::sqrt(nl * nl * (1.0 - a2) + a2);
		const double vis = 0.5 / std::max(lambdaV + lambdaL, 1.0e-9);
		return d * vis * nl;
	}

	// A normal's slope (tan theta along the frame's tangent and bitangent) in the frame of a coarse triangle's face; normals facing
	// away from the face (or nearly edge-on) go far out, where no lobe overlaps them.
	std::array<double, 2> Slope(const Vec& n, const Vec& tangent, const Vec& bitangent, const Vec& face)
	{
		const double z = Dot(n, face);
		if (!(z > 0.05)) {
			return { 1.0e3, 1.0e3 };
		}
		return { Dot(n, tangent) / z, Dot(n, bitangent) / z };
	}

	// The overlap integral of two isotropic 2D Gaussians of per-axis variances va and vb.
	double Overlap(const std::array<double, 2>& a, const std::array<double, 2>& b, double va, double vb)
	{
		const double v = va + vb;
		const double dx = a[0] - b[0], dy = a[1] - b[1];
		return std::exp(-(dx * dx + dy * dy) / (2.0 * v)) / (2.0 * std::numbers::pi * v);
	}

	struct ShadedTriangle {
		std::array<Vec, 3> position;
		std::array<Vec, 3> normal;
		std::array<float, 3> spread{}; // stored v per corner (0 without a block)
	};

	struct Segment {
		uint32_t group = 0;
		int32_t refined = -1;
		bool leaf = false;
		float enter = 0.0f;
		float leave = 0.0f;
		uint32_t firstTriangle = 0;
		uint32_t triangleCount = 0;
		double area = 0.0;
		bool DrawsAt(double cut) const { return enter <= cut && cut < leave; }
	};

	// Per segment: the sampled sums the sweep adds up for the segments a cut draws.
	struct SegmentSums {
		double weight = 0.0;        // sampled area (matched samples)
		double spread = 0.0;        // area integral of the stored v
		double oracleSpread = 0.0;  // area integral of the footprint's measured v
		double spreadArea = 0.0;    // area with stored v > 0
		std::array<double, kRough> plainLobe{}, widenedLobe{}, oracleLobe{};       // area integral of the lobe distance
		std::array<double, kRough> reference{}, plain{}, widened{};                // area integral of the response, summed over pairs
		std::array<double, kRough> plainResponseError{}, widenedResponseError{};  // area integral of |coarse - reference|, summed over pairs
	};

	class SpecularAnalyzer final : public IAnalyzer {
	public:
		const AnalyzerInfo& Info() const override
		{
			static const AnalyzerInfo info{ "quality.specular", 1,
				"Specular lobes against the leaves' at runtime cuts, seen through a pixel-sized footprint, with and without the stored normal spread widening roughness" };
			return info;
		}

		void Run(const AnalysisContext& context, MetricSink& sink) const override
		{
			const CachedMesh& mesh = context.mesh;
			if (mesh.Branch() == CLOD_BRANCH_VOXEL) {
				return;
			}
			const ClusterLODPrebuiltData& prebuilt = mesh.Prebuilt();
			const double footprint = context.specularFootprint > 0.0 ? context.specularFootprint : 1.0;
			const double spreadCap = context.specularSpreadCap >= 0.0 ? context.specularSpreadCap : 1.0;

			std::vector<ShadedTriangle> triangles;
			std::vector<Segment> segments;
			bool cutsModelled = true, missingNormals = false;
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
					segment.leaf = stored.refinedGroup < 0;
					segment.leave = ErrorOf(group);
					if (!segment.leaf) {
						if (static_cast<size_t>(stored.refinedGroup) >= prebuilt.groups.size() ||
							(prebuilt.groups[static_cast<size_t>(stored.refinedGroup)].flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u) {
							cutsModelled = false;
							continue;
						}
						segment.enter = ErrorOf(prebuilt.groups[static_cast<size_t>(stored.refinedGroup)]);
					}
					segment.firstTriangle = static_cast<uint32_t>(triangles.size());
					uint32_t meshPage = 0;
					CLodCache::TrianglePage page;
					const bool parsed = stored.meshletCount != 0u && CLodCache::ResolveSegmentMeshPage(prebuilt, groupIndex, stored, meshPage) &&
						CLodCache::TrianglePage::Parse(mesh.Page(meshPage), page);
					missingNormals = missingNormals || (parsed && !page.HasNormals());
					if (parsed && page.HasNormals() && static_cast<uint64_t>(stored.firstMeshletInPage) + stored.meshletCount <= page.MeshletCount()) {
						for (uint32_t index = 0; index < stored.meshletCount; ++index) {
							const uint32_t meshletIndex = stored.firstMeshletInPage + index;
							const CLodCache::TriangleMeshlet meshlet = page.Meshlet(meshletIndex);
							const std::span<const std::byte> spread = page.NormalSpreadBlock(meshletIndex);
							for (uint32_t triangleIndex = 0; triangleIndex < meshlet.triangleCount; ++triangleIndex) {
								const auto corners = page.Triangle(meshlet, triangleIndex);
								ShadedTriangle triangle;
								for (int corner = 0; corner < 3; ++corner) {
									triangle.position[corner] = ToVec(page.Position(meshlet, corners[corner]));
									triangle.normal[corner] = ToVec(page.Normal(meshlet, corners[corner]));
									triangle.spread[corner] = spread.empty() ? 0.0f
										: clod_normal_spread::Decode(clod_normal_spread::VertexByte(spread, corners[corner]));
								}
								const Vec cross = Cross(triangle.position[1] - triangle.position[0], triangle.position[2] - triangle.position[0]);
								segment.area += 0.5 * std::sqrt(Dot(cross, cross));
								triangles.push_back(triangle);
							}
						}
					}
					segment.triangleCount = static_cast<uint32_t>(triangles.size()) - segment.firstTriangle;
					segments.push_back(segment);
				}
			}
			if (missingNormals) {
				sink.Emit("specular.meshes_without_normals", 1.0, Unit::Count, Better::Neutral, {}, Rollup::Sum);
				return;
			}

			// The leaves, indexed with their normals.
			std::vector<std::array<XMFLOAT3, 3>> leafPositions;
			std::vector<std::array<Vec, 3>> leafNormals;
			std::vector<Vec> leafFaces;
			uint64_t simplifiedTriangles = 0;
			Vec low{ std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max() };
			Vec high = low * -1.0;
			for (const Segment& segment : segments) {
				if (!segment.leaf) {
					simplifiedTriangles += segment.triangleCount;
					continue;
				}
				for (uint32_t t = segment.firstTriangle; t < segment.firstTriangle + segment.triangleCount; ++t) {
					const ShadedTriangle& triangle = triangles[t];
					std::array<XMFLOAT3, 3> positions;
					for (int corner = 0; corner < 3; ++corner) {
						const Vec& p = triangle.position[corner];
						positions[corner] = XMFLOAT3(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z));
						low = { std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z) };
						high = { std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z) };
					}
					leafPositions.push_back(positions);
					leafNormals.push_back(triangle.normal);
					leafFaces.push_back(Normalized(Cross(triangle.position[1] - triangle.position[0], triangle.position[2] - triangle.position[0])));
				}
			}
			sink.Emit("specular.meshes", 1.0, Unit::Count, Better::Neutral, {}, Rollup::Sum);
			if (leafPositions.empty() || simplifiedTriangles == 0u || !cutsModelled) {
				if (!cutsModelled) {
					sink.Emit("specular.cut_excluded_meshes", 1.0, Unit::Count, Better::Neutral, {}, Rollup::Sum);
				}
				return;
			}
			const Vec extent = high - low;
			const double meshSize = std::sqrt(Dot(extent, extent));
			const TriangleGrid grid(std::move(leafPositions));

			// The leaf shading normal at the nearest leaf point facing the point's way about as near as the nearest; whatever its
			// facing if none does. Zero when nothing is found or the normal cancels.
			const auto leafNormal = [&](const Vec& point, const Vec& face) -> Vec {
				const double reach = grid.Nearest(point) * 1.1 + meshSize * 1.0e-5;
				TriangleGrid::Hit hit = grid.NearestIf(point, reach, [&](uint32_t index) { return Dot(leafFaces[index], face) > 0.0; });
				if (!hit.Found()) {
					hit = grid.NearestIf(point, reach, [](uint32_t) { return true; });
				}
				if (!hit.Found()) {
					return {};
				}
				const auto& n = leafNormals[hit.index];
				return Normalized(n[0] * hit.weights[0] + n[1] * hit.weights[1] + n[2] * hit.weights[2]);
			};

			std::array<std::array<Vec, 2>, kPairs> local{};
			for (size_t d = 0; d < kPairs; ++d) {
				const auto dir = [](double polar, double azimuth) {
					const double p = polar * std::numbers::pi / 180.0, a = azimuth * std::numbers::pi / 180.0;
					return Vec{ std::sin(p) * std::cos(a), std::sin(p) * std::sin(a), std::cos(p) };
				};
				local[d] = { dir(kDirections[d].viewPolar, kDirections[d].viewAzimuth), dir(kDirections[d].lightPolar, kDirections[d].lightAzimuth) };
			}

			// Sample the simplified triangles once, into per-segment sums.
			std::vector<SegmentSums> sums(segments.size());
			const uint64_t stride = std::max<uint64_t>((simplifiedTriangles + kMaxCoarseTriangles - 1u) / kMaxCoarseTriangles, 1u);
			uint64_t cursor = 0, unmatched = 0, coarseZero = 0;
			double sampledArea = 0.0, spreadArea = 0.0;
			for (uint32_t segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex) {
				const Segment& segment = segments[segmentIndex];
				if (segment.leaf) {
					continue;
				}
				SegmentSums& out = sums[segmentIndex];
				// A pixel at the segment's finest cut: a disc of half its enter error.
				const double radius = 0.5 * footprint * std::max<double>(segment.enter, meshSize * 1.0e-5);
				for (uint32_t t = segment.firstTriangle; t < segment.firstTriangle + segment.triangleCount; ++t) {
					if ((cursor++ % stride) != 0u) {
						continue;
					}
					const ShadedTriangle& triangle = triangles[t];
					const Vec cross = Cross(triangle.position[1] - triangle.position[0], triangle.position[2] - triangle.position[0]);
					const double doubleArea = std::sqrt(Dot(cross, cross));
					if (!(doubleArea > 0.0)) {
						continue;
					}
					const Vec face = cross * (1.0 / doubleArea);
					// The triangle's frame: tangent along its first edge.
					const Vec tangent = Normalized(triangle.position[1] - triangle.position[0]);
					const Vec bitangent = Cross(face, tangent);
					std::array<std::array<Vec, 2>, kPairs> world{};
					for (size_t d = 0; d < kPairs; ++d) {
						for (int k = 0; k < 2; ++k) {
							const Vec& l = local[d][k];
							world[d][k] = tangent * l.x + bitangent * l.y + face * l.z;
						}
					}
					std::array<Vec, kFootprintPoints> offsets{};
					std::array<double, kFootprintPoints> footprintWeights{};
					footprintWeights[0] = 0.25;
					for (size_t k = 1; k < kFootprintPoints; ++k) {
						const double angle = (static_cast<double>(k) - 1.0) * std::numbers::pi / 3.0;
						offsets[k] = (tangent * std::cos(angle) + bitangent * std::sin(angle)) * (radius * std::sqrt(2.0 / 3.0));
						footprintWeights[k] = 0.125;
					}
					const double weight = 0.5 * doubleArea / kSamplePoints.size();
					bool any = false;
					for (const auto& w : kSamplePoints) {
						const Vec point = triangle.position[0] * w[0] + triangle.position[1] * w[1] + triangle.position[2] * w[2];
						Vec coarse = Normalized(triangle.normal[0] * w[0] + triangle.normal[1] * w[1] + triangle.normal[2] * w[2]);
						if (Dot(coarse, coarse) == 0.0) {
							++coarseZero;
							coarse = face * -1.0; // cancels: the worst case, as quality.appearance counts it
						}
						const double v = w[0] * triangle.spread[0] + w[1] * triangle.spread[1] + w[2] * triangle.spread[2];
						// The footprint's leaves (unmatched points drop out, the rest renormalised).
						std::array<Vec, kFootprintPoints> leaves{};
						std::array<double, kFootprintPoints> leafWeights{};
						double weightSum = 0.0;
						for (size_t k = 0; k < kFootprintPoints; ++k) {
							leaves[k] = leafNormal(point + offsets[k], face);
							leafWeights[k] = Dot(leaves[k], leaves[k]) > 0.0 ? footprintWeights[k] : 0.0;
							weightSum += leafWeights[k];
						}
						if (!(weightSum > 0.0)) {
							++unmatched;
							continue;
						}
						any = true;
						double oracleV = 0.0;
						const std::array<double, 2> coarseSlope = Slope(coarse, tangent, bitangent, face);
						std::array<std::array<double, 2>, kFootprintPoints> leafSlopes{};
						for (size_t k = 0; k < kFootprintPoints; ++k) {
							leafWeights[k] /= weightSum;
							if (leafWeights[k] > 0.0) {
								const Vec d = coarse - leaves[k];
								oracleV += leafWeights[k] * std::min(Dot(d, d), kOracleSampleCap);
								leafSlopes[k] = Slope(leaves[k], tangent, bitangent, face);
							}
						}
						out.weight += weight;
						out.spread += weight * v;
						out.oracleSpread += weight * oracleV;
						out.spreadArea += v > 0.0 ? weight : 0.0;
						for (size_t r = 0; r < kRough; ++r) {
							const double alpha = kRoughness[r] * kRoughness[r];
							const double base = 0.5 * alpha * alpha;
							// The reference mixture's self-overlap, shared by the three coarse lobes.
							double rr = 0.0;
							for (size_t i = 0; i < kFootprintPoints; ++i) {
								for (size_t j = 0; j < kFootprintPoints; ++j) {
									if (leafWeights[i] > 0.0 && leafWeights[j] > 0.0) {
										rr += leafWeights[i] * leafWeights[j] * Overlap(leafSlopes[i], leafSlopes[j], base, base);
									}
								}
							}
							const auto lobeDistance = [&](double variance) {
								double rc = 0.0;
								for (size_t k = 0; k < kFootprintPoints; ++k) {
									if (leafWeights[k] > 0.0) {
										rc += leafWeights[k] * Overlap(leafSlopes[k], coarseSlope, base, variance);
									}
								}
								const double cc = 1.0 / (4.0 * std::numbers::pi * variance);
								return std::clamp(1.0 - 2.0 * rc / (rr + cc), 0.0, 1.0);
							};
							const double alpha2 = alpha * alpha;
							const double widenedVariance = 0.5 * std::min(alpha2 + std::min(v, spreadCap * alpha2), 1.0);
							const double oracleVariance = 0.5 * std::min(alpha2 + std::min(oracleV, spreadCap * alpha2), 1.0);
							const double plainDistance = lobeDistance(base);
							out.plainLobe[r] += weight * plainDistance;
							out.widenedLobe[r] += weight * (v > 0.0 ? lobeDistance(widenedVariance) : plainDistance);
							out.oracleLobe[r] += weight * lobeDistance(oracleVariance);
							const double widenedAlpha = std::sqrt(2.0 * widenedVariance);
							for (size_t d = 0; d < kPairs; ++d) {
								const Vec& view = world[d][0];
								const Vec& light = world[d][1];
								double ref = 0.0;
								for (size_t k = 0; k < kFootprintPoints; ++k) {
									ref += leafWeights[k] > 0.0 ? leafWeights[k] * Specular(leaves[k], view, light, alpha) : 0.0;
								}
								const double plain = Specular(coarse, view, light, alpha);
								const double widened = v > 0.0 ? Specular(coarse, view, light, widenedAlpha) : plain;
								out.reference[r] += weight * ref;
								out.plain[r] += weight * plain;
								out.widened[r] += weight * widened;
								out.plainResponseError[r] += weight * std::fabs(plain - ref);
								out.widenedResponseError[r] += weight * std::fabs(widened - ref);
							}
						}
					}
					if (any) {
						sampledArea += 0.5 * doubleArea;
						spreadArea += (triangle.spread[0] + triangle.spread[1] + triangle.spread[2]) > 0.0f ? 0.5 * doubleArea : 0.0;
					}
				}
			}
			sink.Emit("specular.unmatched_samples", static_cast<double>(unmatched), Unit::Count, Better::Lower, {}, Rollup::Sum);
			sink.Emit("specular.coarse_zero_normal_samples", static_cast<double>(coarseZero), Unit::Count, Better::Lower, {}, Rollup::Sum);
			sink.EmitRatio("specular.spread_area_fraction", spreadArea, sampledArea, Unit::Ratio, Better::Neutral, {});

			// The sweep: stored errors and every cut rule.
			const GroupErrorChannels channels = ReadGroupErrorChannels(prebuilt);
			struct Selection {
				Tags tags;
				std::vector<Segment> segments;
			};
			std::vector<Selection> selections;
			selections.push_back({ {}, segments });
			if (context.cutRules != nullptr) {
				for (const CutRule& rule : *context.cutRules) {
					Selection selection{ { { "rule", rule.name } }, segments };
					const auto error = [&](uint32_t group) {
						const float e = SelectionError(prebuilt.groups[group].bounds.error, channels.measured[group], channels.appearance[group], rule);
						return std::isfinite(e) && e < kSentinelError ? e : std::numeric_limits<float>::infinity();
					};
					for (Segment& segment : selection.segments) {
						segment.leave = error(segment.group);
						segment.enter = segment.leaf ? 0.0f : error(static_cast<uint32_t>(segment.refined));
					}
					selections.push_back(std::move(selection));
				}
			}
			for (const Selection& selection : selections) {
				for (int exponent = kMinCutLog2; exponent <= kMaxCutLog2; ++exponent) {
					const double cut = std::ldexp(1.0, exponent);
					double drawnTriangles = 0.0, leafDrawnArea = 0.0, coarseArea = 0.0;
					SegmentSums total;
					for (size_t si = 0; si < selection.segments.size(); ++si) {
						const Segment& segment = selection.segments[si];
						if (!segment.DrawsAt(cut)) continue;
						drawnTriangles += segment.triangleCount;
						if (segment.leaf) {
							leafDrawnArea += segment.area;
							continue;
						}
						coarseArea += segment.area;
						const SegmentSums& s = sums[si];
						total.weight += s.weight;
						total.spread += s.spread;
						total.oracleSpread += s.oracleSpread;
						total.spreadArea += s.spreadArea;
						for (size_t r = 0; r < kRough; ++r) {
							total.plainLobe[r] += s.plainLobe[r];
							total.widenedLobe[r] += s.widenedLobe[r];
							total.oracleLobe[r] += s.oracleLobe[r];
							total.reference[r] += s.reference[r];
							total.plain[r] += s.plain[r];
							total.widened[r] += s.widened[r];
							total.plainResponseError[r] += s.plainResponseError[r];
							total.widenedResponseError[r] += s.widenedResponseError[r];
						}
					}
					// Sampled points stand for the drawn simplified area; leaves drawn as themselves are exact.
					const double scale = total.weight > 0.0 ? coarseArea / total.weight : 0.0;
					const double area = leafDrawnArea + (total.weight > 0.0 ? coarseArea : 0.0);
					Tags tags = selection.tags;
					tags["cut"] = std::format("{:g}", cut);
					sink.EmitRollupOnly("specular.cut.triangles", drawnTriangles, Unit::Count, Better::Lower, tags, Rollup::Sum);
					sink.EmitRatioRollupOnly("specular.cut.spread_mean", total.spread, total.weight, Unit::None, Better::Neutral, tags);
					sink.EmitRatioRollupOnly("specular.cut.oracle_spread_mean", total.oracleSpread, total.weight, Unit::None, Better::Neutral, tags);
					sink.EmitRatioRollupOnly("specular.cut.spread_area_fraction", total.spreadArea * scale, area, Unit::Ratio, Better::Neutral, tags);
					for (size_t r = 0; r < kRough; ++r) {
						Tags rt = tags;
						rt["roughness"] = std::format("{:g}", kRoughness[r]);
						sink.EmitRatioRollupOnly("specular.cut.lobe_distance", total.widenedLobe[r] * scale, area, Unit::Ratio, Better::Lower, rt);
						sink.EmitRatioRollupOnly("specular.cut.plain_lobe_distance", total.plainLobe[r] * scale, area, Unit::Ratio, Better::Lower, rt);
						sink.EmitRatioRollupOnly("specular.cut.oracle_lobe_distance", total.oracleLobe[r] * scale, area, Unit::Ratio, Better::Lower, rt);
						sink.EmitRatioRollupOnly("specular.cut.simplified_lobe_distance", total.widenedLobe[r], total.weight, Unit::Ratio, Better::Lower, rt);
						sink.EmitRatioRollupOnly("specular.cut.plain_simplified_lobe_distance", total.plainLobe[r], total.weight, Unit::Ratio, Better::Lower, rt);
						sink.EmitRatioRollupOnly("specular.cut.oracle_simplified_lobe_distance", total.oracleLobe[r], total.weight, Unit::Ratio, Better::Lower, rt);
						sink.EmitRatioRollupOnly("specular.cut.response_error", total.widenedResponseError[r], total.reference[r], Unit::Ratio, Better::Lower, rt);
						sink.EmitRatioRollupOnly("specular.cut.plain_response_error", total.plainResponseError[r], total.reference[r], Unit::Ratio, Better::Lower, rt);
						sink.EmitRatioRollupOnly("specular.cut.energy_ratio", total.widened[r], total.reference[r], Unit::Ratio, Better::Neutral, rt);
						sink.EmitRatioRollupOnly("specular.cut.plain_energy_ratio", total.plain[r], total.reference[r], Unit::Ratio, Better::Neutral, rt);
					}
				}
			}
		}
	};
}

std::unique_ptr<IAnalyzer> MakeSpecularAnalyzer() { return std::make_unique<SpecularAnalyzer>(); }

}
