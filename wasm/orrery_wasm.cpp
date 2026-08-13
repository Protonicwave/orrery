#include "orrery_wasm.h"

#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "orrery/core/diagnostics.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/sim/assembly.hpp"
#include "orrery/sim/config_file.hpp"
#include "orrery/sim/configuration.hpp"
#include "orrery/sim/simulation.hpp"
#include "orrery/solvers/direct_kernel.hpp"

namespace {

/// The revision of the interface in `orrery_wasm.h`.
///
/// Bumped when a signature changes or a measurement moves, which are the two
/// changes a caller compiled against an older header cannot survive. Adding a
/// function does not require it.
constexpr int kAbiVersion = 1;

/// The largest configuration this module will run.
///
/// Chosen from measurement rather than preference. At this count the direct
/// solver takes 45 ms a step in this build and Barnes-Hut 28 ms, which is
/// already a Worker occupied for longer than a frame; the quadratic term makes
/// the next power of two four times the first of those.
/// `docs/webassembly.md` records the measurements and the machine.
///
/// The number is here rather than in the client because the module is what
/// enforces it, and a limit stated in one place and enforced in another is a
/// limit that will one day differ.
constexpr int kParticleLimit = 4096;

/// The most steps one call will take before returning to the Worker's loop.
///
/// A call that ran for a minute would make the Worker unable to answer a pause,
/// and the module has no way to be interrupted from outside: WebAssembly has no
/// signals and the Worker's message queue is not read while a call is on the
/// stack. So the bound is on the call rather than on the caller's patience.
constexpr int kStepLimit = 1024;

/// The last failure, in a sentence.
///
/// A file-scope string because the interface has nowhere else to put one: the
/// call that failed returned a null or a false, and the reason has to survive
/// until the caller asks for it. Single threaded, so there is one of these and
/// it belongs to the one thread that can call in.
std::string& last_error() {
    static std::string message;
    return message;
}

void clear_error() {
    last_error().clear();
}

void set_error(std::string message) {
    last_error() = std::move(message);
}

} // namespace

/// A run, and everything the interface reports about it that the run itself
/// does not hold.
struct OrrerySimulation {
    orrery::sim::Simulation simulation;

    /// What the module changed about the configuration it was given.
    std::string report;

    /// The solver's name, kept as a string because the interface returns a
    /// pointer that has to outlive the call and `name()` returns a view into
    /// storage this struct does not own.
    std::string solver_name;

    /// The first total energy measured, which is what the energy error is
    /// relative to. Empty until something measures.
    std::optional<orrery::core::Real> reference_energy;
};

namespace {

/// Read the configuration, refuse what cannot be run, and say what was ignored.
///
/// Throws `ConfigurationError` for a document that is not a configuration, and
/// `std::runtime_error` for one that is but should not be run here. Both are
/// caught by the one entry point that calls this.
[[nodiscard]] orrery::sim::Configuration prepare(const char* text, std::ostringstream& report) {
    if (text == nullptr) {
        throw std::runtime_error("no configuration was given");
    }

    std::istringstream input{std::string{text}};
    orrery::sim::Configuration configuration =
        orrery::sim::parse_configuration(input, "the configuration");

    const std::vector<std::string> problems = orrery::sim::problems_with(configuration);
    if (!problems.empty()) {
        std::ostringstream message;
        message << "the configuration cannot be run: " << problems.front();
        if (problems.size() > 1) {
            message << ", and " << problems.size() - 1 << " more";
        }
        throw std::runtime_error(message.str());
    }

    if (configuration.initial_conditions.count > static_cast<orrery::core::Index>(kParticleLimit)) {
        std::ostringstream message;
        message << "this build runs at most " << kParticleLimit << " particles, and "
                << configuration.initial_conditions.count << " were asked for";
        throw std::runtime_error(message.str());
    }

    // The three outputs are paths, and there is no filesystem here worth
    // writing one to. Cleared rather than left alone, so that assembly cannot
    // open anything, and reported rather than cleared quietly.
    if (!configuration.output.trajectory_path.empty() ||
        !configuration.output.diagnostics_path.empty() ||
        !configuration.output.checkpoint_path.empty()) {
        report << "The output paths are ignored: a browser tab has nowhere to write them. ";
        configuration.output = {};
    }

    // Single threaded, because the module is built without threads: shared
    // memory in a browser needs two response headers that GitHub Pages will not
    // send (ADR-0052). A configuration naming the work-stealing scheduler is
    // asking for something this build does not have, and the honest answer is
    // the same one the GPU fallback gives: run it anyway, more slowly, and say
    // so.
    if (configuration.solver.executor != orrery::sim::ExecutorKind::kSerial ||
        configuration.solver.threads > 1) {
        report << "The " << orrery::sim::to_string(configuration.solver.executor)
               << " scheduler is not available here, so the run is serial. ";
    }
    configuration.solver.executor = orrery::sim::ExecutorKind::kSerial;
    configuration.solver.threads = 1;

    return configuration;
}

} // namespace

extern "C" {

int orrery_abi_version(void) {
    return kAbiVersion;
}

int orrery_scalar_size(void) {
    return static_cast<int>(sizeof(orrery::core::Real));
}

int orrery_particle_limit(void) {
    return kParticleLimit;
}

int orrery_step_limit(void) {
    return kStepLimit;
}

const char* orrery_last_error(void) {
    return last_error().c_str();
}

OrrerySimulation* orrery_create(const char* configuration) {
    clear_error();

    try {
        std::ostringstream report;
        const orrery::sim::Configuration settings = prepare(configuration, report);

        // Assembly writes its own notes to the same stream, so a GPU solver
        // falling back to the CPU is reported beside the scheduler this module
        // forced, in one sentence-per-decision paragraph.
        orrery::sim::Simulation simulation = orrery::sim::assemble(settings, nullptr, report);

        if (simulation.particles().size() > static_cast<orrery::core::Index>(kParticleLimit)) {
            std::ostringstream message;
            message << "this build runs at most " << kParticleLimit
                    << " particles, and the configuration produced "
                    << simulation.particles().size();
            throw std::runtime_error(message.str());
        }

        auto handle =
            std::make_unique<OrrerySimulation>(OrrerySimulation{.simulation = std::move(simulation),
                                                                .report = report.str(),
                                                                .solver_name = {},
                                                                .reference_energy = {}});
        handle->solver_name = std::string{handle->simulation.solver().name()};
        return handle.release();
    } catch (const std::exception& error) {
        set_error(error.what());
        return nullptr;
    } catch (...) {
        set_error("the run could not be started");
        return nullptr;
    }
}

void orrery_destroy(OrrerySimulation* simulation) {
    // A destructor that threw here would unwind into JavaScript, which cannot
    // catch it, so it is caught at the boundary like everything else.
    try {
        delete simulation;
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // Nothing to report to and nothing left to report about.
    }
}

const char* orrery_report(const OrrerySimulation* simulation) {
    return simulation == nullptr ? "" : simulation->report.c_str();
}

const char* orrery_solver_name(const OrrerySimulation* simulation) {
    return simulation == nullptr ? "" : simulation->solver_name.c_str();
}

const char* orrery_kernel_name(const OrrerySimulation* simulation) {
    if (simulation == nullptr) {
        return "";
    }

    // The kernel is asked of the dispatch rather than of the solver, which does
    // not expose one through the interface every solver shares. That is exact
    // here for a reason particular to this target: no vector kernel is compiled
    // for WebAssembly, so the dispatch has one answer and every solver in the
    // module reaches the same summation. On a target with two kernels this
    // would be a guess, and would have to be asked of the solver instead.
    //
    // Copied into a string of its own rather than returned as the view's data,
    // because a view is not required to be terminated and C strings are.
    static const std::string kKernel{
        orrery::solvers::to_string(orrery::solvers::fastest_available_kernel())};
    return kKernel.c_str();
}

int orrery_particle_count(const OrrerySimulation* simulation) {
    return simulation == nullptr ? 0 : static_cast<int>(simulation->simulation.particles().size());
}

int orrery_step_index(const OrrerySimulation* simulation) {
    return simulation == nullptr ? 0 : static_cast<int>(simulation->simulation.step_index());
}

double orrery_timestep(const OrrerySimulation* simulation) {
    return simulation == nullptr ? 0.0 : static_cast<double>(simulation->simulation.timestep());
}

double orrery_time(const OrrerySimulation* simulation) {
    return simulation == nullptr ? 0.0 : static_cast<double>(simulation->simulation.time());
}

int orrery_advance(OrrerySimulation* simulation, int steps) {
    clear_error();

    if (simulation == nullptr || steps <= 0) {
        return 0;
    }

    const int wanted = steps < kStepLimit ? steps : kStepLimit;

    int taken = 0;
    try {
        for (; taken < wanted; ++taken) {
            simulation->simulation.step();
        }
    } catch (const std::exception& error) {
        set_error(error.what());
    } catch (...) {
        set_error("the run stopped");
    }

    return taken;
}

int orrery_read_positions(const OrrerySimulation* simulation, float* out) {
    if (simulation == nullptr || out == nullptr) {
        return 0;
    }

    const auto positions = simulation->simulation.particles().positions();
    const std::size_t count = positions.x.size();

    for (std::size_t index = 0; index < count; ++index) {
        out[index] = static_cast<float>(positions.x[index]);
        out[count + index] = static_cast<float>(positions.y[index]);
        out[(2 * count) + index] = static_cast<float>(positions.z[index]);
    }

    return 1;
}

int orrery_read_masses(const OrrerySimulation* simulation, float* out) {
    if (simulation == nullptr || out == nullptr) {
        return 0;
    }

    const auto masses = simulation->simulation.particles().masses();
    for (std::size_t index = 0; index < masses.size(); ++index) {
        out[index] = static_cast<float>(masses[index]);
    }

    return 1;
}

int orrery_measure(OrrerySimulation* simulation, double* out) {
    clear_error();

    if (simulation == nullptr || out == nullptr) {
        return 0;
    }

    try {
        const orrery::core::Diagnostics diagnostics = simulation->simulation.measure();
        const orrery::core::Real energy = diagnostics.total_energy();

        // The first measurement of this run fixes the reference, exactly as the
        // first row of a diagnostics file does. A run measured for the first
        // time after a thousand steps is therefore relative to that instant and
        // reports no drift at it, which is the same behaviour, and the same
        // caveat, as a resumed native run.
        if (!simulation->reference_energy) {
            simulation->reference_energy = energy;
        }

        out[0] = static_cast<double>(diagnostics.kinetic_energy);
        out[1] = static_cast<double>(diagnostics.potential_energy);
        out[2] = static_cast<double>(energy);
        out[3] = static_cast<double>(
            orrery::core::relative_energy_error(*simulation->reference_energy, energy));
        out[4] = static_cast<double>(diagnostics.virial_ratio());
        out[5] = static_cast<double>(diagnostics.linear_momentum.x);
        out[6] = static_cast<double>(diagnostics.linear_momentum.y);
        out[7] = static_cast<double>(diagnostics.linear_momentum.z);
        out[8] = static_cast<double>(diagnostics.angular_momentum.x);
        out[9] = static_cast<double>(diagnostics.angular_momentum.y);
        out[10] = static_cast<double>(diagnostics.angular_momentum.z);
        return 1;
    } catch (const std::exception& error) {
        set_error(error.what());
        return 0;
    } catch (...) {
        set_error("the diagnostics could not be measured");
        return 0;
    }
}

} // extern "C"
