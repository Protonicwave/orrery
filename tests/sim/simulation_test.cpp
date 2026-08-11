#include "orrery/sim/simulation.hpp"

#include <cstddef>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "exact_state.hpp"
#include "orrery/backend/executor.hpp"
#include "orrery/core/diagnostics.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/integrators/velocity_verlet.hpp"
#include "orrery/sim/assembly.hpp"
#include "orrery/sim/checkpoint.hpp"
#include "orrery/sim/configuration.hpp"
#include "orrery/sim/run_output.hpp"
#include "orrery/solvers/direct_solver.hpp"
#include "orrery/solvers/force_solver.hpp"
#include "temporary_file.hpp"

namespace {

using orrery::core::Index;
using orrery::core::ParticleData;
using orrery::core::Real;
using orrery::core::Vec3;
using orrery::sim::Checkpoint;
using orrery::sim::Configuration;
using orrery::sim::InitialConditionKind;
using orrery::sim::IntegratorKind;
using orrery::sim::RunOutput;
using orrery::sim::Simulation;
using orrery::sim::SolverKind;
using orrery::sim::testing::identical_states;
using orrery::sim::testing::TemporaryFile;

/// A run small enough to take a few hundred steps of in a test and large enough
/// that the solvers do something interesting with it.
constexpr Index kParticles = 96;
constexpr Index kTotalSteps = 100;
constexpr Index kInterruptAt = 60;

[[nodiscard]] Configuration test_configuration() {
    Configuration configuration;
    configuration.run.timestep = static_cast<Real>(0.002);
    configuration.run.steps = kTotalSteps;
    configuration.run.seed = 20260811;
    configuration.initial_conditions.kind = InitialConditionKind::kPlummer;
    configuration.initial_conditions.count = kParticles;
    configuration.solver.kind = SolverKind::kBarnesHut;
    configuration.solver.softening = static_cast<Real>(0.05);
    configuration.integrator.kind = IntegratorKind::kVelocityVerlet;
    return configuration;
}

/// Assemble a run and discard the notices, which a test has no use for.
[[nodiscard]] Simulation make_simulation(const Configuration& configuration,
                                         std::unique_ptr<RunOutput> output = nullptr) {
    std::ostringstream report;
    return orrery::sim::assemble(configuration, std::move(output), report);
}

/// Continue a run from a checkpoint, exactly as `orrery resume` does.
///
/// The simulation is built with no particles and the state is then restored,
/// rather than built from the state, because the constructor establishes the
/// acceleration invariant by evaluating the field and that would replace the
/// accelerations the checkpoint carries.
[[nodiscard]] Simulation resume_from(const Checkpoint& checkpoint) {
    std::ostringstream report;
    std::unique_ptr<orrery::backend::Executor> executor =
        orrery::sim::make_executor(checkpoint.configuration);
    std::unique_ptr<orrery::solvers::ForceSolver> solver =
        orrery::sim::make_solver(checkpoint.configuration, executor.get(), report);

    Simulation simulation(ParticleData{}, std::move(solver),
                          orrery::sim::make_integrator(checkpoint.configuration),
                          checkpoint.configuration.run.timestep, std::move(executor));
    simulation.restore(checkpoint.particles, checkpoint.step, true);
    return simulation;
}

/// Keeps every state it is shown, so that a test can ask what a run did rather
/// than what it wrote to a disc.
class RecordingOutput final : public RunOutput {
public:
    struct Entry {
        Index step;
        Real time;
        bool is_final;
    };

    void record(const Simulation& simulation, bool is_final) override {
        entries.push_back({simulation.step_index(), simulation.time(), is_final});
    }

    std::vector<Entry> entries;
};

/// A pair of particles at rest, which is the simplest configuration with a force
/// in it and the one whose acceleration can be written down.
[[nodiscard]] ParticleData two_bodies() {
    ParticleData data;
    data.add(Vec3{-1, 0, 0}, Vec3{0, 0, 0}, 1);
    data.add(Vec3{1, 0, 0}, Vec3{0, 0, 0}, 1);
    return data;
}

} // namespace

TEST_CASE("a simulation starts with the accelerations its integrator requires",
          "[sim][simulation]") {
    // The integrators require that `accelerations()` holds the acceleration at
    // `positions()` on entry to every step (ADR-0013), and establishing that is
    // the constructor's job so that no caller has to know the rule exists.
    Simulation simulation(two_bodies(), std::make_unique<orrery::solvers::DirectSolver>(),
                          std::make_unique<orrery::integrators::VelocityVerlet>(),
                          static_cast<Real>(0.01));

    // Two unit masses two apart attract each other with an acceleration of
    // 1/4 in units where G is one, each towards the other.
    const auto accelerations = simulation.particles().accelerations();
    CHECK(accelerations.get(0).x == Catch::Approx(0.25));
    CHECK(accelerations.get(1).x == Catch::Approx(-0.25));
}

TEST_CASE("the clock is the step counter", "[sim][simulation]") {
    // Time is computed as step times timestep rather than accumulated, so that a
    // resumed run and an uninterrupted one agree about what time it is. An
    // accumulated total would differ in its last bits after a million additions.
    Simulation simulation(two_bodies(), std::make_unique<orrery::solvers::DirectSolver>(),
                          std::make_unique<orrery::integrators::VelocityVerlet>(),
                          static_cast<Real>(0.1));

    CHECK(simulation.step_index() == 0);
    CHECK(simulation.time() == static_cast<Real>(0));

    for (Index step = 1; step <= 10; ++step) {
        simulation.step();
        CHECK(simulation.step_index() == step);
        CHECK(simulation.time() == static_cast<Real>(step) * static_cast<Real>(0.1));
    }
}

TEST_CASE("a run records the state it started from and every step after", "[sim][simulation]") {
    auto output = std::make_unique<RecordingOutput>();
    RecordingOutput& recorded = *output;

    Simulation simulation(two_bodies(), std::make_unique<orrery::solvers::DirectSolver>(),
                          std::make_unique<orrery::integrators::VelocityVerlet>(),
                          static_cast<Real>(0.01), nullptr, std::move(output));
    simulation.run(5);

    // Six entries for five steps: the initial state is a result too. Without it
    // a trajectory would begin one step in and a diagnostics file would have
    // nothing to measure its energy error against.
    REQUIRE(recorded.entries.size() == 6);
    CHECK(recorded.entries.front().step == 0);
    CHECK(recorded.entries.back().step == 5);
    CHECK(recorded.entries.back().is_final);

    for (std::size_t index = 0; index + 1 < recorded.entries.size(); ++index) {
        INFO("entry " << index);
        CHECK_FALSE(recorded.entries[index].is_final);
    }
}

TEST_CASE("a run of no steps still records where it was", "[sim][simulation]") {
    auto output = std::make_unique<RecordingOutput>();
    RecordingOutput& recorded = *output;

    Simulation simulation(two_bodies(), std::make_unique<orrery::solvers::DirectSolver>(),
                          std::make_unique<orrery::integrators::VelocityVerlet>(),
                          static_cast<Real>(0.01), nullptr, std::move(output));
    simulation.run(0);

    REQUIRE(recorded.entries.size() == 1);
    CHECK(recorded.entries.front().is_final);
}

TEST_CASE("restoring a state re-establishes the invariant unless told otherwise",
          "[sim][simulation]") {
    Simulation simulation(two_bodies(), std::make_unique<orrery::solvers::DirectSolver>(),
                          std::make_unique<orrery::integrators::VelocityVerlet>(),
                          static_cast<Real>(0.01));

    ParticleData state = two_bodies();
    state.accelerations().set(0, Vec3{99, 99, 99});

    SECTION("by default the accelerations are recomputed, since they may be stale") {
        simulation.restore(state, 7);
        CHECK(simulation.step_index() == 7);
        CHECK(simulation.particles().accelerations().get(0).x == Catch::Approx(0.25));
    }

    SECTION("a state from a checkpoint keeps the accelerations it carries") {
        simulation.restore(state, 7, true);
        CHECK(simulation.particles().accelerations().get(0).x == static_cast<Real>(99));
    }
}

TEST_CASE("two runs of the same configuration are the same run", "[sim][simulation]") {
    // Everything below depends on this. A resumed run cannot be compared against
    // an uninterrupted one unless the uninterrupted one is reproducible in the
    // first place, and that in turn rests on the seeded sampler of Phase 3 and
    // the thread-count independence of the solvers.
    const Configuration configuration = test_configuration();

    Simulation first = make_simulation(configuration);
    Simulation second = make_simulation(configuration);
    first.run(kTotalSteps);
    second.run(kTotalSteps);

    CHECK(identical_states(first.particles(), second.particles()));
}

TEST_CASE("an interrupted run resumes to bitwise-identical state",
          "[sim][simulation][validation]") {
    // The requirement in section 7 of the implementation plan, demonstrated
    // through the real machinery: a checkpoint written to a file, read back, and
    // continued. Not nearly identical. Every bit of every position, velocity,
    // acceleration and mass.
    const TemporaryFile file("resume-state.ock");
    const Configuration configuration = test_configuration();

    Simulation uninterrupted = make_simulation(configuration);
    uninterrupted.run(kTotalSteps);

    Simulation interrupted = make_simulation(configuration);
    interrupted.run(kInterruptAt);

    Checkpoint checkpoint;
    checkpoint.configuration = configuration;
    checkpoint.step = interrupted.step_index();
    checkpoint.particles = interrupted.particles();
    orrery::sim::write_checkpoint(file.path(), checkpoint);

    // Read from the file rather than reused from memory, so that the format is
    // part of what is being tested. A resume that only worked when the state had
    // never left the process would not be a resume.
    const Checkpoint read = orrery::sim::read_checkpoint(file.path());
    REQUIRE(read.step == kInterruptAt);
    CHECK(identical_states(read.particles, interrupted.particles()));

    Simulation resumed = resume_from(read);
    resumed.run(kTotalSteps - kInterruptAt);

    CHECK(resumed.step_index() == uninterrupted.step_index());
    CHECK(resumed.time() == uninterrupted.time());
    CHECK(identical_states(resumed.particles(), uninterrupted.particles()));
}

TEST_CASE("resuming is bitwise-identical for every integrator", "[sim][simulation][validation]") {
    // Velocity Verlet carries one acceleration between steps, Yoshida's
    // composition applies three sub-steps within one, and RK4 keeps stage
    // buffers it rebuilds each time. Only the first of those obviously
    // round-trips through a checkpoint holding one acceleration, so the other
    // two are asserted rather than assumed.
    const auto kind = GENERATE(IntegratorKind::kVelocityVerlet, IntegratorKind::kYoshida4,
                               IntegratorKind::kRungeKutta4);

    Configuration configuration = test_configuration();
    configuration.integrator.kind = kind;
    INFO("integrator " << orrery::sim::to_string(kind));

    Simulation uninterrupted = make_simulation(configuration);
    uninterrupted.run(kTotalSteps);

    Simulation interrupted = make_simulation(configuration);
    interrupted.run(kInterruptAt);

    Checkpoint checkpoint;
    checkpoint.configuration = configuration;
    checkpoint.step = interrupted.step_index();
    checkpoint.particles = interrupted.particles();

    Simulation resumed = resume_from(checkpoint);
    resumed.run(kTotalSteps - kInterruptAt);

    CHECK(identical_states(resumed.particles(), uninterrupted.particles()));
}

TEST_CASE("resuming is bitwise-identical for every CPU solver", "[sim][simulation][validation]") {
    const auto kind = GENERATE(SolverKind::kDirect, SolverKind::kBarnesHut);

    Configuration configuration = test_configuration();
    configuration.solver.kind = kind;
    if (kind == SolverKind::kDirect) {
        // The direct solver computes every pair, so the tree settings would be
        // reported as having no effect.
        configuration.solver.opening_angle = static_cast<Real>(0.5);
    }
    INFO("solver " << orrery::sim::to_string(kind));

    Simulation uninterrupted = make_simulation(configuration);
    uninterrupted.run(kTotalSteps);

    Simulation interrupted = make_simulation(configuration);
    interrupted.run(kInterruptAt);

    Checkpoint checkpoint;
    checkpoint.configuration = configuration;
    checkpoint.step = interrupted.step_index();
    checkpoint.particles = interrupted.particles();

    Simulation resumed = resume_from(checkpoint);
    resumed.run(kTotalSteps - kInterruptAt);

    CHECK(identical_states(resumed.particles(), uninterrupted.particles()));
}

TEST_CASE("the diagnostics use the softening the solver used", "[sim][simulation]") {
    // ADR-0008. A potential energy computed with a different softening from the
    // force kernel turns the conservation result into a measurement of the
    // mismatch, so the simulation asks the solver rather than keeping a second
    // copy of the number.
    Configuration configuration = test_configuration();
    configuration.solver.softening = static_cast<Real>(0.2);

    const Simulation simulation = make_simulation(configuration);
    CHECK(simulation.solver().softening().squared() ==
          static_cast<Real>(0.2) * static_cast<Real>(0.2));

    const orrery::core::Diagnostics diagnostics = simulation.measure();
    const orrery::core::Real expected = orrery::core::potential_energy(
        simulation.particles().positions(), simulation.particles().masses(),
        simulation.solver().softening());
    CHECK(diagnostics.potential_energy == expected);
}

TEST_CASE("a sampled cluster starts in the state its model claims",
          "[sim][simulation][validation]") {
    // The Plummer sphere is an equilibrium solution, so a sample drawn from it
    // sits at a virial ratio of one. Measuring that through the driver rather
    // than through the sampler is what says the driver assembled the
    // configuration the file asked for.
    Configuration configuration = test_configuration();
    configuration.initial_conditions.count = 2000;
    configuration.solver.softening = 0;

    const Simulation simulation = make_simulation(configuration);
    const orrery::core::Diagnostics diagnostics = simulation.measure();

    // A wide tolerance, because this is a sample of two thousand particles and
    // its scatter goes as one over the square root of that. The assertion is
    // that the cluster is in equilibrium, not that the sampler is exact.
    CHECK(diagnostics.virial_ratio() == Catch::Approx(1.0).margin(0.15));
}
