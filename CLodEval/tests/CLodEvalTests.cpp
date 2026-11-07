// Checks the numbers CLodEval reports against a hierarchy it builds here, and its comparison rules
// against hand-made results. No GPU, no source assets.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include <windows.h>

#include <BasicRenderer/Assets/CLodCacheStore.h>
#include <BasicRenderer/Assets/DefaultCLodSettings.h>
#include <BasicRenderer/Assets/Geometry/VertexLayout.h>
#include <BasicRenderer/Assets/Import/ClusterLODUtilities.h>
#include <BasicRenderer/Streaming/TaskScheduler.h>

#include <BasicRenderer/Assets/VoxelSurfaceTrace.h>

#include "Analyzers/Analyzers.h"
#include "Capture.h"
#include "Core/Image.h"
#include "Transition.h"
#include "Core/Results.h"
#include "Suite.h"

namespace {
namespace fs = std::filesystem;
using namespace clodeval;

int g_failures = 0;

#define CHECK(condition)                                                              \
	do {                                                                              \
		if (!(condition)) {                                                           \
			std::fprintf(stderr, "%s:%d FAILED: %s\n", __FILE__, __LINE__, #condition); \
			++g_failures;                                                             \
		}                                                                             \
	} while (false)

struct SourceMesh {
	std::vector<std::byte> vertices;
	std::vector<uint32_t> indices;
};

SourceMesh MakeSheet(uint32_t resolution, float zOffset)
{
	SourceMesh mesh;
	const auto height = [](float x, float y) { return 0.08f * std::sin(x * 37.0f) * std::cos(y * 41.0f) + 0.03f * std::sin(x * 131.0f + y * 113.0f); };
	for (uint32_t row = 0; row <= resolution; ++row) {
		for (uint32_t column = 0; column <= resolution; ++column) {
			const float x = static_cast<float>(column) / static_cast<float>(resolution);
			const float y = static_cast<float>(row) / static_cast<float>(resolution);
			const DirectX::XMFLOAT3 position{ x, y, height(x, y) + zOffset };
			constexpr float kStep = 1.0e-3f;
			const float dx = (height(x + kStep, y) - height(x - kStep, y)) / (2.0f * kStep);
			const float dy = (height(x, y + kStep) - height(x, y - kStep)) / (2.0f * kStep);
			const float length = std::sqrt(dx * dx + dy * dy + 1.0f);
			const DirectX::XMFLOAT3 normal{ -dx / length, -dy / length, 1.0f / length };
			const size_t offset = mesh.vertices.size();
			mesh.vertices.resize(offset + MeshVertexLayout::BaseVertexSize);
			std::memcpy(mesh.vertices.data() + offset, &position, sizeof(position));
			std::memcpy(mesh.vertices.data() + offset + MeshVertexLayout::NormalOffset, &normal, sizeof(normal));
		}
	}
	const uint32_t stride = resolution + 1u;
	for (uint32_t row = 0; row < resolution; ++row) {
		for (uint32_t column = 0; column < resolution; ++column) {
			const uint32_t a = row * stride + column;
			mesh.indices.insert(mesh.indices.end(), { a, a + 1u, a + stride + 1u, a, a + stride + 1u, a + stride });
		}
	}
	return mesh;
}

fs::path SaveBuild(
	const fs::path& root,
	const SourceMesh& source,
	ClusterLODCoveragePreservationMode mode,
	const std::string& primPath,
	ClusterLODVoxelForkMode fork = ClusterLODVoxelForkMode::Off)
{
	ClusterLODBuilderSettings settings = GetDefaultBuilderSettings();
	settings.coveragePreservationMode = mode;
	settings.voxelFork = fork;
	const ClusterLODPrebuildArtifacts artifacts = BuildClusterLODArtifactsFromGeometry(
		source.vertices, MeshVertexLayout::BaseVertexSize, nullptr, 0u, source.indices, {}, VertexFlags::VERTEX_NORMALS, settings);
	CHECK(CLodCacheLoader::SavePrebuilt({ "clod_eval_test.source", primPath, "", false }, artifacts.prebuiltData, artifacts.cacheBuildData.AsPayload()));
	for (const fs::path& path : FindCacheMetadata(root)) {
		std::string error;
		const auto mesh = CachedMesh::Open(path, error);
		if (mesh && mesh->Prebuilt().cacheSource.primPath == primPath) {
			return path;
		}
	}
	CHECK(false);
	return {};
}

double Value(const ResultSet& results, const std::string& id, const Tags& tags = {}, const std::string& mesh = "*")
{
	for (const MetricRecord& record : results.records) {
		if (record.id == id && record.mesh == mesh && record.tags == tags) {
			return record.value;
		}
	}
	std::fprintf(stderr, "metric %s not found\n", id.c_str());
	++g_failures;
	return std::numeric_limits<double>::quiet_NaN();
}

ResultSet Analyze(const fs::path& metadata, const std::string& build)
{
	ResultSet results;
	std::string error;
	CHECK(AnalyzeMeshes({ { metadata, "sheet" } }, build, build, {}, results, error));
	if (!error.empty()) {
		std::fprintf(stderr, "analyze: %s\n", error.c_str());
	}
	return results;
}

void TestTriangleHierarchy(const fs::path& root)
{
	constexpr uint32_t kResolution = 256u;
	const SourceMesh source = MakeSheet(kResolution, 0.0f);
	const fs::path metadata = SaveBuild(root, source, ClusterLODCoveragePreservationMode::PrioritizeEdges, "/triangles");
	const ResultSet results = Analyze(metadata, "triangles");

	CHECK(Value(results, "hierarchy.leaf_triangles") == static_cast<double>(source.indices.size() / 3u));
	CHECK(Value(results, "hierarchy.voxel_groups") == 0.0);
	CHECK(Value(results, "hierarchy.max_depth") >= 1.0);
	// Coarser levels really are coarser.
	CHECK(Value(results, "hierarchy.coarsest_triangles") < Value(results, "hierarchy.leaf_triangles"));

	// A healthy hierarchy has no consistency problems.
	for (const char* problem : { "validity.nonfinite_group_error", "validity.bad_refined_group", "validity.bad_segment_page", "validity.page_kind_mismatch",
			 "validity.bad_node_range", "validity.unreachable_groups", "validity.bad_pages" }) {
		if (Value(results, problem) != 0.0) {
			std::fprintf(stderr, "%s = %g\n", problem, Value(results, problem));
			++g_failures;
		}
	}

	// Page and stream totals account for the whole container, less its header and directory.
	const auto mesh = [&] { std::string error; return CachedMesh::Open(metadata, error); }();
	CHECK(mesh != nullptr);
	double streamTotal = 0.0;
	for (const MetricRecord& record : results.records) {
		if (record.id == "memory.stream.bytes" && record.mesh == "*") {
			streamTotal += record.value;
		}
	}
	const double pages = Value(results, "memory.triangle_pages.bytes");
	CHECK(streamTotal == pages);
	uint64_t pageBytes = 0;
	for (const ClusterLODGroupDiskLocator& locator : mesh->Prebuilt().pageDiskLocators) {
		pageBytes += locator.blobSizeBytes;
	}
	CHECK(pages == static_cast<double>(pageBytes));
	CHECK(pages <= static_cast<double>(mesh->ContainerBytes()));
	CHECK(Value(results, "memory.total.bytes") == static_cast<double>(mesh->ContainerBytes() + mesh->MetadataBytes()));
}

void TestVoxelHierarchy(const fs::path& root)
{
	const SourceMesh source = MakeSheet(256u, 0.0f);
	const fs::path metadata = SaveBuild(root, source, ClusterLODCoveragePreservationMode::Voxel, "/voxels");
	const ResultSet results = Analyze(metadata, "voxels");

	CHECK(Value(results, "hierarchy.voxel_groups") > 0.0);
	CHECK(Value(results, "memory.voxel_pages.bytes") > 0.0);
	CHECK(Value(results, "hierarchy.leaf_triangles") == static_cast<double>(source.indices.size() / 3u));

	// Attributes are 28 bytes a cell today, so the current scheme's total is exact.
	const double cells = Value(results, "memory.voxel.cells");
	CHECK(cells > 0.0);
	CHECK(Value(results, "memory.whatif.voxel_attributes.bytes", { { "scheme", "current" } }) == cells * 28.0);
	CHECK(Value(results, "memory.whatif.voxel_attributes.bytes", { { "scheme", "quantized_13b" } }) == cells * 13.0);
	CHECK(Value(results, "memory.stream.bytes", { { "kind", "voxel" }, { "stream", "voxel_cubes" } }) == Value(results, "memory.voxel.cubes") * 32.0);

	double voxelStreamTotal = 0.0;
	for (const MetricRecord& record : results.records) {
		if (record.id == "memory.stream.bytes" && record.mesh == "*" && record.tags.at("kind") == "voxel") {
			voxelStreamTotal += record.value;
		}
	}
	CHECK(voxelStreamTotal == Value(results, "memory.voxel_pages.bytes"));
	CHECK(Value(results, "voxel.cell_opacity", { { "stat", "max" } }, "/voxels") <= 1.0);
	CHECK(Value(results, "voxel.width", { { "stat", "p50" } }, "/voxels") > 0.0);
	// Every cell of a voxelized sheet touches the sheet, so its centre is within half a cell diagonal.
	CHECK(Value(results, "voxel.truth.cell_distance_over_width", { { "stat", "max" } }, "/voxels") <= 0.87);
	CHECK(Value(results, "voxel.truth.cells_far_from_surface", {}, "/voxels") == 0.0);

	// Coarse-end triangles are not reported when the coarse end is voxels.
	for (const MetricRecord& record : results.records) {
		CHECK(record.id != "hierarchy.coarsest_triangles");
	}
}

// At cut t a group draws its segments when its boundary error is above t, except those whose child is too; along any
// path from a root (error infinite) to a leaf exactly one segment is then drawn as long as errors fall along every edge.
// Checks that for a branch view, and returns the triangle area drawn at a range of cuts.
std::vector<double> CheckBranchCoversOnce(const CachedMesh& view, const char* label, bool measureArea)
{
	std::vector<double> areas;
	const ClusterLODPrebuiltData& prebuilt = view.Prebuilt();
	uint64_t edges = 0, nonMonotonic = 0, badChildren = 0, roots = 0, finiteRoots = 0;
	std::vector<bool> hasParent(prebuilt.groups.size(), false);
	for (uint32_t index = 0; index < prebuilt.groups.size(); ++index) {
		const ClusterLODGroup& group = prebuilt.groups[index];
		for (uint32_t s = 0; s < group.segmentCount; ++s) {
			const int32_t child = prebuilt.segments[group.firstSegment + s].refinedGroup;
			if (child < 0) continue;
			if (static_cast<size_t>(child) >= prebuilt.groups.size()) { ++badChildren; continue; }
			hasParent[child] = true;
			++edges;
			if (!(group.bounds.error > prebuilt.groups[child].bounds.error)) ++nonMonotonic;
		}
	}
	for (uint32_t index = 0; index < prebuilt.groups.size(); ++index) {
		if (hasParent[index]) continue;
		++roots;
		finiteRoots += prebuilt.groups[index].bounds.error < 1.0e30f ? 1u : 0u;
	}
	std::fprintf(stderr, "%s: groups=%zu edges=%llu roots=%llu finite_roots=%llu non_monotonic=%llu\n", label, prebuilt.groups.size(),
		static_cast<unsigned long long>(edges), static_cast<unsigned long long>(roots), static_cast<unsigned long long>(finiteRoots),
		static_cast<unsigned long long>(nonMonotonic));
	CHECK(badChildren == 0u);
	CHECK(nonMonotonic == 0u);
	CHECK(roots > 0u && finiteRoots == 0u);
	if (!measureArea) return areas;
	for (const float cut : { 0.0f, 1.0e-4f, 1.0e-3f, 4.0e-3f, 1.6e-2f, 6.4e-2f, 0.25f, 1.0f, 1.0e6f }) {
		double area = 0.0;
		for (uint32_t index = 0; index < prebuilt.groups.size(); ++index) {
			const ClusterLODGroup& group = prebuilt.groups[index];
			if (!(group.bounds.error > cut)) continue;
			for (uint32_t s = 0; s < group.segmentCount; ++s) {
				const ClusterLODGroupSegment& segment = prebuilt.segments[group.firstSegment + s];
				if (segment.refinedGroup >= 0 && prebuilt.groups[segment.refinedGroup].bounds.error > cut) continue;
				CHECK(view.VisitSegmentMeshlets(index, segment, [&](const CLodCache::TrianglePage& page, const CLodCache::TriangleMeshlet& meshlet) {
					for (uint32_t t = 0; t < meshlet.triangleCount; ++t) {
						const auto tri = page.Triangle(meshlet, t);
						const DirectX::XMFLOAT3 a = page.Position(meshlet, tri[0]), b = page.Position(meshlet, tri[1]), c = page.Position(meshlet, tri[2]);
						const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
						const double x = uy * vz - uz * vy, y = uz * vx - ux * vz, z = ux * vy - uy * vx;
						area += 0.5 * std::sqrt(x * x + y * y + z * z);
					}
				}));
			}
		}
		areas.push_back(area);
	}
	return areas;
}

void TestVoxelFork(const fs::path& root)
{
	const SourceMesh source = MakeSheet(256u, 0.0f);
	const fs::path metadata = SaveBuild(root, source, ClusterLODCoveragePreservationMode::PrioritizeEdges, "/fork", ClusterLODVoxelForkMode::Forced);
	std::string error;
	const auto mesh = CachedMesh::Open(metadata, error);
	CHECK(mesh != nullptr);
	if (!mesh) return;
	CHECK(mesh->IsForked());
	{
		const auto triangles = CachedMesh::Open(SaveBuild(root, source, ClusterLODCoveragePreservationMode::PrioritizeEdges, "/fork-reference"), error);
		CHECK(triangles != nullptr);
		if (!triangles) return;
		CHECK(!triangles->IsForked());
		const auto view = mesh->BranchView(CLOD_BRANCH_TRIANGLE, "");
		CHECK(view->Prebuilt().groups.size() == triangles->Prebuilt().groups.size());
		CHECK(view->LeafGeometryHash() == triangles->LeafGeometryHash());
		// The triangle branch draws exactly what the triangle build draws, at every cut.
		const std::vector<double> built = CheckBranchCoversOnce(*triangles, "triangle build", true);
		const std::vector<double> branch = CheckBranchCoversOnce(*view, "triangle branch", true);
		CHECK(!built.empty() && built == branch);
	}
	const auto voxel = mesh->BranchView(CLOD_BRANCH_VOXEL, "");
	CheckBranchCoversOnce(*voxel, "voxel branch", false);
	uint64_t voxelGroups = 0;
	for (const ClusterLODGroup& group : voxel->Prebuilt().groups) voxelGroups += (group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u ? 1u : 0u;
	CHECK(voxelGroups > 0u);

	// Analysis measures each branch under its own name.
	const ResultSet results = Analyze(metadata, "fork");
	CHECK(Value(results, "hierarchy.voxel_groups", {}, "/fork#voxel") > 0.0);
	CHECK(Value(results, "hierarchy.leaf_triangles", {}, "/fork#triangle") == static_cast<double>(source.indices.size() / 3u));
	CHECK(Value(results, "hierarchy.leaf_triangles", {}, "/fork#voxel") == static_cast<double>(source.indices.size() / 3u));
}

// The same source partitioned differently must agree, and a different source must not.
void TestLeafGeometryHash(const fs::path& root)
{
	const SourceMesh source = MakeSheet(256u, 0.0f);
	const SourceMesh moved = MakeSheet(256u, 1.0f);
	const auto open = [](const fs::path& path) { std::string error; return CachedMesh::Open(path, error); };

	const auto triangles = open(SaveBuild(root, source, ClusterLODCoveragePreservationMode::PrioritizeEdges, "/hash-triangles"));
	const auto voxels = open(SaveBuild(root, source, ClusterLODCoveragePreservationMode::Voxel, "/hash-voxels"));
	const auto other = open(SaveBuild(root, moved, ClusterLODCoveragePreservationMode::PrioritizeEdges, "/hash-moved"));
	CHECK(triangles->LeafGeometryHash() == voxels->LeafGeometryHash());
	CHECK(triangles->LeafGeometryHash() != other->LeafGeometryHash());
}

ResultSet MakeResults(const std::string& build, double memory, double quality)
{
	ResultSet results;
	results.build = build;
	results.leafGeometry["a|/m"] = "same";
	const auto add = [&](const char* id, double value, Better better) {
		MetricRecord record;
		record.id = id;
		record.value = value;
		record.better = better;
		record.unit = Unit::Bytes;
		record.analyzer = "test";
		record.analyzerVersion = 1u;
		record.asset = "a";
		record.mesh = "*";
		results.records.push_back(std::move(record));
	};
	add("memory", memory, Better::Lower);
	add("quality", quality, Better::Higher);
	return results;
}

void TestCompare()
{
	const ResultSet base = MakeResults("base", 100.0, 10.0);

	// Identical results compare as the same.
	CHECK(ComparisonExitCode(Compare(base, base, 0.0, {})) == 0);

	// The direction a metric improves in decides the verdict, whichever way the number moved.
	{
		const Comparison comparison = Compare(base, MakeResults("b", 90.0, 8.0), 0.0, {});
		CHECK(ComparisonExitCode(comparison) == 1);
		CHECK(comparison.differences.size() == 2u);
		CHECK(comparison.differences[0].improved && !comparison.differences[0].regressed);
		CHECK(comparison.differences[1].regressed && !comparison.differences[1].improved);
	}

	// A change under the threshold is not a difference.
	CHECK(ComparisonExitCode(Compare(base, MakeResults("b", 100.5, 10.0), 0.01, {})) == 0);

	// Different leaf geometry makes every number meaningless.
	{
		ResultSet other = base;
		other.leafGeometry["a|/m"] = "different";
		CHECK(ComparisonExitCode(Compare(base, other, 0.0, {})) == 2);
	}

	// A changed analyzer's numbers are not comparable with its old ones.
	{
		ResultSet other = base;
		other.records[0].analyzerVersion = 2u;
		CHECK(ComparisonExitCode(Compare(base, other, 0.0, {})) == 2);
	}

	// Absolute and baseline-relative budgets, and a budget that matches nothing.
	{
		Budget absolute;
		absolute.metric = "memory";
		absolute.value = 95.0;
		const ResultSet over = MakeResults("b", 100.0, 10.0);
		CHECK(!CheckBudgets(over, { absolute }, nullptr).empty());
		CHECK(CheckBudgets(MakeResults("b", 90.0, 10.0), { absolute }, nullptr).empty());

		Budget relative;
		relative.metric = "memory";
		relative.baselineFactor = 1.05;
		CHECK(CheckBudgets(MakeResults("b", 104.0, 10.0), { relative }, &base).empty());
		CHECK(!CheckBudgets(MakeResults("b", 106.0, 10.0), { relative }, &base).empty());
		// With no baseline, a relative budget has nothing to say.
		CHECK(CheckBudgets(MakeResults("b", 106.0, 10.0), { relative }, nullptr).empty());

		Budget misspelt;
		misspelt.metric = "memry";
		misspelt.value = 1.0;
		CHECK(!CheckBudgets(base, { misspelt }, nullptr).empty());
	}
}

void TestResultsRoundTrip(const fs::path& root)
{
	ResultSet results = MakeResults("round-trip", 12.5, 3.0);
	results.records[0].tags["stream"] = "a,b";
	results.buildKey = "abc";
	std::string error;
	const fs::path path = root / "results" / "round.jsonl";
	CHECK(WriteResults(path, results, error));
	ResultSet loaded;
	CHECK(ReadResults(path, loaded, error));
	CHECK(loaded.build == "round-trip");
	CHECK(loaded.buildKey == "abc");
	CHECK(loaded.records.size() == 2u);
	CHECK(loaded.records[0].Key() == results.records[0].Key());
	CHECK(loaded.records[0].value == 12.5);
	CHECK(loaded.records[0].better == Better::Lower);
	CHECK(ComparisonExitCode(Compare(results, loaded, 0.0, {})) == 0);
	CHECK(WriteResultsCsv(root / "results" / "round.csv", results, error));
	ResultSet missing;
	CHECK(!ReadResults(root / "results" / "missing.jsonl", missing, error));
}

void TestBadInputsAreErrors(const fs::path& root)
{
	// An analyzer that does not exist, and a cache that does not exist, are failures rather than empty results.
	ResultSet results;
	std::string error;
	AnalyzeOptions unknown;
	unknown.analyzers = { "no.such.analyzer" };
	CHECK(!AnalyzeMeshes({}, "x", "x", unknown, results, error));
	error.clear();
	CHECK(!AnalyzeMeshes({ { root / "missing.clodmeta", "a" } }, "x", "x", {}, results, error));
	error.clear();
	CHECK(!AnalyzeMeshes({}, "x", "x", {}, results, error));
}
}

namespace {
// A wall loses rays to gaps in its cells and an aggregate has no walls to lose, which is the whole basis of the build's surface test.
void TestVoxelSurfaceTrace()
{
	using DirectX::XMFLOAT3;
	constexpr float kWidth = 0.1f;
	const XMFLOAT3 origin{ 0.0f, 0.0f, 0.0f };

	// A flat sheet across x and y, in the middle of cell layer z = 20.
	constexpr float kSheetZ = 2.05f;
	const std::vector<XMFLOAT3> sheet{
		{ 0.0f, 0.0f, kSheetZ }, { 4.0f, 0.0f, kSheetZ }, { 4.0f, 4.0f, kSheetZ },
		{ 0.0f, 0.0f, kSheetZ }, { 4.0f, 4.0f, kSheetZ }, { 0.0f, 4.0f, kSheetZ },
	};
	const std::span<const XMFLOAT3> sheetList(sheet);
	std::vector<std::array<int32_t, 3>> allCells, someCells;
	for (int32_t y = 0; y < 40; ++y) {
		for (int32_t x = 0; x < 40; ++x) {
			allCells.push_back({ x, y, 20 });
			if ((x + y) % 2 == 0) {
				someCells.push_back({ x, y, 20 });
			}
		}
	}
	const auto whole = CLodCache::TraceVoxelSurface({ &sheetList, 1u }, allCells, origin, kWidth);
	CHECK(whole.wallShare > 0.1f);
	CHECK(whole.SurfaceLoss() < 0.001f);
	const auto gaps = CLodCache::TraceVoxelSurface({ &sheetList, 1u }, someCells, origin, kWidth);
	CHECK(std::abs(gaps.wallShare - whole.wallShare) < 0.001f);
	CHECK(gaps.wallDeficit > 0.2f);
	CHECK(gaps.SurfaceLoss() > whole.SurfaceLoss() + 0.02f);
	const auto none = CLodCache::TraceVoxelSurface({ &sheetList, 1u }, {}, origin, kWidth);
	CHECK(std::abs(none.wallDeficit - 1.0f) < 0.001f);
	CHECK(std::abs(none.SurfaceLoss() - none.wallShare) < 0.001f);

	// Slivers much narrower than a block, at every angle: plenty of rays stopped, and no walls, whatever the cells do.
	std::vector<XMFLOAT3> needles;
	uint32_t state = 12345u;
	const auto next = [&]() {
		state = state * 1664525u + 1013904223u;
		return static_cast<float>(state >> 8u) / static_cast<float>(1u << 24u);
	};
	for (int needle = 0; needle < 4000; ++needle) {
		const XMFLOAT3 base{ next() * 4.0f, next() * 4.0f, next() * 4.0f };
		const XMFLOAT3 along{ (next() - 0.5f) * 0.4f, (next() - 0.5f) * 0.4f, (next() - 0.5f) * 0.4f };
		needles.push_back(base);
		needles.push_back({ base.x + along.x, base.y + along.y, base.z + along.z });
		needles.push_back({ base.x + 0.01f, base.y, base.z + 0.01f });
	}
	const std::span<const XMFLOAT3> needleList(needles);
	const auto aggregate = CLodCache::TraceVoxelSurface({ &needleList, 1u }, {}, origin, kWidth);
	CHECK(aggregate.meanSource > 0.01f);
	CHECK(aggregate.SurfaceLoss() < 0.001f);

	// Coarse enough that a block is wider than the sheet, it is no longer a wall.
	const auto coarse = CLodCache::TraceVoxelSurface({ &sheetList, 1u }, {}, origin, 2.0f);
	CHECK(coarse.wallShare < whole.wallShare);

	// Splitting the triangles across lists changes nothing.
	const std::array<std::span<const XMFLOAT3>, 2> split{ sheetList.subspan(0u, 3u), sheetList.subspan(3u) };
	const auto splitResult = CLodCache::TraceVoxelSurface(split, someCells, origin, kWidth);
	CHECK(splitResult.SurfaceLoss() == gaps.SurfaceLoss());
}

void TestImageKernels()
{
	using namespace clodeval::image;
	const auto approx = [](double a, double b, double tolerance = 1.0e-6) { return std::abs(a - b) <= tolerance; };
	const auto check = [](bool passed, const char* what) {
		if (!passed) {
			std::fprintf(stderr, "FAILED: %s\n", what);
			++g_failures;
		}
	};

	check(approx(HalfToFloat(0x3C00), 1.0) && approx(HalfToFloat(0xC000), -2.0) && approx(HalfToFloat(0x3800), 0.5) && HalfToFloat(0) == 0.0f, "half decoding");

	Plane a{ 2, 2, 3, std::vector<float>(12, 0.5f) };
	Plane b = a;
	const std::vector<uint8_t> all(4, 1u);
	check(Psnr(a, b, all) == 100.0, "identical images report 100 dB");
	for (float& value : b.data) {
		value = 0.6f;
	}
	// MSE is 0.01 everywhere, so PSNR is exactly 20 dB.
	check(approx(Psnr(a, b, all), 20.0, 1.0e-4), "PSNR of a constant 0.1 error");
	const std::vector<uint8_t> none(4, 0u);
	check(MeanSquaredError(a, b, none) == 0.0, "an empty mask measures nothing");

	Plane big{ 4, 4, 1, std::vector<float>(16, 0.0f) };
	big.At(0, 0) = 1.0f;
	big.At(1, 1) = 1.0f;
	const Plane reduced = BoxDownsample(big, 2);
	check(reduced.width == 2u && reduced.height == 2u && approx(reduced.At(0, 0), 0.5) && approx(reduced.At(1, 1), 0.0), "box downsample");

	Plane reference{ 2, 1, 1, { 1.0f, 0.0f } };
	Plane test{ 2, 1, 1, { 0.5f, 0.5f } };
	const CoverageComparison coverage = CompareCoverage(reference, test);
	check(approx(coverage.ratio, 1.0) && approx(coverage.softIou, 0.5 / 1.5) && approx(coverage.excessFraction, 0.5) && approx(coverage.missingFraction, 0.5), "coverage comparison");

	Plane ramp{ 16, 16, 1, std::vector<float>(256) };
	for (uint32_t index = 0; index < 256u; ++index) {
		ramp.data[index] = static_cast<float>(index) / 255.0f;
	}
	const std::vector<uint8_t> rampMask(256, 1u);
	check(approx(Ssim(ramp, ramp, rampMask), 1.0, 1.0e-9), "SSIM of an image with itself");
	Plane inverted = ramp;
	for (float& value : inverted.data) {
		value = 1.0f - value;
	}
	check(Ssim(ramp, inverted, rampMask) < 0.0, "SSIM of an inverted image is negative");

	// One key spanning 3 x 2 pixels, another in one pixel, and an empty pixel that must not count.
	KeyImage keys{ 4, 2, { 7u, 7u, 7u, kEmptyKey, 7u, 9u, 0u, 0u } };
	const std::vector<uint8_t> keyMask{ 1, 1, 1, 1, 1, 1, 0, 0 };
	std::vector<double> extents = KeyExtents(keys, keyMask);
	std::sort(extents.begin(), extents.end());
	check(extents == std::vector<double>{ 1.0, 3.0 }, "key extents use the larger side and skip empty pixels");

	const fs::path directory = fs::temp_directory_path() / "clod_eval_image_test";
	fs::create_directories(directory);
	std::string error;
	Plane written{ 2, 1, 4, { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f } };
	Plane read;
	check(WritePlane(directory / "plane.dds", written, error) && ReadPlane(directory / "plane.dds", read, error) && read.data == written.data, "DDS round trip");
	// A visibility layer as CLodCapture writes it: DXGI_FORMAT_R32G32_UINT is 17.
	{
		std::vector<char> dds(148 + 8, 0);
		std::memcpy(dds.data(), "DDS ", 4);
		const uint32_t one = 1u, dx10 = 0x30315844u, format = 17u;
		std::memcpy(dds.data() + 12, &one, 4);
		std::memcpy(dds.data() + 16, &one, 4);
		std::memcpy(dds.data() + 84, &dx10, 4);
		std::memcpy(dds.data() + 128, &format, 4);
		const uint64_t key = 0x123456789ABCDEF0ull;
		std::memcpy(dds.data() + 148, &key, 8);
		std::ofstream(directory / "keys.dds", std::ios::binary).write(dds.data(), static_cast<std::streamsize>(dds.size()));
		KeyImage readKeys;
		check(ReadKeys(directory / "keys.dds", readKeys, error) && readKeys.keys == std::vector<uint64_t>{ key }, "a visibility layer reads back");
	}
	const std::vector<uint8_t> pixels(2 * 2 * 3, 128u);
	check(WritePng(directory / "image.png", 2, 2, pixels, error) && fs::file_size(directory / "image.png") > 8u, "PNG writes");
	fs::remove_all(directory);

	// A 90 degree view is twice as tall as it is far away.
	check(approx(clodeval::TelephotoDistance(10.0, 90.0), 5.0), "telephoto distance");
	const clodeval::TransitionPick pick = clodeval::PickTransition({ { 0.25, 0.0 }, { 0.5, 0.1 }, { 1.0, 0.6 }, { 2.0, 0.99 } });
	check(pick.hasLow && pick.low == 0.5 && pick.hasMid && pick.mid == 1.0 && pick.hasHigh && pick.high == 2.0, "transition thresholds");
	check(!clodeval::PickTransition({ { 1.0, 0.1 } }).hasMid, "a hierarchy that never turns to voxels has no transition");
	const std::vector<double> fine = clodeval::FineThresholds(8u);
	check(fine.size() == 9u && approx(fine.front(), 0.25) && approx(fine.back(), 2.5) && std::ranges::find(fine, 1.0) != fine.end(), "fine thresholds surround one pixel");
	// A plateau at 0.94 with a later rise must not push "after" onto the later rise.
	const auto chosen = clodeval::PickBeforeAfter({ { 0.25, 0.0 }, { 0.5, 0.01 }, { 1.0, 0.5 }, { 1.5, 0.94 }, { 2.5, 0.95 } });
	check(chosen.first == 0.5 && chosen.second == 1.5, "before and after surround the first switch");

	// A view at 2 radii and a corner view, both placed from a sphere at (1, 2, 3).
	clodeval::SuiteCapture capture;
	capture.distances = { 2.0 };
	capture.cornerDistances = { 4.0 };
	capture.cornerNdc = 0.5;
	capture.fovDegrees = 90.0;  // tangent 1
	capture.width = 200u;
	capture.height = 100u;
	const clodeval::AssetFrame frame{ { 1.0, 2.0, 3.0 }, 10.0 };
	const auto rig = clodeval::MakeRig(capture, frame);
	check(rig.size() == 2u && rig[0].name == "d2" && approx(rig[0].position[2], 23.0) && approx(rig[0].position[1], 5.0), "rig distance");
	// Sideways by ndc * tangent * aspect * distance = 0.5 * 1 * 2 * 40 = 40, and up by 0.5 * 1 * 40 = 20.
	check(rig[1].name == "corner4" && approx(rig[1].position[0], 1.0 - 40.0) && approx(rig[1].lookAt[1], 2.0 - 20.0), "rig corner offset");
}
}

int main()
{
	TestImageKernels();
	TestVoxelSurfaceTrace();
	wchar_t tempDirectory[MAX_PATH] = {};
	GetTempPathW(MAX_PATH, tempDirectory);
	const fs::path root = fs::path(tempDirectory) / ("clod_eval_tests_" + std::to_string(GetCurrentProcessId()));
	fs::remove_all(root);
	fs::create_directories(root);
	SetEnvironmentVariableW(L"SARP_CACHE_ROOT", root.c_str());

	auto& scheduler = br::TaskSchedulerManager::GetInstance();
	scheduler.Initialize();
	int result = 0;
	try {
		TestCompare();
		TestResultsRoundTrip(root);
		TestBadInputsAreErrors(root);
		TestTriangleHierarchy(root);
		TestVoxelHierarchy(root);
		TestVoxelFork(root);
		TestLeafGeometryHash(root);
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
