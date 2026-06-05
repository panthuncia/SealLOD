#include "Managers/TextureStreamingManager.h"

#include "../generated/BuiltinResources.h"
#include "Factories/TextureFactory.h"
#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Materials/MaterialTextureStreaming.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Render/Runtime/IReadbackService.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace {
	constexpr uint32_t kTextureStreamingFlagEligible = 1u << 0;
	constexpr uint32_t kTextureStreamingFlagEnabled = 1u << 1;
	constexpr uint32_t kTextureStreamingFeedbackUnused = 0xffffffffu;
	constexpr uint64_t kTextureStreamingIdleFramesBeforeCoarsen = 180u;
	constexpr int64_t kTextureUploadAdvanceBudgetUs = 2000;
	constexpr std::string_view kTextureStreamingFeedbackReadbackAnchorPass = "MenuRenderPass";

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
}

TextureStreamingManager::TextureStreamingManager()
{
	m_textureStreamingMetadataBuffer = DynamicStructuredBuffer<TextureStreamingGPUInfo>::CreateShared(
		m_textureStreamingMetadataCapacity,
		"Builtin::Material::TextureStreamingMetadataBuffer",
		true);
	m_textureStreamingFeedbackBuffer = DynamicStructuredBuffer<uint32_t>::CreateShared(
		m_textureStreamingMetadataCapacity,
		"Builtin::Material::TextureStreamingFeedbackBuffer",
		true);
	rg::memory::SetResourceUsageHint(*m_textureStreamingMetadataBuffer, "Material texture streaming buffers");
	rg::memory::SetResourceUsageHint(*m_textureStreamingFeedbackBuffer, "Material texture streaming buffers");
	m_textureStreamingMetadataBuffer->UpdateAt(0u, TextureStreamingGPUInfo{});
	m_textureStreamingFeedbackBuffer->UpdateAt(0u, kTextureStreamingFeedbackUnused);
	m_resources[Builtin::Material::TextureStreamingMetadataBuffer] = m_textureStreamingMetadataBuffer;
	m_resources[Builtin::Material::TextureStreamingFeedbackBuffer] = m_textureStreamingFeedbackBuffer;
}

uint64_t TextureStreamingManager::RegisterTextureBinding(
	const std::shared_ptr<TextureAsset>& texture,
	TextureFactory& textureFactory,
	BindingChangedCallback onBindingChanged,
	std::string debugLabel)
{
	if (!texture) {
		return 0u;
	}

	const uint32_t streamingTextureID = texture->GetStreamingTextureID();
	if (streamingTextureID == 0u) {
		return 0u;
	}

	const uint64_t bindingID = m_nextBindingID++;
	m_bindingsByID.emplace(bindingID, TextureBindingOwner{
		.bindingID = bindingID,
		.streamingTextureID = streamingTextureID,
		.texture = texture,
		.onBindingChanged = std::move(onBindingChanged),
		.debugLabel = std::move(debugLabel)
	});
	m_bindingIDsByStreamingTextureID[streamingTextureID].push_back(bindingID);
	TrackTexture(texture);
	EnsureTextureUploadAdvanced(texture, textureFactory);
	FlushDirtyTextureMetadata(texture);
	return bindingID;
}

void TextureStreamingManager::UnregisterTextureBinding(uint64_t bindingID)
{
	if (bindingID == 0u) {
		return;
	}

	auto bindingIt = m_bindingsByID.find(bindingID);
	if (bindingIt == m_bindingsByID.end()) {
		return;
	}

	const uint32_t streamingTextureID = bindingIt->second.streamingTextureID;
	m_bindingsByID.erase(bindingIt);
	auto ownersIt = m_bindingIDsByStreamingTextureID.find(streamingTextureID);
	if (ownersIt != m_bindingIDsByStreamingTextureID.end()) {
		std::erase(ownersIt->second, bindingID);
		if (ownersIt->second.empty()) {
			m_bindingIDsByStreamingTextureID.erase(ownersIt);
			m_streamingTexturesByID.erase(streamingTextureID);
			m_textureStreamingMetadataRevisions.erase(streamingTextureID);
			m_dirtyTextureStreamingIDSet.erase(streamingTextureID);
			m_texturesNeedingUploadAdvanceSet.erase(streamingTextureID);
			m_activeTextureStreamingFeedbackIDSet.erase(streamingTextureID);
			std::erase(m_dirtyTextureStreamingIDs, streamingTextureID);
			std::erase(m_texturesNeedingUploadAdvance, streamingTextureID);
			std::erase(m_activeTextureStreamingFeedbackIDs, streamingTextureID);
		}
	}
}

void TextureStreamingManager::UnregisterTextureBindings(const std::vector<uint64_t>& bindingIDs)
{
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

		const uint64_t previousRevision = texture->GetStreamingStateRevision();
		const bool needsUploadAdvance = texture->ApplyStreamingSystemRequest(requestedTopMip, frameIndex);
		if (texture->GetStreamingStateRevision() != previousRevision) {
			MarkTextureStreamingMetadataDirty(texture, needsUploadAdvance, "feedback");
		}
	}

	for (uint32_t streamingTextureID : expiredTextureIDs) {
		m_streamingTexturesByID.erase(streamingTextureID);
	}

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
		if (state.lastSeenFrame == 0u || frameIndex <= state.lastSeenFrame + kTextureStreamingIdleFramesBeforeCoarsen) {
			++it;
			continue;
		}

		const uint32_t coarsenedTopMip = (std::min)(
			state.residency.totalMipCount - 1u,
			(std::max)(state.requestedTopMip, state.residency.residentTopMip + 1u));
		if (coarsenedTopMip != state.requestedTopMip) {
			const uint64_t previousRevision = texture->GetStreamingStateRevision();
			const bool needsUploadAdvance = texture->ApplyStreamingSystemRequest(coarsenedTopMip, frameIndex, true);
			if (texture->GetStreamingStateRevision() != previousRevision) {
				MarkTextureStreamingMetadataDirty(texture, needsUploadAdvance, "idle_coarsen");
			}
		}

		++it;
	}

	for (const uint32_t streamingTextureID : m_activeTextureStreamingFeedbackIDs) {
		if (streamingTextureID < m_textureStreamingMetadataCapacity) {
			m_textureStreamingFeedbackBuffer->UpdateAt(streamingTextureID, kTextureStreamingFeedbackUnused);
		}
	}
}

void TextureStreamingManager::RequestTextureStreamingFeedbackReadback(rg::runtime::IReadbackService* readbackService)
{
	if (!IsMaterialTextureStreamingEnabledSetting() ||
		!readbackService ||
		!m_textureStreamingFeedbackBuffer ||
		m_activeTextureStreamingFeedbackIDs.empty()) {
		return;
	}

	std::vector<uint32_t> activeStreamingTextureIDs = m_activeTextureStreamingFeedbackIDs;
	readbackService->RequestReadbackCapture(
		std::string(kTextureStreamingFeedbackReadbackAnchorPass),
		m_textureStreamingFeedbackBuffer.get(),
		RangeSpec{},
		[this, activeStreamingTextureIDs = std::move(activeStreamingTextureIDs)](ReadbackCaptureResult&& result) {
			if (result.desc.kind != ReadbackResourceKind::Buffer || result.data.empty()) {
				return;
			}

			TaskSchedulerManager::GetInstance().RunBackgroundTask(
				"TextureStreamingManager::DecodeTextureStreamingFeedback",
				[this,
				 activeStreamingTextureIDs = std::move(activeStreamingTextureIDs),
				 resultData = std::move(result.data)]() mutable {
					std::vector<std::pair<uint32_t, uint32_t>> decodedFeedback;
					decodedFeedback.reserve(activeStreamingTextureIDs.size());
					const size_t wordCount = resultData.size() / sizeof(uint32_t);
					for (uint32_t streamingTextureID : activeStreamingTextureIDs) {
						if (streamingTextureID >= wordCount) {
							continue;
						}

						uint32_t requestedTopMip = kTextureStreamingFeedbackUnused;
						std::memcpy(
							&requestedTopMip,
							resultData.data() + static_cast<size_t>(streamingTextureID) * sizeof(uint32_t),
							sizeof(uint32_t));
						if (requestedTopMip == kTextureStreamingFeedbackUnused) {
							continue;
						}

						decodedFeedback.emplace_back(streamingTextureID, requestedTopMip);
					}

					if (decodedFeedback.empty()) {
						return;
					}

					std::lock_guard lock(m_textureStreamingFeedbackMutex);
					m_pendingTextureStreamingFeedback.insert(
						m_pendingTextureStreamingFeedback.end(),
						decodedFeedback.begin(),
						decodedFeedback.end());
				});
		},
		QueueKind::Copy);
}

void TextureStreamingManager::EnsureTextureUploadAdvanced(
	const std::shared_ptr<TextureAsset>& texture,
	TextureFactory& textureFactory)
{
	if (!texture) {
		return;
	}

	const uint64_t previousBindingRevision = texture->GetBindingRevision();
	const uint64_t previousStreamingRevision = texture->GetStreamingStateRevision();
	texture->SetGenerateMipmaps(true);
	texture->EnsureUploaded(textureFactory, TextureUploadAdvanceMode::NonBlocking);

	if (texture->GetStreamingStateRevision() != previousStreamingRevision) {
		MarkTextureStreamingMetadataDirty(texture, false, "upload_state_revision");
	}
	if (texture->HasPendingUploadWork()) {
		MarkTextureStreamingMetadataDirty(texture, true, "upload_pending");
	}
	if (texture->GetBindingRevision() != previousBindingRevision) {
		NotifyBindingChanged(*texture);
	}
}

void TextureStreamingManager::NotifyBindingChanged(TextureAsset& texture)
{
	const uint32_t streamingTextureID = texture.GetStreamingTextureID();
	auto ownersIt = m_bindingIDsByStreamingTextureID.find(streamingTextureID);
	if (ownersIt == m_bindingIDsByStreamingTextureID.end() || ownersIt->second.empty()) {
		++m_textureBindingChangedWithoutOwnerCount;
		spdlog::warn(
			"TextureStreamingManager: binding changed without registered owner textureID={} label='{}'",
			streamingTextureID,
			texture.GetPendingDebugInfo().label);
		return;
	}

	std::vector<uint64_t> bindingIDs = ownersIt->second;
	for (uint64_t bindingID : bindingIDs) {
		auto bindingIt = m_bindingsByID.find(bindingID);
		if (bindingIt == m_bindingsByID.end()) {
			continue;
		}

		if (bindingIt->second.onBindingChanged) {
			bindingIt->second.onBindingChanged(texture);
			++m_textureBindingRefreshCount;
		}
	}
}

void TextureStreamingManager::FlushDirtyTextureMetadata(const std::shared_ptr<TextureAsset>& texture)
{
	if (!texture) {
		return;
	}

	const uint32_t streamingTextureID = texture->GetStreamingTextureID();
	if (streamingTextureID == 0u) {
		return;
	}

	const uint64_t revision = texture->GetStreamingStateRevision();
	auto revisionIt = m_textureStreamingMetadataRevisions.find(streamingTextureID);
	if (revisionIt != m_textureStreamingMetadataRevisions.end() && revisionIt->second == revision) {
		return;
	}

	UpdateTextureStreamingMetadata(texture);
	m_textureStreamingMetadataRevisions[streamingTextureID] = revision;
}

void TextureStreamingManager::UpdateTextureStreamingMetadata(const std::shared_ptr<TextureAsset>& texture)
{
	if (!texture) {
		return;
	}

	const uint32_t streamingTextureID = texture->GetStreamingTextureID();
	if (streamingTextureID == 0u) {
		return;
	}

	if (streamingTextureID >= m_textureStreamingMetadataCapacity) {
		uint32_t newCapacity = m_textureStreamingMetadataCapacity;
		while (streamingTextureID >= newCapacity) {
			newCapacity *= 2u;
		}
		m_textureStreamingMetadataBuffer->Resize(newCapacity);
		m_textureStreamingFeedbackBuffer->Resize(newCapacity);
		m_textureStreamingMetadataCapacity = newCapacity;
	}

	m_textureStreamingMetadataBuffer->UpdateAt(streamingTextureID, BuildTextureStreamingGPUInfo(*texture));
	m_textureStreamingFeedbackBuffer->UpdateAt(streamingTextureID, kTextureStreamingFeedbackUnused);
	if (auto image = texture->ImagePtr()) {
		m_textureAssetsByImageResourceID[image->GetGlobalResourceID()] = texture;
	}
	TrackTexture(texture);
	if (m_activeTextureStreamingFeedbackIDSet.insert(streamingTextureID).second) {
		m_activeTextureStreamingFeedbackIDs.push_back(streamingTextureID);
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
	for (const uint32_t streamingTextureID : texturesToAdvance) {
		if (uploadAdvanceVisited != 0) {
			const auto elapsedUploadUs = std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - uploadStart).count();
			if (elapsedUploadUs >= kTextureUploadAdvanceBudgetUs) {
				deferredTextureIDs.push_back(streamingTextureID);
				continue;
			}
		}
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
		if (m_textureStreamingMetadataRevisions[streamingTextureID] != previousRevision) {
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
			m_textureBindingRefreshCount,
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
}

MaterialTextureStreamingStats TextureStreamingManager::GetTextureStreamingStats(
	const std::vector<std::shared_ptr<Resource>>& activeTextureResources) const
{
	MaterialTextureStreamingStats stats{};
	std::unordered_set<uint64_t> seenImageResourceIDs;

	for (const auto& textureResource : activeTextureResources) {
		if (!textureResource) {
			continue;
		}

		auto image = std::dynamic_pointer_cast<PixelBuffer>(textureResource);
		if (!image) {
			continue;
		}

		const uint64_t imageResourceID = image->GetGlobalResourceID();
		if (!seenImageResourceIDs.insert(imageResourceID).second) {
			continue;
		}

		stats.uniqueMaterialTextureCount++;
		stats.totalResidentBytes += ComputeTextureResidentBytes(image->GetDescription());

		uint32_t residentTopMip = 0u;
		auto textureIt = m_textureAssetsByImageResourceID.find(imageResourceID);
		if (textureIt != m_textureAssetsByImageResourceID.end()) {
			auto texture = textureIt->second.lock();
			if (!texture) {
				continue;
			}

			const TextureStreamingState& streamingState = texture->GetStreamingState();
			residentTopMip = streamingState.residency.residentTopMip;
			if (streamingState.eligible) {
				stats.uniqueStreamableTextureCount++;
				stats.streamableResidentBytes += ComputeTextureResidentBytes(image->GetDescription());
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
		}

		if (residentTopMip == 0u) {
			stats.fullResolutionResidentTextureCount++;
		}

		if (stats.residentTopMipHistogram.size() <= residentTopMip) {
			stats.residentTopMipHistogram.resize(static_cast<size_t>(residentTopMip) + 1u, 0u);
		}
		stats.residentTopMipHistogram[residentTopMip]++;
	}

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

		const TextureStreamingState& streamingState = texture->GetStreamingState();
		const uint32_t residentTopMip = streamingState.residency.residentTopMip;
		stats.uniqueMaterialTextureCount++;
		stats.totalResidentBytes += ComputeTextureResidentBytes(image->GetDescription());
		if (streamingState.eligible) {
			stats.uniqueStreamableTextureCount++;
			stats.streamableResidentBytes += ComputeTextureResidentBytes(image->GetDescription());
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
	}

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
