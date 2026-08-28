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
#include "Render/Runtime/IReadbackService.h"
#include "RenderPasses/Base/CopyPass.h"
#include "Resources/Buffers/Buffer.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>

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

	TextureStreamingGPUInfo BuildTextureStreamingGPUInfo(const TextureAsset& texture) {
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

	class MaterialTextureStreamingReadbackPass final : public CopyPass, public IHasImmediateModeCommands {
	public:
		MaterialTextureStreamingReadbackPass(
			std::shared_ptr<Resource> source,
			std::shared_ptr<Buffer> staging,
			uint64_t bytes,
			std::function<PassReturn()> complete,
			std::function<void()> cancel)
			: m_staging(std::move(staging)), m_bytes(bytes), m_complete(std::move(complete)),
			  m_cancel(std::move(cancel)) {
			SetInputs(MaterialTextureStreamingReadbackInputs{std::move(source)});
		}
		~MaterialTextureStreamingReadbackPass() override {
			if (!m_executed && m_cancel) {
				m_cancel();
			}
		}

		void DeclareResourceUsages(CopyPassBuilder* builder) override {
			const auto& inputs = Inputs<MaterialTextureStreamingReadbackInputs>();
			builder->WithCopySource(inputs.source);
			builder->WithCopyDest(m_staging);
			builder->PreferQueue(QueueKind::Copy);
		}
		void Setup() override {}
		void RecordImmediateCommands(ImmediateExecutionContext& context) override {
			const auto& inputs = Inputs<MaterialTextureStreamingReadbackInputs>();
			if (inputs.source && m_staging && m_bytes != 0) {
				context.list.CopyBufferRegion(m_staging, 0, inputs.source.get(), 0, m_bytes);
			}
		}
		PassReturn Execute(PassExecutionContext&) override {
			m_executed = true;
			return m_complete ? m_complete() : PassReturn{};
		}
		void Cleanup() override {}
	private:
		std::shared_ptr<Buffer> m_staging;
		uint64_t m_bytes = 0;
		std::function<PassReturn()> m_complete;
		std::function<void()> m_cancel;
		bool m_executed = false;
	};
}

TextureStreamingManager::TextureStreamingManager()
{
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
	m_resources[Builtin::Material::TextureStreamingFeedbackBuffer] = m_textureStreamingFeedbackBuffer;
}

TextureStreamingManager::~TextureStreamingManager()
{
	Shutdown();
}

void TextureStreamingManager::SetRendererStateRequestService(
	br::render::RendererStateRequestService* service)
{
	m_graphBindingObservation.Reset();
	{
		std::lock_guard lock(m_graphBindingAwaiterMutex);
		m_graphBindingAwaiters.clear();
	}
	m_rendererStateRequests = service;
	{
		std::lock_guard lock(m_graphBindingStateMutex);
		m_observedGraphBindingStates.clear();
	}
	if (!m_rendererStateRequests) return;
	m_graphBindingObservation = m_rendererStateRequests->ObserveKind(
		br::render::ArtifactKind::TextureBinding,
		[this](std::uint64_t, const br::render::ArtifactSnapshot& snapshot) {
			if (snapshot.revision == 0 || snapshot.generation == 0) return;
			std::lock_guard lock(m_graphBindingStateMutex);
			auto& observed = m_observedGraphBindingStates[
				static_cast<uint32_t>(snapshot.key.primaryID)];
			const br::render::ArtifactVersionID version{
				snapshot.key, snapshot.revision, snapshot.generation };
			if (!observed.version || version.revision > observed.version.revision ||
				(version.revision == observed.version.revision &&
				 version.generation >= observed.version.generation)) {
				observed.version = version;
				observed.readiness = snapshot.readiness;
			}
		});
}

void TextureStreamingManager::Initialize(TextureFactory& textureFactory, uint32_t framesInFlight)
{
	(void)framesInFlight;
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
}

void TextureStreamingManager::Shutdown()
{
	if (!m_initialized.exchange(false, std::memory_order_acq_rel)) {
		return;
	}
	m_workerQuit.store(true, std::memory_order_release);
	m_graphBindingObservation.Reset();
	{
		std::lock_guard lock(m_graphBindingStateMutex);
		m_observedGraphBindingStates.clear();
	}
	if (m_taskScope.Valid()) m_taskScope.CancelAndWait();
	m_drainScheduled.store(false, std::memory_order_release);
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
	{
		std::lock_guard lock(m_liveBindingMutex);
		m_liveBindingsByID.clear();
		m_liveBindingIDsByStreamingTextureID.clear();
		m_dirtyLiveBindingIDs.clear();
		m_dirtyLiveBindingIDSet.clear();
	}
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
	bool expected = false;
	if (!m_drainScheduled.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
	if (!TaskSchedulerManager::GetInstance().Submit(m_taskScope, TaskLane::Streaming,
		TaskDomain::TextureProcessing, "TextureStreamingManager::Drain",
		[this](const br::TaskContext& context) { Drain(context); })) {
		m_drainScheduled.store(false, std::memory_order_release);
	}
}

void TextureStreamingManager::Drain(const br::TaskContext& context)
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
	if (!context.StopRequested()) PollCompletedReadbackSlots(m_lastProcessedReadbackFence);

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
	bool hasMore = false;
	{
		std::lock_guard lock(m_workerCommandMutex);
		hasMore = !m_workerCommands.empty();
	}
	m_drainScheduled.store(false, std::memory_order_release);
	if (hasMore && !context.StopRequested()) ScheduleDrain();
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
	// Graph-native material and terrain registrations carry streaming policy but
	// no compatibility callback. Do not create a main-thread adoption owner for
	// them; this makes owner cost independent of material sharing fan-out.
	if (onBindingChanged) {
		std::lock_guard lock(m_liveBindingMutex);
		m_liveBindingsByID.emplace(bindingID, MainThreadBindingOwner{
			.streamingTextureID = streamingTextureID,
			.texture = texture,
			.callback = onBindingChanged,
		});
		m_liveBindingIDsByStreamingTextureID[streamingTextureID].push_back(bindingID);
		if (m_dirtyLiveBindingIDSet.insert(bindingID).second) {
			m_dirtyLiveBindingIDs.push_back(bindingID);
		}
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
				input->samplerDescriptorIndex = texture->SamplerDescriptorIndex();
				input->image = published.image;
				input->streamingMetadata = BuildTextureStreamingGPUInfo(
					published.streamingState, texture->GetFullMip0Width(), texture->GetFullMip0Height());
				const auto fingerprint =
					(published.bindingRevision << 1u) ^ input->streamingStateRevision ^
					streamingTextureID ^ 0x54455842494e44ull;
				const auto request = m_rendererStateRequests->Request(
					address,
					published.bindingRevision, {},
					br::render::ArtifactPayload::Make<br::render::TextureBindingBuildInput>(
						std::move(input)), fingerprint);
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

	{
		std::lock_guard lock(m_liveBindingMutex);
		auto liveIt = m_liveBindingsByID.find(bindingID);
		if (liveIt != m_liveBindingsByID.end()) {
			const uint32_t streamingTextureID = liveIt->second.streamingTextureID;
			m_liveBindingsByID.erase(liveIt);
			auto idsIt = m_liveBindingIDsByStreamingTextureID.find(streamingTextureID);
			if (idsIt != m_liveBindingIDsByStreamingTextureID.end()) {
				std::erase(idsIt->second, bindingID);
				if (idsIt->second.empty()) m_liveBindingIDsByStreamingTextureID.erase(idsIt);
			}
		}
		m_dirtyLiveBindingIDSet.erase(bindingID);
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

void TextureStreamingManager::MarkLiveTextureBindingsDirty(uint32_t streamingTextureID)
{
	std::lock_guard lock(m_liveBindingMutex);
	auto idsIt = m_liveBindingIDsByStreamingTextureID.find(streamingTextureID);
	if (idsIt == m_liveBindingIDsByStreamingTextureID.end()) return;
	for (uint64_t bindingID : idsIt->second) {
		auto ownerIt = m_liveBindingsByID.find(bindingID);
		if (ownerIt == m_liveBindingsByID.end()) continue;
		if (m_dirtyLiveBindingIDSet.insert(bindingID).second) {
			m_dirtyLiveBindingIDs.push_back(bindingID);
		}
	}
}

std::size_t TextureStreamingManager::RefreshDirtyLiveBindings()
{
	std::vector<uint64_t> dirtyBindingIDs;
	{
		std::lock_guard lock(m_liveBindingMutex);
		dirtyBindingIDs.swap(m_dirtyLiveBindingIDs);
		m_dirtyLiveBindingIDSet.clear();
	}

	std::size_t refreshed = 0u;
	for (uint64_t bindingID : dirtyBindingIDs) {
		MainThreadBindingOwner owner{};
		{
			std::lock_guard lock(m_liveBindingMutex);
			auto ownerIt = m_liveBindingsByID.find(bindingID);
			if (ownerIt == m_liveBindingsByID.end()) continue;
			owner = ownerIt->second;
		}
		auto texture = owner.texture.lock();
		const auto published = texture
			? texture->GetPublishedBindingSnapshot()
			: TextureAsset::PublishedBindingSnapshot{};
		auto image = published.image;
		if (!texture || !image || !image->HasValidBackingResource()) continue;
		const uint64_t bindingRevision = published.bindingRevision;
		const uint64_t imageResourceID = image->GetGlobalResourceID();
		if (owner.appliedBindingRevision == bindingRevision && owner.appliedImageResourceID == imageResourceID) continue;
		if (owner.callback) owner.callback(*texture);
		{
			std::lock_guard lock(m_liveBindingMutex);
			auto ownerIt = m_liveBindingsByID.find(bindingID);
			if (ownerIt != m_liveBindingsByID.end() && ownerIt->second.texture.lock() == texture) {
				ownerIt->second.appliedBindingRevision = bindingRevision;
				ownerIt->second.appliedImageResourceID = imageResourceID;
			}
		}
		++refreshed;
	}
	m_textureBindingRefreshCount.fetch_add(refreshed, std::memory_order_relaxed);
	return refreshed;
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

std::shared_ptr<CopyPass> TextureStreamingManager::CreateTextureStreamingFeedbackReadbackPass()
{
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
	return std::make_shared<MaterialTextureStreamingReadbackPass>(
		std::move(source), std::move(staging), bytes,
		[this, selectedSlot]() -> PassReturn {
			uint64_t fenceValue = 0;
			{
				std::lock_guard lock(m_readbackSlotMutex);
				if (selectedSlot >= m_readbackSlots.size() || !m_readbackSlots[selectedSlot].inFlight) return {};
				fenceValue = m_readbackFenceCounter.fetch_add(1u, std::memory_order_acq_rel) + 1u;
				m_readbackSlots[selectedSlot].fenceValue = fenceValue;
			}
			ScheduleDrain();
			return {m_readbackFence, fenceValue};
		},
		[this, selectedSlot]() {
			std::lock_guard lock(m_readbackSlotMutex);
			if (selectedSlot >= m_readbackSlots.size()) {
				return;
			}
			auto& slot = m_readbackSlots[selectedSlot];
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
	// The streaming worker owns immutable binding requests. The render thread is
	// only handed a compatibility-adoption record after the exact graph version
	// reaches Submitted. This removes Request/Snapshot/requeue polling from the
	// frame loop while retaining the current PublishPreparedImage boundary.
	if (m_rendererStateRequests && change.transfer && change.transfer->gpuSubmissions) {
		auto input = std::make_shared<br::render::TextureBindingBuildInput>();
		input->streamingTextureID = change.streamingTextureID;
		input->bindingRevision = change.bindingRevision;
		input->streamingStateRevision = change.streamingStateRevision;
		input->samplerDescriptorIndex = texture.SamplerDescriptorIndex();
		input->image = change.newImage;
		input->transfer = change.transfer;
		input->gpuSubmissions = change.transfer->gpuSubmissions;
		input->streamingMetadata = change.metadata;
		const br::render::ArtifactAddress address{
			br::render::ArtifactKind::TextureBinding, change.streamingTextureID, 0 };
		const auto request = m_rendererStateRequests->Request(
			address, change.bindingRevision, {},
			br::render::ArtifactPayload::Make<br::render::TextureBindingBuildInput>(
				std::move(input)),
			(change.bindingRevision << 1u) ^ change.streamingStateRevision ^
				change.streamingTextureID ^ 0x54455842494e44ull);
		if (request) {
			change.graphRequested = true;
			change.waitingForGraphWake = true;
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
					pending->waitingForGraphWake = false;
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

std::size_t TextureStreamingManager::DrainPendingBindingChanges()
{
	std::vector<PendingBindingChange> changes;
	{
		BT_ZONE_SCOPE("TextureStreamingManager::DrainPendingBindingChanges::Detach");
		PendingBindingChange queuedChange;
		while (m_pendingBindingChanges.try_pop(queuedChange)) {
			changes.push_back(std::move(queuedChange));
		}
	}
	std::unordered_map<uint32_t, std::size_t> latestByTexture;
	std::vector<PendingBindingChange> coalesced;
	coalesced.reserve(changes.size());
	for (auto& change : changes) {
		auto [it, inserted] = latestByTexture.emplace(change.streamingTextureID, coalesced.size());
		if (inserted) {
			coalesced.push_back(std::move(change));
			continue;
		}
		auto& previous = coalesced[it->second];
		if (previous.previousImage) previous.supersededImages.push_back(std::move(previous.previousImage));
		if (previous.newImage && previous.newImage != change.newImage) previous.supersededImages.push_back(std::move(previous.newImage));
		previous.texture = std::move(change.texture);
		previous.bindingRevision = change.bindingRevision;
		previous.streamingStateRevision = change.streamingStateRevision;
		previous.queuedAt = change.queuedAt;
		previous.previousImage = std::move(change.previousImage);
		previous.newImage = std::move(change.newImage);
		previous.metadata = change.metadata;
		// A newer immutable binding revision must issue its own graph request.
		// Retaining the superseded request bit here stranded the replacement.
		previous.graphRequested = change.graphRequested;
		previous.waitingForGraphWake = change.waitingForGraphWake;
		previous.graphReady = change.graphReady;
		previous.graphVersion = change.graphVersion;
		previous.transfer = std::move(change.transfer);
	}
	{
		std::lock_guard lock(m_graphBindingAwaiterMutex);
		for (const auto& change : coalesced) {
			if (!change.graphReady) continue;
			m_graphBindingAwaiters.erase(change.streamingTextureID);
		}
	}
	// Copy only observations which this detached batch can consume.  The old
	// whole-map copy made the render thread walk every historical texture while
	// graph completion producers were also trying to publish into the map.
	std::unordered_map<uint32_t, ObservedGraphBindingState> observedGraphBindings;
	observedGraphBindings.reserve(coalesced.size());
	{
		BT_ZONE_SCOPE("TextureStreamingManager::DrainPendingBindingChanges::CaptureObservedState");
		std::lock_guard lock(m_graphBindingStateMutex);
		for (const auto& change : coalesced) {
			if (!change.waitingForGraphWake) continue;
			if (const auto found = m_observedGraphBindingStates.find(change.streamingTextureID);
				found != m_observedGraphBindingStates.end()) {
				observedGraphBindings.emplace(found->first, found->second);
			}
		}
	}
	std::size_t adopted = 0;
	std::vector<PendingBindingChange> waitingForGpu;
	std::size_t waitingForGraphCount = 0;
	std::size_t graphFailureCount = 0;
	for (auto& change : coalesced) {
		if (change.waitingForGraphWake) {
			const auto observed = observedGraphBindings.find(change.streamingTextureID);
			const bool exactVersionObserved = observed != observedGraphBindings.end() &&
				observed->second.version == change.graphVersion;
			if (exactVersionObserved &&
				(observed->second.readiness == br::render::ArtifactReadiness::Failed ||
				 observed->second.readiness == br::render::ArtifactReadiness::Cancelled)) {
				++graphFailureCount;
				if (change.texture) {
					(void)change.texture->RejectPreparedImage(
						change.bindingRevision, change.newImage);
					EnqueueTextureUploadAdvance(change.texture, "graph_binding_failed");
				}
				continue;
			}
			if (!exactVersionObserved ||
				!br::render::ArtifactReachedMilestone(observed->second.readiness,
					br::render::ArtifactReadiness::UploadSubmitted)) {
				waitingForGpu.push_back(std::move(change));
				continue;
			}
		}
		change.waitingForGraphWake = false;
		// Unregistration and upload completion are independent queues. A change
		// captured before the last owner disappeared must not recreate the graph
		// address after ApplyUnregisterCommand released it.
		bool hasLiveOwner = false;
		{
			std::lock_guard lock(m_liveBindingMutex);
			const auto owners = m_liveBindingIDsByStreamingTextureID.find(
				change.streamingTextureID);
			hasLiveOwner = owners != m_liveBindingIDsByStreamingTextureID.end() &&
				!owners->second.empty();
		}
		if (!hasLiveOwner) {
			if (m_rendererStateRequests) {
				m_rendererStateRequests->Release({ br::render::ArtifactKind::TextureBinding,
					change.streamingTextureID, 0 });
			}
			continue;
		}
		std::shared_ptr<const br::render::GpuSubmissionSet> transferSubmission;
		{
			BT_ZONE_SCOPE("TextureStreamingManager::DrainPendingBindingChanges::TransferState");
			if (change.graphReady && change.transfer) {
				transferSubmission = change.transfer->gpuSubmissions;
			}
			else if (m_materialTextureTransfers && change.newImage &&
				!m_materialTextureTransfers->IsShaderReady(change.newImage)) {
				transferSubmission = m_materialTextureTransfers->ShaderReadySubmission(change.newImage);
				if (!transferSubmission && !m_materialTextureTransfers->HasFailed(change.newImage)) {
					waitingForGpu.push_back(std::move(change));
				}
				else if (!transferSubmission && change.texture) {
					(void)change.texture->RejectPreparedImage(change.bindingRevision, change.newImage);
					EnqueueTextureUploadAdvance(change.texture, "external_transfer_failed");
				}
				if (!transferSubmission) continue;
			} else if (m_materialTextureTransfers && change.newImage) {
				transferSubmission = m_materialTextureTransfers->ShaderReadySubmission(change.newImage);
			}
		}
		if (m_rendererStateRequests) {
			BT_ZONE_SCOPE("TextureStreamingManager::DrainPendingBindingChanges::GraphRequest");
			const br::render::ArtifactKey bindingKey{
				br::render::ArtifactKind::TextureBinding,
				change.streamingTextureID,
				0 };
			if (!change.graphRequested) {
				auto input = std::make_shared<br::render::TextureBindingBuildInput>();
				input->streamingTextureID = change.streamingTextureID;
				input->bindingRevision = change.bindingRevision;
				input->streamingStateRevision = change.streamingStateRevision;
				input->samplerDescriptorIndex = change.texture
					? change.texture->SamplerDescriptorIndex() : 0u;
				input->image = change.newImage;
				input->transfer = change.transfer;
				input->gpuSubmissions = transferSubmission;
				input->streamingMetadata = change.metadata;
				const auto request = m_rendererStateRequests->Request(
					bindingKey, change.bindingRevision, {},
					br::render::ArtifactPayload::Make<br::render::TextureBindingBuildInput>(
						std::move(input)),
					(change.bindingRevision << 1u) ^ change.streamingStateRevision ^
						change.streamingTextureID ^ 0x54455842494e44ull);
				change.graphRequested = static_cast<bool>(request);
				change.graphVersion = request.version;
				if (!change.graphRequested) {
					// A rejected immutable revision cannot become accepted by retrying it
					// every frame.  In particular, StaleRevision means the address already
					// desires a successor.  Drop this historical observation and enqueue a
					// reconciliation from the texture's current prepared/published state.
					if (change.texture) {
						(void)change.texture->RejectPreparedImage(
							change.bindingRevision, change.newImage);
						MarkLiveTextureBindingsDirty(change.streamingTextureID);
						EnqueueTextureUploadAdvance(change.texture, "graph_binding_superseded");
						const auto currentPrepared = change.texture->PreparedImagePtr();
						const auto currentPublished = change.texture->ImagePtr();
						if (change.newImage && change.newImage != currentPrepared &&
							change.newImage != currentPublished) {
							DescriptorHeapManager::GetInstance().RetireResource(
								std::move(change.newImage));
						}
					}
					continue;
				}
			}
			const auto graphSnapshot = change.graphReady
				? br::render::ArtifactSnapshot{
					change.graphVersion.address, change.graphVersion.revision,
					change.graphVersion.generation,
					br::render::ArtifactReadiness::UploadSubmitted }
				: change.graphVersion
				? m_rendererStateRequests->Snapshot(change.graphVersion)
				: br::render::ArtifactSnapshot{};
			if (graphSnapshot.readiness == br::render::ArtifactReadiness::Failed ||
				graphSnapshot.readiness == br::render::ArtifactReadiness::Cancelled) {
				++graphFailureCount;
				if (change.texture) {
					(void)change.texture->RejectPreparedImage(
						change.bindingRevision, change.newImage);
					EnqueueTextureUploadAdvance(change.texture, "graph_binding_failed");
				}
				continue;
			}
			if (!change.graphRequested ||
				graphSnapshot.revision != change.bindingRevision ||
				graphSnapshot.generation != change.graphVersion.generation ||
				!br::render::ArtifactReachedMilestone(graphSnapshot.readiness,
					br::render::ArtifactReadiness::UploadSubmitted)) {
				change.waitingForGraphWake = change.graphRequested;
				waitingForGpu.push_back(std::move(change));
				++waitingForGraphCount;
				continue;
			}
		}
		const auto adoptionStart = std::chrono::steady_clock::now();
		std::shared_ptr<PixelBuffer> replacedPublishedImage;
		const uint32_t workerObservedOldSrv = TextureSrvIndex(change.previousImage);
		const uint32_t newSrv = TextureSrvIndex(change.newImage);
		if (!change.texture || !change.newImage ||
			!change.texture->PublishPreparedImage(
				change.bindingRevision,
				change.newImage,
				&replacedPublishedImage)) {
			if (MaterialTextureStreamingTransitionLoggingEnabled()) {
				spdlog::info(
					"MaterialTextureStreaming transition stale: textureID={} revision={} oldSrv={} newSrv={} queueToRejectUs={} superseded={}",
					change.streamingTextureID,
					change.bindingRevision,
					workerObservedOldSrv,
					newSrv,
					std::chrono::duration_cast<std::chrono::microseconds>(adoptionStart - change.queuedAt).count(),
					change.supersededImages.size());
			}
			if (change.texture) {
				MarkLiveTextureBindingsDirty(change.streamingTextureID);
				EnqueueTextureUploadAdvance(change.texture, "stale_adoption_reconcile");
			}
			const auto currentPrepared = change.texture ? change.texture->PreparedImagePtr() : nullptr;
			const auto currentPublished = change.texture ? change.texture->ImagePtr() : nullptr;
			if (change.newImage && change.newImage != currentPrepared && change.newImage != currentPublished) {
				DescriptorHeapManager::GetInstance().RetireResource(std::move(change.newImage));
			}
			for (auto& image : change.supersededImages) {
				if (image && image != currentPrepared && image != currentPublished) {
					DescriptorHeapManager::GetInstance().RetireResource(std::move(image));
				}
			}
			continue;
		}
		const uint32_t oldSrv = TextureSrvIndex(replacedPublishedImage);
		MarkLiveTextureBindingsDirty(change.streamingTextureID);
		if (m_textureStreamingMetadataBuffer && change.texture) {
			std::lock_guard publicationLock(m_gpuMetadataPublicationMutex);
			if (change.texture->GetStreamingStateRevision() == change.streamingStateRevision) {
				(void)m_textureStreamingMetadataBuffer->TryEnsureCapacityForIndex(change.streamingTextureID);
				(void)m_textureStreamingMetadataBuffer->TryUpdateAt(change.streamingTextureID, change.metadata);
			}
		}
		// Retire the image atomically displaced by PublishPreparedImage.  The image
		// captured by the worker is only a historical observation and can differ
		// when an intermediate revision was adopted concurrently.
		const bool workerObservedActualPublishedImage =
			change.previousImage == replacedPublishedImage;
		const auto currentPrepared = change.texture->PreparedImagePtr();
		const auto currentPublished = change.texture->ImagePtr();
		auto isCurrentImage = [&](const std::shared_ptr<PixelBuffer>& image) {
			return image && (image == currentPrepared || image == currentPublished);
		};
		if (replacedPublishedImage && replacedPublishedImage != change.newImage &&
			!isCurrentImage(replacedPublishedImage)) {
			m_imagesPendingOwnerPatchRetirement.push_back(std::move(replacedPublishedImage));
		}
		if (change.previousImage && change.previousImage != change.newImage &&
			!workerObservedActualPublishedImage && !isCurrentImage(change.previousImage)) {
			m_imagesPendingOwnerPatchRetirement.push_back(std::move(change.previousImage));
		}
		for (auto& image : change.supersededImages) {
			if (image && image != change.newImage && !isCurrentImage(image)) {
				DescriptorHeapManager::GetInstance().RetireResource(std::move(image));
			}
		}
		if (MaterialTextureStreamingTransitionLoggingEnabled()) {
			const auto adoptionEnd = std::chrono::steady_clock::now();
			std::size_t ownerCount = 0u;
			{
				std::lock_guard lock(m_liveBindingMutex);
				if (const auto ownersIt = m_liveBindingIDsByStreamingTextureID.find(change.streamingTextureID);
					ownersIt != m_liveBindingIDsByStreamingTextureID.end()) {
					ownerCount = ownersIt->second.size();
				}
			}
			spdlog::info(
				"MaterialTextureStreaming transition adopted: textureID={} revision={} oldSrv={} newSrv={} residentTopMip={} pendingTopMip={} queueToAdoptUs={} callbackAndWritesUs={} callbacks={} superseded={}",
				change.streamingTextureID,
				change.bindingRevision,
				oldSrv,
				newSrv,
				change.metadata.residentTopMip,
				change.metadata.pendingTopMip,
				std::chrono::duration_cast<std::chrono::microseconds>(adoptionStart - change.queuedAt).count(),
				std::chrono::duration_cast<std::chrono::microseconds>(adoptionEnd - adoptionStart).count(),
				ownerCount,
				change.supersededImages.size());
		}
		++adopted;
	}
	if (!waitingForGpu.empty()) {
		for (auto& change : waitingForGpu) m_pendingBindingChanges.push(std::move(change));
	}
	std::size_t refreshed = 0;
	{
		BT_ZONE_SCOPE("TextureStreamingManager::DrainPendingBindingChanges::RefreshOwners");
		refreshed = RefreshDirtyLiveBindings();
	}
	TracyPlot("TextureStreaming.MainThreadAdoptions", static_cast<int64_t>(adopted));
	TracyPlot("TextureStreaming.MainThreadBindingRefreshes", static_cast<int64_t>(refreshed));
	TracyPlot("TextureStreaming.GraphBindingWaits", static_cast<int64_t>(waitingForGraphCount));
	TracyPlot("TextureStreaming.GraphBindingFailures", static_cast<int64_t>(graphFailureCount));
	basic_telemetry::SetGauge("SARP.TextureStreaming.GraphBindingWaits",
		static_cast<std::int64_t>(waitingForGraphCount));
	basic_telemetry::AddCounter("SARP.TextureStreaming.GraphBindingFailures",
		static_cast<std::uint64_t>(graphFailureCount));
	return adopted;
}

void TextureStreamingManager::RetirePatchedBindingResources()
{
	ZoneScopedN("TextureStreamingManager::RetirePatchedBindingResources");
	for (auto& image : m_imagesPendingOwnerPatchRetirement) {
		if (image) DescriptorHeapManager::GetInstance().RetireResource(std::move(image));
	}
	TracyPlot("TextureStreaming.OwnerPatchedRetirements",
		static_cast<int64_t>(m_imagesPendingOwnerPatchRetirement.size()));
	m_imagesPendingOwnerPatchRetirement.clear();
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

bool TextureStreamingManager::RequestStreamingTextureReadback(
	uint32_t streamingTextureID,
	std::wstring outputFile,
	std::function<void()> callback)
{
	const auto found = m_streamingTexturesByID.find(streamingTextureID);
	const auto texture = found != m_streamingTexturesByID.end() ? found->second.lock() : nullptr;
	std::shared_ptr<PixelBuffer> selectedImage;
	if (m_rendererStateRequests) {
		const auto graphBinding = m_rendererStateRequests->Snapshot(br::render::ArtifactAddress{
			br::render::ArtifactKind::TextureBinding, streamingTextureID, 0 })
			.payload.Get<br::render::PublishedTextureBinding>();
		if (graphBinding) selectedImage = graphBinding->image;
	}
	if (!selectedImage && texture) selectedImage = texture->GetPublishedBindingSnapshot().image;
	return texture && RequestExternalMaterialTextureReadback(
		selectedImage,
		std::move(outputFile), std::move(callback));
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
	std::lock_guard publicationLock(m_gpuMetadataPublicationMutex);

	if (!m_textureStreamingMetadataBuffer->TryEnsureCapacityForIndex(streamingTextureID) ||
		!m_textureStreamingFeedbackBuffer->TryEnsureCapacityForIndex(streamingTextureID)) {
		return false;
	}

	{
		ZoneScopedN("TextureStreamingManager::UpdateTextureStreamingMetadata::UploadMetadata");
		if (!m_textureStreamingMetadataBuffer->TryUpdateAt(streamingTextureID, BuildTextureStreamingGPUInfo(*texture))) {
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
	return true;
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
	for (const uint32_t streamingTextureID : texturesToAdvance) {
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
	for (const uint32_t streamingTextureID : dirtyTextureIDs) {
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
		std::lock_guard statsLock(m_statsMutex);
		m_publishedStats = BuildTextureStreamingStats();
	}

}

MaterialTextureStreamingStats TextureStreamingManager::GetTextureStreamingStats(
	const std::vector<std::shared_ptr<Resource>>& activeTextureResources) const
{
	std::lock_guard statsLock(m_statsMutex);
	auto stats = m_publishedStats;
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
	return {};
}

std::shared_ptr<IResourceResolver> TextureStreamingManager::ProvideResolver(ResourceIdentifier const& key)
{
	return nullptr;
}
