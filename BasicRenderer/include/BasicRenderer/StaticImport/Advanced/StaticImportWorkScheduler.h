#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace br::staticimport
{
	enum class StaticImportWorkSource : std::uint8_t
	{
		Publication,
		TemplatePublication,
		RetirementApply,
		RetirementCommit,
		MaterializedDecisionCleanup,
		RemovalCleanup,
		GroupArtifactRelease,
		GroupAssembly,
		TransactionMaterialize,
		AssetPublish,
		MaterialVariant,
		ShaderVariant,
		// Host work sources (br::staticimport::StaticHostWorkSource), consumed by the
		// extension that claimed them; mapped in StaticImportDrainScheduler.h.
		HostDependency,
		HostPrepare,
		RecipeCompile,
		SpeculativeAssetPublish,
		Count
	};

	class StaticImportWorkScheduler
	{
	public:
		using Clock = std::chrono::steady_clock;
		static constexpr std::size_t SourceCount =
			static_cast<std::size_t>(StaticImportWorkSource::Count);

		struct Claim
		{
			StaticImportWorkSource source{ StaticImportWorkSource::Publication };
			std::uint64_t ticket{ 0 };
			std::uint64_t epoch{ 0 };
			Clock::time_point publishedAt{};
		};

		struct SourceSnapshot
		{
			std::uint64_t published{ 0 };
			std::uint64_t claimed{ 0 };
			std::uint64_t productive{ 0 };
			std::uint64_t staleClaims{ 0 };
			std::uint64_t recoveries{ 0 };
			std::uint64_t totalSchedulingLatencyUs{ 0 };
			std::uint64_t maxSchedulingLatencyUs{ 0 };
			std::uint32_t active{ 0 };
		};

		StaticImportWorkScheduler()
		{
			for (auto& source : _sources) {
				source.maxParallelism.store((std::numeric_limits<std::uint32_t>::max)(),
					std::memory_order_relaxed);
			}
			SetMaxParallelism(StaticImportWorkSource::Publication, 1);
			SetMaxParallelism(StaticImportWorkSource::TemplatePublication, 1);
			SetMaxParallelism(StaticImportWorkSource::RetirementApply, 1);
			SetMaxParallelism(StaticImportWorkSource::RetirementCommit, 1);
			SetMaxParallelism(StaticImportWorkSource::MaterializedDecisionCleanup, 1);
			SetMaxParallelism(StaticImportWorkSource::RemovalCleanup, 1);
			SetMaxParallelism(StaticImportWorkSource::GroupArtifactRelease, 1);
			SetMaxParallelism(StaticImportWorkSource::SpeculativeAssetPublish, 1);
		}

		void SetMaxParallelism(StaticImportWorkSource source, std::uint32_t value)
		{
			State(source).maxParallelism.store(value == 0 ? 1 : value, std::memory_order_release);
		}

		[[nodiscard]] std::uint32_t MaxParallelism(StaticImportWorkSource source) const
		{
			return State(source).maxParallelism.load(std::memory_order_acquire);
		}

		[[nodiscard]] std::uint64_t Epoch() const noexcept
		{
			return _epoch.load(std::memory_order_acquire);
		}

		// Publish after the work item has entered its owning queue. This ordering
		// ensures that every claim corresponds to an item the consumer can observe.
		void Publish(StaticImportWorkSource source, std::uint64_t count = 1)
		{
			if (count == 0) return;
			auto& state = State(source);
			const auto previous = state.published.fetch_add(count, std::memory_order_acq_rel);
			if (previous == state.claimed.load(std::memory_order_acquire)) {
				const auto now = Clock::now().time_since_epoch();
				state.oldestPendingNs.store(
					std::chrono::duration_cast<std::chrono::nanoseconds>(now).count(),
					std::memory_order_release);
			}
		}

		// Level/generation sources collapse duplicate notifications while one is
		// pending, but preserve a notification that arrives while a claim is active.
		void Signal(StaticImportWorkSource source)
		{
			auto& state = State(source);
			auto published = state.published.load(std::memory_order_acquire);
			for (;;) {
				const auto claimed = state.claimed.load(std::memory_order_acquire);
				if (published > claimed) return;
				if (state.published.compare_exchange_weak(
						published, claimed + 1, std::memory_order_acq_rel,
						std::memory_order_acquire)) {
					const auto now = Clock::now().time_since_epoch();
					state.oldestPendingNs.store(
						std::chrono::duration_cast<std::chrono::nanoseconds>(now).count(),
						std::memory_order_release);
					return;
				}
			}
		}

		[[nodiscard]] std::optional<Claim> TryClaim()
		{
			constexpr auto starvationLimit = std::chrono::seconds(2);
			const auto now = Clock::now();
			std::optional<StaticImportWorkSource> starved;
			Clock::time_point oldest{};
			for (std::size_t index = 0; index < SourceCount; ++index) {
				const auto source = static_cast<StaticImportWorkSource>(index);
				const auto& state = State(source);
				if (state.claimed.load(std::memory_order_acquire) >=
					state.published.load(std::memory_order_acquire) ||
					state.active.load(std::memory_order_acquire) >=
					state.maxParallelism.load(std::memory_order_acquire)) continue;
				const auto oldestNs = state.oldestPendingNs.load(std::memory_order_acquire);
				const auto publishedAt = Clock::time_point{
					std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(oldestNs)) };
				if (now - publishedAt < starvationLimit) continue;
				if (!starved || publishedAt < oldest) {
					starved = source;
					oldest = publishedAt;
				}
			}
			if (starved) {
				if (auto claim = TryClaim(*starved)) return claim;
			}
			const auto begin = _fairCursor.fetch_add(1, std::memory_order_relaxed);
			for (std::size_t offset = 0; offset < FairOrder.size(); ++offset) {
				const auto source = FairOrder[(begin + offset) % FairOrder.size()];
				if (auto claim = TryClaim(source)) return claim;
			}
			return std::nullopt;
		}

		[[nodiscard]] std::optional<Claim> TryClaim(StaticImportWorkSource source)
		{
			auto& state = State(source);
			auto active = state.active.load(std::memory_order_acquire);
			const auto maximum = state.maxParallelism.load(std::memory_order_acquire);
			while (active < maximum) {
				if (state.active.compare_exchange_weak(
						active, active + 1, std::memory_order_acq_rel,
						std::memory_order_acquire)) break;
			}
			if (active >= maximum) return std::nullopt;

			auto claimed = state.claimed.load(std::memory_order_acquire);
			for (;;) {
				const auto published = state.published.load(std::memory_order_acquire);
				if (claimed >= published) {
					state.active.fetch_sub(1, std::memory_order_acq_rel);
					return std::nullopt;
				}
				if (state.claimed.compare_exchange_weak(
						claimed, claimed + 1, std::memory_order_acq_rel,
						std::memory_order_acquire)) break;
			}

			const auto oldestNs = state.oldestPendingNs.load(std::memory_order_acquire);
			const auto publishedAt = Clock::time_point{
				std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(oldestNs)) };
			const auto latencyUs = static_cast<std::uint64_t>((std::max<std::int64_t>)(0,
				std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - publishedAt).count()));
			state.totalSchedulingLatencyUs.fetch_add(latencyUs, std::memory_order_relaxed);
			auto maximumLatency = state.maxSchedulingLatencyUs.load(std::memory_order_relaxed);
			while (maximumLatency < latencyUs &&
				!state.maxSchedulingLatencyUs.compare_exchange_weak(
					maximumLatency, latencyUs, std::memory_order_relaxed)) {}
			return Claim{ source, claimed + 1, Epoch(), publishedAt };
		}

		// A productive consumer may cooperatively consume more tickets from its
		// exact source to form a bounded batch. These tickets do not create extra
		// active consumers and cannot be borrowed by a different source.
		[[nodiscard]] std::uint64_t ClaimAdditional(
			StaticImportWorkSource source,
			std::uint64_t maximum)
		{
			if (maximum == 0) return 0;
			auto& state = State(source);
			auto claimed = state.claimed.load(std::memory_order_acquire);
			for (;;) {
				const auto published = state.published.load(std::memory_order_acquire);
				if (claimed >= published) return 0;
				const auto count = (std::min)(maximum, published - claimed);
				if (state.claimed.compare_exchange_weak(
						claimed, claimed + count, std::memory_order_acq_rel,
						std::memory_order_acquire)) return count;
			}
		}

		void Complete(const Claim& claim, bool productive)
		{
			auto& state = State(claim.source);
			if (claim.epoch != Epoch()) {
				state.staleClaims.fetch_add(1, std::memory_order_relaxed);
			} else if (productive) {
				state.productive.fetch_add(1, std::memory_order_relaxed);
			} else {
				// Level-triggered sources may legitimately find that another consumer
				// satisfied the condition first. A later readiness edge must resignal.
				state.staleClaims.fetch_add(1, std::memory_order_relaxed);
			}
			state.active.fetch_sub(1, std::memory_order_acq_rel);
		}

		void Recover(StaticImportWorkSource source)
		{
			State(source).recoveries.fetch_add(1, std::memory_order_relaxed);
			Signal(source);
		}

		[[nodiscard]] bool HasPending() const
		{
			for (const auto& source : _sources) {
				if (source.claimed.load(std::memory_order_acquire) <
					source.published.load(std::memory_order_acquire)) return true;
			}
			return false;
		}

		[[nodiscard]] std::uint64_t PendingCount() const
		{
			std::uint64_t total = 0;
			for (const auto& source : _sources) {
				const auto published = source.published.load(std::memory_order_acquire);
				const auto claimed = source.claimed.load(std::memory_order_acquire);
				total += published > claimed ? published - claimed : 0;
			}
			return total;
		}

		[[nodiscard]] std::uint64_t ClaimableCount() const
		{
			std::uint64_t total = 0;
			for (const auto& source : _sources) {
				const auto published = source.published.load(std::memory_order_acquire);
				const auto claimed = source.claimed.load(std::memory_order_acquire);
				const auto pending = published > claimed ? published - claimed : 0;
				const auto active = source.active.load(std::memory_order_acquire);
				const auto maximum = source.maxParallelism.load(std::memory_order_acquire);
				const auto sourceSlots = maximum > active ? maximum - active : 0;
				total += (std::min<std::uint64_t>)(pending, sourceSlots);
			}
			return total;
		}

		[[nodiscard]] std::uint64_t ClaimableCount(StaticImportWorkSource workSource) const
		{
			const auto& source = State(workSource);
			const auto published = source.published.load(std::memory_order_acquire);
			const auto claimed = source.claimed.load(std::memory_order_acquire);
			const auto pending = published > claimed ? published - claimed : 0;
			const auto active = source.active.load(std::memory_order_acquire);
			const auto maximum = source.maxParallelism.load(std::memory_order_acquire);
			const auto sourceSlots = maximum > active ? maximum - active : 0;
			return (std::min<std::uint64_t>)(pending, sourceSlots);
		}

		[[nodiscard]] bool HasPending(StaticImportWorkSource workSource) const
		{
			const auto& source = State(workSource);
			return source.claimed.load(std::memory_order_acquire) <
				source.published.load(std::memory_order_acquire);
		}

		void Reset()
		{
			_epoch.fetch_add(1, std::memory_order_acq_rel);
			for (auto& source : _sources) {
				source.claimed.store(source.published.load(std::memory_order_acquire),
					std::memory_order_release);
				source.oldestPendingNs.store(0, std::memory_order_release);
			}
			_fairCursor.store(0, std::memory_order_release);
		}

		[[nodiscard]] SourceSnapshot Snapshot(StaticImportWorkSource source) const
		{
			const auto& state = State(source);
			return {
				state.published.load(std::memory_order_acquire),
				state.claimed.load(std::memory_order_acquire),
				state.productive.load(std::memory_order_relaxed),
				state.staleClaims.load(std::memory_order_relaxed),
				state.recoveries.load(std::memory_order_relaxed),
				state.totalSchedulingLatencyUs.load(std::memory_order_relaxed),
				state.maxSchedulingLatencyUs.load(std::memory_order_relaxed),
				state.active.load(std::memory_order_acquire)
			};
		}

	private:
		struct SourceState
		{
			std::atomic<std::uint64_t> published{ 0 };
			std::atomic<std::uint64_t> claimed{ 0 };
			std::atomic<std::uint64_t> productive{ 0 };
			std::atomic<std::uint64_t> staleClaims{ 0 };
			std::atomic<std::uint64_t> recoveries{ 0 };
			std::atomic<std::uint64_t> totalSchedulingLatencyUs{ 0 };
			std::atomic<std::uint64_t> maxSchedulingLatencyUs{ 0 };
			std::atomic<std::int64_t> oldestPendingNs{ 0 };
			std::atomic<std::uint32_t> active{ 0 };
			std::atomic<std::uint32_t> maxParallelism{ 1 };
		};

		// Repeated entries are weights. Publication and materialization receive
		// bounded precedence, while every source retains a regular service slot.
		static constexpr std::array FairOrder{
			StaticImportWorkSource::Publication,
			StaticImportWorkSource::TemplatePublication,
			StaticImportWorkSource::RetirementApply,
			StaticImportWorkSource::RetirementCommit,
			StaticImportWorkSource::GroupAssembly,
			StaticImportWorkSource::TransactionMaterialize,
			StaticImportWorkSource::HostPrepare,
			StaticImportWorkSource::Publication,
			StaticImportWorkSource::TemplatePublication,
			StaticImportWorkSource::GroupAssembly,
			StaticImportWorkSource::TransactionMaterialize,
			StaticImportWorkSource::AssetPublish,
			StaticImportWorkSource::MaterialVariant,
			StaticImportWorkSource::ShaderVariant,
			StaticImportWorkSource::HostDependency,
			StaticImportWorkSource::HostPrepare,
			StaticImportWorkSource::RecipeCompile,
			StaticImportWorkSource::MaterializedDecisionCleanup,
			StaticImportWorkSource::RemovalCleanup,
			StaticImportWorkSource::GroupArtifactRelease,
			StaticImportWorkSource::SpeculativeAssetPublish
		};

		[[nodiscard]] SourceState& State(StaticImportWorkSource source)
		{
			return _sources[static_cast<std::size_t>(source)];
		}
		[[nodiscard]] const SourceState& State(StaticImportWorkSource source) const
		{
			return _sources[static_cast<std::size_t>(source)];
		}

		std::array<SourceState, SourceCount> _sources{};
		std::atomic<std::uint64_t> _epoch{ 1 };
		std::atomic<std::size_t> _fairCursor{ 0 };
	};
}
