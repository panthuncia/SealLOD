#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "Core/Image.h"
#include "Core/Results.h"
#include "Suite.h"

// Renders a suite's built caches with CLodCapture and scores the images. CLodCapture knows nothing about
// cameras or metrics: the rig and the probes are generated here, so changing them never touches it.
namespace clodeval {

struct RigView {
	std::string name;
	double position[3]{};
	double lookAt[3]{};
};

struct AssetFrame {
	double center[3]{};
	double radius = 0.0;
};

// The views a suite's capture block asks for. Distances are in bounding radii.
std::vector<RigView> MakeRig(const SuiteCapture& capture, const AssetFrame& frame);
std::vector<RigView> MakeSweepRig(const SuiteCapture& capture, const AssetFrame& frame);

struct CaptureOptions {
	std::filesystem::path captureTool;
	bool force = false;
	bool transitionOnly = false;  // skip everything but the triangle-to-voxel views
};

// Which pixels voxels drew, from the debug and visibility layers of a voxel-path probe.
std::vector<uint8_t> VoxelPathMask(const image::KeyImage& debug, const image::KeyImage& visibility);

// Runs every job a variant still needs. Skips any job whose inputs have not changed.
bool CaptureVariant(const Suite& suite, const SuiteVariant& variant, const CaptureOptions& options, std::string& outError);

struct ViewerOptions {
	std::filesystem::path captureTool;
	std::filesystem::path script;  // optional steps for an unattended run
	uint32_t width = 1920u;
	uint32_t height = 1080u;
	std::string partFilter;  // only parts whose name contains this
};

// Opens an interactive window on one built asset, with the same environment its captures get. Waits until it closes.
bool RunViewer(const Suite& suite, const SuiteVariant& variant, const SuiteAsset& asset, const ViewerOptions& options, std::string& outError);

// Opens the viewer on a merge-hierarchy root (docs/merge_hierarchies.md in SARP) and the parts its portal parts file
// names, from the cache tree they are in. Writes viewer.json and the log under `directory`.
bool RunRootViewer(const std::filesystem::path& rootMetadata, const std::filesystem::path& directory, const ViewerOptions& options, std::string& outError);

// Scores a variant's bundles and writes per-view contact sheets under report/<variant>/<asset>/.
bool ScoreVariant(const Suite& suite, const SuiteVariant& variant, bool transitionOnly, ResultSet& outResults, std::string& outError);

std::filesystem::path GetScoreResultsPath(const Suite& suite, const std::string& variantName);

// Writes report/index.html from whatever results the suite's variants have, and returns its path.
bool WriteReport(const Suite& suite, std::filesystem::path& outPath, std::string& outError);

}
