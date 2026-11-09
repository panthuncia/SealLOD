#include "GltfSceneBuild.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <exception>
#include <mutex>
#include <regex>
#include <thread>
#include <vector>

#include <windows.h>
#include <psapi.h>

#include <spdlog/spdlog.h>

#include <BasicRenderer/Assets/Import/CLodCache.h>
#include <BasicRenderer/Assets/Import/GlTFGeometryExtractor.h>
#include <BasicRenderer/Assets/Import/SceneManifest.h>
#include <BasicRenderer/Assets/TexturePipeline.h>

#include "Assets/Import/GlTF/GlTFLoader.h"

namespace fs = std::filesystem;

namespace
{
	using Kind = GlTFGeometryExtractor::PrimitiveOutcome::Kind;

	std::uint64_t PhysicalMemoryBytes()
	{
		MEMORYSTATUSEX status{};
		status.dwLength = sizeof(status);
		return GlobalMemoryStatusEx(&status) ? status.ullTotalPhys : 0ull;
	}

	std::uint64_t PeakWorkingSetBytes()
	{
		PROCESS_MEMORY_COUNTERS counters{};
		return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) ? counters.PeakWorkingSetSize : 0ull;
	}

	std::string UtcNow()
	{
		const std::time_t now = std::time(nullptr);
		std::tm utc{};
		gmtime_s(&utc, &now);
		char text[32];
		std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
		return text;
	}

	double Gib(std::uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0); }

	struct Counters
	{
		std::atomic<std::uint64_t> done{ 0 };
		std::atomic<std::uint64_t> hit{ 0 };
		std::atomic<std::uint64_t> built{ 0 };
		std::atomic<std::uint64_t> missing{ 0 };
		std::atomic<std::uint64_t> failed{ 0 };
		std::atomic<std::uint64_t> trianglesDone{ 0 };
		std::atomic<std::uint64_t> trianglesBuilt{ 0 };
	};

	struct TextureCounts
	{
		std::uint64_t requested = 0;
		std::uint64_t streamed = 0;  // straight from its file, never cached
		std::uint64_t cached = 0;    // already conditioned
		std::uint64_t built = 0;     // conditioned now
		std::uint64_t missing = 0;   // cache-only, not conditioned
		std::uint64_t failed = 0;
	};

	TextureCounts ConditionTextures(const nlohmann::json& gltf, const fs::path& scenePath, const GltfSceneBuildOptions& options)
	{
		br::import::ImportSettings settings;
		settings.profile = options.profile;
		auto requests = GlTFLoader::BuildMaterialTextureRequests(gltf, scenePath.string(), settings);
		TextureCounts counts;
		counts.requested = requests.size();

		// The shape decides, read from each provided image's header: no pixels are read for a texture that streams.
		std::vector<br::textures::TextureRequest> toCondition;
		for (auto& request : requests) {
			if (br::textures::IsProvidedPassthrough(request)) {
				++counts.streamed;
			}
			else {
				toCondition.push_back(std::move(request));
			}
		}
		spdlog::info("  Textures: {} requested, {} stream from their files, {} need a processed cache entry",
			counts.requested, counts.streamed, toCondition.size());
		if (toCondition.empty()) return counts;
		if (options.cacheOnly) {
			// Without conditioning, a texture with no entry is a miss the renderer would fill at load.
			counts.missing = toCondition.size();
			return counts;
		}

		// A large texture's whole chain is in memory while it is processed (hundreds of MiB at 8K): a few at a time.
		std::atomic<size_t> next{ 0 };
		std::mutex mutex;
		std::vector<std::thread> workers;
		const unsigned workerCount = (std::min<unsigned>)(8u, (std::max)(1u, std::thread::hardware_concurrency()));
		for (unsigned i = 0; i < workerCount; ++i) {
			workers.emplace_back([&] {
				for (size_t index = next++; index < toCondition.size(); index = next++) {
					const auto& request = toCondition[index];
					try {
						const auto result = br::textures::ConditionTextureOnCpu(br::textures::RequestTexture(request));
						std::lock_guard lock(mutex);
						if (result == br::textures::TextureConditionResult::Built) ++counts.built;
						else if (result == br::textures::TextureConditionResult::AlreadyCached) ++counts.cached;
						else ++counts.streamed;
					}
					catch (const std::exception& error) {
						std::lock_guard lock(mutex);
						++counts.failed;
						spdlog::error("  Texture '{}' failed: {}", request.source.identity, error.what());
					}
				}
			});
		}
		for (auto& worker : workers) worker.join();
		return counts;
	}
}

bool BuildGltfScene(const fs::path& scenePath, const GltfSceneBuildOptions& options)
{
	const auto begin = std::chrono::steady_clock::now();
	std::optional<std::regex> only;
	std::optional<std::regex> skip;
	try {
		if (!options.onlyMeshes.empty()) only.emplace(options.onlyMeshes, std::regex::ECMAScript | std::regex::optimize);
		if (!options.skipMeshes.empty()) skip.emplace(options.skipMeshes, std::regex::ECMAScript | std::regex::optimize);
	}
	catch (const std::regex_error& error) {
		spdlog::error("  Mesh filter is not a valid regex: {}", error.what());
		return false;
	}

	GlTFGeometryExtractor::ExtractOptions extract;
	std::atomic<std::uint64_t> selectedMeshes{ 0 };
	std::atomic<std::uint64_t> skippedMeshes{ 0 };
	extract.skipMesh = [&](size_t meshIndex, std::string_view name) {
		const std::string text(name);
		const bool skipped =
			(options.shardCount > 1u && meshIndex % options.shardCount != options.shardIndex) ||
			(options.profile && options.profile->SkipsMesh(name)) ||
			(skip && std::regex_match(text, *skip)) ||
			(only && !std::regex_match(text, *only));
		(skipped ? skippedMeshes : selectedMeshes).fetch_add(1, std::memory_order_relaxed);
		return skipped;
	};
	extract.cacheOnly = options.cacheOnly;
	extract.continueOnError = true;
	extract.memoryBudgetBytes = options.memoryBudgetBytes != 0 ? options.memoryBudgetBytes : PhysicalMemoryBytes() / 2u;
	extract.bytesPerTriangle = options.bytesPerTriangle;
	extract.jobs = options.jobs;
	spdlog::info("  Memory budget {:.1f} GiB at {} bytes/triangle, {} jobs", Gib(extract.memoryBudgetBytes), extract.bytesPerTriangle,
		options.jobs != 0u ? options.jobs : std::thread::hardware_concurrency());

	// Report and drop each primitive as it is done: the scene's CLod data is never all in memory.
	extract.onPrimitive = [&](GlTFGeometryExtractor::ExtractedPrimitive&& primitive) {
		if (options.report) {
			options.report("glTF mesh " + std::to_string(primitive.meshIndex) + " primitive " + std::to_string(primitive.primitiveIndex), primitive.result);
		}
	};

	Counters counters;
	std::mutex failureMutex;
	std::vector<GlTFGeometryExtractor::PrimitiveOutcome> failures;
	std::atomic<std::int64_t> lastProgressMs{ 0 };
	std::atomic<std::uint64_t> totalTriangles{ 0 };
	extract.onOutcome = [&](const GlTFGeometryExtractor::PrimitiveOutcome& outcome) {
		counters.done.fetch_add(1, std::memory_order_relaxed);
		counters.trianglesDone.fetch_add(outcome.triangles, std::memory_order_relaxed);
		switch (outcome.kind) {
		case Kind::Hit: counters.hit.fetch_add(1, std::memory_order_relaxed); break;
		case Kind::Built:
			counters.built.fetch_add(1, std::memory_order_relaxed);
			counters.trianglesBuilt.fetch_add(outcome.triangles, std::memory_order_relaxed);
			break;
		case Kind::Missing: counters.missing.fetch_add(1, std::memory_order_relaxed); break;
		case Kind::Failed: {
			counters.failed.fetch_add(1, std::memory_order_relaxed);
			std::lock_guard lock(failureMutex);
			failures.push_back(outcome);
			break;
		}
		}
		if (outcome.kind == Kind::Built && outcome.seconds > 60.0) {
			spdlog::info("  mesh {} primitive {}: {} triangles in {:.0f} s", outcome.meshIndex, outcome.primitiveIndex, outcome.triangles, outcome.seconds);
		}
		const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
		auto last = lastProgressMs.load(std::memory_order_relaxed);
		if (elapsedMs - last >= 10000 && lastProgressMs.compare_exchange_strong(last, elapsedMs)) {
			spdlog::info("  Progress: {} primitives ({} hit, {} built, {} failed, {} missing), {:.2f} G triangles built, {:.0f} s, peak {:.1f} GiB",
				counters.done.load(), counters.hit.load(), counters.built.load(), counters.failed.load(), counters.missing.load(),
				static_cast<double>(counters.trianglesBuilt.load()) / 1e9, elapsedMs / 1000.0, Gib(PeakWorkingSetBytes()));
		}
	};

	GlTFGeometryExtractor::ExtractionResult result;
	try {
		result = GlTFGeometryExtractor::ExtractAll(scenePath.string(), extract);
	}
	catch (const std::exception& error) {
		spdlog::error("  glTF scene failed: {}", error.what());
		return false;
	}
	const double geometrySeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
	spdlog::info("  Geometry: {} meshes selected, {} skipped; {} primitives: {} hit, {} built ({:.2f} G triangles), {} failed, {} missing; {:.0f} s, peak {:.1f} GiB",
		selectedMeshes.load(), skippedMeshes.load(), counters.done.load(), counters.hit.load(), counters.built.load(),
		static_cast<double>(counters.trianglesBuilt.load()) / 1e9, counters.failed.load(), counters.missing.load(), geometrySeconds,
		Gib(PeakWorkingSetBytes()));
	{
		std::lock_guard lock(failureMutex);
		std::sort(failures.begin(), failures.end(), [](const auto& a, const auto& b) {
			return a.meshIndex != b.meshIndex ? a.meshIndex < b.meshIndex : a.primitiveIndex < b.primitiveIndex;
		});
		for (size_t i = 0; i < failures.size() && i < 50u; ++i) {
			const auto& failure = failures[i];
			const auto& meshes = result.gltf["meshes"];
			spdlog::error("  FAILED mesh {} '{}' primitive {}: {}", failure.meshIndex,
				meshes[failure.meshIndex].value("name", std::string()), failure.primitiveIndex, failure.error);
		}
		if (failures.size() > 50u) spdlog::error("  ... and {} more failures", failures.size() - 50u);
	}

	TextureCounts textures;
	if (options.conditionTextures) {
		try {
			textures = ConditionTextures(result.gltf, scenePath, options);
		}
		catch (const std::exception& error) {
			spdlog::error("  Texture stage failed: {}", error.what());
			textures.failed = 1;
		}
		spdlog::info("  Textures: {} streamed from their files, {} already cached, {} conditioned, {} missing, {} failed",
			textures.streamed, textures.cached, textures.built, textures.missing, textures.failed);
	}

	const bool complete = counters.failed == 0 && counters.missing == 0 && textures.failed == 0 && textures.missing == 0;
	if (!options.cacheOnly) {
		br::import::SceneManifest manifest;
		const fs::path absoluteScene = fs::absolute(scenePath).lexically_normal();
		const fs::path manifestPath = br::import::SceneManifestPathFor(options.outDirectory, absoluteScene);
		std::error_code error;
		const fs::path relative = fs::relative(absoluteScene, fs::absolute(options.outDirectory), error);
		manifest.sourcePath = error ? std::string() : relative.generic_string();
		manifest.sourceAbsolutePath = absoluteScene.generic_string();
		manifest.sourceIdentifier = result.sourceIdentifier;
		manifest.cacheRoot = ".";
		if (options.profile) manifest.profile = *options.profile;
		manifest.clodSchemaVersion = CLodCache::kSchemaVersion;
		manifest.clodBuildConfigHash = CLodCache::ComputeBuildConfigHash(result.sourceIdentifier);
		manifest.counts = {
			{ "meshesSelected", selectedMeshes.load() },
			{ "meshesSkipped", skippedMeshes.load() },
			{ "primitives", counters.done.load() },
			{ "primitivesHit", counters.hit.load() },
			{ "primitivesBuilt", counters.built.load() },
			{ "primitivesFailed", counters.failed.load() },
			{ "trianglesSelected", counters.trianglesDone.load() },
			{ "texturesRequested", textures.requested },
			{ "texturesStreamedFromSource", textures.streamed },
			{ "texturesCached", textures.cached + textures.built },
			{ "texturesFailed", textures.failed },
		};
		manifest.generator = "CLodCacheTool";
		manifest.created = UtcNow();
		try {
			br::import::WriteSceneManifest(manifestPath, manifest);
			spdlog::info("  Scene manifest: {}{}", manifestPath.string(), complete ? "" : " (incomplete: see failures above)");
		}
		catch (const std::exception& writeError) {
			spdlog::error("  Scene manifest {} not written: {}", manifestPath.string(), writeError.what());
			return false;
		}
	}
	return complete;
}
