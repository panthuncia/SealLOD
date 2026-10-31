#pragma once

// The coordinator's own state: the cells and groups it tracks, the graph ownership
// it publishes from, its dependency waiters and template references. Only the
// coordinator reads or writes it, on the coordinator thread.

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <BasicRenderer/StaticImport/Advanced/StaticImportMessages.h>

namespace br::staticimport
{
	// A group in the coordinator's desired set: the revision of its current add
	// and the batch key it was added under.
	struct StaticDesiredGroup
	{
		std::uint64_t revision{ 0 };
		std::uint64_t batchKey{ 0 };
	};

	struct StaticImportCoordinatorState
	{
		StaticImportCoordinatorState()
		{
			templateRefs = std::make_shared<StaticTemplateRefMap>();
			templateSnapshot = templateRefs;
		}

		StaticImportServiceStats stats;
		std::optional<StaticStreamKey> lastKey;
		StaticDesiredCellArea desiredCells;
		std::unordered_map<StaticCellKey, std::vector<std::uint64_t>, StaticCellKeyHash> activeCellGroups;
		std::deque<StaticImportCellPrepareJob> pendingCellQueue;
		std::unordered_set<StaticCellKey, StaticCellKeyHash> pendingCellKeys;
		// The flat group intake (ApplyGroupChanges): every cell-static group that is
		// wanted, at its current revision. Host-resident groups keep their extension
		// liveness and are not listed. Mirrored to staticDesiredGroupRevisions.
		std::unordered_map<std::uint64_t, StaticDesiredGroup> desiredGroups;
		// The next group revision. One counter for every id keeps each id's revisions
		// strictly increasing across adds and removals without remembering removed ids.
		std::uint64_t nextGroupRevision{ 1 };
		std::unordered_map<std::uint64_t, std::shared_ptr<StaticGroupLifecycleRecord>> pendingGroups;
		std::deque<std::uint64_t> pendingGroupQueue;
		// The prefix is variant-sorted and can be consumed without repeatedly
		// sorting groups appended by concurrently completing cell preparation.
		// Once it is exhausted, the accumulated tail becomes the next cohort.
		std::size_t pendingGroupSortedPrefix{ 0 };
		std::unordered_set<std::uint64_t> pendingQueued;
		std::unordered_set<std::uint64_t> workerInFlightGroups;
		// Groups whose producer invocation is queued or currently executing.
		// Suspended graph versions remain in workerInFlightGroups but release this
		// admission slot so dependency work cannot sit behind an unbounded flood.
		std::unordered_set<std::uint64_t> producerActiveGroups;
		std::unordered_map<std::uint64_t, std::uint64_t> groupArtifactRevisions;
		std::unordered_map<std::uint64_t, br::render::ArtifactVersionHandle> packetArtifactVersions;
		std::unordered_map<br::render::ArtifactAddress, br::render::ArtifactVersionHandle,
			br::render::ArtifactAddress::Hasher> variantArtifactVersions;
		// Terminal variant failures are durable for their source fingerprint. A
		// radius load prepares cells concurrently, so additional groups using a
		// failed variant can arrive after its one graph terminal notification.
		std::unordered_set<br::render::ArtifactAddress,
			br::render::ArtifactAddress::Hasher> failedVariantAddresses;
		std::unordered_map<std::uint64_t, std::uint64_t> groupPacketIDs;
		std::unordered_map<std::uint64_t, std::size_t> packetLiveMemberCounts;
		std::unordered_map<std::uint64_t, std::uint64_t> packetPreparationCredits;
		std::uint64_t activePreparationCredits{ 0 };
		std::unordered_map<std::uint64_t, std::uint64_t> preparedBytesByPublishTicket;
		std::uint64_t preparedUnpublishedBytes{ 0 };
		std::unordered_map<std::uint64_t, std::uint64_t> materializedBytesByPublishTicket;
		std::uint64_t materializedUnpublishedBytes{ 0 };
		std::unordered_map<std::uint64_t, std::shared_ptr<StaticGroupIntentToken>> groupIntentTokens;
		std::unordered_map<std::uint64_t, std::shared_ptr<const StaticGroupPreparationCheckpoint>> groupCheckpoints;
		std::unordered_map<std::uint64_t, StaticGroupSuspensionState> groupSuspensions;
		std::unordered_map<StaticImportDependencyKey, std::unordered_set<std::uint64_t>,
			StaticImportDependencyKeyHash> graphDependencyWaiters;
		std::unordered_map<std::uint64_t, std::string_view> pendingGroupLastTransition;
		std::unordered_set<std::uint64_t> readyGroups;
		std::unordered_map<std::uint64_t, std::uint64_t> readyGroupTickets;
		// Authoritative graph ownership for groups accepted by the bridge. This
		// is active publication state, not a migration/shadow copy: removals,
		// placement convergence and StaticScene roots are derived from it.
		std::unordered_set<std::uint64_t> graphOwnedActiveGroups;
		std::unordered_map<std::uint64_t, br::render::ArtifactVersionID> graphOwnedGroupVersions;
		std::unordered_map<std::uint64_t, std::uint64_t> graphOwnedGroupPlacementCounts;
		// The group revision each accepted group was published from.
		std::unordered_map<std::uint64_t, std::uint64_t> graphOwnedGroupRevisions;
		std::array<std::unordered_map<std::uint64_t, br::render::ArtifactVersionID>,
			br::render::kStaticScenePageCount> graphOwnedPageOwners;
		std::array<std::unordered_map<std::uint64_t, br::render::ArtifactVersionID>,
			br::render::kStaticScenePageCount> graphOwnedPendingPageOwners;
		std::array<std::unordered_set<std::uint64_t>,
			br::render::kStaticScenePageCount> graphOwnedPendingPageRemovals;
		std::array<std::uint64_t, br::render::kStaticScenePageCount> graphOwnedPageRevisions{};
		std::array<br::render::ArtifactVersionHandle,
			br::render::kStaticScenePageCount> graphOwnedDesiredPages{};
		std::array<br::render::ArtifactSnapshot,
			br::render::kStaticScenePageCount> graphOwnedReadyPages{};
		std::array<br::render::ArtifactAwaiter,
			br::render::kStaticScenePageCount> graphOwnedPageAwaiters{};
		std::array<br::render::ArtifactVersionID,
			br::render::kStaticScenePageCount> graphOwnedPublishedPages{};
		std::shared_ptr<const br::render::PublishedStaticSceneState> graphOwnedPublishedScene;
		std::unordered_set<std::uint32_t> graphOwnedDirtyPages;
		std::uint64_t graphOwnedDesiredPlacementCount{ 0 };
		std::uint64_t graphOwnedDesiredGroupDigest{ 0 };
		std::uint64_t graphOwnedSceneRevision{ 0 };
		std::uint64_t graphOwnedLastSceneFingerprint{ 0 };
		// Geometry mutation sequence covered by the last submitted scene cut.
		std::uint64_t graphOwnedLastSceneGeometryCoverage{ 0 };
		br::render::ArtifactVersionHandle graphOwnedSceneInFlight{};
		br::render::ArtifactAwaiter graphOwnedSceneAwaiter;
		std::size_t graphOwnedLastSceneGroupCount{ 0 };
		std::size_t graphOwnedMembershipChangesSinceCheckpoint{ 0 };
		std::chrono::steady_clock::time_point graphOwnedDirtySince{};
		std::chrono::steady_clock::time_point graphOwnedLastSceneDiagnostic{};
		std::uint64_t nextPublishTicketID{ 1 };
		std::uint64_t nextPreparationPacketID{ 1 };
		// Updated from completed preparation packets. The initial estimate targets
		// 32 groups per millisecond and converges without a tuning-only worker cap.
		std::uint64_t preparationNanosecondsPerGroup{ 31'250 };
		std::unordered_map<std::uint64_t, std::uint64_t> capacityHintDrawRecordsByGroup;
		std::unordered_set<std::uint64_t> capacityHintCountedGroups;
		std::uint64_t capacityHintTransformRows{ 0 };
		std::uint64_t capacityHintDrawRecords{ 0 };
		std::uint64_t livenessEpoch{ 1 };
		std::uint64_t observedAssetReadinessGeneration{ 0 };
		// One extension's cells blocked on dependencies (the host cell hooks).
		struct HostCellBlocks
		{
			std::unordered_map<StaticCellKey, std::unordered_set<StaticImportDependencyKey, StaticImportDependencyKeyHash>, StaticCellKeyHash> blockedCells;
			std::unordered_map<StaticImportDependencyKey, std::unordered_set<StaticCellKey, StaticCellKeyHash>, StaticImportDependencyKeyHash> dependencyWaiters;
		};
		// Indexed like the registered extensions.
		std::vector<HostCellBlocks> hostCellBlocks;
		std::unordered_map<StaticImportDependencyKey, StaticImportDependencyRequest, StaticImportDependencyKeyHash> pendingDependencyRequests;
		std::unordered_map<StaticImportDependencyKey, DelayedStaticImportDependencyRequest, StaticImportDependencyKeyHash> delayedDependencyRequests;
		std::shared_ptr<StaticTemplateRefMap> templateRefs;
		StaticTemplateRefSnapshot templateSnapshot;
		std::unordered_set<DirectStaticTemplateKey, DirectStaticTemplateKeyHash> pendingTemplateKeys;
		std::unordered_set<DirectStaticTemplateKey, DirectStaticTemplateKeyHash> failedTemplateKeys;
		std::unordered_map<DirectStaticTemplateKey, br::render::ArtifactVersionHandle,
			DirectStaticTemplateKeyHash> templateArtifactVersions;
		std::unordered_map<DirectStaticTemplateKey, br::render::ArtifactAwaiter,
			DirectStaticTemplateKeyHash> templateArtifactAwaiters;
		std::unordered_set<std::string> pendingAssetPublishKeys;
		std::unordered_set<std::string> pendingMaterialVariantKeys;
		std::unordered_set<std::string> pendingShaderVariantKeys;
		StaticImportCameraTarget latestTarget;
		std::chrono::steady_clock::time_point lastStatsPublish{};
		std::chrono::steady_clock::time_point lastBlockedHostCellDiagnostic{};
		std::chrono::steady_clock::time_point lastPendingGroupDiagnostic{};
	};
}
