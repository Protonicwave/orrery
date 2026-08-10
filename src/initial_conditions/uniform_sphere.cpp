#include "orrery/initial_conditions/uniform_sphere.hpp"

#include <cmath>
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

void require_valid_sphere(const UniformSphereParameters& parameters) {
    if (!(parameters.total_mass > 0)) {
        throw std::invalid_argument{"A uniform sphere needs a positive total mass"};
    }

    if (!(parameters.radius > 0)) {
        throw std::invalid_argument{"A uniform sphere needs a positive radius"};
    }
}

} // namespace

core::ParticleData make_uniform_sphere(const UniformSphereParameters& parameters,
                                       core::RandomSource& random) {
    require_valid_sphere(parameters);

    if (parameters.count == 0) {
        return {};
    }

    const Real particle_mass = parameters.total_mass / static_cast<Real>(parameters.count);

    core::ParticleData data;
    data.reserve(parameters.count);

    for (core::Index particle = 0; particle < parameters.count; ++particle) {
        // The cube root of a uniform draw, not a uniform draw. Volume grows as
        // the cube of the radius, so a radius drawn uniformly would put a third
        // of the particles in the innermost eighth of the volume and produce a
        // centrally concentrated sphere rather than a uniform one.
        const Real radius = parameters.radius * std::cbrt(random.uniform());

        data.add(radius * random.unit_vector(), core::Vec3{}, particle_mass);
    }

    // The velocity half of this is a subtraction of zero from zero, since the
    // configuration is at rest: a sum of zeros is exactly zero, so the momentum
    // stays exactly zero rather than becoming a rounded approximation to it.
    move_to_centre_of_mass_frame(data);

    return data;
}

Real uniform_sphere_potential_energy(const UniformSphereParameters& parameters) {
    require_valid_sphere(parameters);

    const Real mass = parameters.total_mass;
    return -3 * kGravitationalConstant * mass * mass / (5 * parameters.radius);
}

} // namespace orrery::initial_conditions
