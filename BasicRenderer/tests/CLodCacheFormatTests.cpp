// Builds a hierarchy from a procedural mesh, saves it, opens it by file and decodes every page.
// This is the only test that runs the generator, so it also checks what the build produced.

#include <BasicRenderer/Assets/CLodCacheStore.h>
#include <BasicRenderer/Assets/CLodPageDecoder.h>
#include <BasicRenderer/Assets/ClusterLODTextureDecode.h>
#include <BasicRenderer/Assets/ClusterLODTypes.h>
#include <BasicRenderer/Assets/DefaultCLodSettings.h>
#include <BasicRenderer/Assets/Import/CLodCache.h>
#include <BasicRenderer/Assets/Import/CLodOpacityMicromapCache.h>
#include <BasicRenderer/Assets/Import/ClusterLODUtilities.h>
#include <BasicRenderer/Assets/Geometry/VertexLayout.h>
#include <BasicRenderer/Streaming/TaskScheduler.h>

#include "../src/Assets/GeometryProcessing/ClusterLOD/ClusterLODVoxelClosure.h"
#include "../src/Assets/Textures/Cache/ProcessedTextureCache.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <ktx.h>

namespace {
namespace fs = std::filesystem;

int g_failures = 0;

#define CHECK(condition)                                                          \
	do {                                                                          \
		if (!(condition)) {                                                       \
			std::fprintf(stderr, "%s:%d FAILED: %s\n", __FILE__, __LINE__, #condition); \
			++g_failures;                                                         \
		}                                                                         \
	} while (false)

struct SourceMesh {
	std::vector<std::byte> vertices;
	std::vector<uint32_t> indices;
	double area = 0.0;
};

DirectX::XMFLOAT3 Sub(DirectX::XMFLOAT3 a, DirectX::XMFLOAT3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }

double TriangleArea(DirectX::XMFLOAT3 a, DirectX::XMFLOAT3 b, DirectX::XMFLOAT3 c)
{
	const DirectX::XMFLOAT3 u = Sub(b, a);
	const DirectX::XMFLOAT3 v = Sub(c, a);
	const double x = static_cast<double>(u.y) * v.z - static_cast<double>(u.z) * v.y;
	const double y = static_cast<double>(u.z) * v.x - static_cast<double>(u.x) * v.z;
	const double z = static_cast<double>(u.x) * v.y - static_cast<double>(u.y) * v.x;
	return 0.5 * std::sqrt(x * x + y * y + z * z);
}

// A bumpy sheet: enough triangles for several LOD levels, and enough curvature that simplification
// accumulates the error voxel seeding compares against.
SourceMesh MakeBumpySheet(uint32_t resolution)
{
	SourceMesh mesh;
	const auto height = [](float x, float y) {
		return 0.08f * std::sin(x * 37.0f) * std::cos(y * 41.0f) + 0.03f * std::sin(x * 131.0f + y * 113.0f);
	};
	std::vector<DirectX::XMFLOAT3> positions;
	positions.reserve(static_cast<size_t>(resolution + 1u) * (resolution + 1u));
	for (uint32_t row = 0; row <= resolution; ++row) {
		for (uint32_t column = 0; column <= resolution; ++column) {
			const float x = static_cast<float>(column) / static_cast<float>(resolution);
			const float y = static_cast<float>(row) / static_cast<float>(resolution);
			const DirectX::XMFLOAT3 position{ x, y, height(x, y) };
			positions.push_back(position);

			constexpr float kStep = 1.0e-3f;
			const float dx = (height(x + kStep, y) - height(x - kStep, y)) / (2.0f * kStep);
			const float dy = (height(x, y + kStep) - height(x, y - kStep)) / (2.0f * kStep);
			const float length = std::sqrt(dx * dx + dy * dy + 1.0f);
			const DirectX::XMFLOAT3 normal{ -dx / length, -dy / length, 1.0f / length };

			const size_t offset = mesh.vertices.size();
			mesh.vertices.resize(offset + MeshVertexLayout::BaseVertexSize);
			std::memcpy(mesh.vertices.data() + offset + MeshVertexLayout::PositionOffset, &position, sizeof(position));
			std::memcpy(mesh.vertices.data() + offset + MeshVertexLayout::NormalOffset, &normal, sizeof(normal));
		}
	}
	const uint32_t stride = resolution + 1u;
	for (uint32_t row = 0; row < resolution; ++row) {
		for (uint32_t column = 0; column < resolution; ++column) {
			const uint32_t a = row * stride + column;
			const uint32_t b = a + 1u;
			const uint32_t c = a + stride;
			const uint32_t d = c + 1u;
			mesh.indices.insert(mesh.indices.end(), { a, b, d, a, d, c });
			mesh.area += TriangleArea(positions[a], positions[b], positions[d]);
			mesh.area += TriangleArea(positions[a], positions[d], positions[c]);
		}
	}
	return mesh;
}

ClusterLODPrebuildArtifacts Build(
	const SourceMesh& mesh,
	ClusterLODCoveragePreservationMode mode,
	ClusterLODVoxelForkMode fork = ClusterLODVoxelForkMode::Off)
{
	ClusterLODBuilderSettings settings = GetDefaultBuilderSettings();
	settings.coveragePreservationMode = mode;
	settings.voxelFork = fork;
	return BuildClusterLODArtifactsFromGeometry(
		mesh.vertices,
		MeshVertexLayout::BaseVertexSize,
		nullptr,
		0u,
		mesh.indices,
		{},
		VertexFlags::VERTEX_NORMALS,
		settings);
}

fs::path FindMetadataFile(const fs::path& root, const std::string& primPath)
{
	for (const auto& entry : fs::recursive_directory_iterator(root)) {
		if (entry.path().extension() == L".clodmeta") {
			const auto opened = CLodCache::OpenMetadataFile(entry.path());
			if (opened.status == CLodCache::OpenMetadataStatus::Ok && opened.data.prebuiltData.cacheSource.primPath == primPath) {
				return entry.path();
			}
		}
	}
	return {};
}

std::vector<std::vector<std::byte>> ReadAllPages(const CLodCache::OpenedMetadata& opened)
{
	const ClusterLODPrebuiltData& prebuilt = opened.data.prebuiltData;
	std::ifstream container;
	uint32_t pageCount = 0;
	CHECK(CLodCache::OpenContainerFile(opened.containerPath, container, pageCount));
	CHECK(pageCount == prebuilt.pageDiskLocators.size());
	CLodCache::LoadedGroupPayload payload;
	const std::vector<bool> fetchAll;
	CHECK(CLodCache::LoadMeshPagesSelective(container, prebuilt.pageDiskLocators, 0u, pageCount, fetchAll, payload));
	CHECK(payload.pageBlobs.size() == pageCount);
	return std::move(payload.pageBlobs);
}

// The triangles that are never refined further are the original mesh, whatever the hierarchy did.
void CheckTerminalGeometryMatchesSource(
	const ClusterLODPrebuiltData& prebuilt,
	const std::vector<std::vector<std::byte>>& pages,
	const SourceMesh& source)
{
	uint64_t terminalTriangles = 0;
	double terminalArea = 0.0;
	for (uint32_t groupIndex = 0; groupIndex < prebuilt.groups.size(); ++groupIndex) {
		const ClusterLODGroup& group = prebuilt.groups[groupIndex];
		if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u) {
			continue;
		}
		for (uint32_t segmentIndex = group.firstSegment; segmentIndex < group.firstSegment + group.segmentCount; ++segmentIndex) {
			const ClusterLODGroupSegment& segment = prebuilt.segments[segmentIndex];
			if (segment.refinedGroup != -1 || segment.meshletCount == 0u) {
				continue;
			}
			uint32_t meshPage = 0;
			CHECK(CLodCache::ResolveSegmentMeshPage(prebuilt, groupIndex, segment, meshPage));
			CHECK(meshPage < pages.size());
			CLodCache::TrianglePage page;
			std::string error;
			if (!CLodCache::TrianglePage::Parse(pages[meshPage], page, &error)) {
				std::fprintf(stderr, "triangle page %u: %s\n", meshPage, error.c_str());
				++g_failures;
				continue;
			}
			for (uint32_t index = 0; index < segment.meshletCount; ++index) {
				const CLodCache::TriangleMeshlet meshlet = page.Meshlet(segment.firstMeshletInPage + index);
				CHECK(meshlet.refinedGroup == -1);
				terminalTriangles += meshlet.triangleCount;
				for (uint32_t triangle = 0; triangle < meshlet.triangleCount; ++triangle) {
					const auto vertices = page.Triangle(meshlet, triangle);
					CHECK(vertices[0] < meshlet.vertexCount && vertices[1] < meshlet.vertexCount && vertices[2] < meshlet.vertexCount);
					terminalArea += TriangleArea(
						page.Position(meshlet, vertices[0]), page.Position(meshlet, vertices[1]), page.Position(meshlet, vertices[2]));
				}
			}
		}
	}
	CHECK(terminalTriangles == source.indices.size() / 3u);
	CHECK(std::abs(terminalArea - source.area) < source.area * 1.0e-5);
}

void TestPrioritizeEdgesRoundTrip(const fs::path& root, const SourceMesh& source)
{
	const ClusterLODPrebuildArtifacts artifacts = Build(source, ClusterLODCoveragePreservationMode::PrioritizeEdges);
	const ClusterLODPrebuiltData& built = artifacts.prebuiltData;
	CHECK(built.groups.size() > 1u);
	CHECK(built.voxelPageCount == 0u);
	CHECK(built.maxDepth > 0u);

	CLodCacheLoader::MeshCacheIdentity identity{ "clod_format_test.source", "/triangles", "", false };
	CHECK(CLodCacheLoader::SavePrebuilt(identity, built, artifacts.cacheBuildData.AsPayload()));

	const fs::path metadataPath = FindMetadataFile(root, "/triangles");
	CHECK(!metadataPath.empty());
	const CLodCache::OpenedMetadata opened = CLodCache::OpenMetadataFile(metadataPath);
	CHECK(opened.status == CLodCache::OpenMetadataStatus::Ok);
	const ClusterLODPrebuiltData& loaded = opened.data.prebuiltData;
	CHECK(loaded.groups.size() == built.groups.size());
	CHECK(loaded.segments.size() == built.segments.size());
	CHECK(loaded.nodes.size() == built.nodes.size());
	CHECK(loaded.trianglePageCount == built.trianglePageCount);

	const auto pages = ReadAllPages(opened);
	CHECK(pages.size() == artifacts.cacheBuildData.meshPageBlobs.size());
	for (size_t index = 0; index < pages.size() && index < artifacts.cacheBuildData.meshPageBlobs.size(); ++index) {
		CHECK(pages[index] == artifacts.cacheBuildData.meshPageBlobs[index]);
	}

	// The named streams account for every byte of a page, which the memory metrics rely on.
	for (const auto& blob : pages) {
		CHECK(CLodCache::GetPageKind(blob) == CLodCache::PageKind::Triangle);
		CLodCache::TrianglePage page;
		CHECK(CLodCache::TrianglePage::Parse(blob, page));
		uint64_t streamBytes = 0;
		for (const CLodCache::PageStream& stream : page.Streams()) {
			streamBytes += stream.sizeBytes;
		}
		CHECK(streamBytes == blob.size());
	}
	CheckTerminalGeometryMatchesSource(loaded, pages, source);

	// A normal survives its 16-bit oct encoding well enough to measure normal error against.
	const auto& firstPage = pages.front();
	CLodCache::TrianglePage page;
	CHECK(CLodCache::TrianglePage::Parse(firstPage, page));
	CHECK(page.HasNormals());
	const CLodCache::TriangleMeshlet meshlet = page.Meshlet(0u);
	const DirectX::XMFLOAT3 normal = page.Normal(meshlet, 0u);
	CHECK(std::abs(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z - 1.0f) < 1.0e-4f);
}

void TestVoxelRoundTrip(const fs::path& root, const SourceMesh& source)
{
	const ClusterLODPrebuildArtifacts artifacts = Build(source, ClusterLODCoveragePreservationMode::Voxel);
	const ClusterLODPrebuiltData& built = artifacts.prebuiltData;
	CHECK(built.voxelPageCount > 0u);
	CHECK(built.voxelPageBase == built.trianglePageCount);

	CLodCacheLoader::MeshCacheIdentity identity{ "clod_format_test.source", "/voxels", "", false };
	CHECK(CLodCacheLoader::SavePrebuilt(identity, built, artifacts.cacheBuildData.AsPayload()));
	const CLodCache::OpenedMetadata opened = CLodCache::OpenMetadataFile(FindMetadataFile(root, "/voxels"));
	CHECK(opened.status == CLodCache::OpenMetadataStatus::Ok);
	const ClusterLODPrebuiltData& loaded = opened.data.prebuiltData;
	const auto pages = ReadAllPages(opened);
	CheckTerminalGeometryMatchesSource(loaded, pages, source);

	uint64_t voxelGroups = 0;
	for (const ClusterLODGroup& group : loaded.groups) {
		if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u) {
			++voxelGroups;
			CHECK(group.representationError > 0.0f);
		}
	}
	CHECK(voxelGroups > 0u);

	uint64_t cubes = 0;
	uint64_t cells = 0;
	for (uint32_t meshPage = loaded.voxelPageBase; meshPage < loaded.voxelPageBase + loaded.voxelPageCount; ++meshPage) {
		const auto& blob = pages[meshPage];
		CHECK(CLodCache::GetPageKind(blob) == CLodCache::PageKind::Voxel);
		CLodCache::VoxelPage page;
		std::string error;
		if (!CLodCache::VoxelPage::Parse(blob, page, &error)) {
			std::fprintf(stderr, "voxel page %u: %s\n", meshPage, error.c_str());
			++g_failures;
			continue;
		}
		uint64_t streamBytes = 0;
		for (const CLodCache::PageStream& stream : page.Streams()) {
			streamBytes += stream.sizeBytes;
		}
		CHECK(streamBytes == blob.size());
		for (uint32_t clusterIndex = 0; clusterIndex < page.ClusterCount(); ++clusterIndex) {
			const CLodVoxelClusterRecord cluster = page.Cluster(clusterIndex);
			CHECK(cluster.aabbMinAndVoxelWidth.w > 0.0f);
			CHECK(cluster.cubeCount <= CLOD_VOXEL_MAX_CUBES_PER_CLUSTER);
		}
		cubes += page.CubeCount();
		cells += page.AttributeCount();
		for (uint32_t attributeIndex = 0; attributeIndex < page.AttributeCount(); ++attributeIndex) {
			const CLodVoxelAttributeSample attribute = page.Attribute(attributeIndex);
			CHECK(attribute.opacity >= 0.0f && attribute.opacity <= 1.0f);
		}
	}
	CHECK(cubes > 0u);
	CHECK(cells >= cubes);  // an empty cube would not have been stored
}

// A forced fork keeps the triangle build as its triangle branch and adds voxel groups beside it: the groups before the
// voxel ones are the triangle build's, and each branch is closed (a group refines only into groups in its branches).
void TestVoxelForkRoundTrip(const fs::path& root, const SourceMesh& source)
{
	const ClusterLODPrebuildArtifacts triangles = Build(source, ClusterLODCoveragePreservationMode::PrioritizeEdges);
	const ClusterLODPrebuildArtifacts artifacts =
		Build(source, ClusterLODCoveragePreservationMode::PrioritizeEdges, ClusterLODVoxelForkMode::Forced);
	const ClusterLODPrebuiltData& built = artifacts.prebuiltData;
	CHECK(built.voxelPageCount > 0u);
	CHECK(built.voxelPageBase == built.trianglePageCount);
	CHECK(built.trianglePageCount >= triangles.prebuiltData.trianglePageCount);

	uint32_t triangleOnly = 0, voxelOnly = 0, shadows = 0, boundary = 0, voxelGroups = 0;
	for (uint32_t index = 0; index < built.groups.size(); ++index) {
		const ClusterLODGroup& group = built.groups[index];
		triangleOnly += (group.flags & CLOD_GROUP_FLAG_BRANCH_TRIANGLE) != 0u ? 1u : 0u;
		voxelOnly += (group.flags & CLOD_GROUP_FLAG_BRANCH_VOXEL) != 0u ? 1u : 0u;
		shadows += (group.flags & CLOD_GROUP_FLAG_BRANCH_SHADOW) != 0u ? 1u : 0u;
		voxelGroups += (group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u ? 1u : 0u;
		if (group.branchGroupId >= 0) {
			++boundary;
			CHECK(static_cast<size_t>(group.branchGroupId) < built.groups.size());
			CHECK((built.groups[group.branchGroupId].flags & CLOD_GROUP_FLAG_BRANCH_SHADOW) != 0u);
			CHECK(ClusterLODGroupBranches(group.flags) == CLOD_BRANCH_ALL);
		}
		if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u) {
			CHECK((group.flags & CLOD_GROUP_FLAG_BRANCH_VOXEL) != 0u);
		}
		if ((group.flags & CLOD_GROUP_FLAG_BRANCH_SHADOW) != 0u) {
			CHECK(group.segmentCount == 0u && group.pageCount == 0u);
			continue;
		}
		const uint32_t branches = ClusterLODGroupBranches(group.flags);
		for (uint32_t s = 0; s < group.segmentCount; ++s) {
			const int32_t child = built.segments[group.firstSegment + s].refinedGroup;
			if (child < 0) continue;
			// Every branch the parent is in has the child.
			CHECK((ClusterLODGroupBranches(built.groups[child].flags) & branches) == branches);
		}
	}
	std::fprintf(stderr, "fork: groups=%zu triangle_only=%u voxel_only=%u voxel=%u shadows=%u boundary=%u\n",
		built.groups.size(), triangleOnly, voxelOnly, voxelGroups, shadows, boundary);
	CHECK(triangleOnly > 0u);
	CHECK(voxelOnly >= triangleOnly);
	CHECK(voxelGroups > 0u);
	CHECK(shadows == boundary);
	CHECK(boundary > 0u);

	// The triangle branch is the triangle build.
	const auto& plain = triangles.prebuiltData.groups;
	CHECK(built.groups.size() == plain.size() + voxelOnly + shadows);
	for (size_t index = 0; index < plain.size() && index < built.groups.size(); ++index) {
		const ClusterLODGroup& a = plain[index];
		const ClusterLODGroup& b = built.groups[index];
		CHECK(std::memcmp(&a.bounds, &b.bounds, sizeof(a.bounds)) == 0);
		CHECK(a.depth == b.depth && a.meshletCount == b.meshletCount && a.segmentCount == b.segmentCount);
		CHECK(a.parentGroupId == b.parentGroupId);
		CHECK(a.maxParentError == b.maxParentError);
		CHECK((b.flags & CLOD_GROUP_FLAG_BRANCH_VOXEL) == 0u);
	}

	CLodCacheLoader::MeshCacheIdentity identity{ "clod_format_test.source", "/fork", "", false };
	CHECK(CLodCacheLoader::SavePrebuilt(identity, built, artifacts.cacheBuildData.AsPayload()));
	const CLodCache::OpenedMetadata opened = CLodCache::OpenMetadataFile(FindMetadataFile(root, "/fork"));
	CHECK(opened.status == CLodCache::OpenMetadataStatus::Ok);
	const ClusterLODPrebuiltData& loaded = opened.data.prebuiltData;
	CHECK(loaded.groups.size() == built.groups.size());
	for (size_t index = 0; index < loaded.groups.size(); ++index) {
		CHECK(loaded.groups[index].branchGroupId == built.groups[index].branchGroupId);
		CHECK(loaded.groups[index].flags == built.groups[index].flags);
	}
	const auto pages = ReadAllPages(opened);
	CheckTerminalGeometryMatchesSource(loaded, pages, source);
	for (uint32_t meshPage = 0; meshPage < loaded.trianglePageCount; ++meshPage) {
		CHECK(CLodCache::GetPageKind(pages[meshPage]) == CLodCache::PageKind::Triangle);
	}
	for (uint32_t meshPage = loaded.voxelPageBase; meshPage < loaded.voxelPageBase + loaded.voxelPageCount; ++meshPage) {
		CHECK(CLodCache::GetPageKind(pages[meshPage]) == CLodCache::PageKind::Voxel);
	}
}

// A whole-asset assembly with a forked part (an automatic fork is what a stalled foliage part gets without the sloppy
// fallback). The assembly copies the part with its branches, rebuilds its shadow records over the assembly, and uses direct
// instance roots, so each view resolves the part's fork below its instances.
void TestForkedAssemblyPart(const fs::path& root, const SourceMesh& source)
{
	const ClusterLODPrebuildArtifacts forked =
		Build(source, ClusterLODCoveragePreservationMode::PrioritizeEdges, ClusterLODVoxelForkMode::Forced);
	const ClusterLODPrebuildArtifacts plain = Build(source, ClusterLODCoveragePreservationMode::PrioritizeEdges);
	const auto& forkedGroups = forked.prebuiltData.groups;
	uint32_t copied = static_cast<uint32_t>(forkedGroups.size());
	while (copied != 0u && (forkedGroups[copied - 1u].flags & CLOD_GROUP_FLAG_BRANCH_SHADOW) != 0u) {
		--copied;
	}
	const uint32_t partShadows = static_cast<uint32_t>(forkedGroups.size()) - copied;
	CHECK(partShadows > 0u);

	const std::vector<ClusterLODAssemblyPart> parts = {
		ClusterLODAssemblyPart{ .artifacts = &forked, .coverageVertices = &source.vertices, .coverageIndices = &source.indices, .coverageVertexSize = MeshVertexLayout::BaseVertexSize },
		ClusterLODAssemblyPart{ .artifacts = &plain, .coverageVertices = &source.vertices, .coverageIndices = &source.indices, .coverageVertexSize = MeshVertexLayout::BaseVertexSize },
	};
	std::vector<ClusterLODAssemblyInstanceSpec> instances(3);
	instances[1].transform.row0.w = 2.0f;
	instances[2].partIndex = 1u;
	instances[2].transform.row1.w = 2.0f;

	bool threw = false;
	try {
		(void)BuildClusterLODAssemblyArtifacts(parts, instances, GetDefaultBuilderSettings(), 8u, true);
	}
	catch (const std::runtime_error&) {
		threw = true;
	}
	CHECK(threw);  // assembly voxel parents are in no branch

	const ClusterLODPrebuildArtifacts assembly =
		BuildClusterLODAssemblyArtifactsPreservingTriangleOnly(parts, instances, GetDefaultBuilderSettings(), 8u);
	const ClusterLODPrebuiltData& built = assembly.prebuiltData;
	CHECK(built.groups.size() > copied);
	uint32_t shadows = 0u;
	for (uint32_t index = 0; index < built.groups.size(); ++index) {
		const ClusterLODGroup& group = built.groups[index];
		CHECK((group.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) == 0u);
		if ((group.flags & CLOD_GROUP_FLAG_BRANCH_SHADOW) != 0u) {
			++shadows;
			CHECK(group.segmentCount == 0u && group.pageCount == 0u);
			continue;
		}
		const uint32_t branches = ClusterLODGroupBranches(group.flags);
		for (uint32_t s = 0; s < group.segmentCount; ++s) {
			const int32_t child = built.segments[group.firstSegment + s].refinedGroup;
			if (child >= 0) {
				CHECK((ClusterLODGroupBranches(built.groups[child].flags) & branches) == branches);
			}
		}
	}
	CHECK(shadows == partShadows);
	// The forked part is part 0, copied first: its groups keep their flags, parents and voxel-branch shadows.
	for (uint32_t index = 0; index < copied && index < built.groups.size(); ++index) {
		const ClusterLODGroup& a = forkedGroups[index];
		const ClusterLODGroup& b = built.groups[index];
		CHECK(a.flags == b.flags && a.depth == b.depth && a.segmentCount == b.segmentCount);
		CHECK(std::memcmp(&a.bounds, &b.bounds, sizeof(a.bounds)) == 0);
		CHECK(a.parentGroupId == b.parentGroupId && a.maxParentError == b.maxParentError);
		CHECK((a.branchGroupId >= 0) == (b.branchGroupId >= 0));
		if (a.branchGroupId >= 0 && b.branchGroupId >= 0 && static_cast<size_t>(b.branchGroupId) < built.groups.size()) {
			const ClusterLODGroup& oldShadow = forkedGroups[a.branchGroupId];
			const ClusterLODGroup& newShadow = built.groups[b.branchGroupId];
			CHECK((newShadow.flags & CLOD_GROUP_FLAG_BRANCH_SHADOW) != 0u);
			CHECK(std::memcmp(&oldShadow.bounds, &newShadow.bounds, sizeof(oldShadow.bounds)) == 0);
			CHECK(oldShadow.parentGroupId == newShadow.parentGroupId && oldShadow.maxParentError == newShadow.maxParentError);
		}
	}
	CHECK(built.assemblyInstances.size() == instances.size());
	CHECK(built.voxelPageCount > 0u);

	CLodCacheLoader::MeshCacheIdentity identity{ "clod_format_test.source", "/forked-assembly", "", false };
	CHECK(CLodCacheLoader::SavePrebuilt(identity, built, assembly.cacheBuildData.AsPayload()));
	const CLodCache::OpenedMetadata opened = CLodCache::OpenMetadataFile(FindMetadataFile(root, "/forked-assembly"));
	CHECK(opened.status == CLodCache::OpenMetadataStatus::Ok);
	CHECK(opened.data.prebuiltData.groups.size() == built.groups.size());
	CHECK(ReadAllPages(opened).size() == built.trianglePageCount + built.voxelPageCount);
}

// Voxel replace with a stuck sibling, the Baltic pine's failure without the sloppy fallback: group 4 (a seed) and group 5
// (left terminal by simplification, so never a seed) share child 2. Replacing 2, as every below-seed group without
// terminal segments used to be, gave voxel 2 a triangle parent ("voxel refined child has non-voxel DAG parent"). 2 and
// what is below it (1) keep triangles; 3, refined only from the seed, is still replaced; leaf 0 has terminal segments.
//
//        4 (seed)    5 (stuck)
//        /     \     /
//       3       2 --
//       |       |
//       0       1
//               |
//               0
void TestVoxelReplaceClosure()
{
	const std::vector<std::vector<uint32_t>> parents = { { 3u, 1u }, { 2u }, { 4u, 5u }, { 4u }, {}, {} };
	const std::vector<uint8_t> replaceMask = { 0u, 0u, 0u, 0u, 1u, 0u };  // the seed; it has no parents to close over
	const std::vector<uint8_t> buildMask = { 1u, 1u, 1u, 1u, 1u, 0u };    // the seed and everything below it
	const std::vector<uint32_t> terminalSegments = { 4u, 0u, 0u, 0u, 0u, 0u };
	uint32_t kept = 0u;
	const std::vector<uint8_t> replace = clod_detail::ComputeVoxelReplaceClosure(parents, replaceMask, buildMask, terminalSegments, &kept);
	CHECK((replace == std::vector<uint8_t>{ 0u, 0u, 0u, 1u, 1u, 0u }));
	CHECK(kept == 2u);
	for (size_t child = 0; child < parents.size(); ++child) {
		for (const uint32_t parent : parents[child]) {
			CHECK(replace[child] == 0u || replace[parent] != 0u);
		}
	}

	// Without the stuck sibling, everything below the seed without terminal segments is replaced, as before.
	std::vector<std::vector<uint32_t>> treeParents = parents;
	treeParents[2] = { 4u };
	const std::vector<uint8_t> tree = clod_detail::ComputeVoxelReplaceClosure(treeParents, replaceMask, buildMask, terminalSegments, &kept);
	CHECK((tree == std::vector<uint8_t>{ 0u, 1u, 1u, 1u, 1u, 0u }));
	CHECK(kept == 0u);
}

void TestOpenFailures(const fs::path& root)
{
	CHECK(CLodCache::OpenMetadataFile(root / "missing.clodmeta").status == CLodCache::OpenMetadataStatus::NotFound);

	const fs::path garbagePath = root / "garbage.clodmeta";
	std::ofstream(garbagePath, std::ios::binary) << "this is not a cache";
	CHECK(CLodCache::OpenMetadataFile(garbagePath).status == CLodCache::OpenMetadataStatus::BadHeader);

	// A cache from another build reports which schema it has rather than looking corrupt.
	const fs::path source = FindMetadataFile(root, "/triangles");
	const fs::path stale = root / "stale.clodmeta";
	fs::copy_file(source, stale, fs::copy_options::overwrite_existing);
	{
		std::fstream file(stale, std::ios::binary | std::ios::in | std::ios::out);
		constexpr std::streamoff kEnvelopeBytes = sizeof(uint32_t) * 2 + sizeof(uint64_t);
		const uint32_t otherSchema = CLodCache::kSchemaVersion + 1u;
		file.seekp(kEnvelopeBytes);
		file.write(reinterpret_cast<const char*>(&otherSchema), sizeof(otherSchema));
	}
	const auto mismatch = CLodCache::OpenMetadataFile(stale);
	CHECK(mismatch.status == CLodCache::OpenMetadataStatus::SchemaMismatch);
	CHECK(mismatch.foundSchemaVersion == CLodCache::kSchemaVersion + 1u);

	// The schemas before the single layout (81: no appearance blocks, 82: UV remap section, 83: normal spread section) are
	// rejected as other schemas, not read: their bodies have no appearance counts where this one has them.
	for (const uint32_t oldSchema : { 81u, 82u, 83u }) {
		const fs::path old = root / ("schema_" + std::to_string(oldSchema) + ".clodmeta");
		fs::copy_file(source, old, fs::copy_options::overwrite_existing);
		{
			std::fstream file(old, std::ios::binary | std::ios::in | std::ios::out);
			constexpr std::streamoff kEnvelopeBytes = sizeof(uint32_t) * 2 + sizeof(uint64_t);
			file.seekp(kEnvelopeBytes);
			file.write(reinterpret_cast<const char*>(&oldSchema), sizeof(oldSchema));
		}
		const auto opened = CLodCache::OpenMetadataFile(old);
		CHECK(opened.status == CLodCache::OpenMetadataStatus::SchemaMismatch && opened.foundSchemaVersion == oldSchema);
		fs::remove(old);
	}

	// A truncated body is corrupt, not a different schema.
	const fs::path truncated = root / "truncated.clodmeta";
	fs::copy_file(source, truncated, fs::copy_options::overwrite_existing);
	fs::resize_file(truncated, fs::file_size(truncated) - 8u);
	CHECK(CLodCache::OpenMetadataFile(truncated).status != CLodCache::OpenMetadataStatus::Ok);
	fs::remove(stale);
	fs::remove(truncated);
	fs::remove(garbagePath);
}

// The sheet with a UV set (u, v = x, y), which normal spread needs.
std::vector<MeshUvSetData> SheetUvSet(const SourceMesh& mesh)
{
	MeshUvSetData set;
	set.name = "TEXCOORD_0";
	const size_t vertexCount = mesh.vertices.size() / MeshVertexLayout::BaseVertexSize;
	set.values.resize(vertexCount);
	for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
		DirectX::XMFLOAT3 position{};
		std::memcpy(&position, mesh.vertices.data() + vertex * MeshVertexLayout::BaseVertexSize + MeshVertexLayout::PositionOffset, sizeof(position));
		set.values[vertex] = { position.x, position.y };
	}
	return { std::move(set) };
}

// The one metadata layout (schema 84) carries the appearance block counts for every mesh: zero for a mesh without blocks, the stored
// counts for a mesh with them, which match the blocks its pages hold. Both kinds of mesh read back under the same schema, through
// the loader's TryLoad as well as by file, and a body whose counts disagree with themselves is corrupt.
void TestAppearanceBlocksRoundTrip(const fs::path& root, const SourceMesh& source)
{
	const std::vector<MeshUvSetData> uvSets = SheetUvSet(source);
	const auto build = [&](bool spread) {
		ClusterLODBuilderSettings settings = GetDefaultBuilderSettings();
		settings.lodNormalSpread = spread;
		return BuildClusterLODArtifactsFromGeometry(
			source.vertices, MeshVertexLayout::BaseVertexSize, nullptr, 0u, source.indices, uvSets, VertexFlags::VERTEX_NORMALS, settings);
	};
	struct Case {
		const char* primPath;
		bool spread;
	};
	for (const Case c : { Case{ "/appearance-none", false }, Case{ "/appearance-spread", true } }) {
		const ClusterLODPrebuildArtifacts artifacts = build(c.spread);
		const ClusterLODPrebuiltData& built = artifacts.prebuiltData;
		CHECK(built.uvRemapClusters == 0u && built.uvRemapBytes == 0u); // remaps need the importer's material
		CHECK(c.spread ? built.normalSpreadClusters > 0u && built.normalSpreadBytes > 0u : built.normalSpreadClusters == 0u && built.normalSpreadBytes == 0u);

		CLodCacheLoader::MeshCacheIdentity identity{ "clod_format_test.source", c.primPath, "", false };
		CHECK(CLodCacheLoader::SavePrebuilt(identity, built, artifacts.cacheBuildData.AsPayload()));
		const fs::path metadataPath = FindMetadataFile(root, c.primPath);
		const CLodCache::OpenedMetadata opened = CLodCache::OpenMetadataFile(metadataPath);
		CHECK(opened.status == CLodCache::OpenMetadataStatus::Ok && opened.foundSchemaVersion == CLodCache::kSchemaVersion);
		const ClusterLODPrebuiltData& loaded = opened.data.prebuiltData;
		CHECK(opened.data.schemaVersion == CLodCache::kSchemaVersion);
		CHECK(loaded.uvRemapClusters == built.uvRemapClusters && loaded.uvRemapBytes == built.uvRemapBytes);
		CHECK(loaded.normalSpreadClusters == built.normalSpreadClusters && loaded.normalSpreadBytes == built.normalSpreadBytes);
		const std::optional<ClusterLODPrebuiltData> viaLoader = CLodCacheLoader::TryLoadPrebuilt(identity);
		CHECK(viaLoader.has_value() && viaLoader->normalSpreadClusters == built.normalSpreadClusters && viaLoader->uvRemapClusters == 0u);

		// The pages hold what the counts say, and decode.
		const auto pages = ReadAllPages(opened);
		uint32_t blocks = 0, pagesWithBit = 0;
		uint64_t blockBytes = 0;
		for (uint32_t meshPage = 0; meshPage < loaded.trianglePageCount && meshPage < pages.size(); ++meshPage) {
			CLodCache::TrianglePage page;
			std::string error;
			if (!CLodCache::TrianglePage::Parse(pages[meshPage], page, &error)) {
				std::fprintf(stderr, "%s page %u: %s\n", c.primPath, meshPage, error.c_str());
				++g_failures;
				continue;
			}
			CHECK(!page.HasUvRemap());
			pagesWithBit += page.HasNormalSpread() ? 1u : 0u;
			for (uint32_t meshlet = 0; meshlet < page.MeshletCount(); ++meshlet) {
				const std::span<const std::byte> block = page.NormalSpreadBlock(meshlet);
				blocks += block.empty() ? 0u : 1u;
				blockBytes += block.size();
			}
		}
		std::fprintf(stderr, "%s: spread blocks %u (%u stored), pages with the bit %u\n", c.primPath, blocks, loaded.normalSpreadClusters, pagesWithBit);
		CHECK(blocks == loaded.normalSpreadClusters && blockBytes == loaded.normalSpreadBytes);
		CHECK((pagesWithBit != 0u) == c.spread);
		CheckTerminalGeometryMatchesSource(loaded, pages, source);
	}

	// Counts without bytes (or bytes without counts) are corrupt, not a mesh without blocks.
	{
		const ClusterLODPrebuildArtifacts artifacts = build(false);
		ClusterLODPrebuiltData bad = artifacts.prebuiltData;
		bad.uvRemapClusters = 1u;
		CLodCacheLoader::MeshCacheIdentity identity{ "clod_format_test.source", "/appearance-bad-counts", "", false };
		CHECK(CLodCacheLoader::SavePrebuilt(identity, bad, artifacts.cacheBuildData.AsPayload()));
		bool foundCorrupt = false;
		for (const auto& entry : fs::recursive_directory_iterator(root)) {
			if (entry.path().extension() != L".clodmeta") continue;
			const auto opened = CLodCache::OpenMetadataFile(entry.path());
			if (opened.status == CLodCache::OpenMetadataStatus::Corrupt) foundCorrupt = true;
		}
		CHECK(foundCorrupt);
	}
}

// The builder defaults are the recommended build, every one can be turned back off from the environment, and the cache key
// follows the effective values: a variable set to its default keys the same caches as leaving it unset, any other value does not.
void TestDefaultsAndCacheKey()
{
	const ClusterLODBuilderSettings defaults = GetDefaultBuilderSettings();
	CHECK(defaults.disableSloppyFallback && defaults.simplifyUpdate && !defaults.simplifyUpdateMeasuredError && defaults.simplifyUpdateUvTransfer);
	CHECK(defaults.lodErrorChannels && defaults.lodErrorChannelsTexture == 1.0f);
	CHECK(defaults.simplifyTopology && defaults.simplifyTopologyRelaxUv && defaults.simplifyTopologyTextureCharts &&
		defaults.simplifyTopologyTextureColor == 10.0f);
	CHECK(defaults.coveragePreservationMode == ClusterLODCoveragePreservationMode::PrioritizeEdges);
	CHECK(!defaults.simplifyValidation && !defaults.simplifyOptimal && !defaults.lodNormalSpread && defaults.lodErrorChannelsTextureRemap == 0.0f &&
		defaults.lodErrorChannelsTextureRemapLayout == 1u && defaults.lodErrorChannelsTextureColor == 0.0f);
	CHECK(!ClusterLODUvRemapActive(defaults) && !ClusterLODNormalSpreadActive(defaults) && ClusterLODTopologyTextureColorActive(defaults));
	// The struct and GetDefaultBuilderSettings agree on everything the key hashes by value.
	const ClusterLODBuilderSettings plain{};
	CHECK(plain.disableSloppyFallback == defaults.disableSloppyFallback && plain.simplifyTopologyTextureColor == defaults.simplifyTopologyTextureColor &&
		plain.sloppyFallbackErrorFactor == defaults.sloppyFallbackErrorFactor && plain.simplifyPermissive == defaults.simplifyPermissive &&
		plain.normalAttributeWeight == defaults.normalAttributeWeight);

	// The key is computed once per thread from the process environment, so each case asks a fresh thread.
	const auto keyWith = [](std::vector<std::pair<const char*, const char*>> variables) {
		for (const auto& [name, value] : variables) _putenv_s(name, value);
		uint64_t key = 0;
		ClusterLODBuilderSettings effective{};
		std::thread([&] {
			key = CLodCache::ComputeBuildConfigHash("clod_format_test.key");
			effective = GetDefaultBuilderSettings("clod_format_test.key");
		}).join();
		for (const auto& [name, value] : variables) _putenv_s(name, "");
		return std::pair{ key, effective };
	};
	const auto [unset, unsetSettings] = keyWith({});
	const auto [explicitDefaults, explicitSettings] = keyWith({
		{ "BASICRENDERER_CLOD_DISABLE_SLOPPY_FALLBACK", "1" }, { "BASICRENDERER_CLOD_SIMPLIFY_UPDATE", "1" },
		{ "BASICRENDERER_CLOD_SIMPLIFY_UPDATE_MEASURED_ERROR", "0" }, { "BASICRENDERER_CLOD_ERROR_CHANNELS", "1" },
		{ "BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY", "1" }, { "BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY_RELAX_UV", "1" },
		{ "BASICRENDERER_CLOD_SIMPLIFY_UPDATE_UV_TRANSFER", "1" }, { "BASICRENDERER_CLOD_ERROR_CHANNELS_TEXTURE", "1" },
		{ "BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY_TEXTURE_CHARTS", "1" }, { "BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY_TEXTURE_COLOR", "10.0" },
		{ "BASICRENDERER_CLOD_NORMAL_SPREAD", "0" }, { "BASICRENDERER_CLOD_ERROR_CHANNELS_TEXTURE_REMAP_LAYOUT", "1" } });
	CHECK(unset == explicitDefaults);
	const auto [noTopology, noTopologySettings] = keyWith({ { "BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY", "0" } });
	CHECK(noTopology != unset && !noTopologySettings.simplifyTopology && unsetSettings.simplifyTopology);
	const auto [sloppy, sloppySettings] = keyWith({ { "BASICRENDERER_CLOD_DISABLE_SLOPPY_FALLBACK", "0" } });
	CHECK(sloppy != unset && !sloppySettings.disableSloppyFallback);
	// The last resort is on by default, has its own key when off, and does nothing (same key) under the global fallback.
	CHECK(unsetSettings.sloppyLastResort);
	const auto [noLastResort, noLastResortSettings] = keyWith({ { "BASICRENDERER_CLOD_SLOPPY_LAST_RESORT", "0" } });
	CHECK(noLastResort != unset && noLastResort != sloppy && !noLastResortSettings.sloppyLastResort);
	const auto [sloppyNoLastResort, sloppyNoLastResortSettings] =
		keyWith({ { "BASICRENDERER_CLOD_DISABLE_SLOPPY_FALLBACK", "0" }, { "BASICRENDERER_CLOD_SLOPPY_LAST_RESORT", "0" } });
	CHECK(sloppyNoLastResort == sloppy);
	// Rejected groups are off by default (only stuck ones); fit and the last resort's error factor are keyed, all only with the
	// last resort on.
	const auto [withRejected, withRejectedSettings] = keyWith({ { "BASICRENDERER_CLOD_SLOPPY_LAST_RESORT_REJECTED", "1" } });
	CHECK(!unsetSettings.sloppyLastResortRejected && withRejectedSettings.sloppyLastResortRejected);
	CHECK(withRejected != unset && withRejected != noLastResort);
	const auto [noFit, noFitSettings] = keyWith({ { "BASICRENDERER_CLOD_SLOPPY_LAST_RESORT_FIT", "0" } });
	CHECK(unsetSettings.sloppyLastResortFit && !noFitSettings.sloppyLastResortFit);
	CHECK(noFit != unset && noFit != noLastResort && noFit != withRejected);
	const auto [factor, factorSettings] = keyWith({ { "BASICRENDERER_CLOD_SLOPPY_LAST_RESORT_ERROR_FACTOR", "4" } });
	CHECK(unsetSettings.sloppyLastResortErrorFactor == 0.0f && factorSettings.sloppyLastResortErrorFactor == 4.0f);
	CHECK(factor != unset && factor != noFit);
	const auto [offAll, offAllSettings] = keyWith({ { "BASICRENDERER_CLOD_SLOPPY_LAST_RESORT", "0" }, { "BASICRENDERER_CLOD_SLOPPY_LAST_RESORT_REJECTED", "1" },
		{ "BASICRENDERER_CLOD_SLOPPY_LAST_RESORT_FIT", "0" }, { "BASICRENDERER_CLOD_SLOPPY_LAST_RESORT_ERROR_FACTOR", "4" } });
	CHECK(offAll == noLastResort);
	const auto [colour, colourSettings] = keyWith({ { "BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY_TEXTURE_COLOR", "0" } });
	CHECK(colour != unset && colourSettings.simplifyTopologyTextureColor == 0.0f && !ClusterLODTopologyTextureColorActive(colourSettings));
	const auto [layout2, layout2Settings] = keyWith({ { "BASICRENDERER_CLOD_ERROR_CHANNELS_TEXTURE_REMAP_LAYOUT", "2" } });
	CHECK(layout2 != unset && layout2Settings.lodErrorChannelsTextureRemapLayout == 2u);
	const auto [spread, spreadSettings] = keyWith({ { "BASICRENDERER_CLOD_NORMAL_SPREAD", "1" } });
	CHECK(spread != unset && spreadSettings.lodNormalSpread && spread != layout2);
	CHECK(explicitSettings.simplifyTopology == unsetSettings.simplifyTopology);
	// Build cost defaults: facing bins (grid 8) and one error-channel area sample per triangle, keyed by effective value: the
	// explicit defaults (1 is grid 8, and a sample count other than 1 or 2 is 4) key as unset; other values differ.
	CHECK(unsetSettings.simplifyMatchFacingBins == 8u && ClusterLODMatchFacingGrid(unsetSettings) == 8u && unsetSettings.lodErrorChannelsSamples == 1u &&
		unsetSettings.simplifyValidationSamples == 4u && unsetSettings.simplifyUpdateFitSamples == 31u && unsetSettings.simplifyMatchCandidates == 0u);
	const auto [costDefaults, costDefaultsSettings] = keyWith({ { "BASICRENDERER_CLOD_SIMPLIFY_MATCH_FACING_BINS", "8" },
		{ "BASICRENDERER_CLOD_ERROR_CHANNELS_SAMPLES", "1" }, { "BASICRENDERER_CLOD_SIMPLIFY_VALIDATION_SAMPLES", "4" },
		{ "BASICRENDERER_CLOD_SIMPLIFY_UPDATE_FIT_SAMPLES", "31" }, { "BASICRENDERER_CLOD_SIMPLIFY_MATCH_REACH", "1.1" } });
	CHECK(costDefaults == unset);
	const auto [binsOne, binsOneSettings] = keyWith({ { "BASICRENDERER_CLOD_SIMPLIFY_MATCH_FACING_BINS", "1" } });
	CHECK(binsOne == unset && ClusterLODMatchFacingGrid(binsOneSettings) == 8u);
	const auto [samplesThree, samplesThreeSettings] = keyWith({ { "BASICRENDERER_CLOD_SIMPLIFY_VALIDATION_SAMPLES", "3" } });
	CHECK(samplesThree == unset && samplesThreeSettings.simplifyValidationSamples == 3u);
	const auto [exactMatch, exactMatchSettings] = keyWith({ { "BASICRENDERER_CLOD_SIMPLIFY_MATCH_FACING_BINS", "0" } });
	CHECK(exactMatch != unset && ClusterLODMatchFacingGrid(exactMatchSettings) == 0u);
	const auto [bins16, bins16Settings] = keyWith({ { "BASICRENDERER_CLOD_SIMPLIFY_MATCH_FACING_BINS", "16" } });
	CHECK(bins16 != unset && bins16 != exactMatch && ClusterLODMatchFacingGrid(bins16Settings) == 16u);
	const auto [channelSamples4, channelSamples4Settings] = keyWith({ { "BASICRENDERER_CLOD_ERROR_CHANNELS_SAMPLES", "4" } });
	CHECK(channelSamples4 != unset && channelSamples4 != exactMatch && channelSamples4Settings.lodErrorChannelsSamples == 4u);
	// Under facing bins the candidate limit does nothing (same key); without them it has its own.
	const auto [binsWithCap, binsWithCapSettings] = keyWith({ { "BASICRENDERER_CLOD_SIMPLIFY_MATCH_CANDIDATES", "16" } });
	CHECK(binsWithCap == unset && binsWithCapSettings.simplifyMatchCandidates == 16u);
	const auto [cap, capSettings] = keyWith({ { "BASICRENDERER_CLOD_SIMPLIFY_MATCH_FACING_BINS", "0" }, { "BASICRENDERER_CLOD_SIMPLIFY_MATCH_CANDIDATES", "16" } });
	CHECK(cap != exactMatch && cap != unset && ClusterLODMatchFacingGrid(capSettings) == 0u);
	const auto [fitMask, fitMaskSettings] = keyWith({ { "BASICRENDERER_CLOD_SIMPLIFY_UPDATE_FIT_SAMPLES", "17" } });
	CHECK(fitMask != unset && fitMaskSettings.simplifyUpdateFitSamples == 17u);
}

// Opacity micromap sidecars: what's written reads back, and a file for another material, another container, or with an
// index past its micromaps is refused rather than built from.
void TestOpacityMicromapRoundTrip(const fs::path& root)
{
	CLodCache::OpacityMicromapMaterialKey key{};
	key.baseColorTexture = "textures/trees/leaf.dds|processed";
	key.alphaCutoff = 128.0f / 255.0f;
	const uint32_t identity = CLodCache::OpacityMicromapIdentity(key);
	CHECK(identity != 0u);
	CHECK(identity == CLodCache::OpacityMicromapIdentity(key));
	CLodCache::OpacityMicromapMaterialKey other = key;
	other.alphaCutoff = 100.0f / 255.0f;
	CHECK(identity != CLodCache::OpacityMicromapIdentity(other));
	other = key;
	other.baseColorTexture = "textures/trees/leaf2.dds|processed";
	CHECK(identity != CLodCache::OpacityMicromapIdentity(other));

	const std::wstring container = (root / "clod_0123.clodbin").wstring();
	const std::wstring path = CLodCache::OpacityMicromapPath(container, identity);
	CHECK(fs::path(path).parent_path() == root);
	CHECK(fs::path(path).extension() == L".clodomm");

	// Group 1 of 3: two clusters, two level-4 micromaps (64 bytes each), the rest special indices.
	CLodCache::OpacityMicromapGroupInput group{};
	group.groupLocalIndex = 1u;
	group.clusterCount = 2u;
	group.descs = { { 0u, 4u, CLodCache::kOpacityMicromapFormat4State }, { 64u, 4u, CLodCache::kOpacityMicromapFormat4State } };
	group.indices.assign(2u * CLodCache::kOpacityMicromapMaxTrianglesPerCluster, -2);
	group.indices[0] = 0;
	group.indices[CLodCache::kOpacityMicromapMaxTrianglesPerCluster + 5u] = 1;
	group.indices[3] = -1;
	group.data.resize(128u);
	for (size_t i = 0; i < group.data.size(); ++i) group.data[i] = static_cast<std::byte>(i * 7u);
	CLodCache::OpacityMicromapFileInfo info{};
	info.identity = identity;
	info.subdivisionLevel = 4u;
	info.containerBytes = 123456u;
	info.groupCount = 3u;
	std::string error;
	CHECK(CLodCache::SaveOpacityMicromaps(path, info, std::span(&group, 1), &error));

	const auto file = CLodCache::OpacityMicromapFile::Open(path, identity, 123456u, &error);
	CHECK(file != nullptr);
	if (file) {
		CLodCache::OpacityMicromapGroupView view{};
		CHECK(!file->Group(0u, view));
		CHECK(!file->Group(2u, view));
		CHECK(file->Group(1u, view));
		CHECK(view.clusterCount == 2u);
		CHECK(view.descs.size() == 2u && view.descs[1].byteOffset == 64u && view.descs[1].subdivisionLevel == 4u);
		CHECK(view.levelCounts[4] == 2u);
		CHECK(view.data.size() == 128u && std::memcmp(view.data.data(), group.data.data(), 128u) == 0);
		CHECK(view.indices.size() == 2u * CLodCache::kOpacityMicromapIndexUnitBytes);
		CHECK(std::memcmp(view.indices.data(), group.indices.data(), view.indices.size()) == 0);
		CHECK(view.blob.data() + view.indicesOffset == view.indices.data());
		CHECK(view.blob.data() + view.dataOffset == view.data.data());
	}
	CHECK(CLodCache::OpacityMicromapFile::Open(path, identity + 1u, 123456u, &error) == nullptr);
	CHECK(CLodCache::OpacityMicromapFile::Open(path, identity, 123457u, &error) == nullptr);
	CHECK(CLodCache::OpacityMicromapFile::Open((root / "missing.clodomm").wstring(), identity, 123456u, &error) == nullptr);

	// An index past the group's micromaps.
	group.indices[7] = 2;
	const std::wstring bad = (root / "bad.clodomm").wstring();
	CHECK(CLodCache::SaveOpacityMicromaps(bad, info, std::span(&group, 1), &error));
	CHECK(CLodCache::OpacityMicromapFile::Open(bad, identity, 123456u, &error) == nullptr);
	std::error_code ignored;
	fs::remove(path, ignored);
	fs::remove(bad, ignored);
}

// The file layout has to match ID3D12Device::GetCopyableFootprints, which the runtime reads it with, but tools
// without a device compute it. These sizes come from files the device laid out.
void TestProcessedTextureLayoutMatchesDevice()
{
	const auto chainBytes = [](uint32_t size, uint32_t blockBytes) {
		uint64_t end = 0;
		for (uint32_t mip = size;; mip /= 2u) {
			end = br::processed_texture_cache::EndOfSubresource(
				br::processed_texture_cache::PlaceSubresource(end, mip, mip, 4u, 4u, blockBytes));
			if (mip == 1u) return end;
		}
	};
	CHECK(chainBytes(2048u, 16u) == 5595664u);
	CHECK(chainBytes(2048u, 8u) == 2802184u);

	const auto first = br::processed_texture_cache::PlaceSubresource(0u, 3u, 5u, 1u, 1u, 4u);
	CHECK(first.offset == 0u && first.rowSize == 12u && first.rowPitch == 256u && first.rowCount == 5u);
	CHECK(br::processed_texture_cache::EndOfSubresource(first) == 256u * 4u + 12u);
	const auto second = br::processed_texture_cache::PlaceSubresource(1036u, 1u, 1u, 1u, 1u, 4u);
	CHECK(second.offset == 1536u);
}

void TestCacheOnlyMode(const SourceMesh& source)
{
	CHECK(CLodCache::GetCacheOnlyViolationCount() == 0u);
	CLodCache::SetCacheOnlyMode(true);

	// A saved cache still loads.
	CLodCacheLoader::MeshCacheIdentity saved{ "clod_format_test.source", "/triangles", "", false };
	CHECK(CLodCacheLoader::TryLoadPrebuilt(saved).has_value());

	// A miss is not an error by itself, because importers look before they build...
	CLodCacheLoader::MeshCacheIdentity missing{ "clod_format_test.source", "/not-built", "", false };
	CHECK(!CLodCacheLoader::TryLoadPrebuilt(missing).has_value());
	CHECK(CLodCache::GetCacheOnlyViolationCount() == 0u);

	// ...but the build that would follow it is.
	bool threw = false;
	try {
		(void)Build(source, ClusterLODCoveragePreservationMode::PrioritizeEdges);
	}
	catch (const std::runtime_error&) {
		threw = true;
	}
	CHECK(threw);
	CHECK(CLodCache::GetCacheOnlyViolationCount() == 1u);
	CLodCache::SetCacheOnlyMode(false);
}
// A KTX2 written by libktx: levels given bottom-up from level 0, optionally Zstandard supercompressed.
std::vector<uint8_t> WriteTestKtx2(uint32_t vkFormat, uint32_t width, uint32_t height, const std::vector<std::vector<uint8_t>>& levels, bool zstd)
{
	ktxTextureCreateInfo info{};
	info.vkFormat = vkFormat;
	info.baseWidth = width;
	info.baseHeight = height;
	info.baseDepth = 1;
	info.numDimensions = 2;
	info.numLevels = static_cast<uint32_t>(levels.size());
	info.numLayers = 1;
	info.numFaces = 1;
	info.isArray = KTX_FALSE;
	info.generateMipmaps = KTX_FALSE;
	ktxTexture2* texture = nullptr;
	if (ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture) != KTX_SUCCESS) {
		CHECK(false);
		return {};
	}
	for (uint32_t level = 0; level < levels.size(); ++level) {
		CHECK(ktxTexture_SetImageFromMemory(ktxTexture(texture), level, 0, 0, levels[level].data(), levels[level].size()) == KTX_SUCCESS);
	}
	if (zstd) CHECK(ktxTexture2_DeflateZstd(texture, 5) == KTX_SUCCESS);
	ktx_uint8_t* written = nullptr;
	ktx_size_t writtenSize = 0;
	CHECK(ktxTexture_WriteToMemory(ktxTexture(texture), &written, &writtenSize) == KTX_SUCCESS);
	ktxTexture_Destroy(ktxTexture(texture));
	std::vector<uint8_t> file(written, written + writtenSize);
	std::free(written);
	return file;
}

bool TexelIs(const clod_texture::Texture& texture, uint32_t level, uint32_t x, uint32_t y, std::array<uint8_t, 4> expected, int tolerance = 0)
{
	if (level >= texture.Levels()) return false;
	const uint8_t* texel = &texture.mips[level][(static_cast<size_t>(y) * texture.MipWidth(level) + x) * 4u];
	for (int c = 0; c < 4; ++c) {
		if (std::abs(static_cast<int>(texel[c]) - expected[c]) > tolerance) return false;
	}
	return true;
}

// The stage 6 KTX2 decoder (ClusterLODTextureDecode): block formats decode bit-exactly to the RGBA8 the GPU samples, sRGB bytes
// pass through untouched, a short mip chain is extended to 1x1, B8G8R8A8 is swizzled, Basis UASTC transcodes, and what it does not
// read is refused (identity 0) so importers keep the binary term.
void TestKtx2TextureDecode()
{
	// BC1 sRGB, 8x8 with two of its four levels, Zstandard: level 0 all colour0 (pure red), level 1 all colour1 (pure blue).
	const std::vector<uint8_t> red = { 0x00, 0xF8, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00 };
	const std::vector<uint8_t> blue = { 0x00, 0xF8, 0x1F, 0x00, 0x55, 0x55, 0x55, 0x55 };
	std::vector<uint8_t> level0;
	for (int block = 0; block < 4; ++block) level0.insert(level0.end(), red.begin(), red.end());
	const std::vector<uint8_t> bc1 = WriteTestKtx2(132u, 8u, 8u, { level0, blue }, true);
	CHECK(clod_texture::IsKtx2(bc1) && clod_texture::Ktx2DecoderIdentity(bc1) == clod_texture::kKtx2DecoderRevision);
	std::string error;
	const auto decoded = clod_texture::DecodeKtx2(bc1, &error);
	CHECK(decoded != nullptr);
	if (decoded != nullptr) {
		CHECK(decoded->width == 8u && decoded->height == 8u && decoded->Levels() == 4u);
		CHECK(TexelIs(*decoded, 0, 0, 0, { 255, 0, 0, 255 }) && TexelIs(*decoded, 0, 7, 7, { 255, 0, 0, 255 }));
		CHECK(TexelIs(*decoded, 1, 3, 3, { 0, 0, 255, 255 }));
		CHECK(TexelIs(*decoded, 3, 0, 0, { 0, 0, 255, 255 })); // extended by BuildMips from the stored level 1
	}

	// BC7 sRGB, one mode 6 block with both endpoints (201, 101, 51, 255): every texel is that colour, bit-exactly.
	std::array<uint8_t, 16> bc7{};
	uint32_t bit = 0;
	const auto put = [&](uint32_t value, uint32_t bits) {
		for (uint32_t i = 0; i < bits; ++i, ++bit) {
			if ((value >> i) & 1u) bc7[bit / 8u] |= static_cast<uint8_t>(1u << (bit % 8u));
		}
	};
	put(1u << 6, 7);                                 // mode 6
	for (uint32_t c7 : { 100u, 50u, 25u, 127u }) {  // R, G, B, A: endpoint 0 then 1, 7 bits each
		put(c7, 7);
		put(c7, 7);
	}
	put(1u, 1);  // p-bit 0
	put(1u, 1);  // p-bit 1 (indices stay 0)
	const std::vector<uint8_t> bc7File = WriteTestKtx2(146u, 4u, 4u, { std::vector<uint8_t>(bc7.begin(), bc7.end()) }, false);
	const auto bc7Decoded = clod_texture::DecodeKtx2(bc7File, &error);
	CHECK(bc7Decoded != nullptr && TexelIs(*bc7Decoded, 0, 0, 0, { 201, 101, 51, 255 }) && TexelIs(*bc7Decoded, 0, 3, 2, { 201, 101, 51, 255 }));

	// B8G8R8A8 sRGB: swizzled, bytes untouched.
	const std::vector<uint8_t> bgraFile = WriteTestKtx2(50u, 2u, 1u, { { 10, 20, 30, 40, 50, 60, 70, 80 } }, false);
	const auto bgra = clod_texture::DecodeKtx2(bgraFile, &error);
	CHECK(bgra != nullptr && bgra->Levels() == 2u && TexelIs(*bgra, 0, 0, 0, { 30, 20, 10, 40 }) && TexelIs(*bgra, 0, 1, 0, { 70, 60, 50, 80 }));

	// Basis UASTC (when this libktx can encode it): transcoded to RGBA8 near the solid colour it encoded.
	{
		std::vector<uint8_t> solid(16u * 16u * 4u);
		for (size_t i = 0; i < solid.size(); i += 4u) {
			solid[i] = 180;
			solid[i + 1u] = 90;
			solid[i + 2u] = 30;
			solid[i + 3u] = 255;
		}
		const std::vector<uint8_t> rgba = WriteTestKtx2(43u, 16u, 16u, { solid }, false);
		ktxTexture2* texture = nullptr;
		if (ktxTexture2_CreateFromMemory(rgba.data(), rgba.size(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &texture) == KTX_SUCCESS) {
			ktxBasisParams params{};
			params.structSize = sizeof(params);
			params.uastc = KTX_TRUE;
			params.threadCount = 1;
			ktx_uint8_t* written = nullptr;
			ktx_size_t writtenSize = 0;
			if (ktxTexture2_CompressBasisEx(texture, &params) == KTX_SUCCESS &&
				ktxTexture_WriteToMemory(ktxTexture(texture), &written, &writtenSize) == KTX_SUCCESS) {
				const std::vector<uint8_t> uastc(written, written + writtenSize);
				std::free(written);
				CHECK(clod_texture::Ktx2DecoderIdentity(uastc) == clod_texture::kKtx2DecoderRevision);
				const auto transcoded = clod_texture::DecodeKtx2(uastc, &error);
				CHECK(transcoded != nullptr && transcoded->width == 16u && TexelIs(*transcoded, 0, 5, 9, { 180, 90, 30, 255 }, 4));
			}
			ktxTexture_Destroy(ktxTexture(texture));
		}
	}

	// Refused: BC6H (float), and anything that is not a KTX2.
	const std::vector<uint8_t> bc6File = WriteTestKtx2(143u, 4u, 4u, { std::vector<uint8_t>(16u, 0u) }, false);
	CHECK(clod_texture::IsKtx2(bc6File) && clod_texture::Ktx2DecoderIdentity(bc6File) == 0u);
	CHECK(clod_texture::DecodeKtx2(bc6File, &error) == nullptr && !error.empty());
	const std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 13 };
	CHECK(!clod_texture::IsKtx2(png) && clod_texture::Ktx2DecoderIdentity(png) == 0u && clod_texture::DecodeKtx2(png) == nullptr);
	CHECK(clod_texture::DecodeKtx2(std::span<const uint8_t>(bc1.data(), 100u)) == nullptr); // truncated
}
}

int main()
{
	// Set before anything asks for the cache root, which is resolved once per process.
	wchar_t tempDirectory[MAX_PATH] = {};
	GetTempPathW(MAX_PATH, tempDirectory);
	const fs::path root = fs::path(tempDirectory) / ("clod_format_tests_" + std::to_string(GetCurrentProcessId()));
	fs::remove_all(root);
	fs::create_directories(root);
	SetEnvironmentVariableW(L"SARP_CACHE_ROOT", root.c_str());

	auto& scheduler = br::TaskSchedulerManager::GetInstance();
	scheduler.Initialize();

	int result = 0;
	try {
		const SourceMesh source = MakeBumpySheet(256u);
		TestPrioritizeEdgesRoundTrip(root, source);
		TestVoxelRoundTrip(root, source);
		TestVoxelForkRoundTrip(root, source);
		TestForkedAssemblyPart(root, source);
		TestVoxelReplaceClosure();
		TestOpenFailures(root);
		TestAppearanceBlocksRoundTrip(root, source);
		TestDefaultsAndCacheKey();
		TestOpacityMicromapRoundTrip(root);
		TestCacheOnlyMode(source);
		TestProcessedTextureLayoutMatchesDevice();
		TestKtx2TextureDecode();
	}
	catch (const std::exception& error) {
		std::fprintf(stderr, "unexpected exception: %s\n", error.what());
		result = 1;
	}

	scheduler.Cleanup();
	std::error_code removeError;
	fs::remove_all(root, removeError);
	if (g_failures != 0) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	return result;
}
