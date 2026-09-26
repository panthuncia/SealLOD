#pragma once

#include <BasicRenderer/Assets/Import/ClusterLODUtilities.h>
#include <BasicRenderer/Assets/Import/VoxelGroupBuilder.h>
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

	template<typename T>
	static inline void StorePod(std::vector<std::byte>& bytes, size_t offset, const T& value)
	{
		if (bytes.size() < offset + sizeof(T))
		{
			bytes.resize(offset + sizeof(T));
		}
		std::memcpy(bytes.data() + offset, &value, sizeof(T));
	}

	static inline uint32_t CountVoxelAttributeSamples(uint64_t occupancyMask)
	{
		return static_cast<uint32_t>(std::popcount(occupancyMask));
	}

	static inline uint32_t CountVoxelAttributeSamples(
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

bool BuildVoxelGroupPayloadFromPackedMapping(const VoxelGroupMapping& mapping, uint32_t groupIndex, VoxelGroupPayload& outPayload);
std::vector<std::vector<std::byte>> BuildVoxelGroupPageBlobs(
    std::span<const ClusterLODGroupSegment> pageSegments,
    std::span<const CLodVoxelClusterRecord> clusterRecords,
    std::span<const CLodVoxelCubeRecord> cubeRecords,
    std::span<const CLodVoxelAttributeSample> attributeSamples,
    uint32_t attributeSampleBase, uint32_t nodeBoneLimit);
void SplitVoxelClustersIntoPageSegments(
    PackedVoxelGroupBuildResult& packed,
    std::vector<ClusterLODGroupSegment>& outSegments,
    std::vector<BoundingSphere>& outSegmentBounds,
    uint32_t nodeBoneLimit);
