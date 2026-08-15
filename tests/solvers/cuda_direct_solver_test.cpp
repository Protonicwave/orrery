/// \file
/// What the CUDA kernel gets right, measured against the project's reference.
///
/// Phase 10's definition of done asks that the same run produce the same physics
/// on both devices to within the stated tolerance. That is worth being careful
/// about, because the obvious reading of it is not available: no machine this
/// project runs on has an Intel GPU and an NVIDIA one, so no test can evaluate
/// the same configuration on both and subtract.
///
/// The available reading is stronger rather than weaker. Every solver in this
/// project is measured against `solvers/reference_kernel.hpp`, which sums the
/// same softened force law with compensation in double precision whatever the
/// build's scalar type, and whose own summation error is negligible against what
/// is being measured. The bounds below are the bounds
/// `tests/solvers/sycl_direct_solver_test.cpp` states, character for character.
/// Two backends meeting the same bound against the same reference is a claim
/// about each one's distance from the truth, where comparing them with each
/// other would only be a claim about the distance between two approximations.
///
/// The tolerance follows the precision the build was configured with rather than
/// being a constant, for the reason that file gives: the same source is compiled
/// both ways and a bound written for double precision would assert nothing at
/// all in single.

// Everything is inside the guard, including the includes. Without the backend
// this file has no cases at all, and headers included ahead of a block that is
// compiled out are unused by construction, which the lint job reports as errors.

#ifdef ORRERY_ENABLE_CUDA

#    include "orrery/solvers/cuda_direct_solver.hpp"

#    include <algorithm>
#    include <cmath>
#    include <cstdint>
#    include <memory>

#    include <catch2/catch_message.hpp>
#    include <catch2/catch_test_macros.hpp>

#    include "orrery/backend/cuda_device.hpp"
#    include "orrery/core/particle_data.hpp"
#    include "orrery/core/random.hpp"
#    include "orrery/core/softening.hpp"
#    include "orrery/core/types.hpp"
#    include "orrery/core/vec3.hpp"
#    include "orrery/initial_conditions/plummer.hpp"
#    include "orrery/solvers/direct_solver.hpp"
#    include "orrery/solvers/reference_kernel.hpp"

namespace {

using orrery::backend::to_string;
using orrery::core::Index;
using orrery::core::kSinglePrecision;
using orrery::core::norm;
using orrery::core::ParticleData;
using orrery::core::RandomSource;
using orrery::core::Real;
using orrery::core::Softening;
using orrery::core::Vec3;
using orrery::initial_conditions::make_plummer_sphere;
using orrery::initial_conditions::PlummerParameters;
using orrery::solvers::CudaDirectSolver;
using orrery::solvers::DirectSolver;
using orrery::solvers::reference_acceleration;
using orrery::solvers::ReferenceAcceleration;

/// The same seed the SYCL tests use, so that the two backends are measured on
/// the same particles rather than on two samples of the same distribution.
constexpr std::uint64_t kSeed = 20260811;

/// Softened throughout, for the reason the other backends' tests give: a close
/// pair in a sampled sphere would otherwise make this a measurement of how two
/// kernels rounded a near-singular term rather than of how they summed.
const Softening kSoftening{static_cast<Real>(0.05)};

struct ErrorSummary {
    double worst{};
    double root_mean_square{};
};

[[nodiscard]] ErrorSummary measure_error(const ParticleData& data, Softening softening) {
    ErrorSummary summary;
    double total = 0;

    for (Index i = 0; i < data.size(); ++i) {
        const ReferenceAcceleration exact =
            reference_acceleration(data.positions(), data.masses(), i, softening);
        const Vec3 measured = data.accelerations().get(i);

        const double dx = static_cast<double>(measured.x) - exact.x;
        const double dy = static_cast<double>(measured.y) - exact.y;
        const double dz = static_cast<double>(measured.z) - exact.z;

        const double magnitude =
            std::sqrt((exact.x * exact.x) + (exact.y * exact.y) + (exact.z * exact.z));
        if (magnitude == 0) {
            continue;
        }

        const double relative = std::sqrt((dx * dx) + (dy * dy) + (dz * dz)) / magnitude;
        summary.worst = std::max(summary.worst, relative);
        total += relative * relative;
    }

    if (data.size() > 0) {
        summary.root_mean_square = std::sqrt(total / static_cast<double>(data.size()));
    }
    return summary;
}

/// The bound the build's precision makes meaningful.
///
/// These are the SYCL solver's numbers, deliberately unchanged. The argument
/// behind them is about the arithmetic rather than about the vendor: in single
/// precision the accumulation rounds at about 1.2e-7 per operation and the
/// tile-wise reassociation lets that grow like the square root of the term
/// count, and in double precision the same argument at 2.2e-16 leaves an
/// enormous margin. Nothing in either statement mentions a device, which is why
/// the same bounds are the right ones to hold a second device to.
[[nodiscard]] constexpr double worst_case_bound() noexcept {
    return kSinglePrecision ? 2.0e-4 : 1.0e-10;
}

[[nodiscard]] constexpr double mean_square_bound() noexcept {
    return kSinglePrecision ? 2.0e-5 : 1.0e-11;
}

} // namespace

TEST_CASE("The CUDA solver agrees with the reference", "[solvers][cuda][validation]") {
    const std::unique_ptr<CudaDirectSolver> solver = CudaDirectSolver::try_create(kSoftening);
    if (solver == nullptr) {
        SKIP("no usable CUDA device on this machine");
    }

    INFO("device: " << to_string(solver->device()));
    INFO("block: " << solver->block_size());

    RandomSource random{kSeed};
    ParticleData data = make_plummer_sphere(PlummerParameters{.count = 4096}, random);

    solver->evaluate(data.positions(), data.masses(), data.accelerations());

    const ErrorSummary error = measure_error(data, kSoftening);
    INFO("worst relative error: " << error.worst);
    INFO("root mean square error: " << error.root_mean_square);

    REQUIRE(error.worst < worst_case_bound());
    REQUIRE(error.root_mean_square < mean_square_bound());
}

TEST_CASE("The CUDA solver agrees with the CPU solver", "[solvers][cuda][validation]") {
    const std::unique_ptr<CudaDirectSolver> gpu = CudaDirectSolver::try_create(kSoftening);
    if (gpu == nullptr) {
        SKIP("no usable CUDA device on this machine");
    }

    RandomSource random{kSeed};
    ParticleData on_gpu = make_plummer_sphere(PlummerParameters{.count = 2048}, random);

    RandomSource same_random{kSeed};
    ParticleData on_cpu = make_plummer_sphere(PlummerParameters{.count = 2048}, same_random);

    gpu->evaluate(on_gpu.positions(), on_gpu.masses(), on_gpu.accelerations());

    DirectSolver cpu{kSoftening};
    cpu.evaluate(on_cpu.positions(), on_cpu.masses(), on_cpu.accelerations());

    // The looser of the two comparisons, and it exists to catch a kernel that is
    // systematically offset rather than merely rounded differently: two answers
    // that each sit within tolerance of the truth sit within twice it of each
    // other, and anything worse means they are not approximating the same sum.
    double worst_between = 0;
    for (Index i = 0; i < on_gpu.size(); ++i) {
        const Vec3 from_gpu = on_gpu.accelerations().get(i);
        const Vec3 from_cpu = on_cpu.accelerations().get(i);

        const double magnitude = static_cast<double>(norm(from_cpu));
        if (magnitude == 0) {
            continue;
        }

        const Vec3 difference{from_gpu.x - from_cpu.x, from_gpu.y - from_cpu.y,
                              from_gpu.z - from_cpu.z};
        worst_between = std::max(worst_between, static_cast<double>(norm(difference)) / magnitude);
    }

    INFO("worst relative difference between backends: " << worst_between);
    REQUIRE(worst_between < 2 * worst_case_bound());
}

TEST_CASE("The CUDA solver conserves momentum", "[solvers][cuda][property]") {
    const std::unique_ptr<CudaDirectSolver> solver = CudaDirectSolver::try_create(kSoftening);
    if (solver == nullptr) {
        SKIP("no usable CUDA device on this machine");
    }

    RandomSource random{kSeed};
    ParticleData data = make_plummer_sphere(PlummerParameters{.count = 1024}, random);

    solver->evaluate(data.positions(), data.masses(), data.accelerations());

    // Every pair contributes equal and opposite forces, so the mass-weighted sum
    // of the accelerations is zero exactly. It is not zero in floating point,
    // and what remains is the accumulated rounding of the whole evaluation,
    // which makes this a sensitive check on the kernel having summed the terms
    // it was meant to and no others. A missing self-mask or a stale tile shows
    // up here immediately.
    Vec3 total{};
    double scale = 0;
    for (Index i = 0; i < data.size(); ++i) {
        const Vec3 acceleration = data.accelerations().get(i);
        const Real mass = data.masses()[i];
        total.x += mass * acceleration.x;
        total.y += mass * acceleration.y;
        total.z += mass * acceleration.z;
        scale += static_cast<double>(mass) * static_cast<double>(norm(acceleration));
    }

    const double residual = static_cast<double>(norm(total)) / scale;
    INFO("relative momentum residual: " << residual);
    REQUIRE(residual < (kSinglePrecision ? 1.0e-4 : 1.0e-12));
}

TEST_CASE("The CUDA solver reports what it did", "[solvers][cuda]") {
    const std::unique_ptr<CudaDirectSolver> solver = CudaDirectSolver::try_create(kSoftening);
    if (solver == nullptr) {
        SKIP("no usable CUDA device on this machine");
    }

    REQUIRE(solver->name() == "cuda-direct");
    REQUIRE(solver->softening().squared() == kSoftening.squared());
    REQUIRE(solver->block_size() > 0);

    RandomSource random{kSeed};
    ParticleData data = make_plummer_sphere(PlummerParameters{.count = 512}, random);
    solver->evaluate(data.positions(), data.masses(), data.accelerations());

    // N(N-1), the same closed form the CPU and SYCL solvers report, so a
    // benchmark table can put the three side by side.
    REQUIRE(solver->interaction_count().evaluations == 1);
    REQUIRE(solver->interaction_count().particle_particle == 512ULL * 511ULL);
    REQUIRE(solver->interaction_count().particle_cell == 0);

    // The property this backend's timings rest on, asked of the runtime after an
    // evaluation has actually allocated the arrays: the kernel reads device
    // memory and the host stages through pinned memory, so the transfer between
    // them is the one the two transfer columns describe rather than a page
    // migration hidden inside the kernel column.
    REQUIRE(solver->uses_device_memory());

    // Every part of an evaluation took some time, which is a weak assertion
    // about each field and a strong one about the breakdown: a timing structure
    // where a field was never assigned reads as an evaluation that skipped a
    // step, and on this backend the two transfer fields are exactly the steps a
    // reader is most likely to suspect are missing.
    const auto& timings = solver->timings();
    REQUIRE(timings.transfer_in.count() > 0);
    REQUIRE(timings.kernel.count() > 0);
    REQUIRE(timings.transfer_out.count() > 0);

    solver->reset_interaction_count();
    REQUIRE(solver->interaction_count().evaluations == 0);
    REQUIRE(solver->interaction_count().particle_particle == 0);
}

TEST_CASE("The CUDA solver handles configurations with nothing in them",
          "[solvers][cuda][property]") {
    const std::unique_ptr<CudaDirectSolver> solver = CudaDirectSolver::try_create();
    if (solver == nullptr) {
        SKIP("no usable CUDA device on this machine");
    }

    // An empty configuration reaches the solvers by the same path as any other,
    // and a launch over nothing is a runtime error rather than a no-op, so the
    // case is handled before submission.
    ParticleData empty;
    solver->evaluate(empty.positions(), empty.masses(), empty.accelerations());
    REQUIRE(solver->interaction_count().evaluations == 1);
    REQUIRE(solver->interaction_count().particle_particle == 0);

    // One particle has nothing to attract it, and the self-mask is the only
    // thing standing between that and a division by zero in an unsoftened run.
    ParticleData single;
    single.add(Vec3{1, 2, 3}, Vec3{}, Real{1});
    solver->evaluate(single.positions(), single.masses(), single.accelerations());

    const Vec3 acceleration = single.accelerations().get(0);
    REQUIRE(acceleration.x == Real{0});
    REQUIRE(acceleration.y == Real{0});
    REQUIRE(acceleration.z == Real{0});
}

TEST_CASE("A particle at the origin survives the padding on CUDA", "[solvers][cuda][regression]") {
    const std::unique_ptr<CudaDirectSolver> solver = CudaDirectSolver::try_create();
    if (solver == nullptr) {
        SKIP("no usable CUDA device on this machine");
    }

    // The case that caught a real defect in the SYCL kernel, carried over
    // because the CUDA kernel is the same kernel and would reproduce it. The
    // launch runs over whole blocks, so a particle count that is not a multiple
    // of the block leaves padded sources carrying zero mass at position zero.
    // Masking only their mass is not enough: a real particle sitting exactly at
    // the origin is at zero separation from every one of them, and in an
    // unsoftened run the reciprocal of that is infinite, so the contribution is
    // `inf * 0` and the whole acceleration becomes a NaN.
    //
    // It is not a contrived configuration. The central body of the Kepler
    // two-body problem sits at the origin, and no softening is exactly what the
    // analytic comparisons use.
    ParticleData data;
    data.add(Vec3{0, 0, 0}, Vec3{}, Real{1});
    data.add(Vec3{1, 0, 0}, Vec3{}, Real{1});
    data.add(Vec3{0, 2, 0}, Vec3{}, Real{1});

    solver->evaluate(data.positions(), data.masses(), data.accelerations());

    for (Index i = 0; i < data.size(); ++i) {
        const Vec3 acceleration = data.accelerations().get(i);
        INFO("particle " << i);
        REQUIRE(std::isfinite(acceleration.x));
        REQUIRE(std::isfinite(acceleration.y));
        REQUIRE(std::isfinite(acceleration.z));
    }

    // And the answer is right, not merely finite. The particle at the origin is
    // pulled by a unit mass at distance one along x and a unit mass at distance
    // two along y, so its acceleration is (1, 1/4, 0) exactly in these units.
    const Vec3 origin = data.accelerations().get(0);
    const auto tolerance = static_cast<Real>(kSinglePrecision ? 1.0e-6 : 1.0e-14);
    REQUIRE(std::abs(origin.x - Real{1}) < tolerance);
    REQUIRE(std::abs(origin.y - Real{0.25}) < tolerance);
    REQUIRE(std::abs(origin.z) < tolerance);
}

#endif // ORRERY_ENABLE_CUDA
