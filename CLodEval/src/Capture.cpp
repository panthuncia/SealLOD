#include "Capture.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "Core/Image.h"
#include "Core/Metrics.h"
#include "Core/Process.h"
#include "Transition.h"

namespace clodeval {
namespace fs = std::filesystem;
using image::Plane;

namespace {
	constexpr uint32_t kOutputColor = 0u;
	constexpr uint32_t kOutputVoxelGeometryPath = 34u;  // OutputType::VOXEL_GEOMETRY_PATH
	constexpr std::string_view kSettingPrefix = "BASICRENDERER_CLOD_";
	constexpr uint32_t kAnalyzerVersion = 1u;

	std::string ReadText(const fs::path& path)
	{
		std::ifstream input(path);
		std::ostringstream text;
		text << input.rdbuf();
		return text.str();
	}

	fs::path CaptureDirectory(const Suite& suite, const SuiteVariant& variant, const SuiteAsset& asset, std::string_view job)
	{
		return suite.workspace / "captures" / variant.name / asset.name / job;
	}

	std::string Fixed(double value)
	{
		return std::format("{:g}", value);
	}

	nlohmann::json MakeProbe(std::string_view name, double threshold, uint32_t frames, std::vector<std::string> layers, uint32_t outputType)
	{
		// Every probe states its output, because a setting stays as the previous probe left it.
		return {
			{ "name", name },
			{ "lod_error_pixels", threshold },
			{ "accumulate_frames", frames },
			{ "layers", std::move(layers) },
			{ "settings", { { "outputType", outputType } } },
		};
	}

	nlohmann::json MakeJob(const SuiteAsset& asset, const fs::path& output, const SuiteCapture& capture, const AssetFrame& frame, const std::vector<RigView>& rig, uint32_t scale, nlohmann::json probes)
	{
		nlohmann::json views = nlohmann::json::array();
		for (const RigView& view : rig) {
			views.push_back({
				{ "name", view.name },
				{ "position", { view.position[0], view.position[1], view.position[2] } },
				{ "look_at", { view.lookAt[0], view.lookAt[1], view.lookAt[2] } },
				{ "fov_degrees", capture.fovDegrees },
				{ "near", frame.radius * 0.005 },
				{ "far", frame.radius * capture.farRadii },
			});
		}
		return {
			{ "assets", { { { "path", asset.path.generic_string() } } } },
			{ "resolution", { capture.width * scale, capture.height * scale } },
			{ "output", output.generic_string() },
			{ "cache_only", true },
			{ "ready_timeout_seconds", capture.readyTimeoutSeconds },
			{ "views", std::move(views) },
			{ "probes", std::move(probes) },
		};
	}

	// The reference is rendered larger than everything else and is only useful once reduced, and at full size
	// one view of it is over half a gigabyte. So it is reduced straight away and the originals deleted. Surface
	// attributes are averaged only over covered subpixels, because empty ones hold no colour.
	bool ReduceReference(const fs::path& directory, uint32_t scale, std::string& outError)
	{
		Plane hdr, coverage, baseColor, normalRoughness;
		image::KeyImage visibility;
		if (!image::ReadPlane(directory / "hdr-mean.dds", hdr, outError) || !image::ReadPlane(directory / "coverage-mean.dds", coverage, outError) ||
			!image::ReadPlane(directory / "base-color-opacity.dds", baseColor, outError) || !image::ReadPlane(directory / "normal-roughness.dds", normalRoughness, outError) ||
			!image::ReadKeys(directory / "visibility.dds", visibility, outError)) {
			return false;
		}
		const uint32_t width = hdr.width / scale, height = hdr.height / scale;
		Plane surfaceColor{ width, height, 4, std::vector<float>(static_cast<size_t>(width) * height * 4u, 0.0f) };
		Plane surfaceNormal{ width, height, 4, std::vector<float>(static_cast<size_t>(width) * height * 4u, 0.0f) };
		std::vector<uint32_t> counts(static_cast<size_t>(width) * height, 0u);
		for (uint32_t y = 0; y < height * scale; ++y) {
			for (uint32_t x = 0; x < width * scale; ++x) {
				if (visibility.keys[static_cast<size_t>(y) * visibility.width + x] == image::kEmptyKey) {
					continue;
				}
				const uint32_t tx = x / scale, ty = y / scale;
				++counts[static_cast<size_t>(ty) * width + tx];
				for (uint32_t channel = 0; channel < 4u; ++channel) {
					surfaceColor.At(tx, ty, channel) += baseColor.At(x, y, channel);
					surfaceNormal.At(tx, ty, channel) += normalRoughness.At(x, y, channel);
				}
			}
		}
		for (size_t index = 0; index < counts.size(); ++index) {
			if (counts[index] == 0u) {
				continue;
			}
			for (uint32_t channel = 0; channel < 4u; ++channel) {
				surfaceColor.data[index * 4u + channel] /= static_cast<float>(counts[index]);
				surfaceNormal.data[index * 4u + channel] /= static_cast<float>(counts[index]);
			}
		}
		if (!image::WritePlane(directory / "reduced-hdr.dds", image::BoxDownsample(hdr, scale), outError) ||
			!image::WritePlane(directory / "reduced-coverage.dds", image::BoxDownsample(coverage, scale), outError) ||
			!image::WritePlane(directory / "reduced-base-color.dds", surfaceColor, outError) ||
			!image::WritePlane(directory / "reduced-normal-roughness.dds", surfaceNormal, outError)) {
			return false;
		}
		std::error_code error;
		for (const char* name : { "hdr-mean.dds", "hdr-luminance-variance.dds", "coverage-mean.dds", "base-color-opacity.dds", "normal-roughness.dds", "visibility.dds" }) {
			fs::remove(directory / name, error);
		}
		return true;
	}

	// The cache keys depend on this environment, so a capture and a viewer of one variant have to agree on it.
	ProcessRequest MakeCaptureProcess(const Suite& suite, const SuiteVariant& variant, const fs::path& captureTool)
	{
		const BuildPaths paths = GetBuildPaths(suite, variant);
		ProcessRequest request;
		request.executable = captureTool;
		request.workingDirectory = captureTool.parent_path();
		request.clearEnvironmentPrefixes = { std::string(kSettingPrefix) };
		request.environment = variant.settings;
		fs::path assetSettings = variant.assetSettings;
		if (assetSettings.empty()) {
			assetSettings = paths.directory / "no_asset_settings.json";
		}
		request.environment["SARP_ASSET_SETTINGS_CONFIG"] = assetSettings.string();
		request.environment["SARP_CACHE_ROOT"] = paths.cacheRoot.string();
		if (suite.capture.pagePoolSlabs != 0u) {
			request.environment["BASICRENDERER_CLOD_PAGE_POOL_SLABS"] = std::to_string(suite.capture.pagePoolSlabs);
		}
		request.pathPrepend = { captureTool.parent_path() };
		return request;
	}

	bool RunJob(const Suite& suite, const SuiteVariant& variant, const CaptureOptions& options, const fs::path& directory, const nlohmann::json& job, bool force, std::string& outError)
	{
		std::error_code error;
		const std::string jobText = job.dump(2);
		const std::string key = jobText + "\n" + ReadText(GetBuildPaths(suite, variant).buildRecord) + std::to_string(fs::last_write_time(options.captureTool, error).time_since_epoch().count());
		const fs::path keyPath = directory / "capture_key.txt";
		if (!force && fs::exists(directory / "bundle.json", error) && ReadText(keyPath) == key) {
			return true;
		}
		fs::remove_all(directory, error);
		fs::create_directories(directory, error);
		const fs::path jobPath = directory / "job.json";
		std::ofstream(jobPath) << jobText << '\n';

		ProcessRequest request = MakeCaptureProcess(suite, variant, options.captureTool);
		request.logPath = directory / "capture.log";
		request.arguments = { "--job", jobPath.string() };
		const int exitCode = RunProcess(request, outError);
		if (exitCode != 0) {
			if (outError.empty()) {
				outError = std::format("CLodCapture exited with {}; see '{}'", exitCode, request.logPath.string());
			}
			return false;
		}
		const nlohmann::json bundle = nlohmann::json::parse(ReadText(directory / "bundle.json"));
		if (bundle.value("cache_only_violations", 0u) != 0u) {
			outError = std::format("the capture in '{}' needed meshes the cache does not have", directory.string());
			return false;
		}
		std::ofstream(keyPath) << key;
		return true;
	}

	std::vector<uint8_t> VoxelMask(const image::KeyImage& debug, const image::KeyImage& visibility) { return VoxelPathMask(debug, visibility); }

	struct Layers {
		Plane hdr, coverage, variance, baseColor, normal;
		image::KeyImage visibility;
	};

	bool ReadCaptureLayers(const fs::path& directory, Layers& out, std::string& outError)
	{
		return image::ReadPlane(directory / "hdr-mean.dds", out.hdr, outError) && image::ReadPlane(directory / "coverage-mean.dds", out.coverage, outError) &&
			image::ReadPlane(directory / "hdr-luminance-variance.dds", out.variance, outError) && image::ReadPlane(directory / "base-color-opacity.dds", out.baseColor, outError) &&
			image::ReadPlane(directory / "normal-roughness.dds", out.normal, outError) && image::ReadKeys(directory / "visibility.dds", out.visibility, outError);
	}

	struct SurfaceError {
		double albedo = 0.0, albedoLumaRatio = 0.0, normalDegrees = 0.0, roughness = 0.0, opacity = 0.0;
		size_t pixels = 0;
	};

	// Compares where both have a surface and the reference is mostly covered, so edges do not dominate.
	SurfaceError CompareSurface(const Layers& test, const Plane& referenceColor, const Plane& referenceNormal, const Plane& referenceCoverage, const std::vector<uint8_t>& select, bool wantSelected)
	{
		SurfaceError result;
		double testLuma = 0.0, referenceLuma = 0.0;
		for (size_t index = 0; index < select.size(); ++index) {
			if (test.visibility.keys[index] == image::kEmptyKey || referenceCoverage.data[index] < 0.5f || (select[index] != 0u) != wantSelected) {
				continue;
			}
			const float* c = test.baseColor.data.data() + index * 4u;
			const float* r = referenceColor.data.data() + index * 4u;
			result.albedo += (std::abs(c[0] - r[0]) + std::abs(c[1] - r[1]) + std::abs(c[2] - r[2])) / 3.0;
			testLuma += 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
			referenceLuma += 0.2126 * r[0] + 0.7152 * r[1] + 0.0722 * r[2];
			const float* n = test.normal.data.data() + index * 4u;
			const float* m = referenceNormal.data.data() + index * 4u;
			const double lengthN = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			const double lengthM = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
			if (lengthN > 0.0 && lengthM > 0.0) {
				const double cosine = std::clamp((n[0] * m[0] + n[1] * m[1] + n[2] * m[2]) / (lengthN * lengthM), -1.0, 1.0);
				result.normalDegrees += std::acos(cosine) * 180.0 / 3.14159265358979323846;
			}
			result.roughness += std::abs(n[3] - m[3]);
			++result.pixels;
		}
		if (result.pixels != 0u) {
			const double count = static_cast<double>(result.pixels);
			result.albedo /= count;
			result.normalDegrees /= count;
			result.roughness /= count;
			result.albedoLumaRatio = referenceLuma > 0.0 ? testLuma / referenceLuma : 0.0;
		}
		return result;
	}

	// A row of equally sized panels, reduced by half so a sheet stays a reasonable size.
	class ContactSheet {
	public:
		ContactSheet(uint32_t width, uint32_t height, uint32_t panels)
			: m_panelWidth(width / 2u), m_panelHeight(height / 2u), m_panels(panels), m_rgb(static_cast<size_t>(m_panelWidth) * panels * m_panelHeight * 3u, 0u) {}

		template <typename Sampler>
		void Panel(uint32_t panel, Sampler&& sample)
		{
			for (uint32_t y = 0; y < m_panelHeight; ++y) {
				for (uint32_t x = 0; x < m_panelWidth; ++x) {
					float rgb[3]{};
					sample(x * 2u, y * 2u, rgb);
					const size_t offset = (static_cast<size_t>(y) * m_panelWidth * m_panels + static_cast<size_t>(panel) * m_panelWidth + x) * 3u;
					for (int channel = 0; channel < 3; ++channel) {
						m_rgb[offset + channel] = static_cast<uint8_t>(std::clamp(rgb[channel], 0.0f, 1.0f) * 255.0f + 0.5f);
					}
				}
			}
		}

		bool Write(const fs::path& path, std::string& outError) const
		{
			return image::WritePng(path, m_panelWidth * m_panels, m_panelHeight, m_rgb, outError);
		}

	private:
		uint32_t m_panelWidth, m_panelHeight, m_panels;
		std::vector<uint8_t> m_rgb;
	};

	// Exposure comes from the reference alone, so it is the same for every variant of an asset.
	float ExposureFromReference(const Plane& hdr, const Plane& coverage)
	{
		std::vector<float> luminance;
		for (size_t index = 0; index < coverage.Pixels(); ++index) {
			if (coverage.data[index] > 0.5f) {
				const float* p = hdr.data.data() + index * hdr.channels;
				luminance.push_back(0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]);
			}
		}
		if (luminance.empty()) {
			return 1.0f;
		}
		auto middle = luminance.begin() + static_cast<ptrdiff_t>(luminance.size() / 2u);
		std::nth_element(luminance.begin(), middle, luminance.end());
		return *middle > 0.0f ? 1.0f / *middle : 1.0f;
	}

	bool FindViewsAndCheck(const fs::path& bundlePath, nlohmann::json& outBundle, std::string& outError)
	{
		std::error_code error;
		if (!fs::exists(bundlePath, error)) {
			outError = std::format("'{}' does not exist; run the capture command first", bundlePath.string());
			return false;
		}
		outBundle = nlohmann::json::parse(ReadText(bundlePath));
		return true;
	}

	const nlohmann::json* FindProbe(const nlohmann::json& view, std::string_view name)
	{
		for (const nlohmann::json& probe : view.at("probes")) {
			if (probe.at("name").get<std::string>() == name) {
				return &probe;
			}
		}
		return nullptr;
	}

	bool IsInterestingPass(const std::string& name)
	{
		return name.find("CLod") != std::string::npos || name.find("Voxel") != std::string::npos || name.find("ClusterLOD") != std::string::npos ||
			name == "EvaluateMaterialGroupsPass" || name == "DeferredShadingPass";
	}

	struct ViewScore {
		std::string name;
		std::map<std::string, double> values;
	};

	bool ScoreView(
		const fs::path& mainDirectory, const fs::path& referenceDirectory, const nlohmann::json& view, const fs::path& reportDirectory, MetricSink& sink, std::string& outError)
	{
		const std::string viewName = view.at("name").get<std::string>();
		const Tags viewTag{ { "view", viewName } };
		Layers production, leaf;
		if (!ReadCaptureLayers(mainDirectory / viewName / "production", production, outError) || !ReadCaptureLayers(mainDirectory / viewName / "leaf", leaf, outError)) {
			return false;
		}
		Plane referenceHdr, referenceCoverage, referenceColor, referenceNormal;
		const fs::path reference = referenceDirectory / viewName / "reference";
		if (!image::ReadPlane(reference / "reduced-hdr.dds", referenceHdr, outError) || !image::ReadPlane(reference / "reduced-coverage.dds", referenceCoverage, outError) ||
			!image::ReadPlane(reference / "reduced-base-color.dds", referenceColor, outError) || !image::ReadPlane(reference / "reduced-normal-roughness.dds", referenceNormal, outError)) {
			return false;
		}
		image::KeyImage pathDebug, pathVisibility;
		if (!image::ReadKeys(mainDirectory / viewName / "voxel-path" / "debug.dds", pathDebug, outError) ||
			!image::ReadKeys(mainDirectory / viewName / "voxel-path" / "visibility.dds", pathVisibility, outError)) {
			return false;
		}
		if (production.hdr.width != referenceHdr.width || production.hdr.height != referenceHdr.height) {
			outError = std::format("'{}': the reference is {}x{} after reduction but the capture is {}x{}", viewName, referenceHdr.width, referenceHdr.height, production.hdr.width, production.hdr.height);
			return false;
		}
		const size_t pixels = production.hdr.Pixels();

		// Coverage.
		const image::CoverageComparison coverage = image::CompareCoverage(referenceCoverage, production.coverage);
		const image::CoverageComparison floorCoverage = image::CompareCoverage(referenceCoverage, leaf.coverage);
		sink.Emit("coverage.reference_pixels", coverage.referenceSum, Unit::Pixels, Better::Neutral, viewTag);
		sink.Emit("coverage.ratio", coverage.ratio, Unit::Ratio, Better::Neutral, viewTag);
		sink.Emit("coverage.soft_iou", coverage.softIou, Unit::Ratio, Better::Higher, viewTag);
		sink.Emit("coverage.excess_fraction", coverage.excessFraction, Unit::Ratio, Better::Lower, viewTag);
		sink.Emit("coverage.missing_fraction", coverage.missingFraction, Unit::Ratio, Better::Lower, viewTag);
		sink.Emit("coverage.floor_ratio", floorCoverage.ratio, Unit::Ratio, Better::Neutral, viewTag);
		sink.Emit("coverage.floor_soft_iou", floorCoverage.softIou, Unit::Ratio, Better::Higher, viewTag);
		// How much of what was drawn blocks the background in every frame. A voxel renderer that honours cell
		// opacity would have this well below 1 for a sparse asset, as the reference does.
		const auto fullyCovered = [](const Plane& plane) {
			size_t covered = 0, full = 0;
			for (const float value : plane.data) {
				covered += value > 0.0f ? 1u : 0u;
				full += value > 0.99f ? 1u : 0u;
			}
			return covered != 0u ? static_cast<double>(full) / static_cast<double>(covered) : 0.0;
		};
		sink.Emit("coverage.full_pixel_fraction", fullyCovered(production.coverage), Unit::Ratio, Better::Neutral, viewTag);
		sink.Emit("coverage.reference_full_pixel_fraction", fullyCovered(referenceCoverage), Unit::Ratio, Better::Neutral, viewTag);

		// Which path drew each pixel.
		const std::vector<uint8_t> voxels = VoxelMask(pathDebug, pathVisibility);
		size_t voxelPixels = 0, coveredPixels = 0;
		for (size_t index = 0; index < pixels; ++index) {
			voxelPixels += voxels[index];
			coveredPixels += pathVisibility.keys[index] != image::kEmptyKey ? 1u : 0u;
		}
		sink.Emit("path.covered_pixels", static_cast<double>(coveredPixels), Unit::Pixels, Better::Neutral, viewTag);
		sink.Emit("path.voxel_fraction", coveredPixels != 0u ? static_cast<double>(voxelPixels) / static_cast<double>(coveredPixels) : 0.0, Unit::Ratio, Better::Neutral, viewTag);

		// The final image, compared after a fixed tone map where anything was drawn by either side.
		std::vector<uint8_t> mask(pixels, 0u);
		size_t maskPixels = 0;
		for (size_t index = 0; index < pixels; ++index) {
			if (referenceCoverage.data[index] > 0.0f || production.coverage.data[index] > 0.0f) {
				mask[index] = 1u;
				++maskPixels;
			}
		}
		const float exposure = ExposureFromReference(referenceHdr, referenceCoverage);
		const Plane referenceImage = image::ToneMap(referenceHdr, exposure);
		const Plane productionImage = image::ToneMap(production.hdr, exposure);
		const Plane leafImage = image::ToneMap(leaf.hdr, exposure);
		if (maskPixels != 0u) {
			const Plane referenceLuma = image::Luminance(referenceImage), productionLuma = image::Luminance(productionImage), leafLuma = image::Luminance(leafImage);
			sink.Emit("image.psnr_db", image::Psnr(productionImage, referenceImage, mask), Unit::None, Better::Higher, viewTag);
			sink.Emit("image.ssim", image::Ssim(productionLuma, referenceLuma, mask), Unit::Ratio, Better::Higher, viewTag);
			sink.Emit("image.floor_psnr_db", image::Psnr(leafImage, referenceImage, mask), Unit::None, Better::Higher, viewTag);
			sink.Emit("image.floor_ssim", image::Ssim(leafLuma, referenceLuma, mask), Unit::Ratio, Better::Higher, viewTag);
			double sumProduction = 0.0, sumReference = 0.0;
			for (size_t index = 0; index < pixels; ++index) {
				if (mask[index] != 0u) {
					sumProduction += productionLuma.data[index];
					sumReference += referenceLuma.data[index];
				}
			}
			sink.Emit("image.brightness_ratio", sumReference > 0.0 ? sumProduction / sumReference : 0.0, Unit::Ratio, Better::Neutral, viewTag);
		}

		// Surface attributes, split by path.
		for (const bool voxelPath : { true, false }) {
			const SurfaceError error = CompareSurface(production, referenceColor, referenceNormal, referenceCoverage, voxels, voxelPath);
			if (error.pixels == 0u) {
				continue;
			}
			Tags tags = viewTag;
			tags["path"] = voxelPath ? "voxel" : "triangle";
			sink.Emit("surface.pixels", static_cast<double>(error.pixels), Unit::Pixels, Better::Neutral, tags);
			sink.Emit("surface.albedo_error", error.albedo, Unit::Ratio, Better::Lower, tags);
			sink.Emit("surface.albedo_brightness_ratio", error.albedoLumaRatio, Unit::Ratio, Better::Neutral, tags);
			sink.Emit("surface.normal_error_degrees", error.normalDegrees, Unit::None, Better::Lower, tags);
			sink.Emit("surface.roughness_error", error.roughness, Unit::Ratio, Better::Lower, tags);
		}

		// The error leaf triangles have at this resolution. Single-pixel attributes never match a 4x average
		// exactly, so a voxel error only means something above this.
		const SurfaceError floorSurface = CompareSurface(leaf, referenceColor, referenceNormal, referenceCoverage, std::vector<uint8_t>(pixels, 0u), false);
		if (floorSurface.pixels != 0u) {
			sink.Emit("surface.floor_albedo_error", floorSurface.albedo, Unit::Ratio, Better::Lower, viewTag);
			sink.Emit("surface.floor_normal_error_degrees", floorSurface.normalDegrees, Unit::None, Better::Lower, viewTag);
		}

		// Noise that accumulation hides and a single frame shows.
		for (const bool voxelPath : { true, false }) {
			double deviation = 0.0, luminance = 0.0;
			size_t count = 0;
			const Plane meanLuminance = image::Luminance(production.hdr);
			for (size_t index = 0; index < pixels; ++index) {
				if (production.coverage.data[index] > 0.0f && (voxels[index] != 0u) == voxelPath) {
					deviation += std::sqrt(std::max(production.variance.data[index], 0.0f));
					luminance += meanLuminance.data[index];
					++count;
				}
			}
			if (count != 0u && luminance > 0.0) {
				Tags tags = viewTag;
				tags["path"] = voxelPath ? "voxel" : "triangle";
				sink.Emit("temporal.relative_deviation", deviation / luminance, Unit::Ratio, Better::Lower, tags);
			}
		}

		// Measured voxel size. One visibility key is one cube of 4x4x4 cells in that frame.
		std::vector<double> extents = image::KeyExtents(pathVisibility, voxels);
		if (!extents.empty()) {
			std::vector<double> cells = extents;
			for (double& value : cells) {
				value /= 4.0;
			}
			EmitDistribution(sink, "voxel.footprint.cube_px", extents, Unit::Pixels, Better::Lower, viewTag);
			EmitDistribution(sink, "voxel.footprint.cell_px", cells, Unit::Pixels, Better::Lower, viewTag);
		}

		// Timings. Residency cannot be read here, because pages stay resident from earlier views and probes.
		for (const char* probeName : { "production", "leaf" }) {
			const nlohmann::json* probe = FindProbe(view, probeName);
			if (probe == nullptr) {
				continue;
			}
			Tags tags = viewTag;
			tags["probe"] = probeName;
			if (const auto passes = probe->find("pass_ms"); passes != probe->end()) {
				double total = 0.0;
				for (const auto& [pass, milliseconds] : passes->items()) {
					if (IsInterestingPass(pass)) {
						Tags passTags = tags;
						passTags["pass"] = pass;
						sink.Emit("perf.pass_ms", milliseconds.get<double>(), Unit::Milliseconds, Better::Lower, passTags);
						total += milliseconds.get<double>();
					}
				}
				sink.Emit("perf.clod_total_ms", total, Unit::Milliseconds, Better::Lower, tags);
			}
		}

		// Reference, production, error, coverage difference and path.
		ContactSheet sheet(production.hdr.width, production.hdr.height, 5u);
		const uint32_t width = production.hdr.width;
		sheet.Panel(0, [&](uint32_t x, uint32_t y, float* rgb) { for (uint32_t c = 0; c < 3u; ++c) rgb[c] = referenceImage.At(x, y, c); });
		sheet.Panel(1, [&](uint32_t x, uint32_t y, float* rgb) { for (uint32_t c = 0; c < 3u; ++c) rgb[c] = productionImage.At(x, y, c); });
		sheet.Panel(2, [&](uint32_t x, uint32_t y, float* rgb) {
			float error = 0.0f;
			for (uint32_t c = 0; c < 3u; ++c) error += std::abs(referenceImage.At(x, y, c) - productionImage.At(x, y, c));
			rgb[0] = rgb[1] = rgb[2] = error * 2.0f;
		});
		sheet.Panel(3, [&](uint32_t x, uint32_t y, float* rgb) {
			// Red: too much coverage. Blue: too little. Grey: the reference's coverage.
			const float r = referenceCoverage.At(x, y), t = production.coverage.At(x, y);
			rgb[0] = r * 0.25f + std::max(t - r, 0.0f);
			rgb[1] = r * 0.25f;
			rgb[2] = r * 0.25f + std::max(r - t, 0.0f);
		});
		sheet.Panel(4, [&](uint32_t x, uint32_t y, float* rgb) {
			// Red: voxels. Green: triangles.
			const size_t index = static_cast<size_t>(y) * width + x;
			rgb[0] = voxels[index] != 0u ? 0.95f : 0.0f;
			rgb[1] = voxels[index] == 0u && pathVisibility.keys[index] != image::kEmptyKey ? 0.8f : 0.0f;
			rgb[2] = 0.0f;
		});
		return sheet.Write(reportDirectory / (viewName + ".png"), outError);
	}

	bool ScoreSweep(const fs::path& sweepDirectory, const nlohmann::json& bundle, const SuiteCapture& capture, MetricSink& sink, std::string& outError)
	{
		for (const nlohmann::json& view : bundle.at("views")) {
			const std::string viewName = view.at("name").get<std::string>();
			std::vector<std::pair<double, Plane>> images;
			Plane firstCoverage;
			std::vector<uint8_t> mask;
			float exposure = 1.0f;
			for (double threshold : capture.thresholds) {
				const fs::path directory = sweepDirectory / viewName / std::format("t{}", Fixed(threshold));
				Plane hdr, coverage;
				if (!image::ReadPlane(directory / "hdr-mean.dds", hdr, outError) || !image::ReadPlane(directory / "coverage-mean.dds", coverage, outError)) {
					return false;
				}
				if (images.empty()) {
					exposure = ExposureFromReference(hdr, coverage);
					mask.assign(coverage.Pixels(), 0u);
				}
				// How coverage and the share of voxels follow the threshold shows whether extra coverage comes
				// from voxel size: if it does, finer voxels shrink it before triangles take over.
				{
					Tags tags{ { "view", viewName }, { "threshold", Fixed(threshold) } };
					double sum = 0.0;
					for (const float value : coverage.data) {
						sum += value;
					}
					sink.Emit("sweep.coverage_pixels", sum, Unit::Pixels, Better::Neutral, tags);
					image::KeyImage pathDebug, pathVisibility;
					const fs::path pathDirectory = sweepDirectory / viewName / std::format("p{}", Fixed(threshold));
					if (image::ReadKeys(pathDirectory / "debug.dds", pathDebug, outError) && image::ReadKeys(pathDirectory / "visibility.dds", pathVisibility, outError)) {
						const std::vector<uint8_t> voxels = VoxelMask(pathDebug, pathVisibility);
						size_t voxelPixels = 0, covered = 0;
						for (size_t index = 0; index < voxels.size(); ++index) {
							voxelPixels += voxels[index];
							covered += pathVisibility.keys[index] != image::kEmptyKey ? 1u : 0u;
						}
						sink.Emit("sweep.voxel_fraction", covered != 0u ? static_cast<double>(voxelPixels) / static_cast<double>(covered) : 0.0, Unit::Ratio, Better::Neutral, tags);
						std::vector<double> extents = image::KeyExtents(pathVisibility, voxels);
						if (!extents.empty()) {
							EmitDistribution(sink, "sweep.cube_px", extents, Unit::Pixels, Better::Lower, tags);
						}
					}
					else {
						return false;
					}
				}
				for (size_t index = 0; index < coverage.Pixels(); ++index) {
					mask[index] = mask[index] != 0u || coverage.data[index] > 0.0f ? 1u : 0u;
				}
				images.emplace_back(threshold, image::ToneMap(hdr, exposure));
			}
			for (size_t step = 1; step < images.size(); ++step) {
				const Plane& a = images[step - 1u].second;
				const Plane& b = images[step].second;
				double total = 0.0;
				size_t count = 0, changed = 0;
				for (size_t index = 0; index < mask.size(); ++index) {
					if (mask[index] == 0u) {
						continue;
					}
					double difference = 0.0;
					for (uint32_t channel = 0; channel < 3u; ++channel) {
						difference += std::abs(static_cast<double>(a.data[index * 3u + channel]) - b.data[index * 3u + channel]) / 3.0;
					}
					total += difference;
					changed += difference > 0.02 ? 1u : 0u;
					++count;
				}
				const Tags tags{ { "view", viewName }, { "from", Fixed(images[step - 1u].first) }, { "to", Fixed(images[step].first) } };
				sink.Emit("transition.mean_abs_delta", count != 0u ? total / static_cast<double>(count) : 0.0, Unit::Ratio, Better::Lower, tags);
				sink.Emit("transition.changed_fraction", count != 0u ? static_cast<double>(changed) / static_cast<double>(count) : 0.0, Unit::Ratio, Better::Lower, tags);
			}
		}
		return true;
	}
}

// A voxel pixel is written red and a triangle pixel green by OUTPUT_VOXEL_GEOMETRY_PATH.
std::vector<uint8_t> VoxelPathMask(const image::KeyImage& debug, const image::KeyImage& visibility)
{
	std::vector<uint8_t> mask(debug.keys.size(), 0u);
	for (size_t index = 0; index < mask.size(); ++index) {
		if (visibility.keys[index] == image::kEmptyKey) {
			continue;
		}
		const float red = image::HalfToFloat(static_cast<uint16_t>(debug.keys[index] & 0xFFFFu));
		const float green = image::HalfToFloat(static_cast<uint16_t>((debug.keys[index] >> 16u) & 0xFFFFu));
		mask[index] = red > green ? 1u : 0u;
	}
	return mask;
}

std::vector<RigView> MakeRig(const SuiteCapture& capture, const AssetFrame& frame)
{
	std::vector<RigView> views;
	const auto add = [&](std::string name, double distance, double offsetX, double offsetY) {
		RigView view;
		view.name = std::move(name);
		view.position[0] = frame.center[0] + offsetX;
		view.position[1] = frame.center[1] + distance * 0.15 + offsetY;
		view.position[2] = frame.center[2] + distance;
		view.lookAt[0] = frame.center[0] + offsetX;
		view.lookAt[1] = frame.center[1] + offsetY;
		view.lookAt[2] = frame.center[2];
		views.push_back(std::move(view));
	};
	for (const double radii : capture.distances) {
		add(std::format("d{}", Fixed(radii)), frame.radius * radii, 0.0, 0.0);
	}
	// Moves the camera sideways so the asset lands off-axis, where voxels stretch and the LOD test does not notice.
	const double tangent = std::tan(capture.fovDegrees * 0.5 * 3.14159265358979323846 / 180.0);
	const double aspect = static_cast<double>(capture.width) / capture.height;
	for (const double radii : capture.cornerDistances) {
		const double distance = frame.radius * radii;
		add(std::format("corner{}", Fixed(radii)), distance, -capture.cornerNdc * tangent * aspect * distance, -capture.cornerNdc * tangent * distance);
	}
	return views;
}

std::vector<RigView> MakeSweepRig(const SuiteCapture& capture, const AssetFrame& frame)
{
	std::vector<RigView> views;
	for (const double radii : capture.sweepDistances) {
		const double distance = frame.radius * radii;
		RigView view;
		view.name = std::format("d{}", Fixed(radii));
		view.position[0] = frame.center[0];
		view.position[1] = frame.center[1] + distance * 0.15;
		view.position[2] = frame.center[2] + distance;
		view.lookAt[0] = frame.center[0];
		view.lookAt[1] = frame.center[1];
		view.lookAt[2] = frame.center[2];
		views.push_back(std::move(view));
	}
	return views;
}

bool CaptureVariant(const Suite& suite, const SuiteVariant& variant, const CaptureOptions& options, std::string& outError)
{
	if (!suite.capture.enabled) {
		outError = "the suite has no capture block";
		return false;
	}
	std::error_code error;
	if (!fs::exists(options.captureTool, error)) {
		outError = std::format("capture tool not found at '{}'", options.captureTool.string());
		return false;
	}
	const SuiteCapture& capture = suite.capture;
	for (const SuiteAsset& asset : suite.assets) {
		// Every variant uses the first variant's frame. Bounds differ between builds, and images of the same source
		// from different cameras cannot be compared.
		AssetFrame frame;
		if (capture.hasFrame) {
			std::copy_n(capture.frameCenter, 3, frame.center);
			frame.radius = capture.frameRadius;
		}
		else if (!GetAssetFrame(suite, suite.variants.front(), asset, frame.center, frame.radius, outError)) {
			return false;
		}
		const std::vector<RigView> rig = MakeRig(capture, frame);

		if (capture.transition) {
			const CaptureRunner runner = [&](const fs::path& directory, const SuiteCapture& jobCapture, const std::vector<RigView>& views, nlohmann::json probes, bool force, std::string& runError) {
				return RunJob(suite, variant, options, directory, MakeJob(asset, directory, jobCapture, frame, views, 1u, std::move(probes)), force, runError);
			};
			if (!CaptureTransition(suite, variant, asset, frame, runner, options.force, outError)) {
				return false;
			}
		}
		if (options.transitionOnly) {
			continue;
		}

		const std::vector<std::string> allLayers{ "visibility", "base-color-opacity", "normal-roughness" };
		nlohmann::json probes = nlohmann::json::array();
		probes.push_back(MakeProbe("production", 1.0, capture.accumulateFrames, allLayers, kOutputColor));
		probes.push_back(MakeProbe("voxel-path", 1.0, 0u, { "visibility", "debug" }, kOutputVoxelGeometryPath));
		probes.push_back(MakeProbe("leaf", -1.0, capture.accumulateFrames, allLayers, kOutputColor));
		const fs::path mainDirectory = CaptureDirectory(suite, variant, asset, "main");
		if (!RunJob(suite, variant, options, mainDirectory, MakeJob(asset, mainDirectory, capture, frame, rig, 1u, probes), options.force, outError)) {
			return false;
		}

		// Leaf triangles do not change between frames, so the reference only needs a few.
		nlohmann::json referenceProbes = nlohmann::json::array();
		referenceProbes.push_back(MakeProbe("reference", -1.0, 4u, allLayers, kOutputColor));
		const fs::path referenceDirectory = CaptureDirectory(suite, variant, asset, "reference");
		const std::string reducedMark = (referenceDirectory / "reduced.txt").string();
		const bool hadReduced = fs::exists(reducedMark, error);
		if (!RunJob(suite, variant, options, referenceDirectory, MakeJob(asset, referenceDirectory, capture, frame, rig, capture.referenceScale, referenceProbes), options.force, outError)) {
			return false;
		}
		if (!hadReduced || !fs::exists(reducedMark, error)) {
			for (const RigView& view : rig) {
				const fs::path directory = referenceDirectory / view.name / "reference";
				if (fs::exists(directory / "hdr-mean.dds", error) && !ReduceReference(directory, capture.referenceScale, outError)) {
					return false;
				}
			}
			std::ofstream(reducedMark) << "reduced\n";
		}

		// Far to near in a fresh process, with nothing rendered at a finer level first. Pages are not evicted while
		// there is room, so each number is what this view and all farther ones need, which is close to what this
		// view alone needs because a nearer view's cut uses much the same coarse pages.
		if (!capture.distances.empty()) {
			SuiteCapture onAxis = capture;
			onAxis.cornerDistances.clear();
			std::ranges::sort(onAxis.distances, std::greater<>{});
			nlohmann::json residencyProbes = nlohmann::json::array();
			residencyProbes.push_back(MakeProbe("production", 1.0, 0u, {}, kOutputColor));
			const fs::path residencyDirectory = CaptureDirectory(suite, variant, asset, "residency");
			if (!RunJob(suite, variant, options, residencyDirectory, MakeJob(asset, residencyDirectory, capture, frame, MakeRig(onAxis, frame), 1u, residencyProbes), options.force, outError)) {
				return false;
			}
		}

		if (!capture.thresholds.empty() && !capture.sweepDistances.empty()) {
			nlohmann::json sweepProbes = nlohmann::json::array();
			for (const double threshold : capture.thresholds) {
				sweepProbes.push_back(MakeProbe(std::format("t{}", Fixed(threshold)), threshold, capture.accumulateFrames, {}, kOutputColor));
				sweepProbes.push_back(MakeProbe(std::format("p{}", Fixed(threshold)), threshold, 0u, { "visibility", "debug" }, kOutputVoxelGeometryPath));
			}
			const fs::path sweepDirectory = CaptureDirectory(suite, variant, asset, "sweep");
			if (!RunJob(suite, variant, options, sweepDirectory, MakeJob(asset, sweepDirectory, capture, frame, MakeSweepRig(capture, frame), 1u, sweepProbes), options.force, outError)) {
				return false;
			}
		}
	}
	return true;
}

bool RunViewer(const Suite& suite, const SuiteVariant& variant, const SuiteAsset& asset, const ViewerOptions& options, std::string& outError)
{
	double center[3]{};
	double radius = 0.0;
	if (!GetAssetFrame(suite, variant, asset, center, radius, outError)) {
		return false;
	}
	const double transitionFov = suite.capture.transitionFovDegrees;
	const auto distanceForPixel = [&](double pixelWorld) { return TelephotoDistance(pixelWorld * options.height, transitionFov); };

	nlohmann::json presets = nlohmann::json::array();
	presets.push_back({
		{ "name", "Telephoto, whole asset" },
		{ "focus", { center[0], center[1], center[2] } },
		{ "distance", TelephotoDistance(radius, transitionFov) },
		{ "fov_degrees", transitionFov },
	});

	// What the transition capture found, if it has run: the same views, at this window's size.
	nlohmann::json curve = nlohmann::json::array();
	const fs::path transitionPath = suite.workspace / "captures" / variant.name / asset.name / "transition.json";
	if (std::ifstream transitionFile(transitionPath); transitionFile) {
		const nlohmann::json transition = nlohmann::json::parse(transitionFile);
		const double wholePixel = transition.value("whole_pixel_world", 0.0);
		for (const nlohmann::json& point : transition.value("curve", nlohmann::json::array())) {
			curve.push_back({ point.at(0).get<double>() * wholePixel, point.at(1) });
		}
		for (const nlohmann::json& stage : transition.value("stages", nlohmann::json::array())) {
			const double pixelWorld = stage.at("pixel_world").get<double>();
			presets.push_back({
				{ "name", std::format("Telephoto, transition '{}' at {:.2f} mm a pixel", stage.at("name").get<std::string>(), pixelWorld * 1000.0) },
				{ "focus", { stage.at("focus").at(0), stage.at("focus").at(1), center[2] } },
				{ "distance", distanceForPixel(pixelWorld) },
				{ "fov_degrees", transitionFov },
				{ "world_cut", pixelWorld },
			});
		}
	}

	const fs::path directory = suite.workspace / "view" / variant.name / asset.name;
	std::error_code error;
	fs::create_directories(directory, error);
	const nlohmann::json config{
		{ "title", std::format("{} / {} / {}", suite.name, variant.name, asset.name) },
		{ "assets", { { { "path", asset.path.generic_string() } } } },
		{ "resolution", { options.width, options.height } },
		{ "cache_root", GetBuildPaths(suite, variant).cacheRoot.generic_string() },
		{ "frame", { { "center", { center[0], center[1], center[2] } }, { "radius", radius } } },
		{ "presets", std::move(presets) },
		{ "curve", std::move(curve) },
		{ "screenshot_directory", (directory / "screenshots").generic_string() },
		{ "part_filter", options.partFilter },
	};
	const fs::path configPath = directory / "viewer.json";
	std::ofstream(configPath) << config.dump(2) << '\n';

	ProcessRequest request = MakeCaptureProcess(suite, variant, options.captureTool);
	request.logPath = directory / "viewer.log";
	request.arguments = { "--view", configPath.string() };
	if (!options.script.empty()) {
		request.arguments.push_back("--script");
		request.arguments.push_back(options.script.string());
	}
	const int exitCode = RunProcess(request, outError);
	if (exitCode != 0) {
		if (outError.empty()) {
			outError = std::format("the viewer exited with {}; see '{}'", exitCode, request.logPath.string());
		}
		return false;
	}
	return true;
}

fs::path GetScoreResultsPath(const Suite& suite, const std::string& variantName)
{
	return suite.workspace / "results" / (variantName + ".images.jsonl");
}

bool ScoreVariant(const Suite& suite, const SuiteVariant& variant, bool transitionOnly, ResultSet& outResults, std::string& outError)
{
	MetricSink sink;
	for (const SuiteAsset& asset : suite.assets) {
		sink.SetScope({ "image.score", kAnalyzerVersion, variant.name, asset.name, std::string(kAssetRollupMesh) });
		if (!ScoreTransition(suite, variant, asset, sink, outError)) {
			return false;
		}
		if (transitionOnly) {
			continue;
		}
		const fs::path mainDirectory = CaptureDirectory(suite, variant, asset, "main");
		const fs::path referenceDirectory = CaptureDirectory(suite, variant, asset, "reference");
		nlohmann::json bundle;
		if (!FindViewsAndCheck(mainDirectory / "bundle.json", bundle, outError)) {
			return false;
		}
		const fs::path reportDirectory = suite.workspace / "report" / variant.name / asset.name;
		std::error_code error;
		fs::create_directories(reportDirectory, error);
		for (const nlohmann::json& view : bundle.at("views")) {
			if (!ScoreView(mainDirectory, referenceDirectory, view, reportDirectory, sink, outError)) {
				return false;
			}
		}
		const fs::path residencyDirectory = CaptureDirectory(suite, variant, asset, "residency");
		if (fs::exists(residencyDirectory / "bundle.json", error)) {
			nlohmann::json residencyBundle;
			if (!FindViewsAndCheck(residencyDirectory / "bundle.json", residencyBundle, outError)) {
				return false;
			}
			for (const nlohmann::json& view : residencyBundle.at("views")) {
				const nlohmann::json* probe = FindProbe(view, "production");
				if (probe != nullptr) {
					const Tags tags{ { "view", view.at("name").get<std::string>() } };
					sink.Emit("residency.production_bytes", probe->at("readiness").value("resident_clod_allocation_bytes", 0.0), Unit::Bytes, Better::Lower, tags);
					sink.Emit("residency.production_groups", probe->at("readiness").value("resident_clod_groups", 0.0), Unit::Count, Better::Lower, tags);
				}
			}
		}
		const fs::path sweepDirectory = CaptureDirectory(suite, variant, asset, "sweep");
		if (fs::exists(sweepDirectory / "bundle.json", error)) {
			nlohmann::json sweepBundle;
			if (!FindViewsAndCheck(sweepDirectory / "bundle.json", sweepBundle, outError) || !ScoreSweep(sweepDirectory, sweepBundle, suite.capture, sink, outError)) {
				return false;
			}
		}
	}
	outResults.build = variant.name;
	outResults.provenance = ProvenanceJson();
	outResults.records = sink.Finish();
	if (!sink.DroppedNonFinite().empty()) {
		outError = std::format("{} non-finite metric(s), first: {}", sink.DroppedNonFinite().size(), sink.DroppedNonFinite().front());
		return false;
	}
	return true;
}

namespace {
	std::string Escape(std::string_view text)
	{
		std::string result;
		for (const char character : text) {
			switch (character) {
			case '&': result += "&amp;"; break;
			case '<': result += "&lt;"; break;
			case '>': result += "&gt;"; break;
			case '"': result += "&quot;"; break;
			default: result += character; break;
			}
		}
		return result;
	}

	std::string FormatValue(double value)
	{
		const double magnitude = std::abs(value);
		if (magnitude != 0.0 && (magnitude >= 1.0e6 || magnitude < 1.0e-3)) {
			return std::format("{:.3g}", value);
		}
		return std::format("{:.3f}", value);
	}

	const MetricRecord* Find(const std::vector<MetricRecord>& records, std::string_view id, const Tags& tags)
	{
		for (const MetricRecord& record : records) {
			if (record.id == id && record.tags == tags && record.mesh == kAssetRollupMesh) {
				return &record;
			}
		}
		return nullptr;
	}
}

bool WriteReport(const Suite& suite, fs::path& outPath, std::string& outError)
{
	std::ostringstream html;
	html << "<!doctype html><html><head><meta charset=\"utf-8\"><title>CLod evaluation: " << Escape(suite.name) << "</title><style>"
		"body{font:14px system-ui,sans-serif;margin:24px;max-width:1500px;color:#222}"
		"table{border-collapse:collapse;margin:8px 0 24px}td,th{border:1px solid #ccc;padding:3px 8px;text-align:right}th{background:#f2f2f2}"
		"td:first-child,th:first-child{text-align:left}img{width:100%;display:block;margin:4px 0 2px}h3{margin:18px 0 0}"
		"</style></head><body><h1>CLod evaluation: " << Escape(suite.name) << "</h1>"
		"<p>Sheets, left to right: reference, production, error (x2), coverage difference (red too much, blue too little), path (red voxels, green triangles).</p>";

	struct Column {
		const char* label;
		const char* id;
		Tags extra;
	};
	const std::vector<Column> columns{
		{ "voxel px %", "path.voxel_fraction", {} },
		{ "PSNR dB", "image.psnr_db", {} },
		{ "floor PSNR", "image.floor_psnr_db", {} },
		{ "SSIM", "image.ssim", {} },
		{ "floor SSIM", "image.floor_ssim", {} },
		{ "brightness", "image.brightness_ratio", {} },
		{ "coverage", "coverage.ratio", {} },
		{ "floor cov", "coverage.floor_ratio", {} },
		{ "full px", "coverage.full_pixel_fraction", {} },
		{ "soft IoU", "coverage.soft_iou", {} },
		{ "floor IoU", "coverage.floor_soft_iou", {} },
		{ "albedo err (vox)", "surface.albedo_error", { { "path", "voxel" } } },
		{ "albedo ratio (vox)", "surface.albedo_brightness_ratio", { { "path", "voxel" } } },
		{ "normal deg (vox)", "surface.normal_error_degrees", { { "path", "voxel" } } },
		{ "normal deg (tri)", "surface.normal_error_degrees", { { "path", "triangle" } } },
		{ "floor normal", "surface.floor_normal_error_degrees", {} },
		{ "noise (vox)", "temporal.relative_deviation", { { "path", "voxel" } } },
		{ "cell px p50", "voxel.footprint.cell_px", { { "stat", "p50" } } },
		{ "cell px p95", "voxel.footprint.cell_px", { { "stat", "p95" } } },
		{ "resident MB", "residency.production_bytes", {} },
		{ "CLod ms", "perf.clod_total_ms", { { "probe", "production" } } },
	};

	for (const SuiteVariant& variant : suite.variants) {
		html << "<h2>" << Escape(variant.name) << "</h2>";
		ResultSet results;
		const fs::path scorePath = GetScoreResultsPath(suite, variant.name);
		std::error_code error;
		if (!fs::exists(scorePath, error) || !ReadResults(scorePath, results, outError)) {
			html << "<p>Not scored.</p>";
			outError.clear();
			continue;
		}
		std::vector<std::string> views;
		std::set<std::string> seen;
		for (const MetricRecord& record : results.records) {
			if (record.id == "path.voxel_fraction" && seen.insert(record.tags.at("view")).second) {
				views.push_back(record.tags.at("view"));
			}
		}
		for (const SuiteAsset& asset : suite.assets) {
			html << "<table><tr><th>view</th>";
			for (const Column& column : columns) {
				html << "<th>" << column.label << "</th>";
			}
			html << "</tr>";
			for (const std::string& view : views) {
				html << "<tr><td>" << Escape(view) << "</td>";
				for (const Column& column : columns) {
					Tags tags = column.extra;
					tags["view"] = view;
					const MetricRecord* record = Find(results.records, column.id, tags);
					double value = record != nullptr ? record->value : 0.0;
					if (record != nullptr && column.label == std::string_view("voxel px %")) {
						value *= 100.0;
					}
					if (record != nullptr && record->unit == Unit::Bytes) {
						value /= 1.0e6;
					}
					html << "<td>" << (record != nullptr ? FormatValue(value) : "-") << "</td>";
				}
				html << "</tr>";
			}
			html << "</table>";
			for (const char* stage : { "first", "last" }) {
				if (fs::exists(suite.workspace / "report" / variant.name / asset.name / std::format("transition_{}.png", stage), error)) {
					html << "<h3>Triangle-to-voxel transition: " << stage << " parts to switch</h3><p>Rows: lit, base colour, normal. Columns: just before, just after, difference, path just after (red voxels, dark red voxels with no surface before, green triangles, blue surface lost).</p>";
					html << "<img loading=\"lazy\" src=\"" << Escape(variant.name) << "/" << Escape(asset.name) << "/transition_" << stage << ".png\">";
					html << "<img loading=\"lazy\" src=\"" << Escape(variant.name) << "/" << Escape(asset.name) << "/transition_" << stage << "_zoom4.png\">";
				}
			}
			for (const std::string& view : views) {
				html << "<h3>" << Escape(view) << "</h3><img loading=\"lazy\" src=\"" << Escape(variant.name) << "/" << Escape(asset.name) << "/" << Escape(view) << ".png\">";
			}
		}
	}
	html << "</body></html>\n";

	const fs::path directory = suite.workspace / "report";
	std::error_code error;
	fs::create_directories(directory, error);
	outPath = directory / "index.html";
	std::ofstream output(outPath, std::ios::trunc);
	output << html.str();
	output.flush();
	if (!output) {
		outError = std::format("failed to write '{}'", outPath.string());
		return false;
	}
	return true;
}

}
