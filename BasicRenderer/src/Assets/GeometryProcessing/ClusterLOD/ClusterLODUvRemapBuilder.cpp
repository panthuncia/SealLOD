#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODUvRemapBuilder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace clod_detail {

	namespace {

		// The texture term's sample points (ClusterLODTopology.cpp), at which a triangle is judged to need a remap.
		constexpr std::array<std::array<float, 3>, 4> kSamples{ {
			{ 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f },
			{ 4.0f / 6.0f, 1.0f / 6.0f, 1.0f / 6.0f },
			{ 1.0f / 6.0f, 4.0f / 6.0f, 1.0f / 6.0f },
			{ 1.0f / 6.0f, 1.0f / 6.0f, 4.0f / 6.0f },
		} };

		// A remapped triangle has at least this many nodes per edge: with only its corners (2) a remap holds what the corners
		// already do and resolves nothing inside.
		constexpr uint32_t kMinNodesPerEdge = 4u;

		inline const float* At(const float* base, size_t strideBytes, uint32_t index)
		{
			return reinterpret_cast<const float*>(reinterpret_cast<const std::byte*>(base) + static_cast<size_t>(index) * strideBytes);
		}

		inline float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

		// Unit face normal of abc, returning its area (zero normal for degenerate triangles).
		float FaceNormalArea(const float* a, const float* b, const float* c, float* normal)
		{
			const float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
			const float ac[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
			normal[0] = ab[1] * ac[2] - ab[2] * ac[1];
			normal[1] = ab[2] * ac[0] - ab[0] * ac[2];
			normal[2] = ab[0] * ac[1] - ab[1] * ac[0];
			const float length = std::sqrt(Dot3(normal, normal));
			if (!(length > 0.0f))
			{
				normal[0] = normal[1] = normal[2] = 0.0f;
				return 0.0f;
			}
			normal[0] /= length;
			normal[1] /= length;
			normal[2] /= length;
			return 0.5f * length;
		}

// The original's UV set 0 at barycentrics of its triangle.
		bool SourceUvAt(const TextureCheckSource& source, uint32_t triangle, const float* weights, UvRemapMatch& out)
		{
			if (static_cast<size_t>(triangle) * 3u + 2u >= source.indexCount)
			{
				return false;
			}
			const uint32_t* s = &source.indices[static_cast<size_t>(triangle) * 3u];
			const float* q[3] = { At(source.positions, source.positionStride, s[0]), At(source.positions, source.positionStride, s[1]),
				At(source.positions, source.positionStride, s[2]) };
			const float* t[3] = { At(source.uvs, source.uvStride, s[0]), At(source.uvs, source.uvStride, s[1]), At(source.uvs, source.uvStride, s[2]) };
			float face[3];
			const float area = FaceNormalArea(q[0], q[1], q[2], face);
			const float uvArea = 0.5f * std::fabs((t[1][0] - t[0][0]) * (t[2][1] - t[0][1]) - (t[2][0] - t[0][0]) * (t[1][1] - t[0][1]));
			if (!(area > 0.0f) || !std::isfinite(uvArea))
			{
				return false;
			}
			float w[3] = { std::max(weights[0], 0.0f), std::max(weights[1], 0.0f), std::max(weights[2], 0.0f) };
			const float sum = w[0] + w[1] + w[2];
			if (!(sum > 0.0f))
			{
				return false;
			}
			for (float& value : w)
			{
				value /= sum;
			}
			out.uv[0] = t[0][0] * w[0] + t[1][0] * w[1] + t[2][0] * w[2];
			out.uv[1] = t[0][1] * w[0] + t[1][1] * w[1] + t[2][1] * w[2];
			out.chart = source.charts != nullptr ? source.charts[s[0]] : 0u;
			out.scale = std::sqrt(uvArea / area);
			out.found = true;
			return true;
		}

		float Median(std::vector<float>& values)
		{
			if (values.empty())
			{
				return 0.0f;
			}
			const size_t middle = values.size() / 2u;
			std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle), values.end());
			return values[middle];
		}

		// The grid triangle's nodes are matched (found or not): fill the unmatched from matched neighbours, and set its UVs, charts,
		// interpolation bits and step codes (stage 6 step 2; shared by the grid layout and layout 2's grid fallback).
		void FinishGridTriangle(uint32_t n, float gridEdge, std::vector<UvRemapMatch>& nodes, const std::vector<std::array<float, 3>>& nodePositions,
			std::vector<uint8_t>& matched, clod_uv_remap::TriangleRemap& remap, std::vector<uint32_t>& charts, UvRemapCluster& result,
			std::vector<float>& goodSteps, std::vector<float>& scaleSteps)
		{
			using namespace clod_uv_remap;
			const uint32_t nodeCount = NodeCount(n);
			// Nodes without a match take the nearest matched node's (grid distance, lowest index on ties).
			matched.assign(nodeCount, 0u);
			for (uint32_t index = 0; index < nodeCount; ++index)
			{
				matched[index] = nodes[index].found ? 1u : 0u;
			}
			for (uint32_t j = 0; j < n; ++j)
			{
				for (uint32_t i = 0; i + j < n; ++i)
				{
					const uint32_t index = NodeIndex(n, i, j);
					if (matched[index] != 0u)
					{
						continue;
					}
					++result.nodesUnmatched;
					int bestDistance = std::numeric_limits<int>::max();
					uint32_t best = UINT32_MAX;
					for (uint32_t jj = 0; jj < n; ++jj)
					{
						for (uint32_t ii = 0; ii + jj < n; ++ii)
						{
							const uint32_t other = NodeIndex(n, ii, jj);
							if (matched[other] == 0u)
							{
								continue;
							}
							const int di = static_cast<int>(ii) - static_cast<int>(i);
							const int dj = static_cast<int>(jj) - static_cast<int>(j);
							const int distance = (std::abs(di) + std::abs(dj) + std::abs(di + dj)) / 2;
							if (distance < bestDistance)
							{
								bestDistance = distance;
								best = other;
							}
						}
					}
					if (best != UINT32_MAX)
					{
						UvRemapMatch copy = nodes[best];
						copy.alongNormal = false;
						nodes[index] = copy;
					}
				}
			}

			remap.n = n;
			remap.uvs.resize(nodeCount);
			charts.resize(nodeCount);
			for (uint32_t index = 0; index < nodeCount; ++index)
			{
				remap.uvs[index] = { nodes[index].uv[0], nodes[index].uv[1] };
				charts[index] = nodes[index].chart;
				result.nodesAlongNormal += nodes[index].alongNormal ? 1u : 0u;
				if (nodes[index].scale > 0.0f)
				{
					scaleSteps.push_back(nodes[index].scale * gridEdge);
				}
			}
			result.nodes += nodeCount;

			// A sub-triangle interpolates its nodes when they come from one chart and their UVs are as near as the original's
			// mapping puts them (within three times the larger UV scale), which also keeps seams inside a chart (a wrapped
			// cylinder) from being interpolated across.
			remap.interpolate.assign(SubTriangleCount(n), 0u);
			const auto good = [&](uint32_t a, uint32_t b, uint32_t c) {
				if (nodes[a].chart != nodes[b].chart || nodes[a].chart != nodes[c].chart)
				{
					return false;
				}
				const uint32_t corners[3] = { a, b, c };
				for (int e = 0; e < 3; ++e)
				{
					const UvRemapMatch& x = nodes[corners[e]];
					const UvRemapMatch& y = nodes[corners[(e + 1) % 3]];
					const std::array<float, 3>& px = nodePositions[corners[e]];
					const std::array<float, 3>& py = nodePositions[corners[(e + 1) % 3]];
					const float d[3] = { px[0] - py[0], px[1] - py[1], px[2] - py[2] };
					const float uvDistance = std::hypot(x.uv[0] - y.uv[0], x.uv[1] - y.uv[1]);
					if (!(uvDistance <= 3.0f * std::max(x.scale, y.scale) * std::sqrt(Dot3(d, d)) + 1.0e-6f))
					{
						return false;
					}
				}
				for (int e = 0; e < 3; ++e)
				{
					const UvRemapMatch& x = nodes[corners[e]];
					const UvRemapMatch& y = nodes[corners[(e + 1) % 3]];
					goodSteps.push_back(std::hypot(x.uv[0] - y.uv[0], x.uv[1] - y.uv[1]));
				}
				return true;
			};
			// Each node's step code: the grid neighbour along i and along j in its own chart, as near in UV as the original's mapping
			// puts them, that gives its local gradient where it extrapolates (the next one preferred).
			remap.steps.assign(nodeCount, 0u);
			const auto continuous = [&](uint32_t x, uint32_t y) {
				const UvRemapMatch& p0 = nodes[x];
				const UvRemapMatch& p1 = nodes[y];
				if (p0.chart != p1.chart)
				{
					return false;
				}
				const std::array<float, 3>& q0 = nodePositions[x];
				const std::array<float, 3>& q1 = nodePositions[y];
				const float d[3] = { q0[0] - q1[0], q0[1] - q1[1], q0[2] - q1[2] };
				return std::hypot(p0.uv[0] - p1.uv[0], p0.uv[1] - p1.uv[1]) <= 3.0f * std::max(p0.scale, p1.scale) * std::sqrt(Dot3(d, d)) + 1.0e-6f;
			};
			for (uint32_t j = 0; j < n; ++j)
			{
				for (uint32_t i = 0; i + j < n; ++i)
				{
					const uint32_t index = NodeIndex(n, i, j);
					uint8_t code = 0u;
					if (i + j + 1u < n && continuous(index, NodeIndex(n, i + 1u, j)))
					{
						code |= static_cast<uint8_t>(kStepNext);
					}
					else if (i > 0u && continuous(index, NodeIndex(n, i - 1u, j)))
					{
						code |= static_cast<uint8_t>(kStepPrevious);
					}
					if (i + j + 1u < n && continuous(index, NodeIndex(n, i, j + 1u)))
					{
						code |= static_cast<uint8_t>(kStepNext << 2u);
					}
					else if (j > 0u && continuous(index, NodeIndex(n, i, j - 1u)))
					{
						code |= static_cast<uint8_t>(kStepPrevious << 2u);
					}
					remap.steps[index] = code;
				}
			}
			for (uint32_t j = 0; j + 1u < n; ++j)
			{
				for (uint32_t i = 0; i + j + 1u < n; ++i)
				{
					const auto classify = [&](uint32_t sub, uint32_t a, uint32_t b, uint32_t c) {
						const bool interpolate = good(a, b, c);
						remap.interpolate[sub] = interpolate ? 1u : 0u;
						++result.subTriangles;
						const bool oneChart = nodes[a].chart == nodes[b].chart && nodes[a].chart == nodes[c].chart;
						result.subTrianglesStraddlingCharts += oneChart ? 0u : 1u;
						result.subTrianglesStraddlingSeams += oneChart && !interpolate ? 1u : 0u;
					};
					classify(SubTriangleIndex(n, i, j, false), NodeIndex(n, i, j), NodeIndex(n, i + 1u, j), NodeIndex(n, i, j + 1u));
					if (i + j + 2u < n)
					{
						classify(SubTriangleIndex(n, i, j, true), NodeIndex(n, i + 1u, j + 1u), NodeIndex(n, i, j + 1u), NodeIndex(n, i + 1u, j));
					}
				}
			}
			++result.trianglesRemapped;
		}

	}

	UvRemapMatch MatchUvRemapPoint(const SimplifyValidator& original, const TextureCheckSource& source, const float point[3], const float face[3],
		float reach, std::vector<uint32_t>& slots)
	{
		UvRemapMatch match;
		const SimplifyTriangleBvh& bvh = original.OriginalBvh();
		(void)slots;
		SimplifyTriangleBvh::Match crossing;
		if (bvh.NearestCrossing(point, face, reach, 0.5f, crossing) && SourceUvAt(source, crossing.triangle, crossing.barycentric, match))
		{
			match.alongNormal = true;
			return match;
		}
		SimplifyTriangleBvh::Match nearest;
		if (bvh.MatchFacing(point, face, 2.0f * reach, original.Diagonal() * 1.0e-5f, nearest))
		{
			SourceUvAt(source, nearest.triangle, nearest.barycentric, match);
		}
		return match;
	}

	// Layout 2: where nothing is crossed within the reach, the crossing nearest the coarse surface within this many reaches.
	static constexpr float kUvRemapExtendedReach = 4.0f;

	UvRemapMatch MatchUvRemapFrontmost(const SimplifyValidator& original, const TextureCheckSource& source, const float point[3], const float face[3],
		float reach)
	{
		UvRemapMatch match;
		const SimplifyTriangleBvh& bvh = original.OriginalBvh();
		SimplifyTriangleBvh::Match crossing;
		if (bvh.FrontmostCrossing(point, face, reach, 0.5f, crossing) && SourceUvAt(source, crossing.triangle, crossing.barycentric, match))
		{
			match.alongNormal = true;
			return match;
		}
		// Nothing within reach (a recess deeper than the measured deviation, which is a nearest-point distance): the crossing nearest
		// the coarse surface within the extended reach.
		if (bvh.NearestCrossing(point, face, kUvRemapExtendedReach * reach, 0.5f, crossing) && SourceUvAt(source, crossing.triangle, crossing.barycentric, match))
		{
			match.alongNormal = true;
			return match;
		}
		SimplifyTriangleBvh::Match nearest;
		if (bvh.MatchFacing(point, face, 2.0f * reach, original.Diagonal() * 1.0e-5f, nearest))
		{
			SourceUvAt(source, nearest.triangle, nearest.barycentric, match);
		}
		return match;
	}

	namespace {

		// Layout 2: an original triangle facing a coarse triangle within reach, projected along the coarse normal into the coarse
		// triangle's barycentric plane (b1, b2), with the exact affine maps from there to its UVs and to its offset along the normal.
		struct RegionCandidate
		{
			uint32_t source = 0;
			uint32_t chart = 0;
			float scale = 0.0f;            // UV units per mesh unit of the original triangle
			float origin[2] = {};          // projected corner 0
			float inverse[4] = {};         // (lambda1, lambda2) = inverse * (b - origin)
			float height[3] = {};          // the corners' offsets along the coarse normal
			float corner[3][2] = {};       // projected corners
			float uv[3][2] = {};
			float low[2] = {}, high[2] = {};
			clod_uv_remap::RegionMap map;  // b -> uv
		};

		// Which original triangle a viewer looking at the coarse triangle sees at each of its points: the frontmost candidate covering
		// it within reach (the same rule as MatchUvRemapFrontmost, evaluated on the projected candidates).
		class RegionLabeler
		{
		public:
			static constexpr int kBins = 8;

			void Build(const SimplifyValidator& original, const TextureCheckSource& source, const float* const p[3], const float face[3], float reach,
				std::vector<uint32_t>& slots)
			{
				m_candidates.clear();
				for (std::vector<uint32_t>& bin : m_bins)
				{
					bin.clear();
				}
				m_reach = reach;
				const float e1[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
				const float e2[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
				const float g11 = Dot3(e1, e1), g12 = Dot3(e1, e2), g22 = Dot3(e2, e2);
				const float det = g11 * g22 - g12 * g12;
				if (!(det > 0.0f))
				{
					return;
				}
				float boundsMin[3], boundsMax[3];
				for (int axis = 0; axis < 3; ++axis)
				{
					boundsMin[axis] = std::min(std::min(p[0][axis], p[1][axis]), p[2][axis]) - kUvRemapExtendedReach * reach;
					boundsMax[axis] = std::max(std::max(p[0][axis], p[1][axis]), p[2][axis]) + kUvRemapExtendedReach * reach;
				}
				const SimplifyTriangleBvh& bvh = original.OriginalBvh();
				slots.clear();
				bvh.CollectInBox(boundsMin, boundsMax, slots);
				constexpr float kMargin = 0.02f;
				for (const uint32_t slot : slots)
				{
					const SimplifyTriangleBvh::Triangle& triangle = bvh.TriangleAt(slot);
					if (!(Dot3(triangle.normal, face) >= 0.5f) || static_cast<size_t>(triangle.source) * 3u + 2u >= source.indexCount)
					{
						continue;
					}
					const uint32_t* s = &source.indices[static_cast<size_t>(triangle.source) * 3u];
					RegionCandidate c;
					c.source = triangle.source;
					c.chart = source.charts != nullptr ? source.charts[s[0]] : 0u;
					float heightLow = std::numeric_limits<float>::max(), heightHigh = -std::numeric_limits<float>::max();
					c.low[0] = c.low[1] = std::numeric_limits<float>::max();
					c.high[0] = c.high[1] = -std::numeric_limits<float>::max();
					for (int k = 0; k < 3; ++k)
					{
						const float* v = At(source.positions, source.positionStride, s[k]);
						const float d[3] = { v[0] - p[0][0], v[1] - p[0][1], v[2] - p[0][2] };
						const float r1 = Dot3(d, e1), r2 = Dot3(d, e2);
						c.corner[k][0] = (g22 * r1 - g12 * r2) / det;
						c.corner[k][1] = (g11 * r2 - g12 * r1) / det;
						c.height[k] = Dot3(d, face);
						const float* t = At(source.uvs, source.uvStride, s[k]);
						c.uv[k][0] = t[0];
						c.uv[k][1] = t[1];
						heightLow = std::min(heightLow, c.height[k]);
						heightHigh = std::max(heightHigh, c.height[k]);
						for (int axis = 0; axis < 2; ++axis)
						{
							c.low[axis] = std::min(c.low[axis], c.corner[k][axis]);
							c.high[axis] = std::max(c.high[axis], c.corner[k][axis]);
						}
					}
					if (c.high[0] < -kMargin || c.high[1] < -kMargin || c.low[0] > 1.0f + kMargin || c.low[1] > 1.0f + kMargin ||
						c.low[0] + c.low[1] > 1.0f + kMargin || heightHigh < -kUvRemapExtendedReach * reach || heightLow > kUvRemapExtendedReach * reach)
					{
						continue;
					}
					const float ax = c.corner[1][0] - c.corner[0][0], ay = c.corner[1][1] - c.corner[0][1];
					const float bx = c.corner[2][0] - c.corner[0][0], by = c.corner[2][1] - c.corner[0][1];
					const float projected = ax * by - bx * ay;
					if (!(std::fabs(projected) > 1.0e-12f) || !std::isfinite(projected))
					{
						continue;
					}
					c.inverse[0] = by / projected;
					c.inverse[1] = -bx / projected;
					c.inverse[2] = -ay / projected;
					c.inverse[3] = ax / projected;
					c.origin[0] = c.corner[0][0];
					c.origin[1] = c.corner[0][1];
					for (int axis = 0; axis < 2; ++axis)
					{
						const float du1 = c.uv[1][axis] - c.uv[0][axis];
						const float du2 = c.uv[2][axis] - c.uv[0][axis];
						c.map.dUvDb1[axis] = du1 * c.inverse[0] + du2 * c.inverse[2];
						c.map.dUvDb2[axis] = du1 * c.inverse[1] + du2 * c.inverse[3];
						c.map.origin[axis] = c.uv[0][axis] - c.map.dUvDb1[axis] * c.origin[0] - c.map.dUvDb2[axis] * c.origin[1];
					}
					float sourceFace[3];
					const float sourceArea = FaceNormalArea(At(source.positions, source.positionStride, s[0]), At(source.positions, source.positionStride, s[1]),
						At(source.positions, source.positionStride, s[2]), sourceFace);
					const float uvArea = 0.5f * std::fabs((c.uv[1][0] - c.uv[0][0]) * (c.uv[2][1] - c.uv[0][1]) - (c.uv[2][0] - c.uv[0][0]) * (c.uv[1][1] - c.uv[0][1]));
					c.scale = sourceArea > 0.0f && std::isfinite(uvArea) ? std::sqrt(uvArea / sourceArea) : 0.0f;
					m_candidates.push_back(c);
				}
				// By source index, so ties go to the lowest index whatever order the BVH listed them in.
				std::sort(m_candidates.begin(), m_candidates.end(), [](const RegionCandidate& a, const RegionCandidate& b) { return a.source < b.source; });
				for (uint32_t index = 0; index < m_candidates.size(); ++index)
				{
					const RegionCandidate& c = m_candidates[index];
					const int x0 = Bin(c.low[0]), x1 = Bin(c.high[0]), y0 = Bin(c.low[1]), y1 = Bin(c.high[1]);
					for (int y = y0; y <= y1; ++y)
					{
						for (int x = x0; x <= x1; ++x)
						{
							m_bins[static_cast<size_t>(y * kBins + x)].push_back(index);
						}
					}
				}
			}

			bool Empty() const { return m_candidates.empty(); }
			size_t Count() const { return m_candidates.size(); }
			const RegionCandidate& Candidate(int index) const { return m_candidates[static_cast<size_t>(index)]; }

			// The frontmost candidate covering (b1, b2) within reach; else the one nearest the coarse surface within the extended reach
			// (as MatchUvRemapFrontmost); else -1.
			int Label(float b1, float b2) const
			{
				int best = -1, nearest = -1;
				float bestHeight = 0.0f, nearestDistance = 0.0f;
				for (const uint32_t index : m_bins[static_cast<size_t>(Bin(b2) * kBins + Bin(b1))])
				{
					const RegionCandidate& c = m_candidates[index];
					const float d1 = b1 - c.origin[0], d2 = b2 - c.origin[1];
					const float l1 = c.inverse[0] * d1 + c.inverse[1] * d2;
					const float l2 = c.inverse[2] * d1 + c.inverse[3] * d2;
					const float l0 = 1.0f - l1 - l2;
					if (std::min(std::min(l0, l1), l2) < -1.0e-4f)
					{
						continue;
					}
					const float height = c.height[0] * l0 + c.height[1] * l1 + c.height[2] * l2;
					if (!(std::fabs(height) <= kUvRemapExtendedReach * m_reach))
					{
						continue;
					}
					if (std::fabs(height) < nearestDistance || nearest < 0)
					{
						nearest = static_cast<int>(index);
						nearestDistance = std::fabs(height);
					}
					if (!(height >= -m_reach && height <= m_reach))
					{
						continue;
					}
					if (best < 0 || height > bestHeight)
					{
						best = static_cast<int>(index);
						bestHeight = height;
					}
				}
				return best >= 0 ? best : nearest;
			}

		private:
			static int Bin(float value) { return std::clamp(static_cast<int>(std::floor(value * static_cast<float>(kBins))), 0, kBins - 1); }

			std::vector<RegionCandidate> m_candidates;
			std::array<std::vector<uint32_t>, static_cast<size_t>(kBins * kBins)> m_bins;
			float m_reach = 0.0f;
		};

		// Layout 2 judges whether a triangle needs a remap at these ten points (the texture term's four, and six more towards the
		// corners and edges), so a chart that covers the inside of a triangle whose corners lie in another is not missed.
		constexpr std::array<std::array<float, 3>, 10> kNeededSamples{ {
			{ 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f },
			{ 4.0f / 6.0f, 1.0f / 6.0f, 1.0f / 6.0f },
			{ 1.0f / 6.0f, 4.0f / 6.0f, 1.0f / 6.0f },
			{ 1.0f / 6.0f, 1.0f / 6.0f, 4.0f / 6.0f },
			{ 0.45f, 0.45f, 0.10f },
			{ 0.45f, 0.10f, 0.45f },
			{ 0.10f, 0.45f, 0.45f },
			{ 0.80f, 0.10f, 0.10f },
			{ 0.10f, 0.80f, 0.10f },
			{ 0.10f, 0.10f, 0.80f },
		} };

		// Reference points per edge of a region triangle: 16 steps, so the nodes of the cell grids tried (2, 3, 5, 9, 17 per edge) fall
		// on them.
		constexpr uint32_t kReferenceLattice = 17u;
		constexpr std::array<uint32_t, 5> kRegionGrids{ 2u, 3u, 5u, 9u, 17u };
		// A chart without one planar mapping over a triangle may be cut into this many exact planar pieces before it is fitted.
		constexpr uint32_t kMaxPlanarPieces = 4u;
		// A mapping error under this share of the group's error (in mesh units on the original) is sub-pixel wherever the cluster draws.
		constexpr float kRegionErrorShare = 0.25f;
		constexpr bool kRegionCapCells = false;
		// Cell grids from this many nodes per edge store half-word cells.
		constexpr uint32_t kHalfCellGrid = 9u;

		// Whether a candidate's own UVs agree with a region's map at its corners within tolerance (one planar chart mapping).
		bool MapAgrees(const clod_uv_remap::RegionMap& map, const RegionCandidate& c, float tolerance)
		{
			for (int k = 0; k < 3; ++k)
			{
				for (int axis = 0; axis < 2; ++axis)
				{
					const float uv = map.origin[axis] + c.corner[k][0] * map.dUvDb1[axis] + c.corner[k][1] * map.dUvDb2[axis];
					if (!(std::fabs(uv - c.uv[k][axis]) <= tolerance))
					{
						return false;
					}
				}
			}
			return true;
		}

		struct CellPoint
		{
			float fx = 0.0f, fy = 0.0f;
			int region = -1;
		};

		// The cell word for regions a (most points) and b, the line between them chosen over the cell's points to put the fewest on the
		// wrong side (points of other regions always count); outMiss: the points the encoded cell decodes to another region.
		// half: only lines a half cell holds (angles in steps of 16, offsets in steps of 273).
		uint32_t FitCellLine(const std::vector<CellPoint>& points, uint32_t a, uint32_t b, bool half, uint32_t& outMiss)
		{
			using namespace clod_uv_remap;
			const size_t count = points.size();
			std::vector<std::pair<float, uint32_t>> order(count);
			uint32_t bestAngle = 0u;
			float bestOffset = 0.0f;
			uint32_t bestMiss = std::numeric_limits<uint32_t>::max();
			const auto tryAngle = [&](uint32_t angle) {
				const float theta = static_cast<float>(angle & 255u) * (6.28318530718f / 256.0f);
				const float c = std::cos(theta), s = std::sin(theta);
				uint32_t totalA = 0u, others = 0u;
				for (size_t k = 0; k < count; ++k)
				{
					const CellPoint& point = points[k];
					const uint32_t code = point.region == static_cast<int>(a) ? 0u : point.region == static_cast<int>(b) ? 1u : 2u;
					order[k] = { (point.fx - 0.5f) * c + (point.fy - 0.5f) * s, code };
					totalA += code == 0u ? 1u : 0u;
					others += code == 2u ? 1u : 0u;
				}
				std::sort(order.begin(), order.end());
				// Split before index k: points [0, k) on the negative side (A), [k, count) on the positive side (B).
				uint32_t missPositive = totalA, missNegative = 0u;
				for (size_t k = 0; k <= count; ++k)
				{
					const uint32_t miss = missPositive + missNegative + others;
					if (miss < bestMiss)
					{
						bestMiss = miss;
						bestAngle = angle & 255u;
						bestOffset = count == 0u ? 0.0f
							: k == 0u ? order[0].first - 0.01f
							: k == count ? order[count - 1u].first + 0.01f
							: 0.5f * (order[k - 1u].first + order[k].first);
					}
					if (k < count)
					{
						missPositive -= order[k].second == 0u ? 1u : 0u;
						missNegative += order[k].second == 1u ? 1u : 0u;
					}
				}
			};
			for (uint32_t angle = 0u; angle < 256u; angle += half ? 16u : 4u)
			{
				tryAngle(angle);
			}
			if (!half)
			{
				const uint32_t coarse = bestAngle;
				for (int delta = -3; delta <= 3; ++delta)
				{
					if (delta != 0)
					{
						tryAngle(static_cast<uint32_t>(static_cast<int>(coarse) + delta + 256) & 255u);
					}
				}
			}
			const float normalized = std::clamp(bestOffset / kCellOffsetRange, -1.0f, 1.0f);
			const uint32_t offset = half ? static_cast<uint32_t>(std::clamp(std::lround((normalized + 1.0f) * 0.5f * 15.0f), 0l, 15l)) * 273u
				: static_cast<uint32_t>(std::clamp(std::lround((normalized + 1.0f) * 0.5f * 4095.0f), 0l, 4095l));
			const uint32_t cell = EncodeCell(a, b, bestAngle, offset);
			outMiss = 0u;
			for (const CellPoint& point : points)
			{
				outMiss += CellRegion(cell, point.fx, point.fy) != static_cast<uint32_t>(point.region) ? 1u : 0u;
			}
			return cell;
		}

		struct RegionOption
		{
			uint32_t n = 2u;
			std::vector<uint32_t> cells;
			float miss = 0.0f;          // estimated share of the triangle's area decoded to another region
			uint32_t words = 0u;
			uint32_t boundaryCells = 0u;
			bool half = false;          // cells stored as half words
		};

		UvRemapCluster BuildClusterUvRemapRegions(
			const SimplifyValidator& original, const TextureCheckSource& source, const UvRemapSettings& settings,
			std::span<const UvRemapTriangle> triangles, float radius, float reach, uint64_t byteBudget)
		{
			using namespace clod_uv_remap;
			UvRemapCluster result;
			result.layout = 2u;
			result.triangles.assign(triangles.begin(), triangles.end());
			result.nodeCharts.resize(triangles.size());
			result.regionCharts.resize(triangles.size());
			result.reach = reach;
			if (source.uvs == nullptr || source.positions == nullptr || original.OriginalBvh().Empty())
			{
				return result;
			}
			const uint32_t maxNodes = std::clamp(settings.maxNodes, kMinNodesPerEdge, kMaxNodesPerEdge);
			const float tolerance = std::max(settings.slide, 0.0f) * radius;
			const float mapTolerance = 0.25f * settings.sourceTexelUv; // one planar mapping
			const float fitTolerance = settings.sourceTexelUv;          // a fitted map's worst point
			const float seenTolerance = 0.5f * settings.sourceTexelUv;  // one mapping, where a candidate is seen
			std::vector<TriangleRemap> remaps(triangles.size());
			std::vector<float> goodSteps;
			std::vector<float> scaleSteps;
			std::vector<uint32_t> slots;
			std::vector<UvRemapMatch> nodes;
			std::vector<std::array<float, 3>> nodePositions;
			std::vector<uint8_t> matched;
			RegionLabeler labeler;

			struct Plan
			{
				bool planned = false;
				bool region = false;
				uint32_t gridN = 0u;                 // the node grid's nodes per edge (a grid triangle, or a region triangle's fallback)
				std::vector<RegionOption> options;   // a region triangle's cell grids, coarsest first
				uint32_t option = 0u;
				std::vector<RegionMap> maps;
				std::vector<uint32_t> charts;
				float area = 0.0f;
				uint32_t Words() const { return region ? options[option].words : TriangleWords(gridN); }
			};
			std::vector<Plan> plans(triangles.size());
			const uint32_t m = kReferenceLattice;
			std::vector<int> reference(NodeCount(m));
			std::vector<int> pointRegion(NodeCount(m));
			std::vector<uint32_t> queue;
			std::vector<uint32_t> candidateCount;
			std::vector<int> regionOf;
			std::vector<CellPoint> cellPoints;
			std::vector<uint32_t> regionCount;

			for (size_t t = 0; t < triangles.size(); ++t)
			{
				const UvRemapTriangle& triangle = triangles[t];
				if (!triangle.valid)
				{
					continue;
				}
				const float* p[3] = { triangle.position[0], triangle.position[1], triangle.position[2] };
				float face[3];
				const float area = FaceNormalArea(p[0], p[1], p[2], face);
				if (!(area > 0.0f))
				{
					continue;
				}

				// Whether it needs one, as the grid layout judges it, against the frontmost correspondence.
				bool needed = triangle.chart[0] != triangle.chart[1] || triangle.chart[0] != triangle.chart[2];
				float sourceScale = 0.0f;
				for (const auto& w : kNeededSamples)
				{
					float point[3];
					for (int axis = 0; axis < 3; ++axis)
					{
						point[axis] = p[0][axis] * w[0] + p[1][axis] * w[1] + p[2][axis] * w[2];
					}
					const UvRemapMatch sample = MatchUvRemapFrontmost(original, source, point, face, reach);
					if (!sample.found)
					{
						continue;
					}
					sourceScale = std::max(sourceScale, sample.scale);
					const float linear[2] = { triangle.uv[0][0] * w[0] + triangle.uv[1][0] * w[1] + triangle.uv[2][0] * w[2],
						triangle.uv[0][1] * w[0] + triangle.uv[1][1] * w[1] + triangle.uv[2][1] * w[2] };
					const float slide = sample.scale > 0.0f ? std::hypot(linear[0] - sample.uv[0], linear[1] - sample.uv[1]) / sample.scale : 0.0f;
					if (sample.chart != triangle.chart[0] || slide > tolerance)
					{
						needed = true;
					}
				}
				if (!needed)
				{
					continue;
				}
				Plan& plan = plans[t];
				plan.planned = true;
				plan.area = area;
				float maxEdge = 0.0f;
				for (int e = 0; e < 3; ++e)
				{
					const float* a = p[e];
					const float* b = p[(e + 1) % 3];
					const float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
					maxEdge = std::max(maxEdge, std::sqrt(Dot3(d, d)));
				}
				float spacing = settings.texel * radius;
				if (sourceScale > 0.0f)
				{
					spacing = std::max(spacing, settings.sourceTexelUv / sourceScale);
				}
				const float gridSteps = spacing > 0.0f ? std::ceil(maxEdge / spacing) : static_cast<float>(maxNodes);
				plan.gridN = static_cast<uint32_t>(std::clamp(gridSteps + 1.0f, static_cast<float>(kMinNodesPerEdge), static_cast<float>(maxNodes)));

				// What a viewer sees at the reference points.
				labeler.Build(original, source, p, face, reach, slots);
				if (labeler.Empty())
				{
					continue; // the node grid (its nodes fall back to the nearest facing point)
				}
				uint32_t labelled = 0u;
				for (uint32_t j = 0; j < m; ++j)
				{
					for (uint32_t i = 0; i + j < m; ++i)
					{
						const int label = labeler.Label(static_cast<float>(i) / static_cast<float>(m - 1u), static_cast<float>(j) / static_cast<float>(m - 1u));
						reference[NodeIndex(m, i, j)] = label;
						labelled += label >= 0 ? 1u : 0u;
					}
				}
				if (labelled == 0u)
				{
					continue;
				}
				result.pointsUnlabeled += NodeCount(m) - labelled;
				// Points without a crossing (gaps, a coarse edge past the original's) take their nearest labelled neighbour's, breadth
				// first over the lattice in index order.
				if (labelled < NodeCount(m))
				{
					queue.clear();
					for (uint32_t index = 0; index < NodeCount(m); ++index)
					{
						if (reference[index] >= 0)
						{
							queue.push_back(index);
						}
					}
					std::vector<uint32_t> lattice(NodeCount(m) * 2u);
					for (uint32_t j = 0; j < m; ++j)
					{
						for (uint32_t i = 0; i + j < m; ++i)
						{
							lattice[NodeIndex(m, i, j) * 2u] = i;
							lattice[NodeIndex(m, i, j) * 2u + 1u] = j;
						}
					}
					for (size_t head = 0; head < queue.size(); ++head)
					{
						const int i = static_cast<int>(lattice[queue[head] * 2u]);
						const int j = static_cast<int>(lattice[queue[head] * 2u + 1u]);
						constexpr int kNeighbours[6][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, -1 }, { -1, 1 } };
						for (const auto& step : kNeighbours)
						{
							const int ni = i + step[0], nj = j + step[1];
							if (ni < 0 || nj < 0 || ni + nj > static_cast<int>(m) - 1)
							{
								continue;
							}
							const uint32_t neighbour = NodeIndex(m, static_cast<uint32_t>(ni), static_cast<uint32_t>(nj));
							if (reference[neighbour] < 0)
							{
								reference[neighbour] = reference[queue[head]];
								queue.push_back(neighbour);
							}
						}
					}
				}

				// Regions: one per chart seen, most points first. A chart whose source triangles seen here share one affine mapping (within
				// a quarter texel) keeps it exactly (a planar chart); otherwise its least-squares affine fit over its points when that is
				// within a texel of every one, else the triangle takes the node grid (a curved or wrapped chart one map cannot hold).
				candidateCount.assign(labeler.Count(), 0u);
				for (const int label : reference)
				{
					++candidateCount[static_cast<size_t>(label)];
				}
				std::vector<uint32_t> seen;
				for (uint32_t c = 0; c < candidateCount.size(); ++c)
				{
					if (candidateCount[c] != 0u)
					{
						seen.push_back(c);
					}
				}
				std::stable_sort(seen.begin(), seen.end(), [&](uint32_t a, uint32_t b) { return candidateCount[a] > candidateCount[b]; });
				plan.maps.clear();
				plan.charts.clear();
				const auto regionOfChart = [&](uint32_t chart) {
					for (size_t r = 0; r < plan.charts.size(); ++r)
					{
						if (plan.charts[r] == chart)
						{
							return static_cast<int>(r);
						}
					}
					return -1;
				};
				bool overflow = false;
				for (const uint32_t c : seen)
				{
					const RegionCandidate& candidate = labeler.Candidate(static_cast<int>(c));
					if (regionOfChart(candidate.chart) >= 0)
					{
						continue;
					}
					if (plan.maps.size() >= kMaxRegions)
					{
						overflow = true;
						break;
					}
					plan.maps.push_back(candidate.map);
					plan.charts.push_back(candidate.chart);
				}
				if (overflow)
				{
					++result.trianglesRegionsRejected;
					continue;
				}
				// Each chart: one exact mapping; else a few exact planar pieces (a profiled strip seen across its bevels); else its
				// least-squares fit within a texel; else the node grid. candidateRegion: each candidate's region (-1: none).
				std::vector<int> candidateRegion(labeler.Count(), -1);
				// Whether a mapping agrees with a candidate's own wherever the candidate is what is seen: within half a texel, or what
				// a share of the group's error covers of its texture (sub-pixel wherever the cluster draws).
				const auto agreesWhereSeen = [&](const RegionMap& map, uint32_t c) {
					const RegionCandidate& candidate = labeler.Candidate(static_cast<int>(c));
					const float tolerance = std::max(seenTolerance, kRegionErrorShare * radius * candidate.scale);
					for (uint32_t j = 0; j < m; ++j)
					{
						for (uint32_t i = 0; i + j < m; ++i)
						{
							if (reference[NodeIndex(m, i, j)] != static_cast<int>(c))
							{
								continue;
							}
							const float b1 = static_cast<float>(i) / static_cast<float>(m - 1u), b2 = static_cast<float>(j) / static_cast<float>(m - 1u);
							for (int axis = 0; axis < 2; ++axis)
							{
								const float a = map.origin[axis] + b1 * map.dUvDb1[axis] + b2 * map.dUvDb2[axis];
								const float b = candidate.map.origin[axis] + b1 * candidate.map.dUvDb1[axis] + b2 * candidate.map.dUvDb2[axis];
								if (!(std::fabs(a - b) <= tolerance))
								{
									return false;
								}
							}
						}
					}
					return true;
				};
				for (const uint32_t c : seen)
				{
					candidateRegion[c] = regionOfChart(labeler.Candidate(static_cast<int>(c)).chart);
				}
				bool fits = true;
				const size_t chartRegions = plan.maps.size();
				std::vector<uint8_t> splitChart(chartRegions, 0u);
				for (size_t r = 0; r < chartRegions && fits; ++r)
				{
					bool exact = true;
					for (const uint32_t c : seen)
					{
						const RegionCandidate& candidate = labeler.Candidate(static_cast<int>(c));
						if (candidate.chart == plan.charts[r] && !agreesWhereSeen(plan.maps[r], c))
						{
							exact = false;
							break;
						}
					}
					if (exact)
					{
						continue;
					}
					// Planar pieces: the chart's candidates, most points first, each joining the first piece whose mapping it agrees with.
					std::vector<RegionMap> pieces{ plan.maps[r] };
					std::vector<std::pair<uint32_t, uint32_t>> assignment; // candidate, piece
					for (const uint32_t c : seen)
					{
						const RegionCandidate& candidate = labeler.Candidate(static_cast<int>(c));
						if (candidate.chart != plan.charts[r])
						{
							continue;
						}
						uint32_t piece = 0u;
						while (piece < pieces.size() && !agreesWhereSeen(pieces[piece], c))
						{
							++piece;
						}
						if (piece == pieces.size())
						{
							if (pieces.size() >= kMaxPlanarPieces)
							{
								break;
							}
							pieces.push_back(candidate.map);
						}
						assignment.push_back({ c, piece });
					}
					size_t chartCandidates = 0u;
					for (const uint32_t c : seen)
					{
						chartCandidates += labeler.Candidate(static_cast<int>(c)).chart == plan.charts[r] ? 1u : 0u;
					}
					if (assignment.size() == chartCandidates && plan.maps.size() + pieces.size() - 1u <= kMaxRegions)
					{
						splitChart[r] = 1u;
						for (size_t piece = 1; piece < pieces.size(); ++piece)
						{
							plan.maps.push_back(pieces[piece]);
							plan.charts.push_back(plan.charts[r]);
						}
						for (const auto& [c, piece] : assignment)
						{
							candidateRegion[c] = piece == 0u ? static_cast<int>(r) : static_cast<int>(plan.maps.size() - pieces.size() + piece);
						}
						result.regionsPlanarPieces += pieces.size() - 1u;
						continue;
					}
					// uv = origin + b1 e1 + b2 e2 by least squares over the chart's lattice points (each at its candidate's exact UV).
					double normal[3][3] = {}, rhs[2][3] = {};
					for (uint32_t j = 0; j < m; ++j)
					{
						for (uint32_t i = 0; i + j < m; ++i)
						{
							const RegionCandidate& candidate = labeler.Candidate(reference[NodeIndex(m, i, j)]);
							if (candidate.chart != plan.charts[r])
							{
								continue;
							}
							const double x[3] = { 1.0, static_cast<double>(i) / (m - 1u), static_cast<double>(j) / (m - 1u) };
							for (int axis = 0; axis < 2; ++axis)
							{
								const double uv = candidate.map.origin[axis] + x[1] * candidate.map.dUvDb1[axis] + x[2] * candidate.map.dUvDb2[axis];
								for (int row = 0; row < 3; ++row)
								{
									rhs[axis][row] += x[row] * uv;
								}
							}
							for (int row = 0; row < 3; ++row)
							{
								for (int column = 0; column < 3; ++column)
								{
									normal[row][column] += x[row] * x[column];
								}
							}
						}
					}
					// The 3x3 normal equations by Cramer's rule.
					const auto det3 = [](const double a[3][3]) {
						return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
							a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
					};
					const double determinant = det3(normal);
					if (!(std::fabs(determinant) > 1.0e-12 * std::max(normal[0][0] * normal[1][1] * normal[2][2], 1.0e-30)))
					{
						fits = false;
						break;
					}
					RegionMap fitted;
					for (int axis = 0; axis < 2; ++axis)
					{
						double solution[3];
						for (int column = 0; column < 3; ++column)
						{
							double replaced[3][3];
							for (int row = 0; row < 3; ++row)
							{
								for (int k = 0; k < 3; ++k)
								{
									replaced[row][k] = k == column ? rhs[axis][row] : normal[row][k];
								}
							}
							solution[column] = det3(replaced) / determinant;
						}
						fitted.origin[axis] = static_cast<float>(solution[0]);
						fitted.dUvDb1[axis] = static_cast<float>(solution[1]);
						fitted.dUvDb2[axis] = static_cast<float>(solution[2]);
					}
					float worst = 0.0f;
					for (uint32_t j = 0; j < m; ++j)
					{
						for (uint32_t i = 0; i + j < m; ++i)
						{
							const RegionCandidate& candidate = labeler.Candidate(reference[NodeIndex(m, i, j)]);
							if (candidate.chart != plan.charts[r])
							{
								continue;
							}
							const float b1 = static_cast<float>(i) / static_cast<float>(m - 1u), b2 = static_cast<float>(j) / static_cast<float>(m - 1u);
							for (int axis = 0; axis < 2; ++axis)
							{
								const float exactUv = candidate.map.origin[axis] + b1 * candidate.map.dUvDb1[axis] + b2 * candidate.map.dUvDb2[axis];
								const float fittedUv = fitted.origin[axis] + b1 * fitted.dUvDb1[axis] + b2 * fitted.dUvDb2[axis];
								worst = std::max(worst, std::fabs(exactUv - fittedUv));
							}
						}
					}
					float chartScale = 0.0f;
					for (const uint32_t c : seen)
					{
						const RegionCandidate& candidate = labeler.Candidate(static_cast<int>(c));
						chartScale = candidate.chart == plan.charts[r] ? std::max(chartScale, candidate.scale) : chartScale;
					}
					if (!(worst <= std::max(fitTolerance, kRegionErrorShare * radius * chartScale)))
					{
						fits = false;
						break;
					}
					plan.maps[r] = fitted;
					++result.regionsFitted;
				}
				if (!fits)
				{
					++result.trianglesRegionsRejected;
					continue;
				}
				// Candidates seen nowhere on the lattice (met by a cell's own points): their chart's region, or for a chart cut into
				// pieces, the piece whose mapping they agree with.
				for (uint32_t c = 0; c < labeler.Count(); ++c)
				{
					if (candidateCount[c] != 0u)
					{
						continue;
					}
					const RegionCandidate& candidate = labeler.Candidate(static_cast<int>(c));
					const int r = regionOfChart(candidate.chart);
					if (r < 0 || splitChart[static_cast<size_t>(r)] == 0u)
					{
						candidateRegion[c] = r;
						continue;
					}
					for (size_t k = 0; k < plan.maps.size(); ++k)
					{
						if (plan.charts[k] == candidate.chart && MapAgrees(plan.maps[k], candidate, mapTolerance))
						{
							candidateRegion[c] = static_cast<int>(k);
							break;
						}
					}
				}
				regionOf = candidateRegion;
				for (size_t index = 0; index < reference.size(); ++index)
				{
					pointRegion[index] = regionOf[static_cast<size_t>(reference[index])];
				}
				const uint32_t regions = static_cast<uint32_t>(plan.maps.size());
				plan.options.clear();
				if (regions == 1u)
				{
					RegionOption option;
					option.n = 2u;
					option.words = RegionTriangleWords(2u, 1u);
					plan.options.push_back(std::move(option));
					plan.option = 0u;
					plan.region = true;
					continue;
				}

				// The cell grids, coarsest first, until the lines miss little enough.
				// Cells no finer than the node grid's spacing (the smallest grid tried at least as fine as it).
				uint32_t finest = kRegionGrids.back();
				for (const uint32_t n : kRegionGrids)
				{
					if (n >= plan.gridN)
					{
						finest = n;
						break;
					}
				}
				for (const uint32_t n : kRegionGrids)
				{
					if (n > maxNodes || (kRegionCapCells && n > finest))
					{
						break;
					}
					RegionOption option;
					option.n = n;
					option.cells.assign(SubTriangleCount(n), 0u);
					// Fine grids store half-word cells (a coarser line per cell, which a small cell can afford).
					option.half = n >= kHalfCellGrid && regions <= kMaxHalfCellRegions;
					option.words = RegionTriangleWords(n, regions, option.half);
					const uint32_t span = n - 1u;
					const uint32_t stride = (m - 1u) / span;
					const float cellShare = 1.0f / static_cast<float>(span * span);
					for (uint32_t jj = 0; jj + 1u < n; ++jj)
					{
						for (uint32_t ii = 0; ii + jj + 1u < n; ++ii)
						{
							for (int upper = 0; upper < 2; ++upper)
							{
								if (upper != 0 && ii + jj + 2u >= n)
								{
									continue;
								}
								cellPoints.clear();
								bool uniform = true;
								int first = -1;
								for (uint32_t dj = 0; dj <= stride; ++dj)
								{
									for (uint32_t di = 0; di <= stride; ++di)
									{
										if (upper == 0 ? di + dj > stride : di + dj < stride)
										{
											continue;
										}
										const uint32_t i = ii * stride + di, j = jj * stride + dj;
										if (i + j >= m)
										{
											continue;
										}
										CellPoint point;
										point.fx = static_cast<float>(di) / static_cast<float>(stride);
										point.fy = static_cast<float>(dj) / static_cast<float>(stride);
										point.region = pointRegion[NodeIndex(m, i, j)];
										first = first < 0 ? point.region : first;
										uniform = uniform && point.region == first;
										cellPoints.push_back(point);
									}
								}
								const uint32_t cellIndex = SubTriangleIndex(n, ii, jj, upper != 0);
								if (uniform)
								{
									option.cells[cellIndex] = EncodeCell(static_cast<uint32_t>(std::max(first, 0)), static_cast<uint32_t>(std::max(first, 0)), 0u, 0u);
									continue;
								}
								// A boundary cell: more points of its own, then the line between its two largest regions.
								constexpr uint32_t kSub = 4u;
								for (uint32_t v = 0; v <= kSub; ++v)
								{
									for (uint32_t u = 0; u + v <= kSub; ++u)
									{
										CellPoint point;
										point.fx = static_cast<float>(u) / static_cast<float>(kSub);
										point.fy = static_cast<float>(v) / static_cast<float>(kSub);
										if (upper != 0)
										{
											point.fx = 1.0f - point.fx;
											point.fy = 1.0f - point.fy;
										}
										const int label = labeler.Label((static_cast<float>(ii) + point.fx) / static_cast<float>(span),
											(static_cast<float>(jj) + point.fy) / static_cast<float>(span));
										if (label < 0)
										{
											continue;
										}
										point.region = regionOf[static_cast<size_t>(label)];
										if (point.region < 0)
										{
											continue; // a candidate seen nowhere on the reference lattice
										}
										cellPoints.push_back(point);
									}
								}
								regionCount.assign(regions, 0u);
								for (const CellPoint& point : cellPoints)
								{
									++regionCount[static_cast<size_t>(point.region)];
								}
								uint32_t a = 0u, b = 0u;
								for (uint32_t r = 0; r < regions; ++r)
								{
									if (regionCount[r] > regionCount[a])
									{
										a = r;
									}
								}
								b = a == 0u ? 1u : 0u;
								for (uint32_t r = 0; r < regions; ++r)
								{
									if (r != a && regionCount[r] > regionCount[b])
									{
										b = r;
									}
								}
								if (regionCount[b] == 0u)
								{
									option.cells[cellIndex] = EncodeCell(a, a, 0u, 0u);
									continue;
								}
								uint32_t miss = 0u;
								option.cells[cellIndex] = FitCellLine(cellPoints, a, b, option.half, miss);
								++option.boundaryCells;
								option.miss += cellShare * static_cast<float>(miss) / static_cast<float>(std::max<size_t>(cellPoints.size(), 1u));
							}
						}
					}
					const bool good = option.miss <= settings.cellMiss;
					plan.options.push_back(std::move(option));
					if (good)
					{
						break;
					}
				}
				if (plan.options.empty())
				{
					continue;
				}
				// The first good grid, else the one missing least (the coarser on ties), however much it misses: a line per cell between the
				// two largest regions gets only the cells' third regions wrong, where the node grid (at the group's spacing, often 4 nodes per
				// edge on the large triangles that see most charts) straddles charts in every sub-triangle and smears its extrapolation over
				// the whole triangle (the Bistro door wedges, docs/clod-appearance-stage6.md).
				uint32_t chosen = static_cast<uint32_t>(plan.options.size() - 1u);
				if (plan.options[chosen].miss > settings.cellMiss)
				{
					for (uint32_t k = 0; k < plan.options.size(); ++k)
					{
						if (plan.options[k].miss < plan.options[chosen].miss)
						{
							chosen = k;
						}
					}
				}
				plan.options.resize(chosen + 1u);
				plan.option = chosen;
				plan.region = true;
			}

			// The byte budget: the largest triangle steps down first (a coarser cell grid, or a coarser node grid), then the smallest
			// triangles go without.
			const auto blockBytes = [&]() {
				uint64_t words = kHeaderWords + triangles.size();
				for (const Plan& plan : plans)
				{
					words += plan.planned ? plan.Words() : 0u;
				}
				return words * 4u;
			};
			while (blockBytes() > byteBudget)
			{
				size_t largest = triangles.size();
				for (size_t t = 0; t < plans.size(); ++t)
				{
					const Plan& plan = plans[t];
					const bool canShrink = plan.planned && (plan.region ? plan.option > 0u : plan.gridN > kMinNodesPerEdge);
					if (canShrink && (largest == triangles.size() || plan.Words() > plans[largest].Words()))
					{
						largest = t;
					}
				}
				if (largest != triangles.size())
				{
					Plan& plan = plans[largest];
					if (plan.region)
					{
						--plan.option;
					}
					else
					{
						--plan.gridN;
					}
					continue;
				}
				size_t smallest = triangles.size();
				for (size_t t = 0; t < plans.size(); ++t)
				{
					if (plans[t].planned && (smallest == triangles.size() || plans[t].area < plans[smallest].area))
					{
						smallest = t;
					}
				}
				if (smallest == triangles.size())
				{
					break;
				}
				plans[smallest].planned = false;
				++result.trianglesOverBudget;
			}

			for (size_t t = 0; t < triangles.size(); ++t)
			{
				Plan& plan = plans[t];
				if (!plan.planned)
				{
					continue;
				}
				const UvRemapTriangle& triangle = triangles[t];
				if (plan.region)
				{
					RegionOption& option = plan.options[plan.option];
					TriangleRemap& remap = remaps[t];
					remap.n = option.n;
					remap.regions = plan.maps;
					remap.halfCells = option.half;
					if (plan.maps.size() > 1u)
					{
						remap.cells = option.cells;
						for (const uint32_t cell : option.cells)
						{
							result.regionCells += (cell & 63u) != ((cell >> 6u) & 63u) ? 1u : 0u;
						}
					}
					result.regionCharts[t] = plan.charts;
					result.regions += plan.maps.size();
					for (size_t k = 0; k < kRegionGrids.size(); ++k)
					{
						result.regionGridCounts[k] += kRegionGrids[k] == option.n && plan.maps.size() > 1u ? 1u : 0u;
					}
					result.regionArea += plan.area;
					result.regionMissArea += static_cast<double>(option.miss) * plan.area;
					++result.trianglesRegions;
					++result.trianglesRemapped;
					continue;
				}

				// The node grid, each node at the frontmost correspondence.
				const uint32_t n = plan.gridN;
				const float* p[3] = { triangle.position[0], triangle.position[1], triangle.position[2] };
				float face[3];
				FaceNormalArea(p[0], p[1], p[2], face);
				float maxEdge = 0.0f;
				for (int e = 0; e < 3; ++e)
				{
					const float* a = p[e];
					const float* b = p[(e + 1) % 3];
					const float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
					maxEdge = std::max(maxEdge, std::sqrt(Dot3(d, d)));
				}
				const float gridEdge = maxEdge / static_cast<float>(n - 1u);
				const uint32_t nodeCount = NodeCount(n);
				nodes.assign(nodeCount, {});
				nodePositions.assign(nodeCount, {});
				bool anyMatched = false;
				for (uint32_t j = 0; j < n; ++j)
				{
					for (uint32_t i = 0; i + j < n; ++i)
					{
						const float b1 = static_cast<float>(i) / static_cast<float>(n - 1u);
						const float b2 = static_cast<float>(j) / static_cast<float>(n - 1u);
						const float b0 = 1.0f - b1 - b2;
						const uint32_t index = NodeIndex(n, i, j);
						std::array<float, 3>& point = nodePositions[index];
						for (int axis = 0; axis < 3; ++axis)
						{
							point[axis] = p[0][axis] * b0 + p[1][axis] * b1 + p[2][axis] * b2;
						}
						nodes[index] = MatchUvRemapFrontmost(original, source, point.data(), face, reach);
						anyMatched = anyMatched || nodes[index].found;
					}
				}
				if (!anyMatched)
				{
					continue;
				}
				FinishGridTriangle(n, gridEdge, nodes, nodePositions, matched, remaps[t], result.nodeCharts[t], result, goodSteps, scaleSteps);
				++result.trianglesGrid;
			}

			if (result.trianglesRemapped == 0u)
			{
				result.nodeCharts.clear();
				result.regionCharts.clear();
				return result;
			}
			const float uvStep = !goodSteps.empty() ? Median(goodSteps) : Median(scaleSteps);
			result.block = EncodeBlock(remaps, uvStep);
			if (result.block.empty())
			{
				result.nodeCharts.clear();
				result.regionCharts.clear();
				result.trianglesRemapped = 0u;
			}
			return result;
		}

	}

	UvRemapCluster BuildClusterUvRemap(
		const SimplifyValidator& original, const TextureCheckSource& source, const UvRemapSettings& settings,
		std::span<const UvRemapTriangle> triangles, float radius, float reach, uint64_t byteBudget)
	{
		using namespace clod_uv_remap;
		if (settings.layout >= 2u)
		{
			return BuildClusterUvRemapRegions(original, source, settings, triangles, radius, reach, byteBudget);
		}
		UvRemapCluster result;
		result.triangles.assign(triangles.begin(), triangles.end());
		result.nodeCharts.resize(triangles.size());
		result.reach = reach;
		if (source.uvs == nullptr || source.positions == nullptr || original.OriginalBvh().Empty())
		{
			return result;
		}
		const uint32_t maxNodes = std::clamp(settings.maxNodes, kMinNodesPerEdge, kMaxNodesPerEdge);
		const float tolerance = std::max(settings.slide, 0.0f) * radius;
		std::vector<TriangleRemap> remaps(triangles.size());
		std::vector<float> goodSteps;
		std::vector<float> scaleSteps;
		std::vector<uint32_t> slots;
		std::vector<UvRemapMatch> nodes;
		std::vector<std::array<float, 3>> nodePositions;
		std::vector<uint8_t> matched;

		// First which triangles need a remap and at how many nodes per edge, then (within the byte budget) their nodes.
		std::vector<uint32_t> planned(triangles.size(), 0u);
		std::vector<float> plannedArea(triangles.size(), 0.0f);
		for (size_t t = 0; t < triangles.size(); ++t)
		{
			const UvRemapTriangle& triangle = triangles[t];
			if (!triangle.valid)
			{
				continue;
			}
			const float* p[3] = { triangle.position[0], triangle.position[1], triangle.position[2] };
			float face[3];
			const float area = FaceNormalArea(p[0], p[1], p[2], face);
			if (!(area > 0.0f))
			{
				continue;
			}

			// Whether it needs one: at the texture term's sample points, the corners' interpolated UV is in another chart than the
			// original's (or the corners mix charts), or slides farther than the term's tolerance.
			const bool mixedCorners = triangle.chart[0] != triangle.chart[1] || triangle.chart[0] != triangle.chart[2];
			bool needed = mixedCorners;
			float sourceScale = 0.0f;
			for (const auto& w : kSamples)
			{
				float point[3];
				for (int axis = 0; axis < 3; ++axis)
				{
					point[axis] = p[0][axis] * w[0] + p[1][axis] * w[1] + p[2][axis] * w[2];
				}
				const UvRemapMatch sample = MatchUvRemapPoint(original, source, point, face, reach, slots);
				if (!sample.found)
				{
					continue;
				}
				sourceScale = std::max(sourceScale, sample.scale);
				const float linear[2] = { triangle.uv[0][0] * w[0] + triangle.uv[1][0] * w[1] + triangle.uv[2][0] * w[2],
					triangle.uv[0][1] * w[0] + triangle.uv[1][1] * w[1] + triangle.uv[2][1] * w[2] };
				const float slide = sample.scale > 0.0f ? std::hypot(linear[0] - sample.uv[0], linear[1] - sample.uv[1]) / sample.scale : 0.0f;
				if (sample.chart != triangle.chart[0] || slide > tolerance)
				{
					needed = true;
				}
			}
			if (!needed)
			{
				continue;
			}

			// The spacing: texel group errors, never finer than a source texel; between kMinNodesPerEdge and maxNodes per edge.
			float maxEdge = 0.0f;
			for (int e = 0; e < 3; ++e)
			{
				const float* a = p[e];
				const float* b = p[(e + 1) % 3];
				const float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
				maxEdge = std::max(maxEdge, std::sqrt(Dot3(d, d)));
			}
			float spacing = settings.texel * radius;
			if (sourceScale > 0.0f)
			{
				spacing = std::max(spacing, settings.sourceTexelUv / sourceScale);
			}
			const float gridSteps = spacing > 0.0f ? std::ceil(maxEdge / spacing) : static_cast<float>(maxNodes);
			planned[t] = static_cast<uint32_t>(std::clamp(gridSteps + 1.0f, static_cast<float>(kMinNodesPerEdge), static_cast<float>(maxNodes)));
			plannedArea[t] = area;
		}

		// The byte budget (what the cluster's page can still hold): coarser grids first, then the smallest triangles go without.
		const auto blockBytes = [&]() {
			uint64_t words = kHeaderWords + triangles.size();
			for (const uint32_t n : planned)
			{
				words += n != 0u ? TriangleWords(n) : 0u;
			}
			return words * 4u;
		};
		while (blockBytes() > byteBudget)
		{
			uint32_t largest = 0u;
			for (const uint32_t n : planned)
			{
				largest = std::max(largest, n);
			}
			if (largest > kMinNodesPerEdge)
			{
				for (uint32_t& n : planned)
				{
					n = n == largest ? n - 1u : n;
				}
				continue;
			}
			size_t smallest = triangles.size();
			for (size_t t = 0; t < triangles.size(); ++t)
			{
				if (planned[t] != 0u && (smallest == triangles.size() || plannedArea[t] < plannedArea[smallest]))
				{
					smallest = t;
				}
			}
			if (smallest == triangles.size())
			{
				break;
			}
			planned[smallest] = 0u;
			++result.trianglesOverBudget;
		}

		for (size_t t = 0; t < triangles.size(); ++t)
		{
			const uint32_t n = planned[t];
			if (n == 0u)
			{
				continue;
			}
			const UvRemapTriangle& triangle = triangles[t];
			const float* p[3] = { triangle.position[0], triangle.position[1], triangle.position[2] };
			float face[3];
			FaceNormalArea(p[0], p[1], p[2], face);
			float maxEdge = 0.0f;
			for (int e = 0; e < 3; ++e)
			{
				const float* a = p[e];
				const float* b = p[(e + 1) % 3];
				const float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
				maxEdge = std::max(maxEdge, std::sqrt(Dot3(d, d)));
			}
			const float gridEdge = maxEdge / static_cast<float>(n - 1u);

			const uint32_t nodeCount = NodeCount(n);
			nodes.assign(nodeCount, {});
			nodePositions.assign(nodeCount, {});
			bool anyMatched = false;
			for (uint32_t j = 0; j < n; ++j)
			{
				for (uint32_t i = 0; i + j < n; ++i)
				{
					const float b1 = static_cast<float>(i) / static_cast<float>(n - 1u);
					const float b2 = static_cast<float>(j) / static_cast<float>(n - 1u);
					const float b0 = 1.0f - b1 - b2;
					const uint32_t index = NodeIndex(n, i, j);
					std::array<float, 3>& point = nodePositions[index];
					for (int axis = 0; axis < 3; ++axis)
					{
						point[axis] = p[0][axis] * b0 + p[1][axis] * b1 + p[2][axis] * b2;
					}
					nodes[index] = MatchUvRemapPoint(original, source, point.data(), face, reach, slots);
					anyMatched = anyMatched || nodes[index].found;
				}
			}
			if (!anyMatched)
			{
				continue;
			}

			FinishGridTriangle(n, gridEdge, nodes, nodePositions, matched, remaps[t], result.nodeCharts[t], result, goodSteps, scaleSteps);
		}

		if (result.trianglesRemapped == 0u)
		{
			result.nodeCharts.clear();
			return result;
		}
		const float uvStep = !goodSteps.empty() ? Median(goodSteps) : Median(scaleSteps);
		result.block = EncodeBlock(remaps, uvStep);
		if (result.block.empty())
		{
			result.nodeCharts.clear();
			result.trianglesRemapped = 0u;
		}
		return result;
	}

	bool EvaluateClusterUvRemap(
		const UvRemapCluster& cluster, const SimplifyValidator& original, const TextureCheckSource& source, uint32_t triangle,
		const float weights[3], std::vector<uint32_t>& slots, TextureRemapSample& out)
	{
		const bool gridTriangle = triangle < cluster.nodeCharts.size() && !cluster.nodeCharts[triangle].empty();
		const bool regionTriangle = triangle < cluster.regionCharts.size() && !cluster.regionCharts[triangle].empty();
		if (cluster.block.empty() || (!gridTriangle && !regionTriangle) || triangle >= cluster.triangles.size())
		{
			return false;
		}
		const clod_uv_remap::Sample sample = clod_uv_remap::Evaluate(cluster.block, triangle, weights[1], weights[2]);
		if (!sample.remapped)
		{
			return false;
		}
		out.uv[0] = sample.uv[0];
		out.uv[1] = sample.uv[1];
		if (sample.region)
		{
			const std::vector<uint32_t>& charts = cluster.regionCharts[triangle];
			out.chart = sample.regionIndex < charts.size() ? charts[sample.regionIndex] : 0u;
		}
		else
		{
			const std::vector<uint32_t>& charts = cluster.nodeCharts[triangle];
			const uint32_t node = sample.interpolated ? sample.nodes[0] : sample.nearestNode;
			out.chart = node < charts.size() ? charts[node] : 0u;
		}

		const UvRemapTriangle& corners = cluster.triangles[triangle];
		const float* p[3] = { corners.position[0], corners.position[1], corners.position[2] };
		float face[3];
		const float area = FaceNormalArea(p[0], p[1], p[2], face);
		const float det = sample.dUvDb1[0] * sample.dUvDb2[1] - sample.dUvDb1[1] * sample.dUvDb2[0];
		out.uvScale = area > 0.0f ? std::sqrt(std::fabs(det) * 0.5f / area) : 0.0f;

		// What the remap should draw there: the correspondence its nodes were matched with, at the sample itself.
		float point[3];
		for (int axis = 0; axis < 3; ++axis)
		{
			point[axis] = p[0][axis] * weights[0] + p[1][axis] * weights[1] + p[2][axis] * weights[2];
		}
		// Layout 2: the frontmost crossing found afresh at the sample (what a viewer sees there), not the regions' cells.
		const UvRemapMatch reference = !(area > 0.0f) ? UvRemapMatch{}
			: cluster.layout >= 2u ? MatchUvRemapFrontmost(original, source, point, face, cluster.reach)
			: MatchUvRemapPoint(original, source, point, face, cluster.reach, slots);
		out.hasReference = reference.found;
		out.referenceUv[0] = reference.uv[0];
		out.referenceUv[1] = reference.uv[1];
		out.referenceChart = reference.chart;
		out.referenceUvScale = reference.scale;
		return true;
	}

}
