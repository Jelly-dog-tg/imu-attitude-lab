#include "imu_lab/frame_source.hpp"

#include <cmath>
#include <fstream>
#include <sstream>
#include <utility>

#include "imu_lab/protocol_parser.hpp"

namespace imu_lab {
namespace {

constexpr double kPi = 3.14159265358979323846;

/// Minimal, fully specified PRNG.
///
/// `std::mt19937` is itself portable, but the *distributions* in <random> are
/// not required to produce identical output across standard libraries. Rolling
/// a tiny splitmix64 plus Box-Muller guarantees that the same seed yields the
/// same bytes on GCC, Clang and MSVC, which is what makes the CI benchmark
/// numbers comparable between runners.
class SplitMix64 {
public:
    explicit SplitMix64(std::uint64_t seed) : state_(seed) {}

    std::uint64_t next_u64() noexcept {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    /// Uniform in [0, 1).
    double next_unit() noexcept {
        return static_cast<double>(next_u64() >> 11) * (1.0 / 9007199254740992.0);
    }

    /// Standard normal via the Box-Muller transform.
    double next_normal() noexcept {
        const double u1 = next_unit() < 1e-12 ? 1e-12 : next_unit();
        const double u2 = next_unit();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
    }

private:
    std::uint64_t state_;
};

/// Deterministic synthetic attitude stream.
class SimulatedSource final : public FrameSource {
public:
    explicit SimulatedSource(const SourceConfig& config)
        : frames_(config.frames),
          interval_s_(config.frame_interval_s()),
          anomaly_rate_(config.anomaly_rate),
          injected_step_deg_(config.injected_step_deg),
          rng_(config.seed),
          step_at_(config.frames / 2) {}

    bool next(RawFrame& out) override {
        if (produced_ >= frames_) {
            return false;
        }

        const auto index = static_cast<double>(produced_);
        const double elapsed = index * interval_s_;

        double pitch = 8.0 * std::sin(2.0 * kPi * 0.2 * elapsed) + 0.03 * rng_.next_normal();
        double roll = 15.0 * std::sin(2.0 * kPi * 0.3 * elapsed + 0.2) + 0.03 * rng_.next_normal();

        if (injected_step_deg_ != 0.0 && produced_ >= step_at_ && !step_applied_) {
            pitch += injected_step_deg_;
            step_applied_ = true;
        }

        out.sequence = produced_;
        out.elapsed_s = elapsed;
        out.line = format_attitude_line(AttitudeSample{pitch, roll, std::nullopt, std::nullopt});

        // Corrupt a deterministic subset of frames to exercise rejection.
        if (anomaly_rate_ > 0.0 && rng_.next_unit() < anomaly_rate_) {
            out.line = corrupt(out.line, produced_);
        }

        ++produced_;
        return true;
    }

    [[nodiscard]] std::string name() const override { return "simulated"; }

private:
    /// Produce one of three classic failure modes, cycled deterministically.
    static std::string corrupt(const std::string& line, std::uint64_t index) {
        switch (index % 3) {
            case 0:
                return line.substr(0, line.size() / 2);          // truncated frame
            case 1:
                return std::string("P:nan,R:") + line.substr(line.find("R:") + 2);  // non-finite
            default:
                return "P:abc,R:0.0";                             // non-numeric
        }
    }

    std::size_t frames_;
    double interval_s_;
    double anomaly_rate_;
    double injected_step_deg_;
    SplitMix64 rng_;
    std::size_t produced_ = 0;
    const std::size_t step_at_;
    bool step_applied_ = false;
};

/// Replays a captured stream, one protocol line per row.
class TextFileSource final : public FrameSource {
public:
    explicit TextFileSource(const SourceConfig& config, std::ifstream stream)
        : interval_s_(config.frame_interval_s()), stream_(std::move(stream)) {}

    bool next(RawFrame& out) override {
        std::string line;
        if (!std::getline(stream_, line)) {
            return false;
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        out.sequence = produced_;
        out.elapsed_s = static_cast<double>(produced_) * interval_s_;
        out.line = std::move(line);
        ++produced_;
        return true;
    }

    [[nodiscard]] std::string name() const override { return "text_file"; }

private:
    double interval_s_;
    std::ifstream stream_;
    std::uint64_t produced_ = 0;
};

}  // namespace

std::unique_ptr<FrameSource> make_simulated_source(const SourceConfig& config) {
    return std::make_unique<SimulatedSource>(config);
}

Result<std::unique_ptr<FrameSource>> make_text_file_source(const SourceConfig& config) {
    if (config.path.empty()) {
        return Error(ErrorCode::unsupported_source, "text_file source requires --input <path>");
    }
    std::ifstream stream(config.path);
    if (!stream) {
        return Error(ErrorCode::file_not_found, "file not found: " + config.path);
    }
    return std::unique_ptr<FrameSource>(new TextFileSource(config, std::move(stream)));
}

Result<std::unique_ptr<FrameSource>> make_frame_source(const SourceConfig& config) {
    switch (config.kind) {
        case SourceKind::simulated:
            return make_simulated_source(config);
        case SourceKind::text_file:
            return make_text_file_source(config);
    }
    std::ostringstream message;
    message << "unsupported source kind";
    return Error(ErrorCode::unsupported_source, message.str());
}

}  // namespace imu_lab
