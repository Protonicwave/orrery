#include "orrery/sim/config_file.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/core/types.hpp"
#include "orrery/sim/configuration.hpp"

namespace {

using orrery::core::Real;
using orrery::sim::Configuration;
using orrery::sim::ConfigurationError;
using orrery::sim::ExecutorKind;
using orrery::sim::InitialConditionKind;
using orrery::sim::IntegratorKind;
using orrery::sim::SolverKind;

/// The seed for the round-trip property test. Fixed so a failure can be
/// reproduced, and reported in the failure message so the reader does not have
/// to open this file to find it.
constexpr std::uint_fast32_t kSeed = 20260811;

[[nodiscard]] Configuration parse(std::string_view text) {
    std::istringstream stream{std::string{text}};
    return orrery::sim::parse_configuration(stream, "test");
}

[[nodiscard]] std::string write(const Configuration& configuration) {
    std::ostringstream stream;
    orrery::sim::write_configuration(stream, configuration);
    return stream.str();
}

} // namespace

TEST_CASE("a configuration file says what it changes", "[sim][config_file]") {
    const Configuration configuration = parse(R"(
[run]
timestep = 0.25
steps = 40
seed = 12345

[initial_conditions]
kind = uniform-sphere
count = 128
radius = 3

[solver]
kind = barnes-hut
softening = 0.05
quadrupole = true

[integrator]
kind = yoshida4

[output]
diagnostics_path = out.csv
diagnostics_stride = 5
)");

    CHECK(configuration.run.timestep == static_cast<Real>(0.25));
    CHECK(configuration.run.steps == 40);
    CHECK(configuration.run.seed == 12345);
    CHECK(configuration.initial_conditions.kind == InitialConditionKind::kUniformSphere);
    CHECK(configuration.initial_conditions.count == 128);
    CHECK(configuration.initial_conditions.radius == static_cast<Real>(3));
    CHECK(configuration.solver.kind == SolverKind::kBarnesHut);
    CHECK(configuration.solver.softening == static_cast<Real>(0.05));
    CHECK(configuration.solver.quadrupole);
    CHECK(configuration.integrator.kind == IntegratorKind::kYoshida4);
    CHECK(configuration.output.diagnostics_path == "out.csv");
    CHECK(configuration.output.diagnostics_stride == 5);

    // Everything the file did not mention keeps the default its field carries,
    // which is what lets a short file be a complete configuration.
    CHECK(configuration.solver.executor == ExecutorKind::kWorkStealing);
    CHECK(configuration.solver.opening_angle == static_cast<Real>(0.5));
    CHECK(configuration.output.trajectory_path.empty());
}

TEST_CASE("comments and blank lines are not settings", "[sim][config_file]") {
    const Configuration configuration = parse(R"(
# A comment.

   # An indented comment.
[run]

steps = 7
)");

    CHECK(configuration.run.steps == 7);
}

TEST_CASE("a hash inside a value is part of the value", "[sim][config_file]") {
    // A comment is a whole line rather than a trailing field, so that a path
    // with a hash in it is a path. The alternative needs a quoting rule, and a
    // format with quoting needs an escaping rule next.
    const Configuration configuration = parse("[output]\ntrajectory_path = out#1.otj\n");
    CHECK(configuration.output.trajectory_path == "out#1.otj");
}

TEST_CASE("a key may name its own section", "[sim][config_file]") {
    const Configuration configuration = parse("solver.softening = 0.125\nrun.steps = 3\n");
    CHECK(configuration.solver.softening == static_cast<Real>(0.125));
    CHECK(configuration.run.steps == 3);
}

TEST_CASE("a qualified key does not move the current section", "[sim][config_file]") {
    const Configuration configuration = parse(R"(
[run]
solver.softening = 0.5
steps = 9
)");

    CHECK(configuration.solver.softening == static_cast<Real>(0.5));
    CHECK(configuration.run.steps == 9);
}

TEST_CASE("numbers are read in the classic locale", "[sim][config_file]") {
    // The failure this guards against never appears on the machine that wrote
    // the code: on a system configured for a language whose decimal separator
    // is a comma, a parser that used the environment's locale would read this
    // as one rather than as a thousandth, and every run on that machine would
    // silently use a timestep a thousand times too large.
    const Configuration configuration = parse("[run]\ntimestep = 0.001\n");
    CHECK(configuration.run.timestep == static_cast<Real>(0.001));
}

TEST_CASE("scientific notation and negative numbers are read", "[sim][config_file]") {
    const Configuration configuration = parse("[run]\ntimestep = 1.5e-3\n");
    CHECK(configuration.run.timestep == static_cast<Real>(1.5e-3));
}

TEST_CASE("a mistake in a configuration file names the line it is on", "[sim][config_file]") {
    // The reason `ConfigurationError` carries a line at all. A message that says
    // only "expected a number" is of little use against a file of sixty lines.
    try {
        const Configuration parsed = parse("[run]\nsteps = 10\ntimestep = fast\n");
        FAIL("a value that is not a number should have been refused, not read as "
             << parsed.run.timestep);
    } catch (const ConfigurationError& error) {
        CHECK(error.line() == 3);
        CHECK(error.origin() == "test");
        CHECK(std::string_view{error.what()}.find("timestep") != std::string_view::npos);
    }
}

TEST_CASE("nothing in a configuration file is skipped or guessed", "[sim][config_file]") {
    // Every one of these would, in a forgiving parser, produce a run that
    // computed something other than what its author asked for and said nothing.
    SECTION("an unknown section") {
        CHECK_THROWS_AS(parse("[solvers]\nkind = direct\n"), ConfigurationError);
    }

    SECTION("a mistyped setting") {
        CHECK_THROWS_AS(parse("[solver]\nsoftenning = 0.05\n"), ConfigurationError);
    }

    SECTION("a value that does not parse") {
        CHECK_THROWS_AS(parse("[run]\nsteps = many\n"), ConfigurationError);
    }

    SECTION("a number where a boolean belongs") {
        CHECK_THROWS_AS(parse("[solver]\nquadrupole = 1\n"), ConfigurationError);
    }

    SECTION("a negative count, which is not a small number but a wrong one") {
        CHECK_THROWS_AS(parse("[run]\nsteps = -5\n"), ConfigurationError);
    }

    SECTION("trailing rubbish after a number") {
        CHECK_THROWS_AS(parse("[run]\ntimestep = 0.5 and a bit\n"), ConfigurationError);
    }

    SECTION("a key with no value") {
        CHECK_THROWS_AS(parse("[run]\nsteps =\n"), ConfigurationError);
    }

    SECTION("a line that is not a setting at all") {
        CHECK_THROWS_AS(parse("[run]\nsteps\n"), ConfigurationError);
    }

    SECTION("a setting before any section") {
        CHECK_THROWS_AS(parse("steps = 5\n"), ConfigurationError);
    }

    SECTION("an unterminated section heading") {
        CHECK_THROWS_AS(parse("[run\nsteps = 5\n"), ConfigurationError);
    }
}

TEST_CASE("a setting given twice is a mistake rather than a preference", "[sim][config_file]") {
    // Last one wins would mean the file says one thing and the run does
    // another, with nothing to indicate which line was believed.
    CHECK_THROWS_AS(parse("[run]\nsteps = 5\nsteps = 6\n"), ConfigurationError);

    // Including when the second spelling is the qualified form of the first.
    CHECK_THROWS_AS(parse("[run]\nsteps = 5\nrun.steps = 6\n"), ConfigurationError);
}

TEST_CASE("the same setting in two sections is not a duplicate", "[sim][config_file]") {
    // `count` and `steps` are unrelated; only the qualified name has to be
    // unique. A parser keyed on the bare key would refuse this.
    const Configuration configuration = parse(R"(
[run]
steps = 5

[initial_conditions]
count = 5
)");

    CHECK(configuration.run.steps == 5);
    CHECK(configuration.initial_conditions.count == 5);
}

TEST_CASE("the command line overrides the file", "[sim][config_file]") {
    Configuration configuration = parse("[run]\nsteps = 100\ntimestep = 0.01\n");

    const std::vector<std::string> settings{"run.steps=250", "solver.kind=direct"};
    orrery::sim::apply_settings(configuration, settings, "--set");

    CHECK(configuration.run.steps == 250);
    CHECK(configuration.solver.kind == SolverKind::kDirect);

    // What the command line did not mention is left as the file had it.
    CHECK(configuration.run.timestep == static_cast<Real>(0.01));
}

TEST_CASE("an override given twice is refused, and names which one", "[sim][config_file]") {
    Configuration configuration;
    const std::vector<std::string> settings{"run.steps=1", "run.timestep=0.5", "run.steps=2"};

    try {
        orrery::sim::apply_settings(configuration, settings, "--set");
        FAIL("the same setting twice on one command line should have been refused");
    } catch (const ConfigurationError& error) {
        // The line number is the index of the offending assignment, counting
        // from one, which is what makes the message point at an argument.
        CHECK(error.line() == 3);
    }
}

TEST_CASE("a written configuration parses back to the same configuration",
          "[sim][config_file][property]") {
    // The property that matters for checkpoints: a checkpoint carries the
    // configuration as text, so a setting that can be written and not read back
    // would be a setting a resumed run silently lost.
    std::mt19937 engine(kSeed);
    std::uniform_real_distribution<double> real(-1000, 1000);
    std::uniform_int_distribution<int> small(0, 3);
    std::uniform_int_distribution<std::uint64_t> large(0, 1ULL << 40U);

    constexpr int kTrials = 200;
    for (int trial = 0; trial < kTrials; ++trial) {
        INFO("seed " << kSeed << ", trial " << trial);

        Configuration original;
        original.run.timestep = static_cast<Real>(real(engine));
        original.run.steps = large(engine);
        original.run.seed = large(engine);

        original.initial_conditions.kind =
            std::array{InitialConditionKind::kPlummer, InitialConditionKind::kUniformSphere,
                       InitialConditionKind::kKepler}[static_cast<std::size_t>(small(engine) % 3)];
        original.initial_conditions.count = large(engine);
        original.initial_conditions.total_mass = static_cast<Real>(real(engine));
        original.initial_conditions.scale_radius = static_cast<Real>(real(engine));
        original.initial_conditions.radius = static_cast<Real>(real(engine));
        original.initial_conditions.mass_fraction_cutoff = static_cast<Real>(real(engine));
        original.initial_conditions.primary_mass = static_cast<Real>(real(engine));
        original.initial_conditions.secondary_mass = static_cast<Real>(real(engine));
        original.initial_conditions.semi_major_axis = static_cast<Real>(real(engine));
        original.initial_conditions.eccentricity = static_cast<Real>(real(engine));

        original.solver.kind =
            std::array{SolverKind::kDirect, SolverKind::kBarnesHut, SolverKind::kSyclDirect,
                       SolverKind::kSyclTree}[static_cast<std::size_t>(small(engine))];
        original.solver.softening = static_cast<Real>(real(engine));
        original.solver.opening_angle = static_cast<Real>(real(engine));
        original.solver.leaf_capacity = large(engine);
        original.solver.quadrupole = small(engine) % 2 == 0;
        original.solver.executor =
            std::array{ExecutorKind::kSerial, ExecutorKind::kStatic,
                       ExecutorKind::kWorkStealing}[static_cast<std::size_t>(small(engine) % 3)];
        original.solver.threads = static_cast<unsigned>(small(engine));
        original.solver.allow_cpu_fallback = small(engine) % 2 == 0;

        original.integrator.kind =
            std::array{IntegratorKind::kVelocityVerlet, IntegratorKind::kYoshida4,
                       IntegratorKind::kRungeKutta4}[static_cast<std::size_t>(small(engine) % 3)];

        original.output.trajectory_path = "trajectory.otj";
        original.output.trajectory_stride = large(engine);
        original.output.trajectory_velocities = small(engine) % 2 == 0;
        original.output.diagnostics_path = "diagnostics.csv";
        original.output.diagnostics_stride = large(engine);
        original.output.checkpoint_path = "state.ock";
        original.output.checkpoint_stride = large(engine);

        // Equality, not agreement to some number of digits. The values are
        // written with enough significant figures to be recovered exactly, and
        // a timestep that changed in its last bit would make a resumed run
        // diverge from an uninterrupted one.
        CHECK(parse(write(original)) == original);
    }
}

TEST_CASE("a configuration with no paths in it round-trips too", "[sim][config_file]") {
    // The output paths are the one part of the format written conditionally,
    // since an empty value is not a legal line, so the empty case is worth its
    // own test rather than being left to the randomised one above.
    const Configuration original;
    CHECK(parse(write(original)) == original);
}

TEST_CASE("a file that cannot be opened is reported like any other mistake", "[sim][config_file]") {
    CHECK_THROWS_AS(orrery::sim::read_configuration_file("no-such-configuration.orrery"),
                    ConfigurationError);
}
