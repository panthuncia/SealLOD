#pragma once

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

private:
	mutable std::mutex m_mutex;
	Pending m_pending;
	uint64_t m_topologyRequests = 0;
};

} // namespace br::render
