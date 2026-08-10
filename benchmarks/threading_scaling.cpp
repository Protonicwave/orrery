/// \file
/// The measurements Phase 6 reports: what threading bought, and why.
///
/// This program answers the two questions section 7 of the implementation plan
/// asks of this phase. How much faster is the direct solver on eight threads
/// than on one, and how much of the available thread time does each partitioning
/// scheme leave unused. The second explains the first, which is the whole reason
/// the scheduler carries per-worker instrumentation.
///
/// ## The baseline has to be pinned
///
/// The first version of this program measured its single-threaded baseline on
/// whichever core Windows happened to have put the main thread on, and produced
/// speedups above ten on an eight-core machine. The cause is the hardware this
/// project targets: a thread left to the operating system may run on a
/// performance core or on an efficiency core, and on this part those differ by
/// about a factor of two. A baseline taken on an efficiency core flatters every
/// figure computed against it, and it does so silently, because the number that
/// comes out is merely large rather than obviously wrong.
///
/// So the baseline is measured twice, once pinned to a performance core and once
/// pinned to an efficiency core. The ratio between them is the most useful
/// single number this program produces: it is the hardware asymmetry that makes
/// equal static shares the wrong partition, quantified directly rather than
/// inferred from the scheduling results it goes on to explain.
///
/// ## What this is not
///
/// It is not the benchmark harness. That is Phase 7's deliverable, and it brings
/// with it the statistical treatment this program does not have: enough repeats
/// to characterise the dispersion rather than to take a median of a handful, and
/// explicit handling of the thermal throttling a laptop part does under
/// sustained load. Until that exists the numbers here are honest about their
/// limits, and the spread is printed beside every median so that a row measured
/// while the machine was busy can be recognised and discarded rather than
/// quoted.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "orrery/backend/cpu_topology.hpp"
#include "orrery/backend/executor.hpp"
#include "orrery/backend/serial_executor.hpp"
#include "orrery/backend/static_executor.hpp"
#include "orrery/backend/thread_pool.hpp"
#include "orrery/backend/work_stealing_executor.hpp"
#include "orrery/backend/worker_statistics.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/initial_conditions/plummer.hpp"
#include "orrery/solvers/direct_solver.hpp"

namespace {

using orrery::backend::Clock;
using orrery::backend::CoreClass;
using orrery::backend::Duration;
using orrery::backend::Executor;
using orrery::backend::ExecutorStatistics;
using orrery::backend::LogicalProcessor;
using orrery::backend::pin_current_thread;
using orrery::backend::query_logical_processors;
using orrery::backend::SerialExecutor;
using orrery::backend::StaticExecutor;
using orrery::backend::ThreadPool;
using orrery::backend::to_string;
using orrery::backend::WorkerStatistics;
using orrery::backend::WorkStealingExecutor;
using orrery::core::Index;
using orrery::core::ParticleData;
using orrery::core::RandomSource;
using orrery::core::Real;
using orrery::core::Softening;
using orrery::initial_conditions::make_plummer_sphere;
using orrery::initial_conditions::PlummerParameters;
using orrery::solvers::DirectSolver;

constexpr std::uint64_t kSeed = 20260810;

/// Large enough that a force evaluation is a substantial fraction of a second
/// serially, so that thread wake-up latency is a small correction rather than
/// the measurement, and small enough that the whole program finishes in a few
/// minutes.
constexpr Index kDefaultParticles = 8192;

constexpr int kDefaultRepeats = 9;

/// A softening length typical of a Plummer sphere run.
///
/// It has no bearing on the scheduling and is set only so that the kernel does
/// the arithmetic it would do in a real run, including the branch-free softened
/// reciprocal, rather than a cheaper unsoftened variant.
const Softening kSoftening{static_cast<Real>(0.05)};

struct WorkerRecord {
    CoreClass core_class{CoreClass::kUnknown};
    std::uint64_t items{};
    Duration busy{};
    Duration idle{};
};

struct Measurement {
    std::string scheme;
    std::string affinity;
    unsigned workers{};

    Duration median{};
    Duration fastest{};
    Duration slowest{};

    double idle_fraction{};
    double performance_idle{-1.0};
    double efficiency_idle{-1.0};

    std::uint64_t steals{};

    std::vector<WorkerRecord> per_worker;
};

/// Idle time as a fraction of available time, for workers of one class.
///
/// Returns a negative value when no worker of that class was found, which the
/// printing renders as a dash rather than as a zero. A zero would read as
/// perfect balance, which is the opposite of not knowing.
[[nodiscard]] double idle_fraction_for(const std::vector<WorkerRecord>& workers, CoreClass wanted) {
    Duration busy{};
    Duration idle{};
    bool found = false;

    for (const WorkerRecord& worker : workers) {
        if (worker.core_class != wanted) {
            continue;
        }

        found = true;
        busy += worker.busy;
        idle += worker.idle;
    }

    const Duration total = busy + idle;
    if (!found || total.count() == 0) {
        return -1.0;
    }

    return static_cast<double>(idle.count()) / static_cast<double>(total.count());
}

/// Time one configuration.
///
/// The solver, the configuration and the arithmetic are identical across every
/// row of the table. The only thing that varies is the executor, which is what
/// makes the comparison a comparison of scheduling rather than of code paths.
/// The serial baseline goes through `SerialExecutor` rather than through the
/// solver's unthreaded path for the same reason: the same task function, called
/// the same way, on one thread.
[[nodiscard]] Measurement measure(ParticleData& data, Executor& executor, std::string affinity,
                                  int repeats) {
    DirectSolver solver{kSoftening, executor};

    // Two evaluations thrown away. The first touches every page of the particle
    // arrays for the first time and wakes threads that have never run, and
    // neither cost recurs in a simulation that evaluates millions of times.
    for (int warmup = 0; warmup < 2; ++warmup) {
        solver.evaluate(data.positions(), data.masses(), data.accelerations());
    }

    executor.reset_statistics();

    std::vector<Duration> samples;
    samples.reserve(static_cast<std::size_t>(repeats));

    for (int repeat = 0; repeat < repeats; ++repeat) {
        const Clock::time_point start = Clock::now();
        solver.evaluate(data.positions(), data.masses(), data.accelerations());
        samples.push_back(std::chrono::duration_cast<Duration>(Clock::now() - start));
    }

    std::ranges::sort(samples);

    const ExecutorStatistics statistics = executor.statistics();

    Measurement measurement;
    measurement.scheme = std::string{executor.name()};
    measurement.affinity = std::move(affinity);
    measurement.workers = executor.worker_count();

    // The median rather than the best of the run. A best-of figure reports the
    // one trial where nothing else on the machine intervened, which is not the
    // performance anybody gets. The extremes are printed beside it so that a
    // median taken over a run that was drifting is visible as such.
    measurement.median = samples[samples.size() / 2];
    measurement.fastest = samples.front();
    measurement.slowest = samples.back();

    measurement.per_worker.reserve(statistics.workers.size());
    for (std::size_t worker = 0; worker < statistics.workers.size(); ++worker) {
        const WorkerStatistics& record = statistics.workers[worker];
        measurement.per_worker.push_back(
            WorkerRecord{.core_class = executor.worker_core_class(static_cast<unsigned>(worker)),
                         .items = record.items,
                         .busy = record.busy,
                         .idle = record.idle});
        measurement.steals += record.steals;
    }

    measurement.idle_fraction = statistics.idle_fraction();
    measurement.performance_idle =
        idle_fraction_for(measurement.per_worker, CoreClass::kPerformance);
    measurement.efficiency_idle = idle_fraction_for(measurement.per_worker, CoreClass::kEfficiency);

    return measurement;
}

void print_percentage(double fraction) {
    if (fraction < 0.0) {
        std::cout << std::setw(9) << "-";
        return;
    }
    std::cout << std::setw(8) << std::fixed << std::setprecision(1) << fraction * 100.0 << "%";
}

void print_row(const Measurement& measurement, Duration baseline) {
    const double milliseconds = static_cast<double>(measurement.median.count()) / 1e6;
    const double spread = static_cast<double>((measurement.slowest - measurement.fastest).count()) /
                          static_cast<double>(measurement.median.count()) * 100.0;
    const double speedup =
        static_cast<double>(baseline.count()) / static_cast<double>(measurement.median.count());

    std::cout << std::setw(14) << measurement.scheme << std::setw(4) << measurement.workers
              << std::setw(9) << measurement.affinity << std::setw(11) << std::fixed
              << std::setprecision(2) << milliseconds << std::setw(8) << std::setprecision(1)
              << spread << "%" << std::setw(9) << std::setprecision(2) << speedup << "x";

    print_percentage(measurement.idle_fraction);
    print_percentage(measurement.performance_idle);
    print_percentage(measurement.efficiency_idle);

    std::cout << std::setw(9) << measurement.steals << '\n';
}

void print_header() {
    std::cout << '\n'
              << std::setw(14) << "scheme" << std::setw(4) << "w" << std::setw(9) << "affinity"
              << std::setw(11) << "median ms" << std::setw(9) << "spread" << std::setw(10)
              << "speedup" << std::setw(9) << "idle" << std::setw(9) << "P idle" << std::setw(9)
              << "E idle" << std::setw(9) << "steals" << '\n'
              << std::string(93, '-') << '\n';
}

/// Per-worker detail for one configuration.
///
/// This is where the static scheme's failure is visible directly rather than as
/// an aggregate. Every worker is given the same number of particles, and the
/// busy times say how long that took each of them.
void print_worker_breakdown(const Measurement& measurement) {
    std::cout << "\nper worker, " << measurement.scheme << " on " << measurement.workers
              << " workers, " << measurement.affinity << ":\n"
              << std::setw(8) << "worker" << std::setw(14) << "core" << std::setw(12) << "items"
              << std::setw(12) << "busy ms" << std::setw(12) << "idle ms" << std::setw(14)
              << "items/ms busy" << '\n'
              << std::string(72, '-') << '\n';

    for (std::size_t worker = 0; worker < measurement.per_worker.size(); ++worker) {
        const WorkerRecord& record = measurement.per_worker[worker];

        const double busy_ms = static_cast<double>(record.busy.count()) / 1e6;
        const double idle_ms = static_cast<double>(record.idle.count()) / 1e6;
        const double rate = busy_ms > 0 ? static_cast<double>(record.items) / busy_ms : 0.0;

        std::cout << std::setw(8) << worker << std::setw(14) << to_string(record.core_class)
                  << std::setw(12) << record.items << std::setw(12) << std::fixed
                  << std::setprecision(1) << busy_ms << std::setw(12) << idle_ms << std::setw(14)
                  << std::setprecision(2) << rate << '\n';
    }
}

/// The first processor of a given class, or the processor count if there is
/// none.
[[nodiscard]] std::size_t first_processor_of(const std::vector<LogicalProcessor>& processors,
                                             CoreClass wanted) {
    for (std::size_t index = 0; index < processors.size(); ++index) {
        if (processors[index].core_class == wanted) {
            return index;
        }
    }
    return processors.size();
}

/// Let the part cool between configurations.
///
/// A laptop under a sustained N^2 kernel heats up within seconds and drops its
/// clock, so back-to-back configurations would be measured at different
/// frequencies and the later ones would look worse for a reason that has
/// nothing to do with their scheduling. A short pause is a crude answer and it
/// is stated as such: Phase 7 addresses throttling properly, and until then the
/// order of the rows is part of the measurement.
void settle() {
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
}

/// What the machine looks like and what was asked of it.
struct Session {
    Index particles{};
    int repeats{};
    unsigned available{};
    std::vector<LogicalProcessor> processors;
    std::size_t performance_processor{};
    std::size_t efficiency_processor{};
    bool hybrid{};
};

[[nodiscard]] Session make_session(Index particles, int repeats) {
    Session session;
    session.particles = particles;
    session.repeats = repeats;
    session.available = ThreadPool::default_worker_count();
    session.processors = query_logical_processors();
    session.performance_processor = first_processor_of(session.processors, CoreClass::kPerformance);
    session.efficiency_processor = first_processor_of(session.processors, CoreClass::kEfficiency);
    session.hybrid = session.performance_processor < session.processors.size() &&
                     session.efficiency_processor < session.processors.size();
    return session;
}

void describe(const Session& session) {
    std::cout << "Orrery threading measurement\n"
              << "particles: " << session.particles << ", repeats: " << session.repeats
              << ", scalar: " << (sizeof(Real) == 4 ? "float" : "double") << '\n'
              << "logical processors: " << session.available << '\n';

    if (session.hybrid) {
        std::cout << "topology: hybrid, performance core at index " << session.performance_processor
                  << " and efficiency core at index " << session.efficiency_processor << '\n';
    } else {
        std::cout << "topology: not reported as hybrid, so the baseline cannot be pinned to a "
                     "known class of core and the per-class columns will be empty\n";
    }
}

struct Report {
    std::vector<Measurement> rows;
    Duration baseline{};

    bool have_efficiency_baseline{false};
    Duration efficiency_baseline{};

    Measurement static_pinned;
    Measurement stealing_pinned;
};

/// The single-threaded baselines, pinned to a known class of core.
///
/// Measured before anything else and, on a hybrid part, once on each kind of
/// core. The main thread is this program's own to place, and `SerialExecutor`
/// runs on whichever thread called it, so pinning here is enough to decide
/// where the baseline is taken.
void collect_baselines(const Session& session, ParticleData& data, Report& report) {
    if (session.hybrid) {
        settle();
        if (pin_current_thread(session.processors[session.efficiency_processor].id)) {
            SerialExecutor serial;
            Measurement measurement = measure(data, serial, "E core", session.repeats);
            report.efficiency_baseline = measurement.median;
            report.have_efficiency_baseline = true;
            report.rows.push_back(std::move(measurement));
        }

        settle();
        if (!pin_current_thread(session.processors[session.performance_processor].id)) {
            std::cerr << "warning: could not pin the baseline to a performance core\n";
        }
    }

    SerialExecutor serial;
    Measurement measurement =
        measure(data, serial, session.hybrid ? "P core" : "free", session.repeats);
    report.baseline = measurement.median;

    // Ahead of the efficiency baseline if one was taken, so that the row the
    // speedups are formed against is the first in the table.
    report.rows.insert(report.rows.begin(), std::move(measurement));
}

[[nodiscard]] Report collect(const Session& session, ParticleData& data) {
    Report report;

    collect_baselines(session, data, report);

    // How the work-stealing scheme scales with the number of threads. These are
    // unpinned, which is how the project ships: the operating system knows about
    // hybrid cores and places threads with information a fixed assignment does
    // not have.
    for (unsigned workers = 1; workers <= session.available; ++workers) {
        settle();
        WorkStealingExecutor stealing{workers};
        report.rows.push_back(measure(data, stealing, "free", session.repeats));
    }

    // The comparison this phase exists for, at the full width of the machine.
    // Pinned so that idle time can be attributed to a class of core, and
    // unpinned so that the figure the project actually ships with is reported
    // rather than only the instrumented one.
    for (const ThreadPool::Affinity affinity :
         {ThreadPool::Affinity::kUnpinned, ThreadPool::Affinity::kPinned}) {
        const bool pinned = affinity == ThreadPool::Affinity::kPinned;
        const char* label = pinned ? "pinned" : "free";

        settle();
        {
            StaticExecutor fixed{session.available, affinity};
            Measurement measurement = measure(data, fixed, label, session.repeats);
            if (pinned) {
                report.static_pinned = measurement;
            }
            report.rows.push_back(std::move(measurement));
        }

        settle();
        {
            WorkStealingExecutor stealing{session.available, affinity};
            Measurement measurement = measure(data, stealing, label, session.repeats);
            if (pinned) {
                report.stealing_pinned = measurement;
            }
            report.rows.push_back(std::move(measurement));
        }
    }

    return report;
}

void present(const Report& report) {
    print_header();
    for (const Measurement& measurement : report.rows) {
        print_row(measurement, report.baseline);
    }

    if (report.have_efficiency_baseline) {
        const double ratio = static_cast<double>(report.efficiency_baseline.count()) /
                             static_cast<double>(report.baseline.count());
        std::cout << "\nperformance core against efficiency core on identical work: " << std::fixed
                  << std::setprecision(2) << ratio << "x\n"
                  << "This is the asymmetry that makes equal static shares the wrong partition.\n";
    }

    if (!report.static_pinned.per_worker.empty()) {
        print_worker_breakdown(report.static_pinned);
    }
    if (!report.stealing_pinned.per_worker.empty()) {
        print_worker_breakdown(report.stealing_pinned);
    }

    std::cout << "\nidle is the share of worker time spent not running the kernel,\n"
                 "summed over workers and including thread wake-up and the wait at\n"
                 "the end of a region for slower workers to finish.\n"
                 "speedup is against the single-threaded baseline in the first row.\n";
}

/// Report a failure with no risk of throwing while doing it.
///
/// Through stdio rather than through `std::cerr`, because inserting into a
/// stream can itself throw, and an exception raised inside an exception handler
/// in `main` would escape after all. The return values are discarded
/// deliberately: there is nothing useful to do when the report of a failure
/// fails.
void report_failure(const char* what) noexcept {
    static_cast<void>(std::fputs("measurement failed", stderr));

    if (what != nullptr) {
        static_cast<void>(std::fputs(": ", stderr));
        static_cast<void>(std::fputs(what, stderr));
    }

    static_cast<void>(std::fputs("\n", stderr));
}

void run(Index particles, int repeats) {
    const Session session = make_session(particles, repeats);
    describe(session);

    RandomSource random{kSeed};
    ParticleData data = make_plummer_sphere(PlummerParameters{.count = particles}, random);

    present(collect(session, data));
}

} // namespace

int main(int argc, char** argv) {
    // Everything below the argument parsing allocates, and an allocation that
    // fails would otherwise leave `main` throwing. Reporting it and returning
    // non-zero is the whole of what a measurement program can usefully do
    // about it.
    try {
        const Index particles =
            argc > 1 ? static_cast<Index>(std::strtoull(argv[1], nullptr, 10)) : kDefaultParticles;
        const int repeats =
            argc > 2 ? static_cast<int>(std::strtol(argv[2], nullptr, 10)) : kDefaultRepeats;

        if (particles == 0 || repeats <= 0) {
            std::cerr << "usage: threading_scaling [particles] [repeats]\n";
            return 1;
        }

        run(particles, repeats);
    } catch (const std::exception& error) {
        report_failure(error.what());
        return 1;
    } catch (...) {
        // The catch-all makes the guarantee that nothing leaves main complete
        // rather than conditional on nothing in this program ever throwing
        // something outside the standard hierarchy.
        report_failure(nullptr);
        return 1;
    }

    return 0;
}
