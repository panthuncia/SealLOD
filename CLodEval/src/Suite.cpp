#include "Suite.h"

#include <algorithm>
#include <atomic>
#include <thread>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

#include "Analyzers/Analyzers.h"
#include "Core/Analyzer.h"
#include "Core/CachedMesh.h"
#include "Core/Process.h"

#include <BasicRenderer/Assets/CachePaths.h>

namespace clodeval {

namespace {
	namespace fs = std::filesystem;

	constexpr std::string_view kSettingPrefix = "BASICRENDERER_CLOD_";

	bool IsValidName(std::string_view name)
	{
		return !name.empty() && name.front() != '.' && std::ranges::all_of(name, [](unsigned char character) {
			return std::isalnum(character) != 0 || character == '-' || character == '_' || character == '.';
		});
	}

	uint64_t HashBytes(uint64_t hash, std::string_view bytes)
	{
		for (unsigned char byte : bytes) {
			hash ^= byte;
			hash *= 1099511628211ull;
		}
		hash ^= 0xFFu;
		hash *= 1099511628211ull;
		return hash;
	}

	// Identifies a file by name, size and time. Reading whole assets and binaries each run would be slow.
	uint64_t HashFileIdentity(uint64_t hash, const fs::path& path)
	{
		hash = HashBytes(hash, path.string());
		std::error_code error;
		const auto size = fs::file_size(path, error);
		hash = HashBytes(hash, error ? std::string("missing") : std::to_string(size));
		const auto time = fs::last_write_time(path, error);
		hash = HashBytes(hash, error ? std::string("missing") : std::to_string(time.time_since_epoch().count()));
		return hash;
	}

	std::string BuildKey(const Suite& suite, const SuiteVariant& variant, const fs::path& cacheTool)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashFileIdentity(hash, cacheTool);
		for (const SuiteAsset& asset : suite.assets) {
			hash = HashFileIdentity(hash, asset.path);
		}
		for (const auto& [name, value] : variant.settings) {
			hash = HashBytes(HashBytes(hash, name), value);
		}
		if (!variant.assetSettings.empty()) {
			hash = HashFileIdentity(hash, variant.assetSettings);
		}
		return std::format("{:016x}", hash);
	}

	std::string ReadTextFile(const fs::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		std::ostringstream text;
		text << input.rdbuf();
		return text.str();
	}

	std::string RunGit(const fs::path& directory, const std::string& arguments)
	{
		const std::string command = std::format("git -C \"{}\" {} 2>NUL", directory.string(), arguments);
		FILE* pipe = _popen(command.c_str(), "r");
		if (pipe == nullptr) {
			return {};
		}
		std::string output;
		char buffer[256];
		while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
			output += buffer;
		}
		_pclose(pipe);
		while (!output.empty() && std::isspace(static_cast<unsigned char>(output.back()))) {
			output.pop_back();
		}
		return output;
	}

	// The asset a mesh came from. Importers name a mesh's source by normalized path, but USD references
	// can name another file, so anything unmatched is grouped under that file's own name.
	// An identifier is the source's path plus any '#' build options, which assemblies have many of.
	std::string AssetForSource(const Suite& suite, const std::string& sourceIdentifier)
	{
		const std::string source = sourceIdentifier.substr(0, sourceIdentifier.find('#'));
		const auto lowered = [](std::string text) {
			std::ranges::transform(text, text.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
			return text;
		};
		for (const SuiteAsset& asset : suite.assets) {
			if (lowered(NormalizeCacheSourcePath(asset.path.string())) == lowered(source)) {
				return asset.name;
			}
		}
		return fs::path(source).stem().string();
	}
}

Suite LoadSuite(const fs::path& path, const fs::path& workspaceOverride)
{
	std::ifstream input(path);
	if (!input) {
		throw std::runtime_error(std::format("cannot open suite '{}'", path.string()));
	}
	nlohmann::json json;
	try {
		input >> json;
	}
	catch (const std::exception& error) {
		throw std::runtime_error(std::format("suite '{}': {}", path.string(), error.what()));
	}

	Suite suite;
	suite.path = fs::absolute(path);
	const fs::path base = suite.path.parent_path();
	suite.name = json.value("name", path.stem().string());

	const auto resolve = [&](const std::string& value) { return fs::weakly_canonical(base / fs::path(value)); };
	if (!workspaceOverride.empty()) {
		suite.workspace = fs::absolute(workspaceOverride);
	}
	else {
		suite.workspace = json.contains("workspace") ? base / json.at("workspace").get<std::string>() : fs::current_path() / "clod_eval" / suite.name;
	}

	for (const nlohmann::json& asset : json.at("assets")) {
		SuiteAsset parsed;
		parsed.path = resolve(asset.at("path").get<std::string>());
		parsed.name = asset.value("name", parsed.path.stem().string());
		if (!fs::exists(parsed.path)) {
			throw std::runtime_error(std::format("suite asset '{}' does not exist at '{}'", parsed.name, parsed.path.string()));
		}
		suite.assets.push_back(std::move(parsed));
	}
	if (suite.assets.empty()) {
		throw std::runtime_error("suite has no assets");
	}

	for (const nlohmann::json& variant : json.at("variants")) {
		SuiteVariant parsed;
		parsed.name = variant.at("name").get<std::string>();
		if (!IsValidName(parsed.name)) {
			throw std::runtime_error(std::format("variant name '{}' may contain only letters, digits, '-', '_' and '.'", parsed.name));
		}
		if (const auto settings = variant.find("settings"); settings != variant.end()) {
			for (const auto& [name, value] : settings->items()) {
				// A misspelt setting would silently build the wrong thing and report on it, so only known forms pass.
				if (!name.starts_with(kSettingPrefix)) {
					throw std::runtime_error(std::format("variant '{}' setting '{}' does not start with {}", parsed.name, name, kSettingPrefix));
				}
				parsed.settings[name] = value.is_string() ? value.get<std::string>() : value.dump();
			}
		}
		if (const auto assetSettings = variant.find("asset_settings"); assetSettings != variant.end()) {
			parsed.assetSettings = resolve(assetSettings->get<std::string>());
			if (!fs::exists(parsed.assetSettings)) {
				throw std::runtime_error(std::format("variant '{}' asset_settings '{}' does not exist", parsed.name, parsed.assetSettings.string()));
			}
		}
		suite.variants.push_back(std::move(parsed));
	}
	if (suite.variants.empty()) {
		throw std::runtime_error("suite has no variants");
	}

	if (const auto analyzers = json.find("analyzers"); analyzers != json.end()) {
		suite.analyzers = analyzers->get<std::vector<std::string>>();
	}
	if (const auto budgets = json.find("budgets"); budgets != json.end()) {
		suite.budgets = ParseBudgets(*budgets);
	}
	if (const auto capture = json.find("capture"); capture != json.end()) {
		SuiteCapture& parsed = suite.capture;
		parsed.enabled = true;
		if (const auto resolution = capture->find("resolution"); resolution != capture->end()) {
			parsed.width = resolution->at(0).get<uint32_t>();
			parsed.height = resolution->at(1).get<uint32_t>();
		}
		parsed.referenceScale = capture->value("reference_scale", parsed.referenceScale);
		parsed.accumulateFrames = capture->value("accumulate_frames", parsed.accumulateFrames);
		parsed.fovDegrees = capture->value("fov_degrees", parsed.fovDegrees);
		parsed.farRadii = capture->value("far_radii", parsed.farRadii);
		parsed.readyTimeoutSeconds = capture->value("ready_timeout_seconds", parsed.readyTimeoutSeconds);
		parsed.cornerNdc = capture->value("corner_ndc", parsed.cornerNdc);
		parsed.pagePoolSlabs = capture->value("page_pool_slabs", parsed.pagePoolSlabs);
		if (const auto frame = capture->find("frame"); frame != capture->end()) {
			parsed.hasFrame = true;
			for (size_t axis = 0; axis < 3u; ++axis) {
				parsed.frameCenter[axis] = frame->at("center").at(axis).get<double>();
			}
			parsed.frameRadius = frame->at("radius").get<double>();
		}
		parsed.distances = capture->value("distances", std::vector<double>{});
		parsed.cornerDistances = capture->value("corner_distances", std::vector<double>{});
		parsed.sweepDistances = capture->value("sweep_distances", std::vector<double>{});
		parsed.thresholds = capture->value("thresholds", std::vector<double>{});
		parsed.transition = capture->value("transition", parsed.transition);
		parsed.transitionFovDegrees = capture->value("transition_fov_degrees", parsed.transitionFovDegrees);
		parsed.transitionSteps = capture->value("transition_steps", parsed.transitionSteps);
		if (parsed.distances.empty() && parsed.cornerDistances.empty()) {
			throw std::runtime_error("a capture block needs distances or corner_distances");
		}
	}
	return suite;
}

BuildPaths GetBuildPaths(const Suite& suite, const SuiteVariant& variant)
{
	BuildPaths paths;
	paths.directory = suite.workspace / "builds" / variant.name;
	paths.cacheRoot = paths.directory / "cache";
	paths.manifest = paths.directory / "manifest.json";
	paths.buildRecord = paths.directory / "build.json";
	paths.log = paths.directory / "build.log";
	return paths;
}

fs::path GetResultsPath(const Suite& suite, const std::string& variantName)
{
	return suite.workspace / "results" / (variantName + ".jsonl");
}

BuildOutcome BuildVariant(const Suite& suite, const SuiteVariant& variant, const BuildOptions& options, std::string& outError)
{
	const BuildPaths paths = GetBuildPaths(suite, variant);
	if (!fs::exists(options.cacheTool)) {
		outError = std::format("cache tool not found at '{}'", options.cacheTool.string());
		return BuildOutcome::Failed;
	}
	const std::string key = BuildKey(suite, variant, options.cacheTool);

	std::error_code error;
	if (fs::exists(paths.buildRecord, error)) {
		try {
			const auto record = nlohmann::json::parse(ReadTextFile(paths.buildRecord));
			if (!options.force && record.value("build_key", "") == key && fs::exists(paths.manifest, error)) {
				return BuildOutcome::UpToDate;
			}
		}
		catch (const std::exception&) {
			// An unreadable record just means the build is redone.
		}
	}
	else if (fs::exists(paths.directory, error) && !fs::is_empty(paths.directory, error)) {
		// The directory is about to be cleared. Only do that to something this tool made.
		outError = std::format("'{}' exists and has no build record, so it was not made by CLodEval; refusing to replace it", paths.directory.string());
		return BuildOutcome::Failed;
	}

	fs::remove_all(paths.directory, error);
	if (error) {
		outError = std::format("could not clear '{}': {}", paths.directory.string(), error.message());
		return BuildOutcome::Failed;
	}
	fs::create_directories(paths.cacheRoot, error);
	if (error) {
		outError = std::format("could not create '{}': {}", paths.cacheRoot.string(), error.message());
		return BuildOutcome::Failed;
	}

	// Without an explicit config, the settings lookup walks up from the working directory and could
	// find some unrelated asset_settings.json. An empty one makes the build depend only on the suite.
	fs::path assetSettings = variant.assetSettings;
	if (assetSettings.empty()) {
		assetSettings = paths.directory / "no_asset_settings.json";
		std::ofstream(assetSettings) << "{ \"assets\": {} }\n";
	}

	ProcessRequest request;
	request.executable = options.cacheTool;
	request.workingDirectory = options.cacheTool.parent_path();
	request.logPath = paths.log;
	request.arguments.push_back("--cache-root=" + paths.cacheRoot.string());
	request.arguments.push_back("--manifest=" + paths.manifest.string());
	for (const SuiteAsset& asset : suite.assets) {
		request.arguments.push_back(asset.path.string());
	}
	request.clearEnvironmentPrefixes = { std::string(kSettingPrefix) };
	request.environment = variant.settings;
	request.environment["SARP_ASSET_SETTINGS_CONFIG"] = assetSettings.string();
	request.environment["SARP_CACHE_ROOT"] = paths.cacheRoot.string();
	request.pathPrepend = { options.cacheTool.parent_path() };

	const auto start = std::chrono::steady_clock::now();
	const int exitCode = RunProcess(request, outError);
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	if (exitCode != 0) {
		if (outError.empty()) {
			outError = std::format("CLodCacheTool exited with {}; see '{}'", exitCode, paths.log.string());
		}
		return BuildOutcome::Failed;
	}
	if (!fs::exists(paths.manifest, error)) {
		outError = std::format("CLodCacheTool succeeded but wrote no manifest; see '{}'", paths.log.string());
		return BuildOutcome::Failed;
	}

	std::ofstream(paths.buildRecord) << nlohmann::json{
		{ "build_key", key },
		{ "variant", variant.name },
		{ "settings", variant.settings },
		{ "asset_settings", assetSettings.string() },
		{ "tool", options.cacheTool.string() },
		{ "build_seconds", seconds },
		{ "provenance", ProvenanceJson() },
	}.dump(2) << '\n';
	return BuildOutcome::Built;
}

bool AnalyzeMeshes(
	const std::vector<MeshToAnalyze>& meshes,
	const std::string& buildName,
	const std::string& buildKey,
	const AnalyzeOptions& options,
	ResultSet& outResults,
	std::string& outError)
{
	std::vector<const IAnalyzer*> analyzers;
	if (options.analyzers.empty()) {
		for (const auto& analyzer : AllAnalyzers()) {
			analyzers.push_back(analyzer.get());
		}
	}
	else {
		for (const std::string& id : options.analyzers) {
			const IAnalyzer* analyzer = FindAnalyzer(id);
			if (analyzer == nullptr) {
				outError = std::format("unknown analyzer '{}'; see list-analyzers", id);
				return false;
			}
			analyzers.push_back(analyzer);
		}
	}

	outResults.build = buildName;
	outResults.buildKey = buildKey;
	outResults.provenance = ProvenanceJson();

	// Each mesh is measured on a worker into its own sink, and the sinks are joined in input order, so the results do
	// not depend on the thread count. Merge roots share one part library.
	std::vector<const MeshToAnalyze*> selected;
	for (const MeshToAnalyze& item : meshes) {
		if (options.assetFilter.empty() || item.asset.find(options.assetFilter) != std::string::npos) {
			selected.push_back(&item);
		}
	}
	struct Outcome {
		MetricSink sink;
		std::string leafKey, leafHash, error;
	};
	std::vector<Outcome> outcomes(selected.size());
	MergePartLibrary mergeParts;
	std::atomic<size_t> next{ 0u };
	std::atomic<bool> failed{ false };
	const auto work = [&]() {
		for (size_t index = next.fetch_add(1u); index < selected.size() && !failed.load(); index = next.fetch_add(1u)) {
			const MeshToAnalyze& item = *selected[index];
			Outcome& outcome = outcomes[index];
			const std::unique_ptr<CachedMesh> mesh = CachedMesh::Open(item.metadataPath, outcome.error);
			if (!mesh) {
				failed.store(true);
				return;
			}
			const std::string meshName = mesh->Name();
			outcome.leafKey = item.asset + "|" + meshName;
			outcome.leafHash = std::format("{:016x}", mesh->LeafGeometryHash());
			std::vector<std::unique_ptr<CachedMesh>> branchViews;
			std::vector<const CachedMesh*> views = { mesh.get() };
			if (mesh->IsForked()) {
				views.clear();
				if (options.branch.empty() || options.branch == "triangle") {
					branchViews.push_back(mesh->BranchView(CLOD_BRANCH_TRIANGLE, options.branch.empty() ? "#triangle" : ""));
				}
				if (options.branch.empty() || options.branch == "voxel") {
					branchViews.push_back(mesh->BranchView(CLOD_BRANCH_VOXEL, options.branch.empty() ? "#voxel" : ""));
				}
				for (const auto& view : branchViews) views.push_back(view.get());
			}
			for (const CachedMesh* view : views) {
				const AnalysisContext context{ *view, item.asset, &mergeParts, options.mergeReachCut };
				for (const IAnalyzer* analyzer : analyzers) {
					outcome.sink.SetScope({ std::string(analyzer->Info().id), analyzer->Info().version, buildName, item.asset, view->Name() });
					analyzer->Run(context, outcome.sink);
				}
			}
		}
	};
	const uint32_t threadCount = static_cast<uint32_t>(std::clamp<size_t>(
		options.threads != 0u ? options.threads : std::max(1u, std::thread::hardware_concurrency()), 1u, std::max<size_t>(selected.size(), 1u)));
	std::vector<std::thread> workers;
	for (uint32_t t = 1; t < threadCount; ++t) {
		workers.emplace_back(work);
	}
	work();
	for (std::thread& worker : workers) {
		worker.join();
	}
	MetricSink sink;
	size_t analyzed = 0;
	for (Outcome& outcome : outcomes) {
		if (!outcome.error.empty()) {
			outError = outcome.error;
			return false;
		}
		if (outcome.leafKey.empty()) {
			continue;
		}
		outResults.leafGeometry[outcome.leafKey] = outcome.leafHash;
		sink.Absorb(std::move(outcome.sink));
		++analyzed;
	}
	if (failed.load()) {
		outError = "a cache could not be opened";
		return false;
	}
	if (analyzed == 0u) {
		outError = options.assetFilter.empty() ? "no caches to analyze" : std::format("no caches match filter '{}'", options.assetFilter);
		return false;
	}
	outResults.records = sink.Finish();
	if (!sink.DroppedNonFinite().empty()) {
		// Non-finite numbers mean a bug in the data or the analyzer, so this must not be quiet.
		outError = std::format("{} non-finite metric(s), first: {}", sink.DroppedNonFinite().size(), sink.DroppedNonFinite().front());
		return false;
	}
	return true;
}

bool AnalyzeVariant(const Suite& suite, const SuiteVariant& variant, const AnalyzeOptions& options, ResultSet& outResults, std::string& outError)
{
	const BuildPaths paths = GetBuildPaths(suite, variant);
	std::error_code error;
	if (!fs::exists(paths.manifest, error)) {
		outError = std::format("variant '{}' has not been built; run the build command first", variant.name);
		return false;
	}
	nlohmann::json manifest;
	nlohmann::json record;
	try {
		manifest = nlohmann::json::parse(ReadTextFile(paths.manifest));
		record = nlohmann::json::parse(ReadTextFile(paths.buildRecord));
	}
	catch (const std::exception& parseError) {
		outError = std::format("variant '{}': {}", variant.name, parseError.what());
		return false;
	}

	std::vector<MeshToAnalyze> meshes;
	for (const nlohmann::json& mesh : manifest.at("meshes")) {
		meshes.push_back({ fs::path(mesh.at("metadata_path").get<std::string>()), AssetForSource(suite, mesh.at("source_identifier").get<std::string>()) });
	}
	if (!AnalyzeMeshes(meshes, variant.name, record.value("build_key", ""), options, outResults, outError)) {
		return false;
	}
	outResults.provenance["build"] = record.value("provenance", nlohmann::json::object());
	outResults.provenance["build_seconds"] = record.value("build_seconds", 0.0);
	return true;
}

bool GetAssetFrame(const Suite& suite, const SuiteVariant& variant, const SuiteAsset& asset, double outCenter[3], double& outRadius, std::string& outError)
{
	const BuildPaths paths = GetBuildPaths(suite, variant);
	nlohmann::json manifest;
	try {
		manifest = nlohmann::json::parse(ReadTextFile(paths.manifest));
	}
	catch (const std::exception& parseError) {
		outError = std::format("variant '{}' has no readable manifest: {}", variant.name, parseError.what());
		return false;
	}
	// An assembly's sphere covers its instanced parts, and a part's own covers only its unplaced mesh.
	const auto gather = [&](bool assembliesOnly, std::array<double, 3>& low, std::array<double, 3>& high) {
		bool any = false;
		for (const nlohmann::json& mesh : manifest.at("meshes")) {
			if (AssetForSource(suite, mesh.at("source_identifier").get<std::string>()) != asset.name) {
				continue;
			}
			std::string openError;
			const auto opened = CachedMesh::Open(mesh.at("metadata_path").get<std::string>(), openError);
			if (!opened) {
				continue;
			}
			const auto& sphere = opened->Prebuilt().objectBoundingSphere.sphere;
			if (assembliesOnly && opened->Name().find("CLodAssetAssembly") == std::string::npos) {
				continue;
			}
			any = true;
			const std::array<double, 3> center{ sphere.x, sphere.y, sphere.z };
			for (size_t axis = 0; axis < 3u; ++axis) {
				low[axis] = std::min(low[axis], center[axis] - sphere.w);
				high[axis] = std::max(high[axis], center[axis] + sphere.w);
			}
		}
		return any;
	};
	std::array<double, 3> low{ 1.0e30, 1.0e30, 1.0e30 };
	std::array<double, 3> high{ -1.0e30, -1.0e30, -1.0e30 };
	if (!gather(true, low, high) && !gather(false, low, high)) {
		outError = std::format("variant '{}' has no caches for asset '{}'", variant.name, asset.name);
		return false;
	}
	double squared = 0.0;
	for (size_t axis = 0; axis < 3u; ++axis) {
		outCenter[axis] = (low[axis] + high[axis]) * 0.5;
		const double half = (high[axis] - low[axis]) * 0.5;
		squared += half * half;
	}
	// The box around a single sphere has a diagonal sqrt(3) times its radius, which frames a little loose but
	// is the same for every variant.
	outRadius = std::sqrt(squared);
	return true;
}

nlohmann::json ProvenanceJson()
{
	const fs::path source(CLOD_EVAL_SOURCE_DIR);
	nlohmann::json provenance;
	provenance["basicrenderer_commit"] = RunGit(source, "rev-parse HEAD");
	provenance["basicrenderer_dirty"] = !RunGit(source, "status --porcelain --untracked-files=no").empty();
	provenance["schema_version"] = CLodCache::kSchemaVersion;
	return provenance;
}

nlohmann::json DescribeCache(const fs::path& metadataPath, int groupIndex, std::string& outError)
{
	const std::unique_ptr<CachedMesh> mesh = CachedMesh::Open(metadataPath, outError);
	if (!mesh) {
		return {};
	}
	const ClusterLODPrebuiltData& prebuilt = mesh->Prebuilt();
	nlohmann::json description;
	description["metadata_path"] = metadataPath.string();
	description["container_path"] = mesh->ContainerPath().string();
	description["source_identifier"] = prebuilt.cacheSource.sourceIdentifier;
	description["prim_path"] = prebuilt.cacheSource.primPath;
	description["subset"] = prebuilt.cacheSource.subsetName;
	description["build_config_hash"] = std::format("{:016X}", mesh->BuildConfigHash());
	description["metadata_bytes"] = mesh->MetadataBytes();
	description["container_bytes"] = mesh->ContainerBytes();
	description["groups"] = prebuilt.groups.size();
	description["segments"] = prebuilt.segments.size();
	description["nodes"] = prebuilt.nodes.size();
	description["parts"] = prebuilt.partRecords.size();
	description["max_depth"] = prebuilt.maxDepth;
	description["triangle_pages"] = prebuilt.trianglePageCount;
	if (mesh->IsForked()) {
		// A triangle/voxel fork (CLOD_GROUP_FLAG_BRANCH_*): groups only in each branch, and shared groups whose voxel-branch
		// boundary is a shadow record.
		uint64_t triangleOnly = 0, voxelOnly = 0, shadows = 0;
		for (const ClusterLODGroup& group : prebuilt.groups) {
			triangleOnly += (group.flags & CLOD_GROUP_FLAG_BRANCH_TRIANGLE) != 0u ? 1u : 0u;
			voxelOnly += (group.flags & CLOD_GROUP_FLAG_BRANCH_VOXEL) != 0u ? 1u : 0u;
			shadows += (group.flags & CLOD_GROUP_FLAG_BRANCH_SHADOW) != 0u ? 1u : 0u;
		}
		description["fork"] = { { "triangle_only_groups", triangleOnly }, { "voxel_only_groups", voxelOnly }, { "boundary_groups", shadows } };
	}
	description["voxel_pages"] = prebuilt.voxelPageCount;
	description["object_sphere"] = {
		prebuilt.objectBoundingSphere.sphere.x, prebuilt.objectBoundingSphere.sphere.y,
		prebuilt.objectBoundingSphere.sphere.z, prebuilt.objectBoundingSphere.sphere.w };

	struct Level {
		uint64_t triangleGroups = 0;
		uint64_t voxelGroups = 0;
		uint64_t triangles = 0;
		float minError = (std::numeric_limits<float>::max)();
		float maxError = 0.0f;
	};
	std::map<int32_t, Level> levels;
	for (uint32_t index = 0; index < prebuilt.groups.size(); ++index) {
		const ClusterLODGroup& group = prebuilt.groups[index];
		Level& level = levels[group.depth];
		const bool isVoxel = (group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u;
		++(isVoxel ? level.voxelGroups : level.triangleGroups);
		if (group.bounds.error < 1.0e19f) {
			level.minError = std::min(level.minError, group.bounds.error);
			level.maxError = std::max(level.maxError, group.bounds.error);
		}
		if (!isVoxel) {
			for (uint32_t segment = group.firstSegment; segment < group.firstSegment + group.segmentCount; ++segment) {
				mesh->VisitSegmentMeshlets(index, prebuilt.segments[segment], [&](const CLodCache::TrianglePage&, const CLodCache::TriangleMeshlet& meshlet) {
					level.triangles += meshlet.triangleCount;
				});
			}
		}
	}
	nlohmann::json levelList = nlohmann::json::array();
	for (const auto& [depth, level] : levels) {
		levelList.push_back({
			{ "depth", depth },
			{ "triangle_groups", level.triangleGroups },
			{ "voxel_groups", level.voxelGroups },
			{ "triangles", level.triangles },
			{ "min_error", level.maxError > 0.0f || level.minError == 0.0f ? level.minError : 0.0f },
			{ "max_error", level.maxError },
		});
	}
	description["levels"] = std::move(levelList);

	if (groupIndex >= 0) {
		if (static_cast<size_t>(groupIndex) >= prebuilt.groups.size()) {
			outError = std::format("group {} is out of range; the mesh has {}", groupIndex, prebuilt.groups.size());
			return {};
		}
		const ClusterLODGroup& group = prebuilt.groups[static_cast<size_t>(groupIndex)];
		nlohmann::json detail;
		detail["index"] = groupIndex;
		detail["depth"] = group.depth;
		detail["is_voxel"] = (group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u;
		detail["error"] = group.bounds.error;
		detail["representation_error"] = group.representationError;
		detail["max_parent_error"] = group.maxParentError;
		detail["parent"] = group.parentGroupId;
		detail["sphere"] = { group.bounds.center[0], group.bounds.center[1], group.bounds.center[2], group.bounds.radius };
		detail["pages"] = group.pageCount;
		nlohmann::json segments = nlohmann::json::array();
		for (uint32_t segmentIndex = group.firstSegment; segmentIndex < group.firstSegment + group.segmentCount; ++segmentIndex) {
			const ClusterLODGroupSegment& segment = prebuilt.segments[segmentIndex];
			nlohmann::json entry{ { "refined_group", segment.refinedGroup }, { "clusters", segment.meshletCount } };
			if (segment.refinedGroup >= 0) {
				entry["refined_error"] = prebuilt.groups[static_cast<size_t>(segment.refinedGroup)].bounds.error;
			}
			uint32_t meshPage = 0;
			if (CLodCache::ResolveSegmentMeshPage(prebuilt, static_cast<uint32_t>(groupIndex), segment, meshPage)) {
				entry["mesh_page"] = meshPage;
				entry["page_bytes"] = mesh->Page(meshPage).size();
			}
			segments.push_back(std::move(entry));
		}
		detail["segments"] = std::move(segments);
		description["group"] = std::move(detail);
	}
	return description;
}

std::string FormatCacheDescription(const nlohmann::json& description)
{
	std::string text = std::format(
		"{}\n  {} {}\n  metadata {} bytes, container {} bytes, config hash {}\n  {} groups, {} segments, {} nodes, {} parts, max depth {}\n  {} triangle pages, {} voxel pages\n",
		description.at("metadata_path").get<std::string>(),
		description.at("source_identifier").get<std::string>(),
		description.at("prim_path").get<std::string>(),
		description.at("metadata_bytes").get<uint64_t>(),
		description.at("container_bytes").get<uint64_t>(),
		description.at("build_config_hash").get<std::string>(),
		description.at("groups").get<uint64_t>(),
		description.at("segments").get<uint64_t>(),
		description.at("nodes").get<uint64_t>(),
		description.at("parts").get<uint64_t>(),
		description.at("max_depth").get<uint32_t>(),
		description.at("triangle_pages").get<uint32_t>(),
		description.at("voxel_pages").get<uint32_t>());
	if (description.contains("fork")) {
		const nlohmann::json& fork = description.at("fork");
		text += std::format("  forked: {} triangle-only groups, {} voxel-only groups, {} boundary groups (--branch measures one)\n",
			fork.at("triangle_only_groups").get<uint64_t>(), fork.at("voxel_only_groups").get<uint64_t>(), fork.at("boundary_groups").get<uint64_t>());
	}
	text += "  depth  tri_groups  vox_groups   triangles    min_error    max_error\n";
	for (const nlohmann::json& level : description.at("levels")) {
		text += std::format(
			"  {:>5}  {:>10}  {:>10}  {:>10}  {:>11.5g}  {:>11.5g}\n",
			level.at("depth").get<int32_t>(),
			level.at("triangle_groups").get<uint64_t>(),
			level.at("voxel_groups").get<uint64_t>(),
			level.at("triangles").get<uint64_t>(),
			level.at("min_error").get<float>(),
			level.at("max_error").get<float>());
	}
	if (description.contains("group")) {
		text += "  group: " + description.at("group").dump(2) + "\n";
	}
	return text;
}

}
