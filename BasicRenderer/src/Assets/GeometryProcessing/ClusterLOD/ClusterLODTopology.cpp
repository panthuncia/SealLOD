#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODTopology.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numeric>

#include <tracy/Tracy.hpp>

namespace clod_detail {

	namespace {

		// The validator's sample points (barycentric weights), a quarter of the triangle's area each.
		constexpr std::array<std::array<float, 3>, 4> kSamples{ {
			{ 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f },
			{ 4.0f / 6.0f, 1.0f / 6.0f, 1.0f / 6.0f },
			{ 1.0f / 6.0f, 4.0f / 6.0f, 1.0f / 6.0f },
			{ 1.0f / 6.0f, 1.0f / 6.0f, 4.0f / 6.0f },
		} };

		// The validator's match slack, as a fraction of the original's bounding diagonal.
		constexpr float kMatchSlackFraction = 1.0e-5f;

		inline const float* At(const float* base, size_t strideBytes, uint32_t index)
		{
			return reinterpret_cast<const float*>(reinterpret_cast<const std::byte*>(base) + static_cast<size_t>(index) * strideBytes);
		}

		// Unit face normal of abc, returning twice its area (zero normal for degenerate triangles).
		inline float FaceNormal(const float* a, const float* b, const float* c, float* normal)
		{
			const float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
			const float ac[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
			normal[0] = ab[1] * ac[2] - ab[2] * ac[1];
			normal[1] = ab[2] * ac[0] - ab[0] * ac[2];
			normal[2] = ab[0] * ac[1] - ab[1] * ac[0];
			const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
			if (!(length > 0.0f))
			{
				normal[0] = normal[1] = normal[2] = 0.0f;
				return 0.0f;
			}
			normal[0] /= length;
			normal[1] /= length;
			normal[2] /= length;
			return length;
		}

	}

	TextureDisplacement MeasureTextureDisplacement(
		const SimplifyValidator& original, const TextureCheckSource& source, const SimplifySurfaceView& output, const float* outputUvs, float radius,
		float tolerance)
	{
		TextureCheckOptions options;
		options.slideTolerance = tolerance;
		return MeasureTextureDisplacement(original, source, output, outputUvs, radius, options);
	}

	TextureDisplacement MeasureTextureDisplacement(
		const SimplifyValidator& original, const TextureCheckSource& source, const SimplifySurfaceView& output, const float* outputUvs, float radius,
		const TextureCheckOptions& options)
	{
		ZoneScopedN("ClusterLODTopology::MeasureTextureDisplacement");
		const float tolerance = options.slideTolerance;
		const bool uvTest = options.uvTolerance > 0.0f;
		const bool chartTest = options.outputCharts != nullptr && source.charts != nullptr;
		TextureDisplacement result;
		if (source.uvs == nullptr || source.positions == nullptr || outputUvs == nullptr || original.OriginalBvh().Empty())
		{
			return result;
		}
		const float slack = original.Diagonal() * kMatchSlackFraction;
		for (size_t i = 0; i + 2u < output.indexCount; i += 3u)
		{
			const uint32_t v[3] = { output.indices[i], output.indices[i + 1u], output.indices[i + 2u] };
			const float* p[3] = { At(output.positions, output.positionStride, v[0]), At(output.positions, output.positionStride, v[1]),
				At(output.positions, output.positionStride, v[2]) };
			float face[3];
			const float area = 0.5f * FaceNormal(p[0], p[1], p[2], face);
			if (!(area > 0.0f))
			{
				continue;
			}
			result.area += area;
			const uint32_t sampleCount = AreaSampleCount(options.samplesPerTriangle);
			const double weight = static_cast<double>(area) / static_cast<double>(sampleCount);
			// Stage 6 step 2: a remapped triangle reads its texcoords through the remap, sample by sample.
			const size_t triangleIndex = i / 3u;
			TextureRemapSample remapped;
			const bool remappedTriangle = options.remap != nullptr && (*options.remap)(triangleIndex, kSamples[0].data(), remapped);
			// Follow-up: a triangle whose corners come from different charts interpolates across the atlas between them.
			const bool mixed = chartTest && !remappedTriangle &&
				(options.outputCharts[v[0]] != options.outputCharts[v[1]] || options.outputCharts[v[0]] != options.outputCharts[v[2]]);
			if (mixed)
			{
				result.mixedArea += area;
				result.chartArea += area;
				result.displacedArea += area;
			}
			// Stage 6 step 1: the output triangle's texcoord scale, for the weight's filter footprint.
			TextureSampleQuery query;
			if (options.sampleWeight != nullptr)
			{
				const float* w[3] = { &outputUvs[static_cast<size_t>(v[0]) * 2u], &outputUvs[static_cast<size_t>(v[1]) * 2u], &outputUvs[static_cast<size_t>(v[2]) * 2u] };
				const float outputUvArea = 0.5f * std::fabs((w[1][0] - w[0][0]) * (w[2][1] - w[0][1]) - (w[2][0] - w[0][0]) * (w[1][1] - w[0][1]));
				query.outputUvScale = std::isfinite(outputUvArea) ? std::sqrt(outputUvArea / area) : 0.0f;
			}
			const auto weighDisplaced = [&](bool matched) {
				query.matched = matched;
				const float sampleWeight = (*options.sampleWeight)(query);
				result.weightedDisplacedArea += weight * static_cast<double>(std::clamp(sampleWeight, 0.0f, 1.0f));
			};
			for (const auto& sample : kSamples)
			{
				const size_t sampleIndex = static_cast<size_t>(&sample - kSamples.data());
				if (!AreaSampleTaken(i / 3u, sampleIndex, sampleCount))
				{
					continue;
				}
				float point[3], uv[2] = { 0.0f, 0.0f };
				for (int axis = 0; axis < 3; ++axis)
				{
					point[axis] = p[0][axis] * sample[0] + p[1][axis] * sample[1] + p[2][axis] * sample[2];
				}
				uint32_t sampleChart = chartTest ? options.outputCharts[v[0]] : 0u;
				if (remappedTriangle)
				{
					if (&sample != &kSamples[0])
					{
						(*options.remap)(triangleIndex, sample.data(), remapped);
					}
					uv[0] = remapped.uv[0];
					uv[1] = remapped.uv[1];
					sampleChart = remapped.chart;
					query.outputUvScale = remapped.uvScale;
				}
				else
				{
					for (int corner = 0; corner < 3; ++corner)
					{
						uv[0] += outputUvs[static_cast<size_t>(v[corner]) * 2u + 0u] * sample[corner];
						uv[1] += outputUvs[static_cast<size_t>(v[corner]) * 2u + 1u] * sample[corner];
					}
				}

				query.outputUv[0] = uv[0];
				query.outputUv[1] = uv[1];

				float sourceUv[2] = { 0.0f, 0.0f };
				float scale = 0.0f; // texcoord units per mesh unit on the matched triangle (isotropic estimate)
				uint32_t sourceChart = 0u;
				if (remappedTriangle)
				{
					// Stage 6 step 2: against the remap's own correspondence (TextureRemapSample::hasReference).
					if (!remapped.hasReference || !(remapped.referenceUvScale > 0.0f))
					{
						continue;
					}
					sourceUv[0] = remapped.referenceUv[0];
					sourceUv[1] = remapped.referenceUv[1];
					scale = remapped.referenceUvScale;
					sourceChart = remapped.referenceChart;
				}
				else
				{
					SimplifyTriangleBvh::Match match;
					const size_t gather = triangleIndex * kSamples.size() + sampleIndex;
					const bool matched = options.matchGathers != nullptr && gather < options.matchGatherCount &&
						options.matchGathers[gather].first != SimplifyTriangleBvh::kNotGathered
						? SimplifyTriangleBvh::SelectFacing(options.matchGathers[gather], options.matchPool, face, match)
						: original.OriginalBvh().MatchFacing(point, face, radius, slack, match);
					if (!matched || static_cast<size_t>(match.triangle) * 3u + 2u >= source.indexCount)
					{
						if (mixed && options.sampleWeight != nullptr)
						{
							weighDisplaced(false);
						}
						continue;
					}
					const uint32_t* s = &source.indices[static_cast<size_t>(match.triangle) * 3u];
					const float* q[3] = { At(source.positions, source.positionStride, s[0]), At(source.positions, source.positionStride, s[1]),
						At(source.positions, source.positionStride, s[2]) };
					const float* t[3] = { At(source.uvs, source.uvStride, s[0]), At(source.uvs, source.uvStride, s[1]), At(source.uvs, source.uvStride, s[2]) };
					float sourceFace[3];
					const float sourceArea = 0.5f * FaceNormal(q[0], q[1], q[2], sourceFace);
					const float uvArea = 0.5f * std::fabs((t[1][0] - t[0][0]) * (t[2][1] - t[0][1]) - (t[2][0] - t[0][0]) * (t[1][1] - t[0][1]));
					if (!(sourceArea > 0.0f) || !(uvArea > 0.0f) || !std::isfinite(uvArea))
					{
						if (mixed && options.sampleWeight != nullptr)
						{
							weighDisplaced(false);
						}
						continue;
					}
					for (int corner = 0; corner < 3; ++corner)
					{
						sourceUv[0] += t[corner][0] * match.barycentric[corner];
						sourceUv[1] += t[corner][1] * match.barycentric[corner];
					}
					scale = std::sqrt(uvArea / sourceArea);
					sourceChart = chartTest ? source.charts[s[0]] : 0u;
				}
				const float du = uv[0] - sourceUv[0], dv = uv[1] - sourceUv[1];
				const float slide = std::sqrt(du * du + dv * dv) / scale;
				result.measuredArea += weight;
				const bool slid = !(slide <= tolerance);
				if (options.sampleWeight != nullptr)
				{
					query.sourceUv[0] = sourceUv[0];
					query.sourceUv[1] = sourceUv[1];
					query.sourceUvScale = scale;
				}
				if (!uvTest && !chartTest)
				{
					if (slid)
					{
						result.displacedArea += weight;
						if (options.sampleWeight != nullptr && !options.weighEverySample)
						{
							weighDisplaced(true);
						}
					}
				}
				else
				{
					// Follow-ups: the UV-space difference itself (texels of a nominal resolution), and the chart identity.
					const float uvDistance = std::sqrt(du * du + dv * dv);
					const bool uvDisplaced = uvTest && !(uvDistance <= options.uvTolerance);
					const bool otherChart = chartTest && !mixed && sampleChart != sourceChart;
					result.slideDisplacedArea += slid ? weight : 0.0;
					result.uvDisplacedArea += uvDisplaced ? weight : 0.0;
					result.chartArea += otherChart ? weight : 0.0;
					if (uvTest)
					{
						for (int level = 0; level < 4; ++level)
						{
							result.uvOverArea[level] += !(uvDistance <= options.uvTolerance * static_cast<float>(1 << (2 * level))) ? weight : 0.0;
						}
					}
					if (!mixed && (slid || uvDisplaced || otherChart))
					{
						result.displacedArea += weight;
					}
					if (options.sampleWeight != nullptr && !options.weighEverySample && (mixed || slid || uvDisplaced || otherChart))
					{
						weighDisplaced(true);
					}
				}
				if (options.sampleWeight != nullptr && options.weighEverySample)
				{
					weighDisplaced(true);
				}
				for (int level = 0; level < 4; ++level)
				{
					if (!(slide <= radius * static_cast<float>(1 << level)))
					{
						result.slideOverArea[level] += weight;
					}
				}
			}
		}
		return result;
	}

	std::vector<uint32_t> ComputeTextureCharts(
		const float* positions, size_t positionStride, const float* uvs, size_t uvStride, size_t vertexCount, const uint32_t* indices, size_t indexCount)
	{
		ZoneScopedN("ClusterLODTopology::ComputeTextureCharts");
		// Union-find with the lower index as root, so the result does not depend on the order of the unions.
		std::vector<uint32_t> parent(vertexCount);
		std::iota(parent.begin(), parent.end(), 0u);
		const auto find = [&](uint32_t v) {
			while (parent[v] != v)
			{
				parent[v] = parent[parent[v]];
				v = parent[v];
			}
			return v;
		};
		const auto unite = [&](uint32_t a, uint32_t b) {
			a = find(a);
			b = find(b);
			if (a != b)
			{
				parent[std::max(a, b)] = std::min(a, b);
			}
		};
		for (size_t i = 0; i + 2u < indexCount; i += 3u)
		{
			if (indices[i] < vertexCount && indices[i + 1u] < vertexCount && indices[i + 2u] < vertexCount)
			{
				unite(indices[i], indices[i + 1u]);
				unite(indices[i], indices[i + 2u]);
			}
		}

		// Vertices at one position (bitwise) whose texcoords agree within 1/1024 (relative above 1) are one vertex.
		const auto key = [&](uint32_t v) {
			std::array<uint32_t, 3> bits;
			std::memcpy(bits.data(), At(positions, positionStride, v), sizeof(bits));
			return bits;
		};
		std::vector<uint32_t> order(vertexCount);
		std::iota(order.begin(), order.end(), 0u);
		std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
			const auto ka = key(a), kb = key(b);
			return ka != kb ? ka < kb : a < b;
		});
		const auto sameUv = [&](uint32_t a, uint32_t b) {
			const float* ua = At(uvs, uvStride, a);
			const float* ub = At(uvs, uvStride, b);
			for (int axis = 0; axis < 2; ++axis)
			{
				if (!(std::fabs(ua[axis] - ub[axis]) <= (1.0f / 1024.0f) * std::max(1.0f, std::max(std::fabs(ua[axis]), std::fabs(ub[axis])))))
				{
					return false;
				}
			}
			return true;
		};
		for (size_t begin = 0; begin < order.size();)
		{
			size_t end = begin + 1u;
			while (end < order.size() && key(order[end]) == key(order[begin]))
			{
				++end;
			}
			for (size_t a = begin; a < end; ++a)
			{
				for (size_t b = a + 1u; b < end; ++b)
				{
					if (sameUv(order[a], order[b]))
					{
						unite(order[a], order[b]);
					}
				}
			}
			begin = end;
		}

		std::vector<uint32_t> charts(vertexCount);
		for (uint32_t v = 0; v < static_cast<uint32_t>(vertexCount); ++v)
		{
			charts[v] = find(v);
		}
		return charts;
	}

}
