#include "orrery/core/diagnostics.hpp"

#include <cmath>
#include <span>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/units.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/core/vec3_span.hpp"

namespace orrery::core {

namespace {

/// A running total that keeps the digits an ordinary accumulation drops.
///
/// Neumaier's variant of compensated summation. Each addition computes the part
/// of the operand that the result was too coarse to hold and keeps it in a
/// second variable, which is added back at the end. The error of the total then
/// depends on the number of terms only through the correction, rather than
/// growing with it, which is what lets a momentum that should be zero be
/// reported as round-off rather than as drift.
///
/// Neumaier's form rather than Kahan's original because it is correct when a
/// single term is larger than the running total, which happens in every one of
/// these sums: the first term of any of them is larger than the zero it is
/// added to, and a momentum sum passes through zero repeatedly.
///
/// The arithmetic here is exact only under IEEE semantics. A build that
/// permitted the compiler to reassociate floating-point additions would be
/// entitled to prove the correction is always zero and delete it, which is one
/// of the reasons the relaxed flags considered in Phase 7 are a measured
/// decision rather than a default.
class CompensatedSum {
public:
    void add(Real value) noexcept {
        const Real updated = total_ + value;

        // The larger operand keeps its magnitude in the sum, so the lost part
        // is recovered from the smaller one. Which is which changes from term
        // to term, and choosing wrongly is exactly the case Kahan's original
        // form gets wrong.
        if (std::abs(total_) >= std::abs(value)) {
            correction_ += (total_ - updated) + value;
        } else {
            correction_ += (value - updated) + total_;
        }

        total_ = updated;
    }

    [[nodiscard]] Real total() const noexcept { return total_ + correction_; }

private:
    Real total_{};
    Real correction_{};
};

/// Three compensated sums, one per component.
///
/// The vector sums need the same treatment as the scalar ones and for a
/// stronger reason: a total momentum is the cancellation of N terms of both
/// signs, so its leading digits are all cancellation and what survives is
/// whatever the accumulation did not lose.
class CompensatedVec3Sum {
public:
    void add(Vec3 value) noexcept {
        x_.add(value.x);
        y_.add(value.y);
        z_.add(value.z);
    }

    [[nodiscard]] Vec3 total() const noexcept { return {x_.total(), y_.total(), z_.total()}; }

private:
    CompensatedSum x_;
    CompensatedSum y_;
    CompensatedSum z_;
};

} // namespace

Real relative_energy_error(Real reference, Real energy) noexcept {
    const Real scale = std::abs(reference);
    const Real difference = energy - reference;
    return scale > 0 ? difference / scale : difference;
}

Real total_mass(std::span<const Real> masses) {
    CompensatedSum sum;
    for (const Real mass : masses) {
        sum.add(mass);
    }

    return sum.total();
}

Vec3 centre_of_mass(Vec3Span<const Real> positions, std::span<const Real> masses) {
    CompensatedVec3Sum weighted;
    for (Index particle = 0; particle < masses.size(); ++particle) {
        weighted.add(positions.get(particle) * masses[particle]);
    }

    // An empty configuration has no centre. Returning the origin rather than
    // dividing by zero keeps a sampler's recentring step a no-op on an empty
    // container instead of filling it with NaNs it would then propagate.
    const Real mass = total_mass(masses);
    return mass == 0 ? Vec3{} : weighted.total() / mass;
}

Vec3 centre_of_mass_velocity(Vec3Span<const Real> velocities, std::span<const Real> masses) {
    const Real mass = total_mass(masses);
    return mass == 0 ? Vec3{} : linear_momentum(velocities, masses) / mass;
}

Real kinetic_energy(Vec3Span<const Real> velocities, std::span<const Real> masses) {
    CompensatedSum sum;
    for (Index particle = 0; particle < masses.size(); ++particle) {
        sum.add(masses[particle] * squared_norm(velocities.get(particle)));
    }

    // Halved once at the end rather than per particle: one rounding instead of
    // N, and the factor is exact in binary either way.
    return sum.total() / 2;
}

Real potential_energy(Vec3Span<const Real> positions, std::span<const Real> masses,
                      Softening softening) {
    const Index count = masses.size();

    CompensatedSum sum;
    for (Index i = 0; i < count; ++i) {
        const Vec3 position = positions.get(i);

        // Each pair once. Starting the inner loop above the outer index is what
        // makes this the sum over pairs rather than twice it, and it is also
        // why the diagonal term, a particle's infinite self-energy, never
        // appears.
        Real interaction = 0;
        for (Index j = i + 1; j < count; ++j) {
            const Vec3 separation = positions.get(j) - position;
            interaction +=
                masses[j] * softened_inverse_distance(squared_norm(separation), softening);
        }

        // The mass of particle i and the gravitational constant are factored
        // out of the inner loop, which halves its multiplications and, more
        // usefully here, means each term entering the compensated sum is a
        // whole row of the interaction matrix rather than a single pair. The
        // inner accumulation is ordinary: its terms all share a sign and a
        // scale, so it is the outer sum over rows that needs the correction.
        sum.add(-kGravitationalConstant * masses[i] * interaction);
    }

    return sum.total();
}

Vec3 linear_momentum(Vec3Span<const Real> velocities, std::span<const Real> masses) {
    CompensatedVec3Sum sum;
    for (Index particle = 0; particle < masses.size(); ++particle) {
        sum.add(velocities.get(particle) * masses[particle]);
    }

    return sum.total();
}

Vec3 angular_momentum(Vec3Span<const Real> positions, Vec3Span<const Real> velocities,
                      std::span<const Real> masses) {
    CompensatedVec3Sum sum;
    for (Index particle = 0; particle < masses.size(); ++particle) {
        sum.add(cross(positions.get(particle), velocities.get(particle)) * masses[particle]);
    }

    return sum.total();
}

Diagnostics measure_diagnostics(const ParticleData& data, Softening softening) {
    const auto positions = data.positions();
    const auto velocities = data.velocities();
    const auto masses = data.masses();

    CompensatedSum kinetic;
    CompensatedVec3Sum momentum;
    CompensatedVec3Sum angular;

    for (Index particle = 0; particle < masses.size(); ++particle) {
        const Vec3 position = positions.get(particle);
        const Vec3 velocity = velocities.get(particle);
        const Real mass = masses[particle];

        kinetic.add(mass * squared_norm(velocity));
        momentum.add(velocity * mass);
        angular.add(cross(position, velocity) * mass);
    }

    return {kinetic.total() / 2, potential_energy(positions, masses, softening), momentum.total(),
            angular.total()};
}

} // namespace orrery::core
