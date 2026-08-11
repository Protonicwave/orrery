/// \file
/// What the tree is worth on the GPU, and what the coherence is worth inside it.
///
/// Four questions, answered in one session so that they are answered on one
/// thermal state of one machine.
///
/// Does the divergence mitigation work? Section 7 of the implementation plan
/// asks for it to be measured rather than assumed, so the two traversals are
/// timed on the same trees over the same configurations, and the node visits
/// each one makes are reported beside the time each one takes. The two columns
/// move in opposite directions, which is the finding: the coherent walk steps
/// through more nodes and finishes sooner, because the nodes it adds are the
/// ones the hardware was already executing under a mask.
///
/// At what granularity? The sub-group width is what decides how many targets
/// agree to walk together, so it sets both how much redundancy the scheme buys
/// and how much divergence it removes. It is swept rather than argued about.
///
/// How far does this scale? The phase exists to establish the largest tractable
/// particle count on this machine, so the table runs until something stops it
/// and reports what that something is. It is not what a reader expects, and the
/// column that shows it is the share of each evaluation spent on the host.
///
/// And is it the right algorithm to be running? A GPU tree is only interesting
/// where it beats both the CPU tree of Phase 8 and the GPU direct kernel of
/// Phase 9, so both are timed here on the same configurations. Comparing a new
/// backend only against the thing it replaces is how a project convinces itself
/// of a result nobody else would accept.
///
/// Accuracy is measured against `solvers/reference_kernel.hpp` rather than
/// against another solver, for the reason Phase 7 built that reference: two
/// approximate answers agreeing says they are wrong in the same way.

#include <cstdio>
#include <exception>

#ifdef ORRERY_ENABLE_SYCL

#    include <algorithm>
#    include <array>
#    include <chrono>
#    include <cmath>
#    include <cstddef>
#    include <cstdint>
#    include <cstdlib>
#    include <fstream>
#    include <iomanip>
#    include <iostream>
#    include <memory>
#    include <string>
#    include <vector>

#    include "harness/machine_state.hpp"
#    include "harness/protocol.hpp"
#    include "harness/statistics.hpp"
#    include "orrery/backend/sycl_device.hpp"
#    include "orrery/backend/thread_pool.hpp"
#    include "orrery/backend/work_stealing_executor.hpp"
#    include "orrery/core/particle_data.hpp"
#    include "orrery/core/random.hpp"
#    include "orrery/core/softening.hpp"
#    include "orrery/core/types.hpp"
#    include "orrery/core/vec3.hpp"
#    include "orrery/initial_conditions/plummer.hpp"
#    include "orrery/solvers/barnes_hut_solver.hpp"
#    include "orrery/solvers/interaction_count.hpp"
#    include "orrery/solvers/octree.hpp"
#    include "orrery/solvers/reference_kernel.hpp"
#    include "orrery/solvers/sycl_direct_solver.hpp"
#    include "orrery/solvers/sycl_tree_solver.hpp"

namespace {

using orrery::backend::ThreadPool;
using orrery::backend::WorkStealingExecutor;
using orrery::benchmark::capture_machine_state;
using orrery::benchmark::Duration;
using orrery::benchmark::MachineState;
using orrery::benchmark::Protocol;
using orrery::benchmark::ThermalCanary;
using orrery::benchmark::TrialSet;
using orrery::core::Index;
using orrery::core::ParticleData;
using orrery::core::RandomSource;
using orrery::core::Real;
using orrery::core::Softening;
using orrery::core::Vec3;
using orrery::initial_conditions::make_plummer_sphere;
using orrery::initial_conditions::PlummerParameters;
using orrery::solvers::BarnesHutSolver;
using orrery::solvers::InteractionCount;
using orrery::solvers::reference_acceleration;
using orrery::solvers::ReferenceAcceleration;
using orrery::solvers::SyclDirectSolver;
using orrery::solvers::SyclTreeSolver;
using orrery::solvers::SyclTreeTimings;
using orrery::solvers::TreeParameters;
using orrery::solvers::TreeTraversal;

constexpr std::uint64_t kSeed = 20260811;

const Softening kSoftening{static_cast<Real>(0.05)};

/// The range of the scaling table.
///
/// The lower end is where a kernel launch costs more than the walk it performs,
/// which a reader needs to see in order to know not to run there. The upper end
/// is the phase's headline figure and is raised from the command line, because
/// establishing the largest tractable size means pushing until something stops
/// it rather than stopping where it was convenient.
constexpr Index kSmallest = 1024;
constexpr Index kLargest = 2097152;

/// Above this the CPU tree solver is not timed.
///
/// `docs/performance/barnes_hut.md` measures one evaluation at 262144 particles
/// at 0.87 seconds, so a row of trials there is already ten seconds and the next
/// size up would be a minute. The comparison this table exists to make is
/// settled two decades below the cut.
constexpr Index kLargestCpuTree = 262144;

/// Above this the GPU direct kernel is not timed.
///
/// `docs/performance/sycl_direct.md` measures 297 milliseconds at 131072
/// particles, and the cost is quadratic, so 262144 would be a second and a
/// quarter per evaluation to establish something the rows below it already show.
constexpr Index kLargestGpuDirect = 131072;

/// The sizes the two traversals are compared at.
///
/// Three, spanning the range over which the tree acquires structure. The
/// coherence has nothing to work with in a shallow tree and everything to work
/// with in a deep one, so a single size would report an accident.
constexpr std::array<Index, 3> kDivergenceSizes{16384, 131072, 1048576};

/// Where the sub-group width is swept.
constexpr Index kWidthSweepSize = 262144;

/// The configurations the error is measured on.
constexpr std::array<Index, 3> kAccuracySizes{4096, 16384, 65536};

/// How many particles the error is sampled over.
///
/// The compensated reference costs a double-precision division per pair, so
/// measuring every particle of a large configuration would dominate the session.
/// A fixed stride samples instead, which is unbiased because the particles are
/// in the order the sampler produced them and that order has nothing to do with
/// position.
constexpr Index kErrorSamples = 512;

/// A longer cool-down than the harness default, for the reason
/// `benchmarks/sycl_direct.cpp` gives: this session loads the GPU and all eight
/// CPU cores in turn on a part where both share one package and one power
/// budget, so each heats the other in a way no phase before Phase 9 produced.
const Protocol kProtocol{.cooldown = std::chrono::seconds(3)};

[[nodiscard]] double milliseconds(Duration duration) {
    return static_cast<double>(duration.count()) / 1e6;
}

[[nodiscard]] ParticleData sampled_sphere(Index count) {
    RandomSource random{kSeed};
    return make_plummer_sphere(PlummerParameters{.count = count}, random);
}

/// One size of the scaling table.
struct ScalingRow {
    Index particles{};
    TrialSet gpu_tree;
    TrialSet cpu_tree;
    TrialSet gpu_direct;
    SyclTreeTimings timings;
    InteractionCount count;
    std::uint64_t visits{};
    Index nodes{};
    unsigned depth{};
    bool cpu_tree_timed{false};
    bool gpu_direct_timed{false};
};

/// One comparison between the two traversals.
struct DivergenceRow {
    Index particles{};
    TrialSet independent;
    TrialSet coherent;
    Duration independent_kernel{};
    Duration coherent_kernel{};
    std::uint64_t independent_visits{};
    std::uint64_t coherent_visits{};
};

/// One point of the sub-group width sweep.
struct WidthRow {
    unsigned width{};
    TrialSet trials;
    Duration kernel{};
    std::uint64_t visits{};
};

struct AccuracyRow {
    Index particles{};
    double worst{};
    double root_mean_square{};
};

[[nodiscard]] AccuracyRow measure_error(const ParticleData& data, Index particles) {
    const Index stride = std::max<Index>(1, data.size() / kErrorSamples);

    AccuracyRow row;
    row.particles = particles;

    double total = 0;
    Index samples = 0;

    for (Index i = 0; i < data.size(); i += stride) {
        const ReferenceAcceleration exact =
            reference_acceleration(data.positions(), data.masses(), i, kSoftening);
        const Vec3 measured = data.accelerations().get(i);

        const double dx = static_cast<double>(measured.x) - exact.x;
        const double dy = static_cast<double>(measured.y) - exact.y;
        const double dz = static_cast<double>(measured.z) - exact.z;

        const double magnitude =
            std::sqrt((exact.x * exact.x) + (exact.y * exact.y) + (exact.z * exact.z));
        if (magnitude == 0) {
            continue;
        }

        const double error = std::sqrt((dx * dx) + (dy * dy) + (dz * dz)) / magnitude;
        row.worst = std::max(row.worst, error);
        total += error * error;
        ++samples;
    }

    if (samples > 0) {
        row.root_mean_square = std::sqrt(total / static_cast<double>(samples));
    }
    return row;
}

[[nodiscard]] ScalingRow measure_size(Index particles, SyclTreeSolver& gpu,
                                      SyclDirectSolver* direct, WorkStealingExecutor& executor,
                                      const Protocol& protocol) {
    ParticleData data = sampled_sphere(particles);

    ScalingRow row;
    row.particles = particles;

    gpu.select_traversal(TreeTraversal::kCoherent);
    gpu.select_sub_group_width(0);

    orrery::benchmark::cool_down(protocol);
    row.gpu_tree = orrery::benchmark::run_trials(
        protocol, [&] { gpu.evaluate(data.positions(), data.masses(), data.accelerations()); });

    // The split and the counters from one evaluation rather than from the whole
    // run, since the counters accumulate and the split is overwritten each time.
    gpu.reset_interaction_count();
    gpu.evaluate(data.positions(), data.masses(), data.accelerations());

    row.timings = gpu.timings();
    row.count = gpu.interaction_count();
    row.visits = gpu.node_visits();
    row.nodes = gpu.tree().nodes().size();
    row.depth = gpu.tree().depth();

    if (particles <= kLargestCpuTree) {
        BarnesHutSolver cpu{TreeParameters{}, kSoftening, executor};

        orrery::benchmark::cool_down(protocol);
        row.cpu_tree = orrery::benchmark::run_trials(
            protocol, [&] { cpu.evaluate(data.positions(), data.masses(), data.accelerations()); });
        row.cpu_tree_timed = true;
    }

    if (direct != nullptr && particles <= kLargestGpuDirect) {
        orrery::benchmark::cool_down(protocol);
        row.gpu_direct = orrery::benchmark::run_trials(protocol, [&] {
            direct->evaluate(data.positions(), data.masses(), data.accelerations());
        });
        row.gpu_direct_timed = true;
    }

    return row;
}

[[nodiscard]] DivergenceRow measure_divergence(Index particles, SyclTreeSolver& solver,
                                               const Protocol& protocol) {
    ParticleData data = sampled_sphere(particles);

    DivergenceRow row;
    row.particles = particles;

    solver.select_sub_group_width(0);

    for (const TreeTraversal traversal : {TreeTraversal::kIndependent, TreeTraversal::kCoherent}) {
        solver.select_traversal(traversal);

        orrery::benchmark::cool_down(protocol);
        const TrialSet trials = orrery::benchmark::run_trials(protocol, [&] {
            solver.evaluate(data.positions(), data.masses(), data.accelerations());
        });

        solver.reset_interaction_count();
        solver.evaluate(data.positions(), data.masses(), data.accelerations());

        if (traversal == TreeTraversal::kIndependent) {
            row.independent = trials;
            row.independent_kernel = solver.timings().kernel;
            row.independent_visits = solver.node_visits();
        } else {
            row.coherent = trials;
            row.coherent_kernel = solver.timings().kernel;
            row.coherent_visits = solver.node_visits();
        }
    }

    return row;
}

[[nodiscard]] std::vector<WidthRow> measure_widths(SyclTreeSolver& solver,
                                                   const Protocol& protocol) {
    ParticleData data = sampled_sphere(kWidthSweepSize);
    std::vector<WidthRow> rows;

    solver.select_traversal(TreeTraversal::kCoherent);

    // Zero first, which is the compiler's own choice and the configuration a run
    // uses. It is in the table so that the named widths can be read against the
    // default rather than only against each other.
    std::vector<unsigned> widths{0};
    for (const unsigned width : solver.supported_sub_group_widths()) {
        widths.push_back(width);
    }

    for (const unsigned width : widths) {
        solver.select_sub_group_width(width);

        WidthRow row;
        row.width = width;

        orrery::benchmark::cool_down(protocol);
        row.trials = orrery::benchmark::run_trials(protocol, [&] {
            solver.evaluate(data.positions(), data.masses(), data.accelerations());
        });

        solver.reset_interaction_count();
        solver.evaluate(data.positions(), data.masses(), data.accelerations());
        row.kernel = solver.timings().kernel;
        row.visits = solver.node_visits();

        rows.push_back(row);
    }

    solver.select_sub_group_width(0);
    return rows;
}

void print_device(const orrery::backend::DeviceDescription& device, const SyclTreeSolver& solver) {
    std::cout << "\ndevice\n"
              << "  name              " << device.name << '\n'
              << "  runtime           " << device.backend << '\n'
              << "  driver            " << device.driver_version << '\n'
              << "  compute units     " << device.compute_units << '\n'
              << "  sub-group width   " << device.sub_group_size << " by default\n"
              << "  widths offered    ";

    for (const unsigned width : solver.supported_sub_group_widths()) {
        std::cout << width << ' ';
    }

    std::cout << "\n  work-group        " << solver.work_group_size() << '\n'
              << "  shared USM        " << (device.supports_shared_usm ? "yes" : "no") << '\n';
}

void print_scaling(const std::vector<ScalingRow>& rows) {
    std::cout << "\nscaling of the GPU tree solver\n"
              << std::setw(10) << "N" << std::setw(12) << "total ms" << std::setw(9) << "spread"
              << std::setw(12) << "walk ms" << std::setw(10) << "host %" << std::setw(10)
              << "exponent" << std::setw(14) << "ms/(N log N)" << std::setw(11) << "cells/N"
              << std::setw(11) << "visits/N" << '\n'
              << std::string(99, '-') << '\n';

    for (std::size_t index = 0; index < rows.size(); ++index) {
        const ScalingRow& row = rows[index];
        const double total = milliseconds(row.gpu_tree.median());
        const double walk = milliseconds(row.timings.kernel);
        const auto count = static_cast<double>(row.particles);

        // Everything that is not the device traversal, which at these sizes is
        // the Morton sort and the tree build. The share is of one evaluation
        // rather than of the median trial, so it is a division of the work
        // rather than a second measurement of it.
        const double host =
            milliseconds(row.timings.ordering) + milliseconds(row.timings.gathering) +
            milliseconds(row.timings.construction) + milliseconds(row.timings.node_staging) +
            milliseconds(row.timings.scatter);

        double exponent = 0;
        if (index > 0) {
            const double previous = milliseconds(rows[index - 1].gpu_tree.median());
            const auto previous_count = static_cast<double>(rows[index - 1].particles);
            exponent = std::log(total / previous) / std::log(count / previous_count);
        }

        std::cout << std::setw(10) << row.particles << std::fixed << std::setprecision(3)
                  << std::setw(12) << total << std::setw(8) << std::setprecision(1)
                  << (row.gpu_tree.relative_spread() * 100.0) << "%" << std::setprecision(3)
                  << std::setw(12) << walk << std::setw(9) << std::setprecision(1)
                  << (host + walk > 0 ? 100.0 * host / (host + walk) : 0.0) << "%" << std::setw(10)
                  << std::setprecision(2) << (index > 0 ? exponent : 0.0) << std::setw(14)
                  << std::scientific << std::setprecision(3) << (total / (count * std::log2(count)))
                  << std::fixed << std::setw(11) << std::setprecision(0)
                  << (static_cast<double>(row.count.particle_cell) / count) << std::setw(11)
                  << (static_cast<double>(row.visits) / count) << '\n';
    }

    std::cout << "\nwalk ms is the device traversal alone. host % is everything else in one\n"
                 "evaluation: the Morton sort, the tree build, the gather into shared memory,\n"
                 "the node conversion and the scatter back out. visits/N is how many nodes\n"
                 "each work-item stepped through, which for the coherent walk is its whole\n"
                 "sub-group's union rather than its own walk.\n";
}

void print_time_split(const std::vector<ScalingRow>& rows) {
    std::cout << "\nwhere one evaluation goes\n"
              << std::setw(10) << "N" << std::setw(10) << "nodes" << std::setw(8) << "depth"
              << std::setw(10) << "sort %" << std::setw(11) << "gather %" << std::setw(10)
              << "build %" << std::setw(11) << "stage %" << std::setw(10) << "walk %"
              << std::setw(11) << "scatter %" << '\n'
              << std::string(91, '-') << '\n';

    for (const ScalingRow& row : rows) {
        const SyclTreeTimings& timings = row.timings;
        const auto total = static_cast<double>(
            timings.ordering.count() + timings.gathering.count() + timings.construction.count() +
            timings.node_staging.count() + timings.kernel.count() + timings.scatter.count());

        const auto share = [total](Duration part) {
            return total > 0 ? 100.0 * static_cast<double>(part.count()) / total : 0.0;
        };

        std::cout << std::setw(10) << row.particles << std::setw(10) << row.nodes << std::setw(8)
                  << row.depth << std::fixed << std::setprecision(1) << std::setw(9)
                  << share(timings.ordering) << "%" << std::setw(10) << share(timings.gathering)
                  << "%" << std::setw(9) << share(timings.construction) << "%" << std::setw(10)
                  << share(timings.node_staging) << "%" << std::setw(9) << share(timings.kernel)
                  << "%" << std::setw(10) << share(timings.scatter) << "%" << '\n';
    }
}

void print_against_the_alternatives(const std::vector<ScalingRow>& rows) {
    std::cout << "\nagainst the two solvers it has to beat to be worth having\n"
              << std::setw(10) << "N" << std::setw(14) << "GPU tree ms" << std::setw(14)
              << "CPU tree ms" << std::setw(16) << "GPU direct ms" << std::setw(13) << "vs CPU tree"
              << std::setw(15) << "vs GPU direct" << '\n'
              << std::string(82, '-') << '\n';

    for (const ScalingRow& row : rows) {
        const double tree = milliseconds(row.gpu_tree.median());

        std::cout << std::setw(10) << row.particles << std::fixed << std::setprecision(3)
                  << std::setw(14) << tree;

        if (row.cpu_tree_timed) {
            std::cout << std::setw(14) << milliseconds(row.cpu_tree.median());
        } else {
            std::cout << std::setw(14) << "-";
        }

        if (row.gpu_direct_timed) {
            std::cout << std::setw(16) << milliseconds(row.gpu_direct.median());
        } else {
            std::cout << std::setw(16) << "-";
        }

        std::cout << std::setprecision(2);

        if (row.cpu_tree_timed && tree > 0) {
            std::cout << std::setw(12) << (milliseconds(row.cpu_tree.median()) / tree) << "x";
        } else {
            std::cout << std::setw(13) << "-";
        }

        if (row.gpu_direct_timed && tree > 0) {
            std::cout << std::setw(14) << (milliseconds(row.gpu_direct.median()) / tree) << "x";
        } else {
            std::cout << std::setw(15) << "-";
        }

        std::cout << '\n';
    }

    std::cout << "\nall three totals include everything an evaluation does, so the tree\n"
                 "solvers are charged for their sort and their build and the direct solvers\n"
                 "for their staging.\n";
}

void print_divergence(const std::vector<DivergenceRow>& rows) {
    std::cout << "\nthe divergence mitigation, measured\n"
              << std::setw(10) << "N" << std::setw(15) << "independent" << std::setw(13)
              << "coherent" << std::setw(11) << "speedup" << std::setw(16) << "indep visits/N"
              << std::setw(16) << "coher visits/N" << std::setw(13) << "redundancy" << '\n'
              << std::string(94, '-') << '\n';

    for (const DivergenceRow& row : rows) {
        const double independent = milliseconds(row.independent_kernel);
        const double coherent = milliseconds(row.coherent_kernel);
        const auto count = static_cast<double>(row.particles);

        const auto independent_visits = static_cast<double>(row.independent_visits);
        const auto coherent_visits = static_cast<double>(row.coherent_visits);

        std::cout << std::setw(10) << row.particles << std::fixed << std::setprecision(3)
                  << std::setw(15) << independent << std::setw(13) << coherent << std::setw(10)
                  << std::setprecision(2) << (coherent > 0 ? independent / coherent : 0.0) << "x"
                  << std::setw(16) << std::setprecision(0) << (independent_visits / count)
                  << std::setw(16) << (coherent_visits / count) << std::setw(13)
                  << std::setprecision(2)
                  << (independent_visits > 0 ? coherent_visits / independent_visits : 0.0) << "x"
                  << '\n';
    }

    std::cout << "\nboth columns of times are the device traversal alone, since the host half\n"
                 "is identical between them. the visit columns are what each work-item\n"
                 "stepped through: its own walk for the independent traversal and its whole\n"
                 "sub-group's union for the coherent one, so redundancy is the extra walking\n"
                 "coherence costs and the speedup is what it buys.\n";
}

void print_widths(const std::vector<WidthRow>& rows, Index particles) {
    std::cout << "\nsub-group width, " << particles << " particles\n"
              << std::setw(10) << "width" << std::setw(13) << "walk ms" << std::setw(12)
              << "total ms" << std::setw(9) << "spread" << std::setw(13) << "visits/N" << '\n'
              << std::string(57, '-') << '\n';

    for (const WidthRow& row : rows) {
        const auto count = static_cast<double>(particles);

        if (row.width == 0) {
            std::cout << std::setw(10) << "default";
        } else {
            std::cout << std::setw(10) << row.width;
        }

        std::cout << std::fixed << std::setprecision(3) << std::setw(13) << milliseconds(row.kernel)
                  << std::setw(12) << milliseconds(row.trials.median()) << std::setw(8)
                  << std::setprecision(1) << (row.trials.relative_spread() * 100.0) << "%"
                  << std::setw(13) << std::setprecision(0)
                  << (static_cast<double>(row.visits) / count) << '\n';
    }

    std::cout << "\nthe width is the granularity of the coherence: how many targets agree to\n"
                 "walk the tree together. a narrower group visits fewer nodes it does not\n"
                 "need and shares the cost of each one over fewer lanes.\n";
}

void print_accuracy(const std::vector<AccuracyRow>& rows) {
    std::cout << "\naccuracy against the compensated double-precision reference\n"
              << std::setw(10) << "N" << std::setw(14) << "worst" << std::setw(14) << "rms" << '\n'
              << std::string(38, '-') << '\n';

    for (const AccuracyRow& row : rows) {
        std::cout << std::setw(10) << row.particles << std::scientific << std::setprecision(3)
                  << std::setw(14) << row.worst << std::setw(14) << row.root_mean_square
                  << std::defaultfloat << '\n';
    }

    std::cout << "\nthis is the opening angle's error rather than the device's. the GPU walk\n"
                 "and the CPU walk sum the same terms in the same order, which\n"
                 "tests/solvers/sycl_tree_solver_test.cpp requires by comparing their\n"
                 "interaction counters rather than their answers.\n";
}

/// What the largest row cost in shared memory, and what actually limits it.
void print_footprint(const ScalingRow& largest, const SyclTreeSolver& solver) {
    // Seven arrays of the scalar type and three of counters, sized to the padded
    // launch, plus the node array. Computed rather than queried because the
    // solver's allocations are private to it and the arithmetic is a property of
    // the design rather than of the run.
    const Index group = solver.work_group_size();
    const Index padded = ((largest.particles + group - 1) / group) * group;

    const double particle_bytes =
        static_cast<double>(padded) * ((7.0 * sizeof(Real)) + (3.0 * sizeof(std::uint32_t)));

    // A node is three coordinates, two scalars and three 32-bit indices.
    const double node_bytes =
        static_cast<double>(largest.nodes) * ((5.0 * sizeof(Real)) + (3.0 * 4.0));

    std::cout << "\nthe largest row\n"
              << "  particles         " << largest.particles << '\n'
              << "  tree nodes        " << largest.nodes << ", depth " << largest.depth << '\n'
              << "  shared memory     " << std::fixed << std::setprecision(1)
              << ((particle_bytes + node_bytes) / (1024.0 * 1024.0)) << " MiB, of which "
              << (node_bytes / (1024.0 * 1024.0)) << " MiB is the tree\n"
              << "  evaluation        " << std::setprecision(1)
              << milliseconds(largest.gpu_tree.median()) << " ms, of which "
              << milliseconds(largest.timings.kernel) << " ms is the device\n";
}

void write_csv(const std::string& path, const std::vector<ScalingRow>& rows) {
    std::ofstream file{path};
    if (!file) {
        std::cerr << "warning: could not write " << path << '\n';
        return;
    }

    file << "particles,gpu_tree_ms,spread,cpu_tree_ms,gpu_direct_ms,walk_ms,ordering_ms,"
            "gathering_ms,construction_ms,staging_ms,scatter_ms,nodes,depth,particle_particle,"
            "particle_cell,node_visits\n";

    for (const ScalingRow& row : rows) {
        file << row.particles << ',' << milliseconds(row.gpu_tree.median()) << ','
             << row.gpu_tree.relative_spread() << ',';

        if (row.cpu_tree_timed) {
            file << milliseconds(row.cpu_tree.median());
        }
        file << ',';

        if (row.gpu_direct_timed) {
            file << milliseconds(row.gpu_direct.median());
        }
        file << ',';

        file << milliseconds(row.timings.kernel) << ',' << milliseconds(row.timings.ordering) << ','
             << milliseconds(row.timings.gathering) << ',' << milliseconds(row.timings.construction)
             << ',' << milliseconds(row.timings.node_staging) << ','
             << milliseconds(row.timings.scatter) << ',' << row.nodes << ',' << row.depth << ','
             << row.count.particle_particle << ',' << row.count.particle_cell << ',' << row.visits
             << '\n';
    }

    std::cout << "wrote " << path << '\n';
}

void write_divergence_csv(const std::string& path, const std::vector<DivergenceRow>& rows) {
    std::ofstream file{path};
    if (!file) {
        std::cerr << "warning: could not write " << path << '\n';
        return;
    }

    file << "particles,independent_walk_ms,coherent_walk_ms,independent_visits,coherent_visits\n";

    for (const DivergenceRow& row : rows) {
        file << row.particles << ',' << milliseconds(row.independent_kernel) << ','
             << milliseconds(row.coherent_kernel) << ',' << row.independent_visits << ','
             << row.coherent_visits << '\n';
    }

    std::cout << "wrote " << path << '\n';
}

void print_canary(double across_tables, double across_session) {
    std::cout << "\nthermal canary\n"
              << "  across the scaling rows        " << std::fixed << std::setprecision(1)
              << (across_tables * 100.0) << " per cent slower\n"
              << "  across the whole session       " << (across_session * 100.0)
              << " per cent slower\n";
}

/// The largest size to measure up to, from the command line or the default.
///
/// Parsed here rather than in `main` because the bounds it is checked against
/// are the constants above, which do not exist in a build without the backend.
/// Returns zero when the arguments do not make sense, which the caller reports.
[[nodiscard]] Index parse_largest(int argc, char** argv) {
    if (argc <= 1) {
        return kLargest;
    }

    const Index largest = static_cast<Index>(std::strtoull(argv[1], nullptr, 10));
    return largest < kSmallest ? 0 : largest;
}

int run(int argc, char** argv) {
    const Index largest = parse_largest(argc, argv);
    if (largest == 0) {
        static_cast<void>(std::fputs("usage: sycl_tree [largest]\n", stderr));
        return 1;
    }

    const MachineState state = capture_machine_state();
    std::cout << "Orrery GPU tree traversal measurement\n\n";
    orrery::benchmark::print(std::cout, state);

    const std::unique_ptr<SyclTreeSolver> gpu =
        SyclTreeSolver::try_create(TreeParameters{}, kSoftening);
    if (gpu == nullptr) {
        std::cout << "\nno usable SYCL GPU on this machine, so there is nothing to measure.\n";
        return 0;
    }

    print_device(gpu->device(), *gpu);

    // The Phase 9 solver, for the comparison that says whether a tree is worth
    // building at all on this device. Absent is not a failure: the tables that
    // do not need it are still produced.
    const std::unique_ptr<SyclDirectSolver> direct = SyclDirectSolver::try_create(kSoftening);

    WorkStealingExecutor executor{ThreadPool::default_worker_count()};

    std::cout << "\nsizes:      " << kSmallest << " to " << largest << ", doubling\n"
              << "trials:     " << kProtocol.trials << " after a settling warm-up\n"
              << "softening:  " << kSoftening.length() << ", seed " << kSeed << '\n'
              << "parameters: opening angle " << gpu->parameters().opening_angle
              << ", leaf capacity " << gpu->parameters().leaf_capacity << ", monopole only\n";

    ThermalCanary canary;
    canary.mark();

    std::vector<ScalingRow> scaling;
    for (Index particles = kSmallest; particles <= largest; particles *= 2) {
        scaling.push_back(measure_size(particles, *gpu, direct.get(), executor, kProtocol));
        canary.mark();
    }

    print_scaling(scaling);
    print_time_split(scaling);
    print_against_the_alternatives(scaling);
    print_footprint(scaling.back(), *gpu);

    canary.mark();
    const double across_tables = canary.slowdown();

    std::vector<DivergenceRow> divergence;
    for (const Index particles : kDivergenceSizes) {
        if (particles > largest) {
            continue;
        }
        divergence.push_back(measure_divergence(particles, *gpu, kProtocol));
        canary.mark();
    }

    print_divergence(divergence);

    if (kWidthSweepSize <= largest) {
        print_widths(measure_widths(*gpu, kProtocol), kWidthSweepSize);
        canary.mark();
    }

    gpu->select_traversal(TreeTraversal::kCoherent);
    gpu->select_sub_group_width(0);

    std::vector<AccuracyRow> accuracy;
    for (const Index particles : kAccuracySizes) {
        ParticleData data = sampled_sphere(particles);
        gpu->evaluate(data.positions(), data.masses(), data.accelerations());
        accuracy.push_back(measure_error(data, particles));
    }

    print_accuracy(accuracy);

    write_csv("sycl_tree_scaling.csv", scaling);
    write_divergence_csv("sycl_tree_divergence.csv", divergence);

    canary.mark();
    print_canary(across_tables, canary.slowdown());

    return 0;
}

} // namespace

#else

namespace {

/// The same signature as the measurement above, so that `main` is one call
/// either way and no project type is named outside the guard that includes it.
int run(int argc, char** argv) {
    static_cast<void>(argc);
    static_cast<void>(argv);

    static_cast<void>(
        std::fputs("This build has no SYCL backend. Configure with ORRERY_ENABLE_SYCL=ON\n"
                   "and the oneAPI DPC++ compiler to measure the GPU.\n",
                   stdout));
    return 0;
}

} // namespace

#endif // ORRERY_ENABLE_SYCL

namespace {

/// Report a failure with no risk of throwing while doing it.
///
/// The same shape `tree_scaling.cpp` uses, and for the same reason: a handler
/// that formats through a stream may throw while reporting that something threw,
/// and main is the one place left with nowhere to put it.
void report_failure(const char* what) noexcept {
    static_cast<void>(std::fputs("measurement failed", stderr));

    if (what != nullptr) {
        static_cast<void>(std::fputs(": ", stderr));
        static_cast<void>(std::fputs(what, stderr));
    }

    static_cast<void>(std::fputs("\n", stderr));
}

} // namespace

int main(int argc, char** argv) {
    // Exceptions are permitted at this boundary and nowhere below it. A device
    // that disappears mid-session, which a driver reset does, arrives here as a
    // sycl::exception, and a benchmark that terminated on it would leave no
    // indication of which row it had reached.
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        report_failure(error.what());
        return 1;
    } catch (...) {
        report_failure(nullptr);
        return 1;
    }
}
