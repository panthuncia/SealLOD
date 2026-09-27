#include <BasicRenderer/Assets/Import/ClusterLODUtilities.h>
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODVoxelPacking.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODPagePackingTelemetry.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODBuildState.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODVoxelFallback.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODHierarchy.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODSkinningSidecar.h"

#include <limits>
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

		std::vector<unsigned int> positionRemap(vertexCount);
		meshopt_generatePositionRemap(
			positionRemap.data(), reinterpret_cast<const float*>(vertices.data()), vertexCount, vertexStrideBytes);

		std::vector<uint64_t> edges;
		edges.reserve(indices.size());
		auto appendEdge = [&](uint32_t sourceA, uint32_t sourceB)
		{
			if (sourceA >= vertexCount || sourceB >= vertexCount)
				return;
			const uint32_t a = positionRemap[sourceA];
			const uint32_t b = positionRemap[sourceB];
			if (a == b)
				return;
			const uint32_t lo = std::min(a, b);
			const uint32_t hi = std::max(a, b);
			edges.push_back((static_cast<uint64_t>(lo) << 32u) | hi);
		};

		for (size_t triangle = 0; triangle < indices.size(); triangle += 3u)
		{
			appendEdge(indices[triangle + 0u], indices[triangle + 1u]);
			appendEdge(indices[triangle + 1u], indices[triangle + 2u]);
			appendEdge(indices[triangle + 2u], indices[triangle + 0u]);
		}

		std::sort(edges.begin(), edges.end());
		std::vector<unsigned char> canonicalBoundary(vertexCount, 0u);
		size_t exteriorEdgeCount = 0;
		for (size_t begin = 0; begin < edges.size();)
		{
			size_t end = begin + 1u;
			while (end < edges.size() && edges[end] == edges[begin])
				++end;
			if (end - begin == 1u)
			{
				canonicalBoundary[static_cast<uint32_t>(edges[begin] >> 32u)] = 1u;
				canonicalBoundary[static_cast<uint32_t>(edges[begin])] = 1u;
				++exteriorEdgeCount;
			}
			begin = end;
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
	};

	struct CapturedClusterLODGroup
	{
		int depth = 0;
		clodBounds simplified{};
		std::vector<unsigned int> flattenedIndices;
		std::vector<CapturedClusterLODCluster> clusters;
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
		bool recomputeNormals)
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

		std::unordered_map<uint32_t, uint32_t> groupVertexToLocal;
		groupVertexToLocal.reserve(capturedGroup.clusters.size() * MS_MESHLET_SIZE);
		std::vector<uint32_t> groupLocalToGlobal;
		groupLocalToGlobal.reserve(capturedGroup.clusters.size() * MS_MESHLET_SIZE);

		auto getGroupLocalVertexIndex = [&](uint32_t globalVertexIndex) -> uint32_t
			{
				auto it = groupVertexToLocal.find(globalVertexIndex);
				if (it != groupVertexToLocal.end())
				{
					return it->second;
				}

				const uint32_t localIndex = static_cast<uint32_t>(groupLocalToGlobal.size());
				groupVertexToLocal.emplace(globalVertexIndex, localIndex);
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
				int32_t refinedGroup)
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
						bucket.refinedGroup);
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

					if (candidateSize > CLOD_PAGE_SIZE && !currentPage.meshletIndices.empty())
					{
						pageBins.emplace_back();
						PageBin& newPage = pageBins.back();
						newPage.attributeMask = comp.attributeMask;
						newPage.uvSetCount = static_cast<uint32_t>(comp.uvSets.size());
						newPage.totals = AddMeshletToPageTotals(PageTotals{}, 0u, 0u, newPage.attributeMask, newPage.uvSetCount, mi);
						newPage.meshletIndices.push_back(mi);
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
	const VoxelCoverageMaterialSampler* coverageMaterialSampler)
{
	ZoneScopedN("ClusterLODUtilities::BuildClusterLODArtifactsFromGeometry");
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

	if (simplifyAttributeCount > 0u)
	{
		ZoneScopedN("ClusterLODUtilities::Build::BuildSimplifyAttributes");
		simplifyAttributeStream.resize(globalVertexCount * static_cast<size_t>(simplifyAttributeCount));
		for (size_t vertexIndex = 0; vertexIndex < globalVertexCount; ++vertexIndex)
		{
			size_t destinationFloatOffset = vertexIndex * static_cast<size_t>(simplifyAttributeCount);

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
	mesh.vertex_attributes_stride = simplifyAttributeStream.empty() ? 0 : sizeof(float) * simplifyAttributeCount;
	mesh.vertex_lock = coveragePriorityFlags.empty() ? nullptr : coveragePriorityFlags.data();
	mesh.attribute_weights = simplifyAttributeWeights.empty() ? nullptr : simplifyAttributeWeights.data();
	mesh.attribute_count = simplifyAttributeStream.empty() ? 0 : simplifyAttributeCount;
	mesh.attribute_protect_mask = simplifyAttributeStream.empty() ? 0 : simplifyProtectMask;

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
	config.simplify_error_factor_sloppy = sloppyFallbackErrorFactor; // Scales error for sloppy groups

	config.simplify_fallback_permissive = false; // Simplify in permissive, disable fallback-only

	config.simplify_error_merge_additive = lodErrorMergeAdditive;
	config.simplify_error_merge_previous = lodErrorMergePrevious;

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
			float meshPositionQuantScale = 1.0f;
			uint32_t meshPositionQuantExp = 0;
			uint32_t nodeBoneLimit = CLOD_NODE_BONE_LIMIT_DEFAULT;
			bool recomputeGroupNormals = false;
			std::atomic<uint32_t> nextGroupId = 0;
			std::mutex finalizeMutex;
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

				ClusterLODGroupBuildOutput output;
				{
					ZoneScopedN("ClusterLODUtilities::Build::OutputGroup::BuildGroupOutput");
					output = BuildClusterLODGroupOutput(
						capturedGroup,
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
						context->recomputeGroupNormals);
				}

				ClusterLODGroup finalizedGroup = output.group;

				std::lock_guard<std::mutex> lock(context->finalizeMutex);

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

				(*context->groupChunks)[groupId] = output.groupChunk;
				(*context->groups)[groupId] = finalizedGroup;

		context->maxChildrenObserved = std::max(context->maxChildrenObserved, finalizedGroup.segmentCount);
				context->maxDepthObserved = (std::max)(context->maxDepthObserved, static_cast<uint32_t>(std::max(finalizedGroup.depth, 0)));

				return static_cast<int>(groupId);
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
		};

		CaptureOutputContext captureContext{};
		captureContext.vertices = &vertices;
		captureContext.uvSets = &uvSets;
		captureContext.vertexFlags = flags;
		captureContext.vertexStrideBytes = vertexStrideBytes;
		captureContext.skinningVertices = skinningVertices;
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
		captureContext.meshPositionQuantScale = meshPositionQuantScale;
		captureContext.meshPositionQuantExp = meshPositionQuantExp;
		captureContext.nodeBoneLimit = std::clamp(settings.nodeBoneLimit, 1u, CLOD_NODE_BONE_LIMIT_HARD_MAX);
		captureContext.recomputeGroupNormals = recomputeGroupNormals;
		{
			ZoneScopedN("ClusterLODUtilities::Build::clodBuildEx");
			clodBuildEx(config, mesh, &captureContext, &ClodBuildCallbacks::Output, nullptr);
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

		if (refinedCapSplitPartitionCount > 0)
		{
			spdlog::info(
				"ClusterLOD: refined-group cap split {} partitions at bucket target {}",
				refinedCapSplitPartitionCount,
				TargetBucketClusters);
		}
	}

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
	{
		ZoneScopedN("ClusterLODUtilities::Build::ReleaseRawStreamsAfterVoxelFallback");
		std::vector<std::vector<std::byte>>().swap(state.groupVertexChunks);
		std::vector<std::vector<std::byte>>().swap(state.groupSkinningChunks);
		std::vector<std::vector<uint32_t>>().swap(state.groupMeshletVertexChunks);
		std::vector<std::vector<meshopt_Meshlet>>().swap(state.groupMeshletChunks);
		std::vector<std::vector<uint8_t>>().swap(state.groupMeshletTriangleChunks);
		std::vector<std::vector<int32_t>>().swap(state.groupMeshletRefinedGroupChunks);
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
