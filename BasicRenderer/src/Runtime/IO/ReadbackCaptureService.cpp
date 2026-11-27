#include "Runtime/IO/ReadbackCaptureService.h"

#include <atomic>
#include <cstring>
#include <stdexcept>

#include <spdlog/spdlog.h>

#include "OpenRenderGraph/ExternalClient.h"
#include "OpenRenderGraph/Runtime.h"
#include "Render/RenderGraph/RenderGraph.h"
#include "Render/RenderGraph/RenderGraphExtension.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "Resources/Resource.h"
#include "Runtime/Device/DeviceManager.h"

namespace br {

namespace {

std::atomic<ReadbackCaptureService*> g_service{nullptr};

} // namespace

ReadbackCaptureService* GetReadbackCaptureService() { return g_service.load(std::memory_order_acquire); }

struct FrameReadbackCaptures::Request {
	std::shared_ptr<org::Resource> resource; // Null for a request by identifier: the site's pass supplies the resource.
	ReadbackCaptureCallback callback;
	ReadbackCaptureResult result;
	rhi::ResourcePtr readback; // The copy's destination, made when the copy is recorded.
	std::shared_ptr<org::Resource> readbackResource; // Or a destination the requester made (AwaitCopy).
	uint64_t totalBytes = 0;
};

struct FrameReadbackCaptures::Site {
	std::string anchor;
	std::string passName;
	std::string identifier; // Set for a graph resource by identifier: each build looks it up in the graph being built.
	std::weak_ptr<org::Resource> resource; // Otherwise the resource itself.
	std::mutex mutex;
	std::vector<std::shared_ptr<Request>> pending;
	// Where a recording's copies go to wait for the frame's fence. Shared with the service.
	std::shared_ptr<std::mutex> recordedMutex;
	std::weak_ptr<std::vector<std::shared_ptr<Request>>> recorded;
};

namespace {

bool IsTexture(const org::Resource& resource) {
	return dynamic_cast<const org::PixelBuffer*>(&resource) != nullptr;
}

// Makes the readback buffer, fills the result's description and records the copy of `source` into it.
bool RecordCapture(rhi::CommandList& commands, rhi::ResourceHandle source, const org::Resource& resource,
	FrameReadbackCaptures::Request& request) {
	auto device = DeviceManager::GetInstance().GetDevice();
	auto& result = request.result;
	result.desc.resourceId = resource.GetGlobalResourceID();
	std::vector<rhi::CopyableFootprint> footprints;
	if (const auto* texture = dynamic_cast<const org::PixelBuffer*>(&resource)) {
		const uint32_t mips = (std::max)(1u, resource.GetMipLevels());
		const uint32_t slices = (std::max)(1u, resource.GetArraySize());
		footprints.resize(static_cast<size_t>(mips) * slices);
		rhi::FootprintRangeDesc range{};
		range.texture = source;
		range.mipCount = mips;
		range.arraySize = slices;
		range.planeCount = 1;
		const auto info = device.GetCopyableFootprints(range, footprints.data(), static_cast<uint32_t>(footprints.size()));
		request.totalBytes = info.totalBytes;
		result.desc.kind = ReadbackResourceKind::Texture;
		result.format = texture->GetFormat();
		result.width = texture->GetWidth();
		result.height = texture->GetHeight();
		result.depth = 1;
	} else {
		const auto* buffer = dynamic_cast<const org::Buffer*>(&resource);
		request.totalBytes = buffer ? buffer->GetSize() : 0;
		result.desc.kind = ReadbackResourceKind::Buffer;
	}
	if (request.totalBytes == 0) return false;

	rhi::ResourceDesc desc{};
	desc.type = rhi::ResourceType::Buffer;
	desc.heapType = rhi::HeapType::Readback;
	desc.buffer.sizeBytes = request.totalBytes;
	desc.debugName = "ReadbackCapture";
	if (rhi::Failed(device.CreateCommittedResource(desc, request.readback))) {
		spdlog::error("ReadbackCapture: could not create a {} byte readback buffer", request.totalBytes);
		return false;
	}
	const auto destination = request.readback->GetHandle();
	if (result.desc.kind == ReadbackResourceKind::Texture) {
		const uint32_t mips = (std::max)(1u, resource.GetMipLevels());
		for (size_t index = 0; index < footprints.size(); ++index) {
			rhi::BufferTextureCopyFootprint region{};
			region.texture = source;
			region.buffer = destination;
			region.mip = static_cast<uint32_t>(index % mips);
			region.arraySlice = static_cast<uint32_t>(index / mips);
			region.footprint = footprints[index];
			commands.CopyTextureToBuffer(region);
		}
	} else {
		commands.CopyBufferRegion(destination, 0, source, 0, request.totalBytes);
	}
	result.layouts = std::move(footprints);
	return true;
}

// Copies the resource after its anchor, for each request waiting on the site this frame.
struct CaptureBindings {
	org::ResourceBindingToken source{};
	std::shared_ptr<FrameReadbackCaptures::Site> site;
	std::shared_ptr<org::Resource> resource;
};

class ReadbackCapturePass : public org::TypedRenderGraphPass<ReadbackCapturePass, org::EmptyPassFrameData, CaptureBindings> {
public:
	ReadbackCapturePass(std::shared_ptr<FrameReadbackCaptures::Site> site, std::shared_ptr<org::Resource> resource)
		: m_site(std::move(site)), m_resource(std::move(resource)) {}

	CaptureBindings Declare(org::PassBuilder& builder) {
		return {builder.CopySource(m_resource), m_site, m_resource};
	}

	static void Record(const CaptureBindings& bindings, org::PassRecordContext& recording) {
		auto& site = *bindings.site;
		std::vector<std::shared_ptr<FrameReadbackCaptures::Request>> taken;
		{
			std::scoped_lock lock(site.mutex);
			taken.swap(site.pending);
		}
		if (taken.empty()) return;
		const auto source = recording.Resolve(bindings.source).GetHandle();
		std::vector<std::shared_ptr<FrameReadbackCaptures::Request>> recorded;
		for (auto& request : taken) {
			if (RecordCapture(recording.Commands(), source, *bindings.resource, *request)) recorded.push_back(std::move(request));
		}
		auto list = site.recorded.lock();
		if (!list || !site.recordedMutex) return;
		std::scoped_lock lock(*site.recordedMutex);
		for (auto& request : recorded) list->push_back(std::move(request));
	}

private:
	std::shared_ptr<FrameReadbackCaptures::Site> m_site;
	std::shared_ptr<org::Resource> m_resource;
};

} // namespace

class ReadbackCaptureExtension final : public org::IRenderGraphExtension {
public:
	explicit ReadbackCaptureExtension(FrameReadbackCaptures& owner) : m_owner(owner) {}

	void GatherStructuralPasses(org::RenderGraph& graph, std::vector<org::ExternalPassDesc>& out) override {
		std::vector<std::shared_ptr<FrameReadbackCaptures::Site>> sites;
		{
			std::scoped_lock lock(m_owner.m_mutex);
			for (auto it = m_owner.m_sites.begin(); it != m_owner.m_sites.end();) {
				if (it->second->identifier.empty() && it->second->resource.expired()) it = m_owner.m_sites.erase(it);
				else sites.push_back((it++)->second);
			}
		}
		for (const auto& site : sites) {
			auto resource = site->identifier.empty() ? site->resource.lock()
				: graph.RequestResourcePtr(org::ResourceIdentifier{site->identifier}, true);
			if (!resource) continue;
			out.push_back(org::ExternalPassDesc::Render(site->passName, std::make_shared<ReadbackCapturePass>(site, std::move(resource)))
				.At(org::ExternalInsertPoint::After(site->anchor)));
		}
	}

private:
	FrameReadbackCaptures& m_owner;
};

FrameReadbackCaptures::FrameReadbackCaptures()
	: m_recordedThisFrame(std::make_shared<std::vector<std::shared_ptr<Request>>>()) {
	g_service.store(this, std::memory_order_release);
}

FrameReadbackCaptures::~FrameReadbackCaptures() {
	ReadbackCaptureService* expected = this;
	g_service.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

namespace {
// The site recordings append to. One mutex for every site: recordings of different sites can run in parallel.
std::shared_ptr<std::mutex> RecordedMutex() {
	static const auto mutex = std::make_shared<std::mutex>();
	return mutex;
}
} // namespace

void FrameReadbackCaptures::RequestReadbackCapture(const std::string& passName, org::Resource* resource, const org::RangeSpec& range,
	ReadbackCaptureCallback callback, org::QueueKind) {
	if (!resource || !callback) return;
	auto request = std::make_shared<Request>();
	request->resource = resource->shared_from_this();
	request->callback = std::move(callback);
	request->result.desc.range = range;
	request->result.desc.kind = IsTexture(*resource) ? ReadbackResourceKind::Texture : ReadbackResourceKind::Buffer;
	request->result.desc.resourceId = resource->GetGlobalResourceID();

	std::scoped_lock lock(m_mutex);
	if (!m_accepting) return;
	if (passName == kReadbackAfterGraph) {
		m_afterGraph.push_back(std::move(request));
		return;
	}
	auto shared = request->resource;
	AddPendingLocked(passName, "#" + std::to_string(resource->GetGlobalResourceID()), {}, shared, std::move(request));
}

void FrameReadbackCaptures::RequestGraphResourceCapture(const std::string& passName, const std::string& resourceIdentifier,
	const org::RangeSpec& range, ReadbackCaptureCallback callback) {
	if (resourceIdentifier.empty() || !callback || passName == kReadbackAfterGraph) return;
	auto request = std::make_shared<Request>();
	request->callback = std::move(callback);
	request->result.desc.range = range;
	std::scoped_lock lock(m_mutex);
	if (!m_accepting) return;
	AddPendingLocked(passName, resourceIdentifier, resourceIdentifier, nullptr, std::move(request));
}

void FrameReadbackCaptures::AddPendingLocked(const std::string& passName, std::string key, std::string identifier,
	const std::shared_ptr<org::Resource>& resource, std::shared_ptr<Request> request) {
	auto& site = m_sites[std::make_pair(passName, key)];
	if (!site || (site->identifier.empty() && site->resource.expired())) {
		site = std::make_shared<Site>();
		site->anchor = passName;
		site->identifier = std::move(identifier);
		site->resource = resource;
		site->passName = "ReadbackCapture::" + passName + "::" + key;
		site->recordedMutex = RecordedMutex();
		site->recorded = m_recordedThisFrame;
		m_rebuildRequested = true;
		spdlog::info("ReadbackCapture: adding a copy of '{}' after '{}'; the frame graph rebuilds once",
			resource ? resource->GetName() : key, passName);
	}
	std::scoped_lock siteLock(site->mutex);
	site->pending.push_back(std::move(request));
}

std::unique_ptr<org::IRenderGraphExtension> FrameReadbackCaptures::CreateExtension() {
	return std::make_unique<ReadbackCaptureExtension>(*this);
}

bool FrameReadbackCaptures::ConsumeRebuildRequest() {
	std::scoped_lock lock(m_mutex);
	return std::exchange(m_rebuildRequested, false);
}

void FrameReadbackCaptures::SubmitAfterGraph(org::ExternalClient& client, uint32_t queueSlot) {
	std::vector<std::shared_ptr<Request>> requests;
	{
		std::scoped_lock lock(m_mutex);
		if (m_afterGraph.empty()) return;
		requests.swap(m_afterGraph);
	}
	std::vector<org::ExternalClient::Access> accesses;
	const org::ResourceState copySource{rhi::ResourceAccessType::CopySource, rhi::ResourceLayout::CopySource,
		rhi::ResourceSyncState::Copy};
	for (const auto& request : requests) accesses.push_back({request->resource.get(), {}, copySource});
	std::vector<std::shared_ptr<Request>> recorded;
	org::ExternalClient::Signal signal{};
	try {
		signal = client.Submit(queueSlot, accesses, [&](rhi::CommandList& list) {
			for (auto& request : requests) {
				if (RecordCapture(list, request->resource->GetAPIResource().GetHandle(), *request->resource, *request)) recorded.push_back(request);
			}
		}, {}, std::make_shared<std::vector<std::shared_ptr<Request>>>(requests), {}, true, [this, requests] {
			// Queued behind the submission thread and failed there: retried next frame, as a throw is. The service outlives the
			// graphs' clients, which wait for their queued submissions.
			spdlog::error("ReadbackCapture: a queued after-graph submission failed, retrying next frame");
			std::scoped_lock lock(m_mutex);
			if (m_accepting) for (const auto& request : requests) m_afterGraph.push_back(request);
		});
	} catch (const std::exception& error) {
		spdlog::error("ReadbackCapture: after-graph submission failed, retrying next frame: {}", error.what());
		std::scoped_lock lock(m_mutex);
		if (m_accepting) for (auto& request : requests) m_afterGraph.push_back(std::move(request));
		return;
	}
	std::scoped_lock lock(m_mutex);
	for (auto& request : recorded) m_inFlight.push_back({signal.timeline, signal.value, std::move(request), {}, signal.failed});
}

void FrameReadbackCaptures::NoteFrameSubmitted(rhi::Timeline frameFence, uint64_t value) {
	std::vector<std::shared_ptr<Request>> recorded;
	{
		std::scoped_lock recordedLock(*RecordedMutex());
		recorded.swap(*m_recordedThisFrame);
	}
	if (recorded.empty()) return;
	std::scoped_lock lock(m_mutex);
	for (auto& request : recorded) m_inFlight.push_back({frameFence, value, std::move(request)});
}

void FrameReadbackCaptures::ProcessCompleted() {
	std::vector<std::shared_ptr<Request>> completed;
	{
		std::scoped_lock lock(m_mutex);
		// After-graph copies signal the graph's external client timeline, which goes with the graph. A rebuild does not wait for
		// the GPU, so a copy can still be in flight when its graph retires. The runtime answers a point on a destroyed timeline
		// as reached (the client waited for it first), where the timeline itself would be a stale handle.
		auto* runtime = org::Runtime::Current();
		for (auto it = m_inFlight.begin(); it != m_inFlight.end();) {
			const bool reached = runtime ? runtime->Reached({it->timeline, it->value})
				: it->timeline.GetCompletedValue() >= it->value;
			if (reached && it->failed && it->failed->load(std::memory_order_acquire)) {
				it = m_inFlight.erase(it);  // queued again by its failure callback
			} else if (reached) {
				completed.push_back(std::move(it->request));
				it = m_inFlight.erase(it);
			} else {
				++it;
			}
		}
	}
	for (auto& request : completed) {
		auto& result = request->result;
		result.data.resize(request->totalBytes);
		void* mapped = nullptr;
		if (request->readbackResource) {
			auto api = request->readbackResource->GetAPIResource();
			api.Map(&mapped);
			if (mapped) {
				std::memcpy(result.data.data(), mapped, request->totalBytes);
				api.Unmap(0, 0);
			}
			request->readbackResource.reset();
		} else if (request->readback) {
			request->readback->Map(&mapped);
			if (mapped) {
				std::memcpy(result.data.data(), mapped, request->totalBytes);
				request->readback->Unmap(0, 0);
			}
			request->readback.Reset();
		}
		try {
			request->callback(std::move(result));
		} catch (const std::exception& error) {
			spdlog::error("ReadbackCapture: a callback threw: {}", error.what());
		}
	}
}

void FrameReadbackCaptures::AwaitCopy(std::shared_ptr<rhi::TimelinePtr> timeline, uint64_t value, std::shared_ptr<org::Resource> readback,
	uint64_t totalBytes, ReadbackCaptureResult result, ReadbackCaptureCallback callback) {
	if (!timeline || !readback || !callback) return;
	auto request = std::make_shared<Request>();
	request->resource = readback;
	request->readbackResource = std::move(readback);
	request->totalBytes = totalBytes;
	request->result = std::move(result);
	request->callback = std::move(callback);
	std::scoped_lock lock(m_mutex);
	if (!m_accepting) return;
	const auto handle = timeline->Get();
	m_inFlight.push_back({handle, value, std::move(request), std::move(timeline)});
}

void FrameReadbackCaptures::Shutdown() {
	std::scoped_lock lock(m_mutex);
	m_accepting = false;
	m_afterGraph.clear();
	m_inFlight.clear();
	for (auto& [key, site] : m_sites) {
		std::scoped_lock siteLock(site->mutex);
		site->pending.clear();
	}
	m_sites.clear();
	std::scoped_lock recordedLock(*RecordedMutex());
	m_recordedThisFrame->clear();
}

} // namespace br
