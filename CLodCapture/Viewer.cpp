#include "Viewer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <thread>

#include <dxgiformat.h>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include <BasicRenderer/Assets/Import/CLodCache.h>
#include <BasicRenderer/Diagnostics/OutputTypes.h>

#include "Core/Image.h"
#include <BasicRenderer/Diagnostics/ExternalUi.h>
#include "Render/RenderGraph/RenderGraph.h"
#include "Render/Runtime/IReadbackService.h"
#include "Resources/ReadbackRequest.h"
#include "Runtime/Settings/SettingsManager.h"

namespace clodcapture {
	namespace {
		constexpr float kPi = 3.14159265f;
		constexpr float kBeforeFactor = 0.97f, kAfterFactor = 1.03f;

		XMFLOAT3 ReadVector(const nlohmann::json& json)
		{
			return { json.at(0).get<float>(), json.at(1).get<float>(), json.at(2).get<float>() };
		}

		float ToneMapChannel(float value, float exposure)
		{
			const float exposed = std::max(value, 0.0f) * exposure;
			return std::pow(exposed / (1.0f + exposed), 1.0f / 2.2f);
		}

		uint32_t PackUnit(float r, float g, float b)
		{
			const auto channel = [](float value) { return static_cast<uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f); };
			return channel(r) | (channel(g) << 8u) | (channel(b) << 16u) | 0xFF000000u;
		}
	}

	// A readback lands on another thread, so everything it writes goes through here.
	struct Viewer::SnapshotRecording {
		std::mutex mutex;
		Snapshot snapshot;
		std::atomic<uint32_t> outstanding{ 0u };
		std::vector<double> sum;
		std::vector<uint32_t> hits;
		uint32_t hdrFrames = 0, coverageFrames = 0;
		std::string error;
	};

	Viewer::Viewer(Renderer& renderer, CaptureScene& scene, nlohmann::json config, uint32_t width, uint32_t height)
		: m_renderer(renderer), m_scene(scene), m_config(std::move(config)), m_width(width), m_height(height)
	{
		m_title = m_config.value("title", std::string("CLod viewer"));
		m_screenshotDirectory = m_config.value("screenshot_directory", std::string("."));
		if (const auto assets = m_config.find("assets"); assets != m_config.end() && !assets->empty()) {
			m_worldScale = assets->front().value("scale", 1.0f);
		}
		const nlohmann::json& frame = m_config.at("frame");
		const XMFLOAT3 center = ReadVector(frame.at("center"));
		m_frameRadius = frame.at("radius").get<float>();
		m_focus = center;
		m_distance = m_frameRadius * 2.5f;
		m_pitch = 0.1f;

		m_presets.push_back({ "Perspective, whole asset", center, 0.0f, 0.1f, m_frameRadius * 2.5f, 60.0f, std::nullopt });
		for (const nlohmann::json& preset : m_config.value("presets", nlohmann::json::array())) {
			ViewerPreset parsed;
			parsed.name = preset.at("name").get<std::string>();
			parsed.focus = ReadVector(preset.at("focus"));
			parsed.yaw = preset.value("yaw", 0.0f);
			parsed.pitch = preset.value("pitch", 0.0f);
			parsed.distance = preset.at("distance").get<float>();
			parsed.fovDegrees = preset.at("fov_degrees").get<float>();
			if (preset.contains("world_cut")) {
				parsed.worldCut = preset.at("world_cut").get<float>();
			}
			m_presets.push_back(std::move(parsed));
		}
		for (const nlohmann::json& point : m_config.value("curve", nlohmann::json::array())) {
			m_savedCurve.push_back({ point.at(0).get<float>(), point.at(1).get<float>() });
		}
		m_hierarchy.StartLoading(m_config.at("cache_root").get<std::string>());
		m_lastInspectRequest = std::chrono::steady_clock::now();
		m_lastFlip = m_lastInspectRequest;
		m_lastCompareFlip = m_lastInspectRequest;
		m_stableSince = m_lastInspectRequest;
		br::diagnostics::SetRendererMenuEnabled(false);
		SetOutput(OutputType::COLOR);
	}

	// ---- Camera

	XMFLOAT3 Viewer::CameraPosition() const
	{
		return {
			m_focus.x + m_distance * std::sin(m_yaw) * std::cos(m_pitch),
			m_focus.y + m_distance * std::sin(m_pitch),
			m_focus.z + m_distance * std::cos(m_yaw) * std::cos(m_pitch),
		};
	}

	float Viewer::PixelWorldPerDepth() const
	{
		return 2.0f * std::tan(m_fovDegrees * kPi / 360.0f) / static_cast<float>(m_height);
	}

	float Viewer::PixelWorldAtFocus() const
	{
		return m_distance * PixelWorldPerDepth();
	}

	void Viewer::ApplyCamera()
	{
		if (m_projectionDirty) {
			const float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);
			m_renderer.GetCurrentScene()->SetCameraProjection(m_fovDegrees * kPi / 180.0f, aspect, m_zNear, m_zFar);
			m_projectionDirty = false;
			m_cameraDirty = true;
		}
		if (!m_cameraDirty) {
			return;
		}
		JobView view;
		view.position = CameraPosition();
		view.lookAt = m_focus;
		view.fovDegrees = m_fovDegrees;
		MoveCamera(m_renderer, view);
		m_cameraDirty = false;
		++m_cameraGeneration;
		if (m_lockWorldCut && !m_forceLeaf) {
			m_threshold = m_lockedWorldCut / PixelWorldAtFocus();
			m_cutDirty = true;
		}
	}

	void Viewer::ApplyPreset(const ViewerPreset& preset)
	{
		m_focus = preset.focus;
		m_yaw = preset.yaw;
		m_pitch = preset.pitch;
		m_distance = preset.distance;
		if (m_fovDegrees != preset.fovDegrees) {
			m_fovDegrees = preset.fovDegrees;
			m_projectionDirty = true;
		}
		m_cameraDirty = true;
		if (preset.worldCut) {
			SetWorldCut(*preset.worldCut);
		}
	}

	void Viewer::SetFov(float degrees, bool keepPixelSize)
	{
		if (degrees == m_fovDegrees) {
			return;
		}
		if (keepPixelSize) {
			m_distance *= std::tan(m_fovDegrees * kPi / 360.0f) / std::tan(degrees * kPi / 360.0f);
		}
		m_fovDegrees = degrees;
		m_projectionDirty = true;
		m_cameraDirty = true;
	}

	void Viewer::HandleSceneInput()
	{
		const ImGuiIO& io = ImGui::GetIO();
		if (!io.WantCaptureMouse) {
			if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
				m_yaw -= io.MouseDelta.x * 0.005f;
				m_pitch = std::clamp(m_pitch + io.MouseDelta.y * 0.005f, -1.55f, 1.55f);
				m_cameraDirty = true;
			}
			if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
				const float pixel = PixelWorldAtFocus();
				const XMFLOAT3 cameraPosition = CameraPosition();
				const XMVECTOR forward = XMVector3Normalize(XMVectorSubtract(XMLoadFloat3(&m_focus), XMLoadFloat3(&cameraPosition)));
				const XMVECTOR right = XMVector3Normalize(XMVector3Cross(forward, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)));
				const XMVECTOR up = XMVector3Cross(right, forward);
				XMVECTOR focus = XMLoadFloat3(&m_focus);
				focus = XMVectorAdd(focus, XMVectorScale(right, -io.MouseDelta.x * pixel));
				focus = XMVectorAdd(focus, XMVectorScale(up, io.MouseDelta.y * pixel));
				XMStoreFloat3(&m_focus, focus);
				m_cameraDirty = true;
			}
			if (io.MouseWheel != 0.0f) {
				m_distance = std::max(m_distance * std::pow(0.9f, io.MouseWheel), 1.0e-3f);
				m_cameraDirty = true;
			}
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
				m_dragDistance = 0.0f;
				m_dragging = true;
			}
			if (m_dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
				m_dragging = false;
				if (ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).x * ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).x +
					ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).y * ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).y < 16.0f) {
					SelectPixel(static_cast<uint32_t>(std::max(io.MousePos.x, 0.0f)), static_cast<uint32_t>(std::max(io.MousePos.y, 0.0f)));
				}
			}
		}
		if (io.WantCaptureKeyboard) {
			return;
		}
		const float step = PixelWorldAtFocus() * 400.0f * io.DeltaTime;
		const XMFLOAT3 position = CameraPosition();
		const XMVECTOR forward = XMVector3Normalize(XMVectorSubtract(XMLoadFloat3(&m_focus), XMLoadFloat3(&position)));
		const XMVECTOR right = XMVector3Normalize(XMVector3Cross(forward, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)));
		const XMVECTOR up = XMVector3Cross(right, forward);
		XMVECTOR move = XMVectorZero();
		if (ImGui::IsKeyDown(ImGuiKey_A)) { move = XMVectorAdd(move, XMVectorScale(right, -step)); }
		if (ImGui::IsKeyDown(ImGuiKey_D)) { move = XMVectorAdd(move, XMVectorScale(right, step)); }
		if (ImGui::IsKeyDown(ImGuiKey_W)) { move = XMVectorAdd(move, XMVectorScale(up, step)); }
		if (ImGui::IsKeyDown(ImGuiKey_S)) { move = XMVectorAdd(move, XMVectorScale(up, -step)); }
		if (ImGui::IsKeyDown(ImGuiKey_Q)) { m_distance *= 1.0f + 2.0f * io.DeltaTime; m_cameraDirty = true; }
		if (ImGui::IsKeyDown(ImGuiKey_E)) { m_distance /= 1.0f + 2.0f * io.DeltaTime; m_cameraDirty = true; }
		if (XMVectorGetX(XMVector3LengthSq(move)) > 0.0f) {
			XMStoreFloat3(&m_focus, XMVectorAdd(XMLoadFloat3(&m_focus), move));
			m_cameraDirty = true;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) { SetThreshold(m_threshold / std::sqrt(2.0f)); }
		if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) { SetThreshold(m_threshold * std::sqrt(2.0f)); }
		if (ImGui::IsKeyPressed(ImGuiKey_F)) {
			m_flipOnB = !m_flipOnB;
			SetWorldCut(m_flipOnB ? m_flipB : m_flipA);
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
			m_hidePanels = !m_hidePanels;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_O)) {
			m_overlay = m_overlay == OverlayMode::Off ? OverlayMode::Path : OverlayMode::Off;
			m_overlayDirty = true;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_F12)) {
			static uint32_t count = 0u;
			CaptureScreenshot(std::filesystem::path(m_screenshotDirectory) / std::format("viewer_{:03}.png", ++count));
		}
	}

	// ---- Cut

	void Viewer::SetThreshold(float threshold)
	{
		m_threshold = std::clamp(threshold, 1.0e-4f, 1.0e5f);
		m_lockedWorldCut = m_threshold * PixelWorldAtFocus();
		m_forceLeaf = false;
		m_cutDirty = true;
	}

	void Viewer::SetWorldCut(float worldCut)
	{
		SetThreshold(worldCut / PixelWorldAtFocus());
	}

	void Viewer::ApplyCut()
	{
		if (!m_cutDirty) {
			return;
		}
		m_cutDirty = false;
		const float effective = EffectiveThreshold();
		if (effective != m_appliedThreshold) {
			br::extensions::MakeSettingSetter<float>(CLodLodErrorPixelsSettingName)(effective);
			m_appliedThreshold = effective;
			++m_cutGeneration;
			m_wasStable = false;
		}
	}

	void Viewer::SetOutput(uint32_t output)
	{
		m_output = output;
		br::extensions::MakeSettingSetter<unsigned int>("outputType")(output);
	}

	// ---- Inspection

	void Viewer::RequestInspection(bool force)
	{
		if (m_inspectInFlight.load()) {
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		if (!force && (!m_liveInspection || std::chrono::duration<float>(now - m_lastInspectRequest).count() < m_inspectIntervalSeconds)) {
			return;
		}
		ViewStamp stamp;
		stamp.cameraGeneration = m_cameraGeneration;
		stamp.cutGeneration = m_cutGeneration;
		stamp.pixelWorldPerDepth = PixelWorldPerDepth();
		stamp.threshold = EffectiveThreshold();
		stamp.worldScale = m_worldScale;
		m_inspectInFlight.store(true);
		m_lastInspectRequest = now;
		const bool requested = m_renderer.RequestCLodInspectionFrame([this, stamp](Renderer::CLodInspectionFrame&& input) {
			if (input.width != 0u) {
				auto decoded = DecodeInspection(std::move(input), m_hierarchy, stamp);
				std::scoped_lock lock(m_inspectMutex);
				m_inspect = std::move(decoded);
			}
			m_inspectArrivals.fetch_add(1u);
			m_inspectInFlight.store(false);
		});
		if (!requested) {
			m_inspectInFlight.store(false);
		}
	}

	std::shared_ptr<const InspectFrame> Viewer::CurrentFrame() const
	{
		std::scoped_lock lock(m_inspectMutex);
		return m_inspect;
	}

	void Viewer::AdvanceInspection()
	{
		// Nothing draws from a frame taken from another camera, so there is no point reading one while moving.
		if (!ImGui::IsMouseDragging(ImGuiMouseButton_Right) && !ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
			RequestInspection(false);
		}
		const auto frame = CurrentFrame();
		if (frame && frame->frame != m_overlayFrame) {
			m_overlayFrame = frame->frame;
			m_overlayDirty = true;
		}
		if (m_overlayDirty) {
			RepaintOverlay();
		}
	}

	void Viewer::RepaintOverlay()
	{
		m_overlayDirty = false;
		if (m_overlay == OverlayMode::Off) {
			return;
		}
		const auto frame = CurrentFrame();
		if (!frame || frame->width == 0u || !m_hierarchy.Ready()) {
			return;
		}
		PaintOverlay(*frame, m_hierarchy, m_overlay, m_selection, m_overlayOpacity, m_overlayPixels);
		m_overlayTexture.Upload(frame->width, frame->height, m_overlayPixels);
	}

	void Viewer::SelectGroup(int32_t mesh, int32_t group)
	{
		m_selection = { mesh, group };
		m_tableMesh = mesh;
		m_selectedCube.reset();
		if (m_overlay == OverlayMode::Selection) {
			m_overlayDirty = true;
		}
	}

	void Viewer::SelectPixel(uint32_t x, uint32_t y)
	{
		const auto frame = CurrentFrame();
		if (!frame || frame->stamp.cameraGeneration != m_cameraGeneration) {
			m_status = "Pick ignored: no frame of this camera yet.";
			return;
		}
		const PixelInfo* pixel = frame->At(x, y);
		if (pixel == nullptr || (pixel->flags & kPixelHit) == 0u) {
			m_status = std::format("Nothing drew pixel {}, {}.", x, y);
			return;
		}
		m_selectedX = x;
		m_selectedY = y;
		m_selectedDepth = pixel->depth;
		m_selectedVoxel = (pixel->flags & kPixelVoxel) != 0u;
		m_selectedCluster = pixel->cluster;
		m_selectedPrimitive = pixel->primitive;
		SelectGroup(pixel->mesh, static_cast<int32_t>(pixel->group));
		if (m_selectedVoxel && pixel->mesh >= 0 && static_cast<size_t>(pixel->cluster) * 16u + 16u <= frame->clusters.size()) {
			std::array<uint32_t, 4> words{};
			std::memcpy(words.data(), frame->clusters.data() + static_cast<size_t>(pixel->cluster) * 16u, sizeof(words));
			const VisibleClusterFields fields = DecodeVisibleCluster(words);
			m_selectedCube = m_hierarchy.FindCube(pixel->mesh, pixel->group, fields.voxelPage, fields.localMeshlet, pixel->primitive);
		}
		m_status = std::format("Selected pixel {}, {}.", x, y);
	}

	// ---- Running things

	bool Viewer::Settled(double seconds)
	{
		const auto snapshot = m_renderer.GetSamplingReadinessSnapshot(false);
		const bool stable = IsIdle(snapshot) && snapshot.residentClodAllocationBytes == m_sweepResident;
		m_sweepResident = snapshot.residentClodAllocationBytes;
		const auto now = std::chrono::steady_clock::now();
		if (stable && !m_wasStable) {
			m_stableSince = now;
		}
		m_wasStable = stable;
		return stable && std::chrono::duration<double>(now - m_stableSince).count() >= seconds;
	}

	void Viewer::StartSweep()
	{
		if (m_sweeping || m_recording) {
			return;
		}
		m_sweepCuts.clear();
		m_liveCurve.clear();
		const uint32_t steps = std::max(m_sweepSteps, 2u);
		const float ratio = std::pow(m_sweepHigh / m_sweepLow, 1.0f / static_cast<float>(steps - 1u));
		float cut = m_sweepLow;
		for (uint32_t step = 0; step < steps; ++step) {
			m_sweepCuts.push_back(cut);
			cut *= ratio;
		}
		m_sweepRestore = m_threshold;
		m_sweepIndex = 0;
		m_sweeping = true;
		m_sweepRequested = false;
		m_sweepBefore = m_sweepAfter = 0.0f;
		SetWorldCut(m_sweepCuts.front());
	}

	void Viewer::AdvanceSweep()
	{
		if (!m_sweeping) {
			return;
		}
		m_status = std::format("Sweeping {} of {}", m_sweepIndex + 1u, m_sweepCuts.size());
		if (!m_sweepRequested) {
			if (m_cutDirty || !Settled(0.5) || m_inspectInFlight.load()) {
				return;
			}
			m_sweepArrivalsAtRequest = m_inspectArrivals.load();
			RequestInspection(true);
			m_sweepRequested = m_inspectInFlight.load();
			return;
		}
		if (m_inspectArrivals.load() == m_sweepArrivalsAtRequest) {
			return;
		}
		m_sweepRequested = false;
		if (const auto frame = CurrentFrame()) {
			m_liveCurve.push_back({ m_sweepCuts[m_sweepIndex], frame->VoxelFraction() });
		}
		if (++m_sweepIndex < m_sweepCuts.size()) {
			SetWorldCut(m_sweepCuts[m_sweepIndex]);
			return;
		}
		m_sweeping = false;
		SetThreshold(m_sweepRestore);
		// The frames either side of the first switch: the last under 2% voxels, and the first point past it where the
		// curve levels off. A share of the maximum would not do, because coarse parts can lift the maximum far later.
		float highest = 0.0f;
		for (const CurvePoint& point : m_liveCurve) {
			highest = std::max(highest, point.voxelFraction);
		}
		size_t beforeIndex = m_liveCurve.size();
		for (size_t index = 0; index < m_liveCurve.size() && m_liveCurve[index].voxelFraction < 0.02f; ++index) {
			beforeIndex = index;
		}
		if (highest > 0.0f && beforeIndex + 1u < m_liveCurve.size()) {
			size_t afterIndex = beforeIndex + 1u;
			while (afterIndex + 1u < m_liveCurve.size() && m_liveCurve[afterIndex + 1u].voxelFraction - m_liveCurve[afterIndex].voxelFraction >= 0.02f) {
				++afterIndex;
			}
			m_sweepBefore = m_liveCurve[beforeIndex].worldCut;
			m_sweepAfter = m_liveCurve[afterIndex].worldCut;
			m_flipA = m_sweepBefore;
			m_flipB = m_sweepAfter;
		}
		m_status = std::format("Sweep finished. Highest voxel fraction {:.1f}%.", highest * 100.0f);
	}

	void Viewer::StartSnapshot(int slot)
	{
		if (m_sweeping || m_recording) {
			return;
		}
		m_recordOutputRestore = m_output;
		m_recordRestore = m_threshold;
		SetOutput(OutputType::COLOR);
		m_recording = std::make_shared<SnapshotRecording>();
		m_recording->snapshot.label = slot == 0 ? "A" : "B";
		m_recordingSlot = slot;
		m_recordingStarted = false;
		m_recordingFramesLeft = static_cast<uint32_t>(m_snapshotFrames);
	}

	void Viewer::AdvanceSnapshot()
	{
		if (!m_recording) {
			return;
		}
		m_status = std::format("Recording snapshot {}", m_recording->snapshot.label);
		org::RenderGraph* graph = m_renderer.GetRenderGraph();
		if (graph == nullptr) {
			return;
		}
		if (!m_recordingStarted) {
			if (m_cutDirty || !Settled(1.0)) {
				return;
			}
			m_renderer.SetDeterministicSamplingMode(true);
			m_recordingStarted = true;
			m_recording->snapshot.worldCut = WorldCut();
			m_recording->snapshot.threshold = m_threshold;
			m_recording->snapshot.frames = m_recordingFramesLeft;
			const auto single = [&](std::string_view name, auto handler) {
				const LayerDefinition& layer = FindLayer(name);
				auto resource = graph->RequestResourcePtr(org::ResourceIdentifier{ std::string(layer.resource) }, true);
				if (!resource) {
					return;
				}
				m_recording->outstanding.fetch_add(1u);
				graph->GetReadbackService()->RequestReadbackCapture(std::string(layer.anchorPass), resource.get(), org::RangeSpec{},
					[recording = m_recording, handler](org::ReadbackCaptureResult&& result) {
						if (!result.layouts.empty()) {
							std::scoped_lock lock(recording->mutex);
							handler(*recording, result);
						}
						recording->outstanding.fetch_sub(1u);
					});
			};
			single("base-color-opacity", [](SnapshotRecording& recording, const org::ReadbackCaptureResult& result) {
				if (result.format != rhi::Format::R8G8B8A8_UNorm) {
					return;
				}
				recording.snapshot.base.resize(static_cast<size_t>(result.width) * result.height);
				const auto& layout = result.layouts.front();
				for (uint32_t y = 0; y < result.height; ++y) {
					std::memcpy(recording.snapshot.base.data() + static_cast<size_t>(y) * result.width, result.data.data() + layout.offset + layout.rowPitch * y, result.width * 4u);
				}
			});
			single("normal-roughness", [](SnapshotRecording& recording, const org::ReadbackCaptureResult& result) {
				if (result.format != rhi::Format::R16G16B16A16_Float) {
					return;
				}
				recording.snapshot.normal.resize(static_cast<size_t>(result.width) * result.height * 4u);
				const auto& layout = result.layouts.front();
				for (uint32_t y = 0; y < result.height; ++y) {
					const auto* row = reinterpret_cast<const uint16_t*>(result.data.data() + layout.offset + layout.rowPitch * y);
					float* out = recording.snapshot.normal.data() + static_cast<size_t>(y) * result.width * 4u;
					for (uint32_t index = 0; index < result.width * 4u; ++index) {
						out[index] = HalfToFloat(row[index]);
					}
				}
			});
		}
		if (m_recordingFramesLeft != 0u) {
			--m_recordingFramesLeft;
			const LayerDefinition& hdrLayer = FindLayer("hdr");
			const LayerDefinition& keyLayer = FindLayer("visibility");
			auto hdr = graph->RequestResourcePtr(org::ResourceIdentifier{ std::string(hdrLayer.resource) }, true);
			auto keys = graph->RequestResourcePtr(org::ResourceIdentifier{ std::string(keyLayer.resource) }, true);
			if (hdr && keys) {
				m_recording->outstanding.fetch_add(2u);
				graph->GetReadbackService()->RequestReadbackCapture(std::string(hdrLayer.anchorPass), hdr.get(), org::RangeSpec{},
					[recording = m_recording](org::ReadbackCaptureResult&& result) {
						std::scoped_lock lock(recording->mutex);
						if (result.format == rhi::Format::R16G16B16A16_Float && !result.layouts.empty()) {
							const size_t pixels = static_cast<size_t>(result.width) * result.height;
							if (recording->sum.empty()) {
								recording->sum.assign(pixels * 3u, 0.0);
								recording->snapshot.width = result.width;
								recording->snapshot.height = result.height;
							}
							const auto& layout = result.layouts.front();
							for (uint32_t y = 0; y < result.height; ++y) {
								const auto* row = reinterpret_cast<const uint16_t*>(result.data.data() + layout.offset + layout.rowPitch * y);
								double* out = recording->sum.data() + static_cast<size_t>(y) * result.width * 3u;
								for (uint32_t x = 0; x < result.width; ++x) {
									out[x * 3u] += HalfToFloat(row[x * 4u]);
									out[x * 3u + 1u] += HalfToFloat(row[x * 4u + 1u]);
									out[x * 3u + 2u] += HalfToFloat(row[x * 4u + 2u]);
								}
							}
							++recording->hdrFrames;
						}
						recording->outstanding.fetch_sub(1u);
					});
				graph->GetReadbackService()->RequestReadbackCapture(std::string(keyLayer.anchorPass), keys.get(), org::RangeSpec{},
					[recording = m_recording](org::ReadbackCaptureResult&& result) {
						std::scoped_lock lock(recording->mutex);
						if (result.format == rhi::Format::R32G32_UInt && !result.layouts.empty()) {
							const size_t pixels = static_cast<size_t>(result.width) * result.height;
							if (recording->hits.empty()) {
								recording->hits.assign(pixels, 0u);
							}
							const auto& layout = result.layouts.front();
							for (uint32_t y = 0; y < result.height; ++y) {
								const auto* row = reinterpret_cast<const uint64_t*>(result.data.data() + layout.offset + layout.rowPitch * y);
								uint32_t* out = recording->hits.data() + static_cast<size_t>(y) * result.width;
								for (uint32_t x = 0; x < result.width; ++x) {
									out[x] += row[x] != ~uint64_t{ 0 } ? 1u : 0u;
								}
							}
							++recording->coverageFrames;
						}
						recording->outstanding.fetch_sub(1u);
					});
			}
			return;
		}
		if (m_recording->outstanding.load() != 0u) {
			return;
		}
		// With the images in, one inspection of the same cut says which pixels are voxels.
		if (m_recording->snapshot.inspect == nullptr) {
			if (m_sweepRequested) {
				if (m_inspectArrivals.load() == m_sweepArrivalsAtRequest) {
					return;
				}
				m_sweepRequested = false;
				m_recording->snapshot.inspect = CurrentFrame();
			}
			else {
				if (m_inspectInFlight.load()) {
					return;
				}
				m_sweepArrivalsAtRequest = m_inspectArrivals.load();
				RequestInspection(true);
				m_sweepRequested = m_inspectInFlight.load();
				return;
			}
		}

		Snapshot& snapshot = m_recording->snapshot;
		const size_t pixels = static_cast<size_t>(snapshot.width) * snapshot.height;
		snapshot.hdr.resize(pixels * 3u);
		const double frames = static_cast<double>(std::max(m_recording->hdrFrames, 1u));
		for (size_t index = 0; index < pixels * 3u && index < m_recording->sum.size(); ++index) {
			snapshot.hdr[index] = static_cast<float>(m_recording->sum[index] / frames);
		}
		snapshot.coverage.resize(pixels);
		const float coverageFrames = static_cast<float>(std::max(m_recording->coverageFrames, 1u));
		for (size_t index = 0; index < pixels && index < m_recording->hits.size(); ++index) {
			snapshot.coverage[index] = static_cast<float>(m_recording->hits[index]) / coverageFrames;
		}
		snapshot.valid = pixels != 0u;
		m_snapshots[m_recordingSlot] = std::move(snapshot);
		m_recording.reset();
		m_renderer.SetDeterministicSamplingMode(false);
		SetOutput(m_recordOutputRestore);
		m_compareDirty = true;
		m_status = "Snapshot finished.";
		if (m_pendingAfter >= 0) {
			const int next = m_pendingAfter;
			m_pendingAfter = -1;
			const float restore = m_recordRestore;
			SetWorldCut(m_pendingCut);
			StartSnapshot(next);
			m_recordRestore = restore;
			return;
		}
		SetThreshold(m_recordRestore);
		ComputeCompareStats();
		m_comparePanel = true;
	}

	void Viewer::CaptureScreenshot(const std::filesystem::path& path)
	{
		if (m_window == nullptr) {
			return;
		}
		RECT client{};
		GetClientRect(m_window, &client);
		POINT origin{ 0, 0 };
		ClientToScreen(m_window, &origin);
		const int width = client.right - client.left, height = client.bottom - client.top;
		HDC screen = GetDC(nullptr);
		HDC memory = CreateCompatibleDC(screen);
		HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
		HGDIOBJ previous = SelectObject(memory, bitmap);
		// Asks the compositor for the window, so another window on top of it does not end up in the image.
		constexpr UINT kClientOnly = 1u, kRenderFullContent = 2u;
		if (!PrintWindow(m_window, memory, kClientOnly | kRenderFullContent)) {
			BitBlt(memory, 0, 0, width, height, screen, origin.x, origin.y, SRCCOPY);
		}
		BITMAPINFO info{};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = width;
		info.bmiHeader.biHeight = -height;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;
		std::vector<uint8_t> bgra(static_cast<size_t>(width) * height * 4u);
		GetDIBits(memory, bitmap, 0, height, bgra.data(), &info, DIB_RGB_COLORS);
		SelectObject(memory, previous);
		DeleteObject(bitmap);
		DeleteDC(memory);
		ReleaseDC(nullptr, screen);
		std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3u);
		for (size_t index = 0; index < static_cast<size_t>(width) * height; ++index) {
			rgb[index * 3u] = bgra[index * 4u + 2u];
			rgb[index * 3u + 1u] = bgra[index * 4u + 1u];
			rgb[index * 3u + 2u] = bgra[index * 4u];
		}
		std::string error;
		std::error_code ignored;
		std::filesystem::create_directories(path.parent_path(), ignored);
		if (clodeval::image::WritePng(path, static_cast<uint32_t>(width), static_cast<uint32_t>(height), rgb, error)) {
			m_status = std::format("Saved {}", path.string());
		}
		else {
			m_status = error;
		}
	}

	void Viewer::OnResize(uint32_t width, uint32_t height)
	{
		m_width = width;
		m_height = height;
		m_projectionDirty = true;
		if (m_lockWorldCut && !m_forceLeaf) {
			m_cutDirty = true;
		}
	}

	void Viewer::BeginFrame()
	{
		ApplyCamera();
		if (m_autoFlip && !m_sweeping && !m_recording) {
			const auto now = std::chrono::steady_clock::now();
			if (std::chrono::duration<float>(now - m_lastFlip).count() >= 0.5f / m_flipHertz) {
				m_lastFlip = now;
				m_flipOnB = !m_flipOnB;
				SetWorldCut(m_flipOnB ? m_flipB : m_flipA);
			}
		}
		ApplyCut();
		AdvanceSweep();
		AdvanceSnapshot();
		AdvanceScript();
	}

	void Viewer::DrawPanels()
	{
		// Scales text with the monitor, since the window is in real pixels.
		// Limited by the window as well, so a small window on a high-DPI monitor still has room for the image.
		if (m_window != nullptr) {
			m_uiScale = std::clamp(std::min(static_cast<float>(GetDpiForWindow(m_window)) / 96.0f, static_cast<float>(m_height) / 900.0f), 1.0f, 3.0f);
			ImGui::GetIO().FontGlobalScale = m_uiScale;
		}
		AdvanceInspection();
		HandleSceneInput();
		DrawOverlayImage();
		if (!m_hidePanels) {
			DrawControlPanel();
			DrawHierarchyPanel();
			DrawGroupPanel();
			DrawTransitionPanel();
			if (m_comparePanel) {
				DrawComparePanel();
			}
		}
		DrawHoverTooltip();
	}

	// ---- Compare

	float Viewer::CompareExposure() const
	{
		const Snapshot& a = m_snapshots[0].valid ? m_snapshots[0] : m_snapshots[1];
		std::vector<float> luminance;
		for (size_t index = 0; index < a.coverage.size(); ++index) {
			if (a.coverage[index] > 0.5f) {
				luminance.push_back(0.2126f * a.hdr[index * 3u] + 0.7152f * a.hdr[index * 3u + 1u] + 0.0722f * a.hdr[index * 3u + 2u]);
			}
		}
		if (luminance.empty()) {
			return 1.0f;
		}
		std::nth_element(luminance.begin(), luminance.begin() + luminance.size() / 2u, luminance.end());
		const float median = luminance[luminance.size() / 2u];
		return median > 0.0f ? 0.5f / median : 1.0f;
	}

	void Viewer::ComputeCompareStats()
	{
		m_compareStats = {};
		m_compareExposure = CompareExposure();
		const Snapshot& a = m_snapshots[0];
		const Snapshot& b = m_snapshots[1];
		if (!a.valid || !b.valid || a.width != b.width || a.height != b.height) {
			return;
		}
		const size_t pixels = static_cast<size_t>(a.width) * a.height;
		double total = 0.0, coverageA = 0.0, coverageB = 0.0, lumA = 0.0, lumB = 0.0, normal = 0.0;
		uint64_t count = 0u, changed = 0u, normalCount = 0u;
		for (size_t index = 0; index < pixels; ++index) {
			coverageA += a.coverage[index];
			coverageB += b.coverage[index];
			if (a.coverage[index] <= 0.0f && b.coverage[index] <= 0.0f) {
				continue;
			}
			++count;
			float delta = 0.0f;
			for (size_t channel = 0; channel < 3u; ++channel) {
				const float ta = ToneMapChannel(a.hdr[index * 3u + channel], m_compareExposure), tb = ToneMapChannel(b.hdr[index * 3u + channel], m_compareExposure);
				delta += std::abs(ta - tb);
				lumA += ta;
				lumB += tb;
			}
			total += delta / 3.0f;
			changed += delta / 3.0f > 2.0f / 255.0f ? 1u : 0u;
			if (!a.normal.empty() && !b.normal.empty() && a.coverage[index] > 0.5f && b.coverage[index] > 0.5f) {
				const float* n = a.normal.data() + index * 4u;
				const float* m = b.normal.data() + index * 4u;
				const float lengths = std::sqrt((n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) * (m[0] * m[0] + m[1] * m[1] + m[2] * m[2]));
				if (lengths > 0.0f) {
					normal += std::acos(std::clamp((n[0] * m[0] + n[1] * m[1] + n[2] * m[2]) / lengths, -1.0f, 1.0f)) * 180.0f / kPi;
					++normalCount;
				}
			}
		}
		m_compareStats.valid = true;
		m_compareStats.meanDelta = count != 0u ? total / static_cast<double>(count) : 0.0;
		m_compareStats.changedPixels = count != 0u ? static_cast<double>(changed) / static_cast<double>(count) : 0.0;
		m_compareStats.coverageRatio = coverageA > 0.0 ? coverageB / coverageA : 0.0;
		m_compareStats.brightnessRatio = lumA > 0.0 ? lumB / lumA : 0.0;
		m_compareStats.normalDegrees = normalCount != 0u ? normal / static_cast<double>(normalCount) : 0.0;
		m_compareStats.voxelFractionA = a.inspect ? a.inspect->VoxelFraction() : 0.0;
		m_compareStats.voxelFractionB = b.inspect ? b.inspect->VoxelFraction() : 0.0;
	}

	void Viewer::BuildTile(int which, uint32_t width, uint32_t height, std::vector<uint32_t>& out) const
	{
		out.assign(static_cast<size_t>(width) * height, 0xFF181818u);
		const Snapshot& reference = m_snapshots[0].valid ? m_snapshots[0] : m_snapshots[1];
		if (!reference.valid) {
			return;
		}
		const auto color = [&](const Snapshot& snapshot, size_t index, float rgb[3]) {
			rgb[0] = rgb[1] = rgb[2] = 0.1f;
			if (!snapshot.valid) {
				return;
			}
			switch (m_compareLayer) {
			case CompareLayer::Lit:
				for (size_t channel = 0; channel < 3u; ++channel) {
					rgb[channel] = ToneMapChannel(snapshot.hdr[index * 3u + channel], m_compareExposure);
				}
				break;
			case CompareLayer::BaseColor:
				if (index < snapshot.base.size() && snapshot.coverage[index] > 0.0f) {
					const uint32_t value = snapshot.base[index];
					rgb[0] = (value & 0xFFu) / 255.0f;
					rgb[1] = ((value >> 8u) & 0xFFu) / 255.0f;
					rgb[2] = ((value >> 16u) & 0xFFu) / 255.0f;
				}
				break;
			case CompareLayer::Normal:
				if (index * 4u + 2u < snapshot.normal.size() && snapshot.coverage[index] > 0.0f) {
					for (size_t channel = 0; channel < 3u; ++channel) {
						rgb[channel] = snapshot.normal[index * 4u + channel] * 0.5f + 0.5f;
					}
				}
				break;
			case CompareLayer::Coverage:
				rgb[0] = rgb[1] = rgb[2] = snapshot.coverage[index];
				break;
			case CompareLayer::Path:
				if (snapshot.inspect && index < snapshot.inspect->pixels.size()) {
					const PixelInfo& pixel = snapshot.inspect->pixels[index];
					if ((pixel.flags & kPixelHit) != 0u) {
						const bool voxel = (pixel.flags & kPixelVoxel) != 0u;
						rgb[0] = voxel ? 0.95f : 0.10f;
						rgb[1] = voxel ? 0.15f : 0.95f;
						rgb[2] = voxel ? 0.15f : 0.20f;
					}
				}
				break;
			default:
				break;
			}
		};
		const float zoom = std::max(m_compareZoom, 1.0f);
		const float sourceWidth = static_cast<float>(reference.width), sourceHeight = static_cast<float>(reference.height);
		const float spanX = sourceWidth / zoom, spanY = sourceHeight / zoom;
		const float originX = std::clamp(m_compareCenter[0] * sourceWidth - spanX * 0.5f, 0.0f, sourceWidth - spanX);
		const float originY = std::clamp(m_compareCenter[1] * sourceHeight - spanY * 0.5f, 0.0f, sourceHeight - spanY);
		for (uint32_t y = 0; y < height; ++y) {
			const uint32_t sourceY = std::min(static_cast<uint32_t>(originY + (y + 0.5f) * spanY / height), reference.height - 1u);
			for (uint32_t x = 0; x < width; ++x) {
				const uint32_t sourceX = std::min(static_cast<uint32_t>(originX + (x + 0.5f) * spanX / width), reference.width - 1u);
				const size_t index = static_cast<size_t>(sourceY) * reference.width + sourceX;
				float rgb[3], other[3];
				if (which == 0) {
					const Snapshot* snapshot = &m_snapshots[0];
					if (m_compareLayout == CompareLayout::Flip && m_compareFlipB) {
						snapshot = &m_snapshots[1];
					}
					else if (m_compareLayout == CompareLayout::Wipe && static_cast<float>(x) / width >= m_compareWipe) {
						snapshot = &m_snapshots[1];
					}
					color(*snapshot, index, rgb);
				}
				else if (which == 1) {
					color(m_snapshots[1], index, rgb);
				}
				else if (which == 2) {
					color(m_snapshots[0], index, rgb);
					color(m_snapshots[1], index, other);
					for (int channel = 0; channel < 3; ++channel) {
						rgb[channel] = std::abs(rgb[channel] - other[channel]) * m_diffGain;
					}
				}
				else {
					// Which path each snapshot took: dark none, green stayed triangles, red stayed voxels, yellow switched.
					const auto path = [&](const Snapshot& snapshot) -> int {
						if (!snapshot.valid || !snapshot.inspect || index >= snapshot.inspect->pixels.size() || (snapshot.inspect->pixels[index].flags & kPixelHit) == 0u) {
							return 0;
						}
						return (snapshot.inspect->pixels[index].flags & kPixelVoxel) != 0u ? 2 : 1;
					};
					const int a = path(m_snapshots[0]), b = path(m_snapshots[1]);
					rgb[0] = rgb[1] = rgb[2] = 0.1f;
					if (a == 1 && b == 1) { rgb[0] = 0.1f; rgb[1] = 0.7f; rgb[2] = 0.2f; }
					else if (a == 2 && b == 2) { rgb[0] = 0.8f; rgb[1] = 0.15f; rgb[2] = 0.15f; }
					else if (a == 1 && b == 2) { rgb[0] = 1.0f; rgb[1] = 0.9f; rgb[2] = 0.1f; }
					else if (a == 2 && b == 1) { rgb[0] = 0.1f; rgb[1] = 0.9f; rgb[2] = 1.0f; }
					else if (a == 0 && b != 0) { rgb[0] = 1.0f; rgb[1] = 0.3f; rgb[2] = 0.9f; }
					else if (a != 0 && b == 0) { rgb[0] = 0.4f; rgb[1] = 0.4f; rgb[2] = 0.9f; }
				}
				out[static_cast<size_t>(y) * width + x] = PackUnit(rgb[0], rgb[1], rgb[2]);
			}
		}
	}

	void Viewer::BuildCompareTiles()
	{
		if (!m_compareDirty) {
			return;
		}
		m_compareDirty = false;
		m_compareExposure = CompareExposure();
		const bool single = m_compareLayout != CompareLayout::Grid;
		const Snapshot& reference = m_snapshots[0].valid ? m_snapshots[0] : m_snapshots[1];
		if (!reference.valid) {
			return;
		}
		const uint32_t width = single ? 1120u : 560u;
		const uint32_t height = std::max(width * reference.height / std::max(reference.width, 1u), 1u);
		for (int tile = 0; tile < (single ? 1 : 4); ++tile) {
			BuildTile(tile, width, height, m_tilePixels[tile]);
			m_tiles[tile].Upload(width, height, m_tilePixels[tile]);
		}
	}

	// ---- Script

	void Viewer::RunScript(nlohmann::json steps)
	{
		for (nlohmann::json& step : steps) {
			m_script.push_back(std::move(step));
		}
	}

	void Viewer::AdvanceScript()
	{
		if (m_script.empty() && m_scriptWaitFor.empty()) {
			return;
		}
		if (m_scriptWait != 0u) {
			--m_scriptWait;
			return;
		}
		if (m_scriptWaitFor == "hierarchy" && !m_hierarchy.Ready()) {
			return;
		}
		if (m_scriptWaitFor == "sweep" && m_sweeping) {
			return;
		}
		if (m_scriptWaitFor == "snapshot" && (m_recording || m_pendingAfter >= 0)) {
			return;
		}
		if (m_scriptWaitFor == "settled" && (m_cutDirty || !Settled(1.0))) {
			return;
		}
		if (m_scriptWaitFor == "inspection" && m_inspectArrivals.load() < m_scriptWaitArrivals) {
			return;
		}
		m_scriptWaitFor.clear();
		if (m_script.empty()) {
			return;
		}
		nlohmann::json step = std::move(m_script.front());
		m_script.pop_front();
		spdlog::info("viewer script: {}", step.dump());
		if (step.contains("wait_frames")) { m_scriptWait = step["wait_frames"].get<uint32_t>(); }
		if (step.contains("wait_for")) { m_scriptWaitFor = step["wait_for"].get<std::string>(); }
		if (step.contains("threshold")) { SetThreshold(step["threshold"].get<float>()); }
		if (step.contains("world_cut")) { SetWorldCut(step["world_cut"].get<float>()); }
		if (step.contains("force_leaf")) { m_forceLeaf = step["force_leaf"].get<bool>(); m_cutDirty = true; }
		if (step.contains("lock_world_cut")) { m_lockWorldCut = step["lock_world_cut"].get<bool>(); m_lockedWorldCut = WorldCut(); }
		if (step.contains("overlay")) { m_overlay = static_cast<OverlayMode>(step["overlay"].get<int>()); m_overlayDirty = true; }
		if (step.contains("output")) { SetOutput(step["output"].get<uint32_t>()); }
		if (step.contains("preset")) { ApplyPreset(m_presets.at(step["preset"].get<size_t>())); }
		if (step.contains("distance")) { m_distance = step["distance"].get<float>(); m_cameraDirty = true; }
		if (step.contains("focus")) { m_focus = ReadVector(step["focus"]); m_cameraDirty = true; }
		if (step.contains("angles")) { m_yaw = step["angles"][0].get<float>(); m_pitch = step["angles"][1].get<float>(); m_cameraDirty = true; }
		if (step.contains("pixel_world")) { m_distance = step["pixel_world"].get<float>() / PixelWorldPerDepth(); m_cameraDirty = true; }
		if (step.contains("fresh_inspection")) {
			// Waits for a frame requested from here on, so what is read next reflects the view as it is now.
			m_scriptWaitArrivals = m_inspectArrivals.load() + 2u;
			m_scriptWaitFor = "inspection";
		}
		if (step.contains("dump_inspection")) {
			// Raw per-pixel attribution for offline analysis: u32 width, u32 height, then per pixel f32 depth, u32 group, i16 mesh, u8 primitive, u8 flags.
			if (const auto frame = CurrentFrame()) {
				std::ofstream output(step["dump_inspection"].get<std::string>(), std::ios::binary);
				output.write(reinterpret_cast<const char*>(&frame->width), sizeof(frame->width));
				output.write(reinterpret_cast<const char*>(&frame->height), sizeof(frame->height));
				for (const PixelInfo& pixel : frame->pixels) {
					output.write(reinterpret_cast<const char*>(&pixel.depth), sizeof(pixel.depth));
					output.write(reinterpret_cast<const char*>(&pixel.group), sizeof(pixel.group));
					output.write(reinterpret_cast<const char*>(&pixel.mesh), sizeof(pixel.mesh));
					output.write(reinterpret_cast<const char*>(&pixel.primitive), sizeof(pixel.primitive));
					output.write(reinterpret_cast<const char*>(&pixel.flags), sizeof(pixel.flags));
				}
			}
		}
		if (step.contains("dump_hierarchy") && m_hierarchy.Ready()) {
			nlohmann::json meshes = nlohmann::json::array();
			for (const MeshInfo& mesh : m_hierarchy.Meshes()) {
				nlohmann::json groups = nlohmann::json::array();
				for (const GroupInfo& group : mesh.groups) {
					groups.push_back({ group.depth, group.voxel ? 1 : 0, group.voxelLevel, group.enterError, std::isfinite(group.leaveError) ? group.leaveError : -1.0f, std::isfinite(group.firstVoxelError) ? group.firstVoxelError : -1.0f, group.voxelWidth, group.triangles, group.cells, group.cubes });
				}
				const ClusterLODCacheSource& source = mesh.cache->Prebuilt().cacheSource;
				meshes.push_back({ { "name", mesh.name }, { "short", mesh.shortName }, { "assembly", mesh.assembly }, { "identifier", source.sourceIdentifier }, { "leaf_triangles", mesh.leafTriangles }, { "groups", std::move(groups) } });
			}
			std::ofstream(step["dump_hierarchy"].get<std::string>()) << meshes.dump() << '\n';
		}
		if (step.contains("fov")) { SetFov(step["fov"].get<float>(), true); }
		if (step.contains("pick")) { SelectPixel(step["pick"][0].get<uint32_t>(), step["pick"][1].get<uint32_t>()); }
		if (step.contains("select")) { SelectGroup(step["select"][0].get<int32_t>(), step["select"][1].get<int32_t>()); }
		if (step.contains("sweep_range")) { m_sweepLow = step["sweep_range"][0].get<float>(); m_sweepHigh = step["sweep_range"][1].get<float>(); }
		if (step.contains("sweep")) { StartSweep(); }
		if (step.contains("snapshot_pair")) {
			SetWorldCut(step["snapshot_pair"][0].get<float>());
			m_pendingAfter = 1;
			m_pendingCut = step["snapshot_pair"][1].get<float>();
			StartSnapshot(0);
		}
		if (step.contains("compare_layer")) { m_compareLayer = static_cast<CompareLayer>(step["compare_layer"].get<int>()); m_compareDirty = true; }
		if (step.contains("compare_layout")) { m_compareLayout = static_cast<CompareLayout>(step["compare_layout"].get<int>()); m_compareDirty = true; }
		if (step.contains("compare_zoom")) { m_compareZoom = step["compare_zoom"].get<float>(); m_compareDirty = true; }
		if (step.contains("compare_panel")) { m_comparePanel = step["compare_panel"].get<bool>(); }
		if (step.contains("hide_panels")) { m_hidePanels = step["hide_panels"].get<bool>(); }
		if (step.contains("zoom")) { m_distance /= step["zoom"].get<float>(); m_cameraDirty = true; }
		if (step.contains("resize")) {
			SetWindowPos(m_window, nullptr, 0, 0, step["resize"][0].get<int>(), step["resize"][1].get<int>(), SWP_NOMOVE | SWP_NOZORDER);
		}
		if (step.contains("screenshot")) { CaptureScreenshot(step["screenshot"].get<std::string>()); }
		if (step.contains("report")) {
			const auto frame = CurrentFrame();
			nlohmann::json report;
			report["threshold"] = m_threshold;
			report["world_cut"] = WorldCut();
			report["pixel_world"] = PixelWorldAtFocus();
			report["hierarchy_ready"] = m_hierarchy.Ready();
			report["hierarchy_meshes"] = m_hierarchy.Ready() ? m_hierarchy.Meshes().size() : 0u;
			report["hierarchy_error"] = m_hierarchy.Error();
			report["status"] = m_status;
			if (frame) {
				report["frame"] = { { "width", frame->width }, { "height", frame->height }, { "hit", frame->hitPixels }, { "voxel", frame->voxelPixels }, { "unknown", frame->unknownPixels }, { "voxel_fraction", frame->VoxelFraction() } };
			}
			report["selection"] = { { "mesh", m_selection.mesh }, { "group", m_selection.group }, { "voxel", m_selectedVoxel }, { "depth", m_selectedDepth }, { "has_cube", m_selectedCube.has_value() } };
			if (m_selection.mesh >= 0 && m_hierarchy.Ready()) {
				const MeshInfo& mesh = m_hierarchy.Meshes()[m_selection.mesh];
				const GroupInfo& group = mesh.groups[m_selection.group];
				report["selection"]["mesh_name"] = mesh.name;
				report["selection"]["enter"] = group.enterError;
				report["selection"]["leave"] = std::isfinite(group.leaveError) ? group.leaveError : -1.0f;
				report["selection"]["level"] = group.voxelLevel;
				report["selection"]["group_depth"] = group.depth;
				report["selection"]["width"] = group.voxelWidth;
				report["selection"]["hand_over"] = std::isfinite(group.firstVoxelError) ? group.firstVoxelError : -1.0f;
			}
			if (m_selectedCube) {
				report["cube"] = { { "occupancy_bits", std::popcount(m_selectedCube->occupancy) }, { "opacity_sum", m_selectedCube->opacitySum }, { "width", m_selectedCube->voxelWidth } };
			}
			nlohmann::json curve = nlohmann::json::array();
			for (const CurvePoint& point : m_liveCurve) {
				curve.push_back({ point.worldCut, point.voxelFraction });
			}
			report["live_curve"] = curve;
			report["sweep_before"] = m_sweepBefore;
			report["sweep_after"] = m_sweepAfter;
			report["compare"] = { { "valid", m_compareStats.valid }, { "mean_delta", m_compareStats.meanDelta }, { "changed", m_compareStats.changedPixels }, { "coverage_ratio", m_compareStats.coverageRatio }, { "brightness_ratio", m_compareStats.brightnessRatio }, { "normal_degrees", m_compareStats.normalDegrees }, { "voxel_a", m_compareStats.voxelFractionA }, { "voxel_b", m_compareStats.voxelFractionB } };
			std::ofstream(step["report"].get<std::string>()) << report.dump(1) << '\n';
		}
		if (step.contains("exit")) { m_exit = true; }
	}

	// ---- Entry

	namespace {
		Viewer* g_viewer = nullptr;
		Renderer* g_renderer = nullptr;
		bool g_quit = false;

		LRESULT CALLBACK ViewerWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
		{
			if (g_renderer != nullptr && g_renderer->HandleMenuInput(window, message, wParam, lParam)) {
				return 0;
			}
			switch (message) {
			case WM_SIZE:
				if (wParam != SIZE_MINIMIZED && g_viewer != nullptr && g_renderer != nullptr && g_renderer->IsInitialized() && LOWORD(lParam) != 0u && HIWORD(lParam) != 0u) {
					g_renderer->OnResize(LOWORD(lParam), HIWORD(lParam));
					g_viewer->OnResize(LOWORD(lParam), HIWORD(lParam));
				}
				return 0;
			case WM_DESTROY:
				g_quit = true;
				PostQuitMessage(0);
				return 0;
			default:
				return DefWindowProcW(window, message, wParam, lParam);
			}
		}

		HWND CreateViewerWindow(uint32_t width, uint32_t height, const std::wstring& title)
		{
			WNDCLASSW windowClass{};
			windowClass.lpfnWndProc = ViewerWindowProcedure;
			windowClass.hInstance = GetModuleHandleW(nullptr);
			windowClass.lpszClassName = L"CLodViewerWindow";
			windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
			if (!RegisterClassW(&windowClass)) {
				throw std::runtime_error("could not register the window class");
			}
			RECT rectangle{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
			AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE);
			HWND window = CreateWindowExW(0, windowClass.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
				rectangle.right - rectangle.left, rectangle.bottom - rectangle.top, nullptr, nullptr, windowClass.hInstance, nullptr);
			if (window == nullptr) {
				throw std::runtime_error("could not create the window");
			}
			return window;
		}
	}

	int RunViewer(const std::filesystem::path& configPath, const std::filesystem::path& scriptPath)
	{
		std::ifstream input(configPath);
		if (!input) {
			throw std::runtime_error(std::format("cannot open viewer config '{}'", configPath.string()));
		}
		nlohmann::json config;
		input >> config;

		CLodCache::SetCacheOnlyMode(true);
		SetDllDirectoryA(".\\D3D\\");
		// Before any window exists. ImGui turns this on later anyway, and a window from before the change keeps
		// coordinates in a different scale from the renderer's.
		SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

		const uint32_t width = config.at("resolution").at(0).get<uint32_t>();
		const uint32_t height = config.at("resolution").at(1).get<uint32_t>();
		const std::string title = config.value("title", std::string("CLod viewer"));
		HWND window = CreateViewerWindow(width, height, std::wstring(title.begin(), title.end()));
		RECT client{};
		GetClientRect(window, &client);
		const uint32_t clientWidth = static_cast<uint32_t>(client.right - client.left), clientHeight = static_cast<uint32_t>(client.bottom - client.top);

		// A window that is closed never tears down: nothing the renderer holds outlives the process, and
		// destroying it mid-frame is only a way to turn a clean exit into a crash.
		Renderer& renderer = *new Renderer();
		renderer.Initialize(window, clientWidth, clientHeight, MakeCapturePipeline(true));
		SettingsManager::GetInstance().getSettingSetter<bool>("enableSceneRenderOverlap")(false);
		SettingsManager::GetInstance().getSettingSetter<UpscalingMode>("upscalingMode")(UpscalingMode::None);

		CaptureScene* scene = nullptr;
		{
			org::BufferBase::ScopedBackingMutation startupBackingMutation;
			renderer.SetCurrentScene(std::make_shared<Scene>());
			scene = new CaptureScene();
			scene->SetPartFilter(config.value("part_filter", std::string()));
			for (const nlohmann::json& asset : config.at("assets")) {
				scene->AddAsset({ asset.at("path").get<std::string>(), asset.value("scale", 1.0f) });
			}
			renderer.SetEnvironment(config.value("environment", std::string("sky")));
			const XMFLOAT3 center = ReadVector(config.at("frame").at("center"));
			const float radius = config.at("frame").at("radius").get<float>();
			const XMFLOAT3 start{ center.x, center.y, center.z + radius * 2.5f };
			renderer.GetCurrentScene()->SetCamera(start, center, XMFLOAT3(0.0f, 1.0f, 0.0f), 60.0f * kPi / 180.0f, static_cast<float>(clientWidth) / clientHeight, 0.01f, 100000.0f);
			const nlohmann::json light = config.value("light", nlohmann::json::object());
			const XMFLOAT3 direction = light.contains("direction") ? ReadVector(light.at("direction")) : XMFLOAT3(-0.1f, -0.5f, -0.9f);
			renderer.GetCurrentScene()->CreateDirectionalLightECS(L"viewer-light", XMFLOAT3(1.0f, 1.0f, 1.0f), light.value("intensity", 10.0f), direction, false);
		}

		Viewer* viewer = new Viewer(renderer, *scene, std::move(config), clientWidth, clientHeight);
		viewer->SetWindow(window);
		if (!scriptPath.empty()) {
			std::ifstream scriptInput(scriptPath);
			if (!scriptInput) {
				throw std::runtime_error(std::format("cannot open script '{}'", scriptPath.string()));
			}
			nlohmann::json steps;
			scriptInput >> steps;
			viewer->RunScript(std::move(steps));
		}
		g_viewer = viewer;
		g_renderer = &renderer;
		br::diagnostics::SetExternalUiPanel([viewer]() { viewer->DrawPanels(); });
		ShowWindow(window, SW_SHOWNORMAL);
		SetForegroundWindow(window);

		auto last = std::chrono::steady_clock::now();
		while (!g_quit && !viewer->WantsExit()) {
			MSG message{};
			while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
				TranslateMessage(&message);
				DispatchMessageW(&message);
			}
			if (g_quit) {
				break;
			}
			if (CLodCache::GetCacheOnlyViolationCount() != 0u) {
				throw std::runtime_error("the cache is missing a mesh this scene needs: see the critical message in the log");
			}
			scene->Tick(renderer);
			viewer->BeginFrame();
			const auto now = std::chrono::steady_clock::now();
			renderer.Update(std::chrono::duration<float>(now - last).count());
			last = now;
			renderer.Render();
		}
		spdlog::default_logger()->flush();
		std::fflush(nullptr);
		std::_Exit(0);
	}

}
