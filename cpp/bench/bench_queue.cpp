// Queue micro-benchmark: imu_lab_bench_queue
//
// Isolates the ThreadSafeQueue so the pipeline's scaling behaviour can be
// attributed to a specific component instead of guessed at.
//
// What it measures
// ----------------
//   * single-threaded push/pop cost  -> the floor for any lock-based queue;
//   * 1 producer / N consumers       -> how the wake-up path behaves as the
//                                        number of blocked threads grows.
//
// Why it is committed rather than a throwaway
// -------------------------------------------
// The pipeline's throughput was measured to *fall* as worker threads are added
// on Windows/MinGW. Without this binary that looks like a bug in the pipeline;
// with it, the cause is localised to the queue's condition-variable wake-up,
// which is a platform characteristic that also needs re-measuring on Linux
// (the primary target) rather than assumed.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "imu_lab/thread_safe_queue.hpp"

namespace {

using imu_lab::ThreadSafeQueue;

double monotonic_seconds() {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

/// 1 producer / N consumers moving `items` through a bounded queue.
struct RunResult {
    double milliseconds = 0.0;
    double items_per_second = 0.0;
    std::size_t consumed = 0;
};

RunResult measure_pipeline(std::size_t consumers, std::size_t items, std::size_t capacity) {
    ThreadSafeQueue<std::size_t> queue(capacity);
    std::atomic<std::size_t> consumed{0};

    const double started = monotonic_seconds();

    std::vector<std::thread> threads;
    threads.reserve(consumers);
    for (std::size_t index = 0; index < consumers; ++index) {
        threads.emplace_back([&queue, &consumed] {
            std::size_t value = 0;
            while (queue.pop(value)) {
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (std::size_t index = 0; index < items; ++index) {
        queue.push(index);
    }
    queue.close();
    for (std::thread& thread : threads) {
        thread.join();
    }

    const double elapsed = monotonic_seconds() - started;
    RunResult result;
    result.milliseconds = elapsed * 1000.0;
    result.items_per_second = static_cast<double>(consumed.load()) / elapsed;
    result.consumed = consumed.load();
    return result;
}

/// Single-threaded baseline: no contention, no wake-ups.
RunResult measure_single_threaded(std::size_t items) {
    ThreadSafeQueue<std::size_t> queue(4096);
    const double started = monotonic_seconds();
    std::size_t value = 0;
    for (std::size_t index = 0; index < items; ++index) {
        queue.push(index);
        queue.try_pop(value);
    }
    const double elapsed = monotonic_seconds() - started;
    RunResult result;
    result.milliseconds = elapsed * 1000.0;
    result.items_per_second = static_cast<double>(2 * items) / elapsed;
    result.consumed = items;
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t items = 1000000;
    std::size_t capacity = 4096;

    for (int index = 1; index < argc; ++index) {
        const std::string token = argv[index];
        if (token == "--items" && index + 1 < argc) {
            items = std::strtoull(argv[++index], nullptr, 10);
        } else if (token == "--queue-capacity" && index + 1 < argc) {
            capacity = std::strtoull(argv[++index], nullptr, 10);
        } else if (token == "--help" || token == "-h") {
            std::cout << "imu_lab_bench_queue [--items N] [--queue-capacity N]\n";
            return 0;
        }
    }

    std::cout << "ThreadSafeQueue micro-benchmark: " << items << " items, queue capacity "
              << capacity << "\n\n";

    const RunResult baseline = measure_single_threaded(items);
    std::cout << std::left << std::setw(30) << "same thread push+try_pop" << std::right
              << std::setw(12) << std::fixed << std::setprecision(1) << baseline.milliseconds
              << " ms" << std::setw(18) << std::setprecision(0) << baseline.items_per_second
              << " ops/s\n\n";

    std::cout << std::left << std::setw(30) << "producer/consumer split" << std::right
              << std::setw(12) << "elapsed" << std::setw(18) << "items/s" << std::setw(12)
              << "consumed" << '\n';
    std::cout << std::string(72, '-') << '\n';

    int exit_code = 0;
    for (const std::size_t consumers : {std::size_t{1}, std::size_t{2}, std::size_t{4},
                                        std::size_t{8}}) {
        const RunResult result = measure_pipeline(consumers, items, capacity);
        std::cout << std::left << std::setw(30)
                  << ("1 producer / " + std::to_string(consumers) + " consumer(s)") << std::right
                  << std::setw(10) << std::setprecision(1) << result.milliseconds << " ms"
                  << std::setw(18) << std::setprecision(0) << result.items_per_second
                  << std::setw(12) << result.consumed << '\n';

        // Integrity is still the first-class requirement: a fast queue that
        // loses items is worthless.
        if (result.consumed != items) {
            std::cerr << "INTEGRITY FAILURE: consumed " << result.consumed << " of " << items
                      << '\n';
            exit_code = 1;
        }
    }

    std::cout << "\nNote: a lock-based queue with several blocked consumers pays a wake-up\n"
                 "      cost that is platform specific. Compare the numbers above on the\n"
                 "      target OS before drawing conclusions about scaling.\n";
    return exit_code;
}
