#include <cmath>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/units.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/initial_conditions/kepler.hpp"
#include "orrery/integrators/integrator.hpp"
#include "orrery/integrators/velocity_verlet.hpp"
#include "orrery/solvers/direct_solver.hpp"

namespace {

using orrery::core::cross;
using orrery::core::Index;
using orrery::core::kGravitationalConstant;
using orrery::core::kSinglePrecision;
using orrery::core::norm;
using orrery::core::ParticleData;
using orrery::core::Real;
using orrery::core::squared_norm;
using orrery::core::Vec3;
using orrery::initial_conditions::kepler_periapsis_distance;
using orrery::initial_conditions::kepler_period;
using orrery::initial_conditions::KeplerParameters;
using orrery::initial_conditions::make_kepler_orbit;
using orrery::integrators::refresh_accelerations;
using orrery::integrators::VelocityVerlet;
using orrery::solvers::DirectSolver;

/// An eccentric orbit, because a circular one hides too much.
///
/// At zero eccentricity the separation never changes, so a force law with the
/// wrong exponent would give a circular orbit of the wrong period and nothing
/// else: the shape would still close. At an eccentricity of one half the
/// separation varies by a factor of three over each orbit, and any departure from
/// the inverse square law turns immediately into an ellipse whose axis rotates.
/// That is the classical test of the exponent and it is what this file applies to
/// the solver.
constexpr KeplerParameters kOrbit{.primary_mass = 1,
                                  .secondary_mass = 1,
                                  .semi_major_axis = 1,
                                  .eccentricity = static_cast<Real>(0.5)};

/// How far the state may be from where it started after one period.
///
/// This bound is set by the integrator rather than by the solver. Velocity
/// Verlet is second order, and nearly all of what remains after one period is
/// the phase error it accumulates through periapsis, where the orbit is fastest
/// and the fixed step least adequate. The measured closure is 8.8e-6 in double
/// precision at twenty thousand steps to the orbit and 8.6e-4 in single
/// precision at two thousand, which is the same result: ten times the step, a
/// hundred times the error, exactly as a second-order method promises. The single
/// precision run takes the coarser step because the finer one would buy nothing
/// there, the round-off of twenty thousand accumulations in `float` being larger
/// than the discretisation error it would remove.
///
/// The bounds are those measurements rounded up by an order of magnitude, so that
/// the test asserts the orbit closes rather than asserting one machine's last
/// digits.
constexpr Real kClosureTolerance =
    kSinglePrecision ? static_cast<Real>(6e-3) : static_cast<Real>(1e-4);

/// How far the semi-major axis and the eccentricity may wander over a long run.
///
/// The measured extremes over two hundred orbits at four hundred steps each are
/// 6.7e-4 in the semi-major axis and 5.0e-4 in the eccentricity, neither of them
/// growing with time. The bound is an order of magnitude above that, for the same
/// reason.
constexpr Real kElementTolerance =
    kSinglePrecision ? static_cast<Real>(0.05) : static_cast<Real>(0.01);

/// How far the axis of the orbit may turn over the whole run, in radians.
///
/// Measured at 0.19 radians over two hundred orbits, or a thousandth of a radian
/// per revolution. That it is not zero is the integrator's doing and not the
/// solver's, which is the subject of the last test in this file.
constexpr Real kPrecessionBound = static_cast<Real>(0.5);

/// The orbital elements recovered from a state.
///
/// Derived from the position and velocity by the standard two-body relations
/// rather than from anything the simulation recorded, so a wrong force law shows
/// up here as elements that move rather than as a number that agrees with itself.
struct Elements {
    Real semi_major_axis = 0;
    Real eccentricity = 0;

    /// The direction of periapsis, measured in the orbital plane from the x axis.
    ///
    /// The generated configuration starts with periapsis along the positive x
    /// axis, so this begins at zero. It is the quantity that exposes a force law
    /// that is not exactly an inverse square: such an orbit is still bound and
    /// still has a well-defined size and shape, but its axis turns a little on
    /// every revolution.
    Real periapsis_angle = 0;
};

[[nodiscard]] Elements elements_of(const ParticleData& data) {
    // The relative orbit. Two bodies about their common centre of mass are one
    // body about a fixed centre with the total mass, which is the form every
    // closed-form expression below is written in.
    const Vec3 separation = data.positions().get(1) - data.positions().get(0);
    const Vec3 relative_velocity = data.velocities().get(1) - data.velocities().get(0);
    const Real parameter = kGravitationalConstant * (data.masses()[0] + data.masses()[1]);

    const Real distance = norm(separation);
    const Vec3 specific_angular_momentum = cross(separation, relative_velocity);

    // The Laplace-Runge-Lenz vector, in the form that is dimensionless and points
    // at periapsis with a magnitude equal to the eccentricity. It is conserved
    // exactly under an inverse square law and under no other power, which is the
    // whole reason for computing it here.
    const Vec3 eccentricity_vector =
        (cross(relative_velocity, specific_angular_momentum) / parameter) - (separation / distance);

    // From the vis-viva relation, `v^2 = GM (2/r - 1/a)`, rearranged.
    const Real semi_major_axis =
        1 / ((2 / distance) - (squared_norm(relative_velocity) / parameter));

    return {semi_major_axis, norm(eccentricity_vector),
            std::atan2(eccentricity_vector.y, eccentricity_vector.x)};
}

/// The extremes of each element over a run.
struct ElementHistory {
    Real semi_major_axis_error = 0;
    Real eccentricity_error = 0;
    Real precession = 0;
    Real closest_approach = 0;
    Real furthest_separation = 0;
};

[[nodiscard]] Real larger(Real a, Real b) {
    return a > b ? a : b;
}

/// Integrate the orbit and record how far each element wandered.
[[nodiscard]] ElementHistory integrate(Index orbits, Index steps_per_orbit) {
    ParticleData data = make_kepler_orbit(kOrbit);

    // No softening. This is an orbit of two point masses and the analytic
    // solution it is being compared against is the point-mass one; softening it
    // would replace the problem whose answer is known with a nearby problem
    // whose answer is not.
    DirectSolver solver;
    VelocityVerlet integrator;

    const Real timestep = kepler_period(kOrbit) / static_cast<Real>(steps_per_orbit);

    refresh_accelerations(data, solver);

    const Elements initial = elements_of(data);
    ElementHistory history;
    history.closest_approach = norm(data.positions().get(1) - data.positions().get(0));
    history.furthest_separation = history.closest_approach;

    for (Index step = 0; step < orbits * steps_per_orbit; ++step) {
        integrator.step(data, timestep, solver);

        const Elements current = elements_of(data);
        const Real distance = norm(data.positions().get(1) - data.positions().get(0));

        history.semi_major_axis_error =
            larger(history.semi_major_axis_error,
                   std::abs(current.semi_major_axis - kOrbit.semi_major_axis));
        history.eccentricity_error = larger(history.eccentricity_error,
                                            std::abs(current.eccentricity - kOrbit.eccentricity));
        history.precession =
            larger(history.precession, std::abs(current.periapsis_angle - initial.periapsis_angle));
        history.closest_approach =
            distance < history.closest_approach ? distance : history.closest_approach;
        history.furthest_separation = larger(history.furthest_separation, distance);
    }

    return history;
}

} // namespace

TEST_CASE("a Kepler orbit returns to its starting state after one period",
          "[validation][solvers]") {
    // The most direct statement that the solver computes gravity: released at
    // periapsis, the two bodies come back to where they started, moving as they
    // were, after the time Kepler's third law says they should. Nothing about
    // that is true of a force law with the wrong constant, the wrong exponent or
    // the wrong sign, and none of it is checked against an earlier run of this
    // code.
    //
    // The remaining error is the integrator's rather than the solver's, and it
    // falls as the square of the timestep because velocity Verlet is second
    // order. The step is chosen fine enough that the closure is unambiguous.
    const Index steps = kSinglePrecision ? 2000 : 20000;

    ParticleData data = make_kepler_orbit(kOrbit);
    DirectSolver solver;
    VelocityVerlet integrator;

    const Vec3 initial_separation = data.positions().get(1) - data.positions().get(0);
    const Vec3 initial_velocity = data.velocities().get(1) - data.velocities().get(0);

    const Real period = kepler_period(kOrbit);
    const Real timestep = period / static_cast<Real>(steps);

    refresh_accelerations(data, solver);

    for (Index step = 0; step < steps; ++step) {
        integrator.step(data, timestep, solver);
    }

    const Vec3 final_separation = data.positions().get(1) - data.positions().get(0);
    const Vec3 final_velocity = data.velocities().get(1) - data.velocities().get(0);

    const Real position_error =
        norm(final_separation - initial_separation) / norm(initial_separation);
    const Real velocity_error = norm(final_velocity - initial_velocity) / norm(initial_velocity);

    CAPTURE(steps, period, position_error, velocity_error);

    REQUIRE(position_error <= kClosureTolerance);
    REQUIRE(velocity_error <= kClosureTolerance);

    // One force evaluation per step, plus the one that established the
    // acceleration invariant before the first. Stated here because the closure
    // above is only evidence about the solver if the solver is what produced
    // every acceleration along the way.
    REQUIRE(solver.interaction_count().evaluations == steps + 1);
    REQUIRE(solver.interaction_count().particle_particle == 2 * (steps + 1));
}

TEST_CASE("a long integration keeps the size and shape of the orbit", "[validation][solvers]") {
    // The same orbit over hundreds of revolutions. What is being tested now is
    // not the accuracy of one period but that the ellipse itself is preserved:
    // the semi-major axis and the eccentricity return to where they were rather
    // than drifting, and the separation stays between the periapsis and apoapsis
    // distances the elements predict.
    //
    // The periapsis direction is the one element that does move. A symplectic
    // integrator conserves the energy of a nearby problem rather than of this
    // one, and the difference shows up as a slow rotation of the orbit's axis
    // that accumulates with the number of revolutions and falls with the square
    // of the timestep. It is bounded here rather than required to vanish,
    // because requiring it to vanish would be asserting something untrue.
    const Index orbits = kSinglePrecision ? 20 : 200;
    const Index steps_per_orbit = 400;

    const ElementHistory history = integrate(orbits, steps_per_orbit);

    const Real periapsis = kepler_periapsis_distance(kOrbit);
    const Real apoapsis = kOrbit.semi_major_axis * (1 + kOrbit.eccentricity);

    CAPTURE(orbits, steps_per_orbit, history.semi_major_axis_error, history.eccentricity_error,
            history.precession, history.closest_approach, history.furthest_separation, periapsis,
            apoapsis);

    REQUIRE(history.semi_major_axis_error <= kElementTolerance);
    REQUIRE(history.eccentricity_error <= kElementTolerance);

    // The orbit stays inside the annulus its elements describe. A solver whose
    // force fell off too slowly would pull the bodies together over many orbits
    // and this is where that would appear first.
    REQUIRE(history.closest_approach >= periapsis - kElementTolerance);
    REQUIRE(history.furthest_separation <= apoapsis + kElementTolerance);

    REQUIRE(history.precession <= kPrecessionBound);
}

TEST_CASE("the orbit's axis turns only as fast as the timestep explains", "[validation][solvers]") {
    // The test above bounds the precession. This one attributes it, and that is
    // the part which says something about the solver rather than about velocity
    // Verlet.
    //
    // Bertrand's theorem is the reason the question is sharp: of all central
    // force laws, only the inverse square and the harmonic one produce closed
    // orbits. Any other exponent, and any softening left switched on by accident,
    // turns the axis of an eccentric orbit at a rate set by the physics, which no
    // choice of timestep changes. The precession of a second-order integrator is
    // set by the timestep instead and falls as its square.
    //
    // So the two are told apart by halving the step: an artefact drops by four,
    // and a wrong force law does not move.
    const Index orbits = kSinglePrecision ? 20 : 50;

    const ElementHistory coarse = integrate(orbits, 200);
    const ElementHistory fine = integrate(orbits, 400);

    const Real ratio = coarse.precession / fine.precession;

    CAPTURE(orbits, coarse.precession, fine.precession, ratio);

    // Four, loosely bounded. The measured ratio is 4.0 in double precision; the
    // window is wide enough that it is not a claim about the third digit, and
    // narrow enough to exclude both a precession that does not respond to the
    // timestep and one that responds far too strongly.
    REQUIRE(ratio > 3);
    REQUIRE(ratio < 5);
}
