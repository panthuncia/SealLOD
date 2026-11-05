#include <BasicRenderer/Assets/CLodPageDecoder.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>

namespace CLodCache {

namespace {
	// Page bytes come from a std::vector<std::byte> or a mapped file, so nothing here assumes alignment.
	template<typename T>
	T Load(std::span<const std::byte> page, size_t offset)
	{
		T value{};
		std::memcpy(&value, page.data() + offset, sizeof(T));
		return value;
	}

	bool Fail(std::string* outError, std::string message)
	{
		if (outError != nullptr) {
			*outError = std::move(message);
		}
		return false;
	}

	bool RangeFits(uint64_t offset, uint64_t size, uint64_t total)
	{
		return offset <= total && size <= total - offset;
	}

	// The inverse of OctEncodeNormal and PackOctNormalSnorm16 in ClusterLODClustering.cpp.
	DirectX::XMFLOAT3 DecodeOctNormalWord(uint32_t word)
	{
		const auto low = static_cast<int16_t>(word & 0xFFFFu);
		const auto high = static_cast<int16_t>(word >> 16u);
		const float octX = std::max(static_cast<float>(low) / 32767.0f, -1.0f);
		const float octY = std::max(static_cast<float>(high) / 32767.0f, -1.0f);

		float x = octX;
		float y = octY;
		const float z = 1.0f - std::abs(octX) - std::abs(octY);
		if (z < 0.0f) {
			x = (1.0f - std::abs(octY)) * (octX >= 0.0f ? 1.0f : -1.0f);
			y = (1.0f - std::abs(octX)) * (octY >= 0.0f ? 1.0f : -1.0f);
		}
		const float length = std::sqrt(x * x + y * y + z * z);
		if (length <= 0.0f) {
			return { 0.0f, 0.0f, 1.0f };
		}
		return { x / length, y / length, z / length };
	}
}

PageKind GetPageKind(std::span<const std::byte> page)
{
	if (page.size() < sizeof(uint32_t)) {
		return PageKind::Invalid;
	}
	switch (Load<uint32_t>(page, 0)) {
	case CLOD_TRIANGLE_PAGE_MAGIC: return PageKind::Triangle;
	case CLOD_VOXEL_PAGE_MAGIC: return PageKind::Voxel;
	default: return PageKind::Invalid;
	}
}

bool TrianglePage::Parse(std::span<const std::byte> page, TrianglePage& out, std::string* outError)
{
	if (page.size() < sizeof(CLodPageHeader)) {
		return Fail(outError, std::format("triangle page of {} bytes is smaller than its header", page.size()));
	}
	if (GetPageKind(page) != PageKind::Triangle) {
		return Fail(outError, "page does not have the triangle page magic");
	}
	out.m_page = page;
	out.m_header = Load<CLodPageHeader>(page, 0);
	if (out.m_header.compressedPositionQuantExp != CLOD_POSITION_FORMAT_FLOAT3) {
		return Fail(outError, std::format("unknown position format {}", out.m_header.compressedPositionQuantExp));
	}
	const uint64_t descriptorBytes = static_cast<uint64_t>(out.m_header.meshletCount) * sizeof(CLodMeshletDescriptor);
	if (!RangeFits(out.m_header.descriptorOffset, descriptorBytes, page.size())) {
		return Fail(outError, std::format("{} meshlet descriptors do not fit in the page", out.m_header.meshletCount));
	}
	for (uint32_t meshletIndex = 0; meshletIndex < out.m_header.meshletCount; ++meshletIndex) {
		const TriangleMeshlet meshlet = out.Meshlet(meshletIndex);
		const uint64_t positionOffset = static_cast<uint64_t>(out.m_header.positionBitstreamOffset) + meshlet.descriptor.positionBitOffset;
		if (!RangeFits(positionOffset, static_cast<uint64_t>(meshlet.vertexCount) * sizeof(DirectX::XMFLOAT3), page.size())) {
			return Fail(outError, std::format("meshlet {} positions do not fit in the page", meshletIndex));
		}
		const uint64_t triangleOffset = static_cast<uint64_t>(out.m_header.triangleStreamOffset) + meshlet.descriptor.triangleByteOffset;
		if (!RangeFits(triangleOffset, static_cast<uint64_t>(meshlet.triangleCount) * 3u, page.size())) {
			return Fail(outError, std::format("meshlet {} triangles do not fit in the page", meshletIndex));
		}
		if (out.HasNormals()) {
			const uint64_t normalOffset = static_cast<uint64_t>(out.m_header.normalArrayOffset) +
				static_cast<uint64_t>(meshlet.descriptor.vertexAttributeOffset) * sizeof(uint32_t);
			if (!RangeFits(normalOffset, static_cast<uint64_t>(meshlet.vertexCount) * sizeof(uint32_t), page.size())) {
				return Fail(outError, std::format("meshlet {} normals do not fit in the page", meshletIndex));
			}
		}
	}
	return true;
}

TriangleMeshlet TrianglePage::Meshlet(uint32_t meshletIndex) const
{
	TriangleMeshlet meshlet;
	meshlet.descriptor = Load<CLodMeshletDescriptor>(
		m_page, m_header.descriptorOffset + static_cast<size_t>(meshletIndex) * sizeof(CLodMeshletDescriptor));
	meshlet.vertexCount = meshlet.descriptor.bitsAndVertexCount >> 24u;
	meshlet.triangleCount = meshlet.descriptor.triangleCountAndRefinedGroup & 0xFFFFu;
	meshlet.refinedGroup = static_cast<int32_t>(meshlet.descriptor.triangleCountAndRefinedGroup >> 16u) - 1;
	return meshlet;
}

DirectX::XMFLOAT3 TrianglePage::Position(const TriangleMeshlet& meshlet, uint32_t vertexIndex) const
{
	return Load<DirectX::XMFLOAT3>(
		m_page,
		static_cast<size_t>(m_header.positionBitstreamOffset) + meshlet.descriptor.positionBitOffset +
			static_cast<size_t>(vertexIndex) * sizeof(DirectX::XMFLOAT3));
}

DirectX::XMFLOAT3 TrianglePage::Normal(const TriangleMeshlet& meshlet, uint32_t vertexIndex) const
{
	return DecodeOctNormalWord(Load<uint32_t>(
		m_page,
		static_cast<size_t>(m_header.normalArrayOffset) +
			(static_cast<size_t>(meshlet.descriptor.vertexAttributeOffset) + vertexIndex) * sizeof(uint32_t)));
}

std::array<uint8_t, 3> TrianglePage::Triangle(const TriangleMeshlet& meshlet, uint32_t triangleIndex) const
{
	return Load<std::array<uint8_t, 3>>(
		m_page,
		static_cast<size_t>(m_header.triangleStreamOffset) + meshlet.descriptor.triangleByteOffset +
			static_cast<size_t>(triangleIndex) * 3u);
}

std::vector<PageStream> TrianglePage::Streams() const
{
	// In the order the encoder lays them out. An offset of 0 means the stream is absent.
	const struct { const char* name; uint32_t offset; } layout[] = {
		{ "header", 0u },
		{ "meshlet_descriptors", m_header.descriptorOffset },
		{ "uv_descriptors", m_header.uvDescriptorOffset },
		{ "positions", m_header.positionBitstreamOffset },
		{ "normals", m_header.normalArrayOffset },
		{ "tangent_frames", m_header.tangentFrameArrayOffset },
		{ "colors", m_header.colorArrayOffset },
		{ "joints", m_header.jointArrayOffset },
		{ "weights", m_header.weightArrayOffset },
		{ "uvs", m_header.uvBitstreamDirectoryOffset },
		{ "bone_indices", m_header.boneIndexStreamOffset },
		{ "triangles", m_header.triangleStreamOffset },
	};
	std::vector<PageStream> streams;
	size_t index = 0;
	for (const auto& entry : layout) {
		const size_t current = index++;
		if (current != 0u && entry.offset == 0u) {
			continue;
		}
		uint64_t end = m_page.size();
		for (size_t next = current + 1u; next < std::size(layout); ++next) {
			if (layout[next].offset != 0u) {
				end = layout[next].offset;
				break;
			}
		}
		streams.push_back({ entry.name, end - entry.offset });
	}
	return streams;
}

bool VoxelPage::Parse(std::span<const std::byte> page, VoxelPage& out, std::string* outError)
{
	if (page.size() < sizeof(CLodVoxelPageHeader)) {
		return Fail(outError, std::format("voxel page of {} bytes is smaller than its header", page.size()));
	}
	if (GetPageKind(page) != PageKind::Voxel) {
		return Fail(outError, "page does not have the voxel page magic");
	}
	out.m_page = page;
	out.m_header = Load<CLodVoxelPageHeader>(page, 0);
	const CLodVoxelPageHeader& header = out.m_header;
	if (header.clusterRecordStride != sizeof(CLodVoxelClusterRecord) || header.cubeRecordStride != sizeof(CLodVoxelCubeRecord) ||
		header.attributeSampleStride != sizeof(CLodVoxelAttributeSample)) {
		return Fail(outError, std::format(
			"voxel page strides {}/{}/{} do not match this build",
			header.clusterRecordStride, header.cubeRecordStride, header.attributeSampleStride));
	}
	if (!RangeFits(header.descriptorOffset, static_cast<uint64_t>(header.clusterCount) * sizeof(CLodVoxelClusterRecord), page.size())) {
		return Fail(outError, std::format("{} voxel clusters do not fit in the page", header.clusterCount));
	}
	if (!RangeFits(header.cubeRecordsOffset, static_cast<uint64_t>(header.cubeCount) * sizeof(CLodVoxelCubeRecord), page.size())) {
		return Fail(outError, std::format("{} voxel cubes do not fit in the page", header.cubeCount));
	}

	uint64_t attributeCount = 0;
	for (uint32_t cubeIndex = 0; cubeIndex < header.cubeCount; ++cubeIndex) {
		const CLodVoxelCubeRecord cube = out.Cube(cubeIndex);
		if (cube.firstAttribute != attributeCount) {
			return Fail(outError, std::format(
				"voxel cube {} starts at attribute {} but {} attributes precede it", cubeIndex, cube.firstAttribute, attributeCount));
		}
		attributeCount += static_cast<uint64_t>(std::popcount(cube.occupancyMask));
	}
	if (!RangeFits(header.attributeSamplesOffset, attributeCount * sizeof(CLodVoxelAttributeSample), page.size())) {
		return Fail(outError, std::format("{} voxel attributes do not fit in the page", attributeCount));
	}
	out.m_attributeCount = static_cast<uint32_t>(attributeCount);
	for (uint32_t clusterIndex = 0; clusterIndex < header.clusterCount; ++clusterIndex) {
		const CLodVoxelClusterRecord cluster = out.Cluster(clusterIndex);
		if (static_cast<uint64_t>(cluster.firstCube) + cluster.cubeCount > header.cubeCount) {
			return Fail(outError, std::format(
				"voxel cluster {} cubes [{}, +{}) exceed the page's {}", clusterIndex, cluster.firstCube, cluster.cubeCount, header.cubeCount));
		}
	}
	return true;
}

CLodVoxelClusterRecord VoxelPage::Cluster(uint32_t clusterIndex) const
{
	return Load<CLodVoxelClusterRecord>(
		m_page, m_header.descriptorOffset + static_cast<size_t>(clusterIndex) * sizeof(CLodVoxelClusterRecord));
}

CLodVoxelCubeRecord VoxelPage::Cube(uint32_t cubeIndex) const
{
	return Load<CLodVoxelCubeRecord>(
		m_page, m_header.cubeRecordsOffset + static_cast<size_t>(cubeIndex) * sizeof(CLodVoxelCubeRecord));
}

CLodVoxelAttributeSample VoxelPage::Attribute(uint32_t attributeIndex) const
{
	return Load<CLodVoxelAttributeSample>(
		m_page, m_header.attributeSamplesOffset + static_cast<size_t>(attributeIndex) * sizeof(CLodVoxelAttributeSample));
}

std::vector<PageStream> VoxelPage::Streams() const
{
	return {
		{ "header", m_header.descriptorOffset },
		{ "voxel_clusters", static_cast<uint64_t>(m_header.cubeRecordsOffset) - m_header.descriptorOffset },
		{ "voxel_cubes", static_cast<uint64_t>(m_header.attributeSamplesOffset) - m_header.cubeRecordsOffset },
		{ "voxel_attributes", static_cast<uint64_t>(m_header.boneIndexStreamOffset) - m_header.attributeSamplesOffset },
		{ "bone_indices", static_cast<uint64_t>(m_page.size()) - m_header.boneIndexStreamOffset },
	};
}

bool ResolveSegmentMeshPage(
	const ClusterLODPrebuiltData& prebuiltData,
	uint32_t groupIndex,
	const ClusterLODGroupSegment& segment,
	uint32_t& outMeshPageIndex)
{
	if (groupIndex >= prebuiltData.groups.size() || groupIndex + 1u >= prebuiltData.groupPageReferenceOffsets.size()) {
		return false;
	}
	const ClusterLODGroup& group = prebuiltData.groups[groupIndex];
	if (segment.pageIndex < group.pageMapBase || segment.pageIndex - group.pageMapBase >= group.pageCount) {
		return false;
	}
	const uint64_t reference = static_cast<uint64_t>(prebuiltData.groupPageReferenceOffsets[groupIndex]) + (segment.pageIndex - group.pageMapBase);
	if (reference >= prebuiltData.groupPageReferenceOffsets[groupIndex + 1u] || reference >= prebuiltData.groupPageReferences.size()) {
		return false;
	}
	outMeshPageIndex = prebuiltData.groupPageReferences[static_cast<size_t>(reference)];
	return true;
}

}
