#pragma once

// The static import pipeline's side of br::staticimport::IStaticImportPipelineAccess
// and of its wider IStaticImportAdvancedAccess: the surface extensions use to act on
// the pipeline. Each operation is the pipeline bookkeeping an extension used to reach
// into directly. It also routes host work sources to the extensions that claimed them.

#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <vector>

#include <BasicRenderer/StaticImport/StaticImportExtension.h>

#include <BasicRenderer/StaticImport/StaticImportShared.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportAdvancedAccess.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportMessages.h>
#include "StaticImport/StaticGroupLifecycleStore.h"
#include "StaticImport/StaticImportCoordinator.h"
#include "StaticImport/StaticImportDrainScheduler.h"
#include "StaticImport/StaticImportPipelineState.h"
#include "StaticImport/StaticImportStreamingPump.h"
#include "StaticImport/StaticImportWorkBus.h"

namespace br::staticimport
{
	class StaticImportPipelineAccess final : public IStaticImportAdvancedAccess
	{
	public:
		// The coordinator and the pump are only used once extensions run, so they may
		// still be under construction when this object is built.
		StaticImportPipelineAccess(StaticImportShared& shared, StaticImportWorkBus& bus, StaticImportPipelineState& importState, StaticImportDrainScheduler& drains, StaticImportCoordinator& coordinator, StaticImportStreamingPump& pump)
			: _shared(std::addressof(shared))
			, _bus(std::addressof(bus))
			, _importState(std::addressof(importState))
			, _drains(std::addressof(drains))
			, _coordinator(std::addressof(coordinator))
			, _pump(std::addressof(pump))
		{
		}

		// Runs a claimed host work source on a pipeline worker. Returns whether the
		// claim was productive; an unclaimed source never is.
		bool RunHostWork(StaticImportWorkSource source)
		{
			const auto host = StaticHostWorkSourceOf(source);
			if (!host) return false;
			auto* owner = _hostWorkOwners[static_cast<std::size_t>(*host)];
			return owner && owner->RunHostWork(*host);
		}

		// Releases every host work source `owner` claimed (the extension is being
		// unregistered). Only while the pipeline has not started.
		void ReleaseHostWorkSources(const br::staticimport::IStaticImportExtension& owner)
		{
			for (std::size_t index = 0; index < _hostWorkOwners.size(); ++index) {
				if (_hostWorkOwners[index] != std::addressof(owner)) continue;
				_hostWorkOwners[index] = nullptr;
				_drains->SetHostWorkTaskName(static_cast<br::staticimport::StaticHostWorkSource>(index), nullptr);
			}
		}

		void SubmitHostGroup(br::staticimport::StaticHostGroupRequest request) override
		{
			PreparedStaticCell prepared;
			prepared.key = request.cell;
			prepared.generation = _importState->staticImportServiceTargetGeneration.load(std::memory_order_acquire);
			prepared.worldspaceResident = true;
			PreparedStaticGroup group;
			group.groupID = request.groupID;
			group.modelPath = std::move(request.assetKey);
			group.name = std::move(request.name);
			group.instanceWorldMatrices = std::move(request.instanceWorldMatrices);
			group.placementCount = group.instanceWorldMatrices.size();
			group.residencyClass = StaticImportResidencyClass::HostGroup;
			group.residencyGeneration = request.residencyGeneration;
			prepared.groups.push_back(std::move(group));
			// The coordinator seals and integrates the record, as it does for prepared cells.
			BuildPreparedStaticCellLifecycle(prepared);
			// Host groups also produce coordinator work. A direct queue push can
			// strand these results once ordinary cell preparation has gone idle.
			(void)_drains->PushBlocking(_bus->staticImportPreparedCellResults, std::move(prepared));
		}

		void WakeCoordinator() override
		{
			_drains->ScheduleCoordinatorDrain();
		}

		[[nodiscard]] br::staticimport::StaticDesiredCellArea DesiredCellArea(const br::staticimport::StaticStreamTarget& target) const override
		{
			return _pump->BuildDesiredStaticCells(target);
		}

		void RecordWorkloadCount(const DrawWorkloadKey& workload, std::uint32_t span) override
		{
			_importState->directStaticWorkloadCounts[workload] = span;
		}

		void AddAppliedPlacements(std::uint64_t placements) override
		{
			_importState->staticAppliedPlacementCount += placements;
		}

		void ReclaimAppliedPlacements(std::uint64_t placements) override
		{
			_importState->staticAppliedPlacementCount -= (std::min)(_importState->staticAppliedPlacementCount, placements);
		}

		void RetireHostGroupPayloads(std::vector<br::render::StaticObjectRemovalPayload>&& payloads) override
		{
			for (auto& payload : payloads) {
				(void)_drains->PushBlocking(_bus->staticRetirementPayloadRequests, std::move(payload), false);
			}
			_shared->staticImportWorkScheduler.Signal(StaticImportWorkSource::RetirementApply);
			_drains->ScheduleWorkerDrains();
		}

		[[nodiscard]] bool IsStopping() const override
		{
			return _shared->staticImportServiceStop.load(std::memory_order_acquire);
		}

		[[nodiscard]] std::uint64_t StreamTargetGeneration() const override
		{
			return _importState->staticImportServiceTargetGeneration.load(std::memory_order_acquire);
		}

		void ClaimHostWorkSource(br::staticimport::IStaticImportExtension& owner, br::staticimport::StaticHostWorkSource source, const char* taskName) override
		{
			_hostWorkOwners[static_cast<std::size_t>(source)] = std::addressof(owner);
			_drains->SetHostWorkTaskName(source, taskName);
		}

		void PublishHostWork(br::staticimport::StaticHostWorkSource source) override
		{
			_shared->staticImportWorkScheduler.Publish(StaticImportWorkSourceFor(source));
			_drains->ScheduleWorkerDrains();
		}

		[[nodiscard]] bool IsHostCellBlocked(const br::staticimport::IStaticImportExtension& owner, const StaticCellKey& cell) const override
		{
			return _coordinator->IsHostCellBlocked(owner, cell);
		}

		[[nodiscard]] const StaticImportCameraTarget& LatestTarget() const override
		{
			return _coordinator->LatestTarget();
		}

		[[nodiscard]] StaticTemplateRefSnapshot TemplateSnapshot() const override
		{
			return _coordinator->TemplateSnapshot();
		}

		StaticHostCellBlockResult BlockHostCell(
			br::staticimport::IStaticImportExtension& owner,
			const StaticCellKey& cell,
			std::vector<StaticImportDependencyRequest> dependencies) override
		{
			return _coordinator->BlockHostCell(owner, cell, std::move(dependencies));
		}

		std::optional<StaticImportDependencyRequest> TakePendingDependency(const StaticImportDependencyKey& key) override
		{
			return _coordinator->TakePendingDependency(key);
		}

		void SettleDependency(
			StaticImportDependencyRequest request,
			StaticImportDependencyResultStatus status,
			std::chrono::steady_clock::duration retryDelay) override
		{
			_coordinator->SettleDependency(std::move(request), status, retryDelay);
		}

		void SubmitHostImportBatch(PreparedDirectStaticImportBatch batch) override
		{
			_coordinator->SubmitHostImportBatch(std::move(batch));
		}

		[[nodiscard]] bool BenchmarkDiagnosticsEnabled() const override
		{
			return _importState->benchmarkDiagnosticsEnabled;
		}

	private:
		StaticImportShared* _shared;
		StaticImportWorkBus* _bus;
		StaticImportPipelineState* _importState;
		StaticImportDrainScheduler* _drains;
		StaticImportCoordinator* _coordinator;
		StaticImportStreamingPump* _pump;
		std::array<br::staticimport::IStaticImportExtension*, static_cast<std::size_t>(br::staticimport::StaticHostWorkSource::Count)> _hostWorkOwners{};
	};
}
