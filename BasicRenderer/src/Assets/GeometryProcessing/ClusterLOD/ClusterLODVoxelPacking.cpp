#include <BasicRenderer/Assets/Import/ClusterLODUtilities.h>
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODPagePackingTelemetry.h"

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

#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODVoxelPacking.h"

namespace
{
	constexpr uint32_t CLOD_STREAMING_PAGE_SIZE_BYTES = 256u * 1024u;
	constexpr uint32_t CLOD_VOXEL_ATTRIBUTE_SAMPLES_COMPACT = 0u;
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
		outPayload.uvDensity = metadata.uvDensity;
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
		uint32_t attributeSampleBase,
		uint32_t nodeBoneLimit)
	{
		nodeBoneLimit = std::clamp(nodeBoneLimit, 1u, CLOD_NODE_BONE_LIMIT_HARD_MAX);
		std::vector<std::vector<std::byte>> pageBlobs;
		if (clusterRecords.empty() || cubeRecords.empty())
		{
			return pageBlobs;
		}

		auto align4 = [](size_t value) -> size_t { return (value + 3u) & ~size_t(3); };
		const uint32_t clusterRecordOffset = sizeof(CLodVoxelPageHeader);

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
			std::vector<std::vector<uint32_t>> pageClusterBones(pageClusterCount);
			uint32_t pageBoneIndexCount = 0u;
			for (uint32_t clusterIndex = 0u; clusterIndex < pageClusterCount; ++clusterIndex)
			{
				const CLodVoxelClusterRecord& cluster = clusterRecords[firstClusterInGroup + clusterIndex];
				auto& bones = pageClusterBones[clusterIndex];
				for (uint32_t cubeOffset = 0u; cubeOffset < cluster.cubeCount; ++cubeOffset)
				{
					const uint32_t bone = cubeRecords[cluster.firstCube + cubeOffset].dominantBoneIndex;
					if (bone != CLOD_VOXEL_STATIC_BONE_INDEX)
						bones.push_back(bone);
				}
				std::sort(bones.begin(), bones.end());
				bones.erase(std::unique(bones.begin(), bones.end()), bones.end());
				if (bones.size() > nodeBoneLimit)
					bones.clear();
				pageBoneIndexCount += static_cast<uint32_t>(bones.size());
			}

			const uint32_t pageCubeRecordOffset = static_cast<uint32_t>(align4(static_cast<size_t>(clusterRecordOffset) + pageClusterCount * sizeof(CLodVoxelClusterRecord)));
			const uint32_t attributeOffset = pageCubeRecordOffset + pageCubeCount * static_cast<uint32_t>(sizeof(CLodVoxelCubeRecord));
			const uint32_t boneIndexStreamOffset = static_cast<uint32_t>(align4(
				static_cast<size_t>(attributeOffset) + static_cast<size_t>(pageAttributeCount) * sizeof(CLodVoxelAttributeSample)));
			const size_t pageSize = static_cast<size_t>(boneIndexStreamOffset) +
				static_cast<size_t>(pageBoneIndexCount) * sizeof(uint32_t);
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
			const CLodVoxelPageHeader header = {
				CLOD_VOXEL_PAGE_MAGIC,
				pageClusterCount,
				clusterRecordOffset,
				boneIndexStreamOffset,
				pageCubeCount,
				pageCubeRecordOffset,
				attributeOffset,
				CLOD_VOXEL_ATTRIBUTE_SAMPLES_COMPACT,
				static_cast<uint32_t>(sizeof(CLodVoxelClusterRecord)),
				static_cast<uint32_t>(sizeof(CLodVoxelCubeRecord)),
				static_cast<uint32_t>(sizeof(CLodVoxelAttributeSample)),
				firstClusterInGroup,
				firstCubeInGroup,
				0u,
				0u,
				0u
			};
			StorePod(blob, 0u, header);

			uint32_t pageBoneCursor = 0u;
			for (uint32_t clusterIndex = 0; clusterIndex < pageClusterCount; ++clusterIndex)
			{
				CLodVoxelClusterRecord pageCluster = clusterRecords[firstClusterInGroup + clusterIndex];
				pageCluster.firstCube -= firstCubeInGroup;
				const bool animated = (pageCluster.flags & CLOD_VOXEL_CLUSTER_FLAG_HAS_SKINNED_CUBES) != 0u;
				const bool overflow = animated && pageClusterBones[clusterIndex].empty();
				const uint32_t cullFlags =
					(animated ? CLOD_CLUSTER_CULL_FLAG_ANIMATED : 0u) |
					(overflow ? CLOD_CLUSTER_CULL_FLAG_BONE_OVERFLOW : 0u) |
					((pageCluster.flags >> CLOD_CLUSTER_CULL_FLAGS_SHIFT) & CLOD_CLUSTER_CULL_FLAG_RIGID_COMPONENT);
				pageCluster.reserved2 = pageBoneCursor;
				pageCluster.flags = CLodPackClusterCullMetadata(
					CLOD_CLUSTER_KIND_VOXEL,
					cullFlags,
					static_cast<uint32_t>(pageClusterBones[clusterIndex].size()));
				StorePod(blob, clusterRecordOffset + clusterIndex * sizeof(CLodVoxelClusterRecord), pageCluster);
				if (!pageClusterBones[clusterIndex].empty())
				{
					std::memcpy(
						blob.data() + boneIndexStreamOffset + static_cast<size_t>(pageBoneCursor) * sizeof(uint32_t),
						pageClusterBones[clusterIndex].data(),
						pageClusterBones[clusterIndex].size() * sizeof(uint32_t));
					pageBoneCursor += static_cast<uint32_t>(pageClusterBones[clusterIndex].size());
				}
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

	static uint32_t ComputeVoxelPageSizeBytes(uint32_t clusterCount, uint32_t cubeCount, uint32_t attributeCount, uint32_t boneIndexCount)
	{
		constexpr uint32_t fixedBytes = sizeof(CLodVoxelPageHeader);
		return fixedBytes +
			clusterCount * static_cast<uint32_t>(sizeof(CLodVoxelClusterRecord)) +
			cubeCount * static_cast<uint32_t>(sizeof(CLodVoxelCubeRecord)) +
			attributeCount * static_cast<uint32_t>(sizeof(CLodVoxelAttributeSample)) +
			boneIndexCount * static_cast<uint32_t>(sizeof(uint32_t));
	}

	void SplitVoxelClustersIntoPageSegments(
		PackedVoxelGroupBuildResult& packed,
		std::vector<ClusterLODGroupSegment>& outSegments,
		std::vector<BoundingSphere>& outSegmentBounds,
		uint32_t nodeBoneLimit)
	{
		nodeBoneLimit = std::clamp(nodeBoneLimit, 1u, CLOD_NODE_BONE_LIMIT_HARD_MAX);
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
		uint32_t pageBoneIndexCount = 0u;
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
			pageBoneIndexCount = 0u;
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

				auto countExplicitBones = [&](uint32_t firstCube, uint32_t cubeCount) -> uint32_t
				{
					std::vector<uint32_t> bones;
					bones.reserve((std::min)(cubeCount, nodeBoneLimit));
					for (uint32_t localCube = 0u; localCube < cubeCount; ++localCube)
					{
						const uint32_t bone = packed.cubeRecords[firstCube + localCube].dominantBoneIndex;
						if (bone != CLOD_VOXEL_STATIC_BONE_INDEX)
							bones.push_back(bone);
					}
					std::sort(bones.begin(), bones.end());
					bones.erase(std::unique(bones.begin(), bones.end()), bones.end());
					return bones.size() <= nodeBoneLimit ? static_cast<uint32_t>(bones.size()) : 0u;
				};

				uint32_t chunkCubes = remainingCubes;
				uint32_t chunkAttributes = CountVoxelAttributeSamples(
					std::span<const CLodVoxelCubeRecord>(packed.cubeRecords.data(), packed.cubeRecords.size()),
					sourceCluster.firstCube + cubeOffset,
					chunkCubes);
				uint32_t chunkBoneIndexCount = countExplicitBones(sourceCluster.firstCube + cubeOffset, chunkCubes);
				while (chunkCubes > 0u &&
					ComputeVoxelPageSizeBytes(
						pageClusterCount + 1u,
						pageCubeCount + chunkCubes,
						pageAttributeCount + chunkAttributes,
						pageBoneIndexCount + chunkBoneIndexCount) > CLOD_STREAMING_PAGE_SIZE_BYTES)
				{
					chunkCubes--;
					chunkAttributes = CountVoxelAttributeSamples(
						std::span<const CLodVoxelCubeRecord>(packed.cubeRecords.data(), packed.cubeRecords.size()),
						sourceCluster.firstCube + cubeOffset,
						chunkCubes);
					chunkBoneIndexCount = countExplicitBones(sourceCluster.firstCube + cubeOffset, chunkCubes);
				}

				if (chunkCubes == 0u)
				{
					flushPage();
					continue;
				}

				CLodVoxelClusterRecord pageCluster = sourceCluster;
				pageCluster.firstCube = sourceCluster.firstCube + cubeOffset;
				pageCluster.cubeCount = chunkCubes;
				pageCluster.flags = ComputeVoxelClusterCullMetadata(
					std::span<const CLodVoxelCubeRecord>(packed.cubeRecords.data(), packed.cubeRecords.size()),
					pageCluster.firstCube,
					pageCluster.cubeCount);
				pageClusterRecords.push_back(pageCluster);
				pageClusterCount++;
				pageCubeCount += chunkCubes;
				pageAttributeCount += chunkAttributes;
				pageBoneIndexCount += chunkBoneIndexCount;
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
