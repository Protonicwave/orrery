#include <cmath>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "integrator_fixtures.hpp"
#include "orrery/core/diagnostics.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/initial_conditions/kepler.hpp"
#include "orrery/integrators/integrator.hpp"
#include "orrery/integrators/runge_kutta4.hpp"
#include "orrery/integrators/velocity_verlet.hpp"
#include "orrery/integrators/yoshida4.hpp"

namespace {

using orrery::core::Index;
using orrery::core::kSinglePrecision;
using orrery::core::measure_diagnostics;
using orrery::core::ParticleData;
using orrery::core::Real;
using orrery::core::Softening;
using orrery::initial_conditions::kepler_period;
using orrery::initial_conditions::KeplerParameters;
using orrery::initial_conditions::make_kepler_orbit;
using orrery::integrators::Integrator;
using orrery::integrators::refresh_accelerations;
using orrery::integrators::RungeKutta4;
using orrery::integrators::VelocityVerlet;
using orrery::integrators::Yoshida4;
using orrery::testing::CountingDirectField;

/// An eccentric orbit, because a circular one would not distinguish the methods.
///
/// At zero eccentricity the acceleration a particle feels never changes in
/// magnitude, and every method here handles that case far better than it handles
/// the general one. At an eccentricity of one half the separation varies by a
/// factor of three between periapsis and apoapsis and the acceleration by a
/// factor of nine, so each orbit contains a short passage where the timestep is
/// least adequate. That is where the energy error of a step is made, and it is
/// what the two families of method then do with it that this test is about.
constexpr KeplerParameters kOrbit{.primary_mass = 1,
                                  .secondary_mass = 1,
                                  .semi_major_axis = 1,
                                  .eccentricity = static_cast<Real>(0.5)};

constexpr Index kStepsPerOrbit = 200;

/// How long the comparison runs.
///
/// Long enough that a drift proportional to elapsed time separates itself from
/// an oscillation that does not. The single-precision build runs a shorter
/// integration, because there the round-off floor is high enough that a very
/// long run measures the accumulation of rounding rather than the property of
/// the method.
constexpr Index kOrbits = kSinglePrecision ? 60 : 400;

/// The band the symplectic methods have to stay inside.
///
/// Velocity Verlet, the wider of the two, holds this orbit to a relative energy
/// error of 2.7e-3 at two hundred steps an orbit, and Yoshida's method to 2.0e-5.
/// The bound is the larger of those rounded up rather than fitted tightly: the
/// claim being tested is that the error stays bounded and small, and a bound
/// pushed against the last digit of one machine's arithmetic would be a claim
/// about the machine instead.
constexpr Real kSymplecticEnvelope =
    kSinglePrecision ? static_cast<Real>(0.02) : static_cast<Real>(0.005);

/// How often the energy is measured, in steps.
///
/// Twenty samples an orbit rather than one at the end of each. The energy error
/// of a symplectic method oscillates over the orbit and is largest near
/// periapsis, so a single sample per orbit taken at a fixed phase measures a
/// point on that oscillation rather than its extent, and the point it lands on
/// moves as the integration's phase slips against the true one. Sampling through
/// the orbit measures the envelope itself, which is the quantity this test is
/// about.
constexpr Index kStepsPerSample = 10;

/// The extent of the relative energy error over a run and over its two ends.
struct EnergyHistory {
    /// The largest relative error over the first twentieth of the run.
    Real early_extreme = 0;

    /// The largest over the last twentieth.
    Real late_extreme = 0;

    /// The largest over the whole run.
    Real extreme = 0;
};

[[nodiscard]] EnergyHistory integrate(Integrator& integrator) {
    ParticleData data = make_kepler_orbit(kOrbit);
    CountingDirectField field;

    // No softening. This is a two-body problem between point masses and the
    // energy being conserved is the point-mass energy; softening it would put a
    // modelling choice inside a measurement of a numerical method.
    const Softening softening;
    const Real reference = measure_diagnostics(data, softening).total_energy();
    const Real timestep = kepler_period(kOrbit) / static_cast<Real>(kStepsPerOrbit);

    refresh_accelerations(data, field);

    const Index total_steps = kOrbits * kStepsPerOrbit;
    const Index window = total_steps / 20;
    EnergyHistory history;

    for (Index step = 0; step < total_steps; ++step) {
        integrator.step(data, timestep, field);

        if ((step + 1) % kStepsPerSample != 0) {
            continue;
        }

        const Real energy = measure_diagnostics(data, softening).total_energy();
        const Real error = std::abs((energy - reference) / reference);

        history.extreme = error > history.extreme ? error : history.extreme;

        if (step < window) {
            history.early_extreme = error > history.early_extreme ? error : history.early_extreme;
        }

        if (step >= total_steps - window) {
            history.late_extreme = error > history.late_extreme ? error : history.late_extreme;
        }
    }

    return history;
}

} // namespace

TEST_CASE("the symplectic methods hold their energy error inside an envelope",
          "[validation][integrators]") {
    // The second headline result of this phase, and the reason the project
    // integrates with a second-order method rather than a fourth-order one.
    //
    // A symplectic integrator does not conserve the energy of the system it was
    // given. It conserves the energy of a nearby system exactly, and the two
    // differ by an amount that depends on the timestep and not on how long the
    // integration has run. The measured error therefore oscillates over each
    // orbit and comes back to where it was, indefinitely. The test states that
    // as: the error in the last twentieth of the run is no larger than the error
    // in the first twentieth, over a run of hundreds of orbits.
    VelocityVerlet velocity_verlet;
    Yoshida4 yoshida;

    const EnergyHistory verlet_history = integrate(velocity_verlet);
    const EnergyHistory yoshida_history = integrate(yoshida);

    CAPTURE(kOrbits, kStepsPerOrbit, verlet_history.early_extreme, verlet_history.late_extreme,
            verlet_history.extreme, yoshida_history.early_extreme, yoshida_history.late_extreme,
            yoshida_history.extreme);

    const auto stays_bounded = [](const EnergyHistory& history) {
        REQUIRE(history.extreme <= kSymplecticEnvelope);

        // A factor of two rather than equality: the sampled extreme of an
        // oscillation is not identical between two windows of a finite run, and
        // the statement being made is that the envelope does not grow, not that
        // it is a constant.
        REQUIRE(history.late_extreme <= 2 * history.early_extreme);
    };

    stays_bounded(verlet_history);
    stays_bounded(yoshida_history);

    // Yoshida's method is the more accurate of the two at the same timestep,
    // which is what its two extra force evaluations buy. Both stay bounded; only
    // one of them stays bounded near zero.
    REQUIRE(yoshida_history.extreme < verlet_history.extreme);
}

TEST_CASE("RK4 loses energy without bound", "[validation][integrators]") {
    // The counterexample the plan asks for, and the most informative single
    // comparison this project makes.
    //
    // RK4 is two orders more accurate than velocity Verlet and spends four force
    // evaluations a step against its one, so at this timestep it is being given
    // four times the work and starts with a far smaller error. It still loses.
    // Its error is not the deviation from a nearby conserved quantity, because it
    // has none; it is an accumulation, and it grows with the number of steps
    // taken rather than settling. Extend the run and the gap widens.
    RungeKutta4 runge_kutta;
    Yoshida4 yoshida;

    const EnergyHistory drifting = integrate(runge_kutta);
    const EnergyHistory bounded = integrate(yoshida);

    CAPTURE(kOrbits, kStepsPerOrbit, drifting.early_extreme, drifting.late_extreme,
            bounded.early_extreme, bounded.late_extreme);

    // Growth by an order of magnitude between the two windows. The ratio the run
    // actually produces is far larger than this, and the bound is stated loosely
    // so that the test asserts the behaviour rather than one machine's arithmetic.
    REQUIRE(drifting.late_extreme >= 10 * drifting.early_extreme);

    // The comparison that matters: by the end of the run the method of the same
    // order that happens to be symplectic is more accurate than the one that is
    // not, having spent a quarter less work to get there.
    REQUIRE(drifting.late_extreme > bounded.late_extreme);
}
