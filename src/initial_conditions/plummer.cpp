#include "orrery/initial_conditions/plummer.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/units.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/initial_conditions/centre_of_mass_frame.hpp"

namespace orrery::initial_conditions {

namespace {

using core::kGravitationalConstant;
using core::Real;

void require_valid_model(const PlummerParameters& parameters) {
    if (!(parameters.total_mass > 0)) {
        throw std::invalid_argument{"A Plummer sphere needs a positive total mass"};
    }

    if (!(parameters.scale_radius > 0)) {
        throw std::invalid_argument{"A Plummer sphere needs a positive scale radius"};
    }

    // As in the Kepler generator, the comparisons are positive assertions
    // negated so that a NaN cutoff is rejected rather than accepted by both
    // halves of a disjunction.
    if (!(parameters.mass_fraction_cutoff > 0) || !(parameters.mass_fraction_cutoff < 1)) {
        throw std::invalid_argument{"A Plummer sphere needs a mass fraction cutoff in (0, 1)"};
    }
}

/// The radius enclosing a given fraction of the model's mass.
///
/// The cumulative mass profile `M(r)/M = r^3 / (r^2 + a^2)^(3/2)` inverts in
/// closed form, so radii need no rejection sampling and the number of draws per
/// particle does not depend on where the particle lands.
[[nodiscard]] Real radius_enclosing(Real mass_fraction, Real scale_radius) {
    return scale_radius / std::sqrt(std::pow(mass_fraction, Real{-2} / 3) - 1);
}

/// The escape speed from the model's potential at a given radius.
///
/// `sqrt(2 G M / sqrt(r^2 + a^2))`, from the Plummer potential. The velocity
/// distribution is expressed as a fraction of this, which is what confines the
/// sampled speeds to the range that stays bound.
[[nodiscard]] Real escape_speed(Real radius, const PlummerParameters& parameters) {
    const Real scale = parameters.scale_radius;
    const Real softened = std::sqrt((radius * radius) + (scale * scale));

    return std::sqrt(2 * kGravitationalConstant * parameters.total_mass / softened);
}

/// A speed as a fraction of the escape speed, drawn from the model's
/// distribution function.
///
/// The distribution of that fraction is `g(q) = q^2 (1 - q^2)^(7/2)` on [0, 1],
/// which has no closed-form inverse, so it is sampled by rejection against a
/// uniform envelope. The envelope's height is the constant below.
[[nodiscard]] Real sample_speed_fraction(core::RandomSource& random) {
    // The maximum of g is 0.0922, at q = sqrt(2/9). An envelope of 0.1 is the
    // smallest round number above it, so a little over a third of the draws are
    // rejected. Aarseth, Henon and Wielen use the same value, which makes a
    // sample from this code comparable with one from theirs.
    constexpr Real kEnvelope = static_cast<Real>(0.1);

    while (true) {
        const Real fraction = random.uniform();
        const Real complement = 1 - (fraction * fraction);

        // (1 - q^2)^(7/2) as a cube times a square root rather than through
        // `pow` with a fractional exponent: three multiplications and one root
        // instead of a logarithm and an exponential, and the result is the same
        // to within a rounding either way.
        const Real density =
            fraction * fraction * complement * complement * complement * std::sqrt(complement);

        if (kEnvelope * random.uniform() < density) {
            return fraction;
        }
    }
}

} // namespace

core::ParticleData make_plummer_sphere(const PlummerParameters& parameters,
                                       core::RandomSource& random) {
    require_valid_model(parameters);

    // A sphere of no particles is empty rather than an error: a caller sweeping
    // a range of counts should not have to special-case its first entry. The
    // early return is what keeps the mass per particle below a division by a
    // positive number.
    if (parameters.count == 0) {
        return {};
    }

    const Real particle_mass = parameters.total_mass / static_cast<Real>(parameters.count);

    core::ParticleData data;
    data.reserve(parameters.count);

    for (core::Index particle = 0; particle < parameters.count; ++particle) {
        // The enclosed mass fraction is the uniform variable, so the particles
        // are distributed in mass rather than in radius. Drawing from the
        // half-open interval below the cutoff keeps the sample inside the
        // radius that fraction encloses; a draw of exactly zero puts a particle
        // at the centre, which is a legitimate outcome rather than an edge case.
        const Real mass_fraction = random.uniform(0, parameters.mass_fraction_cutoff);
        const Real radius = radius_enclosing(mass_fraction, parameters.scale_radius);
        const Real speed = sample_speed_fraction(random) * escape_speed(radius, parameters);

        // Independent directions for the position and the velocity. That is
        // what makes the velocity distribution isotropic at every radius, which
        // is the property the model has and the reason the sample is in
        // equilibrium rather than rotating or radially biased.
        data.add(radius * random.unit_vector(), speed * random.unit_vector(), particle_mass);
    }

    move_to_centre_of_mass_frame(data);

    return data;
}

Real plummer_potential_energy(const PlummerParameters& parameters) {
    require_valid_model(parameters);

    const Real mass = parameters.total_mass;
    return -3 * std::numbers::pi_v<Real> * kGravitationalConstant * mass * mass /
           (32 * parameters.scale_radius);
}

Real plummer_kinetic_energy(const PlummerParameters& parameters) {
    // Half the magnitude of the potential energy, by the virial theorem. Stated
    // that way rather than as its own formula so that the two cannot drift
    // apart, since the relation between them is the thing being asserted.
    return -plummer_potential_energy(parameters) / 2;
}

Real plummer_total_energy(const PlummerParameters& parameters) {
    return plummer_kinetic_energy(parameters) + plummer_potential_energy(parameters);
}

} // namespace orrery::initial_conditions
