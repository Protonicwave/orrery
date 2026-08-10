#include "orrery/integrators/velocity_verlet.hpp"

#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/integrators/acceleration_field.hpp"

namespace orrery::integrators {

namespace {

using core::Index;
using core::Real;
using core::Vec3Span;

/// `v += scale * a` for every particle.
///
/// The two kicks of a step differ only in when they happen, so they are one
/// function called twice rather than two loops that have to be kept the same.
void kick(Vec3Span<Real> velocities, Vec3Span<const Real> accelerations, Real scale) noexcept {
    const Index count = velocities.size();

    // Component by component rather than particle by particle. Each of these
    // three loops walks two contiguous arrays and is the shape a compiler
    // vectorises without being asked; interleaving them would be the same
    // arithmetic over six streams at once for no gain.
    for (Index i = 0; i < count; ++i) {
        velocities.x[i] += scale * accelerations.x[i];
    }

    for (Index i = 0; i < count; ++i) {
        velocities.y[i] += scale * accelerations.y[i];
    }

    for (Index i = 0; i < count; ++i) {
        velocities.z[i] += scale * accelerations.z[i];
    }
}

/// `x += scale * v` for every particle.
void drift(Vec3Span<Real> positions, Vec3Span<const Real> velocities, Real scale) noexcept {
    const Index count = positions.size();

    for (Index i = 0; i < count; ++i) {
        positions.x[i] += scale * velocities.x[i];
    }

    for (Index i = 0; i < count; ++i) {
        positions.y[i] += scale * velocities.y[i];
    }

    for (Index i = 0; i < count; ++i) {
        positions.z[i] += scale * velocities.z[i];
    }
}

} // namespace

void velocity_verlet_step(core::ParticleData& data, Real timestep, AccelerationField& field) {
    const Real half_step = timestep / 2;

    // The acceleration used here is the one left by the previous step, or by
    // `refresh_accelerations` before the first. This is the single evaluation
    // the method saves, and the invariant in `integrator.hpp` is what makes it
    // sound.
    kick(data.velocities(), data.accelerations(), half_step);
    drift(data.positions(), data.velocities(), timestep);

    field.evaluate(data.positions(), data.masses(), data.accelerations());

    kick(data.velocities(), data.accelerations(), half_step);
}

void VelocityVerlet::step(core::ParticleData& data, Real timestep, AccelerationField& field) {
    velocity_verlet_step(data, timestep, field);
}

} // namespace orrery::integrators
