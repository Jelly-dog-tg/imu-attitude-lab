// Tests for the bounded MPMC queue: ordering, back-pressure, close semantics and
// a real concurrent producer/consumer integrity check.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <numeric>
#include <set>
#include <thread>
#include <vector>

#include "imu_lab/test_framework.hpp"
#include "imu_lab/thread_safe_queue.hpp"

namespace {

using imu_lab::ThreadSafeQueue;

}  // namespace

IMU_TEST(ThreadSafeQueue, PreservesFifoOrder) {
    ThreadSafeQueue<int> queue;
    for (int value = 0; value < 5; ++value) {
        CHECK(queue.push(value));
    }
    CHECK_EQ(queue.size(), static_cast<std::size_t>(5));

    for (int expected = 0; expected < 5; ++expected) {
        int value = -1;
        CHECK(queue.pop(value));
        CHECK_EQ(value, expected);
    }
    CHECK(queue.empty());
}

IMU_TEST(ThreadSafeQueue, StartsEmpty) {
    const ThreadSafeQueue<int> queue;
    CHECK(queue.empty());
    CHECK_EQ(queue.size(), static_cast<std::size_t>(0));
    CHECK_EQ(queue.high_water_mark(), static_cast<std::size_t>(0));
    CHECK(!queue.closed());
}

IMU_TEST(ThreadSafeQueue, TryPopDoesNotBlockOnEmptyQueue) {
    ThreadSafeQueue<int> queue;
    int value = 0;
    CHECK(!queue.try_pop(value));

    CHECK(queue.push(7));
    CHECK(queue.try_pop(value));
    CHECK_EQ(value, 7);
    CHECK(!queue.try_pop(value));
}

IMU_TEST(ThreadSafeQueue, PopReturnsFalseOnceClosedAndDrained) {
    ThreadSafeQueue<int> queue;
    CHECK(queue.push(1));
    CHECK(queue.push(2));
    queue.close();
    CHECK(queue.closed());

    int value = 0;
    // Already-queued items must still be deliverable: closing is a shutdown
    // signal, not a discard.
    CHECK(queue.pop(value));
    CHECK_EQ(value, 1);
    CHECK(queue.pop(value));
    CHECK_EQ(value, 2);
    CHECK(!queue.pop(value));
}

IMU_TEST(ThreadSafeQueue, PushFailsAfterClose) {
    ThreadSafeQueue<int> queue;
    queue.close();
    CHECK(!queue.push(1));
}

IMU_TEST(ThreadSafeQueue, TracksHighWaterMark) {
    ThreadSafeQueue<int> queue;
    for (int value = 0; value < 10; ++value) {
        CHECK(queue.push(value));
    }
    CHECK_EQ(queue.high_water_mark(), static_cast<std::size_t>(10));

    int drained = 0;
    for (int index = 0; index < 6; ++index) {
        CHECK(queue.try_pop(drained));
    }
    CHECK_EQ(queue.size(), static_cast<std::size_t>(4));
    // The high-water mark is a peak, so it must not shrink.
    CHECK_EQ(queue.high_water_mark(), static_cast<std::size_t>(10));

    for (int value = 0; value < 2; ++value) {
        CHECK(queue.push(value));
    }
    CHECK_EQ(queue.high_water_mark(), static_cast<std::size_t>(10));
}

IMU_TEST(ThreadSafeQueue, BoundedQueueReportsFull) {
    ThreadSafeQueue<int> queue(2);
    CHECK_EQ(queue.capacity(), static_cast<std::size_t>(2));
    CHECK(queue.push(1));
    CHECK(queue.push(2));
    CHECK(queue.full());
    CHECK(!queue.empty());

    int value = 0;
    CHECK(queue.try_pop(value));
    CHECK(!queue.full());
}

IMU_TEST(ThreadSafeQueue, BoundedQueueAppliesBackPressure) {
    ThreadSafeQueue<int> queue(2);
    CHECK(queue.push(1));
    CHECK(queue.push(2));

    // Assertions must never run on a worker thread (a thrown AssertionFailure
    // there would call std::terminate), so the result travels through an atomic.
    std::atomic<bool> push_returned{false};
    std::atomic<bool> push_result{false};
    std::thread producer([&] {
        push_result.store(queue.push(3));
        push_returned.store(true);
    });

    // Give the producer time to reach the blocking wait.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!push_returned.load());  // it must be blocked while the queue is full

    int value = 0;
    CHECK(queue.pop(value));
    producer.join();
    CHECK(push_returned.load());
    CHECK(push_result.load());
}

IMU_TEST(ThreadSafeQueue, ClosingUnblocksABlockedProducer) {
    ThreadSafeQueue<int> queue(1);
    CHECK(queue.push(1));

    std::atomic<bool> push_returned{false};
    std::atomic<bool> push_result{true};
    std::thread producer([&] {
        push_result.store(queue.push(2));
        push_returned.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!push_returned.load());
    queue.close();
    producer.join();
    // Shutdown must never deadlock a producer waiting for space.
    CHECK(push_returned.load());
    CHECK(!push_result.load());
}

IMU_TEST(ThreadSafeQueue, ClosingUnblocksABlockedConsumer) {
    ThreadSafeQueue<int> queue;

    std::atomic<bool> popped{true};
    std::thread consumer([&] {
        int value = 0;
        popped.store(queue.pop(value));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    queue.close();
    consumer.join();
    CHECK(!popped.load());
}

IMU_TEST(ThreadSafeQueue, ClearDropsQueuedItemsWithoutClosing) {
    ThreadSafeQueue<int> queue;
    for (int value = 0; value < 4; ++value) {
        CHECK(queue.push(value));
    }
    queue.clear();
    CHECK(queue.empty());
    CHECK(!queue.closed());
    CHECK(queue.push(99));
    CHECK_EQ(queue.size(), static_cast<std::size_t>(1));
}

IMU_TEST(ThreadSafeQueue, ConcurrentProducersAndConsumersLoseNothing) {
    // The integrity property that actually matters for an acquisition pipeline:
    // every produced item is consumed exactly once.
    constexpr int kProducers = 4;
    constexpr int kConsumers = 3;
    constexpr int kPerProducer = 2000;
    constexpr int kTotal = kProducers * kPerProducer;

    ThreadSafeQueue<int> queue(64);  // small on purpose: forces back-pressure
    std::atomic<int> consumed{0};
    std::atomic<long long> checksum{0};

    std::vector<std::thread> consumers;
    consumers.reserve(kConsumers);
    for (int index = 0; index < kConsumers; ++index) {
        consumers.emplace_back([&] {
            int value = 0;
            while (queue.pop(value)) {
                checksum.fetch_add(value);
                consumed.fetch_add(1);
            }
        });
    }

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int producer = 0; producer < kProducers; ++producer) {
        producers.emplace_back([&, producer] {
            for (int index = 0; index < kPerProducer; ++index) {
                queue.push(producer * kPerProducer + index + 1);
            }
        });
    }

    for (std::thread& thread : producers) {
        thread.join();
    }
    queue.close();
    for (std::thread& thread : consumers) {
        thread.join();
    }

    CHECK_EQ(consumed.load(), kTotal);
    CHECK_EQ(queue.size(), static_cast<std::size_t>(0));
    // 1 + 2 + ... + kTotal is the expected sum: no duplicates, no losses.
    const long long expected =
        static_cast<long long>(kTotal) * static_cast<long long>(kTotal + 1) / 2LL;
    CHECK_EQ(checksum.load(), expected);
}

IMU_TEST(ThreadSafeQueue, HighWaterMarkNeverExceedsCapacity) {
    ThreadSafeQueue<int> queue(8);
    std::atomic<bool> stop{false};

    std::thread consumer([&] {
        int value = 0;
        while (!stop.load()) {
            if (!queue.try_pop(value)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        while (queue.try_pop(value)) {
        }
    });

    for (int value = 0; value < 5000; ++value) {
        CHECK(queue.push(value));
    }
    queue.close();
    stop.store(true);
    consumer.join();

    CHECK(queue.high_water_mark() <= queue.capacity());
}
