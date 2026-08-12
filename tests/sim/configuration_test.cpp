#include "orrery/sim/configuration.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "orrery/core/types.hpp"
#include "orrery/initial_conditions/plummer.hpp"

namespace {

using orrery::core::Real;
using orrery::sim::Configuration;
using orrery::sim::ExecutorKind;
using orrery::sim::InitialConditionKind;
using orrery::sim::IntegratorKind;
using orrery::sim::SolverKind;

/// A configuration that has nothing wrong with it, to change one field of.
///
/// Every test below asserts about one setting, and starting each from a
/// configuration that is otherwise valid is what makes the assertion about that
/// setting rather than about whichever other field happened to be zero.
[[nodiscard]] Configuration valid_configuration() {
    Configuration configuration;
    configuration.run.timestep = static_cast<Real>(0.001);
    configuration.run.steps = 100;
    configuration.initial_conditions.kind = InitialConditionKind::kPlummer;
    configuration.initial_conditions.count = 64;
    return configuration;
}

/// Whether any complaint names `setting`.
[[nodiscard]] bool complains_about(const Configuration& configuration, std::string_view setting) {
    const std::vector<std::string> problems = orrery::sim::problems_with(configuration);
    return std::ranges::any_of(
        problems, [setting](const std::string& problem) { return problem.starts_with(setting); });
}

} // namespace

TEST_CASE("every enumeration value has a spelling that reads back", "[sim][configuration]") {
    // The parsers are written in terms of `to_string`, so a value can only be
    // unreadable by being missing from the array the parser searches. That is
    // the one thing the implementation cannot check for itself, so it is
    // checked here, value by value.
    SECTION("solvers") {
        constexpr std::array kKinds{SolverKind::kDirect, SolverKind::kBarnesHut,
                                    SolverKind::kSyclDirect, SolverKind::kSyclTree};
        for (const SolverKind kind : kKinds) {
            CHECK(orrery::sim::parse_solver_kind(orrery::sim::to_string(kind)) == kind);
        }
    }

    SECTION("integrators") {
        constexpr std::array kKinds{IntegratorKind::kVelocityVerlet, IntegratorKind::kYoshida4,
                                    IntegratorKind::kRungeKutta4};
        for (const IntegratorKind kind : kKinds) {
            CHECK(orrery::sim::parse_integrator_kind(orrery::sim::to_string(kind)) == kind);
        }
    }

    SECTION("initial conditions") {
        constexpr std::array kKinds{
            InitialConditionKind::kPlummer, InitialConditionKind::kUniformSphere,
            InitialConditionKind::kKepler, InitialConditionKind::kDiscGalaxy,
            InitialConditionKind::kGalaxyCollision};
        for (const InitialConditionKind kind : kKinds) {
            CHECK(orrery::sim::parse_initial_condition_kind(orrery::sim::to_string(kind)) == kind);
        }
    }

    SECTION("executors") {
        constexpr std::array kKinds{ExecutorKind::kSerial, ExecutorKind::kStatic,
                                    ExecutorKind::kWorkStealing};
        for (const ExecutorKind kind : kKinds) {
            CHECK(orrery::sim::parse_executor_kind(orrery::sim::to_string(kind)) == kind);
        }
    }
}

TEST_CASE("a word that names nothing is refused rather than guessed at", "[sim][configuration]") {
    CHECK_FALSE(orrery::sim::parse_solver_kind("direkt").has_value());
    CHECK_FALSE(orrery::sim::parse_solver_kind("").has_value());

    // Case matters. Accepting "Direct" would mean the spelling in a file and the
    // spelling this project writes are not the same string, and the next
    // question would be which other case-insensitive comparisons apply.
    CHECK_FALSE(orrery::sim::parse_solver_kind("Direct").has_value());
}

TEST_CASE("the name lists mention every choice", "[sim][configuration]") {
    // These strings are what an error message offers someone who has mistyped a
    // setting, so a choice missing from one of them is a choice nobody is told
    // about.
    const std::string solvers = orrery::sim::solver_kind_names();
    CHECK(solvers.find("direct") != std::string::npos);
    CHECK(solvers.find("barnes-hut") != std::string::npos);
    CHECK(solvers.find("sycl-direct") != std::string::npos);
    CHECK(solvers.find("sycl-tree") != std::string::npos);

    CHECK(orrery::sim::integrator_kind_names().find("yoshida4") != std::string::npos);
    CHECK(orrery::sim::initial_condition_kind_names().find("kepler") != std::string::npos);
    CHECK(orrery::sim::executor_kind_names().find("work-stealing") != std::string::npos);
}

TEST_CASE("a runnable configuration has nothing said about it", "[sim][configuration]") {
    CHECK(orrery::sim::problems_with(valid_configuration()).empty());
}

TEST_CASE("the run settings must describe a run", "[sim][configuration]") {
    SECTION("a timestep of zero goes nowhere") {
        Configuration configuration = valid_configuration();
        configuration.run.timestep = 0;
        CHECK(complains_about(configuration, "run.timestep"));
    }

    SECTION("a negative timestep is refused here even though the integrators accept one") {
        // The symmetric integrators are exactly time-reversible and a negative
        // step is how a test asserts it, but that is a thing a test does
        // deliberately rather than a thing a configuration file should be able
        // to ask for by leaving out a minus sign.
        Configuration configuration = valid_configuration();
        configuration.run.timestep = static_cast<Real>(-0.001);
        CHECK(complains_about(configuration, "run.timestep"));
    }

    SECTION("a timestep that is not a number is caught") {
        // The check is written as the negation of what is wanted precisely so
        // that this case fails it. A comparison against a NaN is false whichever
        // way round it is written, so `timestep <= 0` would let this through.
        Configuration configuration = valid_configuration();
        configuration.run.timestep = std::numeric_limits<Real>::quiet_NaN();
        CHECK(complains_about(configuration, "run.timestep"));
    }

    SECTION("a run of no steps is not a run") {
        Configuration configuration = valid_configuration();
        configuration.run.steps = 0;
        CHECK(complains_about(configuration, "run.steps"));
    }
}

TEST_CASE("a sampled configuration needs particles to sample", "[sim][configuration]") {
    SECTION("one particle exerts no force on anything") {
        Configuration configuration = valid_configuration();
        configuration.initial_conditions.count = 1;
        CHECK(complains_about(configuration, "initial_conditions.count"));
    }

    SECTION("mass must be positive") {
        Configuration configuration = valid_configuration();
        configuration.initial_conditions.total_mass = 0;
        CHECK(complains_about(configuration, "initial_conditions.total_mass"));
    }

    SECTION("the Plummer cutoff is a fraction of the mass") {
        Configuration configuration = valid_configuration();
        configuration.initial_conditions.mass_fraction_cutoff = 1;
        CHECK(complains_about(configuration, "initial_conditions.mass_fraction_cutoff"));
    }
}

TEST_CASE("a galaxy needs the lengths that give it a shape", "[sim][configuration]") {
    Configuration configuration = valid_configuration();
    configuration.initial_conditions.kind = InitialConditionKind::kDiscGalaxy;

    SECTION("the defaults describe a galaxy") {
        CHECK(orrery::sim::problems_with(configuration).empty());
    }

    SECTION("a galaxy that is all bulge has no disc to orient") {
        configuration.initial_conditions.bulge_fraction = 1;
        CHECK(complains_about(configuration, "initial_conditions.bulge_fraction"));
    }

    SECTION("a disc of no extent") {
        configuration.initial_conditions.scale_length = 0;
        CHECK(complains_about(configuration, "initial_conditions.scale_length"));
    }

    SECTION("a disc of no thickness") {
        configuration.initial_conditions.scale_height = 0;
        CHECK(complains_about(configuration, "initial_conditions.scale_height"));
    }

    SECTION("a bulge of no extent") {
        configuration.initial_conditions.bulge_radius = 0;
        CHECK(complains_about(configuration, "initial_conditions.bulge_radius"));
    }
}

TEST_CASE("a collision needs two galaxies and somewhere to put them", "[sim][configuration]") {
    Configuration configuration = valid_configuration();
    configuration.initial_conditions.kind = InitialConditionKind::kGalaxyCollision;

    SECTION("the defaults describe an encounter") {
        CHECK(orrery::sim::problems_with(configuration).empty());
    }

    SECTION("a mass ratio above one is the same encounter written backwards") {
        configuration.initial_conditions.mass_ratio = 2;
        CHECK(complains_about(configuration, "initial_conditions.mass_ratio"));
    }

    SECTION("two galaxies in the same place have no encounter") {
        configuration.initial_conditions.separation = 0;
        configuration.initial_conditions.impact_parameter = 0;
        CHECK(complains_about(configuration, "initial_conditions.separation"));
    }

    SECTION("a receding pair is not a collision") {
        configuration.initial_conditions.approach_speed = -1;
        CHECK(complains_about(configuration, "initial_conditions.approach_speed"));
    }

    SECTION("four particles is the fewest that is two galaxies") {
        configuration.initial_conditions.count = 3;
        CHECK(complains_about(configuration, "initial_conditions.count"));
    }
}

TEST_CASE("an unbound orbit is not a Kepler configuration", "[sim][configuration]") {
    Configuration configuration = valid_configuration();
    configuration.initial_conditions.kind = InitialConditionKind::kKepler;
    configuration.initial_conditions.count = 0;

    SECTION("a bound orbit is accepted") {
        configuration.initial_conditions.eccentricity = static_cast<Real>(0.9);
        CHECK(orrery::sim::problems_with(configuration).empty());
    }

    SECTION("an eccentricity of one is a parabola, which has no period") {
        configuration.initial_conditions.eccentricity = 1;
        CHECK(complains_about(configuration, "initial_conditions.eccentricity"));
    }

    SECTION("a particle count means the author expects something that will not happen") {
        configuration.initial_conditions.count = 1000;
        CHECK(complains_about(configuration, "initial_conditions.count"));
    }
}

TEST_CASE("a tree setting on a direct solver is reported rather than ignored",
          "[sim][configuration]") {
    // Not pedantry. A file that sets an opening angle beside `kind = direct` was
    // written by someone who believes the run will approximate, and the run will
    // not. Saying so costs a line of output and saves an afternoon.
    Configuration configuration = valid_configuration();
    configuration.solver.kind = SolverKind::kDirect;

    SECTION("an opening angle") {
        configuration.solver.opening_angle = static_cast<Real>(0.7);
        CHECK(complains_about(configuration, "solver.opening_angle"));
    }

    SECTION("quadrupole moments") {
        configuration.solver.quadrupole = true;
        CHECK(complains_about(configuration, "solver.quadrupole"));
    }

    SECTION("but the same settings are fine on a tree") {
        configuration.solver.kind = SolverKind::kBarnesHut;
        configuration.solver.opening_angle = static_cast<Real>(0.7);
        configuration.solver.quadrupole = true;
        CHECK(orrery::sim::problems_with(configuration).empty());
    }
}

TEST_CASE("an opening angle above one would let a particle attract itself",
          "[sim][configuration]") {
    Configuration configuration = valid_configuration();
    configuration.solver.kind = SolverKind::kBarnesHut;
    configuration.solver.opening_angle = static_cast<Real>(1.5);
    CHECK(complains_about(configuration, "solver.opening_angle"));
}

TEST_CASE("an output stride without a path has nothing to write to", "[sim][configuration]") {
    Configuration configuration = valid_configuration();
    configuration.output.trajectory_stride = 10;
    CHECK(complains_about(configuration, "output.trajectory_stride"));

    configuration.output.trajectory_path = "trajectory.otj";
    CHECK_FALSE(complains_about(configuration, "output.trajectory_stride"));
}

TEST_CASE("two outputs may not share a file", "[sim][configuration]") {
    // The two formats have different magic and different layouts, so whichever
    // wrote second would produce a file neither reader accepts.
    Configuration configuration = valid_configuration();
    configuration.output.trajectory_path = "state";
    configuration.output.checkpoint_path = "state";
    CHECK(complains_about(configuration, "output.trajectory_path"));
}

TEST_CASE("every problem is reported, not only the first", "[sim][configuration]") {
    // The whole reason this returns a vector rather than throwing. Someone who
    // has written three mistakes should be told about three mistakes.
    Configuration configuration;
    configuration.run.timestep = 0;
    configuration.run.steps = 0;
    configuration.initial_conditions.count = 0;

    CHECK(orrery::sim::problems_with(configuration).size() >= 3);
}

TEST_CASE("an unset scale radius means the standard N-body value", "[sim][configuration]") {
    orrery::sim::InitialConditionSettings settings;
    settings.scale_radius = 0;
    CHECK(orrery::sim::resolved_scale_radius(settings) ==
          orrery::initial_conditions::kStandardPlummerRadius);

    settings.scale_radius = 2;
    CHECK(orrery::sim::resolved_scale_radius(settings) == static_cast<Real>(2));
}
