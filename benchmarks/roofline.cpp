/// \file
/// The measurements Phase 7 exists for.
///
/// Three questions, answered in one session so that they are answered on one
/// thermal state of one machine.
///
/// What are this machine's ceilings? Section 2 of the implementation plan
/// records the manufacturer's figures and says they are nominal. This program
/// measures the memory bandwidth and the floating-point throughput directly,
/// and after it has run those are the numbers the project quotes.
///
/// What does the direct kernel achieve against them? Every kernel is timed
/// scalar and vectorised, on one thread and on all of them, and each row states
/// the fraction of the relevant limit it reached. The relevant limit is not the
/// same for every kernel, which is the finding this program produces and
/// `docs/performance/roofline.md` explains.
///
/// And how much can any of it be trusted? Every figure carries the dispersion
/// of its trials and the drift across them, and the whole session is bracketed
/// by a thermal canary whose verdict is printed at the end.
///
/// ## The arithmetic of one interaction
///
/// Every rate below is formed from a count of floating-point operations, so the
/// count has to be stated rather than assumed. One pairwise interaction in
/// `src/solvers/direct_kernel_scalar.cpp` performs:
///
///     three subtractions for the separation vector          3
///     three multiplies and two adds for its square          5
///     one add for the softening                             1
///     one square root                                       1
///     one division                                          1
///     two multiplies for the cube of the reciprocal         2
///     one multiply by the mass                              1
///     three multiplies and three adds to accumulate         6
///                                                          --
///                                                          20
///
/// Twenty, counting the square root and the division as one each. That is the
/// convention the N-body literature uses and it is what makes this project's
/// figures comparable with published ones. It also understates the work: on
/// this part those two instructions are many times the cost of a multiply, and
/// the second arithmetic ceiling this program measures is there precisely
/// because of it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "harness/machine_limits.hpp"
#include "harness/machine_state.hpp"
#include "harness/protocol.hpp"
#include "harness/roofline_plot.hpp"
#include "harness/statistics.hpp"
#include "orrery/backend/executor.hpp"
#include "orrery/backend/serial_executor.hpp"
#include "orrery/backend/thread_pool.hpp"
#include "orrery/backend/work_stealing_executor.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/initial_conditions/plummer.hpp"
#include "orrery/solvers/direct_kernel.hpp"
#include "orrery/solvers/direct_solver.hpp"

namespace {

using orrery::backend::Executor;
using orrery::backend::SerialExecutor;
using orrery::backend::ThreadPool;
using orrery::backend::WorkStealingExecutor;
using orrery::benchmark::capture_machine_state;
using orrery::benchmark::Duration;
using orrery::benchmark::Limit;
using orrery::benchmark::MachineState;
using orrery::benchmark::measure_divide_and_sqrt_throughput;
using orrery::benchmark::measure_peak_throughput;
using orrery::benchmark::measure_read_bandwidth;
using orrery::benchmark::measure_triad_bandwidth;
using orrery::benchmark::Protocol;
using orrery::benchmark::RooflineCeilings;
using orrery::benchmark::RooflinePoint;
using orrery::benchmark::ThermalCanary;
using orrery::benchmark::TrialSet;
using orrery::core::Index;
using orrery::core::ParticleData;
using orrery::core::RandomSource;
using orrery::core::Real;
using orrery::core::Softening;
using orrery::initial_conditions::make_plummer_sphere;
using orrery::initial_conditions::PlummerParameters;
using orrery::solvers::DirectSolver;
using orrery::solvers::kernel_available;
using orrery::solvers::KernelKind;
using orrery::solvers::to_string;

constexpr std::uint64_t kSeed = 20260810;

/// Large enough that one evaluation is milliseconds rather than microseconds,
/// so that thread wake-up is a small correction and the clock's resolution is
/// irrelevant, and small enough that the whole table finishes in a few minutes.
///
/// It also puts the four arrays the kernel reads at 256 KiB apiece in double
/// precision, which is comfortably inside the level-two cache of either kind of
/// core. That is deliberate: the direct kernel is not a bandwidth-bound loop
/// and a size chosen to make it look like one would be measuring the wrong
/// thing. The arithmetic intensity reported below says why.
constexpr Index kDefaultParticles = 8192;

/// The softening a Plummer sphere run would use. It has no bearing on the
/// timings and is set so that the kernel does the arithmetic it would do in a
/// real run rather than a cheaper unsoftened variant.
const Softening kSoftening{static_cast<Real>(0.05)};

/// Floating-point operations in one pairwise interaction. Derived in the file
/// comment above.
constexpr double kFlopsPerInteraction = 20.0;

/// Square roots and divisions in one pairwise interaction, which is what the
/// second arithmetic ceiling is compared against.
constexpr double kSlowOperationsPerInteraction = 2.0;

/// One timed configuration of the direct kernel.
struct KernelRow {
    std::string kernel;
    std::string threading;
    unsigned workers{};
    TrialSet trials;

    double interactions{};

    [[nodiscard]] double flops() const {
        return orrery::benchmark::rate_per_second(interactions * kFlopsPerInteraction,
                                                  trials.median());
    }

    [[nodiscard]] double slow_operations() const {
        return orrery::benchmark::rate_per_second(interactions * kSlowOperationsPerInteraction,
                                                  trials.median());
    }
};

/// Compulsory memory traffic for one force evaluation, in bytes.
///
/// Four arrays are read, three coordinates and a mass, and three are written.
/// Each is read once from memory however many targets there are: the whole
/// configuration is a few hundred kilobytes at the sizes here and stays in
/// cache while every target streams over it. So the traffic that reaches memory
/// is compulsory traffic only, and it is what the arithmetic intensity should
/// be formed from.
///
/// Only the reads are counted, because the horizontal axis of a roofline is
/// conventionally operations per byte read and because the reads are what the
/// bandwidth ceiling here was measured against.
[[nodiscard]] double bytes_read_per_evaluation(Index particles) {
    return 4.0 * static_cast<double>(particles) * sizeof(Real);
}

[[nodiscard]] double interactions_per_evaluation(Index particles) {
    if (particles < 2) {
        return 0;
    }
    const auto count = static_cast<double>(particles);
    return count * (count - 1);
}

/// Time one kernel through one executor.
[[nodiscard]] KernelRow measure_kernel(ParticleData& data, Executor& executor, KernelKind kind,
                                       std::string threading, const Protocol& protocol) {
    DirectSolver solver{kSoftening, executor};
    solver.select_kernel(kind);

    const TrialSet trials = orrery::benchmark::run_trials(
        protocol, [&] { solver.evaluate(data.positions(), data.masses(), data.accelerations()); });

    // The kernel the solver settled on rather than the one that was asked for.
    // A row headed avx2 that had quietly run the scalar kernel would be the
    // single most misleading thing this program could print.
    return KernelRow{.kernel = std::string{to_string(solver.kernel())},
                     .threading = std::move(threading),
                     .workers = executor.worker_count(),
                     .trials = trials,
                     .interactions = interactions_per_evaluation(data.size())};
}

/// A rate in engineering units, so that a table of bytes per second and one of
/// operations per second read the same way.
[[nodiscard]] std::string scaled(double value) {
    static constexpr std::array<const char*, 5> kSuffixes{"", "k", "M", "G", "T"};

    std::size_t suffix = 0;
    double magnitude = value;
    while (magnitude >= 1000.0 && suffix + 1 < kSuffixes.size()) {
        magnitude /= 1000.0;
        ++suffix;
    }

    std::ostringstream text;
    text << std::fixed << std::setprecision(2) << magnitude << ' ' << kSuffixes[suffix];
    return text.str();
}

void print_limit(const Limit& limit) {
    std::cout << std::setw(24) << limit.name << std::setw(14) << scaled(limit.rate()) << limit.unit
              << "/s" << std::setw(12) << std::fixed << std::setprecision(1)
              << (limit.trials.relative_spread() * 100.0) << "%" << std::setw(11)
              << (limit.trials.drift() * 100.0) << "%" << std::setw(16) << scaled(limit.best_rate())
              << limit.unit << "/s\n";
}

void print_limits(const std::vector<Limit>& limits) {
    std::cout << "\nmeasured ceilings\n"
              << std::setw(24) << "probe" << std::setw(20) << "sustained" << std::setw(12)
              << "spread" << std::setw(11) << "drift" << std::setw(22) << "best of the trials"
              << '\n'
              << std::string(89, '-') << '\n';

    for (const Limit& limit : limits) {
        print_limit(limit);
    }

    std::cout << "\nsustained is the median trial and is the figure this project quotes.\n"
                 "best is the fastest trial, which is what a best-of convention would\n"
                 "report; the difference between the two columns is why this project\n"
                 "does not use one. spread is the interquartile range over the median,\n"
                 "drift is how much slower the second half of the trials was than the\n"
                 "first.\n";
}

void print_kernels(const std::vector<KernelRow>& rows, const RooflineCeilings& ceilings,
                   double slow_operation_ceiling, Index particles) {
    std::cout << "\ndirect kernel, " << particles << " particles, " << std::fixed
              << std::setprecision(0) << interactions_per_evaluation(particles)
              << " interactions per evaluation\n"
              << std::setw(8) << "kernel" << std::setw(10) << "threads" << std::setw(12)
              << "median ms" << std::setw(9) << "spread" << std::setw(9) << "drift" << std::setw(14)
              << "flop/s" << std::setw(11) << "of peak" << std::setw(15) << "sqrt+div/s"
              << std::setw(12) << "of that" << '\n'
              << std::string(100, '-') << '\n';

    for (const KernelRow& row : rows) {
        const double milliseconds = static_cast<double>(row.trials.median().count()) / 1e6;

        std::cout << std::setw(8) << row.kernel << std::setw(10) << row.workers << std::setw(12)
                  << std::fixed << std::setprecision(3) << milliseconds << std::setw(8)
                  << std::setprecision(1) << (row.trials.relative_spread() * 100.0) << "%"
                  << std::setw(8) << (row.trials.drift() * 100.0) << "%" << std::setw(12)
                  << scaled(row.flops()) << std::setw(10) << std::setprecision(1)
                  << (ceilings.peak > 0 ? row.flops() / ceilings.peak * 100.0 : 0.0) << "%"
                  << std::setw(13) << scaled(row.slow_operations()) << std::setw(11)
                  << (slow_operation_ceiling > 0
                          ? row.slow_operations() / slow_operation_ceiling * 100.0
                          : 0.0)
                  << "%" << '\n';
    }

    std::cout << "\n'of peak' is against the fused multiply-add ceiling, which is the\n"
                 "number every published peak figure quotes. 'of that' is against the\n"
                 "divide and square root ceiling, which is the one this kernel is\n"
                 "actually competing for: it performs two such operations in every\n"
                 "twenty floating-point operations, and they are not interchangeable\n"
                 "with the other eighteen.\n";
}

/// Every canary mark, against the first.
///
/// The endpoint ratio on its own says how much the machine slowed down but not
/// when, and those are different findings. A slowdown that arrives all at once
/// after the first heavy configuration is a part hitting its sustained power
/// limit; one that accumulates evenly across the session is a heatsink filling
/// up. The trace also says which rows above were measured on which machine,
/// since a row taken between two marks was taken at a clock between them.
void print_canary(const ThermalCanary& canary) {
    const std::vector<Duration>& marks = canary.history().in_order();

    std::cout << "\nthermal canary, a fixed block of arithmetic on one thread touching no\n"
                 "memory, so that nothing but the clock can change its duration:\n"
              << std::setw(8) << "mark" << std::setw(12) << "ms" << std::setw(20)
              << "against the first" << '\n'
              << std::string(40, '-') << '\n';

    const double first = marks.empty() ? 0.0 : static_cast<double>(marks.front().count());

    for (std::size_t mark = 0; mark < marks.size(); ++mark) {
        const double milliseconds = static_cast<double>(marks[mark].count()) / 1e6;
        const double ratio = first > 0 ? static_cast<double>(marks[mark].count()) / first : 0.0;

        std::cout << std::setw(8) << mark << std::setw(12) << std::fixed << std::setprecision(3)
                  << milliseconds << std::setw(15) << std::setprecision(2) << ratio << "x" << '\n';
    }

    std::cout << "\nthe machine ran " << std::fixed << std::setprecision(1)
              << (canary.slowdown() * 100.0)
              << "% slower at the end of this session than at the start.\n"
                 "Rows above were measured in the order printed, so an increasing trace\n"
                 "means the later rows were measured on a slower machine than the earlier\n"
                 "ones and the comparison between them understates the later ones.\n";
}

void write_plot(const std::string& path, const std::string& title, const RooflineCeilings& ceilings,
                const std::vector<RooflinePoint>& points) {
    std::ofstream file{path};
    if (!file) {
        std::cerr << "warning: could not write " << path << '\n';
        return;
    }

    if (path.ends_with(".csv")) {
        write_roofline_csv(file, ceilings, points);
    } else {
        write_roofline_svg(file, title, ceilings, points);
    }

    std::cout << "wrote " << path << '\n';
}

void run(Index particles, int trials) {
    const Protocol protocol{.trials = trials};

    const MachineState state = capture_machine_state();
    std::cout << "Orrery roofline measurement\n\n";
    print(std::cout, state);
    std::cout << "particles:  " << particles << ", trials: " << protocol.trials << " after "
              << protocol.warmup << " discarded\n"
              << "probes:     " << orrery::benchmark::throughput_probe_name() << '\n';

    ThermalCanary canary;
    canary.mark();

    // One pool for the whole session. Creating one per configuration would put
    // thread creation inside the measurement and would give each row a
    // different set of threads to be scheduled.
    WorkStealingExecutor parallel{ThreadPool::default_worker_count()};
    SerialExecutor serial;

    std::vector<Limit> limits;

    orrery::benchmark::cool_down(protocol);
    limits.push_back(measure_read_bandwidth(parallel, protocol));
    canary.mark();

    orrery::benchmark::cool_down(protocol);
    limits.push_back(measure_triad_bandwidth(parallel, protocol));
    canary.mark();

    orrery::benchmark::cool_down(protocol);
    limits.push_back(measure_peak_throughput(parallel, protocol));
    canary.mark();

    orrery::benchmark::cool_down(protocol);
    limits.push_back(measure_divide_and_sqrt_throughput(parallel, protocol));
    canary.mark();

    print_limits(limits);

    RandomSource random{kSeed};
    ParticleData data = make_plummer_sphere(PlummerParameters{.count = particles}, random);

    std::vector<KernelRow> rows;

    // Every kernel on one thread and on the whole machine. The single-threaded
    // rows are what isolate vectorisation from threading: a speedup measured
    // only at full width could be either.
    for (const KernelKind kind : {KernelKind::kScalar, KernelKind::kAvx2}) {
        if (!kernel_available(kind)) {
            continue;
        }

        orrery::benchmark::cool_down(protocol);
        rows.push_back(measure_kernel(data, serial, kind, "serial", protocol));
        canary.mark();

        orrery::benchmark::cool_down(protocol);
        rows.push_back(measure_kernel(data, parallel, kind, "stealing", protocol));
        canary.mark();
    }

    const double slow_operation_ceiling = limits[3].rate();

    // The second flat roof, converted into the units of the vertical axis. The
    // kernel performs one square root and one division in every twenty
    // floating-point operations, so a machine retiring S of them per second can
    // sustain at most 10 S flop/s of *this* kernel however good the code is.
    // That is a statement about the algorithm's instruction mix rather than
    // about the implementation, which is why it is drawn as a ceiling and not
    // as a target.
    const RooflineCeilings ceilings{
        .bandwidth = limits[0].rate(),
        .peak = limits[2].rate(),
        .restricted =
            slow_operation_ceiling * (kFlopsPerInteraction / kSlowOperationsPerInteraction),
        .restricted_label = "divide and square root limit for this mix"};

    print_kernels(rows, ceilings, slow_operation_ceiling, particles);

    // Only the full-width rows go on the plot. A single-threaded point placed
    // under a ceiling measured across eight cores would be reporting that one
    // core is not eight, which is true and not worth drawing.
    const double intensity = interactions_per_evaluation(particles) * kFlopsPerInteraction /
                             bytes_read_per_evaluation(particles);

    std::vector<RooflinePoint> points;
    for (const KernelRow& row : rows) {
        if (row.workers > 1) {
            points.push_back(RooflinePoint{.label = row.kernel + ", all cores",
                                           .arithmetic_intensity = intensity,
                                           .achieved = row.flops()});
        }
    }

    const std::string title = "Orrery direct solver against measured limits, " +
                              std::to_string(particles) + " particles, " + state.scalar;

    write_plot("roofline.svg", title, ceilings, points);
    write_plot("roofline.csv", title, ceilings, points);

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
        const Index particles =
            argc > 1 ? static_cast<Index>(std::strtoull(argv[1], nullptr, 10)) : kDefaultParticles;
        const int trials = argc > 2 ? static_cast<int>(std::strtol(argv[2], nullptr, 10)) : 11;

        if (particles < 2 || trials <= 0) {
            std::cerr << "usage: roofline [particles] [trials]\n";
            return 1;
        }

        run(particles, trials);
    } catch (const std::exception& error) {
        report_failure(error.what());
        return 1;
    } catch (...) {
        report_failure(nullptr);
        return 1;
    }

    return 0;
}
