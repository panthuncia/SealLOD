#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "CaptureShared.h"
#include "ViewerHierarchy.h"
#include "ViewerInspect.h"
#include "ViewerTexture.h"

// CLodCapture --view: a window over one suite variant, for looking at where a hierarchy changes from triangles to
// voxels and why. It renders with the capture pipeline, so what it shows is what the captures measure. See
// docs/clod-evaluation.md.
namespace clodcapture {

// A script exercises the same paths a user does, so the viewer can be tested without anyone at the keyboard.
int RunViewer(const std::filesystem::path& configPath, const std::filesystem::path& scriptPath);

struct ViewerPreset {
	std::string name;
	XMFLOAT3 focus{};
	float yaw = 0.0f, pitch = 0.0f, distance = 1.0f, fovDegrees = 60.0f;
	std::optional<float> worldCut;
};

// A hand-over of the first kind found by a sweep or read from a capture.
struct CurvePoint {
	float worldCut = 0.0f;
	float voxelFraction = 0.0f;
};

// One image of the view at one cut, for comparing against another.
struct Snapshot {
	bool valid = false;
	std::string label;
	float worldCut = 0.0f, threshold = 0.0f;
	uint32_t width = 0, height = 0;
	uint32_t frames = 0;
	std::vector<float> hdr;       // rgb
	std::vector<float> coverage;  // fraction of frames that hit
	std::vector<uint32_t> base;   // rgba8
	std::vector<float> normal;    // rgba
	std::shared_ptr<const InspectFrame> inspect;
};

struct CompareStats {
	bool valid = false;
	double meanDelta = 0.0, changedPixels = 0.0, coverageRatio = 0.0, brightnessRatio = 0.0, normalDegrees = 0.0;
	double voxelFractionA = 0.0, voxelFractionB = 0.0;
};

enum class CompareLayer : int { Lit, BaseColor, Normal, Coverage, Path, Count };
enum class CompareLayout : int { Grid, Flip, Wipe, Count };

class Viewer {
public:
	Viewer(Renderer& renderer, CaptureScene& scene, nlohmann::json config, uint32_t width, uint32_t height);

	// Before the renderer's Update: applies what the panels asked for and advances anything running.
	void BeginFrame();
	// Inside ImGui's frame.
	void DrawPanels();

	void OnResize(uint32_t width, uint32_t height);
	void SetWindow(HWND window) { m_window = window; }
	void RunScript(nlohmann::json steps);
	bool WantsExit() const { return m_exit; }
	std::string LastError() const { return m_scriptError; }

private:
	// Camera
	XMFLOAT3 CameraPosition() const;
	float PixelWorldAtFocus() const;
	float PixelWorldPerDepth() const;
	void ApplyCamera();
	void ApplyPreset(const ViewerPreset& preset);
	void SetFov(float degrees, bool keepPixelSize);
	void HandleSceneInput();

	// Cut
	float EffectiveThreshold() const { return m_forceLeaf ? -1.0f : m_threshold; }
	float WorldCut() const { return m_threshold * PixelWorldAtFocus(); }
	void SetThreshold(float threshold);
	void SetWorldCut(float worldCut);
	void ApplyCut();
	void SetOutput(uint32_t output);

	// Inspection
	void RequestInspection(bool force);
	void AdvanceInspection();
	std::shared_ptr<const InspectFrame> CurrentFrame() const;
	void RepaintOverlay();
	void SelectPixel(uint32_t x, uint32_t y);
	void SelectGroup(int32_t mesh, int32_t group);

	// Running things
	void StartSweep();
	void AdvanceSweep();
	void StartSnapshot(int slot);
	void AdvanceSnapshot();
	bool Settled(double seconds);
	void CaptureScreenshot(const std::filesystem::path& path);
	void AdvanceScript();

	// Compare
	void BuildCompareTiles();
	void ComputeCompareStats();
	void BuildTile(int which, uint32_t width, uint32_t height, std::vector<uint32_t>& out) const;
	float CompareExposure() const;

	// Panels (ViewerPanels.cpp)
	void DrawControlPanel();
	void DrawHierarchyPanel();
	void DrawGroupPanel();
	void DrawTransitionPanel();
	void DrawComparePanel();
	void DrawHoverTooltip();
	void DrawOverlayImage();
	void DrawMeshTable();
	void DrawLevelTable(const MeshInfo& mesh);
	void DrawHandOverPlot();
	void DrawGroupChain(const MeshInfo& mesh);
	std::string FormatWorld(float value) const;

	Renderer& m_renderer;
	CaptureScene& m_scene;
	nlohmann::json m_config;
	HWND m_window = nullptr;
	uint32_t m_width, m_height;
	std::string m_title;
	float m_worldScale = 1.0f;

	// Camera state
	XMFLOAT3 m_focus{};
	float m_yaw = 0.0f, m_pitch = 0.0f, m_distance = 10.0f, m_fovDegrees = 60.0f;
	float m_zNear = 0.01f, m_zFar = 100000.0f;
	float m_frameRadius = 1.0f;
	bool m_projectionDirty = true, m_cameraDirty = true;
	uint64_t m_cameraGeneration = 1, m_cutGeneration = 1;
	std::vector<ViewerPreset> m_presets;
	int m_lastPreset = -1;
	bool m_dragging = false;
	float m_dragDistance = 0.0f;

	// Cut state
	float m_threshold = 1.0f;
	bool m_lockWorldCut = false;
	float m_lockedWorldCut = 0.0f;
	bool m_forceLeaf = false;
	bool m_voxelOnly = false;
	bool m_forceDepth = false;
	int m_forcedDepth = 0;
	bool m_cutDirty = true;
	float m_appliedThreshold = 0.0f;
	float m_flipA = 0.5f, m_flipB = 2.0f;
	bool m_autoFlip = false;
	float m_flipHertz = 1.0f;
	bool m_flipOnB = false;
	std::chrono::steady_clock::time_point m_lastFlip{};

	// View state
	uint32_t m_output = 0;
	bool m_showAllOutputs = false;
	OverlayMode m_overlay = OverlayMode::Off;
	float m_overlayOpacity = 1.0f;
	bool m_hidePanels = false;
	float m_uiScale = 1.0f;
	bool m_overlayDirty = true;
	bool m_rendererMenu = false;
	std::vector<uint32_t> m_overlayPixels;
	ViewerTexture m_overlayTexture;
	uint64_t m_overlayFrame = 0;
	std::string m_screenshotDirectory;

	// Inspection state
	HierarchyIndex m_hierarchy;
	mutable std::mutex m_inspectMutex;
	std::shared_ptr<const InspectFrame> m_inspect;
	std::atomic<bool> m_inspectInFlight{ false };
	std::atomic<uint32_t> m_inspectArrivals{ 0u };
	std::chrono::steady_clock::time_point m_lastInspectRequest{};
	float m_inspectIntervalSeconds = 0.25f;
	bool m_liveInspection = true;

	// Selection state
	Selection m_selection;
	int32_t m_tableMesh = -1;
	std::optional<CubeInfo> m_selectedCube;
	float m_selectedDepth = 0.0f;
	uint32_t m_selectedX = 0, m_selectedY = 0;
	bool m_selectedVoxel = false;
	uint32_t m_selectedCluster = 0;
	uint32_t m_selectedPrimitive = 0;

	// Sweep state
	std::vector<CurvePoint> m_savedCurve;
	std::vector<CurvePoint> m_liveCurve;
	std::vector<float> m_sweepCuts;
	size_t m_sweepIndex = 0;
	bool m_sweeping = false;
	uint32_t m_sweepArrivalsAtRequest = 0;
	bool m_sweepRequested = false;
	float m_sweepRestore = 1.0f;
	uint32_t m_sweepSteps = 24u;
	float m_sweepBefore = 0.0f, m_sweepAfter = 0.0f;
	float m_sweepLow = 0.0005f, m_sweepHigh = 0.05f;
	uint64_t m_sweepResident = 0;
	std::chrono::steady_clock::time_point m_stableSince{};
	bool m_wasStable = false;

	// Snapshot state
	struct SnapshotRecording;
	std::shared_ptr<SnapshotRecording> m_recording;
	int m_recordingSlot = -1;
	int m_pendingAfter = -1;  // a second snapshot to take when this one finishes
	float m_pendingCut = 0.0f;
	float m_recordRestore = 0.0f;
	uint32_t m_recordOutputRestore = 0;
	int m_snapshotFrames = 16;
	bool m_recordingStarted = false;
	uint32_t m_recordingFramesLeft = 0;
	std::array<Snapshot, 2> m_snapshots;

	// Compare state
	CompareLayer m_compareLayer = CompareLayer::Lit;
	CompareLayout m_compareLayout = CompareLayout::Grid;
	CompareStats m_compareStats;
	std::array<ViewerTexture, 4> m_tiles;
	std::array<std::vector<uint32_t>, 4> m_tilePixels;
	bool m_comparePanel = false;
	bool m_compareDirty = true;
	float m_compareZoom = 1.0f;
	float m_compareCenter[2] = { 0.5f, 0.5f };
	float m_compareWipe = 0.5f;
	float m_diffGain = 4.0f;
	bool m_compareFlipB = false;
	float m_compareExposure = 1.0f;
	std::chrono::steady_clock::time_point m_lastCompareFlip{};
	bool m_compareAutoFlip = false;

	// Script state
	std::deque<nlohmann::json> m_script;
	uint32_t m_scriptWait = 0;
	std::string m_scriptWaitFor;
	uint32_t m_scriptWaitArrivals = 0;
	std::string m_scriptError;
	bool m_exit = false;
	std::string m_status;
};

}
