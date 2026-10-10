// Locale-independent, allocation-free decimal parser.
//
// Why not `std::from_chars` or `strtod`?
//
//  * `std::from_chars` for floating point is still missing from several
//    toolchains (notably MinGW-w64's libstdc++ in GCC 11), so relying on it
//    would make the project fail to build on a plain Windows GCC setup.
//  * `strtod`/`std::stod` honour the process locale, so on a machine configured
//    with a comma decimal separator they would mis-parse "1.5" as 1.
//
// A hand-written scanner removes both problems, costs no allocation, and lets
// the tests pin down exactly which literals are accepted.
#pragma once

#include <string_view>

namespace imu_lab {

/// Parse a decimal floating-point literal.
///
/// Accepted grammar:
///   [+-]? ( digits [ '.' digits? ] | '.' digits ) [ (e|E) [+-]? digits ]
/// plus the explicit non-finite words `nan`, `inf` and `infinity` (any case),
/// which are reported as *valid parses* so the caller can classify them as
/// `not_finite` rather than as garbage text.
///
/// Rejected: empty input, hex literals, trailing characters, a bare '.',
/// a bare exponent marker.
///
/// @return true when the whole view was consumed; `out` is only written then.
[[nodiscard]] bool parse_double(std::string_view text, double& out) noexcept;

}  // namespace imu_lab
