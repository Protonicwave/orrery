/// \file
/// What a discrete NVIDIA GPU is worth, and where it differs from the
/// integrated one this project measured first.
///
/// Four questions, answered in one session so that they are answered on one
/// thermal state of one machine.
///
/// How does the card compare with the CPU on the same problem? Both run direct
/// summation over the same Plummer spheres with the same softening, so the
/// comparison is between two processors rather than between two algorithms. The
/// CPU side is whatever the machine running this happens to have, which is worth
/// saying plainly: on the hosted notebooks this backend is measured on, that is
/// two shared cores rather than the eight the Phase 9 tables used, so the
/// speedup column here and the speedup column there are not the same
/// measurement. The machine state is printed above the tables for that reason.
///
/// Where does the transfer go? Phase 9 could argue its staging away because both
/// ends were in memory the CPU and GPU shared. Here the bus is real, and the
/// argument has to be made again on evidence: the solver reports the split, the
/// table prints it, and a reader can see at what size the transfer stops
/// mattering and whether it ever does.
///
/// Is the coherent tree traversal worth what it costs on a second vendor? The
/// mitigation in ADR-0029 was justified from how SIMD hardware executes
/// divergent branches, which is a claim about a class of machine. The traversal
/// table runs both walks at every coherence width and reports the node visits
/// each makes beside the time each takes, which is the same pair of columns
/// `docs/performance/sycl_tree.md` reports.
///
/// And what is the arithmetic actually reaching? The three probes measure this
/// device's own ceilings in the same session, because a shared hosted machine is
/// the last place a specification sheet should be trusted.
///
/// Accuracy is measured against `solvers/reference_kernel.hpp` rather than
/// against the CPU solver, for the reason Phase 7 built that reference: two
/// approximate answers agreeing says they are wrong in the same way, not that
/// either is right.

#include <cstdio>
#include <exception>

#ifdef ORRERY_ENABLE_CUDA

#    include <algorithm>
#    include <chrono>
#    include <cmath>
#    include <cstddef>
#    include <cstdint>
#    include <iomanip>
#    include <iostream>
#    include <memory>
#    include <string>
#    include <vector>

#    include "cuda_probes.hpp"
#    include "harness/machine_state.hpp"
#    include "harness/protocol.hpp"
#    include "harness/statistics.hpp"
#    include "orrery/backend/cuda_device.hpp"
#    include "orrery/backend/cuda_memory.hpp"
#    include "orrery/backend/thread_pool.hpp"
#    include "orrery/backend/work_stealing_executor.hpp"
#    include "orrery/core/particle_data.hpp"
#    include "orrery/core/random.hpp"
#    include "orrery/core/softening.hpp"
#    include "orrery/core/types.hpp"
#    include "orrery/core/vec3.hpp"
#    include "orrery/initial_conditions/plummer.hpp"
#    include "orrery/solvers/cuda_direct_solver.hpp"
#    include "orrery/solvers/cuda_kernels.hpp"
#    include "orrery/solvers/cuda_tree_solver.hpp"
#    include "orrery/solvers/direct_solver.hpp"
#    include "orrery/solvers/octree.hpp"
#    include "orrery/solvers/reference_kernel.hpp"

namespace {

using orrery::backend::check_cuda;
using orrery::backend::CudaArray;
using orrery::backend::CudaDeviceDescription;
using orrery::backend::ThreadPool;
using orrery::backend::to_version_string;
using orrery::backend::WorkStealingExecutor;
using orrery::benchmark::capture_machine_state;
using orrery::benchmark::kDivSqrtAccumulators;
using orrery::benchmark::kFmaAccumulators;
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
using orrery::solvers::CudaDirectSolver;
using orrery::solvers::CudaEvaluationTimings;
using orrery::solvers::CudaTraversal;
using orrery::solvers::CudaTreeSolver;
using orrery::solvers::DirectSolver;
using orrery::solvers::reference_acceleration;
using orrery::solvers::ReferenceAcceleration;
using orrery::solvers::TreeParameters;

/// The seed every other measurement and test in this project uses, so that the
/// configurations here are the same configurations.
constexpr std::uint64_t kSeed = 20260811;

const Softening kSoftening{static_cast<Real>(0.05)};

/// The range of the direct scaling table.
///
/// The lower end is small enough that the card is dominated by the cost of
/// launching a kernel and moving the particles at all, which is the regime a
/// reader needs to see in order to know not to use it there. The upper end is
/// where one CPU evaluation is already seconds.
constexpr Index kSmallest = 1024;
constexpr Index kLargest = 131072;

/// Above this the CPU is not timed, for the reason `tree_scaling.cpp` gives: one
/// evaluation over 65536 particles is four billion interactions, and on the two
/// shared cores a hosted notebook offers that is minutes rather than seconds.
constexpr Index kLargestCpu = 32768;

/// The range of the tree table, which starts where a tree is worth building.
constexpr Index kTreeSmallest = 16384;
constexpr Index kTreeLargest = 262144;

/// How many particles the error is sampled over, since the compensated
/// reference costs a double-precision division and a branch per pair.
constexpr Index kErrorSamples = 512;

/// One interaction, in floating-point operations.
///
/// The same accounting `docs/performance/roofline.md` uses and the Phase 9
/// tables repeat, so that a rate from this table can be put beside either:
/// three subtractions, three multiplies and two adds for the squared separation,
/// one add for the softening, a square root, a division, and three fused
/// multiply-adds for the accumulation.
constexpr double kFlopsPerInteraction = 20.0;

/// A longer cool-down than the harness default, for the reason
/// `benchmarks/sycl_direct.cpp` gives at length: this program loads a GPU and a
/// CPU hard in one session, and rows measured minutes apart cannot be compared
/// if the machine has drifted between them.
///
/// The reason applies differently here rather than less. A discrete card has its
/// own power and thermal budget, so it does not heat the CPU rows the way an
/// integrated part does. What it has instead is a host nobody controls: a
/// hosted notebook shares its processor with other tenants, and the remedy for
/// both problems is the same one, which is a canary that says how far the
/// machine moved and a session repeated rather than quoted when it moved far.
const Protocol kProtocol{.cooldown = std::chrono::seconds(3)};

[[nodiscard]] double milliseconds(orrery::benchmark::Duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

[[nodiscard]] ParticleData sampled_sphere(Index count) {
    RandomSource random{kSeed};
    return make_plummer_sphere(PlummerParameters{.count = count}, random);
}

struct DeviceCeilings {
    double fma_gflops{};

    /// Square roots and divisions per second.
    ///
    /// The ceiling that binds the direct kernel on the CPU and did not bind it
    /// on the integrated GPU, where the ratio between the two arithmetic
    /// ceilings turned out to be 4 rather than the CPU's 27. Which of the two
    /// binds on this card is the question, and it is asked rather than assumed.
    double div_sqrt_gops{};

    double read_gbps{};
};

[[nodiscard]] double measure_fma(const Protocol& protocol) {
    constexpr unsigned kItems = 1U << 20U;
    constexpr unsigned kIterations = 512;

    CudaArray<float> output{kItems};
    float* const data = output.data();

    const TrialSet trials = orrery::benchmark::run_trials(protocol, [&] {
        check_cuda(orrery::benchmark::launch_fma_probe(data, kItems, kIterations),
                   "the multiply-add probe");
    });

    // Two floating-point operations per fused multiply-add, which is the
    // accounting every other ceiling in this project uses.
    const double operations = 2.0 * static_cast<double>(kItems) *
                              static_cast<double>(kFmaAccumulators) *
                              static_cast<double>(kIterations);
    const double seconds = milliseconds(trials.median()) * 1.0e-3;
    return seconds > 0 ? operations / seconds * 1.0e-9 : 0;
}

[[nodiscard]] double measure_div_sqrt(const Protocol& protocol) {
    constexpr unsigned kItems = 1U << 20U;
    constexpr unsigned kIterations = 128;

    CudaArray<float> output{kItems};
    float* const data = output.data();

    const TrialSet trials = orrery::benchmark::run_trials(protocol, [&] {
        check_cuda(orrery::benchmark::launch_div_sqrt_probe(data, kItems, kIterations),
                   "the divide and square root probe");
    });

    const double operations = 2.0 * static_cast<double>(kItems) *
                              static_cast<double>(kDivSqrtAccumulators) *
                              static_cast<double>(kIterations);
    const double seconds = milliseconds(trials.median()) * 1.0e-3;
    return seconds > 0 ? operations / seconds * 1.0e-9 : 0;
}

[[nodiscard]] double measure_bandwidth(const Protocol& protocol) {
    // 256 MiB, against whatever caches the part carries, so the figure is memory
    // rather than cache.
    constexpr std::size_t kElements = 64U << 20U;
    constexpr unsigned kBlocks = 4096;
    constexpr unsigned kBlock = 256;

    CudaArray<float> input{kElements};
    CudaArray<float> output{static_cast<std::size_t>(kBlocks) * kBlock};

    // Filled rather than left uninitialised, because a read of memory the driver
    // has never had to back is a read the driver may satisfy without touching
    // it, and this probe exists to make the memory controller work.
    //
    // Filled from ordinary host memory rather than from a pinned buffer, which
    // is the one place in this program that choice is right. Pinning is what
    // makes a repeated transfer fast and it is what the solvers stage through;
    // here there is one transfer, it happens before anything is timed, and
    // pinning a quarter of a gigabyte on a shared hosted machine to save a few
    // milliseconds nobody is measuring would be the wrong trade.
    const std::vector<float> filled(kElements, 1.0F);
    input.copy_from_host(filled.data(), kElements);

    const float* const data = input.data();
    float* const sums = output.data();

    const TrialSet trials = orrery::benchmark::run_trials(protocol, [&] {
        check_cuda(orrery::benchmark::launch_read_probe(data, sums, kElements, kBlocks, kBlock),
                   "the read bandwidth probe");
    });

    const double bytes = static_cast<double>(kElements) * static_cast<double>(sizeof(float));
    const double seconds = milliseconds(trials.median()) * 1.0e-3;
    return seconds > 0 ? bytes / seconds * 1.0e-9 : 0;
}

struct ScalingRow {
    Index particles{};
    TrialSet gpu;
    TrialSet cpu;
    CudaEvaluationTimings timings;
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

[[nodiscard]] ScalingRow measure_size(Index particles, CudaDirectSolver& gpu,
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

/// One row of the traversal table: one size, one traversal, one width.
struct TraversalRow {
    Index particles{};
    CudaTraversal traversal{};
    unsigned width{};
    TrialSet total;
    orrery::benchmark::Duration kernel{};
    std::uint64_t visits{};
    std::uint64_t nodes{};
};

[[nodiscard]] TraversalRow measure_traversal(Index particles, CudaTreeSolver& solver,
                                             CudaTraversal traversal, unsigned width,
                                             const Protocol& protocol) {
    ParticleData data = sampled_sphere(particles);

    solver.select_traversal(traversal);
    solver.select_coherence_width(width);

    TraversalRow row;
    row.particles = particles;
    row.traversal = traversal;
    row.width = solver.coherence_width();

    orrery::benchmark::cool_down(protocol);
    row.total = orrery::benchmark::run_trials(
        protocol, [&] { solver.evaluate(data.positions(), data.masses(), data.accelerations()); });

    // One further evaluation with the counters cleared, so that the visit count
    // is the count of one walk rather than of every trial the protocol ran.
    solver.reset_interaction_count();
    solver.evaluate(data.positions(), data.masses(), data.accelerations());

    row.kernel = solver.timings().kernel;
    row.visits = solver.node_visits();
    row.nodes = solver.tree().nodes().size();

    return row;
}

void print_device(const CudaDeviceDescription& device) {
    std::cout << "\ndevice\n"
              << "  name               " << device.name << '\n'
              << "  compute capability " << device.compute_capability_major << '.'
              << device.compute_capability_minor << '\n'
              << "  multiprocessors    " << device.multiprocessor_count << '\n'
              << "  warp width         " << device.warp_size << '\n'
              << "  max threads/block  " << device.max_threads_per_block << '\n'
              << "  shared memory      " << (device.shared_memory_bytes_per_block / 1024)
              << " KiB per block\n"
              << "  device memory      " << (device.global_memory_bytes / (1024 * 1024)) << " MiB\n"
              << "  driver             " << to_version_string(device.driver_version) << '\n'
              << "  runtime            " << to_version_string(device.runtime_version) << '\n'
              << "  integrated         " << (device.integrated ? "yes" : "no")
              << "   (no is why every evaluation carries a transfer)\n"
              << "  managed memory     " << (device.supports_managed_memory ? "yes" : "no")
              << "   (available and deliberately not used, ADR-0060)\n";
}

void print_scaling(const std::vector<ScalingRow>& rows) {
    std::cout << "\nscaling, CUDA direct summation against the threaded CPU kernel\n"
              << std::setw(9) << "N" << std::setw(12) << "GPU ms" << std::setw(9) << "spread"
              << std::setw(12) << "kernel ms" << std::setw(12) << "moved ms" << std::setw(9)
              << "moved %" << std::setw(12) << "CPU ms" << std::setw(10) << "speedup"
              << std::setw(14) << "Ginteract/s" << std::setw(12) << "Gflop/s" << '\n'
              << std::string(111, '-') << '\n';

    for (const ScalingRow& row : rows) {
        const double gpu = milliseconds(row.gpu.median());
        const double kernel = milliseconds(row.timings.kernel);

        // Both transfers and both staging copies, which together are everything
        // an evaluation does other than compute. Reported as one column because
        // what a reader wants from it is the fraction of the evaluation that is
        // not arithmetic; the solver's five fields are available to anyone who
        // wants the split finer than that.
        const double moved =
            milliseconds(row.timings.staging_in) + milliseconds(row.timings.transfer_in) +
            milliseconds(row.timings.transfer_out) + milliseconds(row.timings.staging_out);

        const auto count = static_cast<double>(row.particles);

        // Interactions per second from the kernel time rather than the total,
        // because that is the rate the hardware achieved. The movement is real
        // and is reported beside it rather than folded in, so the two claims
        // stay separable.
        const double interactions = count * (count - 1);
        const double rate = kernel > 0 ? interactions / (kernel * 1.0e-3) : 0;

        std::cout << std::setw(9) << row.particles << std::fixed << std::setprecision(3)
                  << std::setw(12) << gpu << std::setprecision(3) << std::setw(9)
                  << row.gpu.relative_spread() << std::setw(12) << kernel << std::setw(12) << moved
                  << std::setprecision(1) << std::setw(9) << (gpu > 0 ? 100.0 * moved / gpu : 0);

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

    std::cout << "\nspeedup is CPU total over GPU total, so the transfer counts against the GPU.\n"
                 "the rate columns are from the kernel alone, which is what the hardware did.\n"
                 "the CPU column is this machine's CPU, which on a hosted notebook is not the\n"
                 "one docs/performance/sycl_direct.md compares against.\n";
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

void print_traversals(const std::vector<TraversalRow>& rows) {
    std::cout << "\ntree traversal, both walks at every coherence width\n"
              << std::setw(9) << "N" << std::setw(14) << "traversal" << std::setw(8) << "width"
              << std::setw(12) << "total ms" << std::setw(9) << "spread" << std::setw(12)
              << "kernel ms" << std::setw(10) << "nodes" << std::setw(16) << "node visits"
              << std::setw(12) << "per target" << '\n'
              << std::string(102, '-') << '\n';

    for (const TraversalRow& row : rows) {
        const auto count = static_cast<double>(row.particles);
        const double per_target = count > 0 ? static_cast<double>(row.visits) / count : 0;

        std::cout << std::setw(9) << row.particles << std::setw(14) << to_string(row.traversal)
                  << std::setw(8)
                  << (row.traversal == CudaTraversal::kCoherent ? std::to_string(row.width)
                                                                : std::string{"-"})
                  << std::fixed << std::setprecision(3) << std::setw(12)
                  << milliseconds(row.total.median()) << std::setprecision(3) << std::setw(9)
                  << row.total.relative_spread() << std::setw(12) << milliseconds(row.kernel)
                  << std::setw(10) << row.nodes << std::setw(16) << row.visits
                  << std::setprecision(1) << std::setw(12) << per_target << '\n';
    }

    std::cout << "\nnode visits is summed over threads: for the independent walk it is the sum of\n"
                 "the individual walks, and for the coherent walk it is the segment's union walk\n"
                 "counted once per lane, so the ratio between them is the redundancy coherence\n"
                 "costs. the interesting result is whether the coherent walk is faster while\n"
                 "visiting more nodes, which is what it does on the other device.\n";
}

void print_ceilings(const DeviceCeilings& ceilings, const std::vector<ScalingRow>& rows,
                    Index block) {
    std::cout << "\nmeasured device ceilings\n"
              << "  fused multiply-add  " << std::fixed << std::setprecision(1)
              << ceilings.fma_gflops << " Gflop/s\n"
              << "  divide and sqrt     " << ceilings.div_sqrt_gops << " Gop/s\n"
              << "  read bandwidth      " << ceilings.read_gbps << " GB/s\n";

    double best = 0;
    for (const ScalingRow& row : rows) {
        const double kernel = milliseconds(row.timings.kernel);
        const auto count = static_cast<double>(row.particles);
        if (kernel > 0) {
            best = std::max(best, count * (count - 1) / (kernel * 1.0e-3));
        }
    }

    const double achieved = best * kFlopsPerInteraction * 1.0e-9;

    // Two of these per interaction, one square root and one division, counted
    // the way every other table in this project counts them.
    const double achieved_div_sqrt = best * 2.0 * 1.0e-9;

    // Each block reads all N sources once per evaluation rather than each thread
    // reading them, which is the whole point of staging through shared memory.
    // So the global traffic is N^2/block source records of four scalars, and the
    // intensity carries a factor of the block size.
    const double tile = static_cast<double>(block);
    const double intensity =
        kFlopsPerInteraction * tile / (4.0 * static_cast<double>(sizeof(Real)));

    std::cout << "\nthe kernel's best row reached " << std::setprecision(1) << achieved
              << " Gflop/s, " << std::setprecision(1)
              << (ceilings.fma_gflops > 0 ? 100.0 * achieved / ceilings.fma_gflops : 0)
              << " per cent of this device's multiply-add ceiling, and\n"
              << std::setprecision(2) << achieved_div_sqrt << " Gop/s of divides and square "
              << "roots, " << std::setprecision(1)
              << (ceilings.div_sqrt_gops > 0 ? 100.0 * achieved_div_sqrt / ceilings.div_sqrt_gops
                                             : 0)
              << " per cent of that ceiling.\n"
                 "which of the two binds is the question this table exists to answer: on the CPU\n"
                 "it is the divide and square root unit, and on the integrated GPU it was "
                 "neither.\n\n"
                 "arithmetic intensity is about "
              << std::setprecision(0) << intensity
              << " flop per byte of source data, because each\nblock reads every source once "
                 "rather than each thread doing so.\n";
}

void print_canary(double across_tables, double across_session) {
    std::cout << "\nthermal canary\n"
              << "  across the rows quoted above   " << std::fixed << std::setprecision(1)
              << (across_tables * 100.0) << " per cent slower\n"
              << "  across the whole session       " << (across_session * 100.0)
              << " per cent slower, ceiling probes included\n";
}

int run() {
    const MachineState state = capture_machine_state();
    orrery::benchmark::print(std::cout, state);

    const std::unique_ptr<CudaDirectSolver> gpu = CudaDirectSolver::try_create(kSoftening);
    if (gpu == nullptr) {
        std::cout << "\nno usable CUDA device on this machine, so there is nothing to measure.\n";
        return 0;
    }

    print_device(gpu->device());
    std::cout << "\n  block             " << gpu->block_size()
              << "   (sources staged through shared memory per pass)\n";

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

    const std::unique_ptr<CudaTreeSolver> tree =
        CudaTreeSolver::try_create(TreeParameters{}, kSoftening, &executor);
    if (tree != nullptr) {
        std::vector<TraversalRow> traversals;

        for (Index particles = kTreeSmallest; particles <= kTreeLargest; particles *= 2) {
            // The independent walk first, so that the coherent rows at every
            // width have a baseline taken at the same size on the same thermal
            // state rather than one taken minutes earlier.
            traversals.push_back(measure_traversal(particles, *tree, CudaTraversal::kIndependent,
                                                   tree->device().warp_size, protocol));
            canary.mark();

            for (const unsigned width : tree->supported_coherence_widths()) {
                traversals.push_back(
                    measure_traversal(particles, *tree, CudaTraversal::kCoherent, width, protocol));
                canary.mark();
            }
        }

        print_traversals(traversals);
    }

    // Closes the bracket on the rows above before anything else runs.
    canary.mark();
    const double across_tables = canary.slowdown();

    // The device's own limits, measured last so that a failure here still leaves
    // the tables above printed.
    orrery::benchmark::cool_down(protocol);
    const DeviceCeilings ceilings{.fma_gflops = measure_fma(protocol),
                                  .div_sqrt_gops = measure_div_sqrt(protocol),
                                  .read_gbps = measure_bandwidth(protocol)};
    print_ceilings(ceilings, scaling, gpu->block_size());

    canary.mark();
    print_canary(across_tables, canary.slowdown());

    return 0;
}

} // namespace

#else

namespace {

int run() {
    static_cast<void>(
        std::fputs("This build has no CUDA backend. Configure with --preset cuda, having\n"
                   "put the toolkit's nvcc on PATH, to measure an NVIDIA device.\n",
                   stdout));
    return 0;
}

} // namespace

#endif // ORRERY_ENABLE_CUDA

namespace {

/// Report a failure with no risk of throwing while doing it.
///
/// The same shape the other benchmarks use, and for the same reason: a handler
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

int main() {
    // Exceptions are permitted at this boundary and nowhere below it. A device
    // that disappears mid-session, which a hosted notebook reclaiming its
    // hardware does, arrives here as a backend::CudaError, and a benchmark that
    // terminated on it would leave no indication of which row it had reached.
    try {
        return run();
    } catch (const std::exception& error) {
        report_failure(error.what());
        return 1;
    } catch (...) {
        report_failure(nullptr);
        return 1;
    }
}
