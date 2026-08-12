#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings.hpp"
#include "orrery/backend/executor.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/integrators/integrator.hpp"
#include "orrery/sim/assembly.hpp"
#include "orrery/sim/config_file.hpp"
#include "orrery/sim/configuration.hpp"
#include "orrery/sim/simulation.hpp"
#include "orrery/solvers/force_solver.hpp"
#include "orrery/solvers/interaction_count.hpp"
#include "particle_view.hpp"

namespace py = pybind11;
using namespace pybind11::literals;

using orrery::core::Index;
using orrery::core::ParticleData;
using orrery::core::Real;
using orrery::sim::Configuration;
using orrery::sim::Simulation;
using orrery::solvers::InteractionCount;

namespace orrery::python {

namespace {

/// Where a fallback notice goes.
///
/// `make_solver` writes a line when it does not return the solver that was
/// asked for, which on this hardware is the ordinary case for a configuration
/// naming a GPU solver on a machine without a usable device. The command-line
/// program puts that on the terminal. Here it goes to `sys.stderr`, so that it
/// appears in a notebook beside the cell that caused it rather than on whatever
/// stream the interpreter was started with, and so that a caller who wants it
/// silenced can redirect it the way Python redirects anything else.
void report(const std::string& text) {
    if (text.empty()) {
        return;
    }
    py::module_::import("sys").attr("stderr").attr("write")(text);
}

void bind_enumerations(py::module_& module) {
    // The Python spellings replace the hyphens of the configuration file with
    // underscores, since an identifier cannot hold a hyphen. `str()` gives the
    // spelling the file uses, so a value can be written into a document without
    // a translation table, and the parse functions read that spelling back.
    py::enum_<sim::SolverKind>(module, "SolverKind",
                               "Which force solver computes the accelerations.")
        .value("direct", sim::SolverKind::kDirect)
        .value("barnes_hut", sim::SolverKind::kBarnesHut)
        .value("sycl_direct", sim::SolverKind::kSyclDirect)
        .value("sycl_tree", sim::SolverKind::kSyclTree)
        .def("__str__", [](sim::SolverKind kind) { return std::string(sim::to_string(kind)); });

    py::enum_<sim::IntegratorKind>(module, "IntegratorKind", "Which integrator advances the state.")
        .value("velocity_verlet", sim::IntegratorKind::kVelocityVerlet)
        .value("yoshida4", sim::IntegratorKind::kYoshida4)
        .value("rk4", sim::IntegratorKind::kRungeKutta4)
        .def("__str__", [](sim::IntegratorKind kind) { return std::string(sim::to_string(kind)); });

    py::enum_<sim::InitialConditionKind>(module, "InitialConditionKind",
                                         "Which configuration the run starts from.")
        .value("plummer", sim::InitialConditionKind::kPlummer)
        .value("uniform_sphere", sim::InitialConditionKind::kUniformSphere)
        .value("kepler", sim::InitialConditionKind::kKepler)
        .value("disc_galaxy", sim::InitialConditionKind::kDiscGalaxy)
        .value("galaxy_collision", sim::InitialConditionKind::kGalaxyCollision)
        .def("__str__",
             [](sim::InitialConditionKind kind) { return std::string(sim::to_string(kind)); });

    py::enum_<sim::ExecutorKind>(module, "ExecutorKind",
                                 "How the CPU solvers divide their loop over target particles.")
        .value("serial", sim::ExecutorKind::kSerial)
        .value("static", sim::ExecutorKind::kStatic)
        .value("work_stealing", sim::ExecutorKind::kWorkStealing)
        .def("__str__", [](sim::ExecutorKind kind) { return std::string(sim::to_string(kind)); });

    module.def("parse_solver_kind", &sim::parse_solver_kind, "text"_a,
               "The solver a configuration file's spelling names, or None.");
    module.def("parse_integrator_kind", &sim::parse_integrator_kind, "text"_a,
               "The integrator a configuration file's spelling names, or None.");
    module.def("parse_initial_condition_kind", &sim::parse_initial_condition_kind, "text"_a,
               "The initial condition a configuration file's spelling names, or None.");
    module.def("parse_executor_kind", &sim::parse_executor_kind, "text"_a,
               "The executor a configuration file's spelling names, or None.");
}

void bind_settings(py::module_& module) {
    py::class_<sim::RunSettings>(module, "RunSettings", "How far to go, and from what seed.")
        .def(py::init([](Real timestep, Index steps, std::uint64_t seed) {
                 return sim::RunSettings{timestep, steps, seed};
             }),
             "timestep"_a = Real{0}, "steps"_a = Index{0}, "seed"_a = std::uint64_t{0})
        .def_readwrite("timestep", &sim::RunSettings::timestep)
        .def_readwrite("steps", &sim::RunSettings::steps)
        .def_readwrite("seed", &sim::RunSettings::seed)
        // Every settings record compares by value, which is what makes the
        // round trip through a configuration file testable. `py::self` is
        // pybind11's placeholder for the bound type rather than a value, so the
        // two sides of this comparison are deliberately the same expression and
        // the redundancy check has nothing to act on. The same annotation
        // appears on each of the records below.
        // NOLINTNEXTLINE(misc-redundant-expression)
        .def(py::self == py::self);

    // The initial condition settings are the union of every generator's
    // parameters rather than a variant, exactly as the configuration file is,
    // so they are bound with a default constructor and assignable fields rather
    // than with a constructor naming twenty arguments nobody would pass at
    // once. The Python idiom for the common case is to construct and then
    // assign the three or four fields the chosen generator reads.
    py::class_<sim::InitialConditionSettings>(module, "InitialConditionSettings",
                                              R"(What the run starts from.

Holds the union of every generator's parameters. A field the chosen ``kind``
does not read is ignored rather than rejected, so one object can describe
several configurations and select between them.)")
        .def(py::init<>())
        .def_readwrite("kind", &sim::InitialConditionSettings::kind)
        .def_readwrite("count", &sim::InitialConditionSettings::count)
        .def_readwrite("total_mass", &sim::InitialConditionSettings::total_mass)
        .def_readwrite("scale_radius", &sim::InitialConditionSettings::scale_radius)
        .def_readwrite("radius", &sim::InitialConditionSettings::radius)
        .def_readwrite("mass_fraction_cutoff", &sim::InitialConditionSettings::mass_fraction_cutoff)
        .def_readwrite("primary_mass", &sim::InitialConditionSettings::primary_mass)
        .def_readwrite("secondary_mass", &sim::InitialConditionSettings::secondary_mass)
        .def_readwrite("semi_major_axis", &sim::InitialConditionSettings::semi_major_axis)
        .def_readwrite("eccentricity", &sim::InitialConditionSettings::eccentricity)
        .def_readwrite("bulge_fraction", &sim::InitialConditionSettings::bulge_fraction)
        .def_readwrite("scale_length", &sim::InitialConditionSettings::scale_length)
        .def_readwrite("scale_height", &sim::InitialConditionSettings::scale_height)
        .def_readwrite("bulge_radius", &sim::InitialConditionSettings::bulge_radius)
        .def_readwrite("inclination", &sim::InitialConditionSettings::inclination)
        .def_readwrite("position_angle", &sim::InitialConditionSettings::position_angle)
        .def_readwrite("mass_ratio", &sim::InitialConditionSettings::mass_ratio)
        .def_readwrite("secondary_inclination",
                       &sim::InitialConditionSettings::secondary_inclination)
        .def_readwrite("secondary_position_angle",
                       &sim::InitialConditionSettings::secondary_position_angle)
        .def_readwrite("separation", &sim::InitialConditionSettings::separation)
        .def_readwrite("impact_parameter", &sim::InitialConditionSettings::impact_parameter)
        .def_readwrite("approach_speed", &sim::InitialConditionSettings::approach_speed)
        // NOLINTNEXTLINE(misc-redundant-expression)
        .def(py::self == py::self);

    py::class_<sim::SolverSettings>(module, "SolverSettings",
                                    "Which solver, and what it computes with.")
        .def(py::init<>())
        .def_readwrite("kind", &sim::SolverSettings::kind)
        .def_readwrite("softening", &sim::SolverSettings::softening)
        .def_readwrite("opening_angle", &sim::SolverSettings::opening_angle)
        .def_readwrite("leaf_capacity", &sim::SolverSettings::leaf_capacity)
        .def_readwrite("quadrupole", &sim::SolverSettings::quadrupole)
        .def_readwrite("executor", &sim::SolverSettings::executor)
        .def_readwrite("threads", &sim::SolverSettings::threads)
        .def_readwrite("allow_cpu_fallback", &sim::SolverSettings::allow_cpu_fallback)
        // NOLINTNEXTLINE(misc-redundant-expression)
        .def(py::self == py::self);

    py::class_<sim::IntegratorSettings>(module, "IntegratorSettings", "Which integrator.")
        .def(py::init([](sim::IntegratorKind kind) { return sim::IntegratorSettings{kind}; }),
             "kind"_a = sim::IntegratorKind::kVelocityVerlet)
        .def_readwrite("kind", &sim::IntegratorSettings::kind)
        // NOLINTNEXTLINE(misc-redundant-expression)
        .def(py::self == py::self);

    py::class_<sim::OutputSettings>(module, "OutputSettings",
                                    R"(What a run writes down, and how often.

Read and written by the configuration file, and ignored by ``assemble``: a run
driven from Python reports through the NumPy views of its own state, which is
strictly more than a file it would then have to parse. The command-line program
is what writes trajectories and checkpoints.)")
        .def(py::init<>())
        .def_readwrite("trajectory_path", &sim::OutputSettings::trajectory_path)
        .def_readwrite("trajectory_stride", &sim::OutputSettings::trajectory_stride)
        .def_readwrite("trajectory_velocities", &sim::OutputSettings::trajectory_velocities)
        .def_readwrite("diagnostics_path", &sim::OutputSettings::diagnostics_path)
        .def_readwrite("diagnostics_stride", &sim::OutputSettings::diagnostics_stride)
        .def_readwrite("checkpoint_path", &sim::OutputSettings::checkpoint_path)
        .def_readwrite("checkpoint_stride", &sim::OutputSettings::checkpoint_stride)
        // NOLINTNEXTLINE(misc-redundant-expression)
        .def(py::self == py::self);
}

void bind_configuration(py::module_& module) {
    py::class_<Configuration>(module, "Configuration", R"(Everything a run is, as data.

Inert: it builds no solver, opens no file and validates nothing. The same record
the configuration file parses into, so a run set up in Python and a run set up
from a document are the same run, and either can be written out as the other.)")
        .def(py::init<>())
        .def_readwrite("run", &Configuration::run)
        .def_readwrite("initial_conditions", &Configuration::initial_conditions)
        .def_readwrite("solver", &Configuration::solver)
        .def_readwrite("integrator", &Configuration::integrator)
        .def_readwrite("output", &Configuration::output)
        // NOLINTNEXTLINE(misc-redundant-expression)
        .def(py::self == py::self)
        .def("__repr__", [](const Configuration& configuration) {
            std::ostringstream text;
            sim::write_configuration(text, configuration);
            return text.str();
        });

    module.def("problems_with", &sim::problems_with, "configuration"_a,
               R"(Every objection to a configuration, in the order the fields appear.

Empty for a configuration that can be run. Every objection at once rather than
the first, because three mistakes in a document should be reported three at a
time.)");

    module.def(
        "parse_configuration",
        [](std::string_view text, std::string_view origin) {
            std::istringstream stream{std::string(text)};
            return sim::parse_configuration(stream, origin);
        },
        "text"_a, "origin"_a = std::string_view("<string>"),
        "Read a configuration from the text of one. Raises on a malformed document.");

    module.def(
        "read_configuration_file",
        [](const std::string& path) { return sim::read_configuration_file(path); }, "path"_a,
        "Read a configuration file.");

    module.def(
        "write_configuration",
        [](const Configuration& configuration) {
            std::ostringstream text;
            sim::write_configuration(text, configuration);
            return text.str();
        },
        "configuration"_a, "The configuration as a document, ready to be written to a file.");

    module.def(
        "apply_settings",
        [](Configuration& configuration, const std::vector<std::string>& assignments,
           std::string_view origin) { sim::apply_settings(configuration, assignments, origin); },
        "configuration"_a, "assignments"_a, "origin"_a = std::string_view("apply_settings"),
        R"(Apply `section.setting=value` assignments in place.

The same mechanism as the command-line program's ``--set``, so a parameter sweep
in a notebook overrides a document the same way a shell script would.)");
}

void bind_interaction_count(py::module_& module) {
    py::class_<InteractionCount>(module, "InteractionCount",
                                 R"(The work a solver did.

Counted rather than estimated, which is what makes a comparison between the
direct solver and the tree a statement about the algorithms rather than about the
machine they ran on.)")
        .def_readonly("evaluations", &InteractionCount::evaluations)
        .def_readonly("particle_particle", &InteractionCount::particle_particle)
        .def_readonly("particle_cell", &InteractionCount::particle_cell)
        .def("__repr__", [](const InteractionCount& count) {
            return "InteractionCount(evaluations=" + std::to_string(count.evaluations) +
                   ", particle_particle=" + std::to_string(count.particle_particle) +
                   ", particle_cell=" + std::to_string(count.particle_cell) + ")";
        });
}

void bind_simulation(py::module_& module) {
    py::class_<Simulation>(module, "Simulation", R"(A gravitational simulation in progress.

Built by ``assemble``. Holds the state, the solver, the integrator and the step
counter that is the clock: the simulated time is the step count times the
timestep, computed rather than accumulated, so a resumed run and an
uninterrupted one agree in every bit.

``step`` and ``run`` release the interpreter lock, so the solver's own threads
have the machine to themselves and another Python thread can watch the run.)")
        .def("step", &Simulation::step, py::call_guard<py::gil_scoped_release>(),
             "Advance by one step. Records nothing.")
        .def("run", &Simulation::run, "steps"_a, py::call_guard<py::gil_scoped_release>(),
             "Advance by `steps` steps.")
        .def(
            "restore",
            [](Simulation& simulation, const ParticleData& particles, Index step,
               bool accelerations_are_current) {
                simulation.restore(ParticleData(particles), step, accelerations_are_current);
            },
            "particles"_a, "step"_a = Index{0}, "accelerations_are_current"_a = false,
            R"(Replace the state and the step counter.

The state is copied into the simulation, so the caller keeps its own. The
particle count may differ from the current one.

Re-establishes the acceleration invariant from the positions given, at the cost
of one force evaluation, unless ``accelerations_are_current`` says the
accelerations already belong to these positions and are to be kept exactly.

Invalidates every view previously taken of ``particles``.)")
        .def("measure", &Simulation::measure, py::call_guard<py::gil_scoped_release>(),
             R"(The conserved quantities of the current state.

Softened with whatever the solver softens with, asked of the solver rather than
stored beside it, so the answer cannot be a measurement of a mismatch between
the two. Costs an N^2 pass and shares no code with the solver, which is what
makes it evidence rather than a restatement.)")
        .def_property_readonly("step_index", &Simulation::step_index)
        .def_property_readonly("time", &Simulation::time, "The simulated time, `step * timestep`.")
        .def_property_readonly("timestep", &Simulation::timestep)
        .def_property_readonly(
            "particles",
            [](const py::object& self) {
                return ParticleView(self.cast<Simulation&>().particles(), self);
            },
            "A read-only view of the current state, sharing its memory.")
        .def_property_readonly(
            "solver_name",
            [](const Simulation& simulation) { return std::string(simulation.solver().name()); })
        .def_property_readonly("integrator_name",
                               [](const Simulation& simulation) {
                                   return std::string(simulation.integrator().name());
                               })
        .def_property_readonly(
            "interaction_count",
            [](const Simulation& simulation) { return simulation.solver().interaction_count(); })
        .def("__repr__", [](const Simulation& simulation) {
            return "<Simulation of " + std::to_string(simulation.particles().size()) +
                   " particles at step " + std::to_string(simulation.step_index()) + ">";
        });
}

void bind_run_functions(py::module_& module) {
    module.def(
        "assemble",
        [](const Configuration& configuration) {
            std::ostringstream notices;
            std::optional<Simulation> simulation;
            {
                // Sampling a configuration and the one force evaluation that
                // establishes the acceleration invariant are the expensive part
                // of this call, and neither touches Python.
                const py::gil_scoped_release release;
                simulation.emplace(sim::assemble(configuration, nullptr, notices));
            }
            report(notices.str());
            return std::move(*simulation);
        },
        "configuration"_a,
        R"(Sample the initial conditions and build the run they belong to.

Costs one force evaluation and, for a sampled configuration, the time to draw
it. No output is attached: a run driven from Python reports through the NumPy
views of its state.

A configuration naming a GPU solver is asking for one. If no device can provide
it, and ``solver.allow_cpu_fallback`` is left true, this returns the CPU solver
that computes the same thing and writes a line to ``sys.stderr`` saying so.)");

    module.def(
        "make_initial_conditions",
        [](const Configuration& configuration) {
            const py::gil_scoped_release release;
            return sim::make_initial_conditions(configuration);
        },
        "configuration"_a, "The configuration a run would start from, sampled but not assembled.");

    module.def("primary_galaxy_count", &sim::primary_galaxy_count, "configuration"_a,
               R"(How many particles belong to the first of a collision's two galaxies.

Zero for every other configuration. It is what lets the two galaxies be told
apart in a plot of a merged remnant.)");

    module.def(
        "compute_accelerations",
        [](const Configuration& configuration, ParticleData& particles) {
            std::ostringstream notices;

            // The executor is declared first so that it is destroyed last: a CPU
            // solver holds a reference to one and must not outlive it
            // (ADR-0017). Both live for this call alone, which is what keeps
            // that lifetime rule out of the Python interface entirely.
            const std::unique_ptr<backend::Executor> executor = sim::make_executor(configuration);
            const std::unique_ptr<solvers::ForceSolver> solver =
                sim::make_solver(configuration, executor.get(), notices);

            InteractionCount count;
            {
                const py::gil_scoped_release release;
                solver->evaluate(particles.positions(), particles.masses(),
                                 particles.accelerations());
                count = solver->interaction_count();
            }

            report(notices.str());
            return count;
        },
        "configuration"_a, "particles"_a,
        R"(Evaluate the accelerations of a configuration once, in place.

Writes into the ``acceleration_x``, ``acceleration_y`` and ``acceleration_z``
arrays and returns the work it took. Only the solver section of the
configuration is read.

This is how an approximation is measured against the reference: evaluate the
same particles with ``SolverKind.barnes_hut`` and with ``SolverKind.direct`` and
compare, which is the comparison the tree solver's accuracy figures are made
of.)");
}

} // namespace

void bind_sim(py::module_& module) {
    bind_enumerations(module);
    bind_settings(module);
    bind_configuration(module);
    bind_interaction_count(module);
    bind_simulation(module);
    bind_run_functions(module);
}

} // namespace orrery::python
