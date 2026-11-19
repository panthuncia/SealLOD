#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODReplacementVertices.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

#include <meshoptimizer.h>
#include <oneapi/tbb/task_arena.h>
#include <ThirdParty/meshoptimizer/clusterlod.h>
#include <tracy/Tracy.hpp>

#include <BasicRenderer/Streaming/TaskScheduler.h>

namespace clod_detail {

	namespace {

		// Fit samples on each triangle incident to a vertex, as barycentric weights with the vertex's first: the vertex itself,
		// the points towards it from the centre and from each edge midpoint, and the centre. Every sample the vertex's
		// interpolation weight reaches; the vertex's own weight is zero elsewhere, so nothing else depends on its normal.
		constexpr std::array<std::array<float, 3>, 5> kFitSamples{ {
			{ 1.0f, 0.0f, 0.0f },
			{ 4.0f / 6.0f, 1.0f / 6.0f, 1.0f / 6.0f },
			{ 0.5f, 0.5f, 0.0f },
			{ 0.5f, 0.0f, 0.5f },
			{ 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f },
		} };

		inline float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

		// Unit face normal of abc, returning twice its area (zero normal for degenerate triangles).
		inline float FaceNormal(const float* a, const float* b, const float* c, float* normal)
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
			return length;
		}

		// Removes the normal's component from a tangent and renormalizes it; a tangent (nearly) along the normal is kept.
		inline void Orthogonalize(float* tangent, const float* normal)
		{
			const float d = Dot3(tangent, normal);
			const float t[3] = { tangent[0] - normal[0] * d, tangent[1] - normal[1] * d, tangent[2] - normal[2] * d };
			const float length = std::sqrt(Dot3(t, t));
			if (length > 1.0e-6f)
			{
				tangent[0] = t[0] / length;
				tangent[1] = t[1] / length;
				tangent[2] = t[2] / length;
			}
		}

		constexpr uint32_t kMaxUvStreams = 16u;

		// Vertices per task when one candidate's fit runs in parallel (vertices are fitted independently).
		constexpr uint32_t kFitChunkVertices = 256u;

	}

	ReplacementVertexFitter::ReplacementVertexFitter(const SimplifyValidator& original, const ReplacementVertexLayout& layout, ReplacementVertexStore& store)
		: m_original(original)
		, m_layout(layout)
		, m_store(store)
	{
		const uint32_t streams = (m_layout.texcoordSolved ? 1u : 0u) + m_layout.uvSetsSolved;
		assert(streams <= kMaxUvStreams);
		assert(streams == 0u || m_layout.uvAttribute + streams * 2u <= m_layout.attributeStride);
		(void)streams;
		m_fanKeys.assign(m_layout.vertexStride > 0u ? m_store.vertices.size() / m_layout.vertexStride : 0u, FanKey{});
		const char* memo = std::getenv("BASICRENDERER_CLOD_FIT_FAN_MEMO");
		m_fanMemo = memo == nullptr || memo[0] == 0 ? 2u : static_cast<uint32_t>(std::atoi(memo));
	}

	SimplifySurfaceView ReplacementVertexFitter::View(const uint32_t* indices, size_t indexCount) const
	{
		SimplifySurfaceView view;
		view.positions = reinterpret_cast<const float*>(m_store.vertices.data());
		view.positionStride = m_layout.vertexStride;
		view.normals = reinterpret_cast<const float*>(m_store.vertices.data() + m_layout.normalOffset);
		view.normalStride = m_layout.vertexStride;
		view.indices = indices;
		view.indexCount = indexCount;
		return view;
	}

	void ReplacementVertexFitter::Fit(const clodMesh& mesh, clodReplacementCandidate& c) const
	{
		ZoneScopedN("ClusterLODReplacementVertices::Fit");
		const size_t vertexCount = c.vertex_count;
		const size_t stride = m_layout.vertexStride;
		const size_t attributeStride = m_layout.attributeStride;
		const size_t recordSize = RecordSize();
		const std::byte* records = m_store.vertices.data();
		const uint32_t uvStreams = std::min((m_layout.texcoordSolved ? 1u : 0u) + m_layout.uvSetsSolved, kMaxUvStreams);
		const bool solvedAttributes = c.attributes != nullptr && mesh.vertex_attributes != nullptr && attributeStride > 0u;

		// The triangle corners at each private vertex. Not thread_local: the vertex tasks below read them while this thread waits.
		std::vector<uint32_t> offsets;
		std::vector<uint32_t> cursor;
		std::vector<uint32_t> corners;
		offsets.assign(vertexCount + 1u, 0u);
		for (size_t i = 0; i < c.index_count; ++i)
		{
			offsets[c.indices[i] + 1u]++;
		}
		for (size_t k = 0; k < vertexCount; ++k)
		{
			offsets[k + 1u] += offsets[k];
		}
		cursor.assign(offsets.begin(), offsets.end() - 1);
		corners.resize(c.index_count);
		for (size_t i = 0; i < c.index_count; ++i)
		{
			corners[cursor[c.indices[i]]++] = static_cast<uint32_t>(i);
		}

		const auto position = [&](uint32_t k) { return &c.positions[static_cast<size_t>(k) * 3u]; };
		// A UV stream's stored value (before this candidate): the original's, or an earlier replacement's already bounded one.
		const auto storedUv = [&](uint32_t k, uint32_t stream) {
			return &mesh.vertex_attributes[static_cast<size_t>(c.vertex_ids[k]) * attributeStride + m_layout.uvAttribute + stream * 2u];
		};

		// A private vertex's shading normal before this fit (its record's).
		const auto currentNormal = [&](uint32_t k, float* out) {
			std::memcpy(out, records + static_cast<size_t>(c.vertex_ids[k]) * stride + m_layout.normalOffset, sizeof(float) * 3u);
		};
		struct FitSample
		{
			float source[3]; // the original's unit normal at the sample
			float rest[3];   // the other two corners' share of the interpolated normal
			float lambda;    // the vertex's share
			float weight;    // area times facing
		};

		// Each vertex is fitted from the store and the candidate alone and writes only its own outputs, so ranges of vertices run
		// as independent tasks (the counters are sums) and the result does not depend on how they are split.
		const auto fitRange = [&](uint32_t firstVertex, uint32_t endVertex) {
			const auto rangeStart = std::chrono::steady_clock::now();
			const QueryStatsScope queryScope(QuerySite::Fit);
			// A range runs without waiting on other tasks, so these per-thread buffers are its own while it runs.
			thread_local std::vector<FitSample> samples;
			// Neighbouring vertices sample the same points (a shared corner, an edge midpoint, a centre), and a vertex's match for
			// its UVs is its first sample's point: one traversal per point (MatchFacingCache, exact).
			thread_local MatchFacingCache matches;
			m_original.ContinueOriginalMatchCache(matches, static_cast<size_t>(offsets[endVertex] - offsets[firstVertex]) * 4u);

			uint64_t fitted = 0, fallbacks = 0, refit = 0, clamped = 0, nonFinite = 0, transferred = 0, transferOutside = 0, fanChecks = 0, fanHits = 0,
				checkSkipped = 0;
			std::vector<float> attributeRow(attributeStride);
			for (uint32_t k = firstVertex; k < endVertex; ++k)
			{
				if (offsets[k] == offsets[k + 1u])
				{
					continue;
				}
				const std::byte* source = records + static_cast<size_t>(c.vertex_ids[k]) * stride;
				float current[3];
				std::memcpy(current, source + m_layout.normalOffset, sizeof(current));
				std::memcpy(&c.normals[static_cast<size_t>(k) * 3u], current, sizeof(current));
				if ((c.locks[k] & meshopt_SimplifyVertex_Lock) != 0)
				{
					continue;
				}
				++fitted;

				// An unmoved vertex whose fit inputs (its triangles in this order, their corners' ids and positions; the ids name the
				// records the fit reads) are those of an earlier fit that left it unchanged is unchanged again: the fit is a function of
				// them. Keyed by two independent 64-bit hashes of them (a false match needs both to collide for the same vertex).
				FanKey fanKey{};
				const uint32_t vertexId = c.vertex_ids[k];
				if (!c.moved[k] && m_fanMemo != 0u && vertexId < m_fanKeys.size())
				{
					uint64_t h = 0x9E3779B97F4A7C15ull ^ vertexId, g = 0xD1B54A32D192ED03ull ^ (static_cast<uint64_t>(vertexId) << 17);
					const auto mix = [&h, &g](uint64_t value) {
						h ^= value + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
						h *= 0xBF58476D1CE4E5B9ull;
						h ^= h >> 31;
						g = (g ^ value) * 0x94D049BB133111EBull;
						g ^= g >> 29;
						g = g * 0xFF51AFD7ED558CCDull + 0x2545F4914F6CDD1Dull;
						g ^= g >> 32;
					};
					mix(offsets[k + 1u] - offsets[k]);
					for (uint32_t corner = offsets[k]; corner < offsets[k + 1u]; ++corner)
					{
						const uint32_t first = corners[corner] / 3u * 3u;
						const uint32_t j = corners[corner] % 3u;
						for (uint32_t step = 0; step < 3u; ++step)
						{
							const uint32_t local = c.indices[first + (j + step) % 3u];
							mix(c.vertex_ids[local]);
							const float* p = position(local);
							uint32_t bits[3];
							std::memcpy(bits, p, sizeof(bits));
							mix((static_cast<uint64_t>(bits[0]) << 32) | bits[1]);
							mix(bits[2]);
						}
					}
					fanKey = { h | 1u, g }; // first never 0, the empty slot
					++fanChecks;
					const FanKey stored = { std::atomic_ref<uint64_t>(m_fanKeys[vertexId].first).load(std::memory_order_relaxed),
						std::atomic_ref<uint64_t>(m_fanKeys[vertexId].second).load(std::memory_order_relaxed) };
					if (stored.first == fanKey.first && stored.second == fanKey.second)
					{
						++fanHits;
						if (m_fanMemo >= 2u)
						{
							continue;
						}
					}
				}

				// Build cost (refitCheckCos): an unmoved vertex whose current shading already meets the original within the check angle
				// at the centre of each of its triangles is taken as unchanged without the full fit (its other sample points). A centre
				// is one traversal the triangle's three corners share.
				if (!c.moved[k] && m_layout.refitCheckCos > -1.0f)
				{
					bool within = offsets[k] != offsets[k + 1u];
					for (uint32_t corner = offsets[k]; corner < offsets[k + 1u] && within; ++corner)
					{
						const uint32_t first = corners[corner] / 3u * 3u;
						const float* p[3] = { position(c.indices[first]), position(c.indices[first + 1u]), position(c.indices[first + 2u]) };
						float face[3];
						if (!(FaceNormal(p[0], p[1], p[2], face) > 0.0f))
						{
							continue;
						}
						float centre[3], shading[3] = {};
						for (int axis = 0; axis < 3; ++axis)
						{
							centre[axis] = (p[0][axis] + p[1][axis] + p[2][axis]) * (1.0f / 3.0f);
						}
						for (uint32_t m = 0; m < 3u; ++m)
						{
							float n[3];
							currentNormal(c.indices[first + m], n);
							for (int axis = 0; axis < 3; ++axis)
							{
								shading[axis] += n[axis];
							}
						}
						const float length = std::sqrt(Dot3(shading, shading));
						float source[3];
						float facing = 0.0f;
						if (!m_original.OriginalNormalAt(centre, face, source, facing, matches) || !(facing > 0.0f))
						{
							continue;
						}
						within = length > 1.0e-3f && Dot3(shading, source) >= m_layout.refitCheckCos * length;
					}
					if (within)
					{
						++checkSkipped;
						continue;
					}
				}

				// The original's normals around the vertex, sampled over its triangles in the candidate. Each sample keeps the
				// vertex's interpolation weight there and what its two neighbours' current normals contribute, so a candidate
				// normal for the vertex can be scored by the shading it would produce.
				samples.clear();
				double faces[3] = {};
				for (uint32_t corner = offsets[k]; corner < offsets[k + 1u]; ++corner)
				{
					const uint32_t first = corners[corner] / 3u * 3u;
					const uint32_t j = corners[corner] % 3u;
					const uint32_t v1 = c.indices[first + (j + 1u) % 3u];
					const uint32_t v2 = c.indices[first + (j + 2u) % 3u];
					const float* p0 = position(c.indices[first + j]);
					const float* p1 = position(v1);
					const float* p2 = position(v2);
					float face[3];
					const float area = FaceNormal(p0, p1, p2, face);
					if (!(area > 0.0f))
					{
						continue;
					}
					for (int axis = 0; axis < 3; ++axis)
					{
						faces[axis] += static_cast<double>(face[axis]) * area;
					}
					float n1[3], n2[3];
					currentNormal(v1, n1);
					currentNormal(v2, n2);
					for (const auto& sample : kFitSamples)
					{
						if ((m_layout.fitSamples >> (&sample - kFitSamples.data()) & 1u) == 0u)
						{
							continue;
						}
						float point[3];
						for (int axis = 0; axis < 3; ++axis)
						{
							point[axis] = p0[axis] * sample[0] + p1[axis] * sample[1] + p2[axis] * sample[2];
						}
						FitSample s;
						float facing = 0.0f;
						// Only original surface facing the candidate's way: a recess wall the face folded over does not lend it shading.
						if (!m_original.OriginalNormalAt(point, face, s.source, facing, matches) || !(facing > 0.0f))
						{
							continue;
						}
						s.lambda = sample[0];
						s.weight = area * facing;
						for (int axis = 0; axis < 3; ++axis)
						{
							s.rest[axis] = n1[axis] * sample[1] + n2[axis] * sample[2];
						}
						samples.push_back(s);
					}
				}

				// Candidates: the current normal; the hat-weighted average of the original normals (the vertex's share of each
				// sample); and the vertex's least-squares normal with its neighbours held (one Gauss-Seidel step of fitting the whole
				// patch). The one whose interpolated, normalized shading meets the original best is kept.
				double hat[3] = {}, leastSquares[3] = {};
				for (const FitSample& s : samples)
				{
					for (int axis = 0; axis < 3; ++axis)
					{
						hat[axis] += static_cast<double>(s.weight) * s.lambda * s.source[axis];
						leastSquares[axis] += static_cast<double>(s.weight) * s.lambda * (s.source[axis] - s.rest[axis]);
					}
				}
				const auto score = [&](const float* candidate) {
					double total = 0.0;
					for (const FitSample& s : samples)
					{
						float shading[3];
						for (int axis = 0; axis < 3; ++axis)
						{
							shading[axis] = candidate[axis] * s.lambda + s.rest[axis];
						}
						const float length = std::sqrt(Dot3(shading, shading));
						if (!(length > 1.0e-3f))
						{
							total += 4.0 * s.weight;
							continue;
						}
						const float d[3] = { shading[0] / length - s.source[0], shading[1] / length - s.source[1], shading[2] / length - s.source[2] };
						total += static_cast<double>(s.weight) * Dot3(d, d);
					}
					return total;
				};
				float normal[3] = { current[0], current[1], current[2] };
				const double currentScore = score(current);
				double bestScore = currentScore;
				bool fit = false;
				for (const double* direction : { hat, leastSquares })
				{
					const double length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
					if (!(length > 1.0e-12))
					{
						continue;
					}
					const float candidate[3] = { static_cast<float>(direction[0] / length), static_cast<float>(direction[1] / length), static_cast<float>(direction[2] / length) };
					fit = true;
					const double candidateScore = score(candidate);
					if (candidateScore < bestScore)
					{
						bestScore = candidateScore;
						std::memcpy(normal, candidate, sizeof(normal));
					}
				}

				const bool changed = bestScore < currentScore && Dot3(normal, current) < m_layout.refitCos;
				if (!changed)
				{
					std::memcpy(normal, current, sizeof(normal));
				}
				const bool replace = c.moved[k] != 0 || changed;
				if (fanKey.first != 0u)
				{
					std::atomic_ref<uint64_t>(m_fanKeys[vertexId].first).store(replace ? 0u : fanKey.first, std::memory_order_relaxed);
					std::atomic_ref<uint64_t>(m_fanKeys[vertexId].second).store(replace ? 0u : fanKey.second, std::memory_order_relaxed);
				}
				if (!replace)
				{
					continue;
				}
				if (!fit)
				{
					// Nothing to sample (or it cancelled): keep the current normal unless it faces away from the vertex's triangles.
					++fallbacks;
					const double faceLength = std::sqrt(faces[0] * faces[0] + faces[1] * faces[1] + faces[2] * faces[2]);
					const double agreement = current[0] * faces[0] + current[1] * faces[1] + current[2] * faces[2];
					if (faceLength > 0.0 && !(agreement > 0.0))
					{
						for (int axis = 0; axis < 3; ++axis)
						{
							normal[axis] = static_cast<float>(faces[axis] / faceLength);
						}
					}
				}
				else if (!c.moved[k])
				{
					++refit;
				}
				c.replace[k] = 1u;
				std::memcpy(&c.normals[static_cast<size_t>(k) * 3u], normal, sizeof(normal));

				// UVs: a moved vertex takes the update's solution, kept within the range its neighbours' stored UVs span in the same
				// chart (a triangle's corners share one chart) together with its own stored value, so by induction every UV stays
				// within the original's; an unmoved one keeps its own.
				std::array<std::array<float, 2>, kMaxUvStreams> uvs{};
				// Follow-up (transferUvs): the vertex's match on the original, once for all its streams.
				uint32_t matchTriangle = UINT32_MAX;
				float matchWeights[3] = {};
				const bool transferAll = m_layout.transferUvs && c.moved[k] && solvedAttributes && uvStreams > 0u &&
					OriginalMatchAt(position(k), faces, matchTriangle, matchWeights, &matches);
				for (uint32_t stream = 0; stream < uvStreams; ++stream)
				{
					const float* own = &mesh.vertex_attributes[static_cast<size_t>(c.vertex_ids[k]) * attributeStride + m_layout.uvAttribute + stream * 2u];
					uvs[stream] = { own[0], own[1] };
					if (!c.moved[k] || !solvedAttributes)
					{
						continue;
					}
					const float* solved = &c.attributes[static_cast<size_t>(k) * attributeStride + m_layout.uvAttribute + stream * 2u];
					if (!std::isfinite(solved[0]) || !std::isfinite(solved[1]))
					{
						++nonFinite;
						continue;
					}
					float low[2] = { own[0], own[1] };
					float high[2] = { own[0], own[1] };
					for (uint32_t corner = offsets[k]; corner < offsets[k + 1u]; ++corner)
					{
						const uint32_t first = corners[corner] / 3u * 3u;
						const uint32_t j = corners[corner] % 3u;
						for (uint32_t other : { (j + 1u) % 3u, (j + 2u) % 3u })
						{
							const float* value = storedUv(c.indices[first + other], stream);
							for (int axis = 0; axis < 2; ++axis)
							{
								if (std::isfinite(value[axis]))
								{
									low[axis] = std::min(low[axis], value[axis]);
									high[axis] = std::max(high[axis], value[axis]);
								}
							}
						}
					}
					for (int axis = 0; axis < 2; ++axis)
					{
						const float value = std::clamp(solved[axis], low[axis], high[axis]);
						clamped += value != solved[axis] ? 1u : 0u;
						uvs[stream][axis] = value;
					}
					// Stage 5 (topology candidates only, which ask for their texcoords): the record texcoord is transferred from the
					// original at the vertex's side-aware match, as normals are, when that lands inside the box above grown by a quarter
					// (the vertex's own chart neighbourhood); meshoptimizer's solve drifts on curved surfaces and the drift accumulates
					// over levels. Otherwise the clamped solve stays.
					if (transferAll)
					{
						// Every stream, every candidate: the original's value at the match (source vertices' attribute rows are never
						// rewritten), kept when it lies inside this stream's box grown by a quarter, as stage 5 does for the texcoord.
						const uint32_t* triangle = &m_original.Original().indices[static_cast<size_t>(matchTriangle) * 3u];
						float value[2] = { 0.0f, 0.0f };
						for (uint32_t corner = 0; corner < 3u; ++corner)
						{
							const float* row = &mesh.vertex_attributes[static_cast<size_t>(triangle[corner]) * attributeStride + m_layout.uvAttribute + stream * 2u];
							value[0] += row[0] * matchWeights[corner];
							value[1] += row[1] * matchWeights[corner];
						}
						bool inside = std::isfinite(value[0]) && std::isfinite(value[1]);
						for (int axis = 0; axis < 2; ++axis)
						{
							const float pad = 0.25f * (high[axis] - low[axis]);
							inside = inside && value[axis] >= low[axis] - pad && value[axis] <= high[axis] + pad;
						}
						if (inside)
						{
							uvs[stream] = { value[0], value[1] };
							++transferred;
						}
						else
						{
							++transferOutside;
						}
					}
					else if (c.uvs != nullptr && stream == 0u && m_layout.texcoordSolved)
					{
						float transferred[2];
						if (OriginalTexcoordAt(position(k), faces, transferred))
						{
							bool inside = true;
							for (int axis = 0; axis < 2; ++axis)
							{
								const float pad = 0.25f * (high[axis] - low[axis]);
								inside = inside && transferred[axis] >= low[axis] - pad && transferred[axis] <= high[axis] + pad;
							}
							if (inside)
							{
								uvs[stream] = { transferred[0], transferred[1] };
							}
						}
					}
				}

				std::byte* record = reinterpret_cast<std::byte*>(c.records) + static_cast<size_t>(k) * recordSize;
				std::memcpy(record, source, stride);
				std::memcpy(record, position(k), sizeof(float) * 3u);
				std::memcpy(record + m_layout.normalOffset, normal, sizeof(normal));
				if (m_layout.tangentOffset != SIZE_MAX)
				{
					float tangent[4];
					std::memcpy(tangent, record + m_layout.tangentOffset, sizeof(tangent));
					Orthogonalize(tangent, normal);
					std::memcpy(record + m_layout.tangentOffset, tangent, sizeof(tangent));
				}
				if (m_layout.texcoordSolved && uvStreams > 0u)
				{
					std::memcpy(record + m_layout.texcoordOffset, uvs[0].data(), sizeof(float) * 2u);
				}
				std::byte* sets = record + stride;
				for (size_t set = 0; set < m_store.uvSets.size(); ++set)
				{
					float value[2] = { 0.0f, 0.0f };
					const uint32_t stream = (m_layout.texcoordSolved ? 1u : 0u) + static_cast<uint32_t>(set);
					if (set < m_layout.uvSetsSolved && stream < uvStreams)
					{
						value[0] = uvs[stream][0];
						value[1] = uvs[stream][1];
					}
					else if (c.vertex_ids[k] < m_store.uvSets[set].values.size())
					{
						value[0] = m_store.uvSets[set].values[c.vertex_ids[k]].x;
						value[1] = m_store.uvSets[set].values[c.vertex_ids[k]].y;
					}
					std::memcpy(sets + set * sizeof(float) * 2u, value, sizeof(value));
				}
				if (attributeStride > 0u)
				{
					// The new vertex's attributes for the next level's simplification: its own row, with the fitted normal, the
					// tangent made orthogonal to it, and its UVs.
					std::memcpy(attributeRow.data(), &mesh.vertex_attributes[static_cast<size_t>(c.vertex_ids[k]) * attributeStride], sizeof(float) * attributeStride);
					if (m_layout.normalAttribute >= 0)
					{
						std::memcpy(&attributeRow[m_layout.normalAttribute], normal, sizeof(normal));
					}
					if (m_layout.tangentAttribute >= 0)
					{
						Orthogonalize(&attributeRow[m_layout.tangentAttribute], normal);
					}
					for (uint32_t stream = 0; stream < uvStreams; ++stream)
					{
						attributeRow[m_layout.uvAttribute + stream * 2u + 0u] = uvs[stream][0];
						attributeRow[m_layout.uvAttribute + stream * 2u + 1u] = uvs[stream][1];
					}
					std::memcpy(sets + m_store.uvSets.size() * sizeof(float) * 2u, attributeRow.data(), sizeof(float) * attributeStride);
				}
			}
			m_fitVertices.fetch_add(fitted, std::memory_order_relaxed);
			m_fitFallbacks.fetch_add(fallbacks, std::memory_order_relaxed);
			m_refitReplaced.fetch_add(refit, std::memory_order_relaxed);
			m_uvClamped.fetch_add(clamped, std::memory_order_relaxed);
			m_uvNonFinite.fetch_add(nonFinite, std::memory_order_relaxed);
			m_uvTransferred.fetch_add(transferred, std::memory_order_relaxed);
			m_uvTransferOutside.fetch_add(transferOutside, std::memory_order_relaxed);
			m_fanChecks.fetch_add(fanChecks, std::memory_order_relaxed);
			m_fanHits.fetch_add(fanHits, std::memory_order_relaxed);
			m_checkSkipped.fetch_add(checkSkipped, std::memory_order_relaxed);
			m_fitNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - rangeStart).count()), std::memory_order_relaxed);
		};
		const uint32_t chunks = static_cast<uint32_t>((vertexCount + kFitChunkVertices - 1u) / kFitChunkVertices);
		if (chunks <= 1u)
		{
			fitRange(0u, static_cast<uint32_t>(vertexCount));
		}
		else
		{
			// Isolated: this thread, waiting for the ranges, must not take another group's task, which would reuse its per-thread
			// state (the validation baseline a group's candidates share).
			oneapi::tbb::this_task_arena::isolate([&] {
				TaskSchedulerManager::GetInstance().ParallelFor("ClusterLODReplacementVertices::Fit", chunks, [&](size_t chunk) {
					const uint32_t first = static_cast<uint32_t>(chunk) * kFitChunkVertices;
					fitRange(first, std::min(first + kFitChunkVertices, static_cast<uint32_t>(vertexCount)));
				});
			});
		}

		// Stage 5: the texcoord each used vertex would emit (a replaced one's record, otherwise its stored record), for the topology
		// candidates' texture check.
		if (c.uvs != nullptr && m_layout.texcoordOffset != SIZE_MAX)
		{
			for (uint32_t k = 0; k < vertexCount; ++k)
			{
				if (offsets[k] == offsets[k + 1u])
				{
					continue;
				}
				const std::byte* record = c.replace[k] ? reinterpret_cast<const std::byte*>(c.records) + static_cast<size_t>(k) * recordSize
					: records + static_cast<size_t>(c.vertex_ids[k]) * stride;
				std::memcpy(&c.uvs[static_cast<size_t>(k) * 2u], record + m_layout.texcoordOffset, sizeof(float) * 2u);
			}
		}

	}

	bool ReplacementVertexFitter::OriginalTexcoordAt(const float point[3], const double faces[3], float uv[2]) const
	{
		const SimplifySurfaceView& original = m_original.Original();
		if (m_layout.texcoordOffset == SIZE_MAX || original.indices == nullptr || m_original.OriginalBvh().Empty())
		{
			return false;
		}
		const double length = std::sqrt(faces[0] * faces[0] + faces[1] * faces[1] + faces[2] * faces[2]);
		if (!(length > 0.0))
		{
			return false;
		}
		const float face[3] = { static_cast<float>(faces[0] / length), static_cast<float>(faces[1] / length), static_cast<float>(faces[2] / length) };
		const float diagonal = std::max(m_original.Diagonal(), 1.0e-20f);
		SimplifyTriangleBvh::Match match;
		if (!m_original.OriginalBvh().MatchFacing(point, face, diagonal * 2.0f, diagonal * 1.0e-5f, match) || !(match.facing > 0.0f) ||
			static_cast<size_t>(match.triangle) * 3u + 2u >= original.indexCount)
		{
			return false;
		}
		uv[0] = uv[1] = 0.0f;
		for (uint32_t corner = 0; corner < 3u; ++corner)
		{
			const uint32_t vertex = original.indices[static_cast<size_t>(match.triangle) * 3u + corner];
			if (vertex >= m_store.sourceVertexCount)
			{
				return false;
			}
			float texcoord[2];
			std::memcpy(texcoord, m_store.vertices.data() + static_cast<size_t>(vertex) * m_layout.vertexStride + m_layout.texcoordOffset, sizeof(texcoord));
			uv[0] += texcoord[0] * match.barycentric[corner];
			uv[1] += texcoord[1] * match.barycentric[corner];
		}
		return std::isfinite(uv[0]) && std::isfinite(uv[1]);
	}

	bool ReplacementVertexFitter::OriginalMatchAt(const float point[3], const double faces[3], uint32_t& triangle, float barycentric[3], MatchFacingCache* cache) const
	{
		const SimplifySurfaceView& original = m_original.Original();
		if (original.indices == nullptr || m_original.OriginalBvh().Empty())
		{
			return false;
		}
		const double length = std::sqrt(faces[0] * faces[0] + faces[1] * faces[1] + faces[2] * faces[2]);
		if (!(length > 0.0))
		{
			return false;
		}
		const float face[3] = { static_cast<float>(faces[0] / length), static_cast<float>(faces[1] / length), static_cast<float>(faces[2] / length) };
		const float diagonal = std::max(m_original.Diagonal(), 1.0e-20f);
		SimplifyTriangleBvh::Match match;
		const bool matched = cache != nullptr && cache->Covers(&m_original.OriginalBvh(), diagonal * 2.0f, diagonal * 1.0e-5f)
			? cache->Match(point, face, match)
			: m_original.OriginalBvh().MatchFacing(point, face, diagonal * 2.0f, diagonal * 1.0e-5f, match);
		if (!matched || !(match.facing > 0.0f) || static_cast<size_t>(match.triangle) * 3u + 2u >= original.indexCount)
		{
			return false;
		}
		for (uint32_t corner = 0; corner < 3u; ++corner)
		{
			if (original.indices[static_cast<size_t>(match.triangle) * 3u + corner] >= m_store.sourceVertexCount)
			{
				return false;
			}
			barycentric[corner] = match.barycentric[corner];
		}
		triangle = match.triangle;
		return true;
	}

	void ReplacementVertexFitter::Commit(clodMesh& mesh, const clodReplacementBatch& batch)
	{
		ZoneScopedN("ClusterLODReplacementVertices::Commit");
		const size_t stride = m_layout.vertexStride;
		const size_t recordSize = RecordSize();
		const size_t attributeStride = m_layout.attributeStride;
		const size_t vertexCount = m_store.vertices.size() / stride;
		assert(batch.first_id == vertexCount);
		(void)vertexCount;

		const bool locks = m_store.vertexLock != nullptr && !m_store.vertexLock->empty();
		for (size_t k = 0; k < batch.count; ++k)
		{
			const std::byte* record = reinterpret_cast<const std::byte*>(batch.records) + k * recordSize;
			const size_t count = m_store.vertices.size() / stride;
			m_store.vertices.insert(m_store.vertices.end(), record, record + stride);
			const std::byte* sets = record + stride;
			for (size_t set = 0; set < m_store.uvSets.size(); ++set)
			{
				// A set that never matched the vertex count is ignored by the output; leave it so.
				if (m_store.uvSets[set].values.size() != count)
				{
					continue;
				}
				DirectX::XMFLOAT2 value;
				std::memcpy(&value, sets + set * sizeof(float) * 2u, sizeof(value));
				m_store.uvSets[set].values.push_back(value);
			}
			if (m_store.attributes != nullptr && attributeStride > 0u)
			{
				const size_t offset = m_store.attributes->size();
				m_store.attributes->resize(offset + attributeStride);
				std::memcpy(m_store.attributes->data() + offset, sets + m_store.uvSets.size() * sizeof(float) * 2u, sizeof(float) * attributeStride);
			}
			if (locks)
			{
				const unsigned char parentLock = (*m_store.vertexLock)[batch.parents[k]];
				m_store.vertexLock->push_back(parentLock);
			}
			if (m_store.charts != nullptr)
			{
				const uint32_t parentChart = (*m_store.charts)[batch.parents[k]];
				m_store.charts->push_back(parentChart);
			}
			if (m_store.skinning != nullptr)
			{
				// Skinning is fixed by position, which a last-resort replacement keeps; its normal is the fitted one.
				std::vector<std::byte>& skinning = *m_store.skinning;
				const size_t row = skinning.size();
				skinning.resize(row + m_store.skinningStride);
				std::memcpy(skinning.data() + row, skinning.data() + static_cast<size_t>(batch.parents[k]) * m_store.skinningStride, m_store.skinningStride);
				std::memcpy(skinning.data() + row + m_store.skinningNormalOffset, record + m_layout.normalOffset, sizeof(float) * 3u);
			}
		}

		mesh.vertex_positions = reinterpret_cast<const float*>(m_store.vertices.data());
		mesh.vertex_count = m_store.vertices.size() / stride;
		m_fanKeys.resize(mesh.vertex_count, FanKey{});
		if (m_store.attributes != nullptr && attributeStride > 0u)
		{
			mesh.vertex_attributes = m_store.attributes->data();
		}
		if (locks)
		{
			mesh.vertex_lock = m_store.vertexLock->data();
		}
	}

	float ReplacementVertexFitter::Measure(const clodValidateCandidate& candidate) const
	{
		const SimplifySurfaceView input = View(candidate.input_indices, candidate.input_index_count);
		SimplifySurfaceView output = View(candidate.output_indices, candidate.output_index_count);
		if (candidate.output_positions != nullptr)
		{
			output.positions = candidate.output_positions;
			output.positionStride = sizeof(float) * 3u;
			output.normals = candidate.output_normals;
			output.normalStride = sizeof(float) * 3u;
		}
		return m_original.MeasureDeviation(input, output);
	}

	ReplacementVertexFitter::Stats ReplacementVertexFitter::GetStats() const
	{
		Stats stats;
		stats.fitVertices = m_fitVertices.load();
		stats.fitFallbacks = m_fitFallbacks.load();
		stats.refitReplaced = m_refitReplaced.load();
		stats.uvClamped = m_uvClamped.load();
		stats.uvNonFinite = m_uvNonFinite.load();
		stats.uvTransferred = m_uvTransferred.load();
		stats.uvTransferOutside = m_uvTransferOutside.load();
		stats.fanChecks = m_fanChecks.load();
		stats.fanHits = m_fanHits.load();
		stats.checkSkipped = m_checkSkipped.load();
		stats.fitNs = m_fitNs.load();
		return stats;
	}

}
