// A bounded, thread-safe MPMC queue built on std::mutex + std::condition_variable.
//
// Design notes
// ------------
// * Two condition variables, not one. A single cv would make every producer wake
//   on every push and every consumer wake on every pop ("thundering herd"),
//   which is exactly the kind of contention a data-acquisition pipeline cannot
//   afford. `not_empty_` is signalled after a push, `not_full_` after a pop.
// * Bounded by default. An unbounded queue turns a slow consumer into an
//   out-of-memory crash; a bounded queue turns it into back-pressure, which is
//   what a real acquisition front-end must do.
// * `close()` is the shutdown protocol: it unblocks every waiter, lets
//   consumers drain whatever is still queued, and only then makes `pop()`
//   return false. That is what lets the pipeline join its threads without
//   losing the frames still in flight.
// * The queue never calls user code while holding the lock.
// * Wake-ups are skipped when nobody is waiting (`waiting_*` counters). This is
//   not a micro-optimisation: it was measured to be the single biggest cost in
//   this pipeline. See docs/CPP_PERFORMANCE.md — on MinGW-w64/winpthreads
//   `notify_one()` costs a kernel transition even with zero waiters, so calling
//   it unconditionally once per item made the whole pipeline *slower* with every
//   extra worker thread.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace imu_lab {

/// Multi-producer / multi-consumer FIFO queue.
///
/// @tparam T value type; must be movable.
template <typename T>
class ThreadSafeQueue {
public:
    /// @param capacity maximum number of queued items; `0` means unbounded.
    explicit ThreadSafeQueue(std::size_t capacity = 0) : capacity_(capacity) {}

    ThreadSafeQueue(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;

    ~ThreadSafeQueue() { close(); }

    /// Append an item, blocking while a bounded queue is full.
    ///
    /// @return false when the queue has been closed, so producers can stop
    ///         cleanly instead of spinning.
    bool push(T value) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (is_full_locked() && !closed_) {
            ++waiting_not_full_;
            not_full_.wait(lock);
            --waiting_not_full_;
        }
        if (closed_) {
            return false;
        }
        queue_.push_back(std::move(value));
        if (queue_.size() > high_water_mark_) {
            high_water_mark_ = queue_.size();
        }
        // Read the waiter count while still holding the lock: a consumer that
        // arrives after we unlock will see the new item instead of waiting, so
        // there is no lost-wakeup window.
        const bool someone_is_waiting = waiting_not_empty_ != 0;
        lock.unlock();
        if (someone_is_waiting) {
            not_empty_.notify_one();
        }
        return true;
    }

    /// Remove the oldest item, blocking until one is available.
    ///
    /// @return false only when the queue is closed *and* drained.
    bool pop(T& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (queue_.empty() && !closed_) {
            ++waiting_not_empty_;
            not_empty_.wait(lock);
            --waiting_not_empty_;
        }
        if (queue_.empty()) {
            return false;  // closed and fully drained
        }
        out = std::move(queue_.front());
        queue_.pop_front();
        const bool producer_is_waiting = waiting_not_full_ != 0;
        lock.unlock();
        if (producer_is_waiting) {
            not_full_.notify_one();
        }
        return true;
    }

    /// Non-blocking variant.
    ///
    /// @return false when the queue is empty (whether or not it is closed).
    bool try_pop(T& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop_front();
        const bool producer_is_waiting = waiting_not_full_ != 0;
        if (producer_is_waiting) {
            not_full_.notify_one();
        }
        return true;
    }

    /// Signal that no further items will be produced.
    ///
    /// Already-queued items remain available to consumers.
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        // Shutdown must always wake everyone, waiters or not, because the
        // counters are only advisory and a missed wake here would deadlock.
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    [[nodiscard]] bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    [[nodiscard]] bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    /// Configured capacity; `0` means unbounded.
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    /// Largest depth ever observed — the "queue backlog" stability metric.
    [[nodiscard]] std::size_t high_water_mark() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return high_water_mark_;
    }

    /// True when the queue is at capacity (always false when unbounded).
    [[nodiscard]] bool full() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return is_full_locked();
    }

    /// Drop every queued item without closing the queue.
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        not_full_.notify_all();
    }

private:
    [[nodiscard]] bool is_full_locked() const {
        return capacity_ != 0 && queue_.size() >= capacity_;
    }

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<T> queue_;
    const std::size_t capacity_;
    bool closed_ = false;
    std::size_t high_water_mark_ = 0;
    //: Number of threads currently blocked in `wait()`. Guarded by `mutex_`.
    //: Used to skip pointless wake-ups.
    std::size_t waiting_not_empty_ = 0;
    std::size_t waiting_not_full_ = 0;
};

}  // namespace imu_lab
