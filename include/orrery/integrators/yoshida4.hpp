#pragma once

/// \file
/// Yoshida's fourth-order symplectic integrator, built from three velocity
/// Verlet steps.
///
/// Yoshida's construction says that any symmetric second-order method `S(h)` can
/// be raised to fourth order by composing three of its steps with weights
///
///     w1 = 1 / (2 - 2^(1/3)),  w0 = -2^(1/3) / (2 - 2^(1/3))
///
/// as `S(w1 h) S(w0 h) S(w1 h)`. Velocity Verlet is such a method, so this class
/// is that composition and nothing else. Writing it out as its own sequence of
/// kicks and drifts would restate arithmetic that already exists one file away,
/// and the two copies would then have to be kept in agreement by attention
/// rather than by construction.
///
/// The middle weight is negative: the composition takes a long step forward, a
/// shorter step backwards, and a long step forward again, and `2 w1 + w0 = 1`
/// leaves the net advance equal to `h`. That is not a quirk of this particular
/// construction. No composition of a symmetric method reaches an order above two
/// with all its substeps positive, which is a theorem rather than a limitation
/// of anyone's ingenuity. The practical consequence is that the intermediate
/// states inside a step are not states the system passes through, so a
/// diagnostic taken mid-step would be measuring nothing, and the largest
/// substep is about 1.35 times the nominal step rather than smaller than it.
///
/// Three force evaluations per step against velocity Verlet's one. It earns them
/// where the accuracy per unit of work matters more than the cost per step: at
/// equal work, that is at a third of the timestep, its error falls as the fourth
/// power of a step that is three times smaller rather than as the square, and it
/// remains symplectic while doing so.

#include <string_view>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/integrators/acceleration_field.hpp"
#include "orrery/integrators/integrator.hpp"

namespace orrery::integrators {

/// The fourth-order symplectic composition described above.
class Yoshida4 final : public Integrator {
public:
    void step(core::ParticleData& data, core::Real timestep, AccelerationField& field) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "Yoshida 4"; }

    [[nodiscard]] int order() const noexcept override { return 4; }

    [[nodiscard]] bool is_symplectic() const noexcept override { return true; }

    [[nodiscard]] core::Index force_evaluations_per_step() const noexcept override { return 3; }
};

} // namespace orrery::integrators
