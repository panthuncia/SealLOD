#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rhi.h>

#include <BasicRenderer/Diagnostics/ReadbackCaptures.h>

namespace org {
class ExternalClient;
class Resource;
struct IRenderGraphExtension;
}

namespace br {

// The renderer's ReadbackCaptureService (include/BasicRenderer/Diagnostics/ReadbackCaptures.h).
//
// After-pass captures go through capture sites: one copy pass per (anchor pass, resource), placed after the anchor by the
// extension this service makes. A request for a pair with no site adds one and asks the renderer for a graph rebuild
// (ConsumeRebuildRequest). Each frame's recording of a site's pass copies into the readback buffers of the requests waiting
// on it, which complete with that frame's fence. After-graph captures are submitted through the graph's external client
// once the frame graph is submitted (SubmitAfterGraph).
class FrameReadbackCaptures final : public ReadbackCaptureService {
public:
	FrameReadbackCaptures();
	~FrameReadbackCaptures() override;

	void RequestReadbackCapture(const std::string& passName, org::Resource* resource, const org::RangeSpec& range,
		ReadbackCaptureCallback callback, org::QueueKind preferredQueueKind = org::QueueKind::Graphics) override;
	void RequestGraphResourceCapture(const std::string& passName, const std::string& resourceIdentifier, const org::RangeSpec& range,
		ReadbackCaptureCallback callback) override;

	// The graph extension that places the capture sites. One per graph; it reads the sites at each build.
	std::unique_ptr<org::IRenderGraphExtension> CreateExtension();
	// Render thread, once a frame: true once after a request added a site the graph does not have yet.
	bool ConsumeRebuildRequest();
	// Render thread, after the frame graph was submitted: copies the after-graph requests through the graph's client.
	void SubmitAfterGraph(org::ExternalClient& client, uint32_t queueSlot);
	// Render thread, after the frame's fence signal was queued: the copies recorded in this frame complete at frameFence.
	void NoteFrameSubmitted(rhi::Timeline frameFence, uint64_t value);
	// Render thread: answers the requests whose copies completed. Callbacks run here, without the lock held.
	void ProcessCompleted();
	// Any thread: a copy recorded elsewhere into `readback` (a readback-heap buffer) completes when `timeline` reaches value;
	// then `result` gets the buffer's first totalBytes and goes to callback, as a capture's would.
	void AwaitCopy(std::shared_ptr<rhi::TimelinePtr> timeline, uint64_t value, std::shared_ptr<org::Resource> readback,
		uint64_t totalBytes, ReadbackCaptureResult result, ReadbackCaptureCallback callback);
	// Drops everything waiting. Callbacks are not called.
	void Shutdown();

	struct Request;
	struct Site;

private:
	struct Completion {
		rhi::Timeline timeline;
		uint64_t value = 0;
		std::shared_ptr<Request> request;
		std::shared_ptr<rhi::TimelinePtr> timelineOwner;
		// An after-graph copy queued behind the submission thread that failed there (ExternalClient::Signal::failed): dropped
		// when reached, since its failure callback queued the request again.
		std::shared_ptr<const std::atomic<bool>> failed;
	};

	std::mutex m_mutex;
	bool m_accepting = true;
	bool m_rebuildRequested = false;
	// Sites by (anchor, resource): "#<global id>" for a resource, the identifier for a graph resource by identifier. The
	// extension reads them at each build.
	std::map<std::pair<std::string, std::string>, std::shared_ptr<Site>> m_sites;
	void AddPendingLocked(const std::string& passName, std::string key, std::string identifier, const std::shared_ptr<org::Resource>& resource,
		std::shared_ptr<Request> request);
	std::vector<std::shared_ptr<Request>> m_afterGraph;
	// Copied by a site's recording this frame, waiting for the frame's fence value.
	std::shared_ptr<std::vector<std::shared_ptr<Request>>> m_recordedThisFrame;
	std::deque<Completion> m_inFlight;

	friend class ReadbackCaptureExtension;
	friend struct Site;
};

} // namespace br
