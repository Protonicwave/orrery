#include "orrery/sim/assembly.hpp"

#include <cmath>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "orrery/backend/executor.hpp"
#include "orrery/backend/serial_executor.hpp"
#include "orrery/backend/static_executor.hpp"
#include "orrery/backend/sycl_device.hpp"
#include "orrery/backend/work_stealing_executor.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/initial_conditions/disc_galaxy.hpp"
#include "orrery/initial_conditions/galaxy_collision.hpp"
#include "orrery/initial_conditions/kepler.hpp"
#include "orrery/initial_conditions/plummer.hpp"
#include "orrery/initial_conditions/uniform_sphere.hpp"
#include "orrery/integrators/integrator.hpp"
#include "orrery/integrators/runge_kutta4.hpp"
#include "orrery/integrators/velocity_verlet.hpp"
#include "orrery/integrators/yoshida4.hpp"
#include "orrery/sim/configuration.hpp"
#include "orrery/sim/run_output.hpp"
#include "orrery/sim/simulation.hpp"
#include "orrery/solvers/barnes_hut_solver.hpp"
#include "orrery/solvers/direct_solver.hpp"
#include "orrery/solvers/force_solver.hpp"
#include "orrery/solvers/octree.hpp"

#ifdef ORRERY_ENABLE_SYCL
#    include "orrery/solvers/sycl_direct_solver.hpp"
#    include "orrery/solvers/sycl_tree_solver.hpp"
#endif

namespace orrery::sim {
namespace {

/// One galaxy of the size and mass the caller asks for, described by the
/// settings everything else about a galaxy comes from.
///
/// Takes the count and the mass rather than reading them from the settings,
/// because a collision divides both between two galaxies and a single galaxy
/// takes all of each. Everything that is a property of what a galaxy is, rather
/// than of how big this one is, comes from the configuration.
[[nodiscard]] initial_conditions::DiscGalaxyParameters
galaxy_from(const Configuration& configuration, core::Index count, core::Real mass) {
    const InitialConditionSettings& settings = configuration.initial_conditions;

    initial_conditions::DiscGalaxyParameters parameters;
    parameters.count = count;
    parameters.disc_mass = mass * (1 - settings.bulge_fraction);
    parameters.bulge_mass = mass * settings.bulge_fraction;
    parameters.scale_length = settings.scale_length;
    parameters.scale_height = settings.scale_height;
    parameters.bulge_radius = settings.bulge_radius;
    parameters.mass_fraction_cutoff = settings.mass_fraction_cutoff;
    parameters.inclination = settings.inclination;
    parameters.position_angle = settings.position_angle;

    // The one setting that comes from another section. A disc is built at the
    // speeds the forces it will feel can support, and those depend on the
    // solver's softening, so a galaxy assembled without this would be in
    // balance with a force law the run is not going to use. `disc_galaxy.hpp`
    // sets out what that costs.
    parameters.softening = configuration.solver.softening;
    return parameters;
}

[[nodiscard]] solvers::TreeParameters tree_parameters(const SolverSettings& settings) noexcept {
    solvers::TreeParameters parameters;
    parameters.opening_angle = settings.opening_angle;
    parameters.leaf_capacity = settings.leaf_capacity;
    parameters.quadrupole = settings.quadrupole;
    return parameters;
}

/// The CPU solver that computes the same physics as `kind`.
///
/// Used only when a GPU solver was asked for and cannot be provided. The tree
/// falls back to the tree and direct summation to direct summation, so the
/// answer a fallback produces differs from the one that was asked for by the
/// arithmetic of a different processor and not by the algorithm.
[[nodiscard]] SolverKind cpu_equivalent(SolverKind kind) noexcept {
    return kind == SolverKind::kSyclTree ? SolverKind::kBarnesHut : SolverKind::kDirect;
}

[[nodiscard]] std::unique_ptr<solvers::ForceSolver>
make_cpu_solver(SolverKind kind, const SolverSettings& settings, backend::Executor* executor) {
    const core::Softening softening{settings.softening};

    if (kind == SolverKind::kBarnesHut) {
        if (executor != nullptr) {
            return std::make_unique<solvers::BarnesHutSolver>(tree_parameters(settings), softening,
                                                              *executor);
        }
        return std::make_unique<solvers::BarnesHutSolver>(tree_parameters(settings), softening);
    }

    if (executor != nullptr) {
        return std::make_unique<solvers::DirectSolver>(softening, *executor);
    }
    return std::make_unique<solvers::DirectSolver>(softening);
}

/// The GPU solver, or null when this machine or this build cannot provide one.
///
/// Compiled to a function that always returns null when the backend is off,
/// rather than guarded at each call site, so that the logic deciding what to do
/// about a null is written once and is the same logic in both builds.
[[nodiscard]] std::unique_ptr<solvers::ForceSolver>
try_make_gpu_solver([[maybe_unused]] SolverKind kind,
                    [[maybe_unused]] const SolverSettings& settings,
                    [[maybe_unused]] backend::Executor* executor) {
#ifdef ORRERY_ENABLE_SYCL
    const core::Softening softening{settings.softening};
    if (kind == SolverKind::kSyclTree) {
        return solvers::SyclTreeSolver::try_create(tree_parameters(settings), softening, executor);
    }
    return solvers::SyclDirectSolver::try_create(softening);
#else
    return nullptr;
#endif
}

[[nodiscard]] bool is_gpu_solver(SolverKind kind) noexcept {
    return kind == SolverKind::kSyclDirect || kind == SolverKind::kSyclTree;
}

} // namespace

core::Index primary_galaxy_count(const Configuration& configuration) {
    const InitialConditionSettings& settings = configuration.initial_conditions;
    if (settings.kind != InitialConditionKind::kGalaxyCollision) {
        return 0;
    }

    // Rounded in double precision whatever the build's scalar type is. The split
    // decides which particle belongs to which galaxy, so a single-precision build
    // rounding differently from a double-precision one would mean two builds
    // reading the same configuration file and producing different systems.
    return static_cast<core::Index>(std::llround(static_cast<double>(settings.count) /
                                                 (1.0 + static_cast<double>(settings.mass_ratio))));
}

core::ParticleData make_initial_conditions(const Configuration& configuration) {
    const InitialConditionSettings& settings = configuration.initial_conditions;
    core::RandomSource random(configuration.run.seed);

    switch (settings.kind) {
    case InitialConditionKind::kPlummer: {
        initial_conditions::PlummerParameters parameters;
        parameters.count = settings.count;
        parameters.total_mass = settings.total_mass;
        parameters.scale_radius = resolved_scale_radius(settings);
        parameters.mass_fraction_cutoff = settings.mass_fraction_cutoff;
        return initial_conditions::make_plummer_sphere(parameters, random);
    }
    case InitialConditionKind::kUniformSphere: {
        initial_conditions::UniformSphereParameters parameters;
        parameters.count = settings.count;
        parameters.total_mass = settings.total_mass;
        parameters.radius = settings.radius;
        return initial_conditions::make_uniform_sphere(parameters, random);
    }
    case InitialConditionKind::kDiscGalaxy:
        return initial_conditions::make_disc_galaxy(
            galaxy_from(configuration, settings.count, settings.total_mass), random);
    case InitialConditionKind::kGalaxyCollision: {
        const core::Real ratio = settings.mass_ratio;

        // The requested count and mass are the pair's, split between the two so
        // that both galaxies are made of particles of the same mass. A run that
        // asks for a million particles gets a million whatever the mass ratio
        // is, which is the property that makes a scaling study over the count
        // mean something.
        const core::Index primary_count = primary_galaxy_count(configuration);

        initial_conditions::GalaxyCollisionParameters encounter;
        encounter.primary =
            galaxy_from(configuration, primary_count, settings.total_mass / (1 + ratio));
        encounter.secondary = galaxy_from(configuration, settings.count - primary_count,
                                          settings.total_mass * ratio / (1 + ratio));

        // The smaller galaxy is smaller in size as well as in mass, its three
        // lengths scaled by the square root of the mass ratio so that the two
        // have the same mean surface density. That is roughly the size-mass
        // relation discs are observed to follow, and it matters for the picture
        // rather than only for the realism: a secondary given the primary's
        // scale length at half its mass is half as dense and dissolves on the
        // first passage instead of surviving to make a second one.
        const core::Real size = std::sqrt(ratio);
        encounter.secondary.scale_length *= size;
        encounter.secondary.scale_height *= size;
        encounter.secondary.bulge_radius *= size;
        encounter.secondary.inclination = settings.secondary_inclination;
        encounter.secondary.position_angle = settings.secondary_position_angle;

        encounter.separation = settings.separation;
        encounter.impact_parameter = settings.impact_parameter;
        encounter.approach_speed = settings.approach_speed;
        return initial_conditions::make_galaxy_collision(encounter, random);
    }
    case InitialConditionKind::kKepler:
        break;
    }

    initial_conditions::KeplerParameters parameters;
    parameters.primary_mass = settings.primary_mass;
    parameters.secondary_mass = settings.secondary_mass;
    parameters.semi_major_axis = settings.semi_major_axis;
    parameters.eccentricity = settings.eccentricity;
    return initial_conditions::make_kepler_orbit(parameters);
}

std::unique_ptr<integrators::Integrator> make_integrator(const Configuration& configuration) {
    switch (configuration.integrator.kind) {
    case IntegratorKind::kYoshida4:
        return std::make_unique<integrators::Yoshida4>();
    case IntegratorKind::kRungeKutta4:
        return std::make_unique<integrators::RungeKutta4>();
    case IntegratorKind::kVelocityVerlet:
        break;
    }
    return std::make_unique<integrators::VelocityVerlet>();
}

std::unique_ptr<backend::Executor> make_executor(const Configuration& configuration) {
    const SolverSettings& settings = configuration.solver;
    switch (settings.executor) {
    case ExecutorKind::kSerial:
        return std::make_unique<backend::SerialExecutor>();
    case ExecutorKind::kStatic:
        return std::make_unique<backend::StaticExecutor>(settings.threads);
    case ExecutorKind::kWorkStealing:
        break;
    }
    return std::make_unique<backend::WorkStealingExecutor>(settings.threads);
}

std::unique_ptr<solvers::ForceSolver>
make_solver(const Configuration& configuration, backend::Executor* executor, std::ostream& report) {
    const SolverSettings& settings = configuration.solver;
    if (!is_gpu_solver(settings.kind)) {
        return make_cpu_solver(settings.kind, settings, executor);
    }

    if (std::unique_ptr<solvers::ForceSolver> solver =
            try_make_gpu_solver(settings.kind, settings, executor)) {
        return solver;
    }

    const std::string reason = backend::kSyclBackendCompiled
                                   ? "no usable device was found"
                                   : "this build was configured without the SYCL backend";
    if (!settings.allow_cpu_fallback) {
        throw std::runtime_error("solver.kind is " + std::string{to_string(settings.kind)} +
                                 " and " + reason +
                                 ". Set solver.allow_cpu_fallback = true to run on the CPU "
                                 "instead");
    }

    const SolverKind fallback = cpu_equivalent(settings.kind);
    report << "note: " << to_string(settings.kind) << " was asked for but " << reason
           << ", so this run uses " << to_string(fallback) << " on the CPU\n";
    return make_cpu_solver(fallback, settings, executor);
}

Simulation assemble(const Configuration& configuration, std::unique_ptr<RunOutput> output,
                    std::ostream& report) {
    // The executor is built first and moved into the simulation last, because
    // the solver holds a reference to it that has to stay valid for the run.
    std::unique_ptr<backend::Executor> executor = make_executor(configuration);
    std::unique_ptr<solvers::ForceSolver> solver =
        make_solver(configuration, executor.get(), report);

    return {make_initial_conditions(configuration),
            std::move(solver),
            make_integrator(configuration),
            configuration.run.timestep,
            std::move(executor),
            std::move(output)};
}

} // namespace orrery::sim
