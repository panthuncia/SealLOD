#pragma once

#include <memory>

namespace org { class ExternalClient; }

// Narrow frame-graph boundary for texture-streaming feedback. The service owns
// suppression, the readback slots, and their completion handling.
class ITextureStreamingFeedbackService {
public:
	virtual ~ITextureStreamingFeedbackService() = default;
	// Copies the feedback buffer into a free readback slot, as one submission on `queueSlot` of `client`.
	// Call it after the frame graph's execution, so the copy reads what the frame wrote.
	virtual void SubmitTextureStreamingFeedbackReadback(org::ExternalClient& client, uint32_t queueSlot) = 0;
};
