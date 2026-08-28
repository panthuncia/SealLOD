#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>

#include <rhi.h>

#include "Factories/TextureFactory.h"
#include "Managers/Singletons/TaskSchedulerManager.h"

namespace org { class PixelBuffer; }
using org::PixelBuffer;
namespace org { class Resource; }
using org::Resource;
namespace br::render {
struct GpuSubmissionSet;
struct TextureTransferArtifact {
	std::shared_ptr<PixelBuffer> image;
	std::uint64_t generation = 0;
	std::shared_ptr<const GpuSubmissionSet> gpuSubmissions;
};
}

// Owns final material-texture uploads and the one-time transition into the
// immutable shader-resource state.  Resources submitted here must never be
// declared to the render graph.
class MaterialTextureTransferService {
public:
	void Initialize();
	void Shutdown();

	std::shared_ptr<const br::render::TextureTransferArtifact> EnqueueUpload(
		const std::shared_ptr<PixelBuffer>& image,
		TextureDescription description,
		TextureFactory::TextureInitialData initialData);
	std::shared_ptr<const br::render::TextureTransferArtifact> EnsureShaderReady(
		const std::shared_ptr<PixelBuffer>& image);
	void RequestReadback(
		const std::shared_ptr<PixelBuffer>& image,
		std::wstring outputFile,
		std::function<void()> callback);

	// O(1) render-thread kick. Recording, submission, and completion publication
	// run in the service task scope.
	void Pump();
	bool IsShaderReady(const std::shared_ptr<PixelBuffer>& image) const;
	std::shared_ptr<const br::render::GpuSubmissionSet> ShaderReadySubmission(
		const std::shared_ptr<PixelBuffer>& image) const;
	bool HasFailed(const std::shared_ptr<PixelBuffer>& image) const;

private:
	enum class State : uint8_t { Pending, InFlight, Ready, Failed };
	struct TransferState {
		std::atomic<State> state{ State::Pending };
		std::atomic_uint64_t fenceValue{ 0 };
		std::mutex callbackMutex;
		std::vector<std::function<void()>> callbacks;
		std::string error;
	};
	struct Record {
		State state = State::Pending;
		uint64_t fenceValue = 0;
		std::shared_ptr<TransferState> transferState;
		std::shared_ptr<const br::render::TextureTransferArtifact> artifact;
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
			uint64_t bufferSize = 0;
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
	std::shared_ptr<const br::render::TextureTransferArtifact> EnsureTransferRecordLocked(
		const std::shared_ptr<PixelBuffer>& image);
	static void PublishTransferState(const std::shared_ptr<TransferState>& transfer,
		State state, std::uint64_t fenceValue = 0, std::string error = {});

	void ReapCompletedLocked();
	void PumpWorker();
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
	std::shared_ptr<rhi::TimelinePtr> m_timeline;
	uint64_t m_nextFenceValue = 0;
	std::vector<Request> m_pending;
	std::vector<ReadbackRequest> m_pendingReadbacks;
	std::vector<InFlightBatch> m_inFlight;
	std::unordered_map<uint64_t, Record> m_records;
	br::TaskScope m_taskScope;
	std::atomic_bool m_pumpScheduled{ false };
	std::atomic_uint64_t m_workGeneration{ 0 };
	std::atomic_bool m_shuttingDown{ false };
	bool m_initialized = false;
};
