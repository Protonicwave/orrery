#pragma once

/// \file
/// The object that owns a run.
///
/// Everything below this layer computes something and returns: a solver answers
/// one question about one instant, an integrator advances one step, a sampler
/// produces one configuration. None of them knows that a simulation is a thing
/// that goes on for a while. This class is where that is finally said, and it is
/// deliberately the smallest statement of it that works: a state, the two
/// objects that advance it, a step counter, and somewhere to send what happens.
///
/// ## Why the step counter is the clock
///
/// The simulated time is `step * timestep`, computed rather than accumulated. An
/// accumulated time would drift: adding a timestep a million times gives a
/// number that differs in its last bits from a million times the timestep, and
/// which of the two a run reported would depend on whether it had been resumed.
/// Multiplying keeps a resumed run and an uninterrupted one on the same clock,
/// which is one of the several small things the bitwise-resume requirement in
/// section 7 of the implementation plan turns out to mean.
///
/// ## What it owns and why the executor is in the list
///
/// The solver and the integrator, by unique pointer to their interfaces, so that
/// which of each is in use is a run-time choice made once at assembly. Also the
/// thread pool, when there is one, and that needs a word: a CPU solver is
/// constructed from a reference to an executor which it does not own and which
/// must outlive it (ADR-0017). Something has to hold that executor for exactly as
/// long as the solver, and the simulation is the only object here whose lifetime
/// is the run. It is stored before the solver so that the destruction order is
/// the reverse of the construction order, which is what the reference requires.
///
/// ## The acceleration invariant
///
/// The integrators require that `accelerations()` holds the acceleration at
/// `positions()` on entry to every step, and restore it on exit (ADR-0013). The
/// constructor establishes it, and `restore` re-establishes it after a state
/// arrives from somewhere else, so no caller of this class has to know the rule
/// exists.

#include <memory>

#include "orrery/backend/executor.hpp"
#include "orrery/core/diagnostics.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/integrators/integrator.hpp"
#include "orrery/solvers/force_solver.hpp"

namespace orrery::sim {

class RunOutput;

/// A gravitational simulation in progress.
class Simulation {
public:
    /// Take ownership of a configuration and the machinery to advance it.
    ///
    /// `executor` may be null, and is only held so that it outlives the solver
    /// that refers to it. `output` may be null, which is a run that computes and
    /// records nothing and is what most of the test suite wants.
    ///
    /// Establishes the acceleration invariant, which costs one force evaluation.
    Simulation(core::ParticleData particles, std::unique_ptr<solvers::ForceSolver> solver,
               std::unique_ptr<integrators::Integrator> integrator, core::Real timestep,
               std::unique_ptr<backend::Executor> executor = nullptr,
               std::unique_ptr<RunOutput> output = nullptr);

    ~Simulation();

    Simulation(const Simulation&) = delete;
    Simulation& operator=(const Simulation&) = delete;
    Simulation(Simulation&&) noexcept;
    Simulation& operator=(Simulation&&) noexcept;

    /// Advance by one step and count it.
    ///
    /// Does not record anything. `run` is what drives the output, and a caller
    /// that wants both writes the loop itself.
    void step();

    /// Record the current state, then take `steps` steps, recording as the
    /// output asks.
    ///
    /// The initial state is recorded before the first step, so that a
    /// trajectory begins at the configuration the run started from rather than
    /// one step into it, and a diagnostics file has a row to be relative to.
    void run(core::Index steps);

    /// Replace the state and the step counter, as a resume does.
    ///
    /// The particle count may differ from the current one. Re-establishes the
    /// acceleration invariant from the state given rather than trusting the
    /// accelerations in it, unless `accelerations_are_current` says they came
    /// from a checkpoint and are already the accelerations at these positions,
    /// in which case they are kept exactly. That distinction is the whole of
    /// bitwise resume: recomputing would give the same answer for every solver
    /// in this project and is not guaranteed to for one added later
    /// (ADR-0032).
    void restore(core::ParticleData particles, core::Index step,
                 bool accelerations_are_current = false);

    [[nodiscard]] core::Index step_index() const noexcept { return step_; }

    /// The simulated time, `step * timestep`.
    [[nodiscard]] core::Real time() const noexcept {
        return static_cast<core::Real>(step_) * timestep_;
    }

    [[nodiscard]] core::Real timestep() const noexcept { return timestep_; }

    [[nodiscard]] const core::ParticleData& particles() const noexcept { return particles_; }

    [[nodiscard]] const solvers::ForceSolver& solver() const noexcept { return *solver_; }

    [[nodiscard]] const integrators::Integrator& integrator() const noexcept {
        return *integrator_;
    }

    /// Measure the conserved quantities of the current state.
    ///
    /// Softened with whatever the solver softens with, asked of the solver
    /// rather than stored beside it, for the reason ADR-0008 gives: a potential
    /// energy computed with a different softening from the force kernel turns
    /// the conservation result into a measurement of the mismatch.
    ///
    /// Costs an N^2 pass and shares no code with the solver, which is what
    /// makes it evidence rather than a restatement. A run measures it every few
    /// hundred steps rather than every step.
    [[nodiscard]] core::Diagnostics measure() const;

private:
    /// Held first so that it is destroyed last. The solver below refers to it.
    std::unique_ptr<backend::Executor> executor_;

    std::unique_ptr<solvers::ForceSolver> solver_;
    std::unique_ptr<integrators::Integrator> integrator_;
    std::unique_ptr<RunOutput> output_;

    core::ParticleData particles_;
    core::Real timestep_;
    core::Index step_ = 0;
};

} // namespace orrery::sim
