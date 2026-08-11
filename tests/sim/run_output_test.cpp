#include "orrery/sim/run_output.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "orrery/backend/executor.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/sim/assembly.hpp"
#include "orrery/sim/checkpoint.hpp"
#include "orrery/sim/configuration.hpp"
#include "orrery/sim/simulation.hpp"
#include "orrery/sim/trajectory.hpp"
#include "orrery/solvers/force_solver.hpp"
#include "temporary_file.hpp"

namespace {

using orrery::core::Index;
using orrery::core::Real;
using orrery::sim::Configuration;
using orrery::sim::FileOutput;
using orrery::sim::InitialConditionKind;
using orrery::sim::Simulation;
using orrery::sim::SolverKind;
using orrery::sim::testing::TemporaryFile;

constexpr Index kSteps = 20;

[[nodiscard]] Configuration base_configuration() {
    Configuration configuration;
    configuration.run.timestep = static_cast<Real>(0.001);
    configuration.run.steps = kSteps;
    configuration.run.seed = 4;
    configuration.initial_conditions.kind = InitialConditionKind::kPlummer;
    configuration.initial_conditions.count = 16;
    configuration.solver.kind = SolverKind::kDirect;
    configuration.solver.softening = static_cast<Real>(0.05);
    return configuration;
}

/// Assemble the run the way the application does and take every step of it.
///
/// The pieces are built by hand rather than through `assemble` because the
/// trajectory header carries the masses, so the initial conditions have to exist
/// before the output can be opened.
void run_with_files(const Configuration& configuration) {
    std::ostringstream report;
    orrery::core::ParticleData particles = orrery::sim::make_initial_conditions(configuration);
    auto output = std::make_unique<FileOutput>(configuration, particles.masses());

    std::unique_ptr<orrery::backend::Executor> executor = orrery::sim::make_executor(configuration);
    std::unique_ptr<orrery::solvers::ForceSolver> solver =
        orrery::sim::make_solver(configuration, executor.get(), report);

    Simulation simulation(std::move(particles), std::move(solver),
                          orrery::sim::make_integrator(configuration), configuration.run.timestep,
                          std::move(executor), std::move(output));
    simulation.run(configuration.run.steps);
}

/// How many frames a trajectory file holds.
[[nodiscard]] Index count_frames(const std::filesystem::path& path) {
    orrery::sim::TrajectoryReader reader(path);
    orrery::sim::TrajectoryFrame frame;
    Index frames = 0;
    while (reader.read_frame(frame)) {
        ++frames;
    }
    return frames;
}

/// How many rows a CSV file holds, not counting the header.
[[nodiscard]] Index count_rows(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::string line;
    Index rows = 0;
    while (std::getline(file, line)) {
        ++rows;
    }
    return rows == 0 ? 0 : rows - 1;
}

} // namespace

TEST_CASE("an output with no path opens no file", "[sim][run_output]") {
    // A run that names no files should do no I/O and pay for none of this.
    const Configuration configuration = base_configuration();
    const orrery::core::ParticleData particles =
        orrery::sim::make_initial_conditions(configuration);

    FileOutput output(configuration, particles.masses());
    std::ostringstream report;
    Simulation simulation = orrery::sim::assemble(configuration, nullptr, report);
    output.record(simulation, true);

    CHECK(output.frames_written() == 0);
    CHECK(output.checkpoints_written() == 0);
}

TEST_CASE("a stride writes that many steps apart, and both ends besides", "[sim][run_output]") {
    const TemporaryFile trajectory("output-stride.otj");

    Configuration configuration = base_configuration();
    configuration.output.trajectory_path = trajectory.string();
    configuration.output.trajectory_stride = 5;
    run_with_files(configuration);

    // Steps 0, 5, 10, 15 and 20. The last is both a multiple of the stride and
    // the end of the run, and is written once rather than twice.
    CHECK(count_frames(trajectory.path()) == 5);
}

TEST_CASE("the final step is recorded whatever the stride", "[sim][run_output]") {
    // A run of twenty steps with a stride of seven writes at 0, 7, 14 and then
    // at 20, which is not a multiple of anything. Without the special case the
    // file would end at step 14 and the state the run finished in would be
    // absent from its own output.
    const TemporaryFile trajectory("output-final.otj");

    Configuration configuration = base_configuration();
    configuration.output.trajectory_path = trajectory.string();
    configuration.output.trajectory_stride = 7;
    run_with_files(configuration);

    CHECK(count_frames(trajectory.path()) == 4);

    orrery::sim::TrajectoryReader reader(trajectory.path());
    orrery::sim::TrajectoryFrame frame;
    Index last = 0;
    while (reader.read_frame(frame)) {
        last = frame.step;
    }
    CHECK(last == kSteps);
}

TEST_CASE("a stride of zero records the two ends of the run", "[sim][run_output]") {
    // What a run wants when it cares about the final state and not the path
    // taken to it.
    const TemporaryFile trajectory("output-ends.otj");

    Configuration configuration = base_configuration();
    configuration.output.trajectory_path = trajectory.string();
    configuration.output.trajectory_stride = 0;
    run_with_files(configuration);

    CHECK(count_frames(trajectory.path()) == 2);
}

TEST_CASE("the three outputs have independent strides", "[sim][run_output]") {
    const TemporaryFile trajectory("output-three.otj");
    const TemporaryFile diagnostics("output-three.csv");
    const TemporaryFile checkpoint("output-three.ock");

    Configuration configuration = base_configuration();
    configuration.output.trajectory_path = trajectory.string();
    configuration.output.trajectory_stride = 4;
    configuration.output.diagnostics_path = diagnostics.string();
    configuration.output.diagnostics_stride = 10;
    configuration.output.checkpoint_path = checkpoint.string();
    configuration.output.checkpoint_stride = 0;
    run_with_files(configuration);

    // Steps 0, 4, 8, 12, 16, 20.
    CHECK(count_frames(trajectory.path()) == 6);

    // Steps 0, 10, 20.
    CHECK(count_rows(diagnostics.path()) == 3);

    // The last checkpoint written is the state the run ended in, which is what
    // makes a run resumable from its own output.
    CHECK(orrery::sim::read_checkpoint(checkpoint.path()).step == kSteps);
}

TEST_CASE("the diagnostics file has a header and the columns it promises", "[sim][run_output]") {
    // The header row is what makes this readable by a plotting script that was
    // not written alongside it.
    const TemporaryFile diagnostics("output-header.csv");

    Configuration configuration = base_configuration();
    configuration.output.diagnostics_path = diagnostics.string();
    configuration.output.diagnostics_stride = 5;
    run_with_files(configuration);

    std::ifstream file(diagnostics.path());
    std::string header;
    REQUIRE(std::getline(file, header));

    CHECK(header.starts_with("step,time,"));
    CHECK(header.find("total_energy") != std::string::npos);
    CHECK(header.find("relative_energy_error") != std::string::npos);
    CHECK(header.find("virial_ratio") != std::string::npos);
    CHECK(header.find("angular_momentum_z") != std::string::npos);

    // The first row is the state the run started from, so its energy error is
    // zero by construction: it is the reference the rest are relative to.
    std::string first;
    REQUIRE(std::getline(file, first));
    CHECK(first.starts_with("0,0,"));
}

TEST_CASE("a trajectory carries velocities only when asked", "[sim][run_output]") {
    const TemporaryFile trajectory("output-velocities.otj");

    Configuration configuration = base_configuration();
    configuration.output.trajectory_path = trajectory.string();
    configuration.output.trajectory_velocities = true;
    run_with_files(configuration);

    const orrery::sim::TrajectoryReader reader(trajectory.path());
    CHECK(reader.info().has_velocities);
    CHECK(reader.info().timestep == configuration.run.timestep);
    CHECK(reader.info().particle_count == configuration.initial_conditions.count);
}

TEST_CASE("an unwritable output path is refused before the first step", "[sim][run_output]") {
    // Discovered at construction rather than at the first write, so that a run
    // with a mistyped output directory stops at once instead of computing for an
    // hour and then failing.
    Configuration configuration = base_configuration();
    configuration.output.trajectory_path = "no-such-directory/nowhere/trajectory.otj";

    const orrery::core::ParticleData particles =
        orrery::sim::make_initial_conditions(configuration);
    CHECK_THROWS(FileOutput(configuration, particles.masses()));
}
