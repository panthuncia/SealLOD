#include "Mesh/ClusterLODUtilities.h"

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

#include <spdlog/spdlog.h>

#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Mesh/VertexLayout.h"
#include "Mesh/VertexFlags.h"
#include "Mesh/VoxelGroupBuilder.h"
#include "Utilities/mikktspace.h"

#include "../shaders/Common/defines.h"

namespace
{
	constexpr uint32_t CLOD_COMPRESSED_POSITIONS = 1u << 0;
	constexpr uint32_t CLOD_COMPRESSED_MESHLET_VERTEX_INDICES = 1u << 1;
	constexpr uint32_t CLOD_COMPRESSED_NORMALS = 1u << 2;
	constexpr uint32_t CLOD_VOXEL_PAGE_MAGIC = 0x4C435856u; // VXCL
	constexpr uint32_t CLOD_VOXEL_PAGE_VERSION = 10u;
	constexpr uint32_t CLOD_VOXEL_PAGE_HEADER_SIZE = 64u;
	constexpr uint32_t CLOD_STREAMING_PAGE_SIZE_BYTES = 256u * 1024u;
	constexpr uint32_t CLOD_VOXEL_ATTRIBUTE_SAMPLES_PER_CUBE = 64u;
	constexpr uint32_t kMaxSkinInfluences = 8u;
	constexpr float CLOD_UV_QUANTIZATION_SCALE = 65535.0f;
	constexpr float CLOD_UV_QUANTIZATION_INV_SCALE = 1.0f / CLOD_UV_QUANTIZATION_SCALE;
	constexpr uint32_t CLOD_NATIVE_POSITION_FORMAT = CLOD_POSITION_FORMAT_FLOAT3;
	constexpr uint32_t CLOD_NATIVE_POSITION_STRIDE_BYTES = CLOD_POSITION_FORMAT_FLOAT3_STRIDE_BYTES;

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

	void AppendBits(std::vector<uint32_t>& words, uint64_t& bitCursor, uint32_t value, uint32_t bitCount)
	{
		if (bitCount == 0)
		{
			return;
		}

		const uint64_t requiredBits = bitCursor + bitCount;
		const size_t requiredWords = static_cast<size_t>((requiredBits + 31ull) / 32ull);
		if (words.size() < requiredWords)
		{
			words.resize(requiredWords, 0u);
		}

		const uint64_t bitOffset = bitCursor & 31ull;
		const uint64_t wordIndex = bitCursor >> 5ull;
		const uint64_t mask = (bitCount >= 32u) ? 0xffffffffull : ((1ull << bitCount) - 1ull);
		const uint64_t clampedValue = static_cast<uint64_t>(value) & mask;
		words[static_cast<size_t>(wordIndex)] |= static_cast<uint32_t>(clampedValue << bitOffset);

		const uint32_t spillBits = static_cast<uint32_t>(bitOffset) + bitCount;
		if (spillBits > 32u)
		{
			if (words.size() <= static_cast<size_t>(wordIndex + 1ull))
			{
				words.resize(static_cast<size_t>(wordIndex + 2ull), 0u);
			}
			words[static_cast<size_t>(wordIndex + 1ull)] |= static_cast<uint32_t>(clampedValue >> (32u - static_cast<uint32_t>(bitOffset)));
		}

		bitCursor += bitCount;
	}

	template<typename T>
	void StorePod(std::vector<std::byte>& bytes, size_t offset, const T& value)
	{
		if (bytes.size() < offset + sizeof(T))
		{
			bytes.resize(offset + sizeof(T));
		}
		std::memcpy(bytes.data() + offset, &value, sizeof(T));
	}

	std::vector<std::vector<std::byte>> BuildVoxelGroupPageBlobs(
		const CLodVoxelGroupDescriptor& groupDescriptor,
		std::span<const ClusterLODGroupSegment> pageSegments,
		std::span<const CLodVoxelClusterRecord> clusterRecords,
		std::span<const CLodVoxelCubeRecord> cubeRecords,
		std::span<const CLodVoxelAttributeSample> attributeSamples,
		uint32_t attributeSampleBase)
	{
		std::vector<std::vector<std::byte>> pageBlobs;
		if (clusterRecords.empty() || cubeRecords.empty())
		{
			return pageBlobs;
		}

		auto align4 = [](size_t value) -> size_t { return (value + 3u) & ~size_t(3); };
		const uint32_t groupDescriptorOffset = CLOD_VOXEL_PAGE_HEADER_SIZE;
		const uint32_t clusterRecordOffset = static_cast<uint32_t>(align4(static_cast<size_t>(groupDescriptorOffset) + sizeof(CLodVoxelGroupDescriptor)));

		pageBlobs.reserve(pageSegments.size());
		uint32_t runningFirstClusterInGroup = 0u;
		for (const ClusterLODGroupSegment& segment : pageSegments)
		{
			if (segment.meshletCount == 0u || segment.firstMeshletInPage != 0u)
			{
				pageBlobs.emplace_back();
				continue;
			}

			const uint32_t firstClusterInGroup = runningFirstClusterInGroup;
			if (firstClusterInGroup + segment.meshletCount > static_cast<uint32_t>(clusterRecords.size()))
			{
				pageBlobs.emplace_back();
				break;
			}
			runningFirstClusterInGroup += segment.meshletCount;

			const uint32_t pageClusterCount = segment.meshletCount;
			const uint32_t firstCubeInGroup = clusterRecords[firstClusterInGroup].firstCube;
			const CLodVoxelClusterRecord& lastCluster = clusterRecords[firstClusterInGroup + pageClusterCount - 1u];
			const uint32_t pageCubeCount = (lastCluster.firstCube + lastCluster.cubeCount) - firstCubeInGroup;
			const uint32_t pageCubeRecordOffset = static_cast<uint32_t>(align4(static_cast<size_t>(clusterRecordOffset) + pageClusterCount * sizeof(CLodVoxelClusterRecord)));
			const uint32_t attributeOffset = pageCubeRecordOffset + pageCubeCount * static_cast<uint32_t>(sizeof(CLodVoxelCubeRecord));
			const size_t pageSize = static_cast<size_t>(attributeOffset) +
				static_cast<size_t>(pageCubeCount) * CLOD_VOXEL_ATTRIBUTE_SAMPLES_PER_CUBE * sizeof(CLodVoxelAttributeSample);
			if (pageSize > CLOD_STREAMING_PAGE_SIZE_BYTES)
			{
				spdlog::error(
					"ClusterLOD voxel page build overflow: page={} first_cluster={} clusters={} cubes={} bytes={} page_limit={}",
					pageBlobs.size(),
					firstClusterInGroup,
					pageClusterCount,
					pageCubeCount,
					pageSize,
					CLOD_STREAMING_PAGE_SIZE_BYTES);
				pageBlobs.emplace_back();
				continue;
			}

			std::vector<std::byte> blob(pageSize, std::byte{ 0 });
			const std::array<uint32_t, 16> header = {
				CLOD_VOXEL_PAGE_MAGIC,
				CLOD_VOXEL_PAGE_VERSION,
				firstClusterInGroup,
				pageClusterCount,
				firstCubeInGroup,
				pageCubeCount,
				groupDescriptorOffset,
				0u,
				0u,
				clusterRecordOffset,
				pageCubeRecordOffset,
				attributeOffset,
				CLOD_VOXEL_ATTRIBUTE_SAMPLES_PER_CUBE,
				static_cast<uint32_t>(sizeof(CLodVoxelClusterRecord)),
				static_cast<uint32_t>(sizeof(CLodVoxelCubeRecord)),
				static_cast<uint32_t>(sizeof(CLodVoxelAttributeSample))
			};
			std::memcpy(blob.data(), header.data(), header.size() * sizeof(uint32_t));

			CLodVoxelGroupDescriptor pageDescriptor = groupDescriptor;
			pageDescriptor.firstCluster = 0u;
			pageDescriptor.clusterCount = static_cast<uint32_t>(clusterRecords.size());
			pageDescriptor.firstCube = 0u;
			pageDescriptor.cubeCount = static_cast<uint32_t>(cubeRecords.size());
			StorePod(blob, groupDescriptorOffset, pageDescriptor);

			for (uint32_t clusterIndex = 0; clusterIndex < pageClusterCount; ++clusterIndex)
			{
				CLodVoxelClusterRecord pageCluster = clusterRecords[firstClusterInGroup + clusterIndex];
				pageCluster.firstCube -= firstCubeInGroup;
				StorePod(blob, clusterRecordOffset + clusterIndex * sizeof(CLodVoxelClusterRecord), pageCluster);
			}

			for (uint32_t cubeIndex = 0; cubeIndex < pageCubeCount; ++cubeIndex)
			{
				CLodVoxelCubeRecord pageCube = cubeRecords[firstCubeInGroup + cubeIndex];
				const uint32_t globalFirstAttribute = pageCube.firstAttribute;
				pageCube.firstAttribute = cubeIndex * CLOD_VOXEL_ATTRIBUTE_SAMPLES_PER_CUBE;
				StorePod(blob, pageCubeRecordOffset + cubeIndex * sizeof(CLodVoxelCubeRecord), pageCube);

				if (globalFirstAttribute < attributeSampleBase)
				{
					continue;
				}
				const size_t attributeSourceOffset = static_cast<size_t>(globalFirstAttribute - attributeSampleBase) * sizeof(CLodVoxelAttributeSample);
				const size_t attributeDestOffset = static_cast<size_t>(attributeOffset) +
					static_cast<size_t>(pageCube.firstAttribute) * sizeof(CLodVoxelAttributeSample);
				const size_t attributeBytes = static_cast<size_t>(CLOD_VOXEL_ATTRIBUTE_SAMPLES_PER_CUBE) * sizeof(CLodVoxelAttributeSample);
				if (attributeSourceOffset + attributeBytes <= attributeSamples.size() * sizeof(CLodVoxelAttributeSample))
				{
					std::memcpy(blob.data() + attributeDestOffset,
						reinterpret_cast<const std::byte*>(attributeSamples.data()) + attributeSourceOffset,
						attributeBytes);
				}
			}

			pageBlobs.push_back(std::move(blob));
		}

		return pageBlobs;
	}

	uint32_t ComputeVoxelPageSizeBytes(uint32_t clusterCount, uint32_t cubeCount)
	{
		constexpr uint32_t fixedBytes = CLOD_VOXEL_PAGE_HEADER_SIZE + static_cast<uint32_t>(sizeof(CLodVoxelGroupDescriptor));
		return fixedBytes +
			clusterCount * static_cast<uint32_t>(sizeof(CLodVoxelClusterRecord)) +
			cubeCount * (static_cast<uint32_t>(sizeof(CLodVoxelCubeRecord)) +
				CLOD_VOXEL_ATTRIBUTE_SAMPLES_PER_CUBE * static_cast<uint32_t>(sizeof(CLodVoxelAttributeSample)));
	}

	void SplitVoxelClustersIntoPageSegments(
		PackedVoxelGroupBuildResult& packed,
		std::vector<ClusterLODGroupSegment>& outSegments,
		std::vector<BoundingSphere>& outSegmentBounds)
	{
		outSegments.clear();
		outSegmentBounds.clear();
		if (packed.clusterRecords.empty())
		{
			packed.descriptor.clusterCount = 0u;
			return;
		}

		std::vector<CLodVoxelClusterRecord> pageClusterRecords;
		pageClusterRecords.reserve(packed.clusterRecords.size());

		uint32_t pageFirstCluster = 0u;
		uint32_t pageClusterCount = 0u;
		uint32_t pageCubeCount = 0u;
		int32_t pageRefinedGroup = packed.clusterRecords.front().refinedGroup;
		DirectX::XMFLOAT4 pageBounds{ 0.0f, 0.0f, 0.0f, 0.0f };

		auto mergeBounds = [](const DirectX::XMFLOAT4& a, const DirectX::XMFLOAT4& b) -> DirectX::XMFLOAT4
		{
			if (a.w <= 0.0f) return b;
			if (b.w <= 0.0f) return a;
			const float dx = b.x - a.x;
			const float dy = b.y - a.y;
			const float dz = b.z - a.z;
			const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (dist + b.w <= a.w) return a;
			if (dist + a.w <= b.w) return b;
			const float r = (dist + a.w + b.w) * 0.5f;
			const float t = (r - a.w) / std::max(dist, 1.0e-12f);
			return { a.x + dx * t, a.y + dy * t, a.z + dz * t, r };
		};

		auto flushPage = [&]()
		{
			if (pageClusterCount == 0u)
			{
				return;
			}

			ClusterLODGroupSegment segment{};
			segment.refinedGroup = pageRefinedGroup;
			segment.firstMeshletInPage = 0u;
			segment.meshletCount = pageClusterCount;
			segment.pageIndex = pageFirstCluster;
			outSegments.push_back(segment);
			outSegmentBounds.push_back(BoundingSphere{ pageBounds });

			pageFirstCluster = static_cast<uint32_t>(pageClusterRecords.size());
			pageClusterCount = 0u;
			pageCubeCount = 0u;
			pageBounds = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
		};

		for (const CLodVoxelClusterRecord& sourceCluster : packed.clusterRecords)
		{
			uint32_t remainingCubes = sourceCluster.cubeCount;
			uint32_t cubeOffset = 0u;
			while (remainingCubes > 0u)
			{
				if (pageClusterCount != 0u && sourceCluster.refinedGroup != pageRefinedGroup)
				{
					flushPage();
				}
				if (pageClusterCount == 0u)
				{
					pageRefinedGroup = sourceCluster.refinedGroup;
					pageFirstCluster = static_cast<uint32_t>(pageClusterRecords.size());
				}

				uint32_t chunkCubes = remainingCubes;
				while (chunkCubes > 0u &&
					ComputeVoxelPageSizeBytes(pageClusterCount + 1u, pageCubeCount + chunkCubes) > CLOD_STREAMING_PAGE_SIZE_BYTES)
				{
					chunkCubes--;
				}

				if (chunkCubes == 0u)
				{
					flushPage();
					continue;
				}

				CLodVoxelClusterRecord pageCluster = sourceCluster;
				pageCluster.firstCube = sourceCluster.firstCube + cubeOffset;
				pageCluster.cubeCount = chunkCubes;
				pageClusterRecords.push_back(pageCluster);
				pageClusterCount++;
				pageCubeCount += chunkCubes;
				pageBounds = mergeBounds(pageBounds, pageCluster.bounds);

				remainingCubes -= chunkCubes;
				cubeOffset += chunkCubes;
			}
		}
		flushPage();

		packed.clusterRecords = std::move(pageClusterRecords);
		packed.descriptor.clusterCount = static_cast<uint32_t>(packed.clusterRecords.size());
		packed.descriptor.cubeCount = static_cast<uint32_t>(packed.cubeRecords.size());
		for (uint32_t pageIndex = 0; pageIndex < static_cast<uint32_t>(outSegments.size()); ++pageIndex)
		{
			outSegments[pageIndex].pageIndex = pageIndex;
		}
	}

	// Compute the exact byte size of a packed page blob in the new SoA format:
	// Header(64) + Descriptors(64*N) + PositionBitstream + optional streams + TriangleStream
	size_t ComputePageBlobSize(
		uint32_t attributeMask,
		uint32_t meshletCount,
		uint32_t pageUvSetCount,
		uint32_t totalPositionBytes,
		const std::vector<uint64_t>& totalUvBitsPerSet,
		uint32_t totalVertexCount,
		uint32_t totalNormalWords,
		uint32_t totalTangentFrameWords,
		uint32_t totalColorWords,
		uint32_t totalBoneIndexCount,
		uint32_t totalTriangleBytes)
	{
		auto align4 = [](size_t v) -> size_t { return (v + 3u) & ~size_t(3); };

		size_t size = CLOD_PAGE_HEADER_SIZE; // 64
		size = align4(size) + align4(static_cast<size_t>(meshletCount) * sizeof(CLodMeshletDescriptor));
		if (pageUvSetCount > 0u)
		{
			size = align4(size) + align4(static_cast<size_t>(meshletCount) * static_cast<size_t>(pageUvSetCount) * sizeof(CLodMeshletUvDescriptor));
		}
		size = align4(size) + align4(static_cast<size_t>(totalPositionBytes));
		if ((attributeMask & CLOD_PAGE_ATTRIBUTE_NORMAL) != 0u)
		{
			size = align4(size) + align4(static_cast<size_t>(totalNormalWords) * sizeof(uint32_t));
		}
		if ((attributeMask & CLOD_PAGE_ATTRIBUTE_TANGENT_FRAME) != 0u)
		{
			size = align4(size) + align4(static_cast<size_t>(totalTangentFrameWords) * sizeof(uint32_t));
		}
		if ((attributeMask & CLOD_PAGE_ATTRIBUTE_COLOR) != 0u)
		{
			size = align4(size) + align4(static_cast<size_t>(totalColorWords) * sizeof(uint32_t));
		}
		if ((attributeMask & CLOD_PAGE_ATTRIBUTE_JOINTS) != 0u)
		{
			size = align4(size) + align4(static_cast<size_t>(totalVertexCount) * sizeof(DirectX::XMUINT4) * 2u);
		}
		if ((attributeMask & CLOD_PAGE_ATTRIBUTE_WEIGHTS) != 0u)
		{
			size = align4(size) + align4(static_cast<size_t>(totalVertexCount) * sizeof(DirectX::XMFLOAT4) * 2u);
		}
		if (pageUvSetCount > 0u)
		{
			size = align4(size) + align4(static_cast<size_t>(pageUvSetCount) * sizeof(uint32_t));
			for (uint32_t uvSetIndex = 0; uvSetIndex < pageUvSetCount; ++uvSetIndex)
			{
				const uint64_t totalUvBits = uvSetIndex < totalUvBitsPerSet.size() ? totalUvBitsPerSet[uvSetIndex] : 0ull;
				size = align4(size) + align4(static_cast<size_t>((totalUvBits + 31ull) / 32ull) * sizeof(uint32_t));
			}
		}
		size = align4(size) + align4(static_cast<size_t>(totalBoneIndexCount) * sizeof(uint32_t));
		size = align4(size) + align4(static_cast<size_t>(totalTriangleBytes));
		return align4(size);
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

	struct MikkTangentBuildData
	{
		const std::vector<std::byte>* vertices = nullptr;
		size_t vertexStrideBytes = 0;
		const std::vector<uint32_t>* indices = nullptr;
		size_t positionByteOffset = 0;
		size_t normalByteOffset = 0;
		size_t texcoordByteOffset = 0;
		std::vector<DirectX::XMFLOAT3> accumulatedTangents;
		std::vector<float> accumulatedSigns;
		std::vector<uint32_t> accumulatedContributions;
	};

	int MikkGetNumFaces(const SMikkTSpaceContext* context)
	{
		const MikkTangentBuildData* data = static_cast<const MikkTangentBuildData*>(context->m_pUserData);
		return static_cast<int>(data->indices->size() / 3ull);
	}

	int MikkGetNumVerticesOfFace(const SMikkTSpaceContext*, const int)
	{
		return 3;
	}

	void MikkGetPosition(const SMikkTSpaceContext* context, float positionOut[], const int face, const int vertexInFace)
	{
		const MikkTangentBuildData* data = static_cast<const MikkTangentBuildData*>(context->m_pUserData);
		const size_t index = static_cast<size_t>(face) * 3ull + static_cast<size_t>(vertexInFace);
		const uint32_t vertexIndex = (*data->indices)[index];
		const DirectX::XMFLOAT3 position = ReadVertexFloat3(*data->vertices, data->vertexStrideBytes, vertexIndex, data->positionByteOffset);
		positionOut[0] = position.x;
		positionOut[1] = position.y;
		positionOut[2] = position.z;
	}

	void MikkGetNormal(const SMikkTSpaceContext* context, float normalOut[], const int face, const int vertexInFace)
	{
		const MikkTangentBuildData* data = static_cast<const MikkTangentBuildData*>(context->m_pUserData);
		const size_t index = static_cast<size_t>(face) * 3ull + static_cast<size_t>(vertexInFace);
		const uint32_t vertexIndex = (*data->indices)[index];
		const DirectX::XMFLOAT3 normal = ReadVertexFloat3(*data->vertices, data->vertexStrideBytes, vertexIndex, data->normalByteOffset);
		normalOut[0] = normal.x;
		normalOut[1] = normal.y;
		normalOut[2] = normal.z;
	}

	void MikkGetTexCoord(const SMikkTSpaceContext* context, float texcoordOut[], const int face, const int vertexInFace)
	{
		const MikkTangentBuildData* data = static_cast<const MikkTangentBuildData*>(context->m_pUserData);
		const size_t index = static_cast<size_t>(face) * 3ull + static_cast<size_t>(vertexInFace);
		const uint32_t vertexIndex = (*data->indices)[index];
		const DirectX::XMFLOAT2 texcoord = ReadVertexFloat2(*data->vertices, data->vertexStrideBytes, vertexIndex, data->texcoordByteOffset);
		texcoordOut[0] = texcoord.x;
		texcoordOut[1] = texcoord.y;
	}

	void MikkSetTSpaceBasic(const SMikkTSpaceContext* context, const float tangent[], const float sign, const int face, const int vertexInFace)
	{
		MikkTangentBuildData* data = static_cast<MikkTangentBuildData*>(context->m_pUserData);
		const size_t index = static_cast<size_t>(face) * 3ull + static_cast<size_t>(vertexInFace);
		const uint32_t vertexIndex = (*data->indices)[index];
		if (vertexIndex >= data->accumulatedTangents.size())
		{
			return;
		}

		DirectX::XMFLOAT3& accumulatedTangent = data->accumulatedTangents[vertexIndex];
		accumulatedTangent.x += tangent[0];
		accumulatedTangent.y += tangent[1];
		accumulatedTangent.z += tangent[2];
		data->accumulatedSigns[vertexIndex] += sign;
		data->accumulatedContributions[vertexIndex] += 1u;
	}

	bool GenerateMikkTangents(
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

		MikkTangentBuildData buildData{};
		buildData.vertices = &vertices;
		buildData.vertexStrideBytes = vertexStrideBytes;
		buildData.indices = &indices;
		buildData.positionByteOffset = PositionByteOffset;
		buildData.normalByteOffset = NormalByteOffset;
		buildData.texcoordByteOffset = TexcoordByteOffset;
		buildData.accumulatedTangents.assign(vertexCount, DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f));
		buildData.accumulatedSigns.assign(vertexCount, 0.0f);
		buildData.accumulatedContributions.assign(vertexCount, 0u);

		SMikkTSpaceInterface mikkInterface{};
		mikkInterface.m_getNumFaces = &MikkGetNumFaces;
		mikkInterface.m_getNumVerticesOfFace = &MikkGetNumVerticesOfFace;
		mikkInterface.m_getPosition = &MikkGetPosition;
		mikkInterface.m_getNormal = &MikkGetNormal;
		mikkInterface.m_getTexCoord = &MikkGetTexCoord;
		mikkInterface.m_setTSpaceBasic = &MikkSetTSpaceBasic;

		SMikkTSpaceContext mikkContext{};
		mikkContext.m_pInterface = &mikkInterface;
		mikkContext.m_pUserData = &buildData;

		if (genTangSpaceDefault(&mikkContext) == 0)
		{
			return false;
		}

		outTangents.resize(vertexCount);
		for (size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
		{
			DirectX::XMFLOAT3 tangent = buildData.accumulatedTangents[vertexIndex];
			const float tangentLenSq = tangent.x * tangent.x + tangent.y * tangent.y + tangent.z * tangent.z;

			if (buildData.accumulatedContributions[vertexIndex] == 0u || tangentLenSq <= 1e-20f ||
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

			const float sign = buildData.accumulatedSigns[vertexIndex] < 0.0f ? -1.0f : 1.0f;
			outTangents[vertexIndex] = DirectX::XMFLOAT4(tangent.x, tangent.y, tangent.z, sign);
		}

		return true;
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

		std::vector<DirectX::XMFLOAT3> result;
		result.resize(groupLocalToGlobal.size());

		for (size_t groupVertex = 0; groupVertex < groupLocalToGlobal.size(); ++groupVertex)
		{
			const DirectX::XMFLOAT3 sourceNormal = ReadVertexFloat3(
				vertices,
				vertexStrideBytes,
				groupLocalToGlobal[groupVertex],
				NormalByteOffset);

			result[groupVertex] = NormalizeOrFallback(accumulatedNormals[groupVertex], sourceNormal);
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
		bool recomputeNormals)
	{
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

		std::unordered_map<int32_t, uint32_t> bucketLookup;
		bucketLookup.reserve(capturedGroup.clusters.size());

		auto addToBucket = [&](int32_t refinedGroup, uint32_t clusterIndex)
			{
				auto lookupIt = bucketLookup.find(refinedGroup);
				if (lookupIt != bucketLookup.end())
				{
					buckets[lookupIt->second].clusterIndices.push_back(clusterIndex);
					return;
				}

				const uint32_t newBucketIndex = static_cast<uint32_t>(buckets.size());
				bucketLookup.emplace(refinedGroup, newBucketIndex);
				buckets.push_back(ChildBucket{ refinedGroup, {} });
				buckets.back().clusterIndices.reserve(8);
				buckets.back().clusterIndices.push_back(clusterIndex);
			};

		for (uint32_t clusterIndex = 0; clusterIndex < static_cast<uint32_t>(capturedGroup.clusters.size()); ++clusterIndex)
		{
			addToBucket(capturedGroup.clusters[clusterIndex].refinedGroup, clusterIndex);
		}

		// Track which bucket (refinedGroup) each meshlet belongs to
		std::vector<int32_t> meshletBucketTag;
		meshletBucketTag.reserve(capturedGroup.clusters.size());

		uint32_t groupMeshletVertexCursor = 0;
		uint32_t localMeshletCursor = 0;

		for (const ChildBucket& bucket : buckets)
		{
			for (uint32_t clusterIndex : bucket.clusterIndices)
			{
				const CapturedClusterLODCluster& cluster = capturedGroup.clusters[clusterIndex];
				const uint32_t triangleCount = cluster.indexCount / 3;
				const unsigned int* clusterIndices = capturedGroup.flattenedIndices.data() + cluster.indicesOffset;

				std::vector<unsigned int> localVertices(cluster.vertexCount);
				std::vector<unsigned char> localTriangles(cluster.indexCount);

				const size_t uniqueVertexCount = clodLocalIndices(
					localVertices.data(),
					localTriangles.data(),
					clusterIndices,
					cluster.indexCount);

				assert(uniqueVertexCount == cluster.vertexCount);

				std::vector<uint32_t> groupLocalVertices(uniqueVertexCount);
				for (size_t localVertex = 0; localVertex < uniqueVertexCount; ++localVertex)
				{
					groupLocalVertices[localVertex] = getGroupLocalVertexIndex(localVertices[localVertex]);
				}

				meshopt_Meshlet meshlet{};
				meshlet.vertex_offset = groupMeshletVertexCursor;
				meshlet.triangle_offset = static_cast<uint32_t>(output.meshletTriangles.size());
				meshlet.vertex_count = static_cast<uint32_t>(uniqueVertexCount);
				meshlet.triangle_count = triangleCount;

				output.meshletVertices.insert(output.meshletVertices.end(), groupLocalVertices.begin(), groupLocalVertices.end());
				output.meshletTriangles.insert(output.meshletTriangles.end(), localTriangles.begin(), localTriangles.end());
				groupMeshletVertexCursor += static_cast<uint32_t>(uniqueVertexCount);

				BoundingSphere sphere{};
				sphere.sphere = DirectX::XMFLOAT4(cluster.bounds.center[0], cluster.bounds.center[1], cluster.bounds.center[2], cluster.bounds.radius);

				output.meshlets.push_back(meshlet);
				output.meshletBounds.push_back(sphere);
				meshletBucketTag.push_back(bucket.refinedGroup);

				++localMeshletCursor;
			}
		}

		assert(localMeshletCursor == output.group.meshletCount);
		output.meshletRefinedGroups = meshletBucketTag;

		output.group.groupVertexCount = static_cast<uint32_t>(groupLocalToGlobal.size());

		output.vertexChunk.reserve(static_cast<size_t>(output.group.groupVertexCount) * vertexStrideBytes);

		for (uint32_t globalVertexIndex : groupLocalToGlobal)
		{
			const size_t sourceVertexByteOffset = static_cast<size_t>(globalVertexIndex) * vertexStrideBytes;
			output.vertexChunk.insert(
				output.vertexChunk.end(),
				vertices.begin() + static_cast<std::ptrdiff_t>(sourceVertexByteOffset),
				vertices.begin() + static_cast<std::ptrdiff_t>(sourceVertexByteOffset + vertexStrideBytes));
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
			groupNormals.resize(groupLocalToGlobal.size());

			if (recomputeNormals)
			{
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
					groupNormals[groupVertexIndex] = ReadVertexFloat3(vertices, vertexStrideBytes, groupLocalToGlobal[groupVertexIndex], MeshVertexLayout::NormalOffset);
				}
			}
		}

		std::vector<DirectX::XMFLOAT4> groupTangents;
		if (hasTangentStream)
		{
			groupTangents.resize(groupLocalToGlobal.size());
			for (size_t groupVertexIndex = 0; groupVertexIndex < groupLocalToGlobal.size(); ++groupVertexIndex)
			{
				const uint32_t globalVertexIndex = groupLocalToGlobal[groupVertexIndex];
				DirectX::XMFLOAT4 tangent{};
				const size_t offset = static_cast<size_t>(globalVertexIndex) * vertexStrideBytes + MeshVertexLayout::TangentOffset(vertexFlags);
				std::memcpy(&tangent, vertices.data() + offset, sizeof(tangent));
				groupTangents[groupVertexIndex] = tangent;
			}
		}

		std::vector<MeshUvSetData> groupUvSets;
		groupUvSets.reserve(uvSets.size());
		const size_t sourceVertexCount = vertexStrideBytes > 0 ? (vertices.size() / vertexStrideBytes) : 0u;
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

		if (groupUvSets.empty() && hasTexcoordStream)
		{
			MeshUvSetData legacyUvSet;
			legacyUvSet.name = "UV0";
			legacyUvSet.values.resize(groupLocalToGlobal.size());
			for (size_t groupVertexIndex = 0; groupVertexIndex < groupLocalToGlobal.size(); ++groupVertexIndex)
			{
				legacyUvSet.values[groupVertexIndex] = ReadVertexFloat2(
					vertices,
					vertexStrideBytes,
					groupLocalToGlobal[groupVertexIndex],
					MeshVertexLayout::TexcoordOffset(vertexFlags));
			}
			groupUvSets.push_back(std::move(legacyUvSet));
		}

		std::vector<uint32_t> compressedColorWords;
		if (hasColorStream)
		{
			compressedColorWords.reserve(groupLocalToGlobal.size());
			for (size_t groupVertexIndex = 0; groupVertexIndex < groupLocalToGlobal.size(); ++groupVertexIndex)
			{
				const DirectX::XMFLOAT3 color = ReadVertexFloat3(
					vertices,
					vertexStrideBytes,
					groupLocalToGlobal[groupVertexIndex],
					MeshVertexLayout::ColorOffset(vertexFlags));
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
			constexpr size_t JointByteOffset = sizeof(DirectX::XMFLOAT3) + sizeof(DirectX::XMFLOAT3);
			groupSkinningInfluences.resize(groupLocalToGlobal.size(), PackedSkinningInfluences{});
			output.skinningChunk.reserve(groupLocalToGlobal.size() * skinningVertexStrideBytes);

			for (size_t groupVertexIndex = 0; groupVertexIndex < groupLocalToGlobal.size(); ++groupVertexIndex)
			{
				const uint32_t globalVertexIndex = groupLocalToGlobal[groupVertexIndex];
				if (globalVertexIndex >= sourceSkinningVertexCount)
				{
					continue;
				}

				const size_t sourceByteOffset = static_cast<size_t>(globalVertexIndex) * skinningVertexStrideBytes;
				output.skinningChunk.insert(
					output.skinningChunk.end(),
					skinningVertices->begin() + static_cast<std::ptrdiff_t>(sourceByteOffset),
					skinningVertices->begin() + static_cast<std::ptrdiff_t>(sourceByteOffset + skinningVertexStrideBytes));
				std::memcpy(&groupSkinningInfluences[groupVertexIndex],
					skinningVertices->data() + sourceByteOffset + JointByteOffset,
					sizeof(PackedSkinningInfluences));
			}
		}

		std::vector<DirectX::XMFLOAT3> groupPositions;
		groupPositions.reserve(groupLocalToGlobal.size());
		std::vector<std::array<int32_t, 3>> quantizedGroupPositions;
		quantizedGroupPositions.reserve(groupLocalToGlobal.size());

		for (uint32_t globalVertexIndex : groupLocalToGlobal)
		{
			const size_t byteOffset = static_cast<size_t>(globalVertexIndex) * vertexStrideBytes;
			float px = 0.0f;
			float py = 0.0f;
			float pz = 0.0f;
			std::memcpy(&px, vertices.data() + byteOffset, sizeof(float));
			std::memcpy(&py, vertices.data() + byteOffset + sizeof(float), sizeof(float));
			std::memcpy(&pz, vertices.data() + byteOffset + sizeof(float) * 2, sizeof(float));

			groupPositions.emplace_back(px, py, pz);

			const int32_t qx = static_cast<int32_t>(std::floor(px * meshPositionQuantScale + 0.5f));
			const int32_t qy = static_cast<int32_t>(std::floor(py * meshPositionQuantScale + 0.5f));
			const int32_t qz = static_cast<int32_t>(std::floor(pz * meshPositionQuantScale + 0.5f));

			quantizedGroupPositions.push_back({ qx, qy, qz });
		}

		// Compress normals for page blob construction (oct-encoded, one word per vertex)
		std::vector<uint32_t> compressedNormalWords;
		if (hasNormalStream)
		{
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
					std::unordered_set<uint32_t> uniqueBones;
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
								uniqueBones.insert(jointValues[influence]);
							}
						}
					}

					comp.boneList.assign(uniqueBones.begin(), uniqueBones.end());
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

			auto ComputePageTotals = [&](const std::vector<uint32_t>& meshletIndices, uint32_t pageMask, uint32_t pageUvSetCount) -> PageTotals
			{
				PageTotals totals{};
				totals.totalUvBitsPerSet.assign(pageUvSetCount, 0ull);
				for (uint32_t meshletIndex : meshletIndices)
				{
					const auto& meshlet = output.meshlets[meshletIndex];
					const auto& comp = perMeshletComp[meshletIndex];
					totals.totalPositionBytes += GetMeshletPositionBytes(meshlet);
					totals.totalVertexCount += meshlet.vertex_count;
					for (uint32_t uvSetIndex = 0; uvSetIndex < pageUvSetCount; ++uvSetIndex)
					{
						totals.totalUvBitsPerSet[uvSetIndex] += GetMeshletUvBits(uvSetIndex, meshlet, comp);
					}
					totals.totalNormalWords += GetMeshletNormalWords(pageMask, meshlet);
					totals.totalTangentFrameWords += GetMeshletTangentFrameWords(pageMask, meshlet);
					totals.totalColorWords += GetMeshletColorWords(pageMask, meshlet);
					totals.totalBoneIndexCount += static_cast<uint32_t>(comp.boneList.size());
					totals.totalTriangleBytes += meshlet.triangle_count * 3u;
				}
				return totals;
			};

			// === Page binning (simplified: no vertex set dedup) ===
			struct PageBin {
				std::vector<uint32_t> meshletIndices;
				uint32_t attributeMask = 0;
				uint32_t uvSetCount = 0;
			};

			std::vector<PageBin> pageBins;
			pageBins.emplace_back();

			for (uint32_t mi = 0; mi < totalMeshlets; ++mi)
			{
				const auto& meshlet = output.meshlets[mi];
				const auto& comp = perMeshletComp[mi];
				PageBin& currentPage = pageBins.back();
				uint32_t candidateMask = currentPage.attributeMask | comp.attributeMask;
				uint32_t candidateUvSetCount = std::max(currentPage.uvSetCount, static_cast<uint32_t>(comp.uvSets.size()));
				std::vector<uint32_t> candidateMeshlets = currentPage.meshletIndices;
				candidateMeshlets.push_back(mi);
				PageTotals candidateTotals = ComputePageTotals(candidateMeshlets, candidateMask, candidateUvSetCount);
				const size_t candidateSize = ComputePageBlobSize(
					candidateMask,
					static_cast<uint32_t>(candidateMeshlets.size()),
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
					newPage.meshletIndices.push_back(mi);
					continue;
				}

				currentPage.attributeMask = candidateMask;
				currentPage.uvSetCount = candidateUvSetCount;
				currentPage.meshletIndices.push_back(mi);
			}

			if (!pageBins.empty() && pageBins.back().meshletIndices.empty())
			{
				pageBins.pop_back();
			}

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

			for (uint32_t pi = 0; pi < static_cast<uint32_t>(pageBins.size()); ++pi)
			{
				const PageBin& page = pageBins[pi];
				const uint32_t pageMeshletCount = static_cast<uint32_t>(page.meshletIndices.size());
				if (pageMeshletCount == 0) continue;
				const PageTotals pageTotals = ComputePageTotals(page.meshletIndices, page.attributeMask, page.uvSetCount);
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
				std::vector<CLodMeshletDescriptor> descriptors(pageMeshletCount);
				std::vector<CLodMeshletUvDescriptor> uvDescriptors(static_cast<size_t>(pageMeshletCount) * static_cast<size_t>(page.uvSetCount));
				std::vector<DirectX::XMFLOAT3> pagePositions;
				std::vector<std::vector<uint32_t>> pageUvWordsPerSet(page.uvSetCount);
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

					CLodMeshletDescriptor& desc = descriptors[li];
					desc.positionBitOffset = pagePositionByteCursor;
					desc.vertexAttributeOffset = vertexAttributeCursor;
					desc.triangleByteOffset = triangleByteCursor;
					desc.boneListOffset = boneIndexCursor;
					desc.minQx = 0;
					desc.minQy = 0;
					desc.minQz = 0;
					desc.bitsAndVertexCount =
						((meshlet.vertex_count & 0xFFu) << 24u);

					const int32_t tag = meshletBucketTag[mi];
					const uint32_t refinedGroupEncoded = (tag >= 0) ? static_cast<uint32_t>(tag + 1) : 0u;
					desc.triangleCountAndRefinedGroup =
						(meshlet.triangle_count & 0xFFFFu)
						| (refinedGroupEncoded << 16u);
					desc.boneCount = static_cast<uint32_t>(comp.boneList.size());
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
							CLodMeshletUvDescriptor& uvDesc = uvDescriptors[static_cast<size_t>(li) * static_cast<size_t>(page.uvSetCount) + uvSetIndex];
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
						}
					}

					// Append per-meshlet native positions to the shared page stream.
					for (uint32_t vi = 0; vi < meshlet.vertex_count; ++vi)
					{
						const uint32_t gv = output.meshletVertices[meshlet.vertex_offset + vi];
						pagePositions.push_back(groupPositions[gv]);
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
					if (!comp.boneList.empty())
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

								AppendBits(pageUvWordsPerSet[uvSetIndex], pageUvBitCursors[uvSetIndex], encodedU, uvBitsU);
								AppendBits(pageUvWordsPerSet[uvSetIndex], pageUvBitCursors[uvSetIndex], encodedV, uvBitsV);
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

				// Write descriptors
				std::memcpy(blob.data() + descriptorOffset, descriptors.data(), descriptorBytes);
				if (pageHasUvSets && !uvDescriptors.empty())
				{
					std::memcpy(blob.data() + uvDescriptorOffset, uvDescriptors.data(), uvDescriptorBytes);
				}

				// Write native position stream.
				if (!pagePositions.empty())
				{
					std::memcpy(blob.data() + positionBitstreamOffset,
						pagePositions.data(),
						pagePositions.size() * sizeof(DirectX::XMFLOAT3));
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

		return output;
	}

	BoundingSphere BuildObjectBoundingSphereFromRootNode(const std::vector<ClusterLODNode>& nodes, uint32_t rootNodeIndex)
	{
		BoundingSphere sphere{};
		if (rootNodeIndex >= nodes.size()) {
			return sphere;
		}

		const ClusterLODTraversalMetric& metric = nodes[rootNodeIndex].traversalMetric;
		sphere.sphere = metric.cullingSphere;
		return sphere;
	}

	uint32_t ComputeCLodTraversalDepth(const std::vector<ClusterLODNode>& nodes, uint32_t rootNodeIndex)
	{
		if (rootNodeIndex >= nodes.size()) {
			return 0u;
		}

		std::function<uint32_t(uint32_t)> computeNodeDepth = [&](uint32_t nodeIndex) -> uint32_t {
			if (nodeIndex >= nodes.size()) {
				return 0u;
			}

			const ClusterLODNode& node = nodes[nodeIndex];
			if (node.range.isGroup != 0u) {
				return 1u;
			}

			const uint32_t childCount = node.range.countMinusOne + 1u;
			uint32_t maxChildDepth = 0u;
			for (uint32_t childIndex = 0; childIndex < childCount; ++childIndex)
			{
				maxChildDepth = std::max(maxChildDepth, computeNodeDepth(node.range.indexOrOffset + childIndex));
			}

			return 1u + maxChildDepth;
		};

		return computeNodeDepth(rootNodeIndex);
	}

	struct ClusterLODBuildState
	{
		std::vector<ClusterLODGroup> groups;
		std::vector<ClusterLODGroupSegment> segments;
		std::vector<BoundingSphere> segmentBounds;
		std::vector<ClusterLODGroupChunk> groupChunks;
		std::vector<std::vector<std::vector<std::byte>>> groupPageBlobs;

		// Raw per-group streams kept for voxel fallback candidate construction.
		std::vector<std::vector<std::byte>> groupVertexChunks;
		std::vector<std::vector<std::byte>> groupSkinningChunks;
		std::vector<std::vector<uint32_t>> groupMeshletVertexChunks;
		std::vector<std::vector<meshopt_Meshlet>> groupMeshletChunks;
		std::vector<std::vector<uint8_t>> groupMeshletTriangleChunks;
		std::vector<std::vector<int32_t>> groupMeshletRefinedGroupChunks;

		std::vector<ClusterLODNode> nodes;
		std::vector<ClusterLODNodeRangeAlloc> lodNodeRanges;
		std::vector<uint32_t> lodLevelRoots;
		uint32_t topRootNode = 0;
		uint32_t maxDepth = 0;
		uint32_t maxTraversalDepth = 0;
		VoxelGroupMapping voxelGroupMapping;
		std::vector<VoxelGroupPayload> voxelCarryPayloads;
	};

	struct TriangleMeshPageSegmentRef
	{
		uint32_t groupIndex = 0;
		uint32_t segmentIndex = 0;
		uint32_t sourcePageIndex = 0;
		uint32_t firstMeshletInPage = 0;
		uint32_t meshletCount = 0;
	};

	struct TriangleMeshPageBuildTotals
	{
		uint32_t meshletCount = 0;
		uint32_t totalPositionBytes = 0;
		std::vector<uint64_t> totalUvBitsPerSet;
		uint32_t totalVertexCount = 0;
		uint32_t totalNormalWords = 0;
		uint32_t totalTangentFrameWords = 0;
		uint32_t totalColorWords = 0;
		uint32_t totalBoneIndexCount = 0;
		uint32_t totalTriangleBytes = 0;
	};

	template<typename T>
	bool ReadPodAt(const std::vector<std::byte>& bytes, size_t offset, T& outValue)
	{
		if (offset + sizeof(T) > bytes.size())
		{
			return false;
		}
		std::memcpy(&outValue, bytes.data() + offset, sizeof(T));
		return true;
	}

	uint32_t ReadUint32At(const std::vector<std::byte>& bytes, size_t offset)
	{
		uint32_t value = 0u;
		(void)ReadPodAt(bytes, offset, value);
		return value;
	}

	uint32_t DecodeMeshletVertexCount(const CLodMeshletDescriptor& desc)
	{
		return (desc.bitsAndVertexCount >> 24u) & 0xFFu;
	}

	uint32_t DecodeMeshletTriangleCount(const CLodMeshletDescriptor& desc)
	{
		return desc.triangleCountAndRefinedGroup & 0xFFFFu;
	}

	uint32_t DecodeUvBitsU(const CLodMeshletUvDescriptor& desc)
	{
		return desc.uvBits & 0xFFu;
	}

	uint32_t DecodeUvBitsV(const CLodMeshletUvDescriptor& desc)
	{
		return (desc.uvBits >> 8u) & 0xFFu;
	}

	bool IsVoxelPageBlob(const std::vector<std::byte>& blob)
	{
		return blob.size() >= sizeof(uint32_t) &&
			ReadUint32At(blob, 0u) == CLOD_VOXEL_PAGE_MAGIC;
	}

	bool ReadTrianglePageHeader(const std::vector<std::byte>& blob, CLodPageHeader& outHeader)
	{
		if (blob.size() < sizeof(CLodPageHeader) || IsVoxelPageBlob(blob))
		{
			return false;
		}
		if (!ReadPodAt(blob, 0u, outHeader))
		{
			return false;
		}
		return outHeader.compressedPositionQuantExp == CLOD_POSITION_FORMAT_FLOAT3 &&
			outHeader.descriptorOffset != 0u &&
			outHeader.positionBitstreamOffset != 0u &&
			outHeader.triangleStreamOffset != 0u;
	}

	bool ReadTriangleMeshletDescriptor(
		const std::vector<std::byte>& blob,
		const CLodPageHeader& header,
		uint32_t meshletIndex,
		CLodMeshletDescriptor& outDescriptor)
	{
		if (meshletIndex >= header.meshletCount)
		{
			return false;
		}
		return ReadPodAt(
			blob,
			static_cast<size_t>(header.descriptorOffset) + static_cast<size_t>(meshletIndex) * sizeof(CLodMeshletDescriptor),
			outDescriptor);
	}

	bool ReadTriangleUvDescriptor(
		const std::vector<std::byte>& blob,
		const CLodPageHeader& header,
		uint32_t meshletIndex,
		uint32_t uvSetIndex,
		CLodMeshletUvDescriptor& outDescriptor)
	{
		if (header.uvSetCount == 0u || header.uvDescriptorOffset == 0u ||
			meshletIndex >= header.meshletCount || uvSetIndex >= header.uvSetCount)
		{
			return false;
		}
		const size_t descriptorIndex =
			static_cast<size_t>(meshletIndex) * static_cast<size_t>(header.uvSetCount) + uvSetIndex;
		return ReadPodAt(
			blob,
			static_cast<size_t>(header.uvDescriptorOffset) + descriptorIndex * sizeof(CLodMeshletUvDescriptor),
			outDescriptor);
	}

	void AppendBitsFromBytes(
		const std::vector<std::byte>& source,
		uint32_t sourceByteOffset,
		uint64_t sourceBitOffset,
		uint64_t bitCount,
		std::vector<uint32_t>& destWords,
		uint64_t& destBitCursor)
	{
		for (uint64_t bitIndex = 0; bitIndex < bitCount; ++bitIndex)
		{
			const uint64_t absoluteSourceBit = static_cast<uint64_t>(sourceByteOffset) * 8ull + sourceBitOffset + bitIndex;
			const size_t sourceByteIndex = static_cast<size_t>(absoluteSourceBit >> 3ull);
			if (sourceByteIndex >= source.size())
			{
				AppendBits(destWords, destBitCursor, 0u, 1u);
				continue;
			}

			const uint32_t sourceBitInByte = static_cast<uint32_t>(absoluteSourceBit & 7ull);
			const uint32_t bitValue = (std::to_integer<uint32_t>(source[sourceByteIndex]) >> sourceBitInByte) & 1u;
			AppendBits(destWords, destBitCursor, bitValue, 1u);
		}
	}

	TriangleMeshPageBuildTotals ComputeTriangleMeshPageTotals(
		const ClusterLODBuildState& state,
		std::span<const TriangleMeshPageSegmentRef> segments,
		uint32_t attributeMask,
		uint32_t uvSetCount)
	{
		TriangleMeshPageBuildTotals totals{};
		totals.totalUvBitsPerSet.assign(uvSetCount, 0ull);

		for (const TriangleMeshPageSegmentRef& segment : segments)
		{
			if (segment.groupIndex >= state.groupPageBlobs.size() ||
				segment.sourcePageIndex >= state.groupPageBlobs[segment.groupIndex].size())
			{
				continue;
			}

			const std::vector<std::byte>& blob = state.groupPageBlobs[segment.groupIndex][segment.sourcePageIndex];
			CLodPageHeader header{};
			if (!ReadTrianglePageHeader(blob, header))
			{
				continue;
			}

			for (uint32_t localMeshlet = 0; localMeshlet < segment.meshletCount; ++localMeshlet)
			{
				const uint32_t sourceMeshletIndex = segment.firstMeshletInPage + localMeshlet;
				CLodMeshletDescriptor desc{};
				if (!ReadTriangleMeshletDescriptor(blob, header, sourceMeshletIndex, desc))
				{
					continue;
				}

				const uint32_t vertexCount = DecodeMeshletVertexCount(desc);
				const uint32_t triangleCount = DecodeMeshletTriangleCount(desc);
				totals.meshletCount++;
				totals.totalPositionBytes += vertexCount * CLOD_NATIVE_POSITION_STRIDE_BYTES;
				totals.totalVertexCount += vertexCount;
				totals.totalNormalWords += ((attributeMask & CLOD_PAGE_ATTRIBUTE_NORMAL) != 0u) ? vertexCount : 0u;
				totals.totalTangentFrameWords += ((attributeMask & CLOD_PAGE_ATTRIBUTE_TANGENT_FRAME) != 0u) ? vertexCount : 0u;
				totals.totalColorWords += ((attributeMask & CLOD_PAGE_ATTRIBUTE_COLOR) != 0u) ? vertexCount : 0u;
				totals.totalBoneIndexCount += desc.boneCount;
				totals.totalTriangleBytes += triangleCount * 3u;

				for (uint32_t uvSetIndex = 0; uvSetIndex < uvSetCount; ++uvSetIndex)
				{
					CLodMeshletUvDescriptor uvDesc{};
					if (uvSetIndex < header.uvSetCount &&
						ReadTriangleUvDescriptor(blob, header, sourceMeshletIndex, uvSetIndex, uvDesc))
					{
						totals.totalUvBitsPerSet[uvSetIndex] +=
							static_cast<uint64_t>(vertexCount) *
							static_cast<uint64_t>(DecodeUvBitsU(uvDesc) + DecodeUvBitsV(uvDesc));
					}
					else
					{
						totals.totalUvBitsPerSet[uvSetIndex] += static_cast<uint64_t>(vertexCount) * 2ull;
					}
				}
			}
		}

		return totals;
	}

	std::vector<std::byte> BuildPackedTriangleMeshPageBlob(
		const ClusterLODBuildState& state,
		std::span<const TriangleMeshPageSegmentRef> segments,
		uint32_t attributeMask,
		uint32_t uvSetCount)
	{
		auto align4 = [](size_t v) -> size_t { return (v + 3u) & ~size_t(3); };
		const TriangleMeshPageBuildTotals totals = ComputeTriangleMeshPageTotals(state, segments, attributeMask, uvSetCount);
		if (totals.meshletCount == 0u)
		{
			return {};
		}

		const bool pageHasNormals = (attributeMask & CLOD_PAGE_ATTRIBUTE_NORMAL) != 0u;
		const bool pageHasTangentFrames = (attributeMask & CLOD_PAGE_ATTRIBUTE_TANGENT_FRAME) != 0u;
		const bool pageHasColors = (attributeMask & CLOD_PAGE_ATTRIBUTE_COLOR) != 0u;
		const bool pageHasJoints = (attributeMask & CLOD_PAGE_ATTRIBUTE_JOINTS) != 0u;
		const bool pageHasWeights = (attributeMask & CLOD_PAGE_ATTRIBUTE_WEIGHTS) != 0u;
		const bool pageHasUvSets = uvSetCount > 0u;

		const uint32_t descriptorOffset = static_cast<uint32_t>(align4(CLOD_PAGE_HEADER_SIZE));
		const size_t descriptorBytes = static_cast<size_t>(totals.meshletCount) * sizeof(CLodMeshletDescriptor);
		const uint32_t uvDescriptorOffset = pageHasUvSets
			? static_cast<uint32_t>(align4(descriptorOffset + descriptorBytes))
			: 0u;
		const size_t uvDescriptorBytes = pageHasUvSets
			? static_cast<size_t>(totals.meshletCount) * static_cast<size_t>(uvSetCount) * sizeof(CLodMeshletUvDescriptor)
			: 0u;
		const uint32_t positionBitstreamOffset = static_cast<uint32_t>(align4(pageHasUvSets ? (uvDescriptorOffset + uvDescriptorBytes) : (descriptorOffset + descriptorBytes)));
		const size_t positionBytes = static_cast<size_t>(totals.totalPositionBytes);
		const uint32_t normalArrayOffset = pageHasNormals ? static_cast<uint32_t>(align4(positionBitstreamOffset + positionBytes)) : 0u;
		const size_t normalBytes = pageHasNormals ? static_cast<size_t>(totals.totalNormalWords) * sizeof(uint32_t) : 0u;
		const uint32_t tangentFrameArrayOffset = pageHasTangentFrames
			? static_cast<uint32_t>(align4(pageHasNormals ? (normalArrayOffset + normalBytes) : (positionBitstreamOffset + positionBytes)))
			: 0u;
		const size_t tangentFrameBytes = pageHasTangentFrames ? static_cast<size_t>(totals.totalTangentFrameWords) * sizeof(uint32_t) : 0u;
		const size_t afterNormalAndTangentBytes = pageHasTangentFrames
			? (static_cast<size_t>(tangentFrameArrayOffset) + tangentFrameBytes)
			: (pageHasNormals
				? (static_cast<size_t>(normalArrayOffset) + normalBytes)
				: (static_cast<size_t>(positionBitstreamOffset) + positionBytes));
		const uint32_t colorArrayOffset = pageHasColors ? static_cast<uint32_t>(align4(afterNormalAndTangentBytes)) : 0u;
		const size_t colorBytes = pageHasColors ? static_cast<size_t>(totals.totalColorWords) * sizeof(uint32_t) : 0u;
		const size_t afterColorBytes = pageHasColors
			? (static_cast<size_t>(colorArrayOffset) + colorBytes)
			: afterNormalAndTangentBytes;
		const uint32_t jointArrayOffset = pageHasJoints ? static_cast<uint32_t>(align4(afterColorBytes)) : 0u;
		const size_t jointBytes = pageHasJoints ? static_cast<size_t>(totals.totalVertexCount) * sizeof(DirectX::XMUINT4) * 2u : 0u;
		const size_t afterJointBytes = pageHasJoints
			? (static_cast<size_t>(jointArrayOffset) + jointBytes)
			: afterColorBytes;
		const uint32_t weightArrayOffset = pageHasWeights ? static_cast<uint32_t>(align4(afterJointBytes)) : 0u;
		const size_t weightBytes = pageHasWeights ? static_cast<size_t>(totals.totalVertexCount) * sizeof(DirectX::XMFLOAT4) * 2u : 0u;
		const size_t afterWeightBytes = pageHasWeights
			? (static_cast<size_t>(weightArrayOffset) + weightBytes)
			: afterJointBytes;
		const uint32_t uvBitstreamDirectoryOffset = pageHasUvSets ? static_cast<uint32_t>(align4(afterWeightBytes)) : 0u;

		std::vector<uint32_t> uvBitstreamOffsets(uvSetCount, 0u);
		size_t uvBitstreamCursor = pageHasUvSets
			? align4(static_cast<size_t>(uvBitstreamDirectoryOffset) + static_cast<size_t>(uvSetCount) * sizeof(uint32_t))
			: align4(afterWeightBytes);
		for (uint32_t uvSetIndex = 0; uvSetIndex < uvSetCount; ++uvSetIndex)
		{
			uvBitstreamOffsets[uvSetIndex] = static_cast<uint32_t>(uvBitstreamCursor);
			const size_t uvBytes = static_cast<size_t>((totals.totalUvBitsPerSet[uvSetIndex] + 31ull) / 32ull) * sizeof(uint32_t);
			uvBitstreamCursor = align4(uvBitstreamCursor + uvBytes);
		}

		const uint32_t boneIndexStreamOffset = static_cast<uint32_t>(align4(uvBitstreamCursor));
		const size_t boneIndexBytes = static_cast<size_t>(totals.totalBoneIndexCount) * sizeof(uint32_t);
		const uint32_t triangleStreamOffset = static_cast<uint32_t>(align4(boneIndexStreamOffset + boneIndexBytes));
		const size_t totalBlobSize = align4(triangleStreamOffset + totals.totalTriangleBytes);
		if (totalBlobSize > CLOD_PAGE_SIZE)
		{
			return {};
		}

		std::vector<std::byte> blob(totalBlobSize, std::byte{ 0 });
		std::vector<CLodMeshletDescriptor> descriptors(totals.meshletCount);
		std::vector<CLodMeshletUvDescriptor> uvDescriptors(static_cast<size_t>(totals.meshletCount) * static_cast<size_t>(uvSetCount));
		std::vector<std::vector<uint32_t>> uvWordsPerSet(uvSetCount);
		std::vector<uint64_t> uvBitCursors(uvSetCount, 0ull);

		uint32_t outputMeshletIndex = 0u;
		uint32_t positionByteCursor = 0u;
		uint32_t vertexAttributeCursor = 0u;
		uint32_t boneIndexCursor = 0u;
		uint32_t triangleByteCursor = 0u;

		for (const TriangleMeshPageSegmentRef& segment : segments)
		{
			const std::vector<std::byte>& sourceBlob = state.groupPageBlobs[segment.groupIndex][segment.sourcePageIndex];
			CLodPageHeader sourceHeader{};
			if (!ReadTrianglePageHeader(sourceBlob, sourceHeader))
			{
				continue;
			}

			for (uint32_t localMeshlet = 0; localMeshlet < segment.meshletCount; ++localMeshlet)
			{
				const uint32_t sourceMeshletIndex = segment.firstMeshletInPage + localMeshlet;
				CLodMeshletDescriptor sourceDesc{};
				if (!ReadTriangleMeshletDescriptor(sourceBlob, sourceHeader, sourceMeshletIndex, sourceDesc))
				{
					continue;
				}

				const uint32_t vertexCount = DecodeMeshletVertexCount(sourceDesc);
				const uint32_t triangleCount = DecodeMeshletTriangleCount(sourceDesc);
				const uint32_t positionBytesForMeshlet = vertexCount * CLOD_NATIVE_POSITION_STRIDE_BYTES;
				const uint32_t triangleBytesForMeshlet = triangleCount * 3u;

				CLodMeshletDescriptor& destDesc = descriptors[outputMeshletIndex];
				destDesc = sourceDesc;
				destDesc.positionBitOffset = positionByteCursor;
				destDesc.vertexAttributeOffset = vertexAttributeCursor;
				destDesc.triangleByteOffset = triangleByteCursor;
				destDesc.boneListOffset = boneIndexCursor;
				destDesc.sourceGroupLocalIndex = segment.groupIndex;

				auto copyBytes = [&](uint32_t destOffset, uint32_t sourceOffset, uint32_t byteCount)
				{
					if (byteCount == 0u ||
						static_cast<size_t>(sourceOffset) + byteCount > sourceBlob.size() ||
						static_cast<size_t>(destOffset) + byteCount > blob.size())
					{
						return;
					}
					std::memcpy(blob.data() + destOffset, sourceBlob.data() + sourceOffset, byteCount);
				};

				copyBytes(
					positionBitstreamOffset + positionByteCursor,
					sourceHeader.positionBitstreamOffset + sourceDesc.positionBitOffset,
					positionBytesForMeshlet);

				if (pageHasNormals && sourceHeader.normalArrayOffset != 0u)
				{
					copyBytes(
						normalArrayOffset + vertexAttributeCursor * static_cast<uint32_t>(sizeof(uint32_t)),
						sourceHeader.normalArrayOffset + sourceDesc.vertexAttributeOffset * static_cast<uint32_t>(sizeof(uint32_t)),
						vertexCount * static_cast<uint32_t>(sizeof(uint32_t)));
				}
				if (pageHasTangentFrames && sourceHeader.tangentFrameArrayOffset != 0u)
				{
					copyBytes(
						tangentFrameArrayOffset + vertexAttributeCursor * static_cast<uint32_t>(sizeof(uint32_t)),
						sourceHeader.tangentFrameArrayOffset + sourceDesc.vertexAttributeOffset * static_cast<uint32_t>(sizeof(uint32_t)),
						vertexCount * static_cast<uint32_t>(sizeof(uint32_t)));
				}
				if (pageHasColors && sourceHeader.colorArrayOffset != 0u)
				{
					copyBytes(
						colorArrayOffset + vertexAttributeCursor * static_cast<uint32_t>(sizeof(uint32_t)),
						sourceHeader.colorArrayOffset + sourceDesc.vertexAttributeOffset * static_cast<uint32_t>(sizeof(uint32_t)),
						vertexCount * static_cast<uint32_t>(sizeof(uint32_t)));
				}
				if (pageHasJoints && sourceHeader.jointArrayOffset != 0u)
				{
					copyBytes(
						jointArrayOffset + vertexAttributeCursor * static_cast<uint32_t>(sizeof(DirectX::XMUINT4)) * 2u,
						sourceHeader.jointArrayOffset + sourceDesc.vertexAttributeOffset * static_cast<uint32_t>(sizeof(DirectX::XMUINT4)) * 2u,
						vertexCount * static_cast<uint32_t>(sizeof(DirectX::XMUINT4)) * 2u);
				}
				if (pageHasWeights && sourceHeader.weightArrayOffset != 0u)
				{
					copyBytes(
						weightArrayOffset + vertexAttributeCursor * static_cast<uint32_t>(sizeof(DirectX::XMFLOAT4)) * 2u,
						sourceHeader.weightArrayOffset + sourceDesc.vertexAttributeOffset * static_cast<uint32_t>(sizeof(DirectX::XMFLOAT4)) * 2u,
						vertexCount * static_cast<uint32_t>(sizeof(DirectX::XMFLOAT4)) * 2u);
				}
				if (sourceDesc.boneCount != 0u && sourceHeader.boneIndexStreamOffset != 0u)
				{
					copyBytes(
						boneIndexStreamOffset + boneIndexCursor * static_cast<uint32_t>(sizeof(uint32_t)),
						sourceHeader.boneIndexStreamOffset + sourceDesc.boneListOffset * static_cast<uint32_t>(sizeof(uint32_t)),
						sourceDesc.boneCount * static_cast<uint32_t>(sizeof(uint32_t)));
				}
				copyBytes(
					triangleStreamOffset + triangleByteCursor,
					sourceHeader.triangleStreamOffset + sourceDesc.triangleByteOffset,
					triangleBytesForMeshlet);

				for (uint32_t uvSetIndex = 0; uvSetIndex < uvSetCount; ++uvSetIndex)
				{
					CLodMeshletUvDescriptor destUvDesc{};
					destUvDesc.uvBitOffset = static_cast<uint32_t>(uvBitCursors[uvSetIndex]);
					if (uvSetIndex < sourceHeader.uvSetCount &&
						ReadTriangleUvDescriptor(sourceBlob, sourceHeader, sourceMeshletIndex, uvSetIndex, destUvDesc))
					{
						const uint32_t sourceUvStreamOffset = ReadUint32At(
							sourceBlob,
							static_cast<size_t>(sourceHeader.uvBitstreamDirectoryOffset) + static_cast<size_t>(uvSetIndex) * sizeof(uint32_t));
						const uint64_t sourceUvBitOffset = destUvDesc.uvBitOffset;
						destUvDesc.uvBitOffset = static_cast<uint32_t>(uvBitCursors[uvSetIndex]);
						const uint64_t bitCount =
							static_cast<uint64_t>(vertexCount) *
							static_cast<uint64_t>(DecodeUvBitsU(destUvDesc) + DecodeUvBitsV(destUvDesc));
						AppendBitsFromBytes(sourceBlob, sourceUvStreamOffset, sourceUvBitOffset, bitCount, uvWordsPerSet[uvSetIndex], uvBitCursors[uvSetIndex]);
					}
					else
					{
						destUvDesc.uvScaleU = 0.0f;
						destUvDesc.uvScaleV = 0.0f;
						destUvDesc.uvBits = 1u | (1u << 8u);
						for (uint32_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
						{
							AppendBits(uvWordsPerSet[uvSetIndex], uvBitCursors[uvSetIndex], 0u, 1u);
							AppendBits(uvWordsPerSet[uvSetIndex], uvBitCursors[uvSetIndex], 0u, 1u);
						}
					}
					uvDescriptors[static_cast<size_t>(outputMeshletIndex) * static_cast<size_t>(uvSetCount) + uvSetIndex] = destUvDesc;
				}

				positionByteCursor += positionBytesForMeshlet;
				vertexAttributeCursor += vertexCount;
				boneIndexCursor += sourceDesc.boneCount;
				triangleByteCursor += triangleBytesForMeshlet;
				outputMeshletIndex++;
			}
		}

		std::memcpy(blob.data() + descriptorOffset, descriptors.data(), descriptorBytes);
		if (pageHasUvSets)
		{
			std::memcpy(blob.data() + uvDescriptorOffset, uvDescriptors.data(), uvDescriptorBytes);
			std::memcpy(blob.data() + uvBitstreamDirectoryOffset, uvBitstreamOffsets.data(), static_cast<size_t>(uvSetCount) * sizeof(uint32_t));
			for (uint32_t uvSetIndex = 0; uvSetIndex < uvSetCount; ++uvSetIndex)
			{
				if (!uvWordsPerSet[uvSetIndex].empty())
				{
					std::memcpy(
						blob.data() + uvBitstreamOffsets[uvSetIndex],
						uvWordsPerSet[uvSetIndex].data(),
						uvWordsPerSet[uvSetIndex].size() * sizeof(uint32_t));
				}
			}
		}

		CLodPageHeader header{};
		header.meshletCount = totals.meshletCount;
		header.compressedPositionQuantExp = CLOD_NATIVE_POSITION_FORMAT;
		header.attributeMask = attributeMask;
		header.uvSetCount = uvSetCount;
		header.descriptorOffset = descriptorOffset;
		header.uvDescriptorOffset = uvDescriptorOffset;
		header.positionBitstreamOffset = positionBitstreamOffset;
		header.normalArrayOffset = normalArrayOffset;
		header.tangentFrameArrayOffset = tangentFrameArrayOffset;
		header.colorArrayOffset = colorArrayOffset;
		header.jointArrayOffset = jointArrayOffset;
		header.weightArrayOffset = weightArrayOffset;
		header.uvBitstreamDirectoryOffset = uvBitstreamDirectoryOffset;
		header.triangleStreamOffset = triangleStreamOffset;
		header.boneIndexStreamOffset = boneIndexStreamOffset;
		std::memcpy(blob.data(), &header, sizeof(CLodPageHeader));

		return blob;
	}

	void FinalizeMeshWidePagePacking(
		ClusterLODBuildState& state,
		std::vector<std::vector<std::byte>>& outMeshPageBlobs,
		std::vector<uint32_t>& outGroupPageReferences,
		std::vector<uint32_t>& outGroupPageReferenceOffsets,
		uint32_t& outTrianglePageCount,
		uint32_t& outVoxelPageBase,
		uint32_t& outVoxelPageCount)
	{
		outMeshPageBlobs.clear();
		outGroupPageReferences.clear();
		outGroupPageReferenceOffsets.clear();
		outTrianglePageCount = 0u;
		outVoxelPageBase = 0u;
		outVoxelPageCount = 0u;

		std::vector<TriangleMeshPageSegmentRef> currentTrianglePage;
		uint32_t currentAttributeMask = 0u;
		uint32_t currentUvSetCount = 0u;
		std::vector<std::vector<uint32_t>> groupReferencedPages(state.groups.size());

		auto flushTrianglePage = [&]()
		{
			if (currentTrianglePage.empty())
			{
				return;
			}

			std::vector<std::byte> pageBlob = BuildPackedTriangleMeshPageBlob(
				state,
				std::span<const TriangleMeshPageSegmentRef>(currentTrianglePage.data(), currentTrianglePage.size()),
				currentAttributeMask,
				currentUvSetCount);
			if (pageBlob.empty())
			{
				currentTrianglePage.clear();
				currentAttributeMask = 0u;
				currentUvSetCount = 0u;
				return;
			}

			const uint32_t meshPageIndex = static_cast<uint32_t>(outMeshPageBlobs.size());
			uint32_t pageLocalMeshlet = 0u;
			for (const TriangleMeshPageSegmentRef& segment : currentTrianglePage)
			{
				if (segment.segmentIndex < state.segments.size())
				{
					ClusterLODGroupSegment& outSegment = state.segments[segment.segmentIndex];
					outSegment.pageIndex = meshPageIndex;
					outSegment.firstMeshletInPage = pageLocalMeshlet;
					groupReferencedPages[segment.groupIndex].push_back(meshPageIndex);
				}
				pageLocalMeshlet += segment.meshletCount;
			}

			outMeshPageBlobs.push_back(std::move(pageBlob));
			currentTrianglePage.clear();
			currentAttributeMask = 0u;
			currentUvSetCount = 0u;
		};

		std::vector<uint32_t> groupOrder(state.groups.size());
		std::iota(groupOrder.begin(), groupOrder.end(), 0u);
		std::stable_sort(groupOrder.begin(), groupOrder.end(), [&](uint32_t a, uint32_t b)
		{
			const ClusterLODGroup& groupA = state.groups[a];
			const ClusterLODGroup& groupB = state.groups[b];
			if (groupA.depth != groupB.depth) return groupA.depth < groupB.depth;
			if (groupA.parentGroupId != groupB.parentGroupId) return groupA.parentGroupId < groupB.parentGroupId;
			return a < b;
		});

		for (uint32_t groupIndex : groupOrder)
		{
			if (groupIndex >= state.groups.size())
			{
				continue;
			}
			const ClusterLODGroup& group = state.groups[groupIndex];
			if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u)
			{
				continue;
			}

			const uint32_t segEnd = std::min<uint32_t>(
				group.firstSegment + group.segmentCount,
				static_cast<uint32_t>(state.segments.size()));
			for (uint32_t segmentIndex = group.firstSegment; segmentIndex < segEnd; ++segmentIndex)
			{
				const ClusterLODGroupSegment& segment = state.segments[segmentIndex];
				if (segment.meshletCount == 0u ||
					groupIndex >= state.groupPageBlobs.size() ||
					segment.pageIndex >= state.groupPageBlobs[groupIndex].size())
				{
					continue;
				}

				const std::vector<std::byte>& sourceBlob = state.groupPageBlobs[groupIndex][segment.pageIndex];
				CLodPageHeader sourceHeader{};
				if (!ReadTrianglePageHeader(sourceBlob, sourceHeader))
				{
					continue;
				}

				TriangleMeshPageSegmentRef candidate{};
				candidate.groupIndex = groupIndex;
				candidate.segmentIndex = segmentIndex;
				candidate.sourcePageIndex = segment.pageIndex;
				candidate.firstMeshletInPage = segment.firstMeshletInPage;
				candidate.meshletCount = segment.meshletCount;

				const uint32_t candidateMask = currentTrianglePage.empty()
					? sourceHeader.attributeMask
					: (currentAttributeMask | sourceHeader.attributeMask);
				const uint32_t candidateUvSetCount = currentTrianglePage.empty()
					? sourceHeader.uvSetCount
					: std::max(currentUvSetCount, sourceHeader.uvSetCount);

				std::vector<TriangleMeshPageSegmentRef> candidatePage = currentTrianglePage;
				candidatePage.push_back(candidate);
				const TriangleMeshPageBuildTotals candidateTotals = ComputeTriangleMeshPageTotals(
					state,
					std::span<const TriangleMeshPageSegmentRef>(candidatePage.data(), candidatePage.size()),
					candidateMask,
					candidateUvSetCount);
				const size_t candidateSize = ComputePageBlobSize(
					candidateMask,
					candidateTotals.meshletCount,
					candidateUvSetCount,
					candidateTotals.totalPositionBytes,
					candidateTotals.totalUvBitsPerSet,
					candidateTotals.totalVertexCount,
					candidateTotals.totalNormalWords,
					candidateTotals.totalTangentFrameWords,
					candidateTotals.totalColorWords,
					candidateTotals.totalBoneIndexCount,
					candidateTotals.totalTriangleBytes);

				if (candidateSize > CLOD_PAGE_SIZE && !currentTrianglePage.empty())
				{
					flushTrianglePage();
				}

				currentTrianglePage.push_back(candidate);
				currentAttributeMask = currentTrianglePage.size() == 1u
					? sourceHeader.attributeMask
					: (currentAttributeMask | sourceHeader.attributeMask);
				currentUvSetCount = currentTrianglePage.size() == 1u
					? sourceHeader.uvSetCount
					: std::max(currentUvSetCount, sourceHeader.uvSetCount);
			}
		}

		flushTrianglePage();
		outTrianglePageCount = static_cast<uint32_t>(outMeshPageBlobs.size());
		outVoxelPageBase = outTrianglePageCount;

		for (uint32_t groupIndex : groupOrder)
		{
			if (groupIndex >= state.groups.size())
			{
				continue;
			}
			ClusterLODGroup& group = state.groups[groupIndex];
			if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) == 0u ||
				groupIndex >= state.groupPageBlobs.size())
			{
				continue;
			}

			const uint32_t voxelGroupPageBase = static_cast<uint32_t>(outMeshPageBlobs.size());
			for (uint32_t sourcePageIndex = 0; sourcePageIndex < static_cast<uint32_t>(state.groupPageBlobs[groupIndex].size()); ++sourcePageIndex)
			{
				const uint32_t meshPageIndex = static_cast<uint32_t>(outMeshPageBlobs.size());
				outMeshPageBlobs.push_back(state.groupPageBlobs[groupIndex][sourcePageIndex]);
				groupReferencedPages[groupIndex].push_back(meshPageIndex);
			}

			const uint32_t segEnd = std::min<uint32_t>(
				group.firstSegment + group.segmentCount,
				static_cast<uint32_t>(state.segments.size()));
			for (uint32_t segmentIndex = group.firstSegment; segmentIndex < segEnd; ++segmentIndex)
			{
				ClusterLODGroupSegment& segment = state.segments[segmentIndex];
				segment.pageIndex += voxelGroupPageBase;
			}
		}

		outVoxelPageCount = static_cast<uint32_t>(outMeshPageBlobs.size()) - outVoxelPageBase;

		outGroupPageReferenceOffsets.reserve(state.groups.size() + 1ull);
		std::vector<std::unordered_map<uint32_t, uint32_t>> groupPageMapSlots(state.groups.size());
		uint32_t pageMapCursor = 0u;
		for (uint32_t groupIndex = 0; groupIndex < static_cast<uint32_t>(state.groups.size()); ++groupIndex)
		{
			outGroupPageReferenceOffsets.push_back(static_cast<uint32_t>(outGroupPageReferences.size()));
			std::vector<uint32_t>& refs = groupReferencedPages[groupIndex];
			std::sort(refs.begin(), refs.end());
			refs.erase(std::unique(refs.begin(), refs.end()), refs.end());

			ClusterLODGroup& group = state.groups[groupIndex];
			if (refs.empty())
			{
				group.pageMapBase = 0u;
				group.pageCount = 0u;
				continue;
			}

			group.pageMapBase = pageMapCursor;
			group.pageCount = static_cast<uint32_t>(refs.size());
			auto& slotByMeshPage = groupPageMapSlots[groupIndex];
			slotByMeshPage.reserve(refs.size());
			for (uint32_t pageOffset = 0u; pageOffset < static_cast<uint32_t>(refs.size()); ++pageOffset)
			{
				slotByMeshPage.emplace(refs[pageOffset], group.pageMapBase + pageOffset);
			}
			pageMapCursor += group.pageCount;
			outGroupPageReferences.insert(outGroupPageReferences.end(), refs.begin(), refs.end());
		}
		outGroupPageReferenceOffsets.push_back(static_cast<uint32_t>(outGroupPageReferences.size()));

		for (uint32_t groupIndex = 0; groupIndex < static_cast<uint32_t>(state.groups.size()); ++groupIndex)
		{
			const ClusterLODGroup& group = state.groups[groupIndex];
			if (group.pageCount == 0u)
			{
				continue;
			}

			const uint32_t segEnd = std::min<uint32_t>(
				group.firstSegment + group.segmentCount,
				static_cast<uint32_t>(state.segments.size()));
			const auto& slotByMeshPage = groupPageMapSlots[groupIndex];
			for (uint32_t segmentIndex = group.firstSegment; segmentIndex < segEnd; ++segmentIndex)
			{
				ClusterLODGroupSegment& segment = state.segments[segmentIndex];
				if (segment.meshletCount == 0u)
				{
					continue;
				}
				auto slotIt = slotByMeshPage.find(segment.pageIndex);
				if (slotIt != slotByMeshPage.end())
				{
					segment.pageIndex = slotIt->second;
				}
				else
				{
					spdlog::warn(
						"ClusterLOD: group {} segment {} references mesh page {} without a page-map slot",
						groupIndex,
						segmentIndex,
						segment.pageIndex);
				}
			}
		}
	}

	struct VoxelFallbackGroupAnalysis
	{
		bool valid = false;
		DirectX::XMFLOAT3 aabbMin{};
		DirectX::XMFLOAT3 aabbMax{};
		float surfaceArea = 0.0f;
		float targetVoxelWidth = 0.0f;
		uint32_t targetResolution = 0;
		uint32_t triangleCount = 0;
		uint32_t sourceVertexCount = 0;
		float voxelBudget = 0.0f;
		uint32_t sourcePrimitiveCountForCubeBudget = 0;
		uint32_t cubeBudget = 0;
	};

	struct VoxelFallbackBuildStats
	{
		uint32_t analyzedGroups = 0;
		uint32_t validGroups = 0;
		uint32_t autoCandidateGroups = 0;
		uint32_t acceptedSeedGroups = 0;
		uint32_t forcedGroups = 0;
		uint32_t propagatedGroups = 0;
		uint32_t generatedPayloads = 0;
		uint32_t generatedCubes = 0;
		uint32_t failedBuilds = 0;
		uint32_t coverageBvhBuilds = 0;
		uint32_t coverageBvhReuses = 0;
		uint64_t sourceCoverageQueries = 0;
		uint64_t sourceCoverageCandidates = 0;
		uint64_t sourceCoverageTests = 0;
		uint64_t sourceCoverageOutOfCell = 0;
		uint64_t analysisUs = 0;
		uint64_t sourceBuildUs = 0;
		uint64_t coverageBvhUs = 0;
		uint64_t voxelizeUs = 0;
		uint64_t packUs = 0;
	};

	struct VoxelFallbackGroupBuildInput
	{
		VoxelFallbackGroupAnalysis analysis{};
		std::vector<std::byte> voxelVertices;
		std::vector<std::byte> voxelSkinningVertices;
		std::vector<uint32_t> voxelTriangleIndices;
		std::vector<int32_t> voxelTriangleRefinedGroupIds;
		std::vector<uint32_t> sourceVoxelGroupIndices;
		uint32_t voxelVertexCount = 0;
		uint32_t sourcePrimitiveCountForCubeBudget = 0;
		bool autoWouldFitBudget = false;
		float autoAcceptanceErrorReference = 0.0f;
	};

	struct VoxelSourcePayloadRef
	{
		const VoxelGroupPayload* payload = nullptr;
		uint32_t budgetCellCount = 0;
		float expansionRadius = 0.0f;
	};

	struct VoxelFallbackResolutionTarget
	{
		float targetVoxelWidth = 0.0f;
		float voxelBudget = 0.0f;
	};

	VoxelFallbackResolutionTarget ComputeVoxelFallbackResolutionTarget(
		float surfaceArea,
		uint32_t sourceVertexCount,
		float scalingFactor)
	{
		const float safeScale = std::isfinite(scalingFactor) && scalingFactor > 1.0e-4f
			? scalingFactor
			: 1.0f;
		const float baseBudget = std::max(1.0f, static_cast<float>(std::max(1u, sourceVertexCount)));

		VoxelFallbackResolutionTarget target{};
		target.voxelBudget = std::max(1.0f, baseBudget / (safeScale * safeScale));
		target.targetVoxelWidth = std::sqrt(std::max(surfaceArea, 1.0e-12f) / baseBudget) * safeScale;
		return target;
	}

	uint32_t ComputeVoxelFallbackCubeBudget(uint32_t sourcePrimitiveCount)
	{
		if (sourcePrimitiveCount == 0u)
		{
			return 0u;
		}

		return std::max(1u, sourcePrimitiveCount / 2u + (sourcePrimitiveCount & 1u));
	}

	const VoxelGroupPayload* GetVoxelRenderPayloadForGroup(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		if (groupIndex < state.voxelGroupMapping.groupToPayloadIndex.size())
		{
			const int32_t payloadIndex = state.voxelGroupMapping.groupToPayloadIndex[groupIndex];
			if (payloadIndex >= 0 && static_cast<size_t>(payloadIndex) < state.voxelGroupMapping.payloads.size())
			{
				return &state.voxelGroupMapping.payloads[static_cast<size_t>(payloadIndex)];
			}
		}

		return nullptr;
	}

	float GetVoxelCandidateExpansionRadiusForPayload(const VoxelGroupPayload* payload)
	{
		if (payload == nullptr || !std::isfinite(payload->voxelWidth) || payload->voxelWidth <= 0.0f)
		{
			return 0.0f;
		}

		return 0.5f * payload->voxelWidth;
	}

	void AppendVoxelSourcePayloadRefsForGroup(
		const ClusterLODBuildState& state,
		uint32_t groupIndex,
		std::vector<VoxelSourcePayloadRef>& outPayloads)
	{
		const VoxelGroupPayload* renderPayload = GetVoxelRenderPayloadForGroup(state, groupIndex);
		const uint32_t renderCellCount = renderPayload != nullptr
			? static_cast<uint32_t>(std::min<size_t>(renderPayload->activeCells.size(), std::numeric_limits<uint32_t>::max()))
			: 0u;
		if (groupIndex < state.voxelCarryPayloads.size() &&
			state.voxelCarryPayloads[groupIndex].voxelWidth > 0.0f &&
			!state.voxelCarryPayloads[groupIndex].activeCells.empty())
		{
			const VoxelGroupPayload* carryPayload = &state.voxelCarryPayloads[groupIndex];
			outPayloads.push_back(VoxelSourcePayloadRef{ carryPayload, renderCellCount, GetVoxelCandidateExpansionRadiusForPayload(carryPayload) });
			return;
		}

		if (renderPayload != nullptr)
		{
			outPayloads.push_back(VoxelSourcePayloadRef{ renderPayload, renderCellCount, GetVoxelCandidateExpansionRadiusForPayload(renderPayload) });
		}
	}

	DirectX::XMFLOAT3 ReadGroupVertexPosition(const std::vector<std::byte>& vertices, size_t vertexStrideBytes, uint32_t vertexIndex)
	{
		DirectX::XMFLOAT3 position{};
		const size_t offset = static_cast<size_t>(vertexIndex) * vertexStrideBytes;
		std::memcpy(&position.x, vertices.data() + offset + MeshVertexLayout::PositionOffset, sizeof(float));
		std::memcpy(&position.y, vertices.data() + offset + MeshVertexLayout::PositionOffset + sizeof(float), sizeof(float));
		std::memcpy(&position.z, vertices.data() + offset + MeshVertexLayout::PositionOffset + sizeof(float) * 2, sizeof(float));
		return position;
	}

	float TriangleArea(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, const DirectX::XMFLOAT3& c)
	{
		const float abx = b.x - a.x;
		const float aby = b.y - a.y;
		const float abz = b.z - a.z;
		const float acx = c.x - a.x;
		const float acy = c.y - a.y;
		const float acz = c.z - a.z;
		const float cx = aby * acz - abz * acy;
		const float cy = abz * acx - abx * acz;
		const float cz = abx * acy - aby * acx;
		return 0.5f * std::sqrt(cx * cx + cy * cy + cz * cz);
	}

	std::vector<uint32_t> BuildGroupTriangleIndices(
		const std::vector<meshopt_Meshlet>& meshlets,
		const std::vector<uint32_t>& meshletVertices,
		const std::vector<uint8_t>& meshletTriangles,
		uint32_t firstMeshlet,
		uint32_t meshletCount)
	{
		std::vector<uint32_t> triangleIndices;
		if (firstMeshlet >= meshlets.size() || meshletCount == 0u)
		{
			return triangleIndices;
		}

		const uint32_t endMeshlet = std::min<uint32_t>(
			static_cast<uint32_t>(meshlets.size()),
			firstMeshlet + meshletCount);
		for (uint32_t meshletIndex = firstMeshlet; meshletIndex < endMeshlet; ++meshletIndex)
		{
			const meshopt_Meshlet& meshlet = meshlets[meshletIndex];
			triangleIndices.reserve(triangleIndices.size() + static_cast<size_t>(meshlet.triangle_count) * 3ull);
			for (uint32_t triangleIndex = 0; triangleIndex < meshlet.triangle_count; ++triangleIndex)
			{
				const uint32_t triBase = meshlet.triangle_offset + triangleIndex * 3u;
				if (triBase + 2u >= meshletTriangles.size())
				{
					continue;
				}

				const uint32_t localIndex0 = static_cast<uint32_t>(meshletTriangles[triBase + 0u]);
				const uint32_t localIndex1 = static_cast<uint32_t>(meshletTriangles[triBase + 1u]);
				const uint32_t localIndex2 = static_cast<uint32_t>(meshletTriangles[triBase + 2u]);
				if (localIndex0 >= meshlet.vertex_count || localIndex1 >= meshlet.vertex_count || localIndex2 >= meshlet.vertex_count)
				{
					continue;
				}

				const uint32_t vertexBase = meshlet.vertex_offset;
				if (vertexBase + localIndex0 >= meshletVertices.size() ||
					vertexBase + localIndex1 >= meshletVertices.size() ||
					vertexBase + localIndex2 >= meshletVertices.size())
				{
					continue;
				}

				triangleIndices.push_back(meshletVertices[vertexBase + localIndex0]);
				triangleIndices.push_back(meshletVertices[vertexBase + localIndex1]);
				triangleIndices.push_back(meshletVertices[vertexBase + localIndex2]);
			}
		}
		return triangleIndices;
	}

	std::vector<uint32_t> BuildGroupTriangleIndices(
		const std::vector<meshopt_Meshlet>& meshlets,
		const std::vector<uint32_t>& meshletVertices,
		const std::vector<uint8_t>& meshletTriangles)
	{
		return BuildGroupTriangleIndices(
			meshlets,
			meshletVertices,
			meshletTriangles,
			0u,
			static_cast<uint32_t>(meshlets.size()));
	}

	const std::vector<int32_t>* GetGroupMeshletRefinedGroups(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		if (groupIndex >= state.groupMeshletRefinedGroupChunks.size() ||
			groupIndex >= state.groupMeshletChunks.size())
		{
			return nullptr;
		}

		const std::vector<int32_t>& tags = state.groupMeshletRefinedGroupChunks[groupIndex];
		if (tags.size() != state.groupMeshletChunks[groupIndex].size())
		{
			return nullptr;
		}

		return &tags;
	}

	VoxelFallbackGroupAnalysis AnalyzeVoxelFallbackGroup(
		uint32_t sourceVertexCount,
		const std::vector<std::byte>& groupVertices,
		size_t vertexStrideBytes,
		const std::vector<uint32_t>& triangleIndices,
		const ClusterLODBuilderSettings& settings)
	{
		VoxelFallbackGroupAnalysis analysis{};
		analysis.triangleCount = static_cast<uint32_t>(triangleIndices.size() / 3u);
		analysis.sourceVertexCount = sourceVertexCount;
		if (analysis.triangleCount == 0u || groupVertices.empty() || vertexStrideBytes < sizeof(float) * 3u)
		{
			return analysis;
		}

		DirectX::XMFLOAT3 aabbMin(
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max());
		DirectX::XMFLOAT3 aabbMax(
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::lowest());

		float surfaceArea = 0.0f;
		for (uint32_t index : triangleIndices)
		{
			if (index >= sourceVertexCount)
			{
				continue;
			}

			const DirectX::XMFLOAT3 position = ReadGroupVertexPosition(groupVertices, vertexStrideBytes, index);
			aabbMin.x = std::min(aabbMin.x, position.x);
			aabbMin.y = std::min(aabbMin.y, position.y);
			aabbMin.z = std::min(aabbMin.z, position.z);
			aabbMax.x = std::max(aabbMax.x, position.x);
			aabbMax.y = std::max(aabbMax.y, position.y);
			aabbMax.z = std::max(aabbMax.z, position.z);
		}

		for (size_t triangleBase = 0; triangleBase + 2ull < triangleIndices.size(); triangleBase += 3ull)
		{
			const uint32_t i0 = triangleIndices[triangleBase + 0ull];
			const uint32_t i1 = triangleIndices[triangleBase + 1ull];
			const uint32_t i2 = triangleIndices[triangleBase + 2ull];
			if (i0 >= sourceVertexCount || i1 >= sourceVertexCount || i2 >= sourceVertexCount)
			{
				continue;
			}

			surfaceArea += TriangleArea(
				ReadGroupVertexPosition(groupVertices, vertexStrideBytes, i0),
				ReadGroupVertexPosition(groupVertices, vertexStrideBytes, i1),
				ReadGroupVertexPosition(groupVertices, vertexStrideBytes, i2));
		}

		const float extentX = aabbMax.x - aabbMin.x;
		const float extentY = aabbMax.y - aabbMin.y;
		const float extentZ = aabbMax.z - aabbMin.z;
		const float longestExtent = std::max({ extentX, extentY, extentZ });
		if (longestExtent <= 1.0e-8f || !std::isfinite(longestExtent))
		{
			return analysis;
		}

		const float minVoxelizationThickness = std::max(longestExtent * 1.0e-4f, 1.0e-5f);
		auto padDegenerateAxis = [minVoxelizationThickness](float& minValue, float& maxValue)
		{
			if (maxValue - minValue > minVoxelizationThickness)
			{
				return;
			}

			const float center = 0.5f * (minValue + maxValue);
			minValue = center - 0.5f * minVoxelizationThickness;
			maxValue = center + 0.5f * minVoxelizationThickness;
		};
		padDegenerateAxis(aabbMin.x, aabbMax.x);
		padDegenerateAxis(aabbMin.y, aabbMax.y);
		padDegenerateAxis(aabbMin.z, aabbMax.z);

		auto expandAxisToExtent = [](float& minValue, float& maxValue, float targetExtent)
		{
			const float currentExtent = maxValue - minValue;
			if (currentExtent >= targetExtent)
			{
				return;
			}

			const float center = 0.5f * (minValue + maxValue);
			minValue = center - 0.5f * targetExtent;
			maxValue = center + 0.5f * targetExtent;
		};

		// Runtime voxel reconstruction only stores one scalar voxel width, so the
		// offline voxel volume must be cubic to keep build-time rasterization and
		// runtime cube placement in the same space.
		expandAxisToExtent(aabbMin.x, aabbMax.x, longestExtent);
		expandAxisToExtent(aabbMin.y, aabbMax.y, longestExtent);
		expandAxisToExtent(aabbMin.z, aabbMax.z, longestExtent);

		if (surfaceArea <= 1.0e-12f || !std::isfinite(surfaceArea))
		{
			const float paddedExtentX = aabbMax.x - aabbMin.x;
			const float paddedExtentY = aabbMax.y - aabbMin.y;
			const float paddedExtentZ = aabbMax.z - aabbMin.z;
			surfaceArea = 2.0f * (paddedExtentX * paddedExtentY + paddedExtentX * paddedExtentZ + paddedExtentY * paddedExtentZ);
		}

		const VoxelFallbackResolutionTarget resolutionTarget = ComputeVoxelFallbackResolutionTarget(
			surfaceArea,
			analysis.sourceVertexCount,
			settings.voxelFallbackScalingFactor);
		const float voxelBudget = resolutionTarget.voxelBudget;
		float targetVoxelWidth = resolutionTarget.targetVoxelWidth;
		if (!std::isfinite(targetVoxelWidth) || targetVoxelWidth <= 1.0e-8f)
		{
			targetVoxelWidth = longestExtent / static_cast<float>(std::max(1u, settings.voxelGridBaseResolution));
		}

		const uint32_t minResolution = std::max(2u, settings.voxelMinResolution);
		const uint32_t targetResolution = std::max(
			minResolution,
			static_cast<uint32_t>(std::ceil(longestExtent / std::max(targetVoxelWidth, 1.0e-8f))));

		analysis.valid = targetResolution >= minResolution;
		analysis.aabbMin = aabbMin;
		analysis.aabbMax = aabbMax;
		analysis.surfaceArea = surfaceArea;
		analysis.targetVoxelWidth = targetVoxelWidth;
		analysis.targetResolution = targetResolution;
		analysis.voxelBudget = voxelBudget;
		analysis.sourcePrimitiveCountForCubeBudget = analysis.triangleCount;
		analysis.cubeBudget = ComputeVoxelFallbackCubeBudget(analysis.sourcePrimitiveCountForCubeBudget);
		return analysis;
	}

	VoxelFallbackGroupAnalysis AnalyzeVoxelFallbackBuildInput(
		const ClusterLODBuildState& state,
		const VoxelFallbackGroupBuildInput& buildInput,
		size_t vertexStrideBytes,
		const ClusterLODBuilderSettings& settings)
	{
		VoxelFallbackGroupAnalysis analysis{};
		DirectX::XMFLOAT3 aabbMin(
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max());
		DirectX::XMFLOAT3 aabbMax(
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::lowest());
		float surfaceArea = 0.0f;
		uint32_t sourceVertexCount = buildInput.voxelVertexCount;
		uint32_t triangleCount = static_cast<uint32_t>(buildInput.voxelTriangleIndices.size() / 3u);
		bool hasBounds = false;

		for (uint32_t index : buildInput.voxelTriangleIndices)
		{
			if (index >= buildInput.voxelVertexCount)
			{
				continue;
			}

			const DirectX::XMFLOAT3 position = ReadGroupVertexPosition(buildInput.voxelVertices, vertexStrideBytes, index);
			aabbMin.x = std::min(aabbMin.x, position.x);
			aabbMin.y = std::min(aabbMin.y, position.y);
			aabbMin.z = std::min(aabbMin.z, position.z);
			aabbMax.x = std::max(aabbMax.x, position.x);
			aabbMax.y = std::max(aabbMax.y, position.y);
			aabbMax.z = std::max(aabbMax.z, position.z);
			hasBounds = true;
		}

		for (size_t triangleBase = 0; triangleBase + 2ull < buildInput.voxelTriangleIndices.size(); triangleBase += 3ull)
		{
			const uint32_t i0 = buildInput.voxelTriangleIndices[triangleBase + 0ull];
			const uint32_t i1 = buildInput.voxelTriangleIndices[triangleBase + 1ull];
			const uint32_t i2 = buildInput.voxelTriangleIndices[triangleBase + 2ull];
			if (i0 >= buildInput.voxelVertexCount || i1 >= buildInput.voxelVertexCount || i2 >= buildInput.voxelVertexCount)
			{
				continue;
			}

			surfaceArea += TriangleArea(
				ReadGroupVertexPosition(buildInput.voxelVertices, vertexStrideBytes, i0),
				ReadGroupVertexPosition(buildInput.voxelVertices, vertexStrideBytes, i1),
				ReadGroupVertexPosition(buildInput.voxelVertices, vertexStrideBytes, i2));
		}

		for (uint32_t sourceVoxelGroupIndex : buildInput.sourceVoxelGroupIndices)
		{
			std::vector<VoxelSourcePayloadRef> sourcePayloadRefs;
			AppendVoxelSourcePayloadRefsForGroup(state, sourceVoxelGroupIndex, sourcePayloadRefs);
			for (const VoxelSourcePayloadRef& payloadRef : sourcePayloadRefs)
			{
				const VoxelGroupPayload* payload = payloadRef.payload;
				if (payload == nullptr)
				{
					continue;
				}

				if (payload->voxelWidth <= 0.0f)
				{
					continue;
				}

				if (payloadRef.budgetCellCount > 0u)
				{
					sourceVertexCount += std::min(payloadRef.budgetCellCount, std::numeric_limits<uint32_t>::max() - sourceVertexCount);
					triangleCount += std::min(payloadRef.budgetCellCount, std::numeric_limits<uint32_t>::max() - triangleCount);
				}
				else
				{
					triangleCount += payload->activeCells.empty() ? 0u : 1u;
				}

				const float expandedCellWidth = payload->voxelWidth + 2.0f * std::max(0.0f, payloadRef.expansionRadius);
				const float cellArea = 6.0f * expandedCellWidth * expandedCellWidth;
				for (const VoxelCell& cell : payload->activeCells)
				{
					const float x0 = payload->aabbMin.x + static_cast<float>(cell.x) * payload->voxelWidth - payloadRef.expansionRadius;
					const float y0 = payload->aabbMin.y + static_cast<float>(cell.y) * payload->voxelWidth - payloadRef.expansionRadius;
					const float z0 = payload->aabbMin.z + static_cast<float>(cell.z) * payload->voxelWidth - payloadRef.expansionRadius;
					const float x1 = x0 + expandedCellWidth;
					const float y1 = y0 + expandedCellWidth;
					const float z1 = z0 + expandedCellWidth;
					aabbMin.x = std::min(aabbMin.x, x0);
					aabbMin.y = std::min(aabbMin.y, y0);
					aabbMin.z = std::min(aabbMin.z, z0);
					aabbMax.x = std::max(aabbMax.x, x1);
					aabbMax.y = std::max(aabbMax.y, y1);
					aabbMax.z = std::max(aabbMax.z, z1);
					surfaceArea += cellArea * std::clamp(cell.opacity, 0.0f, 1.0f);
					hasBounds = true;
				}
			}
		}

		analysis.triangleCount = triangleCount;
		analysis.sourceVertexCount = sourceVertexCount;
		if (!hasBounds || triangleCount == 0u || sourceVertexCount == 0u)
		{
			return analysis;
		}

		const float extentX = aabbMax.x - aabbMin.x;
		const float extentY = aabbMax.y - aabbMin.y;
		const float extentZ = aabbMax.z - aabbMin.z;
		const float longestExtent = std::max({ extentX, extentY, extentZ });
		if (longestExtent <= 1.0e-8f || !std::isfinite(longestExtent))
		{
			return analysis;
		}

		const float minVoxelizationThickness = std::max(longestExtent * 1.0e-4f, 1.0e-5f);
		auto padDegenerateAxis = [minVoxelizationThickness](float& minValue, float& maxValue)
		{
			if (maxValue - minValue > minVoxelizationThickness)
			{
				return;
			}

			const float center = 0.5f * (minValue + maxValue);
			minValue = center - 0.5f * minVoxelizationThickness;
			maxValue = center + 0.5f * minVoxelizationThickness;
		};
		padDegenerateAxis(aabbMin.x, aabbMax.x);
		padDegenerateAxis(aabbMin.y, aabbMax.y);
		padDegenerateAxis(aabbMin.z, aabbMax.z);

		auto expandAxisToExtent = [](float& minValue, float& maxValue, float targetExtent)
		{
			const float currentExtent = maxValue - minValue;
			if (currentExtent >= targetExtent)
			{
				return;
			}

			const float center = 0.5f * (minValue + maxValue);
			minValue = center - 0.5f * targetExtent;
			maxValue = center + 0.5f * targetExtent;
		};
		expandAxisToExtent(aabbMin.x, aabbMax.x, longestExtent);
		expandAxisToExtent(aabbMin.y, aabbMax.y, longestExtent);
		expandAxisToExtent(aabbMin.z, aabbMax.z, longestExtent);

		if (surfaceArea <= 1.0e-12f || !std::isfinite(surfaceArea))
		{
			const float paddedExtentX = aabbMax.x - aabbMin.x;
			const float paddedExtentY = aabbMax.y - aabbMin.y;
			const float paddedExtentZ = aabbMax.z - aabbMin.z;
			surfaceArea = 2.0f * (paddedExtentX * paddedExtentY + paddedExtentX * paddedExtentZ + paddedExtentY * paddedExtentZ);
		}

		const VoxelFallbackResolutionTarget resolutionTarget = ComputeVoxelFallbackResolutionTarget(
			surfaceArea,
			sourceVertexCount,
			settings.voxelFallbackScalingFactor);
		const float voxelBudget = resolutionTarget.voxelBudget;
		float targetVoxelWidth = resolutionTarget.targetVoxelWidth;
		if (!std::isfinite(targetVoxelWidth) || targetVoxelWidth <= 1.0e-8f)
		{
			targetVoxelWidth = longestExtent / static_cast<float>(std::max(1u, settings.voxelGridBaseResolution));
		}

		const uint32_t minResolution = std::max(2u, settings.voxelMinResolution);
		const uint32_t targetResolution = std::max(
			minResolution,
			static_cast<uint32_t>(std::ceil(longestExtent / std::max(targetVoxelWidth, 1.0e-8f))));

		analysis.valid = targetResolution >= minResolution;
		analysis.aabbMin = aabbMin;
		analysis.aabbMax = aabbMax;
		analysis.surfaceArea = surfaceArea;
		analysis.targetVoxelWidth = targetVoxelWidth;
		analysis.targetResolution = targetResolution;
		analysis.voxelBudget = voxelBudget;
		analysis.sourcePrimitiveCountForCubeBudget = buildInput.sourcePrimitiveCountForCubeBudget != 0u
			? buildInput.sourcePrimitiveCountForCubeBudget
			: triangleCount;
		analysis.cubeBudget = ComputeVoxelFallbackCubeBudget(analysis.sourcePrimitiveCountForCubeBudget);
		return analysis;
	}

	bool AppendGroupTriangleSourceGeometry(
		const ClusterLODBuildState& state,
		uint32_t groupIndex,
		VoxelFallbackGroupBuildInput& buildInput,
		size_t vertexStrideBytes,
		int32_t refinedGroupTag = -1)
	{
		if (groupIndex >= state.groups.size() ||
			groupIndex >= state.groupVertexChunks.size() ||
			groupIndex >= state.groupMeshletChunks.size() ||
			groupIndex >= state.groupMeshletVertexChunks.size() ||
			groupIndex >= state.groupMeshletTriangleChunks.size())
		{
			return false;
		}

		const ClusterLODGroup& group = state.groups[groupIndex];
		const uint32_t vertexBase = buildInput.voxelVertexCount;
		const std::vector<std::byte>& vertices = state.groupVertexChunks[groupIndex];
		buildInput.voxelVertices.insert(buildInput.voxelVertices.end(), vertices.begin(), vertices.end());
		if (groupIndex < state.groupSkinningChunks.size())
		{
			const std::vector<std::byte>& skinning = state.groupSkinningChunks[groupIndex];
			buildInput.voxelSkinningVertices.insert(
				buildInput.voxelSkinningVertices.end(),
				skinning.begin(),
				skinning.end());
		}
		const uint32_t sourceVertexCount = vertexStrideBytes > 0u
			? static_cast<uint32_t>(std::min<size_t>(vertices.size() / vertexStrideBytes, std::numeric_limits<uint32_t>::max()))
			: group.groupVertexCount;
		buildInput.voxelVertexCount += sourceVertexCount;

		std::vector<uint32_t> triangles = BuildGroupTriangleIndices(
			state.groupMeshletChunks[groupIndex],
			state.groupMeshletVertexChunks[groupIndex],
			state.groupMeshletTriangleChunks[groupIndex]);
		buildInput.voxelTriangleIndices.reserve(buildInput.voxelTriangleIndices.size() + triangles.size());
		buildInput.voxelTriangleRefinedGroupIds.reserve(buildInput.voxelTriangleRefinedGroupIds.size() + triangles.size() / 3ull);
		for (uint32_t index : triangles)
		{
			buildInput.voxelTriangleIndices.push_back(vertexBase + index);
		}
		for (size_t triangleIndex = 0; triangleIndex < triangles.size() / 3ull; ++triangleIndex)
		{
			buildInput.voxelTriangleRefinedGroupIds.push_back(refinedGroupTag);
		}

		return true;
	}

	uint32_t ComputeGroupSegmentFirstMeshlet(const ClusterLODBuildState& state, const ClusterLODGroup& group, const ClusterLODGroupSegment& segment)
	{
		uint32_t firstMeshlet = 0u;
		for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
		{
			const ClusterLODGroupSegment& candidate = state.segments[group.firstSegment + segmentOffset];
			if (candidate.pageIndex < segment.pageIndex)
			{
				firstMeshlet += candidate.meshletCount;
			}
		}
		return firstMeshlet + segment.firstMeshletInPage;
	}

	bool AppendGroupSegmentTriangleSourceGeometry(
		const ClusterLODBuildState& state,
		uint32_t groupIndex,
		const ClusterLODGroupSegment& segment,
		VoxelFallbackGroupBuildInput& buildInput,
		size_t vertexStrideBytes,
		int32_t refinedGroupTag)
	{
		if (groupIndex >= state.groups.size() ||
			groupIndex >= state.groupVertexChunks.size() ||
			groupIndex >= state.groupMeshletChunks.size() ||
			groupIndex >= state.groupMeshletVertexChunks.size() ||
			groupIndex >= state.groupMeshletTriangleChunks.size())
		{
			return false;
		}

		const ClusterLODGroup& group = state.groups[groupIndex];
		const uint32_t vertexBase = buildInput.voxelVertexCount;
		const std::vector<std::byte>& vertices = state.groupVertexChunks[groupIndex];
		buildInput.voxelVertices.insert(buildInput.voxelVertices.end(), vertices.begin(), vertices.end());
		if (groupIndex < state.groupSkinningChunks.size())
		{
			const std::vector<std::byte>& skinning = state.groupSkinningChunks[groupIndex];
			buildInput.voxelSkinningVertices.insert(
				buildInput.voxelSkinningVertices.end(),
				skinning.begin(),
				skinning.end());
		}
		const uint32_t sourceVertexCount = vertexStrideBytes > 0u
			? static_cast<uint32_t>(std::min<size_t>(vertices.size() / vertexStrideBytes, std::numeric_limits<uint32_t>::max()))
			: group.groupVertexCount;
		buildInput.voxelVertexCount += sourceVertexCount;

		const uint32_t firstMeshlet = ComputeGroupSegmentFirstMeshlet(state, group, segment);
		std::vector<uint32_t> triangles = BuildGroupTriangleIndices(
			state.groupMeshletChunks[groupIndex],
			state.groupMeshletVertexChunks[groupIndex],
			state.groupMeshletTriangleChunks[groupIndex],
			firstMeshlet,
			segment.meshletCount);
		buildInput.voxelTriangleIndices.reserve(buildInput.voxelTriangleIndices.size() + triangles.size());
		buildInput.voxelTriangleRefinedGroupIds.reserve(buildInput.voxelTriangleRefinedGroupIds.size() + triangles.size() / 3ull);
		for (uint32_t index : triangles)
		{
			buildInput.voxelTriangleIndices.push_back(vertexBase + index);
		}
		for (size_t triangleIndex = 0; triangleIndex < triangles.size() / 3ull; ++triangleIndex)
		{
			buildInput.voxelTriangleRefinedGroupIds.push_back(refinedGroupTag);
		}

		return true;
	}

	bool AppendTerminalSegmentSourceGeometry(
		const ClusterLODBuildState& state,
		uint32_t groupIndex,
		VoxelFallbackGroupBuildInput& buildInput,
		size_t vertexStrideBytes,
		int32_t refinedGroupTag)
	{
		if (groupIndex >= state.groups.size() ||
			groupIndex >= state.groupVertexChunks.size() ||
			groupIndex >= state.groupMeshletChunks.size() ||
			groupIndex >= state.groupMeshletVertexChunks.size() ||
			groupIndex >= state.groupMeshletTriangleChunks.size())
		{
			return false;
		}

		if (const std::vector<int32_t>* meshletRefinedGroups = GetGroupMeshletRefinedGroups(state, groupIndex))
		{
			bool hasTerminalMeshlet = false;
			for (int32_t refinedGroup : *meshletRefinedGroups)
			{
				if (refinedGroup < 0)
				{
					hasTerminalMeshlet = true;
					break;
				}
			}
			if (!hasTerminalMeshlet)
			{
				return true;
			}

			const ClusterLODGroup& group = state.groups[groupIndex];
			const uint32_t vertexBase = buildInput.voxelVertexCount;
			const std::vector<std::byte>& vertices = state.groupVertexChunks[groupIndex];
			buildInput.voxelVertices.insert(buildInput.voxelVertices.end(), vertices.begin(), vertices.end());
			if (groupIndex < state.groupSkinningChunks.size())
			{
				const std::vector<std::byte>& skinning = state.groupSkinningChunks[groupIndex];
				buildInput.voxelSkinningVertices.insert(
					buildInput.voxelSkinningVertices.end(),
					skinning.begin(),
					skinning.end());
			}

			const uint32_t sourceVertexCount = vertexStrideBytes > 0u
				? static_cast<uint32_t>(std::min<size_t>(vertices.size() / vertexStrideBytes, std::numeric_limits<uint32_t>::max()))
				: group.groupVertexCount;
			buildInput.voxelVertexCount += sourceVertexCount;

			for (uint32_t meshletIndex = 0; meshletIndex < static_cast<uint32_t>(meshletRefinedGroups->size()); ++meshletIndex)
			{
				if ((*meshletRefinedGroups)[meshletIndex] >= 0)
				{
					continue;
				}

				std::vector<uint32_t> triangles = BuildGroupTriangleIndices(
					state.groupMeshletChunks[groupIndex],
					state.groupMeshletVertexChunks[groupIndex],
					state.groupMeshletTriangleChunks[groupIndex],
					meshletIndex,
					1u);
				buildInput.voxelTriangleIndices.reserve(buildInput.voxelTriangleIndices.size() + triangles.size());
				buildInput.voxelTriangleRefinedGroupIds.reserve(buildInput.voxelTriangleRefinedGroupIds.size() + triangles.size() / 3ull);
				for (uint32_t index : triangles)
				{
					buildInput.voxelTriangleIndices.push_back(vertexBase + index);
				}
				for (size_t triangleIndex = 0; triangleIndex < triangles.size() / 3ull; ++triangleIndex)
				{
					buildInput.voxelTriangleRefinedGroupIds.push_back(refinedGroupTag);
				}
			}

			return true;
		}

		const ClusterLODGroup& group = state.groups[groupIndex];
		for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
		{
			const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
			if (segment.refinedGroup >= 0)
			{
				continue;
			}

			if (!AppendGroupSegmentTriangleSourceGeometry(state, groupIndex, segment, buildInput, vertexStrideBytes, refinedGroupTag))
			{
				return false;
			}
		}

		return true;
	}

	bool AppendDescendantTriangleSourceGeometry(
		const ClusterLODBuildState& state,
		uint32_t groupIndex,
		VoxelFallbackGroupBuildInput& buildInput,
		std::unordered_set<uint32_t>& visitedGroups,
		size_t vertexStrideBytes,
		int32_t refinedGroupTag)
	{
		if (groupIndex >= state.groups.size())
		{
			return false;
		}

		if (!visitedGroups.insert(groupIndex).second)
		{
			return true;
		}

		const ClusterLODGroup& group = state.groups[groupIndex];
		std::vector<uint32_t> refinedChildren;
		refinedChildren.reserve(group.segmentCount);
		std::unordered_set<uint32_t> seenChildren;
		seenChildren.reserve(group.segmentCount);
		for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
		{
			const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
			if (segment.refinedGroup < 0)
			{
				continue;
			}

			const uint32_t childGroupIndex = static_cast<uint32_t>(segment.refinedGroup);
			if (childGroupIndex < state.groups.size() && seenChildren.insert(childGroupIndex).second)
			{
				refinedChildren.push_back(childGroupIndex);
			}
		}

		if (refinedChildren.empty())
		{
			const bool appended = AppendGroupTriangleSourceGeometry(state, groupIndex, buildInput, vertexStrideBytes, refinedGroupTag);
			visitedGroups.erase(groupIndex);
			return appended;
		}

		if (!AppendTerminalSegmentSourceGeometry(state, groupIndex, buildInput, vertexStrideBytes, refinedGroupTag))
		{
			visitedGroups.erase(groupIndex);
			return false;
		}

		for (uint32_t childGroupIndex : refinedChildren)
		{
			if (!AppendDescendantTriangleSourceGeometry(state, childGroupIndex, buildInput, visitedGroups, vertexStrideBytes, refinedGroupTag))
			{
				visitedGroups.erase(groupIndex);
				return false;
			}
		}

		visitedGroups.erase(groupIndex);
		return true;
	}

	uint32_t GetVoxelPackedCubeCountForGroup(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		if (groupIndex >= state.voxelGroupMapping.groupToPackedDescriptorIndex.size())
		{
			return 0u;
		}

		const int32_t descriptorIndex = state.voxelGroupMapping.groupToPackedDescriptorIndex[groupIndex];
		if (descriptorIndex < 0 || static_cast<size_t>(descriptorIndex) >= state.voxelGroupMapping.packedGroupDescriptors.size())
		{
			return 0u;
		}

		return state.voxelGroupMapping.packedGroupDescriptors[static_cast<size_t>(descriptorIndex)].cubeCount;
	}

	uint32_t CountGroupMeshTriangles(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		if (groupIndex >= state.groupMeshletChunks.size())
		{
			return 0u;
		}

		uint64_t triangleCount = 0u;
		for (const meshopt_Meshlet& meshlet : state.groupMeshletChunks[groupIndex])
		{
			triangleCount += meshlet.triangle_count;
		}

		return static_cast<uint32_t>(std::min<uint64_t>(triangleCount, std::numeric_limits<uint32_t>::max()));
	}

	uint32_t ComputeVoxelFallbackSourcePrimitiveCount(
		const ClusterLODBuildState& state,
		uint32_t groupIndex,
		const std::vector<uint32_t>& refinedChildren)
	{
		if (refinedChildren.empty())
		{
			return CountGroupMeshTriangles(state, groupIndex);
		}

		uint64_t sourcePrimitiveCount = 0u;
		for (uint32_t childGroupIndex : refinedChildren)
		{
			if (childGroupIndex >= state.groups.size())
			{
				continue;
			}

			const ClusterLODGroup& childGroup = state.groups[childGroupIndex];
			if ((childGroup.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u)
			{
				const uint32_t cubeCount = GetVoxelPackedCubeCountForGroup(state, childGroupIndex);
				if (cubeCount != 0u)
				{
					sourcePrimitiveCount += cubeCount;
					continue;
				}
			}

			sourcePrimitiveCount += CountGroupMeshTriangles(state, childGroupIndex);
		}

		return static_cast<uint32_t>(std::min<uint64_t>(sourcePrimitiveCount, std::numeric_limits<uint32_t>::max()));
	}

	uint32_t GetVoxelPackedClusterCountForGroup(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		if (groupIndex >= state.voxelGroupMapping.groupToPackedDescriptorIndex.size())
		{
			return 0u;
		}

		const int32_t descriptorIndex = state.voxelGroupMapping.groupToPackedDescriptorIndex[groupIndex];
		if (descriptorIndex < 0 || static_cast<size_t>(descriptorIndex) >= state.voxelGroupMapping.packedGroupDescriptors.size())
		{
			return 0u;
		}

		return state.voxelGroupMapping.packedGroupDescriptors[static_cast<size_t>(descriptorIndex)].clusterCount;
	}

	float GetVoxelDescriptorErrorForGroup(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		if (groupIndex >= state.voxelGroupMapping.groupToPackedDescriptorIndex.size())
		{
			return 0.0f;
		}

		const int32_t descriptorIndex = state.voxelGroupMapping.groupToPackedDescriptorIndex[groupIndex];
		if (descriptorIndex < 0 || static_cast<size_t>(descriptorIndex) >= state.voxelGroupMapping.packedGroupDescriptors.size())
		{
			return 0.0f;
		}

		return state.voxelGroupMapping.packedGroupDescriptors[static_cast<size_t>(descriptorIndex)].aabbMaxAndError.w;
	}

	float GetMaxSourceVoxelWidthForBuildInput(
		const ClusterLODBuildState& state,
		const VoxelFallbackGroupBuildInput& buildInput)
	{
		float maxSourceVoxelWidth = 0.0f;
		for (uint32_t sourceVoxelGroupIndex : buildInput.sourceVoxelGroupIndices)
		{
			std::vector<VoxelSourcePayloadRef> sourcePayloadRefs;
			AppendVoxelSourcePayloadRefsForGroup(state, sourceVoxelGroupIndex, sourcePayloadRefs);
			for (const VoxelSourcePayloadRef& payloadRef : sourcePayloadRefs)
			{
				const VoxelGroupPayload* payload = payloadRef.payload;
				if (payload != nullptr && std::isfinite(payload->voxelWidth) && payload->voxelWidth > 0.0f)
				{
					maxSourceVoxelWidth = std::max(maxSourceVoxelWidth, payload->voxelWidth);
				}
			}
		}
		return maxSourceVoxelWidth;
	}

	float ComputeVoxelTraversalError(float voxelWidth, float sourceRepresentationError)
	{
		if (!std::isfinite(voxelWidth) || voxelWidth <= 0.0f)
		{
			return 0.0f;
		}

		const float sourceError = std::isfinite(sourceRepresentationError) && sourceRepresentationError > 0.0f
			? sourceRepresentationError
			: 0.0f;

		// TODO: I don't think this scaling should be necessary- something is wrong with voxel traversal error
		constexpr float kVoxelTraversalErrorScalingFactor = 1.0f;
		const float voxelError = voxelWidth * kVoxelTraversalErrorScalingFactor;
		if (sourceError <= 0.0f)
		{
			return voxelError;
		}

		const float hierarchyFloor = std::nextafter(sourceError, std::numeric_limits<float>::infinity());
		return std::max(voxelError, hierarchyFloor);
	}

	float GetFiniteVoxelErrorForGroup(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		if (groupIndex >= state.groups.size())
		{
			return 0.0f;
		}

		const float representationError = state.groups[groupIndex].representationError;
		if (std::isfinite(representationError) && representationError > 0.0f)
		{
			return representationError;
		}

		const float descriptorError = GetVoxelDescriptorErrorForGroup(state, groupIndex);
		if (std::isfinite(descriptorError) && descriptorError > 0.0f)
		{
			return descriptorError;
		}

		const float groupError = state.groups[groupIndex].bounds.error;
		if (std::isfinite(groupError) && groupError > 0.0f && groupError < std::numeric_limits<float>::max() * 0.5f)
		{
			return groupError;
		}

		const VoxelGroupPayload* payload = GetVoxelRenderPayloadForGroup(state, groupIndex);
		return payload != nullptr ? payload->voxelWidth : 0.0f;
	}

	float GetMaxSourceVoxelTraversalErrorForBuildInput(
		const ClusterLODBuildState& state,
		const VoxelFallbackGroupBuildInput& buildInput)
	{
		float maxSourceError = 0.0f;
		for (uint32_t sourceVoxelGroupIndex : buildInput.sourceVoxelGroupIndices)
		{
			maxSourceError = std::max(maxSourceError, GetFiniteVoxelErrorForGroup(state, sourceVoxelGroupIndex));
		}
		return maxSourceError;
	}

	std::vector<uint32_t> CollectUniqueRefinedChildren(const ClusterLODBuildState& state, uint32_t groupIndex);

	float GetMaxRefinedChildTraversalError(const ClusterLODBuildState& state, uint32_t groupIndex, uint32_t* outCount = nullptr)
	{
		float maxRefinedChildError = 0.0f;
		uint32_t refinedChildErrorCount = 0u;
		for (uint32_t childGroupIndex : CollectUniqueRefinedChildren(state, groupIndex))
		{
			if (childGroupIndex < state.groups.size())
			{
				const float childError = state.groups[childGroupIndex].bounds.error;
				if (std::isfinite(childError) && childError > 0.0f && childError < std::numeric_limits<float>::max() * 0.5f)
				{
					maxRefinedChildError = std::max(maxRefinedChildError, childError);
					refinedChildErrorCount++;
				}
			}
		}

		if (outCount != nullptr)
		{
			*outCount = refinedChildErrorCount;
		}
		return maxRefinedChildError;
	}

	bool IsTerminalErrorSentinel(float error)
	{
		return error >= std::numeric_limits<float>::max() * 0.5f;
	}

	const char* VoxelPruningModeName(ClusterLODVoxelPruningMode mode)
	{
		switch (mode)
		{
		case ClusterLODVoxelPruningMode::None:
			return "none";
		case ClusterLODVoxelPruningMode::Coverage:
			return "coverage";
		case ClusterLODVoxelPruningMode::Spatial:
			return "spatial";
		default:
			return "unknown";
		}
	}

	void LogVoxelTriangleTagHistogram(
		const char* label,
		uint32_t groupIndex,
		int32_t depth,
		const VoxelFallbackGroupBuildInput& buildInput)
	{
		std::unordered_map<int32_t, uint32_t> tagTriangleCounts;
		for (int32_t refinedGroup : buildInput.voxelTriangleRefinedGroupIds)
		{
			tagTriangleCounts[refinedGroup]++;
		}

		std::vector<std::pair<int32_t, uint32_t>> sortedTags(tagTriangleCounts.begin(), tagTriangleCounts.end());
		std::sort(sortedTags.begin(), sortedTags.end(), [](const auto& lhs, const auto& rhs) {
			return lhs.first < rhs.first;
		});

		spdlog::debug(
			"ClusterLOD voxel source histogram: group={} depth={} source={} vertices={} triangles={} source_voxel_groups={} tag_groups={}",
			groupIndex,
			depth,
			label,
			buildInput.voxelVertexCount,
			buildInput.voxelTriangleIndices.size() / 3ull,
			buildInput.sourceVoxelGroupIndices.size(),
			sortedTags.size());

		for (const auto& [refinedGroup, triangleCount] : sortedTags)
		{
			spdlog::debug(
				"ClusterLOD voxel source tag: group={} depth={} source={} refined_group={} triangles={}",
				groupIndex,
				depth,
				label,
				refinedGroup,
				triangleCount);
		}
	}

	void LogVoxelPayloadRefinedGroupCells(
		const char* label,
		uint32_t groupIndex,
		int32_t depth,
		const VoxelGroupPayload& payload)
	{
		std::unordered_map<int32_t, uint32_t> cellCounts;
		for (const VoxelCell& cell : payload.activeCells)
		{
			cellCounts[cell.refinedGroup]++;
		}

		std::vector<std::pair<int32_t, uint32_t>> sortedCounts(cellCounts.begin(), cellCounts.end());
		std::sort(sortedCounts.begin(), sortedCounts.end(), [](const auto& lhs, const auto& rhs) {
			return lhs.first < rhs.first;
		});

		for (const auto& [refinedGroup, cellCount] : sortedCounts)
		{
			spdlog::debug(
				"ClusterLOD voxel payload cells: group={} depth={} payload={} refined_group={} cells={}",
				groupIndex,
				depth,
				label,
				refinedGroup,
				cellCount);
		}
	}

	std::vector<uint32_t> CollectUniqueRefinedChildren(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		std::vector<uint32_t> refinedChildren;
		if (groupIndex >= state.groups.size())
		{
			return refinedChildren;
		}

		if (const std::vector<int32_t>* meshletRefinedGroups = GetGroupMeshletRefinedGroups(state, groupIndex))
		{
			std::unordered_set<uint32_t> seenChildren;
			seenChildren.reserve(meshletRefinedGroups->size());
			for (int32_t refinedGroup : *meshletRefinedGroups)
			{
				if (refinedGroup < 0)
				{
					continue;
				}

				const uint32_t childGroupIndex = static_cast<uint32_t>(refinedGroup);
				if (childGroupIndex < state.groups.size() && seenChildren.insert(childGroupIndex).second)
				{
					refinedChildren.push_back(childGroupIndex);
				}
			}
			return refinedChildren;
		}

		const ClusterLODGroup& group = state.groups[groupIndex];
		refinedChildren.reserve(group.segmentCount);
		std::unordered_set<uint32_t> seenChildren;
		seenChildren.reserve(group.segmentCount);
		for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
		{
			const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
			if (segment.refinedGroup < 0)
			{
				continue;
			}

			const uint32_t childGroupIndex = static_cast<uint32_t>(segment.refinedGroup);
			if (childGroupIndex < state.groups.size() && seenChildren.insert(childGroupIndex).second)
			{
				refinedChildren.push_back(childGroupIndex);
			}
		}

		return refinedChildren;
	}

	bool IsGroupReachableFromGroup(
		const ClusterLODBuildState& state,
		uint32_t targetGroupIndex,
		uint32_t rootGroupIndex)
	{
		if (targetGroupIndex == rootGroupIndex)
		{
			return true;
		}

		if (rootGroupIndex >= state.groups.size())
		{
			return false;
		}

		std::vector<uint32_t> stack;
		std::unordered_set<uint32_t> visitedGroups;
		stack.push_back(rootGroupIndex);
		while (!stack.empty())
		{
			const uint32_t currentGroupIndex = stack.back();
			stack.pop_back();
			if (!visitedGroups.insert(currentGroupIndex).second || currentGroupIndex >= state.groups.size())
			{
				continue;
			}

			const ClusterLODGroup& group = state.groups[currentGroupIndex];
			for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
			{
				const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
				if (segment.refinedGroup < 0)
				{
					continue;
				}

				const uint32_t childGroupIndex = static_cast<uint32_t>(segment.refinedGroup);
				if (childGroupIndex == targetGroupIndex)
				{
					return true;
				}

				stack.push_back(childGroupIndex);
			}
		}

		return false;
	}

	int32_t ResolveVoxelSectionSuppressionRefinedGroup(
		const ClusterLODBuildState& state,
		uint32_t ownerGroupIndex,
		int32_t sectionRefinedGroup)
	{
		if (sectionRefinedGroup < 0 || ownerGroupIndex >= state.groups.size())
		{
			return -1;
		}

		const uint32_t sectionGroupIndex = static_cast<uint32_t>(sectionRefinedGroup);
		const ClusterLODGroup& ownerGroup = state.groups[ownerGroupIndex];
		for (uint32_t segmentOffset = 0; segmentOffset < ownerGroup.segmentCount; ++segmentOffset)
		{
			const ClusterLODGroupSegment& segment = state.segments[ownerGroup.firstSegment + segmentOffset];
			if (segment.refinedGroup < 0)
			{
				continue;
			}

			const uint32_t childGroupIndex = static_cast<uint32_t>(segment.refinedGroup);
			if (sectionGroupIndex == childGroupIndex || IsGroupReachableFromGroup(state, sectionGroupIndex, childGroupIndex))
			{
				return segment.refinedGroup;
			}
		}

		return sectionRefinedGroup;
	}

	void ClearTerminalSentinelForVoxelGroup(ClusterLODBuildState& state, uint32_t groupIndex)
	{
		if (groupIndex >= state.groups.size() || (state.groups[groupIndex].flags & CLOD_GROUP_FLAG_IS_VOXEL) == 0u)
		{
			return;
		}

		if (!IsTerminalErrorSentinel(state.groups[groupIndex].bounds.error))
		{
			return;
		}

		const float finiteError = GetFiniteVoxelErrorForGroup(state, groupIndex);
		if (std::isfinite(finiteError) && finiteError > 0.0f)
		{
			state.groups[groupIndex].bounds.error = finiteError;
		}
	}

	bool BuildVoxelFallbackSourceGeometry(
		const ClusterLODBuildState& state,
		uint32_t groupIndex,
		size_t vertexStrideBytes,
		VoxelFallbackGroupBuildInput& buildInput)
	{
		if (groupIndex >= state.groups.size() || groupIndex >= state.groupVertexChunks.size())
		{
			return false;
		}

		const std::vector<uint32_t> refinedChildren = CollectUniqueRefinedChildren(state, groupIndex);

		if (refinedChildren.empty())
		{
			buildInput.voxelVertices.clear();
			buildInput.voxelSkinningVertices.clear();
			buildInput.voxelTriangleIndices.clear();
			buildInput.voxelTriangleRefinedGroupIds.clear();
			buildInput.sourceVoxelGroupIndices.clear();
			buildInput.voxelVertexCount = 0;
			buildInput.sourcePrimitiveCountForCubeBudget = ComputeVoxelFallbackSourcePrimitiveCount(state, groupIndex, refinedChildren);
			return AppendGroupTriangleSourceGeometry(state, groupIndex, buildInput, vertexStrideBytes);
		}

		buildInput.voxelVertices.clear();
		buildInput.voxelSkinningVertices.clear();
		buildInput.voxelTriangleIndices.clear();
		buildInput.voxelTriangleRefinedGroupIds.clear();
		buildInput.sourceVoxelGroupIndices.clear();
		buildInput.voxelVertexCount = 0;
		buildInput.sourcePrimitiveCountForCubeBudget = ComputeVoxelFallbackSourcePrimitiveCount(state, groupIndex, refinedChildren);
		if (!AppendTerminalSegmentSourceGeometry(state, groupIndex, buildInput, vertexStrideBytes, -1))
		{
			return false;
		}

		// Voxel sections have to preserve the same DAG cut contract as triangle
		// sections: condition 2 suppresses a parent against each immediate refined
		// child group. Do not recursively expand these children here; shared DAG
		// descendants would be attributed to multiple sections and can render as
		// misplaced duplicates while leaving the intended child without coverage.
		for (uint32_t childGroupIndex : refinedChildren)
		{
			if (GetVoxelRenderPayloadForGroup(state, childGroupIndex) != nullptr)
			{
				buildInput.sourceVoxelGroupIndices.push_back(childGroupIndex);
				if (!AppendGroupTriangleSourceGeometry(state, childGroupIndex, buildInput, vertexStrideBytes, static_cast<int32_t>(childGroupIndex)))
				{
					return false;
				}
				continue;
			}

			if (!AppendGroupTriangleSourceGeometry(state, childGroupIndex, buildInput, vertexStrideBytes, static_cast<int32_t>(childGroupIndex)))
			{
				return false;
			}
		}

		return (!buildInput.voxelVertices.empty() && !buildInput.voxelTriangleIndices.empty()) || !buildInput.sourceVoxelGroupIndices.empty();
	}

	bool BuildVoxelFallbackCoverageSourceGeometry(
		const ClusterLODBuildState& state,
		uint32_t groupIndex,
		size_t vertexStrideBytes,
		VoxelFallbackGroupBuildInput& buildInput)
	{
		if (groupIndex >= state.groups.size() || groupIndex >= state.groupVertexChunks.size())
		{
			return false;
		}

		buildInput.voxelVertices.clear();
		buildInput.voxelSkinningVertices.clear();
		buildInput.voxelTriangleIndices.clear();
		buildInput.voxelTriangleRefinedGroupIds.clear();
		buildInput.sourceVoxelGroupIndices.clear();
		buildInput.voxelVertexCount = 0;
		buildInput.sourcePrimitiveCountForCubeBudget = 0;

		const std::vector<uint32_t> refinedChildren = CollectUniqueRefinedChildren(state, groupIndex);

		if (refinedChildren.empty())
		{
			return AppendGroupTriangleSourceGeometry(state, groupIndex, buildInput, vertexStrideBytes);
		}

		if (!AppendTerminalSegmentSourceGeometry(state, groupIndex, buildInput, vertexStrideBytes, -1))
		{
			return false;
		}

		// Coverage must be measured over the same immediate refined-child domains
		// used for candidate ownership; descendant expansion breaks DAG sharing.
		for (uint32_t childGroupIndex : refinedChildren)
		{
			if (!AppendGroupTriangleSourceGeometry(state, childGroupIndex, buildInput, vertexStrideBytes, static_cast<int32_t>(childGroupIndex)))
			{
				return false;
			}
		}

		return !buildInput.voxelVertices.empty() && !buildInput.voxelTriangleIndices.empty();
	}

	void BuildVoxelFallbackCandidates(
		ClusterLODBuildState& state,
		size_t vertexStrideBytes,
		size_t skinningVertexStrideBytes,
		const VoxelSourceTriangleBVH* coverageSourceTriangles,
		const VoxelCoverageMaterialSampler* coverageMaterialSampler,
		const ClusterLODBuilderSettings& settings)
	{
		const bool enabled = settings.enableVoxelFallback && settings.voxelFallbackMode != ClusterLODVoxelFallbackMode::MeshOnly;
		if (!enabled || state.groups.empty())
		{
			return;
		}

		state.voxelGroupMapping.groupToPayloadIndex.assign(state.groups.size(), -1);
		state.voxelGroupMapping.groupToPackedDescriptorIndex.assign(state.groups.size(), -1);
		state.voxelCarryPayloads.assign(state.groups.size(), {});

		VoxelFallbackBuildStats stats{};
		const bool forceAllVoxels = settings.voxelFallbackMode == ClusterLODVoxelFallbackMode::VoxelOnly;
		const bool autoMode = settings.voxelFallbackMode == ClusterLODVoxelFallbackMode::Auto;
		const uint32_t originalGroupCount = static_cast<uint32_t>(state.groups.size());
		std::vector<VoxelFallbackGroupBuildInput> groupInputs(originalGroupCount);
		std::vector<float> finiteParentErrorForGroup(originalGroupCount, 0.0f);
		std::vector<float> originalGroupErrors(originalGroupCount, 0.0f);
		uint32_t maxDepth = 0;

		auto finiteVoxelDecisionError = [](float error) -> bool
		{
			return std::isfinite(error) && error > 0.0f && error < std::numeric_limits<float>::max() * 0.5f;
		};
		auto elapsedUsSince = [](std::chrono::steady_clock::time_point start) -> uint64_t
		{
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - start).count());
		};

		for (uint32_t groupIndex = 0; groupIndex < originalGroupCount; ++groupIndex)
		{
			originalGroupErrors[groupIndex] = state.groups[groupIndex].bounds.error;
		}

		for (const ClusterLODGroup& parentGroup : state.groups)
		{
			if (!finiteVoxelDecisionError(parentGroup.bounds.error))
			{
				continue;
			}

			for (uint32_t segmentOffset = 0; segmentOffset < parentGroup.segmentCount; ++segmentOffset)
			{
				const ClusterLODGroupSegment& segment = state.segments[parentGroup.firstSegment + segmentOffset];
				if (segment.refinedGroup < 0)
				{
					continue;
				}

				const uint32_t childGroupIndex = static_cast<uint32_t>(segment.refinedGroup);
				if (childGroupIndex < finiteParentErrorForGroup.size())
				{
					finiteParentErrorForGroup[childGroupIndex] = std::max(finiteParentErrorForGroup[childGroupIndex], parentGroup.bounds.error);
				}
			}
		}

		for (uint32_t groupIndex = 0; groupIndex < originalGroupCount; ++groupIndex)
		{
			stats.analyzedGroups++;
			maxDepth = std::max(maxDepth, static_cast<uint32_t>(std::max(state.groups[groupIndex].depth, 0)));
			if (groupIndex >= state.groupVertexChunks.size() || groupIndex >= state.groupMeshletVertexChunks.size() ||
				groupIndex >= state.groupMeshletChunks.size() || groupIndex >= state.groupMeshletTriangleChunks.size())
			{
				stats.failedBuilds++;
				continue;
			}

			const ClusterLODGroup& group = state.groups[groupIndex];
			VoxelFallbackGroupBuildInput& buildInput = groupInputs[groupIndex];
			const auto sourceBuildStart = std::chrono::steady_clock::now();
			if (!BuildVoxelFallbackSourceGeometry(state, groupIndex, vertexStrideBytes, buildInput))
			{
				stats.sourceBuildUs += elapsedUsSince(sourceBuildStart);
				stats.failedBuilds++;
				continue;
			}
			stats.sourceBuildUs += elapsedUsSince(sourceBuildStart);

			const auto analysisStart = std::chrono::steady_clock::now();
			buildInput.analysis = AnalyzeVoxelFallbackBuildInput(state, buildInput, vertexStrideBytes, settings);
			stats.analysisUs += elapsedUsSince(analysisStart);

			if (!buildInput.analysis.valid)
			{
				stats.failedBuilds++;
				continue;
			}

			stats.validGroups++;
			const bool hasRefinedDomain = group.terminalSegmentCount < group.segmentCount;
			const bool hasFiniteTriangleReductionError = finiteVoxelDecisionError(originalGroupErrors[groupIndex]);
			buildInput.autoAcceptanceErrorReference = hasFiniteTriangleReductionError
				? originalGroupErrors[groupIndex]
				: finiteParentErrorForGroup[groupIndex];
			const float targetVoxelTraversalError = ComputeVoxelTraversalError(
				buildInput.analysis.targetVoxelWidth,
				GetMaxRefinedChildTraversalError(state, groupIndex));
			// Auto fallback compares the voxel error against the triangle
			// reduction error for the group being replaced. Finite terminal
			// groups can be voxel seeds; this lets the first voxel parent refine
			// into a smaller voxel error instead of being clamped to a large
			// terminal triangle error. FLT_MAX sentinel/source groups still do
			// not auto-voxelize because they do not represent a reduction error.
			buildInput.autoWouldFitBudget = hasFiniteTriangleReductionError &&
				finiteVoxelDecisionError(buildInput.autoAcceptanceErrorReference) &&
				targetVoxelTraversalError * std::max(1.0f, settings.voxelFallbackAcceptanceBias) < buildInput.autoAcceptanceErrorReference;
			if (buildInput.autoWouldFitBudget)
			{
				stats.autoCandidateGroups++;
			}
		}

		auto buildVoxelGroup = [&](uint32_t groupIndex, bool requireBudgetFit) -> bool
		{
			if (groupIndex >= groupInputs.size())
			{
				return false;
			}

			VoxelFallbackGroupBuildInput& buildInput = groupInputs[groupIndex];
			const auto sourceBuildStart = std::chrono::steady_clock::now();
			if (!BuildVoxelFallbackSourceGeometry(state, groupIndex, vertexStrideBytes, buildInput))
			{
				stats.sourceBuildUs += elapsedUsSince(sourceBuildStart);
				stats.failedBuilds++;
				return false;
			}
			stats.sourceBuildUs += elapsedUsSince(sourceBuildStart);

			const auto analysisStart = std::chrono::steady_clock::now();
			buildInput.analysis = AnalyzeVoxelFallbackBuildInput(state, buildInput, vertexStrideBytes, settings);
			stats.analysisUs += elapsedUsSince(analysisStart);

			if (!buildInput.analysis.valid)
			{
				stats.failedBuilds++;
				return false;
			}

			ClusterLODGroup& group = state.groups[groupIndex];
			if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u)
			{
				return true;
			}

			VoxelGroupPayload payload{};
			const uint32_t payloadIndex = static_cast<uint32_t>(state.voxelGroupMapping.payloads.size());
			const uint32_t descriptorIndex = static_cast<uint32_t>(state.voxelGroupMapping.packedGroupDescriptors.size());
			const uint32_t firstCluster = static_cast<uint32_t>(state.voxelGroupMapping.packedClusterRecords.size());
			const uint32_t firstCube = static_cast<uint32_t>(state.voxelGroupMapping.packedCubeRecords.size());
			const uint32_t firstAttribute = static_cast<uint32_t>(state.voxelGroupMapping.packedAttributeSamples.size());
			uint32_t resolution = buildInput.analysis.targetResolution;
			uint32_t refinedChildErrorCount = 0u;
			const float maxRefinedChildError = GetMaxRefinedChildTraversalError(state, groupIndex, &refinedChildErrorCount);
			const float sourceRepresentationError = std::max(
				maxRefinedChildError,
				GetMaxSourceVoxelTraversalErrorForBuildInput(state, buildInput));
			const float hierarchyVoxelErrorFloor = maxRefinedChildError > 0.0f
				? std::nextafter(maxRefinedChildError, std::numeric_limits<float>::infinity())
				: 0.0f;
			const float minSourceVoxelWidth = GetMaxSourceVoxelWidthForBuildInput(state, buildInput);
			LogVoxelTriangleTagHistogram("candidate", groupIndex, group.depth, buildInput);
			float voxelWidth = buildInput.analysis.targetVoxelWidth;
			if (minSourceVoxelWidth > 0.0f && voxelWidth <= minSourceVoxelWidth)
			{
				const float coarseningFactor = std::max(1.01f, settings.voxelFallbackGrowthFactor);
				voxelWidth = minSourceVoxelWidth * coarseningFactor;
				const float extentX = buildInput.analysis.aabbMax.x - buildInput.analysis.aabbMin.x;
				const float extentY = buildInput.analysis.aabbMax.y - buildInput.analysis.aabbMin.y;
				const float extentZ = buildInput.analysis.aabbMax.z - buildInput.analysis.aabbMin.z;
				const float longestExtent = std::max({ extentX, extentY, extentZ });
				resolution = std::max(
					std::max(2u, settings.voxelMinResolution),
					static_cast<uint32_t>(std::ceil(longestExtent / std::max(voxelWidth, 1.0e-8f))));
			}
			float voxelTraversalError = ComputeVoxelTraversalError(voxelWidth, sourceRepresentationError);
			bool payloadFitsBudget = false;
			PackedVoxelGroupBuildResult packed{};
			float packedVoxelTraversalError = 0.0f;
			auto packCurrentPayload = [&]() {
				PackVoxelGroupInput packInput{};
				packInput.payload = &payload;
				packInput.voxelError = voxelTraversalError;
				packInput.opacityThreshold = settings.voxelFallbackOpacityThreshold;
				packInput.dominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
				packInput.firstCube = firstCube;
				packInput.firstAttribute = firstAttribute;
				PackedVoxelGroupBuildResult result = PackVoxelGroupToCubes(packInput);
				result.descriptor.firstCluster = firstCluster;
				BuildVoxelClustersFromCubes(result, CLOD_VOXEL_MAX_CUBES_PER_CLUSTER);
				return result;
			};
			auto computeMaxClusterCubeCount = [](const PackedVoxelGroupBuildResult& result) {
				uint32_t maxClusterCubeCount = 0u;
				for (const CLodVoxelClusterRecord& clusterRecord : result.clusterRecords)
				{
					maxClusterCubeCount = std::max(maxClusterCubeCount, clusterRecord.cubeCount);
				}
				return maxClusterCubeCount;
			};
			VoxelFallbackGroupBuildInput coverageBuildInput;
			VoxelSourceTriangleBVH groupCoverageSourceTriangles;
			const VoxelSourceTriangleBVH* voxelCoverageSourceTriangles = nullptr;
			if (BuildVoxelFallbackCoverageSourceGeometry(state, groupIndex, vertexStrideBytes, coverageBuildInput) &&
				!coverageBuildInput.voxelVertices.empty() && !coverageBuildInput.voxelTriangleIndices.empty())
			{
				LogVoxelTriangleTagHistogram("coverage", groupIndex, group.depth, coverageBuildInput);
				const auto coverageBvhStart = std::chrono::steady_clock::now();
				groupCoverageSourceTriangles.Build(
					&coverageBuildInput.voxelVertices,
					vertexStrideBytes,
					&coverageBuildInput.voxelTriangleIndices,
					coverageBuildInput.voxelSkinningVertices.empty() ? nullptr : &coverageBuildInput.voxelSkinningVertices,
					skinningVertexStrideBytes,
					coverageBuildInput.voxelTriangleRefinedGroupIds.empty() ? nullptr : &coverageBuildInput.voxelTriangleRefinedGroupIds,
					settings.doubleSidedVoxelSourceNormals);
				stats.coverageBvhUs += elapsedUsSince(coverageBvhStart);
				stats.coverageBvhBuilds++;
				if (groupCoverageSourceTriangles.IsValid())
				{
					voxelCoverageSourceTriangles = &groupCoverageSourceTriangles;
				}
			}
			if (voxelCoverageSourceTriangles == nullptr && coverageSourceTriangles != nullptr)
			{
				voxelCoverageSourceTriangles = coverageSourceTriangles;
				stats.coverageBvhReuses++;
			}

			const uint32_t retryCount = std::max(1u, settings.voxelFallbackMaxRetryCount + 1u);
			for (uint32_t attempt = 0; attempt < retryCount; ++attempt)
			{
				if (requireBudgetFit && finiteVoxelDecisionError(buildInput.autoAcceptanceErrorReference) &&
					voxelTraversalError * std::max(1.0f, settings.voxelFallbackAcceptanceBias) >= buildInput.autoAcceptanceErrorReference)
				{
					break;
				}

				std::vector<const VoxelGroupPayload*> sourceVoxelPayloads;
				std::vector<VoxelSourceCandidatePayload> candidateVoxelPayloads;
				std::vector<VoxelGroupPayload> retaggedSourceVoxelPayloads;
				std::vector<float> retaggedSourceVoxelExpansionRadii;
				if (!buildInput.sourceVoxelGroupIndices.empty())
				{
					sourceVoxelPayloads.reserve(buildInput.sourceVoxelGroupIndices.size() * 2ull);
					candidateVoxelPayloads.reserve(buildInput.sourceVoxelGroupIndices.size() * 2ull);
					retaggedSourceVoxelPayloads.reserve(buildInput.sourceVoxelGroupIndices.size() * 2ull);
					retaggedSourceVoxelExpansionRadii.reserve(buildInput.sourceVoxelGroupIndices.size() * 2ull);
					for (uint32_t sourceVoxelGroupIndex : buildInput.sourceVoxelGroupIndices)
					{
						std::vector<VoxelSourcePayloadRef> sourcePayloadRefs;
						AppendVoxelSourcePayloadRefsForGroup(state, sourceVoxelGroupIndex, sourcePayloadRefs);
						for (const VoxelSourcePayloadRef& payloadRef : sourcePayloadRefs)
						{
							if (payloadRef.payload != nullptr)
							{
								retaggedSourceVoxelPayloads.push_back(*payloadRef.payload);
								VoxelGroupPayload& retaggedPayload = retaggedSourceVoxelPayloads.back();
								for (VoxelCell& cell : retaggedPayload.activeCells)
								{
									cell.refinedGroup = static_cast<int32_t>(sourceVoxelGroupIndex);
								}

								retaggedSourceVoxelExpansionRadii.push_back(payloadRef.expansionRadius);
							}
						}
					}

					for (uint32_t payloadIndex = 0; payloadIndex < static_cast<uint32_t>(retaggedSourceVoxelPayloads.size()); ++payloadIndex)
					{
						const VoxelGroupPayload* retaggedPayload = &retaggedSourceVoxelPayloads[payloadIndex];
						sourceVoxelPayloads.push_back(retaggedPayload);
						candidateVoxelPayloads.push_back(VoxelSourceCandidatePayload{ retaggedPayload, retaggedSourceVoxelExpansionRadii[payloadIndex] });
					}
				}

				VoxelizeTrianglesInput voxelInput{};
				voxelInput.vertices = buildInput.voxelVertices.empty() ? nullptr : &buildInput.voxelVertices;
				voxelInput.vertexStrideBytes = vertexStrideBytes;
				voxelInput.skinningVertices = buildInput.voxelSkinningVertices.empty() ? nullptr : &buildInput.voxelSkinningVertices;
				voxelInput.skinningVertexStrideBytes = skinningVertexStrideBytes;
				voxelInput.triangleIndices = buildInput.voxelTriangleIndices.empty() ? nullptr : &buildInput.voxelTriangleIndices;
				voxelInput.triangleRefinedGroupIds = buildInput.voxelTriangleRefinedGroupIds.empty() ? nullptr : &buildInput.voxelTriangleRefinedGroupIds;
				voxelInput.doubleSidedTriangles = settings.doubleSidedVoxelSourceNormals;
				voxelInput.coverageSourceTriangles = voxelCoverageSourceTriangles;
				voxelInput.coverageMaterialSampler = coverageMaterialSampler;
				voxelInput.sourceVoxelPayloads = sourceVoxelPayloads.empty() ? nullptr : &sourceVoxelPayloads;
				voxelInput.candidateVoxelPayloads = candidateVoxelPayloads.empty() ? nullptr : &candidateVoxelPayloads;
				voxelInput.keepZeroCoverageSourceCells = settings.voxelFallbackCarryZeroCoverage;
				voxelInput.aabbMin = buildInput.analysis.aabbMin;
				voxelInput.aabbMax = buildInput.analysis.aabbMax;
				voxelInput.voxelWidth = voxelWidth;
				voxelInput.resolution = resolution;
				voxelInput.raysPerCell = settings.voxelRaysPerCell;
				voxelInput.pruningMode = settings.voxelFallbackPruningMode;
				const auto voxelizeStart = std::chrono::steady_clock::now();
				VoxelizeTrianglesResult voxelResult = VoxelizeTrianglesDetailed(voxelInput);
				stats.voxelizeUs += elapsedUsSince(voxelizeStart);
				stats.sourceCoverageQueries += voxelResult.sourceCoverageQueryCount;
				stats.sourceCoverageCandidates += voxelResult.sourceCoverageTriangleCandidateCount;
				stats.sourceCoverageTests += voxelResult.sourceCoverageTriangleTestCount;
				stats.sourceCoverageOutOfCell += voxelResult.sourceCoverageOutOfCellRejectionCount;
				spdlog::debug(
					"ClusterLOD voxel build detail: group={} depth={} attempt={} resolution={} voxel_width={} traversal_error={} source_representation_error={} min_source_voxel_width={} hierarchy_error_floor={} pruning={} source_tris={} source_voxel_groups={} source_primitives={} cube_budget={} tri_candidates={} voxel_candidates={} candidates={} positive_cells={} total_coverage={} max_coverage={} source_cells={} render_cells={} pruned={} source_coverage_queries={} source_coverage_candidates={} source_coverage_tests={} source_coverage_out_of_cell={}",
					groupIndex,
					group.depth,
					attempt,
					resolution,
					voxelWidth,
					voxelTraversalError,
					sourceRepresentationError,
					minSourceVoxelWidth,
					hierarchyVoxelErrorFloor,
					VoxelPruningModeName(settings.voxelFallbackPruningMode),
					buildInput.voxelTriangleIndices.size() / 3ull,
					buildInput.sourceVoxelGroupIndices.size(),
					buildInput.analysis.sourcePrimitiveCountForCubeBudget,
					buildInput.analysis.cubeBudget,
					voxelResult.triangleCandidateCellCount,
					voxelResult.voxelCandidateCellCount,
					voxelResult.candidateCellCount,
					voxelResult.positiveCoverageCellCount,
					voxelResult.totalCoverage,
					voxelResult.maxCoverage,
					voxelResult.sourcePayload.activeCells.size(),
					voxelResult.renderPayload.activeCells.size(),
					voxelResult.prunedCellCount,
					voxelResult.sourceCoverageQueryCount,
					voxelResult.sourceCoverageTriangleCandidateCount,
					voxelResult.sourceCoverageTriangleTestCount,
					voxelResult.sourceCoverageOutOfCellRejectionCount);
				for (const VoxelizeTrianglesResult::RefinedGroupStats& stats : voxelResult.refinedGroupStats)
				{
					spdlog::debug(
						"ClusterLOD voxel refined detail: group={} depth={} attempt={} refined_group={} candidates={} tri_owned={} voxel_owned={} candidate_owned={} candidate_only={} positive={} zero_dropped={} emitted={} total_coverage={} max_coverage={}",
						groupIndex,
						group.depth,
						attempt,
						stats.refinedGroup,
						stats.candidateKeys,
						stats.triangleOwnedCells,
						stats.voxelOwnedCells,
						stats.candidateOwnedCells,
						stats.candidateOnlyCells,
						stats.positiveCoverageCells,
						stats.zeroCoverageDroppedCells,
						stats.emittedSourceCells,
						stats.totalCoverage,
						stats.maxCoverage);
				}
				payload = std::move(voxelResult.renderPayload);
				state.voxelCarryPayloads[groupIndex] = std::move(voxelResult.sourcePayload);
				LogVoxelPayloadRefinedGroupCells("render", groupIndex, group.depth, payload);
				LogVoxelPayloadRefinedGroupCells("carry", groupIndex, group.depth, state.voxelCarryPayloads[groupIndex]);

				if (!payload.activeCells.empty())
				{
					const auto packStart = std::chrono::steady_clock::now();
					packed = packCurrentPayload();
					stats.packUs += elapsedUsSince(packStart);
					packedVoxelTraversalError = voxelTraversalError;
				}

				const bool cellCountFits = !payload.activeCells.empty() &&
					static_cast<float>(payload.activeCells.size()) <= buildInput.analysis.voxelBudget;
				const bool cubeCountFits = !packed.cubeRecords.empty() &&
					(buildInput.analysis.cubeBudget == 0u ||
						static_cast<uint64_t>(packed.cubeRecords.size()) <= static_cast<uint64_t>(buildInput.analysis.cubeBudget));
				payloadFitsBudget = cellCountFits && cubeCountFits;
				spdlog::debug(
					"ClusterLOD voxel pack attempt: group={} depth={} attempt={} payload_cells={} voxel_budget={} source_primitives={} cube_budget={} packed_cubes={} packed_clusters={} max_cluster_cube_count={} cells_fit={} cubes_fit={}",
					groupIndex,
					group.depth,
					attempt,
					payload.activeCells.size(),
					buildInput.analysis.voxelBudget,
					buildInput.analysis.sourcePrimitiveCountForCubeBudget,
					buildInput.analysis.cubeBudget,
					packed.cubeRecords.size(),
					packed.clusterRecords.size(),
					computeMaxClusterCubeCount(packed),
					cellCountFits,
					cubeCountFits);
				if (payloadFitsBudget)
				{
					break;
				}

				voxelWidth *= std::max(1.01f, settings.voxelFallbackGrowthFactor);
				voxelTraversalError = ComputeVoxelTraversalError(voxelWidth, sourceRepresentationError);
				const float extentX = buildInput.analysis.aabbMax.x - buildInput.analysis.aabbMin.x;
				const float extentY = buildInput.analysis.aabbMax.y - buildInput.analysis.aabbMin.y;
				const float extentZ = buildInput.analysis.aabbMax.z - buildInput.analysis.aabbMin.z;
				const float longestExtent = std::max({ extentX, extentY, extentZ });
				resolution = std::max(
					std::max(2u, settings.voxelMinResolution),
					static_cast<uint32_t>(std::ceil(longestExtent / std::max(voxelWidth, 1.0e-8f))));
			}

			if (payload.activeCells.empty() || packed.cubeRecords.empty() || packed.clusterRecords.empty() || (requireBudgetFit && !payloadFitsBudget))
			{
				stats.failedBuilds++;
				return false;
			}
			const uint32_t maxClusterCubeCount = computeMaxClusterCubeCount(packed);
			spdlog::debug(
				"ClusterLOD voxel pack detail: group={} depth={} payload_cells={} voxel_budget={} source_primitives={} cube_budget={} packed_cubes={} packed_clusters={} max_cluster_cube_count={} packed_attributes={} payload_voxel_width={} traversal_voxel_error={} opacity_threshold={}",
				groupIndex,
				group.depth,
				payload.activeCells.size(),
				buildInput.analysis.voxelBudget,
				buildInput.analysis.sourcePrimitiveCountForCubeBudget,
				buildInput.analysis.cubeBudget,
				packed.cubeRecords.size(),
				packed.clusterRecords.size(),
				maxClusterCubeCount,
				packed.attributeSamples.size(),
				payload.voxelWidth,
				packedVoxelTraversalError,
				settings.voxelFallbackOpacityThreshold);
			if (packed.cubeRecords.empty() || packed.clusterRecords.empty())
			{
				stats.failedBuilds++;
				return false;
			}
			uint32_t packedOccupiedCells = 0u;
			for (const CLodVoxelCubeRecord& cubeRecord : packed.cubeRecords)
			{
				packedOccupiedCells += static_cast<uint32_t>(std::popcount(cubeRecord.occupancyMask));
			}
			if (packedOccupiedCells != payload.activeCells.size())
			{
				spdlog::warn(
					"ClusterLOD voxel pack occupancy mismatch: group={} payload_cells={} packed_occupied_cells={} packed_cubes={}",
					groupIndex,
					payload.activeCells.size(),
					packedOccupiedCells,
					packed.cubeRecords.size());
			}

			state.voxelGroupMapping.groupToPayloadIndex[groupIndex] = static_cast<int32_t>(payloadIndex);
			state.voxelGroupMapping.groupToPackedDescriptorIndex[groupIndex] = static_cast<int32_t>(descriptorIndex);
			std::vector<ClusterLODGroupSegment> voxelSegments;
			std::vector<BoundingSphere> voxelSegmentBounds;
			SplitVoxelClustersIntoPageSegments(packed, voxelSegments, voxelSegmentBounds);
			state.voxelGroupMapping.payloads.push_back(std::move(payload));
			state.voxelGroupMapping.packedGroupDescriptors.push_back(packed.descriptor);
			std::vector<std::vector<std::byte>> voxelPageBlobs = BuildVoxelGroupPageBlobs(
				packed.descriptor,
				voxelSegments,
				packed.clusterRecords,
				packed.cubeRecords,
				packed.attributeSamples,
				firstAttribute);
			state.voxelGroupMapping.packedClusterRecords.insert(
				state.voxelGroupMapping.packedClusterRecords.end(),
				packed.clusterRecords.begin(),
				packed.clusterRecords.end());
			state.voxelGroupMapping.packedCubeRecords.insert(
				state.voxelGroupMapping.packedCubeRecords.end(),
				packed.cubeRecords.begin(),
				packed.cubeRecords.end());
			state.voxelGroupMapping.packedAttributeSamples.insert(
				state.voxelGroupMapping.packedAttributeSamples.end(),
				packed.attributeSamples.begin(),
				packed.attributeSamples.end());

			const float triangleError = group.bounds.error;
			const bool terminalErrorSentinel = triangleError >= std::numeric_limits<float>::max() * 0.5f;
			const bool replaceGroupWithVoxels = forceAllVoxels || group.terminalSegmentCount == 0u;
			group.representationError = packedVoxelTraversalError;
			if (replaceGroupWithVoxels)
			{
				group.flags |= CLOD_GROUP_FLAG_IS_VOXEL;
				group.meshletCount = 0u;
				group.groupVertexCount = 0u;
				group.firstSegment = static_cast<uint32_t>(state.segments.size());
				group.segmentCount = static_cast<uint32_t>(voxelSegments.size());
				group.terminalSegmentCount = 0u;
				for (const ClusterLODGroupSegment& segment : voxelSegments)
				{
					if (segment.refinedGroup < 0)
					{
						group.terminalSegmentCount++;
					}
					else
					{
						break;
					}
				}
				group.pageCount = static_cast<uint32_t>(voxelPageBlobs.size());
				state.segments.insert(state.segments.end(), voxelSegments.begin(), voxelSegments.end());
				state.segmentBounds.insert(state.segmentBounds.end(), voxelSegmentBounds.begin(), voxelSegmentBounds.end());
				if (groupIndex < state.groupPageBlobs.size())
				{
					state.groupPageBlobs[groupIndex] = std::move(voxelPageBlobs);
				}
				if (groupIndex < state.groupChunks.size())
				{
					ClusterLODGroupChunk& chunk = state.groupChunks[groupIndex];
					chunk.groupVertexCount = 0u;
					chunk.meshletCount = 0u;
					chunk.meshletTrianglesByteCount = 0u;
				}
			}
			spdlog::debug(
				"ClusterLOD voxel group error: group={} depth={} triangle_error={} voxel_width={} traversal_error={} terminal_sentinel={} terminal_segments={}/{} refined_child_errors={} max_refined_child_error={} hierarchy_error_floor={} source_primitives={} cube_budget={} packed_cubes={} forced_budget_fit={} replaces_group={}",
				groupIndex,
				group.depth,
				triangleError,
				payload.voxelWidth,
				packedVoxelTraversalError,
				terminalErrorSentinel,
				group.terminalSegmentCount,
				group.segmentCount,
				refinedChildErrorCount,
				maxRefinedChildError,
				hierarchyVoxelErrorFloor,
				buildInput.analysis.sourcePrimitiveCountForCubeBudget,
				buildInput.analysis.cubeBudget,
				packed.cubeRecords.size(),
				requireBudgetFit,
				replaceGroupWithVoxels);
			stats.generatedPayloads++;
			stats.generatedCubes += static_cast<uint32_t>(packed.cubeRecords.size());
			return true;
		};

		for (uint32_t depth = 0; depth <= maxDepth; ++depth)
		{
			for (uint32_t groupIndex = 0; groupIndex < originalGroupCount; ++groupIndex)
			{
				ClusterLODGroup& group = state.groups[groupIndex];
				if (static_cast<uint32_t>(std::max(group.depth, 0)) != depth || (group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u)
				{
					continue;
				}

				const bool acceptSeed = autoMode && groupInputs[groupIndex].autoWouldFitBudget;
				if (forceAllVoxels)
				{
					if (buildVoxelGroup(groupIndex, false))
					{
						stats.forcedGroups++;
					}
				}
				else if (acceptSeed)
				{
					if (buildVoxelGroup(groupIndex, true))
					{
						stats.acceptedSeedGroups++;
					}
				}
			}
		}

		bool propagatedAny = forceAllVoxels;
		while (propagatedAny)
		{
			propagatedAny = false;
			for (uint32_t groupIndex = 0; groupIndex < originalGroupCount; ++groupIndex)
			{
				ClusterLODGroup& group = state.groups[groupIndex];
				if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u)
				{
					continue;
				}

				bool hasVoxelRefinedChild = false;
				for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
				{
					const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
					if (segment.refinedGroup < 0)
					{
						continue;
					}

					const uint32_t childGroupIndex = static_cast<uint32_t>(segment.refinedGroup);
					if (childGroupIndex < state.groups.size() && (state.groups[childGroupIndex].flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u)
					{
						hasVoxelRefinedChild = true;
						break;
					}
				}

				if (!hasVoxelRefinedChild)
				{
					continue;
				}

				if (buildVoxelGroup(groupIndex, false))
				{
					stats.propagatedGroups++;
					propagatedAny = true;
				}
			}
		}

		for (uint32_t groupIndex = 0; groupIndex < static_cast<uint32_t>(state.groups.size()); ++groupIndex)
		{
			const ClusterLODGroup& group = state.groups[groupIndex];
			if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) == 0u)
			{
				continue;
			}

			float minChildError = std::numeric_limits<float>::max();
			float maxChildError = 0.0f;
			uint32_t refinedChildCount = 0;
			uint32_t voxelChildCount = 0;
			uint32_t triangleChildCount = 0;
			bool monotonicWithChildren = true;

			for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
			{
				const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
				if (segment.refinedGroup < 0)
				{
					continue;
				}

				const uint32_t childGroupIndex = static_cast<uint32_t>(segment.refinedGroup);
				if (childGroupIndex >= state.groups.size())
				{
					continue;
				}

				const ClusterLODGroup& childGroup = state.groups[childGroupIndex];
				const float childError = childGroup.bounds.error;
				minChildError = std::min(minChildError, childError);
				maxChildError = std::max(maxChildError, childError);
				refinedChildCount++;
				if ((childGroup.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u)
				{
					voxelChildCount++;
				}
				else
				{
					triangleChildCount++;
				}

				if (!(group.bounds.error > childError))
				{
					monotonicWithChildren = false;
				}
			}

			if (refinedChildCount == 0)
			{
				minChildError = -1.0f;
			}

			spdlog::debug(
				"ClusterLOD voxel hierarchy: group={} depth={} traversal_error={} refined_children={} voxel_children={} triangle_children={} min_child_error={} max_child_error={} monotonic_with_children={}",
				groupIndex,
				group.depth,
				group.bounds.error,
				refinedChildCount,
				voxelChildCount,
				triangleChildCount,
				minChildError,
				maxChildError,
				monotonicWithChildren);
		}

		uint32_t voxelGroups = 0;
		for (const ClusterLODGroup& group : state.groups)
		{
			if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u)
			{
				voxelGroups++;
			}
		}
		const uint32_t triangleGroups = static_cast<uint32_t>(state.groups.size()) - voxelGroups;
		const uint32_t totalVoxelPayloads = static_cast<uint32_t>(state.voxelGroupMapping.payloads.size());
		const uint32_t totalVoxelClusters = static_cast<uint32_t>(state.voxelGroupMapping.packedClusterRecords.size());
		const uint32_t totalVoxelCubes = static_cast<uint32_t>(state.voxelGroupMapping.packedCubeRecords.size());

		spdlog::info(
			"ClusterLOD voxel fallback: analyzed={} valid={} auto_candidates={} accepted_seeds={} forced={} propagated={} voxel_groups={} triangle_groups={} payloads={} clusters={} cubes={} failed={} coverage_bvh_builds={} coverage_bvh_reuses={} source_coverage(queries={} candidates={} tests={} out_of_cell={}) timing_ms(analysis={:.2f} source={:.2f} coverage_bvh={:.2f} voxelize={:.2f} pack={:.2f})",
			stats.analyzedGroups,
			stats.validGroups,
			stats.autoCandidateGroups,
			stats.acceptedSeedGroups,
			stats.forcedGroups,
			stats.propagatedGroups,
			voxelGroups,
			triangleGroups,
			totalVoxelPayloads,
			totalVoxelClusters,
			totalVoxelCubes,
			stats.failedBuilds,
			stats.coverageBvhBuilds,
			stats.coverageBvhReuses,
			stats.sourceCoverageQueries,
			stats.sourceCoverageCandidates,
			stats.sourceCoverageTests,
			stats.sourceCoverageOutOfCell,
			static_cast<double>(stats.analysisUs) / 1000.0,
			static_cast<double>(stats.sourceBuildUs) / 1000.0,
			static_cast<double>(stats.coverageBvhUs) / 1000.0,
			static_cast<double>(stats.voxelizeUs) / 1000.0,
			static_cast<double>(stats.packUs) / 1000.0);
	}

	void BuildClusterLODTraversalHierarchy(ClusterLODBuildState& state, uint32_t preferredNodeWidth)
	{
		if (state.groups.empty())
			return;

		preferredNodeWidth = std::max(2u, preferredNodeWidth);

		state.maxDepth = 0;
		for (const ClusterLODGroup& g : state.groups)
			state.maxDepth = std::max(state.maxDepth, uint32_t(g.depth));

		const uint32_t lodLevelCount = state.maxDepth + 1;

		// Collect traversal leaves by depth. Voxelized groups replace their triangle
		// representation, so they expose only a voxel leaf.
		struct TraversalLeafInfo { uint32_t nodeKind; uint32_t indexOrOffset; uint32_t ownerGroupId; int32_t refinedGroup; };
		std::vector<std::vector<TraversalLeafInfo>> leavesByDepth(lodLevelCount);
		for (uint32_t groupID = 0; groupID < uint32_t(state.groups.size()); ++groupID)
		{
			const ClusterLODGroup& grp = state.groups[groupID];
			const uint32_t d = uint32_t(grp.depth);
			for (uint32_t s = 0; s < grp.segmentCount; ++s)
			{
				leavesByDepth[d].push_back({ 2u, grp.firstSegment + s, groupID, state.segments[grp.firstSegment + s].refinedGroup });
			}
		}

		for (uint32_t d = 0; d < lodLevelCount; ++d)
		{
			if (leavesByDepth[d].empty())
			{
				throw std::runtime_error("Cluster LOD: missing traversal leaves for an intermediate depth; compact depths or handle gaps.");
			}
		}

		// Build parent error map: for each group, store the max traversal error
		// of any parent (coarser) group that refines into it. Also track the
		// parent group ID associated with that max error.
		std::vector<float> parentErrorForGroup(state.groups.size(), 0.0f);
		std::vector<int32_t> parentGroupIdForGroup(state.groups.size(), -1);
		for (uint32_t groupID = 0; groupID < uint32_t(state.groups.size()); ++groupID)
		{
			const ClusterLODGroup& grp = state.groups[groupID];
			const float parentError = grp.bounds.error;
			for (uint32_t s = 0; s < grp.segmentCount; ++s)
			{
				const ClusterLODGroupSegment& seg = state.segments[grp.firstSegment + s];
				if (seg.refinedGroup >= 0)
				{
					const uint32_t childGroupId = static_cast<uint32_t>(seg.refinedGroup);
					if (parentError >= parentErrorForGroup[childGroupId])
					{
						parentErrorForGroup[childGroupId] = parentError;
						parentGroupIdForGroup[childGroupId] = static_cast<int32_t>(groupID);
					}
				}
			}
		}
		// Root groups (no parent) get FLT_MAX so they are always traversed.
		// Assign parentGroupId to each group.
		for (uint32_t i = 0; i < uint32_t(state.groups.size()); ++i)
		{
			if (parentGroupIdForGroup[i] < 0)
			{
				parentErrorForGroup[i] = std::numeric_limits<float>::max();
			}
			state.groups[i].parentGroupId = parentGroupIdForGroup[i];
			state.groups[i].maxParentError = parentErrorForGroup[i];
		}

		state.lodNodeRanges.assign(lodLevelCount, {});
		state.lodLevelRoots.resize(lodLevelCount);
		for (uint32_t d = 0; d < lodLevelCount; ++d) {
			state.lodLevelRoots[d] = 1 + d;
		}

		uint32_t nodeOffset = 1 + lodLevelCount;

		for (uint32_t depth = 0; depth < lodLevelCount; ++depth)
		{
			const uint32_t leafCount = uint32_t(leavesByDepth[depth].size());

			uint32_t nodeCount = leafCount;
			uint32_t iterCount = leafCount;

			while (iterCount > 1)
			{
				iterCount = (iterCount + preferredNodeWidth - 1) / preferredNodeWidth;
				nodeCount += iterCount;
			}

			nodeCount--;

			state.lodNodeRanges[depth].offset = nodeOffset;
			state.lodNodeRanges[depth].count = nodeCount;
			nodeOffset += nodeCount;
		}

		state.nodes.clear();
		state.nodes.resize(nodeOffset);

		for (uint32_t depth = 0; depth < lodLevelCount; ++depth)
		{
			const auto& leaves = leavesByDepth[depth];
			const uint32_t leafCount = uint32_t(leaves.size());
			const ClusterLODNodeRangeAlloc& range = state.lodNodeRanges[depth];

			uint32_t writeOffset = range.offset;
			uint32_t lastLayerOffset = writeOffset;

			for (uint32_t i = 0; i < leafCount; ++i)
			{
				const TraversalLeafInfo& info = leaves[i];
				const ClusterLODGroup& grp = state.groups[info.ownerGroupId];

				ClusterLODNode& node = (leafCount == 1) ? state.nodes[1 + depth] : state.nodes[writeOffset++];

				node = {};
				node.range.isGroup = info.nodeKind;
				node.range.indexOrOffset = info.indexOrOffset;
				node.range.countMinusOne = (info.refinedGroup >= 0)
					? static_cast<uint32_t>(info.refinedGroup + 1)
					: 0u;
				node.range.ownerGroupId = info.ownerGroupId;

				if (info.nodeKind == 2u)
				{
					const BoundingSphere& segBounds = state.segmentBounds[info.indexOrOffset];
					// Expand the BVH leaf bounding sphere to enclose the owning
					// group's bounding sphere for conservative frustum culling.
					// TraverseNodes uses the actual group sphere for LOD checks.
					const float sx = segBounds.sphere.x, sy = segBounds.sphere.y, sz = segBounds.sphere.z;
					const float sr = segBounds.sphere.w;
					const float gx = grp.bounds.center[0], gy = grp.bounds.center[1], gz = grp.bounds.center[2];
					const float gr = grp.bounds.radius;

					const float dx = gx - sx, dy = gy - sy, dz = gz - sz;
					const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

					float cx, cy, cz, cr;
					if (dist + gr <= sr) {
						cx = sx; cy = sy; cz = sz; cr = sr;
					}
					else if (dist + sr <= gr) {
						cx = gx; cy = gy; cz = gz; cr = gr;
					}
					else {
						cr = (dist + sr + gr) * 0.5f;
						const float t = (cr - sr) / std::max(dist, 1e-12f);
						cx = sx + dx * t;
						cy = sy + dy * t;
						cz = sz + dz * t;
					}
					// Pad for floating-point rounding in the minimal-enclosing
					// sphere formula to guarantee strict enclosure.
					cr *= (1.0f + 1e-5f);

					node.traversalMetric.cullingSphere = DirectX::XMFLOAT4(cx, cy, cz, cr);
				}
				else
				{
					node.traversalMetric.cullingSphere = DirectX::XMFLOAT4(
						grp.bounds.center[0],
						grp.bounds.center[1],
						grp.bounds.center[2],
						grp.bounds.radius * (1.0f + 1e-5f));
				}
				node.traversalMetric.lodBoundingSphere = DirectX::XMFLOAT4(
					grp.bounds.center[0],
					grp.bounds.center[1],
					grp.bounds.center[2],
					grp.bounds.radius);
				// Meshoptimizer condition 1 is evaluated against the simplified
				// error of the group that owns this segment. Internal traversal
				// nodes propagate the max of their children below.
				node.traversalMetric.maxQuadricError = grp.bounds.error;
			}

			if (leafCount == 1) {
				writeOffset++;
			}

			uint32_t iterCount = leafCount;

			std::vector<uint32_t> partitioned;
			std::vector<ClusterLODNode> scratch;

			while (iterCount > 1)
			{
				const uint32_t lastCount = iterCount;
				ClusterLODNode* lastNodes = &state.nodes[lastLayerOffset];

				partitioned.resize(lastCount);
				meshopt_spatialClusterPoints(
					partitioned.data(),
					&lastNodes->traversalMetric.cullingSphere.x,
					lastCount,
					sizeof(ClusterLODNode),
					preferredNodeWidth);

				scratch.assign(lastNodes, lastNodes + lastCount);
				for (uint32_t n = 0; n < lastCount; ++n)
					lastNodes[n] = scratch[partitioned[n]];

				iterCount = (lastCount + preferredNodeWidth - 1) / preferredNodeWidth;

				ClusterLODNode* newNodes = (iterCount == 1) ? &state.nodes[1 + depth] : &state.nodes[writeOffset];

				for (uint32_t n = 0; n < iterCount; ++n)
				{
					ClusterLODNode& node = newNodes[n];

					const uint32_t childBegin = n * preferredNodeWidth;
					const uint32_t childEnd = std::min(childBegin + preferredNodeWidth, lastCount);
					const uint32_t childCount = childEnd - childBegin;

					ClusterLODNode* children = &lastNodes[childBegin];

					node = {};
					node.range.isGroup = 0;
					node.range.indexOrOffset = lastLayerOffset + childBegin;
					node.range.countMinusOne = childCount - 1;

					float maxErr = 0.f;
					for (uint32_t c = 0; c < childCount; ++c)
						maxErr = std::max(maxErr, children[c].traversalMetric.maxQuadricError);
					node.traversalMetric.maxQuadricError = maxErr;

					meshopt_Bounds mergedCull = meshopt_computeSphereBounds(
						&children[0].traversalMetric.cullingSphere.x,
						childCount,
						sizeof(ClusterLODNode),
						&children[0].traversalMetric.cullingSphere.w,
						sizeof(ClusterLODNode));
					meshopt_Bounds mergedLod = meshopt_computeSphereBounds(
						&children[0].traversalMetric.lodBoundingSphere.x,
						childCount,
						sizeof(ClusterLODNode),
						&children[0].traversalMetric.lodBoundingSphere.w,
						sizeof(ClusterLODNode));

					node.traversalMetric.cullingSphere = DirectX::XMFLOAT4(
						mergedCull.center[0],
						mergedCull.center[1],
						mergedCull.center[2],
						mergedCull.radius * (1.0f + 1e-5f));
					node.traversalMetric.lodBoundingSphere = DirectX::XMFLOAT4(
						mergedLod.center[0],
						mergedLod.center[1],
						mergedLod.center[2],
						mergedLod.radius * (1.0f + 1e-5f));
				}

				lastLayerOffset = writeOffset;
				writeOffset += iterCount;
			}

			writeOffset--;
			if (range.offset + range.count != writeOffset) {
				throw std::runtime_error("Cluster LOD: traversal node allocation mismatch (range/count).");
			}
		}

		{
			auto BuildInternalNode = [&](uint32_t childOffset, uint32_t childCount) -> ClusterLODNode {
				if (childCount == 0)
					throw std::runtime_error("Cluster LOD: internal node with zero children");

				ClusterLODNode node{};
				node.range.isGroup = 0;
				node.range.indexOrOffset = childOffset;
				node.range.countMinusOne = childCount - 1;

				const ClusterLODNode* children = &state.nodes[childOffset];

				float maxErr = 0.f;
				for (uint32_t c = 0; c < childCount; ++c)
					maxErr = std::max(maxErr, children[c].traversalMetric.maxQuadricError);
				node.traversalMetric.maxQuadricError = maxErr;

				meshopt_Bounds mergedCull = meshopt_computeSphereBounds(
					&children[0].traversalMetric.cullingSphere.x,
					childCount,
					sizeof(ClusterLODNode),
					&children[0].traversalMetric.cullingSphere.w,
					sizeof(ClusterLODNode));
				meshopt_Bounds mergedLod = meshopt_computeSphereBounds(
					&children[0].traversalMetric.lodBoundingSphere.x,
					childCount,
					sizeof(ClusterLODNode),
					&children[0].traversalMetric.lodBoundingSphere.w,
					sizeof(ClusterLODNode));

				node.traversalMetric.cullingSphere = DirectX::XMFLOAT4(
					mergedCull.center[0],
					mergedCull.center[1],
					mergedCull.center[2],
					mergedCull.radius * (1.0f + 1e-5f));
				node.traversalMetric.lodBoundingSphere = DirectX::XMFLOAT4(
					mergedLod.center[0],
					mergedLod.center[1],
					mergedLod.center[2],
					mergedLod.radius * (1.0f + 1e-5f));

				return node;
			};

			std::vector<uint32_t> currentLayer;
			currentLayer.reserve(lodLevelCount);
			for (uint32_t depth = 0; depth < lodLevelCount; ++depth)
				currentLayer.push_back(state.lodLevelRoots[depth]);

			while (currentLayer.size() > preferredNodeWidth)
			{
				std::vector<uint32_t> nextLayer;
				nextLayer.reserve((currentLayer.size() + preferredNodeWidth - 1) / preferredNodeWidth);

				for (uint32_t begin = 0; begin < currentLayer.size(); begin += preferredNodeWidth)
				{
					const uint32_t childCount = std::min<uint32_t>(preferredNodeWidth, uint32_t(currentLayer.size()) - begin);
					const uint32_t childOffset = currentLayer[begin];

					for (uint32_t c = 1; c < childCount; ++c)
					{
						if (currentLayer[begin + c] != childOffset + c)
						{
							throw std::runtime_error("Cluster LOD: expected contiguous node ids while building top hierarchy");
						}
					}

					ClusterLODNode parent = BuildInternalNode(childOffset, childCount);
					const uint32_t parentId = uint32_t(state.nodes.size());
					state.nodes.push_back(parent);
					nextLayer.push_back(parentId);
				}

				currentLayer = std::move(nextLayer);
			}

			if (currentLayer.empty())
				throw std::runtime_error("Cluster LOD: top hierarchy has no roots");

			const uint32_t rootChildOffset = currentLayer.front();
			const uint32_t rootChildCount = uint32_t(currentLayer.size());

			for (uint32_t c = 1; c < rootChildCount; ++c)
			{
				if (currentLayer[c] != rootChildOffset + c)
				{
					throw std::runtime_error("Cluster LOD: expected contiguous root children in top hierarchy");
				}
			}

			ClusterLODNode& root = state.nodes[0];
			root = BuildInternalNode(rootChildOffset, rootChildCount);

			state.topRootNode = 0;
			state.maxTraversalDepth = ComputeCLodTraversalDepth(state.nodes, state.topRootNode);

			uint32_t internalNodes = 0;
			uint32_t segmentLeafNodes = 0;
			uint32_t invalidNodeKindCount = 0;
			for (const ClusterLODNode& node : state.nodes)
			{
				switch (node.range.isGroup)
				{
				case 0u: ++internalNodes; break;
				case 2u: ++segmentLeafNodes; break;
				default: ++invalidNodeKindCount; break;
				}
			}

			uint32_t refinedEdgeCount = 0u;
			uint32_t monotonicErrorViolations = 0u;
			uint32_t invalidSegmentRanges = 0u;
			uint32_t invalidSegmentDomains = 0u;
			uint32_t voxelPayloadMissing = 0u;
			uint32_t voxelTrianglePayloadLeaks = 0u;
			uint32_t invalidPageMapSegments = 0u;
			for (uint32_t groupIndex = 0; groupIndex < static_cast<uint32_t>(state.groups.size()); ++groupIndex)
			{
				const ClusterLODGroup& group = state.groups[groupIndex];
				if (group.firstSegment + group.segmentCount > state.segments.size())
				{
					invalidSegmentRanges++;
					continue;
				}

				const bool isVoxelGroup = (group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u;
				if (isVoxelGroup)
				{
					if (GetVoxelPackedClusterCountForGroup(state, groupIndex) == 0u || GetVoxelPackedCubeCountForGroup(state, groupIndex) == 0u || group.pageCount == 0u)
					{
						voxelPayloadMissing++;
					}
					const bool groupCountsLeak = group.meshletCount != 0u || group.groupVertexCount != 0u;
					const bool chunkCountsLeak = groupIndex < state.groupChunks.size() &&
						(state.groupChunks[groupIndex].meshletCount != 0u ||
							state.groupChunks[groupIndex].groupVertexCount != 0u ||
							state.groupChunks[groupIndex].meshletTrianglesByteCount != 0u);
					if (groupCountsLeak || chunkCountsLeak)
					{
						voxelTrianglePayloadLeaks++;
					}
				}

				for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
				{
					const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
					if (segment.meshletCount != 0u &&
						(segment.pageIndex < group.pageMapBase ||
							segment.pageIndex >= group.pageMapBase + group.pageCount))
					{
						invalidPageMapSegments++;
					}
					if (!isVoxelGroup && groupIndex < state.groupMeshletRefinedGroupChunks.size())
					{
						const std::vector<int32_t>& tags = state.groupMeshletRefinedGroupChunks[groupIndex];
						const uint32_t firstMeshlet = ComputeGroupSegmentFirstMeshlet(state, group, segment);
						if (firstMeshlet + segment.meshletCount > tags.size())
						{
							invalidSegmentRanges++;
						}
						else
						{
							for (uint32_t meshletOffset = 0; meshletOffset < segment.meshletCount; ++meshletOffset)
							{
								if (tags[firstMeshlet + meshletOffset] != segment.refinedGroup)
								{
									invalidSegmentDomains++;
									if (invalidSegmentDomains <= 8u)
									{
										spdlog::error(
											"ClusterLOD hierarchy validation segment domain violation: group={} segment={} page={} first_in_page={} meshlets={} segment_refined={} meshlet={} meshlet_refined={}",
											groupIndex,
											segmentOffset,
											segment.pageIndex,
											segment.firstMeshletInPage,
											segment.meshletCount,
											segment.refinedGroup,
											firstMeshlet + meshletOffset,
											tags[firstMeshlet + meshletOffset]);
									}
									break;
								}
							}
						}
					}

					if (segment.refinedGroup < 0)
					{
						continue;
					}

					refinedEdgeCount++;
					const uint32_t childGroupIndex = static_cast<uint32_t>(segment.refinedGroup);
					if (childGroupIndex >= state.groups.size())
					{
						invalidSegmentRanges++;
						continue;
					}

					const float parentError = group.bounds.error;
					const float childError = state.groups[childGroupIndex].bounds.error;
					const bool finiteParent = std::isfinite(parentError) && parentError < std::numeric_limits<float>::max() * 0.5f;
					const bool finiteChild = std::isfinite(childError) && childError < std::numeric_limits<float>::max() * 0.5f;
					if (finiteParent && finiteChild && !(parentError > childError))
					{
						monotonicErrorViolations++;
						if (monotonicErrorViolations <= 8u)
						{
							spdlog::error(
								"ClusterLOD hierarchy validation monotonic violation: parent_group={} child_group={} parent_depth={} child_depth={} parent_error={} child_error={} parent_flags=0x{:X} child_flags=0x{:X}",
								groupIndex,
								childGroupIndex,
								group.depth,
								state.groups[childGroupIndex].depth,
								parentError,
								childError,
								group.flags,
								state.groups[childGroupIndex].flags);
						}
					}
				}
			}

			uint32_t invalidNodeRanges = 0u;
			uint32_t invalidLeafOwners = 0u;
			uint32_t invalidLeafPayloads = 0u;
			uint32_t internalMaxErrorViolations = 0u;
			std::vector<uint8_t> reachableNodes(state.nodes.size(), 0u);
			std::vector<uint32_t> nodeStack;
			nodeStack.push_back(state.topRootNode);
			while (!nodeStack.empty())
			{
				const uint32_t nodeIndex = nodeStack.back();
				nodeStack.pop_back();
				if (nodeIndex >= state.nodes.size() || reachableNodes[nodeIndex] != 0u)
				{
					continue;
				}

				reachableNodes[nodeIndex] = 1u;
				const ClusterLODNode& node = state.nodes[nodeIndex];
				if (node.range.isGroup == 0u)
				{
					const uint32_t childCount = node.range.countMinusOne + 1u;
					if (childCount == 0u || childCount > preferredNodeWidth || node.range.indexOrOffset + childCount > state.nodes.size())
					{
						invalidNodeRanges++;
						continue;
					}

					float maxChildError = 0.0f;
					for (uint32_t childOffset = 0; childOffset < childCount; ++childOffset)
					{
						const uint32_t childNodeIndex = node.range.indexOrOffset + childOffset;
						maxChildError = std::max(maxChildError, state.nodes[childNodeIndex].traversalMetric.maxQuadricError);
						nodeStack.push_back(childNodeIndex);
					}
					if (node.traversalMetric.maxQuadricError + 1.0e-8f < maxChildError)
					{
						internalMaxErrorViolations++;
					}
					continue;
				}

				if (node.range.ownerGroupId >= state.groups.size())
				{
					invalidLeafOwners++;
					continue;
				}

				const ClusterLODGroup& ownerGroup = state.groups[node.range.ownerGroupId];
				if (node.range.isGroup == 1u)
				{
					invalidLeafPayloads++;
				}
				else if (node.range.isGroup == 2u)
				{
					if (node.range.indexOrOffset >= state.segments.size())
					{
						invalidLeafPayloads++;
					}
				}
			}

			uint32_t unreachableNodes = 0u;
			for (uint8_t reachable : reachableNodes)
			{
				if (reachable == 0u)
				{
					unreachableNodes++;
				}
			}

			spdlog::debug(
				"ClusterLOD runtime hierarchy validation: groups={} refined_edges={} nodes={} reachable_nodes={} unreachable_nodes={} invalid_node_kinds={} invalid_node_ranges={} invalid_leaf_owners={} invalid_leaf_payloads={} internal_max_error_violations={} monotonic_error_violations={} invalid_segment_ranges={} invalid_segment_domains={} invalid_page_map_segments={} voxel_payload_missing={} voxel_triangle_payload_leaks={}",
				state.groups.size(),
				refinedEdgeCount,
				state.nodes.size(),
				state.nodes.size() - unreachableNodes,
				unreachableNodes,
				invalidNodeKindCount,
				invalidNodeRanges,
				invalidLeafOwners,
				invalidLeafPayloads,
				internalMaxErrorViolations,
				monotonicErrorViolations,
				invalidSegmentRanges,
				invalidSegmentDomains,
				invalidPageMapSegments,
				voxelPayloadMissing,
				voxelTrianglePayloadLeaks);

			for (uint32_t depth = 0; depth < lodLevelCount; ++depth)
			{
				uint32_t groupsAtDepth = 0u;
				uint32_t voxelGroupsAtDepth = 0u;
				uint32_t triangleGroupsAtDepth = 0u;
				uint32_t refinedEdgesAtDepth = 0u;
				float minErrorAtDepth = std::numeric_limits<float>::max();
				float maxErrorAtDepth = 0.0f;
				for (const ClusterLODGroup& group : state.groups)
				{
					if (static_cast<uint32_t>(std::max(group.depth, 0)) != depth)
					{
						continue;
					}

					groupsAtDepth++;
					if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u)
					{
						voxelGroupsAtDepth++;
					}
					else
					{
						triangleGroupsAtDepth++;
					}
					for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
					{
						const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
						if (segment.refinedGroup >= 0)
						{
							refinedEdgesAtDepth++;
						}
					}
					minErrorAtDepth = std::min(minErrorAtDepth, group.bounds.error);
					maxErrorAtDepth = std::max(maxErrorAtDepth, group.bounds.error);
				}

				if (groupsAtDepth == 0u)
				{
					minErrorAtDepth = 0.0f;
				}

				uint32_t segmentLeavesAtDepth = 0u;
				for (const TraversalLeafInfo& leaf : leavesByDepth[depth])
				{
					segmentLeavesAtDepth += (leaf.nodeKind == 2u) ? 1u : 0u;
				}

				const uint32_t rootNodeId = state.lodLevelRoots[depth];
				const ClusterLODNode& rootNode = state.nodes[rootNodeId];
				const ClusterLODNodeRangeAlloc range = state.lodNodeRanges[depth];
				spdlog::debug(
					"ClusterLOD runtime hierarchy level: depth={} root={} root_kind={} root_children={} root_error={} range_offset={} range_count={} groups={} voxel_groups={} triangle_groups={} refined_edges={} leaves={} segment_leaves={} min_group_error={} max_group_error={}",
					depth,
					rootNodeId,
					rootNode.range.isGroup,
					(rootNode.range.isGroup == 0u) ? (rootNode.range.countMinusOne + 1u) : 0u,
					rootNode.traversalMetric.maxQuadricError,
					range.offset,
					range.count,
					groupsAtDepth,
					voxelGroupsAtDepth,
					triangleGroupsAtDepth,
					refinedEdgesAtDepth,
					leavesByDepth[depth].size(),
					segmentLeavesAtDepth,
					minErrorAtDepth,
					maxErrorAtDepth);
			}

			if (invalidNodeKindCount != 0u || invalidNodeRanges != 0u || invalidLeafOwners != 0u || invalidLeafPayloads != 0u ||
				internalMaxErrorViolations != 0u || monotonicErrorViolations != 0u || invalidSegmentRanges != 0u || invalidSegmentDomains != 0u ||
				invalidPageMapSegments != 0u || voxelPayloadMissing != 0u || voxelTrianglePayloadLeaks != 0u)
			{
				throw std::runtime_error("Cluster LOD: runtime hierarchy validation failed; see preceding ClusterLOD runtime hierarchy validation logs");
			}

			spdlog::debug(
				"ClusterLOD traversal hierarchy: nodes={} levels={} top_root={} max_depth={} max_traversal_depth={} internal_nodes={} segment_leaf_nodes={}",
				state.nodes.size(),
				lodLevelCount,
				state.topRootNode,
				state.maxDepth,
				state.maxTraversalDepth,
				internalNodes,
				segmentLeafNodes);

			const ClusterLODNode& topRoot = state.nodes[state.topRootNode];
			const uint32_t topRootChildCount = topRoot.range.countMinusOne + 1u;
			spdlog::debug(
				"ClusterLOD traversal top root: child_offset={} child_count={} max_error={} lod_radius={}",
				topRoot.range.indexOrOffset,
				topRootChildCount,
				topRoot.traversalMetric.maxQuadricError,
				topRoot.traversalMetric.lodBoundingSphere.w);
			for (uint32_t childIndex = 0; childIndex < topRootChildCount; ++childIndex)
			{
				const uint32_t childNodeId = topRoot.range.indexOrOffset + childIndex;
				const ClusterLODNode& childNode = state.nodes[childNodeId];
				spdlog::debug(
					"ClusterLOD traversal top root child: index={} node_id={} kind={} child_count={} max_error={} lod_radius={} owner_group={}",
					childIndex,
					childNodeId,
					childNode.range.isGroup,
					(childNode.range.isGroup == 0u) ? (childNode.range.countMinusOne + 1u) : 0u,
					childNode.traversalMetric.maxQuadricError,
					childNode.traversalMetric.lodBoundingSphere.w,
					(childNode.range.isGroup == 0u) ? -1 : static_cast<int32_t>(childNode.range.ownerGroupId));
			}

			for (uint32_t depth = 0; depth < lodLevelCount; ++depth)
			{
				const uint32_t nodeId = state.lodLevelRoots[depth];
				const ClusterLODNode& depthRoot = state.nodes[nodeId];
				const uint32_t nodeKind = depthRoot.range.isGroup;
				const uint32_t childCount = (nodeKind == 0u) ? (depthRoot.range.countMinusOne + 1u) : 0u;
				int32_t ownerGroupId = -1;
				float ownerGroupError = -1.0f;
				uint32_t ownerGroupFlags = 0u;
				uint32_t ownerGroupSegments = 0u;

				if (nodeKind != 0u && depthRoot.range.ownerGroupId < state.groups.size())
				{
					ownerGroupId = static_cast<int32_t>(depthRoot.range.ownerGroupId);
					const ClusterLODGroup& ownerGroup = state.groups[depthRoot.range.ownerGroupId];
					ownerGroupError = ownerGroup.bounds.error;
					ownerGroupFlags = ownerGroup.flags;
					ownerGroupSegments = ownerGroup.segmentCount;
				}

				spdlog::debug(
					"ClusterLOD traversal depth root: depth={} node_id={} kind={} child_count={} max_error={} lod_radius={} owner_group={} owner_error={} owner_flags=0x{:X} owner_segments={}",
					depth,
					nodeId,
					nodeKind,
					childCount,
					depthRoot.traversalMetric.maxQuadricError,
					depthRoot.traversalMetric.lodBoundingSphere.w,
					ownerGroupId,
					ownerGroupError,
					ownerGroupFlags,
					ownerGroupSegments);
			}
		}
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
	ClusterLODBuildState state{};

	const unsigned int* idx = reinterpret_cast<const unsigned int*>(indices.data());

	const size_t vertexStrideBytes = vertexSize;
	const size_t globalVertexCount = vertices.size() / vertexStrideBytes;
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

	if (enableNormalAttributeSimplification && hasNormalStreamInSource && hasTexcoordStreamInSource)
	{
		if (!GenerateMikkTangents(vertices, vertexStrideBytes, indices, tangentAttributeStream))
		{
			spdlog::warn("ClusterLOD: failed to generate MikkTSpace tangents; continuing without tangent simplification attributes");
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
			}
		}
	}

	clodMesh mesh{};
	mesh.indices = idx;
	mesh.index_count = indices.size();
	mesh.vertex_count = globalVertexCount;
	mesh.vertex_positions = reinterpret_cast<const float*>(vertices.data());
	mesh.vertex_positions_stride = vertexStrideBytes;

	mesh.vertex_attributes = simplifyAttributeStream.empty() ? nullptr : simplifyAttributeStream.data();
	mesh.vertex_attributes_stride = simplifyAttributeStream.empty() ? 0 : sizeof(float) * simplifyAttributeCount;
	mesh.vertex_lock = nullptr;
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
					capturedGroup.flattenedIndices.insert(
						capturedGroup.flattenedIndices.end(),
						cluster.indices,
						cluster.indices + cluster.index_count);

					capturedGroup.clusters.push_back(std::move(capturedCluster));
				}

				ClusterLODGroupBuildOutput output = BuildClusterLODGroupOutput(
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
					context->recomputeGroupNormals);

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
		captureContext.recomputeGroupNormals = recomputeGroupNormals;
		// Keep CLOD generation deterministic while investigating cold-build normal/tangent instability.
		// The output callback assigns renderer group IDs in callback arrival order, so parallel
		// clodBuildEx execution can make persisted hierarchy/page ordering depend on worker scheduling.
		clodBuildEx(config, mesh, &captureContext, &ClodBuildCallbacks::Output, nullptr);

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

	VoxelSourceTriangleBVH coverageSourceTriangles;
	coverageSourceTriangles.Build(
		&vertices,
		vertexStrideBytes,
		&indices,
		skinningVertices,
		skinningVertexSize,
		nullptr,
		settings.doubleSidedVoxelSourceNormals);

	BuildVoxelFallbackCandidates(
		state,
		vertexStrideBytes,
		skinningVertexSize,
		coverageSourceTriangles.IsValid() ? &coverageSourceTriangles : nullptr,
		coverageMaterialSampler,
		settings);

	// Build traversal hierarchy.
	BuildClusterLODTraversalHierarchy(state, /*preferredNodeWidth=*/TraversalNodeFanout);

	// Release raw streams after mesh hierarchy construction.
	{ std::vector<std::vector<std::byte>>().swap(state.groupVertexChunks); }
	{ std::vector<std::vector<uint32_t>>().swap(state.groupMeshletVertexChunks); }
	{ std::vector<std::vector<meshopt_Meshlet>>().swap(state.groupMeshletChunks); }
	{ std::vector<std::vector<uint8_t>>().swap(state.groupMeshletTriangleChunks); }
	{ std::vector<std::vector<int32_t>>().swap(state.groupMeshletRefinedGroupChunks); }

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
	FinalizeMeshWidePagePacking(
		state,
		meshPageBlobs,
		groupPageReferences,
		groupPageReferenceOffsets,
		trianglePageCount,
		voxelPageBase,
		voxelPageCount);

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
	artifacts.prebuiltData.lodNodeRanges = std::move(state.lodNodeRanges);
	artifacts.prebuiltData.lodLevelRoots = std::move(state.lodLevelRoots);
	artifacts.prebuiltData.maxDepth = state.maxDepth;
	artifacts.prebuiltData.maxTraversalDepth = state.maxTraversalDepth;

	artifacts.cacheBuildData.groupPageBlobs = std::move(state.groupPageBlobs);
	artifacts.cacheBuildData.meshPageBlobs = std::move(meshPageBlobs);

	return artifacts;
}

ClusterLODPrebuildArtifacts BuildVoxelOnlyClusterLODArtifactsFromGeometry(
	const std::vector<std::byte>& vertices,
	unsigned int vertexSize,
	const std::vector<uint32_t>& indices,
	const ClusterLODBuilderSettings& settings,
	uint32_t maxCubesPerCluster)
{
	return BuildVoxelOnlyClusterLODArtifactsFromGeometry(
		vertices,
		vertexSize,
		indices,
		settings,
		std::nullopt,
		maxCubesPerCluster);
}

ClusterLODPrebuildArtifacts BuildVoxelOnlyClusterLODArtifactsFromPayload(
	const VoxelGroupPayload& payload,
	const ClusterLODBuilderSettings& settings,
	uint32_t maxCubesPerCluster)
{
	ClusterLODPrebuildArtifacts artifacts{};
	if (payload.activeCells.empty() || payload.resolution == 0u || !(payload.voxelWidth > 0.0f))
	{
		return artifacts;
	}

	const float voxelTraversalError = ComputeVoxelTraversalError(payload.voxelWidth, 0.0f);
	PackVoxelGroupInput packInput{};
	packInput.payload = &payload;
	packInput.voxelError = voxelTraversalError;
	packInput.opacityThreshold = settings.voxelFallbackOpacityThreshold;
	packInput.dominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
	PackedVoxelGroupBuildResult packed = PackVoxelGroupToCubes(packInput);
	BuildVoxelClustersFromCubes(packed, std::max(1u, maxCubesPerCluster));
	if (packed.cubeRecords.empty() || packed.clusterRecords.empty())
	{
		return artifacts;
	}

	std::vector<ClusterLODGroupSegment> voxelSegments;
	std::vector<BoundingSphere> voxelSegmentBounds;
	SplitVoxelClustersIntoPageSegments(packed, voxelSegments, voxelSegmentBounds);
	std::vector<std::vector<std::byte>> voxelPageBlobs = BuildVoxelGroupPageBlobs(
		packed.descriptor,
		voxelSegments,
		packed.clusterRecords,
		packed.cubeRecords,
		packed.attributeSamples,
		0u);
	if (voxelSegments.empty() || voxelPageBlobs.empty())
	{
		return artifacts;
	}

	ClusterLODBuildState state{};
	ClusterLODGroup group{};
	const DirectX::XMFLOAT3 payloadMin = payload.aabbMin;
	const DirectX::XMFLOAT3 payloadMax = payload.aabbMax;
	const float centerX = 0.5f * (payloadMin.x + payloadMax.x);
	const float centerY = 0.5f * (payloadMin.y + payloadMax.y);
	const float centerZ = 0.5f * (payloadMin.z + payloadMax.z);
	const float dx = payloadMax.x - centerX;
	const float dy = payloadMax.y - centerY;
	const float dz = payloadMax.z - centerZ;
	group.bounds.center[0] = centerX;
	group.bounds.center[1] = centerY;
	group.bounds.center[2] = centerZ;
	group.bounds.radius = std::sqrt(dx * dx + dy * dy + dz * dz);
	group.bounds.error = std::numeric_limits<float>::max();
	group.depth = 0;
	group.firstSegment = 0u;
	group.segmentCount = static_cast<uint32_t>(voxelSegments.size());
	group.terminalSegmentCount = group.segmentCount;
	group.flags = CLOD_GROUP_FLAG_IS_VOXEL;
	group.pageCount = static_cast<uint32_t>(voxelPageBlobs.size());
	group.representationError = voxelTraversalError;

	state.groups.push_back(group);
	state.segments = std::move(voxelSegments);
	state.segmentBounds = std::move(voxelSegmentBounds);
	state.groupChunks.resize(1);
	state.groupPageBlobs.resize(1);
	state.groupPageBlobs[0] = std::move(voxelPageBlobs);
	state.voxelGroupMapping.groupToPayloadIndex = { 0 };
	state.voxelGroupMapping.groupToPackedDescriptorIndex = { 0 };
	state.voxelGroupMapping.payloads.push_back(payload);
	state.voxelGroupMapping.packedGroupDescriptors.push_back(packed.descriptor);
	state.voxelGroupMapping.packedClusterRecords = std::move(packed.clusterRecords);
	state.voxelGroupMapping.packedCubeRecords = std::move(packed.cubeRecords);
	state.voxelGroupMapping.packedAttributeSamples = std::move(packed.attributeSamples);

	BuildClusterLODTraversalHierarchy(state, /*preferredNodeWidth=*/8u);

	std::vector<std::vector<std::byte>> meshPageBlobs;
	std::vector<uint32_t> groupPageReferences;
	std::vector<uint32_t> groupPageReferenceOffsets;
	uint32_t trianglePageCount = 0u;
	uint32_t voxelPageBase = 0u;
	uint32_t voxelPageCount = 0u;
	FinalizeMeshWidePagePacking(
		state,
		meshPageBlobs,
		groupPageReferences,
		groupPageReferenceOffsets,
		trianglePageCount,
		voxelPageBase,
		voxelPageCount);

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
	artifacts.prebuiltData.lodNodeRanges = std::move(state.lodNodeRanges);
	artifacts.prebuiltData.lodLevelRoots = std::move(state.lodLevelRoots);
	artifacts.prebuiltData.maxDepth = state.maxDepth;
	artifacts.prebuiltData.maxTraversalDepth = state.maxTraversalDepth;
	artifacts.cacheBuildData.groupPageBlobs = std::move(state.groupPageBlobs);
	artifacts.cacheBuildData.meshPageBlobs = std::move(meshPageBlobs);

	return artifacts;
}

ClusterLODPrebuildArtifacts BuildVoxelOnlyClusterLODArtifactsFromGeometry(
	const std::vector<std::byte>& vertices,
	unsigned int vertexSize,
	const std::vector<uint32_t>& indices,
	const ClusterLODBuilderSettings& settings,
	const std::optional<ClusterLODVoxelGridOverride>& gridOverride,
	uint32_t maxCubesPerCluster)
{
	ClusterLODPrebuildArtifacts artifacts{};
	const size_t vertexStrideBytes = vertexSize;
	const size_t vertexCount = vertexStrideBytes != 0u ? vertices.size() / vertexStrideBytes : 0u;
	if (vertexCount == 0u || vertexStrideBytes < sizeof(float) * 3u || indices.empty() || (indices.size() % 3u) != 0u)
	{
		return artifacts;
	}

	DirectX::XMFLOAT3 aabbMin(
		std::numeric_limits<float>::max(),
		std::numeric_limits<float>::max(),
		std::numeric_limits<float>::max());
	DirectX::XMFLOAT3 aabbMax(
		std::numeric_limits<float>::lowest(),
		std::numeric_limits<float>::lowest(),
		std::numeric_limits<float>::lowest());

	for (uint32_t index : indices)
	{
		if (index >= vertexCount)
		{
			continue;
		}

		const DirectX::XMFLOAT3 position = ReadGroupVertexPosition(vertices, vertexStrideBytes, index);
		aabbMin.x = std::min(aabbMin.x, position.x);
		aabbMin.y = std::min(aabbMin.y, position.y);
		aabbMin.z = std::min(aabbMin.z, position.z);
		aabbMax.x = std::max(aabbMax.x, position.x);
		aabbMax.y = std::max(aabbMax.y, position.y);
		aabbMax.z = std::max(aabbMax.z, position.z);
	}

	const float extentX = aabbMax.x - aabbMin.x;
	const float extentY = aabbMax.y - aabbMin.y;
	const float extentZ = aabbMax.z - aabbMin.z;
	const float longestExtent = std::max({ extentX, extentY, extentZ });
	if (!std::isfinite(longestExtent) || longestExtent <= 1.0e-8f)
	{
		return artifacts;
	}

	const uint32_t resolution = std::max(
		2u,
		std::max(settings.voxelMinResolution, settings.voxelGridBaseResolution));
	float voxelWidth = longestExtent / static_cast<float>(resolution);
	if (gridOverride)
	{
		const auto& grid = *gridOverride;
		if (grid.resolution < 2u ||
			!(grid.voxelWidth > 0.0f) ||
			!std::isfinite(grid.voxelWidth) ||
			grid.aabbMax.x <= grid.aabbMin.x ||
			grid.aabbMax.y <= grid.aabbMin.y ||
			grid.aabbMax.z <= grid.aabbMin.z)
		{
			return artifacts;
		}

		aabbMin = grid.aabbMin;
		aabbMax = grid.aabbMax;
		voxelWidth = grid.voxelWidth;
	}
	else
	{
		const float minVoxelizationThickness = std::max(longestExtent * 1.0e-4f, 1.0e-5f);
		auto padDegenerateAxis = [minVoxelizationThickness](float& minValue, float& maxValue)
		{
			if (maxValue - minValue > minVoxelizationThickness)
			{
				return;
			}

			const float center = 0.5f * (minValue + maxValue);
			minValue = center - 0.5f * minVoxelizationThickness;
			maxValue = center + 0.5f * minVoxelizationThickness;
		};
		padDegenerateAxis(aabbMin.x, aabbMax.x);
		padDegenerateAxis(aabbMin.y, aabbMax.y);
		padDegenerateAxis(aabbMin.z, aabbMax.z);

		auto expandAxisToExtent = [](float& minValue, float& maxValue, float targetExtent)
		{
			const float currentExtent = maxValue - minValue;
			if (currentExtent >= targetExtent)
			{
				return;
			}

			const float center = 0.5f * (minValue + maxValue);
			minValue = center - 0.5f * targetExtent;
			maxValue = center + 0.5f * targetExtent;
		};
		expandAxisToExtent(aabbMin.x, aabbMax.x, longestExtent);
		expandAxisToExtent(aabbMin.y, aabbMax.y, longestExtent);
		expandAxisToExtent(aabbMin.z, aabbMax.z, longestExtent);
	}
	if (!(voxelWidth > 0.0f) || !std::isfinite(voxelWidth))
	{
		return artifacts;
	}
	const uint32_t voxelResolution = gridOverride ? gridOverride->resolution : resolution;

	VoxelSourceTriangleBVH coverageSourceTriangles;
	coverageSourceTriangles.Build(
		&vertices,
		vertexStrideBytes,
		&indices,
		nullptr,
		0u,
		nullptr,
		settings.doubleSidedVoxelSourceNormals);

	VoxelizeTrianglesInput voxelInput{};
	voxelInput.vertices = &vertices;
	voxelInput.vertexStrideBytes = vertexStrideBytes;
	voxelInput.triangleIndices = &indices;
	voxelInput.doubleSidedTriangles = settings.doubleSidedVoxelSourceNormals;
	voxelInput.coverageSourceTriangles = coverageSourceTriangles.IsValid() ? &coverageSourceTriangles : nullptr;
	voxelInput.keepZeroCoverageSourceCells = settings.voxelFallbackCarryZeroCoverage;
	voxelInput.aabbMin = aabbMin;
	voxelInput.aabbMax = aabbMax;
	voxelInput.voxelWidth = voxelWidth;
	voxelInput.resolution = voxelResolution;
	voxelInput.raysPerCell = settings.voxelRaysPerCell;
	voxelInput.pruningMode = settings.voxelFallbackPruningMode;
	VoxelizeTrianglesResult voxelResult = VoxelizeTrianglesDetailed(voxelInput);
	if (voxelResult.renderPayload.activeCells.empty())
	{
		return artifacts;
	}

	const float voxelTraversalError = ComputeVoxelTraversalError(voxelWidth, 0.0f);
	PackVoxelGroupInput packInput{};
	packInput.payload = &voxelResult.renderPayload;
	packInput.voxelError = voxelTraversalError;
	packInput.opacityThreshold = settings.voxelFallbackOpacityThreshold;
	packInput.dominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
	PackedVoxelGroupBuildResult packed = PackVoxelGroupToCubes(packInput);
	BuildVoxelClustersFromCubes(packed, std::max(1u, maxCubesPerCluster));
	if (packed.cubeRecords.empty() || packed.clusterRecords.empty())
	{
		return artifacts;
	}

	std::vector<ClusterLODGroupSegment> voxelSegments;
	std::vector<BoundingSphere> voxelSegmentBounds;
	SplitVoxelClustersIntoPageSegments(packed, voxelSegments, voxelSegmentBounds);
	std::vector<std::vector<std::byte>> voxelPageBlobs = BuildVoxelGroupPageBlobs(
		packed.descriptor,
		voxelSegments,
		packed.clusterRecords,
		packed.cubeRecords,
		packed.attributeSamples,
		0u);
	if (voxelSegments.empty() || voxelPageBlobs.empty())
	{
		return artifacts;
	}

	ClusterLODBuildState state{};
	ClusterLODGroup group{};
	const DirectX::XMFLOAT3 payloadMin = voxelResult.renderPayload.aabbMin;
	const DirectX::XMFLOAT3 payloadMax = voxelResult.renderPayload.aabbMax;
	const float centerX = 0.5f * (payloadMin.x + payloadMax.x);
	const float centerY = 0.5f * (payloadMin.y + payloadMax.y);
	const float centerZ = 0.5f * (payloadMin.z + payloadMax.z);
	const float dx = payloadMax.x - centerX;
	const float dy = payloadMax.y - centerY;
	const float dz = payloadMax.z - centerZ;
	group.bounds.center[0] = centerX;
	group.bounds.center[1] = centerY;
	group.bounds.center[2] = centerZ;
	group.bounds.radius = std::sqrt(dx * dx + dy * dy + dz * dz);
	group.bounds.error = std::numeric_limits<float>::max();
	group.depth = 0;
	group.firstSegment = 0u;
	group.segmentCount = static_cast<uint32_t>(voxelSegments.size());
	group.terminalSegmentCount = group.segmentCount;
	group.flags = CLOD_GROUP_FLAG_IS_VOXEL;
	group.pageCount = static_cast<uint32_t>(voxelPageBlobs.size());
	group.representationError = voxelTraversalError;

	state.groups.push_back(group);
	state.segments = std::move(voxelSegments);
	state.segmentBounds = std::move(voxelSegmentBounds);
	state.groupChunks.resize(1);
	state.groupPageBlobs.resize(1);
	state.groupPageBlobs[0] = std::move(voxelPageBlobs);
	state.voxelGroupMapping.groupToPayloadIndex = { 0 };
	state.voxelGroupMapping.groupToPackedDescriptorIndex = { 0 };
	state.voxelGroupMapping.payloads.push_back(std::move(voxelResult.renderPayload));
	state.voxelGroupMapping.packedGroupDescriptors.push_back(packed.descriptor);
	state.voxelGroupMapping.packedClusterRecords = std::move(packed.clusterRecords);
	state.voxelGroupMapping.packedCubeRecords = std::move(packed.cubeRecords);
	state.voxelGroupMapping.packedAttributeSamples = std::move(packed.attributeSamples);

	BuildClusterLODTraversalHierarchy(state, /*preferredNodeWidth=*/8u);

	std::vector<std::vector<std::byte>> meshPageBlobs;
	std::vector<uint32_t> groupPageReferences;
	std::vector<uint32_t> groupPageReferenceOffsets;
	uint32_t trianglePageCount = 0u;
	uint32_t voxelPageBase = 0u;
	uint32_t voxelPageCount = 0u;
	FinalizeMeshWidePagePacking(
		state,
		meshPageBlobs,
		groupPageReferences,
		groupPageReferenceOffsets,
		trianglePageCount,
		voxelPageBase,
		voxelPageCount);

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
	artifacts.prebuiltData.lodNodeRanges = std::move(state.lodNodeRanges);
	artifacts.prebuiltData.lodLevelRoots = std::move(state.lodLevelRoots);
	artifacts.prebuiltData.maxDepth = state.maxDepth;
	artifacts.prebuiltData.maxTraversalDepth = state.maxTraversalDepth;
	artifacts.cacheBuildData.groupPageBlobs = std::move(state.groupPageBlobs);
	artifacts.cacheBuildData.meshPageBlobs = std::move(meshPageBlobs);

	return artifacts;
}
