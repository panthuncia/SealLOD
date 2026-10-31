#pragma once

// The static-import coordinator.
//
// Owns the admission side of static streaming: applies the host's group changes,
// schedules groups, resolves asset/material/shader/host dependencies, and drains the
// results the worker pool publishes back. Drain is its single entry point; the public
// host-key, dependency and host-import operations below back the extension access
// surface and run inside that tick (extension coordinator hooks).
//
// Serialization: the drain is serialized by the coordinator pump, not by a mutex.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <BasicRenderer/Streaming/SerializedTaskPump.h>
#include <BasicTelemetry/Tracy.h>
#include <BasicRenderer/StaticImport/StaticImportShared.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportSupport.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportAdvancedAccess.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportCapacityBroker.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportMessages.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportIdentity.h>
#include "StaticImport/StaticGroupLifecycleStore.h"
#include "StaticImport/StaticImportPipelineState.h"
#include "StaticImport/StaticImportWorkBus.h"
#include "StaticImport/StaticImportDrainScheduler.h"
#include "StaticImport/StaticStreamingContext.h"
#include "StaticImport/StaticImportCoordinatorState.h"

namespace br::staticimport
{



	class StaticImportCoordinator
	{
	public:
		// Collaborators arrive at construction: there is no window in which this
		// object exists unbound, and a missing one is a compile error.
		StaticImportCoordinator(const StaticStreamingContext& ctx, StaticGroupLifecycleStore& lifecycle, StaticImportCapacityBroker& capacity, br::SerializedTaskPump& pump);

		void Drain();

		// ---- Extension operations (coordinator thread; see IStaticImportAdvancedAccess) ----

		[[nodiscard]] StaticTemplateRefSnapshot TemplateSnapshot() const { return _state.templateSnapshot; }

		StaticHostKeyBlockResult BlockHostKey(const br::staticimport::IStaticImportExtension& owner, std::uint64_t hostKey, std::vector<StaticImportDependencyRequest> dependencies);

		void UnblockHostKey(const br::staticimport::IStaticImportExtension& owner, std::uint64_t hostKey);

		[[nodiscard]] bool IsHostKeyBlocked(const br::staticimport::IStaticImportExtension& owner, std::uint64_t hostKey) const;

		std::optional<StaticImportDependencyRequest> TakePendingDependency(const StaticImportDependencyKey& key);

		void SettleDependency(StaticImportDependencyRequest request, StaticImportDependencyResultStatus status, std::chrono::steady_clock::duration retryDelay);

		void SubmitHostImportBatch(PreparedDirectStaticImportBatch batch);

		// Replaces the registered extensions. Only while the pipeline has not started:
		// the coordinator sizes its per-extension state from the list.
		void SetExtensions(std::span<br::staticimport::IStaticImportExtension* const> extensions);

	private:
		using HostKeyBlocks = StaticImportCoordinatorState::HostKeyBlocks;

		[[nodiscard]] std::size_t ExtensionIndex(const br::staticimport::IStaticImportExtension& extension) const;

		static void ClearHostKeyBlock(HostKeyBlocks& blocks, std::uint64_t hostKey);

		// Whether any extension's blocked host key waits on `key`.
		[[nodiscard]] bool HasHostKeyWaiters(const StaticImportDependencyKey& key) const;

		[[nodiscard]] std::size_t BlockedHostKeyCount() const;

		// Takes a host liveness change (StaticImportRuntime::NotifyHostLivenessChanged):
		// adopts the new generation and re-checks the pending host groups. Returns
		// whether there was one.
		bool ConsumeHostLivenessChange();

		bool IsDependencyResolved(const StaticImportDependencyRequest& request) const;

		bool IsDependencyFailed(const StaticImportDependencyRequest& request) const;

		void MarkStaticImportCoordinatorLivenessChanged()
		{
			const auto epoch = ++_state.livenessEpoch;
			_importState->staticImportCoordinatorLivenessEpoch.store(epoch, std::memory_order_release);
		}

		// Every extension's IsHostGroupCurrent.
		bool IsHostGroupCurrent(std::uint64_t groupID, std::uint64_t residencyGeneration) const;

		// Removes pending host-resident groups that are no longer current, and with
		// `requeueLive` queues the current ones again.
		void DropStaleHostGroups(bool requeueLive);

		bool IsGroupLive(const StaticGroupLifecycleRecord& record) const;

		bool IsPreparedLive(const PreparedDirectStaticImport& prepared) const;

		void ClearGroupBlockers(std::uint64_t groupID);

		void ReleasePreparedTicket(std::uint64_t ticketID);

		void TransferPreparedTicketToMaterialized(std::uint64_t ticketID);

		bool RemoveGroup(std::uint64_t groupID, StaticGroupLifecycleStage terminalStage = StaticGroupLifecycleStage::Cancelled, std::vector<std::shared_ptr<StaticGroupLifecycleRecord>>* deferredReleases = nullptr);

		void RefreshGraphCapacityHint();

		bool QueueGroupIfRunnable(std::uint64_t groupID);

		bool SubmitDependencyRequest(StaticImportDependencyRequest request);

		void ResolveDependency(const StaticImportDependencyKey& key);

		void FailDependency(const StaticImportDependencyKey& key);


		bool PumpDueDependencyRequests();

		bool PromoteNotifiedAssetDependencies();

		bool ReconcileOrphanedAssetDependencies();

		std::optional<std::chrono::steady_clock::time_point> NextDependencyPollDue() const;

		void RefreshBlockedStats();

		void PublishStats(bool force = false);

		// The flat group intake. Removes `removes` from the desired set, each under a
		// new revision, and queues their retirement; then seals `adds` (records
		// built by MakeStaticGroupLifecycleRecord) under new revisions, makes the
		// placed ones desired and feeds them to pendingGroups. Adding a desired id
		// replaces its current add. Removing an id that is not desired does nothing.
		void ApplyGroupChanges(std::span<const std::shared_ptr<StaticGroupLifecycleRecord>> adds, std::span<const std::uint64_t> removes);

		bool DrainTemplateRefs();

		bool SubmitDirtyStaticScenePages(br::render::RendererStateRequestService& requests);

		bool TrySubmitReadyStaticScene(br::render::RendererStateRequestService& requests);

		bool DrainGroupAcks();

		// Group changes (StaticImportRuntime::AddOrUpdateGroups / RemoveGroups /
		// AddPreparedGroups, and host groups), through ApplyGroupChanges in submission
		// order. Taken only once the workers started.
		bool DrainGroupChanges();

		[[nodiscard]] bool HasPendingGroupChanges() const
		{
			return _importState->staticImportServiceWorkersStarted.load(std::memory_order_acquire) &&
				!_bus->staticImportGroupChanges.Empty();
		}

		bool EnqueueRequest(StaticTemplateRequest request);

		bool EnqueueRequest(StaticAssetPublishRequest request)
		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportCoordinator::EnqueueAssetPublishRequest");
			if (!_state.pendingAssetPublishKeys.insert(request.modelPath).second) {
				return false;
			}
			return _drains->PushWorker(_bus->staticImportAssetPublishJobs, std::move(request),
				StaticImportWorkSource::AssetPublish);
		}

		bool EnqueueRequest(StaticMaterialVariantRequest request);

		void RequeueGroup(const DirectStaticPrepareInput& input);

		bool DrainAssetResults();

		bool DrainMaterialResults();

		bool DrainShaderVariantResults();


		bool DrainGroupResults();

		bool DrainMaterializedValidationRequests();

		bool ScheduleGroups();

		bool HasWork() const;

		bool DrainStaticGroupSuspensionRequests();

		bool HasReadyWork() const;

		StaticImportCoordinatorState _state;
		StaticImportShared* _shared;
		StaticImportWorkBus* _bus;
		StaticImportPipelineState* _importState;
		StaticImportDrainScheduler* _drains;
		const br::staticimport::StaticImportSettings* _settings;
		br::staticimport::IStaticAssetSource* _assets;
		std::span<br::staticimport::IStaticImportExtension* const> _extensions;
		StaticGroupLifecycleStore* _lifecycle;
		StaticImportCapacityBroker* _capacity;
		br::SerializedTaskPump* _pump;
	};
}
