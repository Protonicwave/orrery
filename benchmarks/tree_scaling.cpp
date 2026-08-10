/// \file
/// What the tree bought, measured against the algorithm it replaces.
///
/// Three questions, answered in one session so that they are answered on one
/// thermal state of one machine.
///
/// How does the tree solver scale? Section 7 of the implementation plan asks
/// for N log N to be shown empirically over at least two decades of N, so the
/// table below covers 1024 to 262144 particles, which is two and a half. What
/// is printed for each size is the time per evaluation, the exponent implied by
/// the step from the previous size, and the time divided by N log N, which is
/// the quantity that should be flat if the claim holds.
///
/// Where does it overtake direct summation? Both solvers are timed on the same
/// configurations with the same kernel, the same executor and the same
/// softening, so the crossover is a property of the algorithms rather than of
/// two different implementations. Direct summation stops being timed above a
/// size where one evaluation takes long enough to dominate the session; the
/// point of the comparison is the crossover, and it is far below that.
///
/// And what does the approximation cost in accuracy? The last table sweeps the
/// opening angle with and without quadrupole moments, and reports the error
/// against `solvers/reference_kernel.hpp` beside the time. That is the error
/// against cost curve the phase exists to produce: every row is a way of
/// buying accuracy, and the question is which way is cheaper on this machine.
///
/// Errors are measured against the compensated reference rather than against
/// the direct solver, for the reason Phase 7 built that reference: the direct
/// solver has its own rounding error, and a comparison between two approximate
/// answers cannot say which of them is wrong.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "harness/machine_state.hpp"
#include "harness/protocol.hpp"
#include "harness/statistics.hpp"
#include "orrery/backend/executor.hpp"
#include "orrery/backend/thread_pool.hpp"
#include "orrery/backend/work_stealing_executor.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/initial_conditions/plummer.hpp"
#include "orrery/solvers/barnes_hut_solver.hpp"
#include "orrery/solvers/direct_solver.hpp"
#include "orrery/solvers/interaction_count.hpp"
#include "orrery/solvers/octree.hpp"
#include "orrery/solvers/reference_kernel.hpp"

namespace {

using orrery::backend::Executor;
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
using orrery::solvers::DirectSolver;
using orrery::solvers::EvaluationTimings;
using orrery::solvers::InteractionCount;
using orrery::solvers::reference_acceleration;
using orrery::solvers::ReferenceAcceleration;
using orrery::solvers::TreeParameters;

constexpr std::uint64_t kSeed = 20260810;

/// The softening a Plummer sphere run would use, applied to every solver here
/// so that none of them is timed on cheaper arithmetic than the others.
const Softening kSoftening{static_cast<Real>(0.05)};

/// The smallest and largest configurations in the scaling table.
///
/// Two and a half decades, which is what the implementation plan asks for. The
/// lower end is small enough that a tree has barely any structure to exploit
/// and the upper end is where the machine's memory starts to matter, so the
/// range brackets the regime this project actually runs in.
constexpr Index kSmallest = 1024;
constexpr Index kLargest = 262144;

/// Above this, direct summation is not timed.
///
/// One evaluation over 65536 particles is about four billion interactions and
/// takes the better part of a second on this machine, so a row of eleven trials
/// is ten seconds. Two sizes further up it would be three minutes, and the
/// session would be measuring a thermal state rather than an algorithm. The
/// crossover this table exists to find is two decades below the cut.
constexpr Index kLargestDirect = 65536;

/// The configuration the accuracy sweep is measured on.
constexpr Index kAccuracyParticles = 16384;

/// How many particles the error is measured over.
///
/// The compensated reference is O(N) per particle, so measuring every particle
/// of a 16384-particle configuration is a quarter of a billion compensated
/// terms per row and would dominate the session. A fixed stride samples the
/// configuration instead, which is unbiased here because the particles are in
/// the order the sampler produced them and that order has nothing to do with
/// position.
constexpr Index kErrorSamples = 1024;

/// One size in the scaling table.
struct ScalingRow {
    Index particles{};
    TrialSet tree;
    TrialSet direct;
    InteractionCount count;
    EvaluationTimings timings;
    Index nodes{};
    Index leaves{};
    unsigned depth{};
};

/// One point of the error against cost curve.
struct AccuracyRow {
    Real angle{};
    bool quadrupole{};
    TrialSet trials;
    InteractionCount count;
    double worst{};
    double root_mean_square{};
};

[[nodiscard]] ParticleData sampled_sphere(Index count) {
    RandomSource random{kSeed};

    return make_plummer_sphere(PlummerParameters{.count = count}, random);
}

[[nodiscard]] double milliseconds(Duration duration) {
    return static_cast<double>(duration.count()) / 1e6;
}

/// The error of the accelerations in `data`, against the compensated reference.
[[nodiscard]] AccuracyRow measure_error(const ParticleData& data, AccuracyRow row) {
    const Index stride = std::max<Index>(1, data.size() / kErrorSamples);

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
        const double error = std::sqrt((dx * dx) + (dy * dy) + (dz * dz)) / magnitude;

        row.worst = std::max(row.worst, error);
        total += error * error;
        ++samples;
    }

    row.root_mean_square = std::sqrt(total / static_cast<double>(samples));

    return row;
}

[[nodiscard]] ScalingRow measure_size(Index particles, Executor& executor,
                                      const Protocol& protocol) {
    ParticleData data = sampled_sphere(particles);

    ScalingRow row;
    row.particles = particles;

    BarnesHutSolver tree{TreeParameters{}, kSoftening, executor};

    orrery::benchmark::cool_down(protocol);
    row.tree = orrery::benchmark::run_trials(
        protocol, [&] { tree.evaluate(data.positions(), data.masses(), data.accelerations()); });

    // The counts and the tree's shape from one evaluation rather than from the
    // whole run, since a counter accumulated across trials would report the
    // work of however many the settling loop happened to discard as well.
    tree.reset_interaction_count();
    tree.evaluate(data.positions(), data.masses(), data.accelerations());

    row.count = tree.interaction_count();
    row.timings = tree.timings();
    row.nodes = tree.tree().nodes().size();
    row.leaves = tree.tree().leaf_count();
    row.depth = tree.tree().depth();

    if (particles <= kLargestDirect) {
        DirectSolver direct{kSoftening, executor};

        orrery::benchmark::cool_down(protocol);
        row.direct = orrery::benchmark::run_trials(protocol, [&] {
            direct.evaluate(data.positions(), data.masses(), data.accelerations());
        });
    }

    return row;
}

[[nodiscard]] AccuracyRow measure_angle(ParticleData& data, Real angle, bool quadrupole,
                                        Executor& executor, const Protocol& protocol) {
    BarnesHutSolver solver{TreeParameters{.opening_angle = angle, .quadrupole = quadrupole},
                           kSoftening, executor};

    orrery::benchmark::cool_down(protocol);
    const TrialSet trials = orrery::benchmark::run_trials(
        protocol, [&] { solver.evaluate(data.positions(), data.masses(), data.accelerations()); });

    solver.reset_interaction_count();
    solver.evaluate(data.positions(), data.masses(), data.accelerations());

    return measure_error(data, AccuracyRow{.angle = angle,
                                           .quadrupole = quadrupole,
                                           .trials = trials,
                                           .count = solver.interaction_count()});
}

void print_scaling(const std::vector<ScalingRow>& rows) {
    std::cout << "\nscaling, Barnes-Hut against direct summation, all cores\n"
              << std::setw(9) << "N" << std::setw(12) << "tree ms" << std::setw(9) << "spread"
              << std::setw(13) << "direct ms" << std::setw(10) << "speedup" << std::setw(10)
              << "exponent" << std::setw(14) << "ms/(N log N)" << std::setw(12) << "pairs/N"
              << std::setw(11) << "cells/N" << '\n'
              << std::string(100, '-') << '\n';

    for (std::size_t index = 0; index < rows.size(); ++index) {
        const ScalingRow& row = rows[index];
        const double tree = milliseconds(row.tree.median());
        const auto count = static_cast<double>(row.particles);

        // The exponent implied by the step from the previous size. A pure
        // N log N cost gives a little over one and a pure N^2 gives two, and
        // the difference between those is the whole claim of this phase.
        double exponent = 0;
        if (index > 0) {
            const double previous = milliseconds(rows[index - 1].tree.median());
            const auto previous_count = static_cast<double>(rows[index - 1].particles);
            exponent = std::log(tree / previous) / std::log(count / previous_count);
        }

        const double normalised = tree / (count * std::log2(count));

        std::cout << std::setw(9) << row.particles << std::setw(12) << std::fixed
                  << std::setprecision(3) << tree << std::setw(8) << std::setprecision(1)
                  << (row.tree.relative_spread() * 100.0) << "%";

        if (row.direct.empty()) {
            std::cout << std::setw(13) << "-" << std::setw(10) << "-";
        } else {
            const double direct = milliseconds(row.direct.median());
            std::cout << std::setw(13) << std::setprecision(3) << direct << std::setw(9)
                      << std::setprecision(2) << (direct / tree) << "x";
        }

        std::cout << std::setw(10) << std::setprecision(2) << (index > 0 ? exponent : 0.0)
                  << std::setw(14) << std::scientific << std::setprecision(3) << normalised
                  << std::fixed << std::setw(12) << std::setprecision(0)
                  << (static_cast<double>(row.count.particle_particle) / count) << std::setw(11)
                  << (static_cast<double>(row.count.particle_cell) / count) << '\n';
    }

    std::cout << "\nexponent is the local slope of the timing against N, from the row above.\n"
                 "ms/(N log N) is the same timing divided by the cost the method claims,\n"
                 "and is the column to read: a figure that is flat down the table is the\n"
                 "claim holding. pairs/N and cells/N are the two kinds of interaction per\n"
                 "particle, which are properties of the algorithm rather than the machine.\n";
}

void print_tree_shape(const std::vector<ScalingRow>& rows) {
    std::cout << "\nwhere the time goes, and what the tree looks like\n"
              << std::setw(9) << "N" << std::setw(10) << "nodes" << std::setw(10) << "leaves"
              << std::setw(9) << "depth" << std::setw(12) << "sort %" << std::setw(12) << "gather %"
              << std::setw(12) << "build %" << std::setw(12) << "walk %" << '\n'
              << std::string(86, '-') << '\n';

    for (const ScalingRow& row : rows) {
        const auto total =
            static_cast<double>(row.timings.ordering.count() + row.timings.gathering.count() +
                                row.timings.construction.count() + row.timings.traversal.count());

        const auto share = [total](Duration part) {
            return total > 0 ? 100.0 * static_cast<double>(part.count()) / total : 0.0;
        };

        std::cout << std::setw(9) << row.particles << std::setw(10) << row.nodes << std::setw(10)
                  << row.leaves << std::setw(9) << row.depth << std::setw(11) << std::fixed
                  << std::setprecision(1) << share(row.timings.ordering) << "%" << std::setw(11)
                  << share(row.timings.gathering) << "%" << std::setw(11)
                  << share(row.timings.construction) << "%" << std::setw(11)
                  << share(row.timings.traversal) << "%" << '\n';
    }

    std::cout << "\nthe four shares are of one evaluation rather than of the median trial,\n"
                 "so they are a division of the work and not a second measurement of it.\n";
}

void print_crossover(const std::vector<ScalingRow>& rows) {
    std::cout << "\ncrossover\n" << std::string(60, '-') << '\n';

    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (rows[index].direct.empty()) {
            continue;
        }

        const double tree = milliseconds(rows[index].tree.median());
        const double direct = milliseconds(rows[index].direct.median());

        if (direct <= tree) {
            continue;
        }

        std::cout << "the tree first wins at N = " << rows[index].particles << ", by " << std::fixed
                  << std::setprecision(2) << (direct / tree) << " times.\n";

        if (index > 0) {
            std::cout << "at N = " << rows[index - 1].particles << " direct summation was still "
                      << std::setprecision(2)
                      << (milliseconds(rows[index - 1].tree.median()) /
                          milliseconds(rows[index - 1].direct.median()))
                      << " times faster, so the crossover is between the two.\n";
        }

        return;
    }

    std::cout << "no crossover in the sizes measured.\n";
}

void print_accuracy(const std::vector<AccuracyRow>& rows, Index particles) {
    std::cout << "\nerror against cost, " << particles << " particles, all cores\n"
              << std::setw(8) << "theta" << std::setw(13) << "quadrupole" << std::setw(12)
              << "median ms" << std::setw(9) << "spread" << std::setw(14) << "worst error"
              << std::setw(14) << "rms error" << std::setw(12) << "pairs/N" << std::setw(11)
              << "cells/N" << '\n'
              << std::string(93, '-') << '\n';

    for (const AccuracyRow& row : rows) {
        const auto count = static_cast<double>(particles);

        std::cout << std::setw(8) << std::fixed << std::setprecision(2) << row.angle
                  << std::setw(13) << (row.quadrupole ? "yes" : "no") << std::setw(12)
                  << std::setprecision(3) << milliseconds(row.trials.median()) << std::setw(8)
                  << std::setprecision(1) << (row.trials.relative_spread() * 100.0) << "%"
                  << std::setw(14) << std::scientific << std::setprecision(2) << row.worst
                  << std::setw(14) << row.root_mean_square << std::fixed << std::setw(12)
                  << std::setprecision(0)
                  << (static_cast<double>(row.count.particle_particle) / count) << std::setw(11)
                  << (static_cast<double>(row.count.particle_cell) / count) << '\n';
    }

    std::cout << "\nerrors are against the compensated reference of Phase 7, over a sample\n"
                 "of the configuration. worst is the largest relative error on any sampled\n"
                 "particle and rms is the root mean square over them, and the two differ by\n"
                 "an order of magnitude because the halo is approximated far worse than the\n"
                 "core.\n";
}

void write_scaling_csv(const std::string& path, const std::vector<ScalingRow>& rows) {
    std::ofstream file{path};
    if (!file) {
        std::cerr << "warning: could not write " << path << '\n';
        return;
    }

    file << "particles,tree_ms,tree_spread,direct_ms,nodes,leaves,depth,particle_particle,"
            "particle_cell\n";

    for (const ScalingRow& row : rows) {
        file << row.particles << ',' << milliseconds(row.tree.median()) << ','
             << row.tree.relative_spread() << ',';

        if (row.direct.empty()) {
            file << ',';
        } else {
            file << milliseconds(row.direct.median()) << ',';
        }

        file << row.nodes << ',' << row.leaves << ',' << row.depth << ','
             << row.count.particle_particle << ',' << row.count.particle_cell << '\n';
    }

    std::cout << "wrote " << path << '\n';
}

void write_accuracy_csv(const std::string& path, const std::vector<AccuracyRow>& rows) {
    std::ofstream file{path};
    if (!file) {
        std::cerr << "warning: could not write " << path << '\n';
        return;
    }

    file << "opening_angle,quadrupole,median_ms,spread,worst_error,rms_error,particle_particle,"
            "particle_cell\n";

    for (const AccuracyRow& row : rows) {
        file << row.angle << ',' << (row.quadrupole ? 1 : 0) << ','
             << milliseconds(row.trials.median()) << ',' << row.trials.relative_spread() << ','
             << row.worst << ',' << row.root_mean_square << ',' << row.count.particle_particle
             << ',' << row.count.particle_cell << '\n';
    }

    std::cout << "wrote " << path << '\n';
}

void print_canary(const ThermalCanary& canary) {
    std::cout << "\nthe machine ran " << std::fixed << std::setprecision(1)
              << (canary.slowdown() * 100.0)
              << "% slower at the end of this session than at the start.\n"
                 "Rows were measured in the order printed, so an increasing figure means\n"
                 "the later rows were measured on a slower machine than the earlier ones.\n";
}

void run(Index largest, int trials) {
    const Protocol protocol{.trials = trials};

    const MachineState state = capture_machine_state();
    std::cout << "Orrery tree solver measurement\n\n";
    print(std::cout, state);
    std::cout << "sizes:      " << kSmallest << " to " << largest << ", doubling\n"
              << "trials:     " << protocol.trials << " after a settling warm-up\n"
              << "softening:  " << kSoftening.length() << ", seed " << kSeed << '\n';

    ThermalCanary canary;
    canary.mark();

    // One pool for the whole session, so that no row pays for thread creation
    // and every row is scheduled across the same threads.
    WorkStealingExecutor executor{ThreadPool::default_worker_count()};

    std::vector<ScalingRow> rows;

    for (Index particles = kSmallest; particles <= largest; particles *= 2) {
        rows.push_back(measure_size(particles, executor, protocol));
        canary.mark();
    }

    print_scaling(rows);
    print_tree_shape(rows);
    print_crossover(rows);

    // The accuracy sweep, on one configuration, so that the only thing changing
    // down the table is the approximation.
    ParticleData data = sampled_sphere(kAccuracyParticles);

    static constexpr std::array<Real, 5> kAngles{static_cast<Real>(0.2), static_cast<Real>(0.4),
                                                 static_cast<Real>(0.5), static_cast<Real>(0.7),
                                                 static_cast<Real>(1.0)};

    std::vector<AccuracyRow> accuracy;

    for (const bool quadrupole : {false, true}) {
        for (const Real angle : kAngles) {
            accuracy.push_back(measure_angle(data, angle, quadrupole, executor, protocol));
            canary.mark();
        }
    }

    print_accuracy(accuracy, kAccuracyParticles);

    write_scaling_csv("tree_scaling.csv", rows);
    write_accuracy_csv("tree_accuracy.csv", accuracy);

    canary.mark();
    print_canary(canary);
}

/// Report a failure with no risk of throwing while doing it.
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
    try {
        const Index largest =
            argc > 1 ? static_cast<Index>(std::strtoull(argv[1], nullptr, 10)) : kLargest;
        const int trials = argc > 2 ? static_cast<int>(std::strtol(argv[2], nullptr, 10)) : 11;

        if (largest < kSmallest || trials <= 0) {
            std::cerr << "usage: tree_scaling [largest] [trials]\n";
            return 1;
        }

        run(largest, trials);
    } catch (const std::exception& error) {
        report_failure(error.what());
        return 1;
    } catch (...) {
        report_failure(nullptr);
        return 1;
    }

    return 0;
}
