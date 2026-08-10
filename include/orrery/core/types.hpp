#pragma once

/// \file
/// The scalar and index types that every layer of the project agrees on.
///
/// A single alias for the floating-point type is what makes the two halves of
/// this project possible at once: an accuracy-oriented build that integrates
/// orbits in double precision, and a throughput-oriented build that moves half
/// as many bytes per particle and so runs closer to the memory bandwidth limit
/// that binds most kernels here. The choice is made when the project is
/// configured rather than per call site, because a run is one or the other and
/// never both. ADR-0006 records the alternatives.

#include <cstddef>
#include <type_traits>

namespace orrery::core {

#ifdef ORRERY_SINGLE_PRECISION
using Real = float;
#else
using Real = double;
#endif

/// True when `Real` is `float`.
///
/// Tests and diagnostics need error tolerances that follow the precision the
/// build was configured with, and a tolerance written for double precision
/// silently becomes an assertion about nothing when the same test is compiled
/// as single precision. `orrery::core::uses_single_precision()` answers the
/// same question at run time from inside the library, which is a different
/// question: this constant describes the translation unit that reads it, and
/// that function describes the one that was compiled into the library.
inline constexpr bool kSinglePrecision = std::is_same_v<Real, float>;

/// The type of a particle index and of any count of particles.
///
/// Unsigned, and `std::size_t` specifically, because every container and view
/// this project uses reports its length as `std::size_t`. A signed index would
/// put a conversion at every boundary between the project's own arithmetic and
/// the standard library's, and the build treats those conversions as errors
/// rather than tolerating them. Arithmetic that genuinely needs a signed value,
/// such as the difference between two indices, says so locally with
/// `std::ptrdiff_t`.
using Index = std::size_t;

} // namespace orrery::core
