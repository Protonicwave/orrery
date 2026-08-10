#pragma once

/// \file
/// What the integrator tests share: the field they run under, the list of
/// methods every general property is asserted over, and one small piece of
/// plumbing for reporting a method's name.
///
/// An integrator cannot be tested without something to integrate, and the force
/// solver arrives in Phase 5. What is written here is therefore a test
/// instrument rather than a preview of that phase: the most direct statement of
/// softened pairwise gravity that can be written, chosen for being obviously
/// correct rather than for being fast, with no threading, no vectorisation, no
/// interaction counting and no attempt at the memory behaviour that makes the
/// real kernel worth writing. It uses the shared softening from `core` so that
/// the potential energy the diagnostics measure is the potential this field
/// differentiates, which is what makes an energy conservation result mean
/// anything at all (ADR-0008).
///
/// The tests that matter here compare against analytic results, not against this
/// code: a circular orbit's exact rotation, an orbital period from Kepler's third
/// law, a convergence order from theory. A mistake in this file would show up as
/// a failure in every one of them.

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/units.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/integrators/acceleration_field.hpp"
#include "orrery/integrators/integrator.hpp"
#include "orrery/integrators/runge_kutta4.hpp"
#include "orrery/integrators/velocity_verlet.hpp"
#include "orrery/integrators/yoshida4.hpp"

namespace orrery::testing {

/// Direct summation over every pair, counting the times it is asked.
///
/// The count is what the tests use to check that each integrator spends the
/// number of force evaluations per step it says it does, which is the figure any
/// comparison between methods has to be normalised by.
class CountingDirectField final : public integrators::AccelerationField {
public:
    CountingDirectField() = default;

    explicit CountingDirectField(core::Softening softening) : softening_(softening) {}

    void evaluate(core::Vec3Span<const core::Real> positions, std::span<const core::Real> masses,
                  core::Vec3Span<core::Real> accelerations) override {
        ++evaluations_;

        const core::Index count = positions.size();

        for (core::Index i = 0; i < count; ++i) {
            core::Vec3 acceleration{};

            for (core::Index j = 0; j < count; ++j) {
                if (i == j) {
                    continue;
                }

                const core::Vec3 separation = positions.get(j) - positions.get(i);
                const core::Real factor = core::softened_inverse_distance_cubed(
                    core::squared_norm(separation), softening_);
                acceleration += core::kGravitationalConstant * masses[j] * factor * separation;
            }

            accelerations.set(i, acceleration);
        }
    }

    [[nodiscard]] core::Index evaluations() const noexcept { return evaluations_; }

    void reset_evaluations() noexcept { evaluations_ = 0; }

private:
    core::Softening softening_;
    core::Index evaluations_ = 0;
};

/// Every integrator in the project.
///
/// A property required of all of them is then stated once, and a method added in
/// a later phase is covered by adding one line here rather than by remembering
/// which test files exist.
[[nodiscard]] inline std::vector<std::unique_ptr<integrators::Integrator>> all_integrators() {
    std::vector<std::unique_ptr<integrators::Integrator>> methods;
    methods.push_back(std::make_unique<integrators::VelocityVerlet>());
    methods.push_back(std::make_unique<integrators::Yoshida4>());
    methods.push_back(std::make_unique<integrators::RungeKutta4>());
    return methods;
}

/// The symmetric methods, which are the ones exact reversibility and exact
/// angular momentum conservation are claimed of.
[[nodiscard]] inline std::vector<std::unique_ptr<integrators::Integrator>> symmetric_integrators() {
    std::vector<std::unique_ptr<integrators::Integrator>> methods;
    methods.push_back(std::make_unique<integrators::VelocityVerlet>());
    methods.push_back(std::make_unique<integrators::Yoshida4>());
    return methods;
}

/// A method's name as an owning string.
///
/// Catch2 renders a `std::string_view` only when its own library was compiled
/// with the same standard as the tests that use it, which is not something this
/// project controls, so the conversion happens here instead of in every failure
/// message.
[[nodiscard]] inline std::string name_of(const integrators::Integrator& integrator) {
    return std::string{integrator.name()};
}

} // namespace orrery::testing
