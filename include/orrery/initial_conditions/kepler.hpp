#pragma once

/// \file
/// The two-body problem, which is the only gravitational system anyone can
/// solve exactly.
///
/// Two point masses on a bound orbit trace ellipses about their common centre
/// of mass, with a period, an energy and an angular momentum that follow from
/// the orbital elements in closed form. That makes this configuration the
/// project's primary validation instrument. A solver that gets the acceleration
/// wrong, an integrator that loses energy, a tree approximation that opens too
/// many cells: each shows up here as a departure from a number that is known
/// rather than from an earlier run of the same code.
///
/// The configuration is placed at periapsis, where the velocity is exactly
/// perpendicular to the separation. That is the one point on the orbit whose
/// state can be written down without solving Kepler's equation, so the starting
/// state carries no iterative error into a measurement that will later be
/// quoted to the last bit.
///
/// The analytic quantities are exposed alongside the generator, and they are
/// what the tests compare against. Deriving them inside the test instead would
/// mean the test and the code under test could be wrong together.

#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"

namespace orrery::initial_conditions {

/// A bound two-body orbit, described by its elements.
///
/// Only the elements that affect the scalars this project measures are
/// included. Inclination, the argument of periapsis and the longitude of the
/// ascending node orient the orbit in space without changing its period, energy
/// or the magnitude of its angular momentum, so they would add three parameters
/// that no test could distinguish. The orbit is placed in the x-y plane with
/// periapsis along the positive x axis.
struct KeplerParameters {
    core::Real primary_mass = 1;
    core::Real secondary_mass = 1;

    /// The semi-major axis of the relative orbit, that is, of the separation
    /// between the two bodies rather than of either body's path about the
    /// centre of mass.
    core::Real semi_major_axis = 1;

    /// Zero for a circle, approaching one for an orbit that grazes the centre.
    ///
    /// Values at or above one describe a parabolic or hyperbolic encounter.
    /// Those are unbound, have no period, and their energy is not negative, so
    /// every function here would be answering a question about an ellipse that
    /// does not exist; the generator rejects them rather than returning
    /// something shaped like an answer.
    core::Real eccentricity = 0;
};

/// Two particles at periapsis, in the centre-of-mass frame at rest.
///
/// The heavier body is not distinguished from the lighter one: particle 0
/// carries `primary_mass` and particle 1 carries `secondary_mass`, whatever
/// their sizes.
///
/// Throws `std::invalid_argument` if either mass is not positive, if the
/// semi-major axis is not positive, or if the eccentricity is outside [0, 1).
/// Initial conditions are a setup boundary, which is where the conventions in
/// section 4 of the implementation plan permit exceptions, and a configuration
/// that cannot be built should say so at the point of the mistake rather than
/// hand back particles that quietly encode it.
[[nodiscard]] core::ParticleData make_kepler_orbit(const KeplerParameters& parameters);

/// The orbital period, from Kepler's third law.
///
/// `2 pi sqrt(a^3 / G(m1 + m2))`. Independent of eccentricity, which is itself
/// one of the results worth demonstrating.
[[nodiscard]] core::Real kepler_period(const KeplerParameters& parameters);

/// The total energy, `-G m1 m2 / 2a`.
///
/// Kinetic and potential together, in the centre-of-mass frame. It depends only
/// on the semi-major axis, so measuring it is how a test recovers the
/// semi-major axis from a state and checks the period that follows from it.
[[nodiscard]] core::Real kepler_energy(const KeplerParameters& parameters);

/// The magnitude of the total angular momentum, `mu sqrt(G M a (1 - e^2))`,
/// where `mu` is the reduced mass and `M` the total.
///
/// Unlike the energy this does depend on the eccentricity, so the two together
/// determine the orbit and a test that checks both has checked the shape of the
/// orbit and not only its size.
[[nodiscard]] core::Real kepler_angular_momentum(const KeplerParameters& parameters);

/// The separation at periapsis, `a(1 - e)`, which is where the generated
/// configuration starts.
[[nodiscard]] core::Real kepler_periapsis_distance(const KeplerParameters& parameters);

} // namespace orrery::initial_conditions
