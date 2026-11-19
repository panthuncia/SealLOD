#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODSimplifyValidation.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#include <emmintrin.h>
#include <intrin.h>

#include <spdlog/spdlog.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_invoke.h>
#include <oneapi/tbb/task_arena.h>
#include <tracy/Tracy.hpp>

namespace clod_detail {

	namespace {

		// Query diagnostics (QueryStatsScope): per site, traversals, candidates taken, overflowed gathers, leaves tested, and taken
		// counts by power of two.
		constexpr uint32_t kQueryHistogramBuckets = 20u;
		struct QueryCounters
		{
			std::atomic<uint64_t> gathers{ 0 };
			std::atomic<uint64_t> candidates{ 0 };
			std::atomic<uint64_t> overflows{ 0 };
			std::atomic<uint64_t> leaves{ 0 };
			std::atomic<uint64_t> maxCandidates{ 0 };
			std::atomic<uint64_t> inner{ 0 };               // inner nodes visited
			std::atomic<uint64_t> cycles{ 0 }, leafCycles{ 0 }; // time stamp counter: whole traversals, their leaf tests
			std::atomic<uint64_t> onSurface{ 0 };           // traversals whose nearest lay within the slack (overlapping source)
			std::atomic<uint64_t> onSurfaceCandidates{ 0 };
			std::array<std::atomic<uint64_t>, kQueryHistogramBuckets> histogram{};
		};
		std::array<QueryCounters, static_cast<size_t>(QuerySite::Count)> g_queryCounters;
		thread_local QuerySite t_querySite = QuerySite::Other;
		const bool g_queryStatsEnabled = [] {
			const char* value = std::getenv("BASICRENDERER_CLOD_QUERY_STATS");
			return value != nullptr && value[0] != 0 && value[0] != '0';
		}();

		void RecordQuery(uint64_t leaves, uint64_t taken, bool overflow, bool onSurface = false, uint64_t inner = 0u, uint64_t cycles = 0u,
			uint64_t leafCycles = 0u)
		{
			QueryCounters& counters = g_queryCounters[static_cast<size_t>(t_querySite)];
			counters.cycles.fetch_add(cycles, std::memory_order_relaxed);
			counters.leafCycles.fetch_add(leafCycles, std::memory_order_relaxed);
			counters.inner.fetch_add(inner, std::memory_order_relaxed);
			counters.onSurface.fetch_add(onSurface ? 1u : 0u, std::memory_order_relaxed);
			counters.onSurfaceCandidates.fetch_add(onSurface ? taken : 0u, std::memory_order_relaxed);
			counters.gathers.fetch_add(1u, std::memory_order_relaxed);
			counters.candidates.fetch_add(taken, std::memory_order_relaxed);
			counters.overflows.fetch_add(overflow ? 1u : 0u, std::memory_order_relaxed);
			counters.leaves.fetch_add(leaves, std::memory_order_relaxed);
			uint64_t seen = counters.maxCandidates.load(std::memory_order_relaxed);
			while (taken > seen && !counters.maxCandidates.compare_exchange_weak(seen, taken, std::memory_order_relaxed))
			{
			}
			const uint32_t bucket = std::min<uint32_t>(taken == 0u ? 0u : static_cast<uint32_t>(std::bit_width(taken)), kQueryHistogramBuckets - 1u);
			counters.histogram[bucket].fetch_add(1u, std::memory_order_relaxed);
		}

		// Sample points per triangle, as barycentric weights: the centre and one point towards each corner. Each stands for a
		// quarter of the triangle's area.
		constexpr std::array<std::array<float, 3>, 4> kSamples{ {
			{ 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f },
			{ 4.0f / 6.0f, 1.0f / 6.0f, 1.0f / 6.0f },
			{ 1.0f / 6.0f, 4.0f / 6.0f, 1.0f / 6.0f },
			{ 1.0f / 6.0f, 1.0f / 6.0f, 4.0f / 6.0f },
		} };

		// Coverage: an input sample is covered by candidate surface within the radius whose face meets the sample's within
		// 120 degrees. Perpendicular cover counts (a shallow recess wall may fold into its panel); the opposite side of a
		// coincident two-sided sheet does not.
		constexpr float kCoverageMinFacing = -0.5f;

		// Interpolated normals shorter than this cancel out: the candidate's counts as severely wrong, the original's as
		// unusable (a source defect, skipped).
		constexpr float kMinInterpolatedNormalLength = 1.0e-3f;

		// Match slack and radius floor, as fractions of the original's bounding diagonal.
		constexpr float kMatchSlackFraction = 1.0e-5f;
		constexpr float kRadiusFloorFraction = 1.0e-4f;

		constexpr uint32_t kLeafTriangles = 4u;
		// Ranges at least this large build their two subtrees as parallel tasks.
		constexpr uint32_t kParallelBuildTriangles = 32768u;
		// Such a range's own passes (bounds, split bins, partition) also run in slices of this many slots as tasks. Each slice's
		// minima and maxima are combined in slice order with the same std::min and std::max, which keep the first of equal values
		// and skip NaN as the serial pass does, so the node is the serial build's.
		constexpr uint32_t kBuildSliceTriangles = 16384u;

		// Runs body(slice, first, end) over a range of slots in slices as isolated tasks.
		template <typename Body>
		void ForBuildSlices(uint32_t begin, uint32_t end, const Body& body)
		{
			const uint32_t slices = (end - begin + kBuildSliceTriangles - 1u) / kBuildSliceTriangles;
			oneapi::tbb::this_task_arena::isolate([&] {
				oneapi::tbb::parallel_for(uint32_t(0), slices, [&](uint32_t slice) {
					const uint32_t first = begin + slice * kBuildSliceTriangles;
					body(slice, first, std::min(end, first + kBuildSliceTriangles));
				});
			});
		}

		// Source coverage samples at most this many original triangles per candidate (by stride over those near the group).
		constexpr size_t kMaxSourceSamples = 4096u;

		// The measurements below take one triangle (or source sample) per slot as tasks, a range at a time, and add the slots up
		// in order afterwards, so their sums are the serial loop's to the bit. Isolated: a waiting thread must not take another
		// group's task, which would reuse this thread's per-group state (the validation baseline). Small counts stay serial.
		constexpr size_t kMeasureRange = 64u;
		// A measurement that may stop early runs this many triangles at a time, so it does little work past where it stops.
		constexpr size_t kMeasureChunk = 2048u;

		template <typename Body>
		void ForMeasureRanges(size_t count, const Body& body)
		{
			if (count <= kMeasureRange)
			{
				body(size_t(0), count);
				return;
			}
			const QuerySite site = t_querySite;
			oneapi::tbb::this_task_arena::isolate([&] {
				oneapi::tbb::parallel_for(size_t(0), (count + kMeasureRange - 1u) / kMeasureRange, [&](size_t range) {
					QueryStatsScope scope(site);
					body(range * kMeasureRange, std::min(count, (range + 1u) * kMeasureRange));
				});
			});
		}

		inline const float* At(const float* base, size_t strideBytes, uint32_t index)
		{
			return reinterpret_cast<const float*>(reinterpret_cast<const std::byte*>(base) + static_cast<size_t>(index) * strideBytes);
		}

		inline void Sub(const float* a, const float* b, float* out)
		{
			out[0] = a[0] - b[0];
			out[1] = a[1] - b[1];
			out[2] = a[2] - b[2];
		}

		inline float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

		inline void Cross(const float* a, const float* b, float* out)
		{
			out[0] = a[1] * b[2] - a[2] * b[1];
			out[1] = a[2] * b[0] - a[0] * b[2];
			out[2] = a[0] * b[1] - a[1] * b[0];
		}

		// Unit face normal of abc, returning twice its area (zero normal for degenerate triangles).
		inline float FaceNormal(const float* a, const float* b, const float* c, float* normal)
		{
			float ab[3], ac[3];
			Sub(b, a, ab);
			Sub(c, a, ac);
			Cross(ab, ac, normal);
			const float length = std::sqrt(Dot(normal, normal));
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

		// Closest point on triangle abc to p as barycentric weights (Ericson, Real-Time Collision Detection 5.1.5); returns
		// the squared distance.
		float ClosestPointBarycentric(const float* p, const float* a, const float* b, const float* c, float* weights)
		{
			float ab[3], ac[3], ap[3];
			Sub(b, a, ab);
			Sub(c, a, ac);
			Sub(p, a, ap);
			const float d1 = Dot(ab, ap), d2 = Dot(ac, ap);
			float u = 1.0f, v = 0.0f, w = 0.0f;
			if (d1 <= 0.0f && d2 <= 0.0f)
			{
				u = 1.0f, v = 0.0f, w = 0.0f;
			}
			else
			{
				float bp[3];
				Sub(p, b, bp);
				const float d3 = Dot(ab, bp), d4 = Dot(ac, bp);
				float cp[3];
				Sub(p, c, cp);
				const float d5 = Dot(ab, cp), d6 = Dot(ac, cp);
				const float vc = d1 * d4 - d3 * d2;
				const float vb = d5 * d2 - d1 * d6;
				const float va = d3 * d6 - d5 * d4;
				if (d3 >= 0.0f && d4 <= d3)
				{
					u = 0.0f, v = 1.0f, w = 0.0f;
				}
				else if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
				{
					const float t = d1 / (d1 - d3);
					u = 1.0f - t, v = t, w = 0.0f;
				}
				else if (d6 >= 0.0f && d5 <= d6)
				{
					u = 0.0f, v = 0.0f, w = 1.0f;
				}
				else if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
				{
					const float t = d2 / (d2 - d6);
					u = 1.0f - t, v = 0.0f, w = t;
				}
				else if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
				{
					const float t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
					u = 0.0f, v = 1.0f - t, w = t;
				}
				else
				{
					const float sum = va + vb + vc;
					if (sum > 0.0f)
					{
						v = vb / sum;
						w = vc / sum;
						u = 1.0f - v - w;
					}
				}
			}
			weights[0] = u;
			weights[1] = v;
			weights[2] = w;
			float q[3];
			for (int axis = 0; axis < 3; ++axis)
			{
				q[axis] = a[axis] * u + b[axis] * v + c[axis] * w - p[axis];
			}
			return Dot(q, q);
		}

		inline float BoxDistanceSquared(const float* p, const float* boundsMin, const float* boundsMax)
		{
			float sum = 0.0f;
			for (int axis = 0; axis < 3; ++axis)
			{
				// Nested two-argument max: the initializer-list form calls the STL's out-of-line vectorized max_element.
				const float d = std::max(std::max(boundsMin[axis] - p[axis], 0.0f), p[axis] - boundsMax[axis]);
				sum += d * d;
			}
			return sum;
		}

		// BoxDistanceSquared with the three axes in one register (p's fourth lane unused): the same operations per axis, std::max(a, b)
		// being _mm_max_ps(b, a) (both return a on ties and unordered inputs), and the squares summed in the same order. boundsMin and
		// boundsMax are a node's (each followed by at least one more float).
		inline float BoxDistanceSquared(__m128 p, const float* boundsMin, const float* boundsMax)
		{
			const __m128 below = _mm_max_ps(_mm_setzero_ps(), _mm_sub_ps(_mm_loadu_ps(boundsMin), p));
			const __m128 d = _mm_max_ps(_mm_sub_ps(p, _mm_loadu_ps(boundsMax)), below);
			const __m128 squares = _mm_mul_ps(d, d);
			const __m128 sum = _mm_add_ss(_mm_add_ss(squares, _mm_shuffle_ps(squares, squares, _MM_SHUFFLE(1, 1, 1, 1))),
				_mm_shuffle_ps(squares, squares, _MM_SHUFFLE(2, 2, 2, 2)));
			return _mm_cvtss_f32(sum);
		}

		// Interpolated unit normal of a triangle's corners at weights; false when it cancels out.
		inline bool InterpolatedNormal(const SimplifySurfaceView& surface, size_t firstIndex, const float* weights, float* normal)
		{
			normal[0] = normal[1] = normal[2] = 0.0f;
			for (int corner = 0; corner < 3; ++corner)
			{
				const float* n = At(surface.normals, surface.normalStride, surface.indices[firstIndex + corner]);
				normal[0] += n[0] * weights[corner];
				normal[1] += n[1] * weights[corner];
				normal[2] += n[2] * weights[corner];
			}
			const float length = std::sqrt(Dot(normal, normal));
			if (!(length > kMinInterpolatedNormalLength))
			{
				return false;
			}
			normal[0] /= length;
			normal[1] /= length;
			normal[2] /= length;
			return true;
		}

		uint64_t ElapsedNs(std::chrono::steady_clock::time_point start)
		{
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
		}

		struct TraversalStack
		{
			std::array<uint32_t, 128> nodes;
			uint32_t size = 0;
			void Push(uint32_t node) { if (size < nodes.size()) nodes[size++] = node; }
			bool Empty() const { return size == 0; }
			uint32_t Pop() { return nodes[--size]; }
		};

		// TraversalStack with each node's box distance, computed once when its parent pushes it (the same value a pop would
		// compute again).
		struct DistanceStack
		{
			std::array<uint32_t, 128> nodes;
			std::array<float, 128> distances;
			uint32_t size = 0;
			void Push(uint32_t node, float distance)
			{
				if (size < nodes.size())
				{
					nodes[size] = node;
					distances[size] = distance;
					++size;
				}
			}
			bool Empty() const { return size == 0; }
		};

		// Facing bins (SimplifyTriangleBvh::SetMatchShell): a normal's cell on a grid x grid octahedral map (8: 64 directions about 25
		// degrees apart, 16: 256 about 12 degrees apart). A zero or non-finite normal is bin 0.
		inline uint32_t FacingBin(float x, float y, float z, uint32_t grid)
		{
			const float s = std::fabs(x) + std::fabs(y) + std::fabs(z);
			if (!(s > 0.0f) || !(s < std::numeric_limits<float>::infinity()))
			{
				return 0u;
			}
			float u = x / s, v = y / s;
			if (z < 0.0f)
			{
				const float folded = (1.0f - std::fabs(v)) * (u >= 0.0f ? 1.0f : -1.0f);
				v = (1.0f - std::fabs(u)) * (v >= 0.0f ? 1.0f : -1.0f);
				u = folded;
			}
			const float half = 0.5f * static_cast<float>(grid);
			const int last = static_cast<int>(grid) - 1;
			const int i = std::clamp(static_cast<int>((u + 1.0f) * half), 0, last);
			const int j = std::clamp(static_cast<int>((v + 1.0f) * half), 0, last);
			return static_cast<uint32_t>(j * static_cast<int>(grid) + i);
		}

		inline __m128 Select(__m128 mask, __m128 a, __m128 b) { return _mm_or_ps(_mm_and_ps(mask, a), _mm_andnot_ps(mask, b)); }
		inline __m128 Dot4(__m128 ax, __m128 ay, __m128 az, __m128 bx, __m128 by, __m128 bz)
		{
			return _mm_add_ps(_mm_add_ps(_mm_mul_ps(ax, bx), _mm_mul_ps(ay, by)), _mm_mul_ps(az, bz));
		}

		// ClosestPointBarycentric for four triangles at once (lanes of a, b, c), with the same operations in the same order per
		// lane, so each lane's weights and squared distance are bitwise the scalar function's. Branches become selects in the
		// scalar function's priority order.
		inline void ClosestPointBarycentric4(
			const float* p, const float* ax_, const float* ay_, const float* az_, const float* bx_, const float* by_, const float* bz_,
			const float* cx_, const float* cy_, const float* cz_, float* distance, float* u, float* v, float* w)
		{
			const __m128 px = _mm_set1_ps(p[0]), py = _mm_set1_ps(p[1]), pz = _mm_set1_ps(p[2]);
			const __m128 ax = _mm_load_ps(ax_), ay = _mm_load_ps(ay_), az = _mm_load_ps(az_);
			const __m128 bx = _mm_load_ps(bx_), by = _mm_load_ps(by_), bz = _mm_load_ps(bz_);
			const __m128 cx = _mm_load_ps(cx_), cy = _mm_load_ps(cy_), cz = _mm_load_ps(cz_);
			const __m128 abx = _mm_sub_ps(bx, ax), aby = _mm_sub_ps(by, ay), abz = _mm_sub_ps(bz, az);
			const __m128 acx = _mm_sub_ps(cx, ax), acy = _mm_sub_ps(cy, ay), acz = _mm_sub_ps(cz, az);
			const __m128 apx = _mm_sub_ps(px, ax), apy = _mm_sub_ps(py, ay), apz = _mm_sub_ps(pz, az);
			const __m128 d1 = Dot4(abx, aby, abz, apx, apy, apz), d2 = Dot4(acx, acy, acz, apx, apy, apz);
			const __m128 bpx = _mm_sub_ps(px, bx), bpy = _mm_sub_ps(py, by), bpz = _mm_sub_ps(pz, bz);
			const __m128 d3 = Dot4(abx, aby, abz, bpx, bpy, bpz), d4 = Dot4(acx, acy, acz, bpx, bpy, bpz);
			const __m128 cpx = _mm_sub_ps(px, cx), cpy = _mm_sub_ps(py, cy), cpz = _mm_sub_ps(pz, cz);
			const __m128 d5 = Dot4(abx, aby, abz, cpx, cpy, cpz), d6 = Dot4(acx, acy, acz, cpx, cpy, cpz);
			const __m128 vc = _mm_sub_ps(_mm_mul_ps(d1, d4), _mm_mul_ps(d3, d2));
			const __m128 vb = _mm_sub_ps(_mm_mul_ps(d5, d2), _mm_mul_ps(d1, d6));
			const __m128 va = _mm_sub_ps(_mm_mul_ps(d3, d6), _mm_mul_ps(d5, d4));
			const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
			const __m128 d43 = _mm_sub_ps(d4, d3), d56 = _mm_sub_ps(d5, d6);

			// Interior (or the default when the sum is not positive).
			const __m128 sum = _mm_add_ps(_mm_add_ps(va, vb), vc);
			const __m128 interior = _mm_cmpgt_ps(sum, zero);
			const __m128 v6 = _mm_div_ps(vb, sum), w6 = _mm_div_ps(vc, sum);
			__m128 U = Select(interior, _mm_sub_ps(_mm_sub_ps(one, v6), w6), one);
			__m128 V = Select(interior, v6, zero);
			__m128 W = Select(interior, w6, zero);
			// Edge bc.
			const __m128 m5 = _mm_and_ps(_mm_and_ps(_mm_cmple_ps(va, zero), _mm_cmpge_ps(d43, zero)), _mm_cmpge_ps(d56, zero));
			const __m128 t5 = _mm_div_ps(d43, _mm_add_ps(d43, d56));
			U = Select(m5, zero, U);
			V = Select(m5, _mm_sub_ps(one, t5), V);
			W = Select(m5, t5, W);
			// Edge ac.
			const __m128 m4 = _mm_and_ps(_mm_and_ps(_mm_cmple_ps(vb, zero), _mm_cmpge_ps(d2, zero)), _mm_cmple_ps(d6, zero));
			const __m128 t4 = _mm_div_ps(d2, _mm_sub_ps(d2, d6));
			U = Select(m4, _mm_sub_ps(one, t4), U);
			V = Select(m4, zero, V);
			W = Select(m4, t4, W);
			// Corner c.
			const __m128 m3 = _mm_and_ps(_mm_cmpge_ps(d6, zero), _mm_cmple_ps(d5, d6));
			U = Select(m3, zero, U);
			V = Select(m3, zero, V);
			W = Select(m3, one, W);
			// Edge ab.
			const __m128 m2 = _mm_and_ps(_mm_and_ps(_mm_cmple_ps(vc, zero), _mm_cmpge_ps(d1, zero)), _mm_cmple_ps(d3, zero));
			const __m128 t2 = _mm_div_ps(d1, _mm_sub_ps(d1, d3));
			U = Select(m2, _mm_sub_ps(one, t2), U);
			V = Select(m2, t2, V);
			W = Select(m2, zero, W);
			// Corner b.
			const __m128 m1 = _mm_and_ps(_mm_cmpge_ps(d3, zero), _mm_cmple_ps(d4, d3));
			U = Select(m1, zero, U);
			V = Select(m1, one, V);
			W = Select(m1, zero, W);
			// Corner a.
			const __m128 m0 = _mm_and_ps(_mm_cmple_ps(d1, zero), _mm_cmple_ps(d2, zero));
			U = Select(m0, one, U);
			V = Select(m0, zero, V);
			W = Select(m0, zero, W);

			const __m128 qx = _mm_sub_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(ax, U), _mm_mul_ps(bx, V)), _mm_mul_ps(cx, W)), px);
			const __m128 qy = _mm_sub_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(ay, U), _mm_mul_ps(by, V)), _mm_mul_ps(cy, W)), py);
			const __m128 qz = _mm_sub_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(az, U), _mm_mul_ps(bz, V)), _mm_mul_ps(cz, W)), pz);
			_mm_store_ps(distance, Dot4(qx, qy, qz, qx, qy, qz));
			if (u != nullptr)
			{
				_mm_store_ps(u, U);
				_mm_store_ps(v, V);
				_mm_store_ps(w, W);
			}
		}

	}

	void SimplifyTriangleBvh::Build(const SimplifySurfaceView& surface)
	{
		ZoneScopedN("ClusterLODSimplifyValidation::BuildBvh");
		static std::atomic<uint64_t> s_buildIds = 0u;
		m_buildId = s_buildIds.fetch_add(1u, std::memory_order_relaxed) + 1u;
		m_nodes.clear();
		m_triangles.clear();
		const size_t triangleCount = surface.indexCount / 3u;
		if (triangleCount == 0u)
		{
			return;
		}

		m_triangles.resize(triangleCount);
		std::vector<float> centroids(triangleCount * 3u);
		std::vector<uint32_t> order(triangleCount);
		// Per-triangle work (here and below, each index written once) runs in ranges as tasks for large surfaces.
		const auto forRanges = [&](size_t count, const auto& body) {
			constexpr size_t kRange = 16384u;
			if (count < 2u * kRange)
			{
				body(size_t(0), count);
				return;
			}
			oneapi::tbb::this_task_arena::isolate([&] {
				oneapi::tbb::parallel_for(size_t(0), (count + kRange - 1u) / kRange,
					[&](size_t range) { body(range * kRange, std::min(count, (range + 1u) * kRange)); });
			});
		};
		forRanges(triangleCount, [&](size_t firstTriangle, size_t endTriangle) {
		for (size_t triangle = firstTriangle; triangle < endTriangle; ++triangle)
		{
			Triangle& t = m_triangles[triangle];
			const float* a = At(surface.positions, surface.positionStride, surface.indices[triangle * 3u + 0u]);
			const float* b = At(surface.positions, surface.positionStride, surface.indices[triangle * 3u + 1u]);
			const float* c = At(surface.positions, surface.positionStride, surface.indices[triangle * 3u + 2u]);
			std::memcpy(t.a, a, sizeof(t.a));
			std::memcpy(t.b, b, sizeof(t.b));
			std::memcpy(t.c, c, sizeof(t.c));
			FaceNormal(t.a, t.b, t.c, t.normal);
			t.source = static_cast<uint32_t>(triangle);
			for (int axis = 0; axis < 3; ++axis)
			{
				centroids[triangle * 3u + axis] = (a[axis] + b[axis] + c[axis]) * (1.0f / 3.0f);
			}
			order[triangle] = static_cast<uint32_t>(triangle);
		}
		});

		m_nodes.reserve(2u * (triangleCount / kLeafTriangles + 1u));
		BuildNode(order, centroids, 0u, static_cast<uint32_t>(triangleCount));

		// Leaves address triangles by slot: store them in BVH order.
		std::vector<Triangle> sorted(triangleCount);
		forRanges(triangleCount, [&](size_t firstSlot, size_t endSlot) {
			for (size_t slot = firstSlot; slot < endSlot; ++slot)
			{
				sorted[slot] = m_triangles[order[slot]];
			}
		});
		m_triangles.swap(sorted);

		// Each leaf's triangles as lanes, its block index above the count bits (blocks in node order).
		m_blocks.clear();
		std::vector<uint32_t> leafNodes;
		for (uint32_t nodeIndex = 0; nodeIndex < m_nodes.size(); ++nodeIndex)
		{
			if (m_nodes[nodeIndex].count != 0u)
			{
				leafNodes.push_back(nodeIndex);
			}
		}
		m_blocks.resize(leafNodes.size());
		forRanges(leafNodes.size(), [&](size_t firstLeaf, size_t endLeaf) {
		for (size_t leaf = firstLeaf; leaf < endLeaf; ++leaf)
		{
			Node& node = m_nodes[leafNodes[leaf]];
			LeafBlock block{};
			for (uint32_t lane = 0; lane < 4u; ++lane)
			{
				const Triangle& t = m_triangles[node.first + std::min(lane, node.count - 1u)];
				block.ax[lane] = t.a[0], block.ay[lane] = t.a[1], block.az[lane] = t.a[2];
				block.bx[lane] = t.b[0], block.by[lane] = t.b[1], block.bz[lane] = t.b[2];
				block.cx[lane] = t.c[0], block.cy[lane] = t.c[1], block.cz[lane] = t.c[2];
				block.nx[lane] = t.normal[0], block.ny[lane] = t.normal[1], block.nz[lane] = t.normal[2];
				block.source[lane] = t.source;
			}
			node.count |= static_cast<uint32_t>(leaf) << 3u;
			m_blocks[leaf] = block;
		}
		});
	}

	void SimplifyTriangleBvh::SetMatchShell(float reachScale, uint32_t maxCandidates, uint32_t facingGrid)
	{
		m_reachScale = reachScale;
		m_facingGrid = facingGrid == 0u ? 0u : facingGrid <= 4u ? 4u : facingGrid <= 8u ? 8u : 16u;
		m_maxCandidates = m_facingGrid != 0u ? 0u : std::min(maxCandidates, 32u);
		m_nodeBins.clear();
		m_blockBins.clear();
		if (m_facingGrid == 0u)
		{
			return;
		}
		// Each leaf lane's facing bin, and each node's bins: its triangles' (a node's children come after it).
		m_blockBins.resize(m_blocks.size());
		for (size_t block = 0; block < m_blocks.size(); ++block)
		{
			for (uint32_t lane = 0; lane < 4u; ++lane)
			{
				m_blockBins[block][lane] = static_cast<uint8_t>(FacingBin(m_blocks[block].nx[lane], m_blocks[block].ny[lane], m_blocks[block].nz[lane], m_facingGrid));
			}
		}
		m_nodeBins.assign(m_nodes.size(), BinMask{});
		for (size_t i = m_nodes.size(); i-- > 0u;)
		{
			const Node& node = m_nodes[i];
			BinMask& bins = m_nodeBins[i];
			if (node.count == 0u)
			{
				for (uint32_t word = 0; word < 4u; ++word)
				{
					bins.words[word] = m_nodeBins[i + 1u].words[word] | m_nodeBins[node.first].words[word];
				}
				continue;
			}
			for (uint32_t k = 0; k < LeafCount(node); ++k)
			{
				const Triangle& t = m_triangles[node.first + k];
				const uint32_t bin = FacingBin(t.normal[0], t.normal[1], t.normal[2], m_facingGrid);
				bins.words[bin >> 6u] |= uint64_t(1) << (bin & 63u);
			}
		}
	}

	uint32_t SimplifyTriangleBvh::BuildNode(std::vector<uint32_t>& order, std::vector<float>& centroids, uint32_t begin, uint32_t end)
	{
		return BuildNodeInto(m_nodes, order, centroids, begin, end);
	}

	uint32_t SimplifyTriangleBvh::BuildNodeInto(std::vector<Node>& nodes, std::vector<uint32_t>& order, std::vector<float>& centroids, uint32_t begin, uint32_t end) const
	{
		const uint32_t nodeIndex = static_cast<uint32_t>(nodes.size());
		nodes.push_back({});
		Node node{};
		const bool parallelRange = end - begin >= kParallelBuildTriangles;

		// The node's bounds and its centroids' bounds over a run of slots, folded into the given running values.
		struct RangeBounds
		{
			float boundsMin[3], boundsMax[3], centroidMin[3], centroidMax[3];
		};
		const auto emptyBounds = [] {
			RangeBounds bounds;
			for (int axis = 0; axis < 3; ++axis)
			{
				bounds.boundsMin[axis] = bounds.centroidMin[axis] = std::numeric_limits<float>::max();
				bounds.boundsMax[axis] = bounds.centroidMax[axis] = -std::numeric_limits<float>::max();
			}
			return bounds;
		};
		const auto foldBounds = [&](RangeBounds& bounds, uint32_t first, uint32_t last) {
			for (uint32_t slot = first; slot < last; ++slot)
			{
				const Triangle& t = m_triangles[order[slot]];
				for (int axis = 0; axis < 3; ++axis)
				{
					bounds.boundsMin[axis] = std::min(std::min(std::min(bounds.boundsMin[axis], t.a[axis]), t.b[axis]), t.c[axis]);
					bounds.boundsMax[axis] = std::max(std::max(std::max(bounds.boundsMax[axis], t.a[axis]), t.b[axis]), t.c[axis]);
					bounds.centroidMin[axis] = std::min(bounds.centroidMin[axis], centroids[order[slot] * 3u + axis]);
					bounds.centroidMax[axis] = std::max(bounds.centroidMax[axis], centroids[order[slot] * 3u + axis]);
				}
			}
		};
		RangeBounds rangeBounds = emptyBounds();
		if (!parallelRange)
		{
			foldBounds(rangeBounds, begin, end);
		}
		else
		{
			std::vector<RangeBounds> slices((end - begin + kBuildSliceTriangles - 1u) / kBuildSliceTriangles, emptyBounds());
			ForBuildSlices(begin, end, [&](uint32_t slice, uint32_t first, uint32_t last) { foldBounds(slices[slice], first, last); });
			for (const RangeBounds& slice : slices)
			{
				for (int axis = 0; axis < 3; ++axis)
				{
					rangeBounds.boundsMin[axis] = std::min(rangeBounds.boundsMin[axis], slice.boundsMin[axis]);
					rangeBounds.boundsMax[axis] = std::max(rangeBounds.boundsMax[axis], slice.boundsMax[axis]);
					rangeBounds.centroidMin[axis] = std::min(rangeBounds.centroidMin[axis], slice.centroidMin[axis]);
					rangeBounds.centroidMax[axis] = std::max(rangeBounds.centroidMax[axis], slice.centroidMax[axis]);
				}
			}
		}
		float centroidMin[3], centroidMax[3];
		for (int axis = 0; axis < 3; ++axis)
		{
			node.boundsMin[axis] = rangeBounds.boundsMin[axis];
			node.boundsMax[axis] = rangeBounds.boundsMax[axis];
			centroidMin[axis] = rangeBounds.centroidMin[axis];
			centroidMax[axis] = rangeBounds.centroidMax[axis];
		}

		if (end - begin <= kLeafTriangles)
		{
			node.first = begin;
			node.count = end - begin;
			nodes[nodeIndex] = node;
			return nodeIndex;
		}

		int splitAxis = 0;
		for (int axis = 1; axis < 3; ++axis)
		{
			if (centroidMax[axis] - centroidMin[axis] > centroidMax[splitAxis] - centroidMin[splitAxis])
			{
				splitAxis = axis;
			}
		}
		// Binned surface-area split (16 bins per axis over the centroid bounds). Bistro's overlapping sheets make the median
		// split's boxes overlap badly; this halves the fit's traversal cost. A degenerate split falls back to the median.
		uint32_t middle = begin + (end - begin) / 2u;
		bool split = false;
		if (centroidMax[splitAxis] > centroidMin[splitAxis])
		{
			constexpr int kBins = 16;
			constexpr float kHuge = std::numeric_limits<float>::max();
			const auto binOf = [&](uint32_t triangle, int axis, float extent) {
				const int bin = static_cast<int>((centroids[triangle * 3u + axis] - centroidMin[axis]) / extent * kBins);
				return std::min(std::max(bin, 0), kBins - 1);
			};
			const auto area = [](const float* lo, const float* hi) {
				const float e0 = hi[0] - lo[0], e1 = hi[1] - lo[1], e2 = hi[2] - lo[2];
				return e0 * e1 + e1 * e2 + e2 * e0;
			};
			// One axis's bins over a run of slots, folded into the given running values.
			struct AxisBins
			{
				uint32_t counts[kBins];
				float binMin[kBins][3], binMax[kBins][3];
			};
			const auto emptyBins = [] {
				AxisBins bins;
				for (int bin = 0; bin < kBins; ++bin)
				{
					bins.counts[bin] = 0u;
					for (int k = 0; k < 3; ++k)
					{
						bins.binMin[bin][k] = kHuge;
						bins.binMax[bin][k] = -kHuge;
					}
				}
				return bins;
			};
			// All three axes in one pass over the slots (each axis's bins see the slots in the same order as a pass of its own).
			const float extents[3] = { centroidMax[0] - centroidMin[0], centroidMax[1] - centroidMin[1], centroidMax[2] - centroidMin[2] };
			const auto foldBins = [&](AxisBins* bins, uint32_t first, uint32_t last) {
				for (uint32_t slot = first; slot < last; ++slot)
				{
					const uint32_t triangle = order[slot];
					const Triangle& t = m_triangles[triangle];
					float lo[3], hi[3];
					for (int k = 0; k < 3; ++k)
					{
						lo[k] = std::min(std::min(t.a[k], t.b[k]), t.c[k]);
						hi[k] = std::max(std::max(t.a[k], t.b[k]), t.c[k]);
					}
					for (int axis = 0; axis < 3; ++axis)
					{
						if (!(extents[axis] > 0.0f))
						{
							continue;
						}
						const int bin = binOf(triangle, axis, extents[axis]);
						++bins[axis].counts[bin];
						for (int k = 0; k < 3; ++k)
						{
							bins[axis].binMin[bin][k] = std::min(bins[axis].binMin[bin][k], lo[k]);
							bins[axis].binMax[bin][k] = std::max(bins[axis].binMax[bin][k], hi[k]);
						}
					}
				}
			};
			AxisBins axisBins[3] = { emptyBins(), emptyBins(), emptyBins() };
			if (!parallelRange)
			{
				foldBins(axisBins, begin, end);
			}
			else
			{
				std::vector<std::array<AxisBins, 3>> slices((end - begin + kBuildSliceTriangles - 1u) / kBuildSliceTriangles);
				ForBuildSlices(begin, end, [&](uint32_t slice, uint32_t first, uint32_t last) {
					slices[slice] = { emptyBins(), emptyBins(), emptyBins() };
					foldBins(slices[slice].data(), first, last);
				});
				for (const std::array<AxisBins, 3>& slice : slices)
				{
					for (int axis = 0; axis < 3; ++axis)
					{
						for (int bin = 0; bin < kBins; ++bin)
						{
							axisBins[axis].counts[bin] += slice[axis].counts[bin];
							for (int k = 0; k < 3; ++k)
							{
								axisBins[axis].binMin[bin][k] = std::min(axisBins[axis].binMin[bin][k], slice[axis].binMin[bin][k]);
								axisBins[axis].binMax[bin][k] = std::max(axisBins[axis].binMax[bin][k], slice[axis].binMax[bin][k]);
							}
						}
					}
				}
			}
			float bestCost = kHuge;
			int bestAxis = -1, bestBin = -1;
			for (int axis = 0; axis < 3; ++axis)
			{
				const float extent = centroidMax[axis] - centroidMin[axis];
				if (!(extent > 0.0f))
				{
					continue;
				}
				const uint32_t* counts = axisBins[axis].counts;
				const auto& binMin = axisBins[axis].binMin;
				const auto& binMax = axisBins[axis].binMax;
				float rightArea[kBins] = {};
				uint32_t rightCount[kBins] = {};
				float lo[3] = { kHuge, kHuge, kHuge }, hi[3] = { -kHuge, -kHuge, -kHuge };
				uint32_t count = 0u;
				for (int bin = kBins - 1; bin > 0; --bin)
				{
					for (int k = 0; k < 3; ++k)
					{
						lo[k] = std::min(lo[k], binMin[bin][k]);
						hi[k] = std::max(hi[k], binMax[bin][k]);
					}
					count += counts[bin];
					rightCount[bin] = count;
					rightArea[bin] = count != 0u ? area(lo, hi) : 0.0f;
				}
				for (int k = 0; k < 3; ++k)
				{
					lo[k] = kHuge;
					hi[k] = -kHuge;
				}
				count = 0u;
				for (int bin = 0; bin < kBins - 1; ++bin)
				{
					for (int k = 0; k < 3; ++k)
					{
						lo[k] = std::min(lo[k], binMin[bin][k]);
						hi[k] = std::max(hi[k], binMax[bin][k]);
					}
					count += counts[bin];
					if (count == 0u || rightCount[bin + 1] == 0u)
					{
						continue;
					}
					const float cost = area(lo, hi) * static_cast<float>(count) + rightArea[bin + 1] * static_cast<float>(rightCount[bin + 1]);
					if (cost < bestCost)
					{
						bestCost = cost;
						bestAxis = axis;
						bestBin = bin;
					}
				}
			}
			if (bestAxis >= 0)
			{
				const float extent = centroidMax[bestAxis] - centroidMin[bestAxis];
				const auto goesLeft = [&](uint32_t triangle) { return binOf(triangle, bestAxis, extent) <= bestBin; };
				uint32_t partition = begin;
				if (!parallelRange)
				{
					// std::stable_partition without its temporary buffer per node: the left side compacts in place, the right side
					// waits in a per-thread list (used up before this node recurses).
					thread_local std::vector<uint32_t> rightSide;
					rightSide.clear();
					partition = begin;
					for (uint32_t slot = begin; slot < end; ++slot)
					{
						const uint32_t triangle = order[slot];
						if (goesLeft(triangle))
						{
							order[partition++] = triangle;
						}
						else
						{
							rightSide.push_back(triangle);
						}
					}
					std::copy(rightSide.begin(), rightSide.end(), order.begin() + partition);
				}
				else
				{
					// std::stable_partition in slices: each slice counts its left side, then writes both sides at their offsets.
					const uint32_t sliceCount = (end - begin + kBuildSliceTriangles - 1u) / kBuildSliceTriangles;
					std::vector<uint32_t> leftCounts(sliceCount, 0u);
					ForBuildSlices(begin, end, [&](uint32_t slice, uint32_t first, uint32_t last) {
						for (uint32_t slot = first; slot < last; ++slot)
						{
							leftCounts[slice] += goesLeft(order[slot]) ? 1u : 0u;
						}
					});
					uint32_t leftTotal = 0u;
					std::vector<uint32_t> leftOffsets(sliceCount), rightOffsets(sliceCount);
					for (uint32_t slice = 0; slice < sliceCount; ++slice)
					{
						leftOffsets[slice] = leftTotal;
						leftTotal += leftCounts[slice];
					}
					uint32_t rightTotal = leftTotal;
					for (uint32_t slice = 0; slice < sliceCount; ++slice)
					{
						rightOffsets[slice] = rightTotal;
						rightTotal += std::min(end, begin + (slice + 1u) * kBuildSliceTriangles) - (begin + slice * kBuildSliceTriangles) - leftCounts[slice];
					}
					std::vector<uint32_t> partitioned(end - begin);
					ForBuildSlices(begin, end, [&](uint32_t slice, uint32_t first, uint32_t last) {
						uint32_t left = leftOffsets[slice], right = rightOffsets[slice];
						for (uint32_t slot = first; slot < last; ++slot)
						{
							partitioned[goesLeft(order[slot]) ? left++ : right++] = order[slot];
						}
					});
					std::copy(partitioned.begin(), partitioned.end(), order.begin() + begin);
					partition = begin + leftTotal;
				}
				if (partition > begin && partition < end)
				{
					middle = partition;
					split = true;
				}
			}
		}
		if (!split)
		{
			std::nth_element(order.begin() + begin, order.begin() + middle, order.begin() + end, [&](uint32_t lhs, uint32_t rhs) {
				return centroids[lhs * 3u + splitAxis] < centroids[rhs * 3u + splitAxis];
			});
		}

		if (end - begin < kParallelBuildTriangles)
		{
			BuildNodeInto(nodes, order, centroids, begin, middle);
			node.first = BuildNodeInto(nodes, order, centroids, middle, end);
		}
		else
		{
			// Large ranges: the two subtrees are built side by side into their own node lists (their ranges of order are disjoint),
			// then placed as the serial build places them, left after this node and right after left, with their child indices
			// moved by where they land. The tree is the serial build's.
			std::vector<Node> left, right;
			oneapi::tbb::this_task_arena::isolate([&] {
				oneapi::tbb::parallel_invoke(
					[&] { BuildNodeInto(left, order, centroids, begin, middle); },
					[&] { BuildNodeInto(right, order, centroids, middle, end); });
			});
			const uint32_t leftBase = nodeIndex + 1u;
			const uint32_t rightBase = leftBase + static_cast<uint32_t>(left.size());
			for (Node& child : left)
			{
				child.first += child.count == 0u ? leftBase : 0u;
			}
			for (Node& child : right)
			{
				child.first += child.count == 0u ? rightBase : 0u;
			}
			nodes.insert(nodes.end(), left.begin(), left.end());
			nodes.insert(nodes.end(), right.begin(), right.end());
			node.first = rightBase;
		}
		node.count = 0u;
		nodes[nodeIndex] = node;
		return nodeIndex;
	}

	bool SimplifyTriangleBvh::NearestCrossing(const float point[3], const float direction[3], float maxDistance, float minFacing, Match& out) const
	{
		if (m_nodes.empty() || !(maxDistance > 0.0f))
		{
			return false;
		}
		float best = maxDistance;
		uint32_t bestTriangle = UINT32_MAX;
		float bestWeights[3] = {};
		float inverse[3];
		for (int axis = 0; axis < 3; ++axis)
		{
			inverse[axis] = direction[axis] != 0.0f ? 1.0f / direction[axis] : 0.0f;
		}
		TraversalStack stack;
		stack.Push(0u);
		while (!stack.Empty())
		{
			const uint32_t nodeIndex = stack.Pop();
			const Node& node = m_nodes[nodeIndex];
			// The segment t in [-best, best] against the node's box (slabs).
			float low = -best, high = best;
			for (int axis = 0; axis < 3 && low <= high; ++axis)
			{
				if (direction[axis] == 0.0f)
				{
					if (point[axis] < node.boundsMin[axis] || point[axis] > node.boundsMax[axis])
					{
						low = 1.0f;
						high = 0.0f;
					}
					continue;
				}
				float t0 = (node.boundsMin[axis] - point[axis]) * inverse[axis];
				float t1 = (node.boundsMax[axis] - point[axis]) * inverse[axis];
				if (t0 > t1)
				{
					std::swap(t0, t1);
				}
				low = std::max(low, t0);
				high = std::min(high, t1);
			}
			if (!(low <= high))
			{
				continue;
			}
			if (node.count == 0u)
			{
				stack.Push(node.first);
				stack.Push(nodeIndex + 1u);
				continue;
			}
			for (uint32_t slot = node.first; slot < node.first + LeafCount(node); ++slot)
			{
				const Triangle& triangle = m_triangles[slot];
				const float facing = Dot(triangle.normal, direction);
				if (!(facing >= minFacing) || !(facing > 0.0f))
				{
					continue;
				}
				float toPlane[3];
				Sub(triangle.a, point, toPlane);
				const float t = Dot(toPlane, triangle.normal) / facing;
				const float distance = std::fabs(t);
				if (!(distance <= best) || (distance == best && triangle.source >= bestTriangle))
				{
					continue;
				}
				const float crossing[3] = { point[0] + t * direction[0], point[1] + t * direction[1], point[2] + t * direction[2] };
				float v0[3], v1[3], v2[3];
				Sub(triangle.b, triangle.a, v0);
				Sub(triangle.c, triangle.a, v1);
				Sub(crossing, triangle.a, v2);
				const float d00 = Dot(v0, v0), d01 = Dot(v0, v1), d11 = Dot(v1, v1), d20 = Dot(v2, v0), d21 = Dot(v2, v1);
				const float denom = d00 * d11 - d01 * d01;
				if (!(std::fabs(denom) > 0.0f))
				{
					continue;
				}
				const float w1 = (d11 * d20 - d01 * d21) / denom;
				const float w2 = (d00 * d21 - d01 * d20) / denom;
				const float w0 = 1.0f - w1 - w2;
				if (std::min(std::min(w0, w1), w2) < -1.0e-4f)
				{
					continue;
				}
				best = distance;
				bestTriangle = triangle.source;
				bestWeights[0] = w0;
				bestWeights[1] = w1;
				bestWeights[2] = w2;
			}
		}
		if (bestTriangle == UINT32_MAX)
		{
			return false;
		}
		out.triangle = bestTriangle;
		std::memcpy(out.barycentric, bestWeights, sizeof(bestWeights));
		out.distanceSquared = best * best;
		out.facing = 0.0f;
		return true;
	}

	bool SimplifyTriangleBvh::FrontmostCrossing(const float point[3], const float direction[3], float maxDistance, float minFacing, Match& out) const
	{
		if (m_nodes.empty() || !(maxDistance > 0.0f))
		{
			return false;
		}
		float best = -maxDistance; // the frontmost offset found so far (crossings behind it cannot win)
		uint32_t bestTriangle = UINT32_MAX;
		float bestWeights[3] = {};
		float inverse[3];
		for (int axis = 0; axis < 3; ++axis)
		{
			inverse[axis] = direction[axis] != 0.0f ? 1.0f / direction[axis] : 0.0f;
		}
		TraversalStack stack;
		stack.Push(0u);
		while (!stack.Empty())
		{
			const uint32_t nodeIndex = stack.Pop();
			const Node& node = m_nodes[nodeIndex];
			// The segment t in [best, maxDistance] against the node's box (slabs).
			float low = best, high = maxDistance;
			for (int axis = 0; axis < 3 && low <= high; ++axis)
			{
				if (direction[axis] == 0.0f)
				{
					if (point[axis] < node.boundsMin[axis] || point[axis] > node.boundsMax[axis])
					{
						low = 1.0f;
						high = 0.0f;
					}
					continue;
				}
				float t0 = (node.boundsMin[axis] - point[axis]) * inverse[axis];
				float t1 = (node.boundsMax[axis] - point[axis]) * inverse[axis];
				if (t0 > t1)
				{
					std::swap(t0, t1);
				}
				low = std::max(low, t0);
				high = std::min(high, t1);
			}
			if (!(low <= high))
			{
				continue;
			}
			if (node.count == 0u)
			{
				stack.Push(node.first);
				stack.Push(nodeIndex + 1u);
				continue;
			}
			for (uint32_t slot = node.first; slot < node.first + LeafCount(node); ++slot)
			{
				const Triangle& triangle = m_triangles[slot];
				const float facing = Dot(triangle.normal, direction);
				if (!(facing >= minFacing) || !(facing > 0.0f))
				{
					continue;
				}
				float toPlane[3];
				Sub(triangle.a, point, toPlane);
				const float t = Dot(toPlane, triangle.normal) / facing;
				if (!(t >= -maxDistance && t <= maxDistance) || t < best || (bestTriangle != UINT32_MAX && t == best && triangle.source >= bestTriangle))
				{
					continue;
				}
				const float crossing[3] = { point[0] + t * direction[0], point[1] + t * direction[1], point[2] + t * direction[2] };
				float v0[3], v1[3], v2[3];
				Sub(triangle.b, triangle.a, v0);
				Sub(triangle.c, triangle.a, v1);
				Sub(crossing, triangle.a, v2);
				const float d00 = Dot(v0, v0), d01 = Dot(v0, v1), d11 = Dot(v1, v1), d20 = Dot(v2, v0), d21 = Dot(v2, v1);
				const float denom = d00 * d11 - d01 * d01;
				if (!(std::fabs(denom) > 0.0f))
				{
					continue;
				}
				const float w1 = (d11 * d20 - d01 * d21) / denom;
				const float w2 = (d00 * d21 - d01 * d20) / denom;
				const float w0 = 1.0f - w1 - w2;
				if (std::min(std::min(w0, w1), w2) < -1.0e-4f)
				{
					continue;
				}
				best = t;
				bestTriangle = triangle.source;
				bestWeights[0] = w0;
				bestWeights[1] = w1;
				bestWeights[2] = w2;
			}
		}
		if (bestTriangle == UINT32_MAX)
		{
			return false;
		}
		out.triangle = bestTriangle;
		std::memcpy(out.barycentric, bestWeights, sizeof(bestWeights));
		out.distanceSquared = best * best;
		out.facing = best;
		return true;
	}

	void SimplifyTriangleBvh::CollectInBox(const float boundsMin[3], const float boundsMax[3], std::vector<uint32_t>& slots) const
	{
		if (m_nodes.empty())
		{
			return;
		}
		TraversalStack stack;
		stack.Push(0u);
		while (!stack.Empty())
		{
			const uint32_t nodeIndex = stack.Pop();
			const Node& node = m_nodes[nodeIndex];
			bool overlaps = true;
			for (int axis = 0; axis < 3; ++axis)
			{
				overlaps = overlaps && node.boundsMin[axis] <= boundsMax[axis] && node.boundsMax[axis] >= boundsMin[axis];
			}
			if (!overlaps)
			{
				continue;
			}
			if (node.count == 0u)
			{
				stack.Push(node.first);
				stack.Push(nodeIndex + 1u);
				continue;
			}
			for (uint32_t slot = node.first; slot < node.first + LeafCount(node); ++slot)
			{
				slots.push_back(slot);
			}
		}
	}

	float SimplifyTriangleBvh::NearestDistanceSquared(const float point[3], float maxDistanceSquared, uint32_t* nearest) const
	{
		const __m128 point4 = _mm_setr_ps(point[0], point[1], point[2], 0.0f);
		float best = maxDistanceSquared;
		if (nearest)
		{
			*nearest = UINT32_MAX;
		}
		if (m_nodes.empty())
		{
			return best;
		}
		alignas(16) float distances[4];
		DistanceStack stack;
		stack.Push(0u, BoxDistanceSquared(point4, m_nodes[0].boundsMin, m_nodes[0].boundsMax));
		while (!stack.Empty())
		{
			--stack.size;
			const uint32_t nodeIndex = stack.nodes[stack.size];
			if (stack.distances[stack.size] > best)
			{
				continue;
			}
			const Node& node = m_nodes[nodeIndex];
			if (node.count > 0u)
			{
				// The leaf's triangles four at a time, then in slot order as one at a time would take them.
				const LeafBlock& block = m_blocks[node.count >> 3u];
				ClosestPointBarycentric4(point, block.ax, block.ay, block.az, block.bx, block.by, block.bz, block.cx, block.cy, block.cz, distances,
					nullptr, nullptr, nullptr);
				const uint32_t count = LeafCount(node);
				for (uint32_t lane = 0; lane < count; ++lane)
				{
					if (distances[lane] < best)
					{
						best = distances[lane];
						if (nearest)
						{
							*nearest = block.source[lane];
						}
					}
				}
				continue;
			}
			const uint32_t left = nodeIndex + 1u, right = node.first;
			const float leftDistance = BoxDistanceSquared(point4, m_nodes[left].boundsMin, m_nodes[left].boundsMax);
			const float rightDistance = BoxDistanceSquared(point4, m_nodes[right].boundsMin, m_nodes[right].boundsMax);
			// Nearer child on top, so it tightens the bound first.
			if (leftDistance <= rightDistance)
			{
				if (rightDistance <= best) stack.Push(right, rightDistance);
				if (leftDistance <= best) stack.Push(left, leftDistance);
			}
			else
			{
				if (leftDistance <= best) stack.Push(left, leftDistance);
				if (rightDistance <= best) stack.Push(right, rightDistance);
			}
		}
		return best;
	}

	bool SimplifyTriangleBvh::MatchFacing(const float point[3], const float faceNormal[3], float maxDistance, float slack, Match& out) const
	{
		const __m128 point4 = _mm_setr_ps(point[0], point[1], point[2], 0.0f);
		if (m_nodes.empty())
		{
			return false;
		}
		if (m_facingGrid != 0u)
		{
			// Facing bins (SetMatchShell) are the gather's: the same result through it.
			thread_local std::vector<NearCandidate> pool;
			pool.clear();
			NearGather gather;
			GatherNear(point, maxDistance, slack, pool, gather);
			return SelectFacing(gather, pool.data(), faceNormal, out);
		}
		// One traversal, nearest child first: the reach shrinks with the nearest distance found so far, and triangles within
		// the current reach are kept as candidates, filtered by the final reach at the end.
		const float maxDistanceSquared = maxDistance * maxDistance;
		float nearest = maxDistanceSquared;
		const float reachScale = m_reachScale;
		const auto reachSquaredFor = [slack, reachScale](float nearestSquared) {
			const float reach = std::sqrt(nearestSquared) * reachScale + slack;
			return reach * reach;
		};
		float reachSquared = reachSquaredFor(nearest);
		// SetMatchShell's limit: the nearest cap within the reach, the traversal pruned at the farthest of them once full.
		const uint32_t cap = m_maxCandidates;
		float selectReachSquared = reachSquared;

		struct Candidate
		{
			float distanceSquared;
			float facing;
			float weights[3];
			uint32_t triangle;
		};
		std::array<Candidate, 32> candidates;
		uint32_t candidateCount = 0u;
		uint64_t leavesTested = 0u, takenCount = 0u;

		alignas(16) float distances[4], u[4], v[4], w[4];
		DistanceStack stack;
		stack.Push(0u, BoxDistanceSquared(point4, m_nodes[0].boundsMin, m_nodes[0].boundsMax));
		while (!stack.Empty())
		{
			--stack.size;
			const uint32_t nodeIndex = stack.nodes[stack.size];
			if (stack.distances[stack.size] > reachSquared)
			{
				continue;
			}
			const Node& node = m_nodes[nodeIndex];
			if (node.count == 0u)
			{
				const uint32_t left = nodeIndex + 1u, right = node.first;
				const float leftDistance = BoxDistanceSquared(point4, m_nodes[left].boundsMin, m_nodes[left].boundsMax);
				const float rightDistance = BoxDistanceSquared(point4, m_nodes[right].boundsMin, m_nodes[right].boundsMax);
				if (leftDistance <= rightDistance)
				{
					if (rightDistance <= reachSquared) stack.Push(right, rightDistance);
					if (leftDistance <= reachSquared) stack.Push(left, leftDistance);
				}
				else
				{
					if (leftDistance <= reachSquared) stack.Push(left, leftDistance);
					if (rightDistance <= reachSquared) stack.Push(right, rightDistance);
				}
				continue;
			}
			const LeafBlock& block = m_blocks[node.count >> 3u];
			++leavesTested;
			ClosestPointBarycentric4(point, block.ax, block.ay, block.az, block.bx, block.by, block.bz, block.cx, block.cy, block.cz, distances, u, v, w);
			const uint32_t count = LeafCount(node);
			for (uint32_t lane = 0; lane < count; ++lane)
			{
				Candidate candidate;
				candidate.distanceSquared = distances[lane];
				if (candidate.distanceSquared > reachSquared)
				{
					continue;
				}
				++takenCount;
				candidate.weights[0] = u[lane];
				candidate.weights[1] = v[lane];
				candidate.weights[2] = w[lane];
				if (candidate.distanceSquared < nearest)
				{
					nearest = candidate.distanceSquared;
					reachSquared = reachSquaredFor(nearest);
					selectReachSquared = reachSquared;
				}
				candidate.facing = block.nx[lane] * faceNormal[0] + block.ny[lane] * faceNormal[1] + block.nz[lane] * faceNormal[2];
				candidate.triangle = block.source[lane];
				if (cap != 0u)
				{
					if (candidateCount == cap)
					{
						uint32_t kept = 0u;
						for (uint32_t i = 0; i < candidateCount; ++i)
						{
							if (candidates[i].distanceSquared <= selectReachSquared) candidates[kept++] = candidates[i];
						}
						candidateCount = kept;
					}
					if (candidateCount < cap)
					{
						candidates[candidateCount++] = candidate;
					}
					else
					{
						uint32_t farthest = 0u;
						for (uint32_t i = 1; i < candidateCount; ++i)
						{
							if (candidates[i].distanceSquared > candidates[farthest].distanceSquared) farthest = i;
						}
						if (candidate.distanceSquared < candidates[farthest].distanceSquared) candidates[farthest] = candidate;
					}
					reachSquared = selectReachSquared;
					if (candidateCount == cap)
					{
						float held = 0.0f;
						for (uint32_t i = 0; i < candidateCount; ++i)
						{
							held = std::max(held, candidates[i].distanceSquared);
						}
						reachSquared = std::min(reachSquared, held);
					}
					continue;
				}
				if (candidateCount == candidates.size())
				{
					uint32_t kept = 0u;
					for (uint32_t i = 0; i < candidateCount; ++i)
					{
						if (candidates[i].distanceSquared <= reachSquared) candidates[kept++] = candidates[i];
					}
					candidateCount = kept;
				}
				if (candidateCount < candidates.size())
				{
					candidates[candidateCount++] = candidate;
				}
				else
				{
					// Still full of equally near triangles: keep the best facing ones.
					uint32_t worst = 0u;
					for (uint32_t i = 1; i < candidateCount; ++i)
					{
						if (candidates[i].facing < candidates[worst].facing) worst = i;
					}
					if (candidate.facing > candidates[worst].facing) candidates[worst] = candidate;
				}
			}
		}

		if (g_queryStatsEnabled)
		{
			RecordQuery(leavesTested, takenCount, takenCount > candidates.size(), nearest <= slack * slack);
		}
		if (nearest >= maxDistanceSquared && maxDistanceSquared > 0.0f)
		{
			return false;
		}
		bool found = false;
		for (uint32_t i = 0; i < candidateCount; ++i)
		{
			const Candidate& candidate = candidates[i];
			if (candidate.distanceSquared > reachSquared)
			{
				continue;
			}
			if (!found || candidate.facing > out.facing || (candidate.facing == out.facing && candidate.distanceSquared < out.distanceSquared))
			{
				found = true;
				out.triangle = candidate.triangle;
				out.barycentric[0] = candidate.weights[0];
				out.barycentric[1] = candidate.weights[1];
				out.barycentric[2] = candidate.weights[2];
				out.distanceSquared = candidate.distanceSquared;
				out.facing = candidate.facing;
			}
		}
		return found;
	}

	void SimplifyTriangleBvh::GatherNear(const float point[3], float maxDistance, float slack, std::vector<NearCandidate>& pool, NearGather& out) const
	{
		const NearRequest request{ maxDistance, slack };
		GatherNearJoint(point, &request, 1u, pool, &out);
	}

	void SimplifyTriangleBvh::GatherNearJoint(const float point[3], const NearRequest* requests, uint32_t requestCount, std::vector<NearCandidate>& pool,
		NearGather* out, float* nearestDistanceSquared) const
	{
		// One instance per combination, so the per-part loops below unroll.
		const bool nearest = nearestDistanceSquared != nullptr;
		switch (std::min(requestCount, kMaxJointRequests) * 2u + (nearest ? 1u : 0u))
		{
		case 0u: return;
		case 1u: return GatherNearJointImpl<0u, true>(point, requests, pool, out, nearestDistanceSquared);
		case 2u: return GatherNearJointImpl<1u, false>(point, requests, pool, out, nearestDistanceSquared);
		case 3u: return GatherNearJointImpl<1u, true>(point, requests, pool, out, nearestDistanceSquared);
		case 4u: return GatherNearJointImpl<2u, false>(point, requests, pool, out, nearestDistanceSquared);
		case 5u: return GatherNearJointImpl<2u, true>(point, requests, pool, out, nearestDistanceSquared);
		case 6u: return GatherNearJointImpl<3u, false>(point, requests, pool, out, nearestDistanceSquared);
		default: return GatherNearJointImpl<3u, true>(point, requests, pool, out, nearestDistanceSquared);
		}
	}

	template <uint32_t RequestCount, bool Nearest>
	void SimplifyTriangleBvh::GatherNearJointImpl(const float point[3], const NearRequest* requests, std::vector<NearCandidate>& pool, NearGather* out,
		float* nearestDistanceSquared) const
	{
		const uint64_t tscStart = g_queryStatsEnabled ? __rdtsc() : 0u;
		uint64_t tscLeaves = 0u;
		const __m128 point4 = _mm_setr_ps(point[0], point[1], point[2], 0.0f);
		// MatchFacing's traversal and candidate buffer without the face, for each request, and NearestDistanceSquared's, side by side
		// (see both). Part i < requestCount is request i; part requestCount is the nearest-distance query.
		constexpr uint32_t kCapacity = 32u; // MatchFacing's candidate buffer
		constexpr uint32_t requestCount = RequestCount;
		static_assert(RequestCount <= kMaxJointRequests);
		// Each part's candidates in the order taken: a lone request writes straight into pool, joint ones into per-thread lists.
		thread_local std::array<std::vector<NearCandidate>, kMaxJointRequests> takenPerThread;
		struct Taken
		{
			std::vector<NearCandidate>* list = nullptr;
			size_t base = 0;
			size_t size() const { return list->size() - base; }
			NearCandidate& operator[](size_t i) { return (*list)[base + i]; }
			void resize(size_t count) { list->resize(base + count); }
			void push_back(const NearCandidate& candidate) { list->push_back(candidate); }
		};
		std::array<Taken, kMaxJointRequests> taken;
		float maxDistanceSquared[kMaxJointRequests], nearest[kMaxJointRequests], reachSquared[kMaxJointRequests];
		float selectReachSquared[kMaxJointRequests]; // with a candidate limit (SetMatchShell): the reach from the nearest alone
		bool overflowed[kMaxJointRequests];
		const float reachScale = m_reachScale;
		const uint32_t cap = m_maxCandidates;
		const auto reachSquaredFor = [reachScale](float nearestSquared, float slack) {
			const float reach = std::sqrt(nearestSquared) * reachScale + slack;
			return reach * reach;
		};
		for (uint32_t r = 0; r < requestCount; ++r)
		{
			out[r] = NearGather{};
			out[r].first = static_cast<uint32_t>(pool.size());
			taken[r].list = requestCount == 1u ? &pool : &takenPerThread[r];
			if (requestCount != 1u)
			{
				taken[r].list->clear();
			}
			taken[r].base = taken[r].list->size();
			overflowed[r] = false;
			maxDistanceSquared[r] = requests[r].maxDistance * requests[r].maxDistance;
			nearest[r] = maxDistanceSquared[r];
			reachSquared[r] = reachSquaredFor(nearest[r], requests[r].slack);
			selectReachSquared[r] = reachSquared[r];
		}
		if (m_nodes.empty())
		{
			return;
		}
		// Facing bins (SetMatchShell): a part whose buffer fills keeps, from then on, the nearest candidate of each facing bin, and
		// skips a node none of whose bins it lacks or holds farther than the node's box.
		const uint32_t facingGrid = m_facingGrid;
		const bool facingBins = facingGrid != 0u;
		const uint32_t binWords = facingGrid * facingGrid / 64u + (facingGrid * facingGrid % 64u != 0u ? 1u : 0u);
		bool binned[kMaxJointRequests] = {};
		BinMask heldBins[kMaxJointRequests] = {};
		std::array<std::array<uint32_t, 256>, kMaxJointRequests> binSlot;
		std::array<std::array<float, 256>, kMaxJointRequests> binDistance;
		const auto binPruned = [&](uint32_t r, uint32_t node, float distance) {
			if (!binned[r])
			{
				return false;
			}
			const BinMask& nodeBins = m_nodeBins[node];
			for (uint32_t word = 0; word < binWords; ++word)
			{
				if ((nodeBins.words[word] & ~heldBins[r].words[word]) != 0u)
				{
					return false;
				}
			}
			for (uint32_t word = 0; word < binWords; ++word)
			{
				for (uint64_t bins = nodeBins.words[word]; bins != 0u; bins &= bins - 1u)
				{
					if (binDistance[r][word * 64u + static_cast<uint32_t>(std::countr_zero(bins))] > distance)
					{
						return false;
					}
				}
			}
			return true;
		};
		const auto binInsert = [&](uint32_t r, const NearCandidate& candidate, uint32_t bin) {
			const uint64_t bit = uint64_t(1) << (bin & 63u);
			uint64_t& heldWord = heldBins[r].words[bin >> 6u];
			if ((heldWord & bit) != 0u)
			{
				NearCandidate& held = taken[r][binSlot[r][bin]];
				if (candidate.distanceSquared < held.distanceSquared)
				{
					held = candidate;
					binDistance[r][bin] = candidate.distanceSquared;
				}
				return;
			}
			binSlot[r][bin] = static_cast<uint32_t>(taken[r].size());
			taken[r].push_back(candidate);
			heldWord |= bit;
			binDistance[r][bin] = candidate.distanceSquared;
		};
		constexpr uint32_t nearestPart = Nearest ? 1u << requestCount : 0u;
		float best = Nearest ? *nearestDistanceSquared : 0.0f;
		// The parts that visit a popped node at this box distance, and that push a child at it (each part's own tests).
		const auto popParts = [&](uint32_t candidates, uint32_t node, float distance) {
			uint32_t mask = 0u;
			for (uint32_t r = 0; r < requestCount; ++r)
			{
				mask |= (candidates >> r & 1u) != 0u && !(distance > reachSquared[r]) && !binPruned(r, node, distance) ? 1u << r : 0u;
			}
			mask |= (candidates & nearestPart) != 0u && !(distance > best) ? nearestPart : 0u;
			return mask;
		};
		const auto pushParts = [&](uint32_t candidates, uint32_t node, float distance) {
			uint32_t mask = 0u;
			for (uint32_t r = 0; r < requestCount; ++r)
			{
				mask |= (candidates >> r & 1u) != 0u && distance <= reachSquared[r] && !binPruned(r, node, distance) ? 1u << r : 0u;
			}
			mask |= (candidates & nearestPart) != 0u && distance <= best ? nearestPart : 0u;
			return mask;
		};

		struct Entry
		{
			uint32_t node;
			uint32_t parts;
			float distance;
		};
		std::array<Entry, 128> stack;
		uint32_t stackSize = 0u;
		uint64_t leavesTested = 0u, innerVisited = 0u;
		constexpr uint32_t allParts = ((1u << requestCount) - 1u) | nearestPart;
		stack[stackSize++] = { 0u, allParts, BoxDistanceSquared(point4, m_nodes[0].boundsMin, m_nodes[0].boundsMax) };
		alignas(16) float distances[4], u[4], v[4], w[4];
		while (stackSize != 0u)
		{
			const Entry entry = stack[--stackSize];
			const uint32_t active = popParts(entry.parts, entry.node, entry.distance);
			if (active == 0u)
			{
				continue;
			}
			const Node& node = m_nodes[entry.node];
			if (node.count == 0u)
			{
				++innerVisited;
				const uint32_t left = entry.node + 1u, right = node.first;
				const float leftDistance = BoxDistanceSquared(point4, m_nodes[left].boundsMin, m_nodes[left].boundsMax);
				const float rightDistance = BoxDistanceSquared(point4, m_nodes[right].boundsMin, m_nodes[right].boundsMax);
				// Nearer child on top. A part's stack holds only what it pushed (the same 128-entry limit applies to each).
				const auto push = [&](uint32_t child, float distance) {
					const uint32_t mask = pushParts(active, child, distance);
					if (mask != 0u && stackSize < stack.size())
					{
						stack[stackSize++] = { child, mask, distance };
					}
				};
				if (leftDistance <= rightDistance)
				{
					push(right, rightDistance);
					push(left, leftDistance);
				}
				else
				{
					push(left, leftDistance);
					push(right, rightDistance);
				}
				continue;
			}
			const LeafBlock& block = m_blocks[node.count >> 3u];
			++leavesTested;
			const bool matching = (active & ~nearestPart) != 0u;
			const uint64_t tscLeaf = g_queryStatsEnabled ? __rdtsc() : 0u;
			ClosestPointBarycentric4(point, block.ax, block.ay, block.az, block.bx, block.by, block.bz, block.cx, block.cy, block.cz, distances,
				matching ? u : nullptr, v, w);
			tscLeaves += g_queryStatsEnabled ? __rdtsc() - tscLeaf : 0u;
			const uint32_t count = LeafCount(node);
			if ((active & nearestPart) != 0u)
			{
				for (uint32_t lane = 0; lane < count; ++lane)
				{
					if (distances[lane] < best)
					{
						best = distances[lane];
					}
				}
			}
			for (uint32_t r = 0; r < requestCount; ++r)
			{
				if ((active >> r & 1u) == 0u)
				{
					continue;
				}
				for (uint32_t lane = 0; lane < count; ++lane)
				{
					if (distances[lane] > reachSquared[r])
					{
						continue;
					}
					if (distances[lane] < nearest[r])
					{
						nearest[r] = distances[lane];
						reachSquared[r] = reachSquaredFor(nearest[r], requests[r].slack);
						selectReachSquared[r] = reachSquared[r];
					}
					NearCandidate candidate;
					candidate.distanceSquared = distances[lane];
					candidate.weights[0] = u[lane];
					candidate.weights[1] = v[lane];
					candidate.weights[2] = w[lane];
					candidate.normal[0] = block.nx[lane];
					candidate.normal[1] = block.ny[lane];
					candidate.normal[2] = block.nz[lane];
					candidate.reachSquared = reachSquared[r];
					candidate.triangle = block.source[lane];
					if (cap != 0u)
					{
						// SetMatchShell's limit: the nearest cap within the reach; once full, nothing past the farthest held is looked at.
						if (taken[r].size() == cap)
						{
							size_t kept = 0u;
							for (size_t i = 0; i < cap; ++i)
							{
								if (taken[r][i].distanceSquared <= selectReachSquared[r]) taken[r][kept++] = taken[r][i];
							}
							taken[r].resize(kept);
						}
						if (taken[r].size() < cap)
						{
							taken[r].push_back(candidate);
						}
						else
						{
							size_t farthest = 0u;
							for (size_t i = 1; i < cap; ++i)
							{
								if (taken[r][i].distanceSquared > taken[r][farthest].distanceSquared) farthest = i;
							}
							if (candidate.distanceSquared < taken[r][farthest].distanceSquared) taken[r][farthest] = candidate;
						}
						reachSquared[r] = selectReachSquared[r];
						if (taken[r].size() == cap)
						{
							float held = 0.0f;
							for (size_t i = 0; i < cap; ++i)
							{
								held = std::max(held, taken[r][i].distanceSquared);
							}
							reachSquared[r] = std::min(reachSquared[r], held);
						}
						continue;
					}
					if (binned[r])
					{
						binInsert(r, candidate, m_blockBins[node.count >> 3u][lane]);
						continue;
					}
					// The buffer's compaction does not depend on the face; only a buffer still full after it does (then every later
					// candidate is kept for SelectFacing's replay).
					if (!overflowed[r] && taken[r].size() == kCapacity)
					{
						size_t kept = 0u;
						for (size_t i = 0; i < kCapacity; ++i)
						{
							if (taken[r][i].distanceSquared <= reachSquared[r]) taken[r][kept++] = taken[r][i];
						}
						taken[r].resize(kept);
						overflowed[r] = kept == kCapacity;
						if (overflowed[r] && facingBins)
						{
							// Facing bins instead of the replay: the held candidates become the nearest of each of their bins.
							std::array<NearCandidate, kCapacity> held;
							for (size_t i = 0; i < kCapacity; ++i)
							{
								held[i] = taken[r][i];
							}
							taken[r].resize(0u);
							overflowed[r] = false;
							binned[r] = true;
							for (const NearCandidate& h : held)
							{
								binInsert(r, h, FacingBin(h.normal[0], h.normal[1], h.normal[2], facingGrid));
							}
							binInsert(r, candidate, m_blockBins[node.count >> 3u][lane]);
							continue;
						}
					}
					taken[r].push_back(candidate);
				}
			}
		}

		if (nearestDistanceSquared != nullptr)
		{
			*nearestDistanceSquared = best;
		}
		for (uint32_t r = 0; r < requestCount; ++r)
		{
			const bool inPool = requestCount == 1u;
			out[r].first = static_cast<uint32_t>(inPool ? taken[r].base : pool.size());
			out[r].reachSquared = reachSquared[r];
			if (nearest[r] >= maxDistanceSquared[r] && maxDistanceSquared[r] > 0.0f)
			{
				if (inPool)
				{
					taken[r].resize(0u);
				}
				continue;
			}
			out[r].none = false;
			if (!overflowed[r])
			{
				// The buffer never stayed full: only those within the final reach can be chosen, in the order held.
				size_t kept = 0u;
				for (size_t i = 0; i < taken[r].size(); ++i)
				{
					if (taken[r][i].distanceSquared <= reachSquared[r])
					{
						if (inPool)
						{
							taken[r][kept++] = taken[r][i];
						}
						else
						{
							pool.push_back(taken[r][i]);
						}
					}
				}
				if (inPool)
				{
					taken[r].resize(kept);
				}
			}
			else
			{
				out[r].overflow = true;
				if (!inPool)
				{
					pool.insert(pool.end(), takenPerThread[r].begin(), takenPerThread[r].end());
				}
			}
			out[r].count = static_cast<uint32_t>(pool.size() - out[r].first);
		}
		if (g_queryStatsEnabled)
		{
			uint64_t takenTotal = 0u;
			bool anyOverflow = false;
			for (uint32_t r = 0; r < requestCount; ++r)
			{
				takenTotal += out[r].count;
				anyOverflow = anyOverflow || out[r].overflow;
			}
			RecordQuery(leavesTested, takenTotal, anyOverflow, requestCount != 0u && nearest[0] <= requests[0].slack * requests[0].slack, innerVisited,
				__rdtsc() - tscStart, tscLeaves);
		}
		// A point with thousands of equally near candidates (overlapping source) must not leave them with this thread.
		for (uint32_t r = 0; r < requestCount && requestCount != 1u; ++r)
		{
			if (takenPerThread[r].capacity() > MatchFacingCache::kPoolCapacity)
			{
				std::vector<NearCandidate>().swap(takenPerThread[r]);
			}
		}
	}

	QueryStatsScope::QueryStatsScope(QuerySite site)
		: m_previous(t_querySite)
	{
		t_querySite = site;
	}

	QueryStatsScope::~QueryStatsScope()
	{
		t_querySite = m_previous;
	}

	bool QueryStatsEnabled()
	{
		return g_queryStatsEnabled;
	}

	void LogQueryStats(const char* label)
	{
		if (!g_queryStatsEnabled)
		{
			return;
		}
		static constexpr const char* kSiteNames[] = { "other", "fit", "channels", "texture" };
		for (size_t site = 0; site < g_queryCounters.size(); ++site)
		{
			const QueryCounters& counters = g_queryCounters[site];
			const uint64_t gathers = counters.gathers.load();
			if (gathers == 0u)
			{
				continue;
			}
			std::string histogram;
			for (uint32_t bucket = 0; bucket < kQueryHistogramBuckets; ++bucket)
			{
				histogram += std::to_string(counters.histogram[bucket].load());
				histogram += bucket + 1u < kQueryHistogramBuckets ? "," : "";
			}
			spdlog::info("ClusterLOD query stats ({}): site={} gathers={} candidates={} per_gather={:.2f} overflows={} leaves={} leaves_per_gather={:.2f} "
				"max_candidates={} on_surface={} on_surface_candidates={} inner_per_gather={:.2f} cycles_per_gather={:.0f} leaf_cycles_per_gather={:.0f} "
				"histogram_log2=[{}]",
				label, kSiteNames[site], gathers, counters.candidates.load(), static_cast<double>(counters.candidates.load()) / static_cast<double>(gathers),
				counters.overflows.load(), counters.leaves.load(), static_cast<double>(counters.leaves.load()) / static_cast<double>(gathers),
				counters.maxCandidates.load(), counters.onSurface.load(), counters.onSurfaceCandidates.load(),
				static_cast<double>(counters.inner.load()) / static_cast<double>(gathers), static_cast<double>(counters.cycles.load()) / static_cast<double>(gathers),
				static_cast<double>(counters.leafCycles.load()) / static_cast<double>(gathers), histogram);
		}
	}

	bool SimplifyTriangleBvh::SelectFacing(const NearGather& gather, const NearCandidate* pool, const float faceNormal[3], Match& out)
	{
		if (gather.none)
		{
			return false;
		}
		const auto facingOf = [&](const NearCandidate& candidate) {
			return candidate.normal[0] * faceNormal[0] + candidate.normal[1] * faceNormal[1] + candidate.normal[2] * faceNormal[2];
		};
		bool found = false;
		const auto consider = [&](const NearCandidate& candidate, float facing) {
			if (!found || facing > out.facing || (facing == out.facing && candidate.distanceSquared < out.distanceSquared))
			{
				found = true;
				out.triangle = candidate.triangle;
				out.barycentric[0] = candidate.weights[0];
				out.barycentric[1] = candidate.weights[1];
				out.barycentric[2] = candidate.weights[2];
				out.distanceSquared = candidate.distanceSquared;
				out.facing = facing;
			}
		};
		if (!gather.overflow)
		{
			for (uint32_t i = 0; i < gather.count; ++i)
			{
				const NearCandidate& candidate = pool[gather.first + i];
				consider(candidate, facingOf(candidate));
			}
			return found;
		}
		// MatchFacing's buffer replayed from where it stayed full (the first kCapacity, in order): compacted by the reach of the moment
		// when full, then the best facing kept.
		// Equivalent shortcuts: a compaction that would keep every held candidate is skipped (farthest, an upper bound of the held
		// distances, within the reach), and the worst facing (the first held at the minimum, as MatchFacing's scan finds it) is
		// found again only after the held set changed.
		alignas(16) std::array<float, 32> heldFacing;
		alignas(16) std::array<float, 32> heldDistance;
		std::array<uint32_t, 32> heldIndex;
		uint32_t heldCount = 0u;
		float farthest = 0.0f; // infinity when a held distance is not a number
		uint32_t worst = 0u;
		bool worstValid = false;
		const auto widen = [](float bound, float d) { return d > bound ? d : (d == d ? bound : std::numeric_limits<float>::infinity()); };
		for (; heldCount < 32u && heldCount < gather.count; ++heldCount)
		{
			const NearCandidate& candidate = pool[gather.first + heldCount];
			heldIndex[heldCount] = heldCount;
			heldFacing[heldCount] = facingOf(candidate);
			heldDistance[heldCount] = candidate.distanceSquared;
			farthest = widen(farthest, candidate.distanceSquared);
		}
		const auto findWorst = [&]() {
			// Full: the minimum four lanes at a time, then its first position (NaN facings take the scalar scan).
			__m128 low = _mm_load_ps(&heldFacing[0]);
			for (uint32_t k = 4u; k < 32u; k += 4u)
			{
				low = _mm_min_ps(low, _mm_load_ps(&heldFacing[k]));
			}
			low = _mm_min_ps(low, _mm_shuffle_ps(low, low, _MM_SHUFFLE(1, 0, 3, 2)));
			low = _mm_min_ps(low, _mm_shuffle_ps(low, low, _MM_SHUFFLE(2, 3, 0, 1)));
			const float minimum = _mm_cvtss_f32(low);
			bool ordered = minimum == minimum;
			for (uint32_t k = 0; k < 32u && ordered; k += 4u)
			{
				ordered = _mm_movemask_ps(_mm_cmpunord_ps(_mm_load_ps(&heldFacing[k]), _mm_load_ps(&heldFacing[k]))) == 0;
			}
			if (ordered)
			{
				for (uint32_t k = 0; k < 32u; k += 4u)
				{
					const int mask = _mm_movemask_ps(_mm_cmpeq_ps(_mm_load_ps(&heldFacing[k]), low));
					if (mask != 0)
					{
						return k + static_cast<uint32_t>(std::countr_zero(static_cast<unsigned>(mask)));
					}
				}
			}
			uint32_t scan = 0u;
			for (uint32_t k = 1; k < 32u; ++k)
			{
				if (heldFacing[k] < heldFacing[scan]) scan = k;
			}
			return scan;
		};
		for (uint32_t i = heldCount; i < gather.count; ++i)
		{
			const NearCandidate& candidate = pool[gather.first + i];
			const float facing = facingOf(candidate);
			if (heldCount == 32u && !(farthest <= candidate.reachSquared))
			{
				uint32_t kept = 0u;
				farthest = 0.0f;
				for (uint32_t k = 0; k < heldCount; ++k)
				{
					if (heldDistance[k] <= candidate.reachSquared)
					{
						heldIndex[kept] = heldIndex[k];
						heldFacing[kept] = heldFacing[k];
						heldDistance[kept] = heldDistance[k];
						farthest = widen(farthest, heldDistance[k]);
						++kept;
					}
				}
				heldCount = kept;
				worstValid = false;
			}
			if (heldCount < 32u)
			{
				heldIndex[heldCount] = i;
				heldFacing[heldCount] = facing;
				heldDistance[heldCount] = candidate.distanceSquared;
				++heldCount;
				farthest = widen(farthest, candidate.distanceSquared);
				worstValid = false;
			}
			else
			{
				if (!worstValid)
				{
					worst = findWorst();
					worstValid = true;
				}
				if (facing > heldFacing[worst])
				{
					heldIndex[worst] = i;
					heldFacing[worst] = facing;
					heldDistance[worst] = candidate.distanceSquared;
					farthest = widen(farthest, candidate.distanceSquared);
					worstValid = false;
				}
			}
		}
		for (uint32_t k = 0; k < heldCount; ++k)
		{
			const NearCandidate& candidate = pool[gather.first + heldIndex[k]];
			if (candidate.distanceSquared > gather.reachSquared)
			{
				continue;
			}
			consider(candidate, heldFacing[k]);
		}
		return found;
	}

	bool SimplifyTriangleBvh::AnyFacingWithin(const float point[3], const float faceNormal[3], float radius, float minFacing) const
	{
		if (m_nodes.empty())
		{
			return false;
		}
		const float radiusSquared = radius * radius;
		TraversalStack stack;
		stack.Push(0u);
		while (!stack.Empty())
		{
			const Node& node = m_nodes[stack.Pop()];
			if (BoxDistanceSquared(point, node.boundsMin, node.boundsMax) > radiusSquared)
			{
				continue;
			}
			if (node.count == 0u)
			{
				stack.Push(node.first);
				stack.Push(static_cast<uint32_t>(&node - m_nodes.data()) + 1u);
				continue;
			}
			for (uint32_t slot = node.first; slot < node.first + LeafCount(node); ++slot)
			{
				const Triangle& t = m_triangles[slot];
				if (!(Dot(t.normal, faceNormal) > minFacing))
				{
					continue;
				}
				float weights[3];
				if (ClosestPointBarycentric(point, t.a, t.b, t.c, weights) <= radiusSquared)
				{
					return true;
				}
			}
		}
		return false;
	}

	SimplifyValidator::SimplifyValidator(const SimplifySurfaceView& original, const SimplifyValidationThresholds& thresholds)
		: m_original(original)
		, m_thresholds(thresholds)
	{
		ZoneScopedN("ClusterLODSimplifyValidation::IndexOriginal");
		m_originalBvh.Build(original);
		m_originalBvh.SetMatchShell(thresholds.matchReachScale, thresholds.matchMaxCandidates, thresholds.matchFacingGrid);
		float boundsMin[3] = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
		float boundsMax[3] = { -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };
		for (size_t i = 0; i < original.indexCount; ++i)
		{
			const float* p = At(original.positions, original.positionStride, original.indices[i]);
			for (int axis = 0; axis < 3; ++axis)
			{
				boundsMin[axis] = std::min(boundsMin[axis], p[axis]);
				boundsMax[axis] = std::max(boundsMax[axis], p[axis]);
			}
		}
		if (original.indexCount > 0u)
		{
			float extent[3];
			Sub(boundsMax, boundsMin, extent);
			m_diagonal = std::sqrt(Dot(extent, extent));
		}
	}

	float SimplifyValidator::RadiusForError(float groupError) const
	{
		const float scaled = std::isfinite(groupError) ? groupError * m_thresholds.radiusErrorScale : m_diagonal;
		return std::min(std::max(scaled, m_diagonal * kRadiusFloorFraction), std::max(m_diagonal, 1.0e-20f));
	}

	void SimplifyValidator::MeasureAgainstOriginal(
		const SimplifySurfaceView& surface, float radius,
		double& area, double& foldArea, double& shadingArea, double& unmatchedArea,
		double foldStop, double shadingStop) const
	{
		ZoneScopedN("ClusterLODSimplifyValidation::MeasureAgainstOriginal");
		area = foldArea = shadingArea = unmatchedArea = 0.0;
		const bool testShading = surface.normals != nullptr && m_original.normals != nullptr;
		const bool mayStop = foldStop >= 0.0 && shadingStop >= 0.0;
		const float slack = m_diagonal * kMatchSlackFraction;
		const uint32_t sampleCount = AreaSampleCount(m_thresholds.samplesPerTriangle);

		// One triangle: its area, and by sample bit the samples with no match, folded and badly shaded.
		struct TriangleSamples
		{
			double area = 0.0;
			uint8_t unmatched = 0u, fold = 0u, shading = 0u;
		};
		const auto measure = [&](size_t first, TriangleSamples& out) {
			out = TriangleSamples{};
			const float* a = At(surface.positions, surface.positionStride, surface.indices[first + 0u]);
			const float* b = At(surface.positions, surface.positionStride, surface.indices[first + 1u]);
			const float* c = At(surface.positions, surface.positionStride, surface.indices[first + 2u]);
			float faceNormal[3];
			out.area = 0.5 * FaceNormal(a, b, c, faceNormal);
			if (!(out.area > 0.0))
			{
				return;
			}
			for (const auto& sample : kSamples)
			{
				const size_t sampleIndex = static_cast<size_t>(&sample - kSamples.data());
				if (!AreaSampleTaken(first / 3u, sampleIndex, sampleCount))
				{
					continue;
				}
				const uint8_t bit = static_cast<uint8_t>(1u << sampleIndex);
				float point[3];
				for (int axis = 0; axis < 3; ++axis)
				{
					point[axis] = a[axis] * sample[0] + b[axis] * sample[1] + c[axis] * sample[2];
				}
				SimplifyTriangleBvh::Match match;
				if (!m_originalBvh.MatchFacing(point, faceNormal, radius, slack, match))
				{
					out.unmatched |= bit;
					continue;
				}
				if (match.facing < m_thresholds.foldCos)
				{
					out.fold |= bit;
				}
				if (!testShading)
				{
					continue;
				}
				float reference[3];
				if (!InterpolatedNormal(m_original, static_cast<size_t>(match.triangle) * 3u, match.barycentric, reference))
				{
					continue;
				}
				float shading[3];
				if (!InterpolatedNormal(surface, first, sample.data(), shading) || Dot(shading, reference) <= m_thresholds.shadingCos)
				{
					out.shading |= bit;
				}
			}
		};
		// The sums in triangle and sample order.
		const auto add = [&](const TriangleSamples& t) {
			if (!(t.area > 0.0))
			{
				return;
			}
			area += t.area;
			const double weight = t.area / static_cast<double>(sampleCount);
			for (size_t sampleIndex = 0; sampleIndex < kSamples.size(); ++sampleIndex)
			{
				const uint8_t bit = static_cast<uint8_t>(1u << sampleIndex);
				if (t.unmatched & bit)
				{
					unmatchedArea += weight;
					continue;
				}
				if (t.fold & bit)
				{
					foldArea += weight;
				}
				if (t.shading & bit)
				{
					shadingArea += weight;
				}
			}
		};
		const auto stopped = [&] { return mayStop && foldArea > foldStop && (!testShading || shadingArea > shadingStop); };

		const size_t triangleCount = surface.indexCount / 3u;
		std::vector<TriangleSamples> slots(std::min(triangleCount, kMeasureChunk));
		for (size_t chunk = 0; chunk < triangleCount; chunk += kMeasureChunk)
		{
			if (stopped())
			{
				return;
			}
			const size_t count = std::min(kMeasureChunk, triangleCount - chunk);
			ForMeasureRanges(count, [&](size_t begin, size_t end) {
				for (size_t i = begin; i < end; ++i)
				{
					measure((chunk + i) * 3u, slots[i]);
				}
			});
			for (size_t i = 0; i < count; ++i)
			{
				if (stopped())
				{
					return;
				}
				add(slots[i]);
			}
		}
	}

	void SimplifyValidator::MeasureCoverage(
		const SimplifySurfaceView& covered, const SimplifyTriangleBvh& coverBvh, float radius,
		double& area, double& uncoveredArea) const
	{
		ZoneScopedN("ClusterLODSimplifyValidation::MeasureCoverage");
		area = uncoveredArea = 0.0;
		const uint32_t sampleCount = AreaSampleCount(m_thresholds.samplesPerTriangle);
		// One triangle: its area, and by sample bit the samples nothing covers.
		struct TriangleCoverage
		{
			double area = 0.0;
			uint8_t uncovered = 0u;
		};
		const size_t triangleCount = covered.indexCount / 3u;
		std::vector<TriangleCoverage> slots(triangleCount);
		ForMeasureRanges(triangleCount, [&](size_t begin, size_t end) {
			for (size_t triangle = begin; triangle < end; ++triangle)
			{
				const size_t first = triangle * 3u;
				TriangleCoverage& out = slots[triangle];
				const float* a = At(covered.positions, covered.positionStride, covered.indices[first + 0u]);
				const float* b = At(covered.positions, covered.positionStride, covered.indices[first + 1u]);
				const float* c = At(covered.positions, covered.positionStride, covered.indices[first + 2u]);
				float faceNormal[3];
				out.area = 0.5 * FaceNormal(a, b, c, faceNormal);
				if (!(out.area > 0.0))
				{
					continue;
				}
				for (const auto& sample : kSamples)
				{
					const size_t sampleIndex = static_cast<size_t>(&sample - kSamples.data());
					if (!AreaSampleTaken(first / 3u, sampleIndex, sampleCount))
					{
						continue;
					}
					float point[3];
					for (int axis = 0; axis < 3; ++axis)
					{
						point[axis] = a[axis] * sample[0] + b[axis] * sample[1] + c[axis] * sample[2];
					}
					if (!coverBvh.AnyFacingWithin(point, faceNormal, radius, kCoverageMinFacing))
					{
						out.uncovered |= static_cast<uint8_t>(1u << sampleIndex);
					}
				}
			}
		});
		for (const TriangleCoverage& t : slots)
		{
			if (!(t.area > 0.0))
			{
				continue;
			}
			area += t.area;
			const double weight = t.area / static_cast<double>(sampleCount);
			for (size_t sampleIndex = 0; sampleIndex < kSamples.size(); ++sampleIndex)
			{
				if (t.uncovered & (1u << sampleIndex))
				{
					uncoveredArea += weight;
				}
			}
		}
	}

	void SimplifyValidator::SetLevel(const SimplifySurfaceView& level, std::vector<uint32_t> triangleGroups)
	{
		ZoneScopedN("ClusterLODSimplifyValidation::SetLevel");
		m_levelBvh.Build(level);
		m_levelGroups = std::move(triangleGroups);
	}

	void SimplifyValidator::MeasureSourceCoverage(
		const SimplifySurfaceView& input, const SimplifyTriangleBvh& coverBvh, uint32_t group, float radius,
		double& area, double& uncoveredArea) const
	{
		ZoneScopedN("ClusterLODSimplifyValidation::MeasureSourceCoverage");
		area = uncoveredArea = 0.0;
		if (m_levelBvh.Empty() || group == UINT32_MAX)
		{
			return;
		}

		// The original the group owns lies within its input's bounds grown by how far the input may have drifted, which its
		// own error (and so the radius) bounds.
		float boundsMin[3] = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
		float boundsMax[3] = { -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };
		for (size_t i = 0; i < input.indexCount; ++i)
		{
			const float* p = At(input.positions, input.positionStride, input.indices[i]);
			for (int axis = 0; axis < 3; ++axis)
			{
				boundsMin[axis] = std::min(boundsMin[axis], p[axis] - radius);
				boundsMax[axis] = std::max(boundsMax[axis], p[axis] + radius);
			}
		}
		thread_local std::vector<uint32_t> slots;
		slots.clear();
		m_originalBvh.CollectInBox(boundsMin, boundsMax, slots);
		// Large regions are sampled by stride; each sample stands for stride triangles of its own area.
		const size_t stride = std::max<size_t>(1u, (slots.size() + kMaxSourceSamples - 1u) / kMaxSourceSamples);
		// One sample: its area if the group owns it (0 if not), and whether the candidate covers it.
		struct SourceSample
		{
			double area = 0.0;
			bool uncovered = false;
		};
		// The tasks read this thread's slots: a thread_local named inside them would be the worker's own.
		const std::vector<uint32_t>& sourceSlots = slots;
		const size_t sampleTotal = (slots.size() + stride - 1u) / stride;
		std::vector<SourceSample> samples(sampleTotal);
		ForMeasureRanges(sampleTotal, [&](size_t begin, size_t end) {
			for (size_t sample = begin; sample < end; ++sample)
			{
				SourceSample& out = samples[sample];
				const SimplifyTriangleBvh::Triangle& t = m_originalBvh.TriangleAt(sourceSlots[sample * stride]);
				float faceNormal[3];
				const double triangleArea = 0.5 * FaceNormal(t.a, t.b, t.c, faceNormal) * static_cast<double>(stride);
				if (!(triangleArea > 0.0))
				{
					continue;
				}
				float centre[3];
				for (int axis = 0; axis < 3; ++axis)
				{
					centre[axis] = (t.a[axis] + t.b[axis] + t.c[axis]) * (1.0f / 3.0f);
				}
				uint32_t owner = UINT32_MAX;
				m_levelBvh.NearestDistanceSquared(centre, std::numeric_limits<float>::max(), &owner);
				if (owner == UINT32_MAX || owner >= m_levelGroups.size() || m_levelGroups[owner] != group)
				{
					continue;
				}
				out.area = triangleArea;
				out.uncovered = !coverBvh.AnyFacingWithin(centre, faceNormal, radius, kCoverageMinFacing);
			}
		});
		for (const SourceSample& sample : samples)
		{
			if (!(sample.area > 0.0))
			{
				continue;
			}
			area += sample.area;
			if (sample.uncovered)
			{
				uncoveredArea += sample.area;
			}
		}
	}

	bool SimplifyValidator::OriginalNormalAt(const float point[3], const float faceNormal[3], float normal[3], float& facing) const
	{
		if (m_original.normals == nullptr)
		{
			return false;
		}
		SimplifyTriangleBvh::Match match;
		if (!m_originalBvh.MatchFacing(point, faceNormal, std::max(m_diagonal, 1.0e-20f) * 2.0f, m_diagonal * kMatchSlackFraction, match))
		{
			return false;
		}
		facing = match.facing;
		return InterpolatedNormal(m_original, static_cast<size_t>(match.triangle) * 3u, match.barycentric, normal);
	}

	bool SimplifyValidator::OriginalNormalAt(const SimplifyTriangleBvh::NearGather& gather, const SimplifyTriangleBvh::NearCandidate* pool,
		const float faceNormal[3], float normal[3], float& facing) const
	{
		SimplifyTriangleBvh::Match match;
		if (m_original.normals == nullptr || !SimplifyTriangleBvh::SelectFacing(gather, pool, faceNormal, match))
		{
			return false;
		}
		facing = match.facing;
		return InterpolatedNormal(m_original, static_cast<size_t>(match.triangle) * 3u, match.barycentric, normal);
	}

	void SimplifyValidator::ContinueOriginalMatchCache(MatchFacingCache& cache, size_t expectedPoints) const
	{
		cache.Continue(&m_originalBvh, std::max(m_diagonal, 1.0e-20f) * 2.0f, m_diagonal * kMatchSlackFraction, expectedPoints);
	}

	void SimplifyValidator::ResetOriginalMatchCache(MatchFacingCache& cache, size_t expectedPoints) const
	{
		cache.Reset(&m_originalBvh, std::max(m_diagonal, 1.0e-20f) * 2.0f, m_diagonal * kMatchSlackFraction, expectedPoints);
	}

	bool SimplifyValidator::OriginalNormalAt(const float point[3], const float faceNormal[3], float normal[3], float& facing, MatchFacingCache& cache) const
	{
		if (m_original.normals == nullptr)
		{
			return false;
		}
		SimplifyTriangleBvh::Match match;
		const bool matched = cache.Covers(&m_originalBvh, std::max(m_diagonal, 1.0e-20f) * 2.0f, m_diagonal * kMatchSlackFraction)
			? cache.Match(point, faceNormal, match)
			: m_originalBvh.MatchFacing(point, faceNormal, std::max(m_diagonal, 1.0e-20f) * 2.0f, m_diagonal * kMatchSlackFraction, match);
		if (!matched)
		{
			return false;
		}
		facing = match.facing;
		return InterpolatedNormal(m_original, static_cast<size_t>(match.triangle) * 3u, match.barycentric, normal);
	}

	void MatchFacingCache::Continue(const SimplifyTriangleBvh* bvh, float maxDistance, float slack, size_t expectedPoints)
	{
		if (!Covers(bvh, maxDistance, slack) || m_table.empty())
		{
			Reset(bvh, maxDistance, slack, expectedPoints);
			return;
		}
		if (m_used + expectedPoints > kMaxPoints)
		{
			Forget();
		}
		while ((m_used + expectedPoints) * 2u > m_table.size() && m_table.size() < kMaxPoints * 4u)
		{
			Grow();
		}
	}

	void MatchFacingCache::Reset(const SimplifyTriangleBvh* bvh, float maxDistance, float slack, size_t expectedPoints)
	{
		m_bvh = bvh;
		m_buildId = bvh != nullptr ? bvh->BuildId() : 0u;
		m_maxDistance = maxDistance;
		m_slack = slack;
		m_pool.clear();
		// One pathological traversal (thousands of overlapping triangles within reach) must not stay with this thread.
		if (m_pool.capacity() > kPoolCapacity * 2u)
		{
			std::vector<SimplifyTriangleBvh::NearCandidate>().swap(m_pool);
		}
		m_used = 0;
		size_t size = 64u;
		while (size < expectedPoints * 2u)
		{
			size *= 2u;
		}
		if (m_table.size() < size)
		{
			m_table.assign(size, Entry{});
			m_generation = 0;
		}
		if (++m_generation == 0u)
		{
			for (Entry& entry : m_table)
			{
				entry.generation = 0u;
			}
			m_generation = 1u;
		}
	}

	void MatchFacingCache::Forget()
	{
		m_pool.clear();
		m_used = 0;
		if (++m_generation == 0u)
		{
			for (Entry& entry : m_table)
			{
				entry.generation = 0u;
			}
			m_generation = 1u;
		}
	}

	void MatchFacingCache::Grow()
	{
		std::vector<Entry> old(m_table.size() * 2u, Entry{});
		old.swap(m_table);
		const size_t mask = m_table.size() - 1u;
		for (const Entry& entry : old)
		{
			if (entry.generation != m_generation)
			{
				continue;
			}
			size_t slot = (entry.key[0] * 0x9E3779B1u ^ entry.key[1] * 0x85EBCA77u ^ entry.key[2] * 0xC2B2AE3Du) & mask;
			while (m_table[slot].generation == m_generation)
			{
				slot = (slot + 1u) & mask;
			}
			m_table[slot] = entry;
		}
	}

	bool MatchFacingCache::Match(const float point[3], const float faceNormal[3], SimplifyTriangleBvh::Match& out)
	{
		uint32_t key[3];
		std::memcpy(key, point, sizeof(key));
		if (m_pool.size() > kPoolCapacity)
		{
			// Memory stays bounded however many candidates the points gather: forget them all (results are exact either way).
			Forget();
		}
		if ((m_used + 1u) * 2u > m_table.size())
		{
			Grow();
		}
		const size_t mask = m_table.size() - 1u;
		size_t slot = (key[0] * 0x9E3779B1u ^ key[1] * 0x85EBCA77u ^ key[2] * 0xC2B2AE3Du) & mask;
		while (m_table[slot].generation == m_generation &&
			(m_table[slot].key[0] != key[0] || m_table[slot].key[1] != key[1] || m_table[slot].key[2] != key[2]))
		{
			slot = (slot + 1u) & mask;
		}
		Entry& entry = m_table[slot];
		if (entry.generation != m_generation)
		{
			entry.generation = m_generation;
			std::memcpy(entry.key, key, sizeof(key));
			m_bvh->GatherNear(point, m_maxDistance, m_slack, m_pool, entry.gather);
			++m_used;
		}
		if (entry.gather.none)
		{
			return false;
		}
		return SimplifyTriangleBvh::SelectFacing(entry.gather, m_pool.data(), faceNormal, out);
	}

	bool SimplifyValidator::OriginalFaceNormalAt(const float point[3], const float faceNormal[3], float normal[3]) const
	{
		SimplifyTriangleBvh::Match match;
		if (!m_originalBvh.MatchFacing(point, faceNormal, std::max(m_diagonal, 1.0e-20f) * 2.0f, m_diagonal * kMatchSlackFraction, match))
		{
			return false;
		}
		const size_t first = static_cast<size_t>(match.triangle) * 3u;
		const float* a = At(m_original.positions, m_original.positionStride, m_original.indices[first + 0u]);
		const float* b = At(m_original.positions, m_original.positionStride, m_original.indices[first + 1u]);
		const float* c = At(m_original.positions, m_original.positionStride, m_original.indices[first + 2u]);
		return FaceNormal(a, b, c, normal) > 0.0f;
	}

	float SimplifyValidator::MeasureDeviation(const SimplifySurfaceView& input, const SimplifySurfaceView& output) const
	{
		ZoneScopedN("ClusterLODSimplifyValidation::MeasureDeviation");
		const auto start = std::chrono::steady_clock::now();
		const float unbounded = std::max(m_diagonal, 1.0e-20f) * 4.0f;
		const float unboundedSquared = unbounded * unbounded;

		// Each triangle's four area samples and its corners.
		const auto forSamples = [](const SimplifySurfaceView& surface, auto&& visit) {
			for (size_t first = 0; first + 2u < surface.indexCount; first += 3u)
			{
				const float* corners[3] = {
					At(surface.positions, surface.positionStride, surface.indices[first + 0u]),
					At(surface.positions, surface.positionStride, surface.indices[first + 1u]),
					At(surface.positions, surface.positionStride, surface.indices[first + 2u]) };
				for (const float* corner : corners)
				{
					visit(corner);
				}
				for (const auto& sample : kSamples)
				{
					float point[3];
					for (int axis = 0; axis < 3; ++axis)
					{
						point[axis] = corners[0][axis] * sample[0] + corners[1][axis] * sample[1] + corners[2][axis] * sample[2];
					}
					visit(point);
				}
			}
		};

		float worstSquared = 0.0f;
		forSamples(output, [&](const float* point) {
			worstSquared = std::max(worstSquared, m_originalBvh.NearestDistanceSquared(point, unboundedSquared));
		});
		SimplifyTriangleBvh outputBvh;
		outputBvh.Build(output);
		if (!outputBvh.Empty())
		{
			forSamples(input, [&](const float* point) {
				worstSquared = std::max(worstSquared, outputBvh.NearestDistanceSquared(point, unboundedSquared));
			});
		}
		m_deviationNs.fetch_add(ElapsedNs(start), std::memory_order_relaxed);
		return std::sqrt(worstSquared);
	}

	void SimplifyValidator::MeasureBaseline(const SimplifySurfaceView& surface, SimplifyInputBaseline& baseline) const
	{
		// Unbounded radius: the input already passed its own validation, and the baseline must not depend on the candidate.
		const auto start = std::chrono::steady_clock::now();
		double unmatched = 0.0;
		MeasureAgainstOriginal(surface, std::max(m_diagonal, 1.0e-20f), baseline.area, baseline.foldArea, baseline.shadingArea, unmatched);
		baseline.measured = true;
		m_baselineNs.fetch_add(ElapsedNs(start), std::memory_order_relaxed);
	}

	uint32_t SimplifyValidator::Validate(
		const SimplifySurfaceView& input, const SimplifySurfaceView& output, float groupError,
		SimplifyInputBaseline* baseline, SimplifyValidationMeasurement* measurement, uint32_t group) const
	{
		ZoneScopedN("ClusterLODSimplifyValidation::Validate");
		SimplifyValidationMeasurement local;
		SimplifyValidationMeasurement& m = measurement ? *measurement : local;
		m = SimplifyValidationMeasurement{};
		m.radius = RadiusForError(groupError);
		SimplifyInputBaseline localBaseline;
		SimplifyInputBaseline& base = baseline ? *baseline : localBaseline;

		double outputArea = 0.0;
		for (size_t first = 0; first + 2u < output.indexCount; first += 3u)
		{
			float faceNormal[3];
			outputArea += 0.5 * FaceNormal(
				At(output.positions, output.positionStride, output.indices[first + 0u]),
				At(output.positions, output.positionStride, output.indices[first + 1u]),
				At(output.positions, output.positionStride, output.indices[first + 2u]), faceNormal);
		}

		// Allowed fold/shading area: the allowance plus the input's own share, when that is known.
		const auto limits = [&](double& foldLimit, double& shadingLimit) {
			const double inputFold = base.measured && base.area > 0.0 ? base.foldArea / base.area : 0.0;
			const double inputShading = base.measured && base.area > 0.0 ? base.shadingArea / base.area : 0.0;
			foldLimit = (m_thresholds.foldAreaFraction + inputFold) * outputArea;
			shadingLimit = (m_thresholds.shadingAreaFraction + inputShading) * outputArea;
		};
		double foldLimit = 0.0, shadingLimit = 0.0;
		limits(foldLimit, shadingLimit);

		auto start = std::chrono::steady_clock::now();
		double sampledArea = 0.0;
		MeasureAgainstOriginal(
			output, m.radius, sampledArea, m.foldArea, m.shadingArea, m.unmatchedArea,
			base.measured ? foldLimit : -1.0, base.measured ? shadingLimit : -1.0);
		m.outputArea = outputArea;
		m.stoppedEarly = sampledArea < outputArea * (1.0 - 1.0e-9);
		m_candidateNs.fetch_add(ElapsedNs(start), std::memory_order_relaxed);
		start = std::chrono::steady_clock::now();
		SimplifyTriangleBvh outputBvh;
		outputBvh.Build(output);
		MeasureCoverage(input, outputBvh, m.radius, m.inputArea, m.uncoveredArea);
		m_coverageNs.fetch_add(ElapsedNs(start), std::memory_order_relaxed);
		start = std::chrono::steady_clock::now();
		MeasureSourceCoverage(input, outputBvh, group, m.radius, m.sourceArea, m.sourceUncoveredArea);
		m_sourceNs.fetch_add(ElapsedNs(start), std::memory_order_relaxed);

		if (!(outputArea > 0.0))
		{
			// Nothing but degenerate triangles left: whatever the input covered is gone.
			return m.inputArea > 0.0 ? static_cast<uint32_t>(SimplifyValidationReject_Coverage) : 0u;
		}

		uint32_t reasons = 0u;
		if (m.unmatchedArea > outputArea * m_thresholds.coverageAreaFraction)
		{
			reasons |= SimplifyValidationReject_Spurious;
		}
		if (m.inputArea > 0.0 && m.uncoveredArea > m.inputArea * m_thresholds.coverageAreaFraction)
		{
			reasons |= SimplifyValidationReject_Coverage;
		}
		if (m.sourceArea > 0.0 && m.sourceUncoveredArea > m.sourceArea * m_thresholds.coverageAreaFraction)
		{
			reasons |= SimplifyValidationReject_SourceCoverage;
		}

		if (!base.measured && (m.foldArea > foldLimit || m.shadingArea > shadingLimit))
		{
			// Only failures the input did not already have are the candidate's: measure the input's own share against the
			// original the same way.
			MeasureBaseline(input, base);
			limits(foldLimit, shadingLimit);
		}
		if (base.measured)
		{
			m.inputMeasured = true;
			m.inputFoldArea = base.foldArea;
			m.inputShadingArea = base.shadingArea;
		}
		if (m.foldArea > foldLimit)
		{
			reasons |= SimplifyValidationReject_Fold;
		}
		if (m.shadingArea > shadingLimit)
		{
			reasons |= SimplifyValidationReject_Shading;
		}
		return reasons;
	}

}
