#include "orrery/initial_conditions/centre_of_mass_frame.hpp"

#include "orrery/core/diagnostics.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"

namespace orrery::initial_conditions {

void move_to_centre_of_mass_frame(core::ParticleData& data) {
    // Both offsets are computed before either is applied. Computing the
    // velocity offset after shifting the positions would be harmless today, but
    // the two are one measurement of one configuration and reading them at
    // different times is the kind of ordering dependence that survives until
    // something else changes and then does not.
    const core::Vec3 centre = core::centre_of_mass(data.positions(), data.masses());
    const core::Vec3 drift = core::centre_of_mass_velocity(data.velocities(), data.masses());

    const auto positions = data.positions();
    const auto velocities = data.velocities();

    for (core::Index particle = 0; particle < data.size(); ++particle) {
        positions.set(particle, positions.get(particle) - centre);
        velocities.set(particle, velocities.get(particle) - drift);
    }
}

} // namespace orrery::initial_conditions
