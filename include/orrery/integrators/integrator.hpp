#pragma once

/// \file
/// The interface every time integrator presents, and the contract about
/// accelerations that makes the cheap ones cheap.
///
/// A gravitational N-body step is dominated by one thing: the force evaluation.
/// Everything else an integrator does is a handful of multiplications per
/// particle against N^2 interactions, so an integrator is judged almost entirely
/// by how many evaluations it needs per step and how much accuracy it gets for
/// them. Velocity Verlet needs one, Yoshida's fourth-order composition three,
/// and RK4 four.
///
/// Velocity Verlet only needs one because it uses the acceleration at the start
/// of the step, which is the same acceleration the previous step already
/// computed at the end of its own. Recomputing it would double the cost of the
/// method for no change in the answer. The saving is only available if the
/// acceleration survives between steps, so this interface makes that explicit:
///
///     `accelerations()` holds the acceleration at `positions()`.
///
/// `refresh_accelerations` establishes it, and every `step` requires it on entry
/// and restores it on exit. A caller that changes the positions behind an
/// integrator's back, or that swaps in a different acceleration field, calls
/// `refresh_accelerations` again.
/// ADR-0013 records why the invariant is stated here rather than left to each
/// integrator to manage privately.
///
/// The methods below are not `const` and an integrator is not safe to use from
/// two threads at once: RK4 owns stage buffers, and a scheme with a variable
/// step would own more. One integrator per simulation is the intended use.

#include <string_view>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/integrators/acceleration_field.hpp"

namespace orrery::integrators {

/// Establish the acceleration invariant for a configuration.
///
/// Call once before the first step, and again after anything outside the
/// integrator changes the positions, the masses or the field.
///
/// A free function rather than a member, because it belongs to the contract
/// rather than to any method: every integrator needs the invariant established
/// the same way, and one that established it differently would be establishing
/// something else. It lives in this header so that it is found where the
/// contract it serves is written.
inline void refresh_accelerations(core::ParticleData& data, AccelerationField& field) {
    field.evaluate(data.positions(), data.masses(), data.accelerations());
}

/// A method for advancing particle positions and velocities through time.
class Integrator {
public:
    virtual ~Integrator() = default;

    /// Advance the configuration by one step of `timestep`.
    ///
    /// Requires the invariant above and restores it. A negative timestep is
    /// meaningful and is not an error: the symmetric methods here are exactly
    /// time-reversible, and stepping backwards is how a test asserts it.
    virtual void step(core::ParticleData& data, core::Real timestep, AccelerationField& field) = 0;

    /// The method's name, for diagnostics output and test messages.
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// The order of the method: the power of the timestep that the error after
    /// a fixed interval of simulated time falls with.
    [[nodiscard]] virtual int order() const noexcept = 0;

    /// Whether the method preserves the phase-space volume of the flow.
    ///
    /// The property that decides whether the energy error stays inside a bounded
    /// envelope for ever or grows without limit, which for a simulation run over
    /// millions of steps matters more than the order does. ADR-0011 sets out the
    /// argument and the validation suite demonstrates it.
    [[nodiscard]] virtual bool is_symplectic() const noexcept = 0;

    /// How many times `step` calls the acceleration field.
    ///
    /// The honest unit of cost for comparing methods. Two schemes compared at
    /// equal timestep are not compared at equal work, and this is the number
    /// that converts between them.
    [[nodiscard]] virtual core::Index force_evaluations_per_step() const noexcept = 0;

protected:
    // As on `AccelerationField`, and for the same reasons.
    Integrator() = default;
    Integrator(const Integrator&) = default;
    Integrator(Integrator&&) = default;
    Integrator& operator=(const Integrator&) = default;
    Integrator& operator=(Integrator&&) = default;
};

} // namespace orrery::integrators
