#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rhi.h>

#include "Factories/TextureFactory.h"

namespace org { class PixelBuffer; }
using org::PixelBuffer;
namespace org { class Resource; }
using org::Resource;

// Owns final material-texture uploads and the one-time transition into the
// immutable shader-resource state.  Resources submitted here must never be
// declared to the render graph.
class MaterialTextureTransferService {
public:
	void Initialize();
	void Shutdown();

	void EnqueueUpload(
		const std::shared_ptr<PixelBuffer>& image,
		TextureDescription description,
		TextureFactory::TextureInitialData initialData);
	void EnsureShaderReady(const std::shared_ptr<PixelBuffer>& image);
	void RequestReadback(
		const std::shared_ptr<PixelBuffer>& image,
		std::wstring outputFile,
		std::function<void()> callback);

	// Called on the renderer thread once per frame.  Completed batches are
	// published first, then newly queued work is submitted as one graphics batch.
	void Pump();
	bool IsShaderReady(const std::shared_ptr<PixelBuffer>& image) const;
	bool HasFailed(const std::shared_ptr<PixelBuffer>& image) const;

private:
	enum class State : uint8_t { Pending, InFlight, Ready, Failed };
	struct Record {
		State state = State::Pending;
		uint64_t fenceValue = 0;
	};
	struct Request {
		std::shared_ptr<PixelBuffer> image;
		TextureDescription description;
		TextureFactory::TextureInitialData initialData;
		bool upload = false;
	};
	struct InFlightBatch {
		uint64_t fenceValue = 0;
		rhi::CommandAllocatorPtr allocator;
		rhi::CommandListPtr commandList;
		std::vector<rhi::ResourcePtr> stagingResources;
		std::vector<std::shared_ptr<PixelBuffer>> images;
		struct ReadbackCompletion {
			std::shared_ptr<Resource> buffer;
			std::vector<rhi::CopyableFootprint> footprints;
			uint32_t width = 0;
			uint32_t height = 0;
			uint32_t mipLevels = 0;
			rhi::Format format = rhi::Format::Unknown;
			std::wstring outputFile;
			std::function<void()> callback;
		};
		std::vector<ReadbackCompletion> readbacks;
	};
	struct ReadbackRequest {
		std::shared_ptr<PixelBuffer> image;
		std::wstring outputFile;
		std::function<void()> callback;
	};
	static void SaveReadbackToDds(InFlightBatch::ReadbackCompletion completion);

	void ReapCompletedLocked();
	static rhi::TextureBarrier MakeWholeTextureBarrier(
		const PixelBuffer& image,
		rhi::ResourceAccessType beforeAccess,
		rhi::ResourceAccessType afterAccess,
		rhi::ResourceLayout beforeLayout,
		rhi::ResourceLayout afterLayout,
		rhi::ResourceSyncState beforeSync,
		rhi::ResourceSyncState afterSync);

	mutable std::mutex m_mutex;
	rhi::Device m_device;
	rhi::Queue m_graphicsQueue;
	rhi::TimelinePtr m_timeline;
	uint64_t m_nextFenceValue = 0;
	std::vector<Request> m_pending;
	std::vector<ReadbackRequest> m_pendingReadbacks;
	std::vector<InFlightBatch> m_inFlight;
	std::unordered_map<uint64_t, Record> m_records;
	bool m_initialized = false;
};
