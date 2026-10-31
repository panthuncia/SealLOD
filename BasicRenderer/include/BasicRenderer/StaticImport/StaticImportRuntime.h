#pragma once

// The static import runtime: everything the static import pipeline owns.
//
// It streams the static cells around a stream target into the renderer's scene. Cell
// placements come from an IStaticPlacementSource, renderable assets from an
// IStaticAssetSource, and host behaviour around the import from the registered
// IStaticImportExtensions. Internally it owns the work bus, the pipeline state, the
// group lifecycle store, the capacity broker, the drain scheduler and the coordinator
// pump, the coordinator and the streaming pump, the extension access surface, the
// StaticGroup / StaticTemplate / StaticVariant artifact producers, the worker sources
// (cell and group preparation, group assembly, transaction materialization,
// publication, retirement, cleanup) and the recipe cache's runtime compilation and
// persistence.
//
// Threading: the public operations run on the thread that owns the runtime (the one
// that publishes stream targets), unless noted. The pipeline's own work runs on the
// task scheduler inside StaticImportShared::staticStreamingScope.
//
// Lifetime:
// 1. Construct it, then register extensions with AddExtension.
// 2. AttachServices once the renderer's scene ingestion services exist (again whenever
//    they are replaced).
// 3. UpdateStreaming with the current target, every frame.
// 4. Shutdown (or destroy it) before the shared state, the sources, the extensions and
//    whatever the hooks reach. Shutdown stops every worker and coordinator task and
//    flushes at most one bounded batch of pending recipe persistence.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <BasicRenderer/Scene/SceneIngestionServices.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportAdvancedAccess.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportMessages.h>
#include <BasicRenderer/StaticImport/StaticAssetSource.h>
#include <BasicRenderer/StaticImport/StaticCellGeometry.h>
#include <BasicRenderer/StaticImport/StaticImportExtension.h>
#include <BasicRenderer/StaticImport/StaticImportSettings.h>
#include <BasicRenderer/StaticImport/StaticImportShared.h>
#include <BasicRenderer/StaticImport/StaticPlacementSource.h>
#include <BasicRenderer/StaticImport/StaticStreamCounters.h>
#include <BasicRenderer/StaticImport/StaticStreamTarget.h>
#include <BasicRenderer/Streaming/ObjectRequests.h>

namespace br::render
{
	class RendererStateRequestService;
}

namespace br::staticimport
{
	// Depths of the pipeline's queues and backlogs (see StaticImportRuntime::QueueDepths).
	struct StaticImportQueueDepths
	{
		std::size_t cellPrepareJobs{ 0 };
		std::size_t preparedCellResults{ 0 };
		std::size_t transactionMaterializeJobs{ 0 };
		std::size_t transactionMaterializeBacklog{ 0 };
		std::size_t materializedValidationRequests{ 0 };
		std::size_t resourcePendingBatchBacklog{ 0 };
		std::size_t groupAcks{ 0 };
		std::size_t groupAckBacklog{ 0 };
	};

	class StaticImportRuntime
	{
	public:
		// Host callbacks at fixed points of the runtime's lifecycle. All are optional.
		struct Hooks
		{
			// Host clients of the renderer state graph detach: before the runtime drops
			// its graph observations, when services are replaced and at shutdown.
			std::function<void()> detachStateGraph;
			// Host clients of the renderer state graph attach to the graph of newly
			// attached services, after the runtime registered its artifact producers
			// and before it observes graph completions.
			std::function<void(br::render::RendererStateRequestService&)> attachStateGraph;
			// Shutdown stopped admitting work and dropped its graph observations; the
			// pipeline's tasks are cancelled right after this returns.
			std::function<void()> stopping;
			// The host's own object-creation counters, reported in the periodic status log.
			std::function<StaticHostObjectCounters()> hostObjectCounters;
		};

		// `shared`, `assets`, `placements` and everything the hooks reach must outlive
		// the runtime (or its Shutdown).
		StaticImportRuntime(
			StaticImportShared& shared,
			const br::staticimport::StaticImportSettings& settings,
			br::staticimport::IStaticAssetSource& assets,
			br::staticimport::IStaticPlacementSource& placements,
			Hooks hooks);
		~StaticImportRuntime();

		StaticImportRuntime(const StaticImportRuntime&) = delete;
		StaticImportRuntime& operator=(const StaticImportRuntime&) = delete;

		// ---- Extensions ----
		//
		// Extensions are registered while the pipeline has not started, i.e. before the
		// first AttachServices or UpdateStreaming, on the owning thread. The pipeline
		// reads the list from its coordinator and worker threads without
		// synchronization and keeps per-extension state indexed by registration order
		// (group holds, host cell blocks, host work sources), so the list is frozen once
		// it starts: both calls then return false and change nothing.
		//
		// AddExtension appends `extension` (hooks run in registration order) and hands it
		// the pipeline access surface (OnAttached) before returning. RemoveExtension
		// unregisters it and releases the host work sources it claimed. An extension
		// must stay alive until it is removed or the runtime shut down. Both return
		// false for a duplicate / unknown extension.
		bool AddExtension(br::staticimport::IStaticImportExtension& extension);
		bool RemoveExtension(br::staticimport::IStaticImportExtension& extension);

		// The access surface extensions receive in OnAttached, including the advanced
		// operations for extensions that run import work of their own. Extensions that
		// need it at construction may take it before they are added.
		[[nodiscard]] IStaticImportAdvancedAccess& AdvancedAccess();

		// ---- Lifecycle ----

		// Binds the renderer's scene ingestion services, replacing earlier ones; captures
		// the renderer settings that shape static imports and registers the artifact
		// producers on the services' state graph (once).
		void AttachServices(br::render::StaticSceneIngestionAccess services);

		// Streams static cells around `target`; the per-frame entry point.
		void UpdateStreaming(const br::staticimport::StaticStreamTarget& target);

		// Stops the pipeline. Idempotent; the destructor calls it.
		void Shutdown();

		void SetBenchmarkDiagnosticsEnabled(bool enabled);
		[[nodiscard]] bool BenchmarkDiagnosticsEnabled() const;

		// ---- Queries (owning thread unless noted) ----

		[[nodiscard]] const br::staticimport::StaticImportSettings& Settings() const;

		// The cell area kept resident for `target`, including every extension's
		// ExpandDesiredCellRect. Any thread.
		[[nodiscard]] StaticDesiredCellArea BuildDesiredStaticCells(const br::staticimport::StaticStreamTarget& target) const;

		// Whether the attached services can run direct static imports.
		[[nodiscard]] bool DirectStaticImportAvailable() const;

		// Shader variants a ready group still waits on (requesting them as a side
		// effect). Any pipeline thread.
		[[nodiscard]] std::vector<StaticImportDependencyRequest> CollectShaderVariantDependencies(const DirectStaticReadyGroup& ready) const;

		// Static groups the published scene owns (or, before a scene is published, the
		// groups the pipeline and its extensions keep live).
		[[nodiscard]] std::size_t LiveStaticObjectCount() const;

		[[nodiscard]] StaticImportServiceStats GetStatsSnapshot() const;
		[[nodiscard]] std::uint64_t PreparationQueueDepth() const;
		[[nodiscard]] StaticStreamBudget MakeStreamBudget() const;
		[[nodiscard]] br::render::ObjectStorageStats CaptureObjectManagerStats() const;

		// Telemetry counters. The host also records its per-frame tick times here, which
		// the streaming budget reads.
		[[nodiscard]] StaticStreamCounters& Counters();
		[[nodiscard]] const StaticStreamCounters& Counters() const;

		// Queue and backlog depths for host diagnostics. Any thread; the backlog sizes
		// are read without synchronization, as approximate telemetry.
		[[nodiscard]] StaticImportQueueDepths QueueDepths() const;

		// The stream key of the last target the streaming pump handled.
		[[nodiscard]] std::optional<StaticStreamKey> LastStreamKey() const;

		// Digest of the placement set the published static scene is converging to. Any
		// thread.
		[[nodiscard]] std::uint64_t GraphDesiredPlacementSetDigest() const;

	private:
		class Impl;
		std::unique_ptr<Impl> _impl;
	};
}
