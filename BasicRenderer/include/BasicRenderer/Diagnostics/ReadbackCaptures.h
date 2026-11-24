#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <rhi.h>

#include "Render/QueueKind.h"
#include "Resources/ResourceStateTracker.h"

namespace org { class Resource; }

// Copies of a resource's contents to the host, for diagnostics, telemetry and frame captures. ORG's capture service went
// away (its RunReadback replays with kept recordings); BasicRenderer keeps this one for one-off and periodic reads.
//
// Two kinds of capture:
// - After a named pass: the contents as that pass left them. The first request for a (pass, resource) pair adds a copy
//   pass after the anchor, which rebuilds the frame graph once; later requests for the pair reuse it. A request whose
//   anchor never runs is never answered, so callers that can lose their anchor need a timeout.
// - After the graph (kReadbackAfterGraph): the end-of-frame contents, copied after the frame's submission without
//   touching the graph. Periodic telemetry should use this. A resource the graph aliases has no end-of-frame contents.
//
// Callbacks run on the render thread, after the copy's frame completed on the GPU, and may request again.
namespace br {

enum class ReadbackResourceKind : uint8_t {
	Buffer,
	Texture
};

struct ReadbackCaptureDesc {
	ReadbackResourceKind kind = ReadbackResourceKind::Buffer;
	uint64_t resourceId = 0;
	org::RangeSpec range;
};

struct ReadbackCaptureResult {
	ReadbackCaptureDesc desc;
	// A texture's subresources in the data, mip-major within each slice (D3D12 subresource order).
	std::vector<rhi::CopyableFootprint> layouts;
	std::vector<std::byte> data;
	rhi::Format format = rhi::Format::Unknown;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t depth = 0;
};

using ReadbackCaptureCallback = std::function<void(ReadbackCaptureResult&&)>;

// The anchor name of an after-graph capture.
inline constexpr const char* kReadbackAfterGraph = "__rg_end__";

class ReadbackCaptureService {
public:
	virtual ~ReadbackCaptureService() = default;
	// Any thread. resource must stay alive until the callback (requests hold a reference).
	virtual void RequestReadbackCapture(const std::string& passName, org::Resource* resource, const org::RangeSpec& range,
		ReadbackCaptureCallback callback, org::QueueKind preferredQueueKind = org::QueueKind::Graphics) = 0;
	// A graph resource by its identifier, after a named pass. The copy reads whatever the identifier names in the graph that
	// runs, so the capture survives the rebuild that its new site causes (a rebuild makes new graph-owned resources, and
	// a request holding the old resource would copy memory the new graph aliases elsewhere). Prefer this for graph resources.
	virtual void RequestGraphResourceCapture(const std::string& passName, const std::string& resourceIdentifier, const org::RangeSpec& range,
		ReadbackCaptureCallback callback) = 0;
	void RequestReadbackCaptureAfterGraph(org::Resource* resource, const org::RangeSpec& range, ReadbackCaptureCallback callback,
		org::QueueKind preferredQueueKind = org::QueueKind::Graphics) {
		RequestReadbackCapture(kReadbackAfterGraph, resource, range, std::move(callback), preferredQueueKind);
	}
};

// The renderer's capture service. Null before the renderer initialized and after it shut down.
ReadbackCaptureService* GetReadbackCaptureService();

} // namespace br
