#pragma once

// Shared state for the static-import pipeline.
//
// The coordinator, the worker pool, the streaming pump and the template stage all
// read and write this state; it is not owned by any one of them, so each takes a
// reference to this object (the runtime owns it); the same arrangement as
// StaticImportWorkBus, which carries the queues that connect the same stages.

#include <array>
#include <BasicRenderer/Streaming/ObjectRequests.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <BasicRenderer/StaticImport/Advanced/StaticImportWorkScheduler.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportMessages.h>
#include <BasicRenderer/StaticImport/StaticSceneRecipe.h>

namespace br::staticimport
{

	// The coordinator's desired groups and their current revisions, mirrored for
	// the stages that check liveness off the coordinator thread (batch trimming,
	// materialized decisions, ownership applied on the streaming pump). The
	// coordinator is the only writer and updates it before it acts on a change, so
	// a revision is listed here before any of its work can be published and is gone
	// before its removal is queued. Sharded by group id like the lifecycle store.
	class StaticDesiredGroupRevisions
	{
	public:
		void Set(std::uint64_t groupID, std::uint64_t revision)
		{
			auto& shard = Select(groupID);
			std::lock_guard lock(shard.mutex);
			shard.revisions.insert_or_assign(groupID, revision);
		}

		void Erase(std::uint64_t groupID)
		{
			auto& shard = Select(groupID);
			std::lock_guard lock(shard.mutex);
			shard.revisions.erase(groupID);
		}

		// Whether `groupID` is desired at exactly `revision`.
		[[nodiscard]] bool IsDesired(std::uint64_t groupID, std::uint64_t revision) const
		{
			const auto& shard = Select(groupID);
			std::lock_guard lock(shard.mutex);
			const auto found = shard.revisions.find(groupID);
			return found != shard.revisions.end() && found->second == revision;
		}

	private:
		static constexpr std::size_t kShardCount = 64;

		struct Shard
		{
			mutable std::mutex mutex;
			std::unordered_map<std::uint64_t, std::uint64_t> revisions;
		};

		[[nodiscard]] Shard& Select(std::uint64_t groupID)
		{
			return _shards[static_cast<std::size_t>(groupID ^ (groupID >> 32)) & (kShardCount - 1u)];
		}

		[[nodiscard]] const Shard& Select(std::uint64_t groupID) const
		{
			return _shards[static_cast<std::size_t>(groupID ^ (groupID >> 32)) & (kShardCount - 1u)];
		}

		std::array<Shard, kShardCount> _shards;
	};

	struct StaticImportPipelineState
	{
		// Async template graph: identities and fingerprints resolved under one lock.
		std::mutex staticTemplateGraphMutex;
		std::unordered_map<DirectStaticTemplateKey, StaticTemplateExternalCompletion,
			DirectStaticTemplateKeyHash> staticTemplateGraphCompletions;
		// Mutable loader objects remain behind the ingestion owner. Async graph inputs
		// carry only an immutable identity/fingerprint and resolve the exact retained
		// source while holding the same lock as completion state.
		std::unordered_map<DirectStaticTemplateKey, StaticTemplateRequest,
			DirectStaticTemplateKeyHash> staticTemplateGraphSources;
		std::function<void(std::uint64_t)> staticTemplateSuspensionNotifier;
		std::function<void(std::uint64_t)> staticGroupSuspensionNotifier;
		std::shared_ptr<const StaticTemplateRefMap> staticTemplateGraphSnapshot;
		StaticTemplateFailureSnapshot staticTemplateGraphFailedSnapshot;
		std::atomic<std::uint64_t> staticTemplateDefensiveDuplicates{ 0 };
		bool staticGroupProducerRegistered{ false };
		bool staticTemplateProducerRegistered{ false };
		bool staticVariantProducerRegistered{ false };

		// Template publication backoff, and the ids of templates' material admissions.
		std::atomic_bool staticTemplatePublicationNeedsBackoff{ false };
		std::atomic_bool staticTemplatePublicationRetryScheduled{ false };
		std::atomic<std::uint64_t> nextStaticTemplateMaterialBatchID{ 0 };

		// Static scene recipes: the store, persistence gating and variant bindings.
		std::unique_ptr<br::staticimport::recipe::Store> staticSceneRecipeStore;
		std::atomic_bool staticSceneRecipePersistenceScheduled{ false };
		mutable std::mutex staticSceneRecipePersistenceStateMutex;
		std::unordered_set<std::uint64_t> staticSceneRecipePersistenceDisabledNamespaces;
		mutable std::shared_mutex staticRecipeVariantBindingMutex;
		std::array<std::mutex, 64> staticRecipeVariantBuildMutexes;
		std::unordered_map<StaticRecipeVariantKey,
			std::shared_ptr<const StaticRecipeVariantBinding>, StaticRecipeVariantKeyHash,
			StaticRecipeVariantKeyEqual> staticRecipeVariantBindings;
		std::mutex staticSceneRecipeCompileMutex;
		std::unordered_set<StaticSceneRecipeCompileKey, StaticSceneRecipeCompileKeyHash>
			staticSceneRecipeCompiles;

		// Group retirement awaiting publication.
		mutable std::mutex staticRetirementMutex;
		std::deque<PendingStaticRetirement> staticRetirementsAwaitingPublication;
		std::exception_ptr staticRetirementFailure;

		// Publication admission, GPU readiness and the cleanup backlogs that trail it.
		std::deque<StaticImportGroupAck> staticImportGroupAckBacklog;
		std::deque<StaticImportTransactionPrepareJob> staticImportTransactionMaterializeBacklog;
		std::vector<PendingStaticImportPublication> staticImportGpuPendingPublications;
		std::atomic_bool staticImportPublicationAdmissionPending{ false };
		std::atomic<std::uint64_t> staticImportPublicationRequiredCoverage{ 0 };
		std::atomic<std::uint64_t> staticImportPublicationObservedCoverage{ 0 };
		std::optional<StaticImportPublicationResult> staticImportPublicationPendingAdmission;
		// A queued job that did not fit in the previous merged cut. Guarded by
		// staticImportPublicationWorkerMutex; the flag lets other threads count it.
		std::optional<StaticImportPublicationJob> staticImportPublicationCarryJob;
		std::atomic_bool staticImportPublicationCarryPending{ false };
		std::mutex staticImportPublicationWorkerMutex;
		std::deque<PreparedDirectStaticImportBatch> staticImportResourcePendingBatchBacklog;
		std::list<StaticImportMaterializedPublishDecision> staticImportMaterializedDecisionBacklog;
		std::list<StaticImportMaterializedPublishDecision> staticImportMaterialGraphPendingDecisions;
		std::list<StaticImportMaterializedPublishDecision> staticImportMaterializedDecisionCleanupBacklog;
		std::vector<br::render::StaticObjectRemovalPayload> staticImportRemovalPayloadCleanupBacklog;

		// Import service: camera targets, worker scheduling and liveness.
		mutable std::mutex staticImportServiceMutex;
		mutable std::mutex staticImportServiceCameraMutex;
		mutable std::mutex staticImportServiceStatsMutex;
		std::mutex staticImportSpeculativeAssetPublishMutex;
		std::optional<StaticImportCameraTarget> staticImportServiceLastPublishedTarget;
		StaticImportCameraTarget staticImportServicePendingTarget;
		std::atomic_bool staticImportServiceHasPendingTarget{ false };
		bool staticImportServiceWorkersStarted{ false };
		std::atomic<std::uint64_t> staticImportServiceTargetGeneration{ 0 };
		StaticImportServiceStats staticImportServicePublishedStats;
		std::atomic<std::uint32_t> staticImportWorkerDrainsActive{ 0 };
		std::array<std::atomic<std::uint32_t>, StaticImportWorkScheduler::SourceCount>
			staticImportSourceDrainsScheduled{};
		std::atomic<std::size_t> staticImportSourceScheduleCursor{ 0 };
		std::uint32_t staticImportWorkerConcurrency{ 1 };
		bool staticImportCoordinatorPublishedQuiescent{ false };
		std::atomic<std::uint64_t> staticImportCoordinatorLivenessEpoch{ 0 };
		StaticDesiredGroupRevisions staticDesiredGroupRevisions;
		std::atomic<std::uint64_t> staticImportAssetReadinessGeneration{ 0 };
		std::atomic<std::uint64_t> graphDesiredPlacementSetDigest{ 0 };
		std::shared_ptr<StaticImportCallbackHandoff> staticImportCallbackHandoff{
			std::make_shared<StaticImportCallbackHandoff>() };

		// Telemetry counters.
		bool benchmarkDiagnosticsEnabled{ false };
		std::atomic<std::uint64_t> staticTransactionReadyEventsPublished{ 0 };
		std::atomic<std::uint64_t> staticTransactionReadyGroupsPublished{ 0 };
		std::atomic<std::uint64_t> staticTransactionReadyEventsAccepted{ 0 };
		std::atomic<std::uint64_t> staticTransactionReadyEventsRejected{ 0 };
		std::atomic<std::uint64_t> staticRemovalTelemetryLogCount{ 0 };

		// Live streaming state shared by the pump and the host: applied placement
		// and active-group counts, workload tallies, and the last stream key/log.
		std::size_t staticAppliedPlacementCount{ 0 };
		std::size_t staticRuntimeGroupActiveCount{ 0 };
		std::unordered_map<DrawWorkloadKey, std::uint64_t, DrawWorkloadKey::Hasher> directStaticWorkloadCounts;
		std::chrono::steady_clock::time_point lastStaticStreamLog{};
		std::optional<StaticStreamKey> lastStaticStreamKey;
		std::unordered_map<std::uint64_t, std::uint32_t> directStaticPrepareRetries;
	};
}
