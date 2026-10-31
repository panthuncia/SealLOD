#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace br::staticimport
{
	// Resource availability, rather than a worker-derived token count, bounds
	// static preparation. TryReserve never waits for capacity or a contended
	// broker lock; callers yield and are woken when an existing reservation is
	// released.
	class StaticImportCapacityBroker
	{
	public:
		struct Cost
		{
			std::uint64_t preparedBytes{ 0 };
			std::uint64_t drawRecords{ 0 };
			std::uint64_t activeIndices{ 0 };
			std::uint64_t uploadBytes{ 0 };
			std::uint64_t publicationCuts{ 0 };
		};

		struct Limits
		{
			std::uint64_t preparedBytes{ 256ull * 1024ull * 1024ull };
			std::uint64_t drawRecords{ (std::numeric_limits<std::uint64_t>::max)() };
			std::uint64_t activeIndices{ (std::numeric_limits<std::uint64_t>::max)() };
			std::uint64_t uploadBytes{ (std::numeric_limits<std::uint64_t>::max)() };
			std::uint64_t publicationCuts{ 4 };
		};

		struct Snapshot
		{
			Cost reserved;
			Limits limits;
			std::uint64_t reservations{ 0 };
			std::uint64_t failedCapacity{ 0 };
			std::uint64_t failedContention{ 0 };
			std::uint64_t releases{ 0 };
		};

	private:
		struct State
		{
			std::mutex mutex;
			Cost reserved;
			Limits limits;
			std::function<void()> wake;
			std::uint64_t reservations{ 0 };
			std::uint64_t failedCapacity{ 0 };
			std::atomic<std::uint64_t> failedContention{ 0 };
			std::uint64_t releases{ 0 };
			bool stopped{ false };
		};

	public:
		class Reservation
		{
		public:
			Reservation() = default;
			Reservation(const Reservation&) = delete;
			Reservation& operator=(const Reservation&) = delete;
			Reservation(Reservation&& other) noexcept = default;
			Reservation& operator=(Reservation&& other) noexcept
			{
				if (this != std::addressof(other)) {
					Release();
					_state = std::move(other._state);
					_cost = std::exchange(other._cost, {});
				}
				return *this;
			}
			~Reservation() { Release(); }

			[[nodiscard]] explicit operator bool() const noexcept { return !!_state; }
			[[nodiscard]] const Cost& ReservedCost() const noexcept { return _cost; }

			// Release components as their ownership transfers to downstream systems.
			// Requests are clamped to the reservation's remaining balance so cleanup
			// and cancellation can safely call Release() afterward.
			void ReleaseComponents(Cost released)
			{
				if (!_state) return;
				released.preparedBytes = (std::min)(released.preparedBytes, _cost.preparedBytes);
				released.drawRecords = (std::min)(released.drawRecords, _cost.drawRecords);
				released.activeIndices = (std::min)(released.activeIndices, _cost.activeIndices);
				released.uploadBytes = (std::min)(released.uploadBytes, _cost.uploadBytes);
				released.publicationCuts = (std::min)(released.publicationCuts, _cost.publicationCuts);
				if (IsZero(released)) return;
				std::function<void()> wake;
				{
					std::lock_guard lock(_state->mutex);
					Subtract(_state->reserved, released);
					++_state->releases;
					if (!_state->stopped) wake = _state->wake;
				}
				Subtract(_cost, released);
				if (wake) wake();
			}

			void Release()
			{
				auto state = std::move(_state);
				if (!state) return;
				std::function<void()> wake;
				{
					std::lock_guard lock(state->mutex);
					Subtract(state->reserved, _cost);
					++state->releases;
					if (!state->stopped) wake = state->wake;
				}
				_cost = {};
				if (wake) wake();
			}

		private:
			friend class StaticImportCapacityBroker;
			Reservation(std::shared_ptr<State> state, Cost cost)
				: _state(std::move(state)), _cost(cost) {}

			static void Subtract(Cost& target, const Cost& value)
			{
				target.preparedBytes -= value.preparedBytes;
				target.drawRecords -= value.drawRecords;
				target.activeIndices -= value.activeIndices;
				target.uploadBytes -= value.uploadBytes;
				target.publicationCuts -= value.publicationCuts;
			}

			static bool IsZero(const Cost& value)
			{
				return value.preparedBytes == 0 && value.drawRecords == 0 &&
					value.activeIndices == 0 && value.uploadBytes == 0 &&
					value.publicationCuts == 0;
			}

			std::shared_ptr<State> _state;
			Cost _cost;
		};

		StaticImportCapacityBroker() : _state(std::make_shared<State>()) {}
		StaticImportCapacityBroker(const StaticImportCapacityBroker&) = delete;
		StaticImportCapacityBroker& operator=(const StaticImportCapacityBroker&) = delete;

		void Configure(Limits limits, std::function<void()> wake = {})
		{
			std::lock_guard lock(_state->mutex);
			_state->limits = limits;
			_state->wake = std::move(wake);
			_state->stopped = false;
		}

		[[nodiscard]] std::optional<Reservation> TryReserve(const Cost& cost)
		{
			std::unique_lock lock(_state->mutex, std::try_to_lock);
			if (!lock.owns_lock()) {
				_state->failedContention.fetch_add(1, std::memory_order_relaxed);
				return std::nullopt;
			}
			if (_state->stopped || !Fits(_state->reserved, cost, _state->limits)) {
				++_state->failedCapacity;
				return std::nullopt;
			}
			Add(_state->reserved, cost);
			++_state->reservations;
			return Reservation{ _state, cost };
		}

		[[nodiscard]] Snapshot GetSnapshot() const
		{
			std::lock_guard lock(_state->mutex);
			return { _state->reserved, _state->limits, _state->reservations,
				_state->failedCapacity,
				_state->failedContention.load(std::memory_order_relaxed), _state->releases };
		}

		void Stop()
		{
			std::lock_guard lock(_state->mutex);
			_state->stopped = true;
			_state->wake = {};
		}

	private:
		static bool FitsOne(std::uint64_t current, std::uint64_t requested, std::uint64_t limit)
		{
			return requested <= limit && current <= limit - requested;
		}

		static bool Fits(const Cost& current, const Cost& requested, const Limits& limits)
		{
			return FitsOne(current.preparedBytes, requested.preparedBytes, limits.preparedBytes) &&
				FitsOne(current.drawRecords, requested.drawRecords, limits.drawRecords) &&
				FitsOne(current.activeIndices, requested.activeIndices, limits.activeIndices) &&
				FitsOne(current.uploadBytes, requested.uploadBytes, limits.uploadBytes) &&
				FitsOne(current.publicationCuts, requested.publicationCuts, limits.publicationCuts);
		}

		static void Add(Cost& target, const Cost& value)
		{
			target.preparedBytes += value.preparedBytes;
			target.drawRecords += value.drawRecords;
			target.activeIndices += value.activeIndices;
			target.uploadBytes += value.uploadBytes;
			target.publicationCuts += value.publicationCuts;
		}

		std::shared_ptr<State> _state;
	};
}
