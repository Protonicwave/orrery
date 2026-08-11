/// \file
/// What the integrated GPU is worth, against the CPU it shares its memory with.
///
/// Three questions, answered in one session so that they are answered on one
/// thermal state of one machine.
///
/// How does the GPU compare with the CPU on the same problem? Both run direct
/// summation over the same configurations with the same softening, so the
/// comparison is between two processors rather than between two algorithms. The
/// CPU side is the fastest thing this project has: the AVX2 kernel from Phase 7
/// on the work-stealing executor from Phase 6, across all eight cores. Beating a
/// single-threaded scalar baseline would be easy and would mean nothing.
///
/// Where does the staging go? ADR-0027 argues that copying the particles into
/// shared memory is O(N) against a kernel that is O(N^2), and therefore stops
/// mattering at the sizes worth using a GPU for. The solver reports the split,
/// and the table prints it, so that a reader who suspects these figures are
/// really measuring memcpy can see that they are not, and can see exactly where
/// they would start to.
///
/// And what is the arithmetic actually reaching? Phase 7 established that this
/// project quotes achieved fractions of measured limits rather than raw
/// timings. The last column is interactions per second, which is the rate the
/// roofline of `docs/performance/roofline.md` can be compared against, since one
/// interaction is a fixed twenty floating-point operations including a square
/// root and a division.
///
/// Accuracy is measured against `solvers/reference_kernel.hpp` rather than
/// against the CPU solver, for the reason Phase 7 built that reference: two
/// approximate answers agreeing says they are wrong in the same way, not that
/// either is right.

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>

#include "orrery/backend/sycl_device.hpp"

#ifdef ORRERY_ENABLE_SYCL

#    include <algorithm>
#    include <chrono>
#    include <cmath>
#    include <cstdint>
#    include <iomanip>
#    include <memory>
#    include <string>
#    include <vector>

#    include "harness/machine_state.hpp"
#    include "harness/protocol.hpp"
#    include "harness/statistics.hpp"
#    include "orrery/backend/thread_pool.hpp"
#    include "orrery/backend/work_stealing_executor.hpp"
#    include "orrery/core/particle_data.hpp"
#    include "orrery/core/random.hpp"
#    include "orrery/core/softening.hpp"
#    include "orrery/core/types.hpp"
#    include "orrery/core/vec3.hpp"
#    include "orrery/initial_conditions/plummer.hpp"
#    include "orrery/solvers/direct_solver.hpp"
#    include "orrery/solvers/reference_kernel.hpp"
#    include "orrery/solvers/sycl_direct_solver.hpp"

namespace {

using orrery::backend::ThreadPool;
using orrery::backend::WorkStealingExecutor;
using orrery::benchmark::capture_machine_state;
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
using orrery::solvers::DirectSolver;
using orrery::solvers::reference_acceleration;
using orrery::solvers::ReferenceAcceleration;
using orrery::solvers::SyclDirectSolver;
using orrery::solvers::SyclEvaluationTimings;

constexpr std::uint64_t kSeed = 20260811;

const Softening kSoftening{static_cast<Real>(0.05)};

/// The range of the scaling table.
///
/// The lower end is small enough that the GPU is dominated by the cost of
/// launching a kernel at all, which is the regime a reader needs to see in order
/// to know not to use it there. The upper end is where one CPU evaluation is
/// already seconds.
constexpr Index kSmallest = 1024;
constexpr Index kLargest = 131072;

/// Above this the CPU is not timed, for the reason `tree_scaling.cpp` gives:
/// one evaluation over 65536 particles is four billion interactions, and two
/// sizes further up a row of trials would be minutes.
constexpr Index kLargestCpu = 65536;

/// How many particles the error is sampled over, since the compensated
/// reference costs a double-precision division and a branch per pair.
constexpr Index kErrorSamples = 512;

/// One interaction, in floating-point operations.
///
/// The same accounting `docs/performance/roofline.md` uses, so that a rate from
/// this table can be put beside the ceilings measured there: three subtractions,
/// three multiplies and two adds for the squared separation, one add for the
/// softening, a square root, a division, and three fused multiply-adds for the
/// accumulation.
constexpr double kFlopsPerInteraction = 20.0;

/// A longer cool-down than the harness default.
///
/// This is the first benchmark in the project to load the GPU and the CPU hard
/// in the same session, and on a Lunar Lake part they are on one package under
/// one power budget. Sustained GPU work therefore heats the cores the CPU rows
/// are measured on, and sustained eight-thread AVX2 work heats the GPU the rows
/// either side of it are measured on, in a way no previous phase produced.
///
/// The first session run with the harness default of 750 milliseconds ended
/// with a thermal canary reporting the machine 191 per cent slower than it
/// started, against 1.7 per cent for Phase 8 and 5.2 for Phase 7. Per-row
/// spreads stayed under 6 per cent, so the rows were internally sound and the
/// session was not: a canary that large means rows measured minutes apart
/// cannot be compared, which is exactly what a speedup column does.
/// `docs/performance/roofline.md` records the same failure mode and the same
/// remedy, which is to repeat the session rather than quote it.
///
/// Three seconds, which is four times the default and still leaves a session of
/// eight sizes finishing in minutes.
const Protocol kProtocol{.cooldown = std::chrono::seconds(3)};

[[nodiscard]] double milliseconds(orrery::benchmark::Duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

[[nodiscard]] ParticleData sampled_sphere(Index count) {
    RandomSource random{kSeed};
    return make_plummer_sphere(PlummerParameters{.count = count}, random);
}

struct ScalingRow {
    Index particles{};
    TrialSet gpu;
    TrialSet cpu;
    SyclEvaluationTimings timings;
    bool cpu_timed{false};
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

[[nodiscard]] ScalingRow measure_size(Index particles, SyclDirectSolver& gpu,
                                      WorkStealingExecutor& executor, const Protocol& protocol) {
    ParticleData data = sampled_sphere(particles);

    ScalingRow row;
    row.particles = particles;

    orrery::benchmark::cool_down(protocol);
    row.gpu = orrery::benchmark::run_trials(
        protocol, [&] { gpu.evaluate(data.positions(), data.masses(), data.accelerations()); });

    // The split from one evaluation rather than from the whole run, since the
    // solver overwrites it each time and the last trial is as representative as
    // any other.
    gpu.evaluate(data.positions(), data.masses(), data.accelerations());
    row.timings = gpu.timings();

    if (particles <= kLargestCpu) {
        DirectSolver cpu{kSoftening, executor};

        orrery::benchmark::cool_down(protocol);
        row.cpu = orrery::benchmark::run_trials(
            protocol, [&] { cpu.evaluate(data.positions(), data.masses(), data.accelerations()); });
        row.cpu_timed = true;
    }

    return row;
}

void print_device(const orrery::backend::DeviceDescription& device) {
    std::cout << "\ndevice\n"
              << "  name              " << device.name << '\n'
              << "  vendor            " << device.vendor << '\n'
              << "  runtime           " << device.backend << '\n'
              << "  driver            " << device.driver_version << '\n'
              << "  compute units     " << device.compute_units << '\n'
              << "  max work-group    " << device.max_work_group_size << '\n'
              << "  sub-group width   " << device.sub_group_size << '\n'
              << "  local memory      " << (device.local_memory_bytes / 1024) << " KiB\n"
              << "  global memory     " << (device.global_memory_bytes / (1024 * 1024)) << " MiB\n"
              << "  fp64              " << (device.supports_double_precision ? "yes" : "no") << '\n'
              << "  shared USM        " << (device.supports_shared_usm ? "yes" : "no") << '\n'
              << "  system USM        " << (device.supports_system_usm ? "yes" : "no")
              << "   (false is why the solver stages, ADR-0027)\n";
}

void print_scaling(const std::vector<ScalingRow>& rows) {
    std::cout << "\nscaling, GPU direct summation against the threaded AVX2 CPU kernel\n"
              << std::setw(9) << "N" << std::setw(12) << "GPU ms" << std::setw(9) << "spread"
              << std::setw(12) << "kernel ms" << std::setw(12) << "stage ms" << std::setw(9)
              << "stage %" << std::setw(12) << "CPU ms" << std::setw(10) << "speedup"
              << std::setw(14) << "Ginteract/s" << std::setw(12) << "Gflop/s" << '\n'
              << std::string(111, '-') << '\n';

    for (const ScalingRow& row : rows) {
        const double gpu = milliseconds(row.gpu.median());
        const double kernel = milliseconds(row.timings.kernel);
        const double staging =
            milliseconds(row.timings.staging_in) + milliseconds(row.timings.staging_out);
        const auto count = static_cast<double>(row.particles);

        // Interactions per second from the kernel time rather than the total,
        // because that is the rate the hardware achieved. The staging is real
        // and is reported beside it rather than folded in, so the two claims
        // stay separable.
        const double interactions = count * (count - 1);
        const double rate = kernel > 0 ? interactions / (kernel * 1.0e-3) : 0;

        std::cout << std::setw(9) << row.particles << std::fixed << std::setprecision(3)
                  << std::setw(12) << gpu << std::setprecision(3) << std::setw(9)
                  << row.gpu.relative_spread() << std::setw(12) << kernel << std::setw(12)
                  << staging << std::setprecision(1) << std::setw(9)
                  << (gpu > 0 ? 100.0 * staging / gpu : 0);

        if (row.cpu_timed) {
            const double cpu = milliseconds(row.cpu.median());
            std::cout << std::setprecision(3) << std::setw(12) << cpu << std::setprecision(2)
                      << std::setw(10) << (gpu > 0 ? cpu / gpu : 0);
        } else {
            std::cout << std::setw(12) << "-" << std::setw(10) << "-";
        }

        std::cout << std::setprecision(2) << std::setw(14) << (rate * 1.0e-9) << std::setw(12)
                  << (rate * kFlopsPerInteraction * 1.0e-9) << '\n';
    }

    std::cout << "\nspeedup is CPU total over GPU total, so the staging counts against the GPU.\n"
                 "the rate columns are from the kernel alone, which is what the hardware did.\n";
}

void print_accuracy(const std::vector<AccuracyRow>& rows) {
    std::cout << "\naccuracy against the compensated double-precision reference\n"
              << std::setw(9) << "N" << std::setw(14) << "worst" << std::setw(14) << "rms" << '\n'
              << std::string(37, '-') << '\n';

    for (const AccuracyRow& row : rows) {
        std::cout << std::setw(9) << row.particles << std::scientific << std::setprecision(3)
                  << std::setw(14) << row.worst << std::setw(14) << row.root_mean_square
                  << std::defaultfloat << '\n';
    }
}

void print_canary(const ThermalCanary& canary) {
    std::cout << "\nthermal canary: the machine ended the session " << std::fixed
              << std::setprecision(1) << (canary.slowdown() * 100.0)
              << " per cent slower than it started.\n";
}

int run() {
    const MachineState state = capture_machine_state();
    orrery::benchmark::print(std::cout, state);

    const std::unique_ptr<SyclDirectSolver> gpu = SyclDirectSolver::try_create(kSoftening);
    if (gpu == nullptr) {
        std::cout << "\nno usable SYCL GPU on this machine, so there is nothing to measure.\n";
        return 0;
    }

    print_device(gpu->device());
    std::cout << "\n  tile              " << gpu->tile_size()
              << "   (sources staged through local memory per pass)\n";

    WorkStealingExecutor executor{ThreadPool::default_worker_count()};

    const Protocol& protocol = kProtocol;
    ThermalCanary canary;
    canary.mark();

    std::vector<ScalingRow> scaling;
    for (Index particles = kSmallest; particles <= kLargest; particles *= 2) {
        scaling.push_back(measure_size(particles, *gpu, executor, protocol));
        canary.mark();
    }
    print_scaling(scaling);

    std::vector<AccuracyRow> accuracy;
    for (const Index particles : {Index{4096}, Index{16384}, Index{65536}}) {
        ParticleData data = sampled_sphere(particles);
        gpu->evaluate(data.positions(), data.masses(), data.accelerations());
        accuracy.push_back(measure_error(data, particles));
    }
    print_accuracy(accuracy);

    canary.mark();
    print_canary(canary);

    return 0;
}

} // namespace

#else

namespace {

int run() {
    std::cout << "This build has no SYCL backend. Configure with ORRERY_ENABLE_SYCL=ON and\n"
                 "the oneAPI DPC++ compiler to measure the GPU.\n";
    return 0;
}

} // namespace

#endif // ORRERY_ENABLE_SYCL

int main() {
    // Exceptions are permitted at this boundary and nowhere below it. A device
    // that disappears mid-session, which a driver reset does, arrives here as a
    // sycl::exception, and a benchmark that terminated on it would leave no
    // indication of which row it had reached.
    try {
        return run();
    } catch (const std::exception& error) {
        std::cerr << "benchmark failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
