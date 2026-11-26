#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace br::render {

// What a change asks of the frame graph (docs/frame_graph_composition_plan.md, "Revision model").
//
// Today only Topology does anything: it rebuilds the frame graph at the renderer's consolidation point. The other classes
// are declared so that changes can be classified as they are moved off Topology.
enum class RevisionClass : uint8_t {
	Topology,  // the graph's passes or resources change: rebuild the epochs named
	Resource,  // a versioned resource gets a new version: re-record the epochs that bind it
	Pipeline,  // a pipeline gets a new build: re-record the epochs that bind it
	Record,    // a value baked into a recording changes: re-record
	Latched,   // a value written each run: nothing to do
};

// The one way anything asks for graph work. Requests can come from any thread (settings observers, pipe commands,
// capacity growth). The renderer consumes them once a frame, on the render thread, at a single consolidation point.
class FrameRevisions {
public:
	static constexpr uint32_t kAllEpochs = ~0u;

	// Any thread. Asks for a rebuild of the epochs in epochMask, for the reason given. Repeated reasons are counted.
	void RequestTopology(std::string_view reason, uint32_t epochMask = kAllEpochs) {
		std::scoped_lock lock(m_mutex);
		m_pending.topologyEpochs |= epochMask;
		++m_pending.reasons[std::string(reason)];
		++m_topologyRequests;
	}

	struct Pending {
		uint32_t topologyEpochs = 0;
		std::map<std::string, uint32_t> reasons;  // each reason with its count
		bool Empty() const noexcept { return topologyEpochs == 0; }
		// Adds other's epochs and reasons (a build that answers both).
		void Merge(Pending other) {
			topologyEpochs |= other.topologyEpochs;
			for (auto& [reason, count] : other.reasons) reasons[reason] += count;
		}
		// "a, b (x3)": for logs.
		std::string Describe() const {
			std::string text;
			for (const auto& [reason, count] : reasons) {
				if (!text.empty()) text += ", ";
				text += reason.empty() ? std::string("unnamed") : reason;
				if (count > 1) text += " (x" + std::to_string(count) + ")";
			}
			return text;
		}
	};

	// Any thread: whether a topology change is waiting.
	bool TopologyPending() const {
		std::scoped_lock lock(m_mutex);
		return m_pending.topologyEpochs != 0;
	}
	// Any thread: a copy of what is waiting, for diagnostics.
	Pending Peek() const {
		std::scoped_lock lock(m_mutex);
		return m_pending;
	}
	// Render thread, at the consolidation point: takes everything requested so far.
	Pending Consume() {
		std::scoped_lock lock(m_mutex);
		Pending taken = std::move(m_pending);
		m_pending = {};
		return taken;
	}
	// Render thread: puts back what Consume took, when the change could not be made yet. Merges with newer requests.
	void Restore(Pending pending) {
		if (pending.Empty()) return;
		std::scoped_lock lock(m_mutex);
		m_pending.topologyEpochs |= pending.topologyEpochs;
		for (auto& [reason, count] : pending.reasons) m_pending.reasons[reason] += count;
	}
	// Totals since the renderer started, for telemetry.
	uint64_t TopologyRequests() const {
		std::scoped_lock lock(m_mutex);
		return m_topologyRequests;
	}

	// The resolution revision (M5 step 4): the latest output size the window asked for, and whether the upscaler's
	// configuration is to be asked again (its mode, quality or guides changed, or it retries Ray Reconstruction). Writers only
	// record it; the renderer consumes it once a frame at the consolidation point, and not while a graph build is in flight
	// (one resolution build at a time). Later requests replace earlier ones: the latest size wins.
	struct ResolutionRequest {
		bool outputSize = false;  // a window size was asked for
		uint32_t width = 0;
		uint32_t height = 0;
		bool recreateSwapchain = false;  // a present reported a mode change: the swapchain is made again even at the same size
		bool upscaler = false;  // the upscaler configuration is asked again
		uint32_t requests = 0;  // requests recorded since the last consumption
		std::map<std::string, uint32_t> reasons;
		bool Empty() const noexcept { return !outputSize && !upscaler; }
		std::string Describe() const {
			std::string text;
			for (const auto& [reason, count] : reasons) {
				if (!text.empty()) text += ", ";
				text += reason;
				if (count > 1) text += " (x" + std::to_string(count) + ")";
			}
			return text;
		}
	};
	// Any thread. The window's client size, as WM_SIZE, a window preset or a present's mode change report it.
	void RequestOutputSize(uint32_t width, uint32_t height, std::string_view reason, bool recreateSwapchain = false) {
		std::scoped_lock lock(m_mutex);
		m_resolution.outputSize = true;
		m_resolution.width = width;
		m_resolution.height = height;
		m_resolution.recreateSwapchain |= recreateSwapchain;
		// The settle rule waits for the size to stop changing: a present that reports a mode change every frame at the size the
		// window kept does not hold it off (M5 step 9).
		if (!m_sizeRequested || width != m_lastRequestedWidth || height != m_lastRequestedHeight)
			m_lastOutputSizeRequest = std::chrono::steady_clock::now();
		m_sizeRequested = true;
		m_lastRequestedWidth = width;
		m_lastRequestedHeight = height;
		++m_resolution.requests;
		++m_resolution.reasons[std::string(reason)];
		++m_resolutionRequests;
	}
	// Any thread. The upscaler settings changed: the next consumption prepares the configuration they ask for.
	void RequestUpscaler(std::string_view reason) {
		std::scoped_lock lock(m_mutex);
		m_resolution.upscaler = true;
		++m_resolution.requests;
		++m_resolution.reasons[std::string(reason)];
		++m_resolutionRequests;
	}
	bool ResolutionPending() const {
		std::scoped_lock lock(m_mutex);
		return !m_resolution.Empty();
	}
	ResolutionRequest PeekResolution() const {
		std::scoped_lock lock(m_mutex);
		return m_resolution;
	}
	// Render thread, at the consolidation point.
	ResolutionRequest ConsumeResolution() {
		std::scoped_lock lock(m_mutex);
		ResolutionRequest taken = std::move(m_resolution);
		m_resolution = {};
		return taken;
	}
	// When the requested window size last changed (RequestOutputSize). A drag's sizes come a few ms apart.
	std::chrono::steady_clock::time_point LastOutputSizeRequest() const {
		std::scoped_lock lock(m_mutex);
		return m_lastOutputSizeRequest;
	}
	uint64_t ResolutionRequests() const {
		std::scoped_lock lock(m_mutex);
		return m_resolutionRequests;
	}

private:
	mutable std::mutex m_mutex;
	Pending m_pending;
	uint64_t m_topologyRequests = 0;
	ResolutionRequest m_resolution;
	uint64_t m_resolutionRequests = 0;
	std::chrono::steady_clock::time_point m_lastOutputSizeRequest{};
	bool m_sizeRequested = false;
	uint32_t m_lastRequestedWidth = 0;
	uint32_t m_lastRequestedHeight = 0;
};

} // namespace br::render
