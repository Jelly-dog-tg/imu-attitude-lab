// Frame sources: where raw protocol lines come from.
//
// The pipeline never talks to a device directly. It consumes a `FrameSource`,
// which keeps two promises that make the whole thing testable and CI-friendly:
//
//   * a *deterministic* simulated source, so a run can be reproduced exactly
//     without hardware;
//   * a file-backed source, so a captured serial log can be replayed on any
//     machine.
//
// Adding a real serial source later means implementing this one interface; the
// receiver/worker/logger threads do not change.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "imu_lab/attitude_sample.hpp"
#include "imu_lab/error.hpp"

namespace imu_lab {

/// Pull-based producer of raw frames.
class FrameSource {
public:
    virtual ~FrameSource() = default;

    FrameSource(const FrameSource&) = delete;
    FrameSource& operator=(const FrameSource&) = delete;

    /// Produce the next frame.
    ///
    /// @return false once the source is exhausted.
    virtual bool next(RawFrame& out) = 0;

    /// Short human-readable name used in reports.
    [[nodiscard]] virtual std::string name() const = 0;

protected:
    FrameSource() = default;
};

/// Which source the CLI should build.
enum class SourceKind {
    simulated,
    text_file,
};

/// Everything a source needs to be constructed.
struct SourceConfig {
    SourceKind kind = SourceKind::simulated;

    // --- simulated ---
    std::size_t frames = 200;
    double sample_rate_hz = 50.0;
    std::uint64_t seed = 7;
    /// Fraction of frames deliberately corrupted, to exercise the rejection and
    /// anomaly-ratio paths without a fault injector on the wire.
    double anomaly_rate = 0.0;
    /// Magnitude of one deliberate angle step, in degrees, injected at the
    /// midpoint so that jump detection has something to find.
    double injected_step_deg = 0.0;

    // --- text_file ---
    std::string path;

    /// Seconds between frames, derived from `sample_rate_hz`.
    [[nodiscard]] double frame_interval_s() const noexcept {
        return sample_rate_hz > 0.0 ? 1.0 / sample_rate_hz : 0.0;
    }
};

/// Build a source, or return a classified error.
///
/// @return `ErrorCode::file_not_found` when a file source points at a missing
///         file, `ErrorCode::unsupported_source` for an unknown kind.
[[nodiscard]] Result<std::unique_ptr<FrameSource>> make_frame_source(const SourceConfig& config);

/// Deterministic synthetic source (`P:`/`R:` lines, optionally corrupted).
[[nodiscard]] std::unique_ptr<FrameSource> make_simulated_source(const SourceConfig& config);

/// Replay a text file containing one protocol line per row.
[[nodiscard]] Result<std::unique_ptr<FrameSource>> make_text_file_source(const SourceConfig& config);

}  // namespace imu_lab
