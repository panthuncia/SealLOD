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
#include <string>

#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Mesh/VertexLayout.h"
#include "Mesh/VertexFlags.h"
#include "Mesh/SGGX.h"
#include "Mesh/VoxelGroupBuilder.h"
#include "Utilities/mikktspace.h"

#include "../shaders/Common/defines.h"

namespace
{
	constexpr uint32_t CLOD_COMPRESSED_POSITIONS = 1u << 0;
	constexpr uint32_t CLOD_COMPRESSED_MESHLET_VERTEX_INDICES = 1u << 1;
	constexpr uint32_t CLOD_COMPRESSED_NORMALS = 1u << 2;
	constexpr uint32_t CLOD_VOXEL_PAGE_MAGIC = 0x4C435856u; // VXCL
	constexpr uint32_t CLOD_VOXEL_PAGE_HEADER_SIZE = 64u;
	constexpr uint32_t CLOD_STREAMING_PAGE_SIZE_BYTES = 256u * 1024u;
	constexpr uint32_t CLOD_VOXEL_ATTRIBUTE_SAMPLES_COMPACT = 0u;
	constexpr uint32_t kMaxSkinInfluences = 8u;
	constexpr float CLOD_UV_QUANTIZATION_SCALE = 65535.0f;
	constexpr float CLOD_UV_QUANTIZATION_INV_SCALE = 1.0f / CLOD_UV_QUANTIZATION_SCALE;
	constexpr const char* OBJECT_REYES_ATLAS_HEIGHT_UV_SET_NAME = "__object_reyes_atlas_height";
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

	uint32_t CountVoxelAttributeSamples(uint64_t occupancyMask)
	{
		return static_cast<uint32_t>(std::popcount(occupancyMask));
	}

	uint32_t CountVoxelAttributeSamples(
		std::span<const CLodVoxelCubeRecord> cubeRecords,
		uint32_t firstCube,
		uint32_t cubeCount)
	{
		uint32_t attributeCount = 0u;
		const uint32_t endCube = std::min<uint32_t>(
			static_cast<uint32_t>(cubeRecords.size()),
			firstCube + cubeCount);
		for (uint32_t cubeIndex = firstCube; cubeIndex < endCube; ++cubeIndex)
		{
			attributeCount += CountVoxelAttributeSamples(cubeRecords[cubeIndex].occupancyMask);
		}
		return attributeCount;
	}

	uint32_t ComputeVoxelClusterFlags(
		std::span<const CLodVoxelCubeRecord> cubeRecords,
		uint32_t firstCube,
		uint32_t cubeCount)
	{
		uint32_t flags = 0u;
		const uint32_t endCube = std::min<uint32_t>(
			static_cast<uint32_t>(cubeRecords.size()),
			firstCube + cubeCount);
		for (uint32_t cubeIndex = firstCube; cubeIndex < endCube; ++cubeIndex)
		{
			if (cubeRecords[cubeIndex].dominantBoneIndex != CLOD_VOXEL_STATIC_BONE_INDEX)
			{
				flags |= CLOD_VOXEL_CLUSTER_FLAG_HAS_SKINNED_CUBES;
				break;
			}
		}
		return flags;
	}

	bool BuildVoxelGroupPayloadFromPackedMapping(
		const VoxelGroupMapping& mapping,
		uint32_t groupIndex,
		VoxelGroupPayload& outPayload)
	{
		if (groupIndex >= mapping.groupToPackedMetadataIndex.size())
		{
			return false;
		}

		const int32_t metadataIndex = mapping.groupToPackedMetadataIndex[groupIndex];
		if (metadataIndex < 0 || static_cast<size_t>(metadataIndex) >= mapping.packedGroupMetadata.size())
		{
			return false;
		}

		const VoxelGroupPackedMetadata& metadata = mapping.packedGroupMetadata[static_cast<size_t>(metadataIndex)];
		if (metadata.resolution == 0u ||
			metadata.firstCube > mapping.packedCubeRecords.size() ||
			metadata.cubeCount > mapping.packedCubeRecords.size() - metadata.firstCube)
		{
			return false;
		}

		const uint32_t firstCube = metadata.firstCube;
		const uint32_t endCube = metadata.firstCube + metadata.cubeCount;
		size_t activeCellCount = 0u;
		for (uint32_t cubeIndex = firstCube; cubeIndex < endCube; ++cubeIndex)
		{
			activeCellCount += CountVoxelAttributeSamples(mapping.packedCubeRecords[cubeIndex].occupancyMask);
		}

		outPayload.resolution = metadata.resolution;
		outPayload.aabbMin = DirectX::XMFLOAT3(
			metadata.aabbMinAndVoxelWidth.x,
			metadata.aabbMinAndVoxelWidth.y,
			metadata.aabbMinAndVoxelWidth.z);
		outPayload.aabbMax = DirectX::XMFLOAT3(
			metadata.aabbMaxAndError.x,
			metadata.aabbMaxAndError.y,
			metadata.aabbMaxAndError.z);
		outPayload.voxelWidth = metadata.aabbMinAndVoxelWidth.w;
		outPayload.activeCells.clear();
		outPayload.activeCells.reserve(activeCellCount);

		for (uint32_t cubeIndex = firstCube; cubeIndex < endCube; ++cubeIndex)
		{
			const CLodVoxelCubeRecord& cube = mapping.packedCubeRecords[cubeIndex];
			const uint32_t cubeX = cube.cubeCoord & 1023u;
			const uint32_t cubeY = (cube.cubeCoord >> 10u) & 1023u;
			const uint32_t cubeZ = (cube.cubeCoord >> 20u) & 1023u;
			uint32_t attributeIndex = cube.firstAttribute;
			for (uint32_t localBit = 0u; localBit < 64u; ++localBit)
			{
				if ((cube.occupancyMask & (uint64_t{ 1 } << localBit)) == 0u)
				{
					continue;
				}
				if (attributeIndex >= mapping.packedAttributeSamples.size())
				{
					outPayload = {};
					return false;
				}

				const uint32_t cellX = cubeX * 4u + (localBit & 3u);
				const uint32_t cellY = cubeY * 4u + ((localBit >> 2u) & 3u);
				const uint32_t cellZ = cubeZ * 4u + ((localBit >> 4u) & 3u);
				if (cellX >= metadata.resolution || cellY >= metadata.resolution || cellZ >= metadata.resolution)
				{
					outPayload = {};
					return false;
				}

				const CLodVoxelAttributeSample& attribute = mapping.packedAttributeSamples[attributeIndex++];
				VoxelCell cell{};
				cell.x = cellX;
				cell.y = cellY;
				cell.z = cellZ;
				cell.opacity = attribute.opacity;
				cell.sggxAxisAndSigmas = attribute.sggxAxisAndSigmas;
				cell.uv = attribute.uv;
				cell.dominantBoneIndex = cube.dominantBoneIndex;
				cell.refinedGroup = cube.refinedGroup;
				outPayload.activeCells.push_back(cell);
			}
		}

		return outPayload.voxelWidth > 0.0f && !outPayload.activeCells.empty();
	}

	std::vector<std::vector<std::byte>> BuildVoxelGroupPageBlobs(
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
		const uint32_t clusterRecordOffset = CLOD_VOXEL_PAGE_HEADER_SIZE;

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
			const uint32_t pageAttributeCount = CountVoxelAttributeSamples(cubeRecords, firstCubeInGroup, pageCubeCount);
			const uint32_t pageCubeRecordOffset = static_cast<uint32_t>(align4(static_cast<size_t>(clusterRecordOffset) + pageClusterCount * sizeof(CLodVoxelClusterRecord)));
			const uint32_t attributeOffset = pageCubeRecordOffset + pageCubeCount * static_cast<uint32_t>(sizeof(CLodVoxelCubeRecord));
			const size_t pageSize = static_cast<size_t>(attributeOffset) +
				static_cast<size_t>(pageAttributeCount) * sizeof(CLodVoxelAttributeSample);
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
				firstClusterInGroup,
				pageClusterCount,
				firstCubeInGroup,
				pageCubeCount,
				0u,
				0u,
				0u,
				clusterRecordOffset,
				pageCubeRecordOffset,
				attributeOffset,
				CLOD_VOXEL_ATTRIBUTE_SAMPLES_COMPACT,
				static_cast<uint32_t>(sizeof(CLodVoxelClusterRecord)),
				static_cast<uint32_t>(sizeof(CLodVoxelCubeRecord)),
				static_cast<uint32_t>(sizeof(CLodVoxelAttributeSample)),
				0u
			};
			std::memcpy(blob.data(), header.data(), header.size() * sizeof(uint32_t));

			for (uint32_t clusterIndex = 0; clusterIndex < pageClusterCount; ++clusterIndex)
			{
				CLodVoxelClusterRecord pageCluster = clusterRecords[firstClusterInGroup + clusterIndex];
				pageCluster.firstCube -= firstCubeInGroup;
				StorePod(blob, clusterRecordOffset + clusterIndex * sizeof(CLodVoxelClusterRecord), pageCluster);
			}

			uint32_t pageAttributeCursor = 0u;
			for (uint32_t cubeIndex = 0; cubeIndex < pageCubeCount; ++cubeIndex)
			{
				CLodVoxelCubeRecord pageCube = cubeRecords[firstCubeInGroup + cubeIndex];
				const uint32_t globalFirstAttribute = pageCube.firstAttribute;
				const uint32_t cubeAttributeCount = CountVoxelAttributeSamples(pageCube.occupancyMask);
				pageCube.firstAttribute = pageAttributeCursor;
				StorePod(blob, pageCubeRecordOffset + cubeIndex * sizeof(CLodVoxelCubeRecord), pageCube);

				if (globalFirstAttribute < attributeSampleBase)
				{
					pageAttributeCursor += cubeAttributeCount;
					continue;
				}
				const size_t attributeSourceOffset = static_cast<size_t>(globalFirstAttribute - attributeSampleBase) * sizeof(CLodVoxelAttributeSample);
				const size_t attributeDestOffset = static_cast<size_t>(attributeOffset) +
					static_cast<size_t>(pageCube.firstAttribute) * sizeof(CLodVoxelAttributeSample);
				const size_t attributeBytes = static_cast<size_t>(cubeAttributeCount) * sizeof(CLodVoxelAttributeSample);
				if (attributeSourceOffset + attributeBytes <= attributeSamples.size() * sizeof(CLodVoxelAttributeSample))
				{
					std::memcpy(blob.data() + attributeDestOffset,
						reinterpret_cast<const std::byte*>(attributeSamples.data()) + attributeSourceOffset,
						attributeBytes);
				}
				pageAttributeCursor += cubeAttributeCount;
			}

			pageBlobs.push_back(std::move(blob));
		}

		return pageBlobs;
	}

	uint32_t ComputeVoxelPageSizeBytes(uint32_t clusterCount, uint32_t cubeCount, uint32_t attributeCount)
	{
		constexpr uint32_t fixedBytes = CLOD_VOXEL_PAGE_HEADER_SIZE;
		return fixedBytes +
			clusterCount * static_cast<uint32_t>(sizeof(CLodVoxelClusterRecord)) +
			cubeCount * static_cast<uint32_t>(sizeof(CLodVoxelCubeRecord)) +
			attributeCount * static_cast<uint32_t>(sizeof(CLodVoxelAttributeSample));
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
			packed.metadata.clusterCount = 0u;
			return;
		}

		std::vector<CLodVoxelClusterRecord> pageClusterRecords;
		pageClusterRecords.reserve(packed.clusterRecords.size());

		uint32_t pageFirstCluster = 0u;
		uint32_t pageClusterCount = 0u;
		uint32_t pageCubeCount = 0u;
		uint32_t pageAttributeCount = 0u;
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
			segment.pageIndex = static_cast<uint32_t>(outSegments.size());
			outSegments.push_back(segment);
			outSegmentBounds.push_back(BoundingSphere{ pageBounds });

			pageFirstCluster = static_cast<uint32_t>(pageClusterRecords.size());
			pageClusterCount = 0u;
			pageCubeCount = 0u;
			pageAttributeCount = 0u;
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
				uint32_t chunkAttributes = CountVoxelAttributeSamples(
					std::span<const CLodVoxelCubeRecord>(packed.cubeRecords.data(), packed.cubeRecords.size()),
					sourceCluster.firstCube + cubeOffset,
					chunkCubes);
				while (chunkCubes > 0u &&
					ComputeVoxelPageSizeBytes(
						pageClusterCount + 1u,
						pageCubeCount + chunkCubes,
						pageAttributeCount + chunkAttributes) > CLOD_STREAMING_PAGE_SIZE_BYTES)
				{
					chunkCubes--;
					chunkAttributes = CountVoxelAttributeSamples(
						std::span<const CLodVoxelCubeRecord>(packed.cubeRecords.data(), packed.cubeRecords.size()),
						sourceCluster.firstCube + cubeOffset,
						chunkCubes);
				}

				if (chunkCubes == 0u)
				{
					flushPage();
					continue;
				}

				CLodVoxelClusterRecord pageCluster = sourceCluster;
				pageCluster.firstCube = sourceCluster.firstCube + cubeOffset;
				pageCluster.cubeCount = chunkCubes;
				pageCluster.flags = ComputeVoxelClusterFlags(
					std::span<const CLodVoxelCubeRecord>(packed.cubeRecords.data(), packed.cubeRecords.size()),
					pageCluster.firstCube,
					pageCluster.cubeCount);
				pageClusterRecords.push_back(pageCluster);
				pageClusterCount++;
				pageCubeCount += chunkCubes;
				pageAttributeCount += chunkAttributes;
				pageBounds = mergeBounds(pageBounds, pageCluster.bounds);

				remainingCubes -= chunkCubes;
				cubeOffset += chunkCubes;
			}
		}
		flushPage();

		packed.clusterRecords = std::move(pageClusterRecords);
		packed.metadata.clusterCount = static_cast<uint32_t>(packed.clusterRecords.size());
		packed.metadata.cubeCount = static_cast<uint32_t>(packed.cubeRecords.size());
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

	void AssignSingleRootPartRecord(ClusterLODPrebuiltData& data, uint32_t rootNode)
	{
		ClusterLODPartRecord rootPart{};
		rootPart.groupBase = 0u;
		rootPart.groupCount = static_cast<uint32_t>(data.groups.size());
		rootPart.nodeBase = 0u;
		rootPart.nodeCount = static_cast<uint32_t>(data.nodes.size());
		rootPart.transformBase = 0u;
		rootPart.transformCount = static_cast<uint32_t>(data.assemblyTransforms.size());
		rootPart.instanceBase = 0u;
		rootPart.instanceCount = static_cast<uint32_t>(data.assemblyInstances.size());
		rootPart.rootNode = rootNode;
		rootPart.flags = CLOD_PART_RECORD_FLAG_ROOT;
		data.partRecords.clear();
		data.partRecords.push_back(rootPart);
		data.rootPartIndex = 0u;
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
		std::vector<uint8_t> traversalGroupMask;
		uint32_t topRootNode = 0;
		uint32_t maxDepth = 0;
		uint32_t maxTraversalDepth = 0;
		VoxelGroupMapping voxelGroupMapping;
		std::vector<VoxelGroupPayload> voxelCarryPayloads;
	};

	struct PagePackingSegmentRef
	{
		uint32_t groupIndex = 0;
		uint32_t segmentIndex = 0;
		uint32_t sourcePageIndex = 0;
		uint32_t firstMeshletInPage = 0;
		uint32_t meshletCount = 0;
	};

	using TriangleMeshPageSegmentRef = PagePackingSegmentRef;

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

	struct VoxelMeshPageBuildTotals
	{
		uint32_t clusterCount = 0;
		uint32_t cubeCount = 0;
		uint32_t attributeCount = 0;
	};

	struct VoxelPageHeaderFields
	{
		uint32_t magic = 0;
		uint32_t firstCluster = 0;
		uint32_t clusterCount = 0;
		uint32_t firstCube = 0;
		uint32_t cubeCount = 0;
		uint32_t reservedPage0 = 0;
		uint32_t reserved0 = 0;
		uint32_t reserved1 = 0;
		uint32_t clusterRecordsOffset = 0;
		uint32_t cubeRecordsOffset = 0;
		uint32_t attributeSamplesOffset = 0;
		uint32_t attributeSamplesPerCube = 0;
		uint32_t clusterRecordStride = 0;
		uint32_t cubeRecordStride = 0;
		uint32_t attributeSampleStride = 0;
		uint32_t reserved2 = 0;
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

	bool ReadVoxelPageHeader(const std::vector<std::byte>& blob, VoxelPageHeaderFields& outHeader)
	{
		if (blob.size() < CLOD_VOXEL_PAGE_HEADER_SIZE || !IsVoxelPageBlob(blob))
		{
			return false;
		}

		std::array<uint32_t, 16> words{};
		std::memcpy(words.data(), blob.data(), words.size() * sizeof(uint32_t));
		outHeader.magic = words[0];
		outHeader.firstCluster = words[1];
		outHeader.clusterCount = words[2];
		outHeader.firstCube = words[3];
		outHeader.cubeCount = words[4];
		outHeader.reservedPage0 = words[5];
		outHeader.reserved0 = words[6];
		outHeader.reserved1 = words[7];
		outHeader.clusterRecordsOffset = words[8];
		outHeader.cubeRecordsOffset = words[9];
		outHeader.attributeSamplesOffset = words[10];
		outHeader.attributeSamplesPerCube = words[11];
		outHeader.clusterRecordStride = words[12];
		outHeader.cubeRecordStride = words[13];
		outHeader.attributeSampleStride = words[14];
		outHeader.reserved2 = words[15];
		return outHeader.magic == CLOD_VOXEL_PAGE_MAGIC &&
			outHeader.clusterRecordsOffset != 0u &&
			outHeader.cubeRecordsOffset != 0u &&
			outHeader.attributeSamplesOffset != 0u &&
			outHeader.attributeSamplesPerCube == CLOD_VOXEL_ATTRIBUTE_SAMPLES_COMPACT &&
			outHeader.clusterRecordStride == sizeof(CLodVoxelClusterRecord) &&
			outHeader.cubeRecordStride == sizeof(CLodVoxelCubeRecord) &&
			outHeader.attributeSampleStride == sizeof(CLodVoxelAttributeSample);
	}

	bool ReadVoxelClusterRecord(
		const std::vector<std::byte>& blob,
		const VoxelPageHeaderFields& header,
		uint32_t clusterIndex,
		CLodVoxelClusterRecord& outRecord)
	{
		if (clusterIndex >= header.clusterCount)
		{
			return false;
		}
		return ReadPodAt(
			blob,
			static_cast<size_t>(header.clusterRecordsOffset) + static_cast<size_t>(clusterIndex) * header.clusterRecordStride,
			outRecord);
	}

	bool ReadVoxelCubeRecord(
		const std::vector<std::byte>& blob,
		const VoxelPageHeaderFields& header,
		uint32_t cubeIndex,
		CLodVoxelCubeRecord& outRecord)
	{
		if (cubeIndex >= header.cubeCount)
		{
			return false;
		}
		return ReadPodAt(
			blob,
			static_cast<size_t>(header.cubeRecordsOffset) + static_cast<size_t>(cubeIndex) * header.cubeRecordStride,
			outRecord);
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

	VoxelMeshPageBuildTotals ComputeVoxelMeshPageTotals(
		const ClusterLODBuildState& state,
		std::span<const PagePackingSegmentRef> segments)
	{
		VoxelMeshPageBuildTotals totals{};
		for (const PagePackingSegmentRef& segment : segments)
		{
			if (segment.groupIndex >= state.groupPageBlobs.size() ||
				segment.sourcePageIndex >= state.groupPageBlobs[segment.groupIndex].size())
			{
				continue;
			}

			const std::vector<std::byte>& sourceBlob = state.groupPageBlobs[segment.groupIndex][segment.sourcePageIndex];
			VoxelPageHeaderFields sourceHeader{};
			if (!ReadVoxelPageHeader(sourceBlob, sourceHeader) ||
				segment.firstMeshletInPage + segment.meshletCount > sourceHeader.clusterCount)
			{
				continue;
			}

			for (uint32_t localCluster = 0; localCluster < segment.meshletCount; ++localCluster)
			{
				CLodVoxelClusterRecord cluster{};
				if (!ReadVoxelClusterRecord(sourceBlob, sourceHeader, segment.firstMeshletInPage + localCluster, cluster))
				{
					continue;
				}
				totals.clusterCount++;
				totals.cubeCount += cluster.cubeCount;
				for (uint32_t cubeOffset = 0; cubeOffset < cluster.cubeCount; ++cubeOffset)
				{
					CLodVoxelCubeRecord cube{};
					if (ReadVoxelCubeRecord(sourceBlob, sourceHeader, cluster.firstCube + cubeOffset, cube))
					{
						totals.attributeCount += CountVoxelAttributeSamples(cube.occupancyMask);
					}
				}
			}
		}
		return totals;
	}

	std::vector<std::byte> BuildPackedVoxelMeshPageBlob(
		const ClusterLODBuildState& state,
		std::span<const PagePackingSegmentRef> segments)
	{
		auto align4 = [](size_t value) -> size_t { return (value + 3u) & ~size_t(3); };
		const VoxelMeshPageBuildTotals totals = ComputeVoxelMeshPageTotals(state, segments);
		if (totals.clusterCount == 0u || totals.cubeCount == 0u)
		{
			return {};
		}

		const uint32_t clusterRecordOffset = CLOD_VOXEL_PAGE_HEADER_SIZE;
		const uint32_t cubeRecordOffset = static_cast<uint32_t>(align4(
			static_cast<size_t>(clusterRecordOffset) +
			static_cast<size_t>(totals.clusterCount) * sizeof(CLodVoxelClusterRecord)));
		const uint32_t attributeOffset = cubeRecordOffset + totals.cubeCount * static_cast<uint32_t>(sizeof(CLodVoxelCubeRecord));
		const size_t pageSize = static_cast<size_t>(attributeOffset) +
			static_cast<size_t>(totals.attributeCount) * sizeof(CLodVoxelAttributeSample);
		if (pageSize > CLOD_STREAMING_PAGE_SIZE_BYTES)
		{
			return {};
		}

		std::vector<std::byte> blob(pageSize, std::byte{ 0 });
		const std::array<uint32_t, 16> header = {
			CLOD_VOXEL_PAGE_MAGIC,
			0u,
			totals.clusterCount,
			0u,
			totals.cubeCount,
			0u,
			0u,
			0u,
			clusterRecordOffset,
			cubeRecordOffset,
			attributeOffset,
			CLOD_VOXEL_ATTRIBUTE_SAMPLES_COMPACT,
			static_cast<uint32_t>(sizeof(CLodVoxelClusterRecord)),
			static_cast<uint32_t>(sizeof(CLodVoxelCubeRecord)),
			static_cast<uint32_t>(sizeof(CLodVoxelAttributeSample)),
			0u
		};
		std::memcpy(blob.data(), header.data(), header.size() * sizeof(uint32_t));

		uint32_t outputClusterIndex = 0u;
		uint32_t outputCubeIndex = 0u;
		uint32_t outputAttributeIndex = 0u;
		for (const PagePackingSegmentRef& segment : segments)
		{
			if (segment.groupIndex >= state.groupPageBlobs.size() ||
				segment.sourcePageIndex >= state.groupPageBlobs[segment.groupIndex].size())
			{
				continue;
			}

			const std::vector<std::byte>& sourceBlob = state.groupPageBlobs[segment.groupIndex][segment.sourcePageIndex];
			VoxelPageHeaderFields sourceHeader{};
			if (!ReadVoxelPageHeader(sourceBlob, sourceHeader) ||
				segment.firstMeshletInPage + segment.meshletCount > sourceHeader.clusterCount)
			{
				continue;
			}

			for (uint32_t localCluster = 0; localCluster < segment.meshletCount; ++localCluster)
			{
				CLodVoxelClusterRecord sourceCluster{};
				if (!ReadVoxelClusterRecord(sourceBlob, sourceHeader, segment.firstMeshletInPage + localCluster, sourceCluster))
				{
					continue;
				}

				const uint32_t outputFirstCube = outputCubeIndex;
				CLodVoxelClusterRecord outputCluster = sourceCluster;
				outputCluster.firstCube = outputFirstCube;
				StorePod(blob, clusterRecordOffset + outputClusterIndex * sizeof(CLodVoxelClusterRecord), outputCluster);
				outputClusterIndex++;

				for (uint32_t cubeOffset = 0; cubeOffset < sourceCluster.cubeCount; ++cubeOffset)
				{
					CLodVoxelCubeRecord sourceCube{};
					if (!ReadVoxelCubeRecord(sourceBlob, sourceHeader, sourceCluster.firstCube + cubeOffset, sourceCube))
					{
						continue;
					}

					const uint32_t outputFirstAttribute = outputAttributeIndex;
					const uint32_t attributeCount = CountVoxelAttributeSamples(sourceCube.occupancyMask);
					CLodVoxelCubeRecord outputCube = sourceCube;
					outputCube.firstAttribute = outputFirstAttribute;
					StorePod(blob, cubeRecordOffset + outputCubeIndex * sizeof(CLodVoxelCubeRecord), outputCube);

					const size_t sourceAttributeOffset =
						static_cast<size_t>(sourceHeader.attributeSamplesOffset) +
						static_cast<size_t>(sourceCube.firstAttribute) * sourceHeader.attributeSampleStride;
					const size_t destAttributeOffset =
						static_cast<size_t>(attributeOffset) +
						static_cast<size_t>(outputFirstAttribute) * sizeof(CLodVoxelAttributeSample);
					const size_t attributeBytes =
						static_cast<size_t>(attributeCount) * sizeof(CLodVoxelAttributeSample);
					if (sourceAttributeOffset + attributeBytes <= sourceBlob.size() &&
						destAttributeOffset + attributeBytes <= blob.size())
					{
						std::memcpy(blob.data() + destAttributeOffset, sourceBlob.data() + sourceAttributeOffset, attributeBytes);
					}

					outputCubeIndex++;
					outputAttributeIndex += attributeCount;
				}
			}
		}

		return blob;
	}

	template <class PageTraits, class ReadTraitsFn, class MergeTraitsFn, class ComputeSizeFn, class BuildPageFn>
	void FinalizeRepresentationPagePacking(
		ClusterLODBuildState& state,
		std::span<const uint32_t> groupOrder,
		bool packVoxelGroups,
		std::vector<std::vector<std::byte>>& outMeshPageBlobs,
		std::vector<std::vector<uint32_t>>& groupReferencedPages,
		ReadTraitsFn readTraits,
		MergeTraitsFn mergeTraits,
		ComputeSizeFn computeSize,
		BuildPageFn buildPage)
	{
		std::vector<PagePackingSegmentRef> currentPage;
		PageTraits currentTraits{};

		auto flushPage = [&]()
		{
			if (currentPage.empty())
			{
				return;
			}

			std::vector<std::byte> pageBlob = buildPage(
				state,
				std::span<const PagePackingSegmentRef>(currentPage.data(), currentPage.size()),
				currentTraits);
			if (pageBlob.empty())
			{
				currentPage.clear();
				currentTraits = {};
				return;
			}

			const uint32_t meshPageIndex = static_cast<uint32_t>(outMeshPageBlobs.size());
			uint32_t pageLocalMeshlet = 0u;
			for (const PagePackingSegmentRef& segment : currentPage)
			{
				if (segment.segmentIndex < state.segments.size())
				{
					ClusterLODGroupSegment& outSegment = state.segments[segment.segmentIndex];
					outSegment.pageIndex = meshPageIndex;
					outSegment.firstMeshletInPage = pageLocalMeshlet;
					if (segment.groupIndex < groupReferencedPages.size())
					{
						groupReferencedPages[segment.groupIndex].push_back(meshPageIndex);
					}
				}
				pageLocalMeshlet += segment.meshletCount;
			}

			outMeshPageBlobs.push_back(std::move(pageBlob));
			currentPage.clear();
			currentTraits = {};
		};

		for (uint32_t groupIndex : groupOrder)
		{
			if (groupIndex >= state.groups.size())
			{
				continue;
			}
			const ClusterLODGroup& group = state.groups[groupIndex];
			const bool isVoxelGroup = (group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u;
			if (isVoxelGroup != packVoxelGroups)
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
				PageTraits sourceTraits{};
				if (!readTraits(sourceBlob, sourceTraits))
				{
					continue;
				}

				PagePackingSegmentRef candidate{};
				candidate.groupIndex = groupIndex;
				candidate.segmentIndex = segmentIndex;
				candidate.sourcePageIndex = segment.pageIndex;
				candidate.firstMeshletInPage = segment.firstMeshletInPage;
				candidate.meshletCount = segment.meshletCount;

				std::vector<PagePackingSegmentRef> candidatePage = currentPage;
				candidatePage.push_back(candidate);
				PageTraits candidateTraits = currentPage.empty() ? sourceTraits : currentTraits;
				if (!currentPage.empty())
				{
					mergeTraits(candidateTraits, sourceTraits);
				}

				const size_t candidateSize = computeSize(
					state,
					std::span<const PagePackingSegmentRef>(candidatePage.data(), candidatePage.size()),
					candidateTraits);
				if (candidateSize > CLOD_STREAMING_PAGE_SIZE_BYTES && !currentPage.empty())
				{
					flushPage();
					candidatePage.clear();
					candidatePage.push_back(candidate);
					candidateTraits = sourceTraits;
				}

				currentPage.push_back(candidate);
				currentTraits = currentPage.size() == 1u ? sourceTraits : currentTraits;
				if (currentPage.size() != 1u)
				{
					mergeTraits(currentTraits, sourceTraits);
				}
			}
		}

		flushPage();
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

		std::vector<std::vector<uint32_t>> groupReferencedPages(state.groups.size());

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

		struct TrianglePageTraits
		{
			uint32_t attributeMask = 0u;
			uint32_t uvSetCount = 0u;
		};
		FinalizeRepresentationPagePacking<TrianglePageTraits>(
			state,
			std::span<const uint32_t>(groupOrder.data(), groupOrder.size()),
			false,
			outMeshPageBlobs,
			groupReferencedPages,
			[](const std::vector<std::byte>& sourceBlob, TrianglePageTraits& outTraits) -> bool
			{
				CLodPageHeader sourceHeader{};
				if (!ReadTrianglePageHeader(sourceBlob, sourceHeader))
				{
					return false;
				}
				outTraits.attributeMask = sourceHeader.attributeMask;
				outTraits.uvSetCount = sourceHeader.uvSetCount;
				return true;
			},
			[](TrianglePageTraits& target, const TrianglePageTraits& source)
			{
				target.attributeMask |= source.attributeMask;
				target.uvSetCount = std::max(target.uvSetCount, source.uvSetCount);
			},
			[](const ClusterLODBuildState& packState, std::span<const PagePackingSegmentRef> segments, const TrianglePageTraits& traits) -> size_t
			{
				const TriangleMeshPageBuildTotals candidateTotals = ComputeTriangleMeshPageTotals(
					packState,
					segments,
					traits.attributeMask,
					traits.uvSetCount);
				return ComputePageBlobSize(
					traits.attributeMask,
					candidateTotals.meshletCount,
					traits.uvSetCount,
					candidateTotals.totalPositionBytes,
					candidateTotals.totalUvBitsPerSet,
					candidateTotals.totalVertexCount,
					candidateTotals.totalNormalWords,
					candidateTotals.totalTangentFrameWords,
					candidateTotals.totalColorWords,
					candidateTotals.totalBoneIndexCount,
					candidateTotals.totalTriangleBytes);
			},
			[](const ClusterLODBuildState& packState, std::span<const PagePackingSegmentRef> segments, const TrianglePageTraits& traits) -> std::vector<std::byte>
			{
				return BuildPackedTriangleMeshPageBlob(packState, segments, traits.attributeMask, traits.uvSetCount);
			});

		outTrianglePageCount = static_cast<uint32_t>(outMeshPageBlobs.size());
		outVoxelPageBase = outTrianglePageCount;

		struct VoxelPageTraits
		{
			uint32_t unused = 0u;
		};
		FinalizeRepresentationPagePacking<VoxelPageTraits>(
			state,
			std::span<const uint32_t>(groupOrder.data(), groupOrder.size()),
			true,
			outMeshPageBlobs,
			groupReferencedPages,
			[](const std::vector<std::byte>& sourceBlob, VoxelPageTraits& outTraits) -> bool
			{
				(void)outTraits;
				VoxelPageHeaderFields sourceHeader{};
				return ReadVoxelPageHeader(sourceBlob, sourceHeader);
			},
			[](VoxelPageTraits& target, const VoxelPageTraits& source)
			{
				(void)target;
				(void)source;
			},
			[](const ClusterLODBuildState& packState, std::span<const PagePackingSegmentRef> segments, const VoxelPageTraits&) -> size_t
			{
				const VoxelMeshPageBuildTotals totals = ComputeVoxelMeshPageTotals(packState, segments);
				return ComputeVoxelPageSizeBytes(totals.clusterCount, totals.cubeCount, totals.attributeCount);
			},
			[](const ClusterLODBuildState& packState, std::span<const PagePackingSegmentRef> segments, const VoxelPageTraits&) -> std::vector<std::byte>
			{
				return BuildPackedVoxelMeshPageBlob(packState, segments);
			});

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

	void ReleaseVoxelGroupPayloadStorage(VoxelGroupPayload& payload)
	{
		payload.resolution = 0u;
		payload.aabbMin = {};
		payload.aabbMax = {};
		payload.voxelWidth = 0.0f;
		std::vector<VoxelCell>().swap(payload.activeCells);
	}

	uint64_t CountLiveCarryPayloadCells(const ClusterLODBuildState& state)
	{
		uint64_t liveCells = 0u;
		for (const VoxelGroupPayload& payload : state.voxelCarryPayloads)
		{
			liveCells += payload.activeCells.size();
		}
		return liveCells;
	}

	bool HasVoxelSourcePayloadForGroup(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		if (groupIndex < state.voxelCarryPayloads.size() &&
			state.voxelCarryPayloads[groupIndex].voxelWidth > 0.0f &&
			!state.voxelCarryPayloads[groupIndex].activeCells.empty())
		{
			return true;
		}
		return GetVoxelRenderPayloadForGroup(state, groupIndex) != nullptr;
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
			const uint32_t carryCellCount = static_cast<uint32_t>(std::min<size_t>(
				carryPayload->activeCells.size(),
				std::numeric_limits<uint32_t>::max()));
			outPayloads.push_back(VoxelSourcePayloadRef{ carryPayload, carryCellCount, GetVoxelCandidateExpansionRadiusForPayload(carryPayload) });
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
		if (groupIndex >= state.voxelGroupMapping.groupToPackedMetadataIndex.size())
		{
			return 0u;
		}

		const int32_t metadataIndex = state.voxelGroupMapping.groupToPackedMetadataIndex[groupIndex];
		if (metadataIndex < 0 || static_cast<size_t>(metadataIndex) >= state.voxelGroupMapping.packedGroupMetadata.size())
		{
			return 0u;
		}

		return state.voxelGroupMapping.packedGroupMetadata[static_cast<size_t>(metadataIndex)].cubeCount;
	}

	uint32_t GetVoxelSourcePrimitiveCountForGroup(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		uint32_t sourceCellCount = 0u;
		std::vector<VoxelSourcePayloadRef> sourcePayloadRefs;
		AppendVoxelSourcePayloadRefsForGroup(state, groupIndex, sourcePayloadRefs);
		for (const VoxelSourcePayloadRef& payloadRef : sourcePayloadRefs)
		{
			sourceCellCount += std::min(
				payloadRef.budgetCellCount,
				std::numeric_limits<uint32_t>::max() - sourceCellCount);
		}

		if (sourceCellCount != 0u)
		{
			return sourceCellCount;
		}

		const VoxelGroupPayload* renderPayload = GetVoxelRenderPayloadForGroup(state, groupIndex);
		if (renderPayload != nullptr && !renderPayload->activeCells.empty())
		{
			return static_cast<uint32_t>(std::min<size_t>(
				renderPayload->activeCells.size(),
				std::numeric_limits<uint32_t>::max()));
		}

		return GetVoxelPackedCubeCountForGroup(state, groupIndex);
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

			if (HasVoxelSourcePayloadForGroup(state, childGroupIndex))
			{
				const uint32_t voxelPrimitiveCount = GetVoxelSourcePrimitiveCountForGroup(state, childGroupIndex);
				if (voxelPrimitiveCount != 0u)
				{
					sourcePrimitiveCount += voxelPrimitiveCount;
					continue;
				}
			}

			sourcePrimitiveCount += CountGroupMeshTriangles(state, childGroupIndex);
		}

		return static_cast<uint32_t>(std::min<uint64_t>(sourcePrimitiveCount, std::numeric_limits<uint32_t>::max()));
	}

	uint32_t GetVoxelPackedClusterCountForGroup(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		if (groupIndex >= state.voxelGroupMapping.groupToPackedMetadataIndex.size())
		{
			return 0u;
		}

		const int32_t metadataIndex = state.voxelGroupMapping.groupToPackedMetadataIndex[groupIndex];
		if (metadataIndex < 0 || static_cast<size_t>(metadataIndex) >= state.voxelGroupMapping.packedGroupMetadata.size())
		{
			return 0u;
		}

		return state.voxelGroupMapping.packedGroupMetadata[static_cast<size_t>(metadataIndex)].clusterCount;
	}

	float GetVoxelMetadataErrorForGroup(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		if (groupIndex >= state.voxelGroupMapping.groupToPackedMetadataIndex.size())
		{
			return 0.0f;
		}

		const int32_t metadataIndex = state.voxelGroupMapping.groupToPackedMetadataIndex[groupIndex];
		if (metadataIndex < 0 || static_cast<size_t>(metadataIndex) >= state.voxelGroupMapping.packedGroupMetadata.size())
		{
			return 0.0f;
		}

		return state.voxelGroupMapping.packedGroupMetadata[static_cast<size_t>(metadataIndex)].aabbMaxAndError.w;
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

	float ComputeVoxelRepresentationError(float voxelWidth)
	{
		if (!std::isfinite(voxelWidth) || voxelWidth <= 0.0f)
		{
			return 0.0f;
		}

		return voxelWidth;
	}

	uint64_t PackVoxelTailCellKey(uint32_t x, uint32_t y, uint32_t z)
	{
		return uint64_t{ x } | (uint64_t{ y } << 21u) | (uint64_t{ z } << 42u);
	}

	VoxelGroupPayload DownsampleVoxelPayloadDirect(
		const VoxelGroupPayload& sourcePayload,
		float voxelWidth,
		uint32_t resolution,
		const DirectX::XMFLOAT3& aabbMin,
		int32_t refinedGroup)
	{
		VoxelGroupPayload result{};
		if (sourcePayload.activeCells.empty() ||
			sourcePayload.voxelWidth <= 0.0f ||
			voxelWidth <= 0.0f ||
			resolution < 2u)
		{
			return result;
		}

		result.resolution = resolution;
		result.aabbMin = aabbMin;
		result.aabbMax = DirectX::XMFLOAT3(
			aabbMin.x + voxelWidth * static_cast<float>(resolution),
			aabbMin.y + voxelWidth * static_cast<float>(resolution),
			aabbMin.z + voxelWidth * static_cast<float>(resolution));
		result.voxelWidth = voxelWidth;

		struct DownsampleCellAccum
		{
			VoxelCell cell{};
			br::mesh::sggx::SymmetricMatrix3 sggxSum{};
			float sggxWeight = 0.0f;
		};

		std::unordered_map<uint64_t, DownsampleCellAccum> cells;
		cells.reserve(sourcePayload.activeCells.size());
		for (const VoxelCell& sourceCell : sourcePayload.activeCells)
		{
			const float centerX = sourcePayload.aabbMin.x + (static_cast<float>(sourceCell.x) + 0.5f) * sourcePayload.voxelWidth;
			const float centerY = sourcePayload.aabbMin.y + (static_cast<float>(sourceCell.y) + 0.5f) * sourcePayload.voxelWidth;
			const float centerZ = sourcePayload.aabbMin.z + (static_cast<float>(sourceCell.z) + 0.5f) * sourcePayload.voxelWidth;
			const auto cellCoord = [&](float value, float minValue) -> uint32_t
			{
				const float local = (value - minValue) / voxelWidth;
				const int32_t coord = static_cast<int32_t>(std::floor(local));
				return static_cast<uint32_t>(std::clamp<int32_t>(coord, 0, static_cast<int32_t>(resolution) - 1));
			};
			const uint32_t x = cellCoord(centerX, aabbMin.x);
			const uint32_t y = cellCoord(centerY, aabbMin.y);
			const uint32_t z = cellCoord(centerZ, aabbMin.z);
			const uint64_t key = PackVoxelTailCellKey(x, y, z);

			DownsampleCellAccum& accum = cells[key];
			VoxelCell& dst = accum.cell;
			const float sourceWeight = std::max(sourceCell.opacity, 1.0e-6f);
			accum.sggxSum = accum.sggxSum + br::mesh::sggx::DecodeAxialSGGX(sourceCell.sggxAxisAndSigmas) * sourceWeight;
			accum.sggxWeight += sourceWeight;

			if (dst.opacity <= sourceCell.opacity)
			{
				dst = sourceCell;
				dst.x = x;
				dst.y = y;
				dst.z = z;
				dst.refinedGroup = refinedGroup;
			}
			else
			{
				dst.opacity = std::min(1.0f, dst.opacity + sourceCell.opacity);
			}
		}

		result.activeCells.reserve(cells.size());
		for (auto& [key, accum] : cells)
		{
			if (accum.sggxWeight > 1.0e-12f)
			{
				const br::mesh::sggx::SymmetricMatrix3 sggx = accum.sggxSum * (1.0f / accum.sggxWeight);
				accum.cell.sggxAxisAndSigmas = br::mesh::sggx::EncodeAxialSGGX(br::mesh::sggx::CompressSGGXToAxial(sggx));
			}
			result.activeCells.push_back(accum.cell);
		}
		return result;
	}

	struct AppendedVoxelGroupResult
	{
		uint32_t groupIndex = std::numeric_limits<uint32_t>::max();
		uint32_t cubeCount = 0u;
		uint32_t clusterCount = 0u;
		VoxelGroupPayload renderPayload;
	};

	bool AppendPackedVoxelGroupToBuildState(
		ClusterLODBuildState& state,
		VoxelGroupPayload payload,
		float voxelRepresentationError,
		int32_t depth,
		uint32_t flags,
		uint32_t refinedChildGroup,
		const ClusterLODBuilderSettings& settings,
		AppendedVoxelGroupResult& outResult)
	{
		outResult = {};
		outResult.groupIndex = std::numeric_limits<uint32_t>::max();
		if (payload.activeCells.empty() || payload.resolution < 2u || payload.voxelWidth <= 0.0f)
		{
			return false;
		}

		const uint32_t firstCluster = static_cast<uint32_t>(state.voxelGroupMapping.packedClusterRecords.size());
		const uint32_t firstCube = static_cast<uint32_t>(state.voxelGroupMapping.packedCubeRecords.size());
		const uint32_t firstAttribute = static_cast<uint32_t>(state.voxelGroupMapping.packedAttributeSamples.size());
		PackVoxelGroupInput packInput{};
		packInput.payload = &payload;
		packInput.voxelError = voxelRepresentationError;
		packInput.opacityThreshold = settings.voxelFallbackOpacityThreshold;
		packInput.dominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
		packInput.firstCube = firstCube;
		packInput.firstAttribute = firstAttribute;
		PackedVoxelGroupBuildResult packed = PackVoxelGroupToCubes(packInput);
		packed.metadata.firstCluster = firstCluster;
		BuildVoxelClustersFromCubes(packed, CLOD_VOXEL_MAX_CUBES_PER_CLUSTER);
		if (packed.cubeRecords.empty() || packed.clusterRecords.empty())
		{
			return false;
		}

		std::vector<ClusterLODGroupSegment> voxelSegments;
		std::vector<BoundingSphere> voxelSegmentBounds;
		SplitVoxelClustersIntoPageSegments(packed, voxelSegments, voxelSegmentBounds);
		std::vector<std::vector<std::byte>> voxelPageBlobs = BuildVoxelGroupPageBlobs(
			voxelSegments,
			packed.clusterRecords,
			packed.cubeRecords,
			packed.attributeSamples,
			firstAttribute);
		if (voxelSegments.empty() || voxelPageBlobs.empty())
		{
			return false;
		}

		ClusterLODGroup group{};
		group.bounds.center[0] = 0.5f * (payload.aabbMin.x + payload.aabbMax.x);
		group.bounds.center[1] = 0.5f * (payload.aabbMin.y + payload.aabbMax.y);
		group.bounds.center[2] = 0.5f * (payload.aabbMin.z + payload.aabbMax.z);
		const float dx = payload.aabbMax.x - group.bounds.center[0];
		const float dy = payload.aabbMax.y - group.bounds.center[1];
		const float dz = payload.aabbMax.z - group.bounds.center[2];
		group.bounds.radius = std::sqrt(dx * dx + dy * dy + dz * dz);
		group.bounds.error = std::numeric_limits<float>::max();
		group.depth = depth;
		group.flags = flags;
		group.firstSegment = static_cast<uint32_t>(state.segments.size());
		group.segmentCount = static_cast<uint32_t>(voxelSegments.size());
		group.terminalSegmentCount = 0u;
		group.pageCount = static_cast<uint32_t>(voxelPageBlobs.size());
		group.representationError = voxelRepresentationError;
		for (ClusterLODGroupSegment& segment : voxelSegments)
		{
			segment.refinedGroup = static_cast<int32_t>(refinedChildGroup);
		}

		const uint32_t groupIndex = static_cast<uint32_t>(state.groups.size());
		state.groups.push_back(group);
		state.groupChunks.emplace_back();
		state.groupPageBlobs.push_back(std::move(voxelPageBlobs));
		state.segments.insert(state.segments.end(), voxelSegments.begin(), voxelSegments.end());
		state.segmentBounds.insert(state.segmentBounds.end(), voxelSegmentBounds.begin(), voxelSegmentBounds.end());
		state.voxelCarryPayloads.emplace_back();
		state.voxelGroupMapping.groupToPayloadIndex.push_back(-1);
		state.voxelGroupMapping.groupToPackedMetadataIndex.push_back(static_cast<int32_t>(state.voxelGroupMapping.packedGroupMetadata.size()));
		state.voxelGroupMapping.packedGroupMetadata.push_back(packed.metadata);
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
		state.groupVertexChunks.emplace_back();
		state.groupSkinningChunks.emplace_back();
		state.groupMeshletVertexChunks.emplace_back();
		state.groupMeshletChunks.emplace_back();
		state.groupMeshletTriangleChunks.emplace_back();
		state.groupMeshletRefinedGroupChunks.emplace_back();
		if (!state.traversalGroupMask.empty())
		{
			state.traversalGroupMask.push_back(1u);
		}
		if (refinedChildGroup < state.groups.size())
		{
			state.groups[refinedChildGroup].parentGroupId = static_cast<int32_t>(groupIndex);
		}

		outResult.groupIndex = groupIndex;
		outResult.cubeCount = static_cast<uint32_t>(packed.cubeRecords.size());
		outResult.clusterCount = static_cast<uint32_t>(packed.clusterRecords.size());
		outResult.renderPayload = std::move(payload);
		return true;
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

		const float metadataError = GetVoxelMetadataErrorForGroup(state, groupIndex);
		if (std::isfinite(metadataError) && metadataError > 0.0f)
		{
			return metadataError;
		}

		const float groupError = state.groups[groupIndex].bounds.error;
		if (std::isfinite(groupError) && groupError > 0.0f && groupError < std::numeric_limits<float>::max() * 0.5f)
		{
			return groupError;
		}

		std::vector<VoxelSourcePayloadRef> sourcePayloadRefs;
		AppendVoxelSourcePayloadRefsForGroup(state, groupIndex, sourcePayloadRefs);
		float maxVoxelWidth = 0.0f;
		for (const VoxelSourcePayloadRef& payloadRef : sourcePayloadRefs)
		{
			if (payloadRef.payload != nullptr)
			{
				maxVoxelWidth = std::max(maxVoxelWidth, payloadRef.payload->voxelWidth);
			}
		}
		return maxVoxelWidth;
	}

	std::vector<uint32_t> CollectUniqueRefinedChildren(const ClusterLODBuildState& state, uint32_t groupIndex);

	struct RefinedChildErrorRange
	{
		float minError = std::numeric_limits<float>::max();
		float maxError = 0.0f;
		uint32_t count = 0u;
	};

	RefinedChildErrorRange GetRefinedChildTraversalErrorRange(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		RefinedChildErrorRange range{};
		for (uint32_t childGroupIndex : CollectUniqueRefinedChildren(state, groupIndex))
		{
			if (childGroupIndex < state.groups.size())
			{
				const float childError = state.groups[childGroupIndex].bounds.error;
				if (std::isfinite(childError) && childError > 0.0f && childError < 5.0e19f)
				{
					range.minError = std::min(range.minError, childError);
					range.maxError = std::max(range.maxError, childError);
					range.count++;
				}
			}
		}

		if (range.count == 0u)
		{
			range.minError = 0.0f;
		}
		return range;
	}

	float GetMaxRefinedChildTraversalError(const ClusterLODBuildState& state, uint32_t groupIndex, uint32_t* outCount = nullptr)
	{
		const RefinedChildErrorRange range = GetRefinedChildTraversalErrorRange(state, groupIndex);
		if (outCount != nullptr)
		{
			*outCount = range.count;
		}
		return range.maxError;
	}

	bool IsTerminalErrorSentinel(float error)
	{
		return error >= std::numeric_limits<float>::max() * 0.5f;
	}

	constexpr float kClusterLODStructuralTraversalError = 1.0e20f;

	bool IsStructuralTraversalError(float error)
	{
		return std::isfinite(error) &&
			error >= kClusterLODStructuralTraversalError * 0.5f &&
			!IsTerminalErrorSentinel(error);
	}

	bool IsFiniteContentTraversalError(float error)
	{
		return std::isfinite(error) &&
			error > 0.0f &&
			!IsTerminalErrorSentinel(error) &&
			!IsStructuralTraversalError(error);
	}

	float TraversalNodeErrorFromGroupError(float error)
	{
		return IsTerminalErrorSentinel(error) ? kClusterLODStructuralTraversalError : error;
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

	uint64_t CountGroupSegmentTriangles(
		const ClusterLODBuildState& state,
		uint32_t groupIndex,
		const ClusterLODGroupSegment& segment)
	{
		if (groupIndex >= state.groups.size() || groupIndex >= state.groupMeshletChunks.size())
		{
			return 0u;
		}

		const ClusterLODGroup& group = state.groups[groupIndex];
		const uint32_t firstMeshlet = ComputeGroupSegmentFirstMeshlet(state, group, segment);
		const uint32_t endMeshlet = std::min<uint32_t>(
			static_cast<uint32_t>(state.groupMeshletChunks[groupIndex].size()),
			firstMeshlet + segment.meshletCount);
		uint64_t triangleCount = 0u;
		for (uint32_t meshletIndex = firstMeshlet; meshletIndex < endMeshlet; ++meshletIndex)
		{
			triangleCount += state.groupMeshletChunks[groupIndex][meshletIndex].triangle_count;
		}
		return triangleCount;
	}

	uint64_t CountTerminalTriangleSourceGeometry(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		if (groupIndex >= state.groups.size() || groupIndex >= state.groupMeshletChunks.size())
		{
			return 0u;
		}

		if (const std::vector<int32_t>* meshletRefinedGroups = GetGroupMeshletRefinedGroups(state, groupIndex))
		{
			uint64_t triangleCount = 0u;
			const uint32_t meshletCount = std::min<uint32_t>(
				static_cast<uint32_t>(meshletRefinedGroups->size()),
				static_cast<uint32_t>(state.groupMeshletChunks[groupIndex].size()));
			for (uint32_t meshletIndex = 0u; meshletIndex < meshletCount; ++meshletIndex)
			{
				if ((*meshletRefinedGroups)[meshletIndex] < 0)
				{
					triangleCount += state.groupMeshletChunks[groupIndex][meshletIndex].triangle_count;
				}
			}
			return triangleCount;
		}

		const ClusterLODGroup& group = state.groups[groupIndex];
		if (group.firstSegment + group.segmentCount > state.segments.size())
		{
			return 0u;
		}

		uint64_t triangleCount = 0u;
		for (uint32_t segmentOffset = 0u; segmentOffset < group.segmentCount; ++segmentOffset)
		{
			const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
			if (segment.refinedGroup < 0)
			{
				triangleCount += CountGroupSegmentTriangles(state, groupIndex, segment);
			}
		}
		return triangleCount;
	}

	uint64_t CountDescendantTriangleSourceGeometry(
		const ClusterLODBuildState& state,
		uint32_t groupIndex,
		std::unordered_set<uint32_t>& visitedGroups)
	{
		if (groupIndex >= state.groups.size() || !visitedGroups.insert(groupIndex).second)
		{
			return 0u;
		}

		const std::vector<uint32_t> refinedChildren = CollectUniqueRefinedChildren(state, groupIndex);
		uint64_t triangleCount = refinedChildren.empty()
			? CountGroupMeshTriangles(state, groupIndex)
			: CountTerminalTriangleSourceGeometry(state, groupIndex);
		for (uint32_t childGroupIndex : refinedChildren)
		{
			triangleCount += CountDescendantTriangleSourceGeometry(state, childGroupIndex, visitedGroups);
		}

		visitedGroups.erase(groupIndex);
		return triangleCount;
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
		VoxelFallbackGroupBuildInput& buildInput,
		const std::vector<uint8_t>* requiredVoxelSourceMask = nullptr)
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

		// Voxel sections preserve the same DAG cut contract as triangle sections:
		// each emitted section is tagged by the immediate refined child. Built
		// child voxel payloads are kept alive until the fallback pass completes,
		// so this triangle fallback should only be used for non-voxelized paths.
		for (uint32_t childGroupIndex : refinedChildren)
		{
			if (HasVoxelSourcePayloadForGroup(state, childGroupIndex))
			{
				buildInput.sourceVoxelGroupIndices.push_back(childGroupIndex);
				continue;
			}

			if (requiredVoxelSourceMask != nullptr &&
				childGroupIndex < requiredVoxelSourceMask->size() &&
				(*requiredVoxelSourceMask)[childGroupIndex] != 0u)
			{
				const ClusterLODGroup& childGroup = state.groups[childGroupIndex];
				const uint64_t carryCells = childGroupIndex < state.voxelCarryPayloads.size()
					? static_cast<uint64_t>(state.voxelCarryPayloads[childGroupIndex].activeCells.size())
					: 0u;
				const VoxelGroupPayload* renderPayload = GetVoxelRenderPayloadForGroup(state, childGroupIndex);
				spdlog::error(
					"ClusterLOD voxel source missing required child payload: parent={} parent_depth={} child={} child_depth={} child_flags=0x{:X} child_segments={} child_terminal_segments={} carry_cells={} render_cells={} build_required=1",
					groupIndex,
					std::max(state.groups[groupIndex].depth, 0),
					childGroupIndex,
					std::max(childGroup.depth, 0),
					childGroup.flags,
					childGroup.segmentCount,
					childGroup.terminalSegmentCount,
					carryCells,
					renderPayload != nullptr ? renderPayload->activeCells.size() : 0ull);
				return false;
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

		// Coverage pruning must be evaluated against the original descendant
		// source geometry whenever possible.  Re-sampling already-pruned voxel
		// opacity between levels compounds partial coverage loss, especially
		// when parent and child voxel grids have similar cell sizes but different
		// origins.  Candidate ownership can still come from child voxel payloads;
		// this BVH is transient and only answers coverage rays.
		for (uint32_t childGroupIndex : refinedChildren)
		{
			std::unordered_set<uint32_t> appendVisitedGroups;
			if (!AppendDescendantTriangleSourceGeometry(
				state,
				childGroupIndex,
				buildInput,
				appendVisitedGroups,
				vertexStrideBytes,
				static_cast<int32_t>(childGroupIndex)))
			{
				return false;
			}
		}

		return !buildInput.voxelVertices.empty() && !buildInput.voxelTriangleIndices.empty();
	}

	void CollectCoverageDomainGroups(
		const ClusterLODBuildState& state,
		uint32_t groupIndex,
		const std::vector<uint8_t>& buildVoxelGroupMask,
		std::unordered_set<uint32_t>& visitedGroups,
		std::vector<int32_t>& outDomainGroups)
	{
		if (groupIndex >= state.groups.size() || !visitedGroups.insert(groupIndex).second)
		{
			return;
		}

		if (groupIndex < buildVoxelGroupMask.size() && buildVoxelGroupMask[groupIndex] != 0u)
		{
			outDomainGroups.push_back(static_cast<int32_t>(groupIndex));
		}
		for (uint32_t childGroupIndex : CollectUniqueRefinedChildren(state, groupIndex))
		{
			CollectCoverageDomainGroups(state, childGroupIndex, buildVoxelGroupMask, visitedGroups, outDomainGroups);
		}
	}

	std::vector<std::vector<int32_t>> BuildVoxelCoverageDomainMap(
		const ClusterLODBuildState& state,
		const std::vector<uint8_t>& buildVoxelGroupMask)
	{
		std::vector<std::vector<int32_t>> domainMap(state.groups.size());
		for (uint32_t groupIndex = 0u; groupIndex < static_cast<uint32_t>(state.groups.size()); ++groupIndex)
		{
			if (groupIndex >= buildVoxelGroupMask.size() || buildVoxelGroupMask[groupIndex] == 0u)
			{
				continue;
			}

			std::unordered_set<uint32_t> visitedGroups;
			CollectCoverageDomainGroups(state, groupIndex, buildVoxelGroupMask, visitedGroups, domainMap[groupIndex]);
		}
		return domainMap;
	}

	bool ComputePayloadActiveCellAabb(
		const VoxelGroupPayload& payload,
		DirectX::XMFLOAT3& outMin,
		DirectX::XMFLOAT3& outMax)
	{
		if (payload.activeCells.empty() || payload.voxelWidth <= 0.0f)
		{
			return false;
		}

		outMin = DirectX::XMFLOAT3(
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max());
		outMax = DirectX::XMFLOAT3(
			-std::numeric_limits<float>::max(),
			-std::numeric_limits<float>::max(),
			-std::numeric_limits<float>::max());
		for (const VoxelCell& cell : payload.activeCells)
		{
			const float minX = payload.aabbMin.x + static_cast<float>(cell.x) * payload.voxelWidth;
			const float minY = payload.aabbMin.y + static_cast<float>(cell.y) * payload.voxelWidth;
			const float minZ = payload.aabbMin.z + static_cast<float>(cell.z) * payload.voxelWidth;
			const float maxX = minX + payload.voxelWidth;
			const float maxY = minY + payload.voxelWidth;
			const float maxZ = minZ + payload.voxelWidth;
			outMin.x = std::min(outMin.x, minX);
			outMin.y = std::min(outMin.y, minY);
			outMin.z = std::min(outMin.z, minZ);
			outMax.x = std::max(outMax.x, maxX);
			outMax.y = std::max(outMax.y, maxY);
			outMax.z = std::max(outMax.z, maxZ);
		}
		return
			std::isfinite(outMin.x) && std::isfinite(outMin.y) && std::isfinite(outMin.z) &&
			std::isfinite(outMax.x) && std::isfinite(outMax.y) && std::isfinite(outMax.z) &&
			outMax.x > outMin.x && outMax.y > outMin.y && outMax.z > outMin.z;
	}

	bool BuildPartVoxelTailLevel(
		ClusterLODBuildState& state,
		uint32_t childGroupIndex,
		const VoxelGroupPayload& sourcePayload,
		const VoxelSourceTriangleBVH* coverageSourceTriangles,
		const VoxelCoverageMaterialSampler* coverageMaterialSampler,
		const ClusterLODBuilderSettings& settings,
		AppendedVoxelGroupResult& outResult)
	{
		outResult = {};
		outResult.groupIndex = std::numeric_limits<uint32_t>::max();
		if (childGroupIndex >= state.groups.size() ||
			sourcePayload.activeCells.empty() ||
			sourcePayload.voxelWidth <= 0.0f)
		{
			return false;
		}

		const float growthFactor = std::max(1.01f, settings.voxelTailGrowthFactor);
		const float voxelWidth = sourcePayload.voxelWidth * growthFactor;
		DirectX::XMFLOAT3 aabbMin{};
		DirectX::XMFLOAT3 aabbMax{};
		if (!ComputePayloadActiveCellAabb(sourcePayload, aabbMin, aabbMax))
		{
			return false;
		}

		const float extentX = aabbMax.x - aabbMin.x;
		const float extentY = aabbMax.y - aabbMin.y;
		const float extentZ = aabbMax.z - aabbMin.z;
		const float longestExtent = std::max({ extentX, extentY, extentZ });
		if (!std::isfinite(longestExtent) || longestExtent <= 1.0e-8f)
		{
			return false;
		}

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

		const uint32_t resolution = std::max(
			std::max(2u, settings.voxelMinResolution),
			static_cast<uint32_t>(std::ceil(longestExtent / std::max(voxelWidth, 1.0e-8f))));
		if (resolution < 2u)
		{
			return false;
		}

		const int32_t refinedChildGroup = static_cast<int32_t>(childGroupIndex);
		VoxelGroupPayload payload{};
		if (coverageSourceTriangles != nullptr && coverageSourceTriangles->IsValid())
		{
			std::vector<VoxelSourcePayloadInstance> sourceInstances;
			sourceInstances.push_back(VoxelSourcePayloadInstance{
				.payload = &sourcePayload,
				.expansionRadius = GetVoxelCandidateExpansionRadiusForPayload(&sourcePayload),
				.refinedGroupOverride = refinedChildGroup });
			VoxelizeTrianglesInput voxelInput{};
			voxelInput.sourceVoxelPayloadInstances = &sourceInstances;
			voxelInput.candidateVoxelPayloadInstances = &sourceInstances;
			voxelInput.coverageSourceTriangles = coverageSourceTriangles;
			voxelInput.coverageMaterialSampler = coverageMaterialSampler;
			voxelInput.aabbMin = aabbMin;
			voxelInput.aabbMax = aabbMax;
			voxelInput.voxelWidth = voxelWidth;
			voxelInput.resolution = resolution;
			voxelInput.raysPerCell = std::max(1u, settings.voxelRaysPerCell);
			voxelInput.emitSourcePayload = false;
			voxelInput.emitRenderPayload = true;
			payload = std::move(VoxelizeTrianglesDetailed(voxelInput).renderPayload);
		}
		if (payload.activeCells.empty())
		{
			payload = DownsampleVoxelPayloadDirect(sourcePayload, voxelWidth, resolution, aabbMin, refinedChildGroup);
		}
		if (payload.activeCells.empty())
		{
			return false;
		}

		const int32_t tailDepth = std::max(state.groups[childGroupIndex].depth + 1, state.groups[childGroupIndex].depth);
		return AppendPackedVoxelGroupToBuildState(
			state,
			std::move(payload),
			ComputeVoxelRepresentationError(voxelWidth),
			tailDepth,
			CLOD_GROUP_FLAG_IS_VOXEL,
			childGroupIndex,
			settings,
			outResult);
	}

	void BuildPartVoxelTailGroups(
		ClusterLODBuildState& state,
		const VoxelSourceTriangleBVH* coverageSourceTriangles,
		const VoxelCoverageMaterialSampler* coverageMaterialSampler,
		const ClusterLODBuilderSettings& settings)
	{
		ZoneScopedN("ClusterLODUtilities::VoxelFallback::BuildPartVoxelTailGroups");
		if (settings.voxelTailMaxLevels == 0u || settings.voxelTailGrowthFactor <= 1.0f)
		{
			return;
		}

		std::vector<uint32_t> parentRefCounts(state.groups.size(), 0u);
		for (const ClusterLODGroup& group : state.groups)
		{
			if (group.firstSegment + group.segmentCount > state.segments.size())
			{
				continue;
			}
			for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
			{
				const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
				if (segment.refinedGroup >= 0 && static_cast<uint32_t>(segment.refinedGroup) < parentRefCounts.size())
				{
					parentRefCounts[static_cast<uint32_t>(segment.refinedGroup)]++;
				}
			}
		}

		std::vector<uint32_t> voxelRoots;
		for (uint32_t groupIndex = 0; groupIndex < static_cast<uint32_t>(state.groups.size()); ++groupIndex)
		{
			const ClusterLODGroup& group = state.groups[groupIndex];
			if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) == 0u ||
				(group.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) != 0u ||
				groupIndex >= parentRefCounts.size() ||
				parentRefCounts[groupIndex] != 0u ||
				GetVoxelPackedCubeCountForGroup(state, groupIndex) == 0u)
			{
				continue;
			}
			voxelRoots.push_back(groupIndex);
		}

		uint32_t generatedLevels = 0u;
		uint32_t generatedCubes = 0u;
		for (uint32_t rootGroupIndex : voxelRoots)
		{
			VoxelGroupPayload sourcePayload;
			if (!BuildVoxelGroupPayloadFromPackedMapping(state.voxelGroupMapping, rootGroupIndex, sourcePayload))
			{
				continue;
			}

			uint32_t childGroupIndex = rootGroupIndex;
			uint32_t previousCubeCount = GetVoxelPackedCubeCountForGroup(state, childGroupIndex);
			for (uint32_t level = 0; level < settings.voxelTailMaxLevels; ++level)
			{
				if (previousCubeCount <= 64u || sourcePayload.activeCells.empty())
				{
					break;
				}

				AppendedVoxelGroupResult tailResult;
				if (!BuildPartVoxelTailLevel(
					state,
					childGroupIndex,
					sourcePayload,
					coverageSourceTriangles,
					coverageMaterialSampler,
					settings,
					tailResult))
				{
					break;
				}

				generatedLevels++;
				generatedCubes += tailResult.cubeCount;
				spdlog::debug(
					"ClusterLOD voxel tail: root={} child={} tail={} level={} voxel_width={} source_cells={} tail_cells={} source_cubes={} tail_cubes={} clusters={}",
					rootGroupIndex,
					childGroupIndex,
					tailResult.groupIndex,
					level,
					tailResult.renderPayload.voxelWidth,
					sourcePayload.activeCells.size(),
					tailResult.renderPayload.activeCells.size(),
					previousCubeCount,
					tailResult.cubeCount,
					tailResult.clusterCount);

				const bool reducedEnough = tailResult.cubeCount * 5u <= previousCubeCount * 4u;
				childGroupIndex = tailResult.groupIndex;
				previousCubeCount = tailResult.cubeCount;
				sourcePayload = std::move(tailResult.renderPayload);
				if (!reducedEnough)
				{
					break;
				}
			}
		}

		TracyPlot("CLOD.VoxelFallback.TailLevels", static_cast<int64_t>(generatedLevels));
		TracyPlot("CLOD.VoxelFallback.TailCubes", static_cast<int64_t>(generatedCubes));
		if (generatedLevels != 0u)
		{
			spdlog::debug(
				"ClusterLOD voxel tail groups: roots={} generated_levels={} generated_cubes={} growth={} max_levels={}",
				voxelRoots.size(),
				generatedLevels,
				generatedCubes,
				settings.voxelTailGrowthFactor,
				settings.voxelTailMaxLevels);
		}
	}

	bool AppendSharedVoxelCoverageSourceGeometry(
		const ClusterLODBuildState& state,
		const std::vector<uint8_t>& buildVoxelGroupMask,
		VoxelFallbackGroupBuildInput& buildInput,
		size_t vertexStrideBytes)
	{
		buildInput.voxelVertices.clear();
		buildInput.voxelSkinningVertices.clear();
		buildInput.voxelTriangleIndices.clear();
		buildInput.voxelTriangleRefinedGroupIds.clear();
		buildInput.sourceVoxelGroupIndices.clear();
		buildInput.voxelVertexCount = 0u;
		buildInput.sourcePrimitiveCountForCubeBudget = 0u;

		for (uint32_t groupIndex = 0u; groupIndex < static_cast<uint32_t>(state.groups.size()); ++groupIndex)
		{
			if (groupIndex >= buildVoxelGroupMask.size() || buildVoxelGroupMask[groupIndex] == 0u)
			{
				continue;
			}

			const std::vector<uint32_t> refinedChildren = CollectUniqueRefinedChildren(state, groupIndex);
			const int32_t refinedGroupTag = static_cast<int32_t>(groupIndex);
			const bool appended = refinedChildren.empty()
				? AppendGroupTriangleSourceGeometry(state, groupIndex, buildInput, vertexStrideBytes, refinedGroupTag)
				: AppendTerminalSegmentSourceGeometry(state, groupIndex, buildInput, vertexStrideBytes, refinedGroupTag);
			if (!appended)
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
		const VoxelCoverageMaterialSampler* coverageMaterialSampler,
		const ClusterLODBuilderSettings& settings)
	{
		ZoneScopedN("ClusterLODUtilities::BuildVoxelFallbackCandidates");
		const bool enabled = settings.enableVoxelFallback && settings.voxelFallbackMode != ClusterLODVoxelFallbackMode::MeshOnly;
		if (!enabled || state.groups.empty())
		{
			return;
		}

		state.voxelGroupMapping.groupToPayloadIndex.assign(state.groups.size(), -1);
		state.voxelGroupMapping.groupToPackedMetadataIndex.assign(state.groups.size(), -1);
		state.voxelCarryPayloads.assign(state.groups.size(), {});

		VoxelFallbackBuildStats stats{};
		const bool forceAllVoxels = settings.voxelFallbackMode == ClusterLODVoxelFallbackMode::VoxelOnly;
		const bool autoMode = settings.voxelFallbackMode == ClusterLODVoxelFallbackMode::Auto;
		const uint32_t originalGroupCount = static_cast<uint32_t>(state.groups.size());
		TracyPlot("CLOD.VoxelFallback.InputGroups", static_cast<int64_t>(originalGroupCount));
		std::vector<VoxelFallbackGroupBuildInput> groupInputs(originalGroupCount);
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

		{
			ZoneScopedN("ClusterLODUtilities::VoxelFallback::AnalyzeGroups");
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
				VoxelFallbackGroupBuildInput buildInput;
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
				const bool hasOnlyRefinedDomain = group.segmentCount != 0u && group.terminalSegmentCount == 0u;
				const bool hasFiniteTriangleReductionError = finiteVoxelDecisionError(originalGroupErrors[groupIndex]);
				const RefinedChildErrorRange childErrorRange = GetRefinedChildTraversalErrorRange(state, groupIndex);
				buildInput.autoAcceptanceErrorReference = childErrorRange.minError;
				const float targetVoxelRepresentationError = ComputeVoxelRepresentationError(buildInput.analysis.targetVoxelWidth);
				// Auto voxel fallback is allowed only when every emitted voxel
				// segment has a refined child boundary that can suppress it near the
				// camera, and the voxel representation is already no worse than the
				// tightest child-side cut boundary. Terminal sections have no child
				// condition 2 guard, so they remain triangle-only unless voxel mode
				// is forced.
				buildInput.autoWouldFitBudget = hasOnlyRefinedDomain &&
					hasFiniteTriangleReductionError &&
					childErrorRange.count != 0u &&
					finiteVoxelDecisionError(buildInput.autoAcceptanceErrorReference) &&
					targetVoxelRepresentationError * std::max(1.0f, settings.voxelFallbackAcceptanceBias) <= buildInput.autoAcceptanceErrorReference;
				if (buildInput.autoWouldFitBudget)
				{
					stats.autoCandidateGroups++;
				}
				groupInputs[groupIndex].analysis = buildInput.analysis;
				groupInputs[groupIndex].autoWouldFitBudget = buildInput.autoWouldFitBudget;
				groupInputs[groupIndex].autoAcceptanceErrorReference = buildInput.autoAcceptanceErrorReference;
				groupInputs[groupIndex].sourcePrimitiveCountForCubeBudget = buildInput.sourcePrimitiveCountForCubeBudget;
			}
		}
		TracyPlot("CLOD.VoxelFallback.ValidGroups", static_cast<int64_t>(stats.validGroups));
		TracyPlot("CLOD.VoxelFallback.AutoCandidateGroups", static_cast<int64_t>(stats.autoCandidateGroups));

		VoxelFallbackGroupBuildInput sharedCoverageBuildInput;
		VoxelSourceTriangleBVH sharedCoverageSourceTriangles;
		const VoxelSourceTriangleBVH* sharedVoxelCoverageSourceTriangles = nullptr;

		auto buildVoxelGroup = [&](uint32_t groupIndex, const std::vector<uint8_t>& requiredVoxelSourceMask, bool requireBudgetFit, bool requireQualityFit, bool forceReplaceGroupWithVoxels) -> bool
		{
			ZoneScopedN("ClusterLODUtilities::VoxelFallback::BuildVoxelGroup");
			if (groupIndex >= groupInputs.size())
			{
				spdlog::error(
					"ClusterLOD voxel build failed: group={} reason=group_index_out_of_range group_inputs={}",
					groupIndex,
					groupInputs.size());
				return false;
			}

			VoxelFallbackGroupBuildInput buildInput = groupInputs[groupIndex];
			auto keepSourceCarryPayloadsForDagParents = [&]()
			{
				ZoneScopedN("ClusterLODUtilities::VoxelFallback::KeepCarryPayloadsForDagParents");
				TracyPlot("CLOD.VoxelFallback.LiveCarryCells", static_cast<int64_t>(CountLiveCarryPayloadCells(state)));
			};
			const auto sourceBuildStart = std::chrono::steady_clock::now();
			if (!BuildVoxelFallbackSourceGeometry(state, groupIndex, vertexStrideBytes, buildInput, &requiredVoxelSourceMask))
			{
				stats.sourceBuildUs += elapsedUsSince(sourceBuildStart);
				stats.failedBuilds++;
				spdlog::error(
					"ClusterLOD voxel build failed: group={} reason=source_geometry_failed required_sources={} source_tris={} source_voxel_groups={}",
					groupIndex,
					groupIndex < requiredVoxelSourceMask.size() ? static_cast<uint32_t>(requiredVoxelSourceMask[groupIndex]) : 0u,
					buildInput.voxelTriangleIndices.size() / 3ull,
					buildInput.sourceVoxelGroupIndices.size());
				return false;
			}
			stats.sourceBuildUs += elapsedUsSince(sourceBuildStart);

			const auto analysisStart = std::chrono::steady_clock::now();
			buildInput.analysis = AnalyzeVoxelFallbackBuildInput(state, buildInput, vertexStrideBytes, settings);
			stats.analysisUs += elapsedUsSince(analysisStart);

			if (!buildInput.analysis.valid)
			{
				stats.failedBuilds++;
				keepSourceCarryPayloadsForDagParents();
				spdlog::error(
					"ClusterLOD voxel build failed: group={} depth={} reason=invalid_analysis source_tris={} source_voxel_groups={} source_vertices={} target_resolution={} target_voxel_width={} aabb_min=({}, {}, {}) aabb_max=({}, {}, {})",
					groupIndex,
					groupIndex < state.groups.size() ? std::max(state.groups[groupIndex].depth, 0) : 0,
					buildInput.voxelTriangleIndices.size() / 3ull,
					buildInput.sourceVoxelGroupIndices.size(),
					buildInput.voxelVertexCount,
					buildInput.analysis.targetResolution,
					buildInput.analysis.targetVoxelWidth,
					buildInput.analysis.aabbMin.x,
					buildInput.analysis.aabbMin.y,
					buildInput.analysis.aabbMin.z,
					buildInput.analysis.aabbMax.x,
					buildInput.analysis.aabbMax.y,
					buildInput.analysis.aabbMax.z);
				return false;
			}

			ClusterLODGroup& group = state.groups[groupIndex];
			if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u)
			{
				keepSourceCarryPayloadsForDagParents();
				return true;
			}

			VoxelGroupPayload payload{};
			const uint32_t metadataIndex = static_cast<uint32_t>(state.voxelGroupMapping.packedGroupMetadata.size());
			const uint32_t firstCluster = static_cast<uint32_t>(state.voxelGroupMapping.packedClusterRecords.size());
			const uint32_t firstCube = static_cast<uint32_t>(state.voxelGroupMapping.packedCubeRecords.size());
			const uint32_t firstAttribute = static_cast<uint32_t>(state.voxelGroupMapping.packedAttributeSamples.size());
			uint32_t resolution = buildInput.analysis.targetResolution;
			uint32_t refinedChildErrorCount = 0u;
			const float maxRefinedChildError = GetMaxRefinedChildTraversalError(state, groupIndex, &refinedChildErrorCount);
			const RefinedChildErrorRange childErrorRange = GetRefinedChildTraversalErrorRange(state, groupIndex);
			const float childCutAcceptanceError = childErrorRange.minError;
			const float maxSourceVoxelWidth = GetMaxSourceVoxelWidthForBuildInput(state, buildInput);
			LogVoxelTriangleTagHistogram("candidate", groupIndex, group.depth, buildInput);
			auto resolutionForVoxelWidth = [&buildInput, &settings](float candidateVoxelWidth) -> uint32_t
			{
				const float extentX = buildInput.analysis.aabbMax.x - buildInput.analysis.aabbMin.x;
				const float extentY = buildInput.analysis.aabbMax.y - buildInput.analysis.aabbMin.y;
				const float extentZ = buildInput.analysis.aabbMax.z - buildInput.analysis.aabbMin.z;
				const float longestExtent = std::max({ extentX, extentY, extentZ });
				return std::max(
					std::max(2u, settings.voxelMinResolution),
					static_cast<uint32_t>(std::ceil(longestExtent / std::max(candidateVoxelWidth, 1.0e-8f))));
			};
			float voxelWidth = buildInput.analysis.targetVoxelWidth;
			float inheritedMinVoxelWidth = 0.0f;
			if (maxSourceVoxelWidth > 0.0f)
			{
				const float coarseningFactor = std::max(1.01f, settings.voxelFallbackGrowthFactor);
				inheritedMinVoxelWidth = maxSourceVoxelWidth * coarseningFactor;
				voxelWidth = inheritedMinVoxelWidth;
				resolution = resolutionForVoxelWidth(voxelWidth);
			}
			float maxQualityVoxelWidth = std::numeric_limits<float>::infinity();
			if (requireQualityFit && finiteVoxelDecisionError(buildInput.autoAcceptanceErrorReference))
			{
				maxQualityVoxelWidth = buildInput.autoAcceptanceErrorReference / std::max(1.0f, settings.voxelFallbackAcceptanceBias);
				if (std::isfinite(maxQualityVoxelWidth) && maxQualityVoxelWidth > 1.0e-8f && voxelWidth > maxQualityVoxelWidth)
				{
					if (inheritedMinVoxelWidth > 0.0f && maxQualityVoxelWidth < inheritedMinVoxelWidth)
					{
						spdlog::debug(
							"ClusterLOD voxel coarsening limited by quality: group={} depth={} target_voxel_width={} max_source_voxel_width={} inherited_min_voxel_width={} max_quality_voxel_width={} acceptance_error={}",
							groupIndex,
							group.depth,
							buildInput.analysis.targetVoxelWidth,
							maxSourceVoxelWidth,
							inheritedMinVoxelWidth,
							maxQualityVoxelWidth,
							buildInput.autoAcceptanceErrorReference);
					}
					voxelWidth = maxQualityVoxelWidth;
					resolution = resolutionForVoxelWidth(voxelWidth);
				}
			}
			float voxelRepresentationError = ComputeVoxelRepresentationError(voxelWidth);
			bool payloadFitsBudget = false;
			bool acceptedBudgetOverflowForQuality = false;
			PackedVoxelGroupBuildResult packed{};
			float packedVoxelRepresentationError = 0.0f;
			uint32_t lastPositiveCoverageCellCount = 0u;
			uint32_t lastCandidateCellCount = 0u;
			uint32_t lastTriangleCandidateCellCount = 0u;
			uint32_t lastVoxelCandidateCellCount = 0u;
			VoxelFallbackGroupBuildInput qualityTriangleSourceInput;
			bool useQualityTriangleSources = false;
			const bool sourceVoxelsTooCoarseForSeed =
				requireQualityFit &&
				maxSourceVoxelWidth > 0.0f &&
				std::isfinite(voxelWidth) &&
				voxelWidth > 1.0e-8f &&
				voxelWidth < maxSourceVoxelWidth;
			if (sourceVoxelsTooCoarseForSeed &&
				BuildVoxelFallbackCoverageSourceGeometry(state, groupIndex, vertexStrideBytes, qualityTriangleSourceInput) &&
				!qualityTriangleSourceInput.voxelTriangleIndices.empty())
			{
				useQualityTriangleSources = true;
				spdlog::debug(
					"ClusterLOD voxel seed using descendant triangle candidates: group={} depth={} voxel_width={} target_voxel_width={} max_source_voxel_width={} max_quality_voxel_width={} triangles={} vertices={}",
					groupIndex,
					group.depth,
					voxelWidth,
					buildInput.analysis.targetVoxelWidth,
					maxSourceVoxelWidth,
					maxQualityVoxelWidth,
					qualityTriangleSourceInput.voxelTriangleIndices.size() / 3ull,
					qualityTriangleSourceInput.voxelVertexCount);
			}
			auto logBuildFailure = [&](const char* reason)
			{
				spdlog::error(
					"ClusterLOD voxel build failed: group={} depth={} reason={} source_tris={} source_voxel_groups={} target_resolution={} voxel_width={} representation_error={} auto_acceptance_error={} require_budget={} require_quality={} force_replace={} payload_cells={} packed_cubes={} packed_clusters={} payload_fits_budget={} voxel_budget={} cube_budget={} positive_coverage={} candidate_cells={} triangle_candidates={} voxel_candidates={}",
					groupIndex,
					group.depth,
					reason,
					buildInput.voxelTriangleIndices.size() / 3ull,
					buildInput.sourceVoxelGroupIndices.size(),
					resolution,
					voxelWidth,
					voxelRepresentationError,
					buildInput.autoAcceptanceErrorReference,
					requireBudgetFit,
					requireQualityFit,
					forceReplaceGroupWithVoxels,
					payload.activeCells.size(),
					packed.cubeRecords.size(),
					packed.clusterRecords.size(),
					payloadFitsBudget,
					buildInput.analysis.voxelBudget,
					buildInput.analysis.cubeBudget,
					lastPositiveCoverageCellCount,
					lastCandidateCellCount,
					lastTriangleCandidateCellCount,
					lastVoxelCandidateCellCount);
			};
			auto packCurrentPayload = [&]() {
				PackVoxelGroupInput packInput{};
				packInput.payload = &payload;
				packInput.voxelError = voxelRepresentationError;
				packInput.opacityThreshold = settings.voxelFallbackOpacityThreshold;
				packInput.dominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
				packInput.firstCube = firstCube;
				packInput.firstAttribute = firstAttribute;
				PackedVoxelGroupBuildResult result = PackVoxelGroupToCubes(packInput);
				result.metadata.firstCluster = firstCluster;
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
			const VoxelSourceTriangleBVH* voxelCoverageSourceTriangles = sharedVoxelCoverageSourceTriangles;

			const uint32_t retryCount = std::max(1u, settings.voxelFallbackMaxRetryCount + 1u);
			bool qualityLimitExceeded = false;
			for (uint32_t attempt = 0; attempt < retryCount; ++attempt)
			{
				ZoneScopedN("ClusterLODUtilities::VoxelFallback::BuildVoxelGroup::Attempt");
				if (requireQualityFit && finiteVoxelDecisionError(buildInput.autoAcceptanceErrorReference) &&
					voxelRepresentationError * std::max(1.0f, settings.voxelFallbackAcceptanceBias) > buildInput.autoAcceptanceErrorReference)
				{
					if (!qualityLimitExceeded)
					{
						qualityLimitExceeded = true;
						spdlog::warn(
							"ClusterLOD voxel build quality limit exceeded; continuing mandatory build: group={} depth={} attempt={} voxel_width={} representation_error={} acceptance_error={} acceptance_bias={} require_quality={} require_budget={}",
							groupIndex,
							group.depth,
							attempt,
							voxelWidth,
							voxelRepresentationError,
							buildInput.autoAcceptanceErrorReference,
							settings.voxelFallbackAcceptanceBias,
							requireQualityFit,
							requireBudgetFit);
					}
				}

				std::vector<VoxelSourcePayloadInstance> sourceVoxelPayloadInstances;
				std::vector<VoxelSourcePayloadInstance> candidateVoxelPayloadInstances;
				if (!useQualityTriangleSources && !buildInput.sourceVoxelGroupIndices.empty())
				{
					sourceVoxelPayloadInstances.reserve(buildInput.sourceVoxelGroupIndices.size() * 2ull);
					candidateVoxelPayloadInstances.reserve(buildInput.sourceVoxelGroupIndices.size() * 2ull);
					for (uint32_t sourceVoxelGroupIndex : buildInput.sourceVoxelGroupIndices)
					{
						std::vector<VoxelSourcePayloadRef> sourcePayloadRefs;
						AppendVoxelSourcePayloadRefsForGroup(state, sourceVoxelGroupIndex, sourcePayloadRefs);
						if (sourcePayloadRefs.empty())
						{
							spdlog::error(
								"ClusterLOD voxel source refs empty: group={} depth={} source_group={} source_depth={} source_flags=0x{:X} source_required={} carry_cells={} render_payload_present={}",
								groupIndex,
								group.depth,
								sourceVoxelGroupIndex,
								sourceVoxelGroupIndex < state.groups.size() ? std::max(state.groups[sourceVoxelGroupIndex].depth, 0) : 0,
								sourceVoxelGroupIndex < state.groups.size() ? state.groups[sourceVoxelGroupIndex].flags : 0u,
								sourceVoxelGroupIndex < requiredVoxelSourceMask.size() ? static_cast<uint32_t>(requiredVoxelSourceMask[sourceVoxelGroupIndex]) : 0u,
								sourceVoxelGroupIndex < state.voxelCarryPayloads.size() ? state.voxelCarryPayloads[sourceVoxelGroupIndex].activeCells.size() : 0ull,
								GetVoxelRenderPayloadForGroup(state, sourceVoxelGroupIndex) != nullptr);
						}
						for (const VoxelSourcePayloadRef& payloadRef : sourcePayloadRefs)
						{
							if (payloadRef.payload != nullptr)
							{
								sourceVoxelPayloadInstances.push_back(VoxelSourcePayloadInstance{
									.payload = payloadRef.payload,
									.expansionRadius = 0.0f,
									.refinedGroupOverride = static_cast<int32_t>(sourceVoxelGroupIndex) });
								candidateVoxelPayloadInstances.push_back(VoxelSourcePayloadInstance{
									.payload = payloadRef.payload,
									.expansionRadius = payloadRef.expansionRadius,
									.refinedGroupOverride = static_cast<int32_t>(sourceVoxelGroupIndex) });
							}
						}
					}
				}

				const VoxelFallbackGroupBuildInput& voxelSourceInput = useQualityTriangleSources
					? qualityTriangleSourceInput
					: buildInput;
				VoxelizeTrianglesInput voxelInput{};
				voxelInput.vertices = voxelSourceInput.voxelVertices.empty() ? nullptr : &voxelSourceInput.voxelVertices;
				voxelInput.vertexStrideBytes = vertexStrideBytes;
				voxelInput.skinningVertices = voxelSourceInput.voxelSkinningVertices.empty() ? nullptr : &voxelSourceInput.voxelSkinningVertices;
				voxelInput.skinningVertexStrideBytes = skinningVertexStrideBytes;
				voxelInput.triangleIndices = voxelSourceInput.voxelTriangleIndices.empty() ? nullptr : &voxelSourceInput.voxelTriangleIndices;
				voxelInput.triangleRefinedGroupIds = voxelSourceInput.voxelTriangleRefinedGroupIds.empty() ? nullptr : &voxelSourceInput.voxelTriangleRefinedGroupIds;
				voxelInput.doubleSidedTriangles = settings.doubleSidedVoxelSourceNormals;
				voxelInput.coverageSourceTriangles = voxelCoverageSourceTriangles;
				voxelInput.coverageMaterialSampler = coverageMaterialSampler;
				voxelInput.terminalCoverageRefinedGroupOverride = static_cast<int32_t>(groupIndex);
				voxelInput.sourceVoxelPayloadInstances = sourceVoxelPayloadInstances.empty() ? nullptr : &sourceVoxelPayloadInstances;
				voxelInput.candidateVoxelPayloadInstances = candidateVoxelPayloadInstances.empty() ? nullptr : &candidateVoxelPayloadInstances;
				voxelInput.aabbMin = buildInput.analysis.aabbMin;
				voxelInput.aabbMax = buildInput.analysis.aabbMax;
				voxelInput.voxelWidth = voxelWidth;
				voxelInput.resolution = resolution;
				voxelInput.raysPerCell = settings.voxelRaysPerCell;
				const auto voxelizeStart = std::chrono::steady_clock::now();
				VoxelizeTrianglesResult voxelResult = VoxelizeTrianglesDetailed(voxelInput);
				stats.voxelizeUs += elapsedUsSince(voxelizeStart);
				lastPositiveCoverageCellCount = voxelResult.positiveCoverageCellCount;
				lastCandidateCellCount = voxelResult.candidateCellCount;
				lastTriangleCandidateCellCount = voxelResult.triangleCandidateCellCount;
				lastVoxelCandidateCellCount = voxelResult.voxelCandidateCellCount;
				{
					ZoneScopedN("ClusterLODUtilities::VoxelFallback::ReleaseVoxelSourceInstances");
					sourceVoxelPayloadInstances.clear();
					candidateVoxelPayloadInstances.clear();
				}
				stats.sourceCoverageQueries += voxelResult.sourceCoverageQueryCount;
				stats.sourceCoverageCandidates += voxelResult.sourceCoverageTriangleCandidateCount;
				stats.sourceCoverageTests += voxelResult.sourceCoverageTriangleTestCount;
				stats.sourceCoverageOutOfCell += voxelResult.sourceCoverageOutOfCellRejectionCount;
				spdlog::debug(
					"ClusterLOD voxel build detail: group={} depth={} attempt={} resolution={} voxel_width={} target_voxel_width={} representation_error={} child_cut_acceptance_error={} max_source_voxel_width={} inherited_min_voxel_width={} max_quality_voxel_width={} source_tris={} source_voxel_groups={} coverage_source_tris={} coverage_source_vertices={} source_primitives={} cube_budget={} tri_candidates={} voxel_candidates={} candidates={} positive_cells={} total_coverage={} max_coverage={} source_cells={} render_cells={} pruned={} source_coverage_queries={} source_coverage_candidates={} source_coverage_tests={} source_coverage_out_of_cell={}",
					groupIndex,
					group.depth,
					attempt,
					resolution,
					voxelWidth,
					buildInput.analysis.targetVoxelWidth,
					voxelRepresentationError,
					childCutAcceptanceError,
					maxSourceVoxelWidth,
					inheritedMinVoxelWidth,
					maxQualityVoxelWidth,
					voxelSourceInput.voxelTriangleIndices.size() / 3ull,
					useQualityTriangleSources ? 0ull : buildInput.sourceVoxelGroupIndices.size(),
					sharedCoverageBuildInput.voxelTriangleIndices.size() / 3ull,
					sharedCoverageBuildInput.voxelVertexCount,
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
						"ClusterLOD voxel refined detail: group={} depth={} attempt={} refined_group={} candidates={} tri_owned={} candidate_owned={} candidate_only={} positive={} zero_dropped={} emitted={} total_coverage={} max_coverage={}",
						groupIndex,
						group.depth,
						attempt,
						stats.refinedGroup,
						stats.candidateKeys,
						stats.triangleOwnedCells,
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
					{
						ZoneScopedN("ClusterLODUtilities::VoxelFallback::PackAttempt");
						packed = packCurrentPayload();
					}
					stats.packUs += elapsedUsSince(packStart);
					packedVoxelRepresentationError = voxelRepresentationError;
				}

				const bool cellCountFits = !payload.activeCells.empty() &&
					static_cast<float>(payload.activeCells.size()) <= buildInput.analysis.voxelBudget;
				const bool cubeCountFits = !packed.cubeRecords.empty() &&
					(buildInput.analysis.cubeBudget == 0u ||
						static_cast<uint64_t>(packed.cubeRecords.size()) <= static_cast<uint64_t>(buildInput.analysis.cubeBudget));
				payloadFitsBudget = cellCountFits && cubeCountFits;
				const bool hasPackedOutput = !payload.activeCells.empty() &&
					!packed.cubeRecords.empty() &&
					!packed.clusterRecords.empty();
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
				if (payloadFitsBudget || (!requireBudgetFit && hasPackedOutput))
				{
					break;
				}

				const float nextVoxelWidth = voxelWidth * std::max(1.01f, settings.voxelFallbackGrowthFactor);
				const bool nextAttemptExceedsQuality = requireQualityFit &&
					std::isfinite(maxQualityVoxelWidth) &&
					maxQualityVoxelWidth > 1.0e-8f &&
					nextVoxelWidth > maxQualityVoxelWidth;
				const bool exhaustedRetries = attempt + 1u >= retryCount;
				const bool preserveQualityOverCellBudget = requireQualityFit &&
					hasPackedOutput &&
					(nextAttemptExceedsQuality || (exhaustedRetries && cubeCountFits));
				if (preserveQualityOverCellBudget)
				{
					acceptedBudgetOverflowForQuality = true;
					spdlog::warn(
						"ClusterLOD voxel build preserving quality over cell budget: group={} depth={} attempt={} reason={} voxel_width={} acceptance_error={} payload_cells={} voxel_budget={} packed_cubes={} cube_budget={} cells_fit={} cubes_fit={}",
						groupIndex,
						group.depth,
						attempt,
						nextAttemptExceedsQuality ? "quality_limit" : "retry_limit",
						voxelWidth,
						buildInput.autoAcceptanceErrorReference,
						payload.activeCells.size(),
						buildInput.analysis.voxelBudget,
						packed.cubeRecords.size(),
						buildInput.analysis.cubeBudget,
						cellCountFits,
						cubeCountFits);
					break;
				}

				voxelWidth = nextVoxelWidth;
				voxelRepresentationError = ComputeVoxelRepresentationError(voxelWidth);
				resolution = resolutionForVoxelWidth(voxelWidth);
			}

			const bool acceptedBudget = payloadFitsBudget || acceptedBudgetOverflowForQuality;
			if (payload.activeCells.empty() || packed.cubeRecords.empty() || packed.clusterRecords.empty() || (requireBudgetFit && !acceptedBudget))
			{
				stats.failedBuilds++;
				keepSourceCarryPayloadsForDagParents();
				logBuildFailure(qualityLimitExceeded ? "quality_limit_invalid_output_or_budget" : "invalid_output_or_budget");
				return false;
			}
			if (qualityLimitExceeded)
			{
				spdlog::warn(
					"ClusterLOD voxel build accepted beyond quality limit: group={} depth={} voxel_width={} representation_error={} acceptance_error={} require_budget={} payload_cells={} packed_cubes={}",
					groupIndex,
					group.depth,
					voxelWidth,
					voxelRepresentationError,
					buildInput.autoAcceptanceErrorReference,
					requireBudgetFit,
					payload.activeCells.size(),
					packed.cubeRecords.size());
			}
			const uint32_t maxClusterCubeCount = computeMaxClusterCubeCount(packed);
			spdlog::debug(
				"ClusterLOD voxel pack detail: group={} depth={} payload_cells={} voxel_budget={} source_primitives={} cube_budget={} packed_cubes={} packed_clusters={} max_cluster_cube_count={} packed_attributes={} payload_voxel_width={} voxel_representation_error={} opacity_threshold={}",
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
				packedVoxelRepresentationError,
				settings.voxelFallbackOpacityThreshold);
			if (packed.cubeRecords.empty() || packed.clusterRecords.empty())
			{
				stats.failedBuilds++;
				logBuildFailure("empty_packed_output");
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

			state.voxelGroupMapping.groupToPayloadIndex[groupIndex] = -1;
			state.voxelGroupMapping.groupToPackedMetadataIndex[groupIndex] = static_cast<int32_t>(metadataIndex);
			std::vector<ClusterLODGroupSegment> voxelSegments;
			std::vector<BoundingSphere> voxelSegmentBounds;
			{
				ZoneScopedN("ClusterLODUtilities::VoxelFallback::SplitVoxelPageSegments");
				SplitVoxelClustersIntoPageSegments(packed, voxelSegments, voxelSegmentBounds);
			}
			state.voxelGroupMapping.packedGroupMetadata.push_back(packed.metadata);
			std::vector<std::vector<std::byte>> voxelPageBlobs;
			{
				ZoneScopedN("ClusterLODUtilities::VoxelFallback::BuildVoxelPageBlobs");
				voxelPageBlobs = BuildVoxelGroupPageBlobs(
					voxelSegments,
					packed.clusterRecords,
					packed.cubeRecords,
					packed.attributeSamples,
					firstAttribute);
			}
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
			const bool replaceGroupWithVoxels = forceAllVoxels || forceReplaceGroupWithVoxels || group.terminalSegmentCount == 0u;
			group.representationError = packedVoxelRepresentationError;
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
				"ClusterLOD voxel group error: group={} depth={} triangle_cut_error={} voxel_width={} representation_error={} terminal_sentinel={} terminal_segments={}/{} refined_child_errors={} min_refined_child_error={} max_refined_child_error={} source_primitives={} cube_budget={} packed_cubes={} forced_budget_fit={} replaces_group={}",
				groupIndex,
				group.depth,
				triangleError,
				payload.voxelWidth,
				packedVoxelRepresentationError,
				terminalErrorSentinel,
				group.terminalSegmentCount,
				group.segmentCount,
				refinedChildErrorCount,
				childErrorRange.minError,
				maxRefinedChildError,
				buildInput.analysis.sourcePrimitiveCountForCubeBudget,
				buildInput.analysis.cubeBudget,
				packed.cubeRecords.size(),
				requireBudgetFit,
				replaceGroupWithVoxels);
			stats.generatedPayloads++;
			stats.generatedCubes += static_cast<uint32_t>(packed.cubeRecords.size());
			keepSourceCarryPayloadsForDagParents();
			return true;
		};

		std::vector<uint8_t> replaceVoxelGroupMask(originalGroupCount, 0u);
		std::vector<uint8_t> buildVoxelGroupMask(originalGroupCount, 0u);
		std::vector<uint8_t> seedVoxelGroupMask(originalGroupCount, 0u);
		if (forceAllVoxels)
		{
			std::fill(replaceVoxelGroupMask.begin(), replaceVoxelGroupMask.end(), 1u);
			std::fill(buildVoxelGroupMask.begin(), buildVoxelGroupMask.end(), 1u);
		}
		else if (autoMode)
		{
			for (uint32_t groupIndex = 0; groupIndex < originalGroupCount; ++groupIndex)
			{
				if (groupInputs[groupIndex].autoWouldFitBudget)
				{
					replaceVoxelGroupMask[groupIndex] = 1u;
					seedVoxelGroupMask[groupIndex] = 1u;
				}
			}

			bool propagatedAny = true;
			while (propagatedAny)
			{
				propagatedAny = false;
				for (uint32_t groupIndex = 0; groupIndex < originalGroupCount; ++groupIndex)
				{
					if (replaceVoxelGroupMask[groupIndex] != 0u)
					{
						continue;
					}

					const ClusterLODGroup& group = state.groups[groupIndex];
					bool hasVoxelRefinedChild = false;
					for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
					{
						const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
						if (segment.refinedGroup < 0)
						{
							continue;
						}

						const uint32_t childGroupIndex = static_cast<uint32_t>(segment.refinedGroup);
						if (childGroupIndex < replaceVoxelGroupMask.size() && replaceVoxelGroupMask[childGroupIndex] != 0u)
						{
							hasVoxelRefinedChild = true;
							break;
						}
					}

					if (hasVoxelRefinedChild)
					{
						replaceVoxelGroupMask[groupIndex] = 1u;
						propagatedAny = true;
					}
				}
			}

			buildVoxelGroupMask = replaceVoxelGroupMask;
			propagatedAny = true;
			while (propagatedAny)
			{
				propagatedAny = false;
				for (uint32_t groupIndex = 0; groupIndex < originalGroupCount; ++groupIndex)
				{
					if (buildVoxelGroupMask[groupIndex] == 0u)
					{
						continue;
					}

					for (uint32_t childGroupIndex : CollectUniqueRefinedChildren(state, groupIndex))
					{
						if (childGroupIndex < buildVoxelGroupMask.size() && buildVoxelGroupMask[childGroupIndex] == 0u)
						{
							buildVoxelGroupMask[childGroupIndex] = 1u;
							propagatedAny = true;
						}
					}
				}
			}
		}

		auto hasMarkedVoxelRefinedChild = [&](uint32_t groupIndex) -> bool
		{
			if (groupIndex >= originalGroupCount)
			{
				return false;
			}

			const ClusterLODGroup& group = state.groups[groupIndex];
			for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
			{
				const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
				if (segment.refinedGroup < 0)
				{
					continue;
				}

				const uint32_t childGroupIndex = static_cast<uint32_t>(segment.refinedGroup);
				if (childGroupIndex < replaceVoxelGroupMask.size() && replaceVoxelGroupMask[childGroupIndex] != 0u)
				{
					return true;
				}
			}
			return false;
		};

		{
			uint32_t replaceVoxelGroupCount = 0u;
			uint32_t buildVoxelGroupCount = 0u;
			for (uint32_t groupIndex = 0; groupIndex < originalGroupCount; ++groupIndex)
			{
				replaceVoxelGroupCount += replaceVoxelGroupMask[groupIndex] != 0u ? 1u : 0u;
				buildVoxelGroupCount += buildVoxelGroupMask[groupIndex] != 0u ? 1u : 0u;
			}
			TracyPlot("CLOD.VoxelFallback.ReplaceGroups", static_cast<int64_t>(replaceVoxelGroupCount));
			TracyPlot("CLOD.VoxelFallback.BuildGroups", static_cast<int64_t>(buildVoxelGroupCount));
			spdlog::debug(
				"ClusterLOD voxel fallback masks: replace_groups={} build_groups={} dependency_groups={}",
				replaceVoxelGroupCount,
				buildVoxelGroupCount,
				buildVoxelGroupCount >= replaceVoxelGroupCount ? buildVoxelGroupCount - replaceVoxelGroupCount : 0u);
		}

		if (std::any_of(buildVoxelGroupMask.begin(), buildVoxelGroupMask.end(), [](uint8_t value) { return value != 0u; }) &&
			AppendSharedVoxelCoverageSourceGeometry(state, buildVoxelGroupMask, sharedCoverageBuildInput, vertexStrideBytes))
		{
			const auto coverageBvhStart = std::chrono::steady_clock::now();
			{
				ZoneScopedN("ClusterLODUtilities::VoxelFallback::BuildSharedCoverageBVH");
				sharedCoverageSourceTriangles.Build(
					&sharedCoverageBuildInput.voxelVertices,
					vertexStrideBytes,
					&sharedCoverageBuildInput.voxelTriangleIndices,
					sharedCoverageBuildInput.voxelSkinningVertices.empty() ? nullptr : &sharedCoverageBuildInput.voxelSkinningVertices,
					skinningVertexStrideBytes,
					sharedCoverageBuildInput.voxelTriangleRefinedGroupIds.empty() ? nullptr : &sharedCoverageBuildInput.voxelTriangleRefinedGroupIds,
					settings.doubleSidedVoxelSourceNormals,
					false);
				sharedCoverageSourceTriangles.SetRefinedGroupDomainMap(BuildVoxelCoverageDomainMap(state, buildVoxelGroupMask));
			}
			stats.coverageBvhUs += elapsedUsSince(coverageBvhStart);
			stats.coverageBvhBuilds++;
			if (sharedCoverageSourceTriangles.IsValid())
			{
				sharedVoxelCoverageSourceTriangles = &sharedCoverageSourceTriangles;
			}
			spdlog::debug(
				"ClusterLOD voxel shared coverage source: vertices={} triangles={} build_groups={} valid={}",
				sharedCoverageBuildInput.voxelVertexCount,
				sharedCoverageBuildInput.voxelTriangleIndices.size() / 3ull,
				std::count_if(buildVoxelGroupMask.begin(), buildVoxelGroupMask.end(), [](uint8_t value) { return value != 0u; }),
				sharedVoxelCoverageSourceTriangles != nullptr);
		}

		std::vector<uint32_t> remainingVoxelSourceConsumers(originalGroupCount, 0u);
		for (uint32_t parentGroupIndex = 0; parentGroupIndex < originalGroupCount; ++parentGroupIndex)
		{
			if (buildVoxelGroupMask[parentGroupIndex] == 0u)
			{
				continue;
			}

			for (uint32_t childGroupIndex : CollectUniqueRefinedChildren(state, parentGroupIndex))
			{
				if (childGroupIndex < originalGroupCount && buildVoxelGroupMask[childGroupIndex] != 0u)
				{
					remainingVoxelSourceConsumers[childGroupIndex]++;
				}
			}
		}

		auto releaseCarryPayloadIfUnconsumed = [&](uint32_t groupIndex)
		{
			if (groupIndex >= remainingVoxelSourceConsumers.size() ||
				remainingVoxelSourceConsumers[groupIndex] != 0u ||
				groupIndex >= state.voxelCarryPayloads.size() ||
				state.voxelCarryPayloads[groupIndex].activeCells.empty())
			{
				return;
			}

			ZoneScopedN("ClusterLODUtilities::VoxelFallback::ReleaseConsumedCarryPayload");
			ReleaseVoxelGroupPayloadStorage(state.voxelCarryPayloads[groupIndex]);
			TracyPlot("CLOD.VoxelFallback.LiveCarryCells", static_cast<int64_t>(CountLiveCarryPayloadCells(state)));
		};

		auto releaseConsumedSourcePayloads = [&](uint32_t parentGroupIndex)
		{
			ZoneScopedN("ClusterLODUtilities::VoxelFallback::ReleaseConsumedSourcePayloads");
			for (uint32_t childGroupIndex : CollectUniqueRefinedChildren(state, parentGroupIndex))
			{
				if (childGroupIndex >= remainingVoxelSourceConsumers.size() ||
					remainingVoxelSourceConsumers[childGroupIndex] == 0u)
				{
					continue;
				}

				remainingVoxelSourceConsumers[childGroupIndex]--;
				releaseCarryPayloadIfUnconsumed(childGroupIndex);
			}
		};

		for (uint32_t depth = 0; depth <= maxDepth; ++depth)
		{
			for (uint32_t groupIndex = 0; groupIndex < originalGroupCount; ++groupIndex)
			{
				ClusterLODGroup& group = state.groups[groupIndex];
				if (buildVoxelGroupMask[groupIndex] == 0u ||
					static_cast<uint32_t>(std::max(group.depth, 0)) != depth ||
					(group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u)
				{
					continue;
				}

				const bool inheritedVoxelPath = hasMarkedVoxelRefinedChild(groupIndex);
				const bool seedVoxelPath = seedVoxelGroupMask[groupIndex] != 0u;
				const bool requireBudgetFit = seedVoxelPath && !inheritedVoxelPath && !forceAllVoxels;
				const bool requireQualityFit = !forceAllVoxels && seedVoxelPath && !inheritedVoxelPath;
				const bool builtVoxelGroup = buildVoxelGroup(groupIndex, buildVoxelGroupMask, requireBudgetFit, requireQualityFit, inheritedVoxelPath || forceAllVoxels);
				if (!builtVoxelGroup)
				{
					throw std::runtime_error(
						std::string("ClusterLOD voxel fallback: mandatory voxel build failed for group ") +
						std::to_string(groupIndex) +
						" depth=" + std::to_string(std::max(group.depth, 0)) +
						" forced=" + std::to_string(forceAllVoxels ? 1 : 0) +
						" seed=" + std::to_string(seedVoxelPath ? 1 : 0) +
						" inherited=" + std::to_string(inheritedVoxelPath ? 1 : 0) +
						" require_budget=" + std::to_string(requireBudgetFit ? 1 : 0) +
						" require_quality=" + std::to_string(requireQualityFit ? 1 : 0));
				}

				if (builtVoxelGroup)
				{
					if (forceAllVoxels)
					{
						stats.forcedGroups++;
					}
					else if (inheritedVoxelPath)
					{
						stats.propagatedGroups++;
					}
					else if (seedVoxelPath)
					{
						stats.acceptedSeedGroups++;
					}

					releaseConsumedSourcePayloads(groupIndex);
					releaseCarryPayloadIfUnconsumed(groupIndex);
				}
			}
		}

		BuildPartVoxelTailGroups(
			state,
			sharedVoxelCoverageSourceTriangles,
			coverageMaterialSampler,
			settings);

		{
			ZoneScopedN("ClusterLODUtilities::VoxelFallback::ValidateVoxelParentClosure");
			for (uint32_t parentGroupIndex = 0; parentGroupIndex < static_cast<uint32_t>(state.groups.size()); ++parentGroupIndex)
			{
				const ClusterLODGroup& parentGroup = state.groups[parentGroupIndex];
				if (parentGroup.firstSegment + parentGroup.segmentCount > state.segments.size())
				{
					continue;
				}

				const bool parentIsVoxel = (parentGroup.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u;
				for (uint32_t segmentOffset = 0; segmentOffset < parentGroup.segmentCount; ++segmentOffset)
				{
					const ClusterLODGroupSegment& segment = state.segments[parentGroup.firstSegment + segmentOffset];
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
					if ((childGroup.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u && !parentIsVoxel)
					{
						throw std::runtime_error(
							std::string("ClusterLOD voxel fallback: voxel refined child has non-voxel DAG parent; child=") +
							std::to_string(childGroupIndex) +
							" child_depth=" + std::to_string(std::max(childGroup.depth, 0)) +
							" parent=" + std::to_string(parentGroupIndex) +
							" parent_depth=" + std::to_string(std::max(parentGroup.depth, 0)));
					}
				}
			}
		}

		{
			ZoneScopedN("ClusterLODUtilities::VoxelFallback::ReleaseRemainingCarryPayloads");
			for (VoxelGroupPayload& carryPayload : state.voxelCarryPayloads)
			{
				ReleaseVoxelGroupPayloadStorage(carryPayload);
			}
			TracyPlot("CLOD.VoxelFallback.LiveCarryCells", int64_t{ 0 });
		}

		std::vector<uint32_t> parentRefCounts(state.groups.size(), 0u);
		std::vector<uint32_t> nonVoxelParentRefCounts(state.groups.size(), 0u);
		std::vector<float> voxelParentRepresentationErrorForGroup(state.groups.size(), 0.0f);
		for (const ClusterLODGroup& group : state.groups)
		{
			if (group.firstSegment + group.segmentCount > state.segments.size())
			{
				continue;
			}

			const bool parentIsVoxel = (group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u;
			const float parentVoxelRepresentationError =
				parentIsVoxel && std::isfinite(group.representationError) && group.representationError > 0.0f
				? group.representationError
				: 0.0f;
			for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
			{
				const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
				if (segment.refinedGroup < 0)
				{
					continue;
				}

				const uint32_t childGroupIndex = static_cast<uint32_t>(segment.refinedGroup);
				if (childGroupIndex < parentRefCounts.size())
				{
					parentRefCounts[childGroupIndex]++;
					if (parentIsVoxel)
					{
						voxelParentRepresentationErrorForGroup[childGroupIndex] = std::max(
							voxelParentRepresentationErrorForGroup[childGroupIndex],
							parentVoxelRepresentationError);
					}
					else
					{
						nonVoxelParentRefCounts[childGroupIndex]++;
					}
				}
			}
		}

		uint32_t voxelTraversalBoundaryRewrites = 0u;
		uint32_t voxelTraversalRootSentinelsPreserved = 0u;
		for (uint32_t groupIndex = 0; groupIndex < static_cast<uint32_t>(state.groups.size()); ++groupIndex)
		{
			ClusterLODGroup& group = state.groups[groupIndex];
			const bool hasVoxelParentBoundary =
				groupIndex < voxelParentRepresentationErrorForGroup.size() &&
				std::isfinite(voxelParentRepresentationErrorForGroup[groupIndex]) &&
				voxelParentRepresentationErrorForGroup[groupIndex] > 0.0f;
			const bool hasNonVoxelParent =
				groupIndex < nonVoxelParentRefCounts.size() &&
				nonVoxelParentRefCounts[groupIndex] != 0u;
			const bool isRootGroup =
				groupIndex < parentRefCounts.size() &&
				parentRefCounts[groupIndex] == 0u;

			if (!hasVoxelParentBoundary)
			{
				if (isRootGroup &&
					(group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u &&
					IsTerminalErrorSentinel(group.bounds.error))
				{
					voxelTraversalRootSentinelsPreserved++;
				}
				continue;
			}

			if (isRootGroup && IsTerminalErrorSentinel(group.bounds.error))
			{
				voxelTraversalRootSentinelsPreserved++;
				continue;
			}

			float traversalBoundary = voxelParentRepresentationErrorForGroup[groupIndex];
			if (hasNonVoxelParent && groupIndex < originalGroupErrors.size())
			{
				const float originalGroupError = originalGroupErrors[groupIndex];
				if (std::isfinite(originalGroupError) && originalGroupError > 0.0f)
				{
					traversalBoundary = std::max(traversalBoundary, originalGroupError);
				}
			}

			if (std::isfinite(traversalBoundary) && traversalBoundary > 0.0f)
			{
				group.bounds.error = traversalBoundary;
				voxelTraversalBoundaryRewrites++;
			}
		}

		uint32_t parentTraversalErrorRaises = 0u;
		bool raisedParentError = true;
		while (raisedParentError)
		{
			raisedParentError = false;
			for (uint32_t groupIndex = 0; groupIndex < originalGroupCount; ++groupIndex)
			{
				ClusterLODGroup& group = state.groups[groupIndex];
				if (IsTerminalErrorSentinel(group.bounds.error))
				{
					continue;
				}

				float maxChildError = 0.0f;
				bool hasFiniteChild = false;
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

					const float childError = state.groups[childGroupIndex].bounds.error;
					if (IsFiniteContentTraversalError(childError))
					{
						maxChildError = std::max(maxChildError, childError);
						hasFiniteChild = true;
					}
				}

				if (!hasFiniteChild || group.bounds.error > maxChildError)
				{
					continue;
				}

				group.bounds.error = std::nextafter(maxChildError, std::numeric_limits<float>::infinity());
				parentTraversalErrorRaises++;
				raisedParentError = true;
			}
		}

		uint32_t voxelTraversalErrorUnderreports = 0u;
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
			else if (autoMode &&
				std::isfinite(group.representationError) &&
				group.representationError > 0.0f &&
				std::isfinite(group.bounds.error) &&
				!IsTerminalErrorSentinel(group.bounds.error) &&
				group.representationError > group.bounds.error)
			{
				voxelTraversalErrorUnderreports++;
				if (voxelTraversalErrorUnderreports <= 8u)
				{
					spdlog::warn(
						"ClusterLOD voxel traversal error underreports representation error: group={} depth={} representation_error={} min_child_error={} max_child_error={} group_cut_error={} terminal_segments={}/{}",
						groupIndex,
						group.depth,
						group.representationError,
						minChildError,
						maxChildError,
						group.bounds.error,
						group.terminalSegmentCount,
						group.segmentCount);
				}
			}

			spdlog::debug(
				"ClusterLOD voxel hierarchy: group={} depth={} cut_error={} representation_error={} refined_children={} voxel_children={} triangle_children={} min_child_error={} max_child_error={} monotonic_with_children={}",
				groupIndex,
				group.depth,
				group.bounds.error,
				group.representationError,
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
		uint64_t retainedPayloadCells = 0u;
		for (const VoxelGroupPayload& payload : state.voxelGroupMapping.payloads)
		{
			retainedPayloadCells += payload.activeCells.size();
		}
		TracyPlot("CLOD.VoxelFallback.VoxelGroups", static_cast<int64_t>(voxelGroups));
		TracyPlot("CLOD.VoxelFallback.TriangleGroups", static_cast<int64_t>(triangleGroups));
		TracyPlot("CLOD.VoxelFallback.Payloads", static_cast<int64_t>(totalVoxelPayloads));
		TracyPlot("CLOD.VoxelFallback.RetainedPayloadCells", static_cast<int64_t>(retainedPayloadCells));
		TracyPlot("CLOD.VoxelFallback.Clusters", static_cast<int64_t>(totalVoxelClusters));
		TracyPlot("CLOD.VoxelFallback.Cubes", static_cast<int64_t>(totalVoxelCubes));
		TracyPlot("CLOD.VoxelFallback.FailedBuilds", static_cast<int64_t>(stats.failedBuilds));

		spdlog::info(
			"ClusterLOD voxel fallback: analyzed={} valid={} auto_candidates={} accepted_seeds={} forced={} propagated={} voxel_groups={} triangle_groups={} payloads={} clusters={} cubes={} traversal_error_underreports={} voxel_boundary_rewrites={} parent_error_raises={} failed={} coverage_bvh_builds={} coverage_bvh_reuses={} source_coverage(queries={} candidates={} tests={} out_of_cell={}) timing_ms(analysis={:.2f} source={:.2f} coverage_bvh={:.2f} voxelize={:.2f} pack={:.2f})",
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
			voxelTraversalErrorUnderreports,
			voxelTraversalBoundaryRewrites,
			parentTraversalErrorRaises,
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
		if (voxelTraversalRootSentinelsPreserved != 0u)
		{
			spdlog::debug(
				"ClusterLOD voxel fallback preserved {} root terminal sentinel traversal errors after voxel boundary rewrites",
				voxelTraversalRootSentinelsPreserved);
		}
	}

	void BuildClusterLODTraversalHierarchy(ClusterLODBuildState& state, uint32_t preferredNodeWidth)
	{
		ZoneScopedN("ClusterLODUtilities::BuildClusterLODTraversalHierarchy");
		if (state.groups.empty())
			return;
		TracyPlot("CLOD.Traversal.Groups", static_cast<int64_t>(state.groups.size()));

		preferredNodeWidth = std::max(2u, preferredNodeWidth);

		auto includeGroupInTraversal = [&](uint32_t groupID) -> bool
		{
			return state.traversalGroupMask.empty() ||
				(groupID < state.traversalGroupMask.size() && state.traversalGroupMask[groupID] != 0u);
		};

		state.maxDepth = 0;
		for (uint32_t groupID = 0; groupID < uint32_t(state.groups.size()); ++groupID)
		{
			if (includeGroupInTraversal(groupID))
			{
				state.maxDepth = std::max(state.maxDepth, uint32_t(state.groups[groupID].depth));
			}
		}

		const uint32_t lodLevelCount = state.maxDepth + 1;

		// Collect traversal leaves by depth. Voxelized groups replace their triangle
		// representation at the group/section level; streaming pages are expanded only
		// after the voxel section wins the normal LOD decision.
		struct TraversalLeafInfo { uint32_t nodeKind; uint32_t indexOrOffset; uint32_t ownerGroupId; int32_t refinedGroup; };
		std::vector<std::vector<TraversalLeafInfo>> leavesByDepth(lodLevelCount);
		for (uint32_t groupID = 0; groupID < uint32_t(state.groups.size()); ++groupID)
		{
			if (!includeGroupInTraversal(groupID))
			{
				continue;
			}

			const ClusterLODGroup& grp = state.groups[groupID];
			const uint32_t d = uint32_t(grp.depth);
			if ((grp.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_PROXY) != 0u)
			{
				leavesByDepth[d].push_back({ CLOD_NODE_INSTANCE_ROOT, grp.firstMeshlet, groupID, -1 });
				continue;
			}

			const bool isVoxelGroup = (grp.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u;
			if (isVoxelGroup)
			{
				uint32_t s = 0;
				while (s < grp.segmentCount)
				{
					const uint32_t firstSectionSegment = s;
					const int32_t refinedGroup = state.segments[grp.firstSegment + s].refinedGroup;
					while (s < grp.segmentCount && state.segments[grp.firstSegment + s].refinedGroup == refinedGroup)
					{
						++s;
					}
					leavesByDepth[d].push_back({ 1u, firstSectionSegment, groupID, refinedGroup });
				}
				continue;
			}

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

			if (leafCount == 1u)
			{
				state.lodNodeRanges[depth].offset = state.lodLevelRoots[depth];
				state.lodNodeRanges[depth].count = 1u;
				continue;
			}

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
				node.traversalMetric.maxQuadricError = TraversalNodeErrorFromGroupError(grp.bounds.error);
			}

			if (leafCount == 1)
			{
				if (range.offset != state.lodLevelRoots[depth] || range.count != 1u)
				{
					throw std::runtime_error("Cluster LOD: single-leaf traversal range mismatch.");
				}
				continue;
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
			auto BuildInternalNode = [&](uint32_t childOffset, uint32_t childCount, bool structuralNode = false) -> ClusterLODNode {
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
				node.traversalMetric.maxQuadricError = structuralNode
					? std::max(kClusterLODStructuralTraversalError, maxErr)
					: maxErr;

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

					ClusterLODNode parent = BuildInternalNode(childOffset, childCount, true);
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
			root = BuildInternalNode(rootChildOffset, rootChildCount, true);

			state.topRootNode = 0;
			state.maxTraversalDepth = ComputeCLodTraversalDepth(state.nodes, state.topRootNode);

			uint32_t internalNodes = 0;
			uint32_t voxelLeafNodes = 0;
			uint32_t segmentLeafNodes = 0;
			uint32_t instanceRootNodes = 0;
			uint32_t invalidNodeKindCount = 0;
			for (const ClusterLODNode& node : state.nodes)
			{
				switch (node.range.isGroup)
				{
				case 0u: ++internalNodes; break;
				case 1u: ++voxelLeafNodes; break;
				case 2u: ++segmentLeafNodes; break;
				case 3u: ++instanceRootNodes; break;
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
					const bool finiteParent = IsFiniteContentTraversalError(parentError);
					const bool finiteChild = IsFiniteContentTraversalError(childError);
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
					if ((ownerGroup.flags & CLOD_GROUP_FLAG_IS_VOXEL) == 0u || node.range.indexOrOffset >= ownerGroup.segmentCount)
					{
						invalidLeafPayloads++;
					}
				}
				else if (node.range.isGroup == 2u)
				{
					if (node.range.indexOrOffset >= state.segments.size())
					{
						invalidLeafPayloads++;
					}
				}
				else if (node.range.isGroup == 3u)
				{
					if ((ownerGroup.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_PROXY) == 0u)
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

			const bool hierarchyValidationFailed =
				invalidNodeKindCount != 0u || invalidNodeRanges != 0u || invalidLeafOwners != 0u || invalidLeafPayloads != 0u ||
				internalMaxErrorViolations != 0u || monotonicErrorViolations != 0u || invalidSegmentRanges != 0u || invalidSegmentDomains != 0u ||
				invalidPageMapSegments != 0u || voxelPayloadMissing != 0u || voxelTrianglePayloadLeaks != 0u;
			auto logHierarchyValidationSummary = [&]()
			{
				spdlog::log(
					hierarchyValidationFailed ? spdlog::level::warn : spdlog::level::debug,
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
			};
			logHierarchyValidationSummary();

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

				uint32_t voxelLeavesAtDepth = 0u;
				uint32_t segmentLeavesAtDepth = 0u;
				for (const TraversalLeafInfo& leaf : leavesByDepth[depth])
				{
					voxelLeavesAtDepth += (leaf.nodeKind == 1u) ? 1u : 0u;
					segmentLeavesAtDepth += (leaf.nodeKind == 2u) ? 1u : 0u;
				}

				const uint32_t rootNodeId = state.lodLevelRoots[depth];
				const ClusterLODNode& rootNode = state.nodes[rootNodeId];
				const ClusterLODNodeRangeAlloc range = state.lodNodeRanges[depth];
				spdlog::debug(
					"ClusterLOD runtime hierarchy level: depth={} root={} root_kind={} root_children={} root_error={} range_offset={} range_count={} groups={} voxel_groups={} triangle_groups={} refined_edges={} leaves={} voxel_leaves={} segment_leaves={} min_group_error={} max_group_error={}",
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
					voxelLeavesAtDepth,
					segmentLeavesAtDepth,
					minErrorAtDepth,
					maxErrorAtDepth);
			}

			if (hierarchyValidationFailed)
			{
				throw std::runtime_error("Cluster LOD: runtime hierarchy validation failed; see preceding ClusterLOD runtime hierarchy validation logs");
			}

			spdlog::debug(
				"ClusterLOD traversal hierarchy: nodes={} levels={} top_root={} max_depth={} max_traversal_depth={} internal_nodes={} voxel_leaf_nodes={} segment_leaf_nodes={} instance_root_nodes={}",
				state.nodes.size(),
				lodLevelCount,
				state.topRootNode,
				state.maxDepth,
				state.maxTraversalDepth,
				internalNodes,
				voxelLeafNodes,
				segmentLeafNodes,
				instanceRootNodes);

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
		ZoneScopedN("ClusterLODUtilities::Build::GenerateMikkTangents");
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
		captureContext.recomputeGroupNormals = recomputeGroupNormals;
		// Keep CLOD generation deterministic while investigating cold-build normal/tangent instability.
		// The output callback assigns renderer group IDs in callback arrival order, so parallel
		// clodBuildEx execution can make persisted hierarchy/page ordering depend on worker scheduling.
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
	artifacts.prebuiltData.lodNodeRanges = std::move(state.lodNodeRanges);
	artifacts.prebuiltData.lodLevelRoots = std::move(state.lodLevelRoots);
	artifacts.prebuiltData.maxDepth = state.maxDepth;
	artifacts.prebuiltData.maxTraversalDepth = state.maxTraversalDepth;
	AssignSingleRootPartRecord(artifacts.prebuiltData, state.topRootNode);

	artifacts.cacheBuildData.groupPageBlobs = std::move(state.groupPageBlobs);
	artifacts.cacheBuildData.voxelGroupMapping = std::move(state.voxelGroupMapping);
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
	ZoneScopedN("ClusterLODUtilities::BuildVoxelOnlyClusterLODArtifactsFromPayload");
	ClusterLODPrebuildArtifacts artifacts{};
	if (payload.activeCells.empty() || payload.resolution == 0u || !(payload.voxelWidth > 0.0f))
	{
		return artifacts;
	}

	const float voxelRepresentationError = ComputeVoxelRepresentationError(payload.voxelWidth);
	PackVoxelGroupInput packInput{};
	packInput.payload = &payload;
	packInput.voxelError = voxelRepresentationError;
	packInput.opacityThreshold = settings.voxelFallbackOpacityThreshold;
	packInput.dominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
	PackedVoxelGroupBuildResult packed = PackVoxelGroupToCubes(packInput);
	BuildVoxelClustersFromCubes(packed, std::max(1u, maxCubesPerCluster));
	TracyPlot("CLOD.VoxelOnly.PayloadCells", static_cast<int64_t>(payload.activeCells.size()));
	if (packed.cubeRecords.empty() || packed.clusterRecords.empty())
	{
		return artifacts;
	}

	std::vector<ClusterLODGroupSegment> voxelSegments;
	std::vector<BoundingSphere> voxelSegmentBounds;
	{
		ZoneScopedN("ClusterLODUtilities::VoxelOnlyPayload::BuildVoxelPages");
		SplitVoxelClustersIntoPageSegments(packed, voxelSegments, voxelSegmentBounds);
	}
	std::vector<std::vector<std::byte>> voxelPageBlobs;
	{
		ZoneScopedN("ClusterLODUtilities::VoxelOnlyPayload::BuildVoxelPageBlobs");
		voxelPageBlobs = BuildVoxelGroupPageBlobs(
			voxelSegments,
			packed.clusterRecords,
			packed.cubeRecords,
			packed.attributeSamples,
			0u);
	}
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
	group.representationError = voxelRepresentationError;

	state.groups.push_back(group);
	state.segments = std::move(voxelSegments);
	state.segmentBounds = std::move(voxelSegmentBounds);
	state.groupChunks.resize(1);
	state.groupPageBlobs.resize(1);
	state.groupPageBlobs[0] = std::move(voxelPageBlobs);
	state.voxelGroupMapping.groupToPayloadIndex = { 0 };
	state.voxelGroupMapping.groupToPackedMetadataIndex = { 0 };
	state.voxelGroupMapping.payloads.push_back(payload);
	state.voxelGroupMapping.packedGroupMetadata.push_back(packed.metadata);
	state.voxelGroupMapping.packedClusterRecords = std::move(packed.clusterRecords);
	state.voxelGroupMapping.packedCubeRecords = std::move(packed.cubeRecords);
	state.voxelGroupMapping.packedAttributeSamples = std::move(packed.attributeSamples);

	{
		ZoneScopedN("ClusterLODUtilities::VoxelOnlyPayload::BuildTraversalHierarchy");
		BuildClusterLODTraversalHierarchy(state, /*preferredNodeWidth=*/8u);
	}

	std::vector<std::vector<std::byte>> meshPageBlobs;
	std::vector<uint32_t> groupPageReferences;
	std::vector<uint32_t> groupPageReferenceOffsets;
	uint32_t trianglePageCount = 0u;
	uint32_t voxelPageBase = 0u;
	uint32_t voxelPageCount = 0u;
	{
		ZoneScopedN("ClusterLODUtilities::VoxelOnlyPayload::FinalizeMeshWidePagePacking");
		FinalizeMeshWidePagePacking(
			state,
			meshPageBlobs,
			groupPageReferences,
			groupPageReferenceOffsets,
			trianglePageCount,
			voxelPageBase,
			voxelPageCount);
	}

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
	AssignSingleRootPartRecord(artifacts.prebuiltData, state.topRootNode);
	artifacts.cacheBuildData.groupPageBlobs = std::move(state.groupPageBlobs);
	artifacts.cacheBuildData.voxelGroupMapping = std::move(state.voxelGroupMapping);
	artifacts.cacheBuildData.meshPageBlobs = std::move(meshPageBlobs);

	return artifacts;
}

namespace
{
	DirectX::XMFLOAT3 TransformPoint3x4(const ClusterLODAssemblyTransform& transform, const DirectX::XMFLOAT3& point)
	{
		return {
			transform.row0.x * point.x + transform.row0.y * point.y + transform.row0.z * point.z + transform.row0.w,
			transform.row1.x * point.x + transform.row1.y * point.y + transform.row1.z * point.z + transform.row1.w,
			transform.row2.x * point.x + transform.row2.y * point.y + transform.row2.z * point.z + transform.row2.w,
		};
	}

	float MaxScale3x4(const ClusterLODAssemblyTransform& transform)
	{
		auto rowLength = [](const DirectX::XMFLOAT4& row) {
			return std::sqrt(row.x * row.x + row.y * row.y + row.z * row.z);
		};
		return std::max(rowLength(transform.row0), std::max(rowLength(transform.row1), rowLength(transform.row2)));
	}

	DirectX::XMFLOAT4 TransformSphere3x4(const ClusterLODAssemblyTransform& transform, const DirectX::XMFLOAT4& sphere)
	{
		const DirectX::XMFLOAT3 center{ sphere.x, sphere.y, sphere.z };
		const DirectX::XMFLOAT3 transformedCenter = TransformPoint3x4(transform, center);
		const float scale = MaxScale3x4(transform);
		return { transformedCenter.x, transformedCenter.y, transformedCenter.z, sphere.w * scale };
	}

	DirectX::XMFLOAT3 VoxelCellMin3(const VoxelGroupPayload& payload, const VoxelCell& cell)
	{
		return {
			payload.aabbMin.x + static_cast<float>(cell.x) * payload.voxelWidth,
			payload.aabbMin.y + static_cast<float>(cell.y) * payload.voxelWidth,
			payload.aabbMin.z + static_cast<float>(cell.z) * payload.voxelWidth
		};
	}

	DirectX::XMFLOAT3 VoxelCellMax3(const VoxelGroupPayload& payload, const VoxelCell& cell)
	{
		const DirectX::XMFLOAT3 cellMin = VoxelCellMin3(payload, cell);
		return {
			cellMin.x + payload.voxelWidth,
			cellMin.y + payload.voxelWidth,
			cellMin.z + payload.voxelWidth
		};
	}

	void ExpandAabbWithPoint(
		DirectX::XMFLOAT3& aabbMin,
		DirectX::XMFLOAT3& aabbMax,
		const DirectX::XMFLOAT3& point)
	{
		aabbMin.x = std::min(aabbMin.x, point.x);
		aabbMin.y = std::min(aabbMin.y, point.y);
		aabbMin.z = std::min(aabbMin.z, point.z);
		aabbMax.x = std::max(aabbMax.x, point.x);
		aabbMax.y = std::max(aabbMax.y, point.y);
		aabbMax.z = std::max(aabbMax.z, point.z);
	}

	void ExpandAabbWithTransformedAabb3x4(
		const ClusterLODAssemblyTransform& transform,
		const DirectX::XMFLOAT3& localMin,
		const DirectX::XMFLOAT3& localMax,
		DirectX::XMFLOAT3& aabbMin,
		DirectX::XMFLOAT3& aabbMax)
	{
		for (uint32_t corner = 0; corner < 8u; ++corner)
		{
			const DirectX::XMFLOAT3 point{
				(corner & 1u) != 0u ? localMax.x : localMin.x,
				(corner & 2u) != 0u ? localMax.y : localMin.y,
				(corner & 4u) != 0u ? localMax.z : localMin.z
			};
			ExpandAabbWithPoint(aabbMin, aabbMax, TransformPoint3x4(transform, point));
		}
	}

	bool BuildAabbFromVoxelSourceCells(
		std::span<const VoxelSourcePayloadInstance> sourceInstances,
		DirectX::XMFLOAT3& aabbMin,
		DirectX::XMFLOAT3& aabbMax)
	{
		aabbMin = DirectX::XMFLOAT3(
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max());
		aabbMax = DirectX::XMFLOAT3(
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::lowest());

		bool valid = false;
		for (const VoxelSourcePayloadInstance& source : sourceInstances)
		{
			if (source.payload == nullptr || source.payload->activeCells.empty() || source.payload->voxelWidth <= 0.0f)
			{
				continue;
			}

			for (const VoxelCell& cell : source.payload->activeCells)
			{
				const DirectX::XMFLOAT3 cellMin = VoxelCellMin3(*source.payload, cell);
				const DirectX::XMFLOAT3 cellMax = VoxelCellMax3(*source.payload, cell);
				ExpandAabbWithTransformedAabb3x4(source.localToTarget, cellMin, cellMax, aabbMin, aabbMax);
				valid = true;
			}
		}

		return valid;
	}

	struct VoxelCellRefinedKey
	{
		uint64_t cellKey = 0;
		int32_t refinedGroup = -1;

		bool operator==(const VoxelCellRefinedKey& other) const
		{
			return cellKey == other.cellKey && refinedGroup == other.refinedGroup;
		}
	};

	struct VoxelCellRefinedKeyHash
	{
		size_t operator()(const VoxelCellRefinedKey& key) const
		{
			size_t seed = std::hash<uint64_t>{}(key.cellKey);
			seed ^= std::hash<int32_t>{}(key.refinedGroup) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
			return seed;
		}
	};

	br::mesh::sggx::SymmetricMatrix3 TransformSGGX3x4(
		const ClusterLODAssemblyTransform& transform,
		const br::mesh::sggx::SymmetricMatrix3& source)
	{
		using br::mesh::sggx::Float3;

		const Float3 r0(transform.row0.x, transform.row0.y, transform.row0.z);
		const Float3 r1(transform.row1.x, transform.row1.y, transform.row1.z);
		const Float3 r2(transform.row2.x, transform.row2.y, transform.row2.z);
		const Float3 c0 = r1.cross(r2);
		const Float3 c1 = r2.cross(r0);
		const Float3 c2 = r0.cross(r1);
		const float det = r0.dot(c0);
		if (std::abs(det) <= 1.0e-8f)
		{
			return source;
		}

		const float invDet = 1.0f / det;
		const float n[3][3] = {
			{ c0.x * invDet, c0.y * invDet, c0.z * invDet },
			{ c1.x * invDet, c1.y * invDet, c1.z * invDet },
			{ c2.x * invDet, c2.y * invDet, c2.z * invDet }
		};
		const float m[3][3] = {
			{ source.xx, source.xy, source.xz },
			{ source.xy, source.yy, source.yz },
			{ source.xz, source.yz, source.zz }
		};
		float nm[3][3]{};
		for (uint32_t row = 0u; row < 3u; ++row)
		{
			for (uint32_t col = 0u; col < 3u; ++col)
			{
				for (uint32_t k = 0u; k < 3u; ++k)
				{
					nm[row][col] += n[row][k] * m[k][col];
				}
			}
		}

		float transformed[3][3]{};
		for (uint32_t row = 0u; row < 3u; ++row)
		{
			for (uint32_t col = 0u; col < 3u; ++col)
			{
				for (uint32_t k = 0u; k < 3u; ++k)
				{
					transformed[row][col] += nm[row][k] * n[col][k];
				}
			}
		}

		br::mesh::sggx::SymmetricMatrix3 result{
			transformed[0][0],
			transformed[1][1],
			transformed[2][2],
			0.5f * (transformed[0][1] + transformed[1][0]),
			0.5f * (transformed[0][2] + transformed[2][0]),
			0.5f * (transformed[1][2] + transformed[2][1])
		};

		const float sourceTrace = std::max(source.xx + source.yy + source.zz, 1.0e-8f);
		const float resultTrace = result.xx + result.yy + result.zz;
		if (resultTrace > 1.0e-8f)
		{
			result = result * (sourceTrace / resultTrace);
		}
		return result;
	}

	void ApplyChildPayloadSGGXToParentCells(
		VoxelGroupPayload& parentPayload,
		std::span<const VoxelSourcePayloadInstance> sourceInstances)
	{
		if (parentPayload.activeCells.empty() || parentPayload.voxelWidth <= 0.0f || parentPayload.resolution == 0u || sourceInstances.empty())
		{
			return;
		}

		struct SGGXAccum
		{
			br::mesh::sggx::SymmetricMatrix3 sum{};
			float weight = 0.0f;
		};

		std::unordered_set<VoxelCellRefinedKey, VoxelCellRefinedKeyHash> activeParentCells;
		activeParentCells.reserve(parentPayload.activeCells.size());
		for (const VoxelCell& parentCell : parentPayload.activeCells)
		{
			activeParentCells.insert(VoxelCellRefinedKey{
				PackVoxelTailCellKey(parentCell.x, parentCell.y, parentCell.z),
				parentCell.refinedGroup });
		}

		const float invParentVoxelWidth = 1.0f / parentPayload.voxelWidth;
		auto minCellCoord = [&](float value, float minValue) -> uint32_t
		{
			const int32_t coord = static_cast<int32_t>(std::floor((value - minValue) * invParentVoxelWidth));
			return static_cast<uint32_t>(std::clamp<int32_t>(coord, 0, static_cast<int32_t>(parentPayload.resolution) - 1));
		};
		auto maxCellCoord = [&](float value, float minValue) -> uint32_t
		{
			const int32_t coord = static_cast<int32_t>(std::ceil((value - minValue) * invParentVoxelWidth)) - 1;
			return static_cast<uint32_t>(std::clamp<int32_t>(coord, 0, static_cast<int32_t>(parentPayload.resolution) - 1));
		};
		auto parentCellMin = [&](uint32_t x, uint32_t y, uint32_t z) -> DirectX::XMFLOAT3
		{
			return DirectX::XMFLOAT3(
				parentPayload.aabbMin.x + static_cast<float>(x) * parentPayload.voxelWidth,
				parentPayload.aabbMin.y + static_cast<float>(y) * parentPayload.voxelWidth,
				parentPayload.aabbMin.z + static_cast<float>(z) * parentPayload.voxelWidth);
		};
		auto overlapAxis = [](float aMin, float aMax, float bMin, float bMax) -> float
		{
			return std::max(0.0f, std::min(aMax, bMax) - std::max(aMin, bMin));
		};
		auto overlapsParentAabb = [&](const DirectX::XMFLOAT3& minValue, const DirectX::XMFLOAT3& maxValue) -> bool
		{
			if (!std::isfinite(minValue.x) || !std::isfinite(minValue.y) || !std::isfinite(minValue.z) ||
				!std::isfinite(maxValue.x) || !std::isfinite(maxValue.y) || !std::isfinite(maxValue.z))
			{
				return false;
			}
			return maxValue.x > parentPayload.aabbMin.x &&
				maxValue.y > parentPayload.aabbMin.y &&
				maxValue.z > parentPayload.aabbMin.z &&
				minValue.x < parentPayload.aabbMax.x &&
				minValue.y < parentPayload.aabbMax.y &&
				minValue.z < parentPayload.aabbMax.z;
		};

		std::unordered_map<VoxelCellRefinedKey, SGGXAccum, VoxelCellRefinedKeyHash> accumulations;
		for (const VoxelSourcePayloadInstance& sourceInstance : sourceInstances)
		{
			const VoxelGroupPayload* sourcePayload = sourceInstance.payload;
			if (sourcePayload == nullptr || sourcePayload->activeCells.empty() || sourcePayload->voxelWidth <= 0.0f)
			{
				continue;
			}

			const bool hasRefinedGroupOverride = sourceInstance.refinedGroupOverride != std::numeric_limits<int32_t>::min();
			for (const VoxelCell& sourceCell : sourcePayload->activeCells)
			{
				const DirectX::XMFLOAT3 sourceMin = VoxelCellMin3(*sourcePayload, sourceCell);
				const DirectX::XMFLOAT3 sourceMax = VoxelCellMax3(*sourcePayload, sourceCell);
				DirectX::XMFLOAT3 transformedMin(
					std::numeric_limits<float>::max(),
					std::numeric_limits<float>::max(),
					std::numeric_limits<float>::max());
				DirectX::XMFLOAT3 transformedMax(
					std::numeric_limits<float>::lowest(),
					std::numeric_limits<float>::lowest(),
					std::numeric_limits<float>::lowest());
				ExpandAabbWithTransformedAabb3x4(sourceInstance.localToTarget, sourceMin, sourceMax, transformedMin, transformedMax);
				if (!overlapsParentAabb(transformedMin, transformedMax))
				{
					continue;
				}

				const int32_t refinedGroup = hasRefinedGroupOverride ? sourceInstance.refinedGroupOverride : sourceCell.refinedGroup;
				const uint32_t xMin = minCellCoord(transformedMin.x, parentPayload.aabbMin.x);
				const uint32_t yMin = minCellCoord(transformedMin.y, parentPayload.aabbMin.y);
				const uint32_t zMin = minCellCoord(transformedMin.z, parentPayload.aabbMin.z);
				const uint32_t xMax = maxCellCoord(transformedMax.x, parentPayload.aabbMin.x);
				const uint32_t yMax = maxCellCoord(transformedMax.y, parentPayload.aabbMin.y);
				const uint32_t zMax = maxCellCoord(transformedMax.z, parentPayload.aabbMin.z);
				const float transformedVolume = std::max(
					(transformedMax.x - transformedMin.x) *
					(transformedMax.y - transformedMin.y) *
					(transformedMax.z - transformedMin.z),
					1.0e-12f);
				const br::mesh::sggx::SymmetricMatrix3 transformedSGGX =
					TransformSGGX3x4(sourceInstance.localToTarget, br::mesh::sggx::DecodeAxialSGGX(sourceCell.sggxAxisAndSigmas));

				for (uint32_t z = zMin; z <= zMax; ++z)
				{
					for (uint32_t y = yMin; y <= yMax; ++y)
					{
						for (uint32_t x = xMin; x <= xMax; ++x)
						{
							const VoxelCellRefinedKey key{ PackVoxelTailCellKey(x, y, z), refinedGroup };
							if (activeParentCells.find(key) == activeParentCells.end())
							{
								continue;
							}

							const DirectX::XMFLOAT3 cellMin = parentCellMin(x, y, z);
							const DirectX::XMFLOAT3 cellMax(
								cellMin.x + parentPayload.voxelWidth,
								cellMin.y + parentPayload.voxelWidth,
								cellMin.z + parentPayload.voxelWidth);
							const float overlapVolume =
								overlapAxis(transformedMin.x, transformedMax.x, cellMin.x, cellMax.x) *
								overlapAxis(transformedMin.y, transformedMax.y, cellMin.y, cellMax.y) *
								overlapAxis(transformedMin.z, transformedMax.z, cellMin.z, cellMax.z);
							if (overlapVolume <= 1.0e-12f)
							{
								continue;
							}

							const float weight = std::max(sourceCell.opacity, 1.0e-6f) * (overlapVolume / transformedVolume);
							SGGXAccum& accum = accumulations[key];
							accum.sum = accum.sum + transformedSGGX * weight;
							accum.weight += weight;
						}
					}
				}
			}
		}

		for (VoxelCell& parentCell : parentPayload.activeCells)
		{
			const VoxelCellRefinedKey key{ PackVoxelTailCellKey(parentCell.x, parentCell.y, parentCell.z), parentCell.refinedGroup };
			const auto accumIt = accumulations.find(key);
			if (accumIt == accumulations.end() || accumIt->second.weight <= 1.0e-12f)
			{
				continue;
			}

			const br::mesh::sggx::SymmetricMatrix3 averagedSGGX = accumIt->second.sum * (1.0f / accumIt->second.weight);
			parentCell.sggxAxisAndSigmas = br::mesh::sggx::EncodeAxialSGGX(br::mesh::sggx::CompressSGGXToAxial(averagedSGGX));
		}
	}

	ClusterLODNode BuildAssemblyInternalNode(
		const std::vector<ClusterLODNode>& nodes,
		uint32_t childOffset,
		uint32_t childCount)
	{
		if (childCount == 0u || childOffset + childCount > nodes.size())
		{
			throw std::runtime_error("ClusterLOD assembly: invalid internal node child range");
		}

		ClusterLODNode node{};
		node.range.isGroup = CLOD_NODE_INTERNAL;
		node.range.indexOrOffset = childOffset;
		node.range.countMinusOne = childCount - 1u;

		float maxError = 0.0f;
		for (uint32_t childIndex = 0; childIndex < childCount; ++childIndex)
		{
			maxError = std::max(maxError, nodes[childOffset + childIndex].traversalMetric.maxQuadricError);
		}
		node.traversalMetric.maxQuadricError = maxError;

		meshopt_Bounds mergedCull = meshopt_computeSphereBounds(
			&nodes[childOffset].traversalMetric.cullingSphere.x,
			childCount,
			sizeof(ClusterLODNode),
			&nodes[childOffset].traversalMetric.cullingSphere.w,
			sizeof(ClusterLODNode));
		meshopt_Bounds mergedLod = meshopt_computeSphereBounds(
			&nodes[childOffset].traversalMetric.lodBoundingSphere.x,
			childCount,
			sizeof(ClusterLODNode),
			&nodes[childOffset].traversalMetric.lodBoundingSphere.w,
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
	}
}

ClusterLODPrebuildArtifacts BuildClusterLODAssemblyArtifacts(
	std::span<const ClusterLODAssemblyPart> parts,
	std::span<const ClusterLODAssemblyInstanceSpec> instances,
	const ClusterLODBuilderSettings& settings,
	uint32_t preferredNodeWidth,
	bool synthesizeVoxelParents)
{
	if (parts.empty())
	{
		throw std::runtime_error("ClusterLOD assembly: at least one part is required");
	}
	if (instances.empty())
	{
		throw std::runtime_error("ClusterLOD assembly: at least one instance is required");
	}

	preferredNodeWidth = std::max(2u, preferredNodeWidth);

	ClusterLODPrebuildArtifacts out{};
	ClusterLODBuildState state{};
	std::vector<ClusterLODNode> libraryNodes;
	std::vector<ClusterLODAssemblyTransform> assemblyTransforms;
	std::vector<ClusterLODAssemblyInstance> assemblyInstances;
	std::vector<ClusterLODAssemblyBoneRemap> assemblyBoneRemaps;
	std::vector<uint32_t> assemblyBoneRemapIndices;
	std::vector<std::vector<VoxelSourcePayloadInstance>> assemblyGroupSources;
	std::vector<std::vector<int32_t>> assemblyCoverageDomainMap;
	std::vector<VoxelSourceTrianglePart> assemblyCoverageParts;
	std::vector<VoxelSourceTriangleInstance> assemblyCoverageInstances;
	VoxelSourceTriangleBVH assemblyCoverageSourceTriangles;
	bool assemblyCoverageDoubleSidedTriangles = settings.doubleSidedVoxelSourceNormals;
	std::vector<uint32_t> groupBases(parts.size(), 0u);
	std::vector<uint32_t> segmentBases(parts.size(), 0u);
	std::vector<uint32_t> nodeBases(parts.size(), 0u);
	std::vector<uint32_t> transformBases(parts.size(), 0u);
	std::vector<uint32_t> instanceBases(parts.size(), 0u);
	std::vector<ClusterLODPartRecord> copiedPartRecords;
	assemblyCoverageParts.resize(parts.size());

	size_t assemblyStorageReserve = instances.size() * 2ull + 64ull;
	size_t transientPayloadReserve = instances.size() * 4ull + 64ull;
	for (const ClusterLODAssemblyPart& partRef : parts)
	{
		if (partRef.artifacts != nullptr)
		{
			assemblyStorageReserve += partRef.artifacts->prebuiltData.groups.size();
			transientPayloadReserve += partRef.artifacts->cacheBuildData.voxelGroupMapping.payloads.size();
		}
	}
	state.groups.reserve(assemblyStorageReserve);
	state.groupChunks.reserve(assemblyStorageReserve);
	state.groupPageBlobs.reserve(assemblyStorageReserve);
	state.traversalGroupMask.reserve(assemblyStorageReserve);
	assemblyGroupSources.reserve(assemblyStorageReserve);
	state.voxelCarryPayloads.reserve(assemblyStorageReserve);
	state.voxelGroupMapping.groupToPayloadIndex.reserve(assemblyStorageReserve);
	state.voxelGroupMapping.groupToPackedMetadataIndex.reserve(assemblyStorageReserve);
	state.voxelGroupMapping.payloads.reserve(transientPayloadReserve);

	for (size_t partIndex = 0; partIndex < parts.size(); ++partIndex)
	{
		const ClusterLODPrebuildArtifacts* partArtifacts = parts[partIndex].artifacts;
		if (partArtifacts == nullptr)
		{
			throw std::runtime_error("ClusterLOD assembly: null part artifact");
		}
		const ClusterLODPrebuiltData& part = partArtifacts->prebuiltData;
		if (part.groups.empty() || part.nodes.empty())
		{
			throw std::runtime_error("ClusterLOD assembly: part has no CLod hierarchy");
		}
		assemblyCoverageParts[partIndex] = VoxelSourceTrianglePart{
			.vertices = parts[partIndex].coverageVertices,
			.vertexStrideBytes = parts[partIndex].coverageVertexSize,
			.triangleIndices = parts[partIndex].coverageIndices };
		assemblyCoverageDoubleSidedTriangles =
			assemblyCoverageDoubleSidedTriangles || parts[partIndex].doubleSidedCoverageTriangles;

		groupBases[partIndex] = static_cast<uint32_t>(state.groups.size());
		segmentBases[partIndex] = static_cast<uint32_t>(state.segments.size());
		nodeBases[partIndex] = static_cast<uint32_t>(libraryNodes.size());
		transformBases[partIndex] = static_cast<uint32_t>(assemblyTransforms.size());
		instanceBases[partIndex] = static_cast<uint32_t>(assemblyInstances.size());

		const VoxelGroupMapping& partVoxelMapping = partArtifacts->cacheBuildData.voxelGroupMapping;
		const uint32_t payloadBase = static_cast<uint32_t>(state.voxelGroupMapping.payloads.size());
		const uint32_t metadataBase = static_cast<uint32_t>(state.voxelGroupMapping.packedGroupMetadata.size());
		const uint32_t clusterBase = static_cast<uint32_t>(state.voxelGroupMapping.packedClusterRecords.size());
		const uint32_t cubeBase = static_cast<uint32_t>(state.voxelGroupMapping.packedCubeRecords.size());
		const uint32_t attributeBase = static_cast<uint32_t>(state.voxelGroupMapping.packedAttributeSamples.size());
		state.voxelGroupMapping.payloads.insert(
			state.voxelGroupMapping.payloads.end(),
			partVoxelMapping.payloads.begin(),
			partVoxelMapping.payloads.end());
		for (VoxelGroupPackedMetadata metadata : partVoxelMapping.packedGroupMetadata)
		{
			metadata.firstCluster += clusterBase;
			metadata.firstCube += cubeBase;
			state.voxelGroupMapping.packedGroupMetadata.push_back(metadata);
		}
		for (CLodVoxelClusterRecord cluster : partVoxelMapping.packedClusterRecords)
		{
			cluster.firstCube += cubeBase;
			state.voxelGroupMapping.packedClusterRecords.push_back(cluster);
		}
		for (CLodVoxelCubeRecord cube : partVoxelMapping.packedCubeRecords)
		{
			cube.firstAttribute += attributeBase;
			state.voxelGroupMapping.packedCubeRecords.push_back(cube);
		}
		state.voxelGroupMapping.packedAttributeSamples.insert(
			state.voxelGroupMapping.packedAttributeSamples.end(),
			partVoxelMapping.packedAttributeSamples.begin(),
			partVoxelMapping.packedAttributeSamples.end());

		for (uint32_t localGroupIndex = 0; localGroupIndex < static_cast<uint32_t>(part.groups.size()); ++localGroupIndex)
		{
			const ClusterLODGroup& srcGroup = part.groups[localGroupIndex];
			ClusterLODGroup group = srcGroup;
			group.firstSegment = static_cast<uint32_t>(state.segments.size());
			group.firstMeshlet += 0u;
			group.firstGroupVertex += 0u;
			group.pageMapBase = 0u;
			if (group.parentGroupId >= 0)
			{
				group.parentGroupId += static_cast<int32_t>(groupBases[partIndex]);
			}

			for (uint32_t localSegmentOffset = 0; localSegmentOffset < srcGroup.segmentCount; ++localSegmentOffset)
			{
				const uint32_t localSegmentIndex = srcGroup.firstSegment + localSegmentOffset;
				if (localSegmentIndex >= part.segments.size())
				{
					throw std::runtime_error("ClusterLOD assembly: part segment range out of bounds");
				}

				ClusterLODGroupSegment segment = part.segments[localSegmentIndex];
				if (segment.refinedGroup >= 0)
				{
					segment.refinedGroup += static_cast<int32_t>(groupBases[partIndex]);
				}
				if (segment.meshletCount != 0u && srcGroup.pageCount != 0u)
				{
					if (segment.pageIndex < srcGroup.pageMapBase || segment.pageIndex >= srcGroup.pageMapBase + srcGroup.pageCount)
					{
						throw std::runtime_error("ClusterLOD assembly: part segment page index outside owning group page map");
					}
					segment.pageIndex -= srcGroup.pageMapBase;
				}
				state.segments.push_back(segment);
				if (localSegmentIndex < part.segmentBounds.size())
				{
					state.segmentBounds.push_back(part.segmentBounds[localSegmentIndex]);
				}
				else
				{
					state.segmentBounds.push_back({});
				}
			}

			state.groups.push_back(group);
			state.groupChunks.push_back(localGroupIndex < part.groupChunks.size() ? part.groupChunks[localGroupIndex] : ClusterLODGroupChunk{});
			if (part.groupPageReferenceOffsets.size() == part.groups.size() + 1ull &&
				localGroupIndex + 1u < part.groupPageReferenceOffsets.size() &&
				partArtifacts->cacheBuildData.meshPageBlobs.size() != 0u)
			{
				std::vector<std::vector<std::byte>> groupPages;
				const uint32_t pageRefBegin = part.groupPageReferenceOffsets[localGroupIndex];
				const uint32_t pageRefEnd = part.groupPageReferenceOffsets[localGroupIndex + 1u];
				groupPages.reserve(pageRefEnd - pageRefBegin);
				for (uint32_t pageRefIndex = pageRefBegin; pageRefIndex < pageRefEnd; ++pageRefIndex)
				{
					if (pageRefIndex >= part.groupPageReferences.size())
					{
						continue;
					}
					const uint32_t meshPageIndex = part.groupPageReferences[pageRefIndex];
					if (meshPageIndex < partArtifacts->cacheBuildData.meshPageBlobs.size())
					{
						groupPages.push_back(partArtifacts->cacheBuildData.meshPageBlobs[meshPageIndex]);
					}
				}
				state.groupPageBlobs.push_back(std::move(groupPages));
			}
			else if (localGroupIndex < partArtifacts->cacheBuildData.groupPageBlobs.size())
			{
				state.groupPageBlobs.push_back(partArtifacts->cacheBuildData.groupPageBlobs[localGroupIndex]);
			}
			else
			{
				state.groupPageBlobs.emplace_back();
			}
			state.traversalGroupMask.push_back(0u);
			assemblyGroupSources.emplace_back();
			state.voxelCarryPayloads.emplace_back();
			state.voxelGroupMapping.groupToPayloadIndex.push_back(-1);
			state.voxelGroupMapping.groupToPackedMetadataIndex.push_back(-1);

			if (localGroupIndex < partVoxelMapping.groupToPayloadIndex.size())
			{
				const int32_t localPayloadIndex = partVoxelMapping.groupToPayloadIndex[localGroupIndex];
				if (localPayloadIndex >= 0 && static_cast<size_t>(localPayloadIndex) < partVoxelMapping.payloads.size())
				{
					state.voxelGroupMapping.groupToPayloadIndex.back() = static_cast<int32_t>(payloadBase + static_cast<uint32_t>(localPayloadIndex));
				}
			}
			if (localGroupIndex < partVoxelMapping.groupToPackedMetadataIndex.size())
			{
				const int32_t localMetadataIndex = partVoxelMapping.groupToPackedMetadataIndex[localGroupIndex];
				if (localMetadataIndex >= 0 && static_cast<size_t>(localMetadataIndex) < partVoxelMapping.packedGroupMetadata.size())
				{
					state.voxelGroupMapping.groupToPackedMetadataIndex.back() = static_cast<int32_t>(metadataBase + static_cast<uint32_t>(localMetadataIndex));
				}
			}
		}

		for (ClusterLODNode node : part.nodes)
		{
			switch (node.range.isGroup)
			{
			case CLOD_NODE_INTERNAL:
				node.range.indexOrOffset += nodeBases[partIndex];
				break;
			case CLOD_NODE_VOXEL_LEAF:
				if (node.range.countMinusOne != 0u)
				{
					node.range.countMinusOne += groupBases[partIndex];
				}
				node.range.ownerGroupId += groupBases[partIndex];
				break;
			case CLOD_NODE_SEGMENT_LEAF:
				node.range.indexOrOffset += segmentBases[partIndex];
				if (node.range.countMinusOne != 0u)
				{
					node.range.countMinusOne += groupBases[partIndex];
				}
				node.range.ownerGroupId += groupBases[partIndex];
				break;
			case CLOD_NODE_INSTANCE_ROOT:
				node.range.indexOrOffset += instanceBases[partIndex];
				break;
			default:
				throw std::runtime_error("ClusterLOD assembly: unknown part node kind");
			}
			libraryNodes.push_back(node);
		}

		const uint32_t remapIndexBase = static_cast<uint32_t>(assemblyBoneRemapIndices.size());
		assemblyBoneRemapIndices.insert(
			assemblyBoneRemapIndices.end(),
			part.assemblyBoneRemapIndices.begin(),
			part.assemblyBoneRemapIndices.end());

		for (uint32_t localTransformIndex = 0; localTransformIndex < static_cast<uint32_t>(part.assemblyTransforms.size()); ++localTransformIndex)
		{
			assemblyTransforms.push_back(part.assemblyTransforms[localTransformIndex]);
			ClusterLODAssemblyBoneRemap remap{};
			if (localTransformIndex < part.assemblyBoneRemaps.size())
			{
				remap = part.assemblyBoneRemaps[localTransformIndex];
				if (remap.remapIndexBase != CLOD_ASSEMBLY_BONE_REMAP_SENTINEL)
				{
					remap.remapIndexBase += remapIndexBase;
				}
			}
			assemblyBoneRemaps.push_back(remap);
		}
		for (ClusterLODAssemblyInstance instance : part.assemblyInstances)
		{
			instance.targetRootNode += nodeBases[partIndex];
			if (instance.transformIndex != CLOD_ASSEMBLY_TRANSFORM_SENTINEL)
			{
				instance.transformIndex += transformBases[partIndex];
			}
			if (instance.stackDepth > CLOD_ASSEMBLY_MAX_STACK_DEPTH)
			{
				throw std::runtime_error("ClusterLOD assembly: nested part exceeds max stack depth");
			}
			assemblyInstances.push_back(instance);
		}

		if (!part.partRecords.empty())
		{
			for (ClusterLODPartRecord record : part.partRecords)
			{
				record.groupBase += groupBases[partIndex];
				record.nodeBase += nodeBases[partIndex];
				record.transformBase += transformBases[partIndex];
				record.instanceBase += instanceBases[partIndex];
				record.rootNode += nodeBases[partIndex];
				record.flags &= ~CLOD_PART_RECORD_FLAG_ROOT;
				copiedPartRecords.push_back(record);
			}
		}
		else
		{
			ClusterLODPartRecord record{};
			record.groupBase = groupBases[partIndex];
			record.groupCount = static_cast<uint32_t>(part.groups.size());
			record.nodeBase = nodeBases[partIndex];
			record.nodeCount = static_cast<uint32_t>(part.nodes.size());
			record.transformBase = transformBases[partIndex];
			record.transformCount = static_cast<uint32_t>(part.assemblyTransforms.size());
			record.instanceBase = instanceBases[partIndex];
			record.instanceCount = static_cast<uint32_t>(part.assemblyInstances.size());
			record.rootNode = nodeBases[partIndex];
			record.flags = 0u;
			copiedPartRecords.push_back(record);
		}
	}

	const uint32_t rootAssemblyGroupBase = static_cast<uint32_t>(state.groups.size());
	auto getVoxelPayloadForGroup = [&](uint32_t groupIndex) -> const VoxelGroupPayload*
	{
		if (groupIndex >= state.voxelGroupMapping.groupToPayloadIndex.size())
		{
			return nullptr;
		}
		const int32_t payloadIndex = state.voxelGroupMapping.groupToPayloadIndex[groupIndex];
		if (payloadIndex < 0 || static_cast<size_t>(payloadIndex) >= state.voxelGroupMapping.payloads.size())
		{
			return nullptr;
		}
		return &state.voxelGroupMapping.payloads[static_cast<size_t>(payloadIndex)];
	};

	auto getOrBuildVoxelPayloadForGroup = [&](uint32_t groupIndex) -> const VoxelGroupPayload*
	{
		if (const VoxelGroupPayload* payload = getVoxelPayloadForGroup(groupIndex))
		{
			return payload;
		}

		if (groupIndex >= state.voxelCarryPayloads.size())
		{
			return nullptr;
		}

		VoxelGroupPayload& carryPayload = state.voxelCarryPayloads[groupIndex];
		if (carryPayload.voxelWidth > 0.0f && !carryPayload.activeCells.empty())
		{
			return &carryPayload;
		}

		if (!BuildVoxelGroupPayloadFromPackedMapping(state.voxelGroupMapping, groupIndex, carryPayload))
		{
			return nullptr;
		}

		TracyPlot("CLOD.Assembly.UnpackedSourceCells", static_cast<int64_t>(carryPayload.activeCells.size()));
		return &carryPayload;
	};

	auto appendGroupStorage = [&](ClusterLODGroup group, bool includeInTraversal) -> uint32_t
	{
		const uint32_t groupIndex = static_cast<uint32_t>(state.groups.size());
		state.groups.push_back(group);
		state.groupChunks.emplace_back();
		state.groupPageBlobs.emplace_back();
		state.traversalGroupMask.push_back(includeInTraversal ? 1u : 0u);
		assemblyGroupSources.emplace_back();
		state.voxelCarryPayloads.emplace_back();
		state.voxelGroupMapping.groupToPayloadIndex.push_back(-1);
		state.voxelGroupMapping.groupToPackedMetadataIndex.push_back(-1);
		assemblyCoverageDomainMap.emplace_back();
		return groupIndex;
	};

	auto appendCoverageDomain = [&](uint32_t groupIndex, int32_t refinedGroup)
	{
		if (groupIndex >= assemblyCoverageDomainMap.size())
		{
			assemblyCoverageDomainMap.resize(static_cast<size_t>(groupIndex) + 1ull);
		}
		std::vector<int32_t>& domain = assemblyCoverageDomainMap[groupIndex];
		if (std::find(domain.begin(), domain.end(), refinedGroup) == domain.end())
		{
			domain.push_back(refinedGroup);
		}
	};

	auto buildAssemblyVoxelGroup = [&](std::span<const uint32_t> childGroups, int32_t depth) -> uint32_t
	{
		std::vector<VoxelSourcePayloadInstance> sourceInstances;
		float maxSourceVoxelWidth = 0.0f;
		sourceInstances.reserve(childGroups.size() * 2ull);
		for (uint32_t childGroup : childGroups)
		{
			for (VoxelSourcePayloadInstance source : assemblyGroupSources[childGroup])
			{
				if (source.payload == nullptr || source.payload->activeCells.empty() || source.payload->voxelWidth <= 0.0f)
				{
					continue;
				}
				source.refinedGroupOverride = static_cast<int32_t>(childGroup);
				sourceInstances.push_back(source);
				maxSourceVoxelWidth = std::max(maxSourceVoxelWidth, source.payload->voxelWidth * MaxScale3x4(source.localToTarget));
			}
		}
		if (sourceInstances.empty() || maxSourceVoxelWidth <= 0.0f)
		{
			throw std::runtime_error("ClusterLOD assembly: child assembly groups have no voxel payload sources");
		}

		DirectX::XMFLOAT3 aabbMin{};
		DirectX::XMFLOAT3 aabbMax{};
		if (!BuildAabbFromVoxelSourceCells(sourceInstances, aabbMin, aabbMax))
		{
			throw std::runtime_error("ClusterLOD assembly: child assembly groups have no finite voxel payload bounds");
		}
		const float extentX = aabbMax.x - aabbMin.x;
		const float extentY = aabbMax.y - aabbMin.y;
		const float extentZ = aabbMax.z - aabbMin.z;
		const float longestExtent = std::max({ extentX, extentY, extentZ });
		if (!std::isfinite(longestExtent) || longestExtent <= 1.0e-8f)
		{
			throw std::runtime_error("ClusterLOD assembly: degenerate assembly voxel bounds");
		}

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

		const float growthFactor = std::max(1.01f, settings.voxelFallbackGrowthFactor);
		const float baseResolution = static_cast<float>(std::max(2u, settings.voxelGridBaseResolution));
		const float voxelWidth = std::max({
			maxSourceVoxelWidth * growthFactor,
			longestExtent / baseResolution });
		const uint32_t resolution = std::max(
			std::max(2u, settings.voxelMinResolution),
			static_cast<uint32_t>(std::ceil(longestExtent / std::max(voxelWidth, 1.0e-8f))));
		const float voxelRepresentationError = ComputeVoxelRepresentationError(voxelWidth);
		const float sourceToParentRatio = std::max(1.0f, voxelWidth / std::max(maxSourceVoxelWidth, 1.0e-8f));
		const uint32_t rayScale = std::clamp(
			static_cast<uint32_t>(std::ceil(sourceToParentRatio * sourceToParentRatio)),
			1u,
			32u);

		VoxelizeTrianglesInput voxelInput{};
		voxelInput.sourceVoxelPayloadInstances = &sourceInstances;
		voxelInput.candidateVoxelPayloadInstances = &sourceInstances;
		voxelInput.coverageSourceTriangles = assemblyCoverageSourceTriangles.IsValid() ? &assemblyCoverageSourceTriangles : nullptr;
		voxelInput.aabbMin = aabbMin;
		voxelInput.aabbMax = aabbMax;
		voxelInput.voxelWidth = voxelWidth;
		voxelInput.resolution = resolution;
		voxelInput.raysPerCell = std::max(1u, settings.voxelRaysPerCell) * rayScale;
		voxelInput.emitSourcePayload = false;
		if (assemblyCoverageSourceTriangles.IsValid())
		{
			assemblyCoverageSourceTriangles.SetRefinedGroupDomainMap(assemblyCoverageDomainMap);
		}
		VoxelizeTrianglesResult voxelResult = VoxelizeTrianglesDetailed(voxelInput);
		if (voxelResult.renderPayload.activeCells.empty())
		{
			throw std::runtime_error("ClusterLOD assembly: assembly voxelization produced no render cells");
		}
		ApplyChildPayloadSGGXToParentCells(voxelResult.renderPayload, sourceInstances);

		const uint32_t firstCluster = static_cast<uint32_t>(state.voxelGroupMapping.packedClusterRecords.size());
		const uint32_t firstCube = static_cast<uint32_t>(state.voxelGroupMapping.packedCubeRecords.size());
		const uint32_t firstAttribute = static_cast<uint32_t>(state.voxelGroupMapping.packedAttributeSamples.size());
		PackVoxelGroupInput packInput{};
		packInput.payload = &voxelResult.renderPayload;
		packInput.voxelError = voxelRepresentationError;
		packInput.opacityThreshold = settings.voxelFallbackOpacityThreshold;
		packInput.dominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
		packInput.firstCube = firstCube;
		packInput.firstAttribute = firstAttribute;
		PackedVoxelGroupBuildResult packed = PackVoxelGroupToCubes(packInput);
		packed.metadata.firstCluster = firstCluster;
		BuildVoxelClustersFromCubes(packed, CLOD_VOXEL_MAX_CUBES_PER_CLUSTER);
		if (packed.cubeRecords.empty() || packed.clusterRecords.empty())
		{
			throw std::runtime_error("ClusterLOD assembly: assembly voxel pack produced no clusters");
		}

		std::vector<ClusterLODGroupSegment> voxelSegments;
		std::vector<BoundingSphere> voxelSegmentBounds;
		SplitVoxelClustersIntoPageSegments(packed, voxelSegments, voxelSegmentBounds);
		std::vector<std::vector<std::byte>> voxelPageBlobs = BuildVoxelGroupPageBlobs(
			voxelSegments,
			packed.clusterRecords,
			packed.cubeRecords,
			packed.attributeSamples,
			firstAttribute);
		if (voxelSegments.empty() || voxelPageBlobs.empty())
		{
			throw std::runtime_error("ClusterLOD assembly: assembly voxel page build produced no pages");
		}

		ClusterLODGroup group{};
		group.bounds.center[0] = 0.5f * (aabbMin.x + aabbMax.x);
		group.bounds.center[1] = 0.5f * (aabbMin.y + aabbMax.y);
		group.bounds.center[2] = 0.5f * (aabbMin.z + aabbMax.z);
		const float dx = aabbMax.x - group.bounds.center[0];
		const float dy = aabbMax.y - group.bounds.center[1];
		const float dz = aabbMax.z - group.bounds.center[2];
		group.bounds.radius = std::sqrt(dx * dx + dy * dy + dz * dz);
		group.bounds.error = voxelRepresentationError;
		group.depth = depth;
		group.flags = CLOD_GROUP_FLAG_IS_VOXEL | CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL;
		group.firstSegment = static_cast<uint32_t>(state.segments.size());
		group.segmentCount = static_cast<uint32_t>(voxelSegments.size());
		group.terminalSegmentCount = 0u;
		group.pageCount = static_cast<uint32_t>(voxelPageBlobs.size());
		group.representationError = voxelRepresentationError;

		const uint32_t groupIndex = appendGroupStorage(group, true);
		for (uint32_t childGroup : childGroups)
		{
			if (childGroup < assemblyCoverageDomainMap.size())
			{
				for (int32_t refinedGroup : assemblyCoverageDomainMap[childGroup])
				{
					appendCoverageDomain(groupIndex, refinedGroup);
				}
			}
		}
		state.segments.insert(state.segments.end(), voxelSegments.begin(), voxelSegments.end());
		state.segmentBounds.insert(state.segmentBounds.end(), voxelSegmentBounds.begin(), voxelSegmentBounds.end());
		state.groupPageBlobs[groupIndex] = std::move(voxelPageBlobs);
		state.voxelGroupMapping.packedGroupMetadata.push_back(packed.metadata);
		state.voxelGroupMapping.groupToPackedMetadataIndex[groupIndex] = static_cast<int32_t>(state.voxelGroupMapping.packedGroupMetadata.size() - 1u);
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

		assemblyGroupSources[groupIndex].reserve(sourceInstances.size());
		for (VoxelSourcePayloadInstance source : sourceInstances)
		{
			source.refinedGroupOverride = static_cast<int32_t>(groupIndex);
			assemblyGroupSources[groupIndex].push_back(source);
		}

		for (uint32_t childGroup : childGroups)
		{
			state.groups[childGroup].parentGroupId = static_cast<int32_t>(groupIndex);
		}
		return groupIndex;
	};

	std::vector<uint32_t> currentLayer;
	currentLayer.reserve(instances.size());

	DirectX::XMFLOAT3 assemblyInstanceAabbMin(
		std::numeric_limits<float>::max(),
		std::numeric_limits<float>::max(),
		std::numeric_limits<float>::max());
	DirectX::XMFLOAT3 assemblyInstanceAabbMax(
		-std::numeric_limits<float>::max(),
		-std::numeric_limits<float>::max(),
		-std::numeric_limits<float>::max());
	for (const ClusterLODAssemblyInstanceSpec& spec : instances)
	{
		if (spec.partIndex >= parts.size())
		{
			continue;
		}
		const ClusterLODPrebuiltData& part = parts[spec.partIndex].artifacts->prebuiltData;
		if (spec.rootNode >= part.nodes.size())
		{
			continue;
		}
		const DirectX::XMFLOAT4 sphere = TransformSphere3x4(spec.transform, part.nodes[spec.rootNode].traversalMetric.lodBoundingSphere);
		assemblyInstanceAabbMin.x = std::min(assemblyInstanceAabbMin.x, sphere.x - sphere.w);
		assemblyInstanceAabbMin.y = std::min(assemblyInstanceAabbMin.y, sphere.y - sphere.w);
		assemblyInstanceAabbMin.z = std::min(assemblyInstanceAabbMin.z, sphere.z - sphere.w);
		assemblyInstanceAabbMax.x = std::max(assemblyInstanceAabbMax.x, sphere.x + sphere.w);
		assemblyInstanceAabbMax.y = std::max(assemblyInstanceAabbMax.y, sphere.y + sphere.w);
		assemblyInstanceAabbMax.z = std::max(assemblyInstanceAabbMax.z, sphere.z + sphere.w);
	}
	const float assemblyInstanceLongestExtent = std::max({
		assemblyInstanceAabbMax.x - assemblyInstanceAabbMin.x,
		assemblyInstanceAabbMax.y - assemblyInstanceAabbMin.y,
		assemblyInstanceAabbMax.z - assemblyInstanceAabbMin.z });
	const float assemblyBaselineVoxelWidth =
		std::isfinite(assemblyInstanceLongestExtent) && assemblyInstanceLongestExtent > 0.0f
		? assemblyInstanceLongestExtent / static_cast<float>(std::max(2u, settings.voxelGridBaseResolution))
		: 0.0f;

	auto collectVoxelTailChain = [&](uint32_t globalRootGroupIndex)
	{
		std::vector<uint32_t> chain;
		uint32_t groupIndex = globalRootGroupIndex;
		std::unordered_set<uint32_t> visited;
		while (groupIndex < state.groups.size() && visited.insert(groupIndex).second)
		{
			const ClusterLODGroup& group = state.groups[groupIndex];
			if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) == 0u)
			{
				break;
			}
			chain.push_back(groupIndex);
			std::vector<uint32_t> children = CollectUniqueRefinedChildren(state, groupIndex);
			if (children.size() != 1u)
			{
				break;
			}
			const uint32_t childGroupIndex = children.front();
			if (childGroupIndex >= state.groups.size() ||
				(state.groups[childGroupIndex].flags & CLOD_GROUP_FLAG_IS_VOXEL) == 0u)
			{
				break;
			}
			groupIndex = childGroupIndex;
		}
		return chain;
	};

	auto selectAssemblySourceGroup = [&](const std::vector<uint32_t>& chain, float instanceScale) -> uint32_t
	{
		if (chain.empty())
		{
			return std::numeric_limits<uint32_t>::max();
		}
		const float growthFactor = std::max(1.01f, settings.voxelTailGrowthFactor);
		const float targetSourceWidth = assemblyBaselineVoxelWidth > 0.0f
			? assemblyBaselineVoxelWidth / growthFactor
			: std::numeric_limits<float>::infinity();
		uint32_t selectedGroup = chain.back();
		for (uint32_t groupIndex : chain)
		{
			const float localWidth = GetFiniteVoxelErrorForGroup(state, groupIndex);
			const float worldWidth = localWidth * instanceScale;
			if (std::isfinite(worldWidth) && worldWidth > 0.0f && worldWidth <= targetSourceWidth)
			{
				selectedGroup = groupIndex;
				break;
			}
		}
		return selectedGroup;
	};

	for (const ClusterLODAssemblyInstanceSpec& spec : instances)
	{
		if (spec.partIndex >= parts.size())
		{
			throw std::runtime_error("ClusterLOD assembly: instance part index out of range");
		}
		const ClusterLODPrebuiltData& part = parts[spec.partIndex].artifacts->prebuiltData;
		if (spec.rootNode >= part.nodes.size())
		{
			throw std::runtime_error("ClusterLOD assembly: instance root node out of range");
		}

		const uint32_t targetRoot = nodeBases[spec.partIndex] + spec.rootNode;
		const ClusterLODNode& targetNode = libraryNodes[targetRoot];
		const uint32_t transformIndex = static_cast<uint32_t>(assemblyTransforms.size());
		assemblyTransforms.push_back(spec.transform);
		ClusterLODAssemblyBoneRemap boneRemap{};
		if (!spec.boneRemapIndices.empty())
		{
			boneRemap.remapIndexBase = static_cast<uint32_t>(assemblyBoneRemapIndices.size());
			boneRemap.remapIndexCount = static_cast<uint32_t>(spec.boneRemapIndices.size());
			assemblyBoneRemapIndices.insert(
				assemblyBoneRemapIndices.end(),
				spec.boneRemapIndices.begin(),
				spec.boneRemapIndices.end());
		}
		else if (spec.boneRemapBase != CLOD_ASSEMBLY_BONE_REMAP_SENTINEL && spec.boneRemapCount != 0u)
		{
			boneRemap.remapIndexBase = spec.boneRemapBase;
			boneRemap.remapIndexCount = spec.boneRemapCount;
		}
		assemblyBoneRemaps.push_back(boneRemap);

		ClusterLODAssemblyInstance assemblyInstance{};
		assemblyInstance.targetRootNode = targetRoot;
		assemblyInstance.transformIndex = transformIndex;
		assemblyInstance.flags = spec.flags;
		assemblyInstance.stackDepth = 1u;
		const uint32_t assemblyInstanceIndex = static_cast<uint32_t>(assemblyInstances.size());
		assemblyInstances.push_back(assemblyInstance);

		const float instanceErrorScale = MaxScale3x4(spec.transform);
		float proxyTraversalError = 0.0f;
		if (IsFiniteContentTraversalError(targetNode.traversalMetric.maxQuadricError))
		{
			proxyTraversalError = std::max(proxyTraversalError, targetNode.traversalMetric.maxQuadricError);
		}
		for (const ClusterLODGroup& partGroup : part.groups)
		{
			if (IsFiniteContentTraversalError(partGroup.bounds.error))
			{
				proxyTraversalError = std::max(proxyTraversalError, partGroup.bounds.error);
			}
			if (std::isfinite(partGroup.representationError) && partGroup.representationError > 0.0f)
			{
				proxyTraversalError = std::max(proxyTraversalError, partGroup.representationError);
			}
		}
		proxyTraversalError *= instanceErrorScale;

		const DirectX::XMFLOAT4 lodSphere = TransformSphere3x4(spec.transform, targetNode.traversalMetric.lodBoundingSphere);
		ClusterLODGroup proxyGroup{};
		proxyGroup.bounds.center[0] = lodSphere.x;
		proxyGroup.bounds.center[1] = lodSphere.y;
		proxyGroup.bounds.center[2] = lodSphere.z;
		proxyGroup.bounds.radius = lodSphere.w;
		proxyGroup.bounds.error = proxyTraversalError;
		proxyGroup.depth = 0;
		proxyGroup.flags = CLOD_GROUP_FLAG_IS_ASSEMBLY_PROXY;
		proxyGroup.firstMeshlet = assemblyInstanceIndex;
		const uint32_t proxyGroupIndex = appendGroupStorage(proxyGroup, true);
		currentLayer.push_back(proxyGroupIndex);
		appendCoverageDomain(proxyGroupIndex, static_cast<int32_t>(proxyGroupIndex));
		assemblyCoverageInstances.push_back(VoxelSourceTriangleInstance{
			.partIndex = spec.partIndex,
			.localToWorld = spec.transform,
			.refinedGroup = static_cast<int32_t>(proxyGroupIndex) });

		uint32_t selectedSourceGroups = 0u;
		float selectedMaxWorldVoxelWidth = 0.0f;
		for (uint32_t localGroupIndex = 0; localGroupIndex < static_cast<uint32_t>(part.groups.size()); ++localGroupIndex)
		{
			const ClusterLODGroup& localGroup = part.groups[localGroupIndex];
			if (localGroup.parentGroupId >= 0)
			{
				continue;
			}
			const uint32_t globalGroupIndex = groupBases[spec.partIndex] + localGroupIndex;
			const std::vector<uint32_t> chain = collectVoxelTailChain(globalGroupIndex);
			const uint32_t selectedGroupIndex = selectAssemblySourceGroup(chain, instanceErrorScale);
			if (selectedGroupIndex == std::numeric_limits<uint32_t>::max())
			{
				continue;
			}
			const VoxelGroupPayload* payload = getOrBuildVoxelPayloadForGroup(selectedGroupIndex);
			if (payload == nullptr)
			{
				continue;
			}
			selectedSourceGroups++;
			selectedMaxWorldVoxelWidth = std::max(selectedMaxWorldVoxelWidth, payload->voxelWidth * instanceErrorScale);
			assemblyGroupSources[proxyGroupIndex].push_back(VoxelSourcePayloadInstance{
				.payload = payload,
				.localToTarget = spec.transform,
				.expansionRadius = GetVoxelCandidateExpansionRadiusForPayload(payload) * MaxScale3x4(spec.transform),
				.refinedGroupOverride = static_cast<int32_t>(proxyGroupIndex) });
		}
		if (selectedSourceGroups != 0u)
		{
			TracyPlot("CLOD.Assembly.SelectedSourceGroups", static_cast<int64_t>(selectedSourceGroups));
			TracyPlot("CLOD.Assembly.SelectedSourceWorldVoxelMicrons", static_cast<int64_t>(selectedMaxWorldVoxelWidth * 1000000.0f));
		}
	}

	if (currentLayer.empty())
	{
		throw std::runtime_error("ClusterLOD assembly: no proxy groups were produced");
	}
	if (synthesizeVoxelParents)
	{
		ZoneScopedN("ClusterLODUtilities::Assembly::BuildCoverageBVH");
		if (assemblyCoverageInstances.empty())
		{
			throw std::runtime_error("ClusterLOD assembly: parent voxel synthesis requires source triangle coverage geometry");
		}
		assemblyCoverageSourceTriangles.BuildInstanced(
			assemblyCoverageParts,
			assemblyCoverageInstances,
			assemblyCoverageDoubleSidedTriangles);
		if (!assemblyCoverageSourceTriangles.IsValid())
		{
			throw std::runtime_error("ClusterLOD assembly: failed to build parent voxel coverage BVH");
		}
		assemblyCoverageSourceTriangles.SetRefinedGroupDomainMap(assemblyCoverageDomainMap);
	}

	uint32_t assemblyDepth = 1u;
	while (synthesizeVoxelParents && currentLayer.size() > 1u)
	{
		std::vector<uint32_t> ordered = currentLayer;
		if (ordered.size() > preferredNodeWidth)
		{
			std::vector<uint32_t> partitioned(ordered.size());
			std::vector<DirectX::XMFLOAT4> spheres;
			spheres.reserve(ordered.size());
			for (uint32_t groupIndex : ordered)
			{
				const ClusterLODGroup& group = state.groups[groupIndex];
				spheres.push_back(DirectX::XMFLOAT4(
					group.bounds.center[0],
					group.bounds.center[1],
					group.bounds.center[2],
					group.bounds.radius));
			}
			meshopt_spatialClusterPoints(
				partitioned.data(),
				&spheres[0].x,
				static_cast<uint32_t>(spheres.size()),
				sizeof(DirectX::XMFLOAT4),
				preferredNodeWidth);
			std::vector<uint32_t> scratch = ordered;
			for (uint32_t i = 0; i < static_cast<uint32_t>(ordered.size()); ++i)
			{
				ordered[i] = scratch[partitioned[i]];
			}
		}

		std::vector<uint32_t> nextLayer;
		const uint32_t groupFanout = ordered.size() <= preferredNodeWidth
			? static_cast<uint32_t>(ordered.size())
			: preferredNodeWidth;
		for (uint32_t begin = 0; begin < static_cast<uint32_t>(ordered.size()); begin += groupFanout)
		{
			const uint32_t childCount = std::min<uint32_t>(groupFanout, static_cast<uint32_t>(ordered.size()) - begin);
			nextLayer.push_back(buildAssemblyVoxelGroup(
				std::span<const uint32_t>(ordered.data() + begin, childCount),
				static_cast<int32_t>(assemblyDepth)));
		}
		currentLayer = std::move(nextLayer);
		++assemblyDepth;
	}

	if (synthesizeVoxelParents && currentLayer.size() == 1u)
	{
		ClusterLODGroup& rootGroup = state.groups[currentLayer.front()];
		if ((rootGroup.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) != 0u)
		{
			rootGroup.bounds.error = std::numeric_limits<float>::max();
		}
	}

	uint32_t assemblyChildBoundaryRewrites = 0u;
	for (uint32_t parentGroupIndex = 0; parentGroupIndex < static_cast<uint32_t>(state.groups.size()); ++parentGroupIndex)
	{
		const ClusterLODGroup& parentGroup = state.groups[parentGroupIndex];
		if ((parentGroup.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) == 0u ||
			!std::isfinite(parentGroup.representationError) ||
			parentGroup.representationError <= 0.0f)
		{
			continue;
		}

		if (parentGroup.firstSegment + parentGroup.segmentCount > state.segments.size())
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
			if (childGroupIndex >= state.groups.size())
			{
				continue;
			}

			ClusterLODGroup& childGroup = state.groups[childGroupIndex];
			if (IsTerminalErrorSentinel(childGroup.bounds.error))
			{
				continue;
			}

			childGroup.bounds.error = parentGroup.representationError;
			assemblyChildBoundaryRewrites++;
		}
	}
	if (assemblyChildBoundaryRewrites != 0u)
	{
		spdlog::debug(
			"ClusterLOD assembly rewrote {} child traversal boundaries from parent voxel representation errors",
			assemblyChildBoundaryRewrites);
	}

	if (!synthesizeVoxelParents)
	{
		spdlog::debug(
			"ClusterLOD assembly using direct instance-root traversal: parts={} instances={} proxy_groups={}",
			parts.size(),
			instances.size(),
			currentLayer.size());
	}

	uint32_t assemblyParentErrorRaises = 0u;
	bool raisedAssemblyParentError = true;
	while (raisedAssemblyParentError)
	{
		raisedAssemblyParentError = false;
		for (uint32_t groupIndex = 0; groupIndex < static_cast<uint32_t>(state.groups.size()); ++groupIndex)
		{
			ClusterLODGroup& group = state.groups[groupIndex];
			if ((group.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) == 0u ||
				IsTerminalErrorSentinel(group.bounds.error))
			{
				continue;
			}

			float maxChildError = 0.0f;
			bool hasFiniteChild = false;
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

				const float childError = state.groups[childGroupIndex].bounds.error;
				if (IsFiniteContentTraversalError(childError))
				{
					maxChildError = std::max(maxChildError, childError);
					hasFiniteChild = true;
				}
			}

			if (!hasFiniteChild || group.bounds.error > maxChildError)
			{
				continue;
			}

			group.bounds.error = std::nextafter(maxChildError, std::numeric_limits<float>::infinity());
			assemblyParentErrorRaises++;
			raisedAssemblyParentError = true;
		}
	}
	if (assemblyParentErrorRaises != 0u)
	{
		spdlog::debug(
			"ClusterLOD assembly raised {} assembly voxel parent traversal errors to preserve monotonic cuts",
			assemblyParentErrorRaises);
	}

	const uint32_t rootAssemblyGroupEnd = static_cast<uint32_t>(state.groups.size());
	BuildClusterLODTraversalHierarchy(state, preferredNodeWidth);

	const uint32_t libraryNodeBase = static_cast<uint32_t>(state.nodes.size());
	std::vector<ClusterLODPartRecord> partRecords;
	{
		ClusterLODPartRecord rootPart{};
		rootPart.groupBase = rootAssemblyGroupBase;
		rootPart.groupCount = rootAssemblyGroupEnd - rootAssemblyGroupBase;
		rootPart.nodeBase = 0u;
		rootPart.nodeCount = libraryNodeBase;
		rootPart.transformBase = 0u;
		rootPart.transformCount = static_cast<uint32_t>(assemblyTransforms.size());
		rootPart.instanceBase = 0u;
		rootPart.instanceCount = static_cast<uint32_t>(assemblyInstances.size());
		rootPart.rootNode = state.topRootNode;
		rootPart.flags = CLOD_PART_RECORD_FLAG_ROOT;
		partRecords.push_back(rootPart);

		for (ClusterLODPartRecord record : copiedPartRecords)
		{
			record.nodeBase += libraryNodeBase;
			record.rootNode += libraryNodeBase;
			record.flags &= ~CLOD_PART_RECORD_FLAG_ROOT;
			partRecords.push_back(record);
		}
	}
	for (ClusterLODNode node : libraryNodes)
	{
		if (node.range.isGroup == CLOD_NODE_INTERNAL)
		{
			node.range.indexOrOffset += libraryNodeBase;
		}
		state.nodes.push_back(node);
	}

	for (ClusterLODAssemblyInstance& instance : assemblyInstances)
	{
		instance.targetRootNode += libraryNodeBase;
	}
	const uint32_t topAssemblyTraversalDepth = ComputeCLodTraversalDepth(state.nodes, state.topRootNode);
	uint32_t maxAssemblyTargetTraversalDepth = 0u;
	for (const ClusterLODAssemblyInstance& instance : assemblyInstances)
	{
		maxAssemblyTargetTraversalDepth = std::max(
			maxAssemblyTargetTraversalDepth,
			ComputeCLodTraversalDepth(state.nodes, instance.targetRootNode));
	}
	state.maxTraversalDepth = topAssemblyTraversalDepth + maxAssemblyTargetTraversalDepth;

	std::vector<std::vector<std::byte>> meshPageBlobs;
	std::vector<uint32_t> groupPageReferences;
	std::vector<uint32_t> groupPageReferenceOffsets;
	uint32_t trianglePageCount = 0u;
	uint32_t voxelPageBase = 0u;
	uint32_t voxelPageCount = 0u;
	{
		ZoneScopedN("ClusterLODUtilities::Assembly::FinalizeMeshWidePagePacking");
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

	out.prebuiltData.groups = std::move(state.groups);
	out.prebuiltData.segments = std::move(state.segments);
	out.prebuiltData.segmentBounds = std::move(state.segmentBounds);
	out.prebuiltData.objectBoundingSphere = BuildObjectBoundingSphereFromRootNode(state.nodes, state.topRootNode);
	out.prebuiltData.groupChunks = std::move(state.groupChunks);
	out.prebuiltData.groupPageReferences = std::move(groupPageReferences);
	out.prebuiltData.groupPageReferenceOffsets = std::move(groupPageReferenceOffsets);
	out.prebuiltData.trianglePageCount = trianglePageCount;
	out.prebuiltData.voxelPageBase = voxelPageBase;
	out.prebuiltData.voxelPageCount = voxelPageCount;
	out.prebuiltData.nodes = std::move(state.nodes);
	out.prebuiltData.lodNodeRanges = std::move(state.lodNodeRanges);
	out.prebuiltData.lodLevelRoots = std::move(state.lodLevelRoots);
	out.prebuiltData.assemblyTransforms = std::move(assemblyTransforms);
	out.prebuiltData.assemblyInstances = std::move(assemblyInstances);
	out.prebuiltData.assemblyBoneRemaps = std::move(assemblyBoneRemaps);
	out.prebuiltData.assemblyBoneRemapIndices = std::move(assemblyBoneRemapIndices);
	out.prebuiltData.partRecords = std::move(partRecords);
	out.prebuiltData.rootPartIndex = 0u;
	out.prebuiltData.maxDepth = state.maxDepth;
	out.prebuiltData.maxTraversalDepth = state.maxTraversalDepth;
	out.cacheBuildData.groupPageBlobs = std::move(state.groupPageBlobs);
	out.cacheBuildData.voxelGroupMapping = std::move(state.voxelGroupMapping);
	out.cacheBuildData.meshPageBlobs = std::move(meshPageBlobs);

	return out;
}

ClusterLODPrebuildArtifacts BuildVoxelOnlyClusterLODArtifactsFromGeometry(
	const std::vector<std::byte>& vertices,
	unsigned int vertexSize,
	const std::vector<uint32_t>& indices,
	const ClusterLODBuilderSettings& settings,
	const std::optional<ClusterLODVoxelGridOverride>& gridOverride,
	uint32_t maxCubesPerCluster)
{
	ZoneScopedN("ClusterLODUtilities::BuildVoxelOnlyClusterLODArtifactsFromGeometry");
	ClusterLODPrebuildArtifacts artifacts{};
	const size_t vertexStrideBytes = vertexSize;
	const size_t vertexCount = vertexStrideBytes != 0u ? vertices.size() / vertexStrideBytes : 0u;
	TracyPlot("CLOD.VoxelOnly.Vertices", static_cast<int64_t>(vertexCount));
	TracyPlot("CLOD.VoxelOnly.Triangles", static_cast<int64_t>(indices.size() / 3u));
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
	{
		ZoneScopedN("ClusterLODUtilities::VoxelOnlyGeometry::BuildCoverageBVH");
		coverageSourceTriangles.Build(
			&vertices,
			vertexStrideBytes,
			&indices,
			nullptr,
			0u,
			nullptr,
			settings.doubleSidedVoxelSourceNormals);
	}

	VoxelizeTrianglesInput voxelInput{};
	voxelInput.vertices = &vertices;
	voxelInput.vertexStrideBytes = vertexStrideBytes;
	voxelInput.triangleIndices = &indices;
	voxelInput.doubleSidedTriangles = settings.doubleSidedVoxelSourceNormals;
	voxelInput.coverageSourceTriangles = coverageSourceTriangles.IsValid() ? &coverageSourceTriangles : nullptr;
	voxelInput.aabbMin = aabbMin;
	voxelInput.aabbMax = aabbMax;
	voxelInput.voxelWidth = voxelWidth;
	voxelInput.resolution = voxelResolution;
	voxelInput.raysPerCell = settings.voxelRaysPerCell;
	voxelInput.emitSourcePayload = false;
	VoxelizeTrianglesResult voxelResult;
	{
		ZoneScopedN("ClusterLODUtilities::VoxelOnlyGeometry::Voxelize");
		voxelResult = VoxelizeTrianglesDetailed(voxelInput);
	}
	if (voxelResult.renderPayload.activeCells.empty())
	{
		return artifacts;
	}

	const float voxelRepresentationError = ComputeVoxelRepresentationError(voxelWidth);
	PackVoxelGroupInput packInput{};
	packInput.payload = &voxelResult.renderPayload;
	packInput.voxelError = voxelRepresentationError;
	packInput.opacityThreshold = settings.voxelFallbackOpacityThreshold;
	packInput.dominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
	PackedVoxelGroupBuildResult packed;
	{
		ZoneScopedN("ClusterLODUtilities::VoxelOnlyGeometry::PackAndCluster");
		packed = PackVoxelGroupToCubes(packInput);
		BuildVoxelClustersFromCubes(packed, std::max(1u, maxCubesPerCluster));
	}
	if (packed.cubeRecords.empty() || packed.clusterRecords.empty())
	{
		return artifacts;
	}

	std::vector<ClusterLODGroupSegment> voxelSegments;
	std::vector<BoundingSphere> voxelSegmentBounds;
	{
		ZoneScopedN("ClusterLODUtilities::VoxelOnlyGeometry::SplitVoxelPageSegments");
		SplitVoxelClustersIntoPageSegments(packed, voxelSegments, voxelSegmentBounds);
	}
	std::vector<std::vector<std::byte>> voxelPageBlobs;
	{
		ZoneScopedN("ClusterLODUtilities::VoxelOnlyGeometry::BuildVoxelPageBlobs");
		voxelPageBlobs = BuildVoxelGroupPageBlobs(
			voxelSegments,
			packed.clusterRecords,
			packed.cubeRecords,
			packed.attributeSamples,
			0u);
	}
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
	group.representationError = voxelRepresentationError;

	state.groups.push_back(group);
	state.segments = std::move(voxelSegments);
	state.segmentBounds = std::move(voxelSegmentBounds);
	state.groupChunks.resize(1);
	state.groupPageBlobs.resize(1);
	state.groupPageBlobs[0] = std::move(voxelPageBlobs);
	state.voxelGroupMapping.groupToPayloadIndex = { 0 };
	state.voxelGroupMapping.groupToPackedMetadataIndex = { 0 };
	state.voxelGroupMapping.payloads.push_back(std::move(voxelResult.renderPayload));
	state.voxelGroupMapping.packedGroupMetadata.push_back(packed.metadata);
	state.voxelGroupMapping.packedClusterRecords = std::move(packed.clusterRecords);
	state.voxelGroupMapping.packedCubeRecords = std::move(packed.cubeRecords);
	state.voxelGroupMapping.packedAttributeSamples = std::move(packed.attributeSamples);

	{
		ZoneScopedN("ClusterLODUtilities::VoxelOnlyGeometry::BuildTraversalHierarchy");
		BuildClusterLODTraversalHierarchy(state, /*preferredNodeWidth=*/8u);
	}

	std::vector<std::vector<std::byte>> meshPageBlobs;
	std::vector<uint32_t> groupPageReferences;
	std::vector<uint32_t> groupPageReferenceOffsets;
	uint32_t trianglePageCount = 0u;
	uint32_t voxelPageBase = 0u;
	uint32_t voxelPageCount = 0u;
	{
		ZoneScopedN("ClusterLODUtilities::VoxelOnlyGeometry::FinalizeMeshWidePagePacking");
		FinalizeMeshWidePagePacking(
			state,
			meshPageBlobs,
			groupPageReferences,
			groupPageReferenceOffsets,
			trianglePageCount,
			voxelPageBase,
			voxelPageCount);
	}

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
	AssignSingleRootPartRecord(artifacts.prebuiltData, state.topRootNode);
	artifacts.cacheBuildData.groupPageBlobs = std::move(state.groupPageBlobs);
	artifacts.cacheBuildData.voxelGroupMapping = std::move(state.voxelGroupMapping);
	artifacts.cacheBuildData.meshPageBlobs = std::move(meshPageBlobs);

	return artifacts;
}
