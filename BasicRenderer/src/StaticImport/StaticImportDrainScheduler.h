#pragma once

// Drain scheduling and queue admission for the static-import pipeline.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

#include <BasicRenderer/StaticImport/StaticImportExtension.h>

#include <BasicRenderer/Streaming/SerializedTaskPump.h>
#include <BasicTelemetry/Telemetry.h>
#include <BasicTelemetry/Tracy.h>
#include <BasicRenderer/StaticImport/StaticImportShared.h>
#include <BasicRenderer/StaticImport/Advanced/StreamingQueues.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportWorkScheduler.h>
#include "StaticImport/StaticImportPipelineState.h"

namespace br::staticimport
{
	// The scheduler source behind each host work source, and back.
	[[nodiscard]] constexpr StaticImportWorkSource StaticImportWorkSourceFor(br::staticimport::StaticHostWorkSource source)
	{
		return source == br::staticimport::StaticHostWorkSource::Prepare
			? StaticImportWorkSource::HostPrepare
			: StaticImportWorkSource::HostDependency;
	}

	[[nodiscard]] constexpr std::optional<br::staticimport::StaticHostWorkSource> StaticHostWorkSourceOf(StaticImportWorkSource source)
	{
		switch (source) {
		case StaticImportWorkSource::HostDependency: return br::staticimport::StaticHostWorkSource::Dependency;
		case StaticImportWorkSource::HostPrepare: return br::staticimport::StaticHostWorkSource::Prepare;
		default: return std::nullopt;
		}
	}
}

namespace br::staticimport
{

	class StaticImportDrainScheduler
	{
	public:
		using WorkSource = StaticImportWorkSource;
		using WorkerDrain = std::function<void(WorkSource, const br::TaskContext&)>;

		// Collaborators arrive at construction: there is no window in which this
		// object exists unbound, and a missing one is a compile error.
		StaticImportDrainScheduler(StaticImportShared& shared, StaticImportPipelineState& state,
			br::SerializedTaskPump& coordinatorPump, WorkerDrain workerDrain)
		{
			_shared = &shared;
			_state = &state;
			_pump = &coordinatorPump;
			_workerDrain = std::move(workerDrain);
		}

		void ScheduleCoordinatorDrain(std::chrono::steady_clock::duration delay = {})
		{
			if (_shared->staticImportServiceStop.load(std::memory_order_acquire)) return;
			const bool accepted = delay <= std::chrono::steady_clock::duration::zero()
				? (*_pump).Notify()
				: (*_pump).NotifyAfter(delay);
			(void)accepted;
		}

		void ScheduleWorkerDrains()
		{
			if (_shared->staticImportServiceStop.load(std::memory_order_acquire)) return;
			// Submit at most the scheduler's currently claimable slots for each source.
			// This reserves task identity before execution without inflating a source
			// beyond its concurrency policy. Latency-sensitive host preparation and
			// publication are considered first so a static-group burst cannot fill the
			// domain FIFO.
			static constexpr std::array fairSources{
				StaticImportWorkSource::TemplatePublication,
				StaticImportWorkSource::RetirementApply,
				StaticImportWorkSource::RetirementCommit,
				StaticImportWorkSource::HostDependency,
				StaticImportWorkSource::AssetPublish,
				StaticImportWorkSource::MaterialVariant,
				StaticImportWorkSource::ShaderVariant,
				StaticImportWorkSource::RecipeCompile,
				StaticImportWorkSource::MaterializedDecisionCleanup,
				StaticImportWorkSource::RemovalCleanup,
				StaticImportWorkSource::GroupArtifactRelease,
				StaticImportWorkSource::SpeculativeAssetPublish,
				StaticImportWorkSource::TransactionMaterialize,
				StaticImportWorkSource::GroupAssembly
			};
			const auto scheduleOne = [this](StaticImportWorkSource source) {
				if (!_shared->staticImportWorkScheduler.HasPending(source)) return false;
				auto& sourceScheduled =
					_state->staticImportSourceDrainsScheduled[static_cast<std::size_t>(source)];
				auto scheduled = sourceScheduled.load(std::memory_order_relaxed);
				const auto sourceMaximum = _shared->staticImportWorkScheduler.MaxParallelism(source);
				while (scheduled < sourceMaximum &&
					!sourceScheduled.compare_exchange_weak(
						scheduled, scheduled + 1, std::memory_order_acq_rel)) {}
				if (scheduled >= sourceMaximum) return false;
				for (;;) {
					auto active = _state->staticImportWorkerDrainsActive.load(std::memory_order_relaxed);
					if (active >= _state->staticImportWorkerConcurrency) {
						sourceScheduled.fetch_sub(1, std::memory_order_acq_rel);
						return false;
					}
					if (!_state->staticImportWorkerDrainsActive.compare_exchange_weak(
							active, active + 1, std::memory_order_acq_rel)) {
						continue;
					}
					if (!TaskSchedulerManager::GetInstance().Submit(
						_shared->staticStreamingScope, TaskLane::Streaming, TaskDomain::StaticImport,
						WorkerTaskName(source),
						[this, source](const br::TaskContext& context) {
							try { _workerDrain(source, context); }
							catch (...) {
								_state->staticImportSourceDrainsScheduled[static_cast<std::size_t>(source)].fetch_sub(
									1, std::memory_order_acq_rel);
								_state->staticImportWorkerDrainsActive.fetch_sub(1, std::memory_order_acq_rel);
								throw;
							}
						}, WorkerTraceMetadata(source))) {
						sourceScheduled.fetch_sub(1, std::memory_order_acq_rel);
						_state->staticImportWorkerDrainsActive.fetch_sub(1, std::memory_order_acq_rel);
						return false;
					}
					return true;
				}
			};
			// Reserve one latency-sensitive host-preparation/publication task first, then give
			// every other ready source one rotating opportunity before bulk sources
			// receive additional consumers. This prevents either a fixed priority list
			// or a single deep queue from monopolizing the eight-slot domain.
			(void)scheduleOne(StaticImportWorkSource::HostPrepare);
			(void)scheduleOne(StaticImportWorkSource::Publication);
			(void)scheduleOne(StaticImportWorkSource::TemplatePublication);
			(void)scheduleOne(StaticImportWorkSource::RetirementApply);
			(void)scheduleOne(StaticImportWorkSource::RetirementCommit);
			const auto begin = _state->staticImportSourceScheduleCursor.fetch_add(1,
				std::memory_order_relaxed);
			for (std::size_t offset = 0; offset < fairSources.size(); ++offset) {
				(void)scheduleOne(fairSources[(begin + offset) % fairSources.size()]);
			}
			for (;;) {
				const bool transaction = scheduleOne(StaticImportWorkSource::TransactionMaterialize);
				const bool assembly = scheduleOne(StaticImportWorkSource::GroupAssembly);
				if (!transaction && !assembly) break;
			}
		}

		template <class T>
		bool PushBlocking(BoundedMpscQueue<T>& queue, T value, bool notifyCoordinator = true)
		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportQueue::PushResumable");
			if (queue.TryPush(std::move(value))) {
				if (notifyCoordinator) {
					ScheduleCoordinatorDrain();
				}
				return true;
			}
			if (_shared->staticImportServiceStop.load(std::memory_order_acquire)) return false;
			(void)queue.PushDeferred(std::move(value));
			basic_telemetry::AddCounter("SARP.StaticImport.QueueAdmissionDeferred");
			basic_telemetry::SetGauge("SARP.StaticImport.DeferredAdmissionDepth",
				static_cast<std::int64_t>(queue.DeferredSize()));
			if (notifyCoordinator) {
				ScheduleCoordinatorDrain();
			}
			return true;
		}

		template <class T>
		bool PushWorker(
			BoundedMpscQueue<T>& queue,
			T value,
			StaticImportWorkSource source)
		{
			if (!PushBlocking(queue, std::move(value), false)) return false;
			_shared->staticImportWorkScheduler.Publish(source);
			ScheduleWorkerDrains();
			return true;
		}

		template <class T>
		bool PushStreaming(BoundedMpscQueue<T>& queue, T value)
		{
			return PushBlocking(queue, std::move(value), false);
		}

		// Names the worker tasks of a host work source (static storage). Set when an
		// extension claims the source, before workers start.
		void SetHostWorkTaskName(br::staticimport::StaticHostWorkSource source, const char* name)
		{
			_hostWorkTaskNames[static_cast<std::size_t>(source)] = name;
		}

	private:
		const char* WorkerTaskName(StaticImportWorkSource source) const
		{
			if (const auto host = StaticHostWorkSourceOf(source)) {
				if (const auto* name = _hostWorkTaskNames[static_cast<std::size_t>(*host)]) return name;
			}
			switch (source) {
			case StaticImportWorkSource::Publication: return "SARP::StaticImport::PublicationDrain";
			case StaticImportWorkSource::TemplatePublication: return "SARP::StaticImport::TemplatePublication";
			case StaticImportWorkSource::RetirementApply: return "SARP::StaticImport::RetirementApply";
			case StaticImportWorkSource::RetirementCommit: return "SARP::StaticImport::RetirementCommit";
			case StaticImportWorkSource::MaterializedDecisionCleanup: return "SARP::StaticImport::DecisionCleanupDrain";
			case StaticImportWorkSource::RemovalCleanup: return "SARP::StaticImport::RemovalCleanupDrain";
			case StaticImportWorkSource::GroupArtifactRelease: return "SARP::StaticImport::ArtifactReleaseDrain";
			case StaticImportWorkSource::GroupAssembly: return "SARP::StaticImport::GroupAssemblyDrain";
			case StaticImportWorkSource::TransactionMaterialize: return "SARP::StaticImport::TransactionMaterializeDrain";
			case StaticImportWorkSource::AssetPublish: return "SARP::StaticImport::AssetPublishDrain";
			case StaticImportWorkSource::MaterialVariant: return "SARP::StaticImport::MaterialVariantDrain";
			case StaticImportWorkSource::ShaderVariant: return "SARP::StaticImport::ShaderVariantDrain";
			case StaticImportWorkSource::HostDependency: return "SARP::StaticImport::HostDependencyDrain";
			case StaticImportWorkSource::HostPrepare: return "SARP::StaticImport::HostPrepareDrain";
			case StaticImportWorkSource::RecipeCompile: return "SARP::StaticImport::RecipeCompileDrain";
			case StaticImportWorkSource::SpeculativeAssetPublish: return "SARP::StaticImport::SpeculativeAssetDrain";
			case StaticImportWorkSource::Count: break;
			}
			return "SARP::StaticImport::SourceDrain";
		}

		static br::TaskTraceMetadata WorkerTraceMetadata(
			StaticImportWorkSource source)
		{
			constexpr std::uint8_t publication = 0;
			constexpr std::uint8_t ingestion = 2;
			constexpr std::uint8_t maintenance = 3;
			const bool cleanup = source == StaticImportWorkSource::RetirementApply ||
				source == StaticImportWorkSource::RetirementCommit ||
				source == StaticImportWorkSource::MaterializedDecisionCleanup ||
				source == StaticImportWorkSource::RemovalCleanup ||
				source == StaticImportWorkSource::GroupArtifactRelease;
			return {
				.admissionKey = static_cast<std::uint64_t>(source) + 1u,
				.workClass = source == StaticImportWorkSource::Publication
					? publication : (cleanup ? maintenance : ingestion),
				.admissionGroup = 2
			};
		}

		StaticImportShared* _shared;
		StaticImportPipelineState* _state;
		br::SerializedTaskPump* _pump;
		WorkerDrain _workerDrain;
		std::array<const char*, static_cast<std::size_t>(br::staticimport::StaticHostWorkSource::Count)> _hostWorkTaskNames{};
	};
}
