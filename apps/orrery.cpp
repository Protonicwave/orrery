/// \file
/// The command-line simulator.
///
/// Everything the earlier phases built is reachable from here: any of the four
/// solvers, any of the three integrators, any of the three initial conditions,
/// on either scheduler, writing any combination of the three output formats. It
/// is deliberately thin. Choosing a solver from a string is `sim/assembly.hpp`,
/// reading a configuration is `sim/config_file.hpp`, and running is
/// `sim/simulation.hpp`, so what is left in this file is argument handling and
/// the report at the end.
///
/// The four commands are `run`, `resume`, `show` and `inspect`. `resume` takes a
/// checkpoint and nothing else, because a checkpoint carries the configuration
/// it was taken under, which is what makes an interrupted run resumable from the
/// file alone rather than from the file plus a memory of how it was started.

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iostream>
#include <limits>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "orrery/backend/executor.hpp"
#include "orrery/core/build_info.hpp"
#include "orrery/core/diagnostics.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/integrators/integrator.hpp"
#include "orrery/sim/assembly.hpp"
#include "orrery/sim/checkpoint.hpp"
#include "orrery/sim/config_file.hpp"
#include "orrery/sim/configuration.hpp"
#include "orrery/sim/run_output.hpp"
#include "orrery/sim/simulation.hpp"
#include "orrery/sim/trajectory.hpp"
#include "orrery/solvers/force_solver.hpp"
#include "orrery/solvers/interaction_count.hpp"

namespace {

using orrery::core::Index;
using orrery::core::Real;
using orrery::sim::Checkpoint;
using orrery::sim::Configuration;
using orrery::sim::ConfigurationError;
using orrery::sim::FileOutput;
using orrery::sim::RunOutput;
using orrery::sim::Simulation;

constexpr int kSuccess = 0;
constexpr int kFailure = 1;

/// How many progress lines a long run prints.
///
/// Twenty is one line per five per cent, which is enough to see that a run is
/// moving and few enough that redirecting the output to a file does not produce
/// a document nobody will read. Runs shorter than this print one line per step
/// and are over before it matters.
constexpr Index kProgressLines = 20;

void print_usage(std::ostream& out) {
    out << "orrery " << orrery::core::version() << ", an N-body gravitational simulator\n\n"
        << "Usage:\n"
        << "  orrery run <configuration> [--set key=value]...\n"
        << "      Run the simulation the configuration file describes.\n\n"
        << "  orrery resume <checkpoint> [--set key=value]...\n"
        << "      Continue a run from a checkpoint, under the settings it was\n"
        << "      taken with. Use --set run.steps=N to run further than the\n"
        << "      original configuration asked for.\n\n"
        << "  orrery show <configuration> [--set key=value]...\n"
        << "      Print the settings a run would use, every one of them, and\n"
        << "      report any that make no sense. Takes no steps.\n\n"
        << "  orrery inspect <file>\n"
        << "      Describe a trajectory or a checkpoint.\n\n"
        << "  orrery --help | --version\n\n"
        << "A setting is named section.key, as in --set solver.softening=0.05.\n"
        << "The configuration format is documented in docs/formats/configuration.md.\n";
}

/// Prints progress while writing whatever the configuration asked for.
///
/// A decorator around `FileOutput` rather than a change to it, because printing
/// to a terminal is a property of this program and not of the file formats. The
/// test suite drives runs through outputs of its own and prints nothing.
class ConsoleOutput final : public RunOutput {
public:
    ConsoleOutput(const Configuration& configuration, std::span<const Real> masses,
                  Index total_steps, std::ostream& out)
        : files_(configuration, masses),
          out_(&out),
          interval_(total_steps > kProgressLines ? total_steps / kProgressLines : 1) {}

    void record(const Simulation& simulation, bool is_final) override {
        files_.record(simulation, is_final);

        const Index step = simulation.step_index();
        if (step % interval_ == 0 || is_final) {
            *out_ << "  step " << step << ", time " << simulation.time() << '\n';
            out_->flush();
        }
    }

    [[nodiscard]] const FileOutput& files() const noexcept { return files_; }

private:
    FileOutput files_;
    std::ostream* out_;
    Index interval_;
};

/// The `--set` assignments, in order, with the rest of the arguments refused.
///
/// A positional argument after the file, or an unknown option, is an error
/// rather than something to ignore. The whole point of the strictness in the
/// configuration parser is that a run does what its settings say, and a command
/// line that quietly dropped an argument would defeat it from the other end.
[[nodiscard]] std::vector<std::string> collect_settings(std::span<const std::string_view> arguments,
                                                        std::ostream& error, bool& ok) {
    std::vector<std::string> settings;
    ok = true;

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] != "--set") {
            error << "orrery: unexpected argument '" << arguments[index] << "'\n";
            ok = false;
            return settings;
        }
        if (index + 1 == arguments.size()) {
            error << "orrery: --set needs an assignment, as --set run.steps=1000\n";
            ok = false;
            return settings;
        }
        settings.emplace_back(arguments[++index]);
    }
    return settings;
}

/// Report the settings that cannot be run, and whether there were any.
[[nodiscard]] bool report_problems(const Configuration& configuration, std::ostream& error) {
    const std::vector<std::string> problems = orrery::sim::problems_with(configuration);
    for (const std::string& problem : problems) {
        error << "orrery: " << problem << '\n';
    }
    return !problems.empty();
}

void describe_run(const Simulation& simulation, const Configuration& configuration,
                  std::ostream& out) {
    out << "solver:      " << simulation.solver().name() << ", softening "
        << simulation.solver().softening().length() << '\n'
        << "integrator:  " << simulation.integrator().name() << ", order "
        << simulation.integrator().order() << ", "
        << (simulation.integrator().is_symplectic() ? "symplectic" : "not symplectic") << '\n'
        << "particles:   " << simulation.particles().size() << '\n'
        << "timestep:    " << configuration.run.timestep << '\n';
}

/// The energy error and what it was measured against, which is the one number a
/// run has to report.
void report_conservation(const orrery::core::Diagnostics& before,
                         const orrery::core::Diagnostics& after, std::ostream& out) {
    const Real initial = before.total_energy();
    const Real change = after.total_energy() - initial;
    const Real magnitude = initial < 0 ? -initial : initial;

    out << "energy:      " << initial << " to " << after.total_energy() << '\n';
    if (magnitude > 0) {
        out << "relative energy error: " << change / magnitude << '\n';
    }
    out << "virial ratio: " << before.virial_ratio() << " to " << after.virial_ratio() << '\n';
}

int run_simulation(Simulation& simulation, const Configuration& configuration, Index steps,
                   std::ostream& out) {
    describe_run(simulation, configuration, out);

    const orrery::core::Diagnostics before = simulation.measure();
    out << "starting at step " << simulation.step_index() << ", taking " << steps << " steps\n";

    const auto started = std::chrono::steady_clock::now();
    simulation.run(steps);
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started);

    const orrery::core::Diagnostics after = simulation.measure();
    report_conservation(before, after, out);

    const orrery::solvers::InteractionCount interactions = simulation.solver().interaction_count();
    out << "steps taken: " << steps << " in " << elapsed.count() << " s";
    if (steps > 0) {
        out << ", " << elapsed.count() / static_cast<double>(steps) * 1e3 << " ms per step";
    }
    // The two counters are reported apart rather than added. A particle-cell
    // interaction is preceded by a walk and is not vectorised, so it costs
    // several times what a particle-particle one does, and one total would hide
    // the ratio that says what the tree bought.
    out << '\n'
        << "evaluations: " << interactions.evaluations << '\n'
        << "interactions: " << interactions.particle_particle << " particle-particle, "
        << interactions.particle_cell << " particle-cell\n";
    return kSuccess;
}

int command_run(std::span<const std::string_view> arguments, std::ostream& out,
                std::ostream& error) {
    if (arguments.empty()) {
        error << "orrery run: expected a configuration file\n";
        return kFailure;
    }

    bool ok = false;
    const std::vector<std::string> settings = collect_settings(arguments.subspan(1), error, ok);
    if (!ok) {
        return kFailure;
    }

    Configuration configuration = orrery::sim::read_configuration_file(arguments[0]);
    orrery::sim::apply_settings(configuration, settings, "--set");
    if (report_problems(configuration, error)) {
        return kFailure;
    }

    // The initial conditions are sampled before the output is opened, because
    // the trajectory header carries the masses. That order also means an
    // unwritable output path is discovered after the sampling rather than
    // before it, which is the one thing this arrangement costs.
    orrery::core::ParticleData particles = orrery::sim::make_initial_conditions(configuration);
    auto output = std::make_unique<ConsoleOutput>(configuration, particles.masses(),
                                                  configuration.run.steps, out);

    std::unique_ptr<orrery::backend::Executor> executor = orrery::sim::make_executor(configuration);
    std::unique_ptr<orrery::solvers::ForceSolver> solver =
        orrery::sim::make_solver(configuration, executor.get(), out);

    Simulation simulation(std::move(particles), std::move(solver),
                          orrery::sim::make_integrator(configuration), configuration.run.timestep,
                          std::move(executor), std::move(output));

    return run_simulation(simulation, configuration, configuration.run.steps, out);
}

int command_resume(std::span<const std::string_view> arguments, std::ostream& out,
                   std::ostream& error) {
    if (arguments.empty()) {
        error << "orrery resume: expected a checkpoint file\n";
        return kFailure;
    }

    bool ok = false;
    const std::vector<std::string> settings = collect_settings(arguments.subspan(1), error, ok);
    if (!ok) {
        return kFailure;
    }

    Checkpoint checkpoint = orrery::sim::read_checkpoint(arguments[0]);
    Configuration configuration = std::move(checkpoint.configuration);
    orrery::sim::apply_settings(configuration, settings, "--set");
    if (report_problems(configuration, error)) {
        return kFailure;
    }

    if (checkpoint.step >= configuration.run.steps) {
        out << "the checkpoint is at step " << checkpoint.step << " and the run asks for "
            << configuration.run.steps
            << ", so there is nothing left to do. Use --set run.steps=N to go further.\n";
        return kSuccess;
    }
    const Index remaining = configuration.run.steps - checkpoint.step;

    auto output = std::make_unique<ConsoleOutput>(configuration, checkpoint.particles.masses(),
                                                  remaining, out);

    std::unique_ptr<orrery::backend::Executor> executor = orrery::sim::make_executor(configuration);
    std::unique_ptr<orrery::solvers::ForceSolver> solver =
        orrery::sim::make_solver(configuration, executor.get(), out);

    // Constructed empty and then restored, rather than constructed with the
    // state. The constructor establishes the acceleration invariant by
    // evaluating the field, which would overwrite the accelerations the
    // checkpoint carries with recomputed ones. They agree for every solver in
    // this project and are not required to for one added later, so the resumed
    // state is the stored state exactly (ADR-0032). Establishing the invariant
    // over no particles costs nothing.
    Simulation simulation(orrery::core::ParticleData{}, std::move(solver),
                          orrery::sim::make_integrator(configuration), configuration.run.timestep,
                          std::move(executor), std::move(output));
    simulation.restore(std::move(checkpoint.particles), checkpoint.step, true);

    out << "resumed from " << arguments[0] << " at step " << checkpoint.step << '\n';
    return run_simulation(simulation, configuration, remaining, out);
}

int command_show(std::span<const std::string_view> arguments, std::ostream& out,
                 std::ostream& error) {
    if (arguments.empty()) {
        error << "orrery show: expected a configuration file\n";
        return kFailure;
    }

    bool ok = false;
    const std::vector<std::string> settings = collect_settings(arguments.subspan(1), error, ok);
    if (!ok) {
        return kFailure;
    }

    Configuration configuration = orrery::sim::read_configuration_file(arguments[0]);
    orrery::sim::apply_settings(configuration, settings, "--set");
    orrery::sim::write_configuration(out, configuration);

    return report_problems(configuration, error) ? kFailure : kSuccess;
}

int inspect_checkpoint(const std::filesystem::path& path, std::ostream& out) {
    const Checkpoint checkpoint = orrery::sim::read_checkpoint(path);
    out << "checkpoint: " << path.string() << '\n'
        << "step:       " << checkpoint.step << " of " << checkpoint.configuration.run.steps << '\n'
        << "time:       "
        << static_cast<Real>(checkpoint.step) * checkpoint.configuration.run.timestep << '\n'
        << "particles:  " << checkpoint.particles.size() << "\n\n"
        << "The configuration it was taken under:\n\n";
    orrery::sim::write_configuration(out, checkpoint.configuration);
    return kSuccess;
}

int inspect_trajectory(const std::filesystem::path& path, std::ostream& out) {
    orrery::sim::TrajectoryReader reader(path);
    const orrery::sim::TrajectoryInfo& info = reader.info();

    orrery::sim::TrajectoryFrame frame;
    Index frames = 0;
    Real last_time = 0;
    while (reader.read_frame(frame)) {
        ++frames;
        last_time = frame.time;
    }

    out << "trajectory: " << path.string() << '\n'
        << "particles:  " << info.particle_count << '\n'
        << "timestep:   " << info.timestep << '\n'
        << "velocities: " << (info.has_velocities ? "yes" : "no") << '\n'
        << "frames:     " << frames << '\n'
        << "last time:  " << last_time << '\n';
    return kSuccess;
}

int command_inspect(std::span<const std::string_view> arguments, std::ostream& out,
                    std::ostream& error) {
    if (arguments.size() != 1) {
        error << "orrery inspect: expected one file\n";
        return kFailure;
    }

    const std::filesystem::path path{arguments[0]};

    // Which format it is comes from the file rather than from its name, since
    // nothing stops anyone naming a checkpoint .traj. The magic is the first
    // eight bytes of both.
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error << "orrery: " << path.string() << ": could not be opened\n";
        return kFailure;
    }
    std::string magic(orrery::sim::kCheckpointMagic.size(), '\0');
    file.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    file.close();

    if (magic == orrery::sim::kCheckpointMagic) {
        return inspect_checkpoint(path, out);
    }
    if (magic == orrery::sim::kTrajectoryMagic) {
        return inspect_trajectory(path, out);
    }

    error << "orrery: " << path.string()
          << " is neither an Orrery checkpoint nor an Orrery trajectory\n";
    return kFailure;
}

int dispatch(std::span<const std::string_view> arguments, std::ostream& out, std::ostream& error) {
    if (arguments.empty() || arguments[0] == "--help" || arguments[0] == "-h") {
        print_usage(out);
        return kSuccess;
    }
    if (arguments[0] == "--version") {
        out << "orrery " << orrery::core::version() << ", "
            << (orrery::core::uses_single_precision() ? "single" : "double") << " precision\n";
        return kSuccess;
    }

    const std::span<const std::string_view> rest = arguments.subspan(1);
    if (arguments[0] == "run") {
        return command_run(rest, out, error);
    }
    if (arguments[0] == "resume") {
        return command_resume(rest, out, error);
    }
    if (arguments[0] == "show") {
        return command_show(rest, out, error);
    }
    if (arguments[0] == "inspect") {
        return command_inspect(rest, out, error);
    }

    error << "orrery: unknown command '" << arguments[0] << "'\n\n";
    print_usage(error);
    return kFailure;
}

} // namespace

namespace {

/// Report a failure and nothing else.
///
/// `noexcept` because it is called from the handlers in `main`, and an exception
/// thrown while reporting one would leave `main` by a path no handler covers.
///
/// Written with C output rather than `std::cerr` for the same reason. A stream
/// inserter may throw, and the alternatives are a handler here that swallows
/// whatever it throws, which is an empty catch block, or a function that
/// promises not to throw and might. Neither is better than writing the three
/// pieces of a line with a function that cannot fail in that way. The two
/// streams are synchronised by default, so this appears in the right place
/// among everything else the program has printed.
void report_failure(const char* message) noexcept {
    // The results are discarded deliberately and the casts say so: this is
    // already the failure path, the exit status carries the outcome whether or
    // not the message reached the terminal, and there is no second place to
    // report a failure to report a failure. Three fixed-argument calls rather
    // than one formatted one, because a vararg call is the other thing this
    // project does not write, and rather than one assembled string, because
    // assembling it allocates and this function may not throw.
    static_cast<void>(std::fputs("orrery: ", stderr));
    static_cast<void>(std::fputs(message, stderr));
    static_cast<void>(std::fputs("\n", stderr));
}

} // namespace

int main(int argc, char** argv) {
    try {
        // Numbers to the precision of the build, so that a diagnostic printed
        // here and the same number in the CSV file agree.
        std::cout << std::setprecision(std::numeric_limits<Real>::max_digits10);

        const std::span<char*> raw(argv, static_cast<std::size_t>(argc));
        std::vector<std::string_view> arguments;
        arguments.reserve(raw.size());
        for (std::size_t index = 1; index < raw.size(); ++index) {
            arguments.emplace_back(raw[index]);
        }

        return dispatch(arguments, std::cout, std::cerr);
    } catch (const ConfigurationError& error) {
        report_failure(error.what());
        return kFailure;
    } catch (const std::exception& error) {
        // Everything below here that fails at a setup boundary throws, and this
        // is where that stops. A message and a non-zero status is what a person
        // running this from a shell can act on; a terminate call and a stack
        // trace is not.
        report_failure(error.what());
        return kFailure;
    } catch (...) {
        // Nothing in this project throws anything that is not derived from
        // std::exception, so this catches only what a standard library
        // implementation might. It is here so that no exception can escape
        // `main`, where the result would be a call to terminate and a report
        // from the operating system rather than from this program.
        report_failure("an unrecognised error stopped the run");
        return kFailure;
    }
}
