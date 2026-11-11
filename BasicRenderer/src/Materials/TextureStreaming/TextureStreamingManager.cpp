#include "Materials/TextureStreaming/TextureStreamingManager.h"
#include "OpenRenderGraph/ExternalClient.h"
#include "Render/PassBuilders.h"

#include "../generated/BuiltinResources.h"
#include "Assets/Textures/TextureFactory.h"
#include "Materials/TextureStreaming/MaterialTextureTransferService.h"
#include <BasicRenderer/Streaming/TaskScheduler.h>
#include "Runtime/Device/DescriptorHeapManager.h"
#include "Runtime/Device/DeviceManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Assets/MaterialTextureStreaming.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Render/BindingTable.h"
#include "Render/PublicationBindingBundle.h"
#include <BasicRenderer/Streaming/RendererStateRequestService.h>
#include <BasicRenderer/Pipeline/RendererSettings.h>
#include "Materials/TextureStreaming/TextureBindingArtifacts.h"
#include <BasicRenderer/Extensions/Resources/PublishedStateResourceResolver.h>
#include "Render/Runtime/IReadbackService.h"
#include "Render/Runtime/IDescriptorService.h"
#include "Resources/Buffers/Buffer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <shared_mutex>
#include <utility>
#include "Assets/Textures/Pipeline/TexturePipelineArtifacts.h"
#include "Assets/Textures/Pipeline/TexturePipelineStages.h"
#include <stdexcept>

#include <tracy/Tracy.hpp>
#include <BasicTelemetry/Telemetry.h>

namespace {
	constexpr uint32_t kTextureStreamingFlagEligible = 1u << 0;
	constexpr uint32_t kTextureStreamingFlagEnabled = 1u << 1;
	constexpr uint32_t kTextureStreamingFeedbackUnused = 0xffffffffu;
	// The feedback buffer interleaves two kinds of word (utilities.hlsli): a texture's requested top mip + 1, and a
	// material slot's encoded footprint.
	constexpr uint64_t TextureFeedbackWord(uint32_t streamingTextureID) { return uint64_t{ streamingTextureID } * 2u; }
	constexpr uint64_t MaterialFeedbackWord(uint32_t materialSlot) { return uint64_t{ materialSlot } * 2u + 1u; }

	uint32_t SettingUint(const char* name) {
		return SettingsManager::GetInstance().getSettingGetter<uint32_t>(name)();
	}

	bool MaterialTextureStreamingTransitionLoggingEnabled() {
		static const bool enabled = [] {
			char* value = nullptr;
			size_t valueLength = 0;
			const bool isSet =
				_dupenv_s(&value, &valueLength, "SARP_TEXTURE_STREAMING_TRANSITION_LOG") == 0 &&
				value != nullptr &&
				value[0] != '\0' &&
				value[0] != '0';
			std::free(value);
			return isSet;
		}();
		return enabled;
	}

	uint32_t TextureSrvIndex(const std::shared_ptr<org::PixelBuffer>& image) {
		return image && image->HasValidBackingResource()
			? image->GetSRVInfo(0).slot.index
			: UINT32_MAX;
	}

	uint64_t ComputeTextureResidentBytes(const org::TextureDescription& desc) {
		uint64_t totalBytes = 0;
		for (const org::ImageDimensions& dims : desc.imageDimensions) {
			totalBytes += dims.slicePitch;
		}
		return totalBytes;
	}

	uint32_t AlphaTestedTopMipCap(const TextureAsset& texture) {
		return AlphaTestedMaterialTextureMaxResidentTopMip(
			texture.GetFullMip0Width(), texture.GetFullMip0Height(),
			texture.GetStreamingState().residency.totalMipCount);
	}

	// Image-table row for the binding a frame actually samples: the published image,
	// described by the residency it was published with rather than the prepared
	// state, which can already name the next window while the old image is bound.
	TextureStreamingGPUInfo BuildPublishedTextureStreamingGPUInfo(const TextureAsset& texture,
		const TextureAsset::PublishedBindingSnapshot& published,
		org::runtime::IDescriptorService& descriptorService) {
		const TextureStreamingState state = texture.GetStreamingState();
		TextureStreamingGPUInfo info = {};
		if (state.eligible) info.flags |= kTextureStreamingFlagEligible;
		if (state.enabled) info.flags |= kTextureStreamingFlagEnabled;
		info.totalMipCount = state.residency.totalMipCount;
		info.residentTopMip = state.residency.residentTopMip;
		info.residentMipCount = state.residency.residentMipCount;
		info.bindingRevisionLo = static_cast<uint32_t>(state.bindingRevision & 0xffffffffull);
		info.bindingRevisionHi = static_cast<uint32_t>(state.bindingRevision >> 32u);
		if (published.image) {
			const auto& residency = published.streamingState.residency;
			info.residentTopMip = residency.residentTopMip;
			info.residentMipCount = residency.residentMipCount;
			info.bindingRevisionLo = static_cast<uint32_t>(published.bindingRevision & 0xffffffffull);
			info.bindingRevisionHi = static_cast<uint32_t>(published.bindingRevision >> 32u);
		}
		info.fullWidth = texture.GetFullMip0Width();
		info.fullHeight = texture.GetFullMip0Height();
		info.requestedTopMip = state.requestedTopMip;
		info.pendingTopMip = state.pendingTopMip;
		info.imageDescriptorIndex = TextureSrvIndex(published.image);
		info.samplerDescriptorIndex = texture.SamplerDescriptorIndex(descriptorService);
		return info;
	}

	TextureStreamingGPUInfo BuildTextureStreamingGPUInfo(const TextureAsset& texture,
		org::runtime::IDescriptorService& descriptorService) {
		const TextureStreamingState& state = texture.GetStreamingState();
		TextureStreamingGPUInfo info = {};
		if (state.eligible) {
			info.flags |= kTextureStreamingFlagEligible;
		}
		if (state.enabled) {
			info.flags |= kTextureStreamingFlagEnabled;
		}
		info.totalMipCount = state.residency.totalMipCount;
		info.residentTopMip = state.residency.residentTopMip;
		info.residentMipCount = state.residency.residentMipCount;
		info.fullWidth = texture.GetFullMip0Width();
		info.fullHeight = texture.GetFullMip0Height();
		info.requestedTopMip = state.requestedTopMip;
		info.pendingTopMip = state.pendingTopMip;
		info.bindingRevisionLo = static_cast<uint32_t>(state.bindingRevision & 0xffffffffull);
		info.bindingRevisionHi = static_cast<uint32_t>(state.bindingRevision >> 32u);
		info.imageDescriptorIndex = TextureSrvIndex(texture.ImagePtr());
		info.samplerDescriptorIndex = texture.SamplerDescriptorIndex(descriptorService);
		return info;
	}

	TextureStreamingGPUInfo BuildTextureStreamingGPUInfo(
		const TextureStreamingState& state, uint32_t fullWidth, uint32_t fullHeight) {
		TextureStreamingGPUInfo info{};
		if (state.eligible) info.flags |= kTextureStreamingFlagEligible;
		if (state.enabled) info.flags |= kTextureStreamingFlagEnabled;
		info.totalMipCount = state.residency.totalMipCount;
		info.residentTopMip = state.residency.residentTopMip;
		info.residentMipCount = state.residency.residentMipCount;
		info.fullWidth = fullWidth;
		info.fullHeight = fullHeight;
		info.requestedTopMip = state.requestedTopMip;
		info.pendingTopMip = state.pendingTopMip;
		info.bindingRevisionLo = static_cast<uint32_t>(state.bindingRevision & 0xffffffffull);
		info.bindingRevisionHi = static_cast<uint32_t>(state.bindingRevision >> 32u);
		return info;
	}

}

TextureStreamingManager::TextureStreamingManager()
{
	m_readbackCallbackState->owner = this;
	TextureStreamingGPUInfo fallback{};
	m_textureImageTableJournal.Initialize(
		std::as_bytes(std::span{ &fallback, std::size_t{ 1 } }), 1, 1);
	m_textureStreamingMetadataBuffer = DynamicStructuredBuffer<TextureStreamingGPUInfo>::CreateShared(
		1u,
		"Builtin::Material::TextureStreamingMetadataBuffer",
		true);
	m_textureStreamingFeedbackBuffer = DynamicStructuredBuffer<uint32_t>::CreateShared(
		1u,
		"Builtin::Material::TextureStreamingFeedbackBuffer",
		true);
	org::memory::SetResourceUsageHint(*m_textureStreamingMetadataBuffer, "Material texture streaming buffers");
	org::memory::SetResourceUsageHint(*m_textureStreamingFeedbackBuffer, "Material texture streaming buffers");
	m_textureStreamingMetadataBuffer->UpdateAt(0u, TextureStreamingGPUInfo{});
	m_textureStreamingFeedbackBuffer->UpdateAt(0u, kTextureStreamingFeedbackUnused);
	m_resources[Builtin::Material::TextureStreamingMetadataBuffer] = m_textureStreamingMetadataBuffer;
	m_textureImageTableResolver = std::make_shared<PublishedStateResourceResolver>(
		br::render::PublishedStateSource::ProcessSource(),
		br::render::PublishedResourceKey{
			br::render::PublishedFragmentKind::TextureImages,
			br::render::PublishedResourceUsage::ShaderResource, 0, 0,
			br::render::kTextureImageTableBufferVariant },
		m_textureStreamingMetadataBuffer, true);
	m_resources[Builtin::Material::TextureStreamingFeedbackBuffer] = m_textureStreamingFeedbackBuffer;
}

TextureStreamingManager::~TextureStreamingManager()
{
	Shutdown();
}

void TextureStreamingManager::SetRendererStateRequestService(
	br::render::RendererStateRequestService* service,
	std::shared_ptr<org::runtime::IUploadService> uploads)
{
	m_uploadService = std::move(uploads);
	{
		std::lock_guard lock(m_graphBindingAwaiterMutex);
		m_graphBindingAwaiters.clear();
	}
	{
		std::lock_guard lock(m_textureImageTableAwaiterMutex);
		m_textureImageTableAwaiter.Reset();
	}
	m_textureImageTableBuildInFlight.store(false, std::memory_order_release);
	m_rendererStateRequests = service;
	if (service && !m_textureImageTableFamily) {
		m_textureImageTableFamily = std::make_shared<br::render::VersionedBufferFamily>(service->VersionedBuffers(),
			br::render::VersionedBufferFamily::Config{
				.address = { br::render::ArtifactKind::BufferVersion, 0,
					br::render::kTextureImageTableBufferVariant },
				.debugName = "PublishedTextureImageTable",
				.elementStride = sizeof(TextureStreamingGPUInfo),
				.catalogOwner = br::render::PublishedFragmentKind::TextureImages,
				.catalogUsage = br::render::PublishedResourceUsage::ShaderResource,
				.catalogVariant = br::render::kTextureImageTableBufferVariant });
		m_textureImageTableJournal.SetBackingPool(m_textureImageTableFamily->Configuration().pool);
	}
}

void TextureStreamingManager::Initialize(TextureFactory& textureFactory, uint32_t framesInFlight)
{
	{
		std::lock_guard lock(m_readbackCallbackState->mutex);
		m_readbackCallbackState->owner = this;
	}
	m_framesInFlight = (std::max)(framesInFlight, 1u);
	if (m_initialized.exchange(true, std::memory_order_acq_rel)) {
		return;
	}
	m_textureFactory = &textureFactory;
	m_pipelineServices = std::make_shared<br::render::TexturePipelineServices>();
	m_pipelineServices->factory = &textureFactory;
	br::textures::SetTextureLoader([this](const std::shared_ptr<TextureAsset>& texture, std::function<void(bool)> ready) {
		LoadPipelineTexture(texture, std::move(ready));
	});
	m_materialTextureTransfers = std::make_unique<MaterialTextureTransferService>();
	m_materialTextureTransfers->Initialize();
	textureFactory.SetMaterialTextureTransferService(m_materialTextureTransfers.get());
	auto device = DeviceManager::GetInstance().GetDevice();
	if (device.CreateTimeline(m_readbackFencePtr, 0, "MaterialTextureStreamingReadbackFence") == rhi::Result::Ok && m_readbackFencePtr) {
		m_readbackFence = m_readbackFencePtr.Get();
	}
	m_readbackSlots.resize((std::max)(framesInFlight, 1u));
	m_workerQuit.store(false, std::memory_order_release);
	m_lastProcessedReadbackFence = 0;
	m_taskScope = TaskSchedulerManager::GetInstance().CreateScope("TextureStreamingManager");
	m_commandPump.Configure(
		[this](br::SerializedTaskPump::Task task) {
			return TaskSchedulerManager::GetInstance().Submit(
				m_taskScope, TaskLane::Streaming, TaskDomain::TextureControl,
				"TextureStreamingManager::Drain",
				[task = std::move(task)](const br::TaskContext& context) mutable {
					if (!context.StopRequested()) task();
				});
		},
		[this] { Drain(); });
}

void TextureStreamingManager::Shutdown()
{
	{
		std::lock_guard lock(m_readbackCallbackState->mutex);
		m_readbackCallbackState->owner = nullptr;
	}
	if (!m_initialized.exchange(false, std::memory_order_acq_rel)) {
		return;
	}
	m_workerQuit.store(true, std::memory_order_release);
	m_commandPump.Stop();
	{
		std::lock_guard lock(m_textureImageTableAwaiterMutex);
		m_textureImageTableAwaiter.Reset();
	}
	if (m_taskScope.Valid()) m_taskScope.CancelAndWait();
	br::textures::SetTextureLoader({});
	if (m_pipelineServices) {
		// Texture pipeline jobs still in the graph cancel themselves without a factory.
		std::unique_lock lock(m_pipelineServices->mutex);
		m_pipelineServices->factory = nullptr;
	}
	{
		std::unordered_map<uint32_t, br::render::ArtifactAwaiter> residencyAwaiters;
		std::unordered_map<uint64_t, br::render::ArtifactAwaiter> processedAwaiters;
		std::lock_guard lock(m_pipelineMutex);
		residencyAwaiters.swap(m_pipelineResidencyAwaiters);
		processedAwaiters.swap(m_pipelineProcessedAwaiters);
		m_pipelineResidenciesInFlight.clear();
		m_completedPipelineResidencies.clear();
	}
	if (m_textureFactory) m_textureFactory->SetMaterialTextureTransferService(nullptr);
	if (m_materialTextureTransfers) m_materialTextureTransfers->Shutdown();
	m_materialTextureTransfers.reset();
	m_textureFactory = nullptr;
	{
		std::lock_guard lock(m_readbackSlotMutex);
		m_readbackSlots.clear();
	}
	m_readbackFence.Reset();
	m_readbackFencePtr.Reset();
}

void TextureStreamingManager::QueueCommand(WorkerCommand&& command)
{
	{
		std::lock_guard lock(m_workerCommandMutex);
		m_workerCommands.push_back(std::move(command));
	}
	ScheduleDrain();
}

void TextureStreamingManager::EnqueueFrameTick(uint64_t frameIndex)
{
	WorkerCommand command{};
	command.kind = WorkerCommand::Kind::FrameTick;
	command.frameIndex = frameIndex;
	QueueCommand(std::move(command));
}

void TextureStreamingManager::EnqueueTextureUploadAdvance(
	const std::shared_ptr<TextureAsset>& texture,
	const char* reason)
{
	WorkerCommand command{};
	command.kind = WorkerCommand::Kind::MarkDirty;
	command.texture = texture;
	command.needsUploadAdvance = true;
	command.reason = reason ? reason : "external";
	QueueCommand(std::move(command));
}

void TextureStreamingManager::EnqueueTextureMetadataRefresh(
	const std::shared_ptr<TextureAsset>& texture, const char* reason)
{
	WorkerCommand command{};
	command.kind = WorkerCommand::Kind::MarkDirty;
	command.texture = texture;
	command.needsUploadAdvance = false;
	command.reason = reason ? reason : "binding_published";
	QueueCommand(std::move(command));
}

void TextureStreamingManager::ScheduleDrain()
{
	if (m_workerQuit.load(std::memory_order_acquire) || !m_taskScope.Valid()) return;
	(void)m_commandPump.Notify();
}

void TextureStreamingManager::Drain()
{
	ZoneScopedN("TextureStreamingWorker::Drain");
	std::deque<WorkerCommand> commands;
	{
		std::lock_guard lock(m_workerCommandMutex);
		const auto count = (std::min<std::size_t>)(m_workerCommands.size(), 256u);
		for (std::size_t i = 0; i < count; ++i) {
			commands.push_back(std::move(m_workerCommands.front()));
			m_workerCommands.pop_front();
		}
	}
	if (!m_workerQuit.load(std::memory_order_acquire)) PollCompletedReadbackSlots(m_lastProcessedReadbackFence);

		uint64_t newestFrame = 0;
		for (auto& command : commands) {
			switch (command.kind) {
			case WorkerCommand::Kind::Register:
				ApplyRegisterCommand(std::move(command));
				break;
			case WorkerCommand::Kind::Unregister:
				ApplyUnregisterCommand(command.bindingID);
				break;
			case WorkerCommand::Kind::MarkDirty:
				MarkTextureStreamingMetadataDirty(command.texture, command.needsUploadAdvance, command.reason.c_str());
				break;
			case WorkerCommand::Kind::FrameTick:
				newestFrame = (std::max)(newestFrame, command.frameIndex);
				break;
			case WorkerCommand::Kind::Load:
				ApplyLoadCommand(command.texture);
				break;
			}
		}

		if (m_textureFactory) AdvanceCompletedPipelineResidencies(*m_textureFactory);
		if (newestFrame != 0 && m_textureFactory) {
			ProcessPendingTextureUpdates(newestFrame, *m_textureFactory);
		}
	// Image-table rows queued by adoption callbacks and metadata refreshes are
	// journaled and published from this serialized drain only.
	if (!m_workerQuit.load(std::memory_order_acquire)) {
		FlushPendingTextureImageTableMetadata();
		PublishTextureImageTable();
	}
	bool hasMore = false;
	{
		std::lock_guard lock(m_workerCommandMutex);
		hasMore = !m_workerCommands.empty();
	}
	{
		std::lock_guard lock(m_pipelineMutex);
		hasMore = hasMore || !m_completedPipelineResidencies.empty();
	}
	if (hasMore && !m_workerQuit.load(std::memory_order_acquire)) ScheduleDrain();
}

void TextureStreamingManager::PollCompletedReadbackSlots(uint64_t& lastProcessedFence)
{
	const uint64_t submitted = m_readbackFenceCounter.load(std::memory_order_acquire);
	if (!m_readbackFence.IsValid() || submitted <= lastProcessedFence) {
		return;
	}
	if (m_readbackFence.GetCompletedValue() < submitted) return;
	const uint64_t completed = m_readbackFence.GetCompletedValue();
	struct CompletedSlot {
		uint32_t index = 0;
		uint64_t fenceValue = 0;
		uint64_t copyBytes = 0;
		std::shared_ptr<org::Buffer> staging;
		std::vector<uint32_t> activeIDs;
		uint32_t materialSlotCount = 0;
	};
	std::vector<CompletedSlot> completedSlots;
	{
		std::lock_guard slotLock(m_readbackSlotMutex);
		for (uint32_t index = 0; index < static_cast<uint32_t>(m_readbackSlots.size()); ++index) {
			auto& slot = m_readbackSlots[index];
			if (!slot.inFlight || slot.fenceValue == 0 || slot.fenceValue > completed || !slot.staging) continue;
			completedSlots.push_back(CompletedSlot{
				.index = index,
				.fenceValue = slot.fenceValue,
				.copyBytes = slot.copyBytes,
				.staging = slot.staging,
				.activeIDs = slot.activeStreamingTextureIDs,
				.materialSlotCount = slot.materialSlotCount});
		}
	}
	for (auto& completedSlot : completedSlots) {
		void* mapped = nullptr;
		completedSlot.staging->GetAPIResource().Map(&mapped);
		if (mapped) {
			std::lock_guard feedbackLock(m_textureStreamingFeedbackMutex);
			const size_t wordCount = static_cast<size_t>(completedSlot.copyBytes / sizeof(uint32_t));
			const auto* words = static_cast<const uint32_t*>(mapped);
			for (uint32_t materialSlot = 0; materialSlot < completedSlot.materialSlotCount; ++materialSlot) {
				const uint64_t word = MaterialFeedbackWord(materialSlot);
				if (word >= wordCount) break;
				const uint32_t value = words[word];
				// Zero: a word no reset has reached yet (see below).
				if (value != kTextureStreamingFeedbackUnused && value != 0u)
					m_pendingMaterialStreamingFeedback.emplace_back(materialSlot, value);
			}
			for (uint32_t streamingTextureID : completedSlot.activeIDs) {
				const uint64_t word = TextureFeedbackWord(streamingTextureID);
				if (word >= wordCount) continue;
				const uint32_t requestedTopMip = words[word];
				// Shaders store mip + 1. Zero is a slot no reset has reached yet (a newly grown backing replays the CPU
				// copy, which can still hold zeros), not a request: read as mip 0, each one loaded a full-resolution image.
				if (requestedTopMip != kTextureStreamingFeedbackUnused && requestedTopMip != 0u) {
					m_pendingTextureStreamingFeedback.emplace_back(streamingTextureID, requestedTopMip - 1u);
				}
			}
			completedSlot.staging->GetAPIResource().Unmap(0, 0);
		}
		{
			std::lock_guard slotLock(m_readbackSlotMutex);
			if (completedSlot.index < m_readbackSlots.size()) {
				auto& slot = m_readbackSlots[completedSlot.index];
				if (slot.fenceValue == completedSlot.fenceValue) {
					slot.activeStreamingTextureIDs.clear();
					slot.inFlight = false;
					slot.copyBytes = 0;
					slot.fenceValue = 0;
				}
			}
		}
	}
	lastProcessedFence = completed;
}

uint64_t TextureStreamingManager::RegisterTextureBinding(
	const std::shared_ptr<TextureAsset>& texture,
	BindingChangedCallback onBindingChanged,
	std::string debugLabel,
	TextureStreamingBindingOptions options)
{
	ZoneScopedN("TextureStreamingManager::RegisterTextureBinding");
	if (!debugLabel.empty()) {
		ZoneText(debugLabel.c_str(), debugLabel.size());
	}
	if (!texture) {
		return 0u;
	}

	const uint32_t streamingTextureID = texture->GetStreamingTextureID();
	if (streamingTextureID == 0u) {
		return 0u;
	}
	ZoneValue(streamingTextureID);
	const uint64_t bindingID = m_nextBindingID.fetch_add(1u, std::memory_order_relaxed);
	// Registrations carry streaming policy only. Consumers observe adopted
	// bindings through the published texture-image table, never through a
	// render-thread callback.
	if (onBindingChanged) {
		spdlog::warn(
			"TextureStreamingManager: binding callbacks are no longer dispatched; consume the published image table instead (label='{}')",
			debugLabel);
	}
	WorkerCommand command{};
	command.kind = WorkerCommand::Kind::Register;
	command.bindingID = bindingID;
	command.texture = texture;
	command.debugLabel = std::move(debugLabel);
	command.options = options;
	QueueCommand(std::move(command));
	return bindingID;
}

void TextureStreamingManager::ApplyRegisterCommand(WorkerCommand&& command)
{
	const auto& texture = command.texture;
	if (!texture) return;
	const uint32_t streamingTextureID = texture->GetStreamingTextureID();
	m_bindingsByID.emplace(command.bindingID, TextureBindingOwner{
		.bindingID = command.bindingID,
		.streamingTextureID = streamingTextureID,
		.texture = texture,
		.debugLabel = std::move(command.debugLabel),
		.options = command.options,
	});
	auto& bindingIDs = m_bindingIDsByStreamingTextureID[streamingTextureID];
	bindingIDs.push_back(command.bindingID);
	const bool firstBindingOwner = bindingIDs.size() == 1u;
	if (command.options.alphaTested) {
		++m_alphaTestedBindingCountsByStreamingTextureID[streamingTextureID];
	}

	if (!command.options.allowIdleCoarsening) {
		++m_idleCoarseningDisabledBindingCountsByStreamingTextureID[streamingTextureID];
	}
	if (command.options.maximumResidentTopMip != (std::numeric_limits<uint32_t>::max)()) {
		++m_maximumResidentTopMipBindingCountsByStreamingTextureID[streamingTextureID]
			[command.options.maximumResidentTopMip];
	}
	uint32_t cappedTopMip = texture->GetStreamingState().residency.totalMipCount - 1u;
	if (command.options.alphaTested) {
		cappedTopMip = (std::min)(cappedTopMip, AlphaTestedTopMipCap(*texture));
	}
	if (auto capIt = m_maximumResidentTopMipBindingCountsByStreamingTextureID.find(streamingTextureID);
		capIt != m_maximumResidentTopMipBindingCountsByStreamingTextureID.end() && !capIt->second.empty()) {
		cappedTopMip = (std::min)(cappedTopMip, capIt->second.begin()->first);
	}
	if (texture->GetStreamingState().requestedTopMip > cappedTopMip) {
		(void)texture->ApplyStreamingSystemRequest(cappedTopMip, 0u);
	}
	TrackTexture(texture);
	MarkTextureStreamingMetadataDirty(texture, true, "track_binding");
	// A texture that is already bound (or failed) satisfies a new owner's display
	// gates immediately; the image-table flush evaluates and requests them.
	QueueTextureImageTableMetadata(texture);
	if (command.options.seedCurrentBinding && firstBindingOwner) {
		auto preparedImage = texture->PreparedImagePtr();
		if (preparedImage) {
			// Seed the publication boundary. The main-thread owner registry independently
			// tracks which individual owners still need to observe this binding.  Do not
			// republish an already-current image for every additional owner: that dirtied
			// all existing owners and produced an O(owner registrations) material rewrite
			// storm without changing a descriptor.
			//
			// The first owner must pass through this boundary even when the prepared and
			// published images are already identical. External-immutable images are no
			// longer transitioned by the render graph, so skipping this seed left initial
			// bindings outside the transfer service's shader-ready state machine.
			QueueBindingChanged(*texture, {});
		}
	}
	if (command.options.seedCurrentBinding && m_rendererStateRequests) {
		// Every owner registration idempotently ensures the graph vertex. The first
		// owner is only a streaming-policy optimization: it may have registered
		// before an image was published, so it cannot own graph correctness.
		const auto published = texture->GetPublishedBindingSnapshot();
		const auto address = br::render::ArtifactAddress{
			br::render::ArtifactKind::TextureBinding, streamingTextureID, 0 };
		// Only the desired revision matters here. Diagnose walks the blocker
		// chain and copies a payload snapshot under the graph's control mutex,
		// which made owner registration the largest Diagnose caller by far.
		const auto desiredRevision = m_rendererStateRequests->DesiredRevision(address);
		if (published.image && published.bindingRevision != 0u &&
			published.image->HasValidBackingResource() &&
			desiredRevision < published.bindingRevision) {
				auto input = std::make_shared<br::render::TextureBindingBuildInput>();
				input->streamingTextureID = streamingTextureID;
				input->bindingRevision = published.bindingRevision;
				input->streamingStateRevision = texture->GetStreamingStateRevision();
				if (!m_descriptorService) throw std::runtime_error("TextureStreamingManager: descriptor service unavailable while seeding binding");
				input->samplerDescriptorIndex = texture->SamplerDescriptorIndex(*m_descriptorService);
				input->image = published.image;
				input->streamingMetadata = BuildTextureStreamingGPUInfo(
					published.streamingState, texture->GetFullMip0Width(), texture->GetFullMip0Height());
				const auto fingerprint =
					(published.bindingRevision << 1u) ^ input->streamingStateRevision ^
					streamingTextureID ^ 0x54455842494e44ull;
				const auto request = m_rendererStateRequests->SubmitLatest({
					address,
					published.bindingRevision, {},
					br::render::ArtifactPayload::Make<br::render::TextureBindingBuildInput>(
						std::move(input)), fingerprint });
				if (!request) {
					spdlog::error(
						"TextureStreamingManager: failed to seed published graph binding textureID={} revision={} status={}",
						streamingTextureID, published.bindingRevision,
						static_cast<unsigned>(request.status));
				}
		}
	}
}

void TextureStreamingManager::UnregisterTextureBinding(uint64_t bindingID)
{
	ZoneScopedN("TextureStreamingManager::UnregisterTextureBinding");
	if (bindingID == 0u) {
		return;
	}

	WorkerCommand command{};
	command.kind = WorkerCommand::Kind::Unregister;
	command.bindingID = bindingID;
	QueueCommand(std::move(command));
}

void TextureStreamingManager::ApplyUnregisterCommand(uint64_t bindingID)
{
	ZoneScopedN("TextureStreamingWorker::ApplyUnregisterCommand");
	auto bindingIt = m_bindingsByID.find(bindingID);
	if (bindingIt == m_bindingsByID.end()) {
		return;
	}

	const uint32_t streamingTextureID = bindingIt->second.streamingTextureID;
	const TextureStreamingBindingOptions removedOptions = bindingIt->second.options;
	ZoneValue(streamingTextureID);
	{
		ZoneScopedN("TextureStreamingManager::UnregisterTextureBinding::EraseBinding");
		m_bindingsByID.erase(bindingIt);
	}
	if (removedOptions.alphaTested) {
		auto alphaCountIt = m_alphaTestedBindingCountsByStreamingTextureID.find(streamingTextureID);
		if (alphaCountIt != m_alphaTestedBindingCountsByStreamingTextureID.end()) {
			if (alphaCountIt->second <= 1u) {
				m_alphaTestedBindingCountsByStreamingTextureID.erase(alphaCountIt);
			}
			else {
				--alphaCountIt->second;
			}
		}
	}
	if (!removedOptions.allowIdleCoarsening) {
		auto countIt = m_idleCoarseningDisabledBindingCountsByStreamingTextureID.find(streamingTextureID);
		if (countIt != m_idleCoarseningDisabledBindingCountsByStreamingTextureID.end()) {
			if (countIt->second <= 1u) m_idleCoarseningDisabledBindingCountsByStreamingTextureID.erase(countIt);
			else --countIt->second;
		}
	}
	if (removedOptions.maximumResidentTopMip != (std::numeric_limits<uint32_t>::max)()) {
		auto textureCapsIt = m_maximumResidentTopMipBindingCountsByStreamingTextureID.find(streamingTextureID);
		if (textureCapsIt != m_maximumResidentTopMipBindingCountsByStreamingTextureID.end()) {
			auto capIt = textureCapsIt->second.find(removedOptions.maximumResidentTopMip);
			if (capIt != textureCapsIt->second.end()) {
				if (capIt->second <= 1u) textureCapsIt->second.erase(capIt);
				else --capIt->second;
			}
			if (textureCapsIt->second.empty()) m_maximumResidentTopMipBindingCountsByStreamingTextureID.erase(textureCapsIt);
		}
	}
	auto ownersIt = m_bindingIDsByStreamingTextureID.find(streamingTextureID);
	if (ownersIt != m_bindingIDsByStreamingTextureID.end()) {
		{
			ZoneScopedN("TextureStreamingManager::UnregisterTextureBinding::EraseOwner");
			TracyPlot("TextureStreamingManager.OwnerCountBeforeErase", static_cast<int64_t>(ownersIt->second.size()));
			std::erase(ownersIt->second, bindingID);
		}
		if (ownersIt->second.empty()) {
			ZoneScopedN("TextureStreamingManager::UnregisterTextureBinding::EraseTextureState");
			m_bindingIDsByStreamingTextureID.erase(ownersIt);
			m_streamingTexturesByID.erase(streamingTextureID);
			m_texturePolicy.erase(streamingTextureID);
			m_textureTopSinceTick.erase(streamingTextureID);
			m_textureStreamingMetadataRevisions.erase(streamingTextureID);
			m_alphaTestedBindingCountsByStreamingTextureID.erase(streamingTextureID);
			m_idleCoarseningDisabledBindingCountsByStreamingTextureID.erase(streamingTextureID);
			m_maximumResidentTopMipBindingCountsByStreamingTextureID.erase(streamingTextureID);
			m_dirtyTextureStreamingIDSet.erase(streamingTextureID);
			m_texturesNeedingUploadAdvanceSet.erase(streamingTextureID);
			{
				std::lock_guard feedbackIDsLock(m_activeFeedbackMutex);
				m_activeTextureStreamingFeedbackIDSet.erase(streamingTextureID);
				std::erase(m_activeTextureStreamingFeedbackIDs, streamingTextureID);
			}
			std::erase(m_dirtyTextureStreamingIDs, streamingTextureID);
			std::erase(m_texturesNeedingUploadAdvance, streamingTextureID);
			if (m_rendererStateRequests) {
				m_rendererStateRequests->Release({
					br::render::ArtifactKind::TextureBinding, streamingTextureID, 0 });
			}
			{
				std::scoped_lock mailboxLock(m_bindingMailboxMutex);
				m_dirtyBindingMailboxes.erase(streamingTextureID);
			}
		}
	}
}

void TextureStreamingManager::UnregisterTextureBindings(const std::vector<uint64_t>& bindingIDs)
{
	ZoneScopedN("TextureStreamingManager::UnregisterTextureBindings");
	TracyPlot("TextureStreamingManager.UnregisterBindingBatchSize", static_cast<int64_t>(bindingIDs.size()));
	for (uint64_t bindingID : bindingIDs) {
		UnregisterTextureBinding(bindingID);
	}
}

void TextureStreamingManager::TrackTexture(const std::shared_ptr<TextureAsset>& texture)
{
	if (!texture) {
		return;
	}
	const uint32_t streamingTextureID = texture->GetStreamingTextureID();
	if (streamingTextureID == 0u) {
		return;
	}
	m_streamingTexturesByID[streamingTextureID] = texture;
}

void TextureStreamingManager::RecordTextureDirtyReason(const char* reason)
{
	if (std::strcmp(reason, "feedback") == 0) {
		++m_textureDirtyReasonFeedback;
	}
	else if (std::strcmp(reason, "idle_coarsen") == 0) {
		++m_textureDirtyReasonIdleCoarsen;
	}
	else if (std::strcmp(reason, "track_binding") == 0) {
		++m_textureDirtyReasonTrackBinding;
	}
	else if (std::strcmp(reason, "upload_state_revision") == 0) {
		++m_textureDirtyReasonUploadStateRevision;
	}
	else if (std::strcmp(reason, "upload_pending") == 0) {
		++m_textureDirtyReasonUploadPending;
	}
	else {
		++m_textureDirtyReasonOther;
	}
}

void TextureStreamingManager::MarkTextureStreamingMetadataDirty(
	const std::shared_ptr<TextureAsset>& texture,
	bool needsUploadAdvance,
	const char* reason)
{
	if (!texture) {
		return;
	}

	const uint32_t streamingTextureID = texture->GetStreamingTextureID();
	if (streamingTextureID == 0u) {
		return;
	}

	TrackTexture(texture);
	if (needsUploadAdvance && m_texturesNeedingUploadAdvanceSet.insert(streamingTextureID).second) {
		m_texturesNeedingUploadAdvance.push_back(streamingTextureID);
	}
	if (IsMaterialTextureStreamingEnabledSetting() && m_dirtyTextureStreamingIDSet.insert(streamingTextureID).second) {
		m_dirtyTextureStreamingIDs.push_back(streamingTextureID);
	}

	RecordTextureDirtyReason(reason);
}

void TextureStreamingManager::SetMaterialFeedbackTextures(uint32_t materialSlot, std::span<const uint32_t> streamingTextureIDs)
{
	std::vector<uint32_t> ids;
	ids.reserve(streamingTextureIDs.size());
	for (const uint32_t id : streamingTextureIDs) {
		if (id != 0u && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
	}
	std::lock_guard lock(m_materialFeedbackSlotsMutex);
	if (materialSlot >= m_materialFeedbackSlots.size()) {
		if (ids.empty()) return;
		m_materialFeedbackSlots.resize(static_cast<size_t>(materialSlot) + 1u);
	}
	if (m_materialFeedbackSlots[materialSlot] == ids) return;
	m_materialFeedbackSlots[materialSlot] = std::move(ids);
	++m_materialFeedbackSlotsRevision;
	m_materialFeedbackSlotCount.store(static_cast<uint32_t>(m_materialFeedbackSlots.size()), std::memory_order_release);
}

uint32_t TextureStreamingManager::CoarsestAllowedTopMip(uint32_t streamingTextureID, const TextureAsset& texture, uint32_t totalMipCount) const
{
	uint32_t top = totalMipCount - 1u;
	if (auto capIt = m_maximumResidentTopMipBindingCountsByStreamingTextureID.find(streamingTextureID);
		capIt != m_maximumResidentTopMipBindingCountsByStreamingTextureID.end() && !capIt->second.empty()) {
		top = (std::min)(top, capIt->second.begin()->first);
	}
	if (m_alphaTestedBindingCountsByStreamingTextureID.contains(streamingTextureID)) {
		top = (std::min)(top, AlphaTestedTopMipCap(texture));
	}
	return top;
}

void TextureStreamingManager::RefreshPolicyBytes(TexturePolicyState& policy, const TextureAsset& texture, const TextureStreamingState& state) const
{
	const uint32_t width = texture.GetFullMip0Width();
	const uint32_t height = texture.GetFullMip0Height();
	const uint32_t mips = state.residency.totalMipCount;
	const rhi::Format format = texture.Format();
	if (policy.bytesWidth == width && policy.bytesHeight == height && policy.bytesMips == mips && policy.bytesFormat == format &&
		!policy.bytesFrom.empty()) {
		return;
	}
	policy.bytesWidth = width;
	policy.bytesHeight = height;
	policy.bytesMips = mips;
	policy.bytesFormat = format;
	const rhi::BlockInfo block = rhi::GetBlockInfo(format);
	const uint64_t blockBytes = block.bytesPerBlock != 0u ? block.bytesPerBlock : 4u;
	const uint64_t blockWidth = (std::max)(block.blockWidth, 1u);
	const uint64_t blockHeight = (std::max)(block.blockHeight, 1u);
	std::vector<uint64_t> mipBytes(mips, 0u);
	for (uint32_t mip = 0; mip < mips; ++mip) {
		const uint64_t w = (std::max)(width >> mip, 1u);
		const uint64_t h = (std::max)(height >> mip, 1u);
		mipBytes[mip] = ((w + blockWidth - 1u) / blockWidth) * ((h + blockHeight - 1u) / blockHeight) * blockBytes;
	}
	policy.bytesFrom.assign(mips, 0u);
	uint64_t running = 0;
	for (uint32_t mip = mips; mip-- > 0;) {
		running += mipBytes[mip];
		policy.bytesFrom[mip] = br::texturestreaming::AlignTextureBytes(running);
	}
}

bool TextureStreamingManager::MeasureTextureStreamingAllowance(uint64_t residentBytes, uint64_t& allowance)
{
	rhi::VideoMemoryInfo local{};
	auto device = DeviceManager::GetInstance().GetDevice();
	if (rhi::Failed(device.QueryVideoMemoryInfo(0, rhi::MemorySegmentGroup::Local, local)) || local.budgetBytes == 0u) {
		return false;
	}
	// Everything that is not a window comes off the budget first. A rise counts within a second, a fall takes tens:
	// a window just replaced stays allocated until its last frame retires, and a load's spike passes.
	const uint64_t other = local.currentUsageBytes > residentBytes ? local.currentUsageBytes - residentBytes : 0u;
	if (!m_budgetMeasured) m_nonWindowBytes = other;
	else if (other > m_nonWindowBytes) m_nonWindowBytes += (other - m_nonWindowBytes) / 4u;
	else m_nonWindowBytes -= (m_nonWindowBytes - other) / 20u;
	m_budgetMeasured = true;

	const uint64_t configured = uint64_t{ SettingUint(MaterialTextureStreamingBudgetMBSettingName) } << 20u;
	const uint64_t budget = configured != 0u ? configured : local.budgetBytes;
	const auto target = static_cast<uint64_t>(static_cast<double>(budget) * br::texturestreaming::kBudgetTarget);
	allowance = target > m_nonWindowBytes ? target - m_nonWindowBytes : 0u;

	m_budgetStats.measured = true;
	m_budgetStats.driverBudgetBytes = local.budgetBytes;
	m_budgetStats.budgetBytes = budget;
	m_budgetStats.usageBytes = local.currentUsageBytes;
	m_budgetStats.nonWindowBytes = m_nonWindowBytes;
	m_budgetStats.allowanceBytes = allowance;
	return true;
}

void TextureStreamingManager::BeginTextureStreamingFeedbackFrame(uint64_t frameIndex)
{
	if (!IsMaterialTextureStreamingEnabledSetting()) {
		return;
	}

	// Fold the readbacks that landed since the last frame into what the next policy tick reads: the finest each
	// material slot and each texture asked for.
	std::vector<std::pair<uint32_t, uint32_t>> textureFeedback;
	std::vector<std::pair<uint32_t, uint32_t>> materialFeedback;
	{
		std::lock_guard lock(m_textureStreamingFeedbackMutex);
		textureFeedback.swap(m_pendingTextureStreamingFeedback);
		materialFeedback.swap(m_pendingMaterialStreamingFeedback);
	}
	for (const auto& [streamingTextureID, requestedTopMip] : textureFeedback) {
		auto [it, inserted] = m_textureTopSinceTick.try_emplace(streamingTextureID, requestedTopMip);
		if (!inserted) it->second = (std::min)(it->second, requestedTopMip);
	}
	for (const auto& [materialSlot, value] : materialFeedback) {
		if (materialSlot >= m_materialFootprintSinceTick.size()) {
			m_materialFootprintSinceTick.resize(static_cast<size_t>(materialSlot) + 1u, std::numeric_limits<float>::infinity());
		}
		auto& footprint = m_materialFootprintSinceTick[materialSlot];
		footprint = (std::min)(footprint, br::texturestreaming::DecodeFootprint(value));
	}

	const auto now = std::chrono::steady_clock::now();
	if (now - m_lastPolicyTick >= std::chrono::milliseconds(br::texturestreaming::kTickMilliseconds)) {
		m_lastPolicyTick = now;
		RunResidencyPolicyTick(frameIndex);
	}

	std::vector<uint32_t> activeFeedbackIDs;
	{
		std::lock_guard feedbackIDsLock(m_activeFeedbackMutex);
		activeFeedbackIDs = m_activeTextureStreamingFeedbackIDs;
	}
	const uint32_t materialSlotCount = m_materialFeedbackSlotCount.load(std::memory_order_acquire);
	uint64_t lastWord = 0u;
	if (!activeFeedbackIDs.empty()) {
		lastWord = TextureFeedbackWord(*std::max_element(activeFeedbackIDs.begin(), activeFeedbackIDs.end()));
	}
	if (materialSlotCount != 0u) {
		lastWord = (std::max)(lastWord, MaterialFeedbackWord(materialSlotCount - 1u));
	}
	if (lastWord != 0u) {
		// Reset one dense range instead of staging one four-byte upload per active
		// word.  Sparse per-ID resets generated hundreds of CopyBufferRegion calls
		// every feedback frame and dominated both FlushClient and upload command
		// recording despite transferring only a few KiB.
		std::vector<uint32_t> resetValues(static_cast<size_t>(lastWord) + 1u, kTextureStreamingFeedbackUnused);
		m_textureStreamingFeedbackBuffer->StageRange(0u, resetValues);
	}
}

void TextureStreamingManager::RunResidencyPolicyTick(uint64_t frameIndex)
{
	namespace ts = br::texturestreaming;
	ZoneScopedN("TextureStreamingManager::RunResidencyPolicyTick");

	{
		std::lock_guard lock(m_materialFeedbackSlotsMutex);
		if (m_policyMaterialSlotsRevision != m_materialFeedbackSlotsRevision) {
			m_policyMaterialSlots = m_materialFeedbackSlots;
			m_policyMaterialSlotsRevision = m_materialFeedbackSlotsRevision;
		}
	}
	// This tick's reports as footprints: a material's reaches every texture its row names.
	std::unordered_map<uint32_t, float> reported;
	for (uint32_t slot = 0; slot < m_materialFootprintSinceTick.size(); ++slot) {
		float& footprint = m_materialFootprintSinceTick[slot];
		if (!std::isfinite(footprint)) continue;
		++m_budgetStats.materialReports;
		if (slot < m_policyMaterialSlots.size()) {
			for (const uint32_t id : m_policyMaterialSlots[slot]) {
				auto [it, inserted] = reported.try_emplace(id, footprint);
				if (!inserted) it->second = (std::min)(it->second, footprint);
			}
		}
		footprint = std::numeric_limits<float>::infinity();
	}
	std::unordered_map<uint32_t, uint32_t> reportedTops;
	reportedTops.swap(m_textureTopSinceTick);
	m_budgetStats.textureReports += reportedTops.size();

	// SARP_TEXTURE_FEEDBACK_FORCE_MIP=<n>: every texture at mip n (clamped to its caps), 0 for a full-resolution
	// reference. Still held to the budget.
	static const int forcedTop = [] { const char* v = std::getenv("SARP_TEXTURE_FEEDBACK_FORCE_MIP"); return v && v[0] ? std::atoi(v) : -1; }();

	struct Entry {
		std::shared_ptr<TextureAsset> texture;
		TexturePolicyState* policy = nullptr;
		uint32_t id = 0;
		uint32_t largest = 1;
		uint32_t coarsest = 0;
		uint32_t resident = br::texturestreaming::kNoWindow;
		bool inFlight = false;
	};
	std::vector<Entry> entries;
	entries.reserve(m_streamingTexturesByID.size());
	for (auto it = m_streamingTexturesByID.begin(); it != m_streamingTexturesByID.end();) {
		auto texture = it->second.lock();
		if (!texture) {
			m_texturePolicy.erase(it->first);
			it = m_streamingTexturesByID.erase(it);
			continue;
		}
		const uint32_t id = it->first;
		++it;
		if (!texture->IsMipStreamingEnabled()) continue;
		const TextureStreamingState state = texture->GetStreamingState();
		const uint32_t totalMips = state.residency.totalMipCount;
		if (totalMips <= 1u) continue;

		auto& policy = m_texturePolicy[id];
		RefreshPolicyBytes(policy, *texture, state);
		Entry entry{};
		entry.texture = std::move(texture);
		entry.policy = &policy;
		entry.id = id;
		entry.largest = (std::max)(entry.texture->GetFullMip0Width(), entry.texture->GetFullMip0Height());
		entry.coarsest = CoarsestAllowedTopMip(id, *entry.texture, totalMips);

		float report = std::numeric_limits<float>::infinity();
		if (const auto found = reported.find(id); found != reported.end()) report = found->second;
		if (const auto found = reportedTops.find(id); found != reportedTops.end()) {
			report = (std::min)(report, ts::FootprintForTop(entry.largest, found->second));
		}
		if (std::isfinite(report)) {
			policy.heldFootprint = report;
			policy.age = 0;
		}
		else if (policy.age < ts::kHoldTicks) {
			++policy.age;
		}
		// A binding that does not let its texture coarsen when idle keeps the last demand.
		if (policy.age >= ts::kHoldTicks && std::isfinite(policy.heldFootprint) &&
			m_idleCoarseningDisabledBindingCountsByStreamingTextureID.contains(id)) {
			policy.age = ts::kHoldTicks - 1u;
		}

		if (entry.texture->IsResidentFinalImage()) {
			entry.resident = (std::min)(state.residency.residentTopMip, totalMips - 1u);
			entry.inFlight = state.pendingTopMip != state.residency.residentTopMip;
		}
		entries.push_back(std::move(entry));
	}

	const auto wantedTop = [&](const Entry& entry, float held, float visible) -> uint32_t {
		const auto& policy = *entry.policy;
		if (forcedTop >= 0) {
			// A forced window is held demand: the budget pulls it back first.
			return ts::TopForFootprint(entry.largest, ts::FootprintForTop(entry.largest, static_cast<uint32_t>(forcedTop)) + held, entry.coarsest);
		}
		if (policy.age >= ts::kHoldTicks) return entry.coarsest;
		const float pullback = policy.age < ts::kVisibleTicks ? visible : held;
		return ts::TopForFootprint(entry.largest, policy.heldFootprint + pullback, entry.coarsest);
	};
	const auto bytesAt = [](const Entry& entry, uint32_t top) {
		const auto& bytes = entry.policy->bytesFrom;
		return bytes.empty() ? uint64_t{ 0 } : bytes[(std::min<size_t>)(top, bytes.size() - 1u)];
	};

	uint64_t residentBytes = 0;
	for (const auto& entry : entries) {
		if (entry.resident != ts::kNoWindow) residentBytes += bytesAt(entry, entry.resident);
	}
	uint64_t allowance = std::numeric_limits<uint64_t>::max();
	const bool bounded = MeasureTextureStreamingAllowance(residentBytes, allowance);
	if (!bounded) {
		m_pullback = {};
	}
	else {
		const float maxVisible = static_cast<float>((std::min)(SettingUint(MaterialTextureStreamingVisibleOffsetSettingName), 16u));
		ts::StepPullback(m_pullback, allowance, maxVisible, [&](float held, float visible) {
			uint64_t bytes = 0;
			for (const auto& entry : entries) {
				if (entry.resident != ts::kNoWindow) bytes += bytesAt(entry, wantedTop(entry, held, visible));
			}
			return bytes;
		});
	}

	struct Candidate {
		Entry* entry = nullptr;
		uint32_t top = 0;
		uint32_t shortMips = 0;
		uint64_t bytes = 0;  // an upgrade's: what it adds. A release's: what it frees.
		bool visible = false;
	};
	std::vector<Candidate> upgrades;
	std::vector<Candidate> downgrades;
	uint64_t committed = residentBytes;  // with what is on its way
	uint64_t wantedBytes = 0, visibleWantedBytes = 0, unconstrainedBytes = 0;
	uint32_t visibleTextures = 0, heldTextures = 0;
	for (auto& entry : entries) {
		auto& policy = *entry.policy;
		const bool visible = policy.age < ts::kVisibleTicks;
		visibleTextures += visible ? 1u : 0u;
		heldTextures += !visible && policy.age < ts::kHoldTicks ? 1u : 0u;
		policy.visible = visible;
		if (entry.resident == ts::kNoWindow) {
			// No window yet: its first load (the bootstrap mip) is the texture's own business.
			policy.wantedTop = entry.coarsest;
			continue;
		}
		const uint32_t wanted = wantedTop(entry, m_pullback.held, m_pullback.visible);
		policy.wantedTop = wanted;
		wantedBytes += bytesAt(entry, wanted);
		unconstrainedBytes += bytesAt(entry, wantedTop(entry, 0.0f, 0.0f));
		if (visible) visibleWantedBytes += bytesAt(entry, wanted);

		// What was asked for and not there yet stays asked for, unless it takes too long.
		if (entry.inFlight) {
			const uint32_t pendingTop = entry.texture->GetStreamingState().pendingTopMip;
			if (++policy.pendingTicks < ts::kPendingTimeoutTicks) {
				if (pendingTop < entry.resident) committed += bytesAt(entry, pendingTop) - bytesAt(entry, entry.resident);
				continue;
			}
			// Stuck: kick its residency again rather than wait on it forever.
			policy.pendingTicks = 0;
			MarkTextureStreamingMetadataDirty(entry.texture, true, "pending_retry");
			continue;
		}
		policy.pendingTicks = 0;

		auto action = ts::Decide(entry.resident, wanted, policy.pace);
		// Coarser than a binding allows (an alpha-tested texture losing its coverage): at once.
		if (entry.resident > entry.coarsest) action = ts::Action::Upgrade;
		if (forcedTop >= 0 && wanted != entry.resident) action = wanted < entry.resident ? ts::Action::Upgrade : ts::Action::Downgrade;
		if (action == ts::Action::Upgrade && wanted < entry.resident) {
			upgrades.push_back({ &entry, wanted, entry.resident - wanted, bytesAt(entry, wanted) - bytesAt(entry, entry.resident),
				visible || entry.resident > entry.coarsest });
		}
		else if (action == ts::Action::Downgrade && wanted > entry.resident) {
			downgrades.push_back({ &entry, wanted, 0u, bytesAt(entry, entry.resident) - bytesAt(entry, wanted), visible });
		}
	}

	const auto post = [&](Candidate& candidate, const char* reason) {
		auto& texture = candidate.entry->texture;
		const uint64_t previousRevision = texture->GetStreamingStateRevision();
		const bool needsUploadAdvance = texture->ApplyStreamingSystemRequest(candidate.top, frameIndex);
		if (texture->GetStreamingStateRevision() != previousRevision) {
			MarkTextureStreamingMetadataDirty(texture, needsUploadAdvance, reason);
		}
		candidate.entry->policy->pendingTicks = 0;
		candidate.entry->inFlight = true;
	};
	for (auto& candidate : downgrades) {
		++m_budgetStats.downgrades;
		post(candidate, "policy_downgrade");
	}
	if (bounded && committed > allowance) {
		// Over the allowance: the wait for a downgrade is off, and the largest surplus goes first. Only down to what is
		// wanted: the controller decides what that is, so a texture on screen is not released to make room for another.
		constexpr size_t kMaxReleasesPerTick = 256u;
		std::vector<Candidate> surplus;
		for (auto& entry : entries) {
			if (entry.resident == ts::kNoWindow || entry.inFlight || entry.policy->wantedTop <= entry.resident) continue;
			surplus.push_back({ &entry, entry.policy->wantedTop, 0u,
				bytesAt(entry, entry.resident) - bytesAt(entry, entry.policy->wantedTop), false });
		}
		std::sort(surplus.begin(), surplus.end(), [](const Candidate& a, const Candidate& b) { return a.bytes > b.bytes; });
		uint64_t over = committed - allowance;
		for (size_t i = 0; i < (std::min)(surplus.size(), kMaxReleasesPerTick) && over != 0u; ++i) {
			++m_budgetStats.pressureDowngrades;
			post(surplus[i], "policy_pressure");
			committed -= (std::min)(committed, surplus[i].bytes);
			over -= (std::min)(over, surplus[i].bytes);
		}
	}
	// What is on screen first, then the furthest short. The rate caps what starts this tick, and the next tick takes the
	// rest. What is on screen loads whatever the allowance says: the controller already stepped it down as far as the
	// setting lets it. Another upgrade that does not fit is skipped rather than ending the list: a smaller one may still.
	std::sort(upgrades.begin(), upgrades.end(), [](const Candidate& a, const Candidate& b) {
		return a.visible != b.visible ? a.visible : a.shortMips > b.shortMips;
	});
	const uint64_t tickRate = (std::max<uint64_t>)(uint64_t{ SettingUint(MaterialTextureStreamingRateMBSettingName) } << 20u, 1ull << 20u) *
		ts::kTickMilliseconds / 1000u;
	uint64_t uploadLeft = tickRate;
	for (auto& candidate : upgrades) {
		if (candidate.bytes > uploadLeft && uploadLeft != tickRate) break;
		if (bounded && (!candidate.visible || forcedTop >= 0) && (candidate.bytes > allowance || committed > allowance - candidate.bytes)) continue;
		uploadLeft -= (std::min)(uploadLeft, candidate.bytes);
		committed += candidate.bytes;
		++m_budgetStats.upgrades;
		post(candidate, "policy_upgrade");
	}

	m_budgetStats.residentBytes = residentBytes;
	m_budgetStats.committedBytes = committed;
	m_budgetStats.wantedBytes = wantedBytes;
	m_budgetStats.visibleWantedBytes = visibleWantedBytes;
	m_budgetStats.unconstrainedBytes = unconstrainedBytes;
	m_budgetStats.heldPullback = m_pullback.held;
	m_budgetStats.visiblePullback = m_pullback.visible;
	m_budgetStats.textures = static_cast<uint32_t>(entries.size());
	m_budgetStats.visibleTextures = visibleTextures;
	m_budgetStats.heldTextures = heldTextures;
	if (!bounded) m_budgetStats.allowanceBytes = 0u;

	// SARP_TEXTURE_STREAMING_LOG_SECONDS: how often this line is logged (default 10 s).
	static const int logSeconds = [] { const char* v = std::getenv("SARP_TEXTURE_STREAMING_LOG_SECONDS"); return v && v[0] ? (std::max)(std::atoi(v), 1) : 10; }();
	static auto lastLog = std::chrono::steady_clock::time_point{};
	const auto now = std::chrono::steady_clock::now();
	if (now - lastLog >= std::chrono::seconds(logSeconds)) {
		lastLog = now;
		const auto mib = [](uint64_t bytes) { return bytes >> 20u; };
		spdlog::info(
			"Texture streaming: budget {} MiB (driver {} MiB), VRAM use {} MiB, not windows {} MiB, allowance {} MiB; windows {} MiB "
			"resident, {} MiB committed, {} MiB wanted ({} MiB on screen, {} MiB unconstrained); pull-back held {:.2f} on screen {:.2f}; "
			"textures {} ({} on screen, {} held); upgrades {} downgrades {} pressure {}",
			mib(m_budgetStats.budgetBytes), mib(m_budgetStats.driverBudgetBytes), mib(m_budgetStats.usageBytes),
			mib(m_budgetStats.nonWindowBytes), mib(m_budgetStats.allowanceBytes), mib(residentBytes), mib(committed), mib(wantedBytes),
			mib(visibleWantedBytes), mib(unconstrainedBytes), m_pullback.held, m_pullback.visible, entries.size(), visibleTextures,
			heldTextures, m_budgetStats.upgrades, m_budgetStats.downgrades, m_budgetStats.pressureDowngrades);
	}
}

void TextureStreamingManager::SubmitTextureStreamingFeedbackReadback(org::ExternalClient& client, uint32_t queueSlot)
{
	// Diagnostic escape hatch for isolating unrelated render-graph failures. It
	// suppresses only the feedback copy/readback; texture publication and uploads
	// continue normally.
	if (const char* disabled = std::getenv("SARP_DISABLE_MATERIAL_TEXTURE_STREAMING_READBACK");
		disabled && disabled[0] != '\0' && disabled[0] != '0') {
		return;
	}
	if (!IsMaterialTextureStreamingEnabledSetting() || !m_readbackFence.IsValid() || !m_textureStreamingFeedbackBuffer) {
		return;
	}
	std::vector<uint32_t> activeIDs;
	{
		std::lock_guard lock(m_activeFeedbackMutex);
		activeIDs = m_activeTextureStreamingFeedbackIDs;
	}
	const uint32_t materialSlotCount = m_materialFeedbackSlotCount.load(std::memory_order_acquire);
	if (activeIDs.empty() && materialSlotCount == 0u) return;

	uint64_t bytes = 0;
	if (!m_textureStreamingFeedbackBuffer->TryGetBufferByteSize(bytes) || bytes == 0) return;
	uint32_t selectedSlot = UINT32_MAX;
	std::shared_ptr<org::Buffer> staging;
	{
		std::lock_guard lock(m_readbackSlotMutex);
		for (uint32_t i = 0; i < static_cast<uint32_t>(m_readbackSlots.size()); ++i) {
			const uint32_t index = (m_readbackSlotCursor + i) % static_cast<uint32_t>(m_readbackSlots.size());
			if (!m_readbackSlots[index].inFlight) {
				selectedSlot = index;
				break;
			}
		}
		if (selectedSlot == UINT32_MAX) {
			TracyPlot("TextureStreaming.ReadbackRingFull", int64_t{1});
			return;
		}
		auto& slot = m_readbackSlots[selectedSlot];
		if (!slot.staging || slot.capacityBytes < bytes) {
			slot.staging = org::Buffer::CreateShared(rhi::HeapType::Readback, bytes);
			slot.staging->SetName(("MaterialTextureStreamingReadback_" + std::to_string(selectedSlot)).c_str());
			org::memory::SetResourceUsageHint(*slot.staging, "Material texture streaming readback");
			slot.capacityBytes = bytes;
		}
		slot.activeStreamingTextureIDs = std::move(activeIDs);
		slot.materialSlotCount = materialSlotCount;
		slot.inFlight = true;
		slot.copyBytes = bytes;
		slot.fenceValue = 0;
		staging = slot.staging;
		m_readbackSlotCursor = (selectedSlot + 1u) % static_cast<uint32_t>(m_readbackSlots.size());
	}

	std::shared_ptr<org::Resource> source = m_textureStreamingFeedbackBuffer;
	const uint64_t fenceValue = m_readbackFenceCounter.fetch_add(1u, std::memory_order_acq_rel) + 1u;
	// After a successful submission: record the slot's fence value so the drain reads it once the copy finishes.
	auto submitted = [state = m_readbackCallbackState, selectedSlot, fenceValue]() {
			std::lock_guard stateLock(state->mutex);
			auto* owner = state->owner;
			if (!owner) return;
			{
				std::lock_guard lock(owner->m_readbackSlotMutex);
				if (selectedSlot >= owner->m_readbackSlots.size()
					|| !owner->m_readbackSlots[selectedSlot].inFlight) return;
				owner->m_readbackSlots[selectedSlot].fenceValue = fenceValue;
			}
			owner->ScheduleDrain();
		};
	// After a failed submission: free the slot again.
	auto cancel = [state = m_readbackCallbackState, selectedSlot]() {
			std::lock_guard stateLock(state->mutex);
			auto* owner = state->owner;
			if (!owner) return;
			std::lock_guard lock(owner->m_readbackSlotMutex);
			if (selectedSlot >= owner->m_readbackSlots.size()) {
				return;
			}
			auto& slot = owner->m_readbackSlots[selectedSlot];
			if (slot.inFlight && slot.fenceValue == 0) {
				slot.activeStreamingTextureIDs.clear();
				slot.inFlight = false;
				slot.copyBytes = 0;
			}
		};
	const org::ResourceState copySource{rhi::ResourceAccessType::CopySource,
		rhi::AccessToLayout(rhi::ResourceAccessType::CopySource, true), rhi::RenderSyncFromAccess(rhi::ResourceAccessType::CopySource)};
	const org::ResourceState copyDest{rhi::ResourceAccessType::CopyDest,
		rhi::AccessToLayout(rhi::ResourceAccessType::CopyDest, true), rhi::RenderSyncFromAccess(rhi::ResourceAccessType::CopyDest)};
	const std::array<org::ExternalClient::Access, 2> accesses{{{source.get(), {}, copySource}, {staging.get(), {}, copyDest}}};
	const rhi::TimelinePoint signal{m_readbackFence.GetHandle(), fenceValue};
	try {
		client.Submit(queueSlot, accesses, [&](rhi::CommandList& list) {
			list.CopyBufferRegion(staging->GetAPIResource().GetHandle(), 0, source->GetAPIResource().GetHandle(), 0, bytes);
		}, {&signal, 1});
	} catch (const std::exception& error) {
		spdlog::error("Texture streaming feedback readback submission failed: {}", error.what());
		cancel();
		return;
	}
	submitted();
}

void TextureStreamingManager::EnsureTextureUploadAdvanced(
	const std::shared_ptr<TextureAsset>& texture,
	TextureFactory& textureFactory)
{
	ZoneScopedN("TextureStreamingManager::EnsureTextureUploadAdvanced");
	if (!texture) {
		return;
	}

	auto previousImage = texture->ImagePtr();
	const uint64_t previousStreamingRevision = texture->GetStreamingStateRevision();
	ZoneValue(texture->GetStreamingTextureID());
	// Processing and DirectStorage completion can update the prepared image on their
	// own service threads between worker polls.  Comparing only the revision before
	// and after EnsureUploaded() therefore misses a ready replacement when the
	// revision was already bumped before this call began.  The publication boundary
	// is the authoritative test: any valid prepared image that differs from the
	// currently published image still needs main-thread adoption.
	const auto preparedImage = texture->PreparedImagePtr();
	const bool bindingNeedsAdoption =
		preparedImage &&
		preparedImage != previousImage &&
		preparedImage->HasValidBackingResource();
	const auto ownersIt = m_bindingIDsByStreamingTextureID.find(texture->GetStreamingTextureID());
	const bool hasAdoptionOwner = ownersIt != m_bindingIDsByStreamingTextureID.end() && !ownersIt->second.empty();
	const bool deferMetadataToAdoption = bindingNeedsAdoption && hasAdoptionOwner;
	if (!deferMetadataToAdoption && texture->GetStreamingStateRevision() != previousStreamingRevision) {
		ZoneScopedN("TextureStreamingManager::EnsureTextureUploadAdvanced::MarkStateDirty");
		MarkTextureStreamingMetadataDirty(texture, false, "upload_state_revision");
	}
	if (texture->HasPendingUploadWork()) {
		ZoneScopedN("TextureStreamingManager::EnsureTextureUploadAdvanced::MarkPendingDirty");
		if (deferMetadataToAdoption) {
			const uint32_t streamingTextureID = texture->GetStreamingTextureID();
			if (m_texturesNeedingUploadAdvanceSet.insert(streamingTextureID).second) {
				m_texturesNeedingUploadAdvance.push_back(streamingTextureID);
			}
			RecordTextureDirtyReason("upload_pending");
		}
		else {
			MarkTextureStreamingMetadataDirty(texture, true, "upload_pending");
		}
	}
	if (bindingNeedsAdoption && hasAdoptionOwner) {
		ZoneScopedN("TextureStreamingManager::EnsureTextureUploadAdvanced::NotifyBindingChanged");
		QueueBindingChanged(*texture, std::move(previousImage));
	}
	if (texture->HasTerminalLoadFailure()) {
		const auto gates = m_textureDisplayGatesRequested.find(texture->GetStreamingTextureID());
		if (gates == m_textureDisplayGatesRequested.end() || gates->second != kAllTextureDisplayGates) {
			QueueTextureImageTableMetadata(texture);
		}
	}
}

bool TextureStreamingManager::RequestPipelineResidency(const std::shared_ptr<TextureAsset>& texture)
{
	if (!m_rendererStateRequests || !m_pipelineServices || !texture->PipelineRequest()) return false;
	const uint32_t streamingTextureID = texture->GetStreamingTextureID();
	{
		std::lock_guard lock(m_pipelineMutex);
		if (!m_pipelineResidenciesInFlight.insert(streamingTextureID).second) return true;
	}
	{
		std::unique_lock lock(m_pipelineServices->mutex);
		if (!m_pipelineServices->notifySuspension) {
			m_pipelineServices->notifySuspension = m_rendererStateRequests->MakeSuspensionNotifier();
		}
	}
	const auto keyHash = br::render::TextureCacheKeyHash(texture->PipelineCacheKey());
	const auto processedAddress = br::render::TextureProcessedAddress(keyHash);
	if (m_rendererStateRequests->DesiredRevision(processedAddress) == 0) {
		auto processedInput = std::make_shared<br::render::TextureProcessedInput>();
		processedInput->request = texture->PipelineRequest();
		processedInput->cacheKey = texture->PipelineCacheKey();
		processedInput->services = m_pipelineServices;
		const auto processed = m_rendererStateRequests->SubmitLatest({
			processedAddress, 1u, {},
			br::render::ArtifactPayload::Make<br::render::TextureProcessedInput>(std::move(processedInput)),
			keyHash ^ 0x5450524f434553ull });
		if (processed.status == br::render::ArtifactRequestStatus::Accepted &&
			texture->PipelineRequest()->preparation == br::textures::TexturePreparation::Processed) {
			// A miss's result is written to the cache in the background; nothing waits for it.
			auto awaiter = m_rendererStateRequests->AwaitExact(processed.Handle(),
				br::render::ArtifactReadiness::CpuReady,
				TaskLane::Background, TaskDomain::TextureProcessing,
				[this, keyHash](const br::render::ArtifactSnapshot& snapshot) {
					const auto published = snapshot.payload.Get<br::render::PublishedTextureProcessed>();
					if (!published || !published->result || m_workerQuit.load(std::memory_order_acquire)) return;
					auto input = std::make_shared<br::render::TextureCacheWriteInput>();
					input->processed = published;
					(void)m_rendererStateRequests->SubmitLatest({
						br::render::TextureCacheWriteAddress(keyHash), 1u, {},
						br::render::ArtifactPayload::Make<br::render::TextureCacheWriteInput>(std::move(input)),
						keyHash ^ 0x54435752495445ull });
				},
				[](const br::render::ArtifactTermination&) {});
			if (awaiter.subscription != 0) {
				br::render::ArtifactAwaiter replaced;
				std::lock_guard lock(m_pipelineMutex);
				replaced = std::exchange(m_pipelineProcessedAwaiters[keyHash], std::move(awaiter));
			}
		}
	}

	auto input = std::make_shared<br::render::TextureResidencyInput>();
	input->texture = texture;
	input->processedKeyHash = keyHash;
	input->services = m_pipelineServices;
	const auto revision = ++m_pipelineResidencyRevisions[streamingTextureID];
	const auto request = m_rendererStateRequests->SubmitLatest({
		br::render::TextureResidencyAddress(streamingTextureID), revision,
		{ br::render::Latest(processedAddress, br::render::ArtifactReadiness::CpuReady) },
		br::render::ArtifactPayload::Make<br::render::TextureResidencyInput>(std::move(input)),
		(revision << 1u) ^ streamingTextureID ^ 0x54524553494445ull });
	if (!request) {
		std::lock_guard lock(m_pipelineMutex);
		m_pipelineResidenciesInFlight.erase(streamingTextureID);
		basic_telemetry::AddCounter("SARP.TexturePipeline.ResidencyRequestRejected");
		return false;
	}
	basic_telemetry::AddCounter("SARP.TexturePipeline.ResidencyRequested");
	const auto finish = [this, streamingTextureID](std::string failure) {
		{
			std::lock_guard lock(m_pipelineMutex);
			if (m_pipelineResidenciesInFlight.erase(streamingTextureID) == 0) return;
			m_completedPipelineResidencies.push_back({ streamingTextureID, std::move(failure) });
		}
		if (!m_workerQuit.load(std::memory_order_acquire)) ScheduleDrain();
	};
	auto awaiter = m_rendererStateRequests->AwaitExact(request.Handle(),
		br::render::ArtifactReadiness::CpuReady,
		TaskLane::Streaming, TaskDomain::TextureControl,
		[finish](const br::render::ArtifactSnapshot&) { finish({}); },
		[finish](const br::render::ArtifactTermination& termination) {
			finish(termination.error.empty() ? std::string("texture load terminated") : termination.error);
		});
	if (awaiter.subscription != 0) {
		// The replaced awaiter, an earlier residency's, is destroyed outside the lock.
		br::render::ArtifactAwaiter replaced;
		std::lock_guard lock(m_pipelineMutex);
		replaced = std::exchange(m_pipelineResidencyAwaiters[streamingTextureID], std::move(awaiter));
	}
	return true;
}

void TextureStreamingManager::AdvanceCompletedPipelineResidencies(TextureFactory& textureFactory)
{
	std::vector<CompletedPipelineResidency> completed;
	{
		std::lock_guard lock(m_pipelineMutex);
		completed.swap(m_completedPipelineResidencies);
	}
	for (auto& residency : completed) {
		const auto it = m_streamingTexturesByID.find(residency.streamingTextureID);
		if (it == m_streamingTexturesByID.end()) continue;
		const auto texture = it->second.lock();
		if (!texture) continue;
		if (!residency.failure.empty()) {
			// A failed texture's placeholder is final; its display gates open on the failure.
			texture->FailPipelineLoad(residency.failure);
			QueueTextureImageTableMetadata(texture);
			FinishPipelineLoad(residency.streamingTextureID, false);
			continue;
		}
		EnsureTextureUploadAdvanced(texture, textureFactory);
		if (!texture->NeedsPipelineResidency()) FinishPipelineLoad(residency.streamingTextureID, true);
	}
}

void TextureStreamingManager::LoadPipelineTexture(const std::shared_ptr<TextureAsset>& texture, std::function<void(bool)> ready)
{
	if (!texture->IsPipelineTexture() || texture->HasTerminalLoadFailure() ||
		(!texture->NeedsPipelineResidency() && texture->HasUsableImage())) {
		if (ready) ready(texture->HasUsableImage() && !texture->HasTerminalLoadFailure());
		return;
	}
	if (ready) {
		std::lock_guard lock(m_pipelineMutex);
		m_pipelineLoadCallbacks[texture->GetStreamingTextureID()].push_back(std::move(ready));
	}
	WorkerCommand command{};
	command.kind = WorkerCommand::Kind::Load;
	command.texture = texture;
	command.reason = "load_texture";
	QueueCommand(std::move(command));
}

void TextureStreamingManager::ApplyLoadCommand(const std::shared_ptr<TextureAsset>& texture)
{
	if (!texture) return;
	TrackTexture(texture);
	if (!texture->NeedsPipelineResidency()) {
		FinishPipelineLoad(texture->GetStreamingTextureID(), !texture->HasTerminalLoadFailure());
		return;
	}
	// Started now, not on the next frame tick: renderer initialization waits on some loads.
	if (!RequestPipelineResidency(texture)) MarkTextureStreamingMetadataDirty(texture, true, "load_texture_retry");
}

void TextureStreamingManager::FinishPipelineLoad(uint32_t streamingTextureID, bool loaded)
{
	std::vector<std::function<void(bool)>> callbacks;
	{
		std::lock_guard lock(m_pipelineMutex);
		const auto it = m_pipelineLoadCallbacks.find(streamingTextureID);
		if (it == m_pipelineLoadCallbacks.end()) return;
		callbacks.swap(it->second);
		m_pipelineLoadCallbacks.erase(it);
	}
	for (auto& callback : callbacks) callback(loaded);
}

void TextureStreamingManager::NotifyBindingChanged(TextureAsset& texture)
{
	QueueBindingChanged(texture, {});
}

void TextureStreamingManager::FinishBindingMailboxRequest(
	uint32_t streamingTextureID, const std::shared_ptr<TextureAsset>& texture)
{
	bool hasSuccessor = false;
	{
		std::scoped_lock lock(m_bindingMailboxMutex);
		m_activeBindingMailboxRequests.erase(streamingTextureID);
		hasSuccessor = m_dirtyBindingMailboxes.erase(streamingTextureID) != 0;
	}
	if (hasSuccessor && texture && !m_workerQuit.load(std::memory_order_acquire)) {
		EnqueueTextureUploadAdvance(texture, "binding_mailbox_successor");
	}
}

void TextureStreamingManager::QueueBindingChanged(TextureAsset& texture, std::shared_ptr<org::PixelBuffer> previousImage)
{
	ZoneScopedN("TextureStreamingManager::QueueBindingChanged");
	const uint32_t streamingTextureID = texture.GetStreamingTextureID();
	ZoneValue(streamingTextureID);
	auto ownersIt = m_bindingIDsByStreamingTextureID.find(streamingTextureID);
	if (ownersIt == m_bindingIDsByStreamingTextureID.end() || ownersIt->second.empty()) {
		++m_textureBindingChangedWithoutOwnerCount;
		spdlog::warn(
			"TextureStreamingManager: binding changed without registered owner textureID={} label='{}'",
			streamingTextureID,
			texture.GetPendingDebugInfo().label);
		return;
	}

	PendingBindingChange change{};
	change.streamingTextureID = streamingTextureID;
	const auto prepared = texture.GetPreparedBindingSnapshot();
	change.bindingRevision = prepared.streamingState.bindingRevision;
	change.streamingStateRevision = prepared.streamingState.stateRevision;
	change.queuedAt = std::chrono::steady_clock::now();
	change.texture = m_streamingTexturesByID[streamingTextureID].lock();
	change.previousImage = std::move(previousImage);
	change.newImage = prepared.image;
	change.metadata = BuildTextureStreamingGPUInfo(
		prepared.streamingState, texture.GetFullMip0Width(), texture.GetFullMip0Height());
	change.metadata.imageDescriptorIndex = TextureSrvIndex(change.newImage);
	if (!m_descriptorService) throw std::runtime_error("TextureStreamingManager: descriptor service unavailable while queuing binding");
	change.metadata.samplerDescriptorIndex = texture.SamplerDescriptorIndex(*m_descriptorService);
	if (!change.newImage) {
		return;
	}
	{
		std::scoped_lock lock(m_bindingMailboxMutex);
		if (m_activeBindingMailboxRequests.contains(streamingTextureID)) {
			m_dirtyBindingMailboxes.insert(streamingTextureID);
			basic_telemetry::AddCounter("SARP.TextureStreaming.BindingMailboxCoalesced");
			return;
		}
		m_activeBindingMailboxRequests.insert(streamingTextureID);
	}
	if (m_materialTextureTransfers) {
		change.transfer = m_materialTextureTransfers->EnsureShaderReady(change.newImage);
	}
	if (MaterialTextureStreamingTransitionLoggingEnabled()) {
		const auto pending = texture.GetPendingDebugInfo();
		spdlog::info(
			"MaterialTextureStreaming transition queued: textureID={} revision={} oldSrv={} newSrv={} residentTopMip={} pendingTopMip={} directStorage={} callbacks={} label='{}'",
			change.streamingTextureID,
			change.bindingRevision,
			TextureSrvIndex(change.previousImage),
			TextureSrvIndex(change.newImage),
			change.metadata.residentTopMip,
			change.metadata.pendingTopMip,
			pending.directStorageState,
			ownersIt->second.size(),
			pending.label);
	}
	// The streaming worker owns every binding request. Adoption runs in the
	// graph completion callback once the exact version reaches UploadSubmitted;
	// the renderer thread never polls, requeues or adopts bindings. Consumers
	// read the adopted descriptor through the published texture-image table.
	if (m_rendererStateRequests && change.transfer && change.transfer->gpuSubmissions) {
		auto input = std::make_shared<br::render::TextureBindingBuildInput>();
		input->streamingTextureID = change.streamingTextureID;
		input->bindingRevision = change.bindingRevision;
		input->streamingStateRevision = change.streamingStateRevision;
		input->samplerDescriptorIndex = texture.SamplerDescriptorIndex(*m_descriptorService);
		input->image = change.newImage;
		input->transfer = change.transfer;
		input->gpuSubmissions = change.transfer->gpuSubmissions;
		input->streamingMetadata = change.metadata;
		const br::render::ArtifactAddress address{
			br::render::ArtifactKind::TextureBinding, change.streamingTextureID, 0 };
		const auto request = m_rendererStateRequests->SubmitLatest({
			address, change.bindingRevision, {},
			br::render::ArtifactPayload::Make<br::render::TextureBindingBuildInput>(
				std::move(input)),
			(change.bindingRevision << 1u) ^ change.streamingStateRevision ^
				change.streamingTextureID ^ 0x54455842494e44ull });
		if (request) {
			change.graphRequested = true;
			change.graphVersion = request.version;
			auto pending = std::make_shared<PendingBindingChange>(std::move(change));
			const auto pendingTextureID = pending->streamingTextureID;
			auto awaiter = m_rendererStateRequests->AwaitExact(request.Handle(),
				br::render::ArtifactReadiness::UploadSubmitted,
				TaskLane::Streaming, TaskDomain::TextureControl,
				[this, pending](const br::render::ArtifactSnapshot&) mutable {
					if (m_workerQuit.load(std::memory_order_acquire)) {
						FinishBindingMailboxRequest(pending->streamingTextureID, pending->texture);
						return;
					}
					pending->graphReady = true;
					std::shared_ptr<org::PixelBuffer> replaced;
					if (!pending->texture || !pending->texture->PublishPreparedImage(
						pending->bindingRevision, pending->newImage, &replaced)) {
						if (pending->texture) {
							EnqueueTextureUploadAdvance(pending->texture, "stale_binding_publish");
						}
						FinishBindingMailboxRequest(pending->streamingTextureID, pending->texture);
						return;
					}
					EnqueueTextureMetadataRefresh(pending->texture, "graph_binding_published");
					// The image-table row (and the display gates evaluated with it)
					// follows every adopted binding, independent of mip streaming.
					QueueTextureImageTableMetadata(pending->texture);
					// Graph and manifest leases retain any still-consumed generation. The
					// displaced compatibility snapshot can enter deferred retirement now.
					if (replaced && replaced != pending->newImage) {
						org::DescriptorHeapManager::GetInstance().RetireResource(std::move(replaced));
					}
					basic_telemetry::AddCounter("SARP.TextureStreaming.BindingsAdopted");
					basic_telemetry::Record("SARP.TextureStreaming.BindingAdoptionLatencyUs",
						static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
							std::chrono::steady_clock::now() - pending->queuedAt).count()));
					FinishBindingMailboxRequest(pending->streamingTextureID, pending->texture);
				},
				[this, pending](const br::render::ArtifactTermination&) mutable {
					pending->graphReady = false;
					if (pending->texture &&
						!m_workerQuit.load(std::memory_order_acquire)) {
						(void)pending->texture->RejectPreparedImage(
							pending->bindingRevision, pending->newImage);
						EnqueueTextureUploadAdvance(pending->texture, "graph_binding_failed");
					}
					FinishBindingMailboxRequest(pending->streamingTextureID, pending->texture);
				});
			if (awaiter.subscription != 0) {
				std::lock_guard lock(m_graphBindingAwaiterMutex);
				m_graphBindingAwaiters.insert_or_assign(
					pendingTextureID, std::move(awaiter));
			}
			return;
		}
	}
	// A binding without a graph request cannot become renderer-visible. Keep the
	// previous published version and retry preparation unless shutdown is active.
	if (change.texture && !m_workerQuit.load(std::memory_order_acquire)) {
		(void)change.texture->RejectPreparedImage(change.bindingRevision, change.newImage);
		EnqueueTextureUploadAdvance(change.texture, "graph_binding_request_unavailable");
	}
	FinishBindingMailboxRequest(streamingTextureID, change.texture);
}

void TextureStreamingManager::PublishTextureImageTable()
{
	if (!m_textureImageTableDirty || !m_rendererStateRequests || !m_uploadService ||
		!m_textureImageTableFamily || m_textureImageTableEpoch == 0) return;
	if (m_textureImageTableHandle) {
		// The previous root's awaiter clears this flag at UploadSubmitted (or on
		// failure) and re-schedules the drain, so no graph query is needed here.
		if (m_textureImageTableBuildInFlight.load(std::memory_order_acquire)) {
			basic_telemetry::AddCounter("SARP.TextureStreaming.ImageTableCandidatesCoalesced");
			return;
		}
		const auto retirementEpoch = m_rendererStateRequests->VersionedBuffers().RetirementEpoch();
		if (retirementEpoch < m_lastTextureImageTableAdmissionRetirementEpoch +
			m_framesInFlight) {
			basic_telemetry::AddCounter("SARP.TextureStreaming.ImageTableRetirementPaced");
			return;
		}
	}
	auto capture = m_textureImageTableJournal.CaptureDesired();
	const auto buffer = m_textureImageTableFamily->RequestCapture(
		*m_rendererStateRequests, m_uploadService, m_textureImageTableEpoch,
		std::move(capture));
	if (!buffer) {
		basic_telemetry::AddCounter("SARP.TextureStreaming.ImageTableBufferRejected");
		return;
	}
	auto input = std::make_shared<br::render::TextureImageTableBuildInput>();
	input->contentEpoch = m_textureImageTableEpoch;
	input->logicalExtent = m_textureImageTableLogicalExtent;
	input->bufferKey = m_textureImageTableFamily->Configuration().address;
	input->bufferFamily = m_textureImageTableFamily;
	input->bindings = m_textureImageBindings;
	const auto root = m_rendererStateRequests->SubmitLatest({
		{ br::render::ArtifactKind::TextureImageTable, 0, 0 }, m_textureImageTableEpoch,
		// The image table is consumed through a published external SRV, outside
		// the upload graph that owns the copy submission.  Publishing at
		// UploadSubmitted exposes a newly reused backing before its copy fence has
		// completed; shaders then observe the previous table contents.  Require
		// GPU completion before the resolver can select this backing.
		{ br::render::Exact(buffer.version, br::render::ArtifactReadiness::GpuReady) },
		br::render::ArtifactPayload::Make<br::render::TextureImageTableBuildInput>(std::move(input)),
		m_textureImageTableEpoch ^ 0x544558494d475442ull });
	if (root) {
		// Display gates wait for this root (or a later one) in the graph: see AppendTextureDisplayGates.
		m_textureImageTableHandle = root.Handle();
		m_lastTextureImageTableAdmissionRetirementEpoch =
			m_rendererStateRequests->VersionedBuffers().RetirementEpoch();
		m_textureImageTableDirty = false;
		m_textureImageTableBuildInFlight.store(true, std::memory_order_release);
		auto awaiter = m_rendererStateRequests->AwaitExact(root.Handle(),
			br::render::ArtifactReadiness::UploadSubmitted,
			TaskLane::Streaming, TaskDomain::TextureControl,
			[this](const br::render::ArtifactSnapshot&) {
				m_textureImageTableBuildInFlight.store(false, std::memory_order_release);
				// A coalesced successor may be waiting; let the drain publish it.
				ScheduleDrain();
			},
			[this](const br::render::ArtifactTermination& termination) {
				// A refused or failed root must clear the in-flight flag too,
				// otherwise no later epoch is ever submitted. Its gates wait for
				// the successor root, which also contains their rows.
				m_textureImageTableBuildInFlight.store(false, std::memory_order_release);
				m_textureImageTableDirty = true;
				spdlog::warn("TextureStreamingManager: image table epoch terminated: {}",
					termination.error);
				basic_telemetry::AddCounter("SARP.TextureStreaming.ImageTableEpochTerminated");
				ScheduleDrain();
			});
		if (awaiter.subscription != 0) {
			std::lock_guard lock(m_textureImageTableAwaiterMutex);
			m_textureImageTableAwaiter = std::move(awaiter);
		} else {
			m_textureImageTableBuildInFlight.store(false, std::memory_order_release);
		}
		basic_telemetry::AddCounter("SARP.TextureStreaming.ImageTableEpochSubmitted");
		basic_telemetry::SetGauge("SARP.TextureStreaming.ImageTableDesiredEpoch",
			static_cast<std::int64_t>(m_textureImageTableEpoch));
		basic_telemetry::SetGauge("SARP.TextureStreaming.ImageTableLogicalExtent",
			static_cast<std::int64_t>(m_textureImageTableLogicalExtent));
	}
}

void TextureStreamingManager::AcknowledgePublishedImageTable(
	const std::shared_ptr<const br::render::PublishedRendererState>& published)
{
	if (!published) return;
	const auto table = published->textureImages.payload
		.Get<br::render::PublishedTextureImageTable>();
	if (!table || !table->table || table->contentEpoch == 0u) return;
	auto acknowledged = m_textureImageTableAcknowledgedEpoch.load(std::memory_order_acquire);
	while (table->contentEpoch > acknowledged) {
		if (m_textureImageTableAcknowledgedEpoch.compare_exchange_weak(
			acknowledged, table->contentEpoch, std::memory_order_acq_rel,
			std::memory_order_acquire)) {
			m_textureImageTableJournal.Acknowledge(table->table);
			basic_telemetry::SetGauge("SARP.TextureStreaming.ImageTableAcknowledgedEpoch",
				static_cast<std::int64_t>(table->contentEpoch));
			basic_telemetry::AddCounter("SARP.TextureStreaming.ImageTableJournalAcknowledged");
			return;
		}
	}
}

std::shared_ptr<org::Resource> TextureStreamingManager::ResolvePublishedImageTableResourceForDiagnostics() const
{
	if (!m_textureImageTableResolver) return {};
	auto resources = m_textureImageTableResolver->Resolve();
	return resources.empty() ? std::shared_ptr<org::Resource>{} : std::move(resources.front());
}

std::shared_ptr<org::Resource> TextureStreamingManager::PublishedImageTableReadbackAnchorForDiagnostics() const
{
	// Capture requests are attached while compiling the consumer pass, which
	// references this logical resource. Its resolver selects the published
	// backing that is actually bound for the pass.
	return m_textureStreamingMetadataBuffer;
}

bool TextureStreamingManager::RequestExternalMaterialTextureReadback(
	const std::shared_ptr<org::PixelBuffer>& image,
	std::wstring outputFile,
	std::function<void()> callback)
{
	if (!m_materialTextureTransfers || !image ||
		image->GetGraphOwnership() != org::Resource::GraphOwnership::ExternalImmutableShaderResource) {
		return false;
	}
	m_materialTextureTransfers->RequestReadback(image, std::move(outputFile), std::move(callback));
	return true;
}

void TextureStreamingManager::FlushDirtyTextureMetadata(const std::shared_ptr<TextureAsset>& texture)
{
	ZoneScopedN("TextureStreamingManager::FlushDirtyTextureMetadata");
	if (!texture) {
		return;
	}

	const uint32_t streamingTextureID = texture->GetStreamingTextureID();
	if (streamingTextureID == 0u) {
		return;
	}
	ZoneValue(streamingTextureID);

	const uint64_t revision = texture->GetStreamingStateRevision();
	auto revisionIt = m_textureStreamingMetadataRevisions.find(streamingTextureID);
	if (revisionIt != m_textureStreamingMetadataRevisions.end() && revisionIt->second == revision) {
		return;
	}

	if (!UpdateTextureStreamingMetadata(texture)) {
		if (m_dirtyTextureStreamingIDSet.insert(streamingTextureID).second) {
			m_dirtyTextureStreamingIDs.push_back(streamingTextureID);
		}
		return;
	}
	m_textureStreamingMetadataRevisions[streamingTextureID] = revision;
}

bool TextureStreamingManager::UpdateTextureStreamingMetadata(const std::shared_ptr<TextureAsset>& texture)
{
	ZoneScopedN("TextureStreamingManager::UpdateTextureStreamingMetadata");
	if (!texture) {
		return false;
	}

	const uint32_t streamingTextureID = texture->GetStreamingTextureID();
	if (streamingTextureID == 0u) {
		return false;
	}
	ZoneValue(streamingTextureID);

	const bool bootstrapMetadata =
		m_textureImageTableAcknowledgedEpoch.load(std::memory_order_acquire) == 0u;
	if ((bootstrapMetadata &&
		!m_textureStreamingMetadataBuffer->TryEnsureCapacityForIndex(streamingTextureID)) ||
		!m_textureStreamingFeedbackBuffer->TryEnsureCapacityForIndex(TextureFeedbackWord(streamingTextureID))) {
		return false;
	}

	if (bootstrapMetadata) {
		ZoneScopedN("TextureStreamingManager::UpdateTextureStreamingMetadata::UploadMetadata");
		if (!m_descriptorService) throw std::runtime_error("Texture streaming descriptor service generation is unavailable");
		if (!m_textureStreamingMetadataBuffer->TryUpdateAt(streamingTextureID,
			BuildTextureStreamingGPUInfo(*texture, *m_descriptorService))) {
			return false;
		}
	}
	{
		ZoneScopedN("TextureStreamingManager::UpdateTextureStreamingMetadata::ResetFeedback");
		if (!m_textureStreamingFeedbackBuffer->TryUpdateAt(static_cast<UINT>(TextureFeedbackWord(streamingTextureID)), kTextureStreamingFeedbackUnused)) {
			return false;
		}
	}
	if (auto image = texture->ImagePtr()) {
		ZoneScopedN("TextureStreamingManager::UpdateTextureStreamingMetadata::TrackImageResource");
		m_textureAssetsByImageResourceID[image->GetGlobalResourceID()] = texture;
	}
	{
		ZoneScopedN("TextureStreamingManager::UpdateTextureStreamingMetadata::TrackTexture");
		TrackTexture(texture);
	}
	{
	std::lock_guard feedbackIDsLock(m_activeFeedbackMutex);
	if (m_activeTextureStreamingFeedbackIDSet.insert(streamingTextureID).second) {
		ZoneScopedN("TextureStreamingManager::UpdateTextureStreamingMetadata::ActivateFeedbackID");
		m_activeTextureStreamingFeedbackIDs.push_back(streamingTextureID);
	}
	}
	// Stable IDs become visible to material and terrain rows before their first
	// image is necessarily available. Publish that state to the immutable image
	// table as well: an absent image is represented by UINT32_MAX and causes the
	// shader to use the row's direct descriptor fallback. Leaving a sparse row
	// unwritten zero-initialized imageDescriptorIndex, which incorrectly selects
	// descriptor 0 and lets RVT generation cache black pages.
	QueueTextureImageTableMetadata(texture);
	return true;
}

void TextureStreamingManager::QueueTextureImageTableMetadata(
	const std::shared_ptr<TextureAsset>& texture)
{
	if (!texture || texture->GetStreamingTextureID() == 0u) return;
	std::lock_guard lock(m_pendingTextureImageTableMetadataMutex);
	if (m_pendingTextureImageTableMetadataIDs.insert(
		texture->GetStreamingTextureID()).second) {
		m_pendingTextureImageTableMetadata.push_back(texture);
	}
}

void TextureStreamingManager::FlushPendingTextureImageTableMetadata()
{
	std::vector<std::weak_ptr<TextureAsset>> pending;
	{
		std::lock_guard lock(m_pendingTextureImageTableMetadataMutex);
		pending.swap(m_pendingTextureImageTableMetadata);
		m_pendingTextureImageTableMetadataIDs.clear();
	}
	if (pending.empty()) return;

	std::vector<br::render::ArtifactIntent> displayGates;
	for (const auto& weakTexture : pending) {
		const auto texture = weakTexture.lock();
		if (!texture) continue;
		const std::uint32_t streamingTextureID = texture->GetStreamingTextureID();
		if (streamingTextureID == 0u) continue;

		if (!m_descriptorService) throw std::runtime_error("Texture streaming descriptor service generation is unavailable");
		// One published snapshot names the row's image, its hold, and the display
		// gates, so a gate never releases against an image this row does not bind.
		const auto published = texture->GetPublishedBindingSnapshot();
		const TextureStreamingGPUInfo metadata =
			BuildPublishedTextureStreamingGPUInfo(*texture, published, *m_descriptorService);
		auto& descriptors = org::DescriptorHeapManager::GetInstance();
		const auto cleanup = descriptors.GetResourceCleanupQueue();
		org::BindingRecordBuilder recordBuilder(cleanup, sizeof(metadata));
		recordBuilder.WriteBytes(0, std::as_bytes(std::span{ &metadata, std::size_t{ 1 } }));
		if (metadata.imageDescriptorIndex != UINT32_MAX) {
			// Shaders read the table's images without the graph knowing, so each must be externally immutable.
			auto binding = org::OwnedDescriptorBinding::CaptureUndeclaredView(
				org::PublicationBindingBundle::Capture(*published.image), {}, descriptors.DeviceGeneration(), cleanup);
			if (binding.Index() != metadata.imageDescriptorIndex)
				throw std::logic_error("Texture table bytes and captured image binding disagree");
			recordBuilder.WriteBinding(offsetof(TextureStreamingGPUInfo, imageDescriptorIndex), std::move(binding));
		}
		if (metadata.samplerDescriptorIndex != UINT32_MAX) {
			recordBuilder.WriteBinding(offsetof(TextureStreamingGPUInfo, samplerDescriptorIndex),
				org::OwnedDescriptorBinding::CaptureSampler(metadata.samplerDescriptorIndex,
					descriptors.DeviceGeneration(), descriptors.GetSamplerHeap(), cleanup));
		}
		auto record = recordBuilder.Seal();
		org::BindingTableBuilder bindings(cleanup, m_textureImageBindings);
		bindings.Set(streamingTextureID, record);
		auto nextBindings = bindings.Seal();
		const auto desiredExtent = (std::max)(m_textureImageTableLogicalExtent,
			static_cast<std::uint64_t>(streamingTextureID) + 1u);
		m_textureImageTableJournal.RequestCapacity(desiredExtent);
		m_textureImageTableEpoch = m_textureImageTableJournal.AppendWrite(
			streamingTextureID,
			record->Bytes(),
			desiredExtent);
		m_textureImageTableLogicalExtent = desiredExtent;
		m_textureImageBindings = std::move(nextBindings);
		m_textureImageTableDirty = true;
		AppendTextureDisplayGates(*texture, published, displayGates);
	}
	SubmitTextureDisplayGates(std::move(displayGates));
}

void TextureStreamingManager::SubmitTextureDisplayGates(std::vector<br::render::ArtifactIntent> gates)
{
	if (gates.empty() || !m_rendererStateRequests) return;
	const auto submitted = static_cast<std::int64_t>(gates.size());
	for (const auto& result : m_rendererStateRequests->SubmitLatestBatch(std::move(gates))) {
		if (!result) basic_telemetry::AddCounter("SARP.TextureStreaming.DisplayGateRequestRejected");
	}
	basic_telemetry::AddCounter("SARP.TextureStreaming.DisplayGatesSubmitted", submitted);
}

void TextureStreamingManager::AppendTextureDisplayGates(const TextureAsset& texture,
	const TextureAsset::PublishedBindingSnapshot& published,
	std::vector<br::render::ArtifactIntent>& intents)
{
	const std::uint32_t streamingTextureID = texture.GetStreamingTextureID();
	auto& requested = m_textureDisplayGatesRequested[streamingTextureID];
	if (requested == kAllTextureDisplayGates) return;
	const bool loadFailed = texture.HasTerminalLoadFailure();
	const bool realImage = published.image && published.image->HasValidBackingResource();
	const bool anyImageReady = realImage || loadFailed;
	const bool alphaCoverageReady = loadFailed ||
		(realImage && published.streamingState.residency.residentTopMip <= AlphaTestedTopMipCap(texture));
	const auto append = [&](br::render::TextureDisplayQuality quality, bool ready) {
		const auto bit = static_cast<std::uint8_t>(1u << static_cast<unsigned>(quality));
		if (!ready || (requested & bit) != 0u) return;
		requested |= bit;
		auto input = std::make_shared<br::render::TextureDisplayGateInput>();
		input->streamingTextureID = streamingTextureID;
		input->quality = quality;
		input->bindingRevision = published.bindingRevision;
		input->loadFailed = loadFailed && !realImage;
		// Gates are latches and are never released: a consumer blocked on a released
		// address would fail terminally. Revisions only need to grow.
		// The gate waits for the first image-table root that contains this row (its
		// epoch was just journaled) to be submitted, so a renderable cannot become
		// visible before the table it samples through binds the image. The latch
		// holds that root: later roots never invalidate the gate or its consumers.
		const auto revision = ++m_textureDisplayGateRevision;
		intents.push_back({
			br::render::TextureDisplayGateAddress(streamingTextureID, quality), revision,
			{ br::render::ReadyGateAtLeast({ br::render::ArtifactKind::TextureImageTable, 0, 0 },
				m_textureImageTableEpoch, br::render::ArtifactReadiness::UploadSubmitted) },
			br::render::ArtifactPayload::Make<br::render::TextureDisplayGateInput>(std::move(input)),
			revision ^ 0x5445584741544500ull });
	};
	append(br::render::TextureDisplayQuality::AnyImage, anyImageReady);
	append(br::render::TextureDisplayQuality::AlphaCoverage, alphaCoverageReady);
}

void TextureStreamingManager::ProcessPendingTextureUpdates(uint64_t frameIndex, TextureFactory& textureFactory)
{
	const auto updateStart = std::chrono::steady_clock::now();

	const auto feedbackStart = std::chrono::steady_clock::now();
	BeginTextureStreamingFeedbackFrame(frameIndex);
	const auto feedbackEnd = std::chrono::steady_clock::now();

	std::vector<uint32_t> texturesToAdvance;
	texturesToAdvance.swap(m_texturesNeedingUploadAdvance);
	m_texturesNeedingUploadAdvanceSet.clear();
	const auto uploadStart = std::chrono::steady_clock::now();
	std::size_t uploadAdvanceVisited = 0;
	std::size_t uploadAdvanceAlive = 0;
	std::size_t uploadAdvanceStillPending = 0;
	std::size_t uploadAdvanceBindingChanged = 0;
	std::size_t uploadAdvanceStateChanged = 0;
	std::size_t pendingNoUsableImage = 0;
	std::size_t pendingPlaceholder = 0;
	std::size_t pendingStreamingReload = 0;
	std::size_t pendingProcessingHandle = 0;
	std::size_t pendingReloadHandle = 0;
	std::size_t pendingDirectStorageHandle = 0;
	std::vector<uint32_t> deferredTextureIDs;
	struct PendingTextureSample {
		uint32_t id = 0;
		uint32_t requestedTopMip = 0;
		uint32_t pendingTopMip = 0;
		uint32_t residentTopMip = 0;
		uint32_t directStorageTargetTopMip = 0;
		uint32_t residentMipCount = 0;
		uint32_t totalMipCount = 0;
		uint64_t stateRevision = 0;
		uint64_t bindingRevision = 0;
		const char* processingState = "None";
		const char* reloadState = "None";
		const char* directStorageState = "None";
		const char* loadPath = "unknown";
		const char* uploadPath = "unknown";
		bool cacheArtifact = false;
		std::string label;
		std::string name;
		std::string sourceIdentity;
		std::string filePath;
		std::string initialData;
	};
	std::vector<PendingTextureSample> pendingSamples;
	pendingSamples.reserve(8);
	constexpr auto cooperativePhaseBudget = std::chrono::milliseconds(2);
	for (std::size_t textureIndex = 0; textureIndex < texturesToAdvance.size(); ++textureIndex) {
		const uint32_t streamingTextureID = texturesToAdvance[textureIndex];
		++uploadAdvanceVisited;
		auto it = m_streamingTexturesByID.find(streamingTextureID);
		if (it == m_streamingTexturesByID.end()) {
			continue;
		}

		auto texture = it->second.lock();
		if (!texture) {
			m_streamingTexturesByID.erase(it);
			continue;
		}

		++uploadAdvanceAlive;
		if (texture->IsPipelineTexture()) {
			// A pipeline texture loads through its residency jobs, whose completion
			// advances it again (AdvanceCompletedPipelineResidencies).
			if (texture->NeedsPipelineResidency()) {
				if (!RequestPipelineResidency(texture)) deferredTextureIDs.push_back(streamingTextureID);
				continue;
			}
			// The residency is loaded, but its prepared image may still be unpublished:
			// a binding change coalesced behind an in-flight request, or one whose
			// publication went stale, is retried through this list. No later residency
			// completion will adopt it, and its image-table row and display gates follow
			// that adoption, so a renderable requiring the gates would wait forever.
			EnsureTextureUploadAdvanced(texture, textureFactory);
			continue;
		}
		const uint64_t previousBindingRevision = texture->GetBindingRevision();
		const uint64_t previousStreamingRevision = texture->GetStreamingStateRevision();
		EnsureTextureUploadAdvanced(texture, textureFactory);
		if (texture->GetBindingRevision() != previousBindingRevision) {
			++uploadAdvanceBindingChanged;
		}
		if (texture->GetStreamingStateRevision() != previousStreamingRevision) {
			++uploadAdvanceStateChanged;
		}
		if (texture->HasPendingUploadWork()) {
			++uploadAdvanceStillPending;
			// Async processing/reload/DirectStorage work commonly spans multiple
			// frames. Keep polling it until the replacement image is adopted;
			// otherwise a one-frame miss strands the old (often full-resolution)
			// image indefinitely even though a coarser mip was requested.
			deferredTextureIDs.push_back(streamingTextureID);
			const auto pending = texture->GetPendingDebugInfo();
			if (!pending.hasUsableImage) {
				++pendingNoUsableImage;
			}
			if (pending.hasPlaceholder) {
				++pendingPlaceholder;
			}
			if (pending.needsStreamingReload) {
				++pendingStreamingReload;
			}
			if (pending.hasProcessingHandle) {
				++pendingProcessingHandle;
			}
			if (pending.hasReloadHandle) {
				++pendingReloadHandle;
			}
			if (pending.hasDirectStorageHandle) {
				++pendingDirectStorageHandle;
			}
			if (pendingSamples.size() < 8) {
				pendingSamples.push_back(PendingTextureSample{
					.id = pending.streamingTextureID,
					.requestedTopMip = pending.requestedTopMip,
					.pendingTopMip = pending.pendingTopMip,
					.residentTopMip = pending.residentTopMip,
					.directStorageTargetTopMip = pending.directStorageTargetTopMip,
					.residentMipCount = pending.residentMipCount,
					.totalMipCount = pending.totalMipCount,
					.stateRevision = pending.stateRevision,
					.bindingRevision = pending.bindingRevision,
					.processingState = pending.processingState,
					.reloadState = pending.reloadState,
					.directStorageState = pending.directStorageState,
					.loadPath = pending.loadPath,
					.uploadPath = pending.uploadPath,
					.cacheArtifact = pending.isProcessingCacheArtifact,
					.label = pending.label,
					.name = pending.debugName,
					.sourceIdentity = pending.sourceIdentity,
					.filePath = pending.filePath,
					.initialData = pending.initialData
				});
			}
		}
		if (std::chrono::steady_clock::now() - uploadStart >= cooperativePhaseBudget) {
			deferredTextureIDs.insert(deferredTextureIDs.end(),
				texturesToAdvance.begin() + static_cast<std::ptrdiff_t>(textureIndex + 1u),
				texturesToAdvance.end());
			break;
		}
	}
	for (uint32_t streamingTextureID : deferredTextureIDs) {
		if (m_texturesNeedingUploadAdvanceSet.insert(streamingTextureID).second) {
			m_texturesNeedingUploadAdvance.push_back(streamingTextureID);
		}
	}
	const auto uploadEnd = std::chrono::steady_clock::now();

	std::vector<uint32_t> dirtyTextureIDs;
	dirtyTextureIDs.swap(m_dirtyTextureStreamingIDs);
	m_dirtyTextureStreamingIDSet.clear();
	const auto dirtyTextureStart = std::chrono::steady_clock::now();
	std::size_t dirtyTextureMetadataVisited = 0;
	std::size_t dirtyTextureMetadataAlive = 0;
	std::size_t dirtyTextureMetadataUpdated = 0;
	std::vector<uint32_t> deferredDirtyTextureIDs;
	for (std::size_t textureIndex = 0; textureIndex < dirtyTextureIDs.size(); ++textureIndex) {
		const uint32_t streamingTextureID = dirtyTextureIDs[textureIndex];
		++dirtyTextureMetadataVisited;
		auto it = m_streamingTexturesByID.find(streamingTextureID);
		if (it == m_streamingTexturesByID.end()) {
			continue;
		}

		auto texture = it->second.lock();
		if (!texture) {
			m_streamingTexturesByID.erase(it);
			m_textureStreamingMetadataRevisions.erase(streamingTextureID);
			continue;
		}

		++dirtyTextureMetadataAlive;
		const auto previousUploadedRevision = m_textureStreamingMetadataRevisions.find(streamingTextureID);
		const uint64_t previousRevision = previousUploadedRevision != m_textureStreamingMetadataRevisions.end()
			? previousUploadedRevision->second
			: std::numeric_limits<uint64_t>::max();
		FlushDirtyTextureMetadata(texture);
		const auto currentUploadedRevision = m_textureStreamingMetadataRevisions.find(streamingTextureID);
		if (currentUploadedRevision != m_textureStreamingMetadataRevisions.end() &&
			currentUploadedRevision->second != previousRevision) {
			++dirtyTextureMetadataUpdated;
		}
		if (std::chrono::steady_clock::now() - dirtyTextureStart >= cooperativePhaseBudget) {
			deferredDirtyTextureIDs.insert(deferredDirtyTextureIDs.end(),
				dirtyTextureIDs.begin() + static_cast<std::ptrdiff_t>(textureIndex + 1u),
				dirtyTextureIDs.end());
			break;
		}
	}
	for (const uint32_t streamingTextureID : deferredDirtyTextureIDs) {
		if (m_dirtyTextureStreamingIDSet.insert(streamingTextureID).second)
			m_dirtyTextureStreamingIDs.push_back(streamingTextureID);
	}
	const auto dirtyTextureEnd = std::chrono::steady_clock::now();

	const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - updateStart).count();
	const auto feedbackUs = std::chrono::duration_cast<std::chrono::microseconds>(feedbackEnd - feedbackStart).count();
	const auto uploadUs = std::chrono::duration_cast<std::chrono::microseconds>(uploadEnd - uploadStart).count();
	const auto dirtyTextureUs = std::chrono::duration_cast<std::chrono::microseconds>(dirtyTextureEnd - dirtyTextureStart).count();
	const auto now = std::chrono::steady_clock::now();
	const bool hadWork =
		uploadAdvanceVisited != 0 ||
		dirtyTextureMetadataVisited != 0 ||
		elapsedUs >= 2000;
	if (hadWork && now - m_lastTextureUpdateStatsLog >= std::chrono::seconds(1)) {
		m_lastTextureUpdateStatsLog = now;
		spdlog::debug(
			"TextureStreamingManager::ProcessPendingTextureUpdates stats: elapsed_us={} feedback_us={} upload_us={} dirtyTexture_us={} uploadAdvance visited={} alive={} stillPending={} bindingChanged={} stateChanged={} pending(noImage={} placeholder={} streamingReload={} processing={} reload={} directStorage={}) dirtyTextureMetadata visited={} alive={} updated={} activeStreamingTextures={} bindings={} bindingRefreshes={} bindingChangedWithoutOwner={} dirtyReasons(feedback={} idle={} trackBinding={} uploadState={} uploadPending={} other={})",
			elapsedUs,
			feedbackUs,
			uploadUs,
			dirtyTextureUs,
			uploadAdvanceVisited,
			uploadAdvanceAlive,
			uploadAdvanceStillPending,
			uploadAdvanceBindingChanged,
			uploadAdvanceStateChanged,
			pendingNoUsableImage,
			pendingPlaceholder,
			pendingStreamingReload,
			pendingProcessingHandle,
			pendingReloadHandle,
			pendingDirectStorageHandle,
			dirtyTextureMetadataVisited,
			dirtyTextureMetadataAlive,
			dirtyTextureMetadataUpdated,
			m_streamingTexturesByID.size(),
			m_bindingsByID.size(),
			m_textureBindingRefreshCount.load(std::memory_order_relaxed),
			m_textureBindingChangedWithoutOwnerCount,
			m_textureDirtyReasonFeedback,
			m_textureDirtyReasonIdleCoarsen,
			m_textureDirtyReasonTrackBinding,
			m_textureDirtyReasonUploadStateRevision,
			m_textureDirtyReasonUploadPending,
			m_textureDirtyReasonOther);
		for (const auto& sample : pendingSamples) {
			spdlog::debug(
				"TextureStreamingManager::ProcessPendingTextureUpdates pendingTexture: id={} label='{}' name='{}' source='{}' file='{}' initial='{}' cacheArtifact={} loadPath={} uploadPath={} requestedTopMip={} pendingTopMip={} residentTopMip={} directStorageTargetTopMip={} residentMipCount={} totalMipCount={} stateRevision={} bindingRevision={} processing={} reload={} directStorage={}",
				sample.id,
				sample.label,
				sample.name,
				sample.sourceIdentity,
				sample.filePath,
				sample.initialData,
				sample.cacheArtifact,
				sample.loadPath,
				sample.uploadPath,
				sample.requestedTopMip,
				sample.pendingTopMip,
				sample.residentTopMip,
				sample.directStorageTargetTopMip,
				sample.residentMipCount,
				sample.totalMipCount,
				sample.stateRevision,
				sample.bindingRevision,
				sample.processingState,
				sample.reloadState,
				sample.directStorageState);
		}
	}
	{
		// These only feed the debug menu and readiness counts, and walking every texture
		// took 7 ms per frame, so they refresh at a human rate rather than the frame rate.
		const auto now = std::chrono::steady_clock::now();
		if (now - m_lastStatsBuild >= std::chrono::milliseconds(100)) {
			m_lastStatsBuild = now;
			// Build outside the lock: the render thread reads the published stats
			// every frame and must never wait for this walk.
			auto built = BuildTextureStreamingStats();
			std::lock_guard statsLock(m_statsMutex);
			m_publishedStats = std::move(built);
			++m_publishedStatsSequence;
		}
	}

}

uint64_t TextureStreamingManager::PublishedStatsSequence() const noexcept
{
	std::lock_guard statsLock(m_statsMutex);
	return m_publishedStatsSequence;
}

MaterialTextureStreamingStats TextureStreamingManager::GetTextureStreamingStats(
	const std::vector<std::shared_ptr<org::Resource>>& activeTextureResources, uint64_t* sequence) const
{
	MaterialTextureStreamingStats stats;
	{
		std::lock_guard statsLock(m_statsMutex);
		stats = m_publishedStats;
		if (sequence) *sequence = m_publishedStatsSequence;
	}
	const std::unordered_set<uint64_t> participatingIDs(
		stats.participatingPublishedResourceIDs.begin(),
		stats.participatingPublishedResourceIDs.end());
	std::unordered_set<uint64_t> seenActiveIDs;
	for (const auto& resource : activeTextureResources) {
		auto image = std::dynamic_pointer_cast<org::PixelBuffer>(resource);
		if (!image || !seenActiveIDs.insert(image->GetGlobalResourceID()).second) {
			continue;
		}
		const uint64_t bytes = ComputeTextureResidentBytes(image->GetDescription());
		stats.activeMaterialResourceCount++;
		stats.activeMaterialResourceBytes += bytes;
		if (image->GetGraphOwnership() == org::Resource::GraphOwnership::ExternalImmutableShaderResource) {
			stats.externallyManagedActiveResourceCount++;
			stats.externallyManagedActiveResourceBytes += bytes;
		}
		else if (participatingIDs.contains(image->GetGlobalResourceID())) {
			stats.graphManagedParticipatingActiveResourceCount++;
			stats.graphManagedParticipatingActiveResourceBytes += bytes;
			static std::mutex diagnosticMutex;
			static std::unordered_set<uint64_t> reported;
			std::lock_guard diagnosticLock(diagnosticMutex);
			if (reported.insert(image->GetGlobalResourceID()).second) {
				spdlog::warn(
					"Participating material binding still references graph-managed image id={} name='{}'",
					image->GetGlobalResourceID(), image->GetName());
			}
		}
	}
	return stats;
}

MaterialTextureStreamingReadinessStats TextureStreamingManager::GetTextureStreamingReadinessStats() const
{
	std::lock_guard statsLock(m_statsMutex);
	return {
		.fullResolutionResidentTextureCount = m_publishedStats.fullResolutionResidentTextureCount,
		.pendingReloadTextureCount = m_publishedStats.pendingReloadTextureCount,
	};
}

MaterialTextureStreamingStats TextureStreamingManager::BuildTextureStreamingStats() const
{
	MaterialTextureStreamingStats stats{};
	stats.budget = m_budgetStats;
	std::unordered_set<uint64_t> seenImageResourceIDs;

	for (const auto& [streamingTextureIDKey, weakTexture] : m_streamingTexturesByID) {
		auto texture = weakTexture.lock();
		if (!texture || !texture->ImagePtr()) {
			continue;
		}
		// What the residency policy's demand asks of it, with no pull-back: its held report's mip, or none.
		uint32_t demandTopMip = UINT32_MAX;
		if (const auto policy = m_texturePolicy.find(streamingTextureIDKey);
			policy != m_texturePolicy.end() && policy->second.age < br::texturestreaming::kHoldTicks) {
			const auto shape = texture->GetStreamingState().residency;
			demandTopMip = br::texturestreaming::TopForFootprint(
				(std::max)(texture->GetFullMip0Width(), texture->GetFullMip0Height()), policy->second.heldFootprint,
				shape.totalMipCount > 0u ? shape.totalMipCount - 1u : 0u);
		}

		auto image = texture->ImagePtr();
		const uint64_t imageResourceID = image->GetGlobalResourceID();
		if (!seenImageResourceIDs.insert(imageResourceID).second) {
			continue;
		}
		stats.publishedResourceIDs.push_back(imageResourceID);
		if (texture->Meta().processing.isParticipatingMaterialTexture) {
			stats.participatingPublishedResourceIDs.push_back(imageResourceID);
		}
		if (auto preparedImage = texture->PreparedImagePtr();
			preparedImage &&
			preparedImage->GetGlobalResourceID() != imageResourceID &&
			preparedImage->HasValidBackingResource()) {
			stats.distinctPreparedTextureCount++;
			stats.distinctPreparedTextureBytes += ComputeTextureResidentBytes(preparedImage->GetDescription());
		}

		const TextureStreamingState& streamingState = texture->GetStreamingState();
		const uint32_t residentTopMip = streamingState.residency.residentTopMip;
		const uint32_t requestedTopMip = streamingState.requestedTopMip;
		const bool alphaTested =
			m_alphaTestedBindingCountsByStreamingTextureID.contains(texture->GetStreamingTextureID());
		const uint32_t streamingTextureID = texture->GetStreamingTextureID();
		const bool idleCoarseningDisabled =
			m_idleCoarseningDisabledBindingCountsByStreamingTextureID.contains(streamingTextureID);
		auto residencyConstraintIt =
			m_maximumResidentTopMipBindingCountsByStreamingTextureID.find(streamingTextureID);
		const bool residencyConstrained =
			residencyConstraintIt != m_maximumResidentTopMipBindingCountsByStreamingTextureID.end() &&
			!residencyConstraintIt->second.empty();
		const uint64_t residentBytes = ComputeTextureResidentBytes(image->GetDescription());
		const auto pendingInfo = texture->GetPendingDebugInfo();
		const auto& imageDesc = image->GetDescription();
		const auto residentDimensions = imageDesc.imageDimensions.empty()
			? org::ImageDimensions{}
			: imageDesc.imageDimensions.front();
		const uint32_t expectedResidentWidth =
			(std::max)(1u, texture->GetFullMip0Width() >> residentTopMip);
		const uint32_t expectedResidentHeight =
			(std::max)(1u, texture->GetFullMip0Height() >> residentTopMip);
		stats.largestResidentTextures.push_back(MaterialTextureStreamingRecord{
			.identifier = !pendingInfo.filePath.empty()
				? pendingInfo.filePath
				: (!pendingInfo.initialData.empty() ? pendingInfo.initialData : pendingInfo.label),
			.streamingTextureID = streamingTextureID,
			.imageDescriptorIndex = image->GetSRVInfo(0).slot.index,
			.imageResourceID = imageResourceID,
			.residentBytes = residentBytes,
			.residentWidth = residentDimensions.width,
			.residentHeight = residentDimensions.height,
			.expectedResidentWidth = expectedResidentWidth,
			.expectedResidentHeight = expectedResidentHeight,
			.totalMipCount = streamingState.residency.totalMipCount,
			.residentTopMip = residentTopMip,
			.residentMipCount = streamingState.residency.residentMipCount,
			.requestedTopMip = requestedTopMip,
			.feedbackTopMip = demandTopMip,
			.eligible = streamingState.eligible,
			.enabled = streamingState.enabled,
			.alphaTested = alphaTested,
		});
		stats.uniqueMaterialTextureCount++;
		if (residentDimensions.width != expectedResidentWidth ||
			residentDimensions.height != expectedResidentHeight) {
			stats.residentShapeMismatchTextureCount++;
			stats.residentShapeMismatchBytes += residentBytes;
		}
		stats.totalResidentBytes += residentBytes;
		if (idleCoarseningDisabled) stats.idleCoarseningDisabledTextureCount++;
		if (residencyConstrained) {
			stats.residencyConstrainedTextureCount++;
			const uint32_t mipCap = (std::min)(
				residencyConstraintIt->second.begin()->first,
				streamingState.residency.totalMipCount - 1u);
			if (residentTopMip > mipCap || requestedTopMip > mipCap) {
				stats.residencyConstraintViolationCount++;
			}
		}
		if (alphaTested) {
			stats.alphaTestedTextureCount++;
			const uint32_t mipCap = AlphaTestedTopMipCap(*texture);
			if (residentTopMip > mipCap || requestedTopMip > mipCap) {
				stats.alphaTestedMipCapViolationCount++;
			}
		}
		if (streamingState.eligible) {
			stats.uniqueStreamableTextureCount++;
			stats.streamableResidentBytes += residentBytes;
			if (streamingState.enabled) {
				stats.uniqueStreamingEnabledTextureCount++;
			}
			if (streamingState.residency.residentTopMip == 0u) {
				stats.streamableFullResolutionResidentTextureCount++;
			}
			if (streamingState.requestedTopMip != streamingState.residency.residentTopMip ||
				streamingState.pendingTopMip != streamingState.residency.residentTopMip) {
				stats.pendingReloadTextureCount++;
			}
		}
		if (residentTopMip == 0u) {
			stats.fullResolutionResidentTextureCount++;
		}
		if (stats.residentTopMipHistogram.size() <= residentTopMip) {
			stats.residentTopMipHistogram.resize(static_cast<size_t>(residentTopMip) + 1u, 0u);
		}
		stats.residentTopMipHistogram[residentTopMip]++;
		if (stats.requestedTopMipHistogram.size() <= requestedTopMip) {
			stats.requestedTopMipHistogram.resize(static_cast<size_t>(requestedTopMip) + 1u, 0u);
		}
		stats.requestedTopMipHistogram[requestedTopMip]++;
		if (demandTopMip == UINT32_MAX) {
			stats.texturesWithoutFeedback++;
		}
		else {
			if (stats.feedbackTopMipHistogram.size() <= demandTopMip) {
				stats.feedbackTopMipHistogram.resize(static_cast<size_t>(demandTopMip) + 1u, 0u);
			}
			stats.feedbackTopMipHistogram[demandTopMip]++;
		}
		if (stats.residentBytesByTopMip.size() <= residentTopMip) {
			stats.residentBytesByTopMip.resize(static_cast<size_t>(residentTopMip) + 1u, 0u);
		}
		stats.residentBytesByTopMip[residentTopMip] += residentBytes;
	}

	std::ranges::sort(
		stats.largestResidentTextures,
		std::greater<>{},
		&MaterialTextureStreamingRecord::residentBytes);
	// SARP_TEXTURE_STATS_DUMP=1: every 5 s, log one row per resident material texture. Logged rather than written to a
	// file: file creation under MO2's virtual filesystem stalled this thread for seconds.
	static const bool dumpStats = [] { const char* v = std::getenv("SARP_TEXTURE_STATS_DUMP"); return v && v[0] != '0' && v[0] != '\0'; }();
	static std::chrono::steady_clock::time_point lastDump{};
	if (dumpStats && std::chrono::steady_clock::now() - lastDump >= std::chrono::seconds(5)) {
		lastDump = std::chrono::steady_clock::now();
		const uint64_t dumpId = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(lastDump.time_since_epoch()).count());
		for (const auto& r : stats.largestResidentTextures) {
			spdlog::info("Texture stats row: dump={} id={} bytes={} width={} height={} mips={} residentTopMip={} requestedTopMip={} feedbackTopMip={} eligible={} enabled={} alphaTested={} identifier='{}'",
				dumpId, r.streamingTextureID, r.residentBytes, r.residentWidth, r.residentHeight, r.totalMipCount, r.residentTopMip, r.requestedTopMip,
				r.feedbackTopMip == UINT32_MAX ? -1 : static_cast<int>(r.feedbackTopMip), r.eligible, r.enabled, r.alphaTested, r.identifier);
		}
	}
	if (stats.largestResidentTextures.size() > 64u) {
		stats.largestResidentTextures.resize(64u);
	}
	std::ranges::sort(stats.publishedResourceIDs);

	return stats;
}

std::shared_ptr<org::Resource> TextureStreamingManager::ProvideResource(org::ResourceIdentifier const& key)
{
	auto it = m_resources.find(key);
	if (it == m_resources.end()) {
		return nullptr;
	}
	return it->second;
}

std::vector<org::ResourceIdentifier> TextureStreamingManager::GetSupportedKeys()
{
	std::vector<org::ResourceIdentifier> keys;
	keys.reserve(m_resources.size());
	for (auto const& [key, _] : m_resources) {
		keys.push_back(key);
	}
	return keys;
}

std::vector<org::ResourceIdentifier> TextureStreamingManager::GetSupportedResolverKeys()
{
	return { Builtin::Material::TextureStreamingMetadataBuffer };
}

std::shared_ptr<org::IResourceResolver> TextureStreamingManager::ProvideResolver(org::ResourceIdentifier const& key)
{
	if (key == Builtin::Material::TextureStreamingMetadataBuffer) return m_textureImageTableResolver;
	return nullptr;
}
