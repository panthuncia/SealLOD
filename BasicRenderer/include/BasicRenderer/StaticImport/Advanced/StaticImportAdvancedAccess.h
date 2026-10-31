#pragma once

// The wider pipeline surface for extensions that run import work of their own.
//
// IStaticImportPipelineAccess covers what any extension needs. An extension that
// blocks its cells on pipeline dependencies and submits prepared imports also needs
// the pipeline's own dependency requests and prepared batches (the Advanced/ message
// vocabulary), so it takes this wider surface. Each operation is still a single,
// explicit pipeline step; nothing here exposes the work bus or the coordinator state.
//
// The coordinator operations are valid only on the coordinator thread, i.e. inside an
// extension's coordinator hooks (host dependency and host cell hooks, coordinator
// drains, OnCellHostPrepared).

#include <chrono>
#include <optional>
#include <vector>

#include <BasicRenderer/StaticImport/StaticImportExtension.h>

#include <BasicRenderer/StaticImport/Advanced/StaticImportMessages.h>

namespace br::staticimport
{
	enum class StaticHostCellBlockResult : std::uint8_t
	{
		Failed,  // A dependency failed; its waiters were failed with it and the cell is not blocked.
		Ready,   // Every dependency is already resolved; the cell is not blocked.
		Blocked  // The cell waits on the remaining dependencies (see OnHostCellsUnblocked).
	};

	class IStaticImportAdvancedAccess : public br::staticimport::IStaticImportPipelineAccess
	{
	public:
		// The coordinator's latest accepted target.
		[[nodiscard]] virtual const StaticImportCameraTarget& LatestTarget() const = 0;

		// The coordinator's current template snapshot, for jobs that look templates up
		// on workers.
		[[nodiscard]] virtual StaticTemplateRefSnapshot TemplateSnapshot() const = 0;

		// Blocks `owner`'s `cell` on the unresolved ones among `dependencies`,
		// replacing any block it had. Resolved dependencies are skipped and the others
		// requested. The first failed one is failed for all of its waiters and ends the
		// call with Failed.
		virtual StaticHostCellBlockResult BlockHostCell(
			br::staticimport::IStaticImportExtension& owner,
			const StaticCellKey& cell,
			std::vector<StaticImportDependencyRequest> dependencies) = 0;

		// Removes a dependency from the pending set and returns it; empty when it is
		// not pending.
		virtual std::optional<StaticImportDependencyRequest> TakePendingDependency(const StaticImportDependencyKey& key) = 0;

		// Settles a dependency whose resolution attempt completed with `status`: Ready
		// resolves and Failed fails it for its waiters; Pending retries it after
		// `retryDelay` while anything still waits on it.
		virtual void SettleDependency(
			StaticImportDependencyRequest request,
			StaticImportDependencyResultStatus status,
			std::chrono::steady_clock::duration retryDelay) = 0;

		// Queues a batch of host imports for publication under the latest target's
		// generation and a fresh publish ticket.
		virtual void SubmitHostImportBatch(PreparedDirectStaticImportBatch batch) = 0;

		// Whether the host asked for extra benchmark diagnostics.
		[[nodiscard]] virtual bool BenchmarkDiagnosticsEnabled() const = 0;
	};
}
