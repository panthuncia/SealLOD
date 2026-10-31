// Method definitions for StaticImportCoordinator.

#include <BasicRenderer/Streaming/ObjectRequests.h>
#include <BasicRenderer/Streaming/RendererStateRequestService.h>
#include "StaticImport/StaticImportCoordinator.h"

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <BasicRenderer/Streaming/PublishedRendererState.h>
#include <BasicRenderer/Streaming/StaticGeometryRequestService.h>
#include <BasicRenderer/Streaming/StaticObjectRequestService.h>

namespace br::staticimport
{
	namespace {
		struct SkippedRediscovery { std::uint64_t generation, placements; };
		struct MembershipTrace {
			std::mutex mutex;
			std::ofstream out;
			std::unordered_map<std::uint64_t, SkippedRediscovery> skipped;
			MembershipTrace(const char* path) : out(std::string(path) + ".lifecycle.csv") {
				out << "event,group_id,target_generation,owner_generation,placements\n";
			}
		};
		MembershipTrace* SceneMembershipTrace() {
			static auto trace = []() -> std::unique_ptr<MembershipTrace> {
				const auto* path = std::getenv("SARP_SCENE_MEMBERSHIP_OUT");
				return path && path[0] ? std::make_unique<MembershipTrace>(path) : nullptr;
			}();
			return trace.get();
		}
	}

	StaticImportCoordinator::StaticImportCoordinator(const StaticStreamingContext& ctx, StaticGroupLifecycleStore& lifecycle, StaticImportCapacityBroker& capacity, br::SerializedTaskPump& pump, Hooks hooks)
	{
		_shared = &ctx.shared;
		_bus = &ctx.bus;
		_importState = &ctx.importState;
		_drains = &ctx.drains;
		_settings = &ctx.settings;
		_assets = &ctx.assets;
		_placements = &ctx.placements;
		_extensions = ctx.extensions;
		_lifecycle = &lifecycle;
		_capacity = &capacity;
		_pump = &pump;
		_hooks = std::move(hooks);
		_state.hostCellBlocks.resize(_extensions.size());
	}

	void StaticImportCoordinator::SetExtensions(std::span<br::staticimport::IStaticImportExtension* const> extensions)
	{
		_extensions = extensions;
		_state.hostCellBlocks.resize(_extensions.size());
	}

	std::size_t StaticImportCoordinator::ExtensionIndex(const br::staticimport::IStaticImportExtension& extension) const
	{
		for (std::size_t index = 0; index < _extensions.size(); ++index) {
			if (_extensions[index] == std::addressof(extension)) return index;
		}
		return _extensions.size();
	}

	bool StaticImportCoordinator::HasHostCellWaiters(const StaticImportDependencyKey& key) const
	{
		return std::ranges::any_of(_state.hostCellBlocks, [&](const HostCellBlocks& blocks) {
			return blocks.dependencyWaiters.contains(key);
		});
	}

	std::size_t StaticImportCoordinator::BlockedHostCellCount() const
	{
		std::size_t count = 0;
		for (const auto& blocks : _state.hostCellBlocks) {
			count += blocks.blockedCells.size();
		}
		return count;
	}

	bool StaticImportCoordinator::IsHostCellBlocked(const br::staticimport::IStaticImportExtension& owner, const StaticCellKey& cell) const
	{
		const auto index = ExtensionIndex(owner);
		return index < _state.hostCellBlocks.size() && _state.hostCellBlocks[index].blockedCells.contains(cell);
	}

	void StaticImportCoordinator::ClearHostCellBlock(HostCellBlocks& blocks, const StaticCellKey& cell)
	{
		if (auto blockedIt = blocks.blockedCells.find(cell); blockedIt != blocks.blockedCells.end()) {
			for (const auto& dependencyKey : blockedIt->second) {
				if (auto waitersIt = blocks.dependencyWaiters.find(dependencyKey); waitersIt != blocks.dependencyWaiters.end()) {
					waitersIt->second.erase(cell);
					if (waitersIt->second.empty()) {
						blocks.dependencyWaiters.erase(waitersIt);
					}
				}
			}
			blocks.blockedCells.erase(blockedIt);
		}
	}

	void StaticImportCoordinator::DropHostCell(const StaticCellKey& cell)
	{
		for (std::size_t index = 0; index < _extensions.size(); ++index) {
			ClearHostCellBlock(_state.hostCellBlocks[index], cell);
			_extensions[index]->OnHostCellDropped(cell);
		}
	}

	void StaticImportCoordinator::DropUnwantedHostCells(const StaticImportCameraTarget& target)
	{
		std::size_t dropped = 0;
		for (std::size_t index = 0; index < _extensions.size(); ++index) {
			auto& blocks = _state.hostCellBlocks[index];
			auto* extension = _extensions[index];
			std::vector<StaticCellKey> staleCells;
			staleCells.reserve(blocks.blockedCells.size());
			for (const auto& [cell, _] : blocks.blockedCells) {
				if (!target.valid || !extension->IsHostCellWanted(target.stream, cell)) {
					staleCells.push_back(cell);
				}
			}
			for (const auto& cell : staleCells) {
				ClearHostCellBlock(blocks, cell);
				extension->OnHostCellDropped(cell);
			}
			dropped += staleCells.size();
		}
		BT_PLOT("SARP.StaticImportCoordinator.StaleBlockedHostCellsCleared",
			static_cast<int64_t>(dropped));
	}

	StaticHostCellBlockResult StaticImportCoordinator::BlockHostCell(const br::staticimport::IStaticImportExtension& owner, const StaticCellKey& cell, std::vector<StaticImportDependencyRequest> dependencies)
	{
		const auto index = ExtensionIndex(owner);
		if (index >= _state.hostCellBlocks.size()) {
			return StaticHostCellBlockResult::Failed;
		}
		ClearHostCellBlock(_state.hostCellBlocks[index], cell);

		std::unordered_set<StaticImportDependencyKey, StaticImportDependencyKeyHash> remaining;
		remaining.reserve(dependencies.size());
		for (auto& dependency : dependencies) {
			if (IsDependencyResolved(dependency)) {
				continue;
			}
			if (IsDependencyFailed(dependency)) {
				FailDependency(dependency.key);
				return StaticHostCellBlockResult::Failed;
			}
			remaining.insert(dependency.key);
			_state.hostCellBlocks[index].dependencyWaiters[dependency.key].insert(cell);
			(void)SubmitDependencyRequest(std::move(dependency));
		}

		if (remaining.empty()) {
			return StaticHostCellBlockResult::Ready;
		}
		_state.hostCellBlocks[index].blockedCells.insert_or_assign(cell, std::move(remaining));
		return StaticHostCellBlockResult::Blocked;
	}

	std::optional<StaticImportDependencyRequest> StaticImportCoordinator::TakePendingDependency(const StaticImportDependencyKey& key)
	{
		auto pending = _state.pendingDependencyRequests.find(key);
		if (pending == _state.pendingDependencyRequests.end()) return std::nullopt;
		auto dependency = std::move(pending->second);
		_state.pendingDependencyRequests.erase(pending);
		return dependency;
	}

	void StaticImportCoordinator::SettleDependency(StaticImportDependencyRequest request, StaticImportDependencyResultStatus status, std::chrono::steady_clock::duration retryDelay)
	{
		const auto key = request.key;
		if (status == StaticImportDependencyResultStatus::Ready) {
			ResolveDependency(key);
		} else if (status == StaticImportDependencyResultStatus::Failed) {
			FailDependency(key);
		} else if (HasHostCellWaiters(key) ||
			_state.graphDependencyWaiters.contains(key)) {
			_state.delayedDependencyRequests.insert_or_assign(key, DelayedStaticImportDependencyRequest{
				.request = std::move(request),
				.dueAt = std::chrono::steady_clock::now() + retryDelay
			});
		}
	}

	void StaticImportCoordinator::SubmitHostImportBatch(PreparedDirectStaticImportBatch batch)
	{
		batch.generation = _state.latestTarget.generation;
		batch.publishTicketID = _state.nextPublishTicketID++;
		for (auto& import : batch.imports) {
			import.generation = batch.generation;
		}
		(void)_drains->PushBlocking(_bus->staticImportCompletedOutput, std::move(batch));
	}

	void StaticImportCoordinator::Drain()
	{
		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::Drain");
		if (_shared->staticImportServiceStop.load(std::memory_order_acquire)) {
			PublishStats(true);
			return;
		}
		const auto sliceDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
		do {
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::Tick");
			bool didWork = false;
			StaticImportCameraTarget target;
			if (ConsumeCameraTarget(target)) {
				ProcessTarget(target);
				didWork = true;
			}
			for (auto* extension : _extensions) {
				if (_state.latestTarget.valid && extension->HasPendingCoordinatorWork()) {
					std::lock_guard publicationLock(_importState->staticImportPublicationWorkerMutex);
					extension->DrainCoordinatorWork(_state.latestTarget.stream);
					didWork = true;
				}
			}
			didWork = DrainGroupAcks() || didWork;
			didWork = DrainTemplateRefs() || didWork;
			didWork = DrainPreparedCells() || didWork;
			didWork = DrainGroupChanges() || didWork;
			for (auto* extension : _extensions) {
				didWork = extension->DrainPreparedResults() || didWork;
			}
			didWork = DrainAssetResults() || didWork;
			didWork = DrainMaterialResults() || didWork;
			didWork = DrainShaderVariantResults() || didWork;
			for (auto* extension : _extensions) {
				didWork = extension->DrainDependencyResults() || didWork;
			}
			didWork = DrainStaticGroupSuspensionRequests() || didWork;
			didWork = DrainGroupResults() || didWork;
			didWork = DrainMaterializedValidationRequests() || didWork;
			didWork = PromoteNotifiedAssetDependencies() || didWork;
			didWork = ReconcileOrphanedAssetDependencies() || didWork;
			didWork = PumpDueDependencyRequests() || didWork;
			didWork = ScheduleCells() || didWork;
			didWork = ScheduleGroups() || didWork;
			bool retirementPending = !_bus->staticImportRemovalOutput.Empty() ||
				!_bus->staticRetirementOwnershipRequests.Empty() || !_bus->staticRetirementPayloadRequests.Empty() ||
				!_bus->staticRetirementCompletions.Empty();
			{
				std::lock_guard lock(_importState->staticRetirementMutex);
				retirementPending = retirementPending || !_importState->staticRetirementsAwaitingPublication.empty();
			}
			const bool quiescent = !didWork && !retirementPending &&
				_bus->staticImportGroupChanges.Empty() &&
				_state.pendingCellQueue.empty() && _state.pendingCellKeys.empty() &&
				_state.pendingGroups.empty() && _state.pendingGroupQueue.empty() &&
				_state.readyGroups.empty() && _state.groupSuspensions.empty() &&
				_state.pendingDependencyRequests.empty() && _state.delayedDependencyRequests.empty() &&
				std::ranges::all_of(_extensions, [](const auto* extension) {
					return extension->PendingHostCellCount() == 0;
				}) &&
				BlockedHostCellCount() == 0;
			PublishStats(_state.pendingGroups.empty() ||
				(quiescent && !_importState->staticImportCoordinatorPublishedQuiescent));
			_importState->staticImportCoordinatorPublishedQuiescent = quiescent;
			if (!didWork) break;
		} while (std::chrono::steady_clock::now() < sliceDeadline &&
			!_shared->staticImportServiceStop.load(std::memory_order_acquire));

		if (HasReadyWork()) {
			_drains->ScheduleCoordinatorDrain();
		} else if (HasWork() &&
			(!_state.pendingGroupQueue.empty() ||
			 _state.producerActiveGroups.empty())) {
			// No producer completion can provide the next wake. Retain one recovery
			// token for a rejected graph batch, a bounded admission tail, or a
			// genuinely non-notifying backend. Dormant variant producers cannot be
			// relied on to wake unrelated groups that have not been submitted yet.
			_drains->ScheduleCoordinatorDrain(std::chrono::milliseconds(2));
		} else if (const auto nextPollDue = NextDependencyPollDue()) {
			_drains->ScheduleCoordinatorDrain((std::max)(
				std::chrono::steady_clock::duration::zero(), *nextPollDue - std::chrono::steady_clock::now()));
		}
		_drains->ScheduleWorkerDrains();
	}

	bool StaticImportCoordinator::ConsumeCameraTarget(StaticImportCameraTarget& target)
	{
		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::ConsumeCameraTarget");
		if (!_importState->staticImportServiceHasPendingTarget.load(std::memory_order_acquire)) {
			return false;
		}
		std::lock_guard lock(_importState->staticImportServiceCameraMutex);
		if (!_importState->staticImportServiceHasPendingTarget.load(std::memory_order_acquire)) {
			return false;
		}
		target = _importState->staticImportServicePendingTarget;
		_importState->staticImportServiceHasPendingTarget.store(false, std::memory_order_release);
		return true;
	}

	bool StaticImportCoordinator::IsDependencyResolved(const StaticImportDependencyRequest& request) const
	{
		switch (request.key.kind) {
		case StaticImportDependencyKind::Template:
			return _state.templateRefs && _state.templateRefs->contains(DirectStaticTemplateKeyFromDependency(request.key));
		case StaticImportDependencyKind::Asset:
			return _assets->TryGetReadyParts(
				request.assetRequest.modelPath,
				br::staticimport::StaticMaterialVariant{}) != nullptr;
		case StaticImportDependencyKind::MaterialVariant:
			return _assets->TryGetReadyParts(
				request.materialVariantRequest.modelPath,
				request.materialVariantRequest.variant) != nullptr;
		case StaticImportDependencyKind::ShaderVariant:
			if (_shared->hasSceneIngestionServices && _shared->sceneIngestionServices.shaderVariants) {
				return _shared->sceneIngestionServices.shaderVariants->RequestShaderVariant(
					request.shaderVariantRequest.request);
			}
			return false;
		case StaticImportDependencyKind::Host:
			return request.hostRequest.owner &&
				request.hostRequest.owner->IsHostDependencyReady(request.hostRequest.dependency);
		}
		return false;
	}

	bool StaticImportCoordinator::IsDependencyFailed(const StaticImportDependencyRequest& request) const
	{
		switch (request.key.kind) {
		case StaticImportDependencyKind::Template:
			return !request.templateRequest.mesh || !request.templateRequest.material ||
				_state.failedTemplateKeys.contains(
					DirectStaticTemplateKeyFromDependency(request.key));
		case StaticImportDependencyKind::Asset:
			return request.assetRequest.modelPath.empty() || _assets->HasKnownFailure(request.assetRequest.modelPath);
		case StaticImportDependencyKind::MaterialVariant:
			return request.materialVariantRequest.modelPath.empty() ||
				_assets->HasKnownFailure(request.materialVariantRequest.modelPath);
		case StaticImportDependencyKind::ShaderVariant:
			return !_shared->hasSceneIngestionServices || !_shared->sceneIngestionServices.shaderVariants;
		case StaticImportDependencyKind::Host:
			return !request.hostRequest.owner ||
				request.hostRequest.owner->IsHostDependencyFailed(request.hostRequest.dependency);
		}
		return true;
	}

	bool StaticImportCoordinator::IsHostGroupCurrent(const StaticCellKey& cell, std::uint64_t residencyGeneration) const
	{
		return std::ranges::all_of(_extensions, [&](const auto* extension) {
			return extension->IsHostGroupCurrent(_state.latestTarget.stream, cell, residencyGeneration);
		});
	}

	bool StaticImportCoordinator::IsGroupLive(const StaticGroupLifecycleRecord& record) const
	{
		const auto& group = record.descriptor;
		if (group.residencyClass == StaticImportResidencyClass::HostGroup) {
			return _state.latestTarget.valid &&
				_state.latestTarget.stream.interiorCellFormID == 0 &&
				IsHostGroupCurrent(group.cellKey, group.residencyGeneration);
		}
		// A cell-static group is live while its add is the desired one. The
		// revision replaces the old cell-membership test: a group whose cell was
		// left and re-entered is a new add, and the work of the earlier add is stale.
		const auto desired = _state.desiredGroups.find(record.groupID);
		return desired != _state.desiredGroups.end() && desired->second.revision == group.groupRevision;
	}

	bool StaticImportCoordinator::IsPreparedLive(const PreparedDirectStaticImport& prepared) const
	{
		if (prepared.residencyClass == StaticImportResidencyClass::HostGroup) {
			return _state.latestTarget.valid &&
				_state.latestTarget.stream.interiorCellFormID == 0 &&
				IsHostGroupCurrent(prepared.cellKey, prepared.residencyGeneration);
		}
		const auto desired = _state.desiredGroups.find(prepared.groupID);
		return desired != _state.desiredGroups.end() && desired->second.revision == prepared.groupRevision;
	}

	void StaticImportCoordinator::ClearGroupBlockers(std::uint64_t groupID)
	{
		if (auto suspensionIt = _state.groupSuspensions.find(groupID);
			suspensionIt != _state.groupSuspensions.end()) {
			for (const auto& dependencyKey : suspensionIt->second.remaining) {
				auto waitersIt = _state.graphDependencyWaiters.find(dependencyKey);
				if (waitersIt == _state.graphDependencyWaiters.end()) continue;
				waitersIt->second.erase(groupID);
				if (waitersIt->second.empty()) _state.graphDependencyWaiters.erase(waitersIt);
			}
			_state.groupSuspensions.erase(suspensionIt);
		}
		_state.groupCheckpoints.erase(groupID);
	}

	void StaticImportCoordinator::ReleasePreparedTicket(std::uint64_t ticketID)
	{
		if (ticketID == 0) return;
		if (const auto found = _state.preparedBytesByPublishTicket.find(ticketID);
			found != _state.preparedBytesByPublishTicket.end()) {
			_state.preparedUnpublishedBytes -= (std::min)(
				_state.preparedUnpublishedBytes, found->second);
			_state.preparedBytesByPublishTicket.erase(found);
		}
		if (const auto found = _state.materializedBytesByPublishTicket.find(ticketID);
			found != _state.materializedBytesByPublishTicket.end()) {
			_state.materializedUnpublishedBytes -= (std::min)(
				_state.materializedUnpublishedBytes, found->second);
			_state.materializedBytesByPublishTicket.erase(found);
		}
	}

	void StaticImportCoordinator::TransferPreparedTicketToMaterialized(std::uint64_t ticketID)
	{
		if (ticketID == 0 || _state.materializedBytesByPublishTicket.contains(ticketID)) return;
		const auto found = _state.preparedBytesByPublishTicket.find(ticketID);
		if (found == _state.preparedBytesByPublishTicket.end()) return;
		const auto bytes = found->second;
		_state.preparedUnpublishedBytes -= (std::min)(
			_state.preparedUnpublishedBytes, bytes);
		_state.preparedBytesByPublishTicket.erase(found);
		_state.materializedBytesByPublishTicket.emplace(ticketID, bytes);
		_state.materializedUnpublishedBytes += bytes;
	}

	bool StaticImportCoordinator::RemoveGroup(std::uint64_t groupID, StaticGroupLifecycleStage terminalStage, std::vector<std::shared_ptr<StaticGroupLifecycleRecord>>* deferredReleases)
	{
		if (auto token = _state.groupIntentTokens.find(groupID);
			token != _state.groupIntentTokens.end()) {
			token->second->valid.store(false, std::memory_order_release);
			_state.groupIntentTokens.erase(token);
		}
		ClearGroupBlockers(groupID);
		_state.pendingQueued.erase(groupID);
		_state.workerInFlightGroups.erase(groupID);
		_state.producerActiveGroups.erase(groupID);
		_state.readyGroups.erase(groupID);
		if (const auto ticket = _state.readyGroupTickets.find(groupID);
			ticket != _state.readyGroupTickets.end()) {
			ReleasePreparedTicket(ticket->second);
			_state.readyGroupTickets.erase(ticket);
		}
		if (auto packet = _state.groupPacketIDs.find(groupID);
			packet != _state.groupPacketIDs.end()) {
			const auto packetID = packet->second;
			_state.groupPacketIDs.erase(packet);
			if (auto members = _state.packetLiveMemberCounts.find(packetID);
				members != _state.packetLiveMemberCounts.end() && members->second != 0) {
				if (--members->second == 0) {
					_state.packetLiveMemberCounts.erase(members);
					if (auto credits = _state.packetPreparationCredits.find(packetID);
						credits != _state.packetPreparationCredits.end()) {
						_state.activePreparationCredits -= (std::min)(
							_state.activePreparationCredits, credits->second);
						_state.packetPreparationCredits.erase(credits);
					}
					_state.packetArtifactVersions.erase(packetID);
					if (auto* requests = _shared->hasSceneIngestionServices
						? _shared->sceneIngestionServices.execution.stateRequests : nullptr) {
						requests->Cancel({ br::render::ArtifactKind::StaticGroup, packetID, 0 });
					}
				}
			}
		}
		_state.pendingGroupLastTransition.erase(groupID);
		auto pendingIt = _state.pendingGroups.find(groupID);
		if (pendingIt == _state.pendingGroups.end()) {
			return false;
		}
		// Final record destruction releases recipe owners, strings, override
		// arrays, and reservation state. Keep it off the serialized coordinator
		// when a publication acknowledgement retires a large batch.
		auto retiredRecord = pendingIt->second;
		_state.stats.pendingPlacements -= (std::min)(
			_state.stats.pendingPlacements,
			retiredRecord->descriptor.placementCount);
		retiredRecord->stage.store(terminalStage,
			std::memory_order_release);
		_lifecycle->Erase(groupID, retiredRecord.get());
		_state.pendingGroups.erase(pendingIt);
		if (!_state.graphOwnedGroupPlacementCounts.contains(groupID)) {
			_state.capacityHintDrawRecordsByGroup.erase(groupID);
		}
		if (deferredReleases) deferredReleases->push_back(std::move(retiredRecord));
		return true;
	}

	void StaticImportCoordinator::RefreshGraphCapacityHint()
	{
		if (!_shared->hasSceneIngestionServices) return;
		auto* objectManager = _shared->sceneIngestionServices.objects;
		if (!objectManager) return;

		// Object-buffer capacity is a high-water request. Reconstructing it from
		// every live group after each small prepared-cell batch makes discovery
		// quadratic in total group count. Groups contribute once when discovered;
		// removals intentionally do not force an eager buffer shrink.
		objectManager->RequestStaticImportGraphCapacityHint(
			_state.capacityHintTransformRows, _state.capacityHintDrawRecords);
	}

	bool StaticImportCoordinator::QueueGroupIfRunnable(std::uint64_t groupID)
	{
		if (_state.readyGroups.contains(groupID)) {
			_state.pendingGroupLastTransition[groupID] = "queue_rejected_ready";
			return false;
		}
		auto pendingIt = _state.pendingGroups.find(groupID);
		if (pendingIt == _state.pendingGroups.end()) {
			return false;
		}
		if (!pendingIt->second ||
			!IsGroupLive(*pendingIt->second)) {
			_state.pendingGroupLastTransition[groupID] = "queue_rejected_not_live";
			return false;
		}
		if (!_state.pendingQueued.insert(groupID).second) {
			_state.pendingGroupLastTransition[groupID] = "queue_rejected_marker_exists";
			return false;
		}
		_state.pendingGroupQueue.push_back(groupID);
		pendingIt->second->stage.store(StaticGroupLifecycleStage::Runnable,
			std::memory_order_release);
		_state.pendingGroupLastTransition[groupID] = "coordinator_queue";
		return true;
	}

	bool StaticImportCoordinator::SubmitDependencyRequest(StaticImportDependencyRequest request)
	{
		if (IsDependencyResolved(request) ||
			IsDependencyFailed(request) ||
			_state.pendingDependencyRequests.contains(request.key) ||
			_state.delayedDependencyRequests.contains(request.key)) {
			return false;
		}

		const auto key = request.key;
		switch (key.kind) {
		case StaticImportDependencyKind::Template:
			if (!EnqueueRequest(std::move(request.templateRequest))) {
				return false;
			}
			break;
		case StaticImportDependencyKind::Asset:
			if (_state.pendingAssetPublishKeys.insert(key.stringKey).second) {
				if (!_drains->PushWorker(_bus->staticImportAssetPublishJobs, request.assetRequest,
						StaticImportWorkSource::AssetPublish)) {
					_state.pendingAssetPublishKeys.erase(key.stringKey);
					return false;
				}
			}
			break;
		case StaticImportDependencyKind::MaterialVariant:
			if (_state.pendingMaterialVariantKeys.insert(key.stringKey).second) {
				if (!_drains->PushWorker(_bus->staticImportMaterialVariantJobs, request.materialVariantRequest,
						StaticImportWorkSource::MaterialVariant)) {
					_state.pendingMaterialVariantKeys.erase(key.stringKey);
					return false;
				}
			}
			break;
		case StaticImportDependencyKind::ShaderVariant:
			if (_state.pendingShaderVariantKeys.insert(key.stringKey).second) {
				if (!_drains->PushWorker(_bus->staticImportShaderVariantJobs, request.shaderVariantRequest,
						StaticImportWorkSource::ShaderVariant)) {
					_state.pendingShaderVariantKeys.erase(key.stringKey);
					return false;
				}
			}
			break;
		case StaticImportDependencyKind::Host:
			if (!request.hostRequest.owner->RequestHostDependency(request.hostRequest.dependency)) {
				return false;
			}
			break;
		}

		_state.pendingDependencyRequests.emplace(key, std::move(request));
		return true;
	}

	void StaticImportCoordinator::ResolveDependency(const StaticImportDependencyKey& key)
	{
		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::ResolveDependency");
		_state.pendingDependencyRequests.erase(key);
		_state.delayedDependencyRequests.erase(key);
		std::vector<std::vector<StaticCellKey>> hostWaiters(_state.hostCellBlocks.size());
		for (std::size_t index = 0; index < _state.hostCellBlocks.size(); ++index) {
			auto& blocks = _state.hostCellBlocks[index];
			if (auto waitersIt = blocks.dependencyWaiters.find(key); waitersIt != blocks.dependencyWaiters.end()) {
				hostWaiters[index].reserve(waitersIt->second.size());
				for (const auto& cellKey : waitersIt->second) {
					hostWaiters[index].push_back(cellKey);
				}
				blocks.dependencyWaiters.erase(waitersIt);
			}
		}
		std::vector<std::uint64_t> graphWaiters;
		if (auto waitersIt = _state.graphDependencyWaiters.find(key);
			waitersIt != _state.graphDependencyWaiters.end()) {
			graphWaiters.assign(waitersIt->second.begin(), waitersIt->second.end());
			_state.graphDependencyWaiters.erase(waitersIt);
		}
		for (const auto groupID : graphWaiters) {
			auto suspensionIt = _state.groupSuspensions.find(groupID);
			if (suspensionIt == _state.groupSuspensions.end()) continue;
			suspensionIt->second.remaining.erase(key);
			if (!suspensionIt->second.remaining.empty()) continue;
			const auto identity = suspensionIt->second.identity;
			_state.groupSuspensions.erase(suspensionIt);
			if (identity != 0 && _importState->staticGroupSuspensionNotifier) {
				_importState->staticGroupSuspensionNotifier(identity);
			} else {
				(void)QueueGroupIfRunnable(groupID);
			}
		}

		for (std::size_t index = 0; index < _state.hostCellBlocks.size(); ++index) {
			auto& blocks = _state.hostCellBlocks[index];
			std::vector<StaticCellKey> unblocked;
			for (const auto& cellKey : hostWaiters[index]) {
				auto blockedIt = blocks.blockedCells.find(cellKey);
				if (blockedIt == blocks.blockedCells.end()) {
					continue;
				}
				blockedIt->second.erase(key);
				if (!blockedIt->second.empty()) {
					continue;
				}
				blocks.blockedCells.erase(blockedIt);
				unblocked.push_back(cellKey);
			}
			_extensions[index]->OnHostCellsUnblocked(unblocked);
		}
	}

	void StaticImportCoordinator::FailDependency(const StaticImportDependencyKey& key)
	{
		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::FailDependency");
		_state.pendingDependencyRequests.erase(key);
		_state.delayedDependencyRequests.erase(key);
		for (std::size_t index = 0; index < _state.hostCellBlocks.size(); ++index) {
			auto& blocks = _state.hostCellBlocks[index];
			if (auto waitersIt = blocks.dependencyWaiters.find(key); waitersIt != blocks.dependencyWaiters.end()) {
				for (const auto& cellKey : waitersIt->second) {
					blocks.blockedCells.erase(cellKey);
					_extensions[index]->OnHostCellDropped(cellKey);
				}
				blocks.dependencyWaiters.erase(waitersIt);
			}
		}
		std::vector<std::uint64_t> graphWaiters;
		if (auto waitersIt = _state.graphDependencyWaiters.find(key);
			waitersIt != _state.graphDependencyWaiters.end()) {
			graphWaiters.assign(waitersIt->second.begin(), waitersIt->second.end());
			_state.graphDependencyWaiters.erase(waitersIt);
		}
		for (const auto groupID : graphWaiters) {
			auto suspensionIt = _state.groupSuspensions.find(groupID);
			if (suspensionIt == _state.groupSuspensions.end()) continue;
			const auto identity = suspensionIt->second.identity;
			for (const auto& remainingKey : suspensionIt->second.remaining) {
				if (remainingKey == key) continue;
				auto otherIt = _state.graphDependencyWaiters.find(remainingKey);
				if (otherIt == _state.graphDependencyWaiters.end()) continue;
				otherIt->second.erase(groupID);
				if (otherIt->second.empty()) _state.graphDependencyWaiters.erase(otherIt);
			}
			_state.groupSuspensions.erase(suspensionIt);
			if (identity != 0 && _importState->staticGroupSuspensionNotifier) {
				_importState->staticGroupSuspensionNotifier(identity);
			} else {
				(void)RemoveGroup(groupID);
			}
		}

	}

	bool StaticImportCoordinator::PumpDueDependencyRequests()
	{
		const auto now = std::chrono::steady_clock::now();
		constexpr std::size_t maxDueRequestsPerTick = 64;
		std::vector<StaticImportDependencyRequest> dueRequests;
		dueRequests.reserve((std::min)(maxDueRequestsPerTick, _state.delayedDependencyRequests.size()));
		for (auto it = _state.delayedDependencyRequests.begin();
			it != _state.delayedDependencyRequests.end() && dueRequests.size() < maxDueRequestsPerTick;) {
			if (it->second.dueAt > now) {
				++it;
				continue;
			}
			dueRequests.push_back(std::move(it->second.request));
			it = _state.delayedDependencyRequests.erase(it);
		}
		if (dueRequests.empty()) {
			return false;
		}

		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::PumpDueDependencyRequests");
		std::size_t submitted = 0;
		for (auto& request : dueRequests) {
			if (!HasHostCellWaiters(request.key) &&
				!_state.graphDependencyWaiters.contains(request.key)) {
				continue;
			}
			if (IsDependencyResolved(request)) {
				ResolveDependency(request.key);
			} else if (IsDependencyFailed(request)) {
				FailDependency(request.key);
			} else if (SubmitDependencyRequest(std::move(request))) {
				++submitted;
			}
		}
		BT_PLOT("SARP.StaticImportCoordinator.DependencyDelayedPollsSubmitted", static_cast<int64_t>(submitted));
		return true;
	}

	bool StaticImportCoordinator::PromoteNotifiedAssetDependencies()
	{
		const auto generation = _importState->staticImportAssetReadinessGeneration.load(
			std::memory_order_acquire);
		if (generation == _state.observedAssetReadinessGeneration) return false;
		_state.observedAssetReadinessGeneration = generation;
		const auto now = std::chrono::steady_clock::now();
		std::size_t promoted = 0;
		for (auto& [key, delayed] : _state.delayedDependencyRequests) {
			if (key.kind != StaticImportDependencyKind::Asset &&
				key.kind != StaticImportDependencyKind::MaterialVariant) continue;
			if (delayed.dueAt > now) {
				delayed.dueAt = now;
				++promoted;
			}
		}
		BT_PLOT("SARP.StaticImportCoordinator.AssetDependenciesCallbackPromoted",
			static_cast<int64_t>(promoted));
		return promoted != 0;
	}

	bool StaticImportCoordinator::ReconcileOrphanedAssetDependencies()
	{
		std::vector<StaticImportDependencyRequest> orphaned;
		orphaned.reserve(_state.graphDependencyWaiters.size());
		for (const auto& [key, waiters] : _state.graphDependencyWaiters) {
			if (waiters.empty() || key.kind != StaticImportDependencyKind::Asset ||
				_state.pendingDependencyRequests.contains(key) ||
				_state.delayedDependencyRequests.contains(key) ||
				_state.pendingAssetPublishKeys.contains(key.stringKey)) continue;
			orphaned.push_back(MakeStaticImportDependencyRequest(*_assets, StaticAssetPublishRequest{
				.modelPath = key.stringKey
			}));
		}
		if (orphaned.empty()) {
			return false;
		}

		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::ReconcileOrphanedAssets");
		std::size_t resolved = 0;
		std::size_t failed = 0;
		std::size_t resubmitted = 0;
		for (auto& request : orphaned) {
			const auto key = request.key;
			if (IsDependencyResolved(request)) {
				ResolveDependency(key);
				++resolved;
			} else if (IsDependencyFailed(request)) {
				FailDependency(key);
				++failed;
			} else if (SubmitDependencyRequest(std::move(request))) {
				++resubmitted;
			}
		}
		BT_PLOT("SARP.StaticImportCoordinator.OrphanedAssetDependenciesResolved", static_cast<int64_t>(resolved));
		BT_PLOT("SARP.StaticImportCoordinator.OrphanedAssetDependenciesFailed", static_cast<int64_t>(failed));
		BT_PLOT("SARP.StaticImportCoordinator.OrphanedAssetDependenciesResubmitted", static_cast<int64_t>(resubmitted));
		return resolved != 0 || failed != 0 || resubmitted != 0;
	}

	std::optional<std::chrono::steady_clock::time_point> StaticImportCoordinator::NextDependencyPollDue() const
	{
		std::optional<std::chrono::steady_clock::time_point> nextDue;
		for (const auto& [_, delayed] : _state.delayedDependencyRequests) {
			if (!nextDue || delayed.dueAt < *nextDue) {
				nextDue = delayed.dueAt;
			}
		}
		return nextDue;
	}

	void StaticImportCoordinator::RefreshBlockedStats()
	{
		std::uint64_t blockedAssets = 0;
		std::uint64_t blockedVariants = 0;
		std::uint64_t blockedTemplates = 0;
		std::uint64_t blockedShaderVariants = 0;
		for (const auto& [_, suspension] : _state.groupSuspensions) {
			bool hasAsset = false;
			bool hasVariant = false;
			bool hasTemplate = false;
			bool hasShaderVariant = false;
			for (const auto& dependency : suspension.remaining) {
				hasAsset = hasAsset || dependency.kind == StaticImportDependencyKind::Asset;
				hasVariant = hasVariant || dependency.kind == StaticImportDependencyKind::MaterialVariant;
				hasTemplate = hasTemplate || dependency.kind == StaticImportDependencyKind::Template;
				hasShaderVariant = hasShaderVariant || dependency.kind == StaticImportDependencyKind::ShaderVariant;
				hasAsset = hasAsset || dependency.kind == StaticImportDependencyKind::Host;
			}
			blockedAssets += hasAsset ? 1 : 0;
			blockedVariants += hasVariant ? 1 : 0;
			blockedTemplates += hasTemplate ? 1 : 0;
			blockedShaderVariants += hasShaderVariant ? 1 : 0;
		}
		const auto blockedHostCells = BlockedHostCellCount();
		blockedAssets += blockedHostCells;
		_state.stats.groupsWaitingAssets = blockedAssets;
		_state.stats.groupsWaitingVariants = blockedVariants;
		_state.stats.groupsWaitingTemplates = blockedTemplates;
		_state.stats.groupsWaitingShaderVariants = blockedShaderVariants;
		BT_PLOT("SARP.StaticImportCoordinator.BlockedAssetGroups", static_cast<int64_t>(blockedAssets));
		BT_PLOT("SARP.StaticImportCoordinator.BlockedVariantGroups", static_cast<int64_t>(blockedVariants));
		BT_PLOT("SARP.StaticImportCoordinator.BlockedTemplateGroups", static_cast<int64_t>(blockedTemplates));
		BT_PLOT("SARP.StaticImportCoordinator.BlockedShaderVariantGroups", static_cast<int64_t>(blockedShaderVariants));
		BT_PLOT("SARP.StaticImportCoordinator.GraphDependencyWaiterKeys", static_cast<int64_t>(_state.graphDependencyWaiters.size()));
		std::size_t hostDependencyWaiterKeys = 0;
		for (const auto& blocks : _state.hostCellBlocks) {
			hostDependencyWaiterKeys += blocks.dependencyWaiters.size();
		}
		BT_PLOT("SARP.StaticImportCoordinator.HostDependencyWaiterKeys", static_cast<int64_t>(hostDependencyWaiterKeys));
		BT_PLOT("SARP.StaticImportCoordinator.BlockedHostCells", static_cast<int64_t>(blockedHostCells));
		BT_PLOT("SARP.StaticImportCoordinator.PendingDependencyRequests", static_cast<int64_t>(_state.pendingDependencyRequests.size()));
		BT_PLOT("SARP.StaticImportCoordinator.DelayedDependencyRequests", static_cast<int64_t>(_state.delayedDependencyRequests.size()));
	}

	void StaticImportCoordinator::PublishStats(bool force)
	{
		const auto now = std::chrono::steady_clock::now();
		if (!force && _state.lastStatsPublish != std::chrono::steady_clock::time_point{} &&
			now - _state.lastStatsPublish < std::chrono::milliseconds(250)) return;
		_state.lastStatsPublish = now;
		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::PublishStats");
		RefreshBlockedStats();
		if (_importState->benchmarkDiagnosticsEnabled && BlockedHostCellCount() != 0 &&
			(force || _state.lastBlockedHostCellDiagnostic == std::chrono::steady_clock::time_point{} ||
			 now - _state.lastBlockedHostCellDiagnostic >= std::chrono::seconds(10))) {
			_state.lastBlockedHostCellDiagnostic = now;
			std::size_t emitted = 0;
			for (std::size_t index = 0; index < _state.hostCellBlocks.size() && emitted < 32; ++index) {
				const auto& blocks = _state.hostCellBlocks[index];
				for (const auto& [cell, dependencies] : blocks.blockedCells) {
					for (const auto& dependency : dependencies) {
						spdlog::info(
							"SARP blocked host cell dependency: extension={} world={:08X} cell=({}, {}) kind={} key='{}' pending={} delayed={} waiter_cells={}",
							index, cell.worldFormID, cell.cellX, cell.cellY,
							static_cast<unsigned>(dependency.kind), dependency.stringKey,
							_state.pendingDependencyRequests.contains(dependency),
							_state.delayedDependencyRequests.contains(dependency),
							blocks.dependencyWaiters.contains(dependency)
								? blocks.dependencyWaiters.at(dependency).size() : 0u);
						if (++emitted >= 32) break;
					}
					if (emitted >= 32) break;
				}
			}
		}
		auto stats = _state.stats;
		stats.desiredCells = _state.desiredCells.size();
		stats.activeCells = _state.activeCellGroups.size();
		stats.pendingCellsQueued = _state.pendingCellQueue.size();
		stats.pendingGroups = _state.pendingGroups.size();
		stats.readyGroupsTracked = _state.readyGroups.size();
		stats.readyGroupsQueued = _bus->staticImportGroupAssemblyJobs.Size() +
			_bus->staticImportCompletedOutput.Size();
		BT_PLOT("SARP.StaticImportCoordinator.PendingGroupRecords",
			static_cast<int64_t>(stats.pendingGroups));
		BT_PLOT("SARP.StaticImportCoordinator.ReadyGroupsTracked",
			static_cast<int64_t>(stats.readyGroupsTracked));
		BT_PLOT("SARP.StaticImportCoordinator.PreparedUnpublishedBytes",
			static_cast<int64_t>(_state.preparedUnpublishedBytes));
		BT_PLOT("SARP.StaticImportCoordinator.MaterializedUnpublishedBytes",
			static_cast<int64_t>(_state.materializedUnpublishedBytes));
		const auto capacity = _capacity->GetSnapshot();
		BT_PLOT("SARP.StaticImportCapacity.PreparedBytes",
			static_cast<int64_t>(capacity.reserved.preparedBytes));
		BT_PLOT("SARP.StaticImportCapacity.DrawRecords",
			static_cast<int64_t>(capacity.reserved.drawRecords));
		BT_PLOT("SARP.StaticImportCapacity.ActiveIndices",
			static_cast<int64_t>(capacity.reserved.activeIndices));
		BT_PLOT("SARP.StaticImportCapacity.UploadBytes",
			static_cast<int64_t>(capacity.reserved.uploadBytes));
		if (_importState->benchmarkDiagnosticsEnabled && !_state.pendingGroups.empty() &&
			_state.workerInFlightGroups.empty() && _state.readyGroups.empty() &&
			(force || _state.lastPendingGroupDiagnostic == std::chrono::steady_clock::time_point{} ||
			 now - _state.lastPendingGroupDiagnostic >= std::chrono::seconds(10))) {
			_state.lastPendingGroupDiagnostic = now;
			spdlog::info(
				"SARP pending static admission: groups={} queued={} markers={} credits={}/{} "
				"prepared={} materialized={} broker(prepared={}/{} draw={}/{} active={}/{} upload={}/{})",
				_state.pendingGroups.size(), _state.pendingGroupQueue.size(), _state.pendingQueued.size(),
				_state.activePreparationCredits,
				_settings->preparationCreditLimit,
				_state.preparedUnpublishedBytes, _state.materializedUnpublishedBytes,
				capacity.reserved.preparedBytes, capacity.limits.preparedBytes,
				capacity.reserved.drawRecords, capacity.limits.drawRecords,
				capacity.reserved.activeIndices, capacity.limits.activeIndices,
				capacity.reserved.uploadBytes, capacity.limits.uploadBytes);
		}
		if (_importState->benchmarkDiagnosticsEnabled && !_state.readyGroups.empty() &&
			_bus->staticImportGroupAssemblyJobs.Empty() && _bus->staticImportCompletedOutput.Empty() &&
			_bus->staticImportTransactionMaterializeJobs.Empty() &&
			_bus->staticImportMaterializedValidationRequests.Empty() &&
			_bus->staticImportMaterializedPublishDecisions.Empty() &&
			_bus->staticImportPublicationJobs.Empty() &&
			!_importState->staticImportPublicationCarryPending.load(std::memory_order_acquire) &&
			_bus->staticImportPublicationResults.Empty() &&
			_bus->staticImportGpuReadyPublicationResults.Empty() &&
			_importState->staticImportGpuPendingPublications.empty() &&
			(force || _state.lastPendingGroupDiagnostic == std::chrono::steady_clock::time_point{} ||
			 now - _state.lastPendingGroupDiagnostic >= std::chrono::seconds(5))) {
			_state.lastPendingGroupDiagnostic = now;
			spdlog::warn(
				"SARP ready-group acknowledgement orphan: ready={} pending={} transaction_events(published={} groups={} accepted={} rejected={})",
				_state.readyGroups.size(), _state.pendingGroups.size(),
				_importState->staticTransactionReadyEventsPublished.load(std::memory_order_relaxed),
				_importState->staticTransactionReadyGroupsPublished.load(std::memory_order_relaxed),
				_importState->staticTransactionReadyEventsAccepted.load(std::memory_order_relaxed),
				_importState->staticTransactionReadyEventsRejected.load(std::memory_order_relaxed));
			std::size_t emitted = 0;
			for (const auto groupID : _state.readyGroups) {
				const auto ticket = _state.readyGroupTickets.find(groupID);
				spdlog::warn(
					"SARP ready-group orphan sample: group=0x{:016X} ticket={} graph_owned={} versioned={} pending={} packet={}",
					groupID, ticket != _state.readyGroupTickets.end() ? ticket->second : 0,
					_state.graphOwnedActiveGroups.contains(groupID),
					_state.graphOwnedGroupVersions.contains(groupID),
					_state.pendingGroups.contains(groupID),
					_state.groupPacketIDs.contains(groupID));
				if (++emitted == 8) break;
			}
		}
		stats.assetPublishRequestsQueued = _bus->staticImportAssetPublishJobs.Size();
		stats.templateRequestsQueued = _bus->staticImportTemplateRequestOutput.Size();
		stats.materialVariantRequestsQueued = _bus->staticImportMaterialVariantJobs.Size();
		stats.shaderVariantRequestsQueued = _bus->staticImportShaderVariantJobs.Size();
		stats.removalDiffsQueued = _bus->staticImportRemovalOutput.Size();
		stats.retirementRequestsQueued = _bus->staticRetirementOwnershipRequests.Size() +
			_bus->staticRetirementPayloadRequests.Size();
		{
			std::lock_guard lock(_importState->staticRetirementMutex);
			stats.retirementsAwaitingPublication = _importState->staticRetirementsAwaitingPublication.size();
		}
		stats.retirementCompletionsQueued = _bus->staticRetirementCompletions.Size();
		stats.pendingHostCells = BlockedHostCellCount();
		for (const auto* extension : _extensions) {
			stats.pendingHostCells += extension->PendingHostCellCount();
		}
		stats.pendingGroupDiagnostics.clear();
		// A single failed variant can fan out to hundreds of placement groups at
		// large radii. Keep this diagnostic bounded, but do not suppress it merely
		// because the terminal asset is widely instanced.
		if (_state.pendingGroups.size() <= 2048) {
			auto dependencyName = [](StaticImportDependencyKind kind) {
				switch (kind) {
				case StaticImportDependencyKind::Template: return "template";
				case StaticImportDependencyKind::Asset: return "asset";
				case StaticImportDependencyKind::MaterialVariant: return "material_variant";
				case StaticImportDependencyKind::ShaderVariant: return "shader_variant";
				case StaticImportDependencyKind::Host: return "host";
				}
				return "unknown";
			};
			for (const auto& [groupID, record] : _state.pendingGroups) {
				if (!record) continue;
				const auto& group = record->descriptor;
				std::string stage = "unqueued";
				if (_state.readyGroups.contains(groupID)) stage = "ready_tracked";
				else if (_state.groupSuspensions.contains(groupID)) stage = "graph_suspended";
				else if (std::find(_state.pendingGroupQueue.begin(), _state.pendingGroupQueue.end(), groupID) != _state.pendingGroupQueue.end()) stage = "coordinator_queue";
				else if (_state.workerInFlightGroups.contains(groupID)) stage = "worker_in_flight";
				else if (_state.pendingQueued.contains(groupID)) stage = "coordinator_queue_marker";
				std::string dependencies;
				if (const auto suspended = _state.groupSuspensions.find(groupID); suspended != _state.groupSuspensions.end()) {
					for (const auto& dependency : suspended->second.remaining) {
						if (!dependencies.empty()) dependencies += ',';
						dependencies += fmt::format("{}:{}:{:X}:{:X}:{:X}", dependencyName(dependency.kind),
							dependency.stringKey, dependency.numericKey, dependency.numericKey2, dependency.numericKey3);
					}
				}
				const auto transition = _state.pendingGroupLastTransition.find(groupID);
				std::size_t pendingDependencyCount = 0;
				std::size_t delayedDependencyCount = 0;
				if (const auto suspended = _state.groupSuspensions.find(groupID); suspended != _state.groupSuspensions.end()) {
					for (const auto& dependency : suspended->second.remaining) {
						pendingDependencyCount += _state.pendingDependencyRequests.contains(dependency) ? 1u : 0u;
						delayedDependencyCount += _state.delayedDependencyRequests.contains(dependency) ? 1u : 0u;
					}
				}
				stats.pendingGroupDiagnostics.push_back(fmt::format(
					"group=0x{:016X} stage={} last_transition={} placements={} model='{}' cell=0x{:08X}/0x{:08X}/({},{}) residency={} residency_generation={} live={} target_valid={} target_world=0x{:08X} target_interior=0x{:08X} target_generation={} dependencies=[{}] pending_dependency_requests={} delayed_dependency_requests={} worker_drains_active={} group_jobs={} group_results={} asset_jobs={} asset_results={}",
					groupID, stage, transition == _state.pendingGroupLastTransition.end() ? "none" : transition->second,
					group.placementCount, group.ModelPath(), group.cellKey.worldFormID,
					group.cellKey.interiorCellFormID, group.cellKey.cellX, group.cellKey.cellY,
					static_cast<unsigned>(group.residencyClass), group.residencyGeneration,
					IsGroupLive(*record), _state.latestTarget.valid,
					_state.latestTarget.stream.worldFormID, _state.latestTarget.stream.interiorCellFormID,
					_state.stats.generation,
					dependencies, pendingDependencyCount, delayedDependencyCount,
					_importState->staticImportWorkerDrainsActive.load(std::memory_order_acquire),
					0, _shared->graphCompletions->StaticGroupCount(),
					_bus->staticImportAssetPublishJobs.Size(), _bus->staticImportAssetPublishResults.Size()));
			}
		}
		{
			std::lock_guard lock(_importState->staticImportServiceStatsMutex);
			_importState->staticImportServicePublishedStats = stats;
		}
		BT_PLOT("SARP.StaticImportCoordinator.PendingGroups", static_cast<int64_t>(stats.pendingGroups));
		BT_PLOT("SARP.StaticImportLifecycle.Shards",
			static_cast<int64_t>(_lifecycle->ShardCount()));
		BT_PLOT("SARP.StaticImportCoordinator.PendingCellsQueued", static_cast<int64_t>(stats.pendingCellsQueued));
		BT_PLOT("SARP.StaticImportCoordinator.PendingPlacements", static_cast<int64_t>(stats.pendingPlacements));
		BT_PLOT("SARP.StaticImportCoordinator.ReadyQueueDepth", static_cast<int64_t>(stats.readyGroupsQueued));
		BT_PLOT("SARP.StaticImportCoordinator.RemovalQueueDepth", static_cast<int64_t>(stats.removalDiffsQueued));
		BT_PLOT("SARP.StaticImportCoordinator.TemplateQueueDepth", static_cast<int64_t>(stats.templateRequestsQueued));
		BT_PLOT("SARP.StaticImportCoordinator.ShaderVariantQueueDepth", static_cast<int64_t>(stats.shaderVariantRequestsQueued));
		const auto wakeStats = _pump->GetStats();
		BT_PLOT("SARP.StaticImportCoordinator.WakeRequestedEpoch",
			static_cast<int64_t>(wakeStats.requestedEpoch));
		BT_PLOT("SARP.StaticImportCoordinator.WakeDrainedEpoch",
			static_cast<int64_t>(wakeStats.drainedEpoch));
		BT_PLOT("SARP.StaticImportCoordinator.WakeRequests",
			static_cast<int64_t>(wakeStats.notifications));
		BT_PLOT("SARP.StaticImportCoordinator.WakeCoalesced",
			static_cast<int64_t>(wakeStats.coalescedNotifications));
		BT_PLOT("SARP.StaticImportCoordinator.WakeDrainPasses",
			static_cast<int64_t>(wakeStats.drainPasses));
		BT_PLOT("SARP.StaticImportCoordinator.WakeExitRaceRetries",
			static_cast<int64_t>(wakeStats.handoffRetries));
		BT_PLOT("SARP.StaticImportCoordinator.WakeTimerRequests",
			static_cast<int64_t>(wakeStats.delayedRequests));
		BT_PLOT("SARP.StaticImportCoordinator.WakeTimerCoalesced",
			static_cast<int64_t>(wakeStats.delayedCoalesced));
		BT_PLOT("SARP.StaticImportCoordinator.WakeTimerFired",
			static_cast<int64_t>(wakeStats.delayedFired));
		BT_PLOT("SARP.StaticImportCoordinator.WakeTimerStale",
			static_cast<int64_t>(wakeStats.staleDelayed));
		BT_PLOT("SARP.StaticImportCoordinator.WakeRejected",
			static_cast<int64_t>(wakeStats.rejected));
		BT_PLOT("SARP.StaticImportCoordinator.WakeRunnerActive",
			wakeStats.runnerActive ? int64_t{ 1 } : int64_t{ 0 });
		BT_PLOT("SARP.StaticImportCoordinator.WakeTimerArmed",
			wakeStats.delayedArmed ? int64_t{ 1 } : int64_t{ 0 });
		static constexpr std::array<std::string_view, StaticImportWorkScheduler::SourceCount> sourceNames{
			"Publication", "TemplatePublication", "RetirementApply", "RetirementCommit",
			"MaterializedDecisionCleanup", "RemovalCleanup",
			"GroupArtifactRelease", "GroupAssembly", "TransactionMaterialize",
			"AssetPublish", "MaterialVariant", "ShaderVariant", "HostDependency",
			"HostPrepare", "CellPrepare", "RecipeCompile", "SpeculativeAssetPublish"
		};
		for (std::size_t sourceIndex = 0; sourceIndex < sourceNames.size(); ++sourceIndex) {
			const auto snapshot = _shared->staticImportWorkScheduler.Snapshot(
				static_cast<StaticImportWorkSource>(sourceIndex));
			const auto pending = snapshot.published > snapshot.claimed
				? snapshot.published - snapshot.claimed : 0;
			basic_telemetry::SetGauge(fmt::format("SARP.StaticImport.Scheduler.{}.Pending",
				sourceNames[sourceIndex]), static_cast<std::int64_t>(pending));
			basic_telemetry::SetGauge(fmt::format("SARP.StaticImport.Scheduler.{}.Productive",
				sourceNames[sourceIndex]), static_cast<std::int64_t>(snapshot.productive));
			basic_telemetry::SetGauge(fmt::format("SARP.StaticImport.Scheduler.{}.StaleClaims",
				sourceNames[sourceIndex]), static_cast<std::int64_t>(snapshot.staleClaims));
			basic_telemetry::SetGauge(fmt::format("SARP.StaticImport.Scheduler.{}.MaxLatencyUs",
				sourceNames[sourceIndex]), static_cast<std::int64_t>(snapshot.maxSchedulingLatencyUs));
		}
	}

	bool StaticImportCoordinator::QueueCellJob(const StaticCellKey& cell, std::uint64_t generation, std::shared_ptr<br::staticimport::recipe::CellRecipeView> mappedRecipe, std::shared_ptr<StaticRecipeSceneBinding> recipeSceneBinding, std::shared_ptr<br::staticimport::recipe::MappedRecipePack> mappedRecipePack)
	{
		if (!_state.desiredCells.contains(cell) ||
			_state.activeCellGroups.contains(cell) ||
			!_state.pendingCellKeys.insert(cell).second) {
			return false;
		}
		_state.pendingCellQueue.push_back(StaticImportCellPrepareJob{
			.key = cell,
			.generation = generation,
			.mappedRecipe = std::move(mappedRecipe),
			.recipeSceneBinding = std::move(recipeSceneBinding),
			.mappedRecipePack = std::move(mappedRecipePack)
		});
		return true;
	}

	void StaticImportCoordinator::ProcessTarget(StaticImportCameraTarget target)
	{
		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::ProcessCameraTarget");
		if (target.generation != _importState->staticImportServiceTargetGeneration.load(std::memory_order_acquire)) {
			return;
		}
		_state.latestTarget = target;
		{
			// Extension target reconciliation (e.g. host-group fallbacks) belongs to
			// the same serialized ownership lane as static-scene membership.  Camera
			// publication is the only main-thread input; repeated coordinator ticks
			// do not rescan host groups.
			std::lock_guard publicationLock(_importState->staticImportPublicationWorkerMutex);
			for (auto* extension : _extensions) {
				extension->ProcessStreamTarget(target.stream);
			}
		}
		// Host-only cells (e.g. grass) are not necessarily present in
		// activeCellGroups, and camera updates within one static cell take the fast
		// path below. Retire obsolete host dependency waiters before that fast path
		// so requests captured along the benchmark flight cannot strand quiescence.
		DropUnwantedHostCells(target);

		std::vector<StaticCellKey> enteredCells;
		std::vector<StaticCellKey> leftCells;
		const auto previousDesiredCellCount = _state.desiredCells.size();
		std::size_t desiredCellCount = 0;
		std::size_t enteredAlreadyActiveCount = 0;
		std::size_t leftStillDesiredCount = 0;
		const auto diffBegin = std::chrono::steady_clock::now();
		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::DiffDesiredCells");
			if (!target.valid) {
				leftCells.reserve(_state.activeCellGroups.size());
				for (const auto& [cell, _] : _state.activeCellGroups) leftCells.push_back(cell);
				_state.desiredCells.clear();
				_state.lastKey.reset();
			} else if (_state.lastKey && *_state.lastKey == target.key) {
				return;
			} else if (_state.lastKey) {
				const auto previousDesired = _state.desiredCells;
				const auto desiredCells = _hooks.buildDesiredStaticCells(target.stream);
				desiredCellCount = desiredCells.size();
				ForEachRealStaticCellInAreaDifference(desiredCells, previousDesired, [&](const StaticCellKey& cell) {
					if (!_state.activeCellGroups.contains(cell)) {
						enteredCells.push_back(cell);
					} else {
						++enteredAlreadyActiveCount;
					}
				});
				for (const auto& [cell, _] : _state.activeCellGroups) {
					if (!desiredCells.contains(cell)) leftCells.push_back(cell);
				}
				leftStillDesiredCount = previousDesired.size() > leftCells.size()
					? previousDesired.size() - leftCells.size()
					: 0;
				_state.desiredCells = desiredCells;
				_state.lastKey = target.key;
			} else {
				auto desiredCells = _hooks.buildDesiredStaticCells(target.stream);
				desiredCellCount = desiredCells.size();
				const auto worldCells = _placements->ExteriorCellCount(target.stream.worldFormID);
				enteredCells.reserve(worldCells != 0 ? worldCells : 1u);
				ForEachRealStaticCellInAreaDifference(desiredCells, {}, [&](const StaticCellKey& cell) {
					enteredCells.push_back(cell);
				});
				_state.desiredCells = std::move(desiredCells);
				_state.lastKey = target.key;
			}
		}
		std::vector<std::uint64_t> staleWorldLodGroups;
		for (const auto& [groupID, record] : _state.pendingGroups) {
			if (!record) continue;
			const auto& group = record->descriptor;
			if (group.residencyClass == StaticImportResidencyClass::HostGroup) {
				if (IsGroupLive(*record)) {
					(void)QueueGroupIfRunnable(groupID);
				} else {
					staleWorldLodGroups.push_back(groupID);
				}
			}
		}
		for (const auto groupID : staleWorldLodGroups) {
			(void)RemoveGroup(groupID);
			++_state.stats.staleGroupsDropped;
		}

		if (!enteredCells.empty() || !leftCells.empty() || !target.valid) {
			MarkStaticImportCoordinatorLivenessChanged();
		}

		_state.stats.generation = target.generation;
		_state.stats.cellDiffUs += ElapsedUs(diffBegin);
		_state.stats.cellsEntered += enteredCells.size();
		_state.stats.cellsLeft += leftCells.size();
		BT_PLOT("SARP.StaticImportCoordinator.EnteredCells", static_cast<int64_t>(enteredCells.size()));
		BT_PLOT("SARP.StaticImportCoordinator.LeftCells", static_cast<int64_t>(leftCells.size()));
		BT_PLOT("SARP.StaticImportCoordinator.PreviousDesiredCells", static_cast<int64_t>(previousDesiredCellCount));
		BT_PLOT("SARP.StaticImportCoordinator.DesiredCells", static_cast<int64_t>(_state.desiredCells.size()));
		BT_PLOT("SARP.StaticImportCoordinator.EnteredAlreadyActiveCells", static_cast<int64_t>(enteredAlreadyActiveCount));
		BT_PLOT("SARP.StaticImportCoordinator.LeftStillDesiredCells", static_cast<int64_t>(leftStillDesiredCount));
		const bool fullRadiusRemovalCandidate = target.valid &&
			previousDesiredCellCount != 0 &&
			leftCells.size() > (std::max<std::size_t>)(1, desiredCellCount / 2);
		BT_PLOT("SARP.StaticImportCoordinator.FullRadiusRemovalCandidate", fullRadiusRemovalCandidate ? int64_t{ 1 } : int64_t{ 0 });
		if (_importState->benchmarkDiagnosticsEnabled &&
			fullRadiusRemovalCandidate &&
			_importState->staticRemovalTelemetryLogCount.fetch_add(1, std::memory_order_relaxed) < 64) {
			spdlog::warn(
				"SARP static import telemetry: coordinator queued large left-cell diff prevDesired={} desired={} entered={} left={} target={}",
				previousDesiredCellCount,
				_state.desiredCells.size(),
				enteredCells.size(),
				leftCells.size(),
				FormatStaticStreamKey(target.key));
		}

		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::SortCellDiffs");
			std::sort(enteredCells.begin(), enteredCells.end(), [&target](const StaticCellKey& lhs, const StaticCellKey& rhs) {
				const auto lhsDistance = StaticCellDistanceToTarget(target.stream, lhs);
				const auto rhsDistance = StaticCellDistanceToTarget(target.stream, rhs);
				if (lhsDistance != rhsDistance) {
					return lhsDistance < rhsDistance;
				}
				if (lhs.cellY != rhs.cellY) {
					return lhs.cellY < rhs.cellY;
				}
				return lhs.cellX < rhs.cellX;
			});
			std::sort(leftCells.begin(), leftCells.end(), [&target](const StaticCellKey& lhs, const StaticCellKey& rhs) {
				const auto lhsDistance = StaticCellDistanceToTarget(target.stream, lhs);
				const auto rhsDistance = StaticCellDistanceToTarget(target.stream, rhs);
				if (lhsDistance != rhsDistance) {
					return lhsDistance > rhsDistance;
				}
				if (lhs.cellY != rhs.cellY) {
					return lhs.cellY < rhs.cellY;
				}
				return lhs.cellX < rhs.cellX;
			});
		}

		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::QueueLeftCellRemovals");
			// A left cell removes its groups through the flat intake, farthest cell first.
			for (const auto& cell : leftCells) {
				DropHostCell(cell);
				if (auto groupsIt = _state.activeCellGroups.find(cell); groupsIt != _state.activeCellGroups.end()) {
					const auto groupIDs = std::move(groupsIt->second);
					_state.activeCellGroups.erase(groupsIt);
					ApplyGroupChanges({}, groupIDs);
				}
			}
		}

		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::QueueEnteredCellJobs");
			// A full-radius batch contains thousands of cells and roughly one hundred
			// thousand logical groups. Parsing every recipe, deduplicating every
			// variant, and priming every asset here turns the serialized coordinator
			// wake into a multi-second stop-the-world boundary. Share the immutable
			// mapped pack and let independent cell tasks parse it in the TBB arena.
			if (enteredCells.size() > 256) {
				std::shared_ptr<br::staticimport::recipe::MappedRecipePack> mappedRecipePack;
				auto recipeSceneBinding = std::make_shared<StaticRecipeSceneBinding>();
				if (_importState->staticSceneRecipeStore && !enteredCells.empty()) {
					const br::staticimport::recipe::CellKey recipeKey{
						enteredCells.front().worldFormID, enteredCells.front().interiorCellFormID,
						enteredCells.front().cellX, enteredCells.front().cellY };
					mappedRecipePack = _importState->staticSceneRecipeStore->OpenNamespace(recipeKey).pack;
				}
				for (const auto& cell : enteredCells) {
					(void)QueueCellJob(
						cell, target.generation, {}, recipeSceneBinding, mappedRecipePack);
				}
				BT_PLOT("SARP.StaticSceneRecipe.SceneLookupDeferredCells",
					static_cast<int64_t>(enteredCells.size()));
			} else {
			std::vector<std::shared_ptr<br::staticimport::recipe::CellRecipeView>> mappedCells(enteredCells.size());
			std::shared_ptr<StaticRecipeSceneBinding> recipeSceneBinding;
			if (_importState->staticSceneRecipeStore && !enteredCells.empty()) {
				BT_ZONE_SCOPE("SARP::StaticSceneRecipe::SceneBatchLookup");
				std::vector<br::staticimport::recipe::CellKey> recipeKeys;
				recipeKeys.reserve(enteredCells.size());
				for (const auto& cell : enteredCells) {
					recipeKeys.push_back({ cell.worldFormID, cell.interiorCellFormID, cell.cellX, cell.cellY });
				}
				auto opened = _importState->staticSceneRecipeStore->OpenNamespace(recipeKeys.front());
				if (opened.pack) {
					auto scene = opened.pack->LookupCells(recipeKeys);
					recipeSceneBinding = std::make_shared<StaticRecipeSceneBinding>();
					std::unordered_map<StaticRecipeVariantKey, std::uint32_t, StaticRecipeVariantKeyHash, StaticRecipeVariantKeyEqual> variantIndices;
					std::unordered_set<std::string> uniqueAssets;
					std::vector<std::string> orderedAssets;
					std::array<std::uint64_t, 5> lookupStatusCounts{};
					for (std::size_t index = 0; index < scene.cells.size(); ++index) {
						auto& cell = scene.cells[index];
						const auto statusIndex = static_cast<std::size_t>(cell.status);
						if (statusIndex < lookupStatusCounts.size()) ++lookupStatusCounts[statusIndex];
						if (cell.status != br::staticimport::recipe::LoadStatus::Hit) {
							continue;
						}
						for (const auto& group : cell.view.Groups()) {
							std::string asset{ cell.view.String(group.modelPath) };
							if (uniqueAssets.emplace(asset).second) orderedAssets.push_back(std::move(asset));
							const StaticRecipeVariantLookupKey lookupKey{ cell.view.String(group.modelPath), group.materialOverrideHash };
							auto found = variantIndices.find(lookupKey);
							std::uint32_t variantIndex = 0;
							if (found == variantIndices.end()) {
								variantIndex = static_cast<std::uint32_t>(recipeSceneBinding->variants.size());
								auto slot = std::make_shared<StaticRecipeSceneVariantSlot>();
								slot->modelPath = lookupKey.modelPath;
								slot->materialVariant = _assets->MakeMaterialVariant(cell.view, group);
								{
									std::shared_lock lock(_importState->staticRecipeVariantBindingMutex);
									if (const auto cached = _importState->staticRecipeVariantBindings.find(lookupKey);
										cached != _importState->staticRecipeVariantBindings.end()) {
										slot->binding = cached->second;
										slot->readyBinding.store(slot->binding.get(), std::memory_order_relaxed);
									}
								}
							recipeSceneBinding->variants.push_back(std::move(slot));
							found = variantIndices.emplace(
								StaticRecipeVariantKey{ std::string{ lookupKey.modelPath }, group.materialOverrideHash }, variantIndex).first;
						} else {
							variantIndex = found->second;
						}
						recipeSceneBinding->variantByKey.try_emplace(
							StaticRecipeVariantKey{ std::string{ lookupKey.modelPath }, group.materialOverrideHash },
							recipeSceneBinding->variants[variantIndex]);
						recipeSceneBinding->variantByGroupID.insert_or_assign(group.stableGroupID, variantIndex);
						}
						mappedCells[index] = std::make_shared<br::staticimport::recipe::CellRecipeView>(std::move(cell.view));
					}
					// Prime the deduplicated scene assets cheaply. Idle import workers sweep
					// completed speculative results so none remain stranded at shutdown.
					for (const auto& asset : orderedAssets) {
						(void)_assets->QueueOrPollRenderableAsset(asset);
					}
					BT_PLOT("SARP.StaticSceneRecipe.SceneLookupCells", static_cast<int64_t>(scene.cells.size()));
					BT_PLOT("SARP.StaticSceneRecipe.SceneLookupHits", static_cast<int64_t>(scene.hitCount));
					BT_PLOT("SARP.StaticSceneRecipe.SceneLookupMissing", static_cast<int64_t>(lookupStatusCounts[static_cast<std::size_t>(br::staticimport::recipe::LoadStatus::Missing)]));
					BT_PLOT("SARP.StaticSceneRecipe.SceneLookupStale", static_cast<int64_t>(lookupStatusCounts[static_cast<std::size_t>(br::staticimport::recipe::LoadStatus::Stale)]));
					BT_PLOT("SARP.StaticSceneRecipe.SceneLookupCorrupt", static_cast<int64_t>(lookupStatusCounts[static_cast<std::size_t>(br::staticimport::recipe::LoadStatus::Corrupt)]));
					BT_PLOT("SARP.StaticSceneRecipe.SceneUniqueAssets", static_cast<int64_t>(orderedAssets.size()));
					BT_PLOT("SARP.StaticSceneRecipe.SceneUniqueVariants", static_cast<int64_t>(recipeSceneBinding->variants.size()));
				}
			}
			for (std::size_t index = 0; index < enteredCells.size(); ++index) {
				(void)QueueCellJob(
					enteredCells[index], target.generation, std::move(mappedCells[index]), recipeSceneBinding);
			}
			}
		}
	}

	bool StaticImportCoordinator::DrainTemplateRefs()
	{
		std::vector<StaticTemplateRefsPublished> publishedPackets;
		// Template snapshots are immutable while worker jobs retain them. Each drain
		// can therefore require copying the complete, steadily growing ref map.
		// Coalesce a larger queue backlog into one snapshot publication so bursts do
		// not repeatedly copy almost-identical maps on consecutive coordinator ticks.
		_bus->staticImportTemplateRefsPublished.Drain(publishedPackets, 512);
		if (publishedPackets.empty()) {
			return false;
		}

		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::DrainTemplateRefs");
		BT_PLOT("SARP.StaticImportCoordinator.TemplateRefPackets", static_cast<int64_t>(publishedPackets.size()));
		const auto templateRefOwners = _state.templateRefs ? _state.templateRefs.use_count() : 0;
		BT_PLOT("SARP.StaticImportCoordinator.TemplateRefOwners", static_cast<int64_t>(templateRefOwners));
		BT_PLOT("SARP.StaticImportCoordinator.TemplateRefCount",
			static_cast<int64_t>(_state.templateRefs ? _state.templateRefs->size() : 0));
		const bool updateTemplateRefsInPlace = _state.templateRefs && templateRefOwners <= 2;
		std::shared_ptr<StaticTemplateRefMap> nextRefs;
		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::PrepareTemplateRefSnapshot");
			nextRefs = updateTemplateRefsInPlace
				? _state.templateRefs
				: std::make_shared<StaticTemplateRefMap>(*_state.templateRefs);
		}
		std::vector<StaticImportDependencyKey> resolvedKeys;
		std::vector<StaticImportDependencyKey> failedKeys;
		for (auto& packet : publishedPackets) {
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::Packet");
			{
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::MergeTemplateRefs");
				for (auto& [key, ref] : packet.refs) {
					nextRefs->insert_or_assign(key, std::move(ref));
					_state.pendingTemplateKeys.erase(key);
					_state.failedTemplateKeys.erase(key);
					_state.templateArtifactAwaiters.erase(key);
					resolvedKeys.push_back(StaticImportDependencyKey{
						.kind = StaticImportDependencyKind::Template,
						.numericKey = key.meshID,
						.numericKey2 = key.materialID,
						.numericKey3 = key.rasterFlags
					});
				}
			}
			{
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::CollectFailedTemplateRefs");
				for (const auto key : packet.failedKeys) {
					_state.pendingTemplateKeys.erase(key);
					_state.failedTemplateKeys.insert(key);
					_state.templateArtifactAwaiters.erase(key);
					failedKeys.push_back(StaticImportDependencyKey{
						.kind = StaticImportDependencyKind::Template,
						.numericKey = key.meshID,
						.numericKey2 = key.materialID,
						.numericKey3 = key.rasterFlags
					});
				}
			}
		}
		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::UpdateTemplateRefs");
			if (!updateTemplateRefsInPlace) {
				_state.templateRefs = std::move(nextRefs);
				_state.templateSnapshot = _state.templateRefs;
			}
		std::atomic_store_explicit(
				std::addressof(_importState->staticTemplateGraphSnapshot),
				_state.templateSnapshot,
				std::memory_order_release);
		std::atomic_store_explicit(
			std::addressof(_importState->staticTemplateGraphFailedSnapshot),
			std::make_shared<const StaticTemplateFailureSet>(_state.failedTemplateKeys),
			std::memory_order_release);
		}
		BT_PLOT("SARP.StaticImportCoordinator.TemplateRefsUpdatedInPlace", updateTemplateRefsInPlace ? int64_t{ 1 } : int64_t{ 0 });
		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::ResolveTemplateDependencies");
			for (const auto& key : resolvedKeys) {
				ResolveDependency(key);
			}
		}
		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::FailTemplateDependencies");
			for (const auto& key : failedKeys) {
				FailDependency(key);
			}
		}
		BT_PLOT("SARP.StaticImportCoordinator.TemplateRefsResolved", static_cast<int64_t>(resolvedKeys.size()));
		BT_PLOT("SARP.StaticImportCoordinator.TemplateRefsFailed", static_cast<int64_t>(failedKeys.size()));
		return true;
	}

	bool StaticImportCoordinator::SubmitDirtyStaticScenePages(br::render::RendererStateRequestService& requests)
	{
		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::SubmitDirtyStaticScenePages");
		if (_state.graphOwnedDirtyPages.empty()) return false;
		// Page successors are independent of scene-root publication. Each page has
		// at most one desired build; mutations arriving while it builds remain in
		// the pending delta and are coalesced into the following successor.
		std::vector<std::uint32_t> dirtyPages(_state.graphOwnedDirtyPages.begin(),
			_state.graphOwnedDirtyPages.end());
		std::ranges::sort(dirtyPages);
		bool submittedAny = false;
		for (const auto pageIndex : dirtyPages) {
			auto& owners = _state.graphOwnedPageOwners[pageIndex];
			auto& desired = _state.graphOwnedDesiredPages[pageIndex];
			auto& ready = _state.graphOwnedReadyPages[pageIndex];
			auto& awaiter = _state.graphOwnedPageAwaiters[pageIndex];
			if (desired && ready.Version() != desired.version) {
				BT_PLOT("SARP.StaticImportCoordinator.PageDeltaCoalesced", int64_t{ 1 });
				continue;
			}

			auto input = std::make_shared<br::render::StaticScenePageBuildInput>();
			input->pageIndex = pageIndex;
			if (desired && ready.Version() == desired.version) {
				input->basePage = desired.version;
				input->basePagePayload = ready.payload.Get<br::render::PublishedStaticScenePage>();
				if (!input->basePagePayload) continue;
			}
			auto& pendingOwners = _state.graphOwnedPendingPageOwners[pageIndex];
			auto& pendingRemovals = _state.graphOwnedPendingPageRemovals[pageIndex];
			if (pendingOwners.empty() && pendingRemovals.empty()) {
				_state.graphOwnedDirtyPages.erase(pageIndex);
				continue;
			}
			input->groupOwners.reserve(pendingOwners.size());
			input->removedGroupIDs.reserve(pendingRemovals.size());
			std::unordered_map<br::render::ArtifactKey, br::render::ArtifactVersionID,
				br::render::ArtifactKey::Hasher> uniqueVersions;
			std::uint64_t fingerprint = (static_cast<std::uint64_t>(pageIndex) + 1u) ^
				(input->basePage.revision + (input->basePage.generation << 1u));
			for (const auto& [groupID, version] : pendingOwners) {
				input->groupOwners.push_back({ groupID, version });
				uniqueVersions.insert_or_assign(version.address, version);
				fingerprint ^= br::render::StaticSceneGroupDigest(groupID);
				fingerprint ^= version.revision + (version.generation << 1u) +
					0x9e3779b97f4a7c15ull;
			}
			for (const auto groupID : pendingRemovals) {
				input->removedGroupIDs.push_back(groupID);
				fingerprint ^= br::render::StaticSceneGroupDigest(groupID) ^ 0xd6e8feb86659fd93ull;
			}
			input->sourceFingerprint = fingerprint;
			std::vector<br::render::ArtifactRequirement> requirements;
			requirements.reserve(uniqueVersions.size());
			for (const auto& [_, version] : uniqueVersions) {
				requirements.push_back(br::render::Exact(
					version, br::render::ArtifactReadiness::UploadSubmitted));
			}
			const auto result = requests.SubmitLatest({
				{ br::render::ArtifactKind::StaticScenePage,
					static_cast<std::uint64_t>(pageIndex) + 1u, 0 },
				++_state.graphOwnedPageRevisions[pageIndex], std::move(requirements),
				br::render::ArtifactPayload::Make<br::render::StaticScenePageBuildInput>(
					std::move(input)), fingerprint == 0 ? 1u : fingerprint });
			if (!result) continue;
			BT_PLOT("SARP.StaticImportCoordinator.PageDeltaOwners",
				static_cast<int64_t>(pendingOwners.size()));
			BT_PLOT("SARP.StaticImportCoordinator.PageDeltaRemovals",
				static_cast<int64_t>(pendingRemovals.size()));
			BT_PLOT("SARP.StaticImportCoordinator.PageRebuildOwners",
				static_cast<int64_t>(owners.size()));
			desired = result.Handle();
			ready = {};
			awaiter = requests.AwaitExact(result.Handle(), br::render::ArtifactReadiness::CpuReady,
				TaskLane::Streaming, TaskDomain::StaticImportControl,
				[this, pageIndex](const br::render::ArtifactSnapshot& snapshot) {
					(void)_drains->PushBlocking(_bus->staticScenePageReadyEvents,
						StaticScenePageReady{ pageIndex, snapshot });
					_drains->ScheduleCoordinatorDrain();
				},
				[this, pageIndex](const br::render::ArtifactTermination& termination) {
					// The drain loop re-dirties the page; it owns _state, this
					// continuation does not.
					(void)_drains->PushBlocking(_bus->staticScenePageReadyEvents,
						StaticScenePageReady{ pageIndex,
							{ termination.version.address, termination.version.revision,
								termination.version.generation, termination.readiness } });
					_drains->ScheduleCoordinatorDrain();
				});
			_state.graphOwnedDirtyPages.erase(pageIndex);
			pendingOwners.clear();
			pendingRemovals.clear();
			submittedAny = true;
		}
		return submittedAny;
	}

	bool StaticImportCoordinator::TrySubmitReadyStaticScene(br::render::RendererStateRequestService& requests)
	{
		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::TrySubmitReadyStaticScene");
		if (_state.graphOwnedSceneInFlight) {
			const auto snapshot = requests.Snapshot(_state.graphOwnedSceneInFlight.version);
			basic_telemetry::SetGauge("SARP.StaticPublication.SceneInFlightReadiness",
				static_cast<std::int64_t>(snapshot.readiness));
			const auto now = std::chrono::steady_clock::now();
			if (_state.graphOwnedLastSceneDiagnostic == std::chrono::steady_clock::time_point{} ||
				now - _state.graphOwnedLastSceneDiagnostic >= std::chrono::seconds(10)) {
				_state.graphOwnedLastSceneDiagnostic = now;
				const auto diagnostic = requests.Diagnose(
					{ br::render::ArtifactKind::StaticScene, 0, 0 });
				spdlog::error(
					"Static scene publication remains in flight: version={}:{}:{} readiness={} desiredRevision={} artifactRevision={} blockers={} chain='{}' ownedGroups={} desiredPlacements={}",
					static_cast<unsigned>(_state.graphOwnedSceneInFlight.version.address.kind),
					_state.graphOwnedSceneInFlight.version.generation,
					_state.graphOwnedSceneInFlight.version.revision,
					static_cast<unsigned>(snapshot.readiness), diagnostic.desiredRevision,
					diagnostic.artifact.revision, diagnostic.blockers.size(),
					diagnostic.blockerChain, _state.graphOwnedGroupVersions.size(),
					_state.graphOwnedDesiredPlacementCount);
			}
			return false;
		}

		auto input = std::make_shared<br::render::StaticSceneBuildInput>();
		input->publishRoot = true;
		input->requireGeometryBufferClosure = true;
		std::vector<br::render::ArtifactRequirement> requirements;
		requirements.reserve(br::render::kStaticScenePageCount + 4u);
		std::uint64_t groupDigest = 0;
		std::uint64_t cutGroupCount = 0;
		std::uint64_t cutPlacementCount = 0;
		std::uint64_t fingerprint = 0;
		std::size_t desiredPageCount = 0;
		std::size_t readyDesiredPageCount = 0;
		std::size_t blockedSuccessorPageCount = 0;
		auto cutPages = std::make_shared<std::array<br::render::ArtifactVersionID,
			br::render::kStaticScenePageCount>>();
		for (std::size_t index = 0; index < br::render::kStaticScenePageCount; ++index) {
			const auto& desired = _state.graphOwnedDesiredPages[index];
			const auto& ready = _state.graphOwnedReadyPages[index];
			desiredPageCount += desired ? 1u : 0u;
			readyDesiredPageCount += desired && ready.Version() == desired.version ? 1u : 0u;
			br::render::ArtifactVersionID selectedVersion;
			std::shared_ptr<const br::render::PublishedStaticScenePage> page;
			if (desired && ready.Version() == desired.version) {
				selectedVersion = desired.version;
				page = ready.payload.Get<br::render::PublishedStaticScenePage>();
			} else if (desired) {
				// A committed scene deliberately owns only its flattened group/resource
				// state; it no longer retains the graph page payload or its base chain.
				// Consequently an in-flight successor cannot be represented by silently
				// omitting this page (nor by looking it up through the committed scene).
				// Wait for the exact desired page and publish the checkpoint atomically.
				// The preceding scene remains installed and continues to own/render all
				// of its regions while this page is prepared.
				++blockedSuccessorPageCount;
			}
			if (!selectedVersion || !page) continue;
			input->pages.push_back({ static_cast<std::uint32_t>(index), selectedVersion });
			(*cutPages)[index] = selectedVersion;
			requirements.push_back(br::render::Exact(
				selectedVersion, br::render::ArtifactReadiness::CpuReady));
			groupDigest ^= page->groupDigest;
			cutGroupCount += page->groupCount;
			cutPlacementCount += page->placementCount;
			fingerprint ^= selectedVersion.revision + (selectedVersion.generation << 1u) +
				(static_cast<std::uint64_t>(index) << 32u) + 0x9e3779b97f4a7c15ull;
		}
		basic_telemetry::SetGauge("SARP.StaticPublication.OwnedGroups",
			static_cast<std::int64_t>(_state.graphOwnedGroupVersions.size()));
		basic_telemetry::SetGauge("SARP.StaticPublication.DirtyPages",
			static_cast<std::int64_t>(_state.graphOwnedDirtyPages.size()));
		basic_telemetry::SetGauge("SARP.StaticPublication.DesiredPages",
			static_cast<std::int64_t>(desiredPageCount));
		basic_telemetry::SetGauge("SARP.StaticPublication.ReadyDesiredPages",
			static_cast<std::int64_t>(readyDesiredPageCount));
		basic_telemetry::SetGauge("SARP.StaticPublication.CutPages",
			static_cast<std::int64_t>(input->pages.size()));
		basic_telemetry::SetGauge("SARP.StaticPublication.BlockedSuccessorPages",
			static_cast<std::int64_t>(blockedSuccessorPageCount));
		if (blockedSuccessorPageCount != 0) {
			// Publishing the other pages would describe a partial directory as a
			// complete scene and retire every group in the omitted pages.
			return false;
		}
		if (input->pages.empty() && !_state.graphOwnedGroupVersions.empty()) {
			const auto now = std::chrono::steady_clock::now();
			if (_state.graphOwnedLastSceneDiagnostic == std::chrono::steady_clock::time_point{} ||
				now - _state.graphOwnedLastSceneDiagnostic >= std::chrono::seconds(10)) {
				_state.graphOwnedLastSceneDiagnostic = now;
				std::uint32_t samplePage = UINT32_MAX;
				br::render::ArtifactSnapshot sampleSnapshot;
				for (std::size_t index = 0; index < br::render::kStaticScenePageCount; ++index) {
					if (!_state.graphOwnedDesiredPages[index]) continue;
					samplePage = static_cast<std::uint32_t>(index);
					sampleSnapshot = requests.Snapshot(_state.graphOwnedDesiredPages[index].version);
					break;
				}
				spdlog::error(
					"Static scene cut has owned groups but no selectable pages: groups={} placements={} dirty={} desiredPages={} readyDesiredPages={} samplePage={} sampleReadiness={} sampleRevision={}:{}:{}",
					_state.graphOwnedGroupVersions.size(), _state.graphOwnedDesiredPlacementCount,
					_state.graphOwnedDirtyPages.size(), desiredPageCount, readyDesiredPageCount,
					samplePage, static_cast<unsigned>(sampleSnapshot.readiness),
					static_cast<unsigned>(sampleSnapshot.key.kind),
					sampleSnapshot.generation, sampleSnapshot.revision);
			}
			return false;
		}
		input->desiredPlacementCount = cutPlacementCount;
		input->materializedPlacementCount = cutPlacementCount;
		// Seal all geometry-table journal mutations once for this scene cut.
		// Template batches may complete concurrently, but the immutable exact
		// requirement below keeps this root paired with the captured version.
		if (!_shared->sceneIngestionServices.meshes) return false;
		(void)_shared->sceneIngestionServices.meshes->PublishDesiredBufferState();
		std::uint64_t geometryCoverage = 0;
		const auto geometryBuffers =
			_shared->sceneIngestionServices.meshes->DesiredBufferStateRequirement(geometryCoverage);
		if (!geometryBuffers) return false;
		const auto desiredDigest = br::render::StaticScenePlacementDigest(groupDigest,
			input->desiredPlacementCount, cutGroupCount);
		fingerprint ^= desiredDigest;
		// Do not capture other publishable roots here. In particular, the current
		// indirect-workload root observes the previously published Geometry root;
		// making its snapshot a dependency of the successor Geometry root creates
		// an unsatisfiable old-geometry -> indirect -> new-geometry publication
		// cycle. The pages already contain finalized draw/object references. Their
		// managers publish those tables on their own serialized domains, while the
		// exact non-root GeometryBufferState below is the resource closure that must
		// remain atomic with this membership cut.
		requirements.push_back(*geometryBuffers);
		// Static draw records publish only alongside a Geometry root covering the
		// templates they reference (ObjectManager::SetGeometryCoverageSource). A
		// commit normally dirties pages too, but if it left them untouched the
		// records would wait indefinitely, so cut again for geometry alone.
		if (const auto* objects = _shared->sceneIngestionServices.objects;
			objects && objects->RequiredGeometryCoverage() > _state.graphOwnedLastSceneGeometryCoverage &&
			geometryCoverage > _state.graphOwnedLastSceneGeometryCoverage) {
			fingerprint ^= geometryBuffers->minimumRevision * 0x9e3779b97f4a7c15ull + 0x47454f4dull;
		}
		if (fingerprint == _state.graphOwnedLastSceneFingerprint) return false;
		input->sourceFingerprint = fingerprint;
		const auto result = requests.SubmitLatest({
			{ br::render::ArtifactKind::StaticScene, 0, 0 },
			++_state.graphOwnedSceneRevision, std::move(requirements),
			br::render::ArtifactPayload::Make<br::render::StaticSceneBuildInput>(
				std::move(input)), fingerprint == 0 ? 1u : fingerprint });
		if (!result) return false;
		_state.graphOwnedSceneInFlight = result.Handle();
		_state.graphOwnedSceneAwaiter = requests.AwaitExact(result.Handle(),
			br::render::ArtifactReadiness::Published, TaskLane::Streaming,
			TaskDomain::StaticImportControl,
			[this, cutPages](const br::render::ArtifactSnapshot& snapshot) {
				const auto fragment = snapshot.payload.Get<
					br::render::RendererStateFragmentArtifact>();
				const auto scene = fragment
					? fragment->fragment.payload.Get<br::render::PublishedStaticSceneState>()
					: nullptr;
				(void)_drains->PushBlocking(_bus->staticSceneRootPublishedEvents,
					StaticSceneRootPublished{ snapshot.Version(), cutPages, scene });
				_drains->ScheduleCoordinatorDrain();
			},
			[this](const br::render::ArtifactTermination& termination) {
				// No scene payload will ever arrive for this version. The event
				// carries no pages, which the drain loop reads as "release the
				// in-flight cursor" so a later cut can be submitted at all.
				(void)_drains->PushBlocking(_bus->staticSceneRootPublishedEvents,
					StaticSceneRootPublished{ termination.version, nullptr, nullptr });
				_drains->ScheduleCoordinatorDrain();
			});
		_state.graphOwnedLastSceneFingerprint = fingerprint;
		_state.graphOwnedLastSceneGroupCount = cutGroupCount;
		_state.graphOwnedLastSceneGeometryCoverage = geometryCoverage;
		return true;
	}

	bool StaticImportCoordinator::DrainGroupAcks()
	{
		std::vector<StaticImportGroupAck> acks;
		std::vector<StaticTransactionReady> readyTransactions;
		std::vector<StaticScenePageReady> readyPages;
		std::vector<StaticSceneRootPublished> publishedScenes;
		// A membership checkpoint is O(active groups), so consume a substantial
		// acknowledgement batch before constructing one. The old 64-item limit
		// repeatedly rebuilt almost-identical whole-scene inputs during loads.
		// Bound each serialized mutation. Level-triggered wakes immediately resume
		// remaining events, while a completion burst can no longer turn a nominal
		// two-millisecond coordinator slice into a multi-second task.
		_bus->staticImportGroupAcks.Drain(acks, 128);
		_bus->staticTransactionReadyEvents.Drain(readyTransactions, 64);
		_bus->staticScenePageReadyEvents.Drain(readyPages, 32);
		_bus->staticSceneRootPublishedEvents.Drain(publishedScenes, 1);
		if (acks.empty() && readyTransactions.empty() && readyPages.empty() && publishedScenes.empty()) {
			return false;
		}
		for (auto& event : publishedScenes) {
			if (!_state.graphOwnedSceneInFlight ||
				event.version != _state.graphOwnedSceneInFlight.version) continue;
			if (!event.pages || !event.scene) {
				// The root terminated (refused, failed or superseded). Releasing
				// the in-flight cursor is mandatory: TrySubmitReadyStaticScene
				// refuses to submit while one is outstanding, so leaving it set
				// would stall every later publication cut permanently.
				_state.graphOwnedSceneInFlight = {};
				_state.graphOwnedSceneAwaiter.Reset();
				_state.graphOwnedLastSceneFingerprint = 0;
				basic_telemetry::AddCounter("SARP.StaticImportCoordinator.SceneRootTerminated");
				continue;
			}
			StaticSceneOwnershipDelta ownershipDelta;
			{
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::DiffSceneOwnership");
				const auto oldOwnership = _state.graphOwnedPublishedScene
					? _state.graphOwnedPublishedScene->ownership : nullptr;
				const auto& newOwnership = event.scene->ownership;
				if (newOwnership) {
					const auto* oldGroups = oldOwnership
						? std::addressof(oldOwnership->groups) : nullptr;
					const auto& newGroups = newOwnership->groups;
					std::size_t oldCursor = 0;
					std::size_t newCursor = 0;
					while ((oldGroups && oldCursor < oldGroups->size()) ||
						newCursor < newGroups.size()) {
						const auto oldID = oldGroups && oldCursor < oldGroups->size()
							? (*oldGroups)[oldCursor].groupID
							: (std::numeric_limits<std::uint64_t>::max)();
						const auto newID = newCursor < newGroups.size()
							? newGroups[newCursor].groupID
							: (std::numeric_limits<std::uint64_t>::max)();
						if (oldID <= newID && oldGroups) {
							const auto& oldGroup = (*oldGroups)[oldCursor];
							if (oldID < newID || oldGroup.ownership != newGroups[newCursor].ownership) {
								if (oldGroup.ownership) ownershipDelta.removed.push_back(
									std::static_pointer_cast<const StaticGraphGroupOwnership>(oldGroup.ownership));
							}
							++oldCursor;
						}
						if (newID <= oldID) {
							const auto& newGroup = newGroups[newCursor];
							if (newID < oldID || !oldGroups ||
								(*oldGroups)[oldCursor - 1].ownership != newGroup.ownership) {
								if (newGroup.ownership) ownershipDelta.added.push_back(
									std::static_pointer_cast<const StaticGraphGroupOwnership>(newGroup.ownership));
							}
							++newCursor;
						}
					}
				} else {
				for (std::size_t pageIndex = 0; pageIndex < br::render::kStaticScenePageCount; ++pageIndex) {
				const auto oldPage = _state.graphOwnedPublishedScene
					? _state.graphOwnedPublishedScene->pages[pageIndex] : nullptr;
				const auto& newPage = event.scene->pages[pageIndex];
				if (oldPage == newPage) continue;
				std::vector<br::render::StaticTransactionGroup> oldMaterializedGroups;
				std::vector<br::render::StaticTransactionGroup> newMaterializedGroups;
				bool usedSuccessorDelta = false;
				if (oldPage && newPage) {
					std::map<std::uint64_t, const br::render::StaticTransactionGroup*> newestMutation;
					std::unordered_set<std::uint64_t> seen;
					const auto* layer = newPage.get();
					while (layer && layer != oldPage.get()) {
						for (const auto groupID : layer->removedGroupIDs) {
							if (seen.insert(groupID).second) newestMutation.emplace(groupID, nullptr);
						}
						for (const auto& group : layer->groups) {
							if (seen.insert(group.groupID).second) newestMutation.emplace(group.groupID, std::addressof(group));
						}
						layer = layer->basePage.get();
					}
					usedSuccessorDelta = layer == oldPage.get();
					if (usedSuccessorDelta) {
						for (const auto& [groupID, newGroup] : newestMutation) {
							if (const auto* oldGroup = oldPage->FindGroup(groupID)) oldMaterializedGroups.push_back(*oldGroup);
							if (newGroup) newMaterializedGroups.push_back(*newGroup);
						}
					}
				}
				if (!usedSuccessorDelta) {
					if (oldPage) oldPage->MaterializeGroups(oldMaterializedGroups);
					if (newPage) newPage->MaterializeGroups(newMaterializedGroups);
				}
				const std::vector<br::render::StaticTransactionGroup>* oldGroups =
					oldPage ? std::addressof(oldMaterializedGroups) : nullptr;
				const std::vector<br::render::StaticTransactionGroup>* newGroups =
					newPage ? std::addressof(newMaterializedGroups) : nullptr;
				std::size_t oldCursor = 0;
				std::size_t newCursor = 0;
				const auto oldSize = oldGroups ? oldGroups->size() : 0u;
				const auto newSize = newGroups ? newGroups->size() : 0u;
				while (oldCursor < oldSize || newCursor < newSize) {
					const auto oldID = oldCursor < oldSize
						? (*oldGroups)[oldCursor].groupID : (std::numeric_limits<std::uint64_t>::max)();
					const auto newID = newCursor < newSize
						? (*newGroups)[newCursor].groupID : (std::numeric_limits<std::uint64_t>::max)();
					if (oldID < newID) {
						if ((*oldGroups)[oldCursor].ownership) {
							ownershipDelta.removed.push_back(
								std::static_pointer_cast<const StaticGraphGroupOwnership>((*oldGroups)[oldCursor].ownership));
						}
						++oldCursor;
					} else if (newID < oldID) {
						if ((*newGroups)[newCursor].ownership) {
							ownershipDelta.added.push_back(
								std::static_pointer_cast<const StaticGraphGroupOwnership>((*newGroups)[newCursor].ownership));
						}
						++newCursor;
					} else {
						if ((*oldGroups)[oldCursor].ownership != (*newGroups)[newCursor].ownership) {
							if ((*oldGroups)[oldCursor].ownership) {
								ownershipDelta.removed.push_back(
									std::static_pointer_cast<const StaticGraphGroupOwnership>((*oldGroups)[oldCursor].ownership));
							}
							if ((*newGroups)[newCursor].ownership) {
								ownershipDelta.added.push_back(
									std::static_pointer_cast<const StaticGraphGroupOwnership>((*newGroups)[newCursor].ownership));
							}
						}
						++oldCursor;
						++newCursor;
					}
				}
				}
				}
			}
			if (!ownershipDelta.added.empty() || !ownershipDelta.removed.empty()) {
				// Extension ownership (e.g. height-atlas registration) follows the
				// immutable scene cut, but its mutexed maps and dependency
				// notifications do not belong on the render thread. Apply them on this
				// StaticImportControl callback.
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::ApplyExtensionOwnership");
				// Removed ownership remains live until retirement's object-buffer cut is
				// coherently consumed by indirect publication. Its extension holds are
				// released by RetirementCommit at that boundary.
				for (const auto& ownership : ownershipDelta.added) {
					if (ownership && ownership->residencyClass == StaticImportResidencyClass::CellStatic) {
						for (std::size_t extensionIndex = 0; extensionIndex < _extensions.size(); ++extensionIndex) {
							_extensions[extensionIndex]->OnGroupOwnershipAdded(ownership->groupID, ownership->cellKey,
								br::staticimport::StaticGroupHoldAt(ownership->extensionHolds, extensionIndex));
						}
					}
				}
				(void)_drains->PushStreaming(
					_bus->staticSceneOwnershipDeltas, std::move(ownershipDelta));
			}
			const auto precedingPlacementCount = _state.graphOwnedPublishedScene
				? _state.graphOwnedPublishedScene->publishedPlacementCount : 0u;
			basic_telemetry::SetGauge("SARP.StaticPublication.SelectedPlacements",
				static_cast<std::int64_t>(event.scene->publishedPlacementCount));
			basic_telemetry::SetGauge("SARP.StaticPublication.DesiredPlacements",
				static_cast<std::int64_t>(_state.graphOwnedDesiredPlacementCount));
			basic_telemetry::SetGauge("SARP.StaticPublication.SceneRevision",
				static_cast<std::int64_t>(event.version.revision));
			if (event.scene->publishedPlacementCount < precedingPlacementCount &&
				_state.graphOwnedDesiredPlacementCount >= precedingPlacementCount) {
				basic_telemetry::AddCounter("SARP.StaticPublication.UnexpectedPlacementDrops");
				spdlog::error(
					"Static scene placement cut regressed: revision={} previous={} selected={} desired={} pages={} groups={}",
					event.version.revision, precedingPlacementCount,
					event.scene->publishedPlacementCount,
					_state.graphOwnedDesiredPlacementCount, event.scene->pages.size(),
					event.scene->groupCount);
			}
			_state.graphOwnedPublishedScene = event.scene;
			_state.graphOwnedPublishedPages = *event.pages;
			_state.graphOwnedSceneAwaiter.Reset();
			_state.graphOwnedSceneInFlight = {};
		}
		for (auto& event : readyPages) {
			if (event.pageIndex >= br::render::kStaticScenePageCount) continue;
			const auto& desired = _state.graphOwnedDesiredPages[event.pageIndex];
			if (!desired || event.snapshot.Version() != desired.version) continue;
			_state.graphOwnedPageAwaiters[event.pageIndex].Reset();
			if (!br::render::ArtifactReachedMilestone(event.snapshot.readiness,
					br::render::ArtifactReadiness::CpuReady)) {
				// The page version terminated. Drop the desired cursor and mark the
				// page dirty so the next submission pass rebuilds it: cutting a
				// scene over a page that was never produced would publish
				// membership for groups whose page content does not exist.
				_state.graphOwnedDesiredPages[event.pageIndex] = {};
				_state.graphOwnedDirtyPages.insert(event.pageIndex);
				basic_telemetry::AddCounter("SARP.StaticImportCoordinator.PageTerminated");
				continue;
			}
			_state.graphOwnedReadyPages[event.pageIndex] = std::move(event.snapshot);
		}

		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::DrainGroupAcks");
		std::size_t published = 0;
		std::size_t requeued = 0;
		std::size_t dropped = 0;
		std::size_t unpublished = 0;
		bool graphMembershipChanged = false;
		bool graphRemovalObserved = false;
		std::size_t graphMembershipChanges = 0;
		std::vector<std::shared_ptr<StaticGroupLifecycleRecord>> retiredRecords;
		retiredRecords.reserve(512);
		const auto erasePending = [this](
			std::uint64_t groupID, StaticGroupLifecycleStage terminalStage,
			std::vector<std::shared_ptr<StaticGroupLifecycleRecord>>& releases) {
			(void)RemoveGroup(groupID, terminalStage, &releases);
		};
		const auto ticketMatches = [this](std::uint64_t ticketID, std::uint64_t groupID) {
			if (ticketID == 0) {
				return true;
			}
			auto ticketIt = _state.readyGroupTickets.find(groupID);
			return ticketIt != _state.readyGroupTickets.end() && ticketIt->second == ticketID;
		};

		// StaticTransaction readiness is the publication acknowledgement. It is
		// produced by the graph only after the exact object-buffer cut is GPU-ready,
		// so coordinator membership no longer waits for the main-thread bridge to
		// reconstruct and return a per-group acknowledgement.
		StaticSceneOwnershipDelta rejectedOwnership;
		constexpr std::size_t kReadyTransactionGroupBudget = 512;
		std::size_t readyTransactionGroupsConsumed = 0;
		{
		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::DrainGroupAcks::AcceptTransactions");
		for (std::size_t transactionIndex = 0;
			transactionIndex < readyTransactions.size(); ++transactionIndex) {
			auto& ready = readyTransactions[transactionIndex];
			if (!ready.version || !ready.input) {
				_importState->staticTransactionReadyEventsRejected.fetch_add(
					1, std::memory_order_relaxed);
				continue;
			}
			const auto groupBegin = ready.nextGroupIndex;
			const auto groupEnd = (std::min)(
				ready.input->groups.size(),
				ready.nextGroupIndex + (kReadyTransactionGroupBudget - readyTransactionGroupsConsumed));
			for (; ready.nextGroupIndex < groupEnd; ++ready.nextGroupIndex) {
				const auto& group = ready.input->groups[ready.nextGroupIndex];
				if (!ticketMatches(group.admissionTicketID, group.groupID)) {
					_importState->staticTransactionReadyEventsRejected.fetch_add(
						1, std::memory_order_relaxed);
					if (_importState->benchmarkDiagnosticsEnabled) {
						const auto current = _state.readyGroupTickets.find(group.groupID);
						spdlog::warn(
							"SARP static transaction acknowledgement rejected: group=0x{:016X} event_ticket={} current_ticket={} version={}:{}:{} pending={} ready={}",
							group.groupID, group.admissionTicketID,
							current != _state.readyGroupTickets.end() ? current->second : 0,
							static_cast<unsigned>(ready.version.address.kind),
							ready.version.generation, ready.version.revision,
							_state.pendingGroups.contains(group.groupID),
							_state.readyGroups.contains(group.groupID));
					}
					if (group.ownership) {
						rejectedOwnership.retireOnly.push_back(
							std::static_pointer_cast<const StaticGraphGroupOwnership>(group.ownership));
					}
					continue;
				}
				_importState->staticTransactionReadyEventsAccepted.fetch_add(
					1, std::memory_order_relaxed);
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::DrainGroupAcks::AcceptGroup");
				const bool newGroup = !_state.graphOwnedGroupVersions.contains(group.groupID);
				const auto pageIndex = static_cast<std::uint32_t>(
					br::render::StaticScenePageIndex(group.groupID));
				if (const auto oldCount = _state.graphOwnedGroupPlacementCounts.find(group.groupID);
					oldCount != _state.graphOwnedGroupPlacementCounts.end()) {
					_state.graphOwnedDesiredPlacementCount -= oldCount->second;
				}
				_state.graphOwnedGroupVersions.insert_or_assign(group.groupID, ready.version);
				if (newGroup) {
					_state.graphOwnedDesiredGroupDigest ^=
						br::render::StaticSceneGroupDigest(group.groupID);
				}
				_state.graphOwnedGroupPlacementCounts.insert_or_assign(
					group.groupID, group.placementCount);
				_state.graphOwnedDesiredPlacementCount += group.placementCount;
				const auto* ownership = static_cast<const StaticGraphGroupOwnership*>(group.ownership.get());
				_state.graphOwnedGroupRevisions.insert_or_assign(
					group.groupID, ownership ? ownership->groupRevision : 0);
				_state.graphOwnedPageOwners[pageIndex].insert_or_assign(
					group.groupID, ready.version);
				_state.graphOwnedPendingPageOwners[pageIndex].insert_or_assign(
					group.groupID, ready.version);
				_state.graphOwnedPendingPageRemovals[pageIndex].erase(group.groupID);
				_state.graphOwnedDirtyPages.insert(pageIndex);
				{
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::DrainGroupAcks::RetirePending");
					erasePending(group.groupID, StaticGroupLifecycleStage::Published, retiredRecords);
				}
				_state.graphOwnedActiveGroups.insert(group.groupID);
				++published;
				++_state.stats.groupsAckPublished;
				++graphMembershipChanges;
				graphMembershipChanged = true;
			}
			readyTransactionGroupsConsumed += groupEnd - groupBegin;
			if (ready.nextGroupIndex < ready.input->groups.size()) {
				(void)_drains->PushBlocking(
					_bus->staticTransactionReadyEvents, std::move(ready), false);
			}
			if (readyTransactionGroupsConsumed >= kReadyTransactionGroupBudget) {
				for (++transactionIndex; transactionIndex < readyTransactions.size(); ++transactionIndex) {
					(void)_drains->PushBlocking(
						_bus->staticTransactionReadyEvents,
						std::move(readyTransactions[transactionIndex]), false);
				}
				break;
			}
		}
		}
		if (!rejectedOwnership.retireOnly.empty()) {
			(void)_drains->PushStreaming(
				_bus->staticSceneOwnershipDeltas, std::move(rejectedOwnership));
		}

		for (auto& ack : acks) {
			unpublished += ack.unpublished.size();
			for (const auto& unpublishedGroup : ack.unpublished) {
				const auto groupID = unpublishedGroup.groupID;
				// A retirement unpublishes only the add it retired: when a re-add of
				// the same id has already been accepted, the graph keeps the newer one.
				const auto ownerRevision = _state.graphOwnedGroupRevisions.find(groupID);
				if (ownerRevision != _state.graphOwnedGroupRevisions.end() &&
					unpublishedGroup.groupRevision != 0 &&
					ownerRevision->second > unpublishedGroup.groupRevision) {
					continue;
				}
				if (auto* trace = SceneMembershipTrace()) {
					std::lock_guard lock(trace->mutex);
					if (const auto skipped = trace->skipped.find(groupID); skipped != trace->skipped.end()) {
						const auto& info = skipped->second;
						trace->out << "unpublish_after_skip," << groupID << ',' << _state.latestTarget.generation
							<< ',' << unpublishedGroup.groupRevision << ',' << info.placements << '\n';
						trace->out.flush();
						trace->skipped.erase(skipped);
					}
				}
				_state.graphOwnedActiveGroups.erase(groupID);
				if (const auto oldCount = _state.graphOwnedGroupPlacementCounts.find(groupID);
					oldCount != _state.graphOwnedGroupPlacementCounts.end()) {
					_state.graphOwnedDesiredPlacementCount -= oldCount->second;
					_state.graphOwnedGroupPlacementCounts.erase(oldCount);
				}
				_state.capacityHintDrawRecordsByGroup.erase(groupID);
				_state.graphOwnedGroupRevisions.erase(groupID);
				const bool removedOwner = _state.graphOwnedGroupVersions.erase(groupID) != 0;
				if (removedOwner) {
					_state.graphOwnedDesiredGroupDigest ^=
						br::render::StaticSceneGroupDigest(groupID);
					const auto pageIndex = static_cast<std::uint32_t>(
						br::render::StaticScenePageIndex(groupID));
					_state.graphOwnedPageOwners[pageIndex].erase(groupID);
					_state.graphOwnedPendingPageOwners[pageIndex].erase(groupID);
					_state.graphOwnedPendingPageRemovals[pageIndex].insert(groupID);
					_state.graphOwnedDirtyPages.insert(pageIndex);
				}
				graphMembershipChanged = removedOwner || graphMembershipChanged;
				graphRemovalObserved = removedOwner || graphRemovalObserved;
				graphMembershipChanges += removedOwner ? 1u : 0u;
			}
			for (const auto groupID : ack.dropped) {
				if (!ticketMatches(ack.ticketID, groupID)) {
					continue;
				}
				erasePending(groupID, StaticGroupLifecycleStage::Cancelled, retiredRecords);
				++dropped;
				++_state.stats.groupsAckDropped;
			}
			for (const auto groupID : ack.requeue) {
				if (!ticketMatches(ack.ticketID, groupID)) {
					continue;
				}
				_state.readyGroups.erase(groupID);
				ReleasePreparedTicket(ack.ticketID);
				_state.readyGroupTickets.erase(groupID);
				ClearGroupBlockers(groupID);
				auto pendingIt = _state.pendingGroups.find(groupID);
				if (pendingIt == _state.pendingGroups.end()) {
					continue;
				}
				if (!pendingIt->second ||
					!IsGroupLive(*pendingIt->second)) {
					erasePending(groupID, StaticGroupLifecycleStage::Cancelled, retiredRecords);
					++dropped;
					++_state.stats.groupsAckDropped;
					continue;
				}
				if (QueueGroupIfRunnable(groupID)) {
					++requeued;
					++_state.stats.groupsAckRequeued;
				}
			}
		}
		_importState->graphDesiredPlacementSetDigest.store(br::render::StaticScenePlacementDigest(
			_state.graphOwnedDesiredGroupDigest, _state.graphOwnedDesiredPlacementCount,
			_state.graphOwnedGroupVersions.size()), std::memory_order_release);
		_state.graphOwnedMembershipChangesSinceCheckpoint += graphMembershipChanges;
		if (graphMembershipChanges != 0 &&
			_state.graphOwnedDirtySince == std::chrono::steady_clock::time_point{}) {
			_state.graphOwnedDirtySince = std::chrono::steady_clock::now();
		}
		// Page publication remains coalesced, but the work at each checkpoint is
		// proportional only to the dirty buckets rather than the full scene.
		// Publish an early cut so scene population remains progressive, then amortize
		// page reconstruction over a substantially larger ingestion batch.  An idle
		// coordinator still flushes dirty pages even when the last callback was the
		// completion of the preceding scene cut rather than a membership mutation.
		const auto now = std::chrono::steady_clock::now();
		constexpr std::size_t kEarlyPageCheckpointMemberships = 4096;
		constexpr std::size_t kInteractiveCheckpointBacklog = 4096;
		const auto pageCheckpointCadence = _state.pendingGroups.size() >
			kInteractiveCheckpointBacklog
			? std::chrono::milliseconds(250)
			: std::chrono::milliseconds(50);
		const bool pageFlushDeadlineReached =
			_state.graphOwnedDirtySince != std::chrono::steady_clock::time_point{} &&
			now - _state.graphOwnedDirtySince >= pageCheckpointCadence;
		const bool graphSceneCheckpointDue = !_state.graphOwnedDirtyPages.empty() &&
			(graphRemovalObserved || _state.graphOwnedSceneRevision == 0 ||
			 _state.pendingGroups.empty() ||
			 _state.graphOwnedMembershipChangesSinceCheckpoint >=
				 kEarlyPageCheckpointMemberships ||
			 pageFlushDeadlineReached);
		if (_shared->hasSceneIngestionServices) {
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::DrainGroupAcks::SubmitSceneCheckpoint");
			if (auto* requests = _shared->sceneIngestionServices.execution.stateRequests) {
				bool sceneSubmitted = TrySubmitReadyStaticScene(*requests);
				if (graphSceneCheckpointDue) {
					(void)SubmitDirtyStaticScenePages(*requests);
					_state.graphOwnedMembershipChangesSinceCheckpoint = 0;
					_state.graphOwnedDirtySince = {};
				}
				if (!sceneSubmitted) {
					sceneSubmitted = TrySubmitReadyStaticScene(*requests);
				}
			}
		}
		BT_PLOT("SARP.StaticImportCoordinator.AckPublished", static_cast<int64_t>(published));
		BT_PLOT("SARP.StaticImportCoordinator.AckRequeued", static_cast<int64_t>(requeued));
		BT_PLOT("SARP.StaticImportCoordinator.AckDropped", static_cast<int64_t>(dropped));
		BT_PLOT("SARP.StaticGraph.UnpublishedGroups", static_cast<int64_t>(unpublished));
		BT_PLOT("SARP.StaticGraph.ActiveGroups",
			static_cast<int64_t>(_state.graphOwnedActiveGroups.size()));
		if (!retiredRecords.empty()) {
			for (auto& record : retiredRecords) {
				(void)_bus->staticImportLifecycleReleaseJobs.TryPush(std::move(record));
			}
			_shared->staticImportWorkScheduler.Signal(
				StaticImportWorkSource::GroupArtifactRelease);
			_drains->ScheduleWorkerDrains();
		}
		// Accepting a transaction only transfers a group from pending capacity to
		// graph-owned capacity; it cannot increase the high-water requirement.
		// Unloads likewise do not require an eager shrink. Re-scanning every live
		// group here made each readiness batch O(scene size). Capacity growth is
		// refreshed when prepared cells introduce new desired work instead.
		return true;
	}

	void StaticImportCoordinator::ApplyGroupChanges(std::span<const std::shared_ptr<StaticGroupLifecycleRecord>> adds, std::span<const std::uint64_t> removes)
	{
		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::ApplyGroupChanges");
		bool livenessChanged = false;
		if (!removes.empty()) {
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::ApplyGroupChanges::Remove");
			std::vector<StaticGroupRemoval> removals;
			removals.reserve(removes.size());
			for (const auto groupID : removes) {
				const auto desired = _state.desiredGroups.find(groupID);
				if (desired == _state.desiredGroups.end()) continue;
				_state.desiredGroups.erase(desired);
				_importState->staticDesiredGroupRevisions.Erase(groupID);
				(void)RemoveGroup(groupID);
				removals.push_back({ .groupID = groupID, .groupRevision = _state.nextGroupRevision++ });
			}
			if (!removals.empty()) {
				livenessChanged = true;
				_bus->staticImportRemovalOutput.PushDeferredRange(removals.begin(), removals.end());
				_shared->staticImportWorkScheduler.Signal(StaticImportWorkSource::RetirementApply);
				_drains->ScheduleWorkerDrains();
			}
		}
		for (const auto& record : adds) {
			if (!record) continue;
			const auto groupID = record->groupID;
			auto& pending = record->descriptor;
			const auto variantAddress = StaticVariantArtifactAddress(
				pending, _shared->staticSceneRenderSettings);
			if (_state.failedVariantAddresses.contains(variantAddress)) {
				record->stage.store(StaticGroupLifecycleStage::Cancelled,
					std::memory_order_release);
				++_state.stats.unsupportedGroupsDropped;
				continue;
			}
			if (pending.residencyClass == StaticImportResidencyClass::HostGroup) {
				// Host-resident groups are not desired by id: their extension decides
				// liveness (IsHostGroupCurrent), and overlapping host submissions can
				// rediscover a group whose first transaction already published. Graph
				// ownership is the authoritative membership guard for that case, and a
				// group still pending keeps its first record.
				const bool owned = _state.graphOwnedActiveGroups.contains(groupID);
				const bool alreadyPending = _state.pendingGroups.contains(groupID);
				if (owned && !alreadyPending) {
					if (auto* trace = SceneMembershipTrace()) {
						std::lock_guard lock(trace->mutex);
						trace->skipped.insert_or_assign(groupID, SkippedRediscovery{
							_state.latestTarget.generation, pending.placementCount});
						trace->out << "skip_owned," << groupID << ',' << _state.latestTarget.generation
							<< ',' << (_state.graphOwnedGroupRevisions.contains(groupID) ? _state.graphOwnedGroupRevisions.at(groupID) : 0)
							<< ',' << pending.placementCount << '\n';
					}
					record->stage.store(StaticGroupLifecycleStage::Published,
						std::memory_order_release);
					continue;
				}
				if (alreadyPending) {
					record->stage.store(StaticGroupLifecycleStage::Cancelled,
						std::memory_order_release);
					(void)QueueGroupIfRunnable(groupID);
					continue;
				}
				pending.groupRevision = _state.nextGroupRevision++;
			} else {
				// Adding a desired id replaces its current add: the earlier revision's
				// pending work is dropped here, and its published ownership (if any)
				// is replaced when this revision publishes.
				if (_state.desiredGroups.contains(groupID)) {
					(void)RemoveGroup(groupID);
					livenessChanged = true;
				}
				pending.groupRevision = _state.nextGroupRevision++;
				_state.desiredGroups.insert_or_assign(groupID, StaticDesiredGroup{
					.revision = pending.groupRevision, .batchKey = pending.batchKey });
				_importState->staticDesiredGroupRevisions.Set(groupID, pending.groupRevision);
			}
			_lifecycle->Insert(record);
			const auto preparedParts = pending.mappedPreparedParts.count != 0
				? pending.mappedPreparedParts.count
				: pending.recipePreparedParts.size();
			std::uint64_t estimatedDrawRecords = 0;
			if (preparedParts != 0 && pending.placementCount <=
				(std::numeric_limits<std::uint64_t>::max)() / preparedParts) {
				estimatedDrawRecords = pending.placementCount * preparedParts;
				_state.capacityHintDrawRecordsByGroup.insert_or_assign(
					groupID, estimatedDrawRecords);
			}
			if (_state.capacityHintCountedGroups.insert(groupID).second) {
				const auto addSaturated = [](std::uint64_t& target, std::uint64_t value) {
					target = value > (std::numeric_limits<std::uint64_t>::max)() - target
						? (std::numeric_limits<std::uint64_t>::max)()
						: target + value;
				};
				addSaturated(_state.capacityHintTransformRows, pending.placementCount);
				addSaturated(_state.capacityHintDrawRecords, estimatedDrawRecords);
			}
			_state.pendingGroups.emplace(groupID, record);
			_state.pendingGroupLastTransition[groupID] = "group_add";
			_state.stats.pendingPlacements += pending.placementCount;
			if (auto* requests = _shared->hasSceneIngestionServices
				? _shared->sceneIngestionServices.execution.stateRequests : nullptr;
				requests && requests->TraceActive()) {
				requests->TraceEvent(br::render::AsyncStateGraphTraceEventID::StaticGroupDiscovered,
					{ br::render::ArtifactKind::StaticGroup, groupID, 0 },
					pending.residencyGeneration, 0,
					{ { pending.placementCount, pending.cellKey.worldFormID,
						static_cast<std::uint64_t>(static_cast<std::int64_t>(pending.cellKey.cellX)),
						static_cast<std::uint64_t>(static_cast<std::int64_t>(pending.cellKey.cellY)),
						static_cast<unsigned>(pending.residencyClass) } });
			}
			(void)QueueGroupIfRunnable(groupID);
		}
		if (livenessChanged) {
			MarkStaticImportCoordinatorLivenessChanged();
		}
	}

	bool StaticImportCoordinator::DrainPreparedCells()
	{
		std::vector<PreparedStaticCell> preparedCells;
		_bus->staticImportPreparedCellResults.Drain(preparedCells,
			static_cast<std::size_t>(_settings->maxPreparedCellDrainPerPass));
		if (preparedCells.empty()) {
			return false;
		}

		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::DrainPreparedCells");
		BT_PLOT("SARP.StaticImportCoordinator.PreparedCellResults", static_cast<int64_t>(preparedCells.size()));
		std::vector<std::shared_ptr<StaticGroupLifecycleRecord>> adds;
		for (auto& prepared : preparedCells) {
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::DrainPreparedCells::PreparedCell");
			_state.pendingCellKeys.erase(prepared.key);
			if (!prepared.worldspaceResident && !_state.desiredCells.contains(prepared.key)) {
				_state.stats.staleGroupsDropped += prepared.lifecycleGroups.size();
				for (const auto& record : prepared.lifecycleGroups) {
					if (!record) continue;
					record->stage.store(StaticGroupLifecycleStage::Cancelled,
						std::memory_order_release);
				}
				continue;
			}
			if (!prepared.worldspaceResident) {
				for (auto* extension : _extensions) {
					extension->OnCellPrepared(prepared.key, prepared.generation,
						static_cast<std::uint32_t>(prepared.lifecycleGroups.size()));
				}
			}
			for (const auto& hostPrepare : prepared.hostCellPrepares) {
				if (hostPrepare.extension < _extensions.size()) {
					_extensions[hostPrepare.extension]->OnCellHostPrepared(
						prepared.key, prepared.generation, hostPrepare.value);
				}
			}
			// The cell layer keeps which groups each active cell added, so leaving the
			// cell can remove them. A group id the cell already added keeps its first
			// record.
			adds.clear();
			adds.reserve(prepared.lifecycleGroups.size());
			if (prepared.worldspaceResident) {
				adds.assign(prepared.lifecycleGroups.begin(), prepared.lifecycleGroups.end());
			} else {
				auto& cellGroups = _state.activeCellGroups[prepared.key];
				cellGroups.reserve(cellGroups.size() + prepared.lifecycleGroups.size());
				std::unordered_set<std::uint64_t> cellGroupMembership;
				cellGroupMembership.reserve(cellGroups.size() + prepared.lifecycleGroups.size());
				cellGroupMembership.insert(cellGroups.begin(), cellGroups.end());
				for (auto& record : prepared.lifecycleGroups) {
					if (!record) continue;
					if (!cellGroupMembership.insert(record->groupID).second) {
						record->stage.store(StaticGroupLifecycleStage::Cancelled,
							std::memory_order_release);
						continue;
					}
					cellGroups.push_back(record->groupID);
					adds.push_back(record);
				}
			}
			{
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::DrainPreparedCells::IntegrateGroups");
				ApplyGroupChanges(adds, {});
			}
			_state.stats.cellPrepareUs += prepared.prepareUs;
			_state.stats.cellsPrepared += 1;
		}
		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::DrainPreparedCells::RefreshCapacityHint");
			RefreshGraphCapacityHint();
		}
		return true;
	}

	bool StaticImportCoordinator::DrainGroupChanges()
	{
		if (!HasPendingGroupChanges()) {
			return false;
		}
		std::vector<StaticGroupChanges> changes;
		_bus->staticImportGroupChanges.Drain(changes,
			static_cast<std::size_t>(_settings->maxPreparedCellDrainPerPass));
		if (changes.empty()) {
			return false;
		}

		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::DrainGroupChanges");
		bool added = false;
		for (const auto& change : changes) {
			if (change.cellChange == StaticHostCellChange::Prepared) {
				for (auto* extension : _extensions) {
					extension->OnCellPrepared(change.cell, change.cellGeneration,
						static_cast<std::uint32_t>(change.adds.size()));
				}
				for (const auto& hostPrepare : change.hostCellPrepares) {
					if (hostPrepare.extension < _extensions.size()) {
						_extensions[hostPrepare.extension]->OnCellHostPrepared(
							change.cell, change.cellGeneration, hostPrepare.value);
					}
				}
			} else if (change.cellChange == StaticHostCellChange::Left) {
				DropHostCell(change.cell);
			}
			ApplyGroupChanges(change.adds, change.removes);
			added = added || !change.adds.empty();
		}
		if (added) {
			RefreshGraphCapacityHint();
		}
		return true;
	}

	bool StaticImportCoordinator::ScheduleCells()
	{
		if (_state.pendingCellQueue.empty()) {
			return false;
		}

		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::ScheduleCells");
		const auto maxJobsPerPass = static_cast<std::size_t>(
			_settings->maxCellPrepareJobsPerPass);
		const auto maxOutstandingCellJobs = static_cast<std::size_t>(
			_settings->maxOutstandingCellPrepareJobs);
		const auto outstandingCellJobs =
			_bus->staticImportCellPrepareJobs.Size() + _bus->staticImportPreparedCellResults.Size();
		if (maxOutstandingCellJobs != 0 && outstandingCellJobs >= maxOutstandingCellJobs) {
			BT_PLOT("SARP.StaticImportCoordinator.CellJobsOutstanding", static_cast<int64_t>(outstandingCellJobs));
			BT_PLOT("SARP.StaticImportCoordinator.PendingCellJobs", static_cast<int64_t>(_state.pendingCellQueue.size()));
			return false;
		}
		std::size_t jobsSubmitted = 0;
		while (!_shared->staticImportServiceStop.load(std::memory_order_acquire) &&
			(maxJobsPerPass == 0 || jobsSubmitted < maxJobsPerPass) &&
			(maxOutstandingCellJobs == 0 || outstandingCellJobs + jobsSubmitted < maxOutstandingCellJobs) &&
			!_state.pendingCellQueue.empty()) {
			auto job = std::move(_state.pendingCellQueue.front());
			_state.pendingCellQueue.pop_front();
			if (!_state.desiredCells.contains(job.key) ||
				_state.activeCellGroups.contains(job.key)) {
				_state.pendingCellKeys.erase(job.key);
				continue;
			}
			if (!_bus->staticImportCellPrepareJobs.TryPush(std::move(job))) {
				_state.pendingCellQueue.push_front(std::move(job));
				break;
			}
			_shared->staticImportWorkScheduler.Publish(StaticImportWorkSource::CellPrepare);
			++jobsSubmitted;
		}
		BT_PLOT("SARP.StaticImportCoordinator.CellJobsSubmitted", static_cast<int64_t>(jobsSubmitted));
		BT_PLOT("SARP.StaticImportCoordinator.CellJobsOutstanding", static_cast<int64_t>(outstandingCellJobs + jobsSubmitted));
		BT_PLOT("SARP.StaticImportCoordinator.PendingCellJobs", static_cast<int64_t>(_state.pendingCellQueue.size()));
		if (jobsSubmitted != 0) {
			_drains->ScheduleWorkerDrains();
		}
		return jobsSubmitted != 0;
	}

	bool StaticImportCoordinator::EnqueueRequest(StaticTemplateRequest request)
	{
		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::EnqueueTemplateRequest");
		if (_state.templateRefs->contains(request.key)) {
			return false;
		}
		if (_state.templateArtifactVersions.contains(request.key)) {
			++_importState->staticTemplateDefensiveDuplicates;
			return false;
		}
		auto* graphRequests = _shared->hasSceneIngestionServices
			? _shared->sceneIngestionServices.execution.stateRequests : nullptr;
		if (!graphRequests) return false;
		const auto key = request.key;
		const auto fingerprint = StaticTemplateSourceFingerprint(key);
		{
			std::lock_guard lock(_importState->staticTemplateGraphMutex);
			_importState->staticTemplateGraphSources.insert_or_assign(key, request);
		}
		auto input = std::make_shared<StaticTemplateArtifactBuildInput>();
		input->key = key;
		input->sourceFingerprint = fingerprint;
		const auto result = graphRequests->Request(
			StaticTemplateArtifactAddress(key), 1, {},
			br::render::ArtifactPayload::Make<StaticTemplateArtifactBuildInput>(
				std::move(input)), fingerprint);
		if (!result) {
			{
				std::lock_guard lock(_importState->staticTemplateGraphMutex);
				_importState->staticTemplateGraphSources.erase(key);
			}
			(void)_drains->PushBlocking(_bus->staticImportTemplateRefsPublished,
				StaticTemplateRefsPublished{ {}, { key } });
			_drains->ScheduleCoordinatorDrain();
			return false;
		}
		_state.pendingTemplateKeys.insert(key);
		_state.templateArtifactVersions.emplace(key, result.Handle());
		auto awaiter = graphRequests->AwaitExact(result.Handle(),
			br::render::ArtifactReadiness::CpuReady, TaskLane::Streaming,
			TaskDomain::StaticImportControl,
			[this, key](const br::render::ArtifactSnapshot& snapshot) {
				StaticTemplateRefsPublished packet;
				if (const auto published = snapshot.payload
					.Get<PublishedStaticTemplateArtifact>()) {
					packet.refs.emplace(key,
						std::make_shared<const br::render::StaticMeshTemplateRef>(published->ref));
				} else {
					packet.failedKeys.push_back(key);
				}
				(void)_drains->PushBlocking(
					_bus->staticImportTemplateRefsPublished, std::move(packet));
				_drains->ScheduleCoordinatorDrain();
			},
			[this, key](const br::render::ArtifactTermination&) {
				// Same unwind as a build that produced no ref: the template is
				// reported failed so its requesters stop waiting on it.
				StaticTemplateRefsPublished packet;
				packet.failedKeys.push_back(key);
				(void)_drains->PushBlocking(
					_bus->staticImportTemplateRefsPublished, std::move(packet));
				_drains->ScheduleCoordinatorDrain();
			});
		_state.templateArtifactAwaiters.emplace(key, std::move(awaiter));
		return true;
	}

	bool StaticImportCoordinator::EnqueueRequest(StaticMaterialVariantRequest request)
	{
		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::EnqueueMaterialVariantRequest");
		const auto variantKey = request.modelPath + "#txsw:" + std::to_string(request.variant.hash);
		if (!_state.pendingMaterialVariantKeys.insert(variantKey).second) {
			return false;
		}
		return _drains->PushWorker(_bus->staticImportMaterialVariantJobs, std::move(request),
			StaticImportWorkSource::MaterialVariant);
	}

	void StaticImportCoordinator::RequeueGroup(const DirectStaticPrepareInput& input)
	{
		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::RequeueGroup");
		if (!_state.pendingGroups.contains(input.groupID)) {
			return;
		}
		if (_state.readyGroups.contains(input.groupID)) {
			return;
		}
		if (!input.record || !IsGroupLive(*input.record)) {
			return;
		}
		if (_state.pendingQueued.insert(input.groupID).second) {
			_state.pendingGroupQueue.push_back(input.groupID);
			input.record->stage.store(StaticGroupLifecycleStage::Runnable,
				std::memory_order_release);
			++_state.stats.groupsRequeued;
		}
	}

	bool StaticImportCoordinator::DrainAssetResults()
	{
		std::vector<StaticImportAssetPublishResult> results;
		_bus->staticImportAssetPublishResults.Drain(results, 64);
		if (results.empty()) {
			return false;
		}

		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::DrainAssetResults");
		// The asset source has a durable readiness callback. Keep a long fallback only
		// for external resolvers that cannot notify; normal completions are promoted
		// immediately by PromoteNotifiedAssetDependencies().
		const auto retryDelay = std::chrono::seconds(5);
		for (auto& result : results) {
			auto dependency = MakeStaticImportDependencyRequest(*_assets, std::move(result.request));
			const auto key = dependency.key;
			_state.pendingAssetPublishKeys.erase(key.stringKey);
			_state.pendingDependencyRequests.erase(key);
			if (result.status == StaticImportDependencyResultStatus::Ready) {
				ResolveDependency(key);
			} else if (result.status == StaticImportDependencyResultStatus::Failed) {
				FailDependency(key);
			} else if (IsDependencyResolved(dependency)) {
				ResolveDependency(key);
			} else if (HasHostCellWaiters(key) ||
				_state.graphDependencyWaiters.contains(key)) {
				_state.delayedDependencyRequests.insert_or_assign(key, DelayedStaticImportDependencyRequest{
					.request = std::move(dependency),
					.dueAt = std::chrono::steady_clock::now() + retryDelay
				});
			}
		}
		return true;
	}

	bool StaticImportCoordinator::DrainMaterialResults()
	{
		std::vector<StaticImportMaterialVariantResult> results;
		_bus->staticImportMaterialVariantResults.Drain(results, 64);
		if (results.empty()) {
			return false;
		}

		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::DrainMaterialResults");
		for (auto& result : results) {
			auto dependency = MakeStaticImportDependencyRequest(*_assets, std::move(result.request));
			const auto key = dependency.key;
			_state.pendingMaterialVariantKeys.erase(key.stringKey);
			_state.pendingDependencyRequests.erase(key);
			if (result.status == StaticImportDependencyResultStatus::Ready) {
				ResolveDependency(key);
			} else if (result.status == StaticImportDependencyResultStatus::Failed) {
				FailDependency(key);
			} else if (IsDependencyResolved(dependency)) {
				ResolveDependency(key);
			} else if (HasHostCellWaiters(key) ||
				_state.graphDependencyWaiters.contains(key)) {
				_state.delayedDependencyRequests.insert_or_assign(key, DelayedStaticImportDependencyRequest{
					.request = std::move(dependency),
					.dueAt = std::chrono::steady_clock::now() + std::chrono::seconds(5)
				});
			}
		}
		return true;
	}

	bool StaticImportCoordinator::DrainShaderVariantResults()
	{
		std::vector<StaticImportShaderVariantResult> results;
		_bus->staticImportShaderVariantResults.Drain(results, 64);
		if (results.empty()) {
			return false;
		}

		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::DrainShaderVariantResults");
		for (auto& result : results) {
			auto dependency = MakeStaticImportDependencyRequest(std::move(result.request));
			const auto key = dependency.key;
			_state.pendingShaderVariantKeys.erase(key.stringKey);
			_state.pendingDependencyRequests.erase(key);
			if (result.status == StaticImportDependencyResultStatus::Ready) {
				ResolveDependency(key);
			} else if (result.status == StaticImportDependencyResultStatus::Failed) {
				FailDependency(key);
			} else if (HasHostCellWaiters(key) ||
				_state.graphDependencyWaiters.contains(key)) {
				_state.delayedDependencyRequests.insert_or_assign(key, DelayedStaticImportDependencyRequest{
					.request = std::move(dependency),
					.dueAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(25)
				});
			}
		}
		return true;
	}

	bool StaticImportCoordinator::DrainGroupResults()
	{
		std::vector<br::render::ArtifactSnapshot> readySnapshots;
		std::vector<std::uint64_t> terminalPacketIDs;
		std::vector<br::render::ArtifactAddress> terminalVariantAddresses;
		{
			std::lock_guard lock(_shared->graphCompletions->staticGroupMutex);
			while (!_shared->graphCompletions->terminalStaticVariants.empty()) {
				terminalVariantAddresses.push_back(
					_shared->graphCompletions->terminalStaticVariants.front().key);
				_shared->graphCompletions->terminalStaticVariants.pop_front();
			}
			// Drain enough completed preparation packets at once for assembly workers to
			// form transaction-sized batches.  The old eight-packet cap caused a
			// burst/idle convoy: workers consumed each small burst before the
			// coordinator exposed the next one, defeating adaptive packet coalescing.
			const auto count = (std::min<std::size_t>)(32,
				_shared->graphCompletions->staticGroups.size());
			for (std::size_t index = 0; index < count; ++index) {
				auto snapshot = std::move(_shared->graphCompletions->staticGroups.front());
				_shared->graphCompletions->staticGroups.pop_front();
				if (snapshot.readiness == br::render::ArtifactReadiness::Failed ||
					snapshot.readiness == br::render::ArtifactReadiness::Cancelled ||
					snapshot.readiness == br::render::ArtifactReadiness::Superseded) {
					terminalPacketIDs.push_back(snapshot.key.primaryID);
				} else if (const auto artifact = snapshot.payload.Get<PreparedStaticGroupArtifact>();
					artifact && !artifact->intentTokens.empty()) {
					readySnapshots.push_back(std::move(snapshot));
				}
			}
		}
		if (!terminalVariantAddresses.empty()) {
			std::vector<std::uint64_t> failedGroups;
			std::unordered_set<std::uint64_t> affectedPackets;
			_state.failedVariantAddresses.insert(
				terminalVariantAddresses.begin(), terminalVariantAddresses.end());
			for (const auto& [groupID, record] : _state.pendingGroups) {
				if (!record) continue;
				const auto address = StaticVariantArtifactAddress(
					record->descriptor, _shared->staticSceneRenderSettings);
				if (std::ranges::find(terminalVariantAddresses, address) !=
					terminalVariantAddresses.end()) {
					failedGroups.push_back(groupID);
					if (const auto packet = _state.groupPacketIDs.find(groupID);
						packet != _state.groupPacketIDs.end())
						affectedPackets.insert(packet->second);
				}
			}
			for (const auto groupID : failedGroups)
				(void)RemoveGroup(groupID);
			// An exact dependency failure blocks the whole graph packet. Detach its
			// surviving members before cancelling it, then admit them with a new
			// dependency union that excludes the failed variant.
			for (const auto packetID : affectedPackets) {
				std::vector<std::uint64_t> survivors;
				for (const auto& [groupID, memberPacketID] : _state.groupPacketIDs)
					if (memberPacketID == packetID) survivors.push_back(groupID);
				spdlog::warn("SARP mixed static packet fallback: packet={} terminal_variant_groups_in_batch={} surviving_groups={} action=reschedule",
					packetID, failedGroups.size(), survivors.size());
				if (auto* requests = _shared->hasSceneIngestionServices
					? _shared->sceneIngestionServices.execution.stateRequests : nullptr)
					requests->Cancel({ br::render::ArtifactKind::StaticGroup, packetID, 0 });
				_state.packetArtifactVersions.erase(packetID);
				_state.packetLiveMemberCounts.erase(packetID);
				if (const auto credits = _state.packetPreparationCredits.find(packetID);
					credits != _state.packetPreparationCredits.end()) {
					_state.activePreparationCredits -= (std::min)(_state.activePreparationCredits, credits->second);
					_state.packetPreparationCredits.erase(credits);
				}
				for (const auto groupID : survivors) {
					_state.groupPacketIDs.erase(groupID);
					_state.workerInFlightGroups.erase(groupID);
					_state.producerActiveGroups.erase(groupID);
					if (const auto token = _state.groupIntentTokens.find(groupID);
						token != _state.groupIntentTokens.end()) {
						token->second->valid.store(false, std::memory_order_release);
						_state.groupIntentTokens.erase(token);
					}
					(void)QueueGroupIfRunnable(groupID);
				}
			}
			if (_importState->benchmarkDiagnosticsEnabled) {
				spdlog::info(
					"SARP terminal static variants: events={} removed_groups={} remaining_pending={}",
					terminalVariantAddresses.size(), failedGroups.size(), _state.pendingGroups.size());
			}
			BT_PLOT("SARP.StaticImportCoordinator.VariantDependencyFailedGroups",
				static_cast<int64_t>(failedGroups.size()));
		}
		for (const auto packetID : terminalPacketIDs) {
			std::vector<std::uint64_t> failedGroups;
			for (const auto& [groupID, memberPacketID] : _state.groupPacketIDs) {
				if (memberPacketID == packetID) failedGroups.push_back(groupID);
			}
			for (const auto groupID : failedGroups) {
				(void)RemoveGroup(groupID);
			}
			_state.packetArtifactVersions.erase(packetID);
			_state.packetLiveMemberCounts.erase(packetID);
			if (const auto credits = _state.packetPreparationCredits.find(packetID);
				credits != _state.packetPreparationCredits.end()) {
				_state.activePreparationCredits -= (std::min)(
					_state.activePreparationCredits, credits->second);
				_state.packetPreparationCredits.erase(credits);
			}
			BT_PLOT("SARP.StaticImportCoordinator.GraphDependencyFailedGroups",
				static_cast<int64_t>(failedGroups.size()));
		}
		if (readySnapshots.empty()) return !terminalPacketIDs.empty() || !terminalVariantAddresses.empty();

		// Admission validates each logical member independently. A packet may contain
		// unrelated models and material variants, so blocked or stale members cannot
		// hold ready members behind their dependency lifetime.
		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::AdmitPreparedPackets");
		const auto currentGeneration =
			_importState->staticImportServiceTargetGeneration.load(std::memory_order_acquire);
		std::vector<br::render::ArtifactKey> releasedGroupArtifacts;
		std::uint64_t assemblyJobs = 0;
		for (auto& snapshot : readySnapshots) {
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::AdmitPreparedPackets::Packet");
			const auto artifact = snapshot.payload.Get<PreparedStaticGroupArtifact>();
			if (!artifact) continue;
			const auto memberCount = (std::max<std::uint64_t>)(
				1u, artifact->intentTokens.size());
			const auto elapsedUs = (std::max<std::uint64_t>)(
				1u, artifact->result.buildUs + artifact->result.prepareUs);
			const auto sampleNsPerGroup = (std::max<std::uint64_t>)(
				1u, elapsedUs * 1000u / memberCount);
			_state.preparationNanosecondsPerGroup =
				(_state.preparationNanosecondsPerGroup * 7u + sampleNsPerGroup) / 8u;
			BT_PLOT("SARP.StaticImportCoordinator.PacketCpuNsPerGroup",
				static_cast<int64_t>(_state.preparationNanosecondsPerGroup));
			br::render::ArtifactVersionHandle exactVersion;
			if (auto version = _state.packetArtifactVersions.find(artifact->packetID);
				version != _state.packetArtifactVersions.end()) {
				exactVersion = std::move(version->second);
				_state.packetArtifactVersions.erase(version);
			}
			if (auto credits = _state.packetPreparationCredits.find(artifact->packetID);
				credits != _state.packetPreparationCredits.end()) {
				_state.activePreparationCredits -= (std::min)(
					_state.activePreparationCredits, credits->second);
				_state.packetPreparationCredits.erase(credits);
			}
			_state.packetLiveMemberCounts.erase(artifact->packetID);
			releasedGroupArtifacts.push_back(
				{ br::render::ArtifactKind::StaticGroup, artifact->packetID, 0 });

			{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::AdmitPreparedPackets::ReleasePacketMembers");
			for (const auto& token : artifact->intentTokens) {
				if (!token) continue;
				_state.producerActiveGroups.erase(token->groupID);
				_state.workerInFlightGroups.erase(token->groupID);
				_state.groupPacketIDs.erase(token->groupID);
			}
			}

			{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::AdmitPreparedPackets::DropUnsupported");
			for (const auto& unsupported : artifact->result.unsupported) {
				spdlog::warn("SARP static import rejected group={} placements={} model='{}' reason={}",
					unsupported.groupID, unsupported.placementCount, unsupported.modelPath,
					unsupported.reason);
				if (RemoveGroup(unsupported.groupID))
					++_state.stats.unsupportedGroupsDropped;
			}
			}

			{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::AdmitPreparedPackets::AdmitBlocked");
			for (auto& blocked : artifact->result.blockedGroups) {
				const auto& token = blocked.intentToken;
				auto pendingIt = _state.pendingGroups.find(blocked.groupID);
				if (!token || !token->valid.load(std::memory_order_acquire) ||
					pendingIt == _state.pendingGroups.end() || !pendingIt->second ||
					!IsGroupLive(*pendingIt->second)) {
					++_state.stats.staleGroupsDropped;
					continue;
				}
				ClearGroupBlockers(blocked.groupID);
				if (blocked.checkpoint)
					_state.groupCheckpoints.insert_or_assign(blocked.groupID, blocked.checkpoint);
				pendingIt->second->stage.store(StaticGroupLifecycleStage::Blocked,
					std::memory_order_release);
				StaticGroupSuspensionState suspension{
					.revision = token->revision, .identity = 0 };
				bool failed = false;
				for (auto& dependency : blocked.dependencies) {
					if (IsDependencyResolved(dependency)) continue;
					if (IsDependencyFailed(dependency)) { failed = true; continue; }
					suspension.remaining.insert(dependency.key);
					_state.graphDependencyWaiters[dependency.key].insert(blocked.groupID);
					(void)SubmitDependencyRequest(std::move(dependency));
				}
				if (failed) {
					(void)RemoveGroup(blocked.groupID);
				} else if (suspension.remaining.empty()) {
					(void)QueueGroupIfRunnable(blocked.groupID);
				} else {
					_state.groupSuspensions.insert_or_assign(blocked.groupID, std::move(suspension));
				}
			}
			}

			std::unordered_map<std::uint64_t, std::uint64_t> publishTickets;
			publishTickets.reserve(artifact->result.ready.size());
			{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::AdmitPreparedPackets::AdmitReady");
			for (const auto& ready : artifact->result.ready) {
				const auto& token = ready.intentToken;
				auto pendingIt = _state.pendingGroups.find(ready.groupID);
				if (!token || !token->valid.load(std::memory_order_acquire) ||
					pendingIt == _state.pendingGroups.end() ||
					!pendingIt->second ||
					pendingIt->second->descriptor.groupRevision != ready.groupRevision ||
					!IsPreparedLive(ready)) {
					++_state.stats.staleGroupsDropped;
					continue;
				}
				if (!_state.readyGroups.insert(ready.groupID).second) continue;
				pendingIt->second->stage.store(StaticGroupLifecycleStage::Ready,
					std::memory_order_release);
				const auto ticket = _state.nextPublishTicketID++;
				_state.readyGroupTickets.insert_or_assign(ready.groupID, ticket);
				const auto preparedCost = StaticImportPreparedRangeCost(
					artifact->result.transactionBuild.prepared,
					ready.firstBuildGroup, ready.submittedGroupCount);
				_state.preparedBytesByPublishTicket.insert_or_assign(
					ticket, preparedCost.bytes);
				_state.preparedUnpublishedBytes += preparedCost.bytes;
				publishTickets.emplace(ready.groupID, ticket);
				++_state.stats.groupsPrepared;
				if (auto* requests = _shared->hasSceneIngestionServices
					? _shared->sceneIngestionServices.execution.stateRequests : nullptr;
					requests && requests->TraceActive()) {
					requests->TraceEvent(
						br::render::AsyncStateGraphTraceEventID::StaticGroupPrepared,
						{ br::render::ArtifactKind::StaticGroup, ready.groupID, 0 },
						token->revision, 0,
						{ { ready.placementCount, artifact->result.buildUs,
							artifact->result.prepareUs } });
				}
			}
			}
			if (!publishTickets.empty()) {
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::AdmitPreparedPackets::QueueAssembly");
				StaticGroupAssemblyJob assemblyJob{
					.snapshot = std::move(snapshot), .version = std::move(exactVersion),
					.targetGeneration = currentGeneration,
					.publishTickets = std::move(publishTickets) };
				if (!_bus->staticImportGroupAssemblyJobs.TryPush(std::move(assemblyJob))) {
					for (const auto& [groupID, _] : assemblyJob.publishTickets) {
						_state.readyGroups.erase(groupID);
						if (const auto ticket = _state.readyGroupTickets.find(groupID);
							ticket != _state.readyGroupTickets.end()) {
							ReleasePreparedTicket(ticket->second);
							_state.readyGroupTickets.erase(ticket);
						}
						(void)QueueGroupIfRunnable(groupID);
					}
				} else {
					++assemblyJobs;
				}
			}
			_state.stats.groupBuildUs += artifact->result.buildUs;
			_state.stats.objectPlanPrepareUs += artifact->result.prepareUs;
		}
		{
		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::AdmitPreparedPackets::DispatchWork");
		if (assemblyJobs != 0) {
			_shared->staticImportWorkScheduler.Publish(
				StaticImportWorkSource::GroupAssembly, assemblyJobs);
		}
		if (!releasedGroupArtifacts.empty()) {
			for (auto& key : releasedGroupArtifacts)
				(void)_bus->staticImportGroupArtifactReleaseJobs.TryPush(std::move(key));
			_shared->staticImportWorkScheduler.Signal(
				StaticImportWorkSource::GroupArtifactRelease);
		}
		if (assemblyJobs != 0 || !releasedGroupArtifacts.empty())
			_drains->ScheduleWorkerDrains();
		}
		return true;
	}

	bool StaticImportCoordinator::DrainMaterializedValidationRequests()
	{
		std::vector<MaterializedDirectStaticImportBatch> requests;
		_bus->staticImportMaterializedValidationRequests.Drain(
			requests,
			static_cast<std::size_t>(_settings->maxMaterializedValidationDrainPerPass));
		if (requests.empty()) {
			return false;
		}

		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::DrainMaterializedValidationRequests");
		BT_PLOT("SARP.StaticDirect.MaterializedValidationRequests", static_cast<int64_t>(requests.size()));
		std::size_t publishDecisions = 0;
		std::size_t cancelDecisions = 0;
		std::size_t validatedGroups = 0;
		std::size_t rejectedGroups = 0;
		const auto currentGeneration = _importState->staticImportServiceTargetGeneration.load(std::memory_order_acquire);

		for (auto& item : requests) {
			StaticImportMaterializedPublishDecision decision;
			decision.item = std::move(item);
			decision.targetGeneration = currentGeneration;
			decision.livenessEpoch = _state.livenessEpoch;
			decision.cost = decision.item.batch.transactionCost;

			bool valid = decision.item.batch.publishTicketID != 0 &&
				!decision.item.batch.imports.empty();
			const auto ackForTicket = [&decision](std::uint64_t ticketID)
				-> StaticImportGroupAck& {
				auto found = std::ranges::find(
					decision.acks, ticketID, &StaticImportGroupAck::ticketID);
				if (found != decision.acks.end()) return *found;
				auto& ack = decision.acks.emplace_back();
				ack.ticketID = ticketID;
				return ack;
			};

			for (const auto& prepared : decision.item.batch.imports) {
				++validatedGroups;
				const bool desired = IsPreparedLive(prepared);
				bool groupValid = false;
				bool ready = false;
				bool ticketMatches = false;
				if (prepared.hostOwner) {
					groupValid = _state.latestTarget.valid &&
						prepared.hostOwner->IsHostImportCurrent(_state.latestTarget.stream, prepared.cellKey, prepared.hostPayload);
				} else {
					const auto pendingIt = _state.pendingGroups.find(prepared.groupID);
					const bool pending = pendingIt != _state.pendingGroups.end();
					ready = _state.readyGroups.contains(prepared.groupID);
					const auto ticketIt = _state.readyGroupTickets.find(prepared.groupID);
					ticketMatches = ticketIt != _state.readyGroupTickets.end() &&
						ticketIt->second == prepared.admissionTicketID;
					groupValid = pending &&
						desired &&
						ready &&
						ticketMatches &&
						pendingIt->second &&
						prepared.groupRevision == pendingIt->second->descriptor.groupRevision;
				}
				if (!groupValid) {
					valid = false;
					++rejectedGroups;
				}
				if (!prepared.hostOwner && desired && ready && ticketMatches) {
					ackForTicket(prepared.admissionTicketID).requeue.push_back(
						prepared.groupID);
				} else if (!prepared.hostOwner) {
					ackForTicket(prepared.admissionTicketID).dropped.push_back(
						prepared.groupID);
				}
			}

			decision.kind = valid
				? StaticImportMaterializedDecisionKind::Publish
				: StaticImportMaterializedDecisionKind::Cancel;
			if (valid) {
				// ObjectManager now owns the exact materialized allocations.  Keep only
				// any publication-cut component reserved until the GPU-ready scene cut;
				// cancellation still releases that remainder with the lifecycle record.
				for (const auto& prepared : decision.item.batch.imports) {
					if (prepared.hostOwner) continue;
					TransferPreparedTicketToMaterialized(
						prepared.admissionTicketID);
					const auto pending = _state.pendingGroups.find(prepared.groupID);
					if (pending == _state.pendingGroups.end() || !pending->second ||
						!pending->second->capacityReservation) continue;
					const auto remaining = pending->second->capacityReservation->ReservedCost();
					pending->second->capacityReservation->ReleaseComponents({
						.preparedBytes = remaining.preparedBytes,
						.drawRecords = remaining.drawRecords,
						.activeIndices = remaining.activeIndices,
						.uploadBytes = remaining.uploadBytes });
				}
				decision.acks.clear();
				++publishDecisions;
			} else {
				++cancelDecisions;
			}
			if (auto* graphRequests = _shared->hasSceneIngestionServices
				? _shared->sceneIngestionServices.execution.stateRequests : nullptr;
				graphRequests && graphRequests->TraceActive()) {
				for (const auto& prepared : decision.item.batch.imports) {
					graphRequests->TraceEvent(br::render::AsyncStateGraphTraceEventID::StaticGroupValidated,
						{ br::render::ArtifactKind::StaticGroup, prepared.groupID, 0 },
						prepared.generation, 0,
						{ { decision.item.batch.publishTicketID, valid } });
				}
			}
			(void)_drains->PushStreaming(
				_bus->staticImportMaterializedPublishDecisions, std::move(decision));
		}

		BT_PLOT("SARP.StaticDirect.MaterializedPublishDecisions", static_cast<int64_t>(publishDecisions));
		BT_PLOT("SARP.StaticDirect.MaterializedCancelDecisions", static_cast<int64_t>(cancelDecisions));
		BT_PLOT("SARP.StaticImportCoordinator.PublishValidationGroups", static_cast<int64_t>(validatedGroups));
		BT_PLOT("SARP.StaticImportCoordinator.PublishValidationRejectedGroups", static_cast<int64_t>(rejectedGroups));
		return true;
	}

	bool StaticImportCoordinator::ScheduleGroups()
	{
		if (_state.pendingGroupQueue.empty()) {
			return false;
		}
		auto* requests = _shared->hasSceneIngestionServices
			? _shared->sceneIngestionServices.execution.stateRequests : nullptr;
		if (!requests || !_importState->staticGroupProducerRegistered) return false;

		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::ScheduleGroups");
		const auto explicitCreditLimit =
			_settings->preparationCreditLimit;
		const bool compatibilityCredits = explicitCreditLimit != 0;
		const auto creditLimit = static_cast<std::uint64_t>(explicitCreditLimit);
		if (compatibilityCredits && _state.activePreparationCredits >= creditLimit) {
			BT_PLOT("SARP.StaticImportCoordinator.AdmissionStoppedCredits", int64_t{ 1 });
			return false;
		}
		const auto preparedByteCeiling = static_cast<std::uint64_t>(
			_settings->preparedUnpublishedByteCeiling);
		if (_state.materializedUnpublishedBytes >= preparedByteCeiling) {
			BT_PLOT("SARP.StaticImportCoordinator.AdmissionStoppedMaterializedBytes", int64_t{ 1 });
			BT_PLOT("SARP.StaticImportCoordinator.MaterializedUnpublishedBytes",
				static_cast<int64_t>(_state.materializedUnpublishedBytes));
			return false;
		}
		if (_state.preparedUnpublishedBytes >= preparedByteCeiling) {
			BT_PLOT("SARP.StaticImportCoordinator.AdmissionStoppedPreparedBytes", int64_t{ 1 });
			BT_PLOT("SARP.StaticImportCoordinator.PreparedUnpublishedBytes",
				static_cast<int64_t>(_state.preparedUnpublishedBytes));
			return false;
		}

		struct PendingSubmission
		{
			std::uint64_t packetID{ 0 };
			std::uint64_t credits{ 0 };
			std::vector<std::uint64_t> groupIDs;
			std::vector<std::uint64_t> revisions;
			std::vector<std::uint64_t> residencyGenerations;
			std::vector<std::uint64_t> placementCounts;
		};
		std::vector<PendingSubmission> submissions;
		std::vector<br::render::ArtifactIntent> intents;
		const auto availableCredits = compatibilityCredits
			? creditLimit - _state.activePreparationCredits
			: (std::numeric_limits<std::uint64_t>::max)();
		constexpr std::size_t maxSubmissionsPerMutation = 32;
		submissions.reserve(compatibilityCredits
			? static_cast<std::size_t>(availableCredits) : maxSubmissionsPerMutation);
		intents.reserve(submissions.capacity());
		const auto hardMaxPacketGroups = static_cast<std::size_t>(
			_settings->preparationPacketGroups);
		const auto targetPacketCpuNanoseconds = static_cast<std::uint64_t>(
			_settings->preparationPacketTargetUs) * 1000u;
		// Dependency discovery can make a cold packet look expensive even though
		// most of that work immediately blocks into independent continuations. Do
		// not let that transient cost recreate one-artifact-per-group scheduling.
		const auto minimumPacketGroups = (std::min<std::size_t>)(
			32u, hardMaxPacketGroups);
		const auto maxPacketGroups = (std::clamp<std::size_t>)(
			static_cast<std::size_t>(targetPacketCpuNanoseconds /
				(std::max<std::uint64_t>)(1u, _state.preparationNanosecondsPerGroup)),
			minimumPacketGroups, hardMaxPacketGroups);
		const auto maxPacketPlacements = static_cast<std::uint64_t>(
			_settings->preparationPacketPlacements);
		const auto maxPacketBytes = static_cast<std::uint64_t>(
			_settings->preparationPacketBytes);
		std::uint64_t plannedCredits = 0;
		bool capacityBlocked = false;
		std::size_t capacityBlockedGroupsInspected = 0;
		const auto admissionGroupsAtStart = _state.pendingGroupQueue.size();
		if (_state.pendingGroupSortedPrefix == 0)
			_state.pendingGroupSortedPrefix = _state.pendingGroupQueue.size();
		{
		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::ScheduleGroups::BuildPackets");
		while (!_shared->staticImportServiceStop.load(std::memory_order_acquire) &&
			!_state.pendingGroupQueue.empty() &&
			_state.pendingGroupSortedPrefix != 0 &&
			(!compatibilityCredits || plannedCredits < availableCredits) &&
			submissions.size() < maxSubmissionsPerMutation) {
			auto input = std::make_shared<StaticGroupArtifactBuildInput>();
			input->job.inputs.reserve(maxPacketGroups);
			input->intentTokens.reserve(maxPacketGroups);
			std::vector<br::render::ArtifactRequirement> variantRequirements;
			variantRequirements.reserve(maxPacketGroups);
			input->packetID = _state.nextPreparationPacketID++;
			input->job.generation = _state.latestTarget.generation;
			input->job.target = _state.latestTarget;
			input->job.templateRefs = _state.templateSnapshot;
			PendingSubmission submission;
			submission.groupIDs.reserve(maxPacketGroups);
			submission.revisions.reserve(maxPacketGroups);
			submission.residencyGenerations.reserve(maxPacketGroups);
			submission.placementCounts.reserve(maxPacketGroups);
			submission.packetID = input->packetID;
			std::uint64_t packetPlacements = 0;
			std::uint64_t packetBytes = 0;
			std::uint64_t fingerprint = input->packetID ^ _state.latestTarget.generation;
			std::optional<std::uint64_t> packetBatchKey;

			while (!_state.pendingGroupQueue.empty() &&
				_state.pendingGroupSortedPrefix != 0 &&
				input->job.inputs.size() < maxPacketGroups) {
				const auto groupID = _state.pendingGroupQueue.front();
				auto groupIt = _state.pendingGroups.find(groupID);
				if (groupIt == _state.pendingGroups.end() || _state.readyGroups.contains(groupID)) {
					_state.pendingGroupQueue.pop_front();
					_state.pendingGroupSortedPrefix -= (std::min<std::size_t>)(
						_state.pendingGroupSortedPrefix, 1u);
					_state.pendingQueued.erase(groupID);
					continue;
				}
				if (!groupIt->second ||
					!IsGroupLive(*groupIt->second)) {
					_state.pendingGroupQueue.pop_front();
					_state.pendingGroupSortedPrefix -= (std::min<std::size_t>)(
						_state.pendingGroupSortedPrefix, 1u);
					_state.pendingQueued.erase(groupID);
					(void)RemoveGroup(groupID);
					++_state.stats.staleGroupsDropped;
					continue;
				}
				auto& descriptor = groupIt->second->descriptor;
				if (packetBatchKey && *packetBatchKey != descriptor.batchKey) break;
				packetBatchKey = descriptor.batchKey;
				const auto variantAddress = StaticVariantArtifactAddress(
					descriptor, _shared->staticSceneRenderSettings);
				const auto fallbackBytes = static_cast<std::uint64_t>(descriptor.placementCount) *
					(sizeof(DirectX::XMMATRIX) + sizeof(std::uint32_t));
				const auto estimatedBytes = descriptor.recipeImportCost.preparedBytes != 0
					? descriptor.recipeImportCost.preparedBytes : fallbackBytes;
				const bool exceeds = !input->job.inputs.empty() &&
					(packetPlacements + descriptor.placementCount > maxPacketPlacements ||
					 packetBytes + estimatedBytes > maxPacketBytes);
				if (exceeds) break;
				if (!compatibilityCredits && !groupIt->second->capacityReservation) {
					const auto drawRecordEstimate = [&] {
						if (descriptor.recipeImportCost.drawRecords != 0)
							return descriptor.recipeImportCost.drawRecords;
						if (const auto estimate = _state.capacityHintDrawRecordsByGroup.find(groupID);
							estimate != _state.capacityHintDrawRecordsByGroup.end()) return estimate->second;
						return static_cast<std::uint64_t>(descriptor.placementCount);
					}();
					auto reservation = _capacity->TryReserve({
						.preparedBytes = (std::max<std::uint64_t>)(estimatedBytes, 1u),
						.drawRecords = drawRecordEstimate,
						.activeIndices = descriptor.recipeImportCost.activeInsertIndices != 0
							? descriptor.recipeImportCost.activeInsertIndices : drawRecordEstimate,
						.uploadBytes = descriptor.recipeImportCost.uploadBytes != 0
							? descriptor.recipeImportCost.uploadBytes : estimatedBytes });
					if (!reservation) {
						capacityBlocked = true;
						break;
					}
					groupIt->second->capacityReservation.emplace(std::move(*reservation));
				}
				auto variant = _state.variantArtifactVersions.find(variantAddress);
				if (variant == _state.variantArtifactVersions.end()) {
					const auto variantFingerprint = StaticVariantFingerprint(
						descriptor, _shared->staticSceneRenderSettings);
					auto variantInput = std::make_shared<StaticVariantArtifactInput>();
					variantInput->gateID = variantAddress.primaryID ^
						std::rotl(variantAddress.variantID, 17) ^ 0x8000000000000000ull;
					variantInput->fingerprint = variantFingerprint;
					variantInput->target = _state.latestTarget;
					variantInput->representative = groupIt->second;
					auto requested = requests->Request(variantAddress, 1, {},
						br::render::ArtifactPayload::Make<StaticVariantArtifactInput>(
							std::move(variantInput)), variantFingerprint);
					if (!requested) {
						capacityBlocked = true;
						break;
					}
					variant = _state.variantArtifactVersions.emplace(
						variantAddress, requested.Handle()).first;
				}
				if (std::ranges::none_of(variantRequirements,
						[&](const auto& requirement) {
							return requirement.key == variantAddress;
						})) {
					variantRequirements.push_back(br::render::Exact(
						variant->second, br::render::ArtifactReadiness::CpuReady));
				}
				_state.pendingGroupQueue.pop_front();
				_state.pendingGroupSortedPrefix -= (std::min<std::size_t>)(
					_state.pendingGroupSortedPrefix, 1u);
				_state.pendingQueued.erase(groupID);
				packetPlacements += descriptor.placementCount;
				packetBytes += estimatedBytes;
				const auto revision = ++_state.groupArtifactRevisions[groupID];
				groupIt->second->intentRevision.store(revision,
					std::memory_order_release);
				groupIt->second->stage.store(StaticGroupLifecycleStage::Preparing,
					std::memory_order_release);
				if (auto previous = _state.groupIntentTokens.find(groupID);
					previous != _state.groupIntentTokens.end())
					previous->second->valid.store(false, std::memory_order_release);
				auto token = std::make_shared<StaticGroupIntentToken>();
				token->groupID = groupID;
				token->cellKey = descriptor.cellKey;
				token->batchKey = descriptor.batchKey;
				token->groupRevision = descriptor.groupRevision;
				token->residencyEpoch = descriptor.residencyGeneration;
				token->revision = revision;
				_state.groupIntentTokens.insert_or_assign(groupID, token);
				input->intentTokens.push_back(token);
				std::shared_ptr<const StaticGroupPreparationCheckpoint> checkpoint;
				if (auto saved = _state.groupCheckpoints.find(groupID);
					saved != _state.groupCheckpoints.end()) {
					checkpoint = saved->second;
				}
				input->job.inputs.push_back(DirectStaticPrepareInput{
					.groupID = groupID, .record = groupIt->second,
					.checkpoint = std::move(checkpoint) });
				submission.groupIDs.push_back(groupID);
				submission.revisions.push_back(revision);
				submission.residencyGenerations.push_back(descriptor.residencyGeneration);
				submission.placementCounts.push_back(descriptor.placementCount);
				fingerprint ^= groupID + revision + descriptor.materialVariant.hash +
					0x9e3779b97f4a7c15ull + (fingerprint << 6u) + (fingerprint >> 2u);
			}
			if (input->job.inputs.empty()) {
				if (capacityBlocked) {
					// A temporarily unreservable group must not become a
					// variant-sorted head-of-line barrier. Rotate it behind the
					// other admission candidates and inspect each initial member
					// at most once in this mutation. The broker's release callback
					// will wake a subsequent pass when capacity actually changes.
					const auto blockedGroupID = _state.pendingGroupQueue.front();
					_state.pendingGroupQueue.pop_front();
					_state.pendingGroupSortedPrefix -= (std::min<std::size_t>)(
						_state.pendingGroupSortedPrefix, 1u);
					_state.pendingGroupQueue.push_back(blockedGroupID);
					++capacityBlockedGroupsInspected;
					capacityBlocked = false;
					if (capacityBlockedGroupsInspected >= admissionGroupsAtStart) break;
					continue;
				}
				continue;
			}
			const auto divideRoundUp = [](std::uint64_t value, std::uint64_t unit) {
				return (value + unit - 1u) / unit;
			};
			submission.credits = (std::max)({ std::uint64_t{ 1 },
				divideRoundUp(input->job.inputs.size(), maxPacketGroups),
				divideRoundUp(packetPlacements, maxPacketPlacements),
				divideRoundUp(packetBytes, maxPacketBytes) });
			if (compatibilityCredits && plannedCredits != 0 &&
				plannedCredits + submission.credits > availableCredits) {
				for (auto it = submission.groupIDs.rbegin(); it != submission.groupIDs.rend(); ++it) {
					_state.pendingQueued.insert(*it);
					_state.pendingGroupQueue.push_front(*it);
					++_state.pendingGroupSortedPrefix;
					_state.groupIntentTokens[*it]->valid.store(false, std::memory_order_release);
				}
				break;
			}
			if (compatibilityCredits) plannedCredits += submission.credits;
			BT_PLOT("SARP.StaticImportCoordinator.PacketGroups", static_cast<int64_t>(submission.groupIDs.size()));
			BT_PLOT("SARP.StaticImportCoordinator.PacketPlacements", static_cast<int64_t>(packetPlacements));
			BT_PLOT("SARP.StaticImportCoordinator.PacketEstimatedBytes", static_cast<int64_t>(packetBytes));
			BT_PLOT("SARP.StaticImportCoordinator.PacketCredits", static_cast<int64_t>(submission.credits));
			BT_PLOT("SARP.StaticImportCoordinator.PacketTargetGroups",
				static_cast<int64_t>(maxPacketGroups));
			intents.push_back({
				{ br::render::ArtifactKind::StaticGroup, input->packetID, 0 }, 1,
				std::move(variantRequirements),
				br::render::ArtifactPayload::Make<StaticGroupArtifactBuildInput>(std::move(input)),
				fingerprint == 0 ? 1u : fingerprint });
			submissions.push_back(std::move(submission));
		}
		}
		if (intents.empty()) return false;

		std::vector<br::render::ArtifactRequestResult> results;
		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::ScheduleGroups::SubmitBatch");
			results = requests->SubmitLatestBatch(std::move(intents));
		}
		std::size_t jobsSubmitted = 0;
		{
		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::ScheduleGroups::RecordSubmissions");
		for (std::size_t index = 0; index < submissions.size(); ++index) {
			const auto& submission = submissions[index];
			if (index >= results.size() || !results[index]) {
				for (const auto groupID : submission.groupIDs)
					(void)QueueGroupIfRunnable(groupID);
				continue;
			}
			_state.packetArtifactVersions.insert_or_assign(
				submission.packetID, results[index].Handle());
			_state.packetLiveMemberCounts.insert_or_assign(
				submission.packetID, submission.groupIDs.size());
			if (compatibilityCredits) {
				_state.packetPreparationCredits.insert_or_assign(
					submission.packetID, submission.credits);
				_state.activePreparationCredits += submission.credits;
			}
			for (std::size_t member = 0; member < submission.groupIDs.size(); ++member) {
				const auto groupID = submission.groupIDs[member];
				_state.groupPacketIDs.insert_or_assign(groupID, submission.packetID);
				_state.groupCheckpoints.erase(groupID);
				_state.workerInFlightGroups.insert(groupID);
				_state.producerActiveGroups.insert(groupID);
				_state.pendingGroupLastTransition[groupID] = "graph_packet_submitted";
				if (requests->TraceActive()) {
					requests->TraceEvent(br::render::AsyncStateGraphTraceEventID::StaticGroupWorkerSubmitted,
						{ br::render::ArtifactKind::StaticGroup, groupID, 0 },
						submission.residencyGenerations[member], submission.revisions[member],
						{ { submission.placementCounts[member], submission.groupIDs.size(),
							submission.packetID } });
				}
			}
			jobsSubmitted += submission.groupIDs.size();
		}
		}
		{
		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::ScheduleGroups::PublishTelemetry");
		BT_PLOT("SARP.StaticImportCoordinator.GroupJobsSubmitted", static_cast<int64_t>(jobsSubmitted));
		BT_PLOT("SARP.StaticImportCoordinator.PacketsSubmitted", static_cast<int64_t>(submissions.size()));
		BT_PLOT("SARP.StaticImportCoordinator.ActivePreparationCredits", static_cast<int64_t>(_state.activePreparationCredits));
		BT_PLOT("SARP.StaticImportCoordinator.PreparedUnpublishedBytes",
			static_cast<int64_t>(_state.preparedUnpublishedBytes));
		BT_PLOT("SARP.StaticImportCoordinator.MaterializedUnpublishedBytes",
			static_cast<int64_t>(_state.materializedUnpublishedBytes));
		BT_PLOT("SARP.StaticImportCoordinator.GroupProducerActive",
			static_cast<int64_t>(_state.producerActiveGroups.size()));
		const auto capacity = _capacity->GetSnapshot();
		BT_PLOT("SARP.StaticImportCapacity.PreparedBytes",
			static_cast<int64_t>(capacity.reserved.preparedBytes));
		BT_PLOT("SARP.StaticImportCapacity.DrawRecords",
			static_cast<int64_t>(capacity.reserved.drawRecords));
		BT_PLOT("SARP.StaticImportCapacity.Reservations",
			static_cast<int64_t>(capacity.reservations));
		BT_PLOT("SARP.StaticImportCapacity.Releases",
			static_cast<int64_t>(capacity.releases));
		BT_PLOT("SARP.StaticImportCapacity.Failed",
			static_cast<int64_t>(capacity.failedCapacity));
		BT_PLOT("SARP.StaticImportCapacity.Blocked", capacityBlocked ? int64_t{ 1 } : int64_t{ 0 });
		BT_PLOT("SARP.StaticImportCapacity.BlockedGroupsInspected",
			static_cast<int64_t>(capacityBlockedGroupsInspected));
		}
		return jobsSubmitted != 0;
	}

	bool StaticImportCoordinator::HasWork() const
	{
		// Internal admission queues are coordinator-owned and have no external
		// producer notification. Omitting them made the next admission slice wait
		// for an already-submitted worker job to finish, coupling discovery latency
		// to preparation time for every four job packets.
		return !_state.pendingCellQueue.empty() ||
			!_state.pendingGroupQueue.empty() ||
			_importState->staticImportServiceHasPendingTarget.load(std::memory_order_acquire) ||
			!_bus->staticImportGroupAcks.Empty() || !_bus->staticTransactionReadyEvents.Empty() ||
			!_bus->staticScenePageReadyEvents.Empty() ||
			!_bus->staticSceneRootPublishedEvents.Empty() ||
			!_bus->staticImportPreparedCellResults.Empty() ||
			HasPendingGroupChanges() ||
			!_bus->staticGroupSuspensionRequests.Empty() ||
			_shared->graphCompletions->StaticGroupCount() != 0 || !_bus->staticImportAssetPublishResults.Empty() ||
			!_bus->staticImportMaterialVariantResults.Empty() || !_bus->staticImportShaderVariantResults.Empty() ||
			std::ranges::any_of(_extensions, [](const auto* extension) { return extension->HasCoordinatorResults(); }) ||
			!_bus->staticImportMaterializedValidationRequests.Empty() || !_bus->staticImportTemplateRefsPublished.Empty();
	}

	bool StaticImportCoordinator::DrainStaticGroupSuspensionRequests()
	{
		std::vector<StaticGroupSuspensionRequest> requests;
		_bus->staticGroupSuspensionRequests.Drain(requests, 64);
		if (requests.empty()) return false;
		for (auto& request : requests) {
			_state.producerActiveGroups.erase(request.groupID);
			auto pendingIt = _state.pendingGroups.find(request.groupID);
			const auto revisionIt = _state.groupArtifactRevisions.find(request.groupID);
			if (!request.externalGraphGate && (pendingIt == _state.pendingGroups.end() ||
				revisionIt == _state.groupArtifactRevisions.end() ||
				revisionIt->second != request.revision ||
				(!pendingIt->second ||
				 !IsGroupLive(*pendingIt->second)))) {
				if (_importState->staticGroupSuspensionNotifier) _importState->staticGroupSuspensionNotifier(request.identity);
				continue;
			}
			ClearGroupBlockers(request.groupID);
			StaticGroupSuspensionState suspension{
				.revision = request.revision,
				.identity = request.identity
			};
			bool failed = false;
			for (auto& dependency : request.dependencies) {
				if (IsDependencyResolved(dependency)) continue;
				if (IsDependencyFailed(dependency)) {
					failed = true;
					continue;
				}
				suspension.remaining.insert(dependency.key);
				_state.graphDependencyWaiters[dependency.key].insert(request.groupID);
				(void)SubmitDependencyRequest(std::move(dependency));
			}
			if (failed || suspension.remaining.empty()) {
				for (const auto& key : suspension.remaining) {
					auto waitersIt = _state.graphDependencyWaiters.find(key);
					if (waitersIt == _state.graphDependencyWaiters.end()) continue;
					waitersIt->second.erase(request.groupID);
					if (waitersIt->second.empty()) _state.graphDependencyWaiters.erase(waitersIt);
				}
				if (_importState->staticGroupSuspensionNotifier) _importState->staticGroupSuspensionNotifier(request.identity);
			} else {
				_state.groupSuspensions.insert_or_assign(request.groupID, std::move(suspension));
			}
		}
		return true;
	}

	bool StaticImportCoordinator::HasReadyWork() const
	{
		// Exclude coordinator-owned admission backlogs. Those can remain
		// non-empty while bounded worker queues are saturated; treating them as
		// an immediate wake source spins the serialized control domain until a
		// worker frees capacity. Completion queues and camera updates are true
		// readiness notifications and should still wake without delay.
		return _importState->staticImportServiceHasPendingTarget.load(std::memory_order_acquire) ||
			!_bus->staticImportGroupAcks.Empty() || !_bus->staticTransactionReadyEvents.Empty() ||
			!_bus->staticScenePageReadyEvents.Empty() ||
			!_bus->staticSceneRootPublishedEvents.Empty() ||
			!_bus->staticImportPreparedCellResults.Empty() ||
			HasPendingGroupChanges() ||
			!_bus->staticGroupSuspensionRequests.Empty() ||
			_shared->graphCompletions->StaticGroupCount() != 0 || !_bus->staticImportAssetPublishResults.Empty() ||
			!_bus->staticImportMaterialVariantResults.Empty() || !_bus->staticImportShaderVariantResults.Empty() ||
			std::ranges::any_of(_extensions, [](const auto* extension) { return extension->HasCoordinatorResults(); }) ||
			!_bus->staticImportMaterializedValidationRequests.Empty() || !_bus->staticImportTemplateRefsPublished.Empty();
	}

}
