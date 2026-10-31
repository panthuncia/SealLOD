#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace br::staticimport
{
	// Worker-safe MPSC handoff. The queue owns admission capacity and wake-up
	// coordination; the subsystem owning an instance owns its shutdown flag.
	template <class T>
	class BoundedMpscQueue
	{
	public:
		// Capacity 0 is an explicit unbounded handoff queue. Use bounded queues for
		// work admission, but avoid producer sleeps on completion/result handoffs.
		explicit BoundedMpscQueue(std::size_t capacity = 4096) :
			_capacity(capacity)
		{}

		bool TryPush(const T& value)
		{
			std::lock_guard lock(_mutex);
			if (FullLocked()) {
				return false;
			}
			_queue.push_back(value);
			return true;
		}

		bool TryPush(T&& value)
		{
			std::lock_guard lock(_mutex);
			if (FullLocked()) {
				return false;
			}
			_queue.push_back(std::move(value));
			return true;
		}

		// Resumable admission for scheduler workers. If the bounded active queue is
		// full, retain the item in FIFO order and promote it when consumers release
		// capacity. The producer never waits on a condition variable.
		bool PushDeferred(T value)
		{
			std::lock_guard lock(_mutex);
			if (!FullLocked() && _deferred.empty()) {
				_queue.push_back(std::move(value));
				return true;
			}
			_deferred.push_back(std::move(value));
			return false;
		}

		// Resumable admission for a whole batch under one lock. Items that do not
		// fit the bounded active queue are retained in FIFO order like PushDeferred.
		template <class It>
		void PushDeferredRange(It first, It last)
		{
			std::lock_guard lock(_mutex);
			for (; first != last; ++first) {
				if (!FullLocked() && _deferred.empty()) {
					_queue.push_back(std::move(*first));
				} else {
					_deferred.push_back(std::move(*first));
				}
			}
		}

		bool PushBlocking(T value, const std::atomic_bool& stopRequested)
		{
			std::unique_lock lock(_mutex);
			if (Bounded()) {
				_notFull.wait(lock, [this, &stopRequested]() {
					return stopRequested.load(std::memory_order_acquire) || !FullLocked();
				});
				if (stopRequested.load(std::memory_order_acquire)) {
					return false;
				}
			}
			_queue.push_back(std::move(value));
			return true;
		}

		bool TryPushFront(const T& value)
		{
			std::lock_guard lock(_mutex);
			if (FullLocked()) {
				return false;
			}
			_queue.push_front(value);
			return true;
		}

		bool TryPushFront(T&& value)
		{
			std::lock_guard lock(_mutex);
			if (FullLocked()) {
				return false;
			}
			_queue.push_front(std::move(value));
			return true;
		}

		bool TryPop(T& out)
		{
			std::lock_guard lock(_mutex);
			if (_queue.empty()) {
				return false;
			}
			out = std::move(_queue.front());
			_queue.pop_front();
			PromoteDeferredLocked();
			_notFull.notify_one();
			return true;
		}

		std::size_t Drain(std::vector<T>& out, std::size_t maxCount = std::numeric_limits<std::size_t>::max())
		{
			std::lock_guard lock(_mutex);
			const auto count = (std::min)(maxCount, _queue.size());
			out.reserve(out.size() + count);
			for (std::size_t i = 0; i < count; ++i) {
				out.push_back(std::move(_queue.front()));
				_queue.pop_front();
				PromoteDeferredLocked();
			}
			if (count != 0) {
				_notFull.notify_all();
			}
			return count;
		}

		void Clear()
		{
			std::lock_guard lock(_mutex);
			_queue.clear();
			_deferred.clear();
			_notFull.notify_all();
		}

		void NotifyAll()
		{
			_notFull.notify_all();
		}

		[[nodiscard]] std::size_t Size() const
		{
			std::lock_guard lock(_mutex);
			return _queue.size() + _deferred.size();
		}

		[[nodiscard]] bool Empty() const
		{
			std::lock_guard lock(_mutex);
			return _queue.empty() && _deferred.empty();
		}

		[[nodiscard]] std::size_t DeferredSize() const
		{
			std::lock_guard lock(_mutex);
			return _deferred.size();
		}

	private:
		[[nodiscard]] bool Bounded() const { return _capacity != 0; }
		[[nodiscard]] bool FullLocked() const {
			return !_deferred.empty() || (Bounded() && _queue.size() >= _capacity);
		}
		void PromoteDeferredLocked()
		{
			while (!_deferred.empty() && (!Bounded() || _queue.size() < _capacity)) {
				_queue.push_back(std::move(_deferred.front()));
				_deferred.pop_front();
			}
		}

		mutable std::mutex _mutex;
		std::condition_variable _notFull;
		std::deque<T> _queue;
		std::deque<T> _deferred;
		std::size_t _capacity{ 0 };
	};
}
