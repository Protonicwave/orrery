#include "orrery/initial_conditions/kepler.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/units.hpp"
#include "orrery/core/vec3.hpp"

namespace orrery::initial_conditions {

namespace {

using core::kGravitationalConstant;
using core::Real;

/// Reject a description that has no ellipse behind it.
///
/// Checked in one place rather than in each function, because a period and an
/// energy computed from the same invalid elements would each be wrong in their
/// own way and the caller would have to notice twice.
void require_bound_orbit(const KeplerParameters& parameters) {
    if (!(parameters.primary_mass > 0) || !(parameters.secondary_mass > 0)) {
        throw std::invalid_argument{"A Kepler orbit needs two positive masses"};
    }

    if (!(parameters.semi_major_axis > 0)) {
        throw std::invalid_argument{"A Kepler orbit needs a positive semi-major axis"};
    }

    // Written as a pair of positive assertions negated, rather than as
    // `e < 0 || e >= 1`, so that a NaN is rejected instead of passing both
    // comparisons and producing a configuration of NaNs.
    if (!(parameters.eccentricity >= 0) || !(parameters.eccentricity < 1)) {
        throw std::invalid_argument{"A bound Kepler orbit needs an eccentricity in [0, 1)"};
    }
}

[[nodiscard]] Real total_mass(const KeplerParameters& parameters) {
    return parameters.primary_mass + parameters.secondary_mass;
}

[[nodiscard]] Real reduced_mass(const KeplerParameters& parameters) {
    return parameters.primary_mass * parameters.secondary_mass / total_mass(parameters);
}

} // namespace

core::ParticleData make_kepler_orbit(const KeplerParameters& parameters) {
    require_bound_orbit(parameters);

    const Real separation = kepler_periapsis_distance(parameters);

    // The vis-viva equation at periapsis. The relative speed there is entirely
    // transverse, so the state needs no decomposition into radial and angular
    // parts and this one number fixes the orbit.
    const Real speed = std::sqrt(kGravitationalConstant * total_mass(parameters) *
                                 (1 + parameters.eccentricity) / separation);

    // Each body sits on its own side of the centre of mass, at a distance in
    // inverse proportion to its mass, and moves in the opposite direction to
    // the other. Written as fractions of the total mass so that the weighted
    // positions cancel: exactly for equal masses, and otherwise to the one
    // rounding that separates m1(m2/M) from m2(m1/M). Recentring afterwards
    // would not improve on that and would perturb the separation, which is the
    // quantity every analytic comparison below depends on.
    const Real primary_fraction = parameters.primary_mass / total_mass(parameters);
    const Real secondary_fraction = parameters.secondary_mass / total_mass(parameters);

    core::ParticleData data;
    data.reserve(2);
    data.add(core::Vec3{-secondary_fraction * separation, 0, 0},
             core::Vec3{0, -secondary_fraction * speed, 0}, parameters.primary_mass);
    data.add(core::Vec3{primary_fraction * separation, 0, 0},
             core::Vec3{0, primary_fraction * speed, 0}, parameters.secondary_mass);

    return data;
}

Real kepler_period(const KeplerParameters& parameters) {
    require_bound_orbit(parameters);

    const Real axis = parameters.semi_major_axis;
    return 2 * std::numbers::pi_v<Real> *
           std::sqrt(axis * axis * axis / (kGravitationalConstant * total_mass(parameters)));
}

Real kepler_energy(const KeplerParameters& parameters) {
    require_bound_orbit(parameters);

    return -kGravitationalConstant * parameters.primary_mass * parameters.secondary_mass /
           (2 * parameters.semi_major_axis);
}

Real kepler_angular_momentum(const KeplerParameters& parameters) {
    require_bound_orbit(parameters);

    const Real eccentricity = parameters.eccentricity;
    return reduced_mass(parameters) *
           std::sqrt(kGravitationalConstant * total_mass(parameters) * parameters.semi_major_axis *
                     (1 - (eccentricity * eccentricity)));
}

Real kepler_periapsis_distance(const KeplerParameters& parameters) {
    require_bound_orbit(parameters);

    return parameters.semi_major_axis * (1 - parameters.eccentricity);
}

} // namespace orrery::initial_conditions
