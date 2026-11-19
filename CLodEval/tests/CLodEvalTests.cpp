// Checks the numbers CLodEval reports against a hierarchy it builds here, and its comparison rules
// against hand-made results. No GPU, no source assets.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include <windows.h>

#include <ktx.h>

#include <BasicRenderer/Assets/CLodCacheStore.h>
#include <BasicRenderer/Assets/DefaultCLodSettings.h>
#include <BasicRenderer/Assets/Geometry/VertexLayout.h>
#include <BasicRenderer/Assets/Import/ClusterLODUtilities.h>
#include <BasicRenderer/Streaming/TaskScheduler.h>

#include <BasicRenderer/Assets/VoxelSurfaceTrace.h>
#include <BasicRenderer/Assets/ClusterLODTextureColor.h>
#include <BasicRenderer/Assets/CLodCacheIdentity.h>
#include <BasicRenderer/Assets/ClusterLODUvRemap.h>
#include <BasicRenderer/Assets/ClusterLODNormalSpread.h>
#include <BasicRenderer/Assets/CLodPageDecoder.h>
#include <BasicRenderer/Assets/Import/CLodCache.h>

#include "Analyzers/Analyzers.h"
#include "Capture.h"
#include "Core/Appearance.h"
#include "Core/Image.h"
#include "Transition.h"
#include "Core/Results.h"
#include "Core/SourceTextures.h"
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

SourceMesh MakeSheet(uint32_t resolution, float zOffset, float amplitude = 1.0f)
{
	SourceMesh mesh;
	const auto height = [amplitude](float x, float y) { return amplitude * (0.08f * std::sin(x * 37.0f) * std::cos(y * 41.0f) + 0.03f * std::sin(x * 131.0f + y * 113.0f)); };
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

// The builder defaults are the recommended build (every stage on that it uses). These tests measure each stage against a build
// without it, so they start from the defaults with those stages off (the defaults before they changed).
ClusterLODBuilderSettings StagesOffSettings()
{
	ClusterLODBuilderSettings settings = GetDefaultBuilderSettings();
	settings.disableSloppyFallback = false;
	settings.simplifyUpdate = false;
	settings.simplifyUpdateUvTransfer = false;
	settings.lodErrorChannels = false;
	settings.lodErrorChannelsTexture = 0.0f;
	settings.simplifyTopology = false;
	settings.simplifyTopologyRelaxUv = false;
	settings.simplifyTopologyTextureCharts = false;
	settings.simplifyTopologyTextureColor = 0.0f;
	return settings;
}

fs::path SaveBuild(
	const fs::path& root,
	const SourceMesh& source,
	ClusterLODCoveragePreservationMode mode,
	const std::string& primPath,
	ClusterLODVoxelForkMode fork = ClusterLODVoxelForkMode::Off)
{
	ClusterLODBuilderSettings settings = StagesOffSettings();
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

// quality.normals against cases with known answers, on a flat plane, which no correct simplification can fold: normals
// that agree with the surface (nothing inverted or flipped), and the same plane with every normal negated (every
// triangle inverted against its own face, yet nothing flipped against the source, which is negated too).
void TestNormalQuality(const fs::path& root)
{
	const SourceMesh source = MakeSheet(128u, 0.0f, 0.0f);
	const ResultSet agreeing = Analyze(SaveBuild(root, source, ClusterLODCoveragePreservationMode::PrioritizeEdges, "/normals_agree"), "normals_agree");
	CHECK(Value(agreeing, "normals.leaf_triangles") == static_cast<double>(source.indices.size() / 3u));
	CHECK(Value(agreeing, "normals.leaf_inverted_area_fraction") == 0.0);
	CHECK(Value(agreeing, "normals.simplified_triangles") > 0.0);
	CHECK(Value(agreeing, "normals.inverted_area_fraction") == 0.0);
	CHECK(Value(agreeing, "normals.flipped_vs_source_area_fraction") == 0.0);
	CHECK(Value(agreeing, "normals.mean_deviation_vs_source") < 1.0e-6);
	// Every simplified triangle is attributed to the path that produced it.
	double attributed = 0.0;
	for (const MetricRecord& record : agreeing.records) {
		if (record.id == "normals.simplified_triangles" && record.mesh == "*" && record.tags.contains("method")) {
			CHECK(record.tags.at("method") != "unknown");
			attributed += record.value;
		}
	}
	CHECK(attributed == Value(agreeing, "normals.simplified_triangles"));

	SourceMesh negated = source;
	for (size_t offset = 0; offset < negated.vertices.size(); offset += MeshVertexLayout::BaseVertexSize) {
		DirectX::XMFLOAT3 normal;
		std::memcpy(&normal, negated.vertices.data() + offset + MeshVertexLayout::NormalOffset, sizeof(normal));
		normal = { -normal.x, -normal.y, -normal.z };
		std::memcpy(negated.vertices.data() + offset + MeshVertexLayout::NormalOffset, &normal, sizeof(normal));
	}
	const ResultSet inverted = Analyze(SaveBuild(root, negated, ClusterLODCoveragePreservationMode::PrioritizeEdges, "/normals_negated"), "normals_negated");
	CHECK(Value(inverted, "normals.leaf_inverted_area_fraction") == 1.0);
	CHECK(Value(inverted, "normals.inverted_area_fraction") == 1.0);
	CHECK(Value(inverted, "normals.flipped_vs_source_area_fraction") == 0.0);
}

// The unit square in z = height as a grid of triangles, every corner normal `normal`, wound to face +z (or -z if flip).
std::vector<ShadedTriangle> MakePlane(uint32_t resolution, double height, const Vec& normal, bool flip = false, double width = 1.0)
{
	std::vector<ShadedTriangle> triangles;
	const double step = width / resolution;
	for (uint32_t row = 0; row < resolution; ++row) {
		for (uint32_t column = 0; column < resolution; ++column) {
			const Vec a{ column * step, row * step, height }, b{ (column + 1) * step, row * step, height };
			const Vec c{ (column + 1) * step, (row + 1) * step, height }, d{ column * step, (row + 1) * step, height };
			for (const std::array<Vec, 3>& corners : { std::array<Vec, 3>{ a, b, c }, std::array<Vec, 3>{ a, c, d } }) {
				ShadedTriangle triangle;
				triangle.position = flip ? std::array<Vec, 3>{ corners[0], corners[2], corners[1] } : corners;
				triangle.normal = { normal, normal, normal };
				triangles.push_back(triangle);
			}
		}
	}
	return triangles;
}

bool Near(double value, double expected, double tolerance) { return std::abs(value - expected) <= tolerance; }

// The pointwise measurement against surfaces whose answers are known exactly.
void TestAppearanceMeasure()
{
	const Vec up{ 0.0, 0.0, 1.0 };
	const std::vector<ShadedTriangle> source = MakePlane(16u, 0.0, up);

	// The same plane in two triangles with correct normals: nothing wrong, everything covered.
	{
		const SurfaceComparison same = CompareSurfaces(source, MakePlane(1u, 0.0, up), 1.0e-4);
		CHECK(Near(same.coarseToSource.area, 1.0, 1.0e-6));
		CHECK(same.coarseToSource.normalQ == 0.0);
		CHECK(same.coarseToSource.maxAngle == 0.0);
		CHECK(same.coarseToSource.beyondArea == 0.0 && same.coarseToSource.ambiguousArea == 0.0 && same.coarseToSource.unmatchedArea == 0.0);
		CHECK(same.sourceToCoarse.beyondArea == 0.0 && same.sourceToCoarse.normalQ == 0.0);
	}

	// Every coarse normal rotated by theta about x, on exactly the right plane: Q_N / area = (2 sin(theta / 2))^2,
	// with no geometric error and nothing ambiguous. The source side sees the same residual.
	{
		const double theta = 40.0 * 3.14159265358979323846 / 180.0;
		const Vec rotated{ 0.0, -std::sin(theta), std::cos(theta) };
		const SurfaceComparison tilted = CompareSurfaces(source, MakePlane(1u, 0.0, rotated), 1.0e-4);
		const double expected = std::pow(2.0 * std::sin(theta / 2.0), 2.0);
		CHECK(Near(tilted.coarseToSource.normalQ / tilted.coarseToSource.measuredArea, expected, 1.0e-6));
		CHECK(Near(tilted.coarseToSource.normalQ, expected, 1.0e-6));  // over unit area
		CHECK(Near(tilted.coarseToSource.maxAngle, theta, 1.0e-5));
		CHECK(Near(tilted.coarseToSource.angle.Quantile(0.5) / theta, 1.0, 0.1));  // within one 8-per-octave bin
		CHECK(Near(tilted.coarseToSource.chord.Quantile(0.95) / (2.0 * std::sin(theta / 2.0)), 1.0, 0.1));
		CHECK(tilted.coarseToSource.angleOverArea[0] == tilted.coarseToSource.area);  // all over 10 degrees
		CHECK(tilted.coarseToSource.angleOverArea[1] == tilted.coarseToSource.area);  // and over 30
		CHECK(tilted.coarseToSource.angleOverArea[2] == 0.0);                         // none over 90
		CHECK(tilted.coarseToSource.ambiguousArea == 0.0 && tilted.coarseToSource.beyondArea == 0.0);
		CHECK(Near(tilted.sourceToCoarse.normalQ / tilted.sourceToCoarse.measuredArea, expected, 1.0e-6));
	}

	// Half the plane covered: half the source area is missing, by area exactly. Nothing drawn is spurious.
	{
		const SurfaceComparison half = CompareSurfaces(source, MakePlane(1u, 0.0, up, false, 0.5), 1.0e-4);
		CHECK(Near(half.sourceToCoarse.beyondArea / half.sourceToCoarse.area, 0.75, 1.0e-9));  // a 0.5 x 0.5 square covers a quarter
		CHECK(half.coarseToSource.beyondArea == 0.0);
	}

	// Lifted by h: spurious and missing everywhere when h is over the allowed error, nowhere when under it. The
	// sampled distance is h.
	{
		const double h = 0.01;
		const std::vector<ShadedTriangle> lifted = MakePlane(1u, h, up);
		const SurfaceComparison tight = CompareSurfaces(source, lifted, 1.0e-3);
		CHECK(Near(tight.coarseToSource.beyondArea / tight.coarseToSource.area, 1.0, 1.0e-9));
		CHECK(Near(tight.sourceToCoarse.beyondArea / tight.sourceToCoarse.area, 1.0, 1.0e-9));
		CHECK(Near(tight.coarseToSource.maxDistance, h, 1.0e-6));
		CHECK(Near(tight.coarseToSource.maxDistanceOverTolerance, 10.0, 1.0e-3));
		CHECK(tight.coarseToSource.normalQ == 0.0);
		const SurfaceComparison loose = CompareSurfaces(source, lifted, 0.02);
		CHECK(loose.coarseToSource.beyondArea == 0.0 && loose.sourceToCoarse.beyondArea == 0.0);
	}

	// Wound backwards with the right normals: the nearest source faces away and none faces its way, so every point is
	// ambiguous and measured against the nearest, which shades the same.
	{
		const SurfaceComparison folded = CompareSurfaces(source, MakePlane(1u, 0.0, up, true), 1.0e-4);
		CHECK(Near(folded.coarseToSource.ambiguousArea / folded.coarseToSource.area, 1.0, 1.0e-9));
		CHECK(folded.coarseToSource.normalQ == 0.0);
	}

	// A two-sided sheet: front facing +z with +z normals, back on the same plane facing -z with -z normals. Each coarse
	// side matches its own sheet, so neither is wrong or ambiguous; the front measured against the back sheet would
	// read 180 degrees.
	{
		std::vector<ShadedTriangle> sheet = source;
		const std::vector<ShadedTriangle> back = MakePlane(16u, 0.0, Vec{ 0.0, 0.0, -1.0 }, true);
		sheet.insert(sheet.end(), back.begin(), back.end());
		for (const bool backSide : { false, true }) {
			const SurfaceComparison side = CompareSurfaces(sheet, MakePlane(1u, 0.0, backSide ? Vec{ 0.0, 0.0, -1.0 } : up, backSide), 1.0e-4);
			CHECK(side.coarseToSource.normalQ == 0.0);
			CHECK(side.coarseToSource.ambiguousArea == 0.0);
		}
		// Front geometry shading with back normals is 180 degrees wrong everywhere: Q_N / area = 4.
		const SurfaceComparison wrong = CompareSurfaces(sheet, MakePlane(1u, 0.0, Vec{ 0.0, 0.0, -1.0 }), 1.0e-4);
		CHECK(Near(wrong.coarseToSource.normalQ / wrong.coarseToSource.measuredArea, 4.0, 1.0e-6));
		CHECK(Near(wrong.coarseToSource.angleOverArea[2] / wrong.coarseToSource.measuredArea, 1.0, 1.0e-9));
	}

	// Coarse normals that cancel count as 180 degrees and are counted; source normals that cancel leave nothing to
	// compare with and are counted, not measured.
	{
		std::vector<ShadedTriangle> cancelling = MakePlane(1u, 0.0, up);
		for (ShadedTriangle& triangle : cancelling) {
			triangle.normal = { Vec{ 1.0, 0.0, 0.0 }, Vec{ -1.0, 0.0, 0.0 }, Vec{ 0.0, 0.0, 0.0 } };
		}
		// The normals cancel exactly at the centroid and at the point towards the zero corner, (1/6)(1,0,0) +
		// (1/6)(-1,0,0) + (2/3)0, and not at the other two: half the area, two points of each of two triangles.
		const SurfaceComparison zero = CompareSurfaces(source, cancelling, 1.0e-4);
		CHECK(Near(zero.coarseToSource.coarseZeroNormalArea / zero.coarseToSource.area, 0.5, 1.0e-9));
		CHECK(zero.coarseToSource.coarseZeroNormalSamples == 4u);
		CHECK(Near(zero.coarseToSource.normalQ / zero.coarseToSource.measuredArea, 0.5 * 4.0 + 0.5 * 2.0, 1.0e-6));  // 180 and 90 degrees
		const SurfaceComparison zeroSource = CompareSurfaces(cancelling, MakePlane(1u, 0.0, up), 1.0e-4);
		CHECK(Near(zeroSource.coarseToSource.sourceZeroNormalArea / zeroSource.coarseToSource.area, 0.5, 1.0e-9));
		CHECK(zeroSource.coarseToSource.sourceZeroNormalSamples > 0u);
	}

	// The rollup's quantiles are the merged histogram's, not a mean of the meshes'.
	{
		LogHistogram a(1.0e-4, 4.0, 8u), b(1.0e-4, 4.0, 8u);
		a.Add(0.01, 1.0);
		b.Add(1.0, 3.0);
		MetricSink sink;
		sink.SetScope({ "test", 1, "build", "asset", "a" });
		sink.EmitQuantiles("q", a, Unit::Radians, Better::Lower);
		sink.SetScope({ "test", 1, "build", "asset", "b" });
		sink.EmitQuantiles("q", b, Unit::Radians, Better::Lower);
		ResultSet results;
		results.records = sink.Finish();
		CHECK(Near(Value(results, "q", { { "stat", "p50" } }), 1.0, 0.1));
		CHECK(Near(Value(results, "q", { { "stat", "p50" } }, "a"), 0.01, 0.001));
	}
}

// quality.appearance's cut sweep on a real hierarchy: under the smallest error the cut draws exactly the leaves, with
// no residual and nothing missing; over every error it draws exactly the coarsest triangles; in between it never draws
// more as the cut grows.
void TestAppearanceCuts(const fs::path& root)
{
	const SourceMesh source = MakeSheet(128u, 0.0f, 1.0f);
	const ResultSet results = Analyze(SaveBuild(root, source, ClusterLODCoveragePreservationMode::PrioritizeEdges, "/appearance"), "appearance");
	const auto cutTags = [](int exponent) { return Tags{ { "cut", std::format("{:g}", std::ldexp(1.0, exponent)) } }; };
	const double leaves = Value(results, "hierarchy.leaf_triangles");
	CHECK(Value(results, "appearance.cut.triangles", cutTags(-14)) == leaves);
	CHECK(Value(results, "appearance.cut.simplified_triangles", cutTags(-14)) == 0.0);
	CHECK(Value(results, "appearance.cut.normal_q_per_area", cutTags(-14)) == 0.0);
	CHECK(Value(results, "appearance.cut.missing_area_fraction", cutTags(-14)) == 0.0);
	CHECK(Value(results, "appearance.cut.spurious_area_fraction", cutTags(-14)) == 0.0);
	CHECK(Value(results, "appearance.cut.triangles", cutTags(8)) == Value(results, "hierarchy.coarsest_triangles"));
	double previous = leaves;
	bool simplifiedSomewhere = false;
	for (int exponent = -14; exponent <= 8; ++exponent) {
		const double triangles = Value(results, "appearance.cut.triangles", cutTags(exponent));
		CHECK(triangles <= previous);
		previous = triangles;
		simplifiedSomewhere |= Value(results, "appearance.cut.simplified_triangles", cutTags(exponent)) > 0.0;
	}
	CHECK(simplifiedSomewhere);
	// Stored geometry covers every simplified triangle, which quality.normals counts independently.
	CHECK(Value(results, "appearance.simplified_triangles") == Value(results, "normals.simplified_triangles"));
	CHECK(Value(results, "appearance.unmatched_samples") == 0.0);
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

// A source in any vertex layout, built with any builder settings.
struct LayoutSource {
	std::vector<std::byte> vertices;
	std::vector<uint32_t> indices;
	uint32_t flags = VertexFlags::VERTEX_NORMALS;
	uint32_t stride = MeshVertexLayout::BaseVertexSize;
	std::vector<MeshUvSetData> uvSets; // as importers give them (glTF: TEXCOORD_n); the pages then store these, not the record texcoord

	void Add(const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT3& normal, const DirectX::XMFLOAT2& uv = {})
	{
		const size_t offset = vertices.size();
		vertices.resize(offset + stride);
		std::memcpy(vertices.data() + offset, &position, sizeof(position));
		std::memcpy(vertices.data() + offset + MeshVertexLayout::NormalOffset, &normal, sizeof(normal));
		if (MeshVertexLayout::HasTexcoords(flags)) {
			std::memcpy(vertices.data() + offset + MeshVertexLayout::TexcoordOffset(flags), &uv, sizeof(uv));
		}
	}
};

std::unique_ptr<CachedMesh> SaveAndOpen(const fs::path& root, const LayoutSource& source, const ClusterLODBuilderSettings& settings, const std::string& primPath)
{
	const ClusterLODPrebuildArtifacts artifacts = BuildClusterLODArtifactsFromGeometry(
		source.vertices, source.stride, nullptr, 0u, source.indices, source.uvSets, source.flags, settings);
	CHECK(CLodCacheLoader::SavePrebuilt({ "clod_eval_test.source", primPath, "", false }, artifacts.prebuiltData, artifacts.cacheBuildData.AsPayload()));
	for (const fs::path& path : FindCacheMetadata(root)) {
		std::string error;
		auto mesh = CachedMesh::Open(path, error);
		if (mesh && mesh->Prebuilt().cacheSource.primPath == primPath) {
			return mesh;
		}
	}
	CHECK(false);
	return nullptr;
}

ClusterLODBuilderSettings ReplacementSettings(bool update, bool validate)
{
	ClusterLODBuilderSettings settings = StagesOffSettings();
	settings.coveragePreservationMode = ClusterLODCoveragePreservationMode::PrioritizeEdges;
	settings.disableSloppyFallback = true;
	// These tests measure what each stage does to groups simplification leaves stuck, so stuck groups stay terminal here (the
	// last-resort sloppy fallback, on by default, would simplify them all; CLodCacheFormatTests covers it).
	settings.sloppyLastResort = false;
	settings.simplifyUpdate = update;
	settings.simplifyValidation = validate;
	return settings;
}

// Every simplified triangle (in a segment that refines into a finer group) as decoded from the pages.
struct DecodedCorner {
	DirectX::XMFLOAT3 position;
	DirectX::XMFLOAT3 normal;
	DirectX::XMFLOAT2 uv;
};
template <typename Visit>
void ForEachSimplifiedTriangle(const CachedMesh& mesh, Visit&& visit)
{
	const ClusterLODPrebuiltData& data = mesh.Prebuilt();
	for (uint32_t groupIndex = 0; groupIndex < data.groups.size(); ++groupIndex) {
		const ClusterLODGroup& group = data.groups[groupIndex];
		for (uint32_t segmentIndex = 0; segmentIndex < group.segmentCount; ++segmentIndex) {
			const ClusterLODGroupSegment& segment = data.segments[group.firstSegment + segmentIndex];
			if (segment.refinedGroup < 0 || segment.meshletCount == 0u) {
				continue;
			}
			uint32_t meshPage = 0;
			CLodCache::TrianglePage page;
			if (!CLodCache::ResolveSegmentMeshPage(data, groupIndex, segment, meshPage) || !CLodCache::TrianglePage::Parse(mesh.Page(meshPage), page)) {
				CHECK(false);
				continue;
			}
			for (uint32_t index = 0; index < segment.meshletCount; ++index) {
				const uint32_t meshletIndex = segment.firstMeshletInPage + index;
				const CLodCache::TriangleMeshlet meshlet = page.Meshlet(meshletIndex);
				for (uint32_t triangle = 0; triangle < meshlet.triangleCount; ++triangle) {
					const auto corners = page.Triangle(meshlet, triangle);
					std::array<DecodedCorner, 3> decoded;
					for (int corner = 0; corner < 3; ++corner) {
						decoded[corner].position = page.Position(meshlet, corners[corner]);
						decoded[corner].normal = page.Normal(meshlet, corners[corner]);
						decoded[corner].uv = page.UvSetCount() > 0u ? page.Uv(meshletIndex, meshlet, 0u, corners[corner]) : DirectX::XMFLOAT2{};
					}
					visit(decoded);
				}
			}
		}
	}
}

uint32_t CountGroupsWithFlag(const CachedMesh& mesh, uint32_t flag)
{
	uint32_t count = 0;
	for (const ClusterLODGroup& group : mesh.Prebuilt().groups) {
		count += (group.flags & flag) != 0u ? 1u : 0u;
	}
	return count;
}

// Whether two builds stored the same thing: group records and every page's bytes.
bool SameBuild(const CachedMesh& a, const CachedMesh& b)
{
	const auto& groupsA = a.Prebuilt().groups;
	const auto& groupsB = b.Prebuilt().groups;
	if (groupsA.size() != groupsB.size() || a.PageCount() != b.PageCount()) {
		return false;
	}
	for (size_t index = 0; index < groupsA.size(); ++index) {
		if (std::memcmp(&groupsA[index], &groupsB[index], sizeof(ClusterLODGroup)) != 0) {
			return false;
		}
	}
	for (uint32_t page = 0; page < a.PageCount(); ++page) {
		const auto pageA = a.Page(page);
		const auto pageB = b.Page(page);
		if (pageA.size() != pageB.size() || std::memcmp(pageA.data(), pageB.data(), pageA.size()) != 0) {
			return false;
		}
	}
	return true;
}

// Replacement vertices (stage 2) leave the leaves alone, change nothing when off, and build the same way every time.
void TestReplacementVerticesOffAndDeterminism(const fs::path& root)
{
	const SourceMesh sheet = MakeSheet(256u, 0.0f);
	LayoutSource source;
	source.vertices = sheet.vertices;
	source.indices = sheet.indices;

	const auto off = SaveAndOpen(root, source, ReplacementSettings(false, false), "/replace-off");
	const auto offAgain = SaveAndOpen(root, source, ReplacementSettings(false, false), "/replace-off-again");
	const auto on = SaveAndOpen(root, source, ReplacementSettings(true, false), "/replace-on");
	const auto onAgain = SaveAndOpen(root, source, ReplacementSettings(true, false), "/replace-on-again");
	CHECK(off && offAgain && on && onAgain);
	if (!off || !offAgain || !on || !onAgain) {
		return;
	}
	CHECK(on->LeafGeometryHash() == off->LeafGeometryHash());
	CHECK(CountGroupsWithFlag(*off, CLOD_GROUP_FLAG_SIMPLIFY_REPLACED) == 0u);
	CHECK(CountGroupsWithFlag(*on, CLOD_GROUP_FLAG_SIMPLIFY_REPLACED) > 0u);
	CHECK(SameBuild(*off, *offAgain));
	// Follow-up: the UV transfer acts only on update candidates.
	ClusterLODBuilderSettings offTransfer = ReplacementSettings(false, false);
	offTransfer.simplifyUpdateUvTransfer = true;
	const auto offTransferred = SaveAndOpen(root, source, offTransfer, "/replace-off-transfer");
	CHECK(offTransferred && SameBuild(*off, *offTransferred));
	// The builder runs each level's groups in parallel; replacement vertices are committed in group order whatever the threads.
	CHECK(SameBuild(*on, *onAgain));
	CHECK(!SameBuild(*on, *off));
}

// Validation outcomes (group flag bits 9-10): every group that was simplified (some coarser segment refines into it) has an
// accepted outcome when validation is on, and none has any outcome when it is off.
void TestValidationFlags(const fs::path& root)
{
	const SourceMesh sheet = MakeSheet(256u, 0.0f);
	LayoutSource source;
	source.vertices = sheet.vertices;
	source.indices = sheet.indices;
	for (const bool validate : { false, true }) {
		const auto mesh = SaveAndOpen(root, source, ReplacementSettings(false, validate), validate ? "/validation-on" : "/validation-off");
		if (!mesh) {
			continue;
		}
		const ClusterLODPrebuiltData& data = mesh->Prebuilt();
		std::vector<bool> simplified(data.groups.size(), false);
		for (const ClusterLODGroupSegment& segment : data.segments) {
			if (segment.refinedGroup >= 0 && static_cast<size_t>(segment.refinedGroup) < simplified.size()) {
				simplified[segment.refinedGroup] = true;
			}
		}
		uint32_t simplifiedCount = 0, flagged = 0, acceptedSimplified = 0;
		for (size_t index = 0; index < data.groups.size(); ++index) {
			const uint32_t outcome = ClusterLODGroupSimplifyValidation(data.groups[index].flags);
			flagged += outcome != 0u ? 1u : 0u;
			if (simplified[index]) {
				++simplifiedCount;
				acceptedSimplified += (outcome == 1u || outcome == 2u) ? 1u : 0u;
			}
		}
		CHECK(simplifiedCount > 0u);
		if (validate) {
			CHECK(acceptedSimplified == simplifiedCount);
		}
		else {
			CHECK(flagged == 0u);
		}
	}
}

// A flat plane in z = 0 with UV = (x, y). Replacement vertices must stay on it, get the plane's normal when the source's is
// the plane's, and carry UVs that still equal their position (a linear UV field is solved exactly wherever a vertex moves).
// With a normal field that varies in x, the update moves vertices within the plane, and their fitted normals follow the
// source field at their new positions.
void TestReplacementVerticesOnPlane(const fs::path& root)
{
	constexpr uint32_t kResolution = 128u;
	const auto makePlane = [](bool varyingNormals) {
		LayoutSource source;
		source.flags = VertexFlags::VERTEX_NORMALS | VertexFlags::VERTEX_TEXCOORDS;
		source.stride = MeshVertexLayout::VertexSize(source.flags);
		for (uint32_t row = 0; row <= kResolution; ++row) {
			for (uint32_t column = 0; column <= kResolution; ++column) {
				const float x = static_cast<float>(column) / kResolution, y = static_cast<float>(row) / kResolution;
				DirectX::XMFLOAT3 normal{ 0.0f, 0.0f, 1.0f };
				if (varyingNormals) {
					const float tilt = 0.5f * std::sin(x * 6.0f);
					const float length = std::sqrt(tilt * tilt + 1.0f);
					normal = { tilt / length, 0.0f, 1.0f / length };
				}
				source.Add({ x, y, 0.0f }, normal, { x, y });
			}
		}
		const uint32_t stride = kResolution + 1u;
		for (uint32_t row = 0; row < kResolution; ++row) {
			for (uint32_t column = 0; column < kResolution; ++column) {
				const uint32_t a = row * stride + column;
				source.indices.insert(source.indices.end(), { a, a + 1u, a + stride + 1u, a, a + stride + 1u, a + stride });
			}
		}
		return source;
	};
	const auto expectedNormal = [](bool varyingNormals, float x) {
		if (!varyingNormals) {
			return DirectX::XMFLOAT3{ 0.0f, 0.0f, 1.0f };
		}
		const float tilt = 0.5f * std::sin(x * 6.0f);
		const float length = std::sqrt(tilt * tilt + 1.0f);
		return DirectX::XMFLOAT3{ tilt / length, 0.0f, 1.0f / length };
	};

	for (const bool varyingNormals : { false, true }) {
		for (const bool validate : { false, true }) {
			const std::string name = std::format("/replace-plane-{}-{}", varyingNormals ? "varying" : "flat", validate ? "validated" : "plain");
			const auto mesh = SaveAndOpen(root, makePlane(varyingNormals), ReplacementSettings(true, validate), name);
			if (!mesh) {
				continue;
			}
			uint64_t corners = 0, offPlane = 0, offGrid = 0, badUv = 0;
			double worstNormalCos = 1.0;
			ForEachSimplifiedTriangle(*mesh, [&](const std::array<DecodedCorner, 3>& triangle) {
				for (const DecodedCorner& corner : triangle) {
					++corners;
					// Positions are quantized to 1/4096 here; z = 0 is exact.
					offPlane += std::abs(corner.position.z) > 1.0e-4f ? 1u : 0u;
					const float gridX = corner.position.x * kResolution, gridY = corner.position.y * kResolution;
					offGrid += (std::abs(gridX - std::round(gridX)) > 0.05f || std::abs(gridY - std::round(gridY)) > 0.05f) ? 1u : 0u;
					badUv += (std::abs(corner.uv.x - corner.position.x) > 2.0e-3f || std::abs(corner.uv.y - corner.position.y) > 2.0e-3f) ? 1u : 0u;
					const DirectX::XMFLOAT3 expected = expectedNormal(varyingNormals, corner.position.x);
					const DirectX::XMFLOAT3& n = corner.normal;
					const double length = std::sqrt(static_cast<double>(n.x) * n.x + static_cast<double>(n.y) * n.y + static_cast<double>(n.z) * n.z);
					worstNormalCos = std::min(worstNormalCos, (n.x * expected.x + n.y * expected.y + n.z * expected.z) / std::max(length, 1.0e-12));
				}
			});
			CHECK(corners > 0u);
			CHECK(offPlane == 0u);
			CHECK(badUv == 0u);
			if (!varyingNormals) {
				// Octahedral normal encoding is good to a fraction of a degree.
				CHECK(worstNormalCos > 0.9995);
			}
			else {
				// The update moved vertices off the source grid, and every fitted normal follows the source field (a hat-weighted
				// average of it over the vertex's triangles, so within a few degrees on this smooth field).
				CHECK(offGrid > 0u);
				CHECK(worstNormalCos > 0.99);
				CHECK(CountGroupsWithFlag(*mesh, CLOD_GROUP_FLAG_SIMPLIFY_REPLACED) > 0u);
			}
			if (badUv != 0u || offPlane != 0u || worstNormalCos <= 0.99) {
				std::fprintf(stderr, "%s: corners=%llu off_plane=%llu off_grid=%llu bad_uv=%llu worst_normal_cos=%.6f\n", name.c_str(),
					static_cast<unsigned long long>(corners), static_cast<unsigned long long>(offPlane), static_cast<unsigned long long>(offGrid),
					static_cast<unsigned long long>(badUv), worstNormalCos);
			}
		}
	}
}

// A flat panel whose authored normals carry relief its geometry does not have (as when detail is baked into vertex normals):
// the vertices tilt 60 degrees one way or the other in a checkerboard. The regular simplifier keeps some of those vertices
// with their tilted normals, which across a coarse triangle meet the original's interpolated normals far beyond stage 1's
// 75 degree shading limit, so validation alone keeps the panel at its leaves. Fitted normals are close to the original's
// average, which stays within 60 degrees of it everywhere: update+fit with validation must reduce much further, and the
// panel is flat, so nothing can fold or flip against the source.
void TestReplacementVerticesOnGrooves(const fs::path& root)
{
	constexpr uint32_t kResolution = 128u;
	const float tilt = std::tan(60.0f * 3.14159265f / 180.0f);
	LayoutSource source;
	for (uint32_t row = 0; row <= kResolution; ++row) {
		for (uint32_t column = 0; column <= kResolution; ++column) {
			const float x = static_cast<float>(column) / kResolution, y = static_cast<float>(row) / kResolution;
			const float slope = ((row + column) % 2u == 0u) ? tilt : -tilt;
			const float length = std::sqrt(slope * slope + 1.0f);
			source.Add({ x, y, 0.0f }, { slope / length, 0.0f, 1.0f / length });
		}
	}
	const uint32_t stride = kResolution + 1u;
	for (uint32_t row = 0; row < kResolution; ++row) {
		for (uint32_t column = 0; column < kResolution; ++column) {
			const uint32_t a = row * stride + column;
			source.indices.insert(source.indices.end(), { a, a + 1u, a + stride + 1u, a, a + stride + 1u, a + stride });
		}
	}

	const auto validated = SaveAndOpen(root, source, ReplacementSettings(false, true), "/grooves-validate");
	const auto updated = SaveAndOpen(root, source, ReplacementSettings(true, true), "/grooves-update-validate");
	if (!validated || !updated) {
		return;
	}
	const ResultSet validateResults = Analyze(validated->MetadataPath(), "grooves-validate");
	const ResultSet updateResults = Analyze(updated->MetadataPath(), "grooves-update-validate");
	const double validateCoarsest = Value(validateResults, "hierarchy.coarsest_triangles");
	const double updateCoarsest = Value(updateResults, "hierarchy.coarsest_triangles");
	std::fprintf(stderr, "grooves: leaves=%g coarsest validate=%g update+validate=%g flipped validate=%g update+validate=%g inverted validate=%g update+validate=%g\n",
		Value(updateResults, "hierarchy.leaf_triangles"), validateCoarsest, updateCoarsest,
		Value(validateResults, "normals.flipped_vs_source_area_fraction"), Value(updateResults, "normals.flipped_vs_source_area_fraction"),
		Value(validateResults, "normals.inverted_area_fraction"), Value(updateResults, "normals.inverted_area_fraction"));
	CHECK(updateCoarsest < validateCoarsest * 0.8);
	CHECK(Value(updateResults, "normals.flipped_vs_source_area_fraction") < 0.01);
	CHECK(Value(updateResults, "normals.inverted_area_fraction") < 0.01);
}

ClusterLODBuilderSettings OptimalSettings(bool optimal, bool validate)
{
	ClusterLODBuilderSettings settings = ReplacementSettings(true, validate);
	settings.simplifyOptimal = optimal;
	return settings;
}

// Optimal placement (stage 4): off, its parameters change nothing (the build equals stage 2's); on, it builds the same way every
// time whatever the threads, simplifies with replacement vertices, and differs from stage 2.
void TestOptimalOffAndDeterminism(const fs::path& root)
{
	const SourceMesh sheet = MakeSheet(256u, 0.0f);
	LayoutSource source;
	source.vertices = sheet.vertices;
	source.indices = sheet.indices;

	ClusterLODBuilderSettings offTuned = OptimalSettings(false, false);
	offTuned.simplifyOptimalSheetAngle = 20.0f;
	offTuned.simplifyOptimalMoves = 0u;
	const auto update = SaveAndOpen(root, source, OptimalSettings(false, false), "/optimal-off");
	const auto off = SaveAndOpen(root, source, offTuned, "/optimal-off-tuned");
	const auto on = SaveAndOpen(root, source, OptimalSettings(true, false), "/optimal-on");
	const auto onAgain = SaveAndOpen(root, source, OptimalSettings(true, false), "/optimal-on-again");
	const auto onValidated = SaveAndOpen(root, source, OptimalSettings(true, true), "/optimal-on-validated");
	const auto onValidatedAgain = SaveAndOpen(root, source, OptimalSettings(true, true), "/optimal-on-validated-again");
	CHECK(update && off && on && onAgain && onValidated && onValidatedAgain);
	if (!update || !off || !on || !onAgain || !onValidated || !onValidatedAgain) {
		return;
	}
	CHECK(SameBuild(*update, *off));
	CHECK(on->LeafGeometryHash() == update->LeafGeometryHash());
	CHECK(CountGroupsWithFlag(*on, CLOD_GROUP_FLAG_SIMPLIFY_REPLACED) > 0u);
	CHECK(SameBuild(*on, *onAgain));
	CHECK(SameBuild(*onValidated, *onValidatedAgain));
	CHECK(!SameBuild(*on, *update));
}

// Smooth vertex normals (area-weighted face normals) for a positions/indices surface, as a LayoutSource.
LayoutSource SmoothSource(const std::vector<DirectX::XMFLOAT3>& positions, const std::vector<uint32_t>& indices)
{
	std::vector<DirectX::XMFLOAT3> normals(positions.size(), { 0.0f, 0.0f, 0.0f });
	for (size_t i = 0; i < indices.size(); i += 3u) {
		const DirectX::XMFLOAT3& p0 = positions[indices[i]];
		const DirectX::XMFLOAT3& p1 = positions[indices[i + 1u]];
		const DirectX::XMFLOAT3& p2 = positions[indices[i + 2u]];
		const float e1[3] = { p1.x - p0.x, p1.y - p0.y, p1.z - p0.z }, e2[3] = { p2.x - p0.x, p2.y - p0.y, p2.z - p0.z };
		const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
		for (uint32_t corner = 0; corner < 3u; ++corner) {
			DirectX::XMFLOAT3& out = normals[indices[i + corner]];
			out.x += n[0];
			out.y += n[1];
			out.z += n[2];
		}
	}
	LayoutSource source;
	for (size_t i = 0; i < positions.size(); ++i) {
		const DirectX::XMFLOAT3 n = normals[i];
		const float length = std::max(std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z), 1.0e-20f);
		source.Add(positions[i], { n.x / length, n.y / length, n.z / length });
	}
	source.indices = indices;
	return source;
}

// A grid heightfield over [0,1]^2 with z from height(column, row), as a LayoutSource with smooth normals.
template <typename Height>
LayoutSource MakeHeightfield(uint32_t resolution, Height&& height)
{
	const uint32_t stride = resolution + 1u;
	std::vector<DirectX::XMFLOAT3> positions;
	for (uint32_t row = 0; row <= resolution; ++row) {
		for (uint32_t column = 0; column <= resolution; ++column) {
			positions.push_back({ static_cast<float>(column) / resolution, static_cast<float>(row) / resolution, height(column, row) });
		}
	}
	std::vector<uint32_t> indices;
	for (uint32_t row = 0; row < resolution; ++row) {
		for (uint32_t column = 0; column < resolution; ++column) {
			const uint32_t a = row * stride + column;
			indices.insert(indices.end(), { a, a + 1u, a + stride + 1u, a, a + stride + 1u, a + stride });
		}
	}
	return SmoothSource(positions, indices);
}

// Rough panels where meshoptimizer's collapse loop turns faces against their fitted shading: a heightfield of random spikes about
// three cells high, and a panel cut by deep one-cell slots (both with smooth normals). Stage 2 (no validation) leaves inverted
// area (shading facing away from the face) on both; optimal placement validates each collapse at its solved position, per
// collapse and against the source sheet, and must leave much less, without folding against the source and reaching about as
// far. (Neither case folds against the source under either path: Bistro's folds come from thin walls these do not reproduce.)
void TestOptimalOnRoughPanels(const fs::path& root)
{
	constexpr uint32_t kResolution = 96u;
	const float h = 1.0f / kResolution;
	const auto noise = [](uint32_t x, uint32_t y) {
		uint32_t v = x * 73856093u ^ y * 19349663u;
		v ^= v >> 13;
		v *= 0x5bd1e995u;
		v ^= v >> 15;
		return static_cast<float>(v & 0xffffu) / 65535.0f - 0.5f;
	};
	struct Case
	{
		const char* name;
		LayoutSource source;
	};
	const Case cases[] = {
		{ "rough-spikes", MakeHeightfield(kResolution, [&](uint32_t c, uint32_t r) { return 3.0f * h * noise(c, r); }) },
		{ "rough-slots", MakeHeightfield(kResolution, [&](uint32_t c, uint32_t r) { return ((c % 8u == 4u) && (r % 16u) > 2u) ? -4.0f * h : 0.0f; }) },
	};
	for (const Case& test : cases) {
		const std::string name = test.name;
		const auto update = SaveAndOpen(root, test.source, OptimalSettings(false, false), "/" + name + "-update");
		const auto optimal = SaveAndOpen(root, test.source, OptimalSettings(true, false), "/" + name + "-optimal");
		if (!update || !optimal) {
			continue;
		}
		const ResultSet u = Analyze(update->MetadataPath(), name + "-update");
		const ResultSet o = Analyze(optimal->MetadataPath(), name + "-optimal");
		const char* coarsest = "hierarchy.coarsest_triangles";
		const char* flipped = "normals.flipped_vs_source_area_fraction";
		const char* inverted = "normals.inverted_area_fraction";
		std::fprintf(stderr, "%s: leaves=%g coarsest update=%g optimal=%g inverted update=%g optimal=%g flipped update=%g optimal=%g\n", test.name,
			Value(u, "hierarchy.leaf_triangles"), Value(u, coarsest), Value(o, coarsest), Value(u, inverted), Value(o, inverted),
			Value(u, flipped), Value(o, flipped));
		CHECK(Value(u, inverted) > 0.0005);
		CHECK(Value(o, inverted) < Value(u, inverted) * 0.5);
		CHECK(Value(o, flipped) < 0.001);
		CHECK(Value(o, coarsest) < Value(u, coarsest) * 1.5);
	}
}

ClusterLODBuilderSettings TopologySettings(bool topology, bool validate)
{
	ClusterLODBuilderSettings settings = ReplacementSettings(true, validate);
	settings.simplifyTopology = topology;
	return settings;
}

// clodSimplifyMethod_Topology (the permissive method's value): what stage 5's topology candidates record.
constexpr uint32_t kMethodTopology = 2u;

uint32_t CountGroupsWithMethod(const CachedMesh& mesh, uint32_t method)
{
	uint32_t count = 0;
	for (const ClusterLODGroup& group : mesh.Prebuilt().groups) {
		count += ((group.flags & CLOD_GROUP_SIMPLIFY_METHOD_MASK) >> CLOD_GROUP_SIMPLIFY_METHOD_SHIFT) == method ? 1u : 0u;
	}
	return count;
}

// A flat-shaded (faceted) dome: every triangle has its own three vertices carrying its face normal, and UV = (x, y). Every
// vertex is on a hard normal edge, so every one carries a protect bit and the ordinary simplifier can hardly collapse anything,
// while the UVs are continuous (no UV seam).
LayoutSource MakeFacetedDome(uint32_t resolution)
{
	LayoutSource source;
	source.flags = VertexFlags::VERTEX_NORMALS | VertexFlags::VERTEX_TEXCOORDS;
	source.stride = MeshVertexLayout::VertexSize(source.flags);
	const auto point = [&](uint32_t column, uint32_t row) {
		const float x = static_cast<float>(column) / resolution, y = static_cast<float>(row) / resolution;
		return DirectX::XMFLOAT3{ x, y, 0.15f * std::sin(3.14159265f * x) * std::sin(3.14159265f * y) };
	};
	const auto addTriangle = [&](const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, const DirectX::XMFLOAT3& c) {
		const float e1[3] = { b.x - a.x, b.y - a.y, b.z - a.z }, e2[3] = { c.x - a.x, c.y - a.y, c.z - a.z };
		float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
		const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		const DirectX::XMFLOAT3 normal{ n[0] / length, n[1] / length, n[2] / length };
		const uint32_t first = static_cast<uint32_t>(source.vertices.size() / source.stride);
		for (const DirectX::XMFLOAT3& p : { a, b, c }) {
			source.Add(p, normal, { p.x, p.y });
		}
		source.indices.insert(source.indices.end(), { first, first + 1u, first + 2u });
	};
	for (uint32_t row = 0; row < resolution; ++row) {
		for (uint32_t column = 0; column < resolution; ++column) {
			addTriangle(point(column, row), point(column + 1u, row), point(column + 1u, row + 1u));
			addTriangle(point(column, row), point(column + 1u, row + 1u), point(column, row + 1u));
		}
	}
	return source;
}

// Controlled topology changes (stage 5): off, its settings change nothing (the build equals stage 2's); on, plain and validated
// builds are each the same every time, use topology candidates (recorded as the permissive method) and differ from stage 2.
void TestTopologyOffAndDeterminism(const fs::path& root)
{
	const LayoutSource source = MakeFacetedDome(48u);
	ClusterLODBuilderSettings offTuned = TopologySettings(false, false);
	offTuned.simplifyTopologyRetries = 5u;
	offTuned.simplifyTopologyRelaxUv = true;
	offTuned.simplifyTopologyTextureArea = 0.5f;
	offTuned.simplifyTopologyTextureCharts = true;
	offTuned.simplifyTopologyTextureTexels = 4.0f;
	offTuned.simplifyTopologyUpdateFallback = true;
	const auto update = SaveAndOpen(root, source, TopologySettings(false, false), "/topology-off");
	const auto off = SaveAndOpen(root, source, offTuned, "/topology-off-tuned");
	const auto on = SaveAndOpen(root, source, TopologySettings(true, false), "/topology-on");
	const auto onAgain = SaveAndOpen(root, source, TopologySettings(true, false), "/topology-on-again");
	const auto onValidated = SaveAndOpen(root, source, TopologySettings(true, true), "/topology-on-validated");
	const auto onValidatedAgain = SaveAndOpen(root, source, TopologySettings(true, true), "/topology-on-validated-again");
	CHECK(update && off && on && onAgain && onValidated && onValidatedAgain);
	if (!update || !off || !on || !onAgain || !onValidated || !onValidatedAgain) {
		return;
	}
	CHECK(SameBuild(*update, *off));
	CHECK(CountGroupsWithMethod(*update, kMethodTopology) == 0u);
	CHECK(on->LeafGeometryHash() == update->LeafGeometryHash());
	CHECK(CountGroupsWithMethod(*on, kMethodTopology) > 0u);
	CHECK(SameBuild(*on, *onAgain));
	CHECK(SameBuild(*onValidated, *onValidatedAgain));
	CHECK(!SameBuild(*on, *update));
}

// The stall stage 5 is for: on the faceted dome every vertex is protected, so stage 2 without sloppy stops at its leaves or
// nearly. Topology candidates relax the hard edges (no UV seam crosses them), refit normals from the original, and pass
// validation: the hierarchy must reach far fewer triangles without flipping against the source.
void TestTopologyOnFacetedDome(const fs::path& root)
{
	const LayoutSource source = MakeFacetedDome(64u);
	const auto update = SaveAndOpen(root, source, TopologySettings(false, false), "/faceted-update");
	const auto topology = SaveAndOpen(root, source, TopologySettings(true, false), "/faceted-topology");
	const auto topologyValidated = SaveAndOpen(root, source, TopologySettings(true, true), "/faceted-topology-validated");
	if (!update || !topology || !topologyValidated) {
		CHECK(false);
		return;
	}
	const ResultSet u = Analyze(update->MetadataPath(), "faceted-update");
	const ResultSet t = Analyze(topology->MetadataPath(), "faceted-topology");
	const ResultSet v = Analyze(topologyValidated->MetadataPath(), "faceted-topology-validated");
	const char* coarsest = "hierarchy.coarsest_triangles";
	const char* flipped = "normals.flipped_vs_source_area_fraction";
	const char* inverted = "normals.inverted_area_fraction";
	std::fprintf(stderr, "faceted dome: leaves=%g coarsest update=%g topology=%g topology+validate=%g flipped topology=%g validated=%g inverted topology=%g validated=%g\n",
		Value(u, "hierarchy.leaf_triangles"), Value(u, coarsest), Value(t, coarsest), Value(v, coarsest), Value(t, flipped), Value(v, flipped),
		Value(t, inverted), Value(v, inverted));
	CHECK(Value(u, coarsest) > Value(u, "hierarchy.leaf_triangles") * 0.5);
	CHECK(Value(t, coarsest) < Value(u, coarsest) * 0.25);
	CHECK(Value(v, coarsest) < Value(u, coarsest) * 0.25);
	CHECK(Value(t, flipped) < 0.005);
	CHECK(Value(v, flipped) < 0.005);
	CHECK(Value(t, inverted) < 0.005);
	CHECK(Value(v, inverted) < 0.005);

	// UVs follow the surface: UV = (x, y) is linear in position, so every decoded simplified corner must still carry its own xy.
	ClusterLODBuilderSettings unchecked = TopologySettings(true, false);
	unchecked.simplifyTopologyTextureArea = 1.0f;
	const auto crossed = SaveAndOpen(root, source, unchecked, "/faceted-topology-unchecked");
	if (!crossed) {
		CHECK(false);
		return;
	}
	// Topology candidates transfer texcoords from the original where the match lies in the vertex's own neighbourhood; elsewhere
	// meshoptimizer's solve (which drifts on curved surfaces) stays. Cells are 1/64.
	float worst = 0.0f;
	size_t corners = 0, beyondHalfCell = 0;
	ForEachSimplifiedTriangle(*crossed, [&](const std::array<DecodedCorner, 3>& triangle) {
		for (const DecodedCorner& corner : triangle) {
			const float error = std::max(std::fabs(corner.uv.x - corner.position.x), std::fabs(corner.uv.y - corner.position.y));
			worst = std::max(worst, error);
			beyondHalfCell += error > 0.5f / 64.0f ? 1u : 0u;
			++corners;
		}
	});
	std::fprintf(stderr, "faceted dome unchecked: corners=%zu worst |uv - xy|=%g beyond half a cell=%zu\n", corners, worst, beyondHalfCell);
	CHECK(corners > 0u);
	CHECK(beyondHalfCell * 50u <= corners); // 1.1% when written
	CHECK(worst < 2.0f / 64.0f);
}

// The last-resort sloppy fallback (on by default): with the global fallback off, a group every other candidate leaves stuck is
// simplified by sloppy instead of staying terminal. The faceted dome without topology candidates is stuck (above); with the
// last resort every group but the root has a parent and a real error, the sloppy groups are exactly the formerly stuck ones,
// and the build is deterministic. Without it (the setting off) the build is the stuck one. (The dome is one group: stuck, it is
// the root with all 8192 triangles; with the last resort the root is a small group sloppy cannot reduce further.)
void TestSloppyLastResort(const fs::path& root)
{
	const LayoutSource source = MakeFacetedDome(64u);
	ClusterLODBuilderSettings stuckSettings = TopologySettings(false, false);
	ClusterLODBuilderSettings lastResortSettings = stuckSettings;
	lastResortSettings.sloppyLastResort = true;
	const auto stuck = SaveAndOpen(root, source, stuckSettings, "/last-resort-off");
	const auto lastResort = SaveAndOpen(root, source, lastResortSettings, "/last-resort-on");
	const auto again = SaveAndOpen(root, source, lastResortSettings, "/last-resort-again");
	if (!stuck || !lastResort || !again) {
		CHECK(false);
		return;
	}
	CHECK(SameBuild(*lastResort, *again));
	constexpr uint32_t kMethodSloppy = 3u;
	const auto countRoots = [](const CachedMesh& mesh) {
		uint32_t roots = 0u;
		for (const ClusterLODGroup& group : mesh.Prebuilt().groups) {
			if (group.parentGroupId < 0) {
				++roots;
			}
			else {
				CHECK(group.bounds.error < std::numeric_limits<float>::max() * 0.5f);
			}
		}
		return roots;
	};
	const uint32_t stuckRoots = countRoots(*stuck);
	const uint32_t lastResortRoots = countRoots(*lastResort);
	const ResultSet s = Analyze(stuck->MetadataPath(), "last-resort-off");
	const ResultSet l = Analyze(lastResort->MetadataPath(), "last-resort-on");
	const char* coarsest = "hierarchy.coarsest_triangles";
	std::fprintf(stderr, "sloppy last resort: roots off=%u on=%u coarsest off=%g on=%g sloppy groups off=%u on=%u\n", stuckRoots, lastResortRoots,
		Value(s, coarsest), Value(l, coarsest), CountGroupsWithMethod(*stuck, kMethodSloppy), CountGroupsWithMethod(*lastResort, kMethodSloppy));
	CHECK(lastResortRoots <= stuckRoots);
	CHECK(CountGroupsWithMethod(*stuck, kMethodSloppy) == 0u);
	CHECK(CountGroupsWithMethod(*lastResort, kMethodSloppy) > 0u);
	CHECK(Value(l, coarsest) < Value(s, coarsest) * 0.25);
}

// UV chart seams: a smooth dome cut into tiles, each its own chart placed and turned differently in the atlas (so its tangents
// differ from its neighbours' and its border vertices are protected), with continuous normals. Topology candidates keep UV seams
// unless asked. Asked, the texture check refuses the crossings: on a curved surface a triangle across a seam mixes two charts, so
// the texture slides by about their separation in the atlas. With the check off the same candidates cross and simplify further.
// (On a flat panel, removing straight chart-border vertices moves no texture, and the check rightly lets it through.)
LayoutSource MakeChartDome()
{
	constexpr uint32_t kTiles = 8u, kCells = 8u;
	LayoutSource source;
	source.flags = VertexFlags::VERTEX_NORMALS | VertexFlags::VERTEX_TEXCOORDS;
	source.stride = MeshVertexLayout::VertexSize(source.flags);
	for (uint32_t tileRow = 0; tileRow < kTiles; ++tileRow) {
		for (uint32_t tileColumn = 0; tileColumn < kTiles; ++tileColumn) {
			const uint32_t tile = tileRow * kTiles + tileColumn;
			const uint32_t first = static_cast<uint32_t>(source.vertices.size() / source.stride);
			const float atlasU = static_cast<float>(tile % 8u) / 8.0f, atlasV = static_cast<float>(tile / 8u) / 8.0f;
			for (uint32_t row = 0; row <= kCells; ++row) {
				for (uint32_t column = 0; column <= kCells; ++column) {
					const float s = static_cast<float>(column) / kCells, t = static_cast<float>(row) / kCells;
					const float x = (tileColumn + s) / kTiles, y = (tileRow + t) / kTiles;
					// each tile turned a quarter turn from the previous one inside its atlas cell
					float u = s, v = t;
					switch (tile % 4u) {
					case 1u: u = t; v = 1.0f - s; break;
					case 2u: u = 1.0f - s; v = 1.0f - t; break;
					case 3u: u = 1.0f - t; v = s; break;
					default: break;
					}
					constexpr float kPi = 3.14159265f, kHeight = 0.15f;
					const float z = kHeight * std::sin(kPi * x) * std::sin(kPi * y);
					const float dx = kHeight * kPi * std::cos(kPi * x) * std::sin(kPi * y), dy = kHeight * kPi * std::sin(kPi * x) * std::cos(kPi * y);
					const float length = std::sqrt(dx * dx + dy * dy + 1.0f);
					source.Add({ x, y, z }, { -dx / length, -dy / length, 1.0f / length }, { atlasU + u * 0.12f, atlasV + v * 0.12f });
				}
			}
			for (uint32_t row = 0; row < kCells; ++row) {
				for (uint32_t column = 0; column < kCells; ++column) {
					const uint32_t a = first + row * (kCells + 1u) + column;
					source.indices.insert(source.indices.end(), { a, a + 1u, a + kCells + 2u, a, a + kCells + 2u, a + kCells + 1u });
				}
			}
		}
	}
	return source;
}

void TestTopologyTextureCheck(const fs::path& root)
{
	const LayoutSource source = MakeChartDome();

	// The default slide tolerance (8 radii) is calibrated to let stage 2's own UV drift through and so lets crossings through at
	// coarse levels, where the radius grows past the charts' separation; a strict tolerance shows the mechanism at every level.
	ClusterLODBuilderSettings relaxUv = TopologySettings(true, false);
	relaxUv.simplifyTopologyRelaxUv = true;
	relaxUv.simplifyTopologyTextureSlide = 1.0f;
	ClusterLODBuilderSettings unchecked = relaxUv;
	unchecked.simplifyTopologyTextureArea = 1.0f;
	const auto keep = SaveAndOpen(root, source, TopologySettings(true, false), "/charts-keep");
	const auto checked = SaveAndOpen(root, source, relaxUv, "/charts-relax-checked");
	const auto crossed = SaveAndOpen(root, source, unchecked, "/charts-relax-unchecked");
	if (!keep || !checked || !crossed) {
		CHECK(false);
		return;
	}
	const ResultSet k = Analyze(keep->MetadataPath(), "charts-keep");
	const ResultSet c = Analyze(checked->MetadataPath(), "charts-relax-checked");
	const ResultSet x = Analyze(crossed->MetadataPath(), "charts-relax-unchecked");
	const char* coarsest = "hierarchy.coarsest_triangles";
	std::fprintf(stderr, "charts: leaves=%g coarsest keep=%g relax+check=%g relax unchecked=%g\n", Value(k, "hierarchy.leaf_triangles"), Value(k, coarsest),
		Value(c, coarsest), Value(x, coarsest));
	CHECK(Value(c, coarsest) >= Value(k, coarsest) * 0.9);
	CHECK(Value(x, coarsest) <= Value(k, coarsest) * 0.5);
}

// A smooth dome over [0,1]^2 (height 0.15, analytic normals) with UV = (x, y): no seams, so nothing is protected, and the UVs are
// linear in position. withUvSets adds a TEXCOORD_0 set (AddTexcoordSet); the pages then store the set, which the update solves as
// its own stream.
// Hands the builder a TEXCOORD_0 set equal to the record texcoord, as the glTF importer does.
void AddTexcoordSet(LayoutSource& source)
{
	MeshUvSetData set;
	set.name = "TEXCOORD_0";
	for (size_t vertex = 0; vertex < source.vertices.size() / source.stride; ++vertex) {
		DirectX::XMFLOAT2 uv;
		std::memcpy(&uv, source.vertices.data() + vertex * source.stride + MeshVertexLayout::TexcoordOffset(source.flags), sizeof(uv));
		set.values.push_back(uv);
	}
	source.uvSets = { std::move(set) };
}

LayoutSource MakeSmoothUvDome(uint32_t resolution, bool withUvSets)
{
	LayoutSource source;
	source.flags = VertexFlags::VERTEX_NORMALS | VertexFlags::VERTEX_TEXCOORDS;
	source.stride = MeshVertexLayout::VertexSize(source.flags);
	constexpr float kPi = 3.14159265f, kHeight = 0.15f;
	for (uint32_t row = 0; row <= resolution; ++row) {
		for (uint32_t column = 0; column <= resolution; ++column) {
			const float x = static_cast<float>(column) / resolution, y = static_cast<float>(row) / resolution;
			const float dx = kHeight * kPi * std::cos(kPi * x) * std::sin(kPi * y), dy = kHeight * kPi * std::sin(kPi * x) * std::cos(kPi * y);
			const float length = std::sqrt(dx * dx + dy * dy + 1.0f);
			source.Add({ x, y, kHeight * std::sin(kPi * x) * std::sin(kPi * y) }, { -dx / length, -dy / length, 1.0f / length }, { x, y });
		}
	}
	const uint32_t stride = resolution + 1u;
	for (uint32_t row = 0; row < resolution; ++row) {
		for (uint32_t column = 0; column < resolution; ++column) {
			const uint32_t a = row * stride + column;
			source.indices.insert(source.indices.end(), { a, a + 1u, a + stride + 1u, a, a + stride + 1u, a + stride });
		}
	}
	if (withUvSets) {
		AddTexcoordSet(source);
	}
	return source;
}

// Decoded UV drift of every simplified corner whose UV should equal its position's xy: mean and worst |uv - xy| (max norm).
struct UvDrift {
	size_t corners = 0;
	double mean = 0.0;
	float worst = 0.0f;
};
UvDrift MeasureXyUvDrift(const CachedMesh& mesh)
{
	UvDrift drift;
	double total = 0.0;
	ForEachSimplifiedTriangle(mesh, [&](const std::array<DecodedCorner, 3>& triangle) {
		for (const DecodedCorner& corner : triangle) {
			const float error = std::max(std::fabs(corner.uv.x - corner.position.x), std::fabs(corner.uv.y - corner.position.y));
			drift.worst = std::max(drift.worst, error);
			total += error;
			++drift.corners;
		}
	});
	drift.mean = drift.corners > 0u ? total / static_cast<double>(drift.corners) : 0.0;
	return drift;
}

// Follow-up to stages 2 and 5 (simplifyUpdateUvTransfer): every update candidate's moved vertices take the original's UVs at
// their match, every UV stream. Drift is measured against the leaves at the match (quality.texcoords), per depth. On stage 5's
// faceted dome with a TEXCOORD_0 set (the pages store the set, which stage 5's own transfer of the record texcoord does not
// reach) meshoptimizer's per-wedge solve drifts and the transfer must cut it, and reach coarser levels; on a smooth dome (stage
// 2's update, with and without a set) the solve is already accurate and the transfer must stay within a texel. Corners must
// improve everywhere. On, it builds the same way twice and leaves the leaves alone.
void TestUpdateUvTransfer(const fs::path& root)
{
	struct Case
	{
		const char* name;
		LayoutSource source;
		ClusterLODBuilderSettings settings;
		bool faceted;
	};
	const Case cases[] = {
		{ "uv-smooth", MakeSmoothUvDome(96u, false), ReplacementSettings(true, false), false },
		{ "uv-smooth-sets", MakeSmoothUvDome(96u, true), ReplacementSettings(true, false), false },
		{ "uv-faceted-sets", [] { LayoutSource s = MakeFacetedDome(64u); AddTexcoordSet(s); return s; }(), TopologySettings(true, false), true },
	};
	for (const Case& test : cases) {
		const LayoutSource& source = test.source;
		ClusterLODBuilderSettings transfer = test.settings;
		transfer.simplifyUpdateUvTransfer = true;
		const std::string name = test.name;
		const auto solve = SaveAndOpen(root, source, test.settings, "/" + name + "-solve");
		const auto moved = SaveAndOpen(root, source, transfer, "/" + name + "-transfer");
		const auto movedAgain = SaveAndOpen(root, source, transfer, "/" + name + "-transfer-again");
		if (!solve || !moved || !movedAgain) {
			CHECK(false);
			continue;
		}
		const UvDrift before = MeasureXyUvDrift(*solve);
		const UvDrift after = MeasureXyUvDrift(*moved);
		const ResultSet b = Analyze(solve->MetadataPath(), name + "-solve");
		const ResultSet a = Analyze(moved->MetadataPath(), name + "-transfer");
		const char* drift = "texcoords.drift_mean";
		const char* corner = "texcoords.corner_drift_mean";
		const Tags over{ { "over", "1/256" } };
		std::fprintf(stderr, "%s: corners solve=%zu transfer=%zu |uv - xy| mean solve=%g transfer=%g worst solve=%g transfer=%g; against the leaves at the match: "
			"interior mean solve=%g transfer=%g, corner mean solve=%g transfer=%g, area over 1/256 solve=%g transfer=%g, coarsest solve=%g transfer=%g\n",
			test.name, before.corners, after.corners, before.mean, after.mean, before.worst, after.worst, Value(b, drift), Value(a, drift), Value(b, corner),
			Value(a, corner), Value(b, "texcoords.drift_over_area_fraction", over), Value(a, "texcoords.drift_over_area_fraction", over),
			Value(b, "hierarchy.coarsest_triangles"), Value(a, "hierarchy.coarsest_triangles"));
		// Per depth, where both builds have simplified triangles: interior and corner drift against the leaves at the match.
		const auto find = [](const ResultSet& results, const char* id, int depth) {
			for (const MetricRecord& record : results.records) {
				if (record.id == id && record.mesh == "*" && record.tags == Tags{ { "depth", std::to_string(depth) } }) {
					return record.value;
				}
			}
			return -1.0;
		};
		int compared = 0;
		for (int depth = 1; depth < 16; ++depth) {
			const double interiorBefore = find(b, drift, depth), interiorAfter = find(a, drift, depth);
			const double cornerBefore = find(b, corner, depth), cornerAfter = find(a, corner, depth);
			if (interiorBefore < 0.0 || interiorAfter < 0.0) {
				continue;
			}
			++compared;
			std::fprintf(stderr, "  depth %d: interior solve=%g transfer=%g, corners solve=%g transfer=%g\n", depth, interiorBefore, interiorAfter, cornerBefore,
				cornerAfter);
			// Corners take the original's UV at their match: exact up to the pages' UV quantization.
			CHECK(cornerAfter < cornerBefore);
			if (test.faceted) {
				// Each wedge solved from its own few triangles drifts; the transfer does not.
				CHECK(interiorAfter < interiorBefore * 0.7);
			}
			else {
				// Where wedges share their triangles the solve fits UVs that are linear in position nearly exactly, and is a
				// least-squares fit over each triangle's area where the transfer interpolates its corners: interiors drift a
				// little more with the transfer, both within two texels of a 1024-texel texture.
				CHECK(interiorAfter < interiorBefore * 3.0 + 1.0e-5);
				CHECK(interiorAfter < 2.0 / 1024.0);
			}
		}
		CHECK(compared > 0);
		if (test.faceted) {
			// Solved wedges of one position no longer agree, so they read as UV seams and stop the next level's topology candidates.
			CHECK(Value(a, "hierarchy.coarsest_triangles") < Value(b, "hierarchy.coarsest_triangles") * 0.5);
		}
		CHECK(SameBuild(*moved, *movedAgain));
		CHECK(moved->LeafGeometryHash() == solve->LeafGeometryHash());
		CHECK(before.corners > 0u && after.corners > 0u);
	}
}

// Follow-up to stages 4 and 5: stage 4's optimal loop simplifies topology candidates (relaxed protect bits) with its per-collapse
// and source-sheet checks and stage 2's fit. On the faceted dome its candidates fail stage 5's texture check at depth 0 (the loop
// reports far smaller errors, and the check's tolerance is in radii of that error), so optimal-only topology leaves the dome at its
// leaves. With simplifyTopologyUpdateFallback the stage 2 candidate follows a rejected optimal one: the hierarchy must then reach
// about as far as topology without the optimal loop, plain and validated, without flipping or inverting against the source, and
// every build must be the same twice.
void TestOptimalTopology(const fs::path& root)
{
	const LayoutSource source = MakeFacetedDome(64u);
	ClusterLODBuilderSettings optimal = TopologySettings(true, false);
	optimal.simplifyOptimal = true;
	ClusterLODBuilderSettings fallback = optimal;
	fallback.simplifyTopologyUpdateFallback = true;
	ClusterLODBuilderSettings fallbackValidated = TopologySettings(true, true);
	fallbackValidated.simplifyOptimal = true;
	fallbackValidated.simplifyTopologyUpdateFallback = true;
	const auto topology = SaveAndOpen(root, source, TopologySettings(true, false), "/faceted-opt-topology");
	const auto only = SaveAndOpen(root, source, optimal, "/faceted-opt-only");
	const auto onlyAgain = SaveAndOpen(root, source, optimal, "/faceted-opt-only-again");
	const auto on = SaveAndOpen(root, source, fallback, "/faceted-opt-fallback");
	const auto onAgain = SaveAndOpen(root, source, fallback, "/faceted-opt-fallback-again");
	const auto onValidated = SaveAndOpen(root, source, fallbackValidated, "/faceted-opt-fallback-validated");
	const auto onValidatedAgain = SaveAndOpen(root, source, fallbackValidated, "/faceted-opt-fallback-validated-again");
	if (!topology || !only || !onlyAgain || !on || !onAgain || !onValidated || !onValidatedAgain) {
		CHECK(false);
		return;
	}
	CHECK(SameBuild(*only, *onlyAgain));
	CHECK(SameBuild(*on, *onAgain));
	CHECK(SameBuild(*onValidated, *onValidatedAgain));
	CHECK(!SameBuild(*on, *only));
	CHECK(CountGroupsWithMethod(*on, kMethodTopology) > 0u);
	CHECK(CountGroupsWithMethod(*onValidated, kMethodTopology) > 0u);
	const char* coarsest = "hierarchy.coarsest_triangles";
	const char* flipped = "normals.flipped_vs_source_area_fraction";
	const char* inverted = "normals.inverted_area_fraction";
	const ResultSet t = Analyze(topology->MetadataPath(), "faceted-opt-topology");
	const ResultSet n = Analyze(only->MetadataPath(), "faceted-opt-only");
	const ResultSet o = Analyze(on->MetadataPath(), "faceted-opt-fallback");
	const ResultSet v = Analyze(onValidated->MetadataPath(), "faceted-opt-fallback-validated");
	std::fprintf(stderr, "faceted dome optimal topology: coarsest topology=%g optimal only=%g optimal+fallback=%g validated=%g flipped=%g validated=%g "
		"inverted=%g validated=%g\n", Value(t, coarsest), Value(n, coarsest), Value(o, coarsest), Value(v, coarsest), Value(o, flipped), Value(v, flipped),
		Value(o, inverted), Value(v, inverted));
	CHECK(Value(o, coarsest) < Value(t, coarsest) * 1.5);
	CHECK(Value(v, coarsest) < Value(t, coarsest) * 1.5);
	CHECK(Value(o, flipped) < 0.005);
	CHECK(Value(v, flipped) < 0.005);
	CHECK(Value(o, inverted) < 0.005);
	CHECK(Value(v, inverted) < 0.005);
}

// Follow-up to stage 5's texture check: at its default tolerance (8 radii) crossings of the chart dome's UV seams pass once the
// radius grows past the charts' separation. The chart test (a triangle mixing charts, or matched to another chart, is displaced
// however near in mesh units) and the texel test (a UV-space tolerance) refuse them at every level: with either, relaxing UV seams
// must keep about the coarsest level that keeping them gives, where the default check alone lets the same candidates cross.
void TestTopologyTextureChartsAndTexels(const fs::path& root)
{
	const LayoutSource source = MakeChartDome();
	ClusterLODBuilderSettings relaxUv = TopologySettings(true, false);
	relaxUv.simplifyTopologyRelaxUv = true;
	ClusterLODBuilderSettings charts = relaxUv;
	charts.simplifyTopologyTextureCharts = true;
	ClusterLODBuilderSettings texels = relaxUv;
	texels.simplifyTopologyTextureTexels = 4.0f;
	const auto keep = SaveAndOpen(root, source, TopologySettings(true, false), "/charts2-keep");
	const auto slide = SaveAndOpen(root, source, relaxUv, "/charts2-relax-slide");
	const auto chartChecked = SaveAndOpen(root, source, charts, "/charts2-relax-charts");
	const auto chartCheckedAgain = SaveAndOpen(root, source, charts, "/charts2-relax-charts-again");
	const auto texelChecked = SaveAndOpen(root, source, texels, "/charts2-relax-texels");
	if (!keep || !slide || !chartChecked || !chartCheckedAgain || !texelChecked) {
		CHECK(false);
		return;
	}
	const char* coarsest = "hierarchy.coarsest_triangles";
	const double k = Value(Analyze(keep->MetadataPath(), "charts2-keep"), coarsest);
	const double s = Value(Analyze(slide->MetadataPath(), "charts2-relax-slide"), coarsest);
	const double c = Value(Analyze(chartChecked->MetadataPath(), "charts2-relax-charts"), coarsest);
	const double t = Value(Analyze(texelChecked->MetadataPath(), "charts2-relax-texels"), coarsest);
	std::fprintf(stderr, "charts follow-up: coarsest keep=%g relax+slide check=%g relax+charts=%g relax+texels=%g\n", k, s, c, t);
	CHECK(SameBuild(*chartChecked, *chartCheckedAgain));
	CHECK(s <= k * 0.75);
	CHECK(c >= k * 0.9);
	CHECK(t >= k * 0.9);
}

ResultSet AnalyzeWithRules(const fs::path& metadata, const std::string& build, const std::vector<CutRule>& rules);

// Stage 6 texture channel: with weight 0 the setting changes nothing; on, groups and pages are those of the build without it (only
// the nodes' appearance words differ), no group's appearance channel falls and some rise where relaxed UV seams let texture cross
// charts, rebuilding is identical, and a rule with the appearance weight then draws at least as many triangles at every cut.
void TestTextureChannel(const fs::path& root)
{
	const LayoutSource source = MakeChartDome();
	ClusterLODBuilderSettings base = TopologySettings(true, false);
	base.simplifyTopologyRelaxUv = true;
	base.simplifyTopologyTextureArea = 1.0f;  // no texture check: let candidates cross charts
	base.lodErrorChannels = true;
	ClusterLODBuilderSettings zero = base;
	zero.lodErrorChannelsTexture = 0.0f;
	zero.lodErrorChannelsTextureSlide = 3.0f;
	ClusterLODBuilderSettings texture = base;
	texture.lodErrorChannelsTexture = 1.0f;
	const auto plain = SaveAndOpen(root, source, base, "/texture-channel-plain");
	const auto off = SaveAndOpen(root, source, zero, "/texture-channel-zero");
	const auto on = SaveAndOpen(root, source, texture, "/texture-channel-on");
	const auto again = SaveAndOpen(root, source, texture, "/texture-channel-on-again");
	if (!plain || !off || !on || !again) {
		CHECK(false);
		return;
	}
	const auto sameNodes = [](const CachedMesh& a, const CachedMesh& b, bool ignoreAppearance) {
		const auto& na = a.Prebuilt().nodes;
		const auto& nb = b.Prebuilt().nodes;
		if (na.size() != nb.size()) return false;
		for (size_t index = 0; index < na.size(); ++index) {
			ClusterLODNode x = na[index], y = nb[index];
			if (ignoreAppearance) {
				x.traversalMetric.maxAppearanceError = 0.0f;
				y.traversalMetric.maxAppearanceError = 0.0f;
			}
			if (std::memcmp(&x, &y, sizeof(ClusterLODNode)) != 0) return false;
		}
		return true;
	};
	CHECK(SameBuild(*plain, *off));
	CHECK(sameNodes(*plain, *off, false));
	CHECK(SameBuild(*plain, *on));
	CHECK(sameNodes(*plain, *on, true));
	CHECK(SameBuild(*on, *again));
	CHECK(sameNodes(*on, *again, false));
	const GroupErrorChannels a = ReadGroupErrorChannels(plain->Prebuilt());
	const GroupErrorChannels b = ReadGroupErrorChannels(on->Prebuilt());
	CHECK(a.present && b.present);
	uint32_t fell = 0, rose = 0;
	for (size_t group = 0; group < a.appearance.size() && group < b.appearance.size(); ++group) {
		fell += b.appearance[group] < a.appearance[group] ? 1u : 0u;
		rose += b.appearance[group] > a.appearance[group] ? 1u : 0u;
		CHECK(b.measured[group] == a.measured[group]);
	}
	std::fprintf(stderr, "texture channel: groups %zu, appearance raised in %u\n", a.appearance.size(), rose);
	CHECK(fell == 0u);
	CHECK(rose > 0u);

	// With a UV set (what glTF pages store and draw) the channel reads the set: the same chart dome, the same raised groups.
	LayoutSource withSet = MakeChartDome();
	AddTexcoordSet(withSet);
	const auto setPlain = SaveAndOpen(root, withSet, base, "/texture-channel-set-plain");
	const auto setOn = SaveAndOpen(root, withSet, texture, "/texture-channel-set-on");
	CHECK(setPlain && setOn);
	if (setPlain && setOn) {
		CHECK(SameBuild(*setPlain, *setOn));
		const GroupErrorChannels sa = ReadGroupErrorChannels(setPlain->Prebuilt());
		const GroupErrorChannels sb = ReadGroupErrorChannels(setOn->Prebuilt());
		uint32_t setRose = 0, setFell = 0;
		for (size_t group = 0; group < sa.appearance.size() && group < sb.appearance.size(); ++group) {
			setRose += sb.appearance[group] > sa.appearance[group] ? 1u : 0u;
			setFell += sb.appearance[group] < sa.appearance[group] ? 1u : 0u;
		}
		CHECK(setFell == 0u);
		CHECK(setRose > 0u);
	}

	// Selecting with the appearance weight: the texture term only delays levels.
	const std::vector<CutRule> rules{ { "w1", CutGeometricChannel::Quadric, 1.0f } };
	const ResultSet plainResults = AnalyzeWithRules(plain->MetadataPath(), "texture-channel-plain", rules);
	const ResultSet onResults = AnalyzeWithRules(on->MetadataPath(), "texture-channel-on", rules);
	const auto drawn = [](const ResultSet& results) {
		std::map<std::string, double> out;
		for (const MetricRecord& record : results.records) {
			const auto rule = record.tags.find("rule");
			if (record.id == "appearance.cut.triangles" && record.mesh == "*" && rule != record.tags.end() && rule->second == "w1") {
				out[record.tags.at("cut")] = record.value;
			}
		}
		return out;
	};
	const auto plainDrawn = drawn(plainResults), onDrawn = drawn(onResults);
	CHECK(!plainDrawn.empty() && plainDrawn.size() == onDrawn.size());
	uint32_t fewer = 0, more = 0;
	for (const auto& [cut, triangles] : plainDrawn) {
		const auto found = onDrawn.find(cut);
		if (found == onDrawn.end()) continue;
		fewer += found->second < triangles ? 1u : 0u;
		more += found->second > triangles ? 1u : 0u;
	}
	CHECK(fewer == 0u);
	CHECK(more > 0u);
}

// The source-material reader quality.texture uses: a hand-made .glb with one material and a 4x4 PNG half black, half white.
void TestSourceTextures(const fs::path& root)
{
	std::vector<uint8_t> rgb(4u * 4u * 3u, 0u);
	for (uint32_t y = 0; y < 4u; ++y) {
		for (uint32_t x = 2u; x < 4u; ++x) {
			for (uint32_t c = 0; c < 3u; ++c) rgb[(y * 4u + x) * 3u + c] = 255u;
		}
	}
	const fs::path png = root / "source-textures.png";
	std::string error;
	CHECK(clodeval::image::WritePng(png, 4u, 4u, rgb, error));
	std::ifstream pngFile(png, std::ios::binary);
	std::vector<char> image((std::istreambuf_iterator<char>(pngFile)), std::istreambuf_iterator<char>());
	while (image.size() % 4u != 0u) image.push_back(0);
	std::string json = std::format(
		R"({{"asset":{{"version":"2.0"}},"buffers":[{{"byteLength":{0}}}],"bufferViews":[{{"buffer":0,"byteOffset":0,"byteLength":{0}}}],)"
		R"("images":[{{"bufferView":0,"mimeType":"image/png"}}],"textures":[{{"source":0}}],)"
		R"("materials":[{{"pbrMetallicRoughness":{{"baseColorFactor":[0.5,1,1,1],"baseColorTexture":{{"index":0}}}},"alphaMode":"MASK","alphaCutoff":0.25}}],)"
		R"("meshes":[{{"primitives":[{{"attributes":{{}},"material":0}},{{"attributes":{{}}}}]}}]}})",
		image.size());
	while (json.size() % 4u != 0u) json.push_back(' ');
	const fs::path glb = root / "source-textures.glb";
	{
		std::ofstream out(glb, std::ios::binary);
		const auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
		u32(0x46546C67u);
		u32(2u);
		u32(static_cast<uint32_t>(12u + 8u + json.size() + 8u + image.size()));
		u32(static_cast<uint32_t>(json.size()));
		u32(0x4E4F534Au);
		out.write(json.data(), static_cast<std::streamsize>(json.size()));
		u32(static_cast<uint32_t>(image.size()));
		u32(0x004E4942u);
		out.write(image.data(), static_cast<std::streamsize>(image.size()));
	}
	const auto materials = SourceMaterials::Open(glb, error);
	CHECK(materials != nullptr);
	if (!materials) {
		std::fprintf(stderr, "source textures: %s\n", error.c_str());
		return;
	}
	const SourceMaterial* material = materials->MaterialForMesh("/glTF/Mesh/0/Primitive/0:__clod_full_mesh__");
	CHECK(material != nullptr && material->baseColorImage == 0 && material->masked && Near(material->alphaCutoff, 0.25, 1e-6) &&
		Near(material->baseColorFactor[0], 0.5, 1e-6));
	CHECK(materials->MaterialForMesh("/glTF/Mesh/0/Primitive/1") == nullptr);
	CHECK(materials->MaterialForMesh("/glTF/Mesh/1/Primitive/0") == nullptr);
	const SourceTexture* texture = materials->Image(0);
	CHECK(texture != nullptr && texture->width == 4u && texture->Levels() == 3u);
	if (texture == nullptr || texture->Levels() != 3u) return;
	// Level 1 keeps the halves apart; level 2 is their mean in linear light.
	CHECK(SampleLinear(*texture, 0.25, 0.5, 1.0)[0] < 0.01f);
	CHECK(SampleLinear(*texture, 0.75, 0.5, 1.0)[0] > 0.99f);
	CHECK(Near(SampleLinear(*texture, 0.5, 0.5, 2.0)[0], 0.5, 0.01));
	CHECK(Near(SampleLinear(*texture, 0.125, 0.5, 0.0)[3], 1.0, 1e-6));
	CHECK(Near(TextureLod(*texture, 0.25), 0.0, 1e-9) && Near(TextureLod(*texture, 1.0), 2.0, 1e-9));
	CHECK(Near(BaseColor(*material, texture, 0.875, 0.5, 0.0)[0], 0.5, 0.01));
	CHECK(Near(DeltaE({ 1.0f, 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 0.0f, 1.0f }), 100.0, 0.1));
	CHECK(DeltaE({ 0.2f, 0.3f, 0.4f, 1.0f }, { 0.2f, 0.3f, 0.4f, 0.0f }) == 0.0);
}

// The same reader on a .gltf whose base colour is a KTX2 file beside it, named through KHR_texture_basisu, with a
// KHR_texture_transform: a 4x4 sRGB RGBA8 KTX2, left half black, right half white, offset by half a texture in u.
void TestSourceTexturesKtx2(const fs::path& root)
{
	std::vector<uint8_t> texels(4u * 4u * 4u, 255u);
	for (uint32_t y = 0; y < 4u; ++y) {
		for (uint32_t x = 0; x < 2u; ++x) {
			for (uint32_t c = 0; c < 3u; ++c) texels[(y * 4u + x) * 4u + c] = 0u;
		}
	}
	ktxTextureCreateInfo info{};
	info.vkFormat = 43u;  // R8G8B8A8_SRGB
	info.baseWidth = 4u;
	info.baseHeight = 4u;
	info.baseDepth = 1u;
	info.numDimensions = 2u;
	info.numLevels = 1u;
	info.numLayers = 1u;
	info.numFaces = 1u;
	info.isArray = KTX_FALSE;
	info.generateMipmaps = KTX_FALSE;
	ktxTexture2* texture = nullptr;
	CHECK(ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture) == KTX_SUCCESS);
	if (texture == nullptr) return;
	CHECK(ktxTexture_SetImageFromMemory(ktxTexture(texture), 0, 0, 0, texels.data(), texels.size()) == KTX_SUCCESS);
	CHECK(ktxTexture2_DeflateZstd(texture, 5) == KTX_SUCCESS);
	const fs::path ktx2 = root / "source-textures-ktx2.ktx2";
	CHECK(ktxTexture_WriteToNamedFile(ktxTexture(texture), ktx2.string().c_str()) == KTX_SUCCESS);
	ktxTexture_Destroy(ktxTexture(texture));
	const fs::path gltf = root / "source-textures-ktx2.gltf";
	{
		std::ofstream out(gltf, std::ios::binary);
		out << R"({"asset":{"version":"2.0"},"images":[{"uri":"source-textures-ktx2.ktx2","mimeType":"image/ktx2"}],)"
			<< R"("textures":[{"extensions":{"KHR_texture_basisu":{"source":0}}}],)"
			<< R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0,"extensions":{"KHR_texture_transform":{"offset":[0.5,0]}}}}}],)"
			<< R"("meshes":[{"primitives":[{"attributes":{},"material":0}]}]})";
	}
	std::string error;
	const auto materials = SourceMaterials::Open(gltf, error);
	CHECK(materials != nullptr);
	if (!materials) {
		std::fprintf(stderr, "source textures (KTX2): %s\n", error.c_str());
		return;
	}
	const SourceMaterial* material = materials->MaterialForMesh("/glTF/Mesh/0/Primitive/0");
	CHECK(material != nullptr && material->baseColorImage == 0 && material->uvTransformed && Near(material->uvFootprintScale, 1.0, 1e-9));
	const SourceTexture* image = materials->Image(0);
	CHECK(image != nullptr && image->width == 4u && image->Levels() == 3u);
	if (material == nullptr || image == nullptr) return;
	CHECK(SampleLinear(*image, 0.125, 0.5, 0.0)[0] < 0.01f && SampleLinear(*image, 0.875, 0.5, 0.0)[0] > 0.99f);
	// The transform moves u by half: what was black reads white.
	CHECK(BaseColor(*material, image, 0.125, 0.5, 0.0)[0] > 0.99f && BaseColor(*material, image, 0.625, 0.5, 0.0)[0] < 0.01f);
}

// Stage 6 step 1, the colour-weighted texture term: on the chart dome (crossings let through), a material whose texture is
// uniform makes the term (nearly) vanish, one with a different colour in every UV chart keeps most of it, and the setting off,
// or on without a material, stores exactly the binary term. Groups and pages never change; rebuilding is identical. Also: an
// appearance hash names its own cache entry and CLodEval's mesh name leaves it out.
void TestTextureColor(const fs::path& root)
{
	const LayoutSource source = MakeChartDome();
	ClusterLODBuilderSettings binary = TopologySettings(true, false);
	binary.simplifyTopologyRelaxUv = true;
	binary.simplifyTopologyTextureArea = 1.0f; // no texture check: let candidates cross charts
	binary.lodErrorChannels = true;
	binary.lodErrorChannelsTexture = 1.0f;
	ClusterLODBuilderSettings plainSettings = binary;
	plainSettings.lodErrorChannelsTexture = 0.0f;
	ClusterLODBuilderSettings color = binary;
	color.lodErrorChannelsTextureColor = 10.0f;

	// 64x64 textures over the dome's 8x8 atlas (each tile's chart fills 0.12 of its 0.125 cell).
	const auto makeTexture = [](bool contrasting) {
		auto texture = std::make_shared<clod_texture::Texture>();
		texture->width = texture->height = 64u;
		std::vector<uint8_t> texels(64u * 64u * 4u, 255u);
		for (uint32_t y = 0; y < 64u; ++y) {
			for (uint32_t x = 0; x < 64u; ++x) {
				uint8_t* t = &texels[(y * 64u + x) * 4u];
				const uint32_t cell = (y / 8u) * 8u + x / 8u;
				if (contrasting) {
					// a saturated colour per chart, so texture from a neighbouring chart is visibly wrong
					t[0] = static_cast<uint8_t>((cell * 97u) % 256u);
					t[1] = static_cast<uint8_t>((cell * 57u + 128u) % 256u);
					t[2] = static_cast<uint8_t>((cell * 23u + 64u) % 256u);
				}
				else {
					t[0] = t[1] = t[2] = 128u;
				}
			}
		}
		texture->mips.push_back(std::move(texels));
		clod_texture::BuildMips(*texture);
		return std::shared_ptr<const clod_texture::Texture>(std::move(texture));
	};
	const auto makeAppearance = [](std::shared_ptr<const clod_texture::Texture> texture) {
		ClusterLODMaterialAppearance appearance;
		appearance.hasBaseColorTexture = true;
		appearance.contentHash = 1u;
		appearance.decodeBaseColor = [texture] { return texture; };
		return appearance;
	};
	const ClusterLODMaterialAppearance uniform = makeAppearance(makeTexture(false));
	const ClusterLODMaterialAppearance contrasting = makeAppearance(makeTexture(true));
	const auto build = [&](const ClusterLODBuilderSettings& settings, const ClusterLODMaterialAppearance* appearance, const std::string& primPath)
		-> std::unique_ptr<CachedMesh> {
		const ClusterLODPrebuildArtifacts artifacts = BuildClusterLODArtifactsFromGeometry(
			source.vertices, source.stride, nullptr, 0u, source.indices, source.uvSets, source.flags, settings, nullptr, appearance);
		CHECK(CLodCacheLoader::SavePrebuilt({ "clod_eval_test.source", primPath, "", false }, artifacts.prebuiltData, artifacts.cacheBuildData.AsPayload()));
		for (const fs::path& path : FindCacheMetadata(root)) {
			std::string error;
			auto mesh = CachedMesh::Open(path, error);
			if (mesh && mesh->Prebuilt().cacheSource.primPath == primPath) return mesh;
		}
		CHECK(false);
		return nullptr;
	};
	const auto plain = build(plainSettings, nullptr, "/texture-color-plain");
	const auto bin = build(binary, &contrasting, "/texture-color-binary");               // setting off, material given: binary
	const auto noMaterial = build(color, nullptr, "/texture-color-no-material");         // setting on, no material: binary
	const auto flat = build(color, &uniform, "/texture-color-uniform");
	const auto sharp = build(color, &contrasting, "/texture-color-contrasting");
	const auto sharpAgain = build(color, &contrasting, "/texture-color-contrasting-again");
	if (!plain || !bin || !noMaterial || !flat || !sharp || !sharpAgain) {
		CHECK(false);
		return;
	}
	const auto nodesEqual = [](const CachedMesh& a, const CachedMesh& b) {
		const auto& na = a.Prebuilt().nodes;
		const auto& nb = b.Prebuilt().nodes;
		return na.size() == nb.size() && std::memcmp(na.data(), nb.data(), na.size() * sizeof(ClusterLODNode)) == 0;
	};
	CHECK(SameBuild(*plain, *bin) && SameBuild(*plain, *flat) && SameBuild(*plain, *sharp));
	CHECK(nodesEqual(*bin, *noMaterial));
	CHECK(SameBuild(*sharp, *sharpAgain) && nodesEqual(*sharp, *sharpAgain));

	// Q_T as stored: the appearance channel squared, over the plain build's (both monotone envelopes, so a sum over groups of
	// what the texture term added).
	const GroupErrorChannels base = ReadGroupErrorChannels(plain->Prebuilt());
	const auto added = [&](const CachedMesh& mesh) {
		const GroupErrorChannels channels = ReadGroupErrorChannels(mesh.Prebuilt());
		double sum = 0.0;
		for (size_t group = 0; group < base.appearance.size() && group < channels.appearance.size(); ++group) {
			const double a = base.appearance[group], b = channels.appearance[group];
			CHECK(b >= a);
			sum += b * b - a * a;
		}
		return sum;
	};
	const double binaryAdded = added(*bin), flatAdded = added(*flat), sharpAdded = added(*sharp);
	std::fprintf(stderr, "texture colour: Q_T added binary %.6g, uniform texture %.6g, contrasting texture %.6g\n", binaryAdded, flatAdded, sharpAdded);
	CHECK(binaryAdded > 0.0);
	CHECK(flatAdded <= 0.05 * binaryAdded);
	CHECK(sharpAdded >= 0.5 * binaryAdded && sharpAdded <= binaryAdded * 1.0001);
	CHECK(sharpAdded > 10.0 * flatAdded);

	// The appearance hash is part of the entry's key (its stored subset) and not of the mesh name CLodEval compares by.
	const ClusterLODPrebuildArtifacts artifacts = BuildClusterLODArtifactsFromGeometry(
		source.vertices, source.stride, nullptr, 0u, source.indices, source.uvSets, source.flags, color, nullptr, &contrasting);
	CLodCacheLoader::MeshCacheIdentity identity{ "clod_eval_test.source", "/texture-color-keyed", "", false };
	identity.appearanceHash = 0x123456789abcdef0ull;
	CHECK(CLodCacheLoader::SavePrebuilt(identity, artifacts.prebuiltData, artifacts.cacheBuildData.AsPayload()));
	CHECK(CLodCacheLoader::TryLoadPrebuilt(identity).has_value());
	CLodCacheLoader::MeshCacheIdentity unkeyed = identity;
	unkeyed.appearanceHash = 0u;
	CHECK(!CLodCacheLoader::TryLoadPrebuilt(unkeyed).has_value());
	for (const fs::path& path : FindCacheMetadata(root)) {
		std::string error;
		auto mesh = CachedMesh::Open(path, error);
		if (mesh && mesh->Prebuilt().cacheSource.primPath == "/texture-color-keyed") {
			CHECK(mesh->Prebuilt().cacheSource.subsetName.find("|appearance=123456789abcdef0") != std::string::npos);
			CHECK(mesh->Name() == "/texture-color-keyed");
		}
	}
}

// Stage 6 step 4: the chart test weighed by colour. Without the chart test or without a material it changes nothing; with a texture
// whose charts all look alike it lets crossings through (about as coarse as no chart test); with charts of contrasting colours it
// keeps them (about as fine as the binary chart test); rebuilding is identical.
void TestTopologyTextureColor(const fs::path& root)
{
	const LayoutSource source = MakeChartDome();
	ClusterLODBuilderSettings relaxUv = TopologySettings(true, false);
	relaxUv.simplifyTopologyRelaxUv = true;
	ClusterLODBuilderSettings charts = relaxUv;
	charts.simplifyTopologyTextureCharts = true;
	ClusterLODBuilderSettings color = charts;
	color.simplifyTopologyTextureColor = 3.0f;
	ClusterLODBuilderSettings colorNoCharts = relaxUv;
	colorNoCharts.simplifyTopologyTextureColor = 3.0f;
	CHECK(!ClusterLODTopologyTextureColorActive(charts) && ClusterLODTopologyTextureColorActive(color) && !ClusterLODTopologyTextureColorActive(colorNoCharts));
	CHECK(ClusterLODMaterialAppearanceWanted(color) && !ClusterLODMaterialAppearanceWanted(charts));

	const auto makeTexture = [](bool contrasting) {
		auto texture = std::make_shared<clod_texture::Texture>();
		texture->width = texture->height = 64u;
		std::vector<uint8_t> texels(64u * 64u * 4u, 255u);
		for (uint32_t y = 0; y < 64u; ++y) {
			for (uint32_t x = 0; x < 64u; ++x) {
				uint8_t* t = &texels[(y * 64u + x) * 4u];
				const uint32_t cell = (y / 8u) * 8u + x / 8u;
				t[0] = contrasting ? static_cast<uint8_t>((cell * 97u) % 256u) : 128u;
				t[1] = contrasting ? static_cast<uint8_t>((cell * 57u + 128u) % 256u) : 128u;
				t[2] = contrasting ? static_cast<uint8_t>((cell * 23u + 64u) % 256u) : 128u;
			}
		}
		texture->mips.push_back(std::move(texels));
		clod_texture::BuildMips(*texture);
		return std::shared_ptr<const clod_texture::Texture>(std::move(texture));
	};
	const auto makeAppearance = [](std::shared_ptr<const clod_texture::Texture> texture) {
		ClusterLODMaterialAppearance appearance;
		appearance.hasBaseColorTexture = true;
		appearance.contentHash = 1u;
		appearance.decodeBaseColor = [texture] { return texture; };
		return appearance;
	};
	const ClusterLODMaterialAppearance uniform = makeAppearance(makeTexture(false));
	const ClusterLODMaterialAppearance contrasting = makeAppearance(makeTexture(true));
	const auto build = [&](const ClusterLODBuilderSettings& settings, const ClusterLODMaterialAppearance* appearance, const std::string& primPath)
		-> std::unique_ptr<CachedMesh> {
		const ClusterLODPrebuildArtifacts artifacts = BuildClusterLODArtifactsFromGeometry(
			source.vertices, source.stride, nullptr, 0u, source.indices, source.uvSets, source.flags, settings, nullptr, appearance);
		CHECK(CLodCacheLoader::SavePrebuilt({ "clod_eval_test.source", primPath, "", false }, artifacts.prebuiltData, artifacts.cacheBuildData.AsPayload()));
		for (const fs::path& path : FindCacheMetadata(root)) {
			std::string error;
			auto mesh = CachedMesh::Open(path, error);
			if (mesh && mesh->Prebuilt().cacheSource.primPath == primPath) return mesh;
		}
		CHECK(false);
		return nullptr;
	};
	const auto slide = build(relaxUv, nullptr, "/topo-color-slide");
	const auto binary = build(charts, &contrasting, "/topo-color-binary");
	const auto noMaterial = build(color, nullptr, "/topo-color-no-material");
	const auto noCharts = build(colorNoCharts, &uniform, "/topo-color-no-charts");
	const auto flat = build(color, &uniform, "/topo-color-uniform");
	const auto sharp = build(color, &contrasting, "/topo-color-contrasting");
	const auto sharpAgain = build(color, &contrasting, "/topo-color-contrasting-again");
	if (!slide || !binary || !noMaterial || !noCharts || !flat || !sharp || !sharpAgain) {
		CHECK(false);
		return;
	}
	CHECK(SameBuild(*binary, *noMaterial));
	CHECK(SameBuild(*slide, *noCharts));
	CHECK(SameBuild(*sharp, *sharpAgain));
	const char* coarsest = "hierarchy.coarsest_triangles";
	const double s = Value(Analyze(slide->MetadataPath(), "topo-color-slide"), coarsest);
	const double b = Value(Analyze(binary->MetadataPath(), "topo-color-binary"), coarsest);
	const double f = Value(Analyze(flat->MetadataPath(), "topo-color-uniform"), coarsest);
	const double c = Value(Analyze(sharp->MetadataPath(), "topo-color-contrasting"), coarsest);
	std::fprintf(stderr, "topology texture colour: coarsest slide-only=%g binary charts=%g uniform=%g contrasting=%g\n", s, b, f, c);
	CHECK(f <= b * 0.8);
	CHECK(f <= s * 1.1);
	CHECK(c >= b * 0.9);
}

// Stage 6 step 2: the UV remap block format on its own. Nodes come back at their grid points (to 16-bit precision), interpolated
// sub-triangles interpolate, the others extrapolate from the node of largest weight along its step codes (or keep its UV without
// them), and malformed blocks are rejected.
void TestUvRemapBlock()
{
	using namespace clod_uv_remap;
	CHECK(NodeIndex(4u, 0u, 0u) == 0u && NodeIndex(4u, 3u, 0u) == 3u && NodeIndex(4u, 0u, 1u) == 4u && NodeIndex(4u, 0u, 3u) == 9u);
	CHECK(SubTriangleIndex(4u, 2u, 0u, false) == 4u && SubTriangleIndex(4u, 0u, 1u, false) == 5u && SubTriangleIndex(4u, 0u, 2u, false) == 8u);
	std::vector<TriangleRemap> triangles(4);
	// Triangle 0: n = 3, UV = (b1, b2) * 2 + (10, 20), every sub-triangle interpolated.
	triangles[0].n = 3u;
	triangles[0].uvs.resize(NodeCount(3u));
	for (uint32_t j = 0; j < 3u; ++j) {
		for (uint32_t i = 0; i + j < 3u; ++i) {
			triangles[0].uvs[NodeIndex(3u, i, j)] = { 10.0f + i * 1.0f, 20.0f + j * 1.0f };
		}
	}
	triangles[0].interpolate.assign(SubTriangleCount(3u), 1u);
	// Triangle 1: not remapped. Triangle 2: n = 2, one sub-triangle, not interpolated.
	triangles[2].n = 2u;
	triangles[2].uvs = { { 0.0f, 0.0f }, { 5.0f, 0.0f }, { 0.0f, 5.0f } };
	triangles[2].interpolate.assign(SubTriangleCount(2u), 0u);
	// Triangle 3: triangle 0's linear UVs, no sub-triangle interpolated, every node with step codes: extrapolation reproduces them.
	triangles[3] = triangles[0];
	triangles[3].interpolate.assign(SubTriangleCount(3u), 0u);
	triangles[3].steps.assign(NodeCount(3u), 0u);
	for (uint32_t j = 0; j < 3u; ++j) {
		for (uint32_t i = 0; i + j < 3u; ++i) {
			const uint32_t alongI = i + j + 1u < 3u ? kStepNext : (i > 0u ? kStepPrevious : kStepNone);
			const uint32_t alongJ = i + j + 1u < 3u ? kStepNext : (j > 0u ? kStepPrevious : kStepNone);
			triangles[3].steps[NodeIndex(3u, i, j)] = static_cast<uint8_t>(alongI | (alongJ << 2u));
		}
	}
	const std::vector<std::byte> block = EncodeBlock(triangles, 0.5f);
	CHECK(!block.empty() && BlockSize(block) == block.size());
	CHECK(ValidateBlock(block, 4u));
	CHECK(!ValidateBlock(block, 3u));
	CHECK(!ValidateBlock(std::span<const std::byte>(block.data(), block.size() - 4u), 4u));
	std::vector<std::byte> damaged = block;
	damaged[0] = std::byte{ 0 };
	CHECK(!ValidateBlock(damaged, 4u));

	const double tolerance = 12.0 / 65535.0 + 1.0e-5; // the block's UV range over 16 bits
	for (uint32_t j = 0; j < 3u; ++j) {
		for (uint32_t i = 0; i + j < 3u; ++i) {
			const Sample sample = Evaluate(block, 0u, i / 2.0f, j / 2.0f);
			CHECK(sample.remapped && sample.interpolated && sample.n == 3u);
			CHECK(Near(sample.uv[0], 10.0 + i, tolerance) && Near(sample.uv[1], 20.0 + j, tolerance));
		}
	}
	const Sample middle = Evaluate(block, 0u, 0.3f, 0.45f); // linear inside: (10.6, 20.9), d/db1 (2, 0), d/db2 (0, 2)
	CHECK(Near(middle.uv[0], 10.6, 1.0e-3) && Near(middle.uv[1], 20.9, 1.0e-3));
	CHECK(Near(middle.dUvDb1[0], 2.0, 1.0e-3) && Near(middle.dUvDb1[1], 0.0, 1.0e-3) && Near(middle.dUvDb2[1], 2.0, 1.0e-3));
	const Sample upper = Evaluate(block, 0u, 0.4f, 0.4f); // the upper sub-triangle of cell (0, 0)
	CHECK(upper.interpolated && Near(upper.uv[0], 10.8, 1.0e-3) && Near(upper.uv[1], 20.8, 1.0e-3));
	CHECK(!Evaluate(block, 1u, 0.3f, 0.3f).remapped);
	CHECK(!Evaluate(block, 4u, 0.3f, 0.3f).remapped);
	const Sample extrapolated = Evaluate(block, 3u, 0.3f, 0.45f);
	CHECK(extrapolated.remapped && !extrapolated.interpolated);
	CHECK(Near(extrapolated.uv[0], 10.6, 1.0e-3) && Near(extrapolated.uv[1], 20.9, 1.0e-3));
	CHECK(Near(extrapolated.dUvDb1[0], 2.0, 1.0e-3) && Near(extrapolated.dUvDb1[1], 0.0, 1.0e-3) && Near(extrapolated.dUvDb2[1], 2.0, 1.0e-3));
	// Nearest node (2, 0) steps back along i and has no step along j, so v keeps its value and its footprint is a grid step.
	const Sample extrapolatedEdge = Evaluate(block, 3u, 0.9f, 0.05f);
	CHECK(Near(extrapolatedEdge.uv[0], 11.8, 1.0e-3) && Near(extrapolatedEdge.uv[1], 20.0, 1.0e-3));
	CHECK(Near(extrapolatedEdge.dUvDb2[0], 0.0, 1.0e-6) && Near(extrapolatedEdge.dUvDb2[1], 0.5 * 2.0, 1.0e-6));
	const Sample nearest = Evaluate(block, 2u, 0.6f, 0.1f); // largest weight: corner 1
	CHECK(nearest.remapped && !nearest.interpolated && Near(nearest.uv[0], 5.0, 1.0e-3) && Near(nearest.uv[1], 0.0, 1.0e-3));
	CHECK(Near(nearest.dUvDb1[0], 0.5, 1.0e-6) && Near(nearest.dUvDb2[1], 0.5, 1.0e-6));
	const Sample clamped = Evaluate(block, 0u, 0.7f, 0.7f); // outside the triangle by rounding: still a value
	CHECK(clamped.remapped && std::isfinite(clamped.uv[0]) && std::isfinite(clamped.uv[1]));
	std::vector<TriangleRemap> none(4);
	CHECK(EncodeBlock(none, 1.0f).empty());
}

// Stage 6 step 2: UV remap blocks on the chart dome with crossings let through. Unset, the build is exactly the texture term's
// (zero remap counts in the cache's one schema); set, groups and segments are unchanged, damaged clusters carry blocks in their pages
// that decode and round-trip through the cache (in the same schema), the stored texture damage of remapped groups falls, a masked
// material gets none, and rebuilding is identical.
void TestTextureRemap(const fs::path& root)
{
	const LayoutSource source = MakeChartDome();
	ClusterLODBuilderSettings binary = TopologySettings(true, false);
	binary.simplifyTopologyRelaxUv = true;
	binary.simplifyTopologyTextureArea = 1.0f; // no texture check: let candidates cross charts
	binary.lodErrorChannels = true;
	binary.lodErrorChannelsTexture = 1.0f;
	ClusterLODBuilderSettings remap = binary;
	remap.lodErrorChannelsTextureRemap = 0.01f;
	CHECK(!ClusterLODUvRemapActive(binary) && ClusterLODUvRemapActive(remap) && ClusterLODMaterialAppearanceWanted(remap));

	auto texture = std::make_shared<clod_texture::Texture>();
	texture->width = texture->height = 64u;
	std::vector<uint8_t> texels(64u * 64u * 4u, 200u);
	for (uint32_t y = 0; y < 64u; ++y) {
		for (uint32_t x = 0; x < 64u; ++x) {
			texels[(y * 64u + x) * 4u + 0u] = static_cast<uint8_t>(((y / 8u) * 8u + x / 8u) * 3u);
		}
	}
	texture->mips.push_back(std::move(texels));
	clod_texture::BuildMips(*texture);
	ClusterLODMaterialAppearance opaque;
	opaque.hasBaseColorTexture = true;
	opaque.contentHash = 7u;
	opaque.decodeBaseColor = [texture] { return std::shared_ptr<const clod_texture::Texture>(texture); };
	ClusterLODMaterialAppearance masked = opaque;
	masked.alphaMasked = true;

	const auto build = [&](const ClusterLODBuilderSettings& settings, const ClusterLODMaterialAppearance* appearance, const std::string& primPath)
		-> std::unique_ptr<CachedMesh> {
		const ClusterLODPrebuildArtifacts artifacts = BuildClusterLODArtifactsFromGeometry(
			source.vertices, source.stride, nullptr, 0u, source.indices, source.uvSets, source.flags, settings, nullptr, appearance);
		CHECK(CLodCacheLoader::SavePrebuilt({ "clod_eval_test.source", primPath, "", false }, artifacts.prebuiltData, artifacts.cacheBuildData.AsPayload()));
		for (const fs::path& path : FindCacheMetadata(root)) {
			std::string error;
			auto mesh = CachedMesh::Open(path, error);
			if (mesh && mesh->Prebuilt().cacheSource.primPath == primPath) return mesh;
		}
		CHECK(false);
		return nullptr;
	};
	const auto plain = build(binary, &opaque, "/texture-remap-off");
	const auto on = build(remap, &opaque, "/texture-remap-on");
	const auto again = build(remap, &opaque, "/texture-remap-on-again");
	const auto maskedBuild = build(remap, &masked, "/texture-remap-masked");
	const auto noMaterial = build(remap, nullptr, "/texture-remap-no-material");
	if (!plain || !on || !again || !maskedBuild || !noMaterial) {
		CHECK(false);
		return;
	}
	const auto nodesEqual = [](const CachedMesh& a, const CachedMesh& b) {
		const auto& na = a.Prebuilt().nodes;
		const auto& nb = b.Prebuilt().nodes;
		return na.size() == nb.size() && std::memcmp(na.data(), nb.data(), na.size() * sizeof(ClusterLODNode)) == 0;
	};
	const auto schemaOf = [&](const std::string& primPath) {
		for (const fs::path& path : FindCacheMetadata(root)) {
			const CLodCache::OpenedMetadata opened = CLodCache::OpenMetadataFile(path);
			if (opened.status == CLodCache::OpenMetadataStatus::Ok && opened.data.prebuiltData.cacheSource.primPath == primPath) {
				return opened.foundSchemaVersion;
			}
		}
		return 0u;
	};

	// Off, masked and without a material: no remap blocks (zero counts in the one schema), the texture term's build.
	CHECK(plain->Prebuilt().uvRemapClusters == 0u && plain->Prebuilt().uvRemapBytes == 0u && schemaOf("/texture-remap-off") == CLodCache::kSchemaVersion);
	CHECK(maskedBuild->Prebuilt().uvRemapClusters == 0u && SameBuild(*plain, *maskedBuild) && nodesEqual(*plain, *maskedBuild));
	CHECK(noMaterial->Prebuilt().uvRemapClusters == 0u && SameBuild(*plain, *noMaterial) && nodesEqual(*plain, *noMaterial));

	// On: the same groups and segments (by refinement), blocks in the pages, the same schema.
	const ClusterLODPrebuiltData& data = on->Prebuilt();
	CHECK(data.uvRemapClusters > 0u && data.uvRemapBytes > 0u);
	CHECK(schemaOf("/texture-remap-on") == CLodCache::kSchemaVersion);
	CHECK(data.groups.size() == plain->Prebuilt().groups.size() && data.segments.size() == plain->Prebuilt().segments.size());
	for (size_t segment = 0; segment < data.segments.size() && segment < plain->Prebuilt().segments.size(); ++segment) {
		CHECK(data.segments[segment].refinedGroup == plain->Prebuilt().segments[segment].refinedGroup &&
			data.segments[segment].meshletCount == plain->Prebuilt().segments[segment].meshletCount);
	}
	uint32_t blocks = 0, remappedTriangles = 0, movedSamples = 0;
	uint64_t blockBytes = 0;
	for (uint32_t pageIndex = 0; pageIndex < on->PageCount(); ++pageIndex) {
		CLodCache::TrianglePage page;
		std::string error;
		if (!CLodCache::TrianglePage::Parse(on->Page(pageIndex), page, &error)) {
			std::fprintf(stderr, "texture remap: page %u: %s\n", pageIndex, error.c_str());
			CHECK(false);
			continue;
		}
		for (uint32_t meshletIndex = 0; meshletIndex < page.MeshletCount(); ++meshletIndex) {
			const std::span<const std::byte> block = page.UvRemapBlock(meshletIndex);
			if (block.empty()) continue;
			CHECK(page.HasUvRemap());
			const CLodCache::TriangleMeshlet meshlet = page.Meshlet(meshletIndex);
			CHECK(meshlet.refinedGroup >= 0); // only simplified clusters
			CHECK(clod_uv_remap::ValidateBlock(block, meshlet.triangleCount));
			++blocks;
			blockBytes += block.size();
			for (uint32_t triangle = 0; triangle < meshlet.triangleCount; ++triangle) {
				const clod_uv_remap::Sample sample = clod_uv_remap::Evaluate(block, triangle, 1.0f / 3.0f, 1.0f / 3.0f);
				if (!sample.remapped) continue;
				++remappedTriangles;
				const auto corners = page.Triangle(meshlet, triangle);
				float linear[2] = {};
				for (int corner = 0; corner < 3; ++corner) {
					const DirectX::XMFLOAT2 uv = page.Uv(meshletIndex, meshlet, 0u, corners[corner]);
					linear[0] += uv.x / 3.0f;
					linear[1] += uv.y / 3.0f;
				}
				movedSamples += std::hypot(sample.uv[0] - linear[0], sample.uv[1] - linear[1]) > 1.0e-3f ? 1u : 0u;
			}
		}
	}
	std::fprintf(stderr, "texture remap: clusters %u (%u stored), triangles %u, centroids moved %u, bytes %llu\n", blocks, data.uvRemapClusters,
		remappedTriangles, movedSamples, static_cast<unsigned long long>(blockBytes));
	CHECK(blocks == data.uvRemapClusters && blockBytes == data.uvRemapBytes);
	CHECK(remappedTriangles > 0u && movedSamples > 0u);

	// Stored texture damage: the remapped build's appearance channel never above the texture term's, and below it somewhere.
	const GroupErrorChannels before = ReadGroupErrorChannels(plain->Prebuilt());
	const GroupErrorChannels after = ReadGroupErrorChannels(data);
	double sumBefore = 0.0, sumAfter = 0.0;
	for (size_t group = 0; group < before.appearance.size() && group < after.appearance.size(); ++group) {
		CHECK(after.appearance[group] <= before.appearance[group] * 1.0001f + 1.0e-7f);
		sumBefore += static_cast<double>(before.appearance[group]) * before.appearance[group];
		sumAfter += static_cast<double>(after.appearance[group]) * after.appearance[group];
	}
	std::fprintf(stderr, "texture remap: appearance squared summed %.6g -> %.6g\n", sumBefore, sumAfter);
	CHECK(sumAfter < sumBefore);

	// Deterministic.
	CHECK(SameBuild(*on, *again) && nodesEqual(*on, *again));
}

// Stage 6 step 2 refinement: region triangles (layout 2) by hand. A triangle split by a line into two charts, each with its own
// exact affine map; a one-region triangle without cells; a grid triangle beside them in the same block; the version gate.
void TestUvRemapRegions()
{
	using namespace clod_uv_remap;
	std::vector<TriangleRemap> triangles(3);
	// Triangle 0: n = 2 (one cell). Region 0: uv = (b1, b2) * 2 + (10, 20); region 1: uv = (b1, b2) * (-3) + (40, 50). The cell's
	// line is b1 = 0.5 (theta 0, offset 0): b1 > 0.5 reads region 1.
	triangles[0].n = 2u;
	RegionMap first, second;
	first.origin[0] = 10.0f; first.origin[1] = 20.0f;
	first.dUvDb1[0] = 2.0f; first.dUvDb2[1] = 2.0f;
	second.origin[0] = 40.0f; second.origin[1] = 50.0f;
	second.dUvDb1[0] = -3.0f; second.dUvDb2[1] = -3.0f;
	triangles[0].regions = { first, second };
	triangles[0].cells = { EncodeCell(0u, 1u, 0u, 2048u) };
	// Triangle 1: the node grid (layout 1's), n = 2, interpolated.
	triangles[1].n = 2u;
	triangles[1].uvs = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f } };
	triangles[1].interpolate.assign(SubTriangleCount(2u), 1u);
	// Triangle 2: one region, n = 5 (no cells stored).
	triangles[2].n = 5u;
	triangles[2].regions = { second };
	const std::vector<std::byte> block = EncodeBlock(triangles, 0.25f);
	const auto blockVersion = [](std::span<const std::byte> bytes) {
		uint32_t word = 0u;
		std::memcpy(&word, bytes.data() + 4u, sizeof(word));
		return word >> 16u;
	};
	CHECK(!block.empty() && blockVersion(block) == kVersion && BlockSize(block) == block.size());
	CHECK(block.size() == (kHeaderWords + 3u + RegionTriangleWords(2u, 2u) + TriangleWords(2u) + RegionTriangleWords(5u, 1u)) * 4u);
	CHECK(ValidateBlock(block, 3u));
	CHECK(!ValidateBlock(std::span<const std::byte>(block.data(), block.size() - 4u), 3u));

	const Sample left = Evaluate(block, 0u, 0.2f, 0.3f);
	CHECK(left.remapped && left.interpolated && left.region && left.regionIndex == 0u);
	CHECK(Near(left.uv[0], 10.4, 1.0e-5) && Near(left.uv[1], 20.6, 1.0e-5) && Near(left.dUvDb1[0], 2.0, 1.0e-6) && Near(left.dUvDb2[1], 2.0, 1.0e-6));
	const Sample right = Evaluate(block, 0u, 0.7f, 0.1f);
	CHECK(right.region && right.regionIndex == 1u && Near(right.uv[0], 37.9, 1.0e-4) && Near(right.uv[1], 49.7, 1.0e-4));
	CHECK(Near(right.dUvDb1[0], -3.0, 1.0e-6) && Near(right.dUvDb2[1], -3.0, 1.0e-6));
	// The map continues past the triangle's edge (barycentrics are not clamped for it).
	const Sample past = Evaluate(block, 0u, -0.1f, 0.5f);
	CHECK(past.region && past.regionIndex == 0u && Near(past.uv[0], 9.8, 1.0e-5));
	const Sample grid = Evaluate(block, 1u, 0.25f, 0.5f);
	CHECK(grid.remapped && !grid.region && Near(grid.uv[0], 0.25, 1.0e-4) && Near(grid.uv[1], 0.5, 1.0e-4));
	const Sample single = Evaluate(block, 2u, 0.6f, 0.3f);
	CHECK(single.region && single.regionIndex == 0u && Near(single.uv[0], 38.2, 1.0e-4) && Near(single.uv[1], 49.1, 1.0e-4));

	// Cells: the line's sides at every angle and offset agree with CellLineDistance; a == b ignores the line.
	CHECK(CellRegion(EncodeCell(3u, 3u, 77u, 9u), 0.9f, 0.9f) == 3u);
	CHECK(CellRegion(EncodeCell(2u, 5u, 64u, 2048u), 0.5f, 0.9f) == 5u && CellRegion(EncodeCell(2u, 5u, 64u, 2048u), 0.5f, 0.1f) == 2u);
	CHECK(CellRegion(EncodeCell(2u, 5u, 0u, 4095u), 0.99f, 0.5f) == 2u); // offset 0.75 from the centre: past the cell

	// A cell naming a region the triangle does not have, and any other block version (the retired layouts 1 and 2, the next one)
	// are rejected.
	std::vector<std::byte> bad = block;
	const uint32_t cellWord = kHeaderWords + 3u + 1u + 2u * kRegionWords;
	const uint32_t badCell = EncodeCell(0u, 2u, 0u, 2048u);
	std::memcpy(bad.data() + cellWord * 4u, &badCell, 4u);
	CHECK(!ValidateBlock(bad, 3u));
	for (const uint32_t otherVersion : { 1u, 2u, kVersion + 1u }) {
		std::vector<std::byte> other = block;
		const uint32_t word = 3u | (otherVersion << 16u);
		std::memcpy(other.data() + 4u, &word, 4u);
		CHECK(!ValidateBlock(other, 3u));
	}
	// Grid triangles alone encode in the same layout, and so do region triangles alone.
	std::vector<TriangleRemap> gridOnly(1, triangles[1]);
	std::vector<TriangleRemap> regionsOnly(1, triangles[0]);
	CHECK(blockVersion(EncodeBlock(gridOnly, 0.25f)) == kVersion && ValidateBlock(EncodeBlock(gridOnly, 0.25f), 1u));
	CHECK(blockVersion(EncodeBlock(regionsOnly, 0.25f)) == kVersion && ValidateBlock(EncodeBlock(regionsOnly, 0.25f), 1u));

	// Half-word cells decode exactly as the full cells they stand for, and take half the words.
	TriangleRemap fine;
	fine.n = 9u;
	fine.regions = { first, second };
	for (uint32_t c = 0; c < SubTriangleCount(9u); ++c) {
		fine.cells.push_back(EncodeCell(c % 2u, (c + 1u) % 2u, (c * 16u) % 256u, ((c * 5u) % 16u) * 273u));
	}
	TriangleRemap fineHalf = fine;
	fineHalf.halfCells = true;
	CHECK(IsHalfCell(fine.cells[7]) && ExpandHalfCell(PackHalfCell(fine.cells[7])) == fine.cells[7] && !IsHalfCell(EncodeCell(0u, 1u, 3u, 0u)));
	const std::vector<std::byte> full = EncodeBlock(std::vector<TriangleRemap>{ fine }, 0.25f);
	const std::vector<std::byte> halved = EncodeBlock(std::vector<TriangleRemap>{ fineHalf }, 0.25f);
	CHECK(full.size() == (kHeaderWords + 1u + RegionTriangleWords(9u, 2u, false)) * 4u && halved.size() == (kHeaderWords + 1u + RegionTriangleWords(9u, 2u, true)) * 4u);
	CHECK(ValidateBlock(full, 1u) && ValidateBlock(halved, 1u));
	uint32_t agree = 0u, total = 0u;
	for (float b2 = 0.01f; b2 < 1.0f; b2 += 0.037f) {
		for (float b1 = 0.01f; b1 + b2 < 1.0f; b1 += 0.041f) {
			const Sample x = Evaluate(full, 0u, b1, b2), y = Evaluate(halved, 0u, b1, b2);
			agree += x.regionIndex == y.regionIndex && x.uv[0] == y.uv[0] && x.uv[1] == y.uv[1] ? 1u : 0u;
			++total;
		}
	}
	CHECK(total > 100u && agree == total);
	// A half-cell triangle with more than 16 regions is malformed.
	std::vector<std::byte> tooMany = halved;
	const uint32_t seventeen = 17u | kRegionHalfCells;
	std::memcpy(tooMany.data() + (kHeaderWords + 1u) * 4u, &seventeen, 4u);
	CHECK(!ValidateBlock(tooMany, 1u));
}

// Stage 6 step 2 refinement: the region layout (BASICRENDERER_CLOD_ERROR_CHANNELS_TEXTURE_REMAP_LAYOUT=2) on the chart dome with
// crossings let through. The same groups, segments and nodes as layout 1, region triangles in the one block layout, which decode exactly
// to the original's UV where they are seen along the normal, appearance no higher than the texture term's, deterministic, and beside
// normal spread blocks in the same pages.
void TestTextureRemapRegions(const fs::path& root)
{
	const LayoutSource source = MakeChartDome();
	ClusterLODBuilderSettings binary = TopologySettings(true, false);
	binary.simplifyTopologyRelaxUv = true;
	binary.simplifyTopologyTextureArea = 1.0f;
	binary.lodErrorChannels = true;
	binary.lodErrorChannelsTexture = 1.0f;
	ClusterLODBuilderSettings grid = binary;
	grid.lodErrorChannelsTextureRemap = 0.01f;
	ClusterLODBuilderSettings regions = grid;
	regions.lodErrorChannelsTextureRemapLayout = 2u;
	ClusterLODBuilderSettings regionsSpread = regions;
	regionsSpread.lodNormalSpread = true;

	auto texture = std::make_shared<clod_texture::Texture>();
	texture->width = texture->height = 64u;
	std::vector<uint8_t> texels(64u * 64u * 4u, 200u);
	for (uint32_t y = 0; y < 64u; ++y) {
		for (uint32_t x = 0; x < 64u; ++x) {
			texels[(y * 64u + x) * 4u + 0u] = static_cast<uint8_t>(((y / 8u) * 8u + x / 8u) * 3u);
		}
	}
	texture->mips.push_back(std::move(texels));
	clod_texture::BuildMips(*texture);
	ClusterLODMaterialAppearance opaque;
	opaque.hasBaseColorTexture = true;
	opaque.contentHash = 7u;
	opaque.decodeBaseColor = [texture] { return std::shared_ptr<const clod_texture::Texture>(texture); };

	const auto build = [&](const ClusterLODBuilderSettings& settings, const std::string& primPath) -> std::unique_ptr<CachedMesh> {
		const ClusterLODPrebuildArtifacts artifacts = BuildClusterLODArtifactsFromGeometry(
			source.vertices, source.stride, nullptr, 0u, source.indices, source.uvSets, source.flags, settings, nullptr, &opaque);
		CHECK(CLodCacheLoader::SavePrebuilt({ "clod_eval_test.source", primPath, "", false }, artifacts.prebuiltData, artifacts.cacheBuildData.AsPayload()));
		for (const fs::path& path : FindCacheMetadata(root)) {
			std::string error;
			auto mesh = CachedMesh::Open(path, error);
			if (mesh && mesh->Prebuilt().cacheSource.primPath == primPath) return mesh;
		}
		CHECK(false);
		return nullptr;
	};
	const auto plain = build(binary, "/remap-regions-off");
	const auto onGrid = build(grid, "/remap-regions-grid");
	const auto on = build(regions, "/remap-regions-on");
	const auto again = build(regions, "/remap-regions-again");
	const auto spread = build(regionsSpread, "/remap-regions-spread");
	if (!plain || !onGrid || !on || !again || !spread) {
		CHECK(false);
		return;
	}
	const auto nodesEqual = [](const CachedMesh& a, const CachedMesh& b) {
		const auto& na = a.Prebuilt().nodes;
		const auto& nb = b.Prebuilt().nodes;
		return na.size() == nb.size() && std::memcmp(na.data(), nb.data(), na.size() * sizeof(ClusterLODNode)) == 0;
	};
	const auto schemaOf = [&](const std::string& primPath) {
		for (const fs::path& path : FindCacheMetadata(root)) {
			const CLodCache::OpenedMetadata opened = CLodCache::OpenMetadataFile(path);
			if (opened.status == CLodCache::OpenMetadataStatus::Ok && opened.data.prebuiltData.cacheSource.primPath == primPath) {
				return opened.foundSchemaVersion;
			}
		}
		return 0u;
	};
	const ClusterLODPrebuiltData& data = on->Prebuilt();
	CHECK(onGrid->Prebuilt().uvRemapClusters > 0u && data.uvRemapClusters > 0u);
	CHECK(schemaOf("/remap-regions-on") == CLodCache::kSchemaVersion && schemaOf("/remap-regions-grid") == CLodCache::kSchemaVersion);
	CHECK(data.groups.size() == plain->Prebuilt().groups.size() && data.segments.size() == plain->Prebuilt().segments.size());
	for (size_t segment = 0; segment < data.segments.size() && segment < plain->Prebuilt().segments.size(); ++segment) {
		CHECK(data.segments[segment].refinedGroup == plain->Prebuilt().segments[segment].refinedGroup &&
			data.segments[segment].meshletCount == plain->Prebuilt().segments[segment].meshletCount);
	}

	// Every block validates; region samples read the original's UV at the frontmost crossing along the normal (a dome: one sheet).
	std::vector<std::array<DirectX::XMFLOAT3, 3>> leafTriangles;
	std::vector<std::array<DirectX::XMFLOAT2, 3>> leafUvs;
	for (size_t t = 0; t + 2u < source.indices.size(); t += 3u) {
		std::array<DirectX::XMFLOAT3, 3> positions;
		std::array<DirectX::XMFLOAT2, 3> uvs;
		for (int corner = 0; corner < 3; ++corner) {
			const uint32_t vertex = source.indices[t + corner];
			std::memcpy(&positions[corner], source.vertices.data() + static_cast<size_t>(vertex) * source.stride, sizeof(DirectX::XMFLOAT3));
			if (!source.uvSets.empty()) {
				uvs[corner] = source.uvSets[0].values[vertex];
			}
			else {
				std::memcpy(&uvs[corner], source.vertices.data() + static_cast<size_t>(vertex) * source.stride + MeshVertexLayout::TexcoordOffset(source.flags),
					sizeof(DirectX::XMFLOAT2));
			}
		}
		leafTriangles.push_back(positions);
		leafUvs.push_back(uvs);
	}
	uint32_t blocks = 0, regionTriangles = 0, regionSamples = 0, exactSamples = 0;
	uint64_t blockBytes = 0;
	for (uint32_t pageIndex = 0; pageIndex < on->PageCount(); ++pageIndex) {
		CLodCache::TrianglePage page;
		std::string error;
		if (!CLodCache::TrianglePage::Parse(on->Page(pageIndex), page, &error)) {
			std::fprintf(stderr, "remap regions: page %u: %s\n", pageIndex, error.c_str());
			CHECK(false);
			continue;
		}
		for (uint32_t meshletIndex = 0; meshletIndex < page.MeshletCount(); ++meshletIndex) {
			const std::span<const std::byte> block = page.UvRemapBlock(meshletIndex);
			if (block.empty()) continue;
			const CLodCache::TriangleMeshlet meshlet = page.Meshlet(meshletIndex);
			CHECK(meshlet.refinedGroup >= 0 && clod_uv_remap::ValidateBlock(block, meshlet.triangleCount));
			++blocks;
			blockBytes += block.size();
			for (uint32_t triangle = 0; triangle < meshlet.triangleCount; ++triangle) {
				if (!clod_uv_remap::Evaluate(block, triangle, 0.25f, 0.25f).region) continue;
				++regionTriangles;
				const auto corners = page.Triangle(meshlet, triangle);
				DirectX::XMFLOAT3 p[3];
				for (int corner = 0; corner < 3; ++corner) p[corner] = page.Position(meshlet, corners[corner]);
				const DirectX::XMVECTOR a = DirectX::XMLoadFloat3(&p[0]), b = DirectX::XMLoadFloat3(&p[1]), c = DirectX::XMLoadFloat3(&p[2]);
				const DirectX::XMVECTOR n = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(DirectX::XMVectorSubtract(b, a), DirectX::XMVectorSubtract(c, a)));
				for (const auto& w : { std::array<float, 2>{ 0.2f, 0.2f }, std::array<float, 2>{ 0.6f, 0.2f }, std::array<float, 2>{ 0.2f, 0.6f } }) {
					const clod_uv_remap::Sample sample = clod_uv_remap::Evaluate(block, triangle, w[0], w[1]);
					const DirectX::XMVECTOR point = DirectX::XMVectorAdd(DirectX::XMVectorScale(a, 1.0f - w[0] - w[1]),
						DirectX::XMVectorAdd(DirectX::XMVectorScale(b, w[0]), DirectX::XMVectorScale(c, w[1])));
					// The frontmost leaf crossing along n, by brute force.
					float bestT = -std::numeric_limits<float>::max();
					float bestUv[2] = {};
					bool found = false;
					for (size_t t = 0; t < leafTriangles.size(); ++t) {
						const DirectX::XMVECTOR la = DirectX::XMLoadFloat3(&leafTriangles[t][0]);
						const DirectX::XMVECTOR lb = DirectX::XMLoadFloat3(&leafTriangles[t][1]);
						const DirectX::XMVECTOR lc = DirectX::XMLoadFloat3(&leafTriangles[t][2]);
						const DirectX::XMVECTOR ln = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(DirectX::XMVectorSubtract(lb, la), DirectX::XMVectorSubtract(lc, la)));
						const float facing = DirectX::XMVectorGetX(DirectX::XMVector3Dot(ln, n));
						if (facing < 0.5f) continue;
						const float tt = DirectX::XMVectorGetX(DirectX::XMVector3Dot(DirectX::XMVectorSubtract(la, point), ln)) / facing;
						const DirectX::XMVECTOR q = DirectX::XMVectorAdd(point, DirectX::XMVectorScale(n, tt));
						const DirectX::XMVECTOR e0 = DirectX::XMVectorSubtract(lb, la), e1 = DirectX::XMVectorSubtract(lc, la), e2 = DirectX::XMVectorSubtract(q, la);
						const float d00 = DirectX::XMVectorGetX(DirectX::XMVector3Dot(e0, e0)), d01 = DirectX::XMVectorGetX(DirectX::XMVector3Dot(e0, e1));
						const float d11 = DirectX::XMVectorGetX(DirectX::XMVector3Dot(e1, e1)), d20 = DirectX::XMVectorGetX(DirectX::XMVector3Dot(e2, e0));
						const float d21 = DirectX::XMVectorGetX(DirectX::XMVector3Dot(e2, e1));
						const float denom = d00 * d11 - d01 * d01;
						const float v = (d11 * d20 - d01 * d21) / denom, u = (d00 * d21 - d01 * d20) / denom;
						if (std::min({ 1.0f - u - v, u, v }) < -1.0e-4f || tt <= bestT || std::fabs(tt) > 1.0f) continue;
						bestT = tt;
						found = true;
						for (int axis = 0; axis < 2; ++axis) {
							const float* uv0 = &leafUvs[t][0].x;
							const float* uv1 = &leafUvs[t][1].x;
							const float* uv2 = &leafUvs[t][2].x;
							bestUv[axis] = uv0[axis] * (1.0f - u - v) + uv1[axis] * v + uv2[axis] * u;
						}
					}
					if (!found) continue;
					++regionSamples;
					// Within a texel of the 64-texel test texture (pieces merge within a quarter of the group error, which on this coarse
					// dome can exceed a hundredth of a UV unit).
					exactSamples += std::hypot(sample.uv[0] - bestUv[0], sample.uv[1] - bestUv[1]) < 1.0f / 64.0f ? 1u : 0u;
				}
			}
		}
	}
	std::fprintf(stderr, "remap regions: clusters %u (%u stored), region triangles %u, samples within a texel %u of %u, bytes %llu (grid layout %llu)\n", blocks,
		data.uvRemapClusters, regionTriangles, exactSamples, regionSamples, static_cast<unsigned long long>(blockBytes),
		static_cast<unsigned long long>(onGrid->Prebuilt().uvRemapBytes));
	CHECK(blocks == data.uvRemapClusters && blockBytes == data.uvRemapBytes);
	CHECK(regionTriangles > 0u && regionSamples > 0u && exactSamples >= regionSamples * 9u / 10u);

	// Appearance never above the texture term's, lower somewhere.
	const GroupErrorChannels before = ReadGroupErrorChannels(plain->Prebuilt());
	const GroupErrorChannels after = ReadGroupErrorChannels(data);
	double sumBefore = 0.0, sumAfter = 0.0;
	for (size_t group = 0; group < before.appearance.size() && group < after.appearance.size(); ++group) {
		CHECK(after.appearance[group] <= before.appearance[group] * 1.0001f + 1.0e-7f);
		sumBefore += static_cast<double>(before.appearance[group]) * before.appearance[group];
		sumAfter += static_cast<double>(after.appearance[group]) * after.appearance[group];
	}
	std::fprintf(stderr, "remap regions: appearance squared summed %.6g -> %.6g\n", sumBefore, sumAfter);
	CHECK(sumAfter < sumBefore);

	// Every block is in the one layout; a page whose block claims a retired layout (2) does not parse rather than draw.
	{
		bool patched = false;
		for (uint32_t pageIndex = 0; pageIndex < on->PageCount() && !patched; ++pageIndex) {
			CLodCache::TrianglePage page;
			if (!CLodCache::TrianglePage::Parse(on->Page(pageIndex), page, nullptr)) continue;
			for (uint32_t meshletIndex = 0; meshletIndex < page.MeshletCount() && !patched; ++meshletIndex) {
				const std::span<const std::byte> block = page.UvRemapBlock(meshletIndex);
				if (block.empty()) continue;
				uint32_t word = 0u;
				std::memcpy(&word, block.data() + 4u, sizeof(word));
				CHECK((word >> 16u) == clod_uv_remap::kVersion);
				const std::span<const std::byte> original = on->Page(pageIndex);
				std::vector<std::byte> bytes(original.begin(), original.end());
				word = (word & 0xFFFFu) | (2u << 16u);
				std::memcpy(bytes.data() + (block.data() - original.data()) + 4u, &word, sizeof(word));
				CLodCache::TrianglePage stale;
				CHECK(!CLodCache::TrianglePage::Parse(bytes, stale, nullptr));
				patched = true;
			}
		}
		CHECK(patched);
	}

	// Deterministic, and beside normal spread blocks (the same schema, both counts, the same nodes).
	CHECK(SameBuild(*on, *again) && nodesEqual(*on, *again));
	CHECK(spread->Prebuilt().uvRemapClusters == data.uvRemapClusters && spread->Prebuilt().normalSpreadClusters > 0u);
	CHECK(schemaOf("/remap-regions-spread") == CLodCache::kSchemaVersion && nodesEqual(*on, *spread));
	for (uint32_t pageIndex = 0; pageIndex < spread->PageCount(); ++pageIndex) {
		CLodCache::TrianglePage page;
		CHECK(CLodCache::TrianglePage::Parse(spread->Page(pageIndex), page, nullptr));
	}
}

// Stage 6 step 3: normal spread blocks. The block format and the roughness widening, by hand.
void TestNormalSpreadBlock()
{
	CHECK(clod_normal_spread::Encode(0.0) == 0u && clod_normal_spread::Encode(-1.0) == 0u);
	CHECK(clod_normal_spread::Encode(1.0) == 255u && clod_normal_spread::Encode(4.0) == 255u);
	for (const double v : { 0.0001, 0.001, 0.01, 0.1, 0.5 }) {
		const float decoded = clod_normal_spread::Decode(clod_normal_spread::Encode(v));
		CHECK(std::fabs(std::sqrt(decoded) - std::sqrt(v)) <= 0.5f / 255.0f + 1.0e-6f); // half a step of sqrt(v)
	}
	// alpha'^2 = alpha^2 + min(scale v, cap alpha^2), with alpha = roughness^2; never below the material's roughness, never above 1.
	const float widened = clod_normal_spread::WidenPerceptualRoughness(0.8f, 0.1f, 1.0f, 1.0f);
	CHECK(std::fabs(std::pow(widened, 4.0f) - (std::pow(0.8f, 4.0f) + 0.1f)) < 1.0e-5f);
	const float capped = clod_normal_spread::WidenPerceptualRoughness(0.5f, 0.1f, 1.0f, 1.0f); // 0.1 > alpha^2 = 0.0625
	CHECK(std::fabs(std::pow(capped, 4.0f) - 2.0f * 0.0625f) < 1.0e-5f);
	CHECK(clod_normal_spread::WidenPerceptualRoughness(0.3f, 0.0f, 1.0f, 1.0f) == 0.3f);
	CHECK(clod_normal_spread::WidenPerceptualRoughness(0.3f, 0.5f, 0.0f, 1.0f) == 0.3f);
	CHECK(clod_normal_spread::WidenPerceptualRoughness(0.3f, 0.5f, 1.0f, 0.0f) == 0.3f);
	CHECK(clod_normal_spread::WidenPerceptualRoughness(0.95f, 1.0f, 1.0f, 4.0f) == 1.0f);

	const std::vector<uint8_t> zeros(7u, 0u);
	CHECK(clod_normal_spread::EncodeBlock(zeros).empty());
	std::vector<uint8_t> bytes{ 0u, 3u, 0u, 200u, 17u };
	const std::vector<std::byte> block = clod_normal_spread::EncodeBlock(bytes);
	CHECK(block.size() == clod_normal_spread::BlockSize(5u) && block.size() == 12u && (block.size() % 4u) == 0u);
	CHECK(clod_normal_spread::ValidateBlock(block, 5u));
	CHECK(!clod_normal_spread::ValidateBlock(block, 6u));
	CHECK(!clod_normal_spread::ValidateBlock(std::span<const std::byte>(block).first(8u), 5u));
	for (uint32_t vertex = 0; vertex < bytes.size(); ++vertex) {
		CHECK(clod_normal_spread::VertexByte(block, vertex) == bytes[vertex]);
	}
	std::vector<std::byte> broken = block;
	broken[0] = std::byte{ 0 };
	CHECK(!clod_normal_spread::ValidateBlock(broken, 5u));
}

// A flat panel over [0,1]^2 with UV = xy whose shading normals carry fine relief where x < bumpyBelow (each vertex tilted by up to
// about 30 degrees, deterministic) and point straight up elsewhere: coarse levels cannot draw the relief with three corner
// normals, so the spread there is large, and none elsewhere.
LayoutSource MakeBumpyPanel(uint32_t resolution, float bumpyBelow)
{
	LayoutSource source;
	source.flags = VertexFlags::VERTEX_NORMALS | VertexFlags::VERTEX_TEXCOORDS;
	source.stride = MeshVertexLayout::VertexSize(source.flags);
	const auto noise = [](uint32_t x, uint32_t y, uint32_t salt) {
		uint32_t v = x * 73856093u ^ y * 19349663u ^ salt * 83492791u;
		v ^= v >> 13;
		v *= 0x5bd1e995u;
		v ^= v >> 15;
		return static_cast<float>(v & 0xffffu) / 65535.0f - 0.5f;
	};
	for (uint32_t row = 0; row <= resolution; ++row) {
		for (uint32_t column = 0; column <= resolution; ++column) {
			const float x = static_cast<float>(column) / resolution, y = static_cast<float>(row) / resolution;
			float nx = 0.0f, ny = 0.0f;
			if (x < bumpyBelow) {
				nx = 1.2f * noise(column, row, 1u);
				ny = 1.2f * noise(column, row, 2u);
			}
			const float length = std::sqrt(nx * nx + ny * ny + 1.0f);
			source.Add({ x, y, 0.0f }, { nx / length, ny / length, 1.0f / length }, { x, y });
		}
	}
	const uint32_t stride = resolution + 1u;
	for (uint32_t row = 0; row < resolution; ++row) {
		for (uint32_t column = 0; column < resolution; ++column) {
			const uint32_t a = row * stride + column;
			source.indices.insert(source.indices.end(), { a, a + 1u, a + stride + 1u, a, a + stride + 1u, a + stride });
		}
	}
	return source;
}

// Stage 6 step 3 (lodNormalSpread): off, no block is written and the counts are zero; on, every simplified cluster that hides normal
// spread carries a block in its page, the groups, segments, geometry and nodes are the same as without it, the metadata (the same
// schema either way) reads back, the stored spread is where the relief is and nowhere else, and building twice gives the same
// bytes. With UV remap blocks (step 2) both kinds sit in the same pages.
void TestNormalSpread(const fs::path& root)
{
	const LayoutSource bumpy = MakeBumpyPanel(64u, 0.5f);
	ClusterLODBuilderSettings off = ReplacementSettings(false, false);
	ClusterLODBuilderSettings on = off;
	on.lodNormalSpread = true;
	CHECK(!ClusterLODNormalSpreadActive(off) && ClusterLODNormalSpreadActive(on));
	const auto schemaOf = [&](const std::string& primPath) {
		for (const fs::path& path : FindCacheMetadata(root)) {
			const CLodCache::OpenedMetadata opened = CLodCache::OpenMetadataFile(path);
			if (opened.status == CLodCache::OpenMetadataStatus::Ok && opened.data.prebuiltData.cacheSource.primPath == primPath) {
				return opened.foundSchemaVersion;
			}
		}
		return 0u;
	};
	const auto nodesEqual = [](const CachedMesh& a, const CachedMesh& b) {
		const auto& na = a.Prebuilt().nodes;
		const auto& nb = b.Prebuilt().nodes;
		return na.size() == nb.size() && std::memcmp(na.data(), nb.data(), na.size() * sizeof(ClusterLODNode)) == 0;
	};
	// Every simplified corner's position, normal and stored spread, in page order.
	struct SpreadCorner {
		DirectX::XMFLOAT3 position, normal;
		float v;
	};
	const auto corners = [](const CachedMesh& mesh, uint32_t& blocks, uint64_t& blockBytes, bool& blocksOnLeaves) {
		std::vector<SpreadCorner> out;
		blocks = 0u;
		blockBytes = 0u;
		blocksOnLeaves = false;
		for (uint32_t pageIndex = 0; pageIndex < mesh.PageCount(); ++pageIndex) {
			CLodCache::TrianglePage page;
			std::string error;
			if (!CLodCache::TrianglePage::Parse(mesh.Page(pageIndex), page, &error)) {
				std::fprintf(stderr, "normal spread: page %u: %s\n", pageIndex, error.c_str());
				CHECK(false);
				continue;
			}
			for (uint32_t meshletIndex = 0; meshletIndex < page.MeshletCount(); ++meshletIndex) {
				const CLodCache::TriangleMeshlet meshlet = page.Meshlet(meshletIndex);
				const std::span<const std::byte> block = page.NormalSpreadBlock(meshletIndex);
				if (!block.empty()) {
					CHECK(page.HasNormalSpread() && clod_normal_spread::ValidateBlock(block, meshlet.vertexCount));
					++blocks;
					blockBytes += block.size();
					blocksOnLeaves = blocksOnLeaves || meshlet.refinedGroup < 0;
				}
				if (meshlet.refinedGroup < 0) continue;
				for (uint32_t vertex = 0; vertex < meshlet.vertexCount; ++vertex) {
					out.push_back({ page.Position(meshlet, vertex), page.Normal(meshlet, vertex), page.NormalSpread(meshletIndex, vertex) });
				}
			}
		}
		return out;
	};

	const auto plain = SaveAndOpen(root, bumpy, off, "/spread-off");
	const auto spread = SaveAndOpen(root, bumpy, on, "/spread-on");
	const auto again = SaveAndOpen(root, bumpy, on, "/spread-on-again");
	if (!plain || !spread || !again) {
		CHECK(false);
		return;
	}
	uint32_t plainBlocks = 0, blocks = 0;
	uint64_t plainBytes = 0, blockBytes = 0;
	bool leafBlocks = false;
	const std::vector<SpreadCorner> plainCorners = corners(*plain, plainBlocks, plainBytes, leafBlocks);
	const std::vector<SpreadCorner> spreadCorners = corners(*spread, blocks, blockBytes, leafBlocks);

	// Off: no blocks, zero counts, the one schema.
	CHECK(plain->Prebuilt().normalSpreadClusters == 0u && plain->Prebuilt().normalSpreadBytes == 0u && plainBlocks == 0u &&
		schemaOf("/spread-off") == CLodCache::kSchemaVersion);
	// On: the same hierarchy and geometry, blocks only on simplified clusters, counted as stored, the same schema.
	const ClusterLODPrebuiltData& data = spread->Prebuilt();
	CHECK(schemaOf("/spread-on") == CLodCache::kSchemaVersion);
	CHECK(blocks > 0u && blocks == data.normalSpreadClusters && blockBytes == data.normalSpreadBytes && !leafBlocks);
	CHECK(data.groups.size() == plain->Prebuilt().groups.size() && data.segments.size() == plain->Prebuilt().segments.size());
	for (size_t group = 0; group < data.groups.size() && group < plain->Prebuilt().groups.size(); ++group) {
		CHECK(std::memcmp(&data.groups[group], &plain->Prebuilt().groups[group], sizeof(ClusterLODGroup)) == 0);
	}
	CHECK(nodesEqual(*plain, *spread)); // no channels: the nodes are untouched
	CHECK(spreadCorners.size() == plainCorners.size());
	bool sameGeometry = spreadCorners.size() == plainCorners.size();
	for (size_t i = 0; sameGeometry && i < spreadCorners.size(); ++i) {
		sameGeometry = std::memcmp(&spreadCorners[i].position, &plainCorners[i].position, sizeof(DirectX::XMFLOAT3)) == 0 &&
			std::memcmp(&spreadCorners[i].normal, &plainCorners[i].normal, sizeof(DirectX::XMFLOAT3)) == 0;
	}
	CHECK(sameGeometry);

	// The spread is where the relief is: large on the bumpy half, nothing on the smooth half (away from the seam between them).
	double bumpySum = 0.0, flatSum = 0.0;
	uint32_t bumpyCount = 0, flatCount = 0;
	float flatMax = 0.0f;
	for (const SpreadCorner& corner : spreadCorners) {
		if (corner.position.x < 0.3f) {
			bumpySum += corner.v;
			++bumpyCount;
		}
		else if (corner.position.x > 0.7f) {
			flatSum += corner.v;
			flatMax = std::max(flatMax, corner.v);
			++flatCount;
		}
	}
	const double bumpyMean = bumpyCount > 0u ? bumpySum / bumpyCount : 0.0, flatMean = flatCount > 0u ? flatSum / flatCount : 0.0;
	std::fprintf(stderr, "normal spread: blocks %u, bytes %llu, simplified corners %zu, mean v bumpy %.4f (%u) flat %.6f (%u, max %.6f)\n", blocks,
		static_cast<unsigned long long>(blockBytes), spreadCorners.size(), bumpyMean, bumpyCount, flatMean, flatCount, flatMax);
	CHECK(bumpyCount > 0u && flatCount > 0u);
	CHECK(bumpyMean > 0.03);       // the relief tilts normals by up to ~30 degrees
	CHECK(flatMean < bumpyMean * 0.02);
	CHECK(flatMax <= clod_normal_spread::Decode(2u));

	// Deterministic.
	CHECK(SameBuild(*spread, *again) && nodesEqual(*spread, *again));

	// With channels: the channels are those of the build without the spread.
	ClusterLODBuilderSettings channels = off;
	channels.lodErrorChannels = true;
	ClusterLODBuilderSettings channelsSpread = channels;
	channelsSpread.lodNormalSpread = true;
	const auto withChannels = SaveAndOpen(root, bumpy, channels, "/spread-channels-off");
	const auto withChannelsSpread = SaveAndOpen(root, bumpy, channelsSpread, "/spread-channels-on");
	if (withChannels && withChannelsSpread) {
		CHECK(nodesEqual(*withChannels, *withChannelsSpread));
		CHECK(withChannelsSpread->Prebuilt().normalSpreadClusters == data.normalSpreadClusters);
	}

	// A flat panel with flat normals hides nothing: no blocks.
	const auto flat = SaveAndOpen(root, MakeBumpyPanel(32u, 0.0f), on, "/spread-flat");
	if (flat) {
		CHECK(flat->Prebuilt().normalSpreadClusters == 0u && schemaOf("/spread-flat") == CLodCache::kSchemaVersion);
	}

	// With UV remap blocks: both in the same pages, the same schema, both validating (Parse checks both).
	const LayoutSource dome = MakeChartDome();
	ClusterLODBuilderSettings remap = TopologySettings(true, false);
	remap.simplifyTopologyRelaxUv = true;
	remap.simplifyTopologyTextureArea = 1.0f;
	remap.lodErrorChannels = true;
	remap.lodErrorChannelsTexture = 1.0f;
	remap.lodErrorChannelsTextureRemap = 0.01f;
	ClusterLODBuilderSettings remapSpread = remap;
	remapSpread.lodNormalSpread = true;
	auto texture = std::make_shared<clod_texture::Texture>();
	texture->width = texture->height = 64u;
	std::vector<uint8_t> texels(64u * 64u * 4u, 200u);
	for (uint32_t y = 0; y < 64u; ++y) {
		for (uint32_t x = 0; x < 64u; ++x) {
			texels[(y * 64u + x) * 4u + 0u] = static_cast<uint8_t>(((y / 8u) * 8u + x / 8u) * 3u);
		}
	}
	texture->mips.push_back(std::move(texels));
	clod_texture::BuildMips(*texture);
	ClusterLODMaterialAppearance opaque;
	opaque.hasBaseColorTexture = true;
	opaque.contentHash = 7u;
	opaque.decodeBaseColor = [texture] { return std::shared_ptr<const clod_texture::Texture>(texture); };
	const auto buildWith = [&](const ClusterLODBuilderSettings& settings, const std::string& primPath) -> std::unique_ptr<CachedMesh> {
		const ClusterLODPrebuildArtifacts artifacts = BuildClusterLODArtifactsFromGeometry(
			dome.vertices, dome.stride, nullptr, 0u, dome.indices, dome.uvSets, dome.flags, settings, nullptr, &opaque);
		CHECK(CLodCacheLoader::SavePrebuilt({ "clod_eval_test.source", primPath, "", false }, artifacts.prebuiltData, artifacts.cacheBuildData.AsPayload()));
		for (const fs::path& path : FindCacheMetadata(root)) {
			std::string error;
			auto mesh = CachedMesh::Open(path, error);
			if (mesh && mesh->Prebuilt().cacheSource.primPath == primPath) return mesh;
		}
		CHECK(false);
		return nullptr;
	};
	const auto remapOnly = buildWith(remap, "/spread-remap-off");
	const auto both = buildWith(remapSpread, "/spread-remap-on");
	if (!remapOnly || !both) {
		CHECK(false);
		return;
	}
	CHECK(schemaOf("/spread-remap-off") == CLodCache::kSchemaVersion && schemaOf("/spread-remap-on") == CLodCache::kSchemaVersion);
	CHECK(remapOnly->Prebuilt().normalSpreadClusters == 0u && both->Prebuilt().uvRemapClusters > 0u && both->Prebuilt().normalSpreadClusters > 0u);
	CHECK(nodesEqual(*remapOnly, *both)); // the same channels: the spread changes no selection
	uint32_t remapBlocks = 0, spreadBlocks = 0, pagesWithBoth = 0;
	for (uint32_t pageIndex = 0; pageIndex < both->PageCount(); ++pageIndex) {
		CLodCache::TrianglePage page;
		if (!CLodCache::TrianglePage::Parse(both->Page(pageIndex), page)) {
			CHECK(false);
			continue;
		}
		pagesWithBoth += page.HasUvRemap() && page.HasNormalSpread() ? 1u : 0u;
		for (uint32_t meshletIndex = 0; meshletIndex < page.MeshletCount(); ++meshletIndex) {
			remapBlocks += page.UvRemapBlock(meshletIndex).empty() ? 0u : 1u;
			spreadBlocks += page.NormalSpreadBlock(meshletIndex).empty() ? 0u : 1u;
		}
	}
	std::fprintf(stderr, "normal spread with remap: remap blocks %u (%u stored), spread blocks %u (%u stored), pages with both %u\n", remapBlocks,
		both->Prebuilt().uvRemapClusters, spreadBlocks, both->Prebuilt().normalSpreadClusters, pagesWithBoth);
	CHECK(remapBlocks == both->Prebuilt().uvRemapClusters && spreadBlocks == both->Prebuilt().normalSpreadClusters);
	CHECK(remapBlocks > 0u && spreadBlocks > 0u && pagesWithBoth > 0u);
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

ClusterLODBuilderSettings ChannelSettings(bool channels)
{
	ClusterLODBuilderSettings settings = ReplacementSettings(false, false);
	settings.lodErrorChannels = channels;
	return settings;
}

ResultSet AnalyzeWithRules(const fs::path& metadata, const std::string& build, const std::vector<CutRule>& rules)
{
	ResultSet results;
	std::string error;
	AnalyzeOptions options;
	options.analyzers = { "quality.appearance", "hierarchy.structure" };
	options.cutRules = rules;
	CHECK(AnalyzeMeshes({ { metadata, "sheet" } }, build, build, options, results, error));
	if (!error.empty()) {
		std::fprintf(stderr, "analyze: %s\n", error.c_str());
	}
	return results;
}

// Error channels (stage 3): off, nothing is stored and nothing changes; on, groups and pages are the same as without them (only
// the nodes' spare words differ), and the runtime rule with weight 0 and the quadric channel selects exactly the stored errors.
void TestErrorChannelsOffAndWeightZero(const fs::path& root)
{
	const SourceMesh sheet = MakeSheet(256u, 0.0f);
	LayoutSource source;
	source.vertices = sheet.vertices;
	source.indices = sheet.indices;
	const auto off = SaveAndOpen(root, source, ChannelSettings(false), "/channels-off");
	const auto on = SaveAndOpen(root, source, ChannelSettings(true), "/channels-on");
	CHECK(off && on);
	if (!off || !on) {
		return;
	}
	CHECK(!ReadGroupErrorChannels(off->Prebuilt()).present);
	CHECK(ReadGroupErrorChannels(on->Prebuilt()).present);
	CHECK(SameBuild(*off, *on));
	const auto& nodesOff = off->Prebuilt().nodes;
	const auto& nodesOn = on->Prebuilt().nodes;
	CHECK(nodesOff.size() == nodesOn.size());
	bool nodesMatch = nodesOff.size() == nodesOn.size();
	for (size_t index = 0; nodesMatch && index < nodesOff.size(); ++index) {
		const ClusterLODNode& a = nodesOff[index];
		ClusterLODNode b = nodesOn[index];
		CHECK(a.traversalMetric.channelFlags == 0u && a.traversalMetric.maxMeasuredGeometricError == 0.0f && a.traversalMetric.maxAppearanceError == 0.0f);
		CHECK(b.traversalMetric.channelFlags == CLOD_NODE_CHANNELS_PRESENT);
		b.traversalMetric.maxMeasuredGeometricError = 0.0f;
		b.traversalMetric.maxAppearanceError = 0.0f;
		b.traversalMetric.channelFlags = 0u;
		nodesMatch = std::memcmp(&a, &b, sizeof(ClusterLODNode)) == 0;
	}
	CHECK(nodesMatch);

	// Weight 0 on the quadric channel is the stored errors: every cut metric equal to the untagged sweep's.
	const ResultSet results = AnalyzeWithRules(on->MetadataPath(), "channels-on", { { "q0", CutGeometricChannel::Quadric, 0.0f } });
	uint32_t compared = 0, differing = 0;
	for (const MetricRecord& record : results.records) {
		const auto rule = record.tags.find("rule");
		if (rule == record.tags.end() || rule->second != "q0") {
			continue;
		}
		Tags tags = record.tags;
		tags.erase("rule");
		for (const MetricRecord& other : results.records) {
			if (other.id == record.id && other.mesh == record.mesh && other.tags == tags) {
				++compared;
				differing += other.value == record.value ? 0u : 1u;
				break;
			}
		}
	}
	CHECK(compared > 100u);
	CHECK(differing == 0u);
	CHECK(Value(results, "appearance.channels.monotonic_violations") == 0.0);
}

// The channels never decrease from a group to the groups its clusters refine into, nodes hold their subtree's largest, and
// segment leaves their group's: so every segment's enter <= leave under any rule.
void TestErrorChannelsMonotone(const fs::path& root)
{
	const SourceMesh sheet = MakeSheet(256u, 0.0f);
	LayoutSource source;
	source.vertices = sheet.vertices;
	source.indices = sheet.indices;
	const auto mesh = SaveAndOpen(root, source, ChannelSettings(true), "/channels-monotone");
	if (!mesh) {
		return;
	}
	const ClusterLODPrebuiltData& data = mesh->Prebuilt();
	const GroupErrorChannels channels = ReadGroupErrorChannels(data);
	CHECK(channels.present);
	uint32_t edges = 0, violations = 0, positiveGeometric = 0, positiveAppearance = 0;
	for (uint32_t group = 0; group < data.groups.size(); ++group) {
		const ClusterLODGroup& record = data.groups[group];
		for (uint32_t s = 0; s < record.segmentCount; ++s) {
			const int32_t child = data.segments[record.firstSegment + s].refinedGroup;
			if (child < 0) {
				continue;
			}
			++edges;
			violations += channels.measured[group] < channels.measured[child] || channels.appearance[group] < channels.appearance[child] ? 1u : 0u;
			positiveGeometric += channels.measured[child] > 0.0f ? 1u : 0u;
			positiveAppearance += channels.appearance[child] > 0.0f ? 1u : 0u;
		}
	}
	CHECK(edges > 0u);
	CHECK(violations == 0u);
	CHECK(positiveGeometric == edges);
	CHECK(positiveAppearance == edges);
	uint32_t nodeViolations = 0;
	for (const ClusterLODNode& node : data.nodes) {
		if (node.range.isGroup == CLOD_NODE_SEGMENT_LEAF) {
			nodeViolations += node.traversalMetric.maxMeasuredGeometricError != channels.measured[node.range.ownerGroupId] ||
				node.traversalMetric.maxAppearanceError != channels.appearance[node.range.ownerGroupId] ? 1u : 0u;
			continue;
		}
		if (node.range.isGroup != CLOD_NODE_INTERNAL) {
			continue;
		}
		for (uint32_t c = 0; c <= node.range.countMinusOne; ++c) {
			const ClusterLODTraversalMetric& child = data.nodes[node.range.indexOrOffset + c].traversalMetric;
			nodeViolations += node.traversalMetric.maxMeasuredGeometricError < child.maxMeasuredGeometricError ||
				node.traversalMetric.maxAppearanceError < child.maxAppearanceError ? 1u : 0u;
		}
	}
	CHECK(nodeViolations == 0u);
}

// A flat plane whose normals carry baked relief: simplified levels lie on the plane (no geometric error worth the name) but cannot
// keep the relief's shading. The appearance channel sees it, a plane with flat normals has none, and selecting with it delays the
// mis-shaded levels: no group's selection error falls, some rise, and at some cut the rule draws finer triangles that shade better.
void TestAppearanceDelaysMisShadedLevels(const fs::path& root)
{
	constexpr uint32_t kResolution = 128u;
	const auto makePlane = [](bool relief) {
		LayoutSource source;
		for (uint32_t row = 0; row <= kResolution; ++row) {
			for (uint32_t column = 0; column <= kResolution; ++column) {
				const float x = static_cast<float>(column) / kResolution, y = static_cast<float>(row) / kResolution;
				DirectX::XMFLOAT3 normal{ 0.0f, 0.0f, 1.0f };
				if (relief) {
					const float nx = 0.4f * std::sin(25.0f * x), ny = 0.4f * std::cos(23.0f * y);
					const float length = std::sqrt(nx * nx + ny * ny + 1.0f);
					normal = { nx / length, ny / length, 1.0f / length };
				}
				source.Add({ x, y, 0.0f }, normal);
			}
		}
		const uint32_t stride = kResolution + 1u;
		for (uint32_t row = 0; row < kResolution; ++row) {
			for (uint32_t column = 0; column < kResolution; ++column) {
				const uint32_t a = row * stride + column;
				source.indices.insert(source.indices.end(), { a, a + 1u, a + stride + 1u, a, a + stride + 1u, a + stride });
			}
		}
		return source;
	};
	const auto flat = SaveAndOpen(root, makePlane(false), ChannelSettings(true), "/channels-flat");
	const auto relief = SaveAndOpen(root, makePlane(true), ChannelSettings(true), "/channels-relief");
	if (!flat || !relief) {
		return;
	}
	const GroupErrorChannels flatChannels = ReadGroupErrorChannels(flat->Prebuilt());
	const GroupErrorChannels reliefChannels = ReadGroupErrorChannels(relief->Prebuilt());
	// The open outer border may be simplified at the coarsest levels (cut corners): that is real geometric error. Below them the
	// levels stay on the plane, and their shading is what is wrong.
	float flatAppearance = 0.0f, reliefAppearance = 0.0f;
	uint32_t reliefGroups = 0, shadingDominated = 0;
	for (size_t group = 0; group < flatChannels.appearance.size(); ++group) {
		if (flat->Prebuilt().groups[group].bounds.error < 1.0e19f) {
			flatAppearance = std::max(flatAppearance, flatChannels.appearance[group]);
		}
	}
	for (size_t group = 0; group < reliefChannels.appearance.size(); ++group) {
		if (relief->Prebuilt().groups[group].bounds.error < 1.0e19f) {
			reliefAppearance = std::max(reliefAppearance, reliefChannels.appearance[group]);
			++reliefGroups;
			shadingDominated += reliefChannels.measured[group] * 100.0f < reliefChannels.appearance[group] ? 1u : 0u;
		}
	}
	std::fprintf(stderr, "channels: flat appearance=%g relief appearance=%g relief groups=%u shading-dominated=%u\n", flatAppearance,
		reliefAppearance, reliefGroups, shadingDominated);
	CHECK(flatAppearance < 1.0e-3f);
	CHECK(reliefAppearance > 0.05f);
	CHECK(shadingDominated * 2u > reliefGroups);

	const CutRule geometryOnly{ "m0", CutGeometricChannel::Measured, 0.0f };
	const CutRule withAppearance{ "m1", CutGeometricChannel::Measured, 1.0f };
	uint32_t raised = 0, lowered = 0;
	for (size_t group = 0; group < reliefChannels.measured.size(); ++group) {
		const ClusterLODGroup& record = relief->Prebuilt().groups[group];
		const float without = SelectionError(record.bounds.error, reliefChannels.measured[group], reliefChannels.appearance[group], geometryOnly);
		const float with = SelectionError(record.bounds.error, reliefChannels.measured[group], reliefChannels.appearance[group], withAppearance);
		raised += with > without ? 1u : 0u;
		lowered += with < without ? 1u : 0u;
	}
	CHECK(raised > 0u);
	CHECK(lowered == 0u);

	const ResultSet results = AnalyzeWithRules(relief->MetadataPath(), "channels-relief", { geometryOnly, withAppearance });
	bool delayed = false;
	for (int exponent = -14; exponent <= 8; ++exponent) {
		const std::string cut = std::format("{:g}", std::ldexp(1.0, exponent));
		const double trianglesWithout = Value(results, "appearance.cut.triangles", { { "cut", cut }, { "rule", "m0" } });
		const double trianglesWith = Value(results, "appearance.cut.triangles", { { "cut", cut }, { "rule", "m1" } });
		CHECK(trianglesWith >= trianglesWithout);
		const double qWithout = Value(results, "appearance.cut.normal_q_per_area", { { "cut", cut }, { "rule", "m0" } });
		const double qWith = Value(results, "appearance.cut.normal_q_per_area", { { "cut", cut }, { "rule", "m1" } });
		delayed |= trianglesWith > trianglesWithout && qWith < qWithout;
	}
	CHECK(delayed);
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


// CLOD_EVAL_TEST_FILTER=<substring>: run only the tests whose names contain it (all when unset).
bool Selected(const char* name)
{
	const char* filter = std::getenv("CLOD_EVAL_TEST_FILTER");
	return filter == nullptr || filter[0] == 0 || std::strstr(name, filter) != nullptr;
}

int main()
{
	if (Selected("TestImageKernels")) TestImageKernels();
	if (Selected("TestVoxelSurfaceTrace")) TestVoxelSurfaceTrace();
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
		if (Selected("TestCompare")) TestCompare();
		if (Selected("TestResultsRoundTrip")) TestResultsRoundTrip(root);
		if (Selected("TestBadInputsAreErrors")) TestBadInputsAreErrors(root);
		if (Selected("TestTriangleHierarchy")) TestTriangleHierarchy(root);
		if (Selected("TestNormalQuality")) TestNormalQuality(root);
		if (Selected("TestAppearanceMeasure")) TestAppearanceMeasure();
		if (Selected("TestAppearanceCuts")) TestAppearanceCuts(root);
		if (Selected("TestVoxelHierarchy")) TestVoxelHierarchy(root);
		if (Selected("TestVoxelFork")) TestVoxelFork(root);
		if (Selected("TestLeafGeometryHash")) TestLeafGeometryHash(root);
		if (Selected("TestValidationFlags")) TestValidationFlags(root);
		if (Selected("TestReplacementVerticesOffAndDeterminism")) TestReplacementVerticesOffAndDeterminism(root);
		if (Selected("TestReplacementVerticesOnPlane")) TestReplacementVerticesOnPlane(root);
		if (Selected("TestReplacementVerticesOnGrooves")) TestReplacementVerticesOnGrooves(root);
		if (Selected("TestOptimalOffAndDeterminism")) TestOptimalOffAndDeterminism(root);
		if (Selected("TestOptimalOnRoughPanels")) TestOptimalOnRoughPanels(root);
		if (Selected("TestErrorChannelsOffAndWeightZero")) TestErrorChannelsOffAndWeightZero(root);
		if (Selected("TestErrorChannelsMonotone")) TestErrorChannelsMonotone(root);
		if (Selected("TestAppearanceDelaysMisShadedLevels")) TestAppearanceDelaysMisShadedLevels(root);
		if (Selected("TestTopologyOffAndDeterminism")) TestTopologyOffAndDeterminism(root);
		if (Selected("TestTopologyOnFacetedDome")) TestTopologyOnFacetedDome(root);
		if (Selected("TestSloppyLastResort")) TestSloppyLastResort(root);
		if (Selected("TestTopologyTextureCheck")) TestTopologyTextureCheck(root);
		if (Selected("TestUpdateUvTransfer")) TestUpdateUvTransfer(root);
		if (Selected("TestOptimalTopology")) TestOptimalTopology(root);
		if (Selected("TestTopologyTextureChartsAndTexels")) TestTopologyTextureChartsAndTexels(root);
		if (Selected("TestTextureChannel")) TestTextureChannel(root);
		if (Selected("TestSourceTextures")) TestSourceTextures(root);
		if (Selected("TestSourceTexturesKtx2")) TestSourceTexturesKtx2(root);
		if (Selected("TestTextureColor")) TestTextureColor(root);
		if (Selected("TestTopologyTextureColor")) TestTopologyTextureColor(root);
		if (Selected("TestUvRemapBlock")) TestUvRemapBlock();
		if (Selected("TestTextureRemap")) TestTextureRemap(root);
		if (Selected("TestUvRemapRegions")) TestUvRemapRegions();
		if (Selected("TestTextureRemapRegions")) TestTextureRemapRegions(root);
		if (Selected("TestNormalSpreadBlock")) TestNormalSpreadBlock();
		if (Selected("TestNormalSpread")) TestNormalSpread(root);
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
