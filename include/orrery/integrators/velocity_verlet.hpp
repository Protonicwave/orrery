#pragma once

/// \file
/// Velocity Verlet, the method this project integrates with by default.
///
/// Second order, symplectic, time-reversible, one force evaluation per step and
/// no storage beyond the state itself. The combination is why it has been the
/// standard choice in stellar dynamics and molecular dynamics for decades: for
/// a long run the bounded energy error of a symplectic method is worth more than
/// the smaller error per step of a higher-order one, and ADR-0011 gives that
/// argument in full.
///
/// The step is written in kick-drift-kick form:
///
///     v(t + h/2) = v(t)       + (h/2) a(x(t))
///     x(t + h)   = x(t)       + h v(t + h/2)
///     v(t + h)   = v(t + h/2) + (h/2) a(x(t + h))
///
/// The two half-kicks make the step symmetric under `h -> -h`, which is what
/// makes it exactly reversible, and they surround a single evaluation of the
/// acceleration. The drift-kick-drift arrangement is the same method to the same
/// order and would work equally well; kick-drift-kick is chosen because it ends
/// on an acceleration evaluated at the final positions, which is precisely the
/// invariant in `integrator.hpp` that lets the next step reuse it.

#include <string_view>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/integrators/acceleration_field.hpp"
#include "orrery/integrators/integrator.hpp"

namespace orrery::integrators {

/// One kick-drift-kick step, as a free function.
///
/// Exposed separately from the class because Yoshida's fourth-order method is
/// literally three of these steps with weighted timesteps, and composing the
/// function is better than restating its arithmetic in a second file where the
/// two could drift apart. It carries the same precondition and postcondition as
/// `Integrator::step`.
void velocity_verlet_step(core::ParticleData& data, core::Real timestep, AccelerationField& field);

/// The second-order symplectic integrator described above.
class VelocityVerlet final : public Integrator {
public:
    void step(core::ParticleData& data, core::Real timestep, AccelerationField& field) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "velocity Verlet"; }

    [[nodiscard]] int order() const noexcept override { return 2; }

    [[nodiscard]] bool is_symplectic() const noexcept override { return true; }

    [[nodiscard]] core::Index force_evaluations_per_step() const noexcept override { return 1; }
};

} // namespace orrery::integrators
