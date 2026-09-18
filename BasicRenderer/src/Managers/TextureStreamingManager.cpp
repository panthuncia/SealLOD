#include "Managers/TextureStreamingManager.h"

#include "../generated/BuiltinResources.h"
#include "Factories/TextureFactory.h"
#include "Managers/MaterialTextureTransferService.h"
#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Managers/Singletons/DescriptorHeapManager.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Materials/MaterialTextureStreaming.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Render/RendererStateRequestService.h"
#include "Render/RendererSettings.h"
#include "Render/TextureBindingArtifacts.h"
#include "Resources/Resolvers/PublishedStateResourceResolver.h"
#include "Render/Runtime/IReadbackService.h"
#include "Render/Runtime/IDescriptorService.h"
#include "RenderPasses/Base/CopyPass.h"
#include "Resources/Buffers/Buffer.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>

#include <tracy/Tracy.hpp>
#include <BasicTelemetry/Telemetry.h>

namespace {
	constexpr uint32_t kTextureStreamingFlagEligible = 1u << 0;
	constexpr uint32_t kTextureStreamingFlagEnabled = 1u << 1;
	constexpr uint32_t kTextureStreamingFeedbackUnused = 0xffffffffu;
	uint64_t TextureStreamingIdleFramesBeforeCoarsen() {
		return (std::max<uint64_t>)(
			1u,
			SettingsManager::GetInstance().getSettingGetter<uint32_t>(
				MaterialTextureStreamingIdleFramesSettingName)());
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

	uint32_t TextureSrvIndex(const std::shared_ptr<PixelBuffer>& image) {
		return image && image->HasValidBackingResource()
			? image->GetSRVInfo(0).slot.index
			: UINT32_MAX;
	}

	uint64_t ComputeTextureResidentBytes(const TextureDescription& desc) {
		uint64_t totalBytes = 0;
		for (const ImageDimensions& dims : desc.imageDimensions) {
			totalBytes += dims.slicePitch;
		}
		return totalBytes;
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

	struct MaterialTextureStreamingReadbackInputs {
		std::shared_ptr<Resource> source;
		RG_DEFINE_PASS_INPUTS(MaterialTextureStreamingReadbackInputs, &MaterialTextureStreamingReadbackInputs::source);
	};

	struct MaterialTextureStreamingReadbackFrameData {
		org::PreparedResourceReference source{}, destination{};
		uint64_t bytes = 0;
	};

	class MaterialReadbackReservation final : public org::PreparedLifecycleEffect {
	public:
		MaterialReadbackReservation(ExternalTimelinePoint signal,
			std::function<void()> submitted, std::function<void()> cancelled)
			: m_signal(signal), m_submitted(std::move(submitted)), m_cancelled(std::move(cancelled)) {}
		std::span<const ExternalTimelinePoint> SignalsAfterCompletion() const override { return {&m_signal, 1u}; }
		void Submitted(org::SubmissionContext) const override {
			if (!m_resolved.exchange(true) && m_submitted) m_submitted();
		}
		void Abandoned(org::AbandonReason) const override {
			if (!m_resolved.exchange(true) && m_cancelled) m_cancelled();
		}
	private:
		ExternalTimelinePoint m_signal{};
		std::function<void()> m_submitted, m_cancelled;
		mutable std::atomic<bool> m_resolved{false};
	};

	class MaterialTextureStreamingReadbackPass final
		: public org::TypedRenderGraphPass<MaterialTextureStreamingReadbackPass,
			  MaterialTextureStreamingReadbackFrameData> {
	public:
		MaterialTextureStreamingReadbackPass(
			std::shared_ptr<Resource> source,
			std::shared_ptr<Buffer> staging,
			uint64_t bytes,
			ExternalTimelinePoint signal,
			std::function<void()> submitted,
			std::function<void()> cancel)
			: m_source(std::move(source)), m_staging(std::move(staging)), m_bytes(bytes),
			  m_signal(signal), m_submitted(std::move(submitted)),
			  m_cancel(std::move(cancel)) {
		}

		void Declare(org::PassBuilder& builder) {
			builder.WithCopySource(m_source);
			builder.WithCopyDest(m_staging);
			builder.PreferQueue(QueueKind::Copy);
		}
		MaterialTextureStreamingReadbackFrameData Prepare(const org::PassPrepareContext& preparation) {
			if (!m_source || !m_staging || m_bytes == 0) return {};
			preparation.Reserve(std::make_shared<MaterialReadbackReservation>(
				m_signal, m_submitted, m_cancel));
			return {preparation.CaptureResource(m_source->GetGlobalResourceID()),
				preparation.CaptureResource(m_staging->GetGlobalResourceID()), m_bytes};
		}
		static void Record(const MaterialTextureStreamingReadbackFrameData& frame,
			org::PassRecordContext& recording) {
			if (frame.bytes) recording.Commands().CopyBufferRegion(
				recording.Resolve(frame.destination).GetHandle(), 0,
				recording.Resolve(frame.source).GetHandle(), 0, frame.bytes);
		}
	private:
		std::shared_ptr<Resource> m_source;
		std::shared_ptr<Buffer> m_staging;
		uint64_t m_bytes = 0;
		ExternalTimelinePoint m_signal{};
		std::function<void()> m_submitted;
		std::function<void()> m_cancel;
	};
}

TextureStreamingManager::TextureStreamingManager()
{
	m_readbackCallbackState->owner = this;
	TextureStreamingGPUInfo fallback{};
	m_textureImageTableJournal.Initialize(
		std::as_bytes(std::span{ &fallback, std::size_t{ 1 } }), 1, 1);
	m_textureImageTableFamily = std::make_shared<br::render::VersionedBufferFamily>(
		br::render::VersionedBufferFamily::Config{
			.address = { br::render::ArtifactKind::BufferVersion, 0,
				br::render::kTextureImageTableBufferVariant },
			.debugName = "PublishedTextureImageTable",
			.elementStride = sizeof(TextureStreamingGPUInfo),
			.catalogOwner = br::render::PublishedFragmentKind::TextureImages,
			.catalogUsage = br::render::PublishedResourceUsage::ShaderResource,
			.catalogVariant = br::render::kTextureImageTableBufferVariant });
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
				m_taskScope, TaskLane::Streaming, TaskDomain::TextureProcessing,
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
			}
		}

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
		std::shared_ptr<Buffer> staging;
		std::vector<uint32_t> activeIDs;
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
				.activeIDs = slot.activeStreamingTextureIDs});
		}
	}
	for (auto& completedSlot : completedSlots) {
		void* mapped = nullptr;
		completedSlot.staging->GetAPIResource().Map(&mapped);
		if (mapped) {
			std::lock_guard feedbackLock(m_textureStreamingFeedbackMutex);
			const size_t wordCount = static_cast<size_t>(completedSlot.copyBytes / sizeof(uint32_t));
			for (uint32_t streamingTextureID : completedSlot.activeIDs) {
				if (streamingTextureID >= wordCount) continue;
				uint32_t requestedTopMip = kTextureStreamingFeedbackUnused;
				std::memcpy(&requestedTopMip,
					static_cast<const std::byte*>(mapped) + static_cast<size_t>(streamingTextureID) * sizeof(uint32_t),
					sizeof(uint32_t));
				if (requestedTopMip != kTextureStreamingFeedbackUnused) {
					m_pendingTextureStreamingFeedback.emplace_back(streamingTextureID, requestedTopMip);
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
		cappedTopMip = (std::min)(cappedTopMip, GetAlphaTestedMaterialTextureMaxResidentTopMipSetting());
	}
	if (auto capIt = m_maximumResidentTopMipBindingCountsByStreamingTextureID.find(streamingTextureID);
		capIt != m_maximumResidentTopMipBindingCountsByStreamingTextureID.end() && !capIt->second.empty()) {
		cappedTopMip = (std::min)(cappedTopMip, capIt->second.begin()->first);
	}
	if (texture->GetStreamingState().requestedTopMip > cappedTopMip) {
		(void)texture->ApplyStreamingSystemRequest(cappedTopMip, 0u, true);
	}
	TrackTexture(texture);
	MarkTextureStreamingMetadataDirty(texture, true, "track_binding");
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
		const auto diagnostic = m_rendererStateRequests->Diagnose(address);
		if (published.image && published.bindingRevision != 0u &&
			published.image->HasValidBackingResource() &&
			diagnostic.desiredRevision < published.bindingRevision) {
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

void TextureStreamingManager::BeginTextureStreamingFeedbackFrame(uint64_t frameIndex)
{
	if (!IsMaterialTextureStreamingEnabledSetting()) {
		return;
	}

	std::vector<std::pair<uint32_t, uint32_t>> pendingFeedback;
	{
		std::lock_guard lock(m_textureStreamingFeedbackMutex);
		pendingFeedback.swap(m_pendingTextureStreamingFeedback);
	}

	std::vector<uint32_t> expiredTextureIDs;
	for (const auto& [streamingTextureID, requestedTopMip] : pendingFeedback) {
		auto it = m_streamingTexturesByID.find(streamingTextureID);
		if (it == m_streamingTexturesByID.end()) {
			continue;
		}

		auto texture = it->second.lock();
		if (!texture) {
			expiredTextureIDs.push_back(streamingTextureID);
			continue;
		}

		if (!texture->IsMipStreamingEnabled()) {
			continue;
		}

		uint32_t policyTopMip = requestedTopMip;
		if (m_alphaTestedBindingCountsByStreamingTextureID.contains(streamingTextureID)) {
			policyTopMip = (std::min)(
				policyTopMip,
				GetAlphaTestedMaterialTextureMaxResidentTopMipSetting());
		}
		if (auto capIt = m_maximumResidentTopMipBindingCountsByStreamingTextureID.find(streamingTextureID);
			capIt != m_maximumResidentTopMipBindingCountsByStreamingTextureID.end() && !capIt->second.empty()) {
			policyTopMip = (std::min)(policyTopMip, capIt->second.begin()->first);
		}
		const uint64_t previousRevision = texture->GetStreamingStateRevision();
		const bool needsUploadAdvance = texture->ApplyStreamingSystemRequest(policyTopMip, frameIndex);
		if (texture->GetStreamingStateRevision() != previousRevision) {
			MarkTextureStreamingMetadataDirty(texture, needsUploadAdvance, "feedback");
		}
	}

	for (uint32_t streamingTextureID : expiredTextureIDs) {
		m_streamingTexturesByID.erase(streamingTextureID);
	}

	const uint64_t idleFramesBeforeCoarsen = TextureStreamingIdleFramesBeforeCoarsen();
	for (auto it = m_streamingTexturesByID.begin(); it != m_streamingTexturesByID.end();) {
		auto texture = it->second.lock();
		if (!texture) {
			it = m_streamingTexturesByID.erase(it);
			continue;
		}

		if (!texture->IsMipStreamingEnabled()) {
			++it;
			continue;
		}

		const TextureStreamingState& state = texture->GetStreamingState();
		uint32_t bindingMipCap = state.residency.totalMipCount - 1u;
		if (auto capIt = m_maximumResidentTopMipBindingCountsByStreamingTextureID.find(it->first);
			capIt != m_maximumResidentTopMipBindingCountsByStreamingTextureID.end() && !capIt->second.empty()) {
			bindingMipCap = (std::min)(bindingMipCap, capIt->second.begin()->first);
		}
		if (m_alphaTestedBindingCountsByStreamingTextureID.contains(it->first)) {
			bindingMipCap = (std::min)(
				bindingMipCap,
				GetAlphaTestedMaterialTextureMaxResidentTopMipSetting());
		}
		if (state.requestedTopMip > bindingMipCap ||
			state.pendingTopMip > bindingMipCap ||
			state.residency.residentTopMip > bindingMipCap) {
				const uint64_t previousRevision = texture->GetStreamingStateRevision();
				const bool needsUploadAdvance =
					texture->ApplyStreamingSystemRequest(bindingMipCap, 0u, true);
				if (texture->GetStreamingStateRevision() != previousRevision) {
					MarkTextureStreamingMetadataDirty(
						texture,
						needsUploadAdvance,
						"binding_mip_cap");
				}
				++it;
				continue;
		}
		if (m_idleCoarseningDisabledBindingCountsByStreamingTextureID.contains(it->first)) {
			++it;
			continue;
		}
		if (state.lastSeenFrame == 0u ||
			frameIndex <= state.lastSeenFrame + idleFramesBeforeCoarsen) {
			++it;
			continue;
		}

		// Once a texture has been unseen for the full idle interval, jump directly to
		// its terminal coarse mip.  Rebuilding every intermediate one-mip window made
		// an idle scene cycle allocate and retire the entire texture set repeatedly.
		uint32_t coarsenedTopMip = state.residency.totalMipCount - 1u;
		if (m_alphaTestedBindingCountsByStreamingTextureID.contains(it->first)) {
			coarsenedTopMip = (std::min)(
				coarsenedTopMip,
				GetAlphaTestedMaterialTextureMaxResidentTopMipSetting());
		}
		if (coarsenedTopMip != state.requestedTopMip) {
			const uint64_t previousRevision = texture->GetStreamingStateRevision();
			const bool needsUploadAdvance = texture->ApplyStreamingSystemRequest(coarsenedTopMip, frameIndex, true);
			if (texture->GetStreamingStateRevision() != previousRevision) {
				MarkTextureStreamingMetadataDirty(texture, needsUploadAdvance, "idle_coarsen");
			}
		}

		++it;
	}

	std::vector<uint32_t> activeFeedbackIDs;
	{
		std::lock_guard feedbackIDsLock(m_activeFeedbackMutex);
		activeFeedbackIDs = m_activeTextureStreamingFeedbackIDs;
	}
	if (!activeFeedbackIDs.empty()) {
		// Reset one dense range instead of staging one four-byte upload per active
		// texture.  Sparse per-ID resets generated hundreds of CopyBufferRegion calls
		// every feedback frame and dominated both FlushClient and upload command
		// recording despite transferring only a few KiB.
		const uint32_t lastActiveID = *std::max_element(activeFeedbackIDs.begin(), activeFeedbackIDs.end());
		std::vector<uint32_t> resetValues(static_cast<size_t>(lastActiveID) + 1u, kTextureStreamingFeedbackUnused);
		m_textureStreamingFeedbackBuffer->StageRange(0u, resetValues);
	}
}

std::shared_ptr<RenderPass> TextureStreamingManager::CreateTextureStreamingFeedbackReadbackPass()
{
	// Diagnostic escape hatch for isolating unrelated render-graph failures. It
	// suppresses only the feedback copy/readback; texture publication and uploads
	// continue normally.
	if (const char* disabled = std::getenv("SARP_DISABLE_MATERIAL_TEXTURE_STREAMING_READBACK");
		disabled && disabled[0] != '\0' && disabled[0] != '0') {
		return {};
	}
	if (!IsMaterialTextureStreamingEnabledSetting() || !m_readbackFence.IsValid() || !m_textureStreamingFeedbackBuffer) {
		return {};
	}
	std::vector<uint32_t> activeIDs;
	{
		std::lock_guard lock(m_activeFeedbackMutex);
		activeIDs = m_activeTextureStreamingFeedbackIDs;
	}
	if (activeIDs.empty()) return {};

	uint64_t bytes = 0;
	if (!m_textureStreamingFeedbackBuffer->TryGetBufferByteSize(bytes) || bytes == 0) return {};
	uint32_t selectedSlot = UINT32_MAX;
	std::shared_ptr<Buffer> staging;
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
			return {};
		}
		auto& slot = m_readbackSlots[selectedSlot];
		if (!slot.staging || slot.capacityBytes < bytes) {
			slot.staging = Buffer::CreateShared(rhi::HeapType::Readback, bytes);
			slot.staging->SetName(("MaterialTextureStreamingReadback_" + std::to_string(selectedSlot)).c_str());
			org::memory::SetResourceUsageHint(*slot.staging, "Material texture streaming readback");
			slot.capacityBytes = bytes;
		}
		slot.activeStreamingTextureIDs = std::move(activeIDs);
		slot.inFlight = true;
		slot.copyBytes = bytes;
		slot.fenceValue = 0;
		staging = slot.staging;
		m_readbackSlotCursor = (selectedSlot + 1u) % static_cast<uint32_t>(m_readbackSlots.size());
	}

	std::shared_ptr<Resource> source = m_textureStreamingFeedbackBuffer;
	const uint64_t fenceValue = m_readbackFenceCounter.fetch_add(1u, std::memory_order_acq_rel) + 1u;
	return std::make_shared<MaterialTextureStreamingReadbackPass>(
		std::move(source), std::move(staging), bytes, ExternalTimelinePoint{m_readbackFence, fenceValue},
		[state = m_readbackCallbackState, selectedSlot, fenceValue]() {
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
		},
		[state = m_readbackCallbackState, selectedSlot]() {
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
		});
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
	{
		ZoneScopedN("TextureStreamingManager::EnsureTextureUploadAdvanced::SetGenerateMipmaps");
		texture->SetGenerateMipmaps(true);
	}
	{
		ZoneScopedN("TextureStreamingManager::EnsureTextureUploadAdvanced::TextureEnsureUploaded");
		texture->EnsureUploaded(textureFactory, TextureUploadAdvanceMode::NonBlocking);
	}

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

void TextureStreamingManager::QueueBindingChanged(TextureAsset& texture, std::shared_ptr<PixelBuffer> previousImage)
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
				TaskLane::Streaming, TaskDomain::TextureProcessing,
				[this, pending](const br::render::ArtifactSnapshot& snapshot) mutable {
					if (m_workerQuit.load(std::memory_order_acquire)) {
						FinishBindingMailboxRequest(pending->streamingTextureID, pending->texture);
						return;
					}
					pending->graphReady = br::render::ArtifactReachedMilestone(
						snapshot.readiness, br::render::ArtifactReadiness::UploadSubmitted);
					if (!pending->graphReady) {
						if (pending->texture) {
							(void)pending->texture->RejectPreparedImage(
								pending->bindingRevision, pending->newImage);
							EnqueueTextureUploadAdvance(pending->texture, "graph_binding_failed");
						}
						FinishBindingMailboxRequest(pending->streamingTextureID, pending->texture);
						return;
					}
					std::shared_ptr<PixelBuffer> replaced;
					if (!pending->texture || !pending->texture->PublishPreparedImage(
						pending->bindingRevision, pending->newImage, &replaced)) {
						if (pending->texture) {
							EnqueueTextureUploadAdvance(pending->texture, "stale_binding_publish");
						}
						FinishBindingMailboxRequest(pending->streamingTextureID, pending->texture);
						return;
					}
					EnqueueTextureMetadataRefresh(pending->texture, "graph_binding_published");
					// Graph and manifest leases retain any still-consumed generation. The
					// displaced compatibility snapshot can enter deferred retirement now.
					if (replaced && replaced != pending->newImage) {
						DescriptorHeapManager::GetInstance().RetireResource(std::move(replaced));
					}
					basic_telemetry::AddCounter("SARP.TextureStreaming.BindingsAdopted");
					basic_telemetry::Record("SARP.TextureStreaming.BindingAdoptionLatencyUs",
						static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
							std::chrono::steady_clock::now() - pending->queuedAt).count()));
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
		const auto retirementEpoch = br::render::VersionedGpuBufferFrameRetirementEpoch();
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
	input->holdChunks = m_textureImageHoldChunks;
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
		m_textureImageTableHandle = root.Handle();
		m_lastTextureImageTableAdmissionRetirementEpoch =
			br::render::VersionedGpuBufferFrameRetirementEpoch();
		m_textureImageTableDirty = false;
		m_textureImageTableBuildInFlight.store(true, std::memory_order_release);
		auto awaiter = m_rendererStateRequests->AwaitExact(root.Handle(),
			br::render::ArtifactReadiness::UploadSubmitted,
			TaskLane::Streaming, TaskDomain::TextureProcessing,
			[this](const br::render::ArtifactSnapshot&) {
				m_textureImageTableBuildInFlight.store(false, std::memory_order_release);
				// A coalesced successor may be waiting; let the drain publish it.
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

std::shared_ptr<Resource> TextureStreamingManager::ResolvePublishedImageTableResourceForDiagnostics() const
{
	if (!m_textureImageTableResolver) return {};
	auto resources = m_textureImageTableResolver->Resolve();
	return resources.empty() ? std::shared_ptr<Resource>{} : std::move(resources.front());
}

std::shared_ptr<Resource> TextureStreamingManager::PublishedImageTableReadbackAnchorForDiagnostics() const
{
	// Capture requests are attached while compiling the consumer pass, which
	// references this logical resource. Its resolver selects the published
	// backing that is actually bound for the pass.
	return m_textureStreamingMetadataBuffer;
}

bool TextureStreamingManager::RequestExternalMaterialTextureReadback(
	const std::shared_ptr<PixelBuffer>& image,
	std::wstring outputFile,
	std::function<void()> callback)
{
	if (!m_materialTextureTransfers || !image ||
		image->GetGraphOwnership() != Resource::GraphOwnership::ExternalImmutableShaderResource) {
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
		!m_textureStreamingFeedbackBuffer->TryEnsureCapacityForIndex(streamingTextureID)) {
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
		if (!m_textureStreamingFeedbackBuffer->TryUpdateAt(streamingTextureID, kTextureStreamingFeedbackUnused)) {
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

	for (const auto& weakTexture : pending) {
		const auto texture = weakTexture.lock();
		if (!texture) continue;
		const std::uint32_t streamingTextureID = texture->GetStreamingTextureID();
		if (streamingTextureID == 0u) continue;

		if (!m_descriptorService) throw std::runtime_error("Texture streaming descriptor service generation is unavailable");
		const TextureStreamingGPUInfo metadata = BuildTextureStreamingGPUInfo(*texture, *m_descriptorService);
		const auto desiredExtent = (std::max)(m_textureImageTableLogicalExtent,
			static_cast<std::uint64_t>(streamingTextureID) + 1u);
		m_textureImageTableJournal.RequestCapacity(desiredExtent);
		m_textureImageTableEpoch = m_textureImageTableJournal.AppendWrite(
			streamingTextureID,
			std::as_bytes(std::span{ &metadata, std::size_t{ 1 } }),
			desiredExtent);
		m_textureImageTableLogicalExtent = desiredExtent;

		const std::size_t chunkIndex = streamingTextureID /
			br::render::kTextureImageHoldChunkSize;
		const std::size_t entryIndex = streamingTextureID %
			br::render::kTextureImageHoldChunkSize;
		if (m_textureImageHoldChunks.size() <= chunkIndex) {
			m_textureImageHoldChunks.resize(chunkIndex + 1u);
		}
		auto chunk = m_textureImageHoldChunks[chunkIndex]
			? std::make_shared<br::render::TextureImageHoldChunk>(
				*m_textureImageHoldChunks[chunkIndex])
			: std::make_shared<br::render::TextureImageHoldChunk>();
		chunk->images[entryIndex] = texture->ImagePtr();
		m_textureImageHoldChunks[chunkIndex] = std::move(chunk);
		m_textureImageTableDirty = true;
	}
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
		// Build outside the lock: the render thread reads the published stats
		// every frame and must never wait for this walk.
		auto built = BuildTextureStreamingStats();
		std::lock_guard statsLock(m_statsMutex);
		m_publishedStats = std::move(built);
		++m_publishedStatsSequence;
	}

}

uint64_t TextureStreamingManager::PublishedStatsSequence() const noexcept
{
	std::lock_guard statsLock(m_statsMutex);
	return m_publishedStatsSequence;
}

MaterialTextureStreamingStats TextureStreamingManager::GetTextureStreamingStats(
	const std::vector<std::shared_ptr<Resource>>& activeTextureResources, uint64_t* sequence) const
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
		auto image = std::dynamic_pointer_cast<PixelBuffer>(resource);
		if (!image || !seenActiveIDs.insert(image->GetGlobalResourceID()).second) {
			continue;
		}
		const uint64_t bytes = ComputeTextureResidentBytes(image->GetDescription());
		stats.activeMaterialResourceCount++;
		stats.activeMaterialResourceBytes += bytes;
		if (image->GetGraphOwnership() == Resource::GraphOwnership::ExternalImmutableShaderResource) {
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
	std::unordered_set<uint64_t> seenImageResourceIDs;

	for (const auto& [_, weakTexture] : m_streamingTexturesByID) {
		auto texture = weakTexture.lock();
		if (!texture || !texture->ImagePtr()) {
			continue;
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
			? ImageDimensions{}
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
			.feedbackTopMip = streamingState.lastFeedbackTopMip,
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
			const uint32_t mipCap = (std::min)(
				GetAlphaTestedMaterialTextureMaxResidentTopMipSetting(),
				streamingState.residency.totalMipCount - 1u);
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
		if (streamingState.lastFeedbackTopMip == UINT32_MAX) {
			stats.texturesWithoutFeedback++;
		}
		else {
			if (stats.feedbackTopMipHistogram.size() <= streamingState.lastFeedbackTopMip) {
				stats.feedbackTopMipHistogram.resize(
					static_cast<size_t>(streamingState.lastFeedbackTopMip) + 1u,
					0u);
			}
			stats.feedbackTopMipHistogram[streamingState.lastFeedbackTopMip]++;
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
	if (stats.largestResidentTextures.size() > 64u) {
		stats.largestResidentTextures.resize(64u);
	}
	std::ranges::sort(stats.publishedResourceIDs);

	return stats;
}

std::shared_ptr<Resource> TextureStreamingManager::ProvideResource(ResourceIdentifier const& key)
{
	auto it = m_resources.find(key);
	if (it == m_resources.end()) {
		return nullptr;
	}
	return it->second;
}

std::vector<ResourceIdentifier> TextureStreamingManager::GetSupportedKeys()
{
	std::vector<ResourceIdentifier> keys;
	keys.reserve(m_resources.size());
	for (auto const& [key, _] : m_resources) {
		keys.push_back(key);
	}
	return keys;
}

std::vector<ResourceIdentifier> TextureStreamingManager::GetSupportedResolverKeys()
{
	return { Builtin::Material::TextureStreamingMetadataBuffer };
}

std::shared_ptr<IResourceResolver> TextureStreamingManager::ProvideResolver(ResourceIdentifier const& key)
{
	if (key == Builtin::Material::TextureStreamingMetadataBuffer) return m_textureImageTableResolver;
	return nullptr;
}
