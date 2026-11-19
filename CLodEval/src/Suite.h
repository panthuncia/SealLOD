#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "Core/ErrorChannels.h"
#include "Core/Results.h"
#include "Core/SourceTextures.h"

namespace clodeval {

struct SuiteAsset {
	std::string name;
	std::filesystem::path path;  // absolute
};

// A way of building the assets. Settings are BASICRENDERER_CLOD_* environment values, which feed the
// build-config hash, so the process that later renders these caches must be given the same ones.
struct SuiteVariant {
	std::string name;
	std::map<std::string, std::string> settings;
	std::filesystem::path assetSettings;  // empty: no per-asset rules
	// A mesh subset of another variant (--meshes): only these glTF mesh indices are built, analyzed and drawn. Its name is
	// "<base>~m<tag>", so its build, results and captures sit beside the base variant's and never replace them.
	std::string baseName;
	std::vector<uint32_t> meshSubset;
};

// Parses --meshes: comma-separated glTF mesh indices, as in the /glTF/Mesh/<index> mesh names results use. An item may
// also be a whole mesh name ("/glTF/Mesh/12/Primitive/0"). Throws on anything else.
std::vector<uint32_t> ParseMeshSubset(const std::string& text);
// The glTF mesh index in a mesh name such as "/glTF/Mesh/12/Primitive/0", or -1.
int64_t MeshIndexOfName(std::string_view meshName);
// The variant restricted to a mesh subset, named "<base>~m<tag>".
SuiteVariant MakeMeshSubsetVariant(const SuiteVariant& base, const std::vector<uint32_t>& meshes);

// How a suite's builds are rendered. See docs/clod-evaluation.md for what each view and probe is for.
struct SuiteCapture {
	bool enabled = false;
	uint32_t width = 1280u;
	uint32_t height = 720u;
	uint32_t referenceScale = 4u;  // the reference renders this many times larger per axis
	// Frames each probe averages. Raster triangles are the same every frame, so one is enough; suites with voxels ask
	// for 64, the period of the SGGX sample sequence.
	uint32_t accumulateFrames = 1u;
	double fovDegrees = 60.0;
	double farRadii = 400.0;
	double readyTimeoutSeconds = 240.0;
	double cornerNdc = 0.65;
	uint32_t pagePoolSlabs = 0u;  // 0: the renderer's cap. Forced leaves of a large asset need more.
	// Replaces framing from the cache's bounding spheres, which node transforms can make misleading.
	bool hasFrame = false;
	double frameCenter[3]{};
	double frameRadius = 0.0;
	std::vector<double> distances;        // in bounding radii
	std::vector<double> cornerDistances;  // the same, with the asset moved towards a screen corner
	std::vector<double> sweepDistances;   // where the LOD threshold is swept
	std::vector<double> thresholds;
	// The triangle-to-voxel handover. See Transition.h.
	bool transition = false;
	double transitionFovDegrees = 2.0;
	uint32_t transitionSteps = 13u;
};

struct Suite {
	std::string name;
	std::filesystem::path path;
	std::filesystem::path workspace;
	std::vector<SuiteAsset> assets;
	std::vector<SuiteVariant> variants;
	std::vector<std::string> analyzers;  // empty: all
	std::vector<Budget> budgets;
	SuiteCapture capture;
	// false: CLodCacheTool --no-textures, for geometry-only suites over assets whose textures take long to process.
	bool textures = true;
	// Runtime LOD selection rules quality.appearance's cut sweep evaluates besides the stored errors, each under a "rule" tag
	// (docs/clod-evaluation.md). Analysis only: they do not change what is built.
	std::vector<CutRule> cutRules;
	// Simulated appearance bakes quality.texture sweeps besides the stored UVs (docs/clod-appearance-stage6.md). Analysis only.
	std::vector<TextureBake> textureBakes;
	// quality.specular's footprint (AnalysisContext::specularFootprint). Analysis only.
	double specularFootprint = 1.0;
	double specularSpreadCap = 1.0; // AnalysisContext::specularSpreadCap. Analysis only.
};

// Throws std::runtime_error, naming the problem, if the suite is malformed.
Suite LoadSuite(const std::filesystem::path& path, const std::filesystem::path& workspaceOverride);

// One opened build of a suite variant, or any directory of caches.
struct BuildPaths {
	std::filesystem::path directory;
	std::filesystem::path cacheRoot;
	std::filesystem::path manifest;
	std::filesystem::path buildRecord;
	std::filesystem::path log;
};
BuildPaths GetBuildPaths(const Suite& suite, const SuiteVariant& variant);

enum class BuildOutcome { Built, UpToDate, Failed };

struct BuildOptions {
	std::filesystem::path cacheTool;
	bool force = false;
	// Keep an existing build whose settings, assets and mesh subset match even though the tool binary changed since
	// (--reuse-builds): for variants a generator change cannot affect, such as ones that leave a new feature off.
	bool reuseBuilds = false;
};

// Runs CLodCacheTool for a variant unless nothing it depends on has changed. A changed tool binary
// counts, so editing the generator and rebuilding the tool is enough to invalidate the cache, unless
// reuseBuilds is set.
BuildOutcome BuildVariant(const Suite& suite, const SuiteVariant& variant, const BuildOptions& options, std::string& outError);

// Where the variant's results go.
std::filesystem::path GetResultsPath(const Suite& suite, const std::string& variantName);

struct AnalyzeOptions {
	std::vector<std::string> analyzers;  // empty: all
	std::string assetFilter;
	uint32_t threads = 0;  // 0: one per hardware thread
	float mergeReachCut = 440.0f;  // AnalysisContext::mergeReachCut
	// A forked mesh (CachedMesh::IsForked) is measured per branch: "" both, as "<mesh>#triangle" and "<mesh>#voxel";
	// "triangle" or "voxel" just that one, under the mesh's own name (comparable with an unforked build).
	std::string branch;
	std::vector<CutRule> cutRules;  // AnalysisContext::cutRules; AnalyzeVariant uses the suite's when empty
	std::vector<TextureBake> textureBakes;  // AnalysisContext::textureBakes; AnalyzeVariant uses the suite's when empty
	double specularFootprint = 0.0;         // AnalysisContext::specularFootprint; AnalyzeVariant uses the suite's when 0
	double specularSpreadCap = -1.0;        // AnalysisContext::specularSpreadCap; AnalyzeVariant uses the suite's when below 0
};

struct MeshToAnalyze {
	std::filesystem::path metadataPath;
	std::string asset;
	std::filesystem::path source;  // the source asset, when known: quality.texture reads a .glb's materials from it
};

// Runs the analyzers over caches. Fails if an analyzer is unknown or a cache cannot be opened:
// a partial result set could be mistaken for a clean one.
bool AnalyzeMeshes(
	const std::vector<MeshToAnalyze>& meshes,
	const std::string& buildName,
	const std::string& buildKey,
	const AnalyzeOptions& options,
	ResultSet& outResults,
	std::string& outError);

bool AnalyzeVariant(const Suite& suite, const SuiteVariant& variant, const AnalyzeOptions& options, ResultSet& outResults, std::string& outError);

// A readable description of a set of caches. --group adds that group's detail.
nlohmann::json DescribeCache(const std::filesystem::path& metadataPath, int groupIndex, std::string& outError);
std::string FormatCacheDescription(const nlohmann::json& description);

nlohmann::json ProvenanceJson();

// The bounding sphere of an asset as built, which is what cameras are placed from.
bool GetAssetFrame(const Suite& suite, const SuiteVariant& variant, const SuiteAsset& asset, double outCenter[3], double& outRadius, std::string& outError);

}
