#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "Capture.h"
#include "Core/Metrics.h"
#include "Suite.h"

// Looks at the moment a hierarchy hands over from triangles to voxels: where it happens, and what changes on
// screen when it does. A narrow field of view from far away gives one world-pixel size across the whole asset, so
// the cut is the same everywhere, as an orthographic view would make it. Stepping the threshold rather than
// moving the camera keeps every frame pixel-aligned, so the difference between two is purely the LOD change.
namespace clodeval {

// Distance at which a view of this field of view is worldHeight tall.
double TelephotoDistance(double worldHeight, double fovDegrees);

struct TransitionPick {
	double low = 0.0, mid = 0.0, high = 0.0;  // thresholds where 5%, 50% and 95% of pixels are voxels
	bool hasLow = false, hasMid = false, hasHigh = false;
};

// curve is (threshold, voxel fraction), in increasing threshold order.
TransitionPick PickTransition(const std::vector<std::pair<double, double>>& curve);

// Thresholds close around a true one-pixel cut, always including 1.
std::vector<double> FineThresholds(uint32_t steps);

// Of (threshold, voxel fraction) in increasing threshold order, the frames just before and just after the switch.
std::pair<double, double> PickBeforeAfter(const std::vector<std::pair<double, double>>& fractions);

using CaptureRunner = std::function<bool(const std::filesystem::path& directory, const SuiteCapture& capture, const std::vector<RigView>& views, nlohmann::json probes, bool force, std::string& outError)>;

bool CaptureTransition(const Suite& suite, const SuiteVariant& variant, const SuiteAsset& asset, const AssetFrame& frame, const CaptureRunner& run, bool force, std::string& outError);

// Emits transition.* and writes transition_<stage>.png and transition_<stage>_zoom4.png. Does nothing if nothing was captured.
bool ScoreTransition(const Suite& suite, const SuiteVariant& variant, const SuiteAsset& asset, MetricSink& sink, std::string& outError);

}
