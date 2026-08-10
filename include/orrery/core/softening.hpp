#pragma once

/// \file
/// Plummer softening, defined once for everything that needs it.
///
/// A gravitational force between point masses diverges as the separation goes
/// to zero. In a simulation that divergence is not physics but a modelling
/// error: a simulation particle represents a great many stars rather than one,
/// so a close approach between two of them is an artefact of the sampling and
/// not an encounter that happened. Left alone it produces an unbounded
/// acceleration, which no fixed timestep can integrate, and the resulting
/// energy error is arbitrarily large.
///
/// Plummer softening replaces the point mass with the potential
///
///     Phi(r) = -G m / sqrt(r^2 + eps^2)
///
/// which is the exact potential of a Plummer sphere of scale radius `eps`. That
/// is what makes this the softening to choose: it is not a fudge that keeps the
/// denominator away from zero, it is the potential of a real mass distribution,
/// so a softened simulation is an exact simulation of extended masses rather
/// than an approximate one of point masses. It agrees with the point-mass
/// result to better than a part in a thousand beyond about twenty softening
/// lengths and is finite everywhere.
///
/// The file exists so that the force kernel and the potential energy diagnostic
/// cannot disagree. Energy conservation is one of the two headline validation
/// results of this project, and if the diagnostic integrated a different
/// potential from the one the kernel differentiated, the measured drift would
/// be a property of the mismatch rather than of the integrator. Both forms
/// below are therefore stated together: the second is the negative gradient of
/// the first, divided by the separation vector, and a test checks that against
/// a numerical derivative. ADR-0008 records why they live here rather than
/// beside the kernel that will use them.

#include <cmath>

#include "orrery/core/types.hpp"

namespace orrery::core {

/// The softening length of the Plummer kernel above.
///
/// A type rather than a bare `Real` because a softening length and a softening
/// length squared are both scalars, they appear within one line of each other
/// in every kernel that uses them, and passing one where the other is expected
/// is silent. Holding the square is what the arithmetic wants, so it is what is
/// stored, and the length is recovered on the rare occasions anything asks.
class Softening {
public:
    /// No softening: exact point masses.
    ///
    /// The right choice for the analytic comparisons, where softening would be
    /// an error term rather than a model. A Kepler orbit is between two point
    /// masses and is checked against the point-mass result.
    constexpr Softening() noexcept = default;

    /// Softening with the given length.
    ///
    /// `explicit` because a length is not a softening until someone says it is,
    /// and the two arguments a diagnostic takes after it are also scalars.
    ///
    /// The sign of the argument does not matter, since only the square is
    /// kept. That is not an invitation to pass a negative length; it is the
    /// reason no check rejects one.
    constexpr explicit Softening(Real length) noexcept : squared_(length * length) {}

    /// The softening length squared, which is the form every kernel uses.
    [[nodiscard]] constexpr Real squared() const noexcept { return squared_; }

    [[nodiscard]] Real length() const noexcept { return std::sqrt(squared_); }

private:
    Real squared_{};
};

/// The factor `1 / sqrt(r^2 + eps^2)` of the softened potential.
///
/// Takes the squared separation because that is what a caller has: computing
/// the separation itself would mean a square root that this function would
/// immediately undo.
[[nodiscard]] inline Real softened_inverse_distance(Real separation_squared,
                                                    Softening softening) noexcept {
    return Real{1} / std::sqrt(separation_squared + softening.squared());
}

/// The factor `1 / (r^2 + eps^2)^(3/2)` of the softened acceleration.
///
/// The gradient of the potential above carries one more power of the softened
/// distance in the denominator, and one factor of the separation vector in the
/// numerator, which the caller supplies. Written as the cube of the reciprocal
/// rather than as a second square root because the two forms must agree in the
/// last bits: an acceleration derived from a slightly different value than the
/// potential energy uses would show up as a drift that no integrator caused.
[[nodiscard]] inline Real softened_inverse_distance_cubed(Real separation_squared,
                                                          Softening softening) noexcept {
    const Real inverse = softened_inverse_distance(separation_squared, softening);
    return inverse * inverse * inverse;
}

} // namespace orrery::core
