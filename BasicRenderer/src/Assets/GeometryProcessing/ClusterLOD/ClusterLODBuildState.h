#pragma once

#include <BasicRenderer/Assets/Import/ClusterLODUtilities.h>
#include <meshoptimizer.h>
#include <vector>
#include "../../../../shaders/Common/defines.h"

namespace clod_detail {
	constexpr uint32_t CLOD_STREAMING_PAGE_SIZE_BYTES = 256u * 1024u;
	constexpr uint32_t CLOD_VOXEL_ATTRIBUTE_SAMPLES_COMPACT = 0u;
	constexpr uint32_t CLOD_NATIVE_POSITION_FORMAT = CLOD_POSITION_FORMAT_FLOAT3;
	constexpr uint32_t CLOD_NATIVE_POSITION_STRIDE_BYTES = CLOD_POSITION_FORMAT_FLOAT3_STRIDE_BYTES;

	inline void AppendBits(std::vector<uint32_t>& words, uint64_t& bitCursor, uint32_t value, uint32_t bitCount)
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

	inline size_t ComputePageBlobSize(
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

	void FinalizeMeshWidePagePacking(
		ClusterLODBuildState& state,
		std::vector<std::vector<std::byte>>& outMeshPageBlobs,
		std::vector<uint32_t>& outGroupPageReferences,
		std::vector<uint32_t>& outGroupPageReferenceOffsets,
		uint32_t& outTrianglePageCount,
		uint32_t& outVoxelPageBase,
		uint32_t& outVoxelPageCount);
}
