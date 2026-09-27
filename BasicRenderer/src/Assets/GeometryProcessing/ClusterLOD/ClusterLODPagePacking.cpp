#include <BasicRenderer/Assets/Import/ClusterLODUtilities.h>
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODVoxelPacking.h"
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

#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODBuildState.h"

namespace clod_detail
{
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
		uint32_t boneIndexCount = 0;
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
			outHeader.formatAndKind == CLOD_TRIANGLE_PAGE_MAGIC &&
			outHeader.descriptorOffset != 0u &&
			outHeader.positionBitstreamOffset != 0u &&
			outHeader.triangleStreamOffset != 0u;
	}

	bool ReadVoxelPageHeader(const std::vector<std::byte>& blob, CLodVoxelPageHeader& outHeader)
	{
		if (blob.size() < sizeof(CLodVoxelPageHeader) || !IsVoxelPageBlob(blob))
		{
			return false;
		}
		if (!ReadPodAt(blob, 0u, outHeader))
		{
			return false;
		}
		return outHeader.formatAndKind == CLOD_VOXEL_PAGE_MAGIC &&
			outHeader.descriptorOffset != 0u &&
			outHeader.cubeRecordsOffset != 0u &&
			outHeader.attributeSamplesOffset != 0u &&
			outHeader.attributeSamplesPerCube == CLOD_VOXEL_ATTRIBUTE_SAMPLES_COMPACT &&
			outHeader.clusterRecordStride == sizeof(CLodVoxelClusterRecord) &&
			outHeader.cubeRecordStride == sizeof(CLodVoxelCubeRecord) &&
			outHeader.attributeSampleStride == sizeof(CLodVoxelAttributeSample);
	}

	bool ReadVoxelClusterRecord(
		const std::vector<std::byte>& blob,
		const CLodVoxelPageHeader& header,
		uint32_t clusterIndex,
		CLodVoxelClusterRecord& outRecord)
	{
		if (clusterIndex >= header.clusterCount)
		{
			return false;
		}
		return ReadPodAt(
			blob,
			static_cast<size_t>(header.descriptorOffset) + static_cast<size_t>(clusterIndex) * header.clusterRecordStride,
			outRecord);
	}

	bool ReadVoxelCubeRecord(
		const std::vector<std::byte>& blob,
		const CLodVoxelPageHeader& header,
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
				totals.totalBoneIndexCount += CLodClusterCullMetadataBoneCount(desc.boneCount);
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
				const uint32_t sourceBoneCount = CLodClusterCullMetadataBoneCount(sourceDesc.boneCount);
				if (sourceBoneCount != 0u && sourceHeader.boneIndexStreamOffset != 0u)
				{
					copyBytes(
						boneIndexStreamOffset + boneIndexCursor * static_cast<uint32_t>(sizeof(uint32_t)),
						sourceHeader.boneIndexStreamOffset + sourceDesc.boneListOffset * static_cast<uint32_t>(sizeof(uint32_t)),
						sourceBoneCount * static_cast<uint32_t>(sizeof(uint32_t)));
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
				boneIndexCursor += sourceBoneCount;
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
			CLodVoxelPageHeader sourceHeader{};
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
				totals.boneIndexCount += CLodClusterCullMetadataBoneCount(cluster.flags);
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

		const uint32_t clusterRecordOffset = sizeof(CLodVoxelPageHeader);
		const uint32_t cubeRecordOffset = static_cast<uint32_t>(align4(
			static_cast<size_t>(clusterRecordOffset) +
			static_cast<size_t>(totals.clusterCount) * sizeof(CLodVoxelClusterRecord)));
		const uint32_t attributeOffset = cubeRecordOffset + totals.cubeCount * static_cast<uint32_t>(sizeof(CLodVoxelCubeRecord));
		const uint32_t boneIndexStreamOffset = static_cast<uint32_t>(align4(
			static_cast<size_t>(attributeOffset) + static_cast<size_t>(totals.attributeCount) * sizeof(CLodVoxelAttributeSample)));
		const size_t pageSize = static_cast<size_t>(boneIndexStreamOffset) +
			static_cast<size_t>(totals.boneIndexCount) * sizeof(uint32_t);
		if (pageSize > CLOD_STREAMING_PAGE_SIZE_BYTES)
		{
			return {};
		}

		std::vector<std::byte> blob(pageSize, std::byte{ 0 });
		const CLodVoxelPageHeader header = {
			CLOD_VOXEL_PAGE_MAGIC,
			totals.clusterCount,
			clusterRecordOffset,
			boneIndexStreamOffset,
			totals.cubeCount,
			cubeRecordOffset,
			attributeOffset,
			CLOD_VOXEL_ATTRIBUTE_SAMPLES_COMPACT,
			static_cast<uint32_t>(sizeof(CLodVoxelClusterRecord)),
			static_cast<uint32_t>(sizeof(CLodVoxelCubeRecord)),
			static_cast<uint32_t>(sizeof(CLodVoxelAttributeSample)),
			0u,
			0u,
			0u,
			0u,
			0u
		};
		StorePod(blob, 0u, header);

		uint32_t outputClusterIndex = 0u;
		uint32_t outputCubeIndex = 0u;
		uint32_t outputAttributeIndex = 0u;
		uint32_t outputBoneIndex = 0u;
		for (const PagePackingSegmentRef& segment : segments)
		{
			if (segment.groupIndex >= state.groupPageBlobs.size() ||
				segment.sourcePageIndex >= state.groupPageBlobs[segment.groupIndex].size())
			{
				continue;
			}

			const std::vector<std::byte>& sourceBlob = state.groupPageBlobs[segment.groupIndex][segment.sourcePageIndex];
			CLodVoxelPageHeader sourceHeader{};
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
				outputCluster.reserved2 = outputBoneIndex;
				StorePod(blob, clusterRecordOffset + outputClusterIndex * sizeof(CLodVoxelClusterRecord), outputCluster);
				outputClusterIndex++;

				const uint32_t clusterBoneCount = CLodClusterCullMetadataBoneCount(sourceCluster.flags);
				if (clusterBoneCount != 0u && sourceHeader.boneIndexStreamOffset != 0u)
				{
					const size_t sourceBoneOffset = static_cast<size_t>(sourceHeader.boneIndexStreamOffset) +
						static_cast<size_t>(sourceCluster.reserved2) * sizeof(uint32_t);
					const size_t boneBytes = static_cast<size_t>(clusterBoneCount) * sizeof(uint32_t);
					if (sourceBoneOffset + boneBytes <= sourceBlob.size())
					{
						std::memcpy(
							blob.data() + boneIndexStreamOffset + static_cast<size_t>(outputBoneIndex) * sizeof(uint32_t),
							sourceBlob.data() + sourceBoneOffset,
							boneBytes);
						outputBoneIndex += clusterBoneCount;
					}
				}

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
		std::span<const uint8_t> rootGroups,
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
			const bool isRootGroup =
				groupIndex < rootGroups.size() && rootGroups[groupIndex];
			const size_t targetPageSize = isRootGroup
				? 16u * 1024u
				: CLOD_STREAMING_PAGE_SIZE_BYTES;
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
				const bool currentPageIsRoot =
					!currentPage.empty() &&
					currentPage.front().groupIndex < rootGroups.size() &&
					rootGroups[currentPage.front().groupIndex];
				if ((!currentPage.empty() && currentPageIsRoot != isRootGroup) ||
					(candidateSize > targetPageSize && !currentPage.empty()))
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

		std::vector<uint32_t> incomingParentCounts(state.groups.size(), 0u);
		for (const ClusterLODGroupSegment& segment : state.segments)
		{
			if (segment.refinedGroup >= 0 &&
				static_cast<uint32_t>(segment.refinedGroup) < incomingParentCounts.size())
			{
				incomingParentCounts[static_cast<uint32_t>(segment.refinedGroup)]++;
			}
		}
		std::vector<uint8_t> rootGroups(state.groups.size(), 0u);
		for (uint32_t groupIndex = 0u; groupIndex < static_cast<uint32_t>(state.groups.size()); ++groupIndex)
		{
			const bool includedInTraversal =
				state.traversalGroupMask.empty() ||
				(groupIndex < state.traversalGroupMask.size() &&
					state.traversalGroupMask[groupIndex] != 0u);
			rootGroups[groupIndex] =
				includedInTraversal && incomingParentCounts[groupIndex] == 0u ? 1u : 0u;
		}
		std::stable_partition(groupOrder.begin(), groupOrder.end(), [&](uint32_t groupIndex)
		{
			return groupIndex < rootGroups.size() && rootGroups[groupIndex];
		});

		struct TrianglePageTraits
		{
			uint32_t attributeMask = 0u;
			uint32_t uvSetCount = 0u;
		};
		FinalizeRepresentationPagePacking<TrianglePageTraits>(
			state,
			std::span<const uint32_t>(groupOrder.data(), groupOrder.size()),
			std::span<const uint8_t>(rootGroups.data(), rootGroups.size()),
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
			std::span<const uint8_t>(rootGroups.data(), rootGroups.size()),
			true,
			outMeshPageBlobs,
			groupReferencedPages,
			[](const std::vector<std::byte>& sourceBlob, VoxelPageTraits& outTraits) -> bool
			{
				(void)outTraits;
				CLodVoxelPageHeader sourceHeader{};
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
				return sizeof(CLodVoxelPageHeader) +
					static_cast<size_t>(totals.clusterCount) * sizeof(CLodVoxelClusterRecord) +
					static_cast<size_t>(totals.cubeCount) * sizeof(CLodVoxelCubeRecord) +
					static_cast<size_t>(totals.attributeCount) * sizeof(CLodVoxelAttributeSample) +
					static_cast<size_t>(totals.boneIndexCount) * sizeof(uint32_t);
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

}
