#pragma once

// The static-import streaming pump.
//
// The publish side of static streaming: it drains the ready diffs, template requests
// and removals that the worker pool produces, trims them against the cells still
// desired, and applies the result to the scene. UpdateStaticStreaming is the per-frame
// entry point.

#include <algorithm>
#include <BasicRenderer/Streaming/ObjectRequests.h>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <BasicRenderer/Streaming/StaticObjectRequestService.h>
#include <BasicTelemetry/Tracy.h>

#include <BasicRenderer/StaticImport/StaticImportShared.h>

#include <BasicRenderer/StaticImport/Advanced/StaticImportSupport.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportCapacityBroker.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportIdentity.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportMessages.h>
#include <BasicRenderer/StaticImport/StaticStreamCounters.h>
#include "StaticImport/StaticImportCoordinator.h"
#include "StaticImport/StaticImportDrainScheduler.h"
#include "StaticImport/StaticGroupLifecycleStore.h"
#include "StaticImport/StaticImportPipelineState.h"
#include "StaticImport/StaticImportWorkBus.h"
#include "StaticImport/StaticStreamingContext.h"

namespace br::staticimport
{


	class StaticImportStreamingPump
	{
	public:
		struct Hooks
		{
			std::function<bool()> ensureStaticWorldSnapshot;
			std::function<std::size_t()> liveStaticObjectCount;
			// The host's object counters, reported in the periodic status log.
			std::function<StaticHostObjectCounters()> hostObjectCounters;
		};

		// Collaborators arrive at construction: there is no window in which this
		// object exists unbound, and a missing one is a compile error.
		StaticImportStreamingPump(const StaticStreamingContext& ctx, StaticImportCoordinator& coordinator, StaticGroupLifecycleStore& lifecycle, StaticImportCapacityBroker& capacity, Hooks hooks);

		StaticStreamBudget MakeStaticStreamBudget() const;

		bool DirectStaticImportAvailable() const
		{
			return _shared->hasSceneIngestionServices &&
				_shared->sceneIngestionServices.objects != nullptr &&
				_shared->sceneIngestionServices.meshes != nullptr &&
				_shared->sceneIngestionServices.materials != nullptr &&
				_shared->sceneIngestionServices.assetRequests != nullptr;
		}

		StaticDesiredCellArea BuildDesiredStaticCells(const br::staticimport::StaticStreamTarget& target) const;

		// Replaces the registered extensions. Only while the pipeline has not started.
		void SetExtensions(std::span<br::staticimport::IStaticImportExtension* const> extensions);

		void ScheduleStaticTemplatePublicationRetry();

		void UpdateStaticStreaming(const br::staticimport::StaticStreamTarget& target);

		void DrainTemplateRequests(const StaticStreamBudget& budget);

		StaticImportServiceStats GetStatsSnapshot() const;

		std::uint64_t StaticPreparationQueueDepth() const;

		br::render::ObjectStorageStats CaptureObjectManagerStats() const
		{
			if (!_shared->hasSceneIngestionServices || !_shared->sceneIngestionServices.objects) {
				return {};
			}
			return _shared->sceneIngestionServices.objects->GetStats();
		}

	private:
		// Every extension's IsMaterialReadyForTemplatePublication.
		bool IsMaterialReadyForTemplatePublication(const Material& material) const;

		void FlushDirectStaticWorkloadCountUpdates( const std::unordered_map<DrawWorkloadKey, std::uint32_t, DrawWorkloadKey::Hasher>& dirtyWorkloadSpans);

		// Drops the batch's imports whose group is no longer desired at their revision
		// (host imports and host groups ask their owners). Returns whether any remain.
		static bool TrimPreparedBatchToDesiredGroups( PreparedDirectStaticImportBatch& batch, const StaticDesiredGroupRevisions& desiredGroups, const br::staticimport::StaticStreamTarget& target, std::uint64_t generation, std::vector<StaticImportGroupAck>* droppedAcks);

		void CompleteStaticTemplateArtifacts(StaticTemplateRefsPublished packet);

		void QueueMaterializedDecisionCleanup(std::list<StaticImportMaterializedPublishDecision>& decisions);

		bool FlushGroupAckBacklog();

		void QueueStaticImportGroupAckForCoordinator(StaticImportGroupAck ack)
		{
			if (ack.unpublished.empty() &&
				ack.requeue.empty() && ack.dropped.empty()) {
				return;
			}
			_importState->staticImportGroupAckBacklog.push_back(std::move(ack));
			(void)FlushGroupAckBacklog();
		}

		bool FlushTransactionMaterializeBacklog();

		bool QueueStaticImportTransactionMaterializeJob(StaticImportTransactionPrepareJob job)
		{
			_importState->staticImportTransactionMaterializeBacklog.push_back(std::move(job));
			(void)FlushTransactionMaterializeBacklog();
			return true;
		}

		void EnsureWorkersStarted();

		void UpdateCameraTarget(const br::staticimport::StaticStreamTarget& target);

		void UpdateStaticStreamingDirectImportService( const br::staticimport::StaticStreamTarget& target, std::chrono::steady_clock::time_point updateBegin);

		void DrainRemovals( const StaticStreamBudget& budget, const StaticDesiredCellArea& currentDesiredCells, const br::staticimport::StaticStreamTarget& target, std::unordered_map<DrawWorkloadKey, std::uint32_t, DrawWorkloadKey::Hasher>* dirtyWorkloadSpans = nullptr);

		void DrainReadyDiffs( std::size_t& applied, std::size_t& pendingOrSkipped, std::size_t& upsertsThisFrame, const StaticStreamBudget& budget, const StaticDesiredCellArea& currentDesiredCells, const br::staticimport::StaticStreamTarget& target, std::unordered_map<DrawWorkloadKey, std::uint32_t, DrawWorkloadKey::Hasher>* dirtyWorkloadSpans = nullptr);

		void LogStaticStreamStatus(const br::staticimport::StaticStreamTarget& target);

		StaticImportShared* _shared;
		StaticImportWorkBus* _bus;
		StaticImportPipelineState* _importState;
		StaticImportDrainScheduler* _drains;
		StaticStreamCounters* _counters;
		const br::staticimport::StaticImportSettings* _settings;
		std::span<br::staticimport::IStaticImportExtension* const> _extensions;
		StaticImportCoordinator* _coordinator;
		StaticGroupLifecycleStore* _lifecycle;
		StaticImportCapacityBroker* _capacity;
		Hooks _hooks;
	};
}
