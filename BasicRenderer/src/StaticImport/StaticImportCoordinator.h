#pragma once

// The static-import coordinator.
//
// Owns the admission side of static streaming: consumes camera targets, schedules
// cells and groups, resolves asset/material/shader/host dependencies, and drains the
// results the worker pool publishes back. Drain is its single entry point; the public
// host-cell, dependency and host-import operations below back the extension access
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
		struct Hooks
		{
			std::function<StaticDesiredCellArea(const br::staticimport::StaticStreamTarget&)> buildDesiredStaticCells;
		};

		// Collaborators arrive at construction: there is no window in which this
		// object exists unbound, and a missing one is a compile error.
		StaticImportCoordinator(const StaticStreamingContext& ctx, StaticGroupLifecycleStore& lifecycle, StaticImportCapacityBroker& capacity, br::SerializedTaskPump& pump, Hooks hooks);

		void Drain();

		// ---- Extension operations (coordinator thread; see IStaticImportAdvancedAccess) ----

		[[nodiscard]] const StaticImportCameraTarget& LatestTarget() const { return _state.latestTarget; }

		[[nodiscard]] StaticTemplateRefSnapshot TemplateSnapshot() const { return _state.templateSnapshot; }

		StaticHostCellBlockResult BlockHostCell(const br::staticimport::IStaticImportExtension& owner, const StaticCellKey& cell, std::vector<StaticImportDependencyRequest> dependencies);

		[[nodiscard]] bool IsHostCellBlocked(const br::staticimport::IStaticImportExtension& owner, const StaticCellKey& cell) const;

		std::optional<StaticImportDependencyRequest> TakePendingDependency(const StaticImportDependencyKey& key);

		void SettleDependency(StaticImportDependencyRequest request, StaticImportDependencyResultStatus status, std::chrono::steady_clock::duration retryDelay);

		void SubmitHostImportBatch(PreparedDirectStaticImportBatch batch);

		// Replaces the registered extensions. Only while the pipeline has not started:
		// the coordinator sizes its per-extension state from the list.
		void SetExtensions(std::span<br::staticimport::IStaticImportExtension* const> extensions);

	private:
		using HostCellBlocks = StaticImportCoordinatorState::HostCellBlocks;

		[[nodiscard]] std::size_t ExtensionIndex(const br::staticimport::IStaticImportExtension& extension) const;

		static void ClearHostCellBlock(HostCellBlocks& blocks, const StaticCellKey& cell);

		// Whether any extension's blocked cell waits on `key`.
		[[nodiscard]] bool HasHostCellWaiters(const StaticImportDependencyKey& key) const;

		[[nodiscard]] std::size_t BlockedHostCellCount() const;

		// Drops every extension's coordinator state for `cell` (left the desired area).
		void DropHostCell(const StaticCellKey& cell);

		// Drops blocked host cells their extensions no longer want for `target`.
		void DropUnwantedHostCells(const StaticImportCameraTarget& target);
		template <class Fn> void ForEachRealStaticCellInAreaDifference( const StaticDesiredCellArea& area, const StaticDesiredCellArea& excluded, Fn&& fn) const
		{
			if (StaticDesiredCellAreaEquals(area, excluded)) {
				return;
			}
			if (area.kind == StaticDesiredCellAreaKind::Interior) {
				if (!excluded.contains(area.interior) && _placements->IsRealCell(area.interior)) {
					fn(area.interior);
				}
				return;
			}
			if (area.kind != StaticDesiredCellAreaKind::ExteriorRect) {
				return;
			}
			_placements->ForEachExteriorCell(area.exterior.worldFormID, [&](const StaticCellKey& key) {
				if (area.contains(key) && !excluded.contains(key)) {
					fn(key);
				}
			});
		}

		bool ConsumeCameraTarget(StaticImportCameraTarget& target);

		bool IsDependencyResolved(const StaticImportDependencyRequest& request) const;

		bool IsDependencyFailed(const StaticImportDependencyRequest& request) const;

		void MarkStaticImportCoordinatorLivenessChanged()
		{
			const auto epoch = ++_state.livenessEpoch;
			_importState->staticImportCoordinatorLivenessEpoch.store(epoch, std::memory_order_release);
		}

		// Every extension's IsHostGroupCurrent for the latest target.
		bool IsHostGroupCurrent(const StaticCellKey& cell, std::uint64_t residencyGeneration) const;

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

		bool QueueCellJob(const StaticCellKey& cell, std::uint64_t generation, std::shared_ptr<br::staticimport::recipe::CellRecipeView> mappedRecipe = {}, std::shared_ptr<StaticRecipeSceneBinding> recipeSceneBinding = {}, std::shared_ptr<br::staticimport::recipe::MappedRecipePack> mappedRecipePack = {});

		// The flat group intake. Removes `removes` from the desired set, each under a
		// new revision, and queues their retirement; then seals `adds` (records
		// built by BuildPreparedStaticCellLifecycle) under new revisions, makes the
		// cell-static ones desired and feeds them to pendingGroups. Adding a desired
		// id replaces its current add. Removing an id that is not desired does
		// nothing. Cell streaming is a client: prepared cells add their groups, left
		// cells remove them.
		void ApplyGroupChanges(std::span<const std::shared_ptr<StaticGroupLifecycleRecord>> adds, std::span<const std::uint64_t> removes);

		void ProcessTarget(StaticImportCameraTarget target);

		bool DrainTemplateRefs();

		bool SubmitDirtyStaticScenePages(br::render::RendererStateRequestService& requests);

		bool TrySubmitReadyStaticScene(br::render::RendererStateRequestService& requests);

		bool DrainGroupAcks();

		bool DrainPreparedCells();

		// Host group changes (StaticImportRuntime::AddOrUpdateGroups / RemoveGroups),
		// through ApplyGroupChanges in submission order. Taken only once the workers
		// started.
		bool DrainGroupChanges();

		[[nodiscard]] bool HasPendingGroupChanges() const
		{
			return _importState->staticImportServiceWorkersStarted.load(std::memory_order_acquire) &&
				!_bus->staticImportGroupChanges.Empty();
		}


		bool ScheduleCells();

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
		const br::staticimport::IStaticPlacementSource* _placements;
		std::span<br::staticimport::IStaticImportExtension* const> _extensions;
		StaticGroupLifecycleStore* _lifecycle;
		StaticImportCapacityBroker* _capacity;
		br::SerializedTaskPump* _pump;
		Hooks _hooks;
	};
}
