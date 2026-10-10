// Command-line front end for the C++ core: imu_lab_cli
//
//   imu_lab_cli stream   --frames 20000 --workers 3 --csv out.csv
//   imu_lab_cli stream   --input captured.txt        # replay a captured stream
//   imu_lab_cli analyze  out.csv --jump-threshold 30
//   imu_lab_cli validate out.csv
//
// Exit codes mirror the Python tooling so shell scripts and CI can treat both
// implementations identically:
//   0 success, 2 usage, 3 invalid data, 4 missing input, 5 source unavailable,
//   10 internal error.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "imu_lab/csv_reader.hpp"
#include "imu_lab/error.hpp"
#include "imu_lab/frame_source.hpp"
#include "imu_lab/jump_detector.hpp"
#include "imu_lab/metrics.hpp"
#include "imu_lab/pipeline.hpp"
#include "imu_lab/statistics.hpp"

namespace {

using imu_lab::AttitudePipeline;
using imu_lab::CsvReader;
using imu_lab::Error;
using imu_lab::ErrorCode;
using imu_lab::PipelineConfig;
using imu_lab::SourceConfig;
using imu_lab::SourceKind;

enum class ExitCode : int {
    ok = 0,
    no_data = 1,
    usage = 2,
    data_invalid = 3,
    input_missing = 4,
    source_unavailable = 5,
    internal = 10,
};

int to_exit_code(ErrorCode code) {
    switch (code) {
        case ErrorCode::file_not_found:
            return static_cast<int>(ExitCode::input_missing);
        case ErrorCode::file_not_writable:
            return static_cast<int>(ExitCode::source_unavailable);
        case ErrorCode::missing_column:
        case ErrorCode::duplicate_column:
        case ErrorCode::not_a_number_in_column:
            return static_cast<int>(ExitCode::data_invalid);
        case ErrorCode::empty_file:
            return static_cast<int>(ExitCode::no_data);
        case ErrorCode::unsupported_source:
            return static_cast<int>(ExitCode::usage);
        default:
            return static_cast<int>(ExitCode::internal);
    }
}

void print_error(const std::string& prefix, const Error& error) {
    std::cerr << prefix << ": " << error.describe() << '\n';
}

void print_usage(std::ostream& out) {
    out << "imu_lab_cli - C++17 core for the IMU Attitude Lab\n"
           "\n"
           "Usage:\n"
           "  imu_lab_cli stream   [options]   run the multithreaded pipeline\n"
           "  imu_lab_cli analyze  <csv>      statistics for an existing log\n"
           "  imu_lab_cli validate <csv>      row-level validation report\n"
           "\n"
           "stream options:\n"
           "  --frames N            simulated frame count (default 20000)\n"
           "  --hz F                simulated sample rate (default 200)\n"
           "  --seed N              PRNG seed (default 7)\n"
           "  --anomaly-rate F      fraction of corrupted frames (default 0)\n"
           "  --step-deg F          inject one angle step of this size\n"
           "  --input PATH          replay a text file of protocol lines instead\n"
           "  --workers N           parsing threads (default 2)\n"
           "  --queue-capacity N    bounded parse queue, 0 = unbounded (default 4096)\n"
           "  --jump-threshold F    large-step threshold in degrees (default 30)\n"
           "  --csv PATH            write the normalised log here\n"
           "  --json PATH           write the metrics as JSON here\n"
           "  --quiet               only print the JSON/report payload\n"
           "\n"
           "analyze options:\n"
           "  --jump-threshold F    large-step threshold in degrees (default 30)\n"
           "  --json                print statistics as JSON\n";
}

/// Parse `--name value` style options into a simple accessor.
struct Args {
    std::vector<std::string> positional;
    std::vector<std::pair<std::string, std::string>> options;

    [[nodiscard]] bool has(const std::string& name) const {
        for (const auto& option : options) {
            if (option.first == name) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::string get(const std::string& name, const std::string& fallback) const {
        for (const auto& option : options) {
            if (option.first == name) {
                return option.second;
            }
        }
        return fallback;
    }

    [[nodiscard]] double get_double(const std::string& name, double fallback) const {
        const std::string raw = get(name, {});
        if (raw.empty()) {
            return fallback;
        }
        // strtod is used only for command-line numbers, where locale is not a
        // concern; the data path uses the hand-written parser.
        char* end = nullptr;
        const double value = std::strtod(raw.c_str(), &end);
        if (end == nullptr || *end != '\0') {
            return fallback;
        }
        return value;
    }

    [[nodiscard]] std::size_t get_size(const std::string& name, std::size_t fallback) const {
        const std::string raw = get(name, {});
        if (raw.empty()) {
            return fallback;
        }
        char* end = nullptr;
        const unsigned long long value = std::strtoull(raw.c_str(), &end, 10);
        if (end == nullptr || *end != '\0') {
            return fallback;
        }
        return static_cast<std::size_t>(value);
    }
};

Args parse_args(int argc, char** argv, int first) {
    Args args;
    for (int index = first; index < argc; ++index) {
        const std::string token = argv[index];
        if (token.rfind("--", 0) != 0) {
            args.positional.push_back(token);
            continue;
        }
        // Support both "--name value" and "--name=value".
        const std::size_t equals = token.find('=');
        if (equals != std::string::npos) {
            args.options.emplace_back(token.substr(0, equals), token.substr(equals + 1));
            continue;
        }
        if (index + 1 < argc && std::strncmp(argv[index + 1], "--", 2) != 0) {
            args.options.emplace_back(token, argv[index + 1]);
            ++index;
        } else {
            args.options.emplace_back(token, "1");
        }
    }
    return args;
}

int command_stream(const Args& args) {
    SourceConfig source_config;
    source_config.frames = args.get_size("--frames", 20000);
    source_config.sample_rate_hz = args.get_double("--hz", 200.0);
    source_config.seed = args.get_size("--seed", 7);
    source_config.anomaly_rate = args.get_double("--anomaly-rate", 0.0);
    source_config.injected_step_deg = args.get_double("--step-deg", 0.0);

    const std::string input = args.get("--input", {});
    if (!input.empty()) {
        source_config.kind = SourceKind::text_file;
        source_config.path = input;
    }

    if (source_config.sample_rate_hz <= 0.0) {
        std::cerr << "error: --hz must be greater than zero\n";
        return static_cast<int>(ExitCode::usage);
    }
    if (source_config.anomaly_rate < 0.0 || source_config.anomaly_rate > 1.0) {
        std::cerr << "error: --anomaly-rate must be between 0 and 1\n";
        return static_cast<int>(ExitCode::usage);
    }

    PipelineConfig pipeline_config;
    pipeline_config.worker_threads = args.get_size("--workers", 2);
    pipeline_config.queue_capacity = args.get_size("--queue-capacity", 4096);
    pipeline_config.output_queue_capacity = pipeline_config.queue_capacity;
    pipeline_config.jump_threshold_deg = args.get_double("--jump-threshold", 30.0);

    if (pipeline_config.jump_threshold_deg <= 0.0) {
        std::cerr << "error: --jump-threshold must be greater than zero\n";
        return static_cast<int>(ExitCode::usage);
    }

    const auto source = imu_lab::make_frame_source(source_config);
    if (!source) {
        print_error("error", source.error());
        return to_exit_code(source.error().code);
    }

    const bool quiet = args.has("--quiet");
    if (!quiet) {
        std::cout << "Processing source '" << source.value()->name() << "' with "
                  << pipeline_config.worker_threads << " worker thread(s)\n";
    }

    AttitudePipeline pipeline(pipeline_config);
    const auto run = pipeline.run(*source.value(), args.get("--csv", {}));

    if (!run.status.ok()) {
        print_error("error", run.status.error());
        return to_exit_code(run.status.error().code);
    }

    const std::string json_path = args.get("--json", {});
    if (!json_path.empty()) {
        std::ofstream(json_path, std::ios::binary) << run.stats.to_json();
    }

    std::cout << run.stats.render();
    if (!run.sample_rejections.empty()) {
        std::cout << "\nSample rejected frames:\n";
        for (const std::string& rejection : run.sample_rejections) {
            std::cout << "  " << rejection << '\n';
        }
    }
    if (!run.csv_path.empty()) {
        std::cout << "\nNormalised log: " << run.csv_path << '\n';
    }
    return static_cast<int>(run.stats.frames_received == 0 ? ExitCode::no_data : ExitCode::ok);
}

int command_analyze(const Args& args) {
    if (args.positional.empty()) {
        std::cerr << "error: analyze requires a CSV path\n";
        return static_cast<int>(ExitCode::usage);
    }
    const std::string path = args.positional.front();

    const double threshold = args.get_double("--jump-threshold", 30.0);
    if (threshold <= 0.0) {
        std::cerr << "error: --jump-threshold must be greater than zero\n";
        return static_cast<int>(ExitCode::usage);
    }

    const auto loaded = CsvReader::load(path);
    if (!loaded) {
        print_error("error", loaded.error());
        return to_exit_code(loaded.error().code);
    }

    const std::vector<imu_lab::CsvRecord>& records = loaded.value().records;
    imu_lab::RunningStats pitch;
    imu_lab::RunningStats roll;
    imu_lab::JumpDetector detector(threshold);

    for (const imu_lab::CsvRecord& record : records) {
        pitch.add(record.sample.pitch_deg);
        roll.add(record.sample.roll_deg);
        (void)detector.observe(record.elapsed_s, record.sample);
    }

    const double span = records.back().elapsed_s - records.front().elapsed_s;
    const double hz = (records.size() > 1 && span > 0.0)
                          ? static_cast<double>(records.size() - 1) / span
                          : 0.0;

    if (args.has("--json")) {
        std::cout << "{\n"
                  << "  \"file\": \"" << path << "\",\n"
                  << "  \"valid_samples\": " << records.size() << ",\n"
                  << "  \"skipped_rows\": " << loaded.value().skipped_rows << ",\n"
                  << "  \"duration_s\": " << span << ",\n"
                  << "  \"estimated_hz\": " << hz << ",\n"
                  << "  \"jump_threshold_deg\": " << threshold << ",\n"
                  << "  \"jump_count\": " << detector.event_count() << ",\n"
                  << "  \"pitch\": {\"min\": " << pitch.minimum() << ", \"max\": "
                  << pitch.maximum() << ", \"mean\": " << pitch.mean() << ", \"stddev\": "
                  << pitch.stddev() << "},\n"
                  << "  \"roll\": {\"min\": " << roll.minimum() << ", \"max\": "
                  << roll.maximum() << ", \"mean\": " << roll.mean() << ", \"stddev\": "
                  << roll.stddev() << "}\n"
                  << "}\n";
        return static_cast<int>(ExitCode::ok);
    }

    std::cout << "File: " << path << '\n';
    std::cout << "Valid samples: " << records.size() << '\n';
    std::cout << "Skipped malformed rows: " << loaded.value().skipped_rows << '\n';
    std::cout << "Duration: " << span << " s\n";
    if (hz > 0.0) {
        std::cout << "Approx. average sample rate: " << hz << " Hz\n";
    } else {
        std::cout << "Approx. average sample rate: N/A\n";
    }
    std::cout << "Pitch (deg): min=" << pitch.minimum() << ", max=" << pitch.maximum()
              << ", mean=" << pitch.mean() << ", std=" << pitch.stddev() << '\n';
    std::cout << "Roll (deg): min=" << roll.minimum() << ", max=" << roll.maximum()
              << ", mean=" << roll.mean() << ", std=" << roll.stddev() << '\n';
    std::cout << "Large angle steps (> " << threshold << " deg): " << detector.event_count()
              << '\n';
    std::cout << "Note: large steps are flags for review, not proof of sensor failure.\n";
    return static_cast<int>(ExitCode::ok);
}

int command_validate(const Args& args) {
    if (args.positional.empty()) {
        std::cerr << "error: validate requires a CSV path\n";
        return static_cast<int>(ExitCode::usage);
    }
    const std::string path = args.positional.front();
    const auto report = CsvReader::validate(path, args.get_size("--max-issues", 200));
    std::cout << report.render();
    if (!report.file_exists) {
        return static_cast<int>(ExitCode::input_missing);
    }
    if (!report.ok()) {
        return static_cast<int>(ExitCode::data_invalid);
    }
    return static_cast<int>(ExitCode::ok);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(std::cerr);
        return static_cast<int>(ExitCode::usage);
    }

    const std::string command = argv[1];
    if (command == "--help" || command == "-h" || command == "help") {
        print_usage(std::cout);
        return static_cast<int>(ExitCode::ok);
    }

    const Args args = parse_args(argc, argv, 2);
    if (command == "stream") {
        return command_stream(args);
    }
    if (command == "analyze") {
        return command_analyze(args);
    }
    if (command == "validate") {
        return command_validate(args);
    }

    std::cerr << "error: unknown command '" << command << "'\n";
    print_usage(std::cerr);
    return static_cast<int>(ExitCode::usage);
}
