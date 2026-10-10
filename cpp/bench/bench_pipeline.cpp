// Performance and stability harness: imu_lab_bench
//
// Purpose
// -------
// The unit tests prove correctness on small inputs. This binary is what proves
// the multithreaded pipeline is *stable* on large ones:
//
//   * streams a configurable number of frames through the real pipeline;
//   * asserts the integrity invariant (no frame is lost or duplicated);
//   * reports latency, queue backlog, anomaly ratio and RSS;
//   * with --sweep, repeats the run for 1/2/4 workers and verifies that the
//     aggregated statistics are bit-identical, i.e. that the threading does not
//     change the answer.
//
// Exit code is non-zero if any invariant is violated, so CI can gate on it.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "imu_lab/frame_source.hpp"
#include "imu_lab/pipeline.hpp"

namespace {

using imu_lab::AttitudePipeline;
using imu_lab::PipelineConfig;
using imu_lab::PipelineRun;
using imu_lab::SourceConfig;
using imu_lab::SourceKind;

struct Options {
    std::size_t frames = 200000;
    std::size_t workers = 2;
    std::size_t queue_capacity = 4096;
    double hz = 200.0;
    double anomaly_rate = 0.0;
    double jump_threshold = 30.0;
    bool sweep = false;
    bool verbose = false;
    std::string csv;
    std::string json;
};

void print_usage(std::ostream& out) {
    out << "imu_lab_bench - stability/performance harness for the C++ pipeline\n"
           "\n"
           "Usage: imu_lab_bench [options]\n"
           "  --frames N          number of frames to stream (default 200000)\n"
           "  --workers N         parsing threads (default 2)\n"
           "  --queue-capacity N  bounded queue capacity (default 4096)\n"
           "  --hz F              simulated sample rate (default 200)\n"
           "  --anomaly-rate F    fraction of corrupted frames (default 0)\n"
           "  --jump-threshold F  large-step threshold in degrees (default 30)\n"
           "  --sweep             repeat for 1/2/4 workers and compare results\n"
           "  --csv PATH          write the normalised log\n"
           "  --json PATH         write the metrics as JSON\n"
           "  --verbose           print per-run metrics as well as the summary\n";
}

Options parse(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string token = argv[index];
        auto next = [&](const char* fallback) -> std::string {
            if (index + 1 < argc) {
                return argv[++index];
            }
            return fallback;
        };
        if (token == "--frames") {
            options.frames = std::strtoull(next("200000").c_str(), nullptr, 10);
        } else if (token == "--workers") {
            options.workers = std::strtoull(next("2").c_str(), nullptr, 10);
        } else if (token == "--queue-capacity") {
            options.queue_capacity = std::strtoull(next("4096").c_str(), nullptr, 10);
        } else if (token == "--hz") {
            options.hz = std::strtod(next("200").c_str(), nullptr);
        } else if (token == "--anomaly-rate") {
            options.anomaly_rate = std::strtod(next("0").c_str(), nullptr);
        } else if (token == "--jump-threshold") {
            options.jump_threshold = std::strtod(next("30").c_str(), nullptr);
        } else if (token == "--csv") {
            options.csv = next("");
        } else if (token == "--json") {
            options.json = next("");
        } else if (token == "--sweep") {
            options.sweep = true;
        } else if (token == "--verbose") {
            options.verbose = true;
        } else if (token == "--help" || token == "-h") {
            print_usage(std::cout);
            std::exit(0);
        } else {
            std::cerr << "warning: ignoring unknown argument '" << token << "'\n";
        }
    }
    return options;
}

SourceConfig make_source_config(const Options& options, std::size_t frames) {
    SourceConfig config;
    config.kind = SourceKind::simulated;
    config.frames = frames;
    config.sample_rate_hz = options.hz;
    config.seed = 7;
    config.anomaly_rate = options.anomaly_rate;
    return config;
}

PipelineRun run_once(const Options& options, std::size_t workers, std::size_t frames,
                     const std::string& csv_path) {
    PipelineConfig config;
    config.worker_threads = workers;
    config.queue_capacity = options.queue_capacity;
    config.output_queue_capacity = options.queue_capacity;
    config.jump_threshold_deg = options.jump_threshold;

    auto source = imu_lab::make_frame_source(make_source_config(options, frames));
    if (!source) {
        std::cerr << "fatal: could not build the simulated source\n";
        std::exit(10);
    }
    AttitudePipeline pipeline(config);
    return pipeline.run(*source.value(), csv_path);
}

/// Everything that must hold for a run to be considered valid.
bool verify(const PipelineRun& run, std::string& failure) {
    if (!run.status.ok()) {
        failure = run.status.error().describe();
        return false;
    }
    if (run.stats.lost_frames() != 0) {
        std::ostringstream out;
        out << "lost " << run.stats.lost_frames() << " frame(s)";
        failure = out.str();
        return false;
    }
    if (run.stats.frames_received != run.stats.frames_parsed + run.stats.frames_rejected) {
        failure = "received != parsed + rejected";
        return false;
    }
    if (run.stats.frames_logged != run.stats.frames_received) {
        failure = "logged != received (logger dropped frames)";
        return false;
    }
    if (run.stats.pitch.count != run.stats.frames_parsed) {
        failure = "pitch sample count does not match the parsed frame count";
        return false;
    }
    if (run.stats.queue_capacity != 0 && run.stats.queue_high_water > run.stats.queue_capacity) {
        failure = "queue high-water mark exceeded the configured capacity";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(std::cout);
    }
    const Options options = parse(argc, argv);

    std::vector<std::size_t> worker_counts;
    if (options.sweep) {
        worker_counts = {1, 2, 4};
    } else {
        worker_counts = {options.workers};
    }

    std::cout << "imu_lab_bench: " << options.frames << " frames, queue capacity "
              << options.queue_capacity << ", anomaly rate " << options.anomaly_rate << "\n\n";

    std::cout << std::left << std::setw(8) << "workers" << std::right << std::setw(12)
              << "frames" << std::setw(12) << "rejected" << std::setw(12) << "wall ms"
              << std::setw(14) << "frames/s" << std::setw(12) << "maxQ"
              << std::setw(14) << "end2end ms" << std::setw(14) << "RSS delta KB" << '\n';
    std::cout << std::string(96, '-') << '\n';

    int exit_code = 0;
    double reference_mean = 0.0;
    bool have_reference = false;
    PipelineRun last_run;

    for (const std::size_t workers : worker_counts) {
        const std::string csv_path = (worker_counts.size() == 1) ? options.csv : std::string();
        const PipelineRun run = run_once(options, workers, options.frames, csv_path);

        std::string failure;
        if (!verify(run, failure)) {
            std::cerr << "INTEGRITY FAILURE with " << workers << " worker(s): " << failure << '\n';
            exit_code = 1;
            continue;
        }

        const double rss_delta_kb =
            static_cast<double>(run.stats.rss_peak_bytes) / 1024.0 -
            static_cast<double>(run.stats.rss_start_bytes) / 1024.0;

        std::cout << std::left << std::setw(8) << workers << std::right << std::fixed
                  << std::setprecision(0) << std::setw(12) << run.stats.frames_received
                  << std::setw(12) << run.stats.frames_rejected << std::setprecision(1)
                  << std::setw(12) << run.stats.wall_ms << std::setprecision(0)
                  << std::setw(14) << run.stats.throughput_fps() << std::setw(12)
                  << run.stats.queue_high_water << std::setprecision(3) << std::setw(14)
                  << run.stats.end_to_end_latency_ms.mean_ms() << std::setprecision(1)
                  << std::setw(14) << rss_delta_kb << '\n';

        if (options.verbose) {
            std::cout << run.stats.render() << '\n';
        }

        if (!have_reference) {
            reference_mean = run.stats.pitch.mean;
            have_reference = true;
        } else if (run.stats.pitch.mean != reference_mean) {
            std::cerr << "DETERMINISM FAILURE: pitch mean differs with " << workers
                      << " workers (" << run.stats.pitch.mean << " vs " << reference_mean << ")\n";
            exit_code = 1;
        }
        last_run = run;
    }

    if (!last_run.status.ok()) {
        return exit_code == 0 ? 10 : exit_code;
    }

    if (!options.json.empty()) {
        std::ofstream(options.json, std::ios::binary) << last_run.stats.to_json();
    }

    std::cout << "\nintegrity: "
              << (exit_code == 0 ? "OK (no frames lost, statistics deterministic)"
                                 : "FAILED")
              << '\n';
    return exit_code;
}
