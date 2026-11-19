// Texture appearance of what runtime cuts draw, against the leaves' texture: sampled base colour, not UVs. Stage 6 of the appearance
// plan (docs/clod-appearance-stage6.md). quality.texcoords measures UV drift, which cannot tell a slide across a uniform wall from
// one that drags a door's paint over its frame; this samples the asset's own base colour texture (Core/SourceTextures.h) at the
// coarse UV and at the matched leaf's UV and reports the CIE76 colour difference, area-weighted over everything a cut draws.
//
// Filtering follows the cut. At cut T (mesh units) a one-pixel threshold draws these triangles when a pixel is about T wide, so
// both colours are read at the mip level where T is one texel: the coarse triangle's own UV scale for its colour (what the GPU's
// derivatives would pick) and the matched leaf's for the reference. Leaves drawn as themselves count as exact.
//
// UV remap blocks (stage 6 step 2, ClusterLODUvRemap.h) in the pages are read as the material evaluation reads them: a remapped
// triangle's colour comes from the remapped UV at each sample, filtered with the remap's own UV scale there. The remapped samples are
// also measured on their own, with and without the remap, at their segment's finest cut (texture.remap.*).
//
// Bakes (a suite's "texture_bakes", TextureBake) are an offline prototype of baked per-group appearance: baked triangles read a
// per-triangle tile of texels, each resampled from the leaves at its own matched point with its own footprint, nearest texel at
// the mip level the cut selects. It measures what such a bake would recover and how many texels it costs, not a runtime path.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <DirectXMath.h>

#include <BasicRenderer/Assets/ClusterLODUvRemap.h>

#include "Analyzers/Analyzers.h"
#include "Core/Analyzer.h"
#include "Core/ErrorChannels.h"
#include "Core/SourceTextures.h"
#include "Core/TriangleGrid.h"

namespace clodeval {
namespace {
	using DirectX::XMFLOAT2;
	using DirectX::XMFLOAT3;

	constexpr int kMinCutLog2 = -14;
	constexpr int kMaxCutLog2 = 8;
	// Simplified triangles measured per mesh, each at four points; past this they are taken at an even stride.
	constexpr uint64_t kMaxCoarseTriangles = 150000u;
	constexpr float kSentinelError = 1.0e19f;
	// Colour-difference thresholds for the area fractions (CIE76: about 2.3 is just noticeable side by side).
	constexpr std::array<double, 2> kDeltaEOver{ 3.0, 10.0 };
	// Largest bake tile edge, in texels.
	constexpr uint32_t kMaxTileTexels = 1024u;

	constexpr std::array<std::array<double, 3>, 4> kSamplePoints{ {
		{ 1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0 }, { 2.0 / 3.0, 1.0 / 6.0, 1.0 / 6.0 }, { 1.0 / 6.0, 2.0 / 3.0, 1.0 / 6.0 }, { 1.0 / 6.0, 1.0 / 6.0, 2.0 / 3.0 } } };

	float ErrorOf(const ClusterLODGroup& group)
	{
		return std::isfinite(group.bounds.error) && group.bounds.error < kSentinelError ? group.bounds.error : std::numeric_limits<float>::infinity();
	}

	struct UvTriangle {
		std::array<Vec, 3> position;
		std::array<XMFLOAT2, 3> uv;
		std::span<const std::byte> remap; // the cluster's UV remap block (empty: none)
		uint32_t remapTriangle = 0;       // the triangle's index in its cluster
	};

	struct Segment {
		uint32_t group = 0;
		int32_t refined = -1;
		int32_t depth = 0;
		bool leaf = false;
		float enter = 0.0f;
		float leave = 0.0f;
		uint32_t firstTriangle = 0;
		uint32_t triangleCount = 0;
		double area = 0.0;
		bool DrawsAt(double cut) const { return enter <= cut && cut < leave; }
	};

	// One measured point of a simplified triangle and its leaf correspondence.
	struct Sample {
		float weight = 0.0f;  // area it stands for
		uint32_t segment = 0;
		uint32_t triangle = 0;  // index into the triangle list
		float wa = 0.0f, wb = 0.0f, wc = 0.0f;
		float coarseU = 0.0f, coarseV = 0.0f, coarseScale = 0.0f;  // UV units per mesh unit
		float plainU = 0.0f, plainV = 0.0f, plainScale = 0.0f;     // the same without the remap (equal when not remapped)
		bool remapped = false;
		bool remapNearest = false; // the remap took a node, not an interpolation (a sub-triangle straddling charts)
		float leafU = 0.0f, leafV = 0.0f, leafScale = 0.0f;
		bool matched = false;
		// The along-normal correspondence (texture.cut.normal.*): the frontmost leaf crossed by the line through the point along the
		// coarse face normal, what a viewer looking at the coarse surface sees of the leaves; the nearest match where nothing is crossed.
		float normalU = 0.0f, normalV = 0.0f, normalScale = 0.0f;
	};

	struct Leaves {
		std::vector<std::array<XMFLOAT2, 3>> uv;
		std::vector<Vec> face;
		std::vector<float> scale;
	};

	struct Match {
		bool found = false;
		float u = 0.0f, v = 0.0f, scale = 0.0f;
	};

	class TextureAppearanceAnalyzer final : public IAnalyzer {
	public:
		const AnalyzerInfo& Info() const override
		{
			static const AnalyzerInfo info{ "quality.texture", 1,
				"Base-colour difference (CIE76) against the leaves' texture at runtime cuts, and a simulated per-group appearance bake" };
			return info;
		}

		void Run(const AnalysisContext& context, MetricSink& sink) const override
		{
			const CachedMesh& mesh = context.mesh;
			if (context.materials == nullptr || mesh.Branch() == CLOD_BRANCH_VOXEL) {
				return;
			}
			const SourceMaterial* material = context.materials->MaterialForMesh(mesh.Name());
			const SourceTexture* texture = material != nullptr ? context.materials->Image(material->baseColorImage) : nullptr;
			if (material == nullptr || texture == nullptr) {
				sink.Emit("texture.untextured_meshes", 1.0, Unit::Count, Better::Neutral, {}, Rollup::Sum);
				return;
			}
			const ClusterLODPrebuiltData& prebuilt = mesh.Prebuilt();

			std::vector<UvTriangle> triangles;
			std::vector<Segment> segments;
			bool cutsModelled = true, missingUvs = false;
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
						if (static_cast<size_t>(stored.refinedGroup) >= prebuilt.groups.size() ||
							(prebuilt.groups[static_cast<size_t>(stored.refinedGroup)].flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u) {
							cutsModelled = false;
							continue;
						}
						segment.enter = ErrorOf(prebuilt.groups[static_cast<size_t>(stored.refinedGroup)]);
					}
					segment.firstTriangle = static_cast<uint32_t>(triangles.size());
					// UVs need the meshlet's index in its page, so the page is walked directly (as quality.texcoords does).
					uint32_t meshPage = 0;
					CLodCache::TrianglePage page;
					const bool parsed = stored.meshletCount != 0u && CLodCache::ResolveSegmentMeshPage(prebuilt, groupIndex, stored, meshPage) &&
						CLodCache::TrianglePage::Parse(mesh.Page(meshPage), page);
					missingUvs = missingUvs || (parsed && page.UvSetCount() <= material->texCoord);
					if (parsed && page.UvSetCount() > material->texCoord &&
						static_cast<uint64_t>(stored.firstMeshletInPage) + stored.meshletCount <= page.MeshletCount()) {
						for (uint32_t index = 0; index < stored.meshletCount; ++index) {
							const uint32_t meshletIndex = stored.firstMeshletInPage + index;
							const CLodCache::TriangleMeshlet meshlet = page.Meshlet(meshletIndex);
							const std::span<const std::byte> remap = material->texCoord == 0u ? page.UvRemapBlock(meshletIndex) : std::span<const std::byte>{};
							for (uint32_t triangleIndex = 0; triangleIndex < meshlet.triangleCount; ++triangleIndex) {
								const auto corners = page.Triangle(meshlet, triangleIndex);
								UvTriangle triangle;
								triangle.remap = remap;
								triangle.remapTriangle = triangleIndex;
								for (int corner = 0; corner < 3; ++corner) {
									triangle.position[corner] = ToVec(page.Position(meshlet, corners[corner]));
									triangle.uv[corner] = page.Uv(meshletIndex, meshlet, material->texCoord, corners[corner]);
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
			if (missingUvs) {
				sink.Emit("texture.meshes_without_uvs", 1.0, Unit::Count, Better::Neutral, {}, Rollup::Sum);
				return;
			}

			// The leaves, indexed with their UVs.
			std::vector<std::array<XMFLOAT3, 3>> leafPositions;
			Leaves leaves;
			uint64_t simplifiedTriangles = 0;
			double leafArea = 0.0;
			Vec low{ std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max() };
			Vec high = low * -1.0;
			for (const Segment& segment : segments) {
				if (!segment.leaf) {
					simplifiedTriangles += segment.triangleCount;
					continue;
				}
				leafArea += segment.area;
				for (uint32_t t = segment.firstTriangle; t < segment.firstTriangle + segment.triangleCount; ++t) {
					const UvTriangle& triangle = triangles[t];
					std::array<XMFLOAT3, 3> positions;
					for (int corner = 0; corner < 3; ++corner) {
						const Vec& p = triangle.position[corner];
						positions[corner] = XMFLOAT3(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z));
						low = { std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z) };
						high = { std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z) };
					}
					const Vec cross = Cross(triangle.position[1] - triangle.position[0], triangle.position[2] - triangle.position[0]);
					const double doubleArea = std::sqrt(Dot(cross, cross));
					const auto& t3 = triangle.uv;
					const double uvDoubleArea = std::fabs((t3[1].x - t3[0].x) * (t3[2].y - t3[0].y) - (t3[2].x - t3[0].x) * (t3[1].y - t3[0].y));
					leafPositions.push_back(positions);
					leaves.uv.push_back(triangle.uv);
					leaves.face.push_back(doubleArea > 0.0 ? cross * (1.0 / doubleArea) : Vec{});
					leaves.scale.push_back(doubleArea > 0.0 ? static_cast<float>(std::sqrt(uvDoubleArea / doubleArea)) : 0.0f);
				}
			}
			sink.Emit("texture.textured_meshes", 1.0, Unit::Count, Better::Neutral, {}, Rollup::Sum);
			bool meshRemapped = false;
			if (leafPositions.empty() || simplifiedTriangles == 0u || !cutsModelled) {
				if (!cutsModelled) {
					sink.Emit("texture.cut_excluded_meshes", 1.0, Unit::Count, Better::Neutral, {}, Rollup::Sum);
				}
				return;
			}
			const Vec extent = high - low;
			const double meshSize = std::sqrt(Dot(extent, extent));
			const TriangleGrid grid(std::move(leafPositions));

			// The nearest leaf point facing the point's way about as near as the nearest (as quality.normals matches); the nearest
			// whatever its facing if none does.
			const auto match = [&](const Vec& point, const Vec& face) {
				Match result;
				const double reach = grid.Nearest(point) * 1.1 + meshSize * 1.0e-5;
				TriangleGrid::Hit hit = grid.NearestIf(point, reach, [&](uint32_t index) { return Dot(leaves.face[index], face) > 0.0; });
				if (!hit.Found()) {
					hit = grid.NearestIf(point, reach, [](uint32_t) { return true; });
				}
				if (!hit.Found()) {
					return result;
				}
				const auto& t = leaves.uv[hit.index];
				result.found = true;
				result.u = static_cast<float>(t[0].x * hit.weights[0] + t[1].x * hit.weights[1] + t[2].x * hit.weights[2]);
				result.v = static_cast<float>(t[0].y * hit.weights[0] + t[1].y * hit.weights[1] + t[2].y * hit.weights[2]);
				result.scale = leaves.scale[hit.index];
				return result;
			};
			// The frontmost leaf crossed by the line through point along face within reach either way, facing the same way within 60
			// degrees (the renderer's UV remap layout 2 builds with the same rule, independently of this).
			const auto frontmost = [&](const Vec& point, const Vec& face, double reach) {
				Match result;
				double best = -std::numeric_limits<double>::max();
				uint32_t bestIndex = std::numeric_limits<uint32_t>::max();
				grid.ForEachNear(point, reach, [&](uint32_t index) {
					const Vec& normal = leaves.face[index];
					const double facing = Dot(normal, face);
					if (!(facing >= 0.5)) return;
					const auto& triangle = grid.Triangle(index);
					const Vec a = ToVec(triangle[0]), b = ToVec(triangle[1]), c = ToVec(triangle[2]);
					const double t = Dot(a - point, normal) / facing;
					if (!(std::fabs(t) <= reach) || t < best || (t == best && index >= bestIndex)) return;
					const Vec q = point + face * t;
					const Vec e0 = b - a, e1 = c - a, e2 = q - a;
					const double d00 = Dot(e0, e0), d01 = Dot(e0, e1), d11 = Dot(e1, e1), d20 = Dot(e2, e0), d21 = Dot(e2, e1);
					const double denom = d00 * d11 - d01 * d01;
					if (!(std::fabs(denom) > 0.0)) return;
					const double w1 = (d11 * d20 - d01 * d21) / denom, w2 = (d00 * d21 - d01 * d20) / denom, w0 = 1.0 - w1 - w2;
					if (std::min({ w0, w1, w2 }) < -1.0e-4) return;
					best = t;
					bestIndex = index;
					const auto& uv = leaves.uv[index];
					result.found = true;
					result.u = static_cast<float>(uv[0].x * w0 + uv[1].x * w1 + uv[2].x * w2);
					result.v = static_cast<float>(uv[0].y * w0 + uv[1].y * w1 + uv[2].y * w2);
					result.scale = leaves.scale[index];
				});
				return result;
			};
			const double textureSize = static_cast<double>(std::max(texture->width, texture->height));
			const auto color = [&](double u, double v, double footprintUv) {
				return BaseColor(*material, texture, u, v, TextureLod(*texture, footprintUv * material->uvFootprintScale));
			};

			// Sample the simplified triangles once.
			std::vector<Sample> samples;
			std::vector<double> sampledArea(segments.size(), 0.0);
			std::vector<Vec> triangleFace(triangles.size());
			const uint64_t stride = std::max<uint64_t>((simplifiedTriangles + kMaxCoarseTriangles - 1u) / kMaxCoarseTriangles, 1u);
			uint64_t cursor = 0, unmatched = 0, normalFound = 0;
			for (uint32_t segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex) {
				const Segment& segment = segments[segmentIndex];
				if (segment.leaf) {
					continue;
				}
				for (uint32_t t = segment.firstTriangle; t < segment.firstTriangle + segment.triangleCount; ++t) {
					if ((cursor++ % stride) != 0u) {
						continue;
					}
					const UvTriangle& triangle = triangles[t];
					const Vec cross = Cross(triangle.position[1] - triangle.position[0], triangle.position[2] - triangle.position[0]);
					const double doubleArea = std::sqrt(Dot(cross, cross));
					if (!(doubleArea > 0.0)) {
						continue;
					}
					const Vec face = cross * (1.0 / doubleArea);
					triangleFace[t] = face;
					const auto& t3 = triangle.uv;
					const double uvDoubleArea = std::fabs((t3[1].x - t3[0].x) * (t3[2].y - t3[0].y) - (t3[2].x - t3[0].x) * (t3[1].y - t3[0].y));
					const float coarseScale = static_cast<float>(std::sqrt(uvDoubleArea / doubleArea));
					sampledArea[segmentIndex] += 0.5 * doubleArea;
					for (const auto& w : kSamplePoints) {
						Sample sample;
						sample.weight = static_cast<float>(0.5 * doubleArea / kSamplePoints.size());
						sample.segment = segmentIndex;
						sample.triangle = t;
						sample.wa = static_cast<float>(w[0]);
						sample.wb = static_cast<float>(w[1]);
						sample.wc = static_cast<float>(w[2]);
						sample.coarseU = static_cast<float>(t3[0].x * w[0] + t3[1].x * w[1] + t3[2].x * w[2]);
						sample.coarseV = static_cast<float>(t3[0].y * w[0] + t3[1].y * w[1] + t3[2].y * w[2]);
						sample.coarseScale = coarseScale;
						sample.plainU = sample.coarseU;
						sample.plainV = sample.coarseV;
						sample.plainScale = coarseScale;
						if (!triangle.remap.empty()) {
							const clod_uv_remap::Sample r = clod_uv_remap::Evaluate(triangle.remap, triangle.remapTriangle, static_cast<float>(w[1]), static_cast<float>(w[2]));
							if (r.remapped) {
								const double det = static_cast<double>(r.dUvDb1[0]) * r.dUvDb2[1] - static_cast<double>(r.dUvDb1[1]) * r.dUvDb2[0];
								sample.coarseU = r.uv[0];
								sample.coarseV = r.uv[1];
								sample.coarseScale = static_cast<float>(std::sqrt(std::fabs(det) / doubleArea));
								sample.remapped = true;
								sample.remapNearest = !r.interpolated;
								meshRemapped = true;
							}
						}
						const Vec point = triangle.position[0] * w[0] + triangle.position[1] * w[1] + triangle.position[2] * w[2];
						const Match m = match(point, face);
						if (m.found) {
							sample.matched = true;
							sample.leafU = m.u;
							sample.leafV = m.v;
							sample.leafScale = m.scale;
							// Reach: the segment's error or three times the nearest distance, whichever is larger.
							const double nearestDistance = grid.Nearest(point);
							const double reach = std::max({ 3.0 * nearestDistance, static_cast<double>(segment.enter), meshSize * 1.0e-5 });
							const Match n = frontmost(point, face, reach);
							sample.normalU = n.found ? n.u : m.u;
							sample.normalV = n.found ? n.v : m.v;
							sample.normalScale = n.found ? n.scale : m.scale;
							normalFound += n.found ? 1u : 0u;
						}
						else {
							++unmatched;
						}
						samples.push_back(sample);
					}
				}
			}
			sink.Emit("texture.unmatched_samples", static_cast<double>(unmatched), Unit::Count, Better::Lower, {}, Rollup::Sum);
			sink.Emit("texture.normal_crossed_samples", static_cast<double>(normalFound), Unit::Count, Better::Neutral, {}, Rollup::Sum);

			// Colours of one sample at a cut: the reference (leaf texture at the matched point) and what the coarse UV reads.
			const auto reference = [&](const Sample& s, double cut) { return color(s.leafU, s.leafV, cut * s.leafScale); };
			const auto normalReference = [&](const Sample& s, double cut) { return color(s.normalU, s.normalV, cut * s.normalScale); };
			const auto drawnColor = [&](const Sample& s, double cut) { return color(s.coarseU, s.coarseV, cut * s.coarseScale); };
			const auto plainColor = [&](const Sample& s, double cut) { return color(s.plainU, s.plainV, cut * s.plainScale); };

			// Each segment's own unbaked error at its finest cut, for bakes restricted to damaged groups.
			std::vector<double> segmentDeltaE(segments.size(), 0.0);
			{
				std::vector<double> weightSum(segments.size(), 0.0);
				for (const Sample& s : samples) {
					if (!s.matched) continue;
					const double cut = std::max<double>(segments[s.segment].enter, std::ldexp(1.0, kMinCutLog2));
					segmentDeltaE[s.segment] += s.weight * DeltaE(drawnColor(s, cut), reference(s, cut));
					weightSum[s.segment] += s.weight;
				}
				for (size_t i = 0; i < segments.size(); ++i) {
					segmentDeltaE[i] = weightSum[i] > 0.0 ? segmentDeltaE[i] / weightSum[i] : 0.0;
				}
			}

			// Bakes: per-triangle tiles.
			struct BakeState {
				const TextureBake* bake = nullptr;
				std::vector<bool> segmentBaked;
				std::vector<float> texel;      // per triangle: world size of a level-0 texel (0: not baked)
				std::vector<uint32_t> tile;    // per triangle: tile edge in texels
				std::vector<double> segmentBytes;
				double bytes = 0.0, idealBytes = 0.0, bakedTriangles = 0.0;
				std::unordered_map<uint64_t, std::array<float, 4>> texels;
			};
			std::vector<BakeState> bakes;
			if (context.textureBakes != nullptr) {
				for (const TextureBake& bake : *context.textureBakes) {
					BakeState state;
					state.bake = &bake;
					state.segmentBaked.assign(segments.size(), false);
					state.texel.assign(triangles.size(), 0.0f);
					state.tile.assign(triangles.size(), 0u);
					state.segmentBytes.assign(segments.size(), 0.0);
					for (size_t si = 0; si < segments.size(); ++si) {
						const Segment& segment = segments[si];
						state.segmentBaked[si] = !segment.leaf && segment.depth >= bake.minDepth && (bake.minDeltaE <= 0.0 || segmentDeltaE[si] >= bake.minDeltaE);
					}
					// The source texel at a triangle: from the leaf its first (centroid) sample matched.
					std::vector<float> sourceTexel(triangles.size(), 0.0f);
					for (const Sample& s : samples) {
						if (s.matched && sourceTexel[s.triangle] == 0.0f && s.leafScale > 0.0f) {
							sourceTexel[s.triangle] = static_cast<float>(1.0 / (s.leafScale * textureSize));
						}
					}
					for (size_t si = 0; si < segments.size(); ++si) {
						if (!state.segmentBaked[si]) continue;
						const Segment& segment = segments[si];
						for (uint32_t t = segment.firstTriangle; t < segment.firstTriangle + segment.triangleCount; ++t) {
							const UvTriangle& triangle = triangles[t];
							const Vec cross = Cross(triangle.position[1] - triangle.position[0], triangle.position[2] - triangle.position[0]);
							const double area = 0.5 * std::sqrt(Dot(cross, cross));
							double maxEdge = 0.0;
							for (int e = 0; e < 3; ++e) {
								const Vec d = triangle.position[(e + 1) % 3] - triangle.position[e];
								maxEdge = std::max(maxEdge, std::sqrt(Dot(d, d)));
							}
							const double texel = std::max<double>(bake.texelScale * segment.enter, sourceTexel[t]);
							if (!(texel > 0.0) || !(maxEdge > 0.0)) continue;
							const uint32_t edge = static_cast<uint32_t>(std::clamp(std::ceil(maxEdge / texel), 1.0, static_cast<double>(kMaxTileTexels)));
							state.texel[t] = static_cast<float>(maxEdge / edge);
							state.tile[t] = edge;
							// Colour: BC7, one byte per texel and a third more for mips. Remap: two 16-bit UNORM per texel, no mips.
							const double perTexel = bake.remap ? 4.0 : 4.0 / 3.0;
							const double bytes = static_cast<double>(edge) * edge * perTexel;
							state.segmentBytes[si] += bytes;
							state.bytes += bytes;
							state.idealBytes += area / (static_cast<double>(state.texel[t]) * state.texel[t]) * perTexel;
							state.bakedTriangles += 1.0;
						}
					}
					bakes.push_back(std::move(state));
				}
			}
			// A baked sample's colour at a cut: the nearest texel of its triangle's tile at the level the cut selects, each texel
			// resampled from the leaves at its own centre with its own footprint.
			const auto bakedTexel = [&](BakeState& state, uint32_t t, uint32_t level, uint32_t n, uint32_t ix, uint32_t iy) -> std::array<float, 4> {
				const uint64_t key = (static_cast<uint64_t>(t) << 32u) | (static_cast<uint64_t>(level) << 24u) | (static_cast<uint64_t>(ix) << 12u) | iy;
				if (const auto found = state.texels.find(key); found != state.texels.end()) {
					return found->second;
				}
				double u = (ix + 0.5) / n, v = (iy + 0.5) / n;
				if (u + v > 1.0) {
					// The tile's upper half lies outside the triangle; its texels take the nearest point on the long edge (gutter).
					const double sum = u + v;
					u /= sum;
					v /= sum;
				}
				const UvTriangle& triangle = triangles[t];
				const Vec point = triangle.position[0] * (1.0 - u - v) + triangle.position[1] * u + triangle.position[2] * v;
				const Match m = match(point, triangleFace[t]);
				const double footprint = static_cast<double>(state.texel[t]) * static_cast<double>(1u << level);
				const std::array<float, 4> value = m.found ? color(m.u, m.v, footprint * m.scale) : material->baseColorFactor;
				state.texels.emplace(key, value);
				return value;
			};
			// A remap texel: the matched leaf's UV and UV scale at the texel centre (w 1), or nothing found (w 0).
			const auto remapTexel = [&](BakeState& state, uint32_t t, uint32_t n, uint32_t ix, uint32_t iy) -> std::array<float, 4> {
				const uint64_t key = (static_cast<uint64_t>(t) << 32u) | (static_cast<uint64_t>(ix) << 12u) | iy;
				if (const auto found = state.texels.find(key); found != state.texels.end()) {
					return found->second;
				}
				double u = (ix + 0.5) / n, v = (iy + 0.5) / n;
				if (u + v > 1.0) {
					const double sum = u + v;
					u /= sum;
					v /= sum;
				}
				const UvTriangle& triangle = triangles[t];
				const Vec point = triangle.position[0] * (1.0 - u - v) + triangle.position[1] * u + triangle.position[2] * v;
				const Match m = match(point, triangleFace[t]);
				const std::array<float, 4> value{ m.u, m.v, m.scale, m.found ? 1.0f : 0.0f };
				state.texels.emplace(key, value);
				return value;
			};
			// Through a remap tile: the four nearest texels' UVs interpolated where they agree with one continuous mapping (within two
			// texels' worth of UV of each other), the nearest texel's otherwise (a chart boundary inside the texel); the material is
			// then sampled there at the cut's footprint.
			const auto remappedColor = [&](BakeState& state, const Sample& s, double cut) -> std::array<float, 4> {
				const uint32_t t = s.triangle;
				const uint32_t n = state.tile[t];
				const double x = s.wb * n - 0.5, y = s.wc * n - 0.5;
				const double fx = std::floor(x), fy = std::floor(y);
				std::array<std::array<float, 4>, 4> texels{};
				std::array<double, 4> weights{};
				double nearestWeight = -1.0;
				int nearest = 0;
				for (int corner = 0; corner < 4; ++corner) {
					const int dx = corner & 1, dy = corner >> 1;
					weights[corner] = (dx ? x - fx : 1.0 - (x - fx)) * (dy ? y - fy : 1.0 - (y - fy));
					const uint32_t ix = static_cast<uint32_t>(std::clamp<int64_t>(static_cast<int64_t>(fx) + dx, 0, n - 1));
					const uint32_t iy = static_cast<uint32_t>(std::clamp<int64_t>(static_cast<int64_t>(fy) + dy, 0, n - 1));
					texels[corner] = remapTexel(state, t, n, ix, iy);
					if (texels[corner][3] > 0.0f && weights[corner] > nearestWeight) {
						nearestWeight = weights[corner];
						nearest = corner;
					}
				}
				if (nearestWeight < 0.0) {
					return material->baseColorFactor;
				}
				const std::array<float, 4>& near = texels[nearest];
				const double tolerance = 2.0 * state.texel[t] * std::max(near[2], 1.0e-9f);
				double u = 0.0, v = 0.0, scale = 0.0, sum = 0.0;
				bool continuous = true;
				for (int corner = 0; corner < 4; ++corner) {
					const std::array<float, 4>& texel = texels[corner];
					if (weights[corner] <= 0.0) continue;
					if (texel[3] <= 0.0f || std::hypot(texel[0] - near[0], texel[1] - near[1]) > tolerance) {
						continuous = false;
						break;
					}
					u += weights[corner] * texel[0];
					v += weights[corner] * texel[1];
					scale += weights[corner] * texel[2];
					sum += weights[corner];
				}
				if (!continuous || !(sum > 0.0)) {
					u = near[0];
					v = near[1];
					scale = near[2];
				}
				else {
					u /= sum;
					v /= sum;
					scale /= sum;
				}
				return color(u, v, cut * scale);
			};
			// Bilinear within the tile at the mip level nearest the cut's footprint, clamped to the tile.
			const auto bakedColor = [&](BakeState& state, const Sample& s, double cut) -> std::array<float, 4> {
				if (state.bake->remap) {
					return remappedColor(state, s, cut);
				}
				const uint32_t t = s.triangle;
				const double texel = state.texel[t];
				const uint32_t edge = state.tile[t];
				const uint32_t maxLevel = static_cast<uint32_t>(std::floor(std::log2(static_cast<double>(edge))));
				const double ideal = cut > texel ? std::log2(cut / texel) : 0.0;
				const uint32_t level = std::min(static_cast<uint32_t>(std::lround(ideal)), std::min(maxLevel, 15u));
				const uint32_t n = std::max(edge >> level, 1u);
				const double x = s.wb * n - 0.5, y = s.wc * n - 0.5;
				const double fx = std::floor(x), fy = std::floor(y);
				std::array<float, 4> out{};
				for (int dy = 0; dy < 2; ++dy) {
					for (int dx = 0; dx < 2; ++dx) {
						const double weight = (dx ? x - fx : 1.0 - (x - fx)) * (dy ? y - fy : 1.0 - (y - fy));
						if (weight <= 0.0) continue;
						const uint32_t ix = static_cast<uint32_t>(std::clamp<int64_t>(static_cast<int64_t>(fx) + dx, 0, n - 1));
						const uint32_t iy = static_cast<uint32_t>(std::clamp<int64_t>(static_cast<int64_t>(fy) + dy, 0, n - 1));
						const std::array<float, 4> value = bakedTexel(state, t, level, n, ix, iy);
						for (int c = 0; c < 4; ++c) out[c] += static_cast<float>(weight * value[c]);
					}
				}
				return out;
			};

			if (meshRemapped) {
				// The remapped samples alone, at their segment's finest cut, against the same reference with and without the remap.
				double remappedArea = 0.0, sampledCoarseArea = 0.0, measuredArea = 0.0, nearestArea = 0.0;
				double withRemap = 0.0, without = 0.0, withOver = 0.0, withoutOver = 0.0;
				double normalWith = 0.0, normalWithout = 0.0, normalWithOver = 0.0, normalWithoutOver = 0.0;
				for (const Sample& s : samples) {
					sampledCoarseArea += s.weight;
					if (!s.remapped) continue;
					remappedArea += s.weight;
					nearestArea += s.remapNearest ? s.weight : 0.0;
					if (!s.matched) continue;
					const double cut = std::max<double>(segments[s.segment].enter, std::ldexp(1.0, kMinCutLog2));
					const std::array<float, 4> ref = reference(s, cut);
					const double on = DeltaE(drawnColor(s, cut), ref), off = DeltaE(plainColor(s, cut), ref);
					measuredArea += s.weight;
					withRemap += s.weight * on;
					without += s.weight * off;
					withOver += on > kDeltaEOver[0] ? s.weight : 0.0;
					withoutOver += off > kDeltaEOver[0] ? s.weight : 0.0;
					const std::array<float, 4> normalRef = normalReference(s, cut);
					const double normalOn = DeltaE(drawnColor(s, cut), normalRef), normalOff = DeltaE(plainColor(s, cut), normalRef);
					normalWith += s.weight * normalOn;
					normalWithout += s.weight * normalOff;
					normalWithOver += normalOn > kDeltaEOver[0] ? s.weight : 0.0;
					normalWithoutOver += normalOff > kDeltaEOver[0] ? s.weight : 0.0;
				}
				sink.Emit("texture.remap.meshes", 1.0, Unit::Count, Better::Neutral, {}, Rollup::Sum);
				sink.EmitRatio("texture.remap.area_fraction", remappedArea, sampledCoarseArea, Unit::Ratio, Better::Neutral, {});
				sink.EmitRatio("texture.remap.nearest_area_fraction", nearestArea, remappedArea, Unit::Ratio, Better::Neutral, {});
				sink.EmitRatio("texture.remap.delta_e_mean", withRemap, measuredArea, Unit::None, Better::Lower, {});
				sink.EmitRatio("texture.remap.plain_delta_e_mean", without, measuredArea, Unit::None, Better::Lower, {});
				sink.EmitRatio("texture.remap.delta_e_over_area_fraction", withOver, measuredArea, Unit::Ratio, Better::Lower, { { "over", "3" } });
				sink.EmitRatio("texture.remap.plain_delta_e_over_area_fraction", withoutOver, measuredArea, Unit::Ratio, Better::Lower, { { "over", "3" } });
				sink.EmitRatio("texture.remap.normal.delta_e_mean", normalWith, measuredArea, Unit::None, Better::Lower, {});
				sink.EmitRatio("texture.remap.normal.plain_delta_e_mean", normalWithout, measuredArea, Unit::None, Better::Lower, {});
				sink.EmitRatio("texture.remap.normal.delta_e_over_area_fraction", normalWithOver, measuredArea, Unit::Ratio, Better::Lower, { { "over", "3" } });
				sink.EmitRatio("texture.remap.normal.plain_delta_e_over_area_fraction", normalWithoutOver, measuredArea, Unit::Ratio, Better::Lower, { { "over", "3" } });
			}
			for (BakeState& state : bakes) {
				const Tags tags{ { "bake", state.bake->name } };
				sink.Emit("texture.bake.bytes", state.bytes, Unit::Bytes, Better::Lower, tags, Rollup::Sum);
				sink.Emit("texture.bake.ideal_bytes", state.idealBytes, Unit::Bytes, Better::Lower, tags, Rollup::Sum);
				sink.Emit("texture.bake.triangles", state.bakedTriangles, Unit::Count, Better::Neutral, tags, Rollup::Sum);
			}

			// The sweep: stored errors and every cut rule, each unbaked and with each bake.
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
				// Bakes do not change what is selected, so they are swept under the stored errors only.
				const int bakeCount = selection.tags.empty() ? static_cast<int>(bakes.size()) : 0;
				for (int bakeIndex = -1; bakeIndex < bakeCount; ++bakeIndex) {
					BakeState* bake = bakeIndex >= 0 ? &bakes[static_cast<size_t>(bakeIndex)] : nullptr;
					Tags baseTags = selection.tags;
					if (bake != nullptr) {
						baseTags["bake"] = bake->bake->name;
					}
					for (int exponent = kMinCutLog2; exponent <= kMaxCutLog2; ++exponent) {
						const double cut = std::ldexp(1.0, exponent);
						double drawnTriangles = 0.0, leafDrawnArea = 0.0, coarseArea = 0.0, coarseSampledArea = 0.0, bakeBytes = 0.0;
						std::vector<bool> drawn(selection.segments.size(), false);
						for (size_t si = 0; si < selection.segments.size(); ++si) {
							const Segment& segment = selection.segments[si];
							if (!segment.DrawsAt(cut)) continue;
							drawn[si] = true;
							drawnTriangles += segment.triangleCount;
							if (segment.leaf) {
								leafDrawnArea += segment.area;
							}
							else {
								coarseArea += segment.area;
								coarseSampledArea += sampledArea[si];
								if (bake != nullptr && bake->segmentBaked[si]) bakeBytes += bake->segmentBytes[si];
							}
						}
						double deltaE = 0.0, measured = 0.0, alphaMismatch = 0.0, normalDeltaE = 0.0;
						std::array<double, kDeltaEOver.size()> over{}, normalOver{};
						for (const Sample& s : samples) {
							if (!drawn[s.segment] || !s.matched) continue;
							const std::array<float, 4> ref = reference(s, cut);
							const std::array<float, 4> got = bake != nullptr && bake->segmentBaked[s.segment] && bake->tile[s.triangle] != 0u
								? bakedColor(*bake, s, cut)
								: drawnColor(s, cut);
							const double d = DeltaE(got, ref);
							deltaE += s.weight * d;
							measured += s.weight;
							for (size_t level = 0; level < kDeltaEOver.size(); ++level) over[level] += d > kDeltaEOver[level] ? s.weight : 0.0;
							if (material->masked && ((got[3] >= material->alphaCutoff) != (ref[3] >= material->alphaCutoff))) alphaMismatch += s.weight;
							if (bake == nullptr) {
								const double dn = DeltaE(got, normalReference(s, cut));
								normalDeltaE += s.weight * dn;
								for (size_t level = 0; level < kDeltaEOver.size(); ++level) normalOver[level] += dn > kDeltaEOver[level] ? s.weight : 0.0;
							}
						}
						// Sampled points stand for the drawn simplified area; leaves drawn as themselves are exact.
						const double scale = measured > 0.0 ? coarseArea / measured : 0.0;
						const double area = leafDrawnArea + (measured > 0.0 ? coarseArea : 0.0);
						Tags tags = baseTags;
						tags["cut"] = std::format("{:g}", cut);
						sink.EmitRollupOnly("texture.cut.triangles", drawnTriangles, Unit::Count, Better::Lower, tags, Rollup::Sum);
						sink.EmitRatioRollupOnly("texture.cut.delta_e_mean", deltaE * scale, area, Unit::None, Better::Lower, tags);
						sink.EmitRatioRollupOnly("texture.cut.simplified_delta_e_mean", deltaE, measured, Unit::None, Better::Lower, tags);
						for (size_t level = 0; level < kDeltaEOver.size(); ++level) {
							Tags overTags = tags;
							overTags["over"] = std::format("{:g}", kDeltaEOver[level]);
							sink.EmitRatioRollupOnly("texture.cut.delta_e_over_area_fraction", over[level] * scale, area, Unit::Ratio, Better::Lower, overTags);
						}
						if (material->masked) {
							sink.EmitRatioRollupOnly("texture.cut.alpha_mismatch_area_fraction", alphaMismatch * scale, area, Unit::Ratio, Better::Lower, tags);
						}
						if (bake == nullptr) {
							sink.EmitRatioRollupOnly("texture.cut.normal.delta_e_mean", normalDeltaE * scale, area, Unit::None, Better::Lower, tags);
							for (size_t level = 0; level < kDeltaEOver.size(); ++level) {
								Tags overTags = tags;
								overTags["over"] = std::format("{:g}", kDeltaEOver[level]);
								sink.EmitRatioRollupOnly("texture.cut.normal.delta_e_over_area_fraction", normalOver[level] * scale, area, Unit::Ratio, Better::Lower, overTags);
							}
						}
						if (bake != nullptr) {
							sink.EmitRollupOnly("texture.cut.bake_bytes", bakeBytes, Unit::Bytes, Better::Lower, tags, Rollup::Sum);
						}
						// Per mesh where it draws simplified triangles, unbaked under the stored errors: to find the meshes behind a rollup.
						if (bake == nullptr && selection.tags.empty() && measured > 0.0) {
							sink.EmitRatio("texture.cut.mesh_delta_e_mean", deltaE * scale, area, Unit::None, Better::Lower, tags);
						}
						(void)coarseSampledArea;
					}
				}
			}
		}
	};
}

std::unique_ptr<IAnalyzer> MakeTextureAppearanceAnalyzer() { return std::make_unique<TextureAppearanceAnalyzer>(); }

}
