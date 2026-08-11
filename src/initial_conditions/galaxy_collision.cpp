#include "orrery/initial_conditions/galaxy_collision.hpp"

#include <cmath>
#include <span>
#include <stdexcept>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/units.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/initial_conditions/centre_of_mass_frame.hpp"
#include "orrery/initial_conditions/disc_galaxy.hpp"

namespace orrery::initial_conditions {

namespace {

using core::Index;
using core::kGravitationalConstant;
using core::Real;
using core::Vec3;

void require_valid_encounter(const GalaxyCollisionParameters& parameters) {
    // The two galaxies check themselves when they are sampled. What is left for
    // this function is the geometry of the encounter, which neither of them can
    // see.
    if (!(core::squared_norm(galaxy_collision_separation(parameters)) > 0)) {
        throw std::invalid_argument{
            "A galaxy collision needs a separation, and (0, 0, 0) has no direction"};
    }
    if (!(parameters.approach_speed >= 0)) {
        throw std::invalid_argument{"A galaxy collision needs an approach speed that is not "
                                    "negative"};
    }
}

/// Append `source`, shifted by `position` and `velocity`, to `data`.
///
/// The shift is applied here rather than by generating each galaxy about its
/// eventual centre, so that `make_disc_galaxy` produces one thing and this
/// produces the other. A generator that took an offset would be a generator
/// whose centre-of-mass frame was a parameter.
void append_shifted(core::ParticleData& data, const core::ParticleData& source, Vec3 position,
                    Vec3 velocity) {
    const core::Vec3Span<const Real> positions = source.positions();
    const core::Vec3Span<const Real> velocities = source.velocities();
    const std::span<const Real> masses = source.masses();

    for (Index particle = 0; particle < source.size(); ++particle) {
        data.add(positions.get(particle) + position, velocities.get(particle) + velocity,
                 masses[particle]);
    }
}

} // namespace

Vec3 galaxy_collision_separation(const GalaxyCollisionParameters& parameters) {
    return {parameters.separation, parameters.impact_parameter, 0};
}

Vec3 galaxy_collision_relative_velocity(const GalaxyCollisionParameters& parameters) {
    const Real total =
        disc_galaxy_total_mass(parameters.primary) + disc_galaxy_total_mass(parameters.secondary);
    const Real distance = core::norm(galaxy_collision_separation(parameters));
    const Real escape = std::sqrt(2 * kGravitationalConstant * total / distance);

    // Along the x axis alone rather than along the separation. The separation is
    // tilted by the impact parameter, and an approach directed along it would
    // aim the secondary at the primary's centre, which is a head-on collision
    // started from an oblique position rather than a grazing encounter.
    return {-parameters.approach_speed * escape, 0, 0};
}

Real galaxy_collision_orbit_energy(const GalaxyCollisionParameters& parameters) {
    const Real primary = disc_galaxy_total_mass(parameters.primary);
    const Real secondary = disc_galaxy_total_mass(parameters.secondary);
    const Real reduced = primary * secondary / (primary + secondary);
    const Real speed = core::norm(galaxy_collision_relative_velocity(parameters));
    const Real distance = core::norm(galaxy_collision_separation(parameters));

    return (reduced * speed * speed / 2) -
           (kGravitationalConstant * primary * secondary / distance);
}

core::ParticleData make_galaxy_collision(const GalaxyCollisionParameters& parameters,
                                         core::RandomSource& random) {
    require_valid_encounter(parameters);

    const core::ParticleData primary = make_disc_galaxy(parameters.primary, random);
    const core::ParticleData secondary = make_disc_galaxy(parameters.secondary, random);

    const Real primary_mass = disc_galaxy_total_mass(parameters.primary);
    const Real secondary_mass = disc_galaxy_total_mass(parameters.secondary);
    const Real total = primary_mass + secondary_mass;

    // Split the separation and the relative velocity between the two in inverse
    // proportion to their masses, which is what puts the pair's centre of mass
    // at the origin and at rest before the recentring below has to do anything.
    const Vec3 separation = galaxy_collision_separation(parameters);
    const Vec3 relative = galaxy_collision_relative_velocity(parameters);

    core::ParticleData data;
    data.reserve(primary.size() + secondary.size());

    append_shifted(data, primary, separation * (-secondary_mass / total),
                   relative * (-secondary_mass / total));
    append_shifted(data, secondary, separation * (primary_mass / total),
                   relative * (primary_mass / total));

    // Both galaxies were centred to round-off rather than exactly, so the pair
    // is too. The final recentring makes the total momentum exactly zero, which
    // is what the conservation tests want to hold a long run to, and shifts both
    // galaxies together so the separation between them is untouched.
    move_to_centre_of_mass_frame(data);

    return data;
}

} // namespace orrery::initial_conditions
