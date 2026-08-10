#include "orrery/integrators/runge_kutta4.hpp"

#include <span>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/integrators/acceleration_field.hpp"

namespace orrery::integrators {

namespace {

using core::Index;
using core::Real;
using core::Vec3Span;

void copy_component(std::span<Real> destination, std::span<const Real> source) noexcept {
    for (Index i = 0; i < destination.size(); ++i) {
        destination[i] = source[i];
    }
}

/// Move the state to a stage: `x = x0 + scale k_x`, `v = v0 + scale k_v`.
///
/// The stage derivative of position is the velocity the state currently holds,
/// and the stage derivative of velocity is the acceleration it currently holds,
/// so the two lines below are the whole of a Runge-Kutta stage for a second
/// order system. Reading the velocity before overwriting it is the only ordering
/// constraint, and it is why the two assignments are in one loop rather than in
/// two.
void advance_to_stage(std::span<Real> position, std::span<Real> velocity,
                      std::span<const Real> acceleration, std::span<const Real> initial_position,
                      std::span<const Real> initial_velocity, Real scale) noexcept {
    for (Index i = 0; i < position.size(); ++i) {
        const Real stage_velocity = velocity[i];
        position[i] = initial_position[i] + (scale * stage_velocity);
        velocity[i] = initial_velocity[i] + (scale * acceleration[i]);
    }
}

void accumulate_component(std::span<Real> sum, std::span<const Real> value, Real weight) noexcept {
    for (Index i = 0; i < sum.size(); ++i) {
        sum[i] += weight * value[i];
    }
}

/// `state = initial + scale * sum`, the final combination of the four stages.
void combine_component(std::span<Real> state, std::span<const Real> initial,
                       std::span<const Real> sum, Real scale) noexcept {
    for (Index i = 0; i < state.size(); ++i) {
        state[i] = initial[i] + (scale * sum[i]);
    }
}

void copy_vectors(Vec3Span<Real> destination, Vec3Span<const Real> source) noexcept {
    copy_component(destination.x, source.x);
    copy_component(destination.y, source.y);
    copy_component(destination.z, source.z);
}

} // namespace

void RungeKutta4::evaluate_stage(core::ParticleData& data, AccelerationField& field, Real scale,
                                 Real weight) {
    const Vec3Span<Real> positions = data.positions();
    const Vec3Span<Real> velocities = data.velocities();
    const Vec3Span<Real> accelerations = data.accelerations();
    const Vec3Span<const Real> initial_positions = initial_positions_.view();
    const Vec3Span<const Real> initial_velocities = initial_velocities_.view();

    advance_to_stage(positions.x, velocities.x, accelerations.x, initial_positions.x,
                     initial_velocities.x, scale);
    advance_to_stage(positions.y, velocities.y, accelerations.y, initial_positions.y,
                     initial_velocities.y, scale);
    advance_to_stage(positions.z, velocities.z, accelerations.z, initial_positions.z,
                     initial_velocities.z, scale);

    field.evaluate(positions, data.masses(), accelerations);

    // After the evaluation the velocity holds this stage's derivative of
    // position and the acceleration holds its derivative of velocity, so both
    // sums take the same weight.
    const Vec3Span<Real> position_sum = position_derivative_sum_.view();
    const Vec3Span<Real> velocity_sum = velocity_derivative_sum_.view();

    accumulate_component(position_sum.x, velocities.x, weight);
    accumulate_component(position_sum.y, velocities.y, weight);
    accumulate_component(position_sum.z, velocities.z, weight);
    accumulate_component(velocity_sum.x, accelerations.x, weight);
    accumulate_component(velocity_sum.y, accelerations.y, weight);
    accumulate_component(velocity_sum.z, accelerations.z, weight);
}

void RungeKutta4::step(core::ParticleData& data, Real timestep, AccelerationField& field) {
    const Index count = data.size();
    initial_positions_.resize(count);
    initial_velocities_.resize(count);
    position_derivative_sum_.resize(count);
    velocity_derivative_sum_.resize(count);

    copy_vectors(initial_positions_.view(), data.positions());
    copy_vectors(initial_velocities_.view(), data.velocities());

    // The first stage is evaluated at the starting state, so its derivatives are
    // the velocity the caller supplied and the acceleration the invariant
    // guarantees. Copying them into the sums rather than adding to them is what
    // clears the accumulator from the previous step.
    copy_vectors(position_derivative_sum_.view(), data.velocities());
    copy_vectors(velocity_derivative_sum_.view(), data.accelerations());

    // The three remaining stages: two at the midpoint and one at the far end,
    // weighted two, two and one against the first stage's one. This is the
    // classical tableau and nothing here is free to be chosen differently.
    const Real half_step = timestep / 2;
    evaluate_stage(data, field, half_step, 2);
    evaluate_stage(data, field, half_step, 2);
    evaluate_stage(data, field, timestep, 1);

    const Real sixth_step = timestep / 6;
    const Vec3Span<Real> positions = data.positions();
    const Vec3Span<Real> velocities = data.velocities();
    const Vec3Span<const Real> initial_positions = initial_positions_.view();
    const Vec3Span<const Real> initial_velocities = initial_velocities_.view();
    const Vec3Span<const Real> position_sum = position_derivative_sum_.view();
    const Vec3Span<const Real> velocity_sum = velocity_derivative_sum_.view();

    combine_component(positions.x, initial_positions.x, position_sum.x, sixth_step);
    combine_component(positions.y, initial_positions.y, position_sum.y, sixth_step);
    combine_component(positions.z, initial_positions.z, position_sum.z, sixth_step);
    combine_component(velocities.x, initial_velocities.x, velocity_sum.x, sixth_step);
    combine_component(velocities.y, initial_velocities.y, velocity_sum.y, sixth_step);
    combine_component(velocities.z, initial_velocities.z, velocity_sum.z, sixth_step);

    // The accelerations still hold the fourth stage's values, taken at a
    // position the system never occupies. Restoring the invariant costs the
    // fourth evaluation, and the next step's first stage is what spends it.
    field.evaluate(positions, data.masses(), data.accelerations());
}

} // namespace orrery::integrators
