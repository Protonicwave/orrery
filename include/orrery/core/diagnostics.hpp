#pragma once

/// \file
/// The conserved quantities, which are how this project knows it is right.
///
/// An N-body system with no external forces conserves its total energy, its
/// linear momentum and its angular momentum exactly. A numerical integration of
/// one does not, and the way it fails is informative: a symplectic scheme keeps
/// the energy inside a bounded envelope for ever while a non-symplectic one of
/// higher order drifts without limit, and that comparison is among the results
/// this project exists to produce. None of it can be shown without these
/// functions, so they are written before the integrators and the solvers that
/// they will judge.
///
/// Two properties matter more here than speed.
///
/// The potential energy uses the same softened potential as the force kernel
/// (`softening.hpp`). Softening changes the physics, not just the arithmetic:
/// the system whose energy is conserved is the one with softened masses in it.
/// An unsoftened diagnostic applied to a softened simulation would report a
/// drift that the integrator did not cause, and the conservation result would
/// be measuring the mismatch instead of the method.
///
/// The sums are compensated. Total momentum is a sum of terms that cancel to
/// something near zero, and the potential energy of N particles is a sum of
/// N(N-1)/2 terms of one sign; both lose digits under ordinary accumulation,
/// the first to cancellation and the second to a running total that grows far
/// beyond any one term. A diagnostic claiming conservation to round-off has to
/// be more accurate than the thing it measures, so the accumulation is carried
/// with a correction term rather than left to chance.
///
/// The potential energy costs N^2 operations, as the direct solver does. It is
/// a diagnostic taken every few hundred steps rather than every step, so this
/// is the right trade: it shares no code with the solver and therefore cannot
/// inherit a bug from it, which is what makes it evidence.

#include <span>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/core/vec3_span.hpp"

namespace orrery::core {

/// Every conserved quantity of a configuration, measured at one instant.
///
/// An aggregate, because it is a record of measurements with no invariant
/// relating them. The two derived quantities are member functions rather than
/// fields so that they cannot be stale.
struct Diagnostics {
    Real kinetic_energy{};
    Real potential_energy{};
    Vec3 linear_momentum{};
    Vec3 angular_momentum{};

    [[nodiscard]] constexpr Real total_energy() const noexcept {
        return kinetic_energy + potential_energy;
    }

    /// The virial ratio `2T / |U|`.
    ///
    /// One for a self-gravitating system in equilibrium, by the virial theorem.
    /// Below one the system is colder than equilibrium and will collapse before
    /// settling; above one it is hotter and will expand. It is the standard
    /// check that a sampled configuration is in the state its model claims,
    /// and it is how the Plummer sampler is validated.
    ///
    /// The convention differs between authors: some write the same statement as
    /// `-2T/U`, which is identical for a bound system, and some as `T/|U|`,
    /// which is a half where this is one. This project quotes `2T/|U|`
    /// throughout.
    ///
    /// A configuration whose particles do not interact has no potential energy
    /// and no virial statement to make. The division then returns an infinity
    /// or a quiet NaN rather than a plausible number, which is the intended
    /// behaviour: a nonsense question should give a visibly nonsense answer.
    [[nodiscard]] constexpr Real virial_ratio() const noexcept {
        const Real magnitude = potential_energy < 0 ? -potential_energy : potential_energy;
        return 2 * kinetic_energy / magnitude;
    }
};

/// The energy error at `energy`, relative to `reference`.
///
/// The quantity every energy plot in this project is of, and what the
/// `relative_energy_error` column of a diagnostics file carries: the total
/// energy less the total energy the run started with, divided by the magnitude
/// of that first value.
///
/// It is a function here rather than an expression at the one place that needed
/// it, because it now has two callers. A native run writes the column into a
/// CSV file; the WebAssembly module has no file to write and reports the same
/// number across its boundary instead. Two spellings of a definition are two
/// definitions, and the one that would drift is the one nobody has a file to
/// check against.
///
/// A run whose first measured energy is zero has no scale to be relative to,
/// and the absolute difference is returned instead. Two particles at rest at
/// infinite separation are the only configuration that does it, so this is a
/// degenerate case rather than a common one, and the alternative is a column of
/// infinities.
[[nodiscard]] Real relative_energy_error(Real reference, Real energy) noexcept;

/// The sum of the masses.
[[nodiscard]] Real total_mass(std::span<const Real> masses);

/// The mass-weighted mean position.
///
/// Every sampler in the project recentres on this, so that a configuration's
/// angular momentum is measured about its own centre rather than about whatever
/// offset the sampling happened to produce.
[[nodiscard]] Vec3 centre_of_mass(Vec3Span<const Real> positions, std::span<const Real> masses);

/// The mass-weighted mean velocity, which is the velocity of the centre of
/// mass and is what a sampler subtracts to bring a configuration to rest.
[[nodiscard]] Vec3 centre_of_mass_velocity(Vec3Span<const Real> velocities,
                                           std::span<const Real> masses);

/// `T = sum over i of m_i v_i^2 / 2`.
[[nodiscard]] Real kinetic_energy(Vec3Span<const Real> velocities, std::span<const Real> masses);

/// `U = -G sum over pairs of m_i m_j / sqrt(r_ij^2 + eps^2)`.
///
/// Each pair is counted once. Costs N^2 operations for the reason given at the
/// top of this file.
[[nodiscard]] Real potential_energy(Vec3Span<const Real> positions, std::span<const Real> masses,
                                    Softening softening);

/// `P = sum over i of m_i v_i`.
[[nodiscard]] Vec3 linear_momentum(Vec3Span<const Real> velocities, std::span<const Real> masses);

/// `L = sum over i of m_i (r_i cross v_i)`, about the origin.
///
/// About the origin rather than about the centre of mass, because that is the
/// quantity an integrator conserves. The two agree for the configurations this
/// project generates, since each is recentred at the origin and left at rest,
/// and where they do not the difference is itself constant.
[[nodiscard]] Vec3 angular_momentum(Vec3Span<const Real> positions, Vec3Span<const Real> velocities,
                                    std::span<const Real> masses);

/// Every quantity above, measured together.
///
/// The three linear-time quantities share one pass over the particles, which is
/// the shape a simulation wants when it records a diagnostic line; the
/// individual functions above walk the particles once each, which is the shape
/// a test wants when it asks for one number. The potential energy keeps its own
/// loop either way, since it is the only quantity that is not a sum over single
/// particles.
[[nodiscard]] Diagnostics measure_diagnostics(const ParticleData& data, Softening softening);

} // namespace orrery::core
