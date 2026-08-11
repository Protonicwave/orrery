#include "orrery/sim/configuration.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "orrery/core/types.hpp"
#include "orrery/initial_conditions/plummer.hpp"

namespace orrery::sim {
namespace {

/// The two sampled models take a particle count and draw from the seed; the
/// Kepler configuration is two bodies whose state is determined by its
/// elements. Several checks below differ only in that, so the question is asked
/// once here.
[[nodiscard]] bool is_sampled(InitialConditionKind kind) noexcept {
    return kind != InitialConditionKind::kKepler;
}

/// The two configurations built from disc galaxies, which share a description
/// of what a galaxy is and differ in how many of them there are.
[[nodiscard]] bool is_galaxy(InitialConditionKind kind) noexcept {
    return kind == InitialConditionKind::kDiscGalaxy ||
           kind == InitialConditionKind::kGalaxyCollision;
}

[[nodiscard]] bool is_tree_solver(SolverKind kind) noexcept {
    return kind == SolverKind::kBarnesHut || kind == SolverKind::kSyclTree;
}

/// Whether a value differs from what the field would hold if nobody had
/// mentioned it.
///
/// Used for the unused-field warnings below. Compared exactly rather than within
/// a tolerance, which is right for once: the question is not whether two
/// computed quantities agree but whether a line appeared in a file, and a value
/// that came from parsing the same text as the default is bit for bit the
/// default.
[[nodiscard]] bool differs(core::Real value, core::Real fallback) noexcept {
    return value != fallback;
}

void add(std::vector<std::string>& problems, std::string_view setting, std::string_view complaint) {
    problems.emplace_back(std::string{setting} + ": " + std::string{complaint});
}

/// Every value of each enumeration, which is what makes reading and writing one
/// source of truth rather than two lists that can drift apart.
///
/// The spellings live in the `to_string` overloads below, where a `switch`
/// makes the compiler insist that a value added later is given one. The parsers
/// and the name lists are then written in terms of `to_string` over these
/// arrays, so a spelling cannot be written and not read back. What the arrays
/// themselves cannot check is that they list every value, and
/// `tests/sim/configuration_test.cpp` covers that by counting them.
constexpr std::array kSolverKinds{SolverKind::kDirect, SolverKind::kBarnesHut,
                                  SolverKind::kSyclDirect, SolverKind::kSyclTree};

constexpr std::array kIntegratorKinds{IntegratorKind::kVelocityVerlet, IntegratorKind::kYoshida4,
                                      IntegratorKind::kRungeKutta4};

constexpr std::array kInitialConditionKinds{
    InitialConditionKind::kPlummer, InitialConditionKind::kUniformSphere,
    InitialConditionKind::kKepler, InitialConditionKind::kDiscGalaxy,
    InitialConditionKind::kGalaxyCollision};

constexpr std::array kExecutorKinds{ExecutorKind::kSerial, ExecutorKind::kStatic,
                                    ExecutorKind::kWorkStealing};

/// The value whose spelling is `text`, if any.
template<typename Kind, std::size_t Count>
[[nodiscard]] std::optional<Kind> parse_kind(std::string_view text,
                                             const std::array<Kind, Count>& kinds) noexcept {
    for (const Kind kind : kinds) {
        if (to_string(kind) == text) {
            return kind;
        }
    }
    return std::nullopt;
}

/// The spellings, for a message that has to list the alternatives.
template<typename Kind, std::size_t Count>
[[nodiscard]] std::string kind_names(const std::array<Kind, Count>& kinds) {
    std::string names;
    for (const Kind kind : kinds) {
        if (!names.empty()) {
            names += ", ";
        }
        names += to_string(kind);
    }
    return names;
}

} // namespace

std::string_view to_string(SolverKind kind) noexcept {
    switch (kind) {
    case SolverKind::kDirect:
        return "direct";
    case SolverKind::kBarnesHut:
        return "barnes-hut";
    case SolverKind::kSyclDirect:
        return "sycl-direct";
    case SolverKind::kSyclTree:
        return "sycl-tree";
    }
    return "direct";
}

std::string_view to_string(IntegratorKind kind) noexcept {
    switch (kind) {
    case IntegratorKind::kVelocityVerlet:
        return "velocity-verlet";
    case IntegratorKind::kYoshida4:
        return "yoshida4";
    case IntegratorKind::kRungeKutta4:
        return "rk4";
    }
    return "velocity-verlet";
}

std::string_view to_string(InitialConditionKind kind) noexcept {
    switch (kind) {
    case InitialConditionKind::kPlummer:
        return "plummer";
    case InitialConditionKind::kUniformSphere:
        return "uniform-sphere";
    case InitialConditionKind::kKepler:
        return "kepler";
    case InitialConditionKind::kDiscGalaxy:
        return "disc-galaxy";
    case InitialConditionKind::kGalaxyCollision:
        return "galaxy-collision";
    }
    return "plummer";
}

std::string_view to_string(ExecutorKind kind) noexcept {
    switch (kind) {
    case ExecutorKind::kSerial:
        return "serial";
    case ExecutorKind::kStatic:
        return "static";
    case ExecutorKind::kWorkStealing:
        return "work-stealing";
    }
    return "work-stealing";
}

std::optional<SolverKind> parse_solver_kind(std::string_view text) noexcept {
    return parse_kind(text, kSolverKinds);
}

std::optional<IntegratorKind> parse_integrator_kind(std::string_view text) noexcept {
    return parse_kind(text, kIntegratorKinds);
}

std::optional<InitialConditionKind> parse_initial_condition_kind(std::string_view text) noexcept {
    return parse_kind(text, kInitialConditionKinds);
}

std::optional<ExecutorKind> parse_executor_kind(std::string_view text) noexcept {
    return parse_kind(text, kExecutorKinds);
}

std::string solver_kind_names() {
    return kind_names(kSolverKinds);
}

std::string integrator_kind_names() {
    return kind_names(kIntegratorKinds);
}

std::string initial_condition_kind_names() {
    return kind_names(kInitialConditionKinds);
}

std::string executor_kind_names() {
    return kind_names(kExecutorKinds);
}

core::Real resolved_scale_radius(const InitialConditionSettings& settings) noexcept {
    return settings.scale_radius > 0 ? settings.scale_radius
                                     : initial_conditions::kStandardPlummerRadius;
}

namespace {

/// One function per section, because the whole check is a list of unrelated
/// conditions and reading it as one function means holding five sections in
/// mind at once. They are free functions taking the vector rather than members
/// of a checker class, since there is no state between them.
void check_run(const RunSettings& run, std::vector<std::string>& problems) {
    if (!(run.timestep > 0)) {
        // Written as the negation of the condition wanted rather than as
        // `<= 0`, so that a NaN timestep is caught. A comparison against NaN is
        // false whichever way round it is written, and `timestep <= 0` would let
        // one through into an integration where every position becomes NaN on
        // the first step.
        add(problems, "run.timestep", "must be a positive number");
    }
    if (run.steps == 0) {
        add(problems, "run.steps", "must be at least one");
    }
}

/// The elements of a bound two-body orbit.
void check_kepler(const InitialConditionSettings& initial, std::vector<std::string>& problems) {
    if (!(initial.primary_mass > 0)) {
        add(problems, "initial_conditions.primary_mass", "must be a positive number");
    }
    if (!(initial.secondary_mass > 0)) {
        add(problems, "initial_conditions.secondary_mass", "must be a positive number");
    }
    if (!(initial.semi_major_axis > 0)) {
        add(problems, "initial_conditions.semi_major_axis", "must be a positive number");
    }
    if (!(initial.eccentricity >= 0) || !(initial.eccentricity < 1)) {
        // An eccentricity of one or more describes a parabolic or hyperbolic
        // encounter. Those are unbound and have no period, so every analytic
        // quantity the configuration exists to be checked against would be
        // answering a question about an ellipse that does not exist.
        add(problems, "initial_conditions.eccentricity", "must lie in [0, 1) for a bound orbit");
    }
    if (initial.count != 0) {
        add(problems, "initial_conditions.count",
            "is not used by the kepler configuration, which is always two bodies");
    }
}

/// What both galaxy configurations need, which is the three lengths that give a
/// galaxy a shape and the split of its mass between the disc and the bulge.
void check_galaxy(const InitialConditionSettings& initial, std::vector<std::string>& problems) {
    if (!(initial.bulge_fraction >= 0) || !(initial.bulge_fraction < 1)) {
        // One would be a galaxy that is all bulge and no disc, which is a
        // Plummer sphere written the long way round and has no spin axis for the
        // inclination to mean anything about.
        add(problems, "initial_conditions.bulge_fraction", "must lie in [0, 1)");
    }
    if (!(initial.scale_length > 0)) {
        add(problems, "initial_conditions.scale_length", "must be a positive number");
    }
    if (!(initial.scale_height > 0)) {
        add(problems, "initial_conditions.scale_height", "must be a positive number");
    }
    if (!(initial.bulge_radius > 0)) {
        add(problems, "initial_conditions.bulge_radius", "must be a positive number");
    }
}

/// What a collision needs beyond two galaxies: somewhere to put them and a
/// speed to bring them together at.
void check_collision(const InitialConditionSettings& initial, std::vector<std::string>& problems) {
    if (!(initial.mass_ratio > 0) || !(initial.mass_ratio <= 1)) {
        // Above one is the same encounter with the two galaxies exchanged, and
        // allowing both spellings would mean two configuration files that
        // describe one scenario and do not compare equal.
        add(problems, "initial_conditions.mass_ratio", "must lie in (0, 1]");
    }
    if (initial.separation == 0 && initial.impact_parameter == 0) {
        add(problems, "initial_conditions.separation",
            "and initial_conditions.impact_parameter cannot both be zero, since the two "
            "galaxies would start on top of one another");
    }
    if (!(initial.approach_speed >= 0)) {
        add(problems, "initial_conditions.approach_speed", "must not be negative");
    }
    if (initial.count < 4) {
        // Two galaxies of at least two particles each. The generator would
        // accept fewer by rounding one galaxy down to a single particle, which
        // is a configuration nobody means to ask for.
        add(problems, "initial_conditions.count",
            "must be at least four for a collision, which is two galaxies");
    }
}

void check_initial_conditions(const InitialConditionSettings& initial,
                              std::vector<std::string>& problems) {
    if (is_sampled(initial.kind)) {
        if (initial.count < 2) {
            add(problems, "initial_conditions.count",
                "must be at least two for a sampled configuration");
        }
        if (!(initial.total_mass > 0)) {
            add(problems, "initial_conditions.total_mass", "must be a positive number");
        }
    }

    // Shared by the Plummer sphere and by the disc of a galaxy, both of which
    // are infinite models drawn from a finite fraction of their mass.
    if (initial.kind == InitialConditionKind::kPlummer || is_galaxy(initial.kind)) {
        if (!(initial.mass_fraction_cutoff > 0) || !(initial.mass_fraction_cutoff < 1)) {
            add(problems, "initial_conditions.mass_fraction_cutoff", "must lie strictly in (0, 1)");
        }
    }

    if (is_galaxy(initial.kind)) {
        check_galaxy(initial, problems);
    }

    switch (initial.kind) {
    case InitialConditionKind::kPlummer:
        if (initial.scale_radius < 0) {
            add(problems, "initial_conditions.scale_radius",
                "must be positive, or zero for the standard N-body value");
        }
        break;
    case InitialConditionKind::kUniformSphere:
        if (!(initial.radius > 0)) {
            add(problems, "initial_conditions.radius", "must be a positive number");
        }
        break;
    case InitialConditionKind::kKepler:
        check_kepler(initial, problems);
        break;
    case InitialConditionKind::kDiscGalaxy:
        break;
    case InitialConditionKind::kGalaxyCollision:
        check_collision(initial, problems);
        break;
    }
}

void check_solver(const SolverSettings& solver, std::vector<std::string>& problems) {
    if (!(solver.softening >= 0)) {
        add(problems, "solver.softening", "must not be negative");
    }
    if (is_tree_solver(solver.kind)) {
        if (!(solver.opening_angle >= 0) || !(solver.opening_angle <= 1)) {
            // Above one a cell can be accepted while the particle being
            // accelerated is still inside it, which would have that particle
            // attract itself through its own cell's centre of mass. The solver
            // clamps rather than refuses, and this reports what the clamp would
            // silently do.
            add(problems, "solver.opening_angle", "must lie in [0, 1]");
        }
        if (solver.leaf_capacity == 0) {
            add(problems, "solver.leaf_capacity", "must be at least one");
        }
    } else {
        if (differs(solver.opening_angle, static_cast<core::Real>(0.5))) {
            add(problems, "solver.opening_angle",
                "is not used by a direct solver, which computes every pair");
        }
        if (solver.quadrupole) {
            add(problems, "solver.quadrupole", "is not used by a direct solver");
        }
    }
}

void check_output(const OutputSettings& output, std::vector<std::string>& problems) {
    if (output.trajectory_path.empty() && output.trajectory_stride != 0) {
        add(problems, "output.trajectory_stride", "has no effect without output.trajectory_path");
    }
    if (output.diagnostics_path.empty() && output.diagnostics_stride != 0) {
        add(problems, "output.diagnostics_stride", "has no effect without output.diagnostics_path");
    }
    if (output.checkpoint_path.empty() && output.checkpoint_stride != 0) {
        add(problems, "output.checkpoint_stride", "has no effect without output.checkpoint_path");
    }
    if (!output.trajectory_path.empty() && output.trajectory_path == output.checkpoint_path) {
        add(problems, "output.trajectory_path",
            "is the same file as output.checkpoint_path, and the two formats differ");
    }
}

} // namespace

std::vector<std::string> problems_with(const Configuration& configuration) {
    // In the order the settings appear, so that a report reads down the
    // configuration file that produced it.
    std::vector<std::string> problems;
    check_run(configuration.run, problems);
    check_initial_conditions(configuration.initial_conditions, problems);
    check_solver(configuration.solver, problems);
    check_output(configuration.output, problems);
    return problems;
}

} // namespace orrery::sim
