// CLodCapture: renders the cameras and render conditions in a job file and writes a bundle of images
// and counters. It knows nothing about metrics or camera rigs: CLodEval generates the job and
// scores the bundle, so changing either never touches this program. See docs/clod-evaluation.md.

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <DirectXMath.h>
#include <dxgiformat.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <BasicRenderer/Assets/Geometry/MeshInstance.h>
#include <BasicRenderer/Assets/Import/CLodCache.h>
#include <BasicRenderer/Scene/Components.h>
#include <BasicRenderer/Extensions/BuiltinResources.h>
#include <BasicRenderer/Extensions/SettingAccess.h>
#include <BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h>
#include <BasicRenderer/Pipeline/PipelineRecipe.h>
#include <BasicRenderer/Pipeline/UpscalingTypes.h>
#include <BasicRenderer/Renderer.h>
#include <BasicRenderer/Scene/Scene.h>
#include <BasicRenderer/StaticImport/ImportedAssetLibrary.h>
#include <BasicRenderer/StaticImport/StaticImportRuntime.h>

#include "CaptureShared.h"
#include "Viewer.h"
#include "Render/RenderGraph/RenderGraph.h"
#include "Render/Runtime/IReadbackService.h"
#include "Render/Runtime/IStatisticsService.h"
#include "Resources/ReadbackRequest.h"
#include "Runtime/Settings/SettingsManager.h"

// Same Agility SDK and GPU preferences as the demo.
extern "C" {
_declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
__declspec(dllexport) extern const UINT D3D12SDKVersion = 722;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D\\";
}

namespace {
using namespace clodcapture;

constexpr int kExitOk = 0;
constexpr int kExitFailed = 3;

// Written last, so a bundle.json on disk means the bundle is complete.
constexpr int kBundleVersion = 1;

// A named render condition. Adding one needs no code unless it uses a new setting.
struct JobProbe {
	std::string name;
	float lodErrorPixels = 1.0f;
	uint32_t accumulateFrames = 1u;
	std::vector<std::string> layers;
	nlohmann::json settings = nlohmann::json::object();
};

struct Job {
	std::vector<JobAsset> assets;
	uint32_t width = 1280u;
	uint32_t height = 720u;
	std::string environment = "sky";
	XMFLOAT3 lightDirection{ -0.1f, -0.5f, -0.9f };
	float lightIntensity = 10.0f;
	std::vector<JobView> views;
	std::vector<JobProbe> probes;
	fs::path output;
	bool cacheOnly = true;
	// Streaming only starts once feedback from a rendered frame comes back, so being idle for a few frames
	// proves nothing. This matches the 3 idle seconds the sampling harness waits for.
	double settleSeconds = 3.0;
	std::string pipeline = "minimal";  // "minimal" or "sarp"
	float frameSeconds = 0.0f;
	bool deterministic = true;
	bool sceneRenderOverlap = false;
	bool holdBindPose = true;
	double readyTimeoutSeconds = 300.0;
	std::string partFilter;
};

XMFLOAT3 ReadVector(const nlohmann::json& json)
{
	return { json.at(0).get<float>(), json.at(1).get<float>(), json.at(2).get<float>() };
}

Job LoadJob(const fs::path& path)
{
	std::ifstream input(path);
	if (!input) {
		throw std::runtime_error(std::format("cannot open job '{}'", path.string()));
	}
	nlohmann::json json;
	input >> json;

	Job job;
	for (const nlohmann::json& asset : json.at("assets")) {
		job.assets.push_back({ asset.at("path").get<std::string>(), asset.value("scale", 1.0f) });
	}
	job.width = json.at("resolution").at(0).get<uint32_t>();
	job.height = json.at("resolution").at(1).get<uint32_t>();
	job.environment = json.value("environment", job.environment);
	if (const auto light = json.find("light"); light != json.end()) {
		job.lightDirection = ReadVector(light->at("direction"));
		job.lightIntensity = light->value("intensity", job.lightIntensity);
	}
	for (const nlohmann::json& view : json.at("views")) {
		JobView parsed;
		parsed.name = view.at("name").get<std::string>();
		parsed.position = ReadVector(view.at("position"));
		parsed.lookAt = ReadVector(view.at("look_at"));
		if (view.contains("up")) {
			parsed.up = ReadVector(view.at("up"));
		}
		parsed.fovDegrees = view.value("fov_degrees", parsed.fovDegrees);
		parsed.zNear = view.value("near", parsed.zNear);
		parsed.zFar = view.value("far", parsed.zFar);
		job.views.push_back(std::move(parsed));
	}
	for (const nlohmann::json& probe : json.at("probes")) {
		JobProbe parsed;
		parsed.name = probe.at("name").get<std::string>();
		parsed.lodErrorPixels = probe.value("lod_error_pixels", parsed.lodErrorPixels);
		parsed.accumulateFrames = probe.value("accumulate_frames", parsed.accumulateFrames);
		parsed.layers = probe.at("layers").get<std::vector<std::string>>();
		parsed.settings = probe.value("settings", nlohmann::json::object());
		job.probes.push_back(std::move(parsed));
	}
	job.output = json.at("output").get<std::string>();
	job.cacheOnly = json.value("cache_only", job.cacheOnly);
	job.settleSeconds = json.value("settle_seconds", job.settleSeconds);
	job.pipeline = json.value("pipeline", job.pipeline);
	job.frameSeconds = json.value("frame_seconds", job.frameSeconds);
	job.deterministic = json.value("deterministic", job.deterministic);
	job.sceneRenderOverlap = json.value("scene_render_overlap", job.sceneRenderOverlap);
	job.holdBindPose = json.value("hold_bind_pose", job.holdBindPose);
	job.readyTimeoutSeconds = json.value("ready_timeout_seconds", job.readyTimeoutSeconds);
	job.partFilter = json.value("part_filter", job.partFilter);
	if (job.assets.empty() || job.views.empty() || job.probes.empty()) {
		throw std::runtime_error("a job needs assets, views and probes");
	}
	for (const JobView& view : job.views) {
		if (view.fovDegrees != job.views.front().fovDegrees || view.zNear != job.views.front().zNear || view.zFar != job.views.front().zFar) {
			throw std::runtime_error("every view in a job must share one field of view, near plane and far plane");
		}
	}
	return job;
}

struct FormatInfo {
	DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
	uint32_t bytesPerPixel = 0;
};

FormatInfo DescribeFormat(rhi::Format format)
{
	switch (format) {
	case rhi::Format::R32G32_UInt: return { DXGI_FORMAT_R32G32_UINT, 8u };
	case rhi::Format::R8G8B8A8_UNorm: return { DXGI_FORMAT_R8G8B8A8_UNORM, 4u };
	case rhi::Format::R16G16B16A16_Float: return { DXGI_FORMAT_R16G16B16A16_FLOAT, 8u };
	case rhi::Format::R16G16_Float: return { DXGI_FORMAT_R16G16_FLOAT, 4u };
	default: throw std::runtime_error("captured layer has an unsupported format");
	}
}

void WriteDds(const fs::path& path, uint32_t width, uint32_t height, DXGI_FORMAT format, uint32_t pitch, const std::byte* pixels, size_t sourcePitch)
{
	constexpr uint32_t kCaps = 0x1, kHeight = 0x2, kWidth = 0x4, kPitch = 0x8, kPixelFormat = 0x1000, kMipCount = 0x20000;
	constexpr uint32_t kFourCc = 0x4, kCapsTexture = 0x1000, kDimensionTexture2D = 3;
	std::array<uint32_t, 31> header{};
	header[0] = 124;
	header[1] = kCaps | kHeight | kWidth | kPitch | kPixelFormat | kMipCount;
	header[2] = height;
	header[3] = width;
	header[4] = pitch;
	header[6] = 1;
	header[18] = 32;
	header[19] = kFourCc;
	header[20] = 0x30315844;  // "DX10"
	header[26] = kCapsTexture;
	const std::array<uint32_t, 5> dx10{ static_cast<uint32_t>(format), kDimensionTexture2D, 0, 1, 0 };

	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	output.write("DDS ", 4);
	output.write(reinterpret_cast<const char*>(header.data()), sizeof(header));
	output.write(reinterpret_cast<const char*>(dx10.data()), sizeof(dx10));
	for (uint32_t row = 0; row < height; ++row) {
		output.write(reinterpret_cast<const char*>(pixels + sourcePitch * row), pitch);
	}
	output.flush();
	if (!output) {
		throw std::runtime_error(std::format("failed to write '{}'", path.string()));
	}
}

// Collects readbacks for one view under one probe. Callbacks arrive on another thread.
class ProbeCapture {
public:
	ProbeCapture(fs::path directory, const JobProbe& probe)
		: m_directory(std::move(directory)), m_accumulateFrames(probe.accumulateFrames) {
		fs::create_directories(m_directory);
	}

	void RequestSingleLayer(org::RenderGraph& graph, const LayerDefinition& layer)
	{
		auto resource = graph.RequestResourcePtr(org::ResourceIdentifier{ std::string(layer.resource) }, true);
		if (!resource) {
			throw std::runtime_error(std::format("the render graph has no resource {}", layer.resource));
		}
		m_outstanding.fetch_add(1u);
		graph.GetReadbackService()->RequestReadbackCapture(std::string(layer.anchorPass), resource.get(), org::RangeSpec{},
			[this, name = std::string(layer.name)](org::ReadbackCaptureResult&& result) {
				try {
					WriteLayer(name, result);
				}
				catch (const std::exception& error) {
					Fail(error.what());
				}
				m_outstanding.fetch_sub(1u);
			});
	}

	void RequestAccumulatedFrame(org::RenderGraph& graph)
	{
		RequestCoverageFrame(graph);
		const LayerDefinition& layer = FindLayer("hdr");
		auto resource = graph.RequestResourcePtr(org::ResourceIdentifier{ std::string(layer.resource) }, true);
		if (!resource) {
			throw std::runtime_error(std::format("the render graph has no resource {}", layer.resource));
		}
		m_outstanding.fetch_add(1u);
		graph.GetReadbackService()->RequestReadbackCapture(std::string(layer.anchorPass), resource.get(), org::RangeSpec{},
			[this](org::ReadbackCaptureResult&& result) {
				try {
					Accumulate(result);
				}
				catch (const std::exception& error) {
					Fail(error.what());
				}
				m_outstanding.fetch_sub(1u);
			});
	}

	bool Done() const { return m_outstanding.load() == 0u; }
	uint32_t Outstanding() const { return m_outstanding.load(); }

	// Call once Done. Writes the accumulated mean and per-pixel variance.
	nlohmann::json Finish()
	{
		std::scoped_lock lock(m_mutex);
		if (!m_error.empty()) {
			throw std::runtime_error(m_error);
		}
		if (m_accumulatedFrames != 0u) {
			if (m_accumulatedFrames != m_accumulateFrames) {
				throw std::runtime_error(std::format("accumulated {} of {} frames", m_accumulatedFrames, m_accumulateFrames));
			}
			WriteAccumulation();
		}
		return m_layers;
	}

private:
	void Fail(const std::string& message)
	{
		std::scoped_lock lock(m_mutex);
		if (m_error.empty()) {
			m_error = message;
		}
	}

	// Stochastic voxel opacity makes coverage fractional over a sequence, which one frame's visibility cannot show.
	void RequestCoverageFrame(org::RenderGraph& graph)
	{
		const LayerDefinition& layer = FindLayer("visibility");
		auto resource = graph.RequestResourcePtr(org::ResourceIdentifier{ std::string(layer.resource) }, true);
		if (!resource) {
			throw std::runtime_error(std::format("the render graph has no resource {}", layer.resource));
		}
		m_outstanding.fetch_add(1u);
		graph.GetReadbackService()->RequestReadbackCapture(std::string(layer.anchorPass), resource.get(), org::RangeSpec{},
			[this](org::ReadbackCaptureResult&& result) {
				try {
					AccumulateCoverage(result);
				}
				catch (const std::exception& error) {
					Fail(error.what());
				}
				m_outstanding.fetch_sub(1u);
			});
	}

	void AccumulateCoverage(const org::ReadbackCaptureResult& result)
	{
		if (result.format != rhi::Format::R32G32_UInt || result.layouts.empty()) {
			throw std::runtime_error("coverage expects an R32G32_UInt visibility target");
		}
		const auto& layout = result.layouts.front();
		const auto rowPitch = static_cast<size_t>(layout.rowPitch);
		const auto offset = static_cast<size_t>(layout.offset);
		if (offset + rowPitch * (result.height - 1u) + result.width * 8u > result.data.size()) {
			throw std::runtime_error("coverage readback footprint does not match its data");
		}
		std::scoped_lock lock(m_mutex);
		if (m_coverageFrames == 0u) {
			m_coverageWidth = result.width;
			m_coverageHeight = result.height;
			m_coverage.assign(static_cast<size_t>(result.width) * result.height, 0u);
		}
		for (uint32_t y = 0; y < result.height; ++y) {
			const std::byte* row = result.data.data() + offset + rowPitch * y;
			for (uint32_t x = 0; x < result.width; ++x) {
				uint64_t key = 0;
				std::memcpy(&key, row + static_cast<size_t>(x) * 8u, sizeof(key));
				if (key != ~uint64_t{ 0 }) {
					++m_coverage[static_cast<size_t>(y) * result.width + x];
				}
			}
		}
		++m_coverageFrames;
	}

	void WriteLayer(const std::string& name, const org::ReadbackCaptureResult& result)
	{
		const FormatInfo format = DescribeFormat(result.format);
		if (result.layouts.empty()) {
			throw std::runtime_error("texture readback has no footprint");
		}
		const auto& layout = result.layouts.front();
		const auto pitch = static_cast<size_t>(result.width) * format.bytesPerPixel;
		const auto rowPitch = static_cast<size_t>(layout.rowPitch);
		const auto offset = static_cast<size_t>(layout.offset);
		// The footprint's total size does not pad the last row, so that row only has `pitch` bytes.
		if (result.height == 0u || rowPitch < pitch || offset + rowPitch * (result.height - 1u) + pitch > result.data.size()) {
			throw std::runtime_error("texture readback footprint does not match its data");
		}
		const std::string file = name + ".dds";
		WriteDds(m_directory / file, result.width, result.height, format.dxgi, static_cast<uint32_t>(pitch), result.data.data() + offset, rowPitch);
		std::scoped_lock lock(m_mutex);
		m_layers.push_back({ { "name", name }, { "file", file }, { "dxgi_format", static_cast<uint32_t>(format.dxgi) }, { "width", result.width }, { "height", result.height } });
	}

	void Accumulate(const org::ReadbackCaptureResult& result)
	{
		if (result.format != rhi::Format::R16G16B16A16_Float || result.layouts.empty()) {
			throw std::runtime_error("accumulation expects an R16G16B16A16_Float target");
		}
		const auto& layout = result.layouts.front();
		const auto rowPitch = static_cast<size_t>(layout.rowPitch);
		const auto offset = static_cast<size_t>(layout.offset);
		if (offset + rowPitch * (result.height - 1u) + result.width * 8u > result.data.size()) {
			throw std::runtime_error("accumulated readback footprint does not match its data");
		}
		std::scoped_lock lock(m_mutex);
		const size_t pixels = static_cast<size_t>(result.width) * result.height;
		if (m_accumulatedFrames == 0u) {
			m_width = result.width;
			m_height = result.height;
			m_sum.assign(pixels * 3u, 0.0);
			m_sumSquaredLuminance.assign(pixels, 0.0);
			m_sumLuminance.assign(pixels, 0.0);
		}
		else if (m_width != result.width || m_height != result.height) {
			throw std::runtime_error("resolution changed while accumulating");
		}
		for (uint32_t y = 0; y < result.height; ++y) {
			const std::byte* row = result.data.data() + offset + rowPitch * y;
			for (uint32_t x = 0; x < result.width; ++x) {
				uint16_t texel[4];
				std::memcpy(texel, row + static_cast<size_t>(x) * 8u, sizeof(texel));
				const size_t index = static_cast<size_t>(y) * result.width + x;
				const double r = HalfToFloat(texel[0]);
				const double g = HalfToFloat(texel[1]);
				const double b = HalfToFloat(texel[2]);
				m_sum[index * 3u] += r;
				m_sum[index * 3u + 1u] += g;
				m_sum[index * 3u + 2u] += b;
				const double luminance = 0.2126 * r + 0.7152 * g + 0.0722 * b;
				m_sumLuminance[index] += luminance;
				m_sumSquaredLuminance[index] += luminance * luminance;
			}
		}
		++m_accumulatedFrames;
	}

	void WriteAccumulation()
	{
		const double count = static_cast<double>(m_accumulatedFrames);
		const size_t pixels = static_cast<size_t>(m_width) * m_height;
		std::vector<float> mean(pixels * 4u);
		std::vector<float> variance(pixels);
		for (size_t index = 0; index < pixels; ++index) {
			mean[index * 4u] = static_cast<float>(m_sum[index * 3u] / count);
			mean[index * 4u + 1u] = static_cast<float>(m_sum[index * 3u + 1u] / count);
			mean[index * 4u + 2u] = static_cast<float>(m_sum[index * 3u + 2u] / count);
			mean[index * 4u + 3u] = 1.0f;
			const double meanLuminance = m_sumLuminance[index] / count;
			variance[index] = static_cast<float>(std::max(m_sumSquaredLuminance[index] / count - meanLuminance * meanLuminance, 0.0));
		}
		const uint32_t meanPitch = m_width * 16u;
		const uint32_t variancePitch = m_width * 4u;
		WriteDds(m_directory / "hdr-mean.dds", m_width, m_height, DXGI_FORMAT_R32G32B32A32_FLOAT, meanPitch, reinterpret_cast<const std::byte*>(mean.data()), meanPitch);
		WriteDds(m_directory / "hdr-luminance-variance.dds", m_width, m_height, DXGI_FORMAT_R32_FLOAT, variancePitch, reinterpret_cast<const std::byte*>(variance.data()), variancePitch);
		m_layers.push_back({ { "name", "hdr-mean" }, { "file", "hdr-mean.dds" }, { "dxgi_format", DXGI_FORMAT_R32G32B32A32_FLOAT }, { "width", m_width }, { "height", m_height }, { "frames", m_accumulatedFrames } });
		m_layers.push_back({ { "name", "hdr-luminance-variance" }, { "file", "hdr-luminance-variance.dds" }, { "dxgi_format", DXGI_FORMAT_R32_FLOAT }, { "width", m_width }, { "height", m_height }, { "frames", m_accumulatedFrames } });

		std::vector<float> coverage(m_coverage.size());
		for (size_t index = 0; index < coverage.size(); ++index) {
			coverage[index] = static_cast<float>(m_coverage[index]) / static_cast<float>(m_coverageFrames);
		}
		const uint32_t coveragePitch = m_coverageWidth * 4u;
		WriteDds(m_directory / "coverage-mean.dds", m_coverageWidth, m_coverageHeight, DXGI_FORMAT_R32_FLOAT, coveragePitch, reinterpret_cast<const std::byte*>(coverage.data()), coveragePitch);
		m_layers.push_back({ { "name", "coverage-mean" }, { "file", "coverage-mean.dds" }, { "dxgi_format", DXGI_FORMAT_R32_FLOAT }, { "width", m_coverageWidth }, { "height", m_coverageHeight }, { "frames", m_coverageFrames } });
	}

	fs::path m_directory;
	uint32_t m_accumulateFrames = 1u;
	std::atomic<uint32_t> m_outstanding{ 0u };
	std::mutex m_mutex;
	std::string m_error;
	nlohmann::json m_layers = nlohmann::json::array();
	uint32_t m_accumulatedFrames = 0u;
	uint32_t m_width = 0u;
	uint32_t m_height = 0u;
	std::vector<double> m_sum;
	std::vector<double> m_sumLuminance;
	std::vector<double> m_sumSquaredLuminance;
	uint32_t m_coverageFrames = 0u;
	uint32_t m_coverageWidth = 0u;
	uint32_t m_coverageHeight = 0u;
	std::vector<uint32_t> m_coverage;
};

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_DESTROY) {
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(window, message, wParam, lParam);
}

// Never shown: rendering works without it, and a capture should not steal focus or depend on the monitor.
HWND CreateHiddenWindow(uint32_t width, uint32_t height)
{
	WNDCLASSW windowClass{};
	windowClass.lpfnWndProc = WindowProcedure;
	windowClass.hInstance = GetModuleHandleW(nullptr);
	windowClass.lpszClassName = L"CLodCaptureWindow";
	if (!RegisterClassW(&windowClass)) {
		throw std::runtime_error("could not register the window class");
	}
	HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"CLodCapture", WS_POPUP, 0, 0, static_cast<int>(width), static_cast<int>(height),
		nullptr, nullptr, windowClass.hInstance, nullptr);
	if (window == nullptr) {
		throw std::runtime_error("could not create the window");
	}
	RECT client{};
	GetClientRect(window, &client);
	if (static_cast<uint32_t>(client.right - client.left) != width || static_cast<uint32_t>(client.bottom - client.top) != height) {
		DestroyWindow(window);
		throw std::runtime_error(std::format("window is {}x{}, not the requested {}x{}", client.right - client.left, client.bottom - client.top, width, height));
	}
	return window;
}

int RunJob(const Job& job)
{
	CLodCache::SetCacheOnlyMode(job.cacheOnly);
	SetDllDirectoryA(".\\D3D\\");
	fs::create_directories(job.output);

	HWND window = CreateHiddenWindow(job.width, job.height);
	// Deleted only on success. A failed run exits straight away, because unwinding would destroy the
	// renderer mid-frame and turn the error into a crash with the wrong exit code.
	Renderer& renderer = *new Renderer();
	renderer.Initialize(window, job.width, job.height, job.pipeline == "sarp" ? br::pipeline::MakeSarpPipeline() : MakeCapturePipeline());
	SettingsManager::GetInstance().getSettingSetter<bool>("enableSceneRenderOverlap")(job.sceneRenderOverlap);
	SettingsManager::GetInstance().getSettingSetter<UpscalingMode>("upscalingMode")(UpscalingMode::None);
	renderer.SetInputMode(InputMode::wasd);

	CaptureScene* scene = nullptr;
	{
		org::BufferBase::ScopedBackingMutation startupBackingMutation;
		renderer.SetCurrentScene(std::make_shared<Scene>());
		scene = new CaptureScene();
		scene->SetHoldBindPose(job.holdBindPose);
		scene->SetPartFilter(job.partFilter);
		for (const JobAsset& asset : job.assets) {
			scene->AddAsset(asset);
		}
		renderer.SetEnvironment(job.environment);
		const JobView& first = job.views.front();
		const float aspect = static_cast<float>(job.width) / static_cast<float>(job.height);
		renderer.GetCurrentScene()->SetCamera(first.position, first.lookAt, first.up, first.fovDegrees * (XM_PI / 180.0f), aspect, first.zNear, first.zFar);
		renderer.GetCurrentScene()->CreateDirectionalLightECS(L"capture-light", XMFLOAT3(1.0f, 1.0f, 1.0f), job.lightIntensity, job.lightDirection, false);
	}

	const auto pumpAndRender = [&]() {
		MSG message{};
		while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
		scene->Tick(renderer);
		renderer.Update(job.frameSeconds);
		renderer.Render();
	};

	nlohmann::json bundleViews = nlohmann::json::array();
	const auto startTime = std::chrono::steady_clock::now();

	// Waits until the scene is published and has been idle for settleSeconds, with the same residency throughout.
	const auto waitForReady = [&](const std::string& what) {
		const auto waitStart = std::chrono::steady_clock::now();
		auto stableSince = std::chrono::steady_clock::now();
		bool wasStable = false;
		uint32_t lastResident = 0u;
		uint64_t lastResidentBytes = 0u;
		uint64_t frames = 0u;
		while (!wasStable || std::chrono::duration<double>(std::chrono::steady_clock::now() - stableSince).count() < job.settleSeconds) {
			pumpAndRender();
			++frames;
			if (scene->AllPlaced() && scene->PlacedParts() == 0u && scene->SkinnedParts() == 0u) {
				throw std::runtime_error("the assets have no renderable parts");
			}
			const auto snapshot = renderer.GetSamplingReadinessSnapshot(false);
			const bool stable = scene->Published() && IsIdle(snapshot) && snapshot.residentClodGroups == lastResident && snapshot.residentClodAllocationBytes == lastResidentBytes;
			if (stable && !wasStable) {
				stableSince = std::chrono::steady_clock::now();
			}
			wasStable = stable;
			lastResident = snapshot.residentClodGroups;
			lastResidentBytes = snapshot.residentClodAllocationBytes;
			const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - waitStart).count();
			if (frames % 300u == 0u) {
				spdlog::info("{}: waiting, frame {} {:.1f}s {} rendered_frames={} {}", what, frames, seconds, scene->DescribePlacement(), renderer.GetTotalFramesRendered(), DescribeReadiness(snapshot));
			}
			if (seconds > job.readyTimeoutSeconds) {
				throw std::runtime_error(std::format("{} was not ready after {:.0f}s: {} {}",
					what, seconds, scene->DescribePlacement(), DescribeReadiness(snapshot)));
			}
			if (job.cacheOnly && CLodCache::GetCacheOnlyViolationCount() != 0u) {
				throw std::runtime_error("the cache is missing a mesh this scene needs: see the critical message in the log");
			}
		}
		spdlog::info("{}: ready after {} frames, {:.1f}s", what, frames, std::chrono::duration<double>(std::chrono::steady_clock::now() - waitStart).count());
	};

	waitForReady("scene");
	renderer.SetDeterministicSamplingMode(job.deterministic);

	for (const JobView& view : job.views) {
		MoveCamera(renderer, view);
		nlohmann::json bundleProbes = nlohmann::json::array();
		for (const JobProbe& probe : job.probes) {
			br::extensions::MakeSettingSetter<float>(CLodLodErrorPixelsSettingName)(probe.lodErrorPixels);
			ApplySettings(probe.settings);
			renderer.SetDeterministicSamplingMode(false);
			waitForReady(view.name + "/" + probe.name);
			renderer.SetDeterministicSamplingMode(job.deterministic);

			ProbeCapture capture(job.output / view.name / probe.name, probe);
			org::RenderGraph* graph = renderer.GetRenderGraph();
			if (graph == nullptr) {
				throw std::runtime_error("the renderer has no render graph");
			}
			const uint64_t firstFrame = renderer.GetTotalFramesRendered();
			for (const std::string& layer : probe.layers) {
				capture.RequestSingleLayer(*graph, FindLayer(layer));
			}
			for (uint32_t frame = 0; frame < probe.accumulateFrames; ++frame) {
				capture.RequestAccumulatedFrame(*graph);
				pumpAndRender();
			}
			const auto waitStart = std::chrono::steady_clock::now();
			while (!capture.Done()) {
				pumpAndRender();
				if (std::chrono::steady_clock::now() - waitStart > std::chrono::seconds(60)) {
					throw std::runtime_error(std::format("{} readbacks outstanding after 60 s: is the anchor pass running?", capture.Outstanding()));
				}
			}
			// Timestamps from the last frame sampled. They are indicative: a readback adds little to a pass but it is not zero.
			nlohmann::json passMilliseconds = nlohmann::json::object();
			if (const auto* statistics = graph->GetStatisticsService()) {
				const auto& names = statistics->GetPassNames();
				const auto& stats = statistics->GetPassStats();
				for (size_t index = 0; index < names.size() && index < stats.size(); ++index) {
					if (stats[index].gpuSampleSerial != 0u) {
						passMilliseconds[names[index]] = stats[index].gpuTimeEma;
					}
				}
			}
			bundleProbes.push_back({
				{ "name", probe.name },
				{ "pass_ms", std::move(passMilliseconds) },
				{ "lod_error_pixels", probe.lodErrorPixels },
				{ "first_frame", firstFrame },
				{ "settings", probe.settings },
				{ "readiness", ReadinessJson(renderer.GetSamplingReadinessSnapshot(false)) },
				{ "layers", capture.Finish() },
			});
			spdlog::info("captured {}/{}", view.name, probe.name);
		}
		nlohmann::json camera;
		camera["position"] = { view.position.x, view.position.y, view.position.z };
		camera["look_at"] = { view.lookAt.x, view.lookAt.y, view.lookAt.z };
		camera["up"] = { view.up.x, view.up.y, view.up.z };
		camera["fov_degrees"] = view.fovDegrees;
		camera["near"] = view.zNear;
		camera["far"] = view.zFar;
		bundleViews.push_back({ { "name", view.name }, { "camera", std::move(camera) }, { "probes", std::move(bundleProbes) } });
	}

	const auto readiness = renderer.GetSamplingReadinessSnapshot(false);
	const nlohmann::json bundle{
		{ "bundle_version", kBundleVersion },
		{ "resolution", { job.width, job.height } },
		{ "cache_only", job.cacheOnly },
		{ "cache_only_violations", CLodCache::GetCacheOnlyViolationCount() },
		{ "seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count() },
		{ "residency", {
			{ "resident_clod_groups", readiness.residentClodGroups },
			{ "resident_clod_allocation_bytes", readiness.residentClodAllocationBytes },
			{ "total_clod_streamed_bytes", readiness.totalClodStreamedBytes },
		} },
		{ "static_parts", scene->PlacedParts() },
		{ "skinned_parts", scene->SkinnedParts() },
		{ "views", std::move(bundleViews) },
	};
	{
		const fs::path temporary = job.output / "bundle.json.tmp";
		std::ofstream output(temporary, std::ios::trunc);
		output << bundle.dump(2) << '\n';
		output.flush();
		if (!output) {
			throw std::runtime_error("failed to write bundle.json");
		}
	}
	fs::rename(job.output / "bundle.json.tmp", job.output / "bundle.json");

	delete scene;
	renderer.Cleanup();
	delete &renderer;
	DestroyWindow(window);
	return kExitOk;
}
}

int main(int argc, char** argv)
{
	try {
		std::string jobPath, viewPath, scriptPath;
		for (int index = 1; index + 1 < argc; ++index) {
			if (std::string_view(argv[index]) == "--job") {
				jobPath = argv[index + 1];
			}
			else if (std::string_view(argv[index]) == "--view") {
				viewPath = argv[index + 1];
			}
			else if (std::string_view(argv[index]) == "--script") {
				scriptPath = argv[index + 1];
			}
		}
		if (!viewPath.empty()) {
			spdlog::set_level(spdlog::level::info);
			return clodcapture::RunViewer(viewPath, scriptPath);
		}
		if (jobPath.empty()) {
			std::fprintf(stderr, "usage: CLodCapture --job job.json | --view viewer.json [--script steps.json]\nRun from the directory holding shaders/ and D3D/.\n");
			return kExitFailed;
		}
		const Job job = LoadJob(jobPath);
		spdlog::set_level(spdlog::level::info);
		return RunJob(job);
	}
	catch (const std::exception& error) {
		spdlog::critical("{}", error.what());
		std::fprintf(stderr, "error: %s\n", error.what());
		std::fflush(nullptr);
		// A failed run may still hold a live renderer, whose teardown is not what the caller asked about.
		// Skipping it keeps the exit code the documented one.
		std::_Exit(kExitFailed);
	}
}
