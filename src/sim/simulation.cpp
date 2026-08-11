#include "orrery/sim/simulation.hpp"

#include <memory>
#include <utility>

#include "orrery/backend/executor.hpp"
#include "orrery/core/diagnostics.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/integrators/integrator.hpp"
#include "orrery/sim/run_output.hpp"
#include "orrery/solvers/force_solver.hpp"

namespace orrery::sim {

Simulation::Simulation(core::ParticleData particles, std::unique_ptr<solvers::ForceSolver> solver,
                       std::unique_ptr<integrators::Integrator> integrator, core::Real timestep,
                       std::unique_ptr<backend::Executor> executor,
                       std::unique_ptr<RunOutput> output)
    : executor_(std::move(executor)),
      solver_(std::move(solver)),
      integrator_(std::move(integrator)),
      output_(std::move(output)),
      particles_(std::move(particles)),
      timestep_(timestep) {
    integrators::refresh_accelerations(particles_, *solver_);
}

// Out of line, all four, because `RunOutput` is only declared in the header.
// A unique pointer needs the complete type where it is destroyed, and defining
// these here is what keeps `sim/run_output.hpp` off the include path of
// everything that mentions a simulation.
Simulation::~Simulation() = default;
Simulation::Simulation(Simulation&&) noexcept = default;
Simulation& Simulation::operator=(Simulation&&) noexcept = default;

void Simulation::step() {
    integrator_->step(particles_, timestep_, *solver_);
    ++step_;
}

void Simulation::run(core::Index steps) {
    if (output_) {
        // The state the run starts from is a result too. Without this a
        // trajectory would begin one step in, and a diagnostics file would have
        // nothing to measure its energy error against but its own second row.
        output_->record(*this, steps == 0);
    }

    for (core::Index taken = 0; taken < steps; ++taken) {
        step();
        if (output_) {
            output_->record(*this, taken + 1 == steps);
        }
    }
}

void Simulation::restore(core::ParticleData particles, core::Index step,
                         bool accelerations_are_current) {
    particles_ = std::move(particles);
    step_ = step;

    if (!accelerations_are_current) {
        integrators::refresh_accelerations(particles_, *solver_);
    }
}

core::Diagnostics Simulation::measure() const {
    return core::measure_diagnostics(particles_, solver_->softening());
}

} // namespace orrery::sim
