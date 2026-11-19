#include <BasicRenderer/Assets/Import/CLodCache.h>
#include <BasicRenderer/Assets/Import/ClusterLODUtilities.h>
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODVoxelPacking.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODPagePackingTelemetry.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODBuildState.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODVoxelFallback.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODHierarchy.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODSkinningSidecar.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODSimplifyValidation.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODReplacementVertices.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODOptimalSimplify.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODErrorChannels.h"
#include <BasicRenderer/Assets/ClusterLODUvRemap.h>
#include <BasicRenderer/Assets/ClusterLODNormalSpread.h>
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODTopology.h"

#include <limits>
#include <memory>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <bit>
#include <cmath>
#include <array>
#include <functional>
#include <cstring>
#include <iterator>
#include <mutex>
#include <atomic>
#include <cassert>
#include <stdexcept>
#include <numeric>
#include <optional>
#include <span>
#include <chrono>
#include <string>
#include <format>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>

#include <oneapi/tbb/task_arena.h>
#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

#include <BasicRenderer/Streaming/TaskScheduler.h>
#include "BasicRenderer/Assets/Geometry/VertexLayout.h"
#include <BasicRenderer/Scene/VertexFlags.h>
#include <BasicRenderer/Assets/SGGX.h>
#include "BasicRenderer/Assets/Import/VoxelGroupBuilder.h"
#include <meshoptimizer.h>

#include "../shaders/Common/defines.h"

namespace
{
	using clod_detail::ClusterLODBuildState;
	using clod_detail::AppendBits;
	using clod_detail::ComputePageBlobSize;
	using clod_detail::CLOD_STREAMING_PAGE_SIZE_BYTES;
	using clod_detail::CLOD_VOXEL_ATTRIBUTE_SAMPLES_COMPACT;
	using clod_detail::CLOD_NATIVE_POSITION_FORMAT;
	using clod_detail::CLOD_NATIVE_POSITION_STRIDE_BYTES;
	using clod_detail::FinalizeMeshWidePagePacking;
	using clod_detail::BuildVoxelFallbackCandidates;
	using clod_detail::BuildClusterLODTraversalHierarchy;
	using clod_detail::ComputeCLodTraversalDepth;
	using clod_detail::BuildObjectBoundingSphereFromRootNode;
	using clod_detail::AssignSingleRootPartRecord;
	using clod_detail::NodeBoneSet;
	using clod_detail::BuildNodeSkinningSidecar;
	using clod_detail::kClusterLODStructuralTraversalError;
	using clod_detail::GetVoxelCandidateExpansionRadiusForPayload;
	using clod_detail::ReadGroupVertexPosition;
	using clod_detail::ComputeGroupSegmentFirstMeshlet;
	using clod_detail::GetVoxelPackedCubeCountForGroup;
	using clod_detail::GetVoxelPackedClusterCountForGroup;
	using clod_detail::ComputeVoxelRepresentationError;
	using clod_detail::PackVoxelTailCellKey;
	using clod_detail::GetFiniteVoxelErrorForGroup;
	using clod_detail::CollectUniqueRefinedChildren;
	using clod_detail::IsTerminalErrorSentinel;
	using clod_detail::IsFiniteContentTraversalError;
	using clod_detail::TraversalNodeErrorFromGroupError;
	constexpr uint32_t CLOD_COMPRESSED_POSITIONS = 1u << 0;
	constexpr uint32_t CLOD_COMPRESSED_MESHLET_VERTEX_INDICES = 1u << 1;
	constexpr uint32_t CLOD_COMPRESSED_NORMALS = 1u << 2;
	constexpr uint32_t kMaxSkinInfluences = 8u;
	constexpr float CLOD_UV_QUANTIZATION_SCALE = 65535.0f;
	constexpr float CLOD_UV_QUANTIZATION_INV_SCALE = 1.0f / CLOD_UV_QUANTIZATION_SCALE;
	constexpr const char* OBJECT_REYES_ATLAS_HEIGHT_UV_SET_NAME = "__object_reyes_atlas_height";

	struct PackedSkinningInfluences
	{
		DirectX::XMUINT4 joints0{ 0, 0, 0, 0 };
		DirectX::XMUINT4 joints1{ 0, 0, 0, 0 };
		DirectX::XMFLOAT4 weights0{ 0, 0, 0, 0 };
		DirectX::XMFLOAT4 weights1{ 0, 0, 0, 0 };
	};

	uint32_t BitsNeededForRange(uint32_t range)
	{
		if (range == 0)
		{
			return 1;
		}
		return 32u - static_cast<uint32_t>(std::countl_zero(range));
	}

	uint32_t ReadBits(const std::vector<uint32_t>& words, uint64_t& bitCursor, uint32_t bitCount)
	{
		if (bitCount == 0) return 0;
		const uint64_t bitOffset = bitCursor & 31ull;
		const uint64_t wordIndex = bitCursor >> 5ull;
		const uint64_t mask = (bitCount >= 32u) ? 0xffffffffull : ((1ull << bitCount) - 1ull);
		uint32_t value = (words[static_cast<size_t>(wordIndex)] >> static_cast<uint32_t>(bitOffset)) & static_cast<uint32_t>(mask);
		const uint32_t spillBits = static_cast<uint32_t>(bitOffset) + bitCount;
		if (spillBits > 32u) {
			value |= (words[static_cast<size_t>(wordIndex + 1ull)] << (32u - static_cast<uint32_t>(bitOffset))) & static_cast<uint32_t>(mask);
		}
		bitCursor += bitCount;
		return value;
	}

	void AppendBitsPreSized(std::vector<uint32_t>& words, uint64_t& bitCursor, uint32_t value, uint32_t bitCount)
	{
		if (bitCount == 0)
		{
			return;
		}

		const uint64_t bitOffset = bitCursor & 31ull;
		const uint64_t wordIndex = bitCursor >> 5ull;
		const uint64_t mask = (bitCount >= 32u) ? 0xffffffffull : ((1ull << bitCount) - 1ull);
		const uint64_t clampedValue = static_cast<uint64_t>(value) & mask;
		words[static_cast<size_t>(wordIndex)] |= static_cast<uint32_t>(clampedValue << bitOffset);

		const uint32_t spillBits = static_cast<uint32_t>(bitOffset) + bitCount;
		if (spillBits > 32u)
		{
			words[static_cast<size_t>(wordIndex + 1ull)] |= static_cast<uint32_t>(clampedValue >> (32u - static_cast<uint32_t>(bitOffset)));
		}

		bitCursor += bitCount;
	}

	uint32_t ComputeMeshQuantizationExponent(const std::vector<std::byte>& vertices, size_t vertexStrideBytes)
	{
		if (vertices.empty() || vertexStrideBytes < sizeof(float) * 3)
		{
			return 10u;
		}

		DirectX::XMFLOAT3 minv{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
		DirectX::XMFLOAT3 maxv{ -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };
		const size_t vertexCount = vertices.size() / vertexStrideBytes;
		for (size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
		{
			const size_t byteOffset = vertexIndex * vertexStrideBytes;
			float px = 0.0f;
			float py = 0.0f;
			float pz = 0.0f;
			std::memcpy(&px, vertices.data() + byteOffset, sizeof(float));
			std::memcpy(&py, vertices.data() + byteOffset + sizeof(float), sizeof(float));
			std::memcpy(&pz, vertices.data() + byteOffset + sizeof(float) * 2, sizeof(float));

			minv.x = std::min(minv.x, px);
			minv.y = std::min(minv.y, py);
			minv.z = std::min(minv.z, pz);
			maxv.x = std::max(maxv.x, px);
			maxv.y = std::max(maxv.y, py);
			maxv.z = std::max(maxv.z, pz);
		}

		const float dx = maxv.x - minv.x;
		const float dy = maxv.y - minv.y;
		const float dz = maxv.z - minv.z;
		const float diagonal = std::sqrt(dx * dx + dy * dy + dz * dz);

		if (diagonal < 1.0f) return 14u;
		if (diagonal < 10.0f) return 12u;
		if (diagonal < 100.0f) return 10u;
		return 8u;
	}

	std::array<float, 2> OctEncodeNormal(DirectX::XMFLOAT3 normal)
	{
		float nx = normal.x;
		float ny = normal.y;
		float nz = normal.z;
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

		return { nx, ny };
	}

	int32_t QuantizeSnorm16(float value)
	{
		const float clamped = std::max(-1.0f, std::min(1.0f, value));
		const float scaled = std::round(clamped * 32767.0f);
		return static_cast<int32_t>(scaled);
	}

	uint32_t PackOctNormalSnorm16(const std::array<float, 2>& oct)
	{
		const uint16_t x = static_cast<uint16_t>(static_cast<int16_t>(QuantizeSnorm16(oct[0])));
		const uint16_t y = static_cast<uint16_t>(static_cast<int16_t>(QuantizeSnorm16(oct[1])));
		return static_cast<uint32_t>(x) | (static_cast<uint32_t>(y) << 16u);
	}

	DirectX::XMFLOAT3 NormalizeOrFallback(DirectX::XMFLOAT3 value, DirectX::XMFLOAT3 fallback);
	DirectX::XMFLOAT3 BuildFallbackTangentFromNormal(DirectX::XMFLOAT3 normal);

	void BuildTangentAngleBasis(DirectX::XMFLOAT3 normal, DirectX::XMFLOAT3& tangent, DirectX::XMFLOAT3& bitangent)
	{
		normal = NormalizeOrFallback(normal, DirectX::XMFLOAT3(0.0f, 0.0f, 1.0f));
		tangent = BuildFallbackTangentFromNormal(normal);
		bitangent = DirectX::XMFLOAT3(
			tangent.y * normal.z - tangent.z * normal.y,
			tangent.z * normal.x - tangent.x * normal.z,
			tangent.x * normal.y - tangent.y * normal.x);
		bitangent = NormalizeOrFallback(bitangent, DirectX::XMFLOAT3(0.0f, 1.0f, 0.0f));
	}

	uint32_t PackTangentFrameAngle(DirectX::XMFLOAT3 normal, DirectX::XMFLOAT4 tangent)
	{
		normal = NormalizeOrFallback(normal, DirectX::XMFLOAT3(0.0f, 0.0f, 1.0f));
		DirectX::XMFLOAT3 tangent3(tangent.x, tangent.y, tangent.z);
		const float projection = tangent3.x * normal.x + tangent3.y * normal.y + tangent3.z * normal.z;
		tangent3.x -= normal.x * projection;
		tangent3.y -= normal.y * projection;
		tangent3.z -= normal.z * projection;
		tangent3 = NormalizeOrFallback(tangent3, BuildFallbackTangentFromNormal(normal));

		DirectX::XMFLOAT3 basisT;
		DirectX::XMFLOAT3 basisB;
		BuildTangentAngleBasis(normal, basisT, basisB);

		const float x = tangent3.x * basisT.x + tangent3.y * basisT.y + tangent3.z * basisT.z;
		const float y = tangent3.x * basisB.x + tangent3.y * basisB.y + tangent3.z * basisB.z;
		constexpr float TwoPi = 6.2831853071795864769f;
		float angle = std::atan2(y, x);
		if (angle < 0.0f)
		{
			angle += TwoPi;
		}

		const uint32_t angleBits = static_cast<uint32_t>(std::lround(std::clamp(angle / TwoPi, 0.0f, 1.0f) * 65535.0f)) & 0xFFFFu;
		const uint32_t signBit = tangent.w < 0.0f ? (1u << 16u) : 0u;
		return angleBits | signBit;
	}

	uint32_t PackColorUnorm8(DirectX::XMFLOAT3 color)
	{
		auto quantize = [](float value) -> uint32_t {
			const float clamped = std::clamp(value, 0.0f, 1.0f);
			return static_cast<uint32_t>(std::lround(clamped * 255.0f));
		};

		const uint32_t r = quantize(color.x);
		const uint32_t g = quantize(color.y);
		const uint32_t b = quantize(color.z);
		return r | (g << 8u) | (b << 16u) | (0xFFu << 24u);
	}

	uint32_t QuantizeUvOffset(float value)
	{
		const int64_t scaled = std::llround(static_cast<double>(value) * static_cast<double>(CLOD_UV_QUANTIZATION_SCALE));
		const int64_t clamped = std::clamp<int64_t>(
			scaled,
			0,
			static_cast<int64_t>((std::numeric_limits<uint32_t>::max)()));
		return static_cast<uint32_t>(clamped);
	}

	DirectX::XMFLOAT3 ReadVertexFloat3(const std::vector<std::byte>& vertices, size_t vertexStrideBytes, uint32_t vertexIndex, size_t attributeByteOffset)
	{
		DirectX::XMFLOAT3 value{};
		const size_t byteOffset = static_cast<size_t>(vertexIndex) * vertexStrideBytes + attributeByteOffset;
		std::memcpy(&value.x, vertices.data() + byteOffset, sizeof(float));
		std::memcpy(&value.y, vertices.data() + byteOffset + sizeof(float), sizeof(float));
		std::memcpy(&value.z, vertices.data() + byteOffset + sizeof(float) * 2, sizeof(float));
		return value;
	}

	DirectX::XMFLOAT3 NormalizeOrFallback(DirectX::XMFLOAT3 value, DirectX::XMFLOAT3 fallback)
	{
		const float lenSq = value.x * value.x + value.y * value.y + value.z * value.z;
		if (lenSq <= 1e-20f)
		{
			const float fallbackLenSq = fallback.x * fallback.x + fallback.y * fallback.y + fallback.z * fallback.z;
			if (fallbackLenSq <= 1e-20f)
			{
				return DirectX::XMFLOAT3(0.0f, 0.0f, 1.0f);
			}

			const float invFallbackLen = 1.0f / std::sqrt(fallbackLenSq);
			return DirectX::XMFLOAT3(
				fallback.x * invFallbackLen,
				fallback.y * invFallbackLen,
				fallback.z * invFallbackLen);
		}

		const float invLen = 1.0f / std::sqrt(lenSq);
		return DirectX::XMFLOAT3(value.x * invLen, value.y * invLen, value.z * invLen);
	}

	DirectX::XMFLOAT2 ReadVertexFloat2(const std::vector<std::byte>& vertices, size_t vertexStrideBytes, uint32_t vertexIndex, size_t attributeByteOffset)
	{
		DirectX::XMFLOAT2 value{};
		const size_t byteOffset = static_cast<size_t>(vertexIndex) * vertexStrideBytes + attributeByteOffset;
		std::memcpy(&value.x, vertices.data() + byteOffset, sizeof(float));
		std::memcpy(&value.y, vertices.data() + byteOffset + sizeof(float), sizeof(float));
		return value;
	}

	DirectX::XMFLOAT3 BuildFallbackTangentFromNormal(DirectX::XMFLOAT3 normal)
	{
		normal = NormalizeOrFallback(normal, DirectX::XMFLOAT3(0.0f, 0.0f, 1.0f));
		DirectX::XMFLOAT3 axis = (std::abs(normal.z) < 0.999f)
			? DirectX::XMFLOAT3(0.0f, 0.0f, 1.0f)
			: DirectX::XMFLOAT3(0.0f, 1.0f, 0.0f);

		DirectX::XMFLOAT3 tangent(
			axis.y * normal.z - axis.z * normal.y,
			axis.z * normal.x - axis.x * normal.z,
			axis.x * normal.y - axis.y * normal.x);

		const float tangentLenSq = tangent.x * tangent.x + tangent.y * tangent.y + tangent.z * tangent.z;
		if (tangentLenSq <= 1e-20f)
		{
			return DirectX::XMFLOAT3(1.0f, 0.0f, 0.0f);
		}

		const float invTangentLen = 1.0f / std::sqrt(tangentLenSq);
		return DirectX::XMFLOAT3(tangent.x * invTangentLen, tangent.y * invTangentLen, tangent.z * invTangentLen);
	}

	bool GenerateMeshoptTangents(
		const std::vector<std::byte>& vertices,
		size_t vertexStrideBytes,
		const std::vector<uint32_t>& indices,
		std::vector<DirectX::XMFLOAT4>& outTangents)
	{
		constexpr size_t PositionByteOffset = MeshVertexLayout::PositionOffset;
		constexpr size_t NormalByteOffset = MeshVertexLayout::NormalOffset;
		constexpr size_t TexcoordByteOffset = MeshVertexLayout::TexcoordOffset(VertexFlags::VERTEX_TEXCOORDS);

		if (vertexStrideBytes < (TexcoordByteOffset + sizeof(float) * 2) || indices.empty() || (indices.size() % 3ull) != 0ull)
		{
			return false;
		}

		const size_t vertexCount = vertices.size() / vertexStrideBytes;
		if (vertexCount == 0)
		{
			return false;
		}

		for (uint32_t index : indices)
		{
			if (static_cast<size_t>(index) >= vertexCount)
			{
				return false;
			}
		}

		std::vector<float> cornerTangents(indices.size() * 4ull);
		const std::byte* vertexData = vertices.data();
		meshopt_generateTangents(
			cornerTangents.data(),
			indices.data(), indices.size(),
			reinterpret_cast<const float*>(vertexData + PositionByteOffset), vertexCount, vertexStrideBytes,
			reinterpret_cast<const float*>(vertexData + NormalByteOffset), vertexStrideBytes,
			reinterpret_cast<const float*>(vertexData + TexcoordByteOffset), vertexStrideBytes,
			0);

		std::vector<DirectX::XMFLOAT3> accumulatedTangents(vertexCount, DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f));
		std::vector<float> accumulatedSigns(vertexCount, 0.0f);
		std::vector<uint32_t> accumulatedContributions(vertexCount, 0u);
		for (size_t cornerIndex = 0; cornerIndex < indices.size(); ++cornerIndex)
		{
			const uint32_t vertexIndex = indices[cornerIndex];
			DirectX::XMFLOAT3& accumulated = accumulatedTangents[vertexIndex];
			accumulated.x += cornerTangents[cornerIndex * 4ull + 0ull];
			accumulated.y += cornerTangents[cornerIndex * 4ull + 1ull];
			accumulated.z += cornerTangents[cornerIndex * 4ull + 2ull];
			accumulatedSigns[vertexIndex] += cornerTangents[cornerIndex * 4ull + 3ull];
			accumulatedContributions[vertexIndex] += 1u;
		}

		outTangents.resize(vertexCount);
		for (size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
		{
			DirectX::XMFLOAT3 tangent = accumulatedTangents[vertexIndex];
			const float tangentLenSq = tangent.x * tangent.x + tangent.y * tangent.y + tangent.z * tangent.z;

			if (accumulatedContributions[vertexIndex] == 0u || tangentLenSq <= 1e-20f ||
				!std::isfinite(tangent.x) || !std::isfinite(tangent.y) || !std::isfinite(tangent.z))
			{
				const DirectX::XMFLOAT3 normal = ReadVertexFloat3(vertices, vertexStrideBytes, static_cast<uint32_t>(vertexIndex), NormalByteOffset);
				tangent = BuildFallbackTangentFromNormal(normal);
			}
			else
			{
				const float invLen = 1.0f / std::sqrt(tangentLenSq);
				tangent.x *= invLen;
				tangent.y *= invLen;
				tangent.z *= invLen;
			}

			const float sign = accumulatedSigns[vertexIndex] < 0.0f ? -1.0f : 1.0f;
			outTangents[vertexIndex] = DirectX::XMFLOAT4(tangent.x, tangent.y, tangent.z, sign);
		}

		return true;
	}

	std::vector<unsigned char> BuildExteriorEdgePriorityFlags(
		const std::vector<std::byte>& vertices,
		size_t vertexStrideBytes,
		const std::vector<uint32_t>& indices)
	{
		ZoneScopedN("ClusterLODUtilities::BuildExteriorEdgePriorityFlags");
		const size_t vertexCount = vertexStrideBytes == 0 ? 0 : vertices.size() / vertexStrideBytes;
		if (vertexCount == 0 || indices.empty() || (indices.size() % 3u) != 0u)
			return {};

		// Overwrite-only scratch is allocated without zero-filling.
		std::unique_ptr<unsigned int[]> positionRemap(new unsigned int[vertexCount]);
		meshopt_generatePositionRemap(
			positionRemap.get(), reinterpret_cast<const float*>(vertices.data()), vertexCount, vertexStrideBytes);

		// An exterior edge is one only a single triangle has. Edges are bucketed by their lower canonical vertex (a counting
		// sort: two linear passes) and compared within each bucket, which holds a vertex's few edges, instead of sorting
		// every edge of the mesh together.
		auto canonicalEdge = [&](uint32_t sourceA, uint32_t sourceB, uint32_t& lo, uint32_t& hi) -> bool
		{
			if (sourceA >= vertexCount || sourceB >= vertexCount)
				return false;
			const uint32_t a = positionRemap[sourceA];
			const uint32_t b = positionRemap[sourceB];
			if (a == b)
				return false;
			lo = std::min(a, b);
			hi = std::max(a, b);
			return true;
		};
		auto forEachEdge = [&](auto&& visit)
		{
			for (size_t triangle = 0; triangle < indices.size(); triangle += 3u)
			{
				uint32_t lo = 0;
				uint32_t hi = 0;
				if (canonicalEdge(indices[triangle + 0u], indices[triangle + 1u], lo, hi)) visit(lo, hi);
				if (canonicalEdge(indices[triangle + 1u], indices[triangle + 2u], lo, hi)) visit(lo, hi);
				if (canonicalEdge(indices[triangle + 2u], indices[triangle + 0u], lo, hi)) visit(lo, hi);
			}
		};

		std::vector<uint32_t> bucketStart(vertexCount + 1u, 0u);
		forEachEdge([&](uint32_t lo, uint32_t) { ++bucketStart[lo + 1u]; });
		for (size_t vertex = 0; vertex < vertexCount; ++vertex)
			bucketStart[vertex + 1u] += bucketStart[vertex];
		std::vector<uint32_t> bucketCursor(bucketStart.begin(), bucketStart.end() - 1);
		std::unique_ptr<uint32_t[]> upperVertices(new uint32_t[bucketStart[vertexCount]]);
		forEachEdge([&](uint32_t lo, uint32_t hi) { upperVertices[bucketCursor[lo]++] = hi; });
		bucketCursor = {};

		std::vector<unsigned char> canonicalBoundary(vertexCount, 0u);
		size_t exteriorEdgeCount = 0;
		for (size_t lo = 0; lo < vertexCount; ++lo)
		{
			uint32_t* const first = upperVertices.get() + bucketStart[lo];
			uint32_t* const last = upperVertices.get() + bucketStart[lo + 1u];
			if (first == last)
				continue;
			std::sort(first, last);
			for (uint32_t* begin = first; begin != last;)
			{
				uint32_t* end = begin + 1;
				while (end != last && *end == *begin)
					++end;
				if (end - begin == 1)
				{
					canonicalBoundary[lo] = 1u;
					canonicalBoundary[*begin] = 1u;
					++exteriorEdgeCount;
				}
				begin = end;
			}
		}

		std::vector<unsigned char> flags(vertexCount, 0u);
		size_t priorityVertexCount = 0;
		for (size_t vertex = 0; vertex < vertexCount; ++vertex)
		{
			if (canonicalBoundary[positionRemap[vertex]] != 0u)
			{
				flags[vertex] = meshopt_SimplifyVertex_Priority;
				++priorityVertexCount;
			}
		}

		TracyPlot("CLOD.Coverage.ExteriorEdges", static_cast<int64_t>(exteriorEdgeCount));
		TracyPlot("CLOD.Coverage.PriorityVertices", static_cast<int64_t>(priorityVertexCount));
		spdlog::debug("ClusterLOD coverage preservation: exterior_edges={} priority_vertices={} vertices={} triangles={}",
			exteriorEdgeCount, priorityVertexCount, vertexCount, indices.size() / 3u);
		return flags;
	}

	struct VertexPositionKey
	{
		uint32_t x;
		uint32_t y;
		uint32_t z;

		bool operator==(const VertexPositionKey&) const = default;
	};

	struct VertexPositionKeyHash
	{
		size_t operator()(const VertexPositionKey& key) const noexcept
		{
			size_t h = static_cast<size_t>(key.x);
			h ^= static_cast<size_t>(key.y) + 0x9e3779b97f4a7c15ull + (h << 6u) + (h >> 2u);
			h ^= static_cast<size_t>(key.z) + 0x9e3779b97f4a7c15ull + (h << 6u) + (h >> 2u);
			return h;
		}
	};

	uint32_t FloatBits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	VertexPositionKey MakeVertexPositionKey(DirectX::XMFLOAT3 position)
	{
		return VertexPositionKey{
			FloatBits(position.x),
			FloatBits(position.y),
			FloatBits(position.z)
		};
	}

	std::vector<DirectX::XMFLOAT3> RecalculateGroupNormals(
		const std::vector<uint32_t>& groupLocalToGlobal,
		const std::vector<meshopt_Meshlet>& meshlets,
		const std::vector<uint32_t>& meshletVertices,
		const std::vector<uint8_t>& meshletTriangles,
		const std::vector<std::byte>& vertices,
		size_t vertexStrideBytes)
	{
		constexpr size_t PositionByteOffset = MeshVertexLayout::PositionOffset;
		constexpr size_t NormalByteOffset = MeshVertexLayout::NormalOffset;

		std::vector<DirectX::XMFLOAT3> accumulatedNormals(groupLocalToGlobal.size(), DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f));

		for (const meshopt_Meshlet& meshlet : meshlets)
		{
			const uint32_t meshletVertexOffset = meshlet.vertex_offset;
			const uint32_t meshletTriangleOffset = meshlet.triangle_offset;

			for (uint32_t triangleIndex = 0; triangleIndex < meshlet.triangle_count; ++triangleIndex)
			{
				const uint32_t triBase = meshletTriangleOffset + triangleIndex * 3u;
				const uint32_t localIndex0 = static_cast<uint32_t>(meshletTriangles[triBase + 0u]);
				const uint32_t localIndex1 = static_cast<uint32_t>(meshletTriangles[triBase + 1u]);
				const uint32_t localIndex2 = static_cast<uint32_t>(meshletTriangles[triBase + 2u]);

				if (localIndex0 >= meshlet.vertex_count || localIndex1 >= meshlet.vertex_count || localIndex2 >= meshlet.vertex_count)
				{
					continue;
				}

				const uint32_t groupVertex0 = meshletVertices[meshletVertexOffset + localIndex0];
				const uint32_t groupVertex1 = meshletVertices[meshletVertexOffset + localIndex1];
				const uint32_t groupVertex2 = meshletVertices[meshletVertexOffset + localIndex2];

				if (groupVertex0 >= groupLocalToGlobal.size() || groupVertex1 >= groupLocalToGlobal.size() || groupVertex2 >= groupLocalToGlobal.size())
				{
					continue;
				}

				const DirectX::XMFLOAT3 p0 = ReadVertexFloat3(vertices, vertexStrideBytes, groupLocalToGlobal[groupVertex0], PositionByteOffset);
				const DirectX::XMFLOAT3 p1 = ReadVertexFloat3(vertices, vertexStrideBytes, groupLocalToGlobal[groupVertex1], PositionByteOffset);
				const DirectX::XMFLOAT3 p2 = ReadVertexFloat3(vertices, vertexStrideBytes, groupLocalToGlobal[groupVertex2], PositionByteOffset);

				const float e10x = p1.x - p0.x;
				const float e10y = p1.y - p0.y;
				const float e10z = p1.z - p0.z;
				const float e20x = p2.x - p0.x;
				const float e20y = p2.y - p0.y;
				const float e20z = p2.z - p0.z;

				const DirectX::XMFLOAT3 faceNormal(
					e10y * e20z - e10z * e20y,
					e10z * e20x - e10x * e20z,
					e10x * e20y - e10y * e20x);

				accumulatedNormals[groupVertex0].x += faceNormal.x;
				accumulatedNormals[groupVertex0].y += faceNormal.y;
				accumulatedNormals[groupVertex0].z += faceNormal.z;
				accumulatedNormals[groupVertex1].x += faceNormal.x;
				accumulatedNormals[groupVertex1].y += faceNormal.y;
				accumulatedNormals[groupVertex1].z += faceNormal.z;
				accumulatedNormals[groupVertex2].x += faceNormal.x;
				accumulatedNormals[groupVertex2].y += faceNormal.y;
				accumulatedNormals[groupVertex2].z += faceNormal.z;
			}
		}

		std::unordered_map<VertexPositionKey, DirectX::XMFLOAT3, VertexPositionKeyHash> coincidentNormalSums;
		coincidentNormalSums.reserve(groupLocalToGlobal.size());
		for (size_t groupVertex = 0; groupVertex < groupLocalToGlobal.size(); ++groupVertex)
		{
			const DirectX::XMFLOAT3 position = ReadVertexFloat3(
				vertices,
				vertexStrideBytes,
				groupLocalToGlobal[groupVertex],
				PositionByteOffset);
			auto [it, inserted] = coincidentNormalSums.try_emplace(
				MakeVertexPositionKey(position),
				0.0f,
				0.0f,
				0.0f);
			DirectX::XMFLOAT3& sum = it->second;
			sum.x += accumulatedNormals[groupVertex].x;
			sum.y += accumulatedNormals[groupVertex].y;
			sum.z += accumulatedNormals[groupVertex].z;
		}

		std::vector<DirectX::XMFLOAT3> result;
		result.resize(groupLocalToGlobal.size());

		for (size_t groupVertex = 0; groupVertex < groupLocalToGlobal.size(); ++groupVertex)
		{
			const DirectX::XMFLOAT3 position = ReadVertexFloat3(
				vertices,
				vertexStrideBytes,
				groupLocalToGlobal[groupVertex],
				PositionByteOffset);
			const DirectX::XMFLOAT3 sourceNormal = ReadVertexFloat3(
				vertices,
				vertexStrideBytes,
				groupLocalToGlobal[groupVertex],
				NormalByteOffset);

			const auto sumIt = coincidentNormalSums.find(MakeVertexPositionKey(position));
			const DirectX::XMFLOAT3 normalSum = sumIt != coincidentNormalSums.end() ? sumIt->second : accumulatedNormals[groupVertex];
			result[groupVertex] = NormalizeOrFallback(normalSum, sourceNormal);
		}

		return result;
	}

	struct CapturedClusterLODCluster
	{
		int32_t refinedGroup = -1;
		clodBounds bounds{};
		uint32_t indicesOffset = 0;
		uint32_t indexCount = 0;
		uint32_t vertexCount = 0;
		uint32_t materialSlot = 0;
	};

	struct CapturedClusterLODGroup
	{
		int depth = 0;
		clodBounds simplified{};
		std::vector<unsigned int> flattenedIndices;
		std::vector<CapturedClusterLODCluster> clusters;
		// Multi-material meshes (merge hierarchies): each cluster's slot goes in descriptor word 15 instead of the
		// terrain RVT radius, which only terrain reads.
		bool writeMaterialSlots = false;
	};

	struct ClusterLODGroupBuildOutput
	{
		ClusterLODGroup group{};
		std::vector<meshopt_Meshlet> meshlets;
		std::vector<uint32_t> meshletVertices;
		std::vector<uint8_t> meshletTriangles;
		std::vector<BoundingSphere> meshletBounds;
		std::vector<ClusterLODGroupSegment> segments;
		std::vector<BoundingSphere> segmentBounds;
		std::vector<std::byte> vertexChunk;
		std::vector<std::byte> skinningChunk;
		std::vector<int32_t> meshletRefinedGroups;
		std::vector<uint32_t> meshletMaterialSlots;
		std::vector<uint32_t> vertexGlobalIds; // each group vertex's build vertex id
		std::vector<std::vector<std::byte>> pageBlobs;
		ClusterLODGroupChunk groupChunk{};
	};

	ClusterLODGroupBuildOutput BuildClusterLODGroupOutput(
		const CapturedClusterLODGroup& capturedGroup,
		uint32_t sourceGroupLocalIndex,
		const std::vector<std::byte>& vertices,
		const std::vector<MeshUvSetData>& uvSets,
		unsigned int vertexFlags,
		size_t vertexStrideBytes,
		const std::vector<std::byte>* skinningVertices,
		size_t skinningVertexStrideBytes,
		float meshPositionQuantScale,
		uint32_t meshPositionQuantExp,
		uint32_t nodeBoneLimit,
		bool recomputeNormals,
		bool reserveNormalSpread = false,
		uint32_t remapReservePerTriangle = 0u)
	{
		ZoneScopedN("ClusterLODUtilities::Build::BuildClusterLODGroupOutput");
		TracyPlot("CLOD.Build.GroupOutput.InputClusters", static_cast<int64_t>(capturedGroup.clusters.size()));
		TracyPlot("CLOD.Build.GroupOutput.InputIndices", static_cast<int64_t>(capturedGroup.flattenedIndices.size()));

		ClusterLODGroupBuildOutput output{};

		output.group.bounds = capturedGroup.simplified;
		output.group.depth = capturedGroup.depth;
		output.group.firstMeshlet = 0;
		output.group.meshletCount = static_cast<uint32_t>(capturedGroup.clusters.size());
		output.group.firstGroupVertex = 0;
		output.group.groupVertexCount = 0;
		output.group.firstSegment = 0;
		output.group.segmentCount = 0;
		output.group.terminalSegmentCount = 0;

		const size_t maxGroupVertices = capturedGroup.clusters.size() * MS_MESHLET_SIZE;
		std::vector<uint32_t> groupLocalToGlobal;
		groupLocalToGlobal.reserve(maxGroupVertices);

		// Global -> group-local vertex index: an open-addressing table at most half full. Local indices are handed out in
		// first-seen order, as before; a slot holds localIndex + 1, with 0 meaning empty.
		size_t tableSize = 64;
		while (tableSize < maxGroupVertices * 2u)
			tableSize <<= 1u;
		const size_t tableMask = tableSize - 1u;
		std::unique_ptr<uint32_t[]> tableKeys(new uint32_t[tableSize]);
		std::unique_ptr<uint32_t[]> tableSlots(new uint32_t[tableSize]());

		auto getGroupLocalVertexIndex = [&](uint32_t globalVertexIndex) -> uint32_t
			{
				size_t slot = (static_cast<size_t>(globalVertexIndex) * 0x9E3779B97F4A7C15ull >> 32u) & tableMask;
				while (tableSlots[slot] != 0u)
				{
					if (tableKeys[slot] == globalVertexIndex)
						return tableSlots[slot] - 1u;
					slot = (slot + 1u) & tableMask;
				}

				const uint32_t localIndex = static_cast<uint32_t>(groupLocalToGlobal.size());
				tableKeys[slot] = globalVertexIndex;
				tableSlots[slot] = localIndex + 1u;
				groupLocalToGlobal.push_back(globalVertexIndex);
				return localIndex;
			};

		struct ChildBucket
		{
			int32_t refinedGroup = -1;
			std::vector<uint32_t> clusterIndices;
		};

		std::vector<ChildBucket> buckets;
		buckets.reserve(capturedGroup.clusters.size());

		auto addToBucket = [&](int32_t refinedGroup, uint32_t clusterIndex)
			{
				for (ChildBucket& bucket : buckets)
				{
					if (bucket.refinedGroup == refinedGroup)
					{
						bucket.clusterIndices.push_back(clusterIndex);
						return;
					}
				}

				buckets.push_back(ChildBucket{ refinedGroup, {} });
				buckets.back().clusterIndices.reserve(8);
				buckets.back().clusterIndices.push_back(clusterIndex);
			};

		{
			ZoneScopedN("ClusterLODUtilities::Build::GroupOutput::BucketClusters");
			for (uint32_t clusterIndex = 0; clusterIndex < static_cast<uint32_t>(capturedGroup.clusters.size()); ++clusterIndex)
			{
				addToBucket(capturedGroup.clusters[clusterIndex].refinedGroup, clusterIndex);
			}
		}
		TracyPlot("CLOD.Build.GroupOutput.Buckets", static_cast<int64_t>(buckets.size()));

		// Track which bucket (refinedGroup) each meshlet belongs to
		std::vector<int32_t> meshletBucketTag;
		meshletBucketTag.reserve(capturedGroup.clusters.size());
		output.meshlets.reserve(capturedGroup.clusters.size());
		output.meshletBounds.reserve(capturedGroup.clusters.size());
		output.meshletVertices.reserve(capturedGroup.clusters.size() * MS_MESHLET_SIZE);
		output.meshletTriangles.reserve(capturedGroup.flattenedIndices.size());

		uint32_t groupMeshletVertexCursor = 0;
		uint32_t localMeshletCursor = 0;

		{
			ZoneScopedN("ClusterLODUtilities::Build::GroupOutput::BuildMeshletStreams");
			std::array<unsigned int, MS_MESHLET_SIZE> localVertices{};
			std::array<unsigned char, MS_MESHLET_SIZE * 3u> localTriangles{};
			auto appendMeshlet = [&](const unsigned int* sourceVertices,
				const unsigned char* sourceTriangles,
				uint32_t vertexCount,
				uint32_t triangleCount,
				const clodBounds& bounds,
				int32_t refinedGroup,
				uint32_t materialSlot)
				{
					assert(vertexCount <= MS_MESHLET_SIZE);
					assert(triangleCount <= MS_MESHLET_SIZE);

					const size_t meshletVertexStart = output.meshletVertices.size();
					output.meshletVertices.resize(meshletVertexStart + vertexCount);
					for (uint32_t localVertex = 0; localVertex < vertexCount; ++localVertex)
					{
						output.meshletVertices[meshletVertexStart + localVertex] =
							getGroupLocalVertexIndex(sourceVertices[localVertex]);
					}

					meshopt_Meshlet meshlet{};
					meshlet.vertex_offset = groupMeshletVertexCursor;
					meshlet.triangle_offset = static_cast<uint32_t>(output.meshletTriangles.size());
					meshlet.vertex_count = vertexCount;
					meshlet.triangle_count = triangleCount;

					const size_t triangleIndexCount = static_cast<size_t>(triangleCount) * 3u;
					const size_t meshletTriangleStart = output.meshletTriangles.size();
					output.meshletTriangles.resize(meshletTriangleStart + triangleIndexCount);
					std::memcpy(
						output.meshletTriangles.data() + meshletTriangleStart,
						sourceTriangles,
						triangleIndexCount);
					groupMeshletVertexCursor += vertexCount;

					BoundingSphere sphere{};
					sphere.sphere = DirectX::XMFLOAT4(bounds.center[0], bounds.center[1], bounds.center[2], bounds.radius);
					output.meshlets.push_back(meshlet);
					output.meshletBounds.push_back(sphere);
					meshletBucketTag.push_back(refinedGroup);
					output.meshletMaterialSlots.push_back(materialSlot);
					++localMeshletCursor;
				};

			for (const ChildBucket& bucket : buckets)
			{
				for (uint32_t clusterIndex : bucket.clusterIndices)
				{
					const CapturedClusterLODCluster& cluster = capturedGroup.clusters[clusterIndex];
					const uint32_t triangleCount = cluster.indexCount / 3;
					const unsigned int* clusterIndices = capturedGroup.flattenedIndices.data() + cluster.indicesOffset;

					if (cluster.vertexCount > MS_MESHLET_SIZE || triangleCount > MS_MESHLET_SIZE)
					{
						TracyPlot("CLOD.Build.GroupOutput.OversizedClusterVertices", static_cast<int64_t>(cluster.vertexCount));
						TracyPlot("CLOD.Build.GroupOutput.OversizedClusterIndices", static_cast<int64_t>(cluster.indexCount));
						spdlog::error(
							"ClusterLOD emitted an oversized cluster: sourceGroup={} depth={} cluster={} vertices={} triangles={} limits=({}, {})",
							sourceGroupLocalIndex,
							capturedGroup.depth,
							clusterIndex,
							cluster.vertexCount,
							triangleCount,
							MS_MESHLET_SIZE,
							MS_MESHLET_SIZE);
						throw std::runtime_error("ClusterLOD emitted a cluster exceeding the GPU meshlet limits");
					}

					const size_t uniqueVertexCount = clodLocalIndices(
						localVertices.data(),
						localTriangles.data(),
						clusterIndices,
						cluster.indexCount);
					assert(uniqueVertexCount == cluster.vertexCount);
					appendMeshlet(
						localVertices.data(),
						localTriangles.data(),
						static_cast<uint32_t>(uniqueVertexCount),
						triangleCount,
						cluster.bounds,
						bucket.refinedGroup,
						cluster.materialSlot);
				}
			}
		}

		assert(localMeshletCursor == output.group.meshletCount);

		output.group.groupVertexCount = static_cast<uint32_t>(groupLocalToGlobal.size());
		TracyPlot("CLOD.Build.GroupOutput.GroupVertices", static_cast<int64_t>(output.group.groupVertexCount));
		TracyPlot("CLOD.Build.GroupOutput.Meshlets", static_cast<int64_t>(output.meshlets.size()));

		{
			ZoneScopedN("ClusterLODUtilities::Build::GroupOutput::CopyVertexChunk");
			output.vertexChunk.resize(static_cast<size_t>(output.group.groupVertexCount) * vertexStrideBytes);

			for (size_t groupVertexIndex = 0; groupVertexIndex < groupLocalToGlobal.size(); ++groupVertexIndex)
			{
				const uint32_t globalVertexIndex = groupLocalToGlobal[groupVertexIndex];
				const size_t sourceVertexByteOffset = static_cast<size_t>(globalVertexIndex) * vertexStrideBytes;
				std::memcpy(
					output.vertexChunk.data() + groupVertexIndex * vertexStrideBytes,
					vertices.data() + sourceVertexByteOffset,
					vertexStrideBytes);
			}
		}

		const bool hasNormalStream = (vertexFlags & VertexFlags::VERTEX_NORMALS) != 0u &&
			vertexStrideBytes >= MeshVertexLayout::NormalOffset + sizeof(float) * 3;
		const bool hasTexcoordStream = (vertexFlags & VertexFlags::VERTEX_TEXCOORDS) != 0u &&
			vertexStrideBytes >= MeshVertexLayout::TexcoordOffset(vertexFlags) + sizeof(float) * 2;
		const bool hasColorStream = (vertexFlags & VertexFlags::VERTEX_COLORS) != 0u &&
			vertexStrideBytes >= MeshVertexLayout::ColorOffset(vertexFlags) + sizeof(float) * 3;
		const bool hasTangentStream = (vertexFlags & VertexFlags::VERTEX_TANGENTS) != 0u &&
			vertexStrideBytes >= MeshVertexLayout::TangentOffset(vertexFlags) + sizeof(float) * 4;
		std::vector<DirectX::XMFLOAT3> groupNormals;
		if (hasNormalStream)
		{
			ZoneScopedN("ClusterLODUtilities::Build::GroupOutput::BuildNormals");
			groupNormals.resize(groupLocalToGlobal.size());

			if (recomputeNormals)
			{
				ZoneScopedN("ClusterLODUtilities::Build::GroupOutput::RecalculateNormals");
				groupNormals = RecalculateGroupNormals(
					groupLocalToGlobal,
					output.meshlets,
					output.meshletVertices,
					output.meshletTriangles,
					vertices,
					vertexStrideBytes);

				for (size_t groupVertexIndex = 0; groupVertexIndex < groupNormals.size(); ++groupVertexIndex)
				{
					const size_t destinationByteOffset = groupVertexIndex * vertexStrideBytes + MeshVertexLayout::NormalOffset;
					std::memcpy(output.vertexChunk.data() + destinationByteOffset, &groupNormals[groupVertexIndex].x, sizeof(float));
					std::memcpy(output.vertexChunk.data() + destinationByteOffset + sizeof(float), &groupNormals[groupVertexIndex].y, sizeof(float));
					std::memcpy(output.vertexChunk.data() + destinationByteOffset + sizeof(float) * 2, &groupNormals[groupVertexIndex].z, sizeof(float));
				}
			}
			else
			{
				for (size_t groupVertexIndex = 0; groupVertexIndex < groupLocalToGlobal.size(); ++groupVertexIndex)
				{
					const size_t offset = groupVertexIndex * vertexStrideBytes + MeshVertexLayout::NormalOffset;
					std::memcpy(&groupNormals[groupVertexIndex], output.vertexChunk.data() + offset, sizeof(DirectX::XMFLOAT3));
				}
			}
		}

		std::vector<DirectX::XMFLOAT4> groupTangents;
		if (hasTangentStream)
		{
			ZoneScopedN("ClusterLODUtilities::Build::GroupOutput::CopyTangents");
			groupTangents.resize(groupLocalToGlobal.size());
			for (size_t groupVertexIndex = 0; groupVertexIndex < groupLocalToGlobal.size(); ++groupVertexIndex)
			{
				DirectX::XMFLOAT4 tangent{};
				const size_t offset = groupVertexIndex * vertexStrideBytes + MeshVertexLayout::TangentOffset(vertexFlags);
				std::memcpy(&tangent, output.vertexChunk.data() + offset, sizeof(tangent));
				groupTangents[groupVertexIndex] = tangent;
			}
		}

		std::vector<MeshUvSetData> groupUvSets;
		groupUvSets.reserve(uvSets.size());
		const size_t sourceVertexCount = vertexStrideBytes > 0 ? (vertices.size() / vertexStrideBytes) : 0u;
		{
			ZoneScopedN("ClusterLODUtilities::Build::GroupOutput::BuildUvSets");
			for (const MeshUvSetData& sourceUvSet : uvSets)
			{
				MeshUvSetData groupUvSet;
				groupUvSet.name = sourceUvSet.name;
				groupUvSet.values.resize(groupLocalToGlobal.size(), DirectX::XMFLOAT2(0.0f, 0.0f));

				if (sourceUvSet.values.size() == sourceVertexCount)
				{
					for (size_t groupVertexIndex = 0; groupVertexIndex < groupLocalToGlobal.size(); ++groupVertexIndex)
					{
						groupUvSet.values[groupVertexIndex] = sourceUvSet.values[groupLocalToGlobal[groupVertexIndex]];
					}
				}

				groupUvSets.push_back(std::move(groupUvSet));
			}
		}

		if (groupUvSets.empty() && hasTexcoordStream)
		{
			ZoneScopedN("ClusterLODUtilities::Build::GroupOutput::BuildLegacyUvSet");
			MeshUvSetData legacyUvSet;
			legacyUvSet.name = "UV0";
			legacyUvSet.values.resize(groupLocalToGlobal.size());
			for (size_t groupVertexIndex = 0; groupVertexIndex < groupLocalToGlobal.size(); ++groupVertexIndex)
			{
				const size_t offset = groupVertexIndex * vertexStrideBytes + MeshVertexLayout::TexcoordOffset(vertexFlags);
				std::memcpy(&legacyUvSet.values[groupVertexIndex], output.vertexChunk.data() + offset, sizeof(DirectX::XMFLOAT2));
			}
			groupUvSets.push_back(std::move(legacyUvSet));
		}

		std::vector<uint32_t> compressedColorWords;
		if (hasColorStream)
		{
			ZoneScopedN("ClusterLODUtilities::Build::GroupOutput::CompressColors");
			compressedColorWords.reserve(groupLocalToGlobal.size());
			for (size_t groupVertexIndex = 0; groupVertexIndex < groupLocalToGlobal.size(); ++groupVertexIndex)
			{
				DirectX::XMFLOAT3 color{};
				const size_t offset = groupVertexIndex * vertexStrideBytes + MeshVertexLayout::ColorOffset(vertexFlags);
				std::memcpy(&color, output.vertexChunk.data() + offset, sizeof(DirectX::XMFLOAT3));
				compressedColorWords.push_back(PackColorUnorm8(color));
			}
		}

		std::vector<PackedSkinningInfluences> groupSkinningInfluences;
		const bool hasSkinningStream =
			(skinningVertices != nullptr) &&
			(skinningVertexStrideBytes >= sizeof(DirectX::XMFLOAT3) + sizeof(DirectX::XMFLOAT3) + sizeof(PackedSkinningInfluences)) &&
			!skinningVertices->empty();
		const size_t sourceSkinningVertexCount =
			(hasSkinningStream && skinningVertexStrideBytes > 0) ? (skinningVertices->size() / skinningVertexStrideBytes) : 0u;
		if (hasSkinningStream)
		{
			ZoneScopedN("ClusterLODUtilities::Build::GroupOutput::CopySkinning");
			constexpr size_t JointByteOffset = sizeof(DirectX::XMFLOAT3) + sizeof(DirectX::XMFLOAT3);
			groupSkinningInfluences.resize(groupLocalToGlobal.size(), PackedSkinningInfluences{});
			output.skinningChunk.resize(groupLocalToGlobal.size() * skinningVertexStrideBytes);

			for (size_t groupVertexIndex = 0; groupVertexIndex < groupLocalToGlobal.size(); ++groupVertexIndex)
			{
				const uint32_t globalVertexIndex = groupLocalToGlobal[groupVertexIndex];
				if (globalVertexIndex >= sourceSkinningVertexCount)
				{
					continue;
				}

				const size_t sourceByteOffset = static_cast<size_t>(globalVertexIndex) * skinningVertexStrideBytes;
				std::memcpy(
					output.skinningChunk.data() + groupVertexIndex * skinningVertexStrideBytes,
					skinningVertices->data() + sourceByteOffset,
					skinningVertexStrideBytes);
				std::memcpy(&groupSkinningInfluences[groupVertexIndex],
					skinningVertices->data() + sourceByteOffset + JointByteOffset,
					sizeof(PackedSkinningInfluences));
			}
		}

		std::vector<DirectX::XMFLOAT3> groupPositions;
		groupPositions.reserve(groupLocalToGlobal.size());
		std::vector<std::array<int32_t, 3>> quantizedGroupPositions;
		quantizedGroupPositions.reserve(groupLocalToGlobal.size());

		{
			ZoneScopedN("ClusterLODUtilities::Build::GroupOutput::BuildPositionsAndQuantize");
			for (size_t groupVertexIndex = 0; groupVertexIndex < groupLocalToGlobal.size(); ++groupVertexIndex)
			{
				const size_t byteOffset = groupVertexIndex * vertexStrideBytes;
				float px = 0.0f;
				float py = 0.0f;
				float pz = 0.0f;
				std::memcpy(&px, output.vertexChunk.data() + byteOffset, sizeof(float));
				std::memcpy(&py, output.vertexChunk.data() + byteOffset + sizeof(float), sizeof(float));
				std::memcpy(&pz, output.vertexChunk.data() + byteOffset + sizeof(float) * 2, sizeof(float));

				groupPositions.emplace_back(px, py, pz);

				const int32_t qx = static_cast<int32_t>(std::floor(px * meshPositionQuantScale + 0.5f));
				const int32_t qy = static_cast<int32_t>(std::floor(py * meshPositionQuantScale + 0.5f));
				const int32_t qz = static_cast<int32_t>(std::floor(pz * meshPositionQuantScale + 0.5f));

				quantizedGroupPositions.push_back({ qx, qy, qz });
			}
		}

		// Compress normals for page blob construction (oct-encoded, one word per vertex)
		std::vector<uint32_t> compressedNormalWords;
		if (hasNormalStream)
		{
			ZoneScopedN("ClusterLODUtilities::Build::GroupOutput::CompressNormals");
			compressedNormalWords.reserve(groupNormals.size());
			for (const DirectX::XMFLOAT3& normal : groupNormals)
			{
				auto oct = OctEncodeNormal(normal);
				compressedNormalWords.push_back(PackOctNormalSnorm16(oct));
			}
		}

		std::vector<uint32_t> compressedTangentFrameWords;
		if (hasTangentStream && !groupNormals.empty())
		{
			ZoneScopedN("ClusterLODUtilities::Build::GroupOutput::CompressTangentFrames");
			compressedTangentFrameWords.reserve(groupTangents.size());
			for (size_t groupVertexIndex = 0; groupVertexIndex < groupTangents.size(); ++groupVertexIndex)
			{
				compressedTangentFrameWords.push_back(PackTangentFrameAngle(groupNormals[groupVertexIndex], groupTangents[groupVertexIndex]));
			}
		}

		output.groupChunk.groupVertexCount = output.group.groupVertexCount;
		output.groupChunk.meshletCount = static_cast<uint32_t>(output.meshlets.size());
		output.groupChunk.meshletTrianglesByteCount = static_cast<uint32_t>(output.meshletTriangles.size());
		output.groupChunk.compressedPositionQuantExp = CLOD_NATIVE_POSITION_FORMAT;
		output.groupChunk.compressedFlags = 0u;
		if (!compressedNormalWords.empty())
		{
			output.groupChunk.compressedFlags |= CLOD_COMPRESSED_NORMALS;
		}

		// Per-meshlet compression + page binning + segment creation + page blob construction
		{
			ZoneScopedN("ClusterLODUtilities::Build::GroupOutput::BuildPages");
			const bool hasNormals = !compressedNormalWords.empty();
			const bool hasTangentFrames = !compressedTangentFrameWords.empty();
			const bool hasColors = !compressedColorWords.empty();
			auto align4 = [](size_t v) -> size_t { return (v + 3u) & ~size_t(3); };

			// === Per-meshlet compression parameters ===
			struct PerUvSetCompression {
				float uvMinU = 0.0f;
				float uvMinV = 0.0f;
				float uvScaleU = 0.0f;
				float uvScaleV = 0.0f;
				uint32_t uvBitsU = 0;
				uint32_t uvBitsV = 0;
				uint64_t totalUvBits = 0;
			};

			struct PerMeshletCompression {
				std::array<int32_t, 3> minQ;
				uint32_t bitsX, bitsY, bitsZ;
				uint32_t attributeMask = 0;
				std::vector<uint32_t> boneList;
				std::vector<PerUvSetCompression> uvSets;
			};

			auto GetMeshletPositionBytes = [](const meshopt_Meshlet& meshlet) -> uint32_t
			{
				static_assert(CLOD_NATIVE_POSITION_FORMAT == CLOD_POSITION_FORMAT_FLOAT3);
				return meshlet.vertex_count * CLOD_NATIVE_POSITION_STRIDE_BYTES;
			};

			const uint32_t totalMeshlets = static_cast<uint32_t>(output.meshlets.size());
			std::vector<PerMeshletCompression> perMeshletComp(totalMeshlets);

			{
				ZoneScopedN("ClusterLODUtilities::Build::GroupOutput::ComputePerMeshletCompression");
				for (uint32_t mi = 0; mi < totalMeshlets; ++mi)
				{
					const auto& meshlet = output.meshlets[mi];
					auto& comp = perMeshletComp[mi];

					std::array<int32_t, 3> meshletMinQ = { std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max() };
					std::array<int32_t, 3> meshletMaxQ = { std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::min() };

					for (uint32_t vi = 0; vi < meshlet.vertex_count; ++vi)
					{
						const uint32_t groupLocalVertex = output.meshletVertices[meshlet.vertex_offset + vi];
						const auto& q = quantizedGroupPositions[groupLocalVertex];
						for (int axis = 0; axis < 3; ++axis)
						{
							meshletMinQ[axis] = std::min(meshletMinQ[axis], q[axis]);
							meshletMaxQ[axis] = std::max(meshletMaxQ[axis], q[axis]);
						}
					}

					if (meshlet.vertex_count == 0)
					{
						meshletMinQ = { 0, 0, 0 };
						meshletMaxQ = { 0, 0, 0 };
					}

					comp.minQ = meshletMinQ;
					comp.bitsX = BitsNeededForRange(static_cast<uint32_t>(meshletMaxQ[0] - meshletMinQ[0]));
					comp.bitsY = BitsNeededForRange(static_cast<uint32_t>(meshletMaxQ[1] - meshletMinQ[1]));
					comp.bitsZ = BitsNeededForRange(static_cast<uint32_t>(meshletMaxQ[2] - meshletMinQ[2]));
					if (hasNormals)
					{
						comp.attributeMask |= CLOD_PAGE_ATTRIBUTE_NORMAL;
					}
					if (hasTangentFrames)
					{
						comp.attributeMask |= CLOD_PAGE_ATTRIBUTE_TANGENT_FRAME;
					}
					if (hasColors)
					{
						comp.attributeMask |= CLOD_PAGE_ATTRIBUTE_COLOR;
					}
					if (hasSkinningStream)
					{
						comp.attributeMask |= (CLOD_PAGE_ATTRIBUTE_JOINTS | CLOD_PAGE_ATTRIBUTE_WEIGHTS);
						std::array<uint32_t, MS_MESHLET_SIZE * kMaxSkinInfluences> uniqueBones{};
						uint32_t uniqueBoneCount = 0u;
						for (uint32_t vi = 0; vi < meshlet.vertex_count; ++vi)
						{
							const uint32_t groupLocalVertex = output.meshletVertices[meshlet.vertex_offset + vi];
							const PackedSkinningInfluences& skinning = groupSkinningInfluences[groupLocalVertex];
							const uint32_t jointValues[kMaxSkinInfluences] = {
								skinning.joints0.x, skinning.joints0.y, skinning.joints0.z, skinning.joints0.w,
								skinning.joints1.x, skinning.joints1.y, skinning.joints1.z, skinning.joints1.w
							};
							const float weightValues[kMaxSkinInfluences] = {
								skinning.weights0.x, skinning.weights0.y, skinning.weights0.z, skinning.weights0.w,
								skinning.weights1.x, skinning.weights1.y, skinning.weights1.z, skinning.weights1.w
							};
							for (uint32_t influence = 0; influence < kMaxSkinInfluences; ++influence)
							{
								if (weightValues[influence] > 0.0f)
								{
									bool seen = false;
									for (uint32_t boneIndex = 0; boneIndex < uniqueBoneCount; ++boneIndex)
									{
										if (uniqueBones[boneIndex] == jointValues[influence])
										{
											seen = true;
											break;
										}
									}
									if (!seen)
									{
										uniqueBones[uniqueBoneCount++] = jointValues[influence];
									}
								}
							}
						}

						comp.boneList.assign(uniqueBones.begin(), uniqueBones.begin() + uniqueBoneCount);
						std::sort(comp.boneList.begin(), comp.boneList.end());
					}
					if (!groupUvSets.empty())
					{
						comp.uvSets.resize(groupUvSets.size());
						for (size_t uvSetIndex = 0; uvSetIndex < groupUvSets.size(); ++uvSetIndex)
						{
							float minU = std::numeric_limits<float>::max();
							float minV = std::numeric_limits<float>::max();
							float maxU = -std::numeric_limits<float>::max();
							float maxV = -std::numeric_limits<float>::max();

							for (uint32_t vi = 0; vi < meshlet.vertex_count; ++vi)
							{
								const uint32_t groupLocalVertex = output.meshletVertices[meshlet.vertex_offset + vi];
								const DirectX::XMFLOAT2 uv = groupUvSets[uvSetIndex].values[groupLocalVertex];
								minU = std::min(minU, uv.x);
								minV = std::min(minV, uv.y);
								maxU = std::max(maxU, uv.x);
								maxV = std::max(maxV, uv.y);
							}

							if (meshlet.vertex_count == 0)
							{
								minU = minV = maxU = maxV = 0.0f;
							}

							const bool useAbsoluteAtlasUvQuantization = groupUvSets[uvSetIndex].name == OBJECT_REYES_ATLAS_HEIGHT_UV_SET_NAME;
							if (useAbsoluteAtlasUvQuantization)
							{
								minU = 0.0f;
								minV = 0.0f;
								maxU = 1.0f;
								maxV = 1.0f;
							}

							const float rangeU = std::max(0.0f, maxU - minU);
							const float rangeV = std::max(0.0f, maxV - minV);
							const uint32_t maxEncodedU = QuantizeUvOffset(rangeU);
							const uint32_t maxEncodedV = QuantizeUvOffset(rangeV);
							PerUvSetCompression& uvComp = comp.uvSets[uvSetIndex];
							uvComp.uvMinU = minU;
							uvComp.uvMinV = minV;
							uvComp.uvScaleU = CLOD_UV_QUANTIZATION_INV_SCALE;
							uvComp.uvScaleV = CLOD_UV_QUANTIZATION_INV_SCALE;
							uvComp.uvBitsU = BitsNeededForRange(maxEncodedU);
							uvComp.uvBitsV = BitsNeededForRange(maxEncodedV);
							uvComp.totalUvBits = static_cast<uint64_t>(meshlet.vertex_count) * (uvComp.uvBitsU + uvComp.uvBitsV);
						}
					}
				}
			}

			auto GetMeshletNormalWords = [&](uint32_t pageMask, const meshopt_Meshlet& meshlet) -> uint32_t
			{
				return ((pageMask & CLOD_PAGE_ATTRIBUTE_NORMAL) != 0u) ? meshlet.vertex_count : 0u;
			};

			auto GetMeshletTangentFrameWords = [&](uint32_t pageMask, const meshopt_Meshlet& meshlet) -> uint32_t
			{
				return ((pageMask & CLOD_PAGE_ATTRIBUTE_TANGENT_FRAME) != 0u) ? meshlet.vertex_count : 0u;
			};

			auto GetMeshletColorWords = [&](uint32_t pageMask, const meshopt_Meshlet& meshlet) -> uint32_t
			{
				return ((pageMask & CLOD_PAGE_ATTRIBUTE_COLOR) != 0u) ? meshlet.vertex_count : 0u;
			};

			auto GetMeshletUvBits = [&](uint32_t pageUvSetIndex, const meshopt_Meshlet& meshlet, const PerMeshletCompression& comp) -> uint64_t
			{
				if (pageUvSetIndex < comp.uvSets.size())
				{
					return comp.uvSets[pageUvSetIndex].totalUvBits;
				}

				// Future mixed-format pages backfill missing UV sets with a constant zero stream.
				return static_cast<uint64_t>(meshlet.vertex_count) * 2ull;
			};

			struct PageTotals {
				uint32_t totalPositionBytes = 0;
				std::vector<uint64_t> totalUvBitsPerSet;
				uint32_t totalVertexCount = 0;
				uint32_t totalNormalWords = 0;
				uint32_t totalTangentFrameWords = 0;
				uint32_t totalColorWords = 0;
				uint32_t totalBoneIndexCount = 0;
				uint32_t totalTriangleBytes = 0;
			};

			auto AddMeshletToPageTotals = [&](PageTotals totals, uint32_t currentMask, uint32_t currentUvSetCount, uint32_t candidateMask, uint32_t candidateUvSetCount, uint32_t meshletIndex) -> PageTotals
			{
				const auto& meshlet = output.meshlets[meshletIndex];
				const auto& comp = perMeshletComp[meshletIndex];
				if (candidateUvSetCount > currentUvSetCount)
				{
					totals.totalUvBitsPerSet.resize(candidateUvSetCount, 0ull);
					const uint64_t backfillBitsPerMissingSet = static_cast<uint64_t>(totals.totalVertexCount) * 2ull;
					for (uint32_t uvSetIndex = currentUvSetCount; uvSetIndex < candidateUvSetCount; ++uvSetIndex)
					{
						totals.totalUvBitsPerSet[uvSetIndex] = backfillBitsPerMissingSet;
					}
				}
				if ((currentMask & CLOD_PAGE_ATTRIBUTE_NORMAL) == 0u && (candidateMask & CLOD_PAGE_ATTRIBUTE_NORMAL) != 0u)
				{
					totals.totalNormalWords = totals.totalVertexCount;
				}
				if ((currentMask & CLOD_PAGE_ATTRIBUTE_TANGENT_FRAME) == 0u && (candidateMask & CLOD_PAGE_ATTRIBUTE_TANGENT_FRAME) != 0u)
				{
					totals.totalTangentFrameWords = totals.totalVertexCount;
				}
				if ((currentMask & CLOD_PAGE_ATTRIBUTE_COLOR) == 0u && (candidateMask & CLOD_PAGE_ATTRIBUTE_COLOR) != 0u)
				{
					totals.totalColorWords = totals.totalVertexCount;
				}

				totals.totalPositionBytes += GetMeshletPositionBytes(meshlet);
				totals.totalVertexCount += meshlet.vertex_count;
				for (uint32_t uvSetIndex = 0; uvSetIndex < candidateUvSetCount; ++uvSetIndex)
				{
					totals.totalUvBitsPerSet[uvSetIndex] += GetMeshletUvBits(uvSetIndex, meshlet, comp);
				}
				totals.totalNormalWords += GetMeshletNormalWords(candidateMask, meshlet);
				totals.totalTangentFrameWords += GetMeshletTangentFrameWords(candidateMask, meshlet);
				totals.totalColorWords += GetMeshletColorWords(candidateMask, meshlet);
				if (comp.boneList.size() <= nodeBoneLimit)
					totals.totalBoneIndexCount += static_cast<uint32_t>(comp.boneList.size());
				totals.totalTriangleBytes += meshlet.triangle_count * 3u;
				return totals;
			};

			// === Page binning (simplified: no vertex set dedup) ===
			struct PageBin {
				std::vector<uint32_t> meshletIndices;
				PageTotals totals;
				uint32_t attributeMask = 0;
				uint32_t uvSetCount = 0;
				size_t spreadReserve = 0; // stage 6 step 3
			};

			std::vector<PageBin> pageBins;
			pageBins.emplace_back();

			{
				ZoneScopedN("ClusterLODUtilities::Build::GroupOutput::BinPages");
				for (uint32_t mi = 0; mi < totalMeshlets; ++mi)
				{
					const auto& meshlet = output.meshlets[mi];
					const auto& comp = perMeshletComp[mi];
					PageBin& currentPage = pageBins.back();
					uint32_t candidateMask = currentPage.attributeMask | comp.attributeMask;
					uint32_t candidateUvSetCount = std::max(currentPage.uvSetCount, static_cast<uint32_t>(comp.uvSets.size()));
					PageTotals candidateTotals = AddMeshletToPageTotals(currentPage.totals, currentPage.attributeMask, currentPage.uvSetCount, candidateMask, candidateUvSetCount, mi);
					const size_t candidateSize = ComputePageBlobSize(
						candidateMask,
						static_cast<uint32_t>(currentPage.meshletIndices.size() + 1ull),
						candidateUvSetCount,
						candidateTotals.totalPositionBytes,
						candidateTotals.totalUvBitsPerSet,
						candidateTotals.totalVertexCount,
						candidateTotals.totalNormalWords,
						candidateTotals.totalTangentFrameWords,
						candidateTotals.totalColorWords,
						candidateTotals.totalBoneIndexCount,
						candidateTotals.totalTriangleBytes);

					// Stage 6 step 3: leave room for the simplified clusters' normal spread blocks, which the mesh-wide pages append
					// (a page holding one segment alone is then still no larger than a page).
					// Stage 6 step 2 refinement (lodErrorChannelsTextureRemapReserve): likewise room for UV remap blocks.
					const size_t remapReserve = mi < meshletBucketTag.size() && meshletBucketTag[mi] >= 0
						? static_cast<size_t>(remapReservePerTriangle) * meshlet.triangle_count : 0u;
					if (reserveNormalSpread && mi < meshletBucketTag.size() && meshletBucketTag[mi] >= 0)
					{
						currentPage.spreadReserve += clod_normal_spread::BlockSize(meshlet.vertex_count);
					}
					currentPage.spreadReserve += remapReserve;
					if (candidateSize + currentPage.spreadReserve > CLOD_PAGE_SIZE && !currentPage.meshletIndices.empty())
					{
						if (reserveNormalSpread && mi < meshletBucketTag.size() && meshletBucketTag[mi] >= 0)
						{
							currentPage.spreadReserve -= clod_normal_spread::BlockSize(meshlet.vertex_count);
						}
						currentPage.spreadReserve -= remapReserve;
						pageBins.emplace_back();
						PageBin& newPage = pageBins.back();
						newPage.attributeMask = comp.attributeMask;
						newPage.uvSetCount = static_cast<uint32_t>(comp.uvSets.size());
						newPage.totals = AddMeshletToPageTotals(PageTotals{}, 0u, 0u, newPage.attributeMask, newPage.uvSetCount, mi);
						newPage.meshletIndices.push_back(mi);
						if (reserveNormalSpread && mi < meshletBucketTag.size() && meshletBucketTag[mi] >= 0)
						{
							newPage.spreadReserve = clod_normal_spread::BlockSize(meshlet.vertex_count);
						}
						newPage.spreadReserve += remapReserve;
						continue;
					}

					currentPage.attributeMask = candidateMask;
					currentPage.uvSetCount = candidateUvSetCount;
					currentPage.totals = std::move(candidateTotals);
					currentPage.meshletIndices.push_back(mi);
				}
			}

			if (!pageBins.empty() && pageBins.back().meshletIndices.empty())
			{
				pageBins.pop_back();
			}

			{
				ZoneScopedN("ClusterLODUtilities::Build::GroupOutput::BuildSegmentsAndBounds");
				// === Create segments: one segment per contiguous refined-group run within a page ===
				// A ClusterLODGroupSegment is a DAG edge, so every meshlet in the
				// segment must have the same refinedGroup tag. Pages may still pack
				// multiple runs; pageIndex + firstMeshletInPage addresses each run.
				for (uint32_t pi = 0; pi < static_cast<uint32_t>(pageBins.size()); ++pi)
				{
					const PageBin& page = pageBins[pi];
					if (page.meshletIndices.empty()) continue;

					uint32_t runStart = 0u;
					while (runStart < static_cast<uint32_t>(page.meshletIndices.size()))
					{
						const int32_t runTag = meshletBucketTag[page.meshletIndices[runStart]];
						uint32_t runEnd = runStart + 1u;
						while (runEnd < static_cast<uint32_t>(page.meshletIndices.size()) &&
							meshletBucketTag[page.meshletIndices[runEnd]] == runTag)
						{
							++runEnd;
						}

						ClusterLODGroupSegment seg{};
						seg.refinedGroup = runTag;
						seg.pageIndex = pi;
						seg.firstMeshletInPage = runStart;
						seg.meshletCount = runEnd - runStart;
						output.segments.push_back(seg);

						runStart = runEnd;
					}
				}

				// Sort segments: terminal (refinedGroup < 0) first.
				std::stable_sort(output.segments.begin(), output.segments.end(),
					[](const ClusterLODGroupSegment& a, const ClusterLODGroupSegment& b) {
						const bool aTerminal = (a.refinedGroup < 0);
						const bool bTerminal = (b.refinedGroup < 0);
						return aTerminal > bTerminal;
					});

				// Compute per-segment bounding spheres.
				output.segmentBounds.resize(output.segments.size());
				for (uint32_t si = 0; si < static_cast<uint32_t>(output.segments.size()); ++si)
				{
					const ClusterLODGroupSegment& seg = output.segments[si];
					const PageBin& page = pageBins[seg.pageIndex];

					float mergedCx = 0.f, mergedCy = 0.f, mergedCz = 0.f, mergedR = 0.f;
					if (seg.meshletCount > 0)
					{
						std::vector<float> centers(seg.meshletCount * 4);
						std::vector<float> radii(seg.meshletCount);
						for (uint32_t i = 0; i < seg.meshletCount; ++i)
						{
							const uint32_t groupLocalMeshlet = page.meshletIndices[seg.firstMeshletInPage + i];
							const BoundingSphere& mb = output.meshletBounds[groupLocalMeshlet];
							centers[i * 4 + 0] = mb.sphere.x;
							centers[i * 4 + 1] = mb.sphere.y;
							centers[i * 4 + 2] = mb.sphere.z;
							centers[i * 4 + 3] = 0.f;
							radii[i] = mb.sphere.w;
						}
						meshopt_Bounds merged = meshopt_computeSphereBounds(
							centers.data(), seg.meshletCount, sizeof(float) * 4,
							radii.data(), sizeof(float));
						mergedCx = merged.center[0];
						mergedCy = merged.center[1];
						mergedCz = merged.center[2];
						mergedR = merged.radius;
					}
					output.segmentBounds[si].sphere = DirectX::XMFLOAT4(mergedCx, mergedCy, mergedCz, mergedR);
				}
			}

			output.group.pageCount = static_cast<uint32_t>(pageBins.size());
			output.group.segmentCount = static_cast<uint32_t>(output.segments.size());
			output.group.terminalSegmentCount = 0;
			for (const ClusterLODGroupSegment& seg : output.segments)
			{
				if (seg.refinedGroup < 0)
					output.group.terminalSegmentCount++;
				else
					break;
			}

			// === Build page blobs: new SoA format ===
				// Layout: Header | CoreDescriptors | UvDescriptors | PositionStream | Optional normal stream |
			//         UV bitstream directory | UV bitstreams | TriangleStream
			output.pageBlobs.resize(pageBins.size());
			TracyPlot("CLOD.Build.GroupOutput.Pages", static_cast<int64_t>(pageBins.size()));
			TracyPlot("CLOD.Build.GroupOutput.Segments", static_cast<int64_t>(output.segments.size()));

			for (uint32_t pi = 0; pi < static_cast<uint32_t>(pageBins.size()); ++pi)
			{
				ZoneScopedN("ClusterLODUtilities::Build::GroupOutput::BuildPageBlob");
				const PageBin& page = pageBins[pi];
				const uint32_t pageMeshletCount = static_cast<uint32_t>(page.meshletIndices.size());
				if (pageMeshletCount == 0) continue;
				const PageTotals& pageTotals = page.totals;
				const bool pageHasNormals = (page.attributeMask & CLOD_PAGE_ATTRIBUTE_NORMAL) != 0u;
				const bool pageHasTangentFrames = (page.attributeMask & CLOD_PAGE_ATTRIBUTE_TANGENT_FRAME) != 0u;
				const bool pageHasColors = (page.attributeMask & CLOD_PAGE_ATTRIBUTE_COLOR) != 0u;
				const bool pageHasJoints = (page.attributeMask & CLOD_PAGE_ATTRIBUTE_JOINTS) != 0u;
				const bool pageHasWeights = (page.attributeMask & CLOD_PAGE_ATTRIBUTE_WEIGHTS) != 0u;
				const bool pageHasUvSets = page.uvSetCount > 0u;

				// Compute stream offsets
				const uint32_t descriptorOffset = static_cast<uint32_t>(align4(CLOD_PAGE_HEADER_SIZE));
				const size_t descriptorBytes = static_cast<size_t>(pageMeshletCount) * sizeof(CLodMeshletDescriptor);
				const uint32_t uvDescriptorOffset = pageHasUvSets
					? static_cast<uint32_t>(align4(descriptorOffset + descriptorBytes))
					: 0u;
				const size_t uvDescriptorBytes = pageHasUvSets
					? static_cast<size_t>(pageMeshletCount) * static_cast<size_t>(page.uvSetCount) * sizeof(CLodMeshletUvDescriptor)
					: 0u;
				const uint32_t positionBitstreamOffset = static_cast<uint32_t>(align4(pageHasUvSets ? (uvDescriptorOffset + uvDescriptorBytes) : (descriptorOffset + descriptorBytes)));
				const size_t positionBytes = static_cast<size_t>(pageTotals.totalPositionBytes);
				const uint32_t normalArrayOffset = pageHasNormals
					? static_cast<uint32_t>(align4(positionBitstreamOffset + positionBytes))
					: 0u;
				const size_t normalBytes = pageHasNormals ? static_cast<size_t>(pageTotals.totalNormalWords) * sizeof(uint32_t) : 0u;
				const uint32_t tangentFrameArrayOffset = pageHasTangentFrames
					? static_cast<uint32_t>(align4(pageHasNormals ? (normalArrayOffset + normalBytes) : (positionBitstreamOffset + positionBytes)))
					: 0u;
				const size_t tangentFrameBytes = pageHasTangentFrames ? static_cast<size_t>(pageTotals.totalTangentFrameWords) * sizeof(uint32_t) : 0u;
				const size_t afterNormalAndTangentBytes = pageHasTangentFrames
					? (static_cast<size_t>(tangentFrameArrayOffset) + tangentFrameBytes)
					: (pageHasNormals
						? (static_cast<size_t>(normalArrayOffset) + normalBytes)
						: (static_cast<size_t>(positionBitstreamOffset) + positionBytes));
				const uint32_t colorArrayOffset = pageHasColors
					? static_cast<uint32_t>(align4(afterNormalAndTangentBytes))
					: 0u;
				const size_t colorBytes = pageHasColors ? static_cast<size_t>(pageTotals.totalColorWords) * sizeof(uint32_t) : 0u;
				const size_t afterColorBytes = pageHasColors
					? (static_cast<size_t>(colorArrayOffset) + colorBytes)
					: afterNormalAndTangentBytes;
				const uint32_t jointArrayOffset = pageHasJoints
					? static_cast<uint32_t>(align4(afterColorBytes))
					: 0u;
				const size_t jointBytes = pageHasJoints ? static_cast<size_t>(pageTotals.totalVertexCount) * sizeof(DirectX::XMUINT4) * 2u : 0u;
				const size_t afterJointBytes = pageHasJoints
					? (static_cast<size_t>(jointArrayOffset) + jointBytes)
					: afterColorBytes;
				const uint32_t weightArrayOffset = pageHasWeights
					? static_cast<uint32_t>(align4(afterJointBytes))
					: 0u;
				const size_t weightBytes = pageHasWeights ? static_cast<size_t>(pageTotals.totalVertexCount) * sizeof(DirectX::XMFLOAT4) * 2u : 0u;
				const size_t afterWeightBytes = pageHasWeights
					? (static_cast<size_t>(weightArrayOffset) + weightBytes)
					: afterJointBytes;
				const uint32_t uvBitstreamDirectoryOffset = pageHasUvSets
					? static_cast<uint32_t>(align4(afterWeightBytes))
					: 0u;
				std::vector<uint32_t> uvBitstreamOffsets(page.uvSetCount, 0u);
				size_t uvBitstreamCursor = pageHasUvSets
					? align4(static_cast<size_t>(uvBitstreamDirectoryOffset) + static_cast<size_t>(page.uvSetCount) * sizeof(uint32_t))
					: align4(afterWeightBytes);
				for (uint32_t uvSetIndex = 0; uvSetIndex < page.uvSetCount; ++uvSetIndex)
				{
					uvBitstreamOffsets[uvSetIndex] = static_cast<uint32_t>(uvBitstreamCursor);
					const size_t uvWords = static_cast<size_t>((pageTotals.totalUvBitsPerSet[uvSetIndex] + 31ull) / 32ull);
					const size_t uvBytes = uvWords * sizeof(uint32_t);
					uvBitstreamCursor = align4(uvBitstreamCursor + uvBytes);
				}
				const uint32_t boneIndexStreamOffset = static_cast<uint32_t>(align4(uvBitstreamCursor));
				const size_t boneIndexBytes = static_cast<size_t>(pageTotals.totalBoneIndexCount) * sizeof(uint32_t);
				const uint32_t triangleStreamOffset = static_cast<uint32_t>(align4(boneIndexStreamOffset + boneIndexBytes));

				const size_t totalBlobSize = align4(triangleStreamOffset + pageTotals.totalTriangleBytes);
				auto& blob = output.pageBlobs[pi];
				blob.assign(totalBlobSize, std::byte{0});

				// Build streams + descriptors in one pass
				std::vector<std::vector<uint32_t>> pageUvWordsPerSet(page.uvSetCount);
				for (uint32_t uvSetIndex = 0; uvSetIndex < page.uvSetCount; ++uvSetIndex)
				{
					const uint64_t uvBits = uvSetIndex < pageTotals.totalUvBitsPerSet.size() ? pageTotals.totalUvBitsPerSet[uvSetIndex] : 0ull;
					pageUvWordsPerSet[uvSetIndex].assign(static_cast<size_t>((uvBits + 31ull) / 32ull), 0u);
				}
				uint32_t pagePositionByteCursor = 0;
				std::vector<uint64_t> pageUvBitCursors(page.uvSetCount, 0ull);
				uint32_t vertexAttributeCursor = 0;
				uint32_t boneIndexCursor = 0;
				uint32_t triangleByteCursor = 0;

				for (uint32_t li = 0; li < pageMeshletCount; ++li)
				{
					const uint32_t mi = page.meshletIndices[li];
					const auto& meshlet = output.meshlets[mi];
					const auto& comp = perMeshletComp[mi];

					CLodMeshletDescriptor desc{};
					desc.positionBitOffset = pagePositionByteCursor;
					desc.vertexAttributeOffset = vertexAttributeCursor;
					desc.triangleByteOffset = triangleByteCursor;
					desc.boneListOffset = boneIndexCursor;
					desc.bitsAndVertexCount =
						((meshlet.vertex_count & 0xFFu) << 24u);

					const int32_t tag = meshletBucketTag[mi];
					const uint32_t refinedGroupEncoded = (tag >= 0) ? static_cast<uint32_t>(tag + 1) : 0u;
					desc.triangleCountAndRefinedGroup =
						(meshlet.triangle_count & 0xFFFFu)
						| (refinedGroupEncoded << 16u);
					const bool boneOverflow = comp.boneList.size() > nodeBoneLimit;
					const uint32_t cullFlags = comp.boneList.empty()
						? 0u
						: CLOD_CLUSTER_CULL_FLAG_ANIMATED |
							(boneOverflow ? CLOD_CLUSTER_CULL_FLAG_BONE_OVERFLOW : 0u);
					desc.boneCount = CLodPackClusterCullMetadata(
						CLOD_CLUSTER_KIND_TRIANGLE,
						cullFlags,
						boneOverflow ? 0u : static_cast<uint32_t>(comp.boneList.size()));
					desc.sourceGroupLocalIndex = sourceGroupLocalIndex;

					const BoundingSphere& bounds = output.meshletBounds[mi];
					desc.bounds = bounds.sphere;
					float minTerrainX = (std::numeric_limits<float>::max)();
					float minTerrainY = (std::numeric_limits<float>::max)();
					float maxTerrainX = -(std::numeric_limits<float>::max)();
					float maxTerrainY = -(std::numeric_limits<float>::max)();
					for (uint32_t vi = 0; vi < meshlet.vertex_count; ++vi)
					{
						const uint32_t gv = output.meshletVertices[meshlet.vertex_offset + vi];
						const DirectX::XMFLOAT3& position = groupPositions[gv];
						minTerrainX = std::min(minTerrainX, position.x);
						minTerrainY = std::min(minTerrainY, -position.z);
						maxTerrainX = std::max(maxTerrainX, position.x);
						maxTerrainY = std::max(maxTerrainY, -position.z);
					}
					desc.terrainRvtLocalSkyrimXYRadius = meshlet.vertex_count > 0u
						? 0.5f * std::max(maxTerrainX - minTerrainX, maxTerrainY - minTerrainY)
						: bounds.sphere.w;
					if (capturedGroup.writeMaterialSlots)
					{
						desc.terrainRvtLocalSkyrimXYRadius = std::bit_cast<float>(
							mi < output.meshletMaterialSlots.size() ? output.meshletMaterialSlots[mi] : 0u);
					}
					if (pageHasUvSets)
					{
						for (uint32_t uvSetIndex = 0; uvSetIndex < page.uvSetCount; ++uvSetIndex)
						{
							CLodMeshletUvDescriptor uvDesc{};
							uvDesc.uvBitOffset = static_cast<uint32_t>(pageUvBitCursors[uvSetIndex]);
							if (uvSetIndex < comp.uvSets.size())
							{
								const PerUvSetCompression& uvComp = comp.uvSets[uvSetIndex];
								uvDesc.uvMinU = uvComp.uvMinU;
								uvDesc.uvMinV = uvComp.uvMinV;
								uvDesc.uvScaleU = uvComp.uvScaleU;
								uvDesc.uvScaleV = uvComp.uvScaleV;
								uvDesc.uvBits = (uvComp.uvBitsU & 0xFFu) | ((uvComp.uvBitsV & 0xFFu) << 8u);
							}
							else
							{
								uvDesc.uvMinU = 0.0f;
								uvDesc.uvMinV = 0.0f;
								uvDesc.uvScaleU = 0.0f;
								uvDesc.uvScaleV = 0.0f;
								uvDesc.uvBits = 1u | (1u << 8u);
							}
							std::memcpy(
								blob.data() + uvDescriptorOffset + (static_cast<size_t>(li) * static_cast<size_t>(page.uvSetCount) + uvSetIndex) * sizeof(CLodMeshletUvDescriptor),
								&uvDesc,
								sizeof(CLodMeshletUvDescriptor));
						}
					}
					std::memcpy(
						blob.data() + descriptorOffset + static_cast<size_t>(li) * sizeof(CLodMeshletDescriptor),
						&desc,
						sizeof(CLodMeshletDescriptor));

					// Append per-meshlet native positions to the shared page stream.
					for (uint32_t vi = 0; vi < meshlet.vertex_count; ++vi)
					{
						const uint32_t gv = output.meshletVertices[meshlet.vertex_offset + vi];
						std::memcpy(
							blob.data() + positionBitstreamOffset + pagePositionByteCursor + static_cast<size_t>(vi) * sizeof(DirectX::XMFLOAT3),
							&groupPositions[gv],
							sizeof(DirectX::XMFLOAT3));
					}
					pagePositionByteCursor += GetMeshletPositionBytes(meshlet);

					// Append per-meshlet compressed normals
					if (pageHasNormals)
					{
						for (uint32_t vi = 0; vi < meshlet.vertex_count; ++vi)
						{
							uint32_t normalWord = 0u;
							if ((comp.attributeMask & CLOD_PAGE_ATTRIBUTE_NORMAL) != 0u)
							{
								const uint32_t gv = output.meshletVertices[meshlet.vertex_offset + vi];
								normalWord = compressedNormalWords[gv];
							}
							std::memcpy(blob.data() + normalArrayOffset + static_cast<size_t>(vertexAttributeCursor + vi) * sizeof(uint32_t),
								&normalWord, sizeof(uint32_t));
						}
					}
					if (pageHasTangentFrames)
					{
						for (uint32_t vi = 0; vi < meshlet.vertex_count; ++vi)
						{
							uint32_t tangentFrameWord = 0u;
							if ((comp.attributeMask & CLOD_PAGE_ATTRIBUTE_TANGENT_FRAME) != 0u)
							{
								const uint32_t gv = output.meshletVertices[meshlet.vertex_offset + vi];
								tangentFrameWord = compressedTangentFrameWords[gv];
							}
							std::memcpy(blob.data() + tangentFrameArrayOffset + static_cast<size_t>(vertexAttributeCursor + vi) * sizeof(uint32_t),
								&tangentFrameWord, sizeof(uint32_t));
						}
					}
					if (pageHasColors)
					{
						for (uint32_t vi = 0; vi < meshlet.vertex_count; ++vi)
						{
							uint32_t colorWord = 0xFFFFFFFFu;
							if ((comp.attributeMask & CLOD_PAGE_ATTRIBUTE_COLOR) != 0u)
							{
								const uint32_t gv = output.meshletVertices[meshlet.vertex_offset + vi];
								colorWord = compressedColorWords[gv];
							}
							std::memcpy(blob.data() + colorArrayOffset + static_cast<size_t>(vertexAttributeCursor + vi) * sizeof(uint32_t),
								&colorWord, sizeof(uint32_t));
						}
					}
					if (pageHasJoints)
					{
						for (uint32_t vi = 0; vi < meshlet.vertex_count; ++vi)
						{
							PackedSkinningInfluences skinning{};
							if ((comp.attributeMask & CLOD_PAGE_ATTRIBUTE_JOINTS) != 0u)
							{
								const uint32_t gv = output.meshletVertices[meshlet.vertex_offset + vi];
								skinning = groupSkinningInfluences[gv];
							}
							std::memcpy(blob.data() + jointArrayOffset + static_cast<size_t>(vertexAttributeCursor + vi) * sizeof(DirectX::XMUINT4) * 2u,
								&skinning.joints0, sizeof(DirectX::XMUINT4) * 2u);
						}
					}
					if (pageHasWeights)
					{
						for (uint32_t vi = 0; vi < meshlet.vertex_count; ++vi)
						{
							PackedSkinningInfluences skinning{};
							if ((comp.attributeMask & CLOD_PAGE_ATTRIBUTE_WEIGHTS) != 0u)
							{
								const uint32_t gv = output.meshletVertices[meshlet.vertex_offset + vi];
								skinning = groupSkinningInfluences[gv];
							}
							std::memcpy(blob.data() + weightArrayOffset + static_cast<size_t>(vertexAttributeCursor + vi) * sizeof(DirectX::XMFLOAT4) * 2u,
								&skinning.weights0, sizeof(DirectX::XMFLOAT4) * 2u);
						}
					}
					if (!comp.boneList.empty() && comp.boneList.size() <= nodeBoneLimit)
					{
						std::memcpy(blob.data() + boneIndexStreamOffset + static_cast<size_t>(boneIndexCursor) * sizeof(uint32_t),
							comp.boneList.data(),
							comp.boneList.size() * sizeof(uint32_t));
						boneIndexCursor += static_cast<uint32_t>(comp.boneList.size());
					}
					vertexAttributeCursor += meshlet.vertex_count;

					if (pageHasUvSets)
					{
						for (uint32_t uvSetIndex = 0; uvSetIndex < page.uvSetCount; ++uvSetIndex)
						{
							const bool meshletHasUv = uvSetIndex < comp.uvSets.size();
							const uint32_t uvBitsU = meshletHasUv ? comp.uvSets[uvSetIndex].uvBitsU : 1u;
							const uint32_t uvBitsV = meshletHasUv ? comp.uvSets[uvSetIndex].uvBitsV : 1u;
							for (uint32_t vi = 0; vi < meshlet.vertex_count; ++vi)
							{
								uint32_t encodedU = 0u;
								uint32_t encodedV = 0u;
								if (meshletHasUv)
								{
									const uint32_t gv = output.meshletVertices[meshlet.vertex_offset + vi];
									const DirectX::XMFLOAT2 uv = groupUvSets[uvSetIndex].values[gv];
									const PerUvSetCompression& uvComp = comp.uvSets[uvSetIndex];
									const float offsetU = std::max(0.0f, uv.x - uvComp.uvMinU);
									const float offsetV = std::max(0.0f, uv.y - uvComp.uvMinV);
									const uint32_t maxEncodedU = (uvBitsU >= 32u) ? 0xFFFFFFFFu : ((1u << uvBitsU) - 1u);
									const uint32_t maxEncodedV = (uvBitsV >= 32u) ? 0xFFFFFFFFu : ((1u << uvBitsV) - 1u);
									encodedU = std::min(maxEncodedU, QuantizeUvOffset(offsetU));
									encodedV = std::min(maxEncodedV, QuantizeUvOffset(offsetV));
								}

								AppendBitsPreSized(pageUvWordsPerSet[uvSetIndex], pageUvBitCursors[uvSetIndex], encodedU, uvBitsU);
								AppendBitsPreSized(pageUvWordsPerSet[uvSetIndex], pageUvBitCursors[uvSetIndex], encodedV, uvBitsV);
							}
						}
					}

					// Append per-meshlet triangle bytes (already meshlet-local 0..vertexCount-1)
					const uint32_t triBytes = meshlet.triangle_count * 3u;
					std::memcpy(blob.data() + triangleStreamOffset + triangleByteCursor,
						output.meshletTriangles.data() + meshlet.triangle_offset,
						triBytes);
					triangleByteCursor += triBytes;
				}

				if (pageHasUvSets)
				{
					std::memcpy(blob.data() + uvBitstreamDirectoryOffset,
						uvBitstreamOffsets.data(),
						static_cast<size_t>(page.uvSetCount) * sizeof(uint32_t));
					for (uint32_t uvSetIndex = 0; uvSetIndex < page.uvSetCount; ++uvSetIndex)
					{
						if (!pageUvWordsPerSet[uvSetIndex].empty())
						{
							std::memcpy(blob.data() + uvBitstreamOffsets[uvSetIndex],
								pageUvWordsPerSet[uvSetIndex].data(),
								pageUvWordsPerSet[uvSetIndex].size() * sizeof(uint32_t));
						}
					}
				}

				// Write header
				CLodPageHeader header{};
				header.meshletCount = pageMeshletCount;
				header.compressedPositionQuantExp = CLOD_NATIVE_POSITION_FORMAT;
				header.attributeMask = page.attributeMask;
				header.uvSetCount = page.uvSetCount;
				header.descriptorOffset = descriptorOffset;
				header.uvDescriptorOffset = uvDescriptorOffset;
				header.positionBitstreamOffset = positionBitstreamOffset;
				header.normalArrayOffset = normalArrayOffset;
				header.colorArrayOffset = colorArrayOffset;
				header.jointArrayOffset = jointArrayOffset;
				header.weightArrayOffset = weightArrayOffset;
				header.uvBitstreamDirectoryOffset = uvBitstreamDirectoryOffset;
				header.triangleStreamOffset = triangleStreamOffset;
				header.boneIndexStreamOffset = boneIndexStreamOffset;
				header.tangentFrameArrayOffset = tangentFrameArrayOffset;
				std::memcpy(blob.data(), &header, sizeof(CLodPageHeader));

				assert(blob.size() <= CLOD_PAGE_SIZE && "Page blob exceeds CLOD_PAGE_SIZE");
			}
		}

		output.meshletRefinedGroups = std::move(meshletBucketTag);
		output.vertexGlobalIds = std::move(groupLocalToGlobal);

		return output;
	}



}

ClusterLODPrebuildArtifacts BuildClusterLODArtifactsFromGeometry(
	const std::vector<std::byte>& vertices,
	unsigned int vertexSize,
	const std::vector<std::byte>* skinningVertices,
	unsigned int skinningVertexSize,
	const std::vector<uint32_t>& indices,
	const std::vector<MeshUvSetData>& uvSets,
	unsigned int flags,
	const ClusterLODBuilderSettings& settings,
	const VoxelCoverageMaterialSampler* coverageMaterialSampler,
	const ClusterLODMaterialAppearance* materialAppearance)
{
	ZoneScopedN("ClusterLODUtilities::BuildClusterLODArtifactsFromGeometry");
	CLodCache::ThrowIfCacheOnly("BuildClusterLODArtifactsFromGeometry");
	ClusterLODBuildState state{};

	const unsigned int* idx = reinterpret_cast<const unsigned int*>(indices.data());

	const size_t vertexStrideBytes = vertexSize;
	const size_t globalVertexCount = vertices.size() / vertexStrideBytes;
	TracyPlot("CLOD.Build.Vertices", static_cast<int64_t>(globalVertexCount));
	TracyPlot("CLOD.Build.Triangles", static_cast<int64_t>(indices.size() / 3u));
	const uint32_t meshPositionQuantExp = ComputeMeshQuantizationExponent(vertices, vertexStrideBytes);
	const float meshPositionQuantScale = static_cast<float>(1u << meshPositionQuantExp);

	const bool enableNormalAttributeSimplification = settings.enableNormalAttributeSimplification;
	const float normalAttributeWeight = std::max(0.0f, settings.normalAttributeWeight);
	const float tangentAttributeWeight = std::max(0.0f, settings.simplifyTangentWeight);
	const float tangentSignAttributeWeight = std::max(0.0f, settings.simplifyTangentSignWeight);
	const bool hasNormalStreamInSource = (flags & VertexFlags::VERTEX_NORMALS) != 0u &&
		vertexStrideBytes >= MeshVertexLayout::NormalOffset + sizeof(float) * 3;
	const bool hasTexcoordStreamInSource = (flags & VertexFlags::VERTEX_TEXCOORDS) != 0u &&
		vertexStrideBytes >= MeshVertexLayout::TexcoordOffset(flags) + sizeof(float) * 2;
	const bool recomputeGroupNormals = hasNormalStreamInSource && !settings.preserveImportedNormals;
	std::vector<float> simplifyAttributeStream;
	std::vector<float> simplifyAttributeWeights;
	uint32_t simplifyAttributeCount = 0;
	uint32_t simplifyProtectMask = 0;
	std::vector<DirectX::XMFLOAT4> tangentAttributeStream;

	if (enableNormalAttributeSimplification &&
		(tangentAttributeWeight > 0.0f || tangentSignAttributeWeight > 0.0f) &&
		hasNormalStreamInSource &&
		hasTexcoordStreamInSource)
	{
		ZoneScopedN("ClusterLODUtilities::Build::GenerateMeshoptTangents");
		if (!GenerateMeshoptTangents(vertices, vertexStrideBytes, indices, tangentAttributeStream))
		{
			spdlog::warn("ClusterLOD: failed to generate meshoptimizer tangents; continuing without tangent simplification attributes");
			tangentAttributeStream.clear();
		}
	}

	if (enableNormalAttributeSimplification && hasNormalStreamInSource)
	{
		simplifyAttributeWeights.push_back(normalAttributeWeight);
		simplifyAttributeWeights.push_back(normalAttributeWeight);
		simplifyAttributeWeights.push_back(normalAttributeWeight);
		simplifyProtectMask |= ((1u << 3u) - 1u) << simplifyAttributeCount;
		simplifyAttributeCount += 3u;
	}

	if (enableNormalAttributeSimplification && !tangentAttributeStream.empty())
	{
		simplifyAttributeWeights.push_back(tangentAttributeWeight);
		simplifyAttributeWeights.push_back(tangentAttributeWeight);
		simplifyAttributeWeights.push_back(tangentAttributeWeight);
		simplifyAttributeWeights.push_back(tangentSignAttributeWeight);
		simplifyProtectMask |= ((1u << 4u) - 1u) << simplifyAttributeCount;
		simplifyAttributeCount += 4u;
	}

	// Replacement vertices (stage 2, docs/clod-simplification-validation.md): normals are fitted from the original, so they need
	// imported normals that the output keeps; skinned meshes are left alone (moved positions would need new skin weights).
	const bool hasSkinningStream = skinningVertices != nullptr && !skinningVertices->empty();
	// Optimal placement (stage 4) commits replacement vertices through the same machinery, so it turns them on, with their restrictions.
	// Stage 5's topology candidates are update candidates too.
	const bool wantsReplacements = settings.simplifyUpdate || settings.simplifyOptimal || settings.simplifyTopology;
	const bool simplifyUpdate = wantsReplacements && hasNormalStreamInSource && !hasSkinningStream && settings.preserveImportedNormals;
	const bool simplifyOptimal = simplifyUpdate && settings.simplifyOptimal;
	const bool simplifyTopology = simplifyUpdate && settings.simplifyTopology;
	// Last-resort fit: the sloppy last resort's results get their normals refitted through the same fitter. On meshes stage 2
	// leaves alone for their skinning it is the only user (no vertex moves; a replacement copies its parent's skinning row).
	const bool lastResortFit = settings.disableSloppyFallback && settings.sloppyLastResort && settings.sloppyLastResortFit && !settings.simplifyValidation &&
		hasNormalStreamInSource && settings.preserveImportedNormals;
	const bool replacementsActive = simplifyUpdate || lastResortFit;
	if (wantsReplacements && !simplifyUpdate)
	{
		spdlog::info("ClusterLOD: replacement vertices off for this mesh (normals={} skinning={} preserveImportedNormals={})",
			hasNormalStreamInSource ? 1 : 0, hasSkinningStream ? 1 : 0, settings.preserveImportedNormals ? 1 : 0);
	}
	// Moving a vertex moves its UVs with it: the update solves them as attributes after the ranked ones, weighted so lightly that
	// they hardly change which collapses happen. The record texcoord first, then the UV sets, as many as meshoptimizer's 32
	// attributes allow.
	constexpr float kUpdateUvWeight = 0.01f;
	bool updateTexcoordSolved = false;
	uint32_t updateUvSetsSolved = 0u;
	std::vector<float> updateAttributeWeights;
	if (simplifyUpdate && settings.simplifyUpdatePositions)
	{
		uint32_t budget = std::min<uint32_t>((32u - std::min(simplifyAttributeCount, 32u)) / 2u, 16u);
		if (hasTexcoordStreamInSource && budget > 0u)
		{
			updateTexcoordSolved = true;
			--budget;
		}
		while (updateUvSetsSolved < uvSets.size() && updateUvSetsSolved < budget && uvSets[updateUvSetsSolved].values.size() == globalVertexCount)
		{
			++updateUvSetsSolved;
		}
		updateAttributeWeights.assign(((updateTexcoordSolved ? 1u : 0u) + updateUvSetsSolved) * 2u, kUpdateUvWeight);
	}
	const uint32_t simplifyAttributeStride = simplifyAttributeCount + static_cast<uint32_t>(updateAttributeWeights.size());

	if (simplifyAttributeStride > 0u)
	{
		ZoneScopedN("ClusterLODUtilities::Build::BuildSimplifyAttributes");
		simplifyAttributeStream.resize(globalVertexCount * static_cast<size_t>(simplifyAttributeStride));
		for (size_t vertexIndex = 0; vertexIndex < globalVertexCount; ++vertexIndex)
		{
			size_t destinationFloatOffset = vertexIndex * static_cast<size_t>(simplifyAttributeStride);

			if (enableNormalAttributeSimplification && hasNormalStreamInSource)
			{
				const size_t normalSourceByteOffset = vertexIndex * vertexStrideBytes + MeshVertexLayout::NormalOffset;
				std::memcpy(&simplifyAttributeStream[destinationFloatOffset], vertices.data() + normalSourceByteOffset, sizeof(float) * 3);
				destinationFloatOffset += 3ull;
			}

			if (enableNormalAttributeSimplification && !tangentAttributeStream.empty())
			{
				const DirectX::XMFLOAT4 tangent = tangentAttributeStream[vertexIndex];
				simplifyAttributeStream[destinationFloatOffset + 0ull] = tangent.x;
				simplifyAttributeStream[destinationFloatOffset + 1ull] = tangent.y;
				simplifyAttributeStream[destinationFloatOffset + 2ull] = tangent.z;
				simplifyAttributeStream[destinationFloatOffset + 3ull] = tangent.w;
				destinationFloatOffset += 4ull;
			}

			if (updateTexcoordSolved)
			{
				const size_t texcoordSourceByteOffset = vertexIndex * vertexStrideBytes + MeshVertexLayout::TexcoordOffset(flags);
				std::memcpy(&simplifyAttributeStream[destinationFloatOffset], vertices.data() + texcoordSourceByteOffset, sizeof(float) * 2);
				destinationFloatOffset += 2ull;
			}

			for (uint32_t set = 0; set < updateUvSetsSolved; ++set)
			{
				simplifyAttributeStream[destinationFloatOffset + 0ull] = uvSets[set].values[vertexIndex].x;
				simplifyAttributeStream[destinationFloatOffset + 1ull] = uvSets[set].values[vertexIndex].y;
				destinationFloatOffset += 2ull;
			}
		}
	}

	clodMesh mesh{};
	std::vector<unsigned char> coveragePriorityFlags;
	if (settings.coveragePreservationMode == ClusterLODCoveragePreservationMode::PrioritizeEdges)
	{
		coveragePriorityFlags = BuildExteriorEdgePriorityFlags(vertices, vertexStrideBytes, indices);
	}
	mesh.indices = idx;
	mesh.index_count = indices.size();
	mesh.vertex_count = globalVertexCount;
	mesh.vertex_positions = reinterpret_cast<const float*>(vertices.data());
	mesh.vertex_positions_stride = vertexStrideBytes;

	mesh.vertex_attributes = simplifyAttributeStream.empty() ? nullptr : simplifyAttributeStream.data();
	mesh.vertex_attributes_stride = simplifyAttributeStream.empty() ? 0 : sizeof(float) * simplifyAttributeStride;
	mesh.vertex_lock = coveragePriorityFlags.empty() ? nullptr : coveragePriorityFlags.data();
	mesh.attribute_weights = simplifyAttributeWeights.empty() ? nullptr : simplifyAttributeWeights.data();
	mesh.attribute_count = simplifyAttributeStream.empty() ? 0 : simplifyAttributeCount;
	mesh.attribute_protect_mask = simplifyAttributeStream.empty() ? 0 : simplifyProtectMask;
	mesh.update_attribute_weights = updateAttributeWeights.empty() ? nullptr : updateAttributeWeights.data();
	mesh.update_attribute_count = updateAttributeWeights.size();

	// The build vertex buffer. With replacement vertices it is a builder-owned copy of the source that grows as groups commit new
	// vertices (between levels, while no task reads it); the source itself stays untouched, for validation's original.
	clod_detail::ReplacementVertexStore replacementStore;
	std::vector<std::byte> replacementSkinning; // the skinning stream grown with the replacements (last-resort fit on skinned meshes)
	if (replacementsActive)
	{
		ZoneScopedN("ClusterLODUtilities::Build::CopyReplacementStore");
		replacementStore.vertices.reserve(vertices.size() + vertices.size() / 2u);
		replacementStore.vertices = vertices;
		replacementStore.uvSets = uvSets;
		replacementStore.attributes = simplifyAttributeStream.empty() ? nullptr : &simplifyAttributeStream;
		replacementStore.vertexLock = coveragePriorityFlags.empty() ? nullptr : &coveragePriorityFlags;
		replacementStore.sourceVertexCount = globalVertexCount;
		mesh.vertex_positions = reinterpret_cast<const float*>(replacementStore.vertices.data());
		if (hasSkinningStream && skinningVertexSize >= sizeof(float) * 6u)
		{
			replacementSkinning = *skinningVertices;
			replacementStore.skinning = &replacementSkinning;
			replacementStore.skinningStride = skinningVertexSize;
			replacementStore.skinningNormalOffset = sizeof(float) * 3u; // position, normal, influences
		}
	}
	const std::vector<std::byte>& buildVertices = replacementsActive ? replacementStore.vertices : vertices;
	const std::vector<MeshUvSetData>& buildUvSets = replacementsActive ? replacementStore.uvSets : uvSets;
	const std::vector<std::byte>* buildSkinning = replacementStore.skinning != nullptr ? replacementStore.skinning : skinningVertices;

	clodConfig config = clodDefaultConfig(/*max_triangles=*/MS_MESHLET_SIZE);
	config.max_vertices = MS_MESHLET_SIZE;
	config.max_triangles = MS_MESHLET_SIZE;
	config.min_triangles = MS_MESHLET_MIN_SIZE;
	config.cluster_spatial = true;
	config.cluster_fill_weight = 0.5f;
	config.cluster_split_factor = 2.0f;
	config.partition_spatial = true;
	config.partition_sort = true;
	config.optimize_clusters = true;
	config.optimize_bounds = true;

	const bool disableSloppyFallback = settings.disableSloppyFallback;
	const float sloppyFallbackErrorFactor = std::max(1.0f, settings.sloppyFallbackErrorFactor);
	const float lodErrorMergeAdditive = std::max(0.0f, settings.lodErrorMergeAdditive);
	const float lodErrorMergePrevious = std::max(0.0f, settings.lodErrorMergePrevious);
	const uint32_t partitionSizeFloor = std::max<uint32_t>(1u, settings.partitionSizeFloor);

	config.simplify_fallback_sloppy = !disableSloppyFallback; // TODO: Useful?
	config.simplify_last_resort_sloppy = disableSloppyFallback && settings.sloppyLastResort;
	config.simplify_last_resort_rejected = settings.sloppyLastResortRejected;
	config.simplify_error_factor_last_resort = std::max(0.0f, settings.sloppyLastResortErrorFactor);
	config.simplify_error_factor_sloppy = sloppyFallbackErrorFactor; // Scales error for sloppy groups

	config.simplify_permissive = settings.simplifyPermissive;
	config.simplify_fallback_permissive = false; // Simplify in permissive, disable fallback-only

	config.simplify_error_merge_additive = lodErrorMergeAdditive;
	config.simplify_error_merge_previous = lodErrorMergePrevious;

	// Validation of simplification results against the original surface (docs/clod-simplification-validation.md): clusterlod
	// decides what to retry, this measures each candidate. Candidates index the build vertex buffer (the source, plus any
	// replacement vertices), so one view of it serves every group input and every candidate; an update candidate brings its own
	// private positions and normals. The original is the untouched source.
	static_assert(uint32_t(clod_detail::SimplifyValidationReject_Fold) == uint32_t(clodValidateReject_Fold) &&
		uint32_t(clod_detail::SimplifyValidationReject_Shading) == uint32_t(clodValidateReject_Shading) &&
		uint32_t(clod_detail::SimplifyValidationReject_Coverage) == uint32_t(clodValidateReject_Coverage) &&
		uint32_t(clod_detail::SimplifyValidationReject_Spurious) == uint32_t(clodValidateReject_Spurious) &&
		uint32_t(clod_detail::SimplifyValidationReject_SourceCoverage) == uint32_t(clodValidateReject_SourceCoverage));
	struct SimplifyValidationContext
	{
		std::optional<clod_detail::SimplifyValidator> validator;
		clod_detail::SimplifySurfaceView source;
		// The build vertex buffer; it grows only between levels, so its data pointer is read at each call.
		const std::vector<std::byte>* buildVertices = nullptr;
		std::atomic<uint64_t> nanoseconds = 0;
		bool logCandidates = false;
		// Stage 5: topology candidates' texture check (textureArea < 1 and texcoords present), and its counters.
		clod_detail::TextureCheckSource textureSource;
		float textureArea = 1.0f;
		float textureSlide = 8.0f;
		std::atomic<uint64_t> textureChecks = 0;
		std::atomic<uint64_t> textureNanoseconds = 0;
		// Follow-ups to the texture check: a UV-space tolerance (texels over a nominal resolution; 0 off) and chart identity (the
		// build vertices' charts, null off). Each candidate the check rejects is also counted under every test that alone rejects it.
		float textureUvTolerance = 0.0f;
		const std::vector<uint32_t>* textureCharts = nullptr;
		std::atomic<uint64_t> textureRejectedSlide = 0;
		std::atomic<uint64_t> textureRejectedUv = 0;
		std::atomic<uint64_t> textureRejectedCharts = 0;
		// Stage 6 step 4: the chart test weighs displaced samples by the colour they draw (null: binary), and how many candidates the
		// binary test would have rejected that the weighed one accepted.
		const clod_detail::ErrorChannelTextureColor* textureColor = nullptr;
		std::atomic<uint64_t> textureAcceptedColor = 0;

		clod_detail::SimplifySurfaceView Build(const unsigned int* indices, size_t indexCount) const
		{
			clod_detail::SimplifySurfaceView view = source;
			const std::byte* base = buildVertices->data();
			view.positions = reinterpret_cast<const float*>(base);
			view.normals = source.normals ? reinterpret_cast<const float*>(base + MeshVertexLayout::NormalOffset) : nullptr;
			view.indices = indices;
			view.indexCount = indexCount;
			return view;
		}

		static unsigned int Validate(void* context, const clodMesh*, const clodValidateCandidate* candidate)
		{
			SimplifyValidationContext* self = static_cast<SimplifyValidationContext*>(context);
			const auto start = std::chrono::steady_clock::now();
			const clod_detail::SimplifySurfaceView input = self->Build(candidate->input_indices, candidate->input_index_count);
			clod_detail::SimplifySurfaceView output = self->Build(candidate->output_indices, candidate->output_index_count);
			if (candidate->output_positions)
			{
				output.positions = candidate->output_positions;
				output.positionStride = sizeof(float) * 3u;
				output.normals = self->source.normals ? candidate->output_normals : nullptr;
				output.normalStride = sizeof(float) * 3u;
			}
			// A group's candidates run in turn on one task, attempt 0 first, and share their input: its baseline is measured once.
			// A depth-0 group's input is the original.
			thread_local clod_detail::SimplifyInputBaseline baseline;
			if (candidate->attempt == 0)
			{
				baseline = candidate->depth == 0 ? clod_detail::SimplifyInputBaseline::Original() : clod_detail::SimplifyInputBaseline{};
			}
			clod_detail::SimplifyValidationMeasurement measurement;
			uint32_t reasons = self->validator->Validate(
				input, output, candidate->error, &baseline, &measurement, static_cast<uint32_t>(candidate->group));
			if (self->logCandidates)
			{
				const double outputArea = std::max(measurement.outputArea, 1.0e-30);
				const double inputArea = std::max(measurement.inputArea, 1.0e-30);
				const double baselineArea = std::max(baseline.area, 1.0e-30);
				spdlog::info(
					"ClusterLOD simplify candidate: depth={} attempt={} method={} reasons={} input_triangles={} output_triangles={} "
					"error={:.4g} radius={:.4g} fold={:.4f} shading={:.4f} unmatched={:.4f} uncovered={:.4f} source_uncovered={:.4f} input_fold={:.4f} "
					"input_shading={:.4f} stopped_early={}",
					candidate->depth, candidate->attempt, candidate->method, reasons, candidate->input_index_count / 3u,
					candidate->output_index_count / 3u, candidate->error, measurement.radius, measurement.foldArea / outputArea,
					measurement.shadingArea / outputArea, measurement.unmatchedArea / outputArea, measurement.uncoveredArea / inputArea,
					measurement.sourceUncoveredArea / std::max(measurement.sourceArea, 1.0e-30),
					measurement.inputMeasured ? baseline.foldArea / baselineArea : -1.0,
					measurement.inputMeasured ? baseline.shadingArea / baselineArea : -1.0, measurement.stoppedEarly ? 1 : 0);
			}
			// Stage 5: a topology candidate (it brings its texcoords) must not slide the texture beyond the radius on too much area.
			if (reasons == 0 && candidate->output_uvs && self->textureArea < 1.0f && self->textureSource.uvs)
			{
				const auto textureStart = std::chrono::steady_clock::now();
				const float radius = self->validator->RadiusForError(candidate->error);
				const bool followUps = self->textureUvTolerance > 0.0f || self->textureCharts != nullptr;
				clod_detail::TextureDisplacement texture;
				if (!followUps)
				{
					clod_detail::TextureCheckOptions options;
					options.slideTolerance = radius * self->textureSlide;
					options.samplesPerTriangle = self->validator->Thresholds().samplesPerTriangle;
					texture = clod_detail::MeasureTextureDisplacement(*self->validator, self->textureSource, output, candidate->output_uvs, radius, options);
				}
				else
				{
					// The output vertices' charts: a private vertex is a build vertex (candidate->output_vertex_ids), whose chart the
					// store carries; the source's are the store's first entries. The store grows only between levels.
					thread_local std::vector<uint32_t> outputCharts;
					clod_detail::TextureCheckSource source = self->textureSource;
					clod_detail::TextureCheckOptions options;
					options.slideTolerance = radius * self->textureSlide;
					options.uvTolerance = self->textureUvTolerance;
					options.samplesPerTriangle = self->validator->Thresholds().samplesPerTriangle;
					if (self->textureCharts != nullptr && candidate->output_vertex_ids != nullptr)
					{
						outputCharts.resize(candidate->output_vertex_count);
						for (size_t k = 0; k < candidate->output_vertex_count; ++k)
						{
							outputCharts[k] = (*self->textureCharts)[candidate->output_vertex_ids[k]];
						}
						source.charts = self->textureCharts->data();
						options.outputCharts = outputCharts.data();
					}
					std::function<float(const clod_detail::TextureSampleQuery&)> colorWeight;
					if (self->textureColor != nullptr)
					{
						colorWeight = [&](const clod_detail::TextureSampleQuery& query) {
							return clod_detail::ColorSampleWeight(*self->textureColor, query, radius * std::max(self->textureColor->footprint, 0.0f));
						};
						options.sampleWeight = &colorWeight;
					}
					texture = clod_detail::MeasureTextureDisplacement(*self->validator, source, output, candidate->output_uvs, radius, options);
				}
				self->textureChecks.fetch_add(1u, std::memory_order_relaxed);
				self->textureNanoseconds.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - textureStart).count()), std::memory_order_relaxed);
				const double allowed = static_cast<double>(self->textureArea) * texture.area;
				// Stage 6 step 4: weighed by colour, the displaced area is what the crossing draws visibly wrong.
				const double displaced = followUps && self->textureColor != nullptr ? texture.weightedDisplacedArea : texture.displacedArea;
				if (displaced <= allowed && texture.displacedArea > allowed)
				{
					self->textureAcceptedColor.fetch_add(1u, std::memory_order_relaxed);
				}
				if (displaced > allowed)
				{
					reasons |= clodValidateReject_Texture;
					if (followUps)
					{
						self->textureRejectedSlide.fetch_add(texture.slideDisplacedArea > allowed ? 1u : 0u, std::memory_order_relaxed);
						self->textureRejectedUv.fetch_add(texture.uvDisplacedArea > allowed ? 1u : 0u, std::memory_order_relaxed);
						self->textureRejectedCharts.fetch_add(texture.chartArea > allowed ? 1u : 0u, std::memory_order_relaxed);
					}
				}
				if (self->logCandidates)
				{
					const double area = std::max(texture.area, 1.0e-30);
					spdlog::info("ClusterLOD topology texture: depth={} attempt={} output_triangles={} area={:.4g} measured={:.4f} displaced={:.4f} over1={:.4f} "
						"over2={:.4f} over4={:.4f} over8={:.4f} slide={:.4f} uv={:.4f} uv_over1={:.4f} uv_over4={:.4f} uv_over16={:.4f} uv_over64={:.4f} charts={:.4f} "
						"mixed={:.4f}", candidate->depth, candidate->attempt, candidate->output_index_count / 3u, texture.area,
						texture.measuredArea / area, texture.displacedArea / area, texture.slideOverArea[0] / area, texture.slideOverArea[1] / area,
						texture.slideOverArea[2] / area, texture.slideOverArea[3] / area, texture.slideDisplacedArea / area, texture.uvDisplacedArea / area,
						texture.uvOverArea[0] / area, texture.uvOverArea[1] / area, texture.uvOverArea[2] / area, texture.uvOverArea[3] / area,
						texture.chartArea / area, texture.mixedArea / area);
				}
			}
			self->nanoseconds.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
			return reasons;
		}

		static void Level(void* context, const clodMesh*, int, const unsigned int* indices, const unsigned int* triangleGroups, size_t indexCount)
		{
			SimplifyValidationContext* self = static_cast<SimplifyValidationContext*>(context);
			self->validator->SetLevel(self->Build(indices, indexCount), std::vector<uint32_t>(triangleGroups, triangleGroups + indexCount / 3u));
		}
	};
	SimplifyValidationContext simplifyValidation;
	simplifyValidation.buildVertices = &buildVertices;
	clodValidationStats simplifyValidationStats{};
	double simplifyValidationIndexMs = 0.0;
	// Replacement vertices need the original indexed too, for fitting normals and measuring errors, with or without validation.
	if (settings.simplifyValidation || replacementsActive)
	{
		ZoneScopedN("ClusterLODUtilities::Build::IndexOriginalForValidation");
		const auto start = std::chrono::steady_clock::now();
		constexpr float kDegreesToRadians = 3.14159265358979f / 180.0f;
		clod_detail::SimplifyValidationThresholds thresholds{};
		thresholds.foldCos = std::cos(std::clamp(settings.simplifyValidationFoldAngle, 0.0f, 180.0f) * kDegreesToRadians);
		thresholds.foldAreaFraction = std::max(0.0f, settings.simplifyValidationFoldArea);
		thresholds.shadingCos = std::cos(std::clamp(settings.simplifyValidationShadingAngle, 0.0f, 180.0f) * kDegreesToRadians);
		thresholds.shadingAreaFraction = std::max(0.0f, settings.simplifyValidationShadingArea);
		thresholds.radiusErrorScale = std::max(0.0f, settings.simplifyValidationRadiusScale);
		thresholds.coverageAreaFraction = std::max(0.0f, settings.simplifyValidationCoverageArea);
		thresholds.matchReachScale = std::max(1.0f, settings.simplifyMatchReach);
		thresholds.matchMaxCandidates = settings.simplifyMatchCandidates;
		thresholds.matchFacingGrid = ClusterLODMatchFacingGrid(settings);
		thresholds.samplesPerTriangle = clod_detail::AreaSampleCount(settings.simplifyValidationSamples);

		clod_detail::SimplifySurfaceView& source = simplifyValidation.source;
		source.positions = reinterpret_cast<const float*>(vertices.data());
		source.positionStride = vertexStrideBytes;
		source.normals = hasNormalStreamInSource ? reinterpret_cast<const float*>(vertices.data() + MeshVertexLayout::NormalOffset) : nullptr;
		source.normalStride = vertexStrideBytes;
		source.indices = idx;
		source.indexCount = indices.size();
		simplifyValidation.validator.emplace(source, thresholds);
		if (clod_detail::QueryStatsEnabled())
		{
			// Diagnostics: how much of the source repeats itself (overlapping sheets make the side-aware matches large). Triangles
			// whose corner positions, snapped to a grid of 1e-5 of the diagonal (the match slack), equal another's in any order.
			const float cell = std::max(simplifyValidation.validator->Diagonal() * 1.0e-5f, 1.0e-20f);
			std::unordered_map<uint64_t, uint32_t> seen;
			std::unordered_set<uint64_t> positions;
			seen.reserve(indices.size() / 3u);
			uint64_t duplicates = 0u, exactDuplicates = 0u;
			std::unordered_set<uint64_t> exact;
			for (size_t first = 0; first + 2u < indices.size(); first += 3u)
			{
				std::array<uint64_t, 3> snapped{}, bits{};
				for (uint32_t corner = 0; corner < 3u; ++corner)
				{
					const float* p = reinterpret_cast<const float*>(vertices.data() + static_cast<size_t>(idx[first + corner]) * vertexStrideBytes);
					uint64_t h = 1469598103934665603ull, e = 1469598103934665603ull;
					for (int axis = 0; axis < 3; ++axis)
					{
						h = (h ^ static_cast<uint64_t>(static_cast<int64_t>(std::floor(p[axis] / cell)))) * 1099511628211ull;
						e = (e ^ std::bit_cast<uint32_t>(p[axis])) * 1099511628211ull;
					}
					snapped[corner] = h;
					bits[corner] = e;
					positions.insert(e);
				}
				std::sort(snapped.begin(), snapped.end());
				std::sort(bits.begin(), bits.end());
				const uint64_t key = (snapped[0] * 31u + snapped[1]) * 31u + snapped[2];
				const uint64_t exactKey = (bits[0] * 31u + bits[1]) * 31u + bits[2];
				duplicates += seen[key]++ != 0u ? 1u : 0u;
				exactDuplicates += exact.insert(exactKey).second ? 0u : 1u;
			}
			uint32_t mostRepeated = 0u;
			for (const auto& [key, count] : seen)
			{
				mostRepeated = std::max(mostRepeated, count);
			}
			spdlog::info("ClusterLOD source overlap: triangles={} unique_positions={} duplicate_triangles_exact={} duplicate_triangles_snapped={} most_repeated={}",
				indices.size() / 3u, positions.size(), exactDuplicates, duplicates, mostRepeated);
		}
		// Diagnostics only (not hashed): one log line per candidate.
		simplifyValidation.logCandidates = !GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_SIMPLIFY_VALIDATION_LOG").empty();

		if (settings.simplifyValidation)
		{
			config.simplify_validate = &SimplifyValidationContext::Validate;
			config.simplify_validate_context = &simplifyValidation;
			config.simplify_validate_level = &SimplifyValidationContext::Level;
			config.simplify_validate_retries = static_cast<int>(std::min(settings.simplifyValidationRetries, 16u));
			config.simplify_validate_stats = &simplifyValidationStats;
		}
		simplifyValidationIndexMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}

	// Replacement vertices: clusterlod runs the update candidates and commits their vertices; the fitter owns their records.
	std::optional<clod_detail::ReplacementVertexFitter> replacementFitter;
	clodReplacementStats replacementStats{};
	if (replacementsActive)
	{
		clod_detail::ReplacementVertexLayout layout{};
		layout.vertexStride = vertexStrideBytes;
		layout.normalOffset = MeshVertexLayout::NormalOffset;
		const bool hasTangentStreamInSource = (flags & VertexFlags::VERTEX_TANGENTS) != 0u &&
			vertexStrideBytes >= MeshVertexLayout::TangentOffset(flags) + sizeof(float) * 4;
		layout.tangentOffset = hasTangentStreamInSource ? MeshVertexLayout::TangentOffset(flags) : SIZE_MAX;
		layout.texcoordOffset = hasTexcoordStreamInSource ? MeshVertexLayout::TexcoordOffset(flags) : SIZE_MAX;
		layout.attributeStride = simplifyAttributeStream.empty() ? 0u : simplifyAttributeStride;
		layout.normalAttribute = enableNormalAttributeSimplification && hasNormalStreamInSource ? 0 : -1;
		layout.tangentAttribute = enableNormalAttributeSimplification && !tangentAttributeStream.empty() ? (layout.normalAttribute >= 0 ? 3 : 0) : -1;
		layout.uvAttribute = simplifyAttributeCount;
		layout.texcoordSolved = updateTexcoordSolved;
		layout.uvSetsSolved = updateUvSetsSolved;
		constexpr float kDegreesToRadians = 3.14159265358979f / 180.0f;
		layout.refitCos = std::cos(std::clamp(settings.simplifyUpdateRefitAngle, 0.0f, 180.0f) * kDegreesToRadians);
		layout.transferUvs = settings.simplifyUpdateUvTransfer;
		layout.fitSamples = settings.simplifyUpdateFitSamples & 31u;
		layout.refitCheckCos = settings.simplifyUpdateRefitCheck > 0.0f
			? std::cos(std::min(settings.simplifyUpdateRefitCheck, 180.0f) * kDegreesToRadians) : -2.0f;
		replacementFitter.emplace(*simplifyValidation.validator, layout, replacementStore);

		struct ReplacementCallbacks
		{
			static void Fit(void* context, const clodMesh* mesh, clodReplacementCandidate* candidate)
			{
				static_cast<clod_detail::ReplacementVertexFitter*>(context)->Fit(*mesh, *candidate);
			}
			static void Commit(void* context, clodMesh* mesh, const clodReplacementBatch* batch)
			{
				static_cast<clod_detail::ReplacementVertexFitter*>(context)->Commit(*mesh, *batch);
			}
			static float Measure(void* context, const clodMesh*, const clodValidateCandidate* candidate)
			{
				return static_cast<clod_detail::ReplacementVertexFitter*>(context)->Measure(*candidate);
			}
		};
		config.simplify_update_fit = &ReplacementCallbacks::Fit;
		config.simplify_update_commit = &ReplacementCallbacks::Commit;
		config.simplify_update_context = &*replacementFitter;
		config.simplify_update_record_size = replacementFitter->RecordSize();
		config.simplify_update_positions = settings.simplifyUpdatePositions;
		config.simplify_measure = settings.simplifyUpdateMeasuredError ? &ReplacementCallbacks::Measure : nullptr;
		config.simplify_update_stats = &replacementStats;
		config.simplify_update_fit_last_resort_only = !simplifyUpdate;
		config.simplify_last_resort_fit = lastResortFit;
	}

	// Optimal placement (stage 4): clusterlod offers its candidate first; the original surface gives each candidate's source-sheet
	// references.
	std::optional<clod_detail::OptimalSimplifier> optimalSimplifier;
	if (simplifyOptimal)
	{
		constexpr float kDegreesToRadians = 3.14159265358979f / 180.0f;
		clod_detail::OptimalSimplifyParams params{};
		params.sheetCos = std::cos(std::clamp(settings.simplifyOptimalSheetAngle, 0.0f, 180.0f) * kDegreesToRadians);
		params.flipCos = std::cos(std::clamp(settings.simplifyOptimalFlipAngle, 0.0f, 180.0f) * kDegreesToRadians);
		params.preserveFolds = settings.simplifyOptimalPreserveFolds;
		params.moves = settings.simplifyOptimalMoves & 7u;
		optimalSimplifier.emplace(*simplifyValidation.validator, params);
		struct OptimalCallbacks
		{
			static size_t Simplify(void* context, const clodMesh* mesh, clodOptimalRequest* request)
			{
				return static_cast<const clod_detail::OptimalSimplifier*>(context)->Simplify(*mesh, *request);
			}
		};
		config.simplify_optimal = &OptimalCallbacks::Simplify;
		config.simplify_optimal_context = &*optimalSimplifier;
	}

	// Controlled topology changes (stage 5): a group about to be terminal tries update candidates with relaxed seams. They are
	// validated against the original by stage 1's validator: by clusterlod's simplify_validate when validation is on, otherwise
	// through simplify_topology with the same thresholds, which nothing else then uses. Diagnostics (not hashed; changes nothing
	// built) classify why terminal groups stop.
	clodTopologyStats topologyStats{};
	std::vector<uint32_t> textureCharts; // follow-up: per build vertex, grown by the commits (simplifyTopologyTextureCharts)
	// Stage 6 step 4: the colour the chart test weighs displaced samples by, and the decoded image (shared with the texture channel).
	clod_detail::ErrorChannelTextureColor topologyTextureColor;
	std::shared_ptr<const clod_texture::Texture> decodedBaseColor;
	const char* topologyTextureColorSource = "off";
	const bool topologyDiagnostics = !GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_TOPOLOGY_DIAG").empty();
	// Seams whose protected attributes differ only in the normal are relaxable when the mesh carries no UV streams to tell UV seams
	// apart (normals are attributes 0-2 when present); stage 2's fit rebuilds normals, not UVs.
	config.simplify_topology_relax_mask = enableNormalAttributeSimplification && hasNormalStreamInSource ? 7u : 0u;
	config.simplify_topology_relax_uv = settings.simplifyTopologyRelaxUv;
	config.simplify_topology_update_fallback = settings.simplifyTopologyUpdateFallback;
	if (topologyDiagnostics || simplifyTopology || config.simplify_last_resort_sloppy)
	{
		config.topology_stats = &topologyStats;
		config.topology_diagnostics = topologyDiagnostics;
	}
	if (simplifyTopology)
	{
		config.simplify_topology = &SimplifyValidationContext::Validate;
		config.simplify_topology_retries = static_cast<int>(std::min(settings.simplifyTopologyRetries, 16u));
		config.simplify_validate_context = &simplifyValidation;
		config.simplify_validate_level = &SimplifyValidationContext::Level;
		if (hasTexcoordStreamInSource && settings.simplifyTopologyTextureArea < 1.0f)
		{
			simplifyValidation.textureSource.positions = reinterpret_cast<const float*>(vertices.data());
			simplifyValidation.textureSource.positionStride = vertexStrideBytes;
			simplifyValidation.textureSource.uvs = reinterpret_cast<const float*>(vertices.data() + MeshVertexLayout::TexcoordOffset(flags));
			simplifyValidation.textureSource.uvStride = vertexStrideBytes;
			simplifyValidation.textureSource.indices = idx;
			simplifyValidation.textureSource.indexCount = indices.size();
			simplifyValidation.textureArea = std::max(0.0f, settings.simplifyTopologyTextureArea);
			simplifyValidation.textureSlide = std::max(0.0f, settings.simplifyTopologyTextureSlide);
			// Follow-ups: the builder is given no texture sizes, so the texel tolerance is over a nominal resolution (a UV distance).
			if (settings.simplifyTopologyTextureTexels > 0.0f)
			{
				simplifyValidation.textureUvTolerance = settings.simplifyTopologyTextureTexels / static_cast<float>(std::max(settings.simplifyTopologyTextureResolution, 1u));
			}
			if (settings.simplifyTopologyTextureCharts)
			{
				textureCharts = clod_detail::ComputeTextureCharts(simplifyValidation.textureSource.positions, vertexStrideBytes,
					simplifyValidation.textureSource.uvs, vertexStrideBytes, globalVertexCount, idx, indices.size());
				replacementStore.charts = &textureCharts;
				simplifyValidation.textureCharts = &textureCharts;
				// Stage 6 step 4: weigh the chart test's displaced samples by colour, from the importer's material; any reason it
				// cannot leaves the test binary for this mesh, and says why.
				if (ClusterLODTopologyTextureColorActive(settings))
				{
					if (materialAppearance == nullptr)
					{
						topologyTextureColorSource = "binary (the importer gave no material)";
					}
					else if (materialAppearance->baseColorTexCoord != 0u)
					{
						topologyTextureColorSource = "binary (the base colour reads another UV set)";
					}
					else if (materialAppearance->hasVertexColors)
					{
						topologyTextureColorSource = "binary (vertex colours)";
					}
					else
					{
						if (materialAppearance->hasBaseColorTexture && materialAppearance->decodeBaseColor)
						{
							decodedBaseColor = materialAppearance->decodeBaseColor();
						}
						if (materialAppearance->hasBaseColorTexture && (decodedBaseColor == nullptr || decodedBaseColor->Levels() == 0u))
						{
							topologyTextureColorSource = "binary (the base colour image did not decode)";
						}
						else
						{
							topologyTextureColor.baseColorFactor = materialAppearance->baseColorFactor;
							topologyTextureColor.texture = decodedBaseColor.get();
							topologyTextureColor.alphaMasked = materialAppearance->alphaMasked;
							topologyTextureColor.alphaCutoff = materialAppearance->alphaCutoff;
							topologyTextureColor.uvTransformed = materialAppearance->hasBaseColorUvTransform;
							topologyTextureColor.uvTransform = materialAppearance->baseColorUvTransform;
							topologyTextureColor.referenceDeltaE = settings.simplifyTopologyTextureColor;
							topologyTextureColor.footprint = 1.0f;
							simplifyValidation.textureColor = &topologyTextureColor;
							topologyTextureColorSource = topologyTextureColor.texture != nullptr ? "texture" : "factor";
						}
					}
				}
			}
		}
	}

	// Stage 6 texture channel (docs/clod-appearance-stage6.md): the appearance channel also counts texture displaced against the
	// original's UV set 0 (the set glTF pages store and materials sample; the record texcoord without sets). It needs every build
	// vertex's chart, grown by replacement commits like the chart test's, and each group vertex's build vertex id. Neither changes
	// what is built: charts only label vertices, and the ids are kept beside the group streams.
	const bool textureChannel = settings.lodErrorChannels && settings.lodErrorChannelsTexture > 0.0f && hasTexcoordStreamInSource;
	const bool textureChannelUsesSets = !uvSets.empty() && uvSets[0].values.size() == globalVertexCount;
	const float* textureChannelSourceUvs = nullptr;
	size_t textureChannelSourceUvStride = 0;
	if (textureChannel)
	{
		textureChannelSourceUvs = textureChannelUsesSets ? reinterpret_cast<const float*>(uvSets[0].values.data())
			: reinterpret_cast<const float*>(vertices.data() + MeshVertexLayout::TexcoordOffset(flags));
		textureChannelSourceUvStride = textureChannelUsesSets ? sizeof(DirectX::XMFLOAT2) : vertexStrideBytes;
		if (textureCharts.empty())
		{
			textureCharts = clod_detail::ComputeTextureCharts(reinterpret_cast<const float*>(vertices.data()), vertexStrideBytes, textureChannelSourceUvs,
				textureChannelSourceUvStride, globalVertexCount, idx, indices.size());
		}
		if (replacementsActive)
		{
			replacementStore.charts = &textureCharts;
		}
	}

	constexpr uint32_t MaxGroupChildren = 8;
	constexpr uint32_t TraversalNodeFanout = 8;
	constexpr uint32_t TargetBucketClusters = 512;
	config.partition_max_refined_groups = 8;

	{
		const size_t requestedPartitionSize = std::max<size_t>(1, (TargetBucketClusters * 3) / 4);
		config.partition_size = std::max<size_t>(requestedPartitionSize, static_cast<size_t>(partitionSizeFloor));
		size_t refinedCapSplitPartitionCount = 0;
		config.partition_refined_split_count = &refinedCapSplitPartitionCount;

		struct CaptureOutputContext
		{
			const std::vector<std::byte>* vertices = nullptr;
			const std::vector<MeshUvSetData>* uvSets = nullptr;
			unsigned int vertexFlags = 0;
			size_t vertexStrideBytes = 0;
			const std::vector<std::byte>* skinningVertices = nullptr;
			size_t skinningVertexStrideBytes = 0;
			std::vector<ClusterLODGroup>* groups = nullptr;
			std::vector<ClusterLODGroupSegment>* segments = nullptr;
			std::vector<BoundingSphere>* segmentBounds = nullptr;
			std::vector<ClusterLODGroupChunk>* groupChunks = nullptr;
			std::vector<std::vector<std::vector<std::byte>>>* groupPageBlobs = nullptr;
			// Raw per-group streams for voxel fallback candidate construction.
			std::vector<std::vector<std::byte>>* groupVertexChunks = nullptr;
			std::vector<std::vector<std::byte>>* groupSkinningChunks = nullptr;
			std::vector<std::vector<uint32_t>>* groupMeshletVertexChunks = nullptr;
			std::vector<std::vector<meshopt_Meshlet>>* groupMeshletChunks = nullptr;
			std::vector<std::vector<uint8_t>>* groupMeshletTriangleChunks = nullptr;
			std::vector<std::vector<int32_t>>* groupMeshletRefinedGroupChunks = nullptr;
			std::vector<std::vector<uint32_t>>* groupVertexGlobalIdChunks = nullptr; // null: not kept (stage 6 texture channel off)
			float meshPositionQuantScale = 1.0f;
			uint32_t meshPositionQuantExp = 0;
			uint32_t nodeBoneLimit = CLOD_NODE_BONE_LIMIT_DEFAULT;
			bool recomputeGroupNormals = false;
			bool reserveNormalSpread = false; // stage 6 step 3: lodNormalSpread
			uint32_t remapReservePerTriangle = 0u; // stage 6 step 2 refinement: lodErrorChannelsTextureRemapReserve
			std::atomic<uint32_t> nextGroupId = 0;
			std::mutex finalizeMutex;
			// Groups output but not yet built (FlushOutputs): their build is a function of the group and its id, so the builds run
			// side by side once clodBuildEx is done, and are finalized in output order.
			struct PendingOutput
			{
				uint32_t groupId;
				clodGroup group;
				CapturedClusterLODGroup captured;
			};
			std::vector<PendingOutput> pending;
			uint32_t cumulativeMeshletCount = 0;
			uint32_t cumulativeGroupVertexCount = 0;
			uint32_t maxChildrenObserved = 0;
			uint32_t maxDepthObserved = 0;
		};

		struct ClodBuildCallbacks
		{
			static int Output(void* outputContext, clodGroup group, const clodCluster* clusters, size_t clusterCount, size_t, unsigned int)
			{
				ZoneScopedN("ClusterLODUtilities::Build::OutputGroup");
				CaptureOutputContext* context = static_cast<CaptureOutputContext*>(outputContext);
				const uint32_t groupId = context->nextGroupId.fetch_add(1u, std::memory_order_relaxed);

				CapturedClusterLODGroup capturedGroup{};
				capturedGroup.depth = group.depth;
				capturedGroup.simplified = group.simplified;
				capturedGroup.clusters.reserve(clusterCount);
				capturedGroup.flattenedIndices.reserve(clusterCount * MS_MESHLET_SIZE * 3);

				for (size_t clusterIndex = 0; clusterIndex < clusterCount; ++clusterIndex)
				{
					const clodCluster& cluster = clusters[clusterIndex];

					CapturedClusterLODCluster capturedCluster{};
					capturedCluster.refinedGroup = static_cast<int32_t>(cluster.refined);
					capturedCluster.bounds = cluster.bounds;
					capturedCluster.vertexCount = static_cast<uint32_t>(cluster.vertex_count);
					capturedCluster.indicesOffset = static_cast<uint32_t>(capturedGroup.flattenedIndices.size());
					capturedCluster.indexCount = static_cast<uint32_t>(cluster.index_count);
					const size_t triangleCount = cluster.index_count / 3u;
					if (cluster.vertex_count > MS_MESHLET_SIZE || triangleCount > MS_MESHLET_SIZE)
					{
						spdlog::error(
							"ClusterLOD callback received an oversized cluster: groupDepth={} cluster={} vertices={} triangles={} limits=({}, {})",
							group.depth,
							clusterIndex,
							cluster.vertex_count,
							triangleCount,
							MS_MESHLET_SIZE,
							MS_MESHLET_SIZE);
						throw std::runtime_error("ClusterLOD callback received a cluster exceeding the GPU meshlet limits");
					}
					capturedGroup.flattenedIndices.insert(
						capturedGroup.flattenedIndices.end(),
						cluster.indices,
						cluster.indices + cluster.index_count);

					capturedGroup.clusters.push_back(std::move(capturedCluster));
				}

				// clodBuildEx outputs groups one at a time, in order.
				context->pending.push_back({ groupId, group, std::move(capturedGroup) });
				return static_cast<int>(groupId);
			}

			static void FlushOutputs(CaptureOutputContext* context)
			{
				ZoneScopedN("ClusterLODUtilities::Build::FlushOutputs");
				std::vector<ClusterLODGroupBuildOutput> outputs(context->pending.size());
				const auto build = [&](size_t index) {
					const CaptureOutputContext::PendingOutput& pendingOutput = context->pending[index];
					const uint32_t groupId = pendingOutput.groupId;
					ClusterLODGroupBuildOutput& output = outputs[index];
					{
						ZoneScopedN("ClusterLODUtilities::Build::OutputGroup::BuildGroupOutput");
						output = BuildClusterLODGroupOutput(
							pendingOutput.captured,
							groupId,
							*context->vertices,
							*context->uvSets,
							context->vertexFlags,
							context->vertexStrideBytes,
							context->skinningVertices,
							context->skinningVertexStrideBytes,
							context->meshPositionQuantScale,
							context->meshPositionQuantExp,
							context->nodeBoneLimit,
							context->recomputeGroupNormals,
							context->reserveNormalSpread,
							context->remapReservePerTriangle);
					}
				};
				oneapi::tbb::this_task_arena::isolate([&] {
					TaskSchedulerManager::GetInstance().ParallelFor("ClusterLODUtilities::BuildGroupOutputs", context->pending.size(), build);
				});
				for (size_t index = 0; index < context->pending.size(); ++index)
				{
					const uint32_t groupId = context->pending[index].groupId;
					const clodGroup& group = context->pending[index].group;
					ClusterLODGroupBuildOutput& output = outputs[index];
					ClusterLODGroup finalizedGroup = output.group;
					finalizedGroup.flags = (finalizedGroup.flags & ~CLOD_GROUP_SIMPLIFY_METHOD_MASK) |
						((static_cast<uint32_t>(group.simplify_method) << CLOD_GROUP_SIMPLIFY_METHOD_SHIFT) & CLOD_GROUP_SIMPLIFY_METHOD_MASK);
					finalizedGroup.flags = (finalizedGroup.flags & ~CLOD_GROUP_SIMPLIFY_VALIDATION_MASK) |
						((static_cast<uint32_t>(group.simplify_validation) << CLOD_GROUP_SIMPLIFY_VALIDATION_SHIFT) & CLOD_GROUP_SIMPLIFY_VALIDATION_MASK);
					if (group.simplify_replaced)
					{
						finalizedGroup.flags |= CLOD_GROUP_FLAG_SIMPLIFY_REPLACED;
					}


					auto ensureIndexedStorage = [&](auto& container)
						{
							if (container.size() <= groupId)
							{
								container.resize(static_cast<size_t>(groupId) + 1ull);
							}
						};

					ensureIndexedStorage(*context->groups);
					ensureIndexedStorage(*context->groupChunks);
					ensureIndexedStorage(*context->groupPageBlobs);
					ensureIndexedStorage(*context->groupVertexChunks);
					ensureIndexedStorage(*context->groupSkinningChunks);
					ensureIndexedStorage(*context->groupMeshletVertexChunks);
					ensureIndexedStorage(*context->groupMeshletChunks);
					ensureIndexedStorage(*context->groupMeshletTriangleChunks);
					ensureIndexedStorage(*context->groupMeshletRefinedGroupChunks);

					finalizedGroup.firstMeshlet = context->cumulativeMeshletCount;
					finalizedGroup.firstGroupVertex = context->cumulativeGroupVertexCount;
					finalizedGroup.firstSegment = static_cast<uint32_t>(context->segments->size());

					context->cumulativeMeshletCount += finalizedGroup.meshletCount;
					context->cumulativeGroupVertexCount += finalizedGroup.groupVertexCount;
					context->segments->insert(context->segments->end(), output.segments.begin(), output.segments.end());
					context->segmentBounds->insert(context->segmentBounds->end(), output.segmentBounds.begin(), output.segmentBounds.end());

					(*context->groupPageBlobs)[groupId] = std::move(output.pageBlobs);

					// Store raw streams for voxel fallback candidate construction.
					(*context->groupVertexChunks)[groupId] = std::move(output.vertexChunk);
					(*context->groupSkinningChunks)[groupId] = std::move(output.skinningChunk);
					(*context->groupMeshletVertexChunks)[groupId] = std::move(output.meshletVertices);
					(*context->groupMeshletChunks)[groupId] = std::move(output.meshlets);
					(*context->groupMeshletTriangleChunks)[groupId] = std::move(output.meshletTriangles);
					(*context->groupMeshletRefinedGroupChunks)[groupId] = std::move(output.meshletRefinedGroups);
					if (context->groupVertexGlobalIdChunks != nullptr)
					{
						ensureIndexedStorage(*context->groupVertexGlobalIdChunks);
						(*context->groupVertexGlobalIdChunks)[groupId] = std::move(output.vertexGlobalIds);
					}

					(*context->groupChunks)[groupId] = output.groupChunk;
					(*context->groups)[groupId] = finalizedGroup;

			context->maxChildrenObserved = std::max(context->maxChildrenObserved, finalizedGroup.segmentCount);
					context->maxDepthObserved = (std::max)(context->maxDepthObserved, static_cast<uint32_t>(std::max(finalizedGroup.depth, 0)));

				}
				context->pending.clear();
			}

			static void Iterate(void* iterationContext, void*, int, size_t taskCount)
			{
				ZoneScopedN("ClusterLODUtilities::Build::Iterate");
				TracyPlot("CLOD.Build.IterationTasks", static_cast<int64_t>(taskCount));
				TaskSchedulerManager::GetInstance().ParallelFor("ClusterLODUtilities::BuildIteration", taskCount, [&](size_t taskIndex)
					{
						clodBuild_iterationTask(iterationContext, taskIndex, 0);
					});
			}

			// Splitting each depth's results into clusters (clodBuildParallelConfig::parallel_for).
			static void ParallelFor(void*, size_t count, void (*body)(void*, size_t), void* bodyContext)
			{
				oneapi::tbb::this_task_arena::isolate([&] {
					TaskSchedulerManager::GetInstance().ParallelFor("ClusterLODUtilities::BuildSplit", count, [&](size_t index) { body(bodyContext, index); });
				});
			}
		};

		CaptureOutputContext captureContext{};
		captureContext.vertices = &buildVertices;
		captureContext.uvSets = &buildUvSets;
		captureContext.vertexFlags = flags;
		captureContext.vertexStrideBytes = vertexStrideBytes;
		captureContext.skinningVertices = buildSkinning;
		captureContext.skinningVertexStrideBytes = skinningVertexSize;
		captureContext.groups = &state.groups;
		captureContext.segments = &state.segments;
		captureContext.segmentBounds = &state.segmentBounds;
		captureContext.groupChunks = &state.groupChunks;
		captureContext.groupPageBlobs = &state.groupPageBlobs;
		captureContext.groupVertexChunks = &state.groupVertexChunks;
		captureContext.groupSkinningChunks = &state.groupSkinningChunks;
		captureContext.groupMeshletVertexChunks = &state.groupMeshletVertexChunks;
		captureContext.groupMeshletChunks = &state.groupMeshletChunks;
		captureContext.groupMeshletTriangleChunks = &state.groupMeshletTriangleChunks;
		captureContext.groupMeshletRefinedGroupChunks = &state.groupMeshletRefinedGroupChunks;
		captureContext.groupVertexGlobalIdChunks = textureChannel ? &state.groupVertexGlobalIdChunks : nullptr;
		captureContext.meshPositionQuantScale = meshPositionQuantScale;
		captureContext.meshPositionQuantExp = meshPositionQuantExp;
		captureContext.nodeBoneLimit = std::clamp(settings.nodeBoneLimit, 1u, CLOD_NODE_BONE_LIMIT_HARD_MAX);
		captureContext.recomputeGroupNormals = recomputeGroupNormals;
		captureContext.reserveNormalSpread = ClusterLODNormalSpreadActive(settings);
		// Only meshes that can take blocks (the material gate the remap applies below).
		captureContext.remapReservePerTriangle = textureChannel && ClusterLODUvRemapActive(settings) && materialAppearance != nullptr &&
				!materialAppearance->alphaMasked && materialAppearance->hasBaseColorTexture
			? settings.lodErrorChannelsTextureRemapReserve : 0u;
		{
			ZoneScopedN("ClusterLODUtilities::Build::clodBuildEx");
			// Each depth's simplifications run in parallel; groups are still output and split in order, so the result is
			// the serial one.
			clodBuildParallelConfig parallelConfig{};
			parallelConfig.iteration_callback = &ClodBuildCallbacks::Iterate;
			parallelConfig.parallel_for = &ClodBuildCallbacks::ParallelFor;
			clodBuildEx(config, mesh, &captureContext, &ClodBuildCallbacks::Output, &parallelConfig);
			ClodBuildCallbacks::FlushOutputs(&captureContext);
		}
		TracyPlot("CLOD.Build.Groups", static_cast<int64_t>(state.groups.size()));
		TracyPlot("CLOD.Build.Segments", static_cast<int64_t>(state.segments.size()));
		TracyPlot("CLOD.Build.Meshlets", static_cast<int64_t>(captureContext.cumulativeMeshletCount));

		state.maxDepth = captureContext.maxDepthObserved;

		for (size_t groupIndex = 0; groupIndex < state.groups.size(); ++groupIndex)
		{
			if (state.groups[groupIndex].groupVertexCount != state.groupChunks[groupIndex].groupVertexCount)
			{
				state.groupChunks[groupIndex].groupVertexCount = state.groups[groupIndex].groupVertexCount;
			}
		}

		if (settings.simplifyValidation)
		{
			const clodValidationStats& stats = simplifyValidationStats;
			spdlog::info(
				"ClusterLOD simplify validation: triangles={} candidates={} accepted={} rejected={} rejected_fold={} rejected_shading={} "
				"rejected_coverage={} rejected_spurious={} rejected_source_coverage={} groups_first={} groups_retried={} groups_terminal={} sloppy_accepted={} "
				"sloppy_rejected={} index_ms={:.2f} validate_ms={:.2f} candidate_ms={:.2f} coverage_ms={:.2f} baseline_ms={:.2f} source_ms={:.2f}",
				indices.size() / 3u,
				stats.candidates,
				stats.accepted,
				stats.rejected,
				stats.rejected_reason[0],
				stats.rejected_reason[1],
				stats.rejected_reason[2],
				stats.rejected_reason[3],
				stats.rejected_reason[4],
				stats.groups_first,
				stats.groups_retried,
				stats.groups_terminal,
				stats.sloppy_accepted,
				stats.sloppy_rejected,
				simplifyValidationIndexMs,
				static_cast<double>(simplifyValidation.nanoseconds.load()) * 1.0e-6,
				static_cast<double>(simplifyValidation.validator->Times().candidate) * 1.0e-6,
				static_cast<double>(simplifyValidation.validator->Times().coverage) * 1.0e-6,
				static_cast<double>(simplifyValidation.validator->Times().baseline) * 1.0e-6,
				static_cast<double>(simplifyValidation.validator->Times().source) * 1.0e-6);
		}

		if (replacementsActive)
		{
			const clodReplacementStats& stats = replacementStats;
			const clod_detail::ReplacementVertexFitter::Stats fit = replacementFitter->GetStats();
			spdlog::info(
				"ClusterLOD simplify update: triangles={} vertices_source={} update_candidates={} update_offered={} update_accepted={} replaced_vertices={} "
				"replaced_moved={} fit_refit={} locked_moved={} fit_vertices={} fit_fallbacks={} uv_clamped={} uv_nonfinite={} measured={} measured_raised={} "
				"uv_transferred={} uv_transfer_outside={} fan_checks={} fan_hits={} check_skipped={} fit_ms={:.2f} deviation_ms={:.2f}",
				indices.size() / 3u,
				globalVertexCount,
				stats.candidates,
				stats.offered,
				stats.accepted,
				stats.vertices,
				stats.vertices_moved,
				fit.refitReplaced,
				stats.locked_moved,
				fit.fitVertices,
				fit.fitFallbacks,
				fit.uvClamped,
				fit.uvNonFinite,
				stats.measured,
				stats.measured_raised,
				fit.uvTransferred,
				fit.uvTransferOutside,
				fit.fanChecks,
				fit.fanHits,
				fit.checkSkipped,
				static_cast<double>(fit.fitNs) * 1.0e-6,
				static_cast<double>(simplifyValidation.validator->Times().deviation) * 1.0e-6);
		}

		if (simplifyOptimal)
		{
			const clodReplacementStats& stats = replacementStats;
			const clod_detail::OptimalSimplifier::Stats optimal = optimalSimplifier->GetStats();
			spdlog::info(
				"ClusterLOD simplify optimal: triangles={} optimal_candidates={} optimal_offered={} optimal_accepted={} collapses={} moved_free={} "
				"moved_line={} fallbacks={} rejected_flip={} rejected_sheet={} rejected_vanish={} solve_failed={} solve_clamped={} reference_samples={} "
				"reference_unmatched={} reference_ms={:.2f} loop_ms={:.2f}",
				indices.size() / 3u,
				stats.optimal_candidates,
				stats.optimal_offered,
				stats.optimal_accepted,
				optimal.loop.collapses,
				optimal.loop.movedFree,
				optimal.loop.movedLine,
				optimal.loop.fallbacks,
				optimal.loop.rejectedFlip,
				optimal.loop.rejectedSheet,
				optimal.loop.rejectedVanish,
				optimal.loop.solveFailed,
				optimal.loop.solveClamped,
				optimal.referenceSamples,
				optimal.referenceUnmatched,
				static_cast<double>(optimal.referenceNs) * 1.0e-6,
				static_cast<double>(optimal.loopNs) * 1.0e-6);
		}

		if (config.topology_stats && config.simplify_last_resort_sloppy)
		{
			spdlog::info("ClusterLOD sloppy last resort: groups={} triangles={} rejected_first={} rejected_first_triangles={} stuck_left={} stuck_left_triangles={}",
				topologyStats.last_resort, topologyStats.last_resort_triangles, topologyStats.last_resort_rejected,
				topologyStats.last_resort_rejected_triangles, topologyStats.stuck, topologyStats.stuck_triangles);
			if (topologyDiagnostics && topologyStats.last_resort != 0u)
			{
				const auto& c = topologyStats.last_resort_class_triangles;
				spdlog::info("ClusterLOD sloppy last resort classes: mesh_triangles={} root_triangles={} boundary={} normal_seams={} protected_seams={} "
					"seams={} boundary_seams={} components={} other={}",
					indices.size() / 3u, topologyStats.root_triangles + topologyStats.stuck_triangles + topologyStats.rejected_triangles,
					c[0], c[1], c[2], c[3], c[4], c[5], c[6]);
			}
		}
		if (config.topology_stats)
		{
			const clodTopologyStats& stats = topologyStats;
			spdlog::info(
				"ClusterLOD topology: triangles={} roots={} root_triangles={} rejected={} rejected_triangles={} stuck={} stuck_triangles={} "
				"boundary={} boundary_triangles={} normal_seams={} normal_seams_triangles={} protected_seams={} protected_seams_triangles={} "
				"seams={} seams_triangles={} boundary_seams={} boundary_seams_triangles={} components={} "
				"components_triangles={} other={} other_triangles={} stuck_components={} stuck_small_triangles={} stuck_boundary_vertices={} "
				"stuck_positions={} stuck_uv_seam_vertices={} stuck_uv_seam_tiny={} stuck_uv_seam_integer={} topology_groups={} topology_offered={} "
				"topology_rejected={} topology_rejected_fold={} topology_rejected_shading={} topology_rejected_coverage={} topology_rejected_spurious={} "
				"topology_rejected_source_coverage={} topology_rejected_texture={} topology_stuck={} topology_accepted={} topology_accepted_triangles={} "
				"topology_relaxed_vertices={} texture_checks={} texture_ms={:.2f} texture_rejected_slide={} texture_rejected_uv={} texture_rejected_charts={}",
				indices.size() / 3u,
				stats.roots,
				stats.root_triangles,
				stats.rejected,
				stats.rejected_triangles,
				stats.stuck,
				stats.stuck_triangles,
				stats.stuck_class[0],
				stats.stuck_class_triangles[0],
				stats.stuck_class[1],
				stats.stuck_class_triangles[1],
				stats.stuck_class[2],
				stats.stuck_class_triangles[2],
				stats.stuck_class[3],
				stats.stuck_class_triangles[3],
				stats.stuck_class[4],
				stats.stuck_class_triangles[4],
				stats.stuck_class[5],
				stats.stuck_class_triangles[5],
				stats.stuck_class[6],
				stats.stuck_class_triangles[6],
				stats.stuck_components,
				stats.stuck_small_triangles,
				stats.stuck_boundary_vertices,
				stats.stuck_positions,
				stats.stuck_uv_seam_vertices,
				stats.stuck_uv_seam_tiny,
				stats.stuck_uv_seam_integer,
				stats.topology_groups,
				stats.topology_offered,
				stats.topology_rejected,
				stats.topology_rejected_reason[0],
				stats.topology_rejected_reason[1],
				stats.topology_rejected_reason[2],
				stats.topology_rejected_reason[3],
				stats.topology_rejected_reason[4],
				stats.topology_rejected_reason[5],
				stats.topology_stuck,
				stats.topology_accepted,
				stats.topology_accepted_triangles,
				stats.topology_relaxed_vertices,
				simplifyValidation.textureChecks.load(),
				static_cast<double>(simplifyValidation.textureNanoseconds.load()) * 1.0e-6,
				simplifyValidation.textureRejectedSlide.load(),
				simplifyValidation.textureRejectedUv.load(),
				simplifyValidation.textureRejectedCharts.load());
			if (ClusterLODTopologyTextureColorActive(settings))
			{
				spdlog::info("ClusterLOD topology texture colour: source={} reference_delta_e={:.6g} accepted_by_colour={}", topologyTextureColorSource,
					settings.simplifyTopologyTextureColor, simplifyValidation.textureAcceptedColor.load());
			}
		}

		if (refinedCapSplitPartitionCount > 0)
		{
			spdlog::info(
				"ClusterLOD: refined-group cap split {} partitions at bucket target {}",
				refinedCapSplitPartitionCount,
				TargetBucketClusters);
		}
	}

	// Error channels (stage 3, docs/clod-simplification-validation.md): every group's simplified output measured against the
	// original, from the raw group streams while they are held. The traversal hierarchy stores them in its nodes.
	// Stage 6 step 3 (lodNormalSpread): the same measurement gives each simplified cluster's per-vertex normal spread, with or
	// without the channels; without them the channels it also computes are dropped below, so nodes and metadata stay as they were.
	const bool normalSpreadRequested = ClusterLODNormalSpreadActive(settings);
	if (settings.lodErrorChannels || normalSpreadRequested)
	{
		ZoneScopedN("ClusterLODUtilities::Build::MeasureErrorChannels");
		std::optional<clod_detail::SimplifyValidator> channelOriginal;
		const clod_detail::SimplifyValidator* original = simplifyValidation.validator ? &*simplifyValidation.validator : nullptr;
		if (original == nullptr)
		{
			clod_detail::SimplifySurfaceView source{};
			source.positions = reinterpret_cast<const float*>(vertices.data());
			source.positionStride = vertexStrideBytes;
			source.normals = hasNormalStreamInSource ? reinterpret_cast<const float*>(vertices.data() + MeshVertexLayout::NormalOffset) : nullptr;
			source.normalStride = vertexStrideBytes;
			source.indices = idx;
			source.indexCount = indices.size();
			clod_detail::SimplifyValidationThresholds channelThresholds{};
			channelThresholds.matchReachScale = std::max(1.0f, settings.simplifyMatchReach);
			channelThresholds.matchMaxCandidates = settings.simplifyMatchCandidates;
			channelThresholds.matchFacingGrid = ClusterLODMatchFacingGrid(settings);
			channelOriginal.emplace(source, channelThresholds);
			original = &*channelOriginal;
		}
		clod_detail::ErrorChannelTextureInputs textureInputs;
		if (textureChannel)
		{
			textureInputs.source.positions = reinterpret_cast<const float*>(vertices.data());
			textureInputs.source.positionStride = vertexStrideBytes;
			textureInputs.source.uvs = textureChannelSourceUvs;
			textureInputs.source.uvStride = textureChannelSourceUvStride;
			textureInputs.source.indices = idx;
			textureInputs.source.indexCount = indices.size();
			textureInputs.source.charts = textureCharts.data();
			if (textureChannelUsesSets)
			{
				textureInputs.buildUvs = reinterpret_cast<const float*>(buildUvSets[0].values.data());
				textureInputs.buildUvStride = sizeof(DirectX::XMFLOAT2);
				textureInputs.buildVertexCount = buildUvSets[0].values.size();
			}
			else
			{
				textureInputs.buildUvs = reinterpret_cast<const float*>(buildVertices.data() + MeshVertexLayout::TexcoordOffset(flags));
				textureInputs.buildUvStride = vertexStrideBytes;
				textureInputs.buildVertexCount = buildVertices.size() / vertexStrideBytes;
			}
			textureInputs.buildCharts = &textureCharts;
			textureInputs.weight = settings.lodErrorChannelsTexture;
			textureInputs.slide = settings.lodErrorChannelsTextureSlide;
		}
		// Stage 6 step 1: weight the displaced samples by the colour difference they draw, from the importer's material. Any
		// reason it cannot (no material from this importer, another UV set, vertex colours, an image that does not decode)
		// leaves this mesh with the binary term, and says why.
		const bool textureColorRequested = textureChannel && ClusterLODTextureColorActive(settings);
		clod_detail::ErrorChannelTextureColor textureColor;
		std::shared_ptr<const clod_texture::Texture> textureColorImage;
		const char* textureColorSource = "binary";
		if (textureColorRequested)
		{
			if (materialAppearance == nullptr)
			{
				textureColorSource = "binary (the importer gave no material)";
			}
			else if (materialAppearance->baseColorTexCoord != 0u)
			{
				textureColorSource = "binary (the base colour reads another UV set)";
			}
			else if (materialAppearance->hasVertexColors)
			{
				textureColorSource = "binary (vertex colours)";
			}
			else
			{
				if (materialAppearance->hasBaseColorTexture && materialAppearance->decodeBaseColor)
				{
					textureColorImage = decodedBaseColor != nullptr ? decodedBaseColor : materialAppearance->decodeBaseColor();
				}
				if (materialAppearance->hasBaseColorTexture && (textureColorImage == nullptr || textureColorImage->Levels() == 0u))
				{
					textureColorSource = "binary (the base colour image did not decode)";
				}
				else
				{
					textureColor.baseColorFactor = materialAppearance->baseColorFactor;
					textureColor.texture = textureColorImage.get();
					textureColor.alphaMasked = materialAppearance->alphaMasked;
					textureColor.alphaCutoff = materialAppearance->alphaCutoff;
					textureColor.uvTransformed = materialAppearance->hasBaseColorUvTransform;
					textureColor.uvTransform = materialAppearance->baseColorUvTransform;
					textureColor.referenceDeltaE = settings.lodErrorChannelsTextureColor;
					textureColor.footprint = settings.lodErrorChannelsTextureColorFootprint;
					textureColor.everySample = settings.lodErrorChannelsTextureColorAll;
					textureInputs.color = &textureColor;
					textureColorSource = textureColor.texture != nullptr ? "texture" : "factor";
				}
			}
		}
		// Stage 6 step 2: UV remap blocks for damaged groups. They need the importer's material: one that is not alpha-masked
		// (raster, shadow and ray alpha tests read the corners' UVs) and has a base colour texture (else nothing is drawn wrong).
		clod_detail::ErrorChannelTextureRemap textureRemap;
		const char* textureRemapSource = "off";
		if (textureChannel && ClusterLODUvRemapActive(settings))
		{
			if (materialAppearance == nullptr)
			{
				textureRemapSource = "none (the importer gave no material)";
			}
			else if (materialAppearance->alphaMasked)
			{
				textureRemapSource = "none (alpha-masked material)";
			}
			else if (!materialAppearance->hasBaseColorTexture)
			{
				textureRemapSource = "none (untextured material)";
			}
			else
			{
				textureRemap.minDisplacedShare = settings.lodErrorChannelsTextureRemap;
				textureRemap.settings.texel = std::max(settings.lodErrorChannelsTextureRemapTexel, 0.0f);
				textureRemap.settings.maxNodes = settings.lodErrorChannelsTextureRemapMaxNodes;
				textureRemap.settings.layout = std::clamp(settings.lodErrorChannelsTextureRemapLayout, 1u, 2u);
				textureRemap.settings.cellMiss = std::max(settings.lodErrorChannelsTextureRemapCellMiss, 0.0f);
				// The builder knows a texture's size only when the colour weight decoded it; otherwise the texture check's nominal one.
				const uint32_t resolution = textureColorImage != nullptr && textureColorImage->width > 0u
					? std::max(textureColorImage->width, textureColorImage->height)
					: std::max(settings.simplifyTopologyTextureResolution, 1u);
				textureRemap.settings.sourceTexelUv = 1.0f / static_cast<float>(resolution);
				textureRemap.settings.slide = settings.lodErrorChannelsTextureSlide;
				textureInputs.remap = &textureRemap;
				textureRemapSource = "remap";
			}
		}
		// Stage 6 step 3: the blocks are addressed through the UV set 0 descriptor, and measured against the original's normals.
		const char* normalSpreadSource = "off";
		bool normalSpread = false;
		if (normalSpreadRequested)
		{
			if (!hasNormalStreamInSource)
			{
				normalSpreadSource = "none (no normals)";
			}
			else if (buildUvSets.empty() && !hasTexcoordStreamInSource)
			{
				normalSpreadSource = "none (no UV set to address the blocks)";
			}
			else
			{
				normalSpreadSource = "spread";
				normalSpread = true;
			}
		}
		const clod_detail::ErrorChannelStats stats = clod_detail::MeasureClusterLODErrorChannels(
			state, *original, vertexStrideBytes, hasNormalStreamInSource, textureChannel ? &textureInputs : nullptr, normalSpread,
			settings.lodErrorChannels, settings.lodNormalSpreadFootprint, settings.lodNormalSpreadSampleCap, settings.lodErrorChannelsSamples);
		if (!settings.lodErrorChannels)
		{
			state.errorChannels.clear();
		}
		if (normalSpreadRequested)
		{
			spdlog::info("ClusterLOD normal spread: source={} footprint={:.6g} sample_cap={:.6g} clusters={} vertices={} bytes={} sampled_area={:.6g} mean_v={:.6g} "
				"vertices_zero={} vertices_under_16={} vertices_under_64={} vertices_over_64={} spread_ms={:.2f} measure_ms={:.2f}",
				normalSpreadSource, settings.lodNormalSpreadFootprint, settings.lodNormalSpreadSampleCap, stats.spreadClusters, stats.spreadVertices, stats.spreadBytes, stats.spreadArea,
				stats.spreadArea > 0.0 ? stats.spreadQ / stats.spreadArea : 0.0, stats.spreadVertexBands[0], stats.spreadVertexBands[1],
				stats.spreadVertexBands[2], stats.spreadVertexBands[3], stats.spreadMs, stats.measureMs);
		}
		if (textureChannel && ClusterLODUvRemapActive(settings))
		{
			spdlog::info("ClusterLOD texture remap: source={} min_displaced_share={:.6g} texel={:.6g} max_nodes={} groups={} clusters={} triangles={} "
				"nodes={} nodes_unmatched={} nodes_along_normal={} bytes={} triangles_over_budget={} sub_triangles={} straddling_charts={} straddling_seams={} displaced_before={:.6g} displaced_after={:.6g} q_before={:.6g} q_after={:.6g} "
				"slide_before={:.6g} slide_after={:.6g} chart_before={:.6g} chart_after={:.6g} mixed_before={:.6g} mixed_after={:.6g} remap_ms={:.2f}",
				textureRemapSource, settings.lodErrorChannelsTextureRemap, settings.lodErrorChannelsTextureRemapTexel,
				settings.lodErrorChannelsTextureRemapMaxNodes, stats.remapGroups, stats.remapClusters, stats.remapTriangles, stats.remapNodes,
				stats.remapNodesUnmatched, stats.remapNodesAlongNormal, stats.remapBytes, stats.remapTrianglesOverBudget, stats.remapSubTriangles,
				stats.remapSubTrianglesCharts, stats.remapSubTrianglesSeams, stats.remapDisplacedBefore, stats.remapDisplacedAfter,
				stats.remapQBefore, stats.remapQAfter, stats.remapSlideBefore, stats.remapSlideAfter, stats.remapChartBefore, stats.remapChartAfter,
				stats.remapMixedBefore, stats.remapMixedAfter, stats.remapMs);
			if (settings.lodErrorChannelsTextureRemapLayout >= 2u)
			{
				spdlog::info("ClusterLOD texture remap regions: layout=2 cell_miss={:.6g} triangles_regions={} triangles_grid={} regions={} region_cells={} region_area={:.6g} "
					"region_miss_area={:.6g} points_unlabeled={} regions_fitted={} regions_planar_pieces={} triangles_regions_rejected={} "
					"cell_grid_2={} cell_grid_3={} cell_grid_5={} cell_grid_9={} cell_grid_17={}",
					settings.lodErrorChannelsTextureRemapCellMiss, stats.remapTrianglesRegions, stats.remapTrianglesGrid, stats.remapRegions, stats.remapRegionCells, stats.remapRegionArea,
					stats.remapRegionMissArea, stats.remapPointsUnlabeled, stats.remapRegionsFitted, stats.remapRegionsPlanarPieces,
					stats.remapTrianglesRegionsRejected, stats.remapRegionGridCounts[0], stats.remapRegionGridCounts[1], stats.remapRegionGridCounts[2],
					stats.remapRegionGridCounts[3], stats.remapRegionGridCounts[4]);
			}
		}
		if (textureChannel)
		{
			spdlog::info(
				"ClusterLOD texture channel: weight={:.6g} slide={:.6g} uv_sets={} measured_area={:.6g} displaced_area={:.6g} groups_displaced={} texture_ms={:.2f}",
				settings.lodErrorChannelsTexture, settings.lodErrorChannelsTextureSlide, textureChannelUsesSets ? 1 : 0, stats.textureMeasuredArea,
				stats.textureDisplacedArea, stats.textureGroupsDisplaced, stats.textureMs);
		}
		if (textureColorRequested)
		{
			spdlog::info("ClusterLOD texture colour: reference_de={:.6g} footprint={:.6g} source={} image={}x{} weighted_area={:.6g} displaced_area={:.6g} "
				"samples_de_under_1={} under_3={} under_reference={} over_reference={} unmatched={}",
				settings.lodErrorChannelsTextureColor, settings.lodErrorChannelsTextureColorFootprint, textureColorSource, textureColorImage ? textureColorImage->width : 0u,
				textureColorImage ? textureColorImage->height : 0u, textureInputs.color != nullptr ? stats.textureWeightedArea : stats.textureDisplacedArea,
				stats.textureDisplacedArea, stats.textureColorSamples[0], stats.textureColorSamples[1], stats.textureColorSamples[2],
				stats.textureColorSamples[3], stats.textureColorSamples[4]);
		}
		if (settings.lodErrorChannels)
		spdlog::info(
			"ClusterLOD error channels: triangles={} groups={} simplified_triangles={} samples={} samples_without_normal={} source_triangles={} "
			"raised_over_quadric={} envelope_raised_geometric={} envelope_raised_appearance={} measure_ms={:.2f}",
			indices.size() / 3u,
			stats.groups,
			stats.simplifiedTriangles,
			stats.samples,
			stats.samplesWithoutNormal,
			stats.sourceTriangles,
			stats.raisedOverQuadric,
			stats.envelopeRaisedGeometric,
			stats.envelopeRaisedAppearance,
			stats.measureMs);
	}

	clod_detail::LogQueryStats("cumulative");

	const uint32_t totalGroupCount = static_cast<uint32_t>(state.groups.size());
	uint32_t groupsWithRefinedChildren = 0;
	for (const ClusterLODGroup& group : state.groups)
	{
		if (group.segmentCount > group.terminalSegmentCount)
		{
			groupsWithRefinedChildren++;
		}
	}

	const float refinedGroupRatio = totalGroupCount > 0
		? static_cast<float>(groupsWithRefinedChildren) / static_cast<float>(totalGroupCount)
		: 0.0f;

	spdlog::debug(
		"ClusterLOD metrics: groups={} segments={} refined_groups={} refined_ratio={:.3f} normal_attributes={} tangent_attributes={}",
		totalGroupCount,
		static_cast<uint32_t>(state.segments.size()),
		groupsWithRefinedChildren,
		refinedGroupRatio,
		hasNormalStreamInSource && enableNormalAttributeSimplification ? 1 : 0,
		(!tangentAttributeStream.empty() && enableNormalAttributeSimplification) ? 1 : 0);

	{
		std::vector<uint32_t> refinedGroupParentCounts(state.groups.size(), 0);
		for (const ClusterLODGroupSegment& seg : state.segments)
		{
			if (seg.refinedGroup >= 0)
			{
				const uint32_t refinedGroup = static_cast<uint32_t>(seg.refinedGroup);
				if (refinedGroup < refinedGroupParentCounts.size())
				{
					refinedGroupParentCounts[refinedGroup]++;
				}
			}
		}

		uint32_t groupsWithMultipleParents = 0;
		uint32_t maxParentCount = 0;
		for (uint32_t parentCount : refinedGroupParentCounts)
		{
			if (parentCount > 1)
			{
				groupsWithMultipleParents++;
				maxParentCount = std::max(maxParentCount, parentCount);
			}
		}
	}

	{
		ZoneScopedN("ClusterLODUtilities::Build::BuildVoxelFallbackCandidates");
		BuildVoxelFallbackCandidates(
			state,
			vertexStrideBytes,
			skinningVertexSize,
			coverageMaterialSampler,
			settings);
	}
	if (!state.meshletNormalSpreads.empty())
	{
		// Stage 6 step 3: as the channels below, a mesh with voxel groups or a forked DAG keeps no normal spread (it was measured on
		// the triangle hierarchy the fork replaces).
		const bool voxelized = std::any_of(state.groups.begin(), state.groups.end(), [](const ClusterLODGroup& group) {
			return (group.flags & (CLOD_GROUP_FLAG_IS_VOXEL | CLOD_GROUP_BRANCH_FLAGS)) != 0u;
		});
		if (voxelized)
		{
			spdlog::info("ClusterLOD normal spread: dropped, the mesh has voxel groups or a forked DAG");
			state.meshletNormalSpreads.clear();
		}
	}
	if (!state.errorChannels.empty())
	{
		// The channels measure triangle representations. A voxel group or a forked DAG draws something else at its boundaries,
		// so such a mesh keeps none and selects by its stored errors alone.
		const bool voxelized = state.errorChannels.size() != state.groups.size() ||
			std::any_of(state.groups.begin(), state.groups.end(), [](const ClusterLODGroup& group) {
				return (group.flags & (CLOD_GROUP_FLAG_IS_VOXEL | CLOD_GROUP_BRANCH_FLAGS)) != 0u;
			});
		if (voxelized)
		{
			spdlog::info("ClusterLOD error channels: dropped, the mesh has voxel groups or a forked DAG");
			state.errorChannels.clear();
			// Stage 6 step 2: the remaps go with them (they were judged by the channels' texture term).
			state.meshletUvRemaps.clear();
		}
	}
	{
		ZoneScopedN("ClusterLODUtilities::Build::ReleaseRawStreamsAfterVoxelFallback");
		std::vector<std::vector<std::byte>>().swap(state.groupVertexChunks);
		std::vector<std::vector<std::byte>>().swap(state.groupSkinningChunks);
		std::vector<std::vector<uint32_t>>().swap(state.groupMeshletVertexChunks);
		std::vector<std::vector<meshopt_Meshlet>>().swap(state.groupMeshletChunks);
		std::vector<std::vector<uint8_t>>().swap(state.groupMeshletTriangleChunks);
		std::vector<std::vector<int32_t>>().swap(state.groupMeshletRefinedGroupChunks);
		std::vector<std::vector<uint32_t>>().swap(state.groupVertexGlobalIdChunks);
	}

	// Build traversal hierarchy.
	{
		ZoneScopedN("ClusterLODUtilities::Build::BuildTraversalHierarchy");
		BuildClusterLODTraversalHierarchy(state, /*preferredNodeWidth=*/TraversalNodeFanout);
	}
	TracyPlot("CLOD.Build.Nodes", static_cast<int64_t>(state.nodes.size()));
	std::vector<ClusterLODNodeSkinningInfo> nodeSkinningInfos;
	std::vector<uint32_t> nodeBoneIndices;
	BuildNodeSkinningSidecar(state, settings.nodeBoneLimit, nodeSkinningInfos, nodeBoneIndices);

	for (const ClusterLODNode& node : state.nodes)
	{
		if (node.range.isGroup != 0)
			continue;

		const uint32_t childCount = uint32_t(node.range.countMinusOne) + 1u;
		if (childCount > TraversalNodeFanout)
		{
			throw std::runtime_error("Cluster LOD: traversal node fanout exceeded configured maximum");
		}
	}

	std::vector<std::vector<std::byte>> meshPageBlobs;
	std::vector<uint32_t> groupPageReferences;
	std::vector<uint32_t> groupPageReferenceOffsets;
	uint32_t trianglePageCount = 0u;
	uint32_t voxelPageBase = 0u;
	uint32_t voxelPageCount = 0u;
	{
		ZoneScopedN("ClusterLODUtilities::Build::FinalizeMeshWidePagePacking");
		FinalizeMeshWidePagePacking(
			state,
			meshPageBlobs,
			groupPageReferences,
			groupPageReferenceOffsets,
			trianglePageCount,
			voxelPageBase,
			voxelPageCount);
	}
	TracyPlot("CLOD.Build.MeshPages", static_cast<int64_t>(meshPageBlobs.size()));
	TracyPlot("CLOD.Build.TrianglePages", static_cast<int64_t>(trianglePageCount));
	TracyPlot("CLOD.Build.VoxelPages", static_cast<int64_t>(voxelPageCount));

	ClusterLODPrebuildArtifacts artifacts{};
	artifacts.prebuiltData.groups = std::move(state.groups);
	artifacts.prebuiltData.segments = std::move(state.segments);
	artifacts.prebuiltData.segmentBounds = std::move(state.segmentBounds);
	artifacts.prebuiltData.objectBoundingSphere = BuildObjectBoundingSphereFromRootNode(state.nodes, state.topRootNode);
	artifacts.prebuiltData.groupChunks = std::move(state.groupChunks);
	artifacts.prebuiltData.groupPageReferences = std::move(groupPageReferences);
	artifacts.prebuiltData.groupPageReferenceOffsets = std::move(groupPageReferenceOffsets);
	artifacts.prebuiltData.trianglePageCount = trianglePageCount;
	artifacts.prebuiltData.voxelPageBase = voxelPageBase;
	artifacts.prebuiltData.voxelPageCount = voxelPageCount;
	artifacts.prebuiltData.nodes = std::move(state.nodes);
	artifacts.prebuiltData.nodeSkinningInfos = std::move(nodeSkinningInfos);
	artifacts.prebuiltData.nodeBoneIndices = std::move(nodeBoneIndices);
	artifacts.prebuiltData.nodeBoneLimit = std::clamp(settings.nodeBoneLimit, 1u, CLOD_NODE_BONE_LIMIT_HARD_MAX);
	artifacts.prebuiltData.lodNodeRanges = std::move(state.lodNodeRanges);
	artifacts.prebuiltData.lodLevelRoots = std::move(state.lodLevelRoots);
	artifacts.prebuiltData.maxDepth = state.maxDepth;
	artifacts.prebuiltData.maxTraversalDepth = state.maxTraversalDepth;
	artifacts.prebuiltData.uvRemapClusters = state.uvRemapClustersWritten;
	artifacts.prebuiltData.uvRemapBytes = state.uvRemapBytesWritten;
	std::vector<std::vector<std::vector<std::byte>>>().swap(state.meshletUvRemaps);
	artifacts.prebuiltData.normalSpreadClusters = state.normalSpreadClustersWritten;
	artifacts.prebuiltData.normalSpreadBytes = state.normalSpreadBytesWritten;
	std::vector<std::vector<std::vector<std::byte>>>().swap(state.meshletNormalSpreads);
	AssignSingleRootPartRecord(artifacts.prebuiltData, state.topRootNode);

	artifacts.cacheBuildData.groupPageBlobs = std::move(state.groupPageBlobs);
	artifacts.cacheBuildData.voxelGroupMapping = std::move(state.voxelGroupMapping);
	artifacts.cacheBuildData.meshPageBlobs = std::move(meshPageBlobs);
	std::string representationError;
	if (!ValidateClusterLODPageRepresentations(
		artifacts.prebuiltData,
		&artifacts.cacheBuildData.meshPageBlobs,
		&representationError))
	{
		spdlog::error("ClusterLOD page-representation validation failed: {}", representationError);
		throw std::runtime_error("ClusterLOD page-representation validation failed: " + representationError);
	}
	EmitClusterLODPagePackingTelemetry(
		"geometry",
		artifacts.prebuiltData,
		artifacts.cacheBuildData.meshPageBlobs);

	return artifacts;
}


namespace clod_detail
{
	MergeGroupOutput BuildMergeGroupOutput(
		const MergeCapturedGroup& group,
		uint32_t sourceGroupLocalIndex,
		const std::vector<std::byte>& vertices,
		unsigned int vertexFlags,
		size_t vertexStrideBytes)
	{
		CapturedClusterLODGroup captured{};
		captured.depth = group.depth;
		captured.simplified = group.simplified;
		captured.flattenedIndices = group.flattenedIndices;
		captured.writeMaterialSlots = group.writeMaterialSlots;
		captured.clusters.reserve(group.clusters.size());
		for (const MergeCapturedCluster& source : group.clusters)
		{
			CapturedClusterLODCluster cluster{};
			cluster.refinedGroup = source.refinedGroup;
			cluster.bounds = source.bounds;
			cluster.indicesOffset = source.indicesOffset;
			cluster.indexCount = source.indexCount;
			cluster.vertexCount = source.vertexCount;
			cluster.materialSlot = source.materialSlot;
			captured.clusters.push_back(cluster);
		}
		const uint32_t quantExp = ComputeMeshQuantizationExponent(vertices, vertexStrideBytes);
		ClusterLODGroupBuildOutput built = BuildClusterLODGroupOutput(
			captured,
			sourceGroupLocalIndex,
			vertices,
			{},
			vertexFlags,
			vertexStrideBytes,
			nullptr,
			0u,
			static_cast<float>(1u << quantExp),
			quantExp,
			CLOD_NODE_BONE_LIMIT_DEFAULT,
			false);
		MergeGroupOutput out{};
		out.group = built.group;
		out.segments = std::move(built.segments);
		out.segmentBounds = std::move(built.segmentBounds);
		out.pageBlobs = std::move(built.pageBlobs);
		out.groupChunk = built.groupChunk;
		return out;
	}
}
