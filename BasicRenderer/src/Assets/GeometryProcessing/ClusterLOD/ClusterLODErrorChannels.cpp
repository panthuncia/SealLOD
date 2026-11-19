#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODErrorChannels.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <limits>
#include <utility>
#include <vector>

#include <oneapi/tbb/task_arena.h>
#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

#include <BasicRenderer/Streaming/TaskScheduler.h>
#include <BasicRenderer/Assets/ClusterLODTextureColor.h>
#include <BasicRenderer/Assets/ClusterLODNormalSpread.h>
#include "BasicRenderer/Assets/Geometry/VertexLayout.h"

namespace clod_detail {

	namespace {

		// Area samples per triangle, as barycentric weights, each standing for a quarter of its area (as validation and CLodEval).
		constexpr std::array<std::array<float, 3>, 4> kSamples{ {
			{ 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f },
			{ 4.0f / 6.0f, 1.0f / 6.0f, 1.0f / 6.0f },
			{ 1.0f / 6.0f, 4.0f / 6.0f, 1.0f / 6.0f },
			{ 1.0f / 6.0f, 1.0f / 6.0f, 4.0f / 6.0f },
		} };

		// Stage 6 step 3: the six ring directions of the 7-point disc rule.
		constexpr float kRingCos[6] = { 1.0f, 0.5f, -0.5f, -1.0f, -0.5f, 0.5f };
		constexpr float kRingSin[6] = { 0.0f, 0.86602540f, 0.86602540f, 0.0f, -0.86602540f, -0.86602540f };

		// An interpolated coarse normal shorter than this cancels (CLodEval's appearance analyzer uses the same).
		constexpr float kMinInterpolatedNormalLength = 0.01f;

		constexpr uint32_t kNoOwner = std::numeric_limits<uint32_t>::max();

		// Triangles per task when one group's S(g) is measured in parallel.
		constexpr size_t kMeasureChunkTriangles = 256u;
		// Owned original triangles per task when a group follows many of them up the DAG.
		constexpr size_t kOwnedChunkTriangles = 4096u;
		// Texture matches one group keeps for its texture pass (36 bytes each), beyond which samples are matched again there.
		constexpr size_t kMaxTextureCandidatesKept = size_t(1) << 20;

		inline float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

		// What the pages store and the GPU decodes: OctEncodeNormal and PackOctNormalSnorm16 (ClusterLODClustering.cpp), then
		// DecodeOctNormalWord (CLodPageDecoder.cpp).
		void DecodedNormal(const float* in, float* out)
		{
			float nx = in[0], ny = in[1], nz = in[2];
			const float denom = std::abs(nx) + std::abs(ny) + std::abs(nz);
			if (denom > 1e-8f)
			{
				nx /= denom;
				ny /= denom;
				nz /= denom;
			}
			if (nz < 0.0f)
			{
				const float ox = nx;
				nx = (1.0f - std::abs(ny)) * (ox >= 0.0f ? 1.0f : -1.0f);
				ny = (1.0f - std::abs(ox)) * (ny >= 0.0f ? 1.0f : -1.0f);
			}
			const auto quantize = [](float value) {
				const float clamped = std::max(-1.0f, std::min(1.0f, value));
				return std::max(static_cast<float>(static_cast<int16_t>(std::round(clamped * 32767.0f))) / 32767.0f, -1.0f);
			};
			const float octX = quantize(nx);
			const float octY = quantize(ny);
			float x = octX, y = octY;
			const float z = 1.0f - std::abs(octX) - std::abs(octY);
			if (z < 0.0f)
			{
				x = (1.0f - std::abs(octY)) * (octX >= 0.0f ? 1.0f : -1.0f);
				y = (1.0f - std::abs(octX)) * (octY >= 0.0f ? 1.0f : -1.0f);
			}
			const float length = std::sqrt(x * x + y * y + z * z);
			if (length <= 0.0f)
			{
				out[0] = 0.0f;
				out[1] = 0.0f;
				out[2] = 1.0f;
				return;
			}
			out[0] = x / length;
			out[1] = y / length;
			out[2] = z / length;
		}

		struct ChannelTriangle
		{
			float position[3][3];
			float normal[3][3];
			uint32_t group; // the group holding it
			uint32_t meshlet; // its meshlet in that group's raw chunk, and its index there
			uint32_t triangleInMeshlet;
		};

		// Calls visit(corner vertex indices into the group's vertex chunk, triangle index in the meshlet) for every triangle of one meshlet.
		template <typename Visit>
		void ForEachMeshletTriangleIndexed(const ClusterLODBuildState& state, uint32_t group, uint32_t meshletIndex, Visit&& visit)
		{
			const std::vector<meshopt_Meshlet>& meshlets = state.groupMeshletChunks[group];
			const std::vector<uint32_t>& meshletVertices = state.groupMeshletVertexChunks[group];
			const std::vector<uint8_t>& meshletTriangles = state.groupMeshletTriangleChunks[group];
			if (meshletIndex >= meshlets.size())
			{
				return;
			}
			const meshopt_Meshlet& meshlet = meshlets[meshletIndex];
			for (uint32_t triangle = 0; triangle < meshlet.triangle_count; ++triangle)
			{
				const uint32_t base = meshlet.triangle_offset + triangle * 3u;
				if (base + 2u >= meshletTriangles.size())
				{
					continue;
				}
				uint32_t corners[3];
				bool valid = true;
				for (uint32_t corner = 0; corner < 3u; ++corner)
				{
					const uint32_t local = meshletTriangles[base + corner];
					if (local >= meshlet.vertex_count || meshlet.vertex_offset + local >= meshletVertices.size())
					{
						valid = false;
						break;
					}
					corners[corner] = meshletVertices[meshlet.vertex_offset + local];
				}
				if (valid)
				{
					visit(corners, triangle);
				}
			}
		}

		// Calls visit(corner vertex indices into the group's vertex chunk) for every triangle of one meshlet.
		template <typename Visit>
		void ForEachMeshletTriangle(const ClusterLODBuildState& state, uint32_t group, uint32_t meshletIndex, Visit&& visit)
		{
			ForEachMeshletTriangleIndexed(state, group, meshletIndex, [&](const uint32_t* corners, uint32_t) { visit(corners); });
		}

		void ReadFloat3(const std::vector<std::byte>& vertices, size_t stride, uint32_t vertex, size_t offset, float* out)
		{
			std::memcpy(out, vertices.data() + static_cast<size_t>(vertex) * stride + offset, sizeof(float) * 3u);
		}

		// Unbounded nearest distances to the original by exact point, for corners shared by several triangles.
		class CornerDistances
		{
		public:
			// Reset, but a table of the same build keeps its corners (a corner's distance does not depend on the group), up to
			// kMaxCorners.
			void Continue(uint64_t buildId, size_t expected)
			{
				if (buildId == m_buildId && buildId != 0u && m_used + expected <= kMaxCorners && (m_used + expected) * 2u <= m_table.size())
				{
					return;
				}
				m_buildId = buildId;
				Reset(std::max(expected, std::min<size_t>(kMaxCorners, expected * 8u)));
			}

			void Reset(size_t expected)
			{
				size_t size = 64u;
				while (size < expected * 2u)
				{
					size *= 2u;
				}
				if (m_table.size() < size)
				{
					m_table.assign(size, Entry{});
					m_generation = 0u;
				}
				if (++m_generation == 0u)
				{
					for (Entry& entry : m_table)
					{
						entry.generation = 0u;
					}
					m_generation = 1u;
				}
				m_used = 0u;
			}

			float Get(const SimplifyTriangleBvh& bvh, const float* point)
			{
				uint32_t key[3];
				std::memcpy(key, point, sizeof(key));
				if ((m_used + 1u) * 2u > m_table.size())
				{
					return bvh.NearestDistanceSquared(point, std::numeric_limits<float>::max());
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
					entry.distanceSquared = bvh.NearestDistanceSquared(point, std::numeric_limits<float>::max());
					++m_used;
				}
				return entry.distanceSquared;
			}

		private:
			struct Entry
			{
				uint32_t key[3];
				uint32_t generation;
				float distanceSquared;
			};
			static constexpr size_t kMaxCorners = size_t(1) << 16;
			std::vector<Entry> m_table;
			size_t m_used = 0u;
			uint32_t m_generation = 0u;
			uint64_t m_buildId = 0u;
		};


	}

	// Stage 6 step 1: the visible damage of one displaced sample (ErrorChannelTextureColor), filtered at footprint mesh units.
	float ColorSampleWeight(const ErrorChannelTextureColor& color, const TextureSampleQuery& query, float footprint)
	{
		if (!query.matched)
		{
			return 1.0f;
		}
		std::array<float, 4> drawn = color.baseColorFactor;
		std::array<float, 4> source = color.baseColorFactor;
		if (color.texture != nullptr)
		{
			const clod_texture::Texture& texture = *color.texture;
			std::array<float, 4> a, b;
			if (color.uvTransformed)
			{
				// UVs through the material's affine texture transform; footprints scale by its area ratio.
				const std::array<float, 6>& m = color.uvTransform;
				const double scale = std::sqrt(std::fabs(static_cast<double>(m[0]) * m[4] - static_cast<double>(m[1]) * m[3]));
				const auto transformed = [&](float u, float v, float uvScale) {
					return clod_texture::SampleLinear(texture, static_cast<double>(m[0]) * u + static_cast<double>(m[1]) * v + m[2],
						static_cast<double>(m[3]) * u + static_cast<double>(m[4]) * v + m[5],
						clod_texture::TextureLod(texture, static_cast<double>(footprint) * uvScale * scale));
				};
				a = transformed(query.outputUv[0], query.outputUv[1], query.outputUvScale);
				b = transformed(query.sourceUv[0], query.sourceUv[1], query.sourceUvScale);
			}
			else
			{
				a = clod_texture::SampleLinear(
					texture, query.outputUv[0], query.outputUv[1], clod_texture::TextureLod(texture, static_cast<double>(footprint) * query.outputUvScale));
				b = clod_texture::SampleLinear(
					texture, query.sourceUv[0], query.sourceUv[1], clod_texture::TextureLod(texture, static_cast<double>(footprint) * query.sourceUvScale));
			}
			for (int c = 0; c < 4; ++c)
			{
				drawn[c] *= a[c];
				source[c] *= b[c];
			}
		}
		if (color.alphaMasked)
		{
			const bool drawnVisible = drawn[3] >= color.alphaCutoff;
			const bool sourceVisible = source[3] >= color.alphaCutoff;
			if (drawnVisible != sourceVisible)
			{
				return 1.0f;
			}
			if (!drawnVisible)
			{
				return 0.0f;
			}
		}
		const double ratio = clod_texture::DeltaE(drawn, source) / std::max(static_cast<double>(color.referenceDeltaE), 1.0e-6);
		return static_cast<float>(std::min(1.0, ratio));
	}

	ErrorChannelStats MeasureClusterLODErrorChannels(
		ClusterLODBuildState& state, const SimplifyValidator& original, size_t vertexStrideBytes, bool hasNormals,
		const ErrorChannelTextureInputs* texture, bool normalSpread, bool measureChannels, float normalSpreadFootprint, float normalSpreadSampleCap,
		uint32_t samplesPerTriangle)
	{
		const uint32_t sampleCount = AreaSampleCount(samplesPerTriangle);
		ZoneScopedN("ClusterLODErrorChannels::Measure");
		const auto start = std::chrono::steady_clock::now();
		ErrorChannelStats stats{};
		const uint32_t groupCount = static_cast<uint32_t>(state.groups.size());
		state.errorChannels.assign(groupCount, {});
		if (groupCount == 0u || state.groupVertexChunks.size() < groupCount || state.groupMeshletChunks.size() < groupCount ||
			state.groupMeshletVertexChunks.size() < groupCount || state.groupMeshletTriangleChunks.size() < groupCount ||
			state.groupMeshletRefinedGroupChunks.size() < groupCount)
		{
			state.errorChannels.clear();
			return stats;
		}

		// S(g): the (holding group, meshlet) pairs whose clusters refine into g. Original triangles: the leaf meshlets'.
		std::vector<std::vector<std::pair<uint32_t, uint32_t>>> outputs(groupCount);
		std::vector<float> sourceCentroids;
		std::vector<uint32_t> owners;
		int32_t maxDepth = 0;
		for (uint32_t group = 0; group < groupCount; ++group)
		{
			maxDepth = std::max(maxDepth, state.groups[group].depth);
			const std::vector<int32_t>& tags = state.groupMeshletRefinedGroupChunks[group];
			for (uint32_t meshlet = 0; meshlet < tags.size(); ++meshlet)
			{
				const int32_t refined = tags[meshlet];
				if (refined >= 0 && static_cast<uint32_t>(refined) < groupCount)
				{
					outputs[refined].push_back({ group, meshlet });
				}
				else if (refined < 0)
				{
					const std::vector<std::byte>& vertices = state.groupVertexChunks[group];
					ForEachMeshletTriangle(state, group, meshlet, [&](const uint32_t* corners) {
						float centroid[3] = {};
						for (uint32_t corner = 0; corner < 3u; ++corner)
						{
							float p[3];
							ReadFloat3(vertices, vertexStrideBytes, corners[corner], MeshVertexLayout::PositionOffset, p);
							centroid[0] += p[0] / 3.0f;
							centroid[1] += p[1] / 3.0f;
							centroid[2] += p[2] / 3.0f;
						}
						sourceCentroids.insert(sourceCentroids.end(), centroid, centroid + 3);
						owners.push_back(group);
					});
				}
			}
		}
		stats.sourceTriangles = owners.size();

		std::vector<float> outsideDeviation(groupCount, 0.0f); // S(g) to the original
		std::vector<float> sourceDeviation(groupCount, 0.0f);  // owned original to S(g)
		std::vector<double> normalQ(groupCount, 0.0);
		// Stage 6: Q_T per group, and what it measured. Off unless the texture inputs are complete.
		const bool textureChannel = texture != nullptr && texture->weight > 0.0f && texture->buildUvs != nullptr && texture->source.uvs != nullptr &&
			texture->buildCharts != nullptr && texture->source.charts != nullptr && state.groupVertexGlobalIdChunks.size() >= groupCount;
		std::vector<double> textureQ(groupCount, 0.0);
		std::vector<double> textureDisplaced(groupCount, 0.0); // binary displaced area (Q_T itself unless weighted by colour)
		std::vector<double> textureMeasured(groupCount, 0.0);
		std::atomic<uint64_t> textureNanoseconds = 0;
		std::array<std::atomic<uint64_t>, 5> textureColorSamples{};
		// Stage 6 step 2: one slot per raw meshlet of every group, written by the group its cluster refines into (each meshlet
		// refines into one group, so concurrent groups write disjoint slots).
		const bool remapChannel = textureChannel && texture->remap != nullptr && texture->remap->minDisplacedShare > 0.0f;
		state.meshletUvRemaps.clear();
		if (remapChannel)
		{
			state.meshletUvRemaps.resize(groupCount);
			for (uint32_t group = 0; group < groupCount; ++group)
			{
				state.meshletUvRemaps[group].resize(state.groupMeshletChunks[group].size());
			}
		}
		// Each cluster's byte budget: its share (by triangles) of what its group page blob leaves of a page. A mesh-wide page holding
		// a segment alone is no larger than the page blob it came from, so every block then fits beside its cluster.
		std::vector<std::vector<uint32_t>> remapBudgets;
		if (remapChannel)
		{
			remapBudgets.resize(groupCount);
			for (uint32_t group = 0; group < groupCount; ++group)
			{
				const std::vector<meshopt_Meshlet>& meshlets = state.groupMeshletChunks[group];
				remapBudgets[group].assign(meshlets.size(), 0u);
				uint32_t first = 0u;
				for (const std::vector<std::byte>& page : state.groupPageBlobs[group])
				{
					uint32_t count = 0u;
					if (page.size() >= sizeof(CLodPageHeader))
					{
						std::memcpy(&count, page.data() + offsetof(CLodPageHeader, meshletCount), sizeof(count));
					}
					uint64_t pageTriangles = 0u;
					for (uint32_t m = first; m < first + count && m < meshlets.size(); ++m)
					{
						pageTriangles += meshlets[m].triangle_count;
					}
					// Stage 6 step 3: normal spread blocks take their (small) share first, so both fit beside their clusters.
					uint64_t spreadReserve = 0u;
					if (normalSpread && hasNormals)
					{
						const std::vector<int32_t>& tags = state.groupMeshletRefinedGroupChunks[group];
						for (uint32_t m = first; m < first + count && m < meshlets.size(); ++m)
						{
							spreadReserve += m < tags.size() && tags[m] >= 0 ? clod_normal_spread::BlockSize(meshlets[m].vertex_count) : 0u;
						}
					}
					const uint64_t used = page.size() + 256u + spreadReserve;
					const uint64_t free = used < CLOD_STREAMING_PAGE_SIZE_BYTES ? CLOD_STREAMING_PAGE_SIZE_BYTES - used : 0u;
					for (uint32_t m = first; m < first + count && m < meshlets.size(); ++m)
					{
						remapBudgets[group][m] = pageTriangles != 0u ? static_cast<uint32_t>((free * meshlets[m].triangle_count / pageTriangles) & ~3ull) : 0u;
					}
					first += count;
				}
			}
		}
		std::vector<uint8_t> remapped(groupCount, 0u);
		std::vector<double> remapDisplacedBefore(groupCount, 0.0), remapQBefore(groupCount, 0.0);
		std::vector<std::array<double, 6>> remapParts(groupCount, std::array<double, 6>{});
		std::atomic<uint64_t> remapClusters = 0, remapTriangles = 0, remapNodes = 0, remapNodesUnmatched = 0, remapNodesAlongNormal = 0, remapBytes = 0;
		std::atomic<uint64_t> remapTrianglesOverBudget = 0;
		std::atomic<uint64_t> remapSubTriangles = 0, remapSubTrianglesCharts = 0, remapSubTrianglesSeams = 0;
		std::atomic<uint64_t> remapNanoseconds = 0;
		std::atomic<uint64_t> remapTrianglesRegions = 0, remapTrianglesGrid = 0, remapRegions = 0, remapRegionCells = 0, remapPointsUnlabeled = 0;
		std::atomic<uint64_t> remapRegionsFitted = 0, remapTrianglesRegionsRejected = 0, remapRegionsPlanarPieces = 0;
		std::array<std::atomic<uint64_t>, 5> remapRegionGridCounts{};
		std::vector<std::array<double, 2>> remapRegionAreas(groupCount, std::array<double, 2>{});
		std::atomic<uint64_t> samples = 0, samplesWithoutNormal = 0, simplifiedTriangles = 0;
		const SimplifyTriangleBvh& originalBvh = original.OriginalBvh();
		constexpr float kUnbounded = std::numeric_limits<float>::max();

		// Stage 6 step 3: per holding group, per raw meshlet that refines into a group, the squared chord of each of its triangles'
		// area samples (kSamples order; negative: nothing to compare with), written by the group the meshlet refines into (one writer).
		const bool spreadChannel = normalSpread && hasNormals;
		std::vector<std::vector<std::vector<float>>> spreadChords;
		state.meshletNormalSpreads.clear();
		if (spreadChannel)
		{
			spreadChords.resize(groupCount);
			for (uint32_t group = 0; group < groupCount; ++group)
			{
				const std::vector<meshopt_Meshlet>& meshlets = state.groupMeshletChunks[group];
				const std::vector<int32_t>& tags = state.groupMeshletRefinedGroupChunks[group];
				spreadChords[group].resize(meshlets.size());
				for (uint32_t meshlet = 0; meshlet < meshlets.size() && meshlet < tags.size(); ++meshlet)
				{
					if (tags[meshlet] >= 0)
					{
						spreadChords[group][meshlet].assign(static_cast<size_t>(meshlets[meshlet].triangle_count) * kSamples.size(), -1.0f);
					}
				}
			}
		}

		std::vector<std::vector<uint32_t>> owned(groupCount);
		// Stage 6 step 2: one damaged group's remap blocks and its texture damage through them (the residual). Layout 1 runs it inside the
		// depth loop with the page shares above; layout 2 after it, with page space shared only among the clusters that take blocks.
		struct DeferredRemap
		{
			float radius = 0.0f;
			std::vector<float> flatPositions, flatUvs;
			std::vector<uint32_t> flatCharts, flatIndices;
			std::vector<std::pair<uint32_t, uint32_t>> flatOwners;
			TextureDisplacement displacement;
		};
		std::vector<std::unique_ptr<DeferredRemap>> deferredRemaps(remapChannel ? groupCount : 0u);
		const auto buildGroupRemaps = [&](uint32_t group, float radius, const std::vector<float>& flatPositions, const std::vector<float>& flatUvs,
			const std::vector<uint32_t>& flatCharts, const std::vector<uint32_t>& flatIndices, const std::vector<std::pair<uint32_t, uint32_t>>& flatOwners,
			const SimplifySurfaceView& view, const TextureCheckOptions& options, const TextureDisplacement& displacement,
			const std::vector<std::vector<uint32_t>>& budgets) {
			(void)flatCharts;
			const auto remapStart = std::chrono::steady_clock::now();
			// The match reach covers how far S(g) lies from the original (its measured deviation), at least the error.
			const float remapReach = std::max(1.5f * outsideDeviation[group], radius);
			std::vector<UvRemapCluster> clusters(outputs[group].size());
			std::vector<std::vector<UvRemapTriangle>> clusterTriangles(outputs[group].size());
			for (uint32_t output = 0; output < outputs[group].size(); ++output)
			{
				const auto [holder, meshlet] = outputs[group][output];
				clusterTriangles[output].resize(state.groupMeshletChunks[holder][meshlet].triangle_count);
				for (UvRemapTriangle& triangle : clusterTriangles[output])
				{
					triangle.valid = false;
				}
			}
			for (size_t flat = 0; flat < flatOwners.size(); ++flat)
			{
				const auto [output, triangleInMeshlet] = flatOwners[flat];
				if (triangleInMeshlet >= clusterTriangles[output].size())
				{
					continue;
				}
				UvRemapTriangle& triangle = clusterTriangles[output][triangleInMeshlet];
				for (uint32_t corner = 0; corner < 3u; ++corner)
				{
					const uint32_t vertex = flatIndices[flat * 3u + corner];
					std::memcpy(triangle.position[corner], &flatPositions[static_cast<size_t>(vertex) * 3u], sizeof(float) * 3u);
					std::memcpy(triangle.uv[corner], &flatUvs[static_cast<size_t>(vertex) * 2u], sizeof(float) * 2u);
					triangle.chart[corner] = flatCharts[vertex];
				}
				triangle.valid = true;
			}
			uint32_t clustersWithBlocks = 0;
			for (uint32_t output = 0; output < outputs[group].size(); ++output)
			{
				UvRemapCluster& cluster = clusters[output];
				const auto [budgetHolder, budgetMeshlet] = outputs[group][output];
				cluster = BuildClusterUvRemap(original, texture->source, texture->remap->settings, clusterTriangles[output], radius, remapReach,
					budgets[budgetHolder][budgetMeshlet]);
				remapTrianglesOverBudget.fetch_add(cluster.trianglesOverBudget, std::memory_order_relaxed);
				remapSubTriangles.fetch_add(cluster.subTriangles, std::memory_order_relaxed);
				remapSubTrianglesCharts.fetch_add(cluster.subTrianglesStraddlingCharts, std::memory_order_relaxed);
				remapSubTrianglesSeams.fetch_add(cluster.subTrianglesStraddlingSeams, std::memory_order_relaxed);
				remapPointsUnlabeled.fetch_add(cluster.pointsUnlabeled, std::memory_order_relaxed);
				remapTrianglesRegionsRejected.fetch_add(cluster.trianglesRegionsRejected, std::memory_order_relaxed);
				if (cluster.block.empty())
				{
					continue;
				}
				const auto [holder, meshlet] = outputs[group][output];
				state.meshletUvRemaps[holder][meshlet] = cluster.block;
				++clustersWithBlocks;
				remapTriangles.fetch_add(cluster.trianglesRemapped, std::memory_order_relaxed);
				remapNodes.fetch_add(cluster.nodes, std::memory_order_relaxed);
				remapNodesUnmatched.fetch_add(cluster.nodesUnmatched, std::memory_order_relaxed);
				remapNodesAlongNormal.fetch_add(cluster.nodesAlongNormal, std::memory_order_relaxed);
				remapBytes.fetch_add(cluster.block.size(), std::memory_order_relaxed);
				remapTrianglesRegions.fetch_add(cluster.trianglesRegions, std::memory_order_relaxed);
				remapTrianglesGrid.fetch_add(cluster.trianglesGrid, std::memory_order_relaxed);
				remapRegions.fetch_add(cluster.regions, std::memory_order_relaxed);
				remapRegionCells.fetch_add(cluster.regionCells, std::memory_order_relaxed);
				remapRegionsFitted.fetch_add(cluster.regionsFitted, std::memory_order_relaxed);
				remapRegionsPlanarPieces.fetch_add(cluster.regionsPlanarPieces, std::memory_order_relaxed);
				for (size_t k = 0; k < remapRegionGridCounts.size(); ++k)
				{
					remapRegionGridCounts[k].fetch_add(cluster.regionGridCounts[k], std::memory_order_relaxed);
				}
				remapRegionAreas[group][0] += cluster.regionArea;
				remapRegionAreas[group][1] += cluster.regionMissArea;
			}
			if (clustersWithBlocks != 0u)
			{
				remapClusters.fetch_add(clustersWithBlocks, std::memory_order_relaxed);
				std::vector<uint32_t> remapSlots;
				const std::function<bool(size_t, const float*, TextureRemapSample&)> remapHook =
					[&](size_t flat, const float* weights, TextureRemapSample& out) {
						if (flat >= flatOwners.size())
						{
							return false;
						}
						const auto [output, triangleInMeshlet] = flatOwners[flat];
						return EvaluateClusterUvRemap(clusters[output], original, texture->source, triangleInMeshlet, weights, remapSlots, out);
					};
				TextureCheckOptions remapOptions = options;
				remapOptions.remap = &remapHook;
				const TextureDisplacement residual = MeasureTextureDisplacement(original, texture->source, view, flatUvs.data(), radius, remapOptions);
				remapped[group] = 1u;
				remapParts[group] = { displacement.slideDisplacedArea, residual.slideDisplacedArea, displacement.chartArea, residual.chartArea,
					displacement.mixedArea, residual.mixedArea };
				remapDisplacedBefore[group] = displacement.displacedArea;
				remapQBefore[group] = textureQ[group];
				textureQ[group] = texture->color != nullptr ? residual.weightedDisplacedArea : residual.displacedArea;
				textureDisplaced[group] = residual.displacedArea;
			}
			remapNanoseconds.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - remapStart).count()), std::memory_order_relaxed);
		};

		for (int32_t depth = 0; depth <= maxDepth; ++depth)
		{
			std::vector<uint32_t> groupsAtDepth;
			for (uint32_t group = 0; group < groupCount; ++group)
			{
				if (state.groups[group].depth == depth)
				{
					groupsAtDepth.push_back(group);
					owned[group].clear();
				}
			}
			for (uint32_t sample = 0; sample < owners.size(); ++sample)
			{
				const uint32_t owner = owners[sample];
				if (owner != kNoOwner && state.groups[owner].depth == depth)
				{
					owned[owner].push_back(sample);
				}
			}

			TaskSchedulerManager::GetInstance().ParallelFor("ClusterLODErrorChannels::MeasureDepth", groupsAtDepth.size(), [&](size_t taskIndex)
				{
					const uint32_t group = groupsAtDepth[taskIndex];
					if (outputs[group].empty())
					{
						// Terminal: nothing replaces this group, so the original it owns stops here.
						for (uint32_t sample : owned[group])
						{
							owners[sample] = kNoOwner;
						}
						return;
					}

					std::vector<ChannelTriangle> triangles;
					for (const auto& [holder, meshlet] : outputs[group])
					{
						const std::vector<std::byte>& vertices = state.groupVertexChunks[holder];
						ForEachMeshletTriangleIndexed(state, holder, meshlet, [&](const uint32_t* corners, uint32_t triangleInMeshlet) {
							ChannelTriangle triangle{};
							triangle.group = holder;
							triangle.meshlet = meshlet;
							triangle.triangleInMeshlet = triangleInMeshlet;
							for (uint32_t corner = 0; corner < 3u; ++corner)
							{
								ReadFloat3(vertices, vertexStrideBytes, corners[corner], MeshVertexLayout::PositionOffset, triangle.position[corner]);
								if (hasNormals)
								{
									float stored[3];
									ReadFloat3(vertices, vertexStrideBytes, corners[corner], MeshVertexLayout::NormalOffset, stored);
									DecodedNormal(stored, triangle.normal[corner]);
								}
							}
							triangles.push_back(triangle);
						});
					}
					simplifiedTriangles.fetch_add(triangles.size(), std::memory_order_relaxed);

					// S(g) against the original: deviation of every sample, and the shading residual of the area samples.
					// One traversal per area sample serves its deviation, its normal match and the texture term's match (the texture
					// pass below selects from textureGathers), and corners shared by triangles are measured once (by exact position).
					// Ranges of triangles run as tasks: each writes only its triangles' slots, and the sums are taken in triangle order
					// afterwards, so the result does not depend on the split.
					const float textureRadius = std::max(state.groups[group].bounds.error, original.Diagonal() * 1.0e-4f);
					const bool normalMatches = hasNormals && original.HasOriginalNormals();
					std::vector<SimplifyTriangleBvh::NearGather> textureGathers(textureChannel ? triangles.size() * kSamples.size() : 0u);
					std::vector<double> qTerms(triangles.size() * kSamples.size(), 0.0); // 0 adds nothing to the (non-negative) sum
					struct MeasureRange
					{
						float outside = 0.0f;
						uint64_t samples = 0, withoutNormal = 0;
						std::vector<SimplifyTriangleBvh::NearCandidate> pool; // this range's texture matches (textureGathers index it)
					};
					// The texture matches the group keeps for its texture pass are bounded: a sample past the budget keeps none
					// (SimplifyTriangleBvh::kNotGathered), and the pass traverses for it itself, which gives the same match.
					std::atomic<size_t> textureCandidatesKept = 0;
					const size_t chunks = std::max<size_t>((triangles.size() + kMeasureChunkTriangles - 1u) / kMeasureChunkTriangles, 1u);
					std::vector<MeasureRange> ranges(chunks);
					const auto measureRange = [&](size_t chunk) {
					MeasureRange& range = ranges[chunk];
					const QueryStatsScope queryScope(QuerySite::Channels);
					const size_t firstTriangle = chunk * kMeasureChunkTriangles;
					const size_t endTriangle = std::min(firstTriangle + kMeasureChunkTriangles, triangles.size());
					float& outside = range.outside;
					uint64_t& localSamples = range.samples;
					uint64_t& localWithoutNormal = range.withoutNormal;
					std::vector<SimplifyTriangleBvh::NearCandidate>& texturePool = range.pool;
					// Each sample's traversal; only its texture request's candidates are kept (in texturePool).
					std::vector<SimplifyTriangleBvh::NearCandidate> matchPool;
					// A range runs without waiting on other tasks, so this per-thread table is its own while it runs.
					thread_local CornerDistances cornerDistances;
					cornerDistances.Continue(originalBvh.BuildId(), (endTriangle - firstTriangle) * 3u);
						for (size_t triangleIndex = firstTriangle; triangleIndex < endTriangle; ++triangleIndex)
						{
							const ChannelTriangle& triangle = triangles[triangleIndex];
							for (uint32_t corner = 0; corner < 3u; ++corner)
							{
								outside = std::max(outside, cornerDistances.Get(originalBvh, triangle.position[corner]));
							}
							const float* a = triangle.position[0];
							const float* b = triangle.position[1];
							const float* c = triangle.position[2];
							const float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
							const float ac[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
							float face[3] = { ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0] };
							const float twiceArea = std::sqrt(Dot(face, face));
							// Stage 6 step 3: the footprint a pixel covers where S(g) starts to draw (a one-pixel threshold at g's error).
							const float spreadRadius = 0.5f * std::max(normalSpreadFootprint, 0.0f) *
								std::max(state.groups[group].bounds.error, original.Diagonal() * 1.0e-4f);
							float* chords = nullptr;
							if (spreadChannel)
							{
								std::vector<float>& slots = spreadChords[triangle.group][triangle.meshlet];
								const size_t first = static_cast<size_t>(triangle.triangleInMeshlet) * kSamples.size();
								chords = first + kSamples.size() <= slots.size() ? slots.data() + first : nullptr;
							}
							for (size_t sampleIndex = 0; sampleIndex < kSamples.size(); ++sampleIndex)
							{
								if (!AreaSampleTaken(triangleIndex, sampleIndex, sampleCount))
								{
									continue;
								}
								const auto& weights = kSamples[sampleIndex];
								float point[3];
								for (int axis = 0; axis < 3; ++axis)
								{
									point[axis] = a[axis] * weights[0] + b[axis] * weights[1] + c[axis] * weights[2];
								}
								SimplifyTriangleBvh::NearRequest requests[2];
								SimplifyTriangleBvh::NearGather gathers[2];
								uint32_t requestCount = 0u;
								const bool normalSample = hasNormals && twiceArea > 0.0f;
								const uint32_t normalRequest = normalSample && normalMatches ? requestCount++ : UINT32_MAX;
								if (normalRequest != UINT32_MAX)
								{
									requests[normalRequest] = original.OriginalMatchRequest();
								}
								const uint32_t textureRequest = textureChannel ? requestCount++ : UINT32_MAX;
								if (textureRequest != UINT32_MAX)
								{
									requests[textureRequest] = { textureRadius, original.MatchSlack() };
								}
								float nearestSquared = kUnbounded;
								matchPool.clear();
								originalBvh.GatherNearJoint(point, requests, requestCount, matchPool, gathers, &nearestSquared);
								outside = std::max(outside, nearestSquared);
								if (textureRequest != UINT32_MAX)
								{
									SimplifyTriangleBvh::NearGather kept = gathers[textureRequest];
									if (kept.count != 0u &&
										textureCandidatesKept.fetch_add(kept.count, std::memory_order_relaxed) + kept.count > kMaxTextureCandidatesKept)
									{
										kept.first = SimplifyTriangleBvh::kNotGathered;
									}
									else
									{
										const auto from = matchPool.begin() + kept.first;
										kept.first = static_cast<uint32_t>(texturePool.size());
										texturePool.insert(texturePool.end(), from, from + kept.count);
									}
									textureGathers[triangleIndex * kSamples.size() + sampleIndex] = kept;
								}
								if (!normalSample)
								{
									continue;
								}
								const float faceUnit[3] = { face[0] / twiceArea, face[1] / twiceArea, face[2] / twiceArea };
								float coarse[3];
								for (int axis = 0; axis < 3; ++axis)
								{
									coarse[axis] = triangle.normal[0][axis] * weights[0] + triangle.normal[1][axis] * weights[1] + triangle.normal[2][axis] * weights[2];
								}
								++localSamples;
								float source[3];
								float facing = 0.0f;
								if (normalRequest == UINT32_MAX || !original.OriginalNormalAt(gathers[normalRequest], matchPool.data(), faceUnit, source, facing))
								{
									++localWithoutNormal;
									continue;
								}
								const float length = std::sqrt(Dot(coarse, coarse));
								double chordSquared = 4.0;
								if (length >= kMinInterpolatedNormalLength)
								{
									const float d[3] = { coarse[0] / length - source[0], coarse[1] / length - source[1], coarse[2] / length - source[2] };
									chordSquared = Dot(d, d);
								}
								qTerms[triangleIndex * kSamples.size() + sampleIndex] = 0.5 * static_cast<double>(twiceArea) / static_cast<double>(sampleCount) * chordSquared;
								if (chords != nullptr)
								{
									// Stage 6 step 3: the spread a pixel sees there, the source normals over a disc of half the group's error
									// (times the footprint) around the point in the triangle's plane against the drawn normal, by the 7-point
									// disc rule (this sample at weight 1/4, six at sqrt(2/3) of the radius at 1/8). Footprint 0: the point alone.
									const double sampleCap = normalSpreadSampleCap > 0.0f ? static_cast<double>(normalSpreadSampleCap) : 4.0;
									double spreadSum = 0.25 * std::min(chordSquared, sampleCap), spreadWeight = 0.25;
									if (spreadRadius > 0.0f)
									{
										const float* t = ab;
										const float tLength = std::sqrt(Dot(t, t));
										if (tLength > 0.0f)
										{
											const float tangent[3] = { t[0] / tLength, t[1] / tLength, t[2] / tLength };
											const float bitangent[3] = { faceUnit[1] * tangent[2] - faceUnit[2] * tangent[1], faceUnit[2] * tangent[0] - faceUnit[0] * tangent[2],
												faceUnit[0] * tangent[1] - faceUnit[1] * tangent[0] };
											const float ring = spreadRadius * 0.81649658f;
											for (int k = 0; k < 6; ++k)
											{
												const float c = kRingCos[k] * ring, sn = kRingSin[k] * ring;
												const float at[3] = { point[0] + tangent[0] * c + bitangent[0] * sn, point[1] + tangent[1] * c + bitangent[1] * sn,
													point[2] + tangent[2] * c + bitangent[2] * sn };
												float ringSource[3];
												float ringFacing = 0.0f;
												if (!original.OriginalNormalAt(at, faceUnit, ringSource, ringFacing))
												{
													continue;
												}
												double ringChord = 4.0;
												if (length >= kMinInterpolatedNormalLength)
												{
													const float d[3] = { coarse[0] / length - ringSource[0], coarse[1] / length - ringSource[1], coarse[2] / length - ringSource[2] };
													ringChord = Dot(d, d);
												}
												spreadSum += 0.125 * std::min(ringChord, sampleCap);
												spreadWeight += 0.125;
											}
										}
									}
									chords[sampleIndex] = static_cast<float>(spreadSum / spreadWeight);
								}
							}
						}
					};
					if (chunks == 1u)
					{
						measureRange(0u);
					}
					else
					{
						// Isolated: this thread, waiting for the ranges, takes only their tasks.
						oneapi::tbb::this_task_arena::isolate([&] {
							TaskSchedulerManager::GetInstance().ParallelFor("ClusterLODErrorChannels::MeasureGroup", chunks, measureRange);
						});
					}
					float outside = 0.0f;
					double q = 0.0;
					uint64_t localSamples = 0, localWithoutNormal = 0;
					for (const double term : qTerms)
					{
						q += term;
					}
					// One pool for the texture pass: each range's gathers move by the pools before it.
					std::vector<SimplifyTriangleBvh::NearCandidate> matchPool;
					if (textureChannel)
					{
						size_t keptTotal = 0;
						for (const MeasureRange& range : ranges)
						{
							keptTotal += range.pool.size();
						}
						matchPool.reserve(keptTotal);
					}
					for (size_t chunk = 0; chunk < chunks; ++chunk)
					{
						MeasureRange& range = ranges[chunk];
						outside = std::max(outside, range.outside);
						localSamples += range.samples;
						localWithoutNormal += range.withoutNormal;
						if (textureChannel && !matchPool.empty())
						{
							const size_t endTriangle = std::min((chunk + 1u) * kMeasureChunkTriangles, triangles.size());
							for (size_t slot = chunk * kMeasureChunkTriangles * kSamples.size(); slot < endTriangle * kSamples.size(); ++slot)
							{
								if (textureGathers[slot].first != SimplifyTriangleBvh::kNotGathered)
								{
									textureGathers[slot].first += static_cast<uint32_t>(matchPool.size());
								}
							}
						}
						matchPool.insert(matchPool.end(), range.pool.begin(), range.pool.end());
						std::vector<SimplifyTriangleBvh::NearCandidate>().swap(range.pool);
					}
					outsideDeviation[group] = std::sqrt(outside);
					normalQ[group] = q;


					// Stage 6: the area of S(g) whose texture comes from the wrong place, against the original's UV set 0.
					if (textureChannel)
					{
						const auto textureStart = std::chrono::steady_clock::now();
						const QueryStatsScope queryScope(QuerySite::Texture);
						std::vector<float> flatPositions;
						std::vector<float> flatUvs;
						std::vector<uint32_t> flatCharts;
						std::vector<uint32_t> flatIndices;
						// Stage 6 step 2: each flat triangle's cluster (its position in outputs[group]) and index in it.
						std::vector<std::pair<uint32_t, uint32_t>> flatOwners;
						bool complete = true;
						for (uint32_t output = 0; output < outputs[group].size(); ++output)
						{
							const auto [holder, meshlet] = outputs[group][output];
							const std::vector<std::byte>& vertices = state.groupVertexChunks[holder];
							const std::vector<uint32_t>& ids = state.groupVertexGlobalIdChunks[holder];
							ForEachMeshletTriangleIndexed(state, holder, meshlet, [&](const uint32_t* corners, uint32_t triangleInMeshlet) {
								for (uint32_t corner = 0; corner < 3u; ++corner)
								{
									const uint32_t local = corners[corner];
									const uint32_t id = local < ids.size() ? ids[local] : kNoOwner;
									if (id >= texture->buildVertexCount || id >= texture->buildCharts->size())
									{
										complete = false;
										return;
									}
								}
								for (uint32_t corner = 0; corner < 3u; ++corner)
								{
									const uint32_t local = corners[corner];
									const uint32_t id = ids[local];
									float p[3];
									ReadFloat3(vertices, vertexStrideBytes, local, MeshVertexLayout::PositionOffset, p);
									float uv[2];
									std::memcpy(uv, reinterpret_cast<const std::byte*>(texture->buildUvs) + static_cast<size_t>(id) * texture->buildUvStride, sizeof(uv));
									flatIndices.push_back(static_cast<uint32_t>(flatCharts.size()));
									flatPositions.insert(flatPositions.end(), p, p + 3);
									flatUvs.insert(flatUvs.end(), uv, uv + 2);
									flatCharts.push_back((*texture->buildCharts)[id]);
								}
								flatOwners.push_back({ output, triangleInMeshlet });
							});
						}
						if (complete && !flatIndices.empty())
						{
							SimplifySurfaceView view{};
							view.positions = flatPositions.data();
							view.positionStride = sizeof(float) * 3u;
							view.indices = flatIndices.data();
							view.indexCount = flatIndices.size();
							// The validator's radius floor: 1e-4 of the original's diagonal.
							const float radius = std::max(state.groups[group].bounds.error, original.Diagonal() * 1.0e-4f);
							TextureCheckOptions options;
							options.slideTolerance = radius * std::max(texture->slide, 0.0f);
							options.samplesPerTriangle = sampleCount;
							options.outputCharts = flatCharts.data();
							// The flat triangles are S(g)'s in the order measured above, which gathered their matches at this radius.
							if (radius == textureRadius && flatIndices.size() / 3u == triangles.size())
							{
								options.matchGathers = textureGathers.data();
								options.matchPool = matchPool.data();
								options.matchGatherCount = textureGathers.size();
							}
							std::function<float(const TextureSampleQuery&)> colorWeight;
							if (texture->color != nullptr)
							{
								colorWeight = [&](const TextureSampleQuery& query) {
									const float weight = ColorSampleWeight(*texture->color, query, radius * std::max(texture->color->footprint, 0.0f));
									// Diagnostics: dE bands of the weighed samples (the weight is dE / reference below 1).
									const float reference = texture->color->referenceDeltaE;
									const float deltaE = weight * reference;
									const size_t band = !query.matched ? 4u : weight >= 1.0f ? 3u : deltaE < 1.0f ? 0u : deltaE < 3.0f ? 1u : 2u;
									textureColorSamples[band].fetch_add(1u, std::memory_order_relaxed);
									return weight;
								};
								options.sampleWeight = &colorWeight;
								options.weighEverySample = texture->color->everySample;
							}
							const TextureDisplacement displacement = MeasureTextureDisplacement(original, texture->source, view, flatUvs.data(), radius, options);
							textureQ[group] = texture->color != nullptr ? displacement.weightedDisplacedArea : displacement.displacedArea;
							textureDisplaced[group] = displacement.displacedArea;
							textureMeasured[group] = displacement.area;

							// Stage 6 step 2: a damaged group gets remap blocks, and its texture damage is what is left through them.
							if (remapChannel && displacement.area > 0.0 &&
								displacement.displacedArea >= static_cast<double>(texture->remap->minDisplacedShare) * displacement.area)
							{
								if (texture->remap->settings.layout >= 2u)
								{
									// Layout 2: built once every group's damage is known, so page space goes only to clusters that take blocks.
									auto stash = std::make_unique<DeferredRemap>();
									stash->radius = radius;
									stash->flatPositions = flatPositions;
									stash->flatUvs = flatUvs;
									stash->flatCharts = flatCharts;
									stash->flatIndices = flatIndices;
									stash->flatOwners = flatOwners;
									stash->displacement = displacement;
									deferredRemaps[group] = std::move(stash);
								}
								else
								{
									buildGroupRemaps(group, radius, flatPositions, flatUvs, flatCharts, flatIndices, flatOwners, view, options, displacement, remapBudgets);
								}
							}
						}
						textureNanoseconds.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
							std::chrono::steady_clock::now() - textureStart).count()), std::memory_order_relaxed);
					}
					samples.fetch_add(localSamples, std::memory_order_relaxed);
					samplesWithoutNormal.fetch_add(localWithoutNormal, std::memory_order_relaxed);

					// The original this group owns, against S(g); each original triangle passes to the group holding the nearest
					// S(g) triangle, which is what replaces it there.
					if (owned[group].empty() || !measureChannels)
					{
						return;
					}
					std::vector<float> positions;
					std::vector<uint32_t> indices;
					positions.reserve(triangles.size() * 9u);
					indices.reserve(triangles.size() * 3u);
					for (const ChannelTriangle& triangle : triangles)
					{
						for (uint32_t corner = 0; corner < 3u; ++corner)
						{
							indices.push_back(static_cast<uint32_t>(positions.size() / 3u));
							positions.insert(positions.end(), triangle.position[corner], triangle.position[corner] + 3);
						}
					}
					SimplifySurfaceView view{};
					view.positions = positions.data();
					view.positionStride = sizeof(float) * 3u;
					view.indices = indices.data();
					view.indexCount = indices.size();
					SimplifyTriangleBvh bvh;
					bvh.Build(view);
					// Each owned triangle is queried alone (the largest distance does not depend on the order), in ranges as tasks
					// when there are many: a group near the root can own most of the original.
					const std::vector<uint32_t>& groupOwned = owned[group];
					const size_t ownedChunks = (groupOwned.size() + kOwnedChunkTriangles - 1u) / kOwnedChunkTriangles;
					std::vector<float> chunkInside(std::max<size_t>(ownedChunks, 1u), 0.0f);
					const auto followRange = [&](size_t chunk) {
						float inside = 0.0f;
						const size_t end = std::min((chunk + 1u) * kOwnedChunkTriangles, groupOwned.size());
						for (size_t k = chunk * kOwnedChunkTriangles; k < end; ++k)
						{
							const uint32_t sample = groupOwned[k];
							uint32_t nearest = kNoOwner;
							inside = std::max(inside, bvh.NearestDistanceSquared(&sourceCentroids[static_cast<size_t>(sample) * 3u], kUnbounded, &nearest));
							owners[sample] = nearest < triangles.size() ? triangles[nearest].group : kNoOwner;
						}
						chunkInside[chunk] = inside;
					};
					if (ownedChunks <= 1u)
					{
						followRange(0u);
					}
					else
					{
						oneapi::tbb::this_task_arena::isolate([&] {
							TaskSchedulerManager::GetInstance().ParallelFor("ClusterLODErrorChannels::FollowOwned", ownedChunks, followRange);
						});
					}
					float inside = 0.0f;
					for (const float value : chunkInside)
					{
						inside = std::max(inside, value);
					}
					sourceDeviation[group] = std::sqrt(inside);
				});
		}

		// Layout 2: page space shared by triangles among the clusters that take blocks (those refining into a damaged group), then each
		// damaged group's blocks and residual, as layout 1 builds them inside the depth loop.
		if (remapChannel && texture->remap->settings.layout >= 2u)
		{
			std::vector<std::vector<uint32_t>> budgets(groupCount);
			for (uint32_t group = 0; group < groupCount; ++group)
			{
				const std::vector<meshopt_Meshlet>& meshlets = state.groupMeshletChunks[group];
				const std::vector<int32_t>& tags = state.groupMeshletRefinedGroupChunks[group];
				budgets[group].assign(meshlets.size(), 0u);
				const auto takesBlocks = [&](uint32_t m) {
					return m < tags.size() && tags[m] >= 0 && static_cast<uint32_t>(tags[m]) < groupCount && deferredRemaps[static_cast<uint32_t>(tags[m])] != nullptr;
				};
				uint32_t first = 0u;
				for (const std::vector<std::byte>& page : state.groupPageBlobs[group])
				{
					uint32_t count = 0u;
					if (page.size() >= sizeof(CLodPageHeader))
					{
						std::memcpy(&count, page.data() + offsetof(CLodPageHeader, meshletCount), sizeof(count));
					}
					uint64_t pageTriangles = 0u, spreadReserve = 0u;
					for (uint32_t m = first; m < first + count && m < meshlets.size(); ++m)
					{
						pageTriangles += takesBlocks(m) ? meshlets[m].triangle_count : 0u;
						if (normalSpread && hasNormals)
						{
							spreadReserve += m < tags.size() && tags[m] >= 0 ? clod_normal_spread::BlockSize(meshlets[m].vertex_count) : 0u;
						}
					}
					const uint64_t used = page.size() + 256u + spreadReserve;
					const uint64_t free = used < CLOD_STREAMING_PAGE_SIZE_BYTES ? CLOD_STREAMING_PAGE_SIZE_BYTES - used : 0u;
					for (uint32_t m = first; m < first + count && m < meshlets.size(); ++m)
					{
						budgets[group][m] = pageTriangles != 0u && takesBlocks(m)
							? static_cast<uint32_t>((free * meshlets[m].triangle_count / pageTriangles) & ~3ull) : 0u;
					}
					first += count;
				}
			}
			std::vector<uint32_t> damaged;
			for (uint32_t group = 0; group < groupCount; ++group)
			{
				if (deferredRemaps[group] != nullptr)
				{
					damaged.push_back(group);
				}
			}
			TaskSchedulerManager::GetInstance().ParallelFor("ClusterLODErrorChannels::UvRemapRegions", damaged.size(), [&](size_t taskIndex)
				{
					const uint32_t group = damaged[taskIndex];
					const DeferredRemap& stash = *deferredRemaps[group];
					const float radius = stash.radius;
					SimplifySurfaceView view{};
					view.positions = stash.flatPositions.data();
					view.positionStride = sizeof(float) * 3u;
					view.indices = stash.flatIndices.data();
					view.indexCount = stash.flatIndices.size();
					TextureCheckOptions options;
					options.slideTolerance = radius * std::max(texture->slide, 0.0f);
					options.outputCharts = stash.flatCharts.data();
					std::function<float(const TextureSampleQuery&)> colorWeight;
					if (texture->color != nullptr)
					{
						colorWeight = [&](const TextureSampleQuery& query) {
							return ColorSampleWeight(*texture->color, query, radius * std::max(texture->color->footprint, 0.0f));
						};
						options.sampleWeight = &colorWeight;
						options.weighEverySample = texture->color->everySample;
					}
					buildGroupRemaps(group, radius, stash.flatPositions, stash.flatUvs, stash.flatCharts, stash.flatIndices, stash.flatOwners, view, options,
						stash.displacement, budgets);
				});
			std::vector<std::unique_ptr<DeferredRemap>>().swap(deferredRemaps);
		}

		// Stage 6 step 3: per holding group, each vertex's hat-weighted mean squared chord over the simplified triangles around it,
		// then one block per simplified meshlet. Groups write only their own slots, and sums run in a fixed order.
		if (spreadChannel)
		{
			const auto spreadStart = std::chrono::steady_clock::now();
			state.meshletNormalSpreads.resize(groupCount);
			std::vector<double> groupArea(groupCount, 0.0), groupQ(groupCount, 0.0);
			std::vector<std::array<uint64_t, 7>> groupCounts(groupCount, std::array<uint64_t, 7>{}); // clusters, vertices, bytes, then vertices by spreadVertexBands
			TaskSchedulerManager::GetInstance().ParallelFor("ClusterLODErrorChannels::NormalSpread", groupCount, [&](size_t taskIndex)
				{
					const uint32_t holder = static_cast<uint32_t>(taskIndex);
					const std::vector<meshopt_Meshlet>& meshlets = state.groupMeshletChunks[holder];
					const std::vector<uint32_t>& meshletVertices = state.groupMeshletVertexChunks[holder];
					const std::vector<std::byte>& vertices = state.groupVertexChunks[holder];
					const size_t vertexCount = vertexStrideBytes != 0u ? vertices.size() / vertexStrideBytes : 0u;
					std::vector<double> weightSum(vertexCount, 0.0), chordSum(vertexCount, 0.0);
					bool any = false;
					for (uint32_t meshlet = 0; meshlet < meshlets.size(); ++meshlet)
					{
						const std::vector<float>& chords = spreadChords[holder][meshlet];
						if (chords.empty())
						{
							continue;
						}
						ForEachMeshletTriangleIndexed(state, holder, meshlet, [&](const uint32_t* corners, uint32_t triangleInMeshlet) {
							float p[3][3];
							for (uint32_t corner = 0; corner < 3u; ++corner)
							{
								if (corners[corner] >= vertexCount)
								{
									return;
								}
								ReadFloat3(vertices, vertexStrideBytes, corners[corner], MeshVertexLayout::PositionOffset, p[corner]);
							}
							const float ab[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
							const float ac[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
							const float face[3] = { ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0] };
							const double sampleArea = 0.5 * static_cast<double>(std::sqrt(Dot(face, face))) / static_cast<double>(kSamples.size());
							if (!(sampleArea > 0.0))
							{
								return;
							}
							for (size_t sampleIndex = 0; sampleIndex < kSamples.size(); ++sampleIndex)
							{
								const size_t slot = static_cast<size_t>(triangleInMeshlet) * kSamples.size() + sampleIndex;
								const float chord = slot < chords.size() ? chords[slot] : -1.0f;
								if (!(chord >= 0.0f))
								{
									continue;
								}
								groupArea[holder] += sampleArea;
								groupQ[holder] += sampleArea * chord;
								for (uint32_t corner = 0; corner < 3u; ++corner)
								{
									const double w = sampleArea * static_cast<double>(kSamples[sampleIndex][corner]);
									weightSum[corners[corner]] += w;
									chordSum[corners[corner]] += w * static_cast<double>(chord);
								}
								any = true;
							}
						});
					}
					if (!any)
					{
						return;
					}
					std::vector<std::vector<std::byte>>& blocks = state.meshletNormalSpreads[holder];
					blocks.resize(meshlets.size());
					std::vector<uint8_t> bytes;
					for (uint32_t meshlet = 0; meshlet < meshlets.size(); ++meshlet)
					{
						if (spreadChords[holder][meshlet].empty())
						{
							continue;
						}
						const meshopt_Meshlet& m = meshlets[meshlet];
						bytes.assign(m.vertex_count, 0u);
						for (uint32_t local = 0; local < m.vertex_count; ++local)
						{
							const size_t at = static_cast<size_t>(m.vertex_offset) + local;
							const uint32_t vertex = at < meshletVertices.size() ? meshletVertices[at] : std::numeric_limits<uint32_t>::max();
							if (vertex < vertexCount && weightSum[vertex] > 0.0)
							{
								bytes[local] = clod_normal_spread::Encode(chordSum[vertex] / weightSum[vertex]);
							}
						}
						blocks[meshlet] = clod_normal_spread::EncodeBlock(bytes);
						if (!blocks[meshlet].empty())
						{
							++groupCounts[holder][0];
							groupCounts[holder][1] += m.vertex_count;
							groupCounts[holder][2] += blocks[meshlet].size();
							for (const uint8_t b : bytes)
							{
								++groupCounts[holder][b == 0u ? 3u : b < 16u ? 4u : b < 64u ? 5u : 6u];
							}
						}
					}
				});
			for (uint32_t group = 0; group < groupCount; ++group)
			{
				stats.spreadArea += groupArea[group];
				stats.spreadQ += groupQ[group];
				stats.spreadClusters += static_cast<uint32_t>(groupCounts[group][0]);
				stats.spreadVertices += groupCounts[group][1];
				stats.spreadBytes += groupCounts[group][2];
				for (size_t band = 0; band < 4u; ++band)
				{
					stats.spreadVertexBands[band] += groupCounts[group][3u + band];
				}
			}
			stats.spreadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - spreadStart).count();
		}

		// Monotone through the DAG, children first: a group's channels are at least its children's (the groups its own clusters
		// refine into), so every segment's enter <= leave under any combination the runtime selects.
		std::vector<uint32_t> order(groupCount);
		for (uint32_t group = 0; group < groupCount; ++group)
		{
			order[group] = group;
		}
		std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return state.groups[a].depth < state.groups[b].depth; });
		for (uint32_t group : order)
		{
			ClusterLODGroupErrorChannels& channels = state.errorChannels[group];
			const bool terminal = outputs[group].empty();
			const float ownGeometric = std::max(outsideDeviation[group], sourceDeviation[group]);
			const double ownQ = normalQ[group] + (textureChannel ? static_cast<double>(texture->weight) * textureQ[group] : 0.0);
			const float ownAppearance = static_cast<float>(std::sqrt(ownQ));
			channels.geometric = terminal ? std::numeric_limits<float>::max() : ownGeometric;
			channels.appearance = terminal ? 0.0f : ownAppearance;
			for (const int32_t child : state.groupMeshletRefinedGroupChunks[group])
			{
				if (child < 0 || static_cast<uint32_t>(child) >= groupCount)
				{
					continue;
				}
				channels.geometric = std::max(channels.geometric, state.errorChannels[child].geometric);
				channels.appearance = std::max(channels.appearance, state.errorChannels[child].appearance);
			}
			if (terminal)
			{
				continue;
			}
			++stats.groups;
			stats.textureMeasuredArea += textureMeasured[group];
			stats.textureDisplacedArea += textureDisplaced[group];
			stats.textureWeightedArea += textureQ[group];
			stats.textureGroupsDisplaced += textureDisplaced[group] > 0.0 ? 1u : 0u;
			if (remapped[group] != 0u)
			{
				++stats.remapGroups;
				stats.remapDisplacedBefore += remapDisplacedBefore[group];
				stats.remapDisplacedAfter += textureDisplaced[group];
				stats.remapQBefore += remapQBefore[group];
				stats.remapQAfter += textureQ[group];
				stats.remapSlideBefore += remapParts[group][0];
				stats.remapSlideAfter += remapParts[group][1];
				stats.remapChartBefore += remapParts[group][2];
				stats.remapChartAfter += remapParts[group][3];
				stats.remapMixedBefore += remapParts[group][4];
				stats.remapMixedAfter += remapParts[group][5];
			}
			stats.envelopeRaisedGeometric += channels.geometric > ownGeometric ? 1u : 0u;
			stats.envelopeRaisedAppearance += channels.appearance > ownAppearance ? 1u : 0u;
			stats.raisedOverQuadric += channels.geometric > state.groups[group].bounds.error ? 1u : 0u;
		}

		// Diagnostics only (not hashed): one line per group with its own measurements and the monotone channels.
		if (!GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_ERROR_CHANNELS_LOG").empty())
		{
			for (uint32_t group = 0; group < groupCount; ++group)
			{
				spdlog::info(
					"ClusterLOD error channel: group={} depth={} output_meshlets={} quadric={:.6g} outside={:.6g} source={:.6g} q={:.6g} texture_q={:.6g} geometric={:.6g} appearance={:.6g}{}",
					group, state.groups[group].depth, outputs[group].size(), state.groups[group].bounds.error, outsideDeviation[group],
					sourceDeviation[group], normalQ[group], textureQ[group], state.errorChannels[group].geometric, state.errorChannels[group].appearance,
					remapped[group] != 0u ? std::format(" remapped texture_q_before={:.6g}", remapQBefore[group]) : std::string());
			}
		}

stats.textureMs = static_cast<double>(textureNanoseconds.load()) * 1.0e-6;
		stats.remapClusters = static_cast<uint32_t>(remapClusters.load());
		stats.remapTriangles = remapTriangles.load();
		stats.remapNodes = remapNodes.load();
		stats.remapNodesUnmatched = remapNodesUnmatched.load();
		stats.remapNodesAlongNormal = remapNodesAlongNormal.load();
		stats.remapBytes = remapBytes.load();
		stats.remapTrianglesOverBudget = remapTrianglesOverBudget.load();
		stats.remapSubTriangles = remapSubTriangles.load();
		stats.remapSubTrianglesCharts = remapSubTrianglesCharts.load();
		stats.remapSubTrianglesSeams = remapSubTrianglesSeams.load();
		stats.remapMs = static_cast<double>(remapNanoseconds.load()) * 1.0e-6;
		stats.remapTrianglesRegions = remapTrianglesRegions.load();
		stats.remapTrianglesGrid = remapTrianglesGrid.load();
		stats.remapRegions = remapRegions.load();
		stats.remapRegionCells = remapRegionCells.load();
		stats.remapPointsUnlabeled = remapPointsUnlabeled.load();
		stats.remapRegionsFitted = remapRegionsFitted.load();
		stats.remapTrianglesRegionsRejected = remapTrianglesRegionsRejected.load();
		stats.remapRegionsPlanarPieces = remapRegionsPlanarPieces.load();
		for (size_t k = 0; k < remapRegionGridCounts.size(); ++k)
		{
			stats.remapRegionGridCounts[k] = remapRegionGridCounts[k].load();
		}
		for (const std::array<double, 2>& areas : remapRegionAreas)
		{
			stats.remapRegionArea += areas[0];
			stats.remapRegionMissArea += areas[1];
		}
		for (size_t band = 0; band < textureColorSamples.size(); ++band)
		{
			stats.textureColorSamples[band] = textureColorSamples[band].load();
		}
		stats.samples = samples.load();
		stats.samplesWithoutNormal = samplesWithoutNormal.load();
		stats.simplifiedTriangles = simplifiedTriangles.load();
		stats.measureMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		return stats;
	}

}
