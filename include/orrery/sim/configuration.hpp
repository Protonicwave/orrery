#pragma once

/// \file
/// Everything a run is, as data.
///
/// A simulation in this project is decided by about twenty numbers: what to
/// start from, what to integrate it with, what to compute the forces with, how
/// far to go, and what to write down on the way. This file is the type that
/// holds those numbers, and it is deliberately nothing more than that. It builds
/// no solver, opens no file and validates nothing at construction, so a
/// configuration can be assembled by the parser, by the command line, or by a
/// test writing three fields, and all three produce the same kind of object.
///
/// Two things follow from keeping it inert.
///
/// The first is that a run is reproducible from a document. Every choice that
/// affects the answer is a field here, including the seed, so a configuration
/// file plus a revision of this repository determines the trajectory. That is
/// the property the whole phase exists to provide: a result nobody can
/// regenerate is an anecdote.
///
/// The second is that the checks are a function rather than a constructor.
/// `problems_with` returns every objection at once rather than throwing on the
/// first, because a person who has written a configuration file with three
/// mistakes in it should be told about three mistakes rather than made to
/// discover them one run at a time.
///
/// ## What is not here
///
/// A duration in units of time. A run is a number of steps, and the finishing
/// time is the step count times the timestep. The alternative, stopping when the
/// simulated time passes some target, has to round somewhere, and the rounding
/// would decide whether a resumed run took the same number of steps as an
/// uninterrupted one. Section 7 of the implementation plan requires an
/// interrupted run to resume to bitwise-identical state, and a step count is the
/// only way to say what a run is that cannot make that fail.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "orrery/core/types.hpp"

namespace orrery::sim {

/// Which force solver computes the accelerations.
///
/// The two GPU entries are accepted by the parser in every build, including one
/// compiled without the SYCL backend, and refused when the run is assembled.
/// That separation is deliberate: a configuration file is a document rather than
/// a program, and it should mean the same thing whatever binary reads it. The
/// alternative, a parser that rejects the word `sycl-tree` in a build without
/// SYCL, would report a spelling mistake for a file that is spelled correctly.
enum class SolverKind : std::uint8_t {
    kDirect,
    kBarnesHut,
    kSyclDirect,
    kSyclTree,
};

/// Which integrator advances the state.
enum class IntegratorKind : std::uint8_t {
    kVelocityVerlet,
    kYoshida4,
    kRungeKutta4,
};

/// Which configuration the run starts from.
enum class InitialConditionKind : std::uint8_t {
    kPlummer,
    kUniformSphere,
    kKepler,
};

/// How the CPU solvers divide their loop over target particles.
///
/// A run should use the work-stealing scheduler, which is the default and what
/// Phase 6 measured. The other two are here because a run that reproduces a
/// figure has to be able to ask for the scheme the figure was taken with.
enum class ExecutorKind : std::uint8_t {
    kSerial,
    kStatic,
    kWorkStealing,
};

/// The spelling each of the above has in a configuration file.
///
/// One function per enumeration rather than a template, so that the strings sit
/// beside the values they name and a value added later fails to compile until
/// it has been given a spelling.
[[nodiscard]] std::string_view to_string(SolverKind kind) noexcept;
[[nodiscard]] std::string_view to_string(IntegratorKind kind) noexcept;
[[nodiscard]] std::string_view to_string(InitialConditionKind kind) noexcept;
[[nodiscard]] std::string_view to_string(ExecutorKind kind) noexcept;

/// The inverse of the above, empty for a word that names nothing.
///
/// Paired with `to_string` in the same file and tested for round-tripping,
/// because a spelling that can be written and not read back is a configuration
/// file this project produces and cannot consume.
[[nodiscard]] std::optional<SolverKind> parse_solver_kind(std::string_view text) noexcept;
[[nodiscard]] std::optional<IntegratorKind> parse_integrator_kind(std::string_view text) noexcept;
[[nodiscard]] std::optional<InitialConditionKind>
parse_initial_condition_kind(std::string_view text) noexcept;
[[nodiscard]] std::optional<ExecutorKind> parse_executor_kind(std::string_view text) noexcept;

/// Every spelling each enumeration accepts, for the help text and for the error
/// message that lists the alternatives when a word names nothing.
[[nodiscard]] std::string solver_kind_names();
[[nodiscard]] std::string integrator_kind_names();
[[nodiscard]] std::string initial_condition_kind_names();
[[nodiscard]] std::string executor_kind_names();

/// How far to go, and from what stream of random numbers.
struct RunSettings {
    /// The interval of simulated time one step covers.
    ///
    /// There is no default worth having. The right timestep is a property of
    /// the configuration, roughly the shortest orbital period in it divided by
    /// a few dozen, and a plausible-looking default would produce plausible
    /// looking nonsense for any configuration it did not suit.
    core::Real timestep = 0;

    /// How many steps to take.
    core::Index steps = 0;

    /// The seed for the sampler that draws the initial conditions.
    ///
    /// Recorded even for the analytic configurations, which draw nothing, so
    /// that a configuration file's meaning does not depend on which generator
    /// it names.
    std::uint64_t seed = 0;

    [[nodiscard]] friend bool operator==(const RunSettings&, const RunSettings&) = default;
};

/// What the run starts from.
///
/// One structure with the union of every generator's parameters rather than a
/// variant. The generators share most of their parameters, the total is small,
/// and a flat record is what a key-and-value configuration file already is: a
/// variant would make the parser decide which alternative it was building
/// before it had read the key that says so.
///
/// A field the chosen generator does not use is ignored rather than rejected,
/// since a file that describes several configurations and selects between them
/// with one `kind` line is a reasonable thing to write. The exception is
/// `count` beside `kepler`, which `problems_with` reports: a particle count is
/// the field most likely to be believed, and a two-body configuration is the
/// one case where the number of particles is fixed by the physics rather than
/// chosen.
struct InitialConditionSettings {
    InitialConditionKind kind = InitialConditionKind::kPlummer;

    /// The number of particles, for the two sampled models.
    ///
    /// Ignored by the Kepler configuration, which is two bodies by definition.
    core::Index count = 0;

    /// Shared equally among the particles of a sampled model.
    core::Real total_mass = 1;

    /// The Plummer scale radius. Defaults to the value that puts a unit-mass
    /// sphere into standard N-body units, as `initial_conditions/plummer.hpp`
    /// describes.
    core::Real scale_radius = 0;

    /// The radius of the uniform sphere.
    core::Real radius = 1;

    /// The fraction of a Plummer model's mass the sample is drawn from.
    core::Real mass_fraction_cutoff = static_cast<core::Real>(0.999);

    /// The two masses of the Kepler configuration.
    core::Real primary_mass = 1;
    core::Real secondary_mass = 1;

    core::Real semi_major_axis = 1;
    core::Real eccentricity = 0;

    [[nodiscard]] friend bool operator==(const InitialConditionSettings&,
                                         const InitialConditionSettings&) = default;
};

/// Which solver, and the parameters that decide what it computes.
struct SolverSettings {
    SolverKind kind = SolverKind::kBarnesHut;

    /// The Plummer softening length, in the units of the configuration.
    ///
    /// Zero, meaning exact point masses, is the default for the reason
    /// `solvers/direct_solver.hpp` sets out at length: the right length is a
    /// property of the configuration rather than of the kernel, published rules
    /// of thumb for it differ by an order of magnitude, and a default that
    /// quietly softened would make the analytic comparisons approximate without
    /// saying so.
    core::Real softening = 0;

    /// The Barnes-Hut opening angle, ignored by the direct solvers.
    core::Real opening_angle = static_cast<core::Real>(0.5);

    core::Index leaf_capacity = 32;

    bool quadrupole = false;

    ExecutorKind executor = ExecutorKind::kWorkStealing;

    /// How many workers the executor runs with. Zero means one per core.
    unsigned threads = 0;

    /// Whether a run may fall back to the CPU when the GPU it asked for is
    /// absent.
    ///
    /// True by default, which is what a person running a configuration file on
    /// a laptop without a usable device wants: the same physics, computed more
    /// slowly, with a line on the terminal saying so. A measurement that must
    /// not be taken on the wrong processor sets it false and gets a run that
    /// refuses to start rather than a figure attributed to hardware that did
    /// not produce it.
    bool allow_cpu_fallback = true;

    [[nodiscard]] friend bool operator==(const SolverSettings&, const SolverSettings&) = default;
};

struct IntegratorSettings {
    /// Velocity Verlet by default, on ADR-0011's argument: one force evaluation
    /// per step and an energy error that stays inside a bounded envelope for
    /// ever, which over the number of steps a real run takes matters more than
    /// the order does.
    IntegratorKind kind = IntegratorKind::kVelocityVerlet;

    [[nodiscard]] friend bool operator==(const IntegratorSettings&,
                                         const IntegratorSettings&) = default;
};

/// What the run writes down, and how often.
///
/// Each of the three outputs is switched on by being given a path and off by
/// being left empty, and each has its own stride in steps. A stride of zero
/// means the output is written once at the start and once at the end and not in
/// between, which is what a run wants when it cares about the final state and
/// not the path taken to it.
struct OutputSettings {
    /// Where the binary trajectory goes. `docs/formats/trajectory.md` specifies
    /// the file this produces.
    std::string trajectory_path;

    core::Index trajectory_stride = 0;

    /// Whether trajectory frames carry velocities as well as positions.
    ///
    /// Off by default because a renderer needs positions alone and the file is
    /// twice the size with them. A trajectory is not a checkpoint and cannot be
    /// resumed from either way, which is what `docs/formats/checkpoint.md`
    /// exists to provide.
    bool trajectory_velocities = false;

    /// Where the CSV diagnostics stream goes.
    std::string diagnostics_path;

    /// How often the conserved quantities are measured, in steps.
    ///
    /// Measuring them costs an N^2 pass, since the potential energy is a sum
    /// over pairs and shares no code with the solver on purpose, so this should
    /// be a few hundred rather than one. Zero, the default, measures at the two
    /// ends of the run only.
    core::Index diagnostics_stride = 0;

    /// Where checkpoints go, if any.
    ///
    /// One path rather than a pattern, overwritten each time. A run that keeps
    /// every checkpoint it takes fills a disc with states nobody asked for, and
    /// the state a person wants after an interruption is the last one.
    std::string checkpoint_path;

    core::Index checkpoint_stride = 0;

    [[nodiscard]] friend bool operator==(const OutputSettings&, const OutputSettings&) = default;
};

/// A whole run, as data.
struct Configuration {
    RunSettings run;
    InitialConditionSettings initial_conditions;
    SolverSettings solver;
    IntegratorSettings integrator;
    OutputSettings output;

    [[nodiscard]] friend bool operator==(const Configuration&, const Configuration&) = default;
};

/// Every objection to `configuration`, in the order the fields appear.
///
/// Empty for a configuration that can be run. Each message names the setting in
/// the spelling the file uses, so that the report can be read beside the
/// document that caused it.
///
/// This is a separate step from parsing because the two answer different
/// questions. The parser decides whether the file is a configuration; this
/// decides whether the configuration is a run. A file with `steps = -1` in it
/// fails the first and a file with `steps = 0` fails neither, but neither is
/// worth starting.
[[nodiscard]] std::vector<std::string> problems_with(const Configuration& configuration);

/// The scale radius the Plummer sampler will actually use.
///
/// Zero in the settings means the standard N-body value. Writing that value as
/// a default member initialiser would make `initial_conditions/plummer.hpp` an
/// include of this header and so of everything that mentions a configuration,
/// which is a heavy way to spell one constant and would put a sampler on the
/// include path of the command-line parser. Resolving it in one function instead
/// keeps this header inert data and keeps the parser, the validator and the
/// assembly from each having their own idea of what an unset scale radius means.
[[nodiscard]] core::Real resolved_scale_radius(const InitialConditionSettings& settings) noexcept;

} // namespace orrery::sim
