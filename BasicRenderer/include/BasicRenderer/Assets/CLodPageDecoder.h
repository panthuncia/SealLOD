#pragma once

// CPU decoding of the streaming pages in a .clodbin container. The encoders are
// ClusterLODClustering.cpp (triangle pages) and ClusterLODVoxelPacking.cpp (voxel pages), and the
// layouts these read are the ones the shaders load in clodStructs.hlsli.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <BasicRenderer/Assets/ClusterLODTypes.h>

namespace CLodCache {

enum class PageKind {
	Invalid,
	Triangle,
	Voxel,
};

PageKind GetPageKind(std::span<const std::byte> page);

// One contiguous region of a page, named for memory accounting. Sizes include the alignment
// padding that follows the stream, so the streams of a page add up to the page's size.
struct PageStream {
	const char* name = "";
	uint64_t sizeBytes = 0;
};

struct TriangleMeshlet {
	CLodMeshletDescriptor descriptor{};
	uint32_t vertexCount = 0;
	uint32_t triangleCount = 0;
	int32_t refinedGroup = -1;  // -1: terminal, i.e. original geometry
};

// A view of a triangle page. The page's bytes must outlive it.
class TrianglePage {
public:
	static bool Parse(std::span<const std::byte> page, TrianglePage& out, std::string* outError = nullptr);

	const CLodPageHeader& Header() const { return m_header; }
	uint32_t MeshletCount() const { return m_header.meshletCount; }
	TriangleMeshlet Meshlet(uint32_t meshletIndex) const;

	bool HasNormals() const { return (m_header.attributeMask & CLOD_PAGE_ATTRIBUTE_NORMAL) != 0u; }
	DirectX::XMFLOAT3 Position(const TriangleMeshlet& meshlet, uint32_t vertexIndex) const;
	DirectX::XMFLOAT3 Normal(const TriangleMeshlet& meshlet, uint32_t vertexIndex) const;
	std::array<uint8_t, 3> Triangle(const TriangleMeshlet& meshlet, uint32_t triangleIndex) const;
	uint32_t UvSetCount() const { return m_header.uvSetCount; }
	bool HasColors() const { return (m_header.attributeMask & CLOD_PAGE_ATTRIBUTE_COLOR) != 0u; }
	// A meshlet vertex's UV in `uvSet` (0 when the page has fewer sets), as the resolve shaders decode it.
	DirectX::XMFLOAT2 Uv(uint32_t meshletIndex, const TriangleMeshlet& meshlet, uint32_t uvSet, uint32_t vertexIndex) const;
	DirectX::XMFLOAT3 Color(const TriangleMeshlet& meshlet, uint32_t vertexIndex) const;

	std::vector<PageStream> Streams() const;

private:
	std::span<const std::byte> m_page;
	CLodPageHeader m_header{};
};

// A view of a voxel page. The page's bytes must outlive it.
class VoxelPage {
public:
	static bool Parse(std::span<const std::byte> page, VoxelPage& out, std::string* outError = nullptr);

	const CLodVoxelPageHeader& Header() const { return m_header; }
	uint32_t ClusterCount() const { return m_header.clusterCount; }
	uint32_t CubeCount() const { return m_header.cubeCount; }
	uint32_t AttributeCount() const { return m_attributeCount; }

	CLodVoxelClusterRecord Cluster(uint32_t clusterIndex) const;  // firstCube is page-local
	CLodVoxelCubeRecord Cube(uint32_t cubeIndex) const;           // firstAttribute is page-local
	CLodVoxelAttributeSample Attribute(uint32_t attributeIndex) const;

	std::vector<PageStream> Streams() const;

private:
	std::span<const std::byte> m_page;
	CLodVoxelPageHeader m_header{};
	uint32_t m_attributeCount = 0;
};

// The mesh page holding a segment. Segments name a slot in their group's page map, and the group's
// page references translate that slot to an index into pageDiskLocators. Returns false if the
// metadata is inconsistent.
bool ResolveSegmentMeshPage(
	const ClusterLODPrebuiltData& prebuiltData,
	uint32_t groupIndex,
	const ClusterLODGroupSegment& segment,
	uint32_t& outMeshPageIndex);

}
