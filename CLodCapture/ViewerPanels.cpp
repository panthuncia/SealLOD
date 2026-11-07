#include "Viewer.h"

#include <algorithm>
#include <cmath>
#include <format>

#include <imgui.h>
#include <implot.h>

#include <BasicRenderer/Assets/Import/CLodCache.h>
#include <BasicRenderer/Diagnostics/OutputTypes.h>

#include <BasicRenderer/Diagnostics/ExternalUi.h>

namespace clodcapture {
	namespace {
		constexpr float kBeforeFactor = 0.97f, kAfterFactor = 1.03f;
		constexpr float kPi = 3.14159265f;

		// The debug views a hierarchy question is likely to want, ahead of the renderer's whole list.
		constexpr unsigned int kHierarchyOutputs[] = {
			COLOR, VOXEL_GEOMETRY_PATH, GEOMETRY_GROUP, MESHLETS, CLOD_ASSEMBLY_PARTS, CLOD_ASSEMBLY_VOXEL_INHERITANCE, VOXEL_UV_DENSITY,
			NORMAL, MODEL_NORMAL, ALBEDO, ROUGHNESS, METALLIC, AO, DEPTH, SW_RASTER, MATERIAL_UV, MATERIAL_SELECTED_MIP, MOTION_VECTORS,
		};

		void Help(const char* text)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("(?)");
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", text);
			}
		}

		ImVec4 Color(uint32_t rgba)
		{
			return ImGui::ColorConvertU32ToFloat4(rgba);
		}

		void DrawRampLegend(float width)
		{
			const ImVec2 start = ImGui::GetCursorScreenPos();
			ImDrawList* list = ImGui::GetWindowDrawList();
			constexpr int kSegments = 48;
			for (int segment = 0; segment < kSegments; ++segment) {
				const float x0 = start.x + width * segment / kSegments, x1 = start.x + width * (segment + 1) / kSegments;
				list->AddRectFilled(ImVec2(x0, start.y), ImVec2(x1, start.y + 10.0f), RampColor((segment + 0.5f) / kSegments));
			}
			ImGui::Dummy(ImVec2(width, 12.0f));
		}
	}

	std::string Viewer::FormatWorld(float value) const
	{
		if (!std::isfinite(value)) {
			return "none";
		}
		if (value <= 0.0f) {
			return "0";
		}
		if (value < 0.001f) {
			return std::format("{:.3f} mm", value * 1000.0f);
		}
		if (value < 1.0f) {
			return std::format("{:.2f} mm", value * 1000.0f);
		}
		return std::format("{:.3f} m", value);
	}

	void Viewer::DrawOverlayImage()
	{
		if (m_overlay == OverlayMode::Off || !m_overlayTexture.Valid()) {
			return;
		}
		const auto frame = CurrentFrame();
		// Painted from another camera it would sit in the wrong place. A moved cut still lines up.
		if (!frame || frame->stamp.cameraGeneration != m_cameraGeneration) {
			return;
		}
		const ImGuiIO& io = ImGui::GetIO();
		ImGui::GetBackgroundDrawList()->AddImage(m_overlayTexture.Id(), ImVec2(0.0f, 0.0f), io.DisplaySize);
	}

	void Viewer::DrawHoverTooltip()
	{
		const ImGuiIO& io = ImGui::GetIO();
		if (io.WantCaptureMouse || !ImGui::IsKeyDown(ImGuiKey_LeftAlt)) {
			return;
		}
		const auto frame = CurrentFrame();
		if (!frame || frame->stamp.cameraGeneration != m_cameraGeneration || io.MousePos.x < 0.0f || io.MousePos.y < 0.0f) {
			return;
		}
		const PixelInfo* pixel = frame->At(static_cast<uint32_t>(io.MousePos.x), static_cast<uint32_t>(io.MousePos.y));
		if (pixel == nullptr || (pixel->flags & kPixelHit) == 0u) {
			return;
		}
		ImGui::BeginTooltip();
		ImGui::Text("%s  cluster %u  primitive %u", (pixel->flags & kPixelVoxel) != 0u ? "voxel" : "triangle", pixel->cluster, pixel->primitive);
		ImGui::Text("View depth %.3f", pixel->depth);
		if (pixel->mesh >= 0 && m_hierarchy.Ready()) {
			const MeshInfo& mesh = m_hierarchy.Meshes()[pixel->mesh];
			const GroupInfo& group = mesh.groups[pixel->group];
			const float pixelWorld = frame->stamp.pixelWorldPerDepth * pixel->depth;
			ImGui::Text("%s", mesh.shortName.c_str());
			ImGui::Text("Group %u, depth %d", pixel->group, group.depth);
			ImGui::Text("Draws for a cut from %s %s", FormatWorld(group.enterError * m_worldScale).c_str(), std::isfinite(group.leaveError) ? std::format("to {}", FormatWorld(group.leaveError * m_worldScale)).c_str() : "upward: it is a root");
			if (group.voxel) {
				ImGui::Text("Cell %s, %.2f px", FormatWorld(group.voxelWidth * m_worldScale).c_str(), group.voxelWidth * m_worldScale / pixelWorld);
			}
			ImGui::Text("Becomes voxels at %s", FormatWorld(group.firstVoxelError * m_worldScale).c_str());
		}
		ImGui::EndTooltip();
	}

	void Viewer::DrawControlPanel()
	{
		ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(350.0f * m_uiScale, ImGui::GetIO().DisplaySize.y - 16.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowBgAlpha(0.88f);
		ImGui::Begin("CLod viewer");
		ImGui::TextWrapped("%s", m_title.c_str());

		const auto readiness = m_renderer.GetSamplingReadinessSnapshot(false);
		// Any one frame can look busy for a moment, so "Settled" means it has stayed idle for a while.
		static auto lastBusy = std::chrono::steady_clock::now();
		if (!IsIdle(readiness)) {
			lastBusy = std::chrono::steady_clock::now();
		}
		const bool idle = std::chrono::duration<float>(std::chrono::steady_clock::now() - lastBusy).count() > 0.5f;
		ImGui::TextColored(idle ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f) : ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", idle ? "Settled" : "Streaming");
		ImGui::SameLine();
		ImGui::Text("%u groups, %.0f MB resident", readiness.residentClodGroups, static_cast<double>(readiness.residentClodAllocationBytes) / 1.0e6);
		if (CLodCache::GetCacheOnlyViolationCount() != 0u) {
			ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "The cache is missing a mesh this scene needs.");
		}
		if (!m_hierarchy.Ready()) {
			ImGui::Text("Reading hierarchies: %u of %u", m_hierarchy.Loaded(), m_hierarchy.Total());
		}
		if (const std::string error = m_hierarchy.Error(); !error.empty()) {
			ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", error.c_str());
		}
		if (const auto frame = CurrentFrame()) {
			ImGui::Text("Voxels draw %.1f%% of %u hit pixels", frame->VoxelFraction() * 100.0f, frame->hitPixels);
			if (frame->unknownPixels != 0u && m_hierarchy.Ready()) {
				ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "%u pixels have no hierarchy on disk", frame->unknownPixels);
			}
		}
		if (!m_status.empty()) {
			ImGui::TextDisabled("%s", m_status.c_str());
		}

		if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
			if (ImGui::BeginCombo("Preset", m_lastPreset >= 0 ? m_presets[m_lastPreset].name.c_str() : "Choose")) {
				for (int index = 0; index < static_cast<int>(m_presets.size()); ++index) {
					if (ImGui::Selectable(m_presets[index].name.c_str(), index == m_lastPreset)) {
						m_lastPreset = index;
						ApplyPreset(m_presets[index]);
					}
				}
				ImGui::EndCombo();
			}
			const bool telephoto = m_fovDegrees <= 5.0f;
			if (ImGui::RadioButton("Perspective", !telephoto)) { SetFov(60.0f, true); }
			ImGui::SameLine();
			if (ImGui::RadioButton("Telephoto", telephoto)) { SetFov(2.0f, true); }
			Help("A 2 degree lens from far away: every part of the asset has nearly the same world-pixel size, so one cut applies across it, as an orthographic view would. Switching lens keeps the pixel size at the focus.");
			float fov = m_fovDegrees;
			if (ImGui::SliderFloat("FOV", &fov, 0.5f, 90.0f, "%.1f deg", ImGuiSliderFlags_Logarithmic)) { SetFov(fov, true); }
			float distance = m_distance;
			if (ImGui::DragFloat("Distance", &distance, m_distance * 0.005f, 0.001f, 1.0e5f, "%.3f")) { m_distance = distance; m_cameraDirty = true; }
			ImGui::Text("World pixel at focus: %s", FormatWorld(PixelWorldAtFocus()).c_str());
			if (ImGui::Button("Zoom x2")) { m_distance *= 0.5f; m_cameraDirty = true; }
			ImGui::SameLine();
			if (ImGui::Button("Zoom /2")) { m_distance *= 2.0f; m_cameraDirty = true; }
			ImGui::SameLine();
			if (ImGui::Button("Copy as job view")) {
				const XMFLOAT3 position = CameraPosition();
				ImGui::SetClipboardText(std::format(R"({{ "name": "view", "position": [{}, {}, {}], "look_at": [{}, {}, {}], "fov_degrees": {} }})",
					position.x, position.y, position.z, m_focus.x, m_focus.y, m_focus.z, m_fovDegrees).c_str());
			}
			ImGui::TextWrapped("Right drag orbits. Middle drag or WASD pans. Wheel or QE zooms. Tab hides the panels.");
		}

		if (ImGui::CollapsingHeader("LOD cut", ImGuiTreeNodeFlags_DefaultOpen)) {
			float threshold = m_threshold;
			if (ImGui::SliderFloat("Threshold", &threshold, 0.01f, 256.0f, "%.3f px", ImGuiSliderFlags_Logarithmic)) { SetThreshold(threshold); }
			float worldMm = WorldCut() * 1000.0f;
			if (ImGui::SliderFloat("World cut", &worldMm, 0.05f, 2000.0f, "%.3f mm", ImGuiSliderFlags_Logarithmic)) { SetWorldCut(worldMm / 1000.0f); }
			Help("World cut = threshold x world pixel at the focus. A group draws while its finest child's error is at or under the cut and its own error is over it.");
			if (ImGui::Button("/ sqrt2  [")) { SetThreshold(m_threshold / std::sqrt(2.0f)); }
			ImGui::SameLine();
			if (ImGui::Button("x sqrt2  ]")) { SetThreshold(m_threshold * std::sqrt(2.0f)); }
			ImGui::SameLine();
			if (ImGui::Button("1 px")) { SetThreshold(1.0f); }
			if (ImGui::Checkbox("Lock world cut while zooming", &m_lockWorldCut)) {
				m_lockedWorldCut = WorldCut();
				m_cutDirty = true;
			}
			Help("Holds the same groups on screen as you move closer, so you can zoom in on a cut chosen from further away. Exact in telephoto, close in perspective.");
			if (ImGui::Checkbox("Force leaves", &m_forceLeaf)) { m_cutDirty = true; }
			Help("A negative threshold. Pages may run past the pool, in which case parents draw instead: watch the resident count.");
			ImGui::SameLine();
			if (ImGui::Checkbox("Voxels only", &m_voxelOnly)) {
				br::extensions::MakeSettingSetter<bool>(CLodDisableNonVoxelVisibilitySettingName)(m_voxelOnly);
			}
			if (ImGui::Checkbox("Force depth", &m_forceDepth)) {
				br::extensions::MakeSettingSetter<uint32_t>(CLodForceTraversalDepthRootSettingName)(m_forceDepth ? static_cast<uint32_t>(m_forcedDepth) : CLodForceTraversalDepthRootDisabled);
			}
			Help("Starts traversal at one depth of the hierarchy, whatever the error says.");
			if (m_forceDepth) {
				ImGui::SameLine();
				ImGui::SetNextItemWidth(120.0f);
				if (ImGui::SliderInt("##depth", &m_forcedDepth, 0, std::max(m_hierarchy.Ready() ? m_hierarchy.MaxDepth() : 32, 1))) {
					br::extensions::MakeSettingSetter<uint32_t>(CLodForceTraversalDepthRootSettingName)(static_cast<uint32_t>(m_forcedDepth));
				}
			}
			ImGui::Separator();
			ImGui::Text("A/B flip  (key F)");
			float flipA = m_flipA * 1000.0f, flipB = m_flipB * 1000.0f;
			ImGui::SetNextItemWidth(110.0f);
			if (ImGui::DragFloat("A mm", &flipA, flipA * 0.01f, 0.01f, 5000.0f, "%.3f")) { m_flipA = flipA / 1000.0f; }
			ImGui::SameLine();
			ImGui::SetNextItemWidth(110.0f);
			if (ImGui::DragFloat("B mm", &flipB, flipB * 0.01f, 0.01f, 5000.0f, "%.3f")) { m_flipB = flipB / 1000.0f; }
			if (ImGui::Button("A = now")) { m_flipA = WorldCut(); }
			ImGui::SameLine();
			if (ImGui::Button("B = now")) { m_flipB = WorldCut(); }
			ImGui::SameLine();
			ImGui::Checkbox("Auto", &m_autoFlip);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(90.0f);
			ImGui::SliderFloat("Hz", &m_flipHertz, 0.25f, 8.0f, "%.2f");
		}

		if (ImGui::CollapsingHeader("Renderer debug view", ImGuiTreeNodeFlags_DefaultOpen)) {
			if (ImGui::BeginCombo("Output", OutputTypeNames[m_output].c_str())) {
				if (m_showAllOutputs) {
					for (unsigned int index = 0; index < OutputTypeNames.size(); ++index) {
						if (ImGui::Selectable(OutputTypeNames[index].c_str(), index == m_output)) { SetOutput(index); }
					}
				}
				else {
					for (const unsigned int index : kHierarchyOutputs) {
						if (ImGui::Selectable(OutputTypeNames[index].c_str(), index == m_output)) { SetOutput(index); }
					}
				}
				ImGui::EndCombo();
			}
			ImGui::Checkbox("List every output", &m_showAllOutputs);
			ImGui::SameLine();
			if (ImGui::Checkbox("Renderer menu", &m_rendererMenu)) { br::diagnostics::SetRendererMenuEnabled(m_rendererMenu); }
		}

		if (ImGui::CollapsingHeader("Hierarchy overlay", ImGuiTreeNodeFlags_DefaultOpen)) {
			if (ImGui::BeginCombo("Overlay", DescribeOverlay(m_overlay).name.data())) {
				for (int index = 0; index < static_cast<int>(OverlayMode::Count); ++index) {
					const auto mode = static_cast<OverlayMode>(index);
					if (ImGui::Selectable(DescribeOverlay(mode).name.data(), mode == m_overlay)) {
						m_overlay = mode;
						m_overlayDirty = true;
					}
				}
				ImGui::EndCombo();
			}
			if (ImGui::SliderFloat("Opacity", &m_overlayOpacity, 0.05f, 1.0f, "%.2f")) { m_overlayDirty = true; }
			if (m_overlay != OverlayMode::Off) {
				ImGui::TextWrapped("%s", DescribeOverlay(m_overlay).legend.data());
				if (m_overlay == OverlayMode::Depth || m_overlay == OverlayMode::VoxelLevel || m_overlay == OverlayMode::CellPixels || m_overlay == OverlayMode::Window || m_overlay == OverlayMode::NextSwitch || m_overlay == OverlayMode::HandOver) {
					DrawRampLegend(ImGui::GetContentRegionAvail().x);
				}
				if (m_overlay == OverlayMode::HandOver && m_hierarchy.Ready()) {
					ImGui::Text("Range: %s to %s", FormatWorld(m_hierarchy.MinHandOver() * m_worldScale).c_str(), FormatWorld(m_hierarchy.MaxHandOver() * m_worldScale).c_str());
				}
			}
			ImGui::TextWrapped("Left click selects. Hold Alt to read a pixel. O toggles the overlay.");
			ImGui::Checkbox("Live", &m_liveInspection);
			Help("Reads back visibility and clusters to name pixels. Each read copies both, so lower the rate if it slows the frame.");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(120.0f);
			ImGui::SliderFloat("Interval", &m_inspectIntervalSeconds, 0.05f, 2.0f, "%.2f s", ImGuiSliderFlags_Logarithmic);
		}

		if (ImGui::Button("Screenshot (F12)")) {
			static uint32_t count = 0u;
			CaptureScreenshot(std::filesystem::path(m_screenshotDirectory) / std::format("viewer_button_{:03}.png", ++count));
		}
		ImGui::End();
	}

	void Viewer::DrawMeshTable()
	{
		const std::vector<MeshInfo>& meshes = m_hierarchy.Meshes();
		// How much of the frame each mesh is drawing, which is the quickest way to tell the ones that matter.
		std::vector<uint32_t> pixelsPerMesh(meshes.size(), 0u);
		uint32_t hitPixels = 0u;
		if (const auto frame = CurrentFrame()) {
			hitPixels = frame->hitPixels;
			for (const PixelInfo& pixel : frame->pixels) {
				if (pixel.mesh >= 0) {
					++pixelsPerMesh[pixel.mesh];
				}
			}
		}
		if (!ImGui::BeginTable("meshes", 7, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable, ImVec2(0.0f, 140.0f * m_uiScale))) {
			return;
		}
		ImGui::TableSetupScrollFreeze(0, 1);
		const float unit = m_uiScale;
		ImGui::TableSetupColumn("Mesh", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_WidthFixed, 40.0f * unit);
		ImGui::TableSetupColumn("Tri", ImGuiTableColumnFlags_WidthFixed, 30.0f * unit);
		ImGui::TableSetupColumn("Vox", ImGuiTableColumnFlags_WidthFixed, 30.0f * unit);
		ImGui::TableSetupColumn("Leaf tris", ImGuiTableColumnFlags_WidthFixed, 50.0f * unit);
		ImGui::TableSetupColumn("Depth", ImGuiTableColumnFlags_WidthFixed, 30.0f * unit);
		ImGui::TableSetupColumn("Hand-over", ImGuiTableColumnFlags_WidthFixed, 110.0f * unit);
		ImGui::TableHeadersRow();
		for (int index = 0; index < static_cast<int>(meshes.size()); ++index) {
			const MeshInfo& mesh = meshes[index];
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::PushID(index);
			if (ImGui::Selectable(mesh.shortName.c_str(), m_tableMesh == index, ImGuiSelectableFlags_SpanAllColumns)) {
				m_tableMesh = index;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s\n%s", mesh.name.c_str(), mesh.cache->Prebuilt().cacheSource.sourceIdentifier.c_str());
			}
			ImGui::PopID();
			ImGui::TableNextColumn();
			if (pixelsPerMesh[index] != 0u && hitPixels != 0u) {
				ImGui::Text("%.1f%%", 100.0 * pixelsPerMesh[index] / hitPixels);
			}
			else {
				ImGui::TextDisabled("-");
			}
			ImGui::TableNextColumn();
			ImGui::Text("%u", mesh.triangleGroups);
			ImGui::TableNextColumn();
			ImGui::Text("%u", mesh.voxelGroups);
			ImGui::TableNextColumn();
			ImGui::Text("%llu", static_cast<unsigned long long>(mesh.leafTriangles));
			ImGui::TableNextColumn();
			ImGui::Text("%d", mesh.maxDepth);
			ImGui::TableNextColumn();
			if (!mesh.handOvers.empty()) {
				float low = kNoError, high = 0.0f;
				for (const HandOver& handOver : mesh.handOvers) {
					low = std::min(low, handOver.error);
					high = std::max(high, handOver.error);
				}
				ImGui::Text("%s - %s", FormatWorld(low * m_worldScale).c_str(), FormatWorld(high * m_worldScale).c_str());
			}
			else {
				ImGui::TextDisabled("none");
			}
		}
		ImGui::EndTable();
	}

	void Viewer::DrawLevelTable(const MeshInfo& mesh)
	{
		if (!ImGui::BeginTable("levels", 6, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV, ImVec2(0.0f, 180.0f))) {
			return;
		}
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Depth", ImGuiTableColumnFlags_WidthFixed, 40.0f);
		ImGui::TableSetupColumn("Tri / vox groups", ImGuiTableColumnFlags_WidthFixed, 90.0f);
		ImGui::TableSetupColumn("Tris", ImGuiTableColumnFlags_WidthFixed, 66.0f);
		ImGui::TableSetupColumn("Cells", ImGuiTableColumnFlags_WidthFixed, 66.0f);
		ImGui::TableSetupColumn("Leaves at cut", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Cell width", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableHeadersRow();
		const float cut = WorldCut();
		for (const LevelInfo& level : mesh.levels) {
			ImGui::TableNextRow();
			if (cut >= level.minError * m_worldScale && cut < level.maxError * m_worldScale) {
				ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(60, 80, 40, 255));
			}
			ImGui::TableNextColumn();
			ImGui::Text("%d", level.depth);
			ImGui::TableNextColumn();
			ImGui::Text("%u / %u", level.triangleGroups, level.voxelGroups);
			ImGui::TableNextColumn();
			ImGui::Text("%llu", static_cast<unsigned long long>(level.triangles));
			ImGui::TableNextColumn();
			ImGui::Text("%llu", static_cast<unsigned long long>(level.cells));
			ImGui::TableNextColumn();
			if (std::isfinite(level.minError)) {
				ImGui::PushID(level.depth);
				if (ImGui::SmallButton(std::format("{} - {}", FormatWorld(level.minError * m_worldScale), FormatWorld(level.maxError * m_worldScale)).c_str())) {
					SetWorldCut(std::sqrt(level.minError * level.maxError) * m_worldScale);
				}
				ImGui::PopID();
			}
			ImGui::TableNextColumn();
			if (std::isfinite(level.minWidth)) {
				ImGui::Text("%s - %s", FormatWorld(level.minWidth * m_worldScale).c_str(), FormatWorld(level.maxWidth * m_worldScale).c_str());
			}
		}
		ImGui::EndTable();
	}

	void Viewer::DrawHandOverPlot()
	{
		const std::vector<MeshInfo>& meshes = m_hierarchy.Meshes();
		if (!std::isfinite(m_hierarchy.MinHandOver())) {
			ImGui::TextDisabled("No mesh in this build hands over to voxels.");
			return;
		}
		static bool weighted = true;
		ImGui::Checkbox("Weight by triangles replaced", &weighted);
		const float low = m_hierarchy.MinHandOver() * m_worldScale * 0.5f, high = m_hierarchy.MaxHandOver() * m_worldScale * 2.0f;
		constexpr int kBins = 64;
		std::array<double, kBins> xs{}, parts{}, assemblies{};
		const double logLow = std::log(low), logSpan = std::log(high) - logLow;
		for (int bin = 0; bin < kBins; ++bin) {
			xs[bin] = std::exp(logLow + logSpan * (bin + 0.5) / kBins);
		}
		for (int meshIndex = 0; meshIndex < static_cast<int>(meshes.size()); ++meshIndex) {
			if (m_tableMesh >= 0 && meshIndex != m_tableMesh) {
				continue;
			}
			for (const HandOver& handOver : meshes[meshIndex].handOvers) {
				const int bin = std::clamp(static_cast<int>((std::log(handOver.error * m_worldScale) - logLow) / logSpan * kBins), 0, kBins - 1);
				(meshes[meshIndex].assembly ? assemblies : parts)[bin] += weighted ? static_cast<double>(handOver.replacedTriangles) : 1.0;
			}
		}
		if (ImPlot::BeginPlot("##handover", ImVec2(-1.0f, 130.0f * m_uiScale), ImPlotFlags_NoTitle)) {
			ImPlot::SetupAxes("World cut (m)", nullptr, ImPlotAxisFlags_None, ImPlotAxisFlags_AutoFit);
			ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
			ImPlot::SetupAxisLimits(ImAxis_X1, low, high, ImPlotCond_Once);
			ImPlot::PlotStems("Parts", xs.data(), parts.data(), kBins);
			ImPlot::PlotStems("Assemblies", xs.data(), assemblies.data(), kBins);
			double cut = WorldCut();
			if (ImPlot::DragLineX(0, &cut, ImVec4(1.0f, 1.0f, 0.2f, 1.0f))) {
				SetWorldCut(static_cast<float>(cut));
			}
			if (ImPlot::IsPlotHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
				SetWorldCut(static_cast<float>(ImPlot::GetPlotMousePos().x));
			}
			ImPlot::EndPlot();
		}
		ImGui::TextDisabled("Where each hierarchy changes from triangles to voxels. Click or drag to set the cut.");
	}

	void Viewer::DrawHierarchyPanel()
	{
		const ImVec2 display = ImGui::GetIO().DisplaySize;
		const float width = 420.0f * m_uiScale;
		ImGui::SetNextWindowPos(ImVec2(display.x - width - 8.0f, 8.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(width, display.y * 0.5f - 12.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowBgAlpha(0.88f);
		ImGui::Begin("Hierarchy");
		if (!m_hierarchy.Ready()) {
			ImGui::Text("Reading hierarchies: %u of %u", m_hierarchy.Loaded(), m_hierarchy.Total());
			ImGui::End();
			return;
		}
		DrawMeshTable();
		if (m_tableMesh >= 0) {
			if (ImGui::SmallButton("All meshes")) {
				m_tableMesh = -1;
			}
		}
		if (ImGui::CollapsingHeader("Hand-over map", ImGuiTreeNodeFlags_DefaultOpen)) {
			DrawHandOverPlot();
		}
		if (m_tableMesh >= 0 && ImGui::CollapsingHeader("Levels", ImGuiTreeNodeFlags_DefaultOpen)) {
			DrawLevelTable(m_hierarchy.Meshes()[m_tableMesh]);
		}
		// A merge root: what CLodEval merge-report prints for it, so the tree is readable next to the image.
		const int32_t reportMesh = m_tableMesh >= 0 ? m_tableMesh : m_selection.mesh;
		if (reportMesh >= 0 && !m_hierarchy.Meshes()[reportMesh].mergeReport.empty() && ImGui::CollapsingHeader("Merge tree", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::BeginChild("merge report", ImVec2(0.0f, 260.0f * m_uiScale), true, ImGuiWindowFlags_HorizontalScrollbar);
			ImGui::TextUnformatted(m_hierarchy.Meshes()[reportMesh].mergeReport.c_str());
			ImGui::EndChild();
		}
		ImGui::End();
	}

	void Viewer::DrawGroupChain(const MeshInfo& mesh)
	{
		// Up to the root by way of the coarsest-entering parent, so the table reads as one LOD chain from here.
		std::vector<uint32_t> chain;
		int32_t walker = m_selection.group;
		while (walker >= 0 && chain.size() < 64u) {
			chain.push_back(static_cast<uint32_t>(walker));
			const std::vector<uint32_t> parents = mesh.Parents(static_cast<uint32_t>(walker));
			walker = parents.empty() ? -1 : static_cast<int32_t>(*std::ranges::min_element(parents, {}, [&](uint32_t parent) { return mesh.groups[parent].depth; }));
		}
		if (!ImGui::BeginTable("chain", 6, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV, ImVec2(0.0f, 170.0f))) {
			return;
		}
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Group", ImGuiTableColumnFlags_WidthFixed, 56.0f);
		ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 44.0f);
		ImGui::TableSetupColumn("Enters at", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Leaves at", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Cell", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Set", ImGuiTableColumnFlags_WidthFixed, 34.0f);
		ImGui::TableHeadersRow();
		const float cut = WorldCut();
		bool previousVoxel = false;
		for (size_t index = 0; index < chain.size(); ++index) {
			const uint32_t groupIndex = chain[index];
			const GroupInfo& group = mesh.groups[groupIndex];
			ImGui::TableNextRow();
			const bool active = cut >= group.enterError * m_worldScale && cut < group.leaveError * m_worldScale;
			if (active) {
				ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(60, 80, 40, 255));
			}
			else if (index != 0u && group.voxel != previousVoxel) {
				ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(90, 50, 30, 255));
			}
			previousVoxel = group.voxel;
			ImGui::PushID(static_cast<int>(groupIndex));
			ImGui::TableNextColumn();
			if (ImGui::SmallButton(std::to_string(groupIndex).c_str())) {
				SelectGroup(m_selection.mesh, static_cast<int32_t>(groupIndex));
			}
			ImGui::TableNextColumn();
			ImGui::TextColored(group.voxel ? ImVec4(0.95f, 0.4f, 0.4f, 1.0f) : ImVec4(0.4f, 0.95f, 0.5f, 1.0f), group.voxel ? "vox %d" : "tri", group.voxelLevel);
			ImGui::TableNextColumn();
			ImGui::Text("%s", FormatWorld(group.enterError * m_worldScale).c_str());
			ImGui::TableNextColumn();
			ImGui::Text("%s", FormatWorld(group.leaveError * m_worldScale).c_str());
			ImGui::TableNextColumn();
			if (group.voxel) {
				ImGui::Text("%s", FormatWorld(group.voxelWidth * m_worldScale).c_str());
			}
			ImGui::TableNextColumn();
			if (ImGui::SmallButton("Cut")) {
				const float enter = group.enterError * m_worldScale, leave = group.leaveError * m_worldScale;
				SetWorldCut(enter > 0.0f && std::isfinite(leave) ? std::sqrt(enter * leave) : (enter > 0.0f ? enter * kAfterFactor : (std::isfinite(leave) ? leave * kBeforeFactor : WorldCut())));
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}

	void Viewer::DrawGroupPanel()
	{
		const ImVec2 display = ImGui::GetIO().DisplaySize;
		const float width = 420.0f * m_uiScale;
		ImGui::SetNextWindowPos(ImVec2(display.x - width - 8.0f, display.y * 0.5f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(width, display.y * 0.5f - 8.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowBgAlpha(0.88f);
		ImGui::Begin("Selected group");
		if (m_selection.mesh < 0 || m_selection.group < 0 || !m_hierarchy.Ready()) {
			ImGui::TextDisabled("Left click the image to select what drew that pixel.");
			ImGui::End();
			return;
		}
		const MeshInfo& mesh = m_hierarchy.Meshes()[m_selection.mesh];
		const GroupInfo& group = mesh.groups[m_selection.group];
		const float enter = group.enterError * m_worldScale, leave = group.leaveError * m_worldScale;
		ImGui::TextWrapped("%s", mesh.shortName.c_str());
		if (!mesh.portalUse.empty()) {
			ImGui::TextWrapped("A part, drawn through %s.", mesh.portalUse.c_str());
		}
		if (mesh.merge && group.mergeNode >= 0) {
			const clodeval::MergeNode& node = mesh.merge->nodes[static_cast<uint32_t>(group.mergeNode)];
			ImGui::Text("Merge node %d: level %u, %u nodes above, %zu child nodes, %zu portals", group.mergeNode, node.level, node.depth,
				node.childNodes.size(), node.childPortals.size());
			ImGui::Text("Opens its children below a cut of %s; radius %s", FormatWorld(node.error * m_worldScale).c_str(), FormatWorld(node.sphere.w * m_worldScale).c_str());
		}
		ImGui::Text("Group %d, depth %d of %d, %s", m_selection.group, group.depth, mesh.maxDepth, group.voxel ? std::format("voxel level {}", group.voxelLevel).c_str() : "triangles");
		ImGui::Text("Draws for a cut from %s %s", FormatWorld(enter).c_str(), std::isfinite(leave) ? std::format("to {}", FormatWorld(leave)).c_str() : "upward: it is a root");
		ImGui::Text("Hands over to voxels at %s", FormatWorld(group.firstVoxelError * m_worldScale).c_str());
		if (m_selectedDepth > 0.0f) {
			const float pixel = PixelWorldPerDepth() * m_selectedDepth;
			ImGui::Text("At the picked pixel's depth: threshold %.3f px %s", enter / pixel, std::isfinite(leave) ? std::format("to {:.3f} px", leave / pixel).c_str() : "and up");
			if (group.voxel) {
				ImGui::Text("Cell %.2f px at this zoom", group.voxelWidth * m_worldScale / pixel);
			}
		}
		if (group.voxel) {
			ImGui::Text("%u cubes, %u cells, cell width %s", group.cubes, group.cells, FormatWorld(group.voxelWidth * m_worldScale).c_str());
			if (group.cubes != 0u) {
				ImGui::Text("%.1f cells per cube of 64", static_cast<double>(group.cells) / group.cubes);
			}
		}
		else {
			ImGui::Text("%u triangles", group.triangles);
		}

		if (ImGui::Button("Before it appears") && enter > 0.0f) { SetWorldCut(enter * kBeforeFactor); }
		ImGui::SameLine();
		if (ImGui::Button("After it appears") && enter > 0.0f) { SetWorldCut(enter * kAfterFactor); }
		ImGui::SameLine();
		if (ImGui::Button("Before it leaves") && std::isfinite(leave)) { SetWorldCut(leave * kBeforeFactor); }
		ImGui::SameLine();
		if (ImGui::Button("After") && std::isfinite(leave)) { SetWorldCut(leave * kAfterFactor); }
		if (ImGui::Button("Flip across hand-over") && std::isfinite(group.firstVoxelError)) {
			const float boundary = group.firstVoxelError * m_worldScale;
			m_flipA = boundary * kBeforeFactor;
			m_flipB = boundary * kAfterFactor;
			m_autoFlip = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("Snapshot across hand-over") && std::isfinite(group.firstVoxelError) && !m_recording && !m_sweeping) {
			const float boundary = group.firstVoxelError * m_worldScale;
			SetWorldCut(boundary * kBeforeFactor);
			m_pendingAfter = 1;
			m_pendingCut = boundary * kAfterFactor;
			StartSnapshot(0);
		}

		if (ImGui::CollapsingHeader("LOD chain to the root", ImGuiTreeNodeFlags_DefaultOpen)) {
			DrawGroupChain(mesh);
			ImGui::TextDisabled("Green: drawing at this cut. Orange: the kind changes here.");
		}
		const std::vector<uint32_t> children = mesh.Children(static_cast<uint32_t>(m_selection.group));
		const std::vector<uint32_t> parents = mesh.Parents(static_cast<uint32_t>(m_selection.group));
		if (ImGui::CollapsingHeader(std::format("{} children, {} parents###links", children.size(), parents.size()).c_str())) {
			const auto buttons = [&](const char* label, const std::vector<uint32_t>& groups) {
				ImGui::TextUnformatted(label);
				for (const uint32_t other : groups) {
					ImGui::SameLine();
					ImGui::PushID(static_cast<int>(other));
					ImGui::PushStyleColor(ImGuiCol_Button, mesh.groups[other].voxel ? ImVec4(0.5f, 0.2f, 0.2f, 1.0f) : ImVec4(0.2f, 0.45f, 0.25f, 1.0f));
					if (ImGui::SmallButton(std::to_string(other).c_str())) {
						SelectGroup(m_selection.mesh, static_cast<int32_t>(other));
					}
					ImGui::PopStyleColor();
					ImGui::PopID();
					if (ImGui::GetCursorPosX() > ImGui::GetContentRegionAvail().x - 60.0f) {
						ImGui::NewLine();
					}
				}
			};
			buttons("Children:", children);
			buttons("Parents:", parents);
		}
		if (m_selectedCube && ImGui::CollapsingHeader("Voxel cube", ImGuiTreeNodeFlags_DefaultOpen)) {
			const CubeInfo& cube = *m_selectedCube;
			ImGui::Text("Cube %u of %u at %u, %u, %u of %u", m_selectedPrimitive, cube.clusterCubes, cube.coord[0], cube.coord[1], cube.coord[2], cube.resolution);
			ImGui::Text("Width %s, %d of 64 cells occupied, opacity sum %.2f", FormatWorld(cube.voxelWidth * m_worldScale).c_str(), std::popcount(cube.occupancy), cube.opacitySum);
			// The occupancy mask, slice by slice: index = x + 4y + 16z.
			const ImVec2 start = ImGui::GetCursorScreenPos();
			ImDrawList* list = ImGui::GetWindowDrawList();
			constexpr float kCell = 12.0f, kGap = 14.0f;
			for (uint32_t z = 0; z < 4u; ++z) {
				const float originX = start.x + z * (kCell * 4.0f + kGap);
				for (uint32_t y = 0; y < 4u; ++y) {
					for (uint32_t x = 0; x < 4u; ++x) {
						const bool occupied = ((cube.occupancy >> (x + 4u * y + 16u * z)) & 1u) != 0u;
						const ImVec2 corner(originX + x * kCell, start.y + (3u - y) * kCell);
						list->AddRectFilled(corner, ImVec2(corner.x + kCell - 1.0f, corner.y + kCell - 1.0f), occupied ? IM_COL32(230, 90, 80, 255) : IM_COL32(50, 50, 55, 255));
					}
				}
			}
			ImGui::Dummy(ImVec2(4.0f * (kCell * 4.0f + kGap), kCell * 4.0f + 2.0f));
			ImGui::TextDisabled("Slices z = 0 to 3. y runs up, x runs right.");
		}
		ImGui::End();
	}

	void Viewer::DrawTransitionPanel()
	{
		const ImVec2 display = ImGui::GetIO().DisplaySize;
		const float left = 350.0f * m_uiScale + 16.0f, right = 420.0f * m_uiScale + 16.0f;
		const float height = 250.0f * m_uiScale;
		ImGui::SetNextWindowPos(ImVec2(left, display.y - height - 8.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(std::max(display.x - left - right, 300.0f), height), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowBgAlpha(0.88f);
		ImGui::Begin("Transition");

		if (m_hierarchy.Ready() && m_sweepLow == 0.0005f && std::isfinite(m_hierarchy.MinHandOver())) {
			m_sweepLow = m_hierarchy.MinHandOver() * m_worldScale * 0.5f;
			m_sweepHigh = m_hierarchy.MaxHandOver() * m_worldScale * 2.0f;
		}
		if (ImGui::Button(m_sweeping ? "Sweeping..." : "Sweep cuts")) { StartSweep(); }
		Help("Steps the cut at this camera, waits for streaming to settle at each, and plots how many pixels are voxels. Use a telephoto view so one cut means one thing across the asset.");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(70.0f);
		int steps = static_cast<int>(m_sweepSteps);
		if (ImGui::DragInt("Steps", &steps, 1.0f, 2, 96)) { m_sweepSteps = static_cast<uint32_t>(steps); }
		ImGui::SameLine();
		float rangeMm[2] = { m_sweepLow * 1000.0f, m_sweepHigh * 1000.0f };
		ImGui::SetNextItemWidth(170.0f);
		if (ImGui::DragFloat2("mm range", rangeMm, 0.1f, 0.001f, 5000.0f, "%.2f")) {
			m_sweepLow = std::max(rangeMm[0], 0.001f) / 1000.0f;
			m_sweepHigh = std::max(rangeMm[1], rangeMm[0] * 1.01f) / 1000.0f;
		}

		if (ImPlot::BeginPlot("##curve", ImVec2(-1.0f, 130.0f * m_uiScale), ImPlotFlags_NoTitle)) {
			ImPlot::SetupAxes("World cut (m), against the fraction of hit pixels that are voxels", nullptr);
			ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
			ImPlot::SetupAxisLimits(ImAxis_X1, std::max(m_sweepLow * 0.5f, 1.0e-5f), m_sweepHigh * 2.0f, ImPlotCond_Once);
			ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 1.02, ImPlotCond_Once);
			const auto plot = [](const char* label, const std::vector<CurvePoint>& points) {
				if (points.empty()) {
					return;
				}
				std::vector<double> xs, ys;
				for (const CurvePoint& point : points) {
					xs.push_back(point.worldCut);
					ys.push_back(point.voxelFraction);
				}
				ImPlot::PlotLine(label, xs.data(), ys.data(), static_cast<int>(xs.size()));
			};
			plot("Captured", m_savedCurve);
			plot("This camera", m_liveCurve);
			double cut = WorldCut();
			if (ImPlot::DragLineX(1, &cut, ImVec4(1.0f, 1.0f, 0.2f, 1.0f))) {
				SetWorldCut(static_cast<float>(cut));
			}
			if (m_sweepAfter > 0.0f) {
				double before = m_sweepBefore, after = m_sweepAfter;
				ImPlot::PlotInfLines("Before", &before, 1);
				ImPlot::PlotInfLines("After", &after, 1);
			}
			if (ImPlot::IsPlotHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
				SetWorldCut(static_cast<float>(ImPlot::GetPlotMousePos().x));
			}
			ImPlot::EndPlot();
		}
		if (m_sweepAfter > 0.0f) {
			ImGui::Text("Last sweep: before %s, after %s", FormatWorld(m_sweepBefore).c_str(), FormatWorld(m_sweepAfter).c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton("Go before")) { SetWorldCut(m_sweepBefore); }
			ImGui::SameLine();
			if (ImGui::SmallButton("Go after")) { SetWorldCut(m_sweepAfter); }
		}

		ImGui::Separator();
		ImGui::SetNextItemWidth(120.0f);
		ImGui::SliderInt("Frames", &m_snapshotFrames, 1, 64);
		Help("Frames each snapshot averages. Voxel normals are stochastic with a period of 64 frames, so only 64 is converged.");
		ImGui::SameLine();
		const bool busy = m_recording != nullptr || m_sweeping;
		if (ImGui::Button("Snapshot A") && !busy) { StartSnapshot(0); }
		ImGui::SameLine();
		if (ImGui::Button("Snapshot B") && !busy) { StartSnapshot(1); }
		ImGui::SameLine();
		if (ImGui::Button("A and B from flip") && !busy) {
			SetWorldCut(m_flipA);
			m_pendingAfter = 1;
			m_pendingCut = m_flipB;
			StartSnapshot(0);
		}
		ImGui::SameLine();
		ImGui::Checkbox("Compare", &m_comparePanel);
		ImGui::End();
	}

	void Viewer::DrawComparePanel()
	{
		ImGui::SetNextWindowSize(ImVec2(1160.0f, 800.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
		ImGui::Begin("Compare", &m_comparePanel);
		const Snapshot& a = m_snapshots[0];
		const Snapshot& b = m_snapshots[1];
		if (!a.valid && !b.valid) {
			ImGui::TextDisabled("Take snapshots from the Transition window.");
			ImGui::End();
			return;
		}
		ImGui::Text("A: %s, %.3f px, %u frames, %.1f%% voxels", a.valid ? FormatWorld(a.worldCut).c_str() : "none", a.threshold, a.frames, a.valid && a.inspect ? a.inspect->VoxelFraction() * 100.0f : 0.0f);
		ImGui::SameLine(0.0f, 30.0f);
		ImGui::Text("B: %s, %.3f px, %u frames, %.1f%% voxels", b.valid ? FormatWorld(b.worldCut).c_str() : "none", b.threshold, b.frames, b.valid && b.inspect ? b.inspect->VoxelFraction() * 100.0f : 0.0f);

		static const char* kLayers[] = { "Lit", "Base colour", "Normal", "Coverage", "Path" };
		static const char* kLayouts[] = { "A, B, difference, paths", "Flip", "Wipe" };
		int layer = static_cast<int>(m_compareLayer), layout = static_cast<int>(m_compareLayout);
		ImGui::SetNextItemWidth(130.0f);
		if (ImGui::Combo("Layer", &layer, kLayers, IM_ARRAYSIZE(kLayers))) { m_compareLayer = static_cast<CompareLayer>(layer); m_compareDirty = true; }
		ImGui::SameLine();
		ImGui::SetNextItemWidth(180.0f);
		if (ImGui::Combo("Layout", &layout, kLayouts, IM_ARRAYSIZE(kLayouts))) { m_compareLayout = static_cast<CompareLayout>(layout); m_compareDirty = true; }
		ImGui::SameLine();
		ImGui::SetNextItemWidth(130.0f);
		if (ImGui::SliderFloat("Zoom", &m_compareZoom, 1.0f, 64.0f, "%.1f", ImGuiSliderFlags_Logarithmic)) { m_compareDirty = true; }
		ImGui::SameLine();
		ImGui::SetNextItemWidth(100.0f);
		if (ImGui::SliderFloat("Diff gain", &m_diffGain, 1.0f, 16.0f, "%.0f")) { m_compareDirty = true; }
		if (m_compareLayout == CompareLayout::Flip) {
			ImGui::SameLine();
			if (ImGui::Checkbox("Show B", &m_compareFlipB)) { m_compareDirty = true; }
			ImGui::SameLine();
			ImGui::Checkbox("Auto##compareflip", &m_compareAutoFlip);
			const auto now = std::chrono::steady_clock::now();
			if (m_compareAutoFlip && std::chrono::duration<float>(now - m_lastCompareFlip).count() > 0.5f) {
				m_lastCompareFlip = now;
				m_compareFlipB = !m_compareFlipB;
				m_compareDirty = true;
			}
		}
		else if (m_compareLayout == CompareLayout::Wipe) {
			ImGui::SameLine();
			ImGui::SetNextItemWidth(160.0f);
			if (ImGui::SliderFloat("Wipe", &m_compareWipe, 0.0f, 1.0f)) { m_compareDirty = true; }
		}

		if (m_compareStats.valid) {
			ImGui::Text("Mean change %.3f   pixels changed %.1f%%   coverage B/A %.2f   brightness B/A %.2f   normal change %.0f deg",
				m_compareStats.meanDelta, m_compareStats.changedPixels * 100.0, m_compareStats.coverageRatio, m_compareStats.brightnessRatio, m_compareStats.normalDegrees);
		}
		else if (a.valid && b.valid) {
			ImGui::TextDisabled("A and B were taken at different sizes, so they cannot be compared.");
		}
		else if (ImGui::Button("Compute stats")) {
			ComputeCompareStats();
		}

		BuildCompareTiles();
		const auto tile = [&](int index, const char* label) {
			if (!m_tiles[index].Valid()) {
				return;
			}
			ImGui::BeginGroup();
			ImGui::TextUnformatted(label);
			const ImVec2 size(static_cast<float>(m_tiles[index].Width()), static_cast<float>(m_tiles[index].Height()));
			const ImVec2 corner = ImGui::GetCursorScreenPos();
			ImGui::Image(m_tiles[index].Id(), size);
			if (ImGui::IsItemHovered()) {
				const ImGuiIO& io = ImGui::GetIO();
				if (io.MouseWheel != 0.0f) {
					m_compareZoom = std::clamp(m_compareZoom * std::pow(1.25f, io.MouseWheel), 1.0f, 64.0f);
					m_compareDirty = true;
				}
				if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
					m_compareCenter[0] -= io.MouseDelta.x / size.x / m_compareZoom;
					m_compareCenter[1] -= io.MouseDelta.y / size.y / m_compareZoom;
					m_compareCenter[0] = std::clamp(m_compareCenter[0], 0.0f, 1.0f);
					m_compareCenter[1] = std::clamp(m_compareCenter[1], 0.0f, 1.0f);
					m_compareDirty = true;
				}
				(void)corner;
			}
			ImGui::EndGroup();
		};
		if (m_compareLayout == CompareLayout::Grid) {
			tile(0, "A");
			ImGui::SameLine();
			tile(1, "B");
			tile(2, "Difference x gain");
			ImGui::SameLine();
			tile(3, "Paths: green triangles, red voxels, yellow switched to voxels, cyan switched back, magenta new, blue lost");
		}
		else {
			tile(0, m_compareLayout == CompareLayout::Flip ? (m_compareFlipB ? "B" : "A") : "A left, B right");
		}
		ImGui::TextDisabled("Wheel zooms and drag pans the images, without touching the camera.");
		ImGui::End();
	}

}
