#include "Transition.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "Capture.h"
#include "Core/Image.h"
#include "Core/Metrics.h"

namespace clodeval {
namespace fs = std::filesystem;
using image::Plane;

namespace {
	constexpr uint32_t kOutputColor = 0u;
	constexpr uint32_t kOutputVoxelGeometryPath = 34u;  // OutputType::VOXEL_GEOMETRY_PATH
	constexpr double kPi = 3.14159265358979323846;

	std::string Fixed(double value) { return std::format("{:g}", value); }

	nlohmann::json MakeProbe(const std::string& name, double threshold, uint32_t frames, std::vector<std::string> layers, uint32_t outputType)
	{
		return {
			{ "name", name },
			{ "lod_error_pixels", threshold },
			{ "accumulate_frames", frames },
			{ "layers", std::move(layers) },
			{ "settings", { { "outputType", outputType } } },
		};
	}

	double ReadVoxelFraction(const fs::path& directory, std::vector<uint8_t>* outVoxels, image::KeyImage* outVisibility, std::string& outError, bool& outOk)
	{
		image::KeyImage debug, visibility;
		outOk = image::ReadKeys(directory / "debug.dds", debug, outError) && image::ReadKeys(directory / "visibility.dds", visibility, outError);
		if (!outOk) {
			return 0.0;
		}
		std::vector<uint8_t> voxels = VoxelPathMask(debug, visibility);
		size_t voxelPixels = 0, covered = 0;
		for (size_t index = 0; index < voxels.size(); ++index) {
			voxelPixels += voxels[index];
			covered += visibility.keys[index] != image::kEmptyKey ? 1u : 0u;
		}
		if (outVoxels != nullptr) {
			*outVoxels = std::move(voxels);
		}
		if (outVisibility != nullptr) {
			*outVisibility = std::move(visibility);
		}
		return covered != 0u ? static_cast<double>(voxelPixels) / static_cast<double>(covered) : 0.0;
	}

	double MeanGradient(const Plane& luminance, const std::vector<uint8_t>& mask)
	{
		double total = 0.0;
		size_t count = 0;
		for (uint32_t y = 0; y + 1u < luminance.height; ++y) {
			for (uint32_t x = 0; x + 1u < luminance.width; ++x) {
				if (mask[static_cast<size_t>(y) * luminance.width + x] == 0u) {
					continue;
				}
				total += std::abs(luminance.At(x + 1u, y) - luminance.At(x, y)) + std::abs(luminance.At(x, y + 1u) - luminance.At(x, y));
				++count;
			}
		}
		return count != 0u ? total / static_cast<double>(count) : 0.0;
	}

	struct DetailFrame {
		double threshold = 0.0;
		double voxelFraction = 0.0;
		Plane hdr, coverage, variance, baseColor, normal;
		std::vector<uint8_t> voxels;
		image::KeyImage visibility;
	};

	bool ReadDetail(const fs::path& view, double threshold, DetailFrame& out, std::string& outError)
	{
		out.threshold = threshold;
		const fs::path color = view / std::format("t{}", Fixed(threshold));
		bool ok = false;
		out.voxelFraction = ReadVoxelFraction(view / std::format("p{}", Fixed(threshold)), &out.voxels, &out.visibility, outError, ok);
		return ok && image::ReadPlane(color / "hdr-mean.dds", out.hdr, outError) && image::ReadPlane(color / "coverage-mean.dds", out.coverage, outError) &&
			image::ReadPlane(color / "hdr-luminance-variance.dds", out.variance, outError) && image::ReadPlane(color / "base-color-opacity.dds", out.baseColor, outError) &&
			image::ReadPlane(color / "normal-roughness.dds", out.normal, outError);
	}

	uint8_t ToByte(float value) { return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f); }

	// Panels side by side, each the centre of the image at `zoom` with no filtering, so a voxel keeps its edges.
	template <typename Sampler>
	void AddPanel(std::vector<uint8_t>& rgb, uint32_t sheetWidth, uint32_t panel, uint32_t panelWidth, uint32_t panelHeight, uint32_t imageWidth, uint32_t imageHeight, uint32_t zoom, Sampler&& sample)
	{
		const uint32_t startX = (imageWidth - panelWidth / zoom) / 2u;
		const uint32_t startY = (imageHeight - panelHeight / zoom) / 2u;
		for (uint32_t y = 0; y < panelHeight; ++y) {
			for (uint32_t x = 0; x < panelWidth; ++x) {
				float color[3]{};
				sample(startX + x / zoom, startY + y / zoom, color);
				const size_t offset = (static_cast<size_t>(y) * sheetWidth + static_cast<size_t>(panel) * panelWidth + x) * 3u;
				for (int channel = 0; channel < 3; ++channel) {
					rgb[offset + channel] = ToByte(color[channel]);
				}
			}
		}
	}

	std::string ReadText(const fs::path& path)
	{
		std::ifstream input(path);
		std::ostringstream text;
		text << input.rdbuf();
		return text.str();
	}
}

double TelephotoDistance(double worldHeight, double fovDegrees)
{
	return worldHeight / (2.0 * std::tan(fovDegrees * 0.5 * kPi / 180.0));
}

TransitionPick PickTransition(const std::vector<std::pair<double, double>>& curve)
{
	TransitionPick pick;
	for (const auto& [threshold, fraction] : curve) {
		if (!pick.hasLow && fraction >= 0.05) {
			pick.low = threshold;
			pick.hasLow = true;
		}
		if (!pick.hasMid && fraction >= 0.5) {
			pick.mid = threshold;
			pick.hasMid = true;
		}
		if (!pick.hasHigh && fraction >= 0.95) {
			pick.high = threshold;
			pick.hasHigh = true;
		}
	}
	return pick;
}

std::vector<double> FineThresholds(uint32_t steps)
{
	constexpr double kLow = 0.25, kHigh = 2.5;
	std::vector<double> thresholds;
	for (uint32_t step = 0; step < steps; ++step) {
		const double blend = steps > 1u ? static_cast<double>(step) / (steps - 1u) : 0.0;
		thresholds.push_back(std::exp(std::log(kLow) + (std::log(kHigh) - std::log(kLow)) * blend));
	}
	thresholds.push_back(1.0);
	std::ranges::sort(thresholds);
	return thresholds;
}

std::pair<double, double> PickBeforeAfter(const std::vector<std::pair<double, double>>& fractions)
{
	// After is the first frame at 90% of what this sweep reaches, not at a fixed share, because a part of the view
	// may switch much later. That part stays triangles here and gets its own view at its own scale.
	double reached = 0.0;
	for (const auto& [threshold, fraction] : fractions) {
		reached = std::max(reached, fraction);
	}
	double after = fractions.back().first;
	for (auto it = fractions.rbegin(); it != fractions.rend(); ++it) {
		if (it->second >= reached * 0.9) {
			after = it->first;
		}
	}
	// Before is the last frame with almost no voxels. If voxels are already there when the sweep starts, it is the start.
	double before = fractions.front().first;
	for (const auto& [threshold, fraction] : fractions) {
		if (threshold < after && fraction < 0.02) {
			before = threshold;
		}
	}
	return { before, after };
}

bool CaptureTransition(const Suite& suite, const SuiteVariant& variant, const SuiteAsset& asset, const AssetFrame& frame, const CaptureRunner& run, bool force, std::string& outError)
{
	const SuiteCapture& capture = suite.capture;
	SuiteCapture telephoto = capture;
	telephoto.fovDegrees = capture.transitionFovDegrees;

	const auto makeView = [&](const std::string& name, double focusX, double focusY, double worldHeight) {
		const double distance = TelephotoDistance(worldHeight, telephoto.fovDegrees);
		RigView view;
		view.name = name;
		view.position[0] = focusX;
		view.position[1] = focusY;
		view.position[2] = frame.center[2] + distance;
		view.lookAt[0] = focusX;
		view.lookAt[1] = focusY;
		view.lookAt[2] = frame.center[2];
		return view;
	};
	// Fits every view of both jobs. The near plane stays well in front of the asset.
	telephoto.farRadii = TelephotoDistance(frame.radius, telephoto.fovDegrees) / frame.radius + 4.0;

	// Scout: the whole asset, sweeping the threshold over a wide range to find where it flips to voxels.
	const fs::path root = suite.workspace / "captures" / variant.name / asset.name;
	const double wholeHeight = frame.radius;
	const double wholePixel = wholeHeight / capture.height;
	std::vector<double> scoutThresholds;
	for (double threshold = 1.0 / 128.0; threshold <= 4.01; threshold *= std::sqrt(2.0)) {
		scoutThresholds.push_back(threshold);
	}
	nlohmann::json scoutProbes = nlohmann::json::array();
	for (const double threshold : scoutThresholds) {
		scoutProbes.push_back(MakeProbe(std::format("p{}", Fixed(threshold)), threshold, 0u, { "visibility", "debug" }, kOutputVoxelGeometryPath));
	}
	const fs::path scoutDirectory = root / "transition-scout";
	const RigView scoutView = makeView("whole", frame.center[0], frame.center[1], wholeHeight);
	if (!run(scoutDirectory, telephoto, { scoutView }, std::move(scoutProbes), force, outError)) {
		return false;
	}
	std::vector<std::pair<double, double>> curve;
	for (const double threshold : scoutThresholds) {
		bool ok = false;
		const double fraction = ReadVoxelFraction(scoutDirectory / "whole" / std::format("p{}", Fixed(threshold)), nullptr, nullptr, outError, ok);
		if (!ok) {
			return false;
		}
		curve.emplace_back(threshold, fraction);
	}
	const TransitionPick pick = PickTransition(curve);

	nlohmann::json record;
	record["whole_pixel_world"] = wholePixel;
	record["curve"] = curve;
	record["has_transition"] = pick.hasMid;
	const fs::path recordPath = root / "transition.json";
	if (!pick.hasMid) {
		std::ofstream(recordPath) << record.dump(2) << '\n';
		return true;
	}
	record["low"] = pick.hasLow ? pick.low : pick.mid;
	record["mid"] = pick.mid;
	record["high"] = pick.hasHigh ? pick.high : pick.mid;

	record["stages"] = nlohmann::json::array();
	const auto addStage = [&](const std::string& name, double scoutThreshold) {
		// Focus on the window that holds the most voxels at the size the detail view will have.
		std::vector<uint8_t> voxels;
		image::KeyImage visibility;
		bool ok = false;
		(void)ReadVoxelFraction(scoutDirectory / "whole" / std::format("p{}", Fixed(scoutThreshold)), &voxels, &visibility, outError, ok);
		if (!ok) {
			return false;
		}
		const uint32_t scoutWidth = visibility.width, scoutHeight = visibility.height;
		const uint32_t windowWidth = std::clamp<uint32_t>(static_cast<uint32_t>(capture.width * scoutThreshold), 8u, scoutWidth);
		const uint32_t windowHeight = std::clamp<uint32_t>(static_cast<uint32_t>(capture.height * scoutThreshold), 8u, scoutHeight);
		std::vector<uint32_t> sum(static_cast<size_t>(scoutWidth + 1u) * (scoutHeight + 1u), 0u);
		for (uint32_t y = 0; y < scoutHeight; ++y) {
			for (uint32_t x = 0; x < scoutWidth; ++x) {
				sum[static_cast<size_t>(y + 1u) * (scoutWidth + 1u) + x + 1u] = voxels[static_cast<size_t>(y) * scoutWidth + x] + sum[static_cast<size_t>(y) * (scoutWidth + 1u) + x + 1u] +
					sum[static_cast<size_t>(y + 1u) * (scoutWidth + 1u) + x] - sum[static_cast<size_t>(y) * (scoutWidth + 1u) + x];
			}
		}
		uint32_t bestX = 0, bestY = 0, bestCount = 0;
		for (uint32_t y = 0; y + windowHeight <= scoutHeight; ++y) {
			for (uint32_t x = 0; x + windowWidth <= scoutWidth; ++x) {
				const uint32_t count = sum[static_cast<size_t>(y + windowHeight) * (scoutWidth + 1u) + x + windowWidth] - sum[static_cast<size_t>(y) * (scoutWidth + 1u) + x + windowWidth] -
					sum[static_cast<size_t>(y + windowHeight) * (scoutWidth + 1u) + x] + sum[static_cast<size_t>(y) * (scoutWidth + 1u) + x];
				if (count > bestCount) {
					bestCount = count;
					bestX = x;
					bestY = y;
				}
			}
		}
		const double focusX = frame.center[0] + (bestX + windowWidth * 0.5 - scoutWidth * 0.5) * wholePixel;
		const double focusY = frame.center[1] - (bestY + windowHeight * 0.5 - scoutHeight * 0.5) * wholePixel;
		const double detailPixel = wholePixel * scoutThreshold;

		// The camera holds still at one world-pixel size while the threshold steps, so frames compare pixel for pixel.
		const std::vector<double> thresholds = FineThresholds(capture.transitionSteps);
		nlohmann::json detailProbes = nlohmann::json::array();
		const std::vector<std::string> layers{ "visibility", "base-color-opacity", "normal-roughness" };
		for (const double threshold : thresholds) {
			detailProbes.push_back(MakeProbe(std::format("t{}", Fixed(threshold)), threshold, capture.accumulateFrames, layers, kOutputColor));
			detailProbes.push_back(MakeProbe(std::format("p{}", Fixed(threshold)), threshold, 0u, { "visibility", "debug" }, kOutputVoxelGeometryPath));
		}
		// The same threshold again: whatever differs from the first time is noise, not a LOD change.
		detailProbes.push_back(MakeProbe("repeat", thresholds.back(), capture.accumulateFrames, {}, kOutputColor));
		const RigView detailView = makeView("detail", focusX, focusY, detailPixel * capture.height);
		if (!run(root / ("transition-" + name), telephoto, { detailView }, std::move(detailProbes), force, outError)) {
			return false;
		}
		record["stages"].push_back({ { "name", name }, { "pixel_world", detailPixel }, { "focus", { focusX, focusY } }, { "thresholds", thresholds } });
		return true;
	};
	if (!addStage("first", pick.mid)) {
		return false;
	}
	// Parts can switch far apart. If the last of them switches much later, it gets a view of its own.
	if (pick.hasHigh && pick.high >= pick.mid * 4.0 && !addStage("last", pick.high)) {
		return false;
	}
	std::ofstream(recordPath) << record.dump(2) << '\n';
	return true;
}

static bool ScoreStage(const Suite& suite, const SuiteVariant& variant, const SuiteAsset& asset, const fs::path& root, const nlohmann::json& stage, MetricSink& sink, std::string& outError)
{
	std::error_code error;
	const std::string stageName = stage.at("name").get<std::string>();
	const Tags stageTag{ { "stage", stageName } };
	const auto emit = [&](const std::string& id, double value, Unit unit, Better better, Tags tags = {}) {
		tags["stage"] = stageName;
		sink.Emit(id, value, unit, better, std::move(tags));
	};
	emit("transition.stage_pixel_world", stage.at("pixel_world").get<double>(), Unit::MeshUnits, Better::Neutral);
	const fs::path view = root / ("transition-" + stageName) / "detail";
	const auto thresholds = stage.at("thresholds").get<std::vector<double>>();
	std::vector<std::pair<double, double>> fractions;
	for (const double threshold : thresholds) {
		bool ok = false;
		const double fraction = ReadVoxelFraction(view / std::format("p{}", Fixed(threshold)), nullptr, nullptr, outError, ok);
		if (!ok) {
			return false;
		}
		fractions.emplace_back(threshold, fraction);
		emit("transition.detail_voxel_fraction", fraction, Unit::Ratio, Better::Neutral, { { "threshold", Fixed(threshold) } });
	}

	const auto [beforeThreshold, afterThreshold] = PickBeforeAfter(fractions);
	DetailFrame before, after;
	if (!ReadDetail(view, beforeThreshold, before, outError) || !ReadDetail(view, afterThreshold, after, outError)) {
		return false;
	}
	emit("transition.before_threshold", beforeThreshold, Unit::None, Better::Neutral);
	emit("transition.before_voxel_fraction", before.voxelFraction, Unit::Ratio, Better::Neutral);
	emit("transition.after_threshold", afterThreshold, Unit::None, Better::Neutral);
	emit("transition.after_voxel_fraction", after.voxelFraction, Unit::Ratio, Better::Neutral);

	float exposure = 1.0f;
	{
		std::vector<float> luminance;
		for (size_t index = 0; index < before.coverage.Pixels(); ++index) {
			if (before.coverage.data[index] > 0.5f) {
				const float* p = before.hdr.data.data() + index * 4u;
				luminance.push_back(0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]);
			}
		}
		if (!luminance.empty()) {
			std::ranges::nth_element(luminance, luminance.begin() + static_cast<ptrdiff_t>(luminance.size() / 2u));
			exposure = luminance[luminance.size() / 2u] > 0.0f ? 0.5f / luminance[luminance.size() / 2u] : 1.0f;
		}
	}
	const Plane beforeImage = image::ToneMap(before.hdr, exposure), afterImage = image::ToneMap(after.hdr, exposure);
	const Plane beforeLuma = image::Luminance(beforeImage), afterLuma = image::Luminance(afterImage);
	const size_t pixels = before.hdr.Pixels();
	std::vector<uint8_t> mask(pixels, 0u);
	for (size_t index = 0; index < pixels; ++index) {
		mask[index] = before.coverage.data[index] > 0.0f || after.coverage.data[index] > 0.0f ? 1u : 0u;
	}

	double colorDelta = 0.0;
	size_t changed = 0, masked = 0;
	for (size_t index = 0; index < pixels; ++index) {
		if (mask[index] == 0u) {
			continue;
		}
		double difference = 0.0;
		for (uint32_t channel = 0; channel < 3u; ++channel) {
			difference += std::abs(static_cast<double>(beforeImage.data[index * 3u + channel]) - afterImage.data[index * 3u + channel]) / 3.0;
		}
		colorDelta += difference;
		changed += difference > 0.02 ? 1u : 0u;
		++masked;
	}
	emit("transition.pop.mean_abs_delta", masked != 0u ? colorDelta / static_cast<double>(masked) : 0.0, Unit::Ratio, Better::Lower);
	emit("transition.pop.changed_fraction", masked != 0u ? static_cast<double>(changed) / static_cast<double>(masked) : 0.0, Unit::Ratio, Better::Lower);
	emit("transition.pop.psnr_db", image::Psnr(afterImage, beforeImage, mask), Unit::None, Better::Higher);
	emit("transition.pop.ssim", image::Ssim(afterLuma, beforeLuma, mask), Unit::Ratio, Better::Higher);

	// Above 1 means voxels cover more of the view than the triangles they replace.
	const image::CoverageComparison coverage = image::CompareCoverage(before.coverage, after.coverage);
	emit("transition.pop.coverage_ratio", coverage.ratio, Unit::Ratio, Better::Neutral);

	double sumBefore = 0.0, sumAfter = 0.0;
	for (size_t index = 0; index < pixels; ++index) {
		if (mask[index] != 0u) {
			sumBefore += beforeLuma.data[index];
			sumAfter += afterLuma.data[index];
		}
	}
	emit("transition.pop.brightness_ratio", sumBefore > 0.0 ? sumAfter / sumBefore : 0.0, Unit::Ratio, Better::Neutral);
	// Below 1 means detail was lost, and well above 1 means noise or hard block edges were added.
	const double gradientBefore = MeanGradient(beforeLuma, mask);
	emit("transition.pop.detail_ratio", gradientBefore > 0.0 ? MeanGradient(afterLuma, mask) / gradientBefore : 0.0, Unit::Ratio, Better::Neutral);

	// Surface attributes where both frames drew something, in that frame's single sample.
	double albedo = 0.0, baseBefore = 0.0, baseAfter = 0.0, normalDegrees = 0.0, roughness = 0.0;
	size_t shared = 0;
	for (size_t index = 0; index < pixels; ++index) {
		if (before.visibility.keys[index] == image::kEmptyKey || after.visibility.keys[index] == image::kEmptyKey) {
			continue;
		}
		const float* a = before.baseColor.data.data() + index * 4u;
		const float* b = after.baseColor.data.data() + index * 4u;
		albedo += (std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2])) / 3.0;
		baseBefore += 0.2126 * a[0] + 0.7152 * a[1] + 0.0722 * a[2];
		baseAfter += 0.2126 * b[0] + 0.7152 * b[1] + 0.0722 * b[2];
		const float* n = before.normal.data.data() + index * 4u;
		const float* m = after.normal.data.data() + index * 4u;
		const double lengths = std::sqrt((n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) * (m[0] * m[0] + m[1] * m[1] + m[2] * m[2]));
		if (lengths > 0.0) {
			normalDegrees += std::acos(std::clamp((n[0] * m[0] + n[1] * m[1] + n[2] * m[2]) / lengths, -1.0, 1.0)) * 180.0 / kPi;
		}
		roughness += std::abs(n[3] - m[3]);
		++shared;
	}
	if (shared != 0u) {
		const double count = static_cast<double>(shared);
		emit("transition.pop.albedo_delta", albedo / count, Unit::Ratio, Better::Lower);
		emit("transition.pop.albedo_brightness_ratio", baseBefore > 0.0 ? baseAfter / baseBefore : 0.0, Unit::Ratio, Better::Neutral);
		emit("transition.pop.normal_delta_degrees", normalDegrees / count, Unit::None, Better::Lower);
		emit("transition.pop.roughness_delta", roughness / count, Unit::Ratio, Better::Lower);
	}

	// Noise a single frame shows on each side of the switch.
	const auto relativeDeviation = [&](const DetailFrame& frame) {
		double deviation = 0.0, luminance = 0.0;
		const Plane mean = image::Luminance(frame.hdr);
		for (size_t index = 0; index < pixels; ++index) {
			if (frame.coverage.data[index] > 0.0f) {
				deviation += std::sqrt(std::max(frame.variance.data[index], 0.0f));
				luminance += mean.data[index];
			}
		}
		return luminance > 0.0 ? deviation / luminance : 0.0;
	};
	emit("transition.noise_before", relativeDeviation(before), Unit::Ratio, Better::Lower);
	emit("transition.noise_after", relativeDeviation(after), Unit::Ratio, Better::Lower);

	// What a cube measures on screen just after the switch, where voxels are as large as they get.
	std::vector<double> extents = image::KeyExtents(after.visibility, after.voxels);
	if (!extents.empty()) {
		std::vector<double> cells = extents;
		for (double& value : cells) {
			value /= 4.0;
		}
		EmitDistribution(sink, "transition.cube_px", extents, Unit::Pixels, Better::Lower, stageTag);
		EmitDistribution(sink, "transition.cell_px", cells, Unit::Pixels, Better::Lower, stageTag);
	}

	// The noise floor of a step, since a converged image still varies a little between runs of the sequence.
	{
		Plane first, second;
		if (!image::ReadPlane(view / std::format("t{}", Fixed(thresholds.back())) / "hdr-mean.dds", first, outError) || !image::ReadPlane(view / "repeat" / "hdr-mean.dds", second, outError)) {
			return false;
		}
		const Plane firstImage = image::ToneMap(first, exposure), secondImage = image::ToneMap(second, exposure);
		double total = 0.0;
		for (size_t index = 0; index < pixels; ++index) {
			if (mask[index] != 0u) {
				for (uint32_t channel = 0; channel < 3u; ++channel) {
					total += std::abs(static_cast<double>(firstImage.data[index * 3u + channel]) - secondImage.data[index * 3u + channel]) / 3.0;
				}
			}
		}
		emit("transition.repeat_delta", masked != 0u ? total / static_cast<double>(masked) : 0.0, Unit::Ratio, Better::Lower);
	}

	// The largest single step, in case the switch happens all at once somewhere inside the sweep.
	double largestStep = 0.0;
	double largestFrom = 0.0, largestTo = 0.0;
	Plane previous;
	double previousThreshold = 0.0;
	for (const double threshold : thresholds) {
		Plane hdr;
		if (!image::ReadPlane(view / std::format("t{}", Fixed(threshold)) / "hdr-mean.dds", hdr, outError)) {
			return false;
		}
		Plane current = image::ToneMap(hdr, exposure);
		if (!previous.data.empty()) {
			double total = 0.0;
			for (size_t index = 0; index < pixels; ++index) {
				if (mask[index] != 0u) {
					for (uint32_t channel = 0; channel < 3u; ++channel) {
						total += std::abs(static_cast<double>(previous.data[index * 3u + channel]) - current.data[index * 3u + channel]) / 3.0;
					}
				}
			}
			const double delta = masked != 0u ? total / static_cast<double>(masked) : 0.0;
			emit("transition.step_delta", delta, Unit::Ratio, Better::Lower, { { "from", Fixed(previousThreshold) }, { "to", Fixed(threshold) } });
			if (delta > largestStep) {
				largestStep = delta;
				largestFrom = previousThreshold;
				largestTo = threshold;
			}
		}
		previous = std::move(current);
		previousThreshold = threshold;
	}
	emit("transition.largest_step_delta", largestStep, Unit::Ratio, Better::Lower, { { "from", Fixed(largestFrom) }, { "to", Fixed(largestTo) } });

	// Sheets. Row by row: lit, base colour, normal. Columns: before, after, difference.
	const fs::path reportDirectory = suite.workspace / "report" / variant.name / asset.name;
	fs::create_directories(reportDirectory, error);
	const uint32_t width = before.hdr.width, height = before.hdr.height;
	const auto writeSheet = [&](const fs::path& path, uint32_t zoom) {
		const uint32_t panelWidth = 640u, panelHeight = 360u;
		const uint32_t sheetWidth = panelWidth * 4u, rowHeight = panelHeight;
		std::vector<uint8_t> rgb;
		std::vector<uint8_t> combined(static_cast<size_t>(sheetWidth) * rowHeight * 3u * 3u, 0u);
		const auto writeRow = [&](uint32_t row, auto&& beforeSample, auto&& afterSample) {
			rgb.assign(static_cast<size_t>(sheetWidth) * rowHeight * 3u, 0u);
			AddPanel(rgb, sheetWidth, 0u, panelWidth, panelHeight, width, height, zoom, beforeSample);
			AddPanel(rgb, sheetWidth, 1u, panelWidth, panelHeight, width, height, zoom, afterSample);
			AddPanel(rgb, sheetWidth, 2u, panelWidth, panelHeight, width, height, zoom, [&](uint32_t x, uint32_t y, float* color) {
				float a[3]{}, b[3]{};
				beforeSample(x, y, a);
				afterSample(x, y, b);
				const float difference = (std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2])) * 1.5f;
				color[0] = color[1] = color[2] = difference;
			});
			// Which path drew each pixel just after: red voxels, green triangles, blue where only the frame before had a surface.
			AddPanel(rgb, sheetWidth, 3u, panelWidth, panelHeight, width, height, zoom, [&](uint32_t x, uint32_t y, float* color) {
				const size_t index = static_cast<size_t>(y) * width + x;
				const bool afterCovered = after.visibility.keys[index] != image::kEmptyKey;
				const bool beforeCovered = before.visibility.keys[index] != image::kEmptyKey;
				color[0] = afterCovered && after.voxels[index] != 0u ? (beforeCovered ? 0.95f : 0.55f) : 0.0f;
				color[1] = afterCovered && after.voxels[index] == 0u ? 0.8f : 0.0f;
				color[2] = !afterCovered && beforeCovered ? 0.9f : 0.0f;
			});
			std::copy(rgb.begin(), rgb.end(), combined.begin() + static_cast<ptrdiff_t>(static_cast<size_t>(row) * sheetWidth * rowHeight * 3u));
		};
		writeRow(0u,
			[&](uint32_t x, uint32_t y, float* c) { for (uint32_t i = 0; i < 3u; ++i) c[i] = beforeImage.At(x, y, i); },
			[&](uint32_t x, uint32_t y, float* c) { for (uint32_t i = 0; i < 3u; ++i) c[i] = afterImage.At(x, y, i); });
		writeRow(1u,
			[&](uint32_t x, uint32_t y, float* c) { for (uint32_t i = 0; i < 3u; ++i) c[i] = before.baseColor.At(x, y, i); },
			[&](uint32_t x, uint32_t y, float* c) { for (uint32_t i = 0; i < 3u; ++i) c[i] = after.baseColor.At(x, y, i); });
		const auto normalColor = [](const Plane& normal, const image::KeyImage& keys, uint32_t x, uint32_t y, float* c) {
			if (keys.keys[static_cast<size_t>(y) * keys.width + x] == image::kEmptyKey) {
				return;
			}
			for (uint32_t i = 0; i < 3u; ++i) {
				c[i] = normal.At(x, y, i) * 0.5f + 0.5f;
			}
		};
		writeRow(2u,
			[&](uint32_t x, uint32_t y, float* c) { normalColor(before.normal, before.visibility, x, y, c); },
			[&](uint32_t x, uint32_t y, float* c) { normalColor(after.normal, after.visibility, x, y, c); });
		std::string writeError;
		return image::WritePng(path, sheetWidth, rowHeight * 3u, combined, writeError);
	};
	if (!writeSheet(reportDirectory / std::format("transition_{}.png", stageName), 1u) || !writeSheet(reportDirectory / std::format("transition_{}_zoom4.png", stageName), 4u)) {
		outError = "failed to write the transition sheets";
		return false;
	}
	return true;
}

bool ScoreTransition(const Suite& suite, const SuiteVariant& variant, const SuiteAsset& asset, MetricSink& sink, std::string& outError)
{
	const fs::path root = suite.workspace / "captures" / variant.name / asset.name;
	std::error_code error;
	if (!fs::exists(root / "transition.json", error)) {
		return true;
	}
	const nlohmann::json record = nlohmann::json::parse(ReadText(root / "transition.json"));
	if (!record.value("has_transition", false)) {
		sink.Emit("transition.reached", 0.0, Unit::Count, Better::Neutral);
		return true;
	}
	sink.Emit("transition.reached", 1.0, Unit::Count, Better::Neutral);
	const double wholePixel = record.at("whole_pixel_world").get<double>();
	for (const char* name : { "low", "mid", "high" }) {
		const double world = record.at(name).get<double>() * wholePixel;
		const Tags tags{ { "point", name } };
		sink.Emit("transition.world_pixel", world, Unit::MeshUnits, Better::Neutral, tags);
		sink.Emit("transition.distance_60deg_1080p", world * 1080.0 / (2.0 * std::tan(30.0 * kPi / 180.0)), Unit::MeshUnits, Better::Neutral, tags);
	}
	for (const auto& point : record.at("curve")) {
		sink.Emit("transition.curve_voxel_fraction", point.at(1).get<double>(), Unit::Ratio, Better::Neutral, { { "world_pixel", std::format("{:.5f}", point.at(0).get<double>() * wholePixel) } });
	}

	for (const nlohmann::json& stage : record.at("stages")) {
		if (!ScoreStage(suite, variant, asset, root, stage, sink, outError)) {
			return false;
		}
	}
	return true;
}

}
