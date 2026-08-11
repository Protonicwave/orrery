/// \file
/// What the GPU kernel gets right, measured against the project's reference.
///
/// Phase 9's definition of done asks for the GPU result to match the CPU within
/// a stated tolerance. The statement of that tolerance is the substance of this
/// file, so it is worth being precise about what is being compared with what.
///
/// The comparison is not GPU against CPU. Two approximate answers agreeing tells
/// you they are wrong in the same way, not that either is right, and the AVX2
/// kernel of Phase 7 already established the pattern this project uses instead:
/// both are compared against `solvers/reference_kernel.hpp`, which sums the same
/// softened force law with compensation in double precision whatever the build's
/// scalar type. Its own summation error is negligible against what is being
/// measured, so it stands in for the exact answer.
///
/// The tolerance follows the precision the build was configured with rather than
/// being a constant, because the same source is compiled both ways and a bound
/// written for double precision would assert nothing at all in single. In single
/// precision the expected scale is the rounding of the accumulation itself,
/// which for a sum of N terms reassociated across a tile grows like the square
/// root of N times the machine epsilon. The bounds below sit above that with
/// room, and the measured values are reported on every run so that a regression
/// shows up as a number moving rather than as a bound being crossed.

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/backend/sycl_device.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/initial_conditions/plummer.hpp"
#include "orrery/solvers/direct_solver.hpp"
#include "orrery/solvers/reference_kernel.hpp"

#ifdef ORRERY_ENABLE_SYCL

#include "orrery/solvers/sycl_direct_solver.hpp"

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
using orrery::solvers::DirectSolver;
using orrery::solvers::reference_acceleration;
using orrery::solvers::ReferenceAcceleration;
using orrery::solvers::SyclDirectSolver;

constexpr std::uint64_t kSeed = 20260811;

/// Softened throughout, for the reason the tree tests give: a close pair in a
/// sampled sphere would otherwise make this a measurement of how two kernels
/// rounded a near-singular term rather than of how they summed.
const Softening kSoftening{static_cast<Real>(0.05)};

/// How wrong an answer is, relative to the compensated reference.
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

        const double magnitude = std::sqrt((exact.x * exact.x) + (exact.y * exact.y) +
                                           (exact.z * exact.z));
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
/// Single precision: the accumulation rounds at about 1.2e-7 per operation and
/// the tile-wise reassociation lets that grow like the square root of the term
/// count, so a few parts in a million is the expected scale at these sizes and
/// the bound sits an order of magnitude above it.
///
/// Double precision: the same argument at 2.2e-16 leaves an enormous margin, and
/// the bound is set to catch a kernel that is wrong rather than one that rounds
/// differently.
[[nodiscard]] constexpr double worst_case_bound() noexcept {
    return kSinglePrecision ? 2.0e-4 : 1.0e-10;
}

[[nodiscard]] constexpr double mean_square_bound() noexcept {
    return kSinglePrecision ? 2.0e-5 : 1.0e-11;
}

} // namespace

TEST_CASE("The GPU solver agrees with the reference", "[solvers][sycl][validation]") {
    const std::unique_ptr<SyclDirectSolver> solver = SyclDirectSolver::try_create(kSoftening);
    if (solver == nullptr) {
        SKIP("no usable SYCL GPU on this machine");
    }

    INFO("device: " << to_string(solver->device()));
    INFO("tile: " << solver->tile_size());

    RandomSource random{kSeed};
    ParticleData data = make_plummer_sphere(PlummerParameters{.count = 4096}, random);

    solver->evaluate(data.positions(), data.masses(), data.accelerations());

    const ErrorSummary error = measure_error(data, kSoftening);
    INFO("worst relative error: " << error.worst);
    INFO("root mean square error: " << error.root_mean_square);

    REQUIRE(error.worst < worst_case_bound());
    REQUIRE(error.root_mean_square < mean_square_bound());
}

TEST_CASE("The GPU solver agrees with the CPU solver", "[solvers][sycl][validation]") {
    const std::unique_ptr<SyclDirectSolver> gpu = SyclDirectSolver::try_create(kSoftening);
    if (gpu == nullptr) {
        SKIP("no usable SYCL GPU on this machine");
    }

    RandomSource random{kSeed};
    ParticleData on_gpu = make_plummer_sphere(PlummerParameters{.count = 2048}, random);

    RandomSource same_random{kSeed};
    ParticleData on_cpu = make_plummer_sphere(PlummerParameters{.count = 2048}, same_random);

    gpu->evaluate(on_gpu.positions(), on_gpu.masses(), on_gpu.accelerations());

    DirectSolver cpu{kSoftening};
    cpu.evaluate(on_cpu.positions(), on_cpu.masses(), on_cpu.accelerations());

    // Both against the reference, and then against each other. The second
    // comparison is the looser one and exists to catch a kernel that is
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
        worst_between =
            std::max(worst_between, static_cast<double>(norm(difference)) / magnitude);
    }

    INFO("worst relative difference between backends: " << worst_between);
    REQUIRE(worst_between < 2 * worst_case_bound());
}

TEST_CASE("The GPU solver conserves momentum", "[solvers][sycl][property]") {
    const std::unique_ptr<SyclDirectSolver> solver = SyclDirectSolver::try_create(kSoftening);
    if (solver == nullptr) {
        SKIP("no usable SYCL GPU on this machine");
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

TEST_CASE("The GPU solver reports what it did", "[solvers][sycl]") {
    const std::unique_ptr<SyclDirectSolver> solver = SyclDirectSolver::try_create(kSoftening);
    if (solver == nullptr) {
        SKIP("no usable SYCL GPU on this machine");
    }

    REQUIRE(solver->name() == "sycl-direct");
    REQUIRE(solver->softening().squared() == kSoftening.squared());
    REQUIRE(solver->tile_size() > 0);

    RandomSource random{kSeed};
    ParticleData data = make_plummer_sphere(PlummerParameters{.count = 512}, random);
    solver->evaluate(data.positions(), data.masses(), data.accelerations());

    // N(N-1), the same closed form the CPU solver reports, so a benchmark table
    // can put the two side by side.
    REQUIRE(solver->interaction_count().evaluations == 1);
    REQUIRE(solver->interaction_count().particle_particle == 512ULL * 511ULL);
    REQUIRE(solver->interaction_count().particle_cell == 0);

    // The zero-copy property, asked of the runtime after an evaluation has
    // actually allocated the arrays. `tests/backend/sycl_usm_test.cpp` makes the
    // fuller demonstration; this asserts that the solver is using the kind of
    // memory that demonstration is about.
    REQUIRE(solver->uses_shared_memory());

    solver->reset_interaction_count();
    REQUIRE(solver->interaction_count().evaluations == 0);
    REQUIRE(solver->interaction_count().particle_particle == 0);
}

TEST_CASE("The GPU solver handles configurations with nothing in them",
          "[solvers][sycl][property]") {
    const std::unique_ptr<SyclDirectSolver> solver = SyclDirectSolver::try_create();
    if (solver == nullptr) {
        SKIP("no usable SYCL GPU on this machine");
    }

    // An empty configuration reaches the solvers by the same path as any other,
    // and launching a kernel over nothing is a runtime error on some devices
    // rather than a no-op, so the case is handled before submission.
    ParticleData empty;
    solver->evaluate(empty.positions(), empty.masses(), empty.accelerations());
    REQUIRE(solver->interaction_count().evaluations == 1);
    REQUIRE(solver->interaction_count().particle_particle == 0);

    // One particle has nothing to attract it, and the self-mask is the only
    // thing standing between that and a division by zero in an unsoftened run.
    // This is the case that fails loudly if the mask is ever removed.
    ParticleData single;
    single.add(Vec3{1, 2, 3}, Vec3{}, Real{1});
    solver->evaluate(single.positions(), single.masses(), single.accelerations());

    const Vec3 acceleration = single.accelerations().get(0);
    REQUIRE(acceleration.x == Real{0});
    REQUIRE(acceleration.y == Real{0});
    REQUIRE(acceleration.z == Real{0});
}

#endif // ORRERY_ENABLE_SYCL
