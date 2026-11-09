// CLodEval: measures CLod hierarchies from the command line. See docs/clod-evaluation.md.
//
// Commands that need a build run CLodCacheTool as a child process, so the generator and this tool
// can be rebuilt independently. This binary links neither the renderer nor the generator.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "Analyzers/VoxelFidelity.h"
#include "Analyzers/Analyzers.h"
#include "Capture.h"
#include "Core/Analyzer.h"
#include "Core/MergeHierarchy.h"
#include "Core/Process.h"
#include "Core/Results.h"
#include "Suite.h"

namespace {
namespace fs = std::filesystem;
using namespace clodeval;

constexpr int kExitOk = 0;
constexpr int kExitDiffers = 1;
constexpr int kExitNotComparable = 2;
constexpr int kExitError = 3;

constexpr const char* kUsage = R"(CLodEval: measure CLod hierarchies.

  CLodEval build <suite> [--variant name] [--force] [--workspace dir]
      Build each variant's caches with CLodCacheTool. Skipped if nothing changed.
  CLodEval analyze <suite> [--variant name] [--filter asset] [--analyzer id ...] [--branch triangle|voxel] [--workspace dir]
      Run analyzers on built caches and write results/<variant>.jsonl and .csv. A forked mesh is measured per
      branch, as <mesh>#triangle and <mesh>#voxel, or only the --branch given under the mesh's own name.
  CLodEval run <suite> [same options as build and analyze]
      build, then analyze, then check the suite's budgets.
  CLodEval inspect <cache.clodmeta|directory> [--group N] [--json]
      Describe caches. Works on any cache, including SARPPreprocessor's.
  CLodEval fork-census <cache tree> --out rows.csv [--threads N]
      Every forked (triangle/voxel) mesh in a tree: what each branch draws at a range of cuts, and totals.
  CLodEval merge-report <root.clodmeta|directory> [--json] [--out report.json]
      Describe merge-hierarchy roots: the merge tree by level, portals and parts, and what each cut draws.
  CLodEval merge-census <cache tree> --out prefix [--threads N] [--reach-cut 440] [--branch triangle|voxel]
      Every merge root in a tree through hierarchy.merge, in parallel: results files, one row per root, totals and worst roots.
  CLodEval view-root <root.clodmeta|directory> [--size 1920x1080] [--script steps.json]
      Open the viewer on a merge-hierarchy root and the parts its portals open, from the cache tree alone.
  CLodEval voxel-fidelity <cache.clodmeta|directory> --out rows.csv
      Per voxel group: how much of the source surface its cells cover, and how surface-like that source is.
  CLodEval analyze-cache <cache.clodmeta|directory> --out results.jsonl [--analyzer id ...] [--asset name] [--branch triangle|voxel]
      Run analyzers on caches that did not come from a suite.
  CLodEval compare <a.jsonl> <b.jsonl> [--threshold 0.01] [--budget file.json] [--all] [--json]
      Compare two results files. Exit 0 same, 1 different or over budget, 2 not comparable, 3 error.
  CLodEval capture <suite> [--variant name] [--force] [--workspace dir]
      Render built caches with CLodCapture, as the suite's capture block asks. Skipped if nothing changed.
  CLodEval score <suite> [--variant name] [--workspace dir]
      Score the captures and write results/<variant>.images.jsonl, .csv and per-view contact sheets.
  CLodEval report <suite> [--workspace dir]
      Write report/index.html from the scored variants.
  CLodEval transition <suite> [--variant name] [--force] [--workspace dir]
      Only the triangle-to-voxel views: finds the switch, renders just before and after, writes transition.png.
  CLodEval view <suite> [--variant name] [--asset name] [--parts substring] [--size 1920x1080] [--workspace dir]
      Open the viewer on a built variant: debug views, hierarchy overlays, group selection and before/after compare.
  CLodEval full <suite> [same options as build]
      build, analyze, capture, score, then report.
  CLodEval list-analyzers
)";

struct Arguments {
	std::vector<std::string> positional;
	std::map<std::string, std::vector<std::string>> options;

	bool Has(const std::string& name) const { return options.contains(name); }
	std::string Value(const std::string& name, const std::string& fallback = {}) const
	{
		const auto found = options.find(name);
		return found != options.end() && !found->second.empty() ? found->second.back() : fallback;
	}
	std::vector<std::string> Values(const std::string& name) const
	{
		const auto found = options.find(name);
		return found != options.end() ? found->second : std::vector<std::string>{};
	}
};

// Flags take no value. Anything else starting with -- takes the next argument, and an unknown name is
// an error: a mistyped --filter that was quietly ignored would report on the wrong thing.
Arguments ParseArguments(int argc, char** argv)
{
	static const std::vector<std::string> kFlags = { "--force", "--json", "--all" };
	static const std::vector<std::string> kValued = {
		"--variant", "--workspace", "--filter", "--analyzer", "--group", "--out", "--asset", "--threshold", "--budget", "--cache-tool", "--size", "--script", "--parts",
		"--threads", "--reach-cut", "--branch" };
	Arguments arguments;
	for (int index = 2; index < argc; ++index) {
		const std::string argument = argv[index];
		if (!argument.starts_with("--")) {
			arguments.positional.push_back(argument);
		}
		else if (std::ranges::find(kFlags, argument) != kFlags.end()) {
			arguments.options[argument];
		}
		else if (std::ranges::find(kValued, argument) != kValued.end()) {
			if (index + 1 >= argc) {
				throw std::runtime_error(std::format("{} needs a value", argument));
			}
			arguments.options[argument].push_back(argv[++index]);
		}
		else {
			throw std::runtime_error(std::format("unknown option {}", argument));
		}
	}
	return arguments;
}

std::vector<const SuiteVariant*> SelectVariants(const Suite& suite, const Arguments& arguments)
{
	std::vector<const SuiteVariant*> selected;
	const std::vector<std::string> names = arguments.Values("--variant");
	for (const SuiteVariant& variant : suite.variants) {
		if (names.empty() || std::ranges::find(names, variant.name) != names.end()) {
			selected.push_back(&variant);
		}
	}
	for (const std::string& name : names) {
		if (std::ranges::none_of(suite.variants, [&](const SuiteVariant& variant) { return variant.name == name; })) {
			throw std::runtime_error(std::format("suite has no variant '{}'", name));
		}
	}
	return selected;
}

fs::path FindCacheTool(const Arguments& arguments)
{
	if (arguments.Has("--cache-tool")) {
		return fs::absolute(arguments.Value("--cache-tool"));
	}
	return GetExecutableDirectory() / "CLodCacheTool.exe";
}

int CommandBuild(const Suite& suite, const Arguments& arguments)
{
	BuildOptions options;
	options.cacheTool = FindCacheTool(arguments);
	options.force = arguments.Has("--force");
	for (const SuiteVariant* variant : SelectVariants(suite, arguments)) {
		std::string error;
		switch (BuildVariant(suite, *variant, options, error)) {
		case BuildOutcome::Built:
			std::cout << std::format("{}: built\n", variant->name);
			break;
		case BuildOutcome::UpToDate:
			std::cout << std::format("{}: up to date\n", variant->name);
			break;
		case BuildOutcome::Failed:
			std::cerr << std::format("{}: {}\n", variant->name, error);
			return kExitError;
		}
	}
	return kExitOk;
}

bool WriteResultFiles(const fs::path& path, const ResultSet& results, std::string& error)
{
	fs::path csvPath = path;
	csvPath.replace_extension(".csv");
	return WriteResults(path, results, error) && WriteResultsCsv(csvPath, results, error);
}

int CommandAnalyze(const Suite& suite, const Arguments& arguments)
{
	AnalyzeOptions options;
	options.analyzers = arguments.Has("--analyzer") ? arguments.Values("--analyzer") : suite.analyzers;
	options.assetFilter = arguments.Value("--filter");
	options.branch = arguments.Value("--branch");
	int exitCode = kExitOk;
	for (const SuiteVariant* variant : SelectVariants(suite, arguments)) {
		ResultSet results;
		std::string error;
		if (!AnalyzeVariant(suite, *variant, options, results, error)) {
			std::cerr << std::format("{}: {}\n", variant->name, error);
			return kExitError;
		}
		const fs::path path = GetResultsPath(suite, variant->name);
		if (!WriteResultFiles(path, results, error)) {
			std::cerr << error << '\n';
			return kExitError;
		}
		std::cout << std::format("{}: {} metrics -> {}\n", variant->name, results.records.size(), path.string());

		// Budgets relative to a baseline only apply in compare.
		for (const BudgetFailure& failure : CheckBudgets(results, suite.budgets, nullptr)) {
			std::cout << std::format("  over budget: {}\n", failure.description);
			exitCode = kExitDiffers;
		}
	}
	return exitCode;
}

int CommandCapture(const Suite& suite, const Arguments& arguments, bool transitionOnly = false)
{
	CaptureOptions options;
	options.transitionOnly = transitionOnly;
	options.captureTool = GetExecutableDirectory() / "CLodCapture.exe";
	options.force = arguments.Has("--force");
	for (const SuiteVariant* variant : SelectVariants(suite, arguments)) {
		std::string error;
		if (!CaptureVariant(suite, *variant, options, error)) {
			std::cerr << std::format("{}: {}\n", variant->name, error);
			return kExitError;
		}
		std::cout << std::format("{}: captured\n", variant->name);
	}
	return kExitOk;
}

int CommandView(const Suite& suite, const Arguments& arguments)
{
	const std::vector<const SuiteVariant*> variants = SelectVariants(suite, arguments);
	if (variants.empty() || suite.assets.empty()) {
		std::cerr << "the suite needs a variant and an asset to view\n";
		return kExitError;
	}
	const SuiteAsset* asset = &suite.assets.front();
	if (const std::string name = arguments.Value("--asset"); !name.empty()) {
		const auto found = std::ranges::find(suite.assets, name, &SuiteAsset::name);
		if (found == suite.assets.end()) {
			std::cerr << std::format("the suite has no asset '{}'\n", name);
			return kExitError;
		}
		asset = &*found;
	}
	ViewerOptions options;
	options.captureTool = GetExecutableDirectory() / "CLodCapture.exe";
	options.script = arguments.Value("--script");
	options.partFilter = arguments.Value("--parts");
	if (const std::string size = arguments.Value("--size"); !size.empty()) {
		if (std::sscanf(size.c_str(), "%ux%u", &options.width, &options.height) != 2 || options.width == 0u || options.height == 0u) {
			std::cerr << std::format("--size wants WIDTHxHEIGHT, not '{}'\n", size);
			return kExitError;
		}
	}
	std::string error;
	if (!RunViewer(suite, *variants.front(), *asset, options, error)) {
		std::cerr << std::format("{}: {}\n", variants.front()->name, error);
		return kExitError;
	}
	return kExitOk;
}

int CommandViewRoot(const Arguments& arguments)
{
	if (arguments.positional.empty()) {
		throw std::runtime_error("view-root needs a root's cache file or directory");
	}
	const std::vector<fs::path> paths = FindCacheMetadata(arguments.positional.front());
	if (paths.size() != 1u) {
		std::cerr << std::format("'{}' holds {} caches; name one root\n", arguments.positional.front(), paths.size());
		return kExitError;
	}
	ViewerOptions options;
	options.captureTool = GetExecutableDirectory() / "CLodCapture.exe";
	options.script = arguments.Value("--script");
	if (const std::string size = arguments.Value("--size"); !size.empty()) {
		if (std::sscanf(size.c_str(), "%ux%u", &options.width, &options.height) != 2 || options.width == 0u || options.height == 0u) {
			std::cerr << std::format("--size wants WIDTHxHEIGHT, not '{}'\n", size);
			return kExitError;
		}
	}
	const fs::path workspace = arguments.Value("--workspace", (fs::current_path() / "clod_eval" / "roots").string());
	std::string error;
	if (!RunRootViewer(paths.front(), workspace / paths.front().parent_path().filename(), options, error)) {
		std::cerr << error << '\n';
		return kExitError;
	}
	return kExitOk;
}

int CommandScore(const Suite& suite, const Arguments& arguments, bool transitionOnly = false)
{
	for (const SuiteVariant* variant : SelectVariants(suite, arguments)) {
		ResultSet results;
		std::string error;
		if (!ScoreVariant(suite, *variant, transitionOnly, results, error)) {
			std::cerr << std::format("{}: {}\n", variant->name, error);
			return kExitError;
		}
		fs::path path = GetScoreResultsPath(suite, variant->name);
		if (transitionOnly) {
			path.replace_filename(variant->name + ".transition.jsonl");
		}
		if (!WriteResultFiles(path, results, error)) {
			std::cerr << error << '\n';
			return kExitError;
		}
		std::cout << std::format("{}: {} metrics -> {}\n", variant->name, results.records.size(), path.string());
	}
	return kExitOk;
}

int CommandReport(const Suite& suite)
{
	fs::path path;
	std::string error;
	if (!WriteReport(suite, path, error)) {
		std::cerr << error << '\n';
		return kExitError;
	}
	std::cout << std::format("report -> {}\n", path.string());
	return kExitOk;
}

// Every merge root under a cache tree, through the hierarchy.merge analyzer, in parallel: the results files, a table
// with one row per root, and the totals and worst roots for each issue.
// What each branch of every forked mesh in a tree draws at a range of cuts (object-space error), from the metadata and
// pages alone: triangles in the triangle branch; triangles and voxel cubes in the voxel branch.
int CommandForkCensus(const Arguments& arguments)
{
	if (arguments.positional.empty() || !arguments.Has("--out")) {
		throw std::runtime_error("fork-census needs a cache tree (or its clod directory) and --out");
	}
	const auto started = std::chrono::steady_clock::now();
	std::vector<fs::path> paths;
	std::error_code error;
	for (fs::recursive_directory_iterator it(arguments.positional.front(), error), end; !error && it != end; it.increment(error)) {
		if (it->is_regular_file() && it->path().extension() == L".clodmeta") paths.push_back(it->path());
	}
	static constexpr float kCuts[] = { 0.5f, 2.0f, 8.0f, 32.0f, 128.0f, 512.0f };
	constexpr size_t kCutCount = std::size(kCuts);
	struct Row {
		bool opened = false, forked = false;
		std::string name;
		uint32_t groups = 0, triangleOnly = 0, voxelOnly = 0, shadows = 0;
		uint64_t triangles[kCutCount]{}, voxelTriangles[kCutCount]{}, voxelCubes[kCutCount]{};
	};
	std::vector<Row> rows(paths.size());
	const auto drawn = [](const CachedMesh& view, float cut, uint64_t& triangles, uint64_t& cubes) {
		const ClusterLODPrebuiltData& prebuilt = view.Prebuilt();
		for (uint32_t index = 0; index < prebuilt.groups.size(); ++index) {
			const ClusterLODGroup& group = prebuilt.groups[index];
			if (!(group.bounds.error > cut)) continue;
			for (uint32_t s = 0; s < group.segmentCount; ++s) {
				const ClusterLODGroupSegment& segment = prebuilt.segments[group.firstSegment + s];
				if (segment.refinedGroup >= 0 && prebuilt.groups[static_cast<uint32_t>(segment.refinedGroup)].bounds.error > cut) continue;
				if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) == 0u) {
					view.VisitSegmentMeshlets(index, segment, [&](const CLodCache::TrianglePage&, const CLodCache::TriangleMeshlet& meshlet) {
						triangles += meshlet.triangleCount;
					});
					continue;
				}
				uint32_t meshPage = 0;
				CLodCache::VoxelPage page;
				if (!CLodCache::ResolveSegmentMeshPage(prebuilt, index, segment, meshPage) || !CLodCache::VoxelPage::Parse(view.Page(meshPage), page)) continue;
				for (uint32_t c = segment.firstMeshletInPage; c < segment.firstMeshletInPage + segment.meshletCount && c < page.ClusterCount(); ++c) {
					cubes += page.Cluster(c).cubeCount;
				}
			}
		}
	};
	std::atomic<size_t> next{ 0u };
	const auto work = [&]() {
		for (size_t index = next.fetch_add(1u); index < paths.size(); index = next.fetch_add(1u)) {
			std::string openError;
			const std::unique_ptr<CachedMesh> mesh = CachedMesh::Open(paths[index], openError);
			if (!mesh) continue;
			Row& row = rows[index];
			row.opened = true;
			row.forked = mesh->IsForked();
			if (!row.forked) continue;
			row.name = mesh->Prebuilt().cacheSource.sourceIdentifier + " " + mesh->Name();
			for (const ClusterLODGroup& group : mesh->Prebuilt().groups) {
				++row.groups;
				row.triangleOnly += (group.flags & CLOD_GROUP_FLAG_BRANCH_TRIANGLE) != 0u ? 1u : 0u;
				row.voxelOnly += (group.flags & CLOD_GROUP_FLAG_BRANCH_VOXEL) != 0u ? 1u : 0u;
				row.shadows += (group.flags & CLOD_GROUP_FLAG_BRANCH_SHADOW) != 0u ? 1u : 0u;
			}
			const auto triangleView = mesh->BranchView(CLOD_BRANCH_TRIANGLE, "");
			const auto voxelView = mesh->BranchView(CLOD_BRANCH_VOXEL, "");
			for (size_t c = 0; c < kCutCount; ++c) {
				uint64_t unusedCubes = 0;
				drawn(*triangleView, kCuts[c], row.triangles[c], unusedCubes);
				drawn(*voxelView, kCuts[c], row.voxelTriangles[c], row.voxelCubes[c]);
			}
		}
	};
	const uint32_t threadCount = arguments.Has("--threads") ? static_cast<uint32_t>(std::stoul(arguments.Value("--threads")))
		: std::max(1u, std::thread::hardware_concurrency());
	std::vector<std::thread> workers;
	for (uint32_t t = 1; t < threadCount; ++t) workers.emplace_back(work);
	work();
	for (std::thread& worker : workers) worker.join();

	std::ofstream csv(arguments.Value("--out"));
	csv << "path,mesh,groups,triangle_only,voxel_only,shadows";
	for (const float cut : kCuts) csv << std::format(",tris_{0:g},voxel_tris_{0:g},voxel_cubes_{0:g}", cut);
	csv << '\n';
	uint64_t opened = 0, forked = 0, sum[3][kCutCount]{};
	for (const Row& row : rows) {
		opened += row.opened ? 1u : 0u;
		if (!row.forked) continue;
		++forked;
		std::string name = row.name;
		std::ranges::replace(name, ',', ';');
		csv << std::format("{},{},{},{},{},{}", paths[&row - rows.data()].generic_string(), name, row.groups, row.triangleOnly, row.voxelOnly, row.shadows);
		for (size_t c = 0; c < kCutCount; ++c) {
			csv << std::format(",{},{},{}", row.triangles[c], row.voxelTriangles[c], row.voxelCubes[c]);
			sum[0][c] += row.triangles[c];
			sum[1][c] += row.voxelTriangles[c];
			sum[2][c] += row.voxelCubes[c];
		}
		csv << '\n';
	}
	std::cout << std::format("{} caches, {} forked ({:.1f} s) -> {}\n", opened, forked,
		std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(), arguments.Value("--out"));
	std::cout << "      cut   triangle branch   voxel branch: triangles + cubes\n";
	for (size_t c = 0; c < kCutCount; ++c) {
		std::cout << std::format("{:9g} {:17} {:14} + {}\n", kCuts[c], sum[0][c], sum[1][c], sum[2][c]);
	}
	return kExitOk;
}

int CommandMergeCensus(const Arguments& arguments)
{
	if (arguments.positional.empty() || !arguments.Has("--out")) {
		throw std::runtime_error("merge-census needs a cache tree (or its clod directory) and --out");
	}
	const auto started = std::chrono::steady_clock::now();
	// Roots are the merged meshes, which share one directory of the tree (CLodCache::kMergedMeshPrimPath): the tree holds
	// tens of thousands of other caches.
	std::vector<MeshToAnalyze> roots;
	std::error_code error;
	uint32_t staleRoots = 0;
	fs::path directory = arguments.positional.front();
	if (fs::is_directory(directory / "clod" / "merged")) directory /= fs::path("clod") / "merged";
	else if (fs::is_directory(directory / "merged")) directory /= "merged";
	for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
		if (!it->is_regular_file() || it->path().extension() != L".clodmeta") {
			continue;
		}
		// A root left behind by an older schema (its region now builds under another hash) is skipped, not fatal.
		const CLodCache::OpenedMetadata opened = CLodCache::OpenMetadataFile(it->path());
		if (opened.status != CLodCache::OpenMetadataStatus::Ok || opened.data.prebuiltData.externalParts.empty()) {
			++staleRoots;
			continue;
		}
		roots.push_back({ it->path(), opened.data.prebuiltData.cacheSource.sourceIdentifier });
	}
	if (staleRoots != 0u) {
		std::cerr << std::format("skipped {} roots that this tool cannot read (older schema or damaged)\n", staleRoots);
	}
	std::ranges::sort(roots, {}, &MeshToAnalyze::asset);
	if (roots.empty()) {
		std::cerr << std::format("no merged meshes (clod/merged/*.clodmeta) under '{}'\n", arguments.positional.front());
		return kExitError;
	}
	AnalyzeOptions options;
	options.analyzers = { "hierarchy.merge" };
	options.threads = arguments.Has("--threads") ? static_cast<uint32_t>(std::stoul(arguments.Value("--threads"))) : 0u;
	options.mergeReachCut = arguments.Has("--reach-cut") ? std::stof(arguments.Value("--reach-cut")) : options.mergeReachCut;
	options.branch = arguments.Value("--branch");
	ResultSet results;
	std::string analyzeError;
	if (!AnalyzeMeshes(roots, "census", {}, options, results, analyzeError)) {
		std::cerr << analyzeError << '\n';
		return kExitError;
	}
	const fs::path out = arguments.Value("--out");
	if (!WriteResultFiles(fs::path(out).concat(".jsonl"), results, analyzeError)) {
		std::cerr << analyzeError << '\n';
		return kExitError;
	}
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

	// One row per root: untagged metrics and the issues.
	const std::vector<std::string> columns = { "merge.portals", "merge.parts", "merge.nodes", "merge.levels", "merge.node_triangles",
		"merge.input_triangles", "merge.root_error", "merge.problems", "merge.issue.unreachable_nodes", "merge.issue.unreachable_triangles",
		"merge.issue.unreachable_levels", "merge.issue.root_draws", "merge.issue.reach_nodes", "merge.issue.reach_portals",
		"merge.issue.reach_coarsest_portals", "merge.issue.reach_triangles", "merge.issue.reach_node_triangles", "merge.issue.reach_part_triangles", "merge.issue.always_open_portals",
		"merge.issue.always_open_coarsest_portals", "merge.issue.root_portals", "merge.issue.dead_portals", "merge.issue.dead_portal_triangles",
		"merge.issue.hardly_drawn_nodes", "merge.issue.hardly_drawn_triangles", "merge.issue.sibling_spread", "merge.issue.sibling_spread_max",
		"merge.issue.portal_depth_mean", "merge.issue.portal_depth_max" };
	std::map<std::string, std::map<std::string, double>> rows;
	for (const MetricRecord& record : results.records) {
		if (record.mesh == kAssetRollupMesh || std::ranges::find(columns, record.id) == columns.end()) {
			continue;
		}
		if (!record.tags.empty() && !record.tags.contains("reach_cut")) {
			continue;
		}
		rows[record.asset][record.id] = record.value;
	}
	{
		std::ofstream table(fs::path(out).concat(".roots.csv"));
		table << "root";
		for (const std::string& column : columns) {
			table << ',' << column.substr(column.find_last_of('.') + 1u);
		}
		table << '\n';
		for (const auto& [root, values] : rows) {
			table << root;
			for (const std::string& column : columns) {
				const auto found = values.find(column);
				table << ',' << (found != values.end() ? std::format("{:g}", found->second) : std::string());
			}
			table << '\n';
		}
	}

	const auto value = [&](const std::map<std::string, double>& row, const std::string& id) {
		const auto found = row.find(id);
		return found != row.end() ? found->second : 0.0;
	};
	const auto total = [&](const std::string& id) {
		double sum = 0.0;
		for (const auto& [root, row] : rows) {
			sum += value(row, id);
		}
		return sum;
	};
	const auto rootsWith = [&](const std::string& id) {
		return std::ranges::count_if(rows, [&](const auto& entry) { return value(entry.second, id) > 0.0; });
	};
	const auto percent = [](double part, double whole) { return whole > 0.0 ? 100.0 * part / whole : 0.0; };
	const double portals = total("merge.portals"), nodes = total("merge.nodes"), nodeTriangles = total("merge.node_triangles");
	std::cout << std::format("merge census: {} roots in {:.1f} s ({} threads, {} part references), reach cut {:g}\n", rows.size(), seconds,
		options.threads == 0u ? std::thread::hardware_concurrency() : options.threads, total("merge.parts"), options.mergeReachCut);
	std::cout << std::format("  {:.0f} portals, {:.0f} nodes, {:.0f} node triangles for {:.0f} input triangles; {} roots with problems\n", portals, nodes,
		nodeTriangles, total("merge.input_triangles"), rootsWith("merge.problems"));
	std::cout << std::format("  at the reach cut (each region from its farthest distance): {:.0f} instances ({:.1f}% of portals), {:.0f} of them coarsest; "
		"{:.0f} node draws; {:.0f} triangles, {:.0f} from nodes and {:.0f} from parts\n", total("merge.issue.reach_portals"),
		percent(total("merge.issue.reach_portals"), portals), total("merge.issue.reach_coarsest_portals"), total("merge.issue.reach_nodes"),
		total("merge.issue.reach_triangles"), total("merge.issue.reach_node_triangles"), total("merge.issue.reach_part_triangles"));
	const auto noNodes = std::ranges::count_if(rows, [&](const auto& entry) { return value(entry.second, "merge.issue.reach_nodes") == 0.0; });
	std::cout << std::format("  {} roots draw no node geometry even from their farthest distance; {} roots ever draw as their root alone\n", noNodes,
		rootsWith("merge.issue.root_draws"));
	std::cout << "\n  issue                                   total   share          roots\n";
	const auto line = [&](const char* name, const std::string& id, double whole, const char* of) {
		std::cout << std::format("  {:<36} {:>10.0f} {:>6.1f}% {:<6} {:>6}\n", name, total(id), percent(total(id), whole), of, rootsWith(id));
	};
	line("unreachable nodes", "merge.issue.unreachable_nodes", nodes, "nodes");
	line("unreachable node triangles", "merge.issue.unreachable_triangles", nodeTriangles, "tris");
	line("always-open portals", "merge.issue.always_open_portals", portals, "ports");
	line("  of them coarsest at reach", "merge.issue.always_open_coarsest_portals", portals, "ports");
	line("portals at the top (always open)", "merge.issue.root_portals", portals, "ports");
	line("dead portals (never open)", "merge.issue.dead_portals", portals, "ports");
	line("  their parts' triangles, copied", "merge.issue.dead_portal_triangles", total("merge.input_triangles"), "input");
	line("hardly drawn nodes", "merge.issue.hardly_drawn_nodes", nodes, "nodes");
	line("hardly drawn node triangles", "merge.issue.hardly_drawn_triangles", nodeTriangles, "tris");
	{
		std::vector<double> spreads, depths, levels;
		for (const auto& [root, row] : rows) {
			spreads.push_back(value(row, "merge.issue.sibling_spread"));
			depths.push_back(value(row, "merge.issue.portal_depth_mean"));
			levels.push_back(value(row, "merge.levels"));
		}
		const auto median = [](std::vector<double> v) { std::ranges::sort(v); return v.empty() ? 0.0 : v[v.size() / 2u]; };
		const auto maximum = [](const std::vector<double>& v) { return v.empty() ? 0.0 : *std::ranges::max_element(v); };
		std::cout << std::format("  sibling spread (finest sibling opened early by): median root {:.2f}x, worst root {:.2f}x\n", median(spreads), maximum(spreads));
		std::cout << std::format("  levels: median {:.0f}, max {:.0f}; mean portal depth: median {:.2f}, max {:.2f}\n", median(levels), maximum(levels),
			median(depths), maximum(depths));
	}
	const auto worst = [&](const char* title, const std::string& id) {
		std::vector<std::pair<double, std::string>> ranked;
		for (const auto& [root, row] : rows) {
			ranked.push_back({ value(row, id), root });
		}
		std::ranges::sort(ranked, std::greater<>());
		std::cout << std::format("\n  worst by {}:\n", title);
		for (size_t i = 0; i < std::min<size_t>(ranked.size(), 5u) && ranked[i].first > 0.0; ++i) {
			const auto& row = rows[ranked[i].second];
			std::cout << std::format("    {:>10.4g}  {}  ({:.0f} portals, {:.0f} nodes, {:.0f} levels)\n", ranked[i].first, ranked[i].second,
				value(row, "merge.portals"), value(row, "merge.nodes"), value(row, "merge.levels"));
		}
	};
	worst("unreachable node triangles", "merge.issue.unreachable_triangles");
	worst("always-open coarsest portals", "merge.issue.always_open_coarsest_portals");
	worst("dead portals", "merge.issue.dead_portals");
	worst("hardly drawn node triangles", "merge.issue.hardly_drawn_triangles");
	worst("sibling spread", "merge.issue.sibling_spread");
	std::cout << std::format("\n  -> {}.jsonl, .csv and .roots.csv\n", out.string());
	return kExitOk;
}

int CommandMergeReport(const Arguments& arguments)
{
	if (arguments.positional.empty()) {
		throw std::runtime_error("merge-report needs a cache file or directory");
	}
	nlohmann::json all = nlohmann::json::array();
	uint32_t roots = 0;
	for (const fs::path& path : FindCacheMetadata(arguments.positional.front())) {
		std::string error;
		const std::unique_ptr<CachedMesh> mesh = CachedMesh::Open(path, error);
		if (!mesh) {
			std::cerr << error << '\n';
			return kExitError;
		}
		// A forked root reports each branch on its own.
		std::vector<std::unique_ptr<CachedMesh>> views;
		if (mesh->IsForked()) {
			views.push_back(mesh->BranchView(CLOD_BRANCH_TRIANGLE, "#triangle"));
			views.push_back(mesh->BranchView(CLOD_BRANCH_VOXEL, "#voxel"));
		}
		std::vector<const CachedMesh*> reported;
		if (views.empty()) reported.push_back(mesh.get());
		for (const auto& view : views) reported.push_back(view.get());
		bool isRoot = false;
		for (const CachedMesh* view : reported) {
			const std::unique_ptr<MergeHierarchy> hierarchy = ReadMergeHierarchy(*view, true);
			if (!hierarchy) {
				continue;
			}
			isRoot = true;
			if (arguments.Has("--json") || arguments.Has("--out")) {
				all.push_back(MergeReportJson(*hierarchy, *view));
			}
			if (!arguments.Has("--json")) {
				std::cout << FormatMergeReport(*hierarchy, *view) << '\n';
			}
		}
		roots += isRoot ? 1u : 0u;
	}
	if (roots == 0u) {
		std::cerr << std::format("no merge-hierarchy roots at '{}'\n", arguments.positional.front());
		return kExitError;
	}
	if (arguments.Has("--out")) {
		std::ofstream(arguments.Value("--out")) << all.dump(1, '\t') << '\n';
	}
	if (arguments.Has("--json")) {
		std::cout << all.dump(2) << '\n';
	}
	return kExitOk;
}

int CommandInspect(const Arguments& arguments)
{
	if (arguments.positional.empty()) {
		throw std::runtime_error("inspect needs a cache file or directory");
	}
	const std::vector<fs::path> paths = FindCacheMetadata(arguments.positional.front());
	if (paths.empty()) {
		std::cerr << std::format("no caches at '{}'\n", arguments.positional.front());
		return kExitError;
	}
	const int group = arguments.Has("--group") ? std::stoi(arguments.Value("--group")) : -1;
	nlohmann::json all = nlohmann::json::array();
	for (const fs::path& path : paths) {
		std::string error;
		nlohmann::json description = DescribeCache(path, group, error);
		if (!error.empty()) {
			std::cerr << error << '\n';
			return kExitError;
		}
		if (arguments.Has("--json")) {
			all.push_back(std::move(description));
		}
		else {
			std::cout << FormatCacheDescription(description);
			if (std::string openError; const auto mesh = CachedMesh::Open(path, openError)) {
				if (IsMergeRoot(mesh->Prebuilt())) {
					std::cout << "  a merge-hierarchy root: depth 0 holds its portal proxies and page-less children groups. merge-report describes the merge tree.\n";
				}
			}
		}
	}
	if (arguments.Has("--json")) {
		std::cout << all.dump(2) << '\n';
	}
	return kExitOk;
}

// Every voxel group of every mesh as one row, for looking at what predicts what.
int CommandVoxelFidelity(const Arguments& arguments)
{
	if (arguments.positional.empty() || !arguments.Has("--out")) {
		throw std::runtime_error("voxel-fidelity needs a cache file or directory and --out");
	}
	if (arguments.Has("--group")) {
		// One group's cells, per segment, of meshes whose identifier contains --filter: width, refined group, x, y, z.
		const uint32_t group = static_cast<uint32_t>(std::stoi(arguments.Value("--group")));
		const std::string filter = arguments.Value("--filter");
		std::ofstream cellsOutput(arguments.Value("--out"));
		for (const fs::path& path : FindCacheMetadata(arguments.positional.front())) {
			std::string error;
			const std::unique_ptr<CachedMesh> mesh = CachedMesh::Open(path, error);
			if (!mesh || mesh->Prebuilt().cacheSource.sourceIdentifier.find(filter) == std::string::npos || group >= mesh->Prebuilt().groups.size()) {
				continue;
			}
			for (const VoxelSegmentCells& segment : ReadVoxelSegmentCells(*mesh, group)) {
				for (const auto& cell : segment.cellMins) {
					cellsOutput << std::format("{},{},{},{},{}\n", segment.width, segment.refinedGroup, cell[0], cell[1], cell[2]);
				}
			}
		}
		return kExitOk;
	}
	std::ofstream output(arguments.Value("--out"));
	output << "mesh,identifier,group,depth,level,width,enter,leave,cells,touched,triangles,coverage_ratio,holes,excess,worst_holes,kept,area_per_cell,cell_coherence,neighbour_coherence,neighbour_fill,deficit2,deficit4,deficit8,solid_deficit4,solid_share4,block_source,block_voxel,block_deficit,wall_deficit,wall_share,wall_missed\n";
	for (const fs::path& path : FindCacheMetadata(arguments.positional.front())) {
		std::string error;
		const std::unique_ptr<CachedMesh> mesh = CachedMesh::Open(path, error);
		if (!mesh) {
			std::cerr << error << '\n';
			return kExitError;
		}
		for (const VoxelGroupFidelity& row : MeasureVoxelFidelity(*mesh)) {
			output << std::format("\"{}\",\"{}\",{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n", mesh->Name(), mesh->Prebuilt().cacheSource.sourceIdentifier.substr(0, 0),
				row.group, row.depth, row.voxelLevel, row.width, row.enterError, row.leaveError, row.cells, row.touchedCells, row.sourceTriangles, row.coverageRatio, row.holes, row.excess, row.worstHoles,
				row.keptFraction, row.areaPerCell, row.cellCoherence, row.neighbourhoodCoherence, row.neighbourhoodFill, row.deficit2, row.deficit4, row.deficit8, row.solidDeficit4, row.solidShare4, row.blockSource, row.blockVoxel, row.blockDeficit, row.wallDeficit, row.wallShare, row.wallMissed);
		}
	}
	return kExitOk;
}

int CommandAnalyzeCache(const Arguments& arguments)
{
	if (arguments.positional.empty() || !arguments.Has("--out")) {
		throw std::runtime_error("analyze-cache needs a cache file or directory and --out");
	}
	const std::string asset = arguments.Value("--asset", fs::path(arguments.positional.front()).stem().string());
	std::vector<MeshToAnalyze> meshes;
	for (const fs::path& path : FindCacheMetadata(arguments.positional.front())) {
		meshes.push_back({ path, asset });
	}
	AnalyzeOptions options;
	options.analyzers = arguments.Values("--analyzer");
	options.threads = arguments.Has("--threads") ? static_cast<uint32_t>(std::stoul(arguments.Value("--threads"))) : 0u;
	options.branch = arguments.Value("--branch");
	ResultSet results;
	std::string error;
	if (!AnalyzeMeshes(meshes, asset, {}, options, results, error) || !WriteResultFiles(arguments.Value("--out"), results, error)) {
		std::cerr << error << '\n';
		return kExitError;
	}
	std::cout << std::format("{} metrics from {} caches -> {}\n", results.records.size(), meshes.size(), arguments.Value("--out"));
	return kExitOk;
}

int CommandCompare(const Arguments& arguments)
{
	if (arguments.positional.size() != 2u) {
		throw std::runtime_error("compare needs two results files");
	}
	ResultSet a;
	ResultSet b;
	std::string error;
	if (!ReadResults(arguments.positional[0], a, error) || !ReadResults(arguments.positional[1], b, error)) {
		std::cerr << error << '\n';
		return kExitError;
	}
	std::vector<Budget> budgets;
	if (arguments.Has("--budget")) {
		std::ifstream input(arguments.Value("--budget"));
		if (!input) {
			std::cerr << std::format("cannot open budget file '{}'\n", arguments.Value("--budget"));
			return kExitError;
		}
		nlohmann::json json;
		input >> json;
		budgets = ParseBudgets(json);
	}
	const double threshold = arguments.Has("--threshold") ? std::stod(arguments.Value("--threshold")) : 0.0;
	const Comparison comparison = Compare(a, b, threshold, budgets);

	if (arguments.Has("--json")) {
		std::cout << ToJson(comparison).dump(2) << '\n';
		return ComparisonExitCode(comparison);
	}

	for (const std::string& problem : comparison.notComparable) {
		std::cout << std::format("NOT COMPARABLE: {}\n", problem);
	}
	std::cout << std::format("{} vs {}: {} metrics compared, {} differ\n", a.build, b.build, comparison.compared, comparison.differences.size());
	// A metric with an asset rollup is shown once, as the rollup, unless asked for every mesh.
	const auto rollupKey = [](const MetricRecord& record) {
		MetricRecord rolled = record;
		rolled.mesh = std::string(kAssetRollupMesh);
		return rolled.Key();
	};
	std::set<std::string> rollups;
	for (const MetricRecord& record : a.records) {
		if (record.mesh == kAssetRollupMesh) {
			rollups.insert(record.Key());
		}
	}
	for (const MetricDifference& difference : comparison.differences) {
		if (!arguments.Has("--all") && difference.a.mesh != kAssetRollupMesh && rollups.contains(rollupKey(difference.a))) {
			continue;
		}
		const char* verdict = difference.regressed ? "worse " : (difference.improved ? "better" : "      ");
		std::string label = difference.a.id;
		for (const auto& [name, value] : difference.a.tags) {
			label += std::format(" {}={}", name, value);
		}
		std::cout << std::format("  {} {:<60} {:>14.6g} -> {:>14.6g} ({:+8.2f}%) {} {}\n",
			verdict, label, difference.a.value, difference.b.value, difference.relative * 100.0,
			difference.a.asset, difference.a.mesh);
	}
	for (const BudgetFailure& failure : comparison.budgetFailures) {
		std::cout << std::format("  over budget: {}\n", failure.description);
	}
	if (!comparison.onlyInA.empty() || !comparison.onlyInB.empty()) {
		std::cout << std::format("  {} metrics only in {}, {} only in {}\n", comparison.onlyInA.size(), a.build, comparison.onlyInB.size(), b.build);
	}
	return ComparisonExitCode(comparison);
}

int Run(int argc, char** argv)
{
	if (argc < 2) {
		std::cerr << kUsage;
		return kExitError;
	}
	const std::string command = argv[1];
	const Arguments arguments = ParseArguments(argc, argv);

	if (command == "list-analyzers") {
		for (const auto& analyzer : AllAnalyzers()) {
			std::cout << std::format("{:<24} v{}  {}\n", analyzer->Info().id, analyzer->Info().version, analyzer->Info().description);
		}
		return kExitOk;
	}
	if (command == "inspect") {
		return CommandInspect(arguments);
	}
	if (command == "merge-census") {
		return CommandMergeCensus(arguments);
	}
	if (command == "fork-census") {
		return CommandForkCensus(arguments);
	}
	if (command == "merge-report") {
		return CommandMergeReport(arguments);
	}
	if (command == "view-root") {
		return CommandViewRoot(arguments);
	}
	if (command == "analyze-cache") {
		return CommandAnalyzeCache(arguments);
	}
	if (command == "voxel-fidelity") {
		return CommandVoxelFidelity(arguments);
	}
	if (command == "compare") {
		return CommandCompare(arguments);
	}
	if (command == "build" || command == "analyze" || command == "run") {
		if (arguments.positional.empty()) {
			throw std::runtime_error(std::format("{} needs a suite file", command));
		}
		const Suite suite = LoadSuite(arguments.positional.front(), arguments.Value("--workspace"));
		if (command != "analyze") {
			if (const int exitCode = CommandBuild(suite, arguments); exitCode != kExitOk || command == "build") {
				return exitCode;
			}
		}
		return CommandAnalyze(suite, arguments);
	}
	if (command == "capture" || command == "score" || command == "report" || command == "full" || command == "transition" || command == "view") {
		if (arguments.positional.empty()) {
			throw std::runtime_error(std::format("{} needs a suite file", command));
		}
		const Suite suite = LoadSuite(arguments.positional.front(), arguments.Value("--workspace"));
		if (command == "view") {
			return CommandView(suite, arguments);
		}
		if (command == "capture") {
			return CommandCapture(suite, arguments);
		}
		if (command == "transition") {
			if (const int exitCode = CommandCapture(suite, arguments, true); exitCode != kExitOk) {
				return exitCode;
			}
			return CommandScore(suite, arguments, true);
		}
		if (command == "score") {
			return CommandScore(suite, arguments);
		}
		if (command == "report") {
			return CommandReport(suite);
		}
		if (const int exitCode = CommandBuild(suite, arguments); exitCode != kExitOk) {
			return exitCode;
		}
		// A failed budget is reported but does not stop the images being made: they help explain why it failed.
		const int analyzeExit = CommandAnalyze(suite, arguments);
		if (analyzeExit == kExitError) {
			return analyzeExit;
		}
		if (const int exitCode = CommandCapture(suite, arguments); exitCode != kExitOk) {
			return exitCode;
		}
		if (const int exitCode = CommandScore(suite, arguments); exitCode != kExitOk) {
			return exitCode;
		}
		if (const int exitCode = CommandReport(suite); exitCode != kExitOk) {
			return exitCode;
		}
		return analyzeExit;
	}
	if (command == "help" || command == "--help") {
		std::cout << kUsage;
		return kExitOk;
	}
	std::cerr << std::format("unknown command '{}'\n\n{}", command, kUsage);
	return kExitError;
}
}

int main(int argc, char** argv)
{
	// stdout is this tool's output, and the cache libraries log routine progress at info.
	spdlog::set_level(spdlog::level::warn);
	try {
		return Run(argc, argv);
	}
	catch (const std::exception& error) {
		std::cerr << "error: " << error.what() << '\n';
		return kExitError;
	}
}
