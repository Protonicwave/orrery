#pragma once

/// \file
/// The acceleration of one particle, computed as accurately as this project
/// knows how, so that a fast kernel's error can be measured rather than
/// asserted.
///
/// The direct solver is the reference every approximation in Orrery is compared
/// against, and Phase 7 is the point at which that reference acquires a second
/// implementation. Two kernels now compute the same sum in different orders,
/// and the question "which of them is right" cannot be answered by comparing
/// them to each other: they disagree in the last bits by construction, and
/// neither is the exact answer.
///
/// So there is a third thing, used by no simulation and timed by no benchmark
/// as a candidate. It computes the same physics with two deliberate
/// extravagances.
///
/// Every term is formed in `double`, whatever `Real` the build selected. In the
/// single-precision configuration that makes the reference genuinely more
/// accurate than either kernel rather than merely differently rounded, which is
/// exactly what is needed to say by how much a `float` kernel is wrong.
///
/// The sum is accumulated with Neumaier compensation, which tracks the low-order
/// bits that each addition discards and adds them back at the end. A plain sum
/// of n terms accumulates an error that grows with n; a compensated one behaves
/// as though the additions were performed exactly and rounded once. Since the
/// difference between the two kernels *is* their summation order, a reference
/// whose summation error is negligible is the only way to attribute the
/// difference to the right one.
///
/// What is left is the rounding inside each term: three subtractions, a squared
/// separation, a square root and a division, all in double precision. That
/// residue is of order the machine epsilon of a single term and does not grow
/// with the particle count, so it is far below the errors being measured. The
/// reference is therefore a bound on the kernels' accuracy rather than an exact
/// answer, and `docs/performance/roofline.md` states it as such.
///
/// This is not a kernel. It is O(N) per particle with a branch in the loop and
/// no attempt at anything, and calling it for every particle of a large
/// configuration is slower than the solver it is checking. That is the right
/// trade for an instrument that runs in tests and once per benchmark row.

#include <span>

#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3_span.hpp"

namespace orrery::solvers {

/// An acceleration in double precision, whatever the build's scalar type is.
///
/// A distinct type rather than `core::Vec3` because the whole point of the
/// reference is that it is not held in the precision under test. Returning a
/// `Vec3` would round the answer back to `float` in the single-precision build
/// and quietly destroy the accuracy this file exists to provide.
struct ReferenceAcceleration {
    double x{};
    double y{};
    double z{};
};

/// The acceleration on particle `target`, from every other particle.
///
/// Includes the factor of G, so the result is directly comparable with what
/// `DirectSolver` writes into the acceleration arrays. Softened with the same
/// Plummer form as the kernels, since a reference softened differently would
/// measure the mismatch rather than the kernel.
///
/// A configuration of one particle gives zero, which is the correct
/// acceleration of a body with nothing to attract it.
[[nodiscard]] ReferenceAcceleration
reference_acceleration(core::Vec3Span<const core::Real> positions,
                       std::span<const core::Real> masses, core::Index target,
                       core::Softening softening);

} // namespace orrery::solvers
