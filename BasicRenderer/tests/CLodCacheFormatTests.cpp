// Builds a hierarchy from a procedural mesh, saves it, opens it by file and decodes every page.
// This is the only test that runs the generator, so it also checks what the build produced.

#include <BasicRenderer/Assets/CLodCacheStore.h>
#include <BasicRenderer/Assets/CLodPageDecoder.h>
#include <BasicRenderer/Assets/ClusterLODTypes.h>
#include <BasicRenderer/Assets/DefaultCLodSettings.h>
#include <BasicRenderer/Assets/Import/CLodCache.h>
#include <BasicRenderer/Assets/Import/CLodOpacityMicromapCache.h>
#include <BasicRenderer/Assets/Import/ClusterLODUtilities.h>
#include <BasicRenderer/Assets/Geometry/VertexLayout.h>
#include <BasicRenderer/Streaming/TaskScheduler.h>

#include "../src/Assets/Textures/Cache/ProcessedTextureCache.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include <windows.h>

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

ClusterLODPrebuildArtifacts Build(const SourceMesh& mesh, ClusterLODCoveragePreservationMode mode)
{
	ClusterLODBuilderSettings settings = GetDefaultBuilderSettings();
	settings.coveragePreservationMode = mode;
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

	// A truncated body is corrupt, not a different schema.
	const fs::path truncated = root / "truncated.clodmeta";
	fs::copy_file(source, truncated, fs::copy_options::overwrite_existing);
	fs::resize_file(truncated, fs::file_size(truncated) - 8u);
	CHECK(CLodCache::OpenMetadataFile(truncated).status != CLodCache::OpenMetadataStatus::Ok);
	fs::remove(stale);
	fs::remove(truncated);
	fs::remove(garbagePath);
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
		TestOpenFailures(root);
		TestOpacityMicromapRoundTrip(root);
		TestCacheOnlyMode(source);
		TestProcessedTextureLayoutMatchesDevice();
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
