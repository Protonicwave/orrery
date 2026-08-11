#pragma once

/// \file
/// Turning a configuration into the objects that run it.
///
/// This is the one file in the project that knows every solver, every
/// integrator and every initial condition by name. That is the point of it: the
/// alternative is each of the application, the test suite and any later
/// binding growing its own chain of comparisons against the same strings, and
/// three such chains agree until one of them is extended.
///
/// The functions are separate rather than folded into `assemble` because the
/// pieces are wanted separately. A test that measures a solver builds one
/// without sampling a million particles first, and a run that resumes from a
/// checkpoint builds everything except the initial conditions, which it is about
/// to overwrite with the state it read.
///
/// ## The GPU is a request, not a demand
///
/// A configuration naming a SYCL solver is asking for one. The device may be
/// absent, may not support the precision this build was configured for, or the
/// backend may not have been compiled in at all, and none of those is a broken
/// machine. So `make_solver` falls back to the CPU solver that computes the same
/// thing, writes a line to `report` saying it has, and only refuses when the
/// configuration set `allow_cpu_fallback` to false. A measurement whose whole
/// point is the hardware it ran on sets that; an ordinary run does not, and gets
/// its answer more slowly rather than not at all.

#include <iosfwd>
#include <memory>

#include "orrery/backend/executor.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/integrators/integrator.hpp"
#include "orrery/sim/configuration.hpp"
#include "orrery/sim/run_output.hpp"
#include "orrery/sim/simulation.hpp"
#include "orrery/solvers/force_solver.hpp"

namespace orrery::sim {

/// Sample or construct the configuration the run starts from.
///
/// Throws `std::invalid_argument` from the generators for parameters they
/// refuse. `problems_with` catches every such case first, so reaching one of
/// those means a caller assembled a configuration it had not checked.
[[nodiscard]] core::ParticleData make_initial_conditions(const Configuration& configuration);

[[nodiscard]] std::unique_ptr<integrators::Integrator>
make_integrator(const Configuration& configuration);

/// The scheduler the CPU solvers divide their work with.
///
/// Never null, including for the serial choice, which is a real executor that
/// runs the range on the calling thread. That keeps the reporting uniform: a
/// run can always say which scheme it used and how many workers it had.
[[nodiscard]] std::unique_ptr<backend::Executor> make_executor(const Configuration& configuration);

/// The solver, given the executor it will refer to.
///
/// `executor` must outlive the returned solver, which is why this takes a
/// pointer to one the caller is holding rather than making its own.
///
/// Writes to `report` when it does not return what was asked for. Throws
/// `std::runtime_error` when a GPU solver was demanded and no device can
/// provide one.
[[nodiscard]] std::unique_ptr<solvers::ForceSolver>
make_solver(const Configuration& configuration, backend::Executor* executor, std::ostream& report);

/// Everything above, assembled into a run that has not yet taken a step.
///
/// Costs one force evaluation, which the simulation's constructor spends
/// establishing the acceleration invariant, and for a sampled configuration the
/// time to draw it.
[[nodiscard]] Simulation assemble(const Configuration& configuration,
                                  std::unique_ptr<RunOutput> output, std::ostream& report);

} // namespace orrery::sim
