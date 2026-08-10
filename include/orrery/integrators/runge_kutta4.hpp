#pragma once

/// \file
/// Classical fourth-order Runge-Kutta, included as the counterexample.
///
/// RK4 is the integrator most people reach for first, and on this problem it is
/// the wrong choice. It is not symplectic and it is not time-reversible, so the
/// energy error of a long integration grows without bound instead of oscillating
/// inside an envelope, and it does so while costing four force evaluations per
/// step against velocity Verlet's one. The validation suite integrates the same
/// eccentric orbit with all three methods and shows exactly that: the symplectic
/// schemes stay inside a band for the whole run while RK4 drifts steadily out of
/// it. ADR-0011 is the argument and that test is the evidence.
///
/// It is not here only to lose. A non-symplectic method of known order is a
/// genuinely useful reference. Its error over a short interval is a clean power
/// of the timestep, uncontaminated by the bounded oscillation that makes a
/// symplectic method's error awkward to measure, so it is the easiest of the
/// three to check a convergence order against, and it is the natural thing to
/// compare a new solver against when the question is accuracy over one orbit
/// rather than stability over a million.
///
/// The step advances the first-order system
///
///     dx/dt = v,   dv/dt = a(x)
///
/// through the usual four stages. The stage positions are not states of the
/// system, which is why `AccelerationField` takes spans rather than a particle
/// store, and the stage derivatives have to be accumulated somewhere, which is
/// why this is the one integrator here that owns memory.

#include <string_view>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3_array.hpp"
#include "orrery/integrators/acceleration_field.hpp"
#include "orrery/integrators/integrator.hpp"

namespace orrery::integrators {

/// The classical fourth-order Runge-Kutta method.
///
/// Holds four scratch arrays of the same length as the configuration. They are
/// members rather than locals so that a step of a million particles does not
/// allocate and free 96 MB every time it is called; the first step of a run
/// sizes them and every step after finds them the right length already.
class RungeKutta4 final : public Integrator {
public:
    void step(core::ParticleData& data, core::Real timestep, AccelerationField& field) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "RK4"; }

    [[nodiscard]] int order() const noexcept override { return 4; }

    [[nodiscard]] bool is_symplectic() const noexcept override { return false; }

    /// Four, and the first of them is free.
    ///
    /// The first stage needs the acceleration at the starting positions, which
    /// the invariant in `integrator.hpp` says is already there. The fourth
    /// evaluation is the one that restores that invariant at the new positions,
    /// so it is not spent twice either. The classical count and the count this
    /// implementation pays are the same number.
    [[nodiscard]] core::Index force_evaluations_per_step() const noexcept override { return 4; }

private:
    /// Advance to one stage, evaluate the field there, and add the stage
    /// derivatives into the running sums with the given weight.
    void evaluate_stage(core::ParticleData& data, AccelerationField& field, core::Real scale,
                        core::Real weight);

    core::Vec3Array initial_positions_;
    core::Vec3Array initial_velocities_;
    core::Vec3Array position_derivative_sum_;
    core::Vec3Array velocity_derivative_sum_;
};

} // namespace orrery::integrators
