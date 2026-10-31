#pragma once

// Extension points of the static import pipeline.
//
// The pipeline streams cell-resident static groups around a target on its own. A host
// adds behaviour around that through extensions: content whose residency it manages
// itself (host-resident groups, e.g. coarse stand-ins that cover cells until their
// real groups arrive), per-group resources it tracks while a group is owned by the
// scene, extra gates on template publication, and its own per-target work.
//
// An extension can also run import work of its own inside the pipeline: worker work on
// host work sources the pipeline schedules, dependencies the pipeline tracks next to
// its own (host dependencies), cells of its own blocked on dependencies until they
// resolve (host cells), prepared imports it publishes through the normal transaction
// path and takes back on publication (host imports), and coordinator and per-frame
// drains of its own results.
//
// Any number of extensions may be registered; every hook is called on each of them in
// registration order. All hooks default to doing nothing, so an extension overrides
// only what it needs. An extension reaches back into the pipeline only through the
// IStaticImportPipelineAccess it receives in OnAttached.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include <BasicRenderer/Pipeline/DrawWorkload.h>
#include <BasicRenderer/StaticImport/StaticCellGeometry.h>
#include <BasicRenderer/StaticImport/StaticStreamTarget.h>
#include <BasicRenderer/Streaming/ObjectRequests.h>

class Material;

namespace br::staticimport
{
	// Opaque per-group data an extension attaches to a static group (and to a recipe
	// variant binding shared by groups). The pipeline stores one per extension, keeps
	// it alive while the group is owned, and hands it back to the extension that made
	// it. Null means the extension has nothing for the group.
	using StaticGroupHold = std::shared_ptr<const void>;

	// What a group is built from, for CollectGroupHold.
	struct StaticGroupHoldSource
	{
		StaticCellKey cell;
		// When the group was bound through a recipe variant: the extension's hold
		// from DescribeVariantBinding for that variant (which may itself be null).
		// Null when the group has no variant binding.
		const StaticGroupHold* variantHold{ nullptr };
		// The group's own build infos; empty when it has a variant binding.
		std::span<const br::render::StaticGroupBuildInfo> buildInfos;
	};

	// A group whose residency an extension manages itself rather than the pipeline's
	// cell streaming. It is prepared and published through the normal import path and
	// offered back to extensions by PublishHostGroup.
	struct StaticHostGroupRequest
	{
		// The cell the group is filed under while it is prepared.
		StaticCellKey cell;
		std::uint64_t groupID{ 0 };
		std::string assetKey;
		std::string name;
		std::vector<DirectX::XMMATRIX> instanceWorldMatrices;
		// The extension's own generation for the group, echoed back on publication.
		std::uint64_t residencyGeneration{ 0 };
	};

	// A published host-resident group, offered to extensions by PublishHostGroup.
	struct StaticHostGroupPublication
	{
		std::uint64_t groupID{ 0 };
		std::uint64_t residencyGeneration{ 0 };
		// Destroys the group's renderer objects. The extension that accepts the group
		// moves it out and owns it from then on.
		br::render::StaticObjectRemovalPayload& destructionPayload;
		std::span<const DrawWorkloadKey> workloadKeys;
		std::size_t placementCount{ 0 };
	};

	enum class StaticHostGroupPublishResult : std::uint8_t
	{
		NotOwned, // Not this extension's group; offer it to the next one.
		Accepted, // The extension took ownership of the published group.
		Stale     // The extension's group, but it no longer wants it; the pipeline retires it.
	};

	class IStaticImportExtension;

	// Worker sources the pipeline schedules on behalf of extensions. Each is claimed by
	// at most one extension (IStaticImportPipelineAccess::ClaimHostWorkSource), whose
	// RunHostWork consumes the work the extension published on it. They differ only in
	// scheduling policy.
	enum class StaticHostWorkSource : std::uint8_t
	{
		// Dependency resolution: one rotating slot among the pipeline's own
		// dependency sources.
		Dependency,
		// Latency-sensitive preparation: a worker is reserved for it first on every
		// scheduling pass, it is weighted twice in fair claiming, and it runs at most
		// two consumers at once.
		Prepare,
		Count
	};

	// A dependency resolved by the extension that requested it rather than by the
	// pipeline. `key` identifies it within that extension; `payload` is the extension's
	// own data for it and is opaque to the pipeline.
	struct StaticHostDependency
	{
		std::string key;
		std::shared_ptr<void> payload;
	};

	// Opaque data an extension attaches to a prepared import it submitted (a host
	// import). The pipeline carries it with the import and hands it back to the
	// extension that made it.
	using StaticHostPayload = std::shared_ptr<const void>;

	// A published host import, handed to the extension that owns it by PublishHostImport.
	struct StaticHostImportPublication
	{
		StaticCellKey cell;
		std::uint64_t groupID{ 0 };
		const StaticHostPayload& payload;
		// Destroys the import's renderer objects. The extension moves it out and owns
		// it from then on.
		br::render::StaticObjectRemovalPayload& destructionPayload;
		std::span<const DrawWorkloadKey> workloadKeys;
	};

	// Per-cell data an extension collects while a static cell is prepared on a worker.
	struct StaticCellPrepareRequest
	{
		// Whether the extension wants OnCellHostPrepared for the cell.
		bool requested{ false };
		// The extension's own value for the cell (e.g. a content hash), echoed back.
		std::uint64_t value{ 0 };
	};

	// The narrow surface through which extensions act on the pipeline. Implemented by
	// the pipeline; valid from OnAttached until the extension is detached.
	class IStaticImportPipelineAccess
	{
	public:
		virtual ~IStaticImportPipelineAccess() = default;

		// Queues a host-resident group for preparation. Safe from any thread.
		virtual void SubmitHostGroup(StaticHostGroupRequest request) = 0;

		// Schedules a coordinator drain, e.g. after an extension queued work that
		// HasPendingCoordinatorWork reports. Safe from any thread.
		virtual void WakeCoordinator() = 0;

		// The cell area the pipeline keeps resident for a target (including every
		// extension's ExpandDesiredCellRect).
		[[nodiscard]] virtual StaticDesiredCellArea DesiredCellArea(const StaticStreamTarget& target) const = 0;

		// Records the live draw span of a workload after the extension changed the
		// visibility of its own groups.
		virtual void RecordWorkloadCount(const DrawWorkloadKey& workload, std::uint32_t span) = 0;

		// Placement accounting for host-resident groups.
		virtual void AddAppliedPlacements(std::uint64_t placements) = 0;
		virtual void ReclaimAppliedPlacements(std::uint64_t placements) = 0;

		// Retires renderer objects of host-resident groups the extension dropped.
		virtual void RetireHostGroupPayloads(std::vector<br::render::StaticObjectRemovalPayload>&& payloads) = 0;

		// Whether the pipeline is shutting down; work queued now will not be serviced.
		[[nodiscard]] virtual bool IsStopping() const = 0;

		// The generation of the latest stream target the host published.
		[[nodiscard]] virtual std::uint64_t StreamTargetGeneration() const = 0;

		// Claims a host work source for `owner`, whose RunHostWork then consumes it.
		// `taskName` names its worker tasks and must outlive the pipeline. Call from
		// OnAttached.
		virtual void ClaimHostWorkSource(IStaticImportExtension& owner, StaticHostWorkSource source, const char* taskName) = 0;

		// Announces one unit of work the claiming extension queued on `source` and
		// schedules workers for it. Call after the work entered the extension's queue,
		// so every claim corresponds to an item the consumer can observe. Safe from
		// any thread.
		virtual void PublishHostWork(StaticHostWorkSource source) = 0;

		// Whether `owner` has a dependency block on `cell` (see the host cell hooks).
		// Coordinator thread only.
		[[nodiscard]] virtual bool IsHostCellBlocked(const IStaticImportExtension& owner, const StaticCellKey& cell) const = 0;
	};

	class IStaticImportExtension
	{
	public:
		virtual ~IStaticImportExtension() = default;

		// The pipeline hands the extension its access surface once, before any other hook.
		virtual void OnAttached(IStaticImportPipelineAccess& pipeline) { (void)pipeline; }

		// ---- Stream target (the thread that publishes targets) ----

		// Streaming is inactive this frame (disabled, no valid target, or no world data).
		virtual void OnStreamTargetCleared() {}

		// Streaming is active around `target` this frame. Called before the pipeline
		// publishes the target to its coordinator.
		virtual void OnStreamTargetUpdated(const StaticStreamTarget& target) { (void)target; }

		// Grows the exterior cell rectangle kept resident for `target`, e.g. to whole
		// blocks of host content the rectangle touches. Called once per extension in
		// registration order; an extension returns its own closure. May run on any
		// pipeline thread.
		virtual void ExpandDesiredCellRect(const StaticStreamTarget& target, StaticCellRect& rect) const
		{
			(void)target;
			(void)rect;
		}

		// ---- Coordinator (serialized coordinator drain, under the publication lock) ----

		// The coordinator accepted a new target.
		virtual void ProcessStreamTarget(const StaticStreamTarget& target) { (void)target; }

		// Whether the extension has queued work for DrainCoordinatorWork. Checked on
		// every coordinator tick while a target is valid, outside the publication lock.
		[[nodiscard]] virtual bool HasPendingCoordinatorWork() const { return false; }

		// Runs the work HasPendingCoordinatorWork reported, for the latest target.
		virtual void DrainCoordinatorWork(const StaticStreamTarget& target) { (void)target; }

		// ---- Cell lifecycle notifications (any pipeline thread) ----

		// The coordinator accepted a prepared cell expected to publish `expectedGroups`.
		virtual void OnCellPrepared(const StaticCellKey& cell, std::uint64_t generation, std::uint32_t expectedGroups)
		{
			(void)cell;
			(void)generation;
			(void)expectedGroups;
		}

		// A cell-static group of `cell` became part of the published scene.
		virtual void OnCellGroupPublished(const StaticCellKey& cell, std::uint64_t generation, std::uint64_t groupID)
		{
			(void)cell;
			(void)generation;
			(void)groupID;
		}

		// The renderer finished retiring removed groups of `cell`.
		virtual void OnCellRetired(const StaticCellKey& cell) { (void)cell; }

		// ---- Group holds ----

		// Describes a recipe variant binding once, when the pipeline builds it. The
		// result is cached with the binding and passed to CollectGroupHold for every
		// group bound through it. `boundTemplates` are the variant's bound mesh templates.
		[[nodiscard]] virtual StaticGroupHold DescribeVariantBinding(
			std::span<const br::render::StaticGroupBuildInfo> boundTemplates) const
		{
			(void)boundTemplates;
			return {};
		}

		// The hold kept with a prepared group until it is retired.
		[[nodiscard]] virtual StaticGroupHold CollectGroupHold(const StaticGroupHoldSource& source) const
		{
			(void)source;
			return {};
		}

		// A cell-static group's ownership entered the published scene.
		virtual void OnGroupOwnershipAdded(std::uint64_t groupID, const StaticCellKey& cell, const StaticGroupHold& hold)
		{
			(void)groupID;
			(void)cell;
			(void)hold;
		}

		// The renderer finished retiring a cell-static group.
		virtual void OnGroupRetired(std::uint64_t groupID, const StaticGroupHold& hold)
		{
			(void)groupID;
			(void)hold;
		}

		// ---- Templates ----

		// Whether a material's dependencies allow publishing templates that use it.
		// A template is retried later while any extension returns false.
		[[nodiscard]] virtual bool IsMaterialReadyForTemplatePublication(const Material& material) const
		{
			(void)material;
			return true;
		}

		// ---- Host-resident groups ----

		// Offers a published host-resident group; see StaticHostGroupPublishResult.
		// `desiredCells` is the area the pipeline currently keeps resident.
		virtual StaticHostGroupPublishResult PublishHostGroup(
			StaticHostGroupPublication& publication,
			const StaticDesiredCellArea& desiredCells)
		{
			(void)publication;
			(void)desiredCells;
			return StaticHostGroupPublishResult::NotOwned;
		}

		// Whether a host-resident group filed under `cell` at `residencyGeneration` is
		// still wanted for `target` (an exterior target when the pipeline asks before
		// admitting work). The pipeline drops the group when any extension says no;
		// an extension answers true for groups that are not its own.
		[[nodiscard]] virtual bool IsHostGroupCurrent(
			const StaticStreamTarget& target,
			const StaticCellKey& cell,
			std::uint64_t residencyGeneration) const
		{
			(void)target;
			(void)cell;
			(void)residencyGeneration;
			return true;
		}

		// Host-resident groups the extension currently keeps live, for counts taken
		// before the pipeline has a published scene.
		[[nodiscard]] virtual std::size_t ResidentHostGroupCount() const { return 0; }

		// ---- Host work (pipeline workers) ----

		// Consumes work the extension published on a host work source it claimed.
		// Returns whether the claim was productive.
		virtual bool RunHostWork(StaticHostWorkSource source)
		{
			(void)source;
			return false;
		}

		// Queued work items and results the extension holds, counted into the
		// pipeline's preparation queue depth. Any thread.
		[[nodiscard]] virtual std::uint64_t QueuedWorkCount() const { return 0; }

		// ---- Host dependencies (coordinator thread) ----

		// Whether one of the extension's dependencies is already satisfied.
		[[nodiscard]] virtual bool IsHostDependencyReady(const StaticHostDependency& dependency) const
		{
			(void)dependency;
			return false;
		}

		// Whether one of the extension's dependencies can never be satisfied.
		[[nodiscard]] virtual bool IsHostDependencyFailed(const StaticHostDependency& dependency) const
		{
			(void)dependency;
			return true;
		}

		// Starts resolving a dependency the pipeline is about to track as pending.
		// Returns false when the request could not be queued, which leaves it
		// untracked. The extension reports the outcome from its own coordinator drain.
		virtual bool RequestHostDependency(const StaticHostDependency& dependency)
		{
			(void)dependency;
			return false;
		}

		// ---- Host cells (coordinator thread) ----
		//
		// An extension can block a cell of its own on a set of dependencies; the
		// pipeline tracks the block next to its own dependency waiters.

		// Cells whose last blocking dependency resolved; their blocks are gone.
		// Called once for every resolved dependency, possibly with no cells.
		virtual void OnHostCellsUnblocked(std::span<const StaticCellKey> cells) { (void)cells; }

		// The pipeline dropped the extension's coordinator state for `cell`: one of
		// its blocking dependencies failed, the cell left the desired area, or
		// IsHostCellWanted said no. Any block on the cell is already gone.
		virtual void OnHostCellDropped(const StaticCellKey& cell) { (void)cell; }

		// Whether a blocked cell is still wanted for a newly accepted target; blocks
		// the extension no longer wants are dropped (see OnHostCellDropped).
		[[nodiscard]] virtual bool IsHostCellWanted(const StaticStreamTarget& target, const StaticCellKey& cell) const
		{
			(void)target;
			(void)cell;
			return true;
		}

		// Cells the extension tracks as in flight on the coordinator, not counting
		// blocked cells. Non-zero keeps the coordinator from reporting quiescence.
		[[nodiscard]] virtual std::size_t PendingHostCellCount() const { return 0; }

		// Collects the extension's per-cell data while a static cell is prepared.
		// Any pipeline worker.
		[[nodiscard]] virtual StaticCellPrepareRequest PrepareCell(const StaticCellKey& cell) const
		{
			(void)cell;
			return {};
		}

		// The coordinator accepted a prepared cell for which PrepareCell requested a
		// callback; `value` is what PrepareCell returned.
		virtual void OnCellHostPrepared(const StaticCellKey& cell, std::uint64_t generation, std::uint64_t value)
		{
			(void)cell;
			(void)generation;
			(void)value;
		}

		// ---- Coordinator drains (coordinator thread) ----

		// Whether the extension has results for its coordinator drains. Results wake
		// the coordinator without delay.
		[[nodiscard]] virtual bool HasCoordinatorResults() const { return false; }

		// Drains the extension's prepared results; runs right after the pipeline
		// drains its prepared cells. Returns whether it did work.
		virtual bool DrainPreparedResults() { return false; }

		// Drains the extension's dependency results; runs right after the pipeline
		// drains its own dependency results. Returns whether it did work.
		virtual bool DrainDependencyResults() { return false; }

		// ---- Host imports ----

		// Whether a host import the extension made for `cell` is still wanted for
		// `target`. Any pipeline thread.
		[[nodiscard]] virtual bool IsHostImportCurrent(
			const StaticStreamTarget& target,
			const StaticCellKey& cell,
			const StaticHostPayload& payload) const
		{
			(void)target;
			(void)cell;
			(void)payload;
			return false;
		}

		// A host import the extension made became part of the published scene; the
		// extension takes ownership of it.
		virtual void PublishHostImport(StaticHostImportPublication& publication) { (void)publication; }

		// ---- Frame drain (the thread that publishes stream targets) ----

		// Per-frame work while streaming is active around `target`, after the
		// pipeline flushed its acknowledgement backlogs and before it publishes
		// materialized transactions.
		virtual void DrainFrameWork(const StaticStreamTarget& target) { (void)target; }
	};

	// The extension's hold in a per-extension hold list, or null when there is none.
	[[nodiscard]] inline const StaticGroupHold& StaticGroupHoldAt(std::span<const StaticGroupHold> holds, std::size_t extensionIndex)
	{
		static const StaticGroupHold none;
		return extensionIndex < holds.size() ? holds[extensionIndex] : none;
	}
}
