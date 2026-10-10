// End-to-end tests for the multithreaded pipeline.
//
// These run entirely offline: the simulated source needs no hardware, which is
// the property that makes the concurrency code testable in CI.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include "imu_lab/csv_reader.hpp"
#include "imu_lab/error.hpp"
#include "imu_lab/frame_source.hpp"
#include "imu_lab/pipeline.hpp"
#include "imu_lab/test_framework.hpp"
#include "test_support.hpp"

namespace {

using imu_lab::AttitudePipeline;
using imu_lab::CsvReader;
using imu_lab::ErrorCode;
using imu_lab::FrameSource;
using imu_lab::PipelineConfig;
using imu_lab::PipelineRun;
using imu_lab::SourceConfig;
using imu_lab::SourceKind;
using imu_lab::test::ScratchDir;

SourceConfig simulated(std::size_t frames = 1000, double hz = 200.0) {
    SourceConfig config;
    config.kind = SourceKind::simulated;
    config.frames = frames;
    config.sample_rate_hz = hz;
    config.seed = 7;
    return config;
}

/// Run the pipeline over a freshly built simulated source.
PipelineRun run_simulated(const SourceConfig& source_config, const PipelineConfig& pipeline_config,
                          const std::string& csv_path = {}) {
    auto source = imu_lab::make_frame_source(source_config);
    CHECK(source);
    AttitudePipeline pipeline(pipeline_config);
    return pipeline.run(*source.value(), csv_path);
}

void check_accounting(const PipelineRun& run) {
    CHECK(run.status.ok());
    CHECK_EQ(run.stats.lost_frames(), static_cast<std::uint64_t>(0));
    CHECK_EQ(run.stats.frames_received,
             run.stats.frames_parsed + run.stats.frames_rejected);
    CHECK_EQ(run.stats.frames_logged, run.stats.frames_received);
    CHECK_EQ(run.stats.pitch.count, run.stats.frames_parsed);
}

}  // namespace

// --------------------------------------------------------------------------- //
// Happy path
// --------------------------------------------------------------------------- //
IMU_TEST(Pipeline, ProcessesEverySimulatedFrame) {
    const PipelineRun run = run_simulated(simulated(2000), PipelineConfig{});
    check_accounting(run);
    CHECK_EQ(run.stats.frames_received, static_cast<std::uint64_t>(2000));
    CHECK_EQ(run.stats.frames_parsed, static_cast<std::uint64_t>(2000));
    CHECK_EQ(run.stats.frames_rejected, static_cast<std::uint64_t>(0));
    CHECK_NEAR(run.stats.anomaly_ratio(), 0.0, 0.0);
    CHECK(run.stats.wall_ms > 0.0);
    CHECK(run.stats.pitch.count == 2000u);
    CHECK(run.stats.roll.count == 2000u);
}

IMU_TEST(Pipeline, ComputesPlausibleAngleStatistics) {
    // The simulated signal is 8 deg amplitude on pitch and 15 deg on roll.
    const PipelineRun run = run_simulated(simulated(4000, 200.0), PipelineConfig{});
    check_accounting(run);
    CHECK(run.stats.pitch.maximum <= 9.0);
    CHECK(run.stats.pitch.minimum >= -9.0);
    CHECK(run.stats.roll.maximum <= 16.0);
    CHECK(run.stats.roll.minimum >= -16.0);
    CHECK(run.stats.pitch.stddev > 0.0);
    CHECK(run.stats.roll.stddev > 0.0);
    // Roll swings wider than pitch in this generator.
    CHECK(run.stats.roll.stddev > run.stats.pitch.stddev);
}

IMU_TEST(Pipeline, RecordsQueueAndLatencyMetrics) {
    PipelineConfig config;
    config.queue_capacity = 64;
    const PipelineRun run = run_simulated(simulated(4000), config);
    check_accounting(run);
    CHECK_EQ(run.stats.queue_capacity, static_cast<std::size_t>(64));
    CHECK(run.stats.queue_high_water <= static_cast<std::size_t>(64));
    CHECK_EQ(run.stats.parse_latency_ms.count(), run.stats.frames_received);
    CHECK_EQ(run.stats.end_to_end_latency_ms.count(), run.stats.frames_received);
    CHECK(run.stats.parse_latency_ms.max_ms() >= run.stats.parse_latency_ms.min_ms());
}

IMU_TEST(Pipeline, SamplesResidentMemory) {
    const PipelineRun run = run_simulated(simulated(20000), PipelineConfig{});
    check_accounting(run);
#if defined(_WIN32) || defined(__linux__) || defined(__unix__)
    CHECK(run.stats.rss_start_bytes > static_cast<std::size_t>(0));
    CHECK(run.stats.rss_peak_bytes >= run.stats.rss_start_bytes);
#endif
}

// --------------------------------------------------------------------------- //
// Determinism and thread independence
// --------------------------------------------------------------------------- //
IMU_TEST(Pipeline, IsDeterministicForTheSameSeed) {
    const PipelineRun first = run_simulated(simulated(3000), PipelineConfig{});
    const PipelineRun second = run_simulated(simulated(3000), PipelineConfig{});
    check_accounting(first);
    check_accounting(second);
    CHECK_EQ(first.stats.frames_received, second.stats.frames_received);
    CHECK_EQ(first.stats.frames_rejected, second.stats.frames_rejected);
    // Bit-identical, because the logger restores sequence order before feeding
    // the accumulator regardless of which worker finished first.
    CHECK_NEAR(first.stats.pitch.mean, second.stats.pitch.mean, 0.0);
    CHECK_NEAR(first.stats.roll.mean, second.stats.roll.mean, 0.0);
    CHECK_NEAR(first.stats.pitch.stddev, second.stats.pitch.stddev, 0.0);
}

IMU_TEST(Pipeline, WorkerCountDoesNotChangeTheResult) {
    PipelineConfig single;
    single.worker_threads = 1;
    PipelineConfig many;
    many.worker_threads = 4;

    const PipelineRun one_worker = run_simulated(simulated(5000), single);
    const PipelineRun four_workers = run_simulated(simulated(5000), many);
    check_accounting(one_worker);
    check_accounting(four_workers);

    CHECK_EQ(one_worker.stats.frames_received, four_workers.stats.frames_received);
    CHECK_EQ(one_worker.stats.frames_parsed, four_workers.stats.frames_parsed);
    CHECK_EQ(one_worker.stats.frames_rejected, four_workers.stats.frames_rejected);
    CHECK_NEAR(one_worker.stats.pitch.mean, four_workers.stats.pitch.mean, 0.0);
    CHECK_NEAR(one_worker.stats.roll.mean, four_workers.stats.roll.mean, 0.0);
    CHECK_NEAR(one_worker.stats.pitch.stddev, four_workers.stats.pitch.stddev, 0.0);
    CHECK_EQ(four_workers.stats.workers, static_cast<std::size_t>(4));
}

IMU_TEST(Pipeline, ZeroWorkersIsTreatedAsOne) {
    PipelineConfig config;
    config.worker_threads = 0;
    auto source = imu_lab::make_frame_source(simulated(100));
    CHECK(source);
    AttitudePipeline pipeline(config);
    const PipelineRun run = pipeline.run(*source.value());
    CHECK(run.status.ok());
    CHECK_EQ(run.stats.workers, static_cast<std::size_t>(1));
    CHECK_EQ(run.stats.frames_received, static_cast<std::uint64_t>(100));
}

IMU_TEST(Pipeline, SurvivesAnExtremelySmallQueue) {
    // Capacity 1 maximises lock contention and back-pressure: the integrity
    // check must still hold.
    PipelineConfig config;
    config.queue_capacity = 1;
    config.output_queue_capacity = 1;
    config.worker_threads = 3;

    const PipelineRun run = run_simulated(simulated(3000), config);
    check_accounting(run);
    CHECK_EQ(run.stats.frames_received, static_cast<std::uint64_t>(3000));
    CHECK(run.stats.queue_high_water <= static_cast<std::size_t>(1));
}

IMU_TEST(Pipeline, HandlesUnboundedQueues) {
    PipelineConfig config;
    config.queue_capacity = 0;
    config.output_queue_capacity = 0;
    const PipelineRun run = run_simulated(simulated(2000), config);
    check_accounting(run);
    CHECK_NEAR(run.stats.backpressure_ratio(), 0.0, 0.0);
}

// --------------------------------------------------------------------------- //
// Fault injection
// --------------------------------------------------------------------------- //
IMU_TEST(Pipeline, CountsInjectedAnomalies) {
    SourceConfig source_config = simulated(4000);
    source_config.anomaly_rate = 0.10;

    const PipelineRun run = run_simulated(source_config, PipelineConfig{});
    check_accounting(run);
    CHECK_EQ(run.stats.frames_parsed + run.stats.frames_rejected,
             static_cast<std::uint64_t>(4000));
    CHECK(run.stats.frames_rejected > static_cast<std::uint64_t>(200));
    CHECK(run.stats.frames_rejected < static_cast<std::uint64_t>(700));
    // Roughly 10 %, with generous bounds so the test is not seed-fragile.
    CHECK(run.stats.anomaly_ratio() > 0.05);
    CHECK(run.stats.anomaly_ratio() < 0.17);
    CHECK(!run.sample_rejections.empty());
}

IMU_TEST(Pipeline, DetectsAnInjectedAngleStep) {
    SourceConfig source_config = simulated(600);
    source_config.injected_step_deg = 60.0;  // well above the 30 deg default

    const PipelineRun run = run_simulated(source_config, PipelineConfig{});
    check_accounting(run);
    CHECK(run.stats.jump_count >= static_cast<std::size_t>(1));
    CHECK(!run.jumps.empty());
    CHECK(run.jumps.front().pitch_jump);
    CHECK_NEAR(run.jumps.front().pitch_delta_deg, 60.0, 1.0);
}

IMU_TEST(Pipeline, DetectsNoJumpsInASmoothSignal) {
    const PipelineRun run = run_simulated(simulated(2000), PipelineConfig{});
    check_accounting(run);
    CHECK_EQ(run.stats.jump_count, static_cast<std::size_t>(0));
    CHECK(run.jumps.empty());
}

IMU_TEST(Pipeline, HonoursACustomJumpThreshold) {
    SourceConfig source_config = simulated(600);
    source_config.injected_step_deg = 5.0;

    PipelineConfig strict;
    strict.jump_threshold_deg = 1.0;
    const PipelineRun sensitive = run_simulated(source_config, strict);
    CHECK(sensitive.stats.jump_count >= static_cast<std::size_t>(1));

    PipelineConfig relaxed;
    relaxed.jump_threshold_deg = 45.0;
    const PipelineRun tolerant = run_simulated(source_config, relaxed);
    CHECK_EQ(tolerant.stats.jump_count, static_cast<std::size_t>(0));
}

IMU_TEST(Pipeline, CapsTheNumberOfStoredJumpEvents) {
    SourceConfig source_config = simulated(4000);
    source_config.injected_step_deg = 80.0;

    PipelineConfig config;
    config.jump_cap = 1;
    config.jump_threshold_deg = 1.0;  // flag almost everything
    const PipelineRun run = run_simulated(source_config, config);
    CHECK(run.jumps.size() <= static_cast<std::size_t>(1));
    // The counter still reflects every event even though the vector is capped.
    CHECK(run.stats.jump_count >= run.jumps.size());
}

// --------------------------------------------------------------------------- //
// CSV output
// --------------------------------------------------------------------------- //
IMU_TEST(Pipeline, WritesACsvThatReloadsWithTheSameRowCount) {
    ScratchDir scratch;
    const std::string csv_path = scratch.file("out.csv").string();
    const PipelineRun run = run_simulated(simulated(1500), PipelineConfig{}, csv_path);
    check_accounting(run);
    CHECK_EQ(run.csv_path, csv_path);

    const auto reloaded = CsvReader::load(csv_path);
    CHECK(reloaded);
    CHECK_EQ(reloaded.value().records.size(), static_cast<std::size_t>(1500));
    CHECK_EQ(reloaded.value().skipped_rows, static_cast<std::size_t>(0));
    CHECK(reloaded.value().report.ok());

    // The values must survive the round trip: recompute the extrema from the
    // reloaded file and compare them with what the pipeline reported.
    double pitch_min = reloaded.value().records.front().sample.pitch_deg;
    double pitch_max = pitch_min;
    double roll_min = reloaded.value().records.front().sample.roll_deg;
    double roll_max = roll_min;
    for (const imu_lab::CsvRecord& record : reloaded.value().records) {
        pitch_min = std::min(pitch_min, record.sample.pitch_deg);
        pitch_max = std::max(pitch_max, record.sample.pitch_deg);
        roll_min = std::min(roll_min, record.sample.roll_deg);
        roll_max = std::max(roll_max, record.sample.roll_deg);
    }
    CHECK_NEAR(pitch_min, run.stats.pitch.minimum, 1e-5);
    CHECK_NEAR(pitch_max, run.stats.pitch.maximum, 1e-5);
    CHECK_NEAR(roll_min, run.stats.roll.minimum, 1e-5);
    CHECK_NEAR(roll_max, run.stats.roll.maximum, 1e-5);
}

IMU_TEST(Pipeline, WritesCsvRawLineQuotedSoItReloads) {
    // raw_line contains a comma, so the C++ writer must quote it exactly like
    // Python's csv module or the reload would see extra columns.
    ScratchDir scratch;
    const std::string csv_path = scratch.file("raw.csv").string();
    const PipelineRun run = run_simulated(simulated(50), PipelineConfig{}, csv_path);
    check_accounting(run);

    const std::string content = imu_lab::test::read_file(csv_path);
    CHECK(content.find("\"P:") != std::string::npos);

    const auto reloaded = CsvReader::load(csv_path);
    CHECK(reloaded);
    CHECK_EQ(reloaded.value().records.size(), static_cast<std::size_t>(50));
    CHECK_EQ(reloaded.value().skipped_rows, static_cast<std::size_t>(0));
}

IMU_TEST(Pipeline, WritesCsvHeaderEvenForAnEmptySource) {
    ScratchDir scratch;
    const std::string csv_path = scratch.file("empty.csv").string();
    const PipelineRun run = run_simulated(simulated(0), PipelineConfig{}, csv_path);
    CHECK(run.status.ok());
    CHECK_EQ(run.stats.frames_received, static_cast<std::uint64_t>(0));

    const std::string content = imu_lab::test::read_file(csv_path);
    CHECK(content.find("timestamp_utc,elapsed_s,pitch_deg,roll_deg") == 0u);
}

IMU_TEST(Pipeline, CsvTimestampUsesIso8601Utc) {
    ScratchDir scratch;
    const std::string csv_path = scratch.file("ts.csv").string();
    const PipelineRun run = run_simulated(simulated(10), PipelineConfig{}, csv_path);
    check_accounting(run);

    const std::string content = imu_lab::test::read_file(csv_path);
    const std::size_t first_newline = content.find('\n');
    const std::size_t second_newline = content.find('\n', first_newline + 1);
    const std::string first_row = content.substr(first_newline + 1, second_newline - first_newline - 1);
    // e.g. 2026-10-10T11:22:33.456Z
    CHECK_EQ(first_row.substr(4, 1), std::string("-"));
    CHECK_EQ(first_row.substr(10, 1), std::string("T"));
    CHECK(first_row.find('Z') != std::string::npos);
}

// --------------------------------------------------------------------------- //
// Error handling
// --------------------------------------------------------------------------- //
IMU_TEST(Pipeline, RejectsAnInvalidJumpThreshold) {
    PipelineConfig config;
    config.jump_threshold_deg = -5.0;
    const PipelineRun run = run_simulated(simulated(10), config);
    CHECK(!run.status.ok());
    CHECK(run.status.error().message.find("threshold") != std::string::npos);
    CHECK_EQ(run.stats.frames_received, static_cast<std::uint64_t>(0));
}

IMU_TEST(Pipeline, ReportsAnUnwritableOutputPath) {
    ScratchDir scratch;
    const std::string bad_path = scratch.file("no_such_directory").string() + "/out.csv";
    const PipelineRun run = run_simulated(simulated(10), PipelineConfig{}, bad_path);
    CHECK(!run.status.ok());
    CHECK_EQ(run.status.error().code, ErrorCode::file_not_writable);
}

IMU_TEST(Pipeline, MissingTextFileSourceIsReported) {
    ScratchDir scratch;
    SourceConfig config;
    config.kind = SourceKind::text_file;
    config.path = scratch.missing("nope.txt");

    const auto source = imu_lab::make_frame_source(config);
    CHECK(!source);
    CHECK_EQ(source.error().code, ErrorCode::file_not_found);
}

IMU_TEST(Pipeline, TextFileSourceWithoutPathIsRejected) {
    SourceConfig config;
    config.kind = SourceKind::text_file;
    config.path.clear();
    const auto source = imu_lab::make_frame_source(config);
    CHECK(!source);
    CHECK_EQ(source.error().code, ErrorCode::unsupported_source);
}

// --------------------------------------------------------------------------- //
// Text-file replay
// --------------------------------------------------------------------------- //
IMU_TEST(Pipeline, ReplaysACapturedTextStream) {
    ScratchDir scratch;
    std::string content;
    for (int index = 0; index < 50; ++index) {
        content += "P:" + std::to_string(index) + ".5,R:" + std::to_string(-index) + ".25\r\n";
    }
    // Two deliberate corrupt frames in the middle of the capture.
    content += "P:garbage,R:1\r\n";
    content += "P:1,R:2,Z:3\r\n";

    const std::string path = scratch.write("capture.txt", content);
    SourceConfig source_config;
    source_config.kind = SourceKind::text_file;
    source_config.path = path;
    source_config.sample_rate_hz = 100.0;

    const PipelineRun run = run_simulated(source_config, PipelineConfig{});
    check_accounting(run);
    CHECK_EQ(run.stats.frames_received, static_cast<std::uint64_t>(52));
    CHECK_EQ(run.stats.frames_parsed, static_cast<std::uint64_t>(50));
    CHECK_EQ(run.stats.frames_rejected, static_cast<std::uint64_t>(2));
    CHECK(run.stats.anomaly_ratio() > 0.0);
}

IMU_TEST(Pipeline, TextFileReplayAssignsMonotonicElapsedTimes) {
    ScratchDir scratch;
    std::string content;
    for (int index = 0; index < 20; ++index) {
        content += "P:1.0,R:2.0\n";
    }
    const std::string path = scratch.write("mono.txt", content);

    SourceConfig source_config;
    source_config.kind = SourceKind::text_file;
    source_config.path = path;
    source_config.sample_rate_hz = 50.0;

    const PipelineRun run = run_simulated(source_config, PipelineConfig{});
    check_accounting(run);
    // 20 samples at 50 Hz span 0.38 s.
    CHECK_NEAR(run.stats.pitch.mean, 1.0, 1e-12);
    CHECK_NEAR(run.stats.roll.mean, 2.0, 1e-12);
    CHECK_NEAR(run.stats.pitch.stddev, 0.0, 0.0);
}

// --------------------------------------------------------------------------- //
// Shutdown
// --------------------------------------------------------------------------- //
IMU_TEST(Pipeline, RequestStopEndsTheRunEarlyWithoutLosingFrames) {
    auto source = imu_lab::make_frame_source(simulated(500000, 1000.0));
    CHECK(source);

    AttitudePipeline pipeline(PipelineConfig{});
    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        pipeline.request_stop();
    });

    const PipelineRun run = pipeline.run(*source.value());
    stopper.join();

    // Stopping early is not an error, but fewer frames should have gone through.
    CHECK(run.status.ok());
    CHECK_EQ(run.stats.lost_frames(), static_cast<std::uint64_t>(0));
    CHECK(run.stats.frames_received < static_cast<std::uint64_t>(500000));
    CHECK(run.stats.frames_received > static_cast<std::uint64_t>(0));
    CHECK_EQ(run.stats.frames_logged, run.stats.frames_received);
}

IMU_TEST(Pipeline, CanBeRunTwiceWithTheSameInstance) {
    auto source = imu_lab::make_frame_source(simulated(500));
    CHECK(source);

    AttitudePipeline pipeline(PipelineConfig{});
    const PipelineRun first = pipeline.run(*source.value());
    auto second_source = imu_lab::make_frame_source(simulated(500));
    CHECK(second_source);
    const PipelineRun second = pipeline.run(*second_source.value());

    check_accounting(first);
    check_accounting(second);
    CHECK_EQ(first.stats.frames_received, second.stats.frames_received);
    CHECK_NEAR(first.stats.pitch.mean, second.stats.pitch.mean, 0.0);
}
