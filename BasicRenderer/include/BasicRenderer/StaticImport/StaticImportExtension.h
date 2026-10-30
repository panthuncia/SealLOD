#pragma once

// Extension points of the static import pipeline.
//
// The pipeline streams cell-resident static groups around a target on its own. A host
// adds behaviour around that through extensions: content whose residency it manages
// itself (host-resident groups, e.g. coarse stand-ins that cover cells until their
// real groups arrive), per-group resources it tracks while a group is owned by the
// scene, extra gates on template publication, and its own per-target work.
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

		// A removal of `cell`'s groups was applied to the published scene.
		virtual void OnCellRemovalApplied(const StaticCellKey& cell, std::uint64_t generation)
		{
			(void)cell;
			(void)generation;
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

		// A removal of the group was applied to the published scene (any residency).
		virtual void OnGroupRemovalApplied(std::uint64_t groupID, const StaticGroupHold& hold)
		{
			(void)groupID;
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
	};

	// The extension's hold in a per-extension hold list, or null when there is none.
	[[nodiscard]] inline const StaticGroupHold& StaticGroupHoldAt(std::span<const StaticGroupHold> holds, std::size_t extensionIndex)
	{
		static const StaticGroupHold none;
		return extensionIndex < holds.size() ? holds[extensionIndex] : none;
	}
}
