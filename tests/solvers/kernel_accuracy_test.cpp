#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/initial_conditions/plummer.hpp"
#include "orrery/solvers/direct_kernel.hpp"
#include "orrery/solvers/direct_solver.hpp"
#include "orrery/solvers/reference_kernel.hpp"

/// \file
/// What vectorising the kernel cost in accuracy, measured rather than assumed.
///
/// Phase 7 gives the direct solver a second kernel whose answer differs from
/// the first in the last bits, for two reasons set out in
/// `solvers/direct_kernel.hpp`: the sum is reassociated across lanes, and the
/// accumulation is fused. Neither is a relaxation of IEEE arithmetic and this
/// project enables no fast-math flag anywhere (ADR-0020), but "not a relaxation"
/// is an argument and not a measurement.
///
/// The measurement needs a third answer, because comparing the two kernels to
/// each other cannot say which is closer to the truth. That is
/// `solvers/reference_kernel.hpp`: the same physics summed with compensation in
/// double precision whatever the build's scalar type, whose own summation error
/// is negligible against what is being measured here.
///
/// The result the phase reports is that the vector kernel is not merely no
/// worse than the scalar one, it is better, because splitting a sum into
/// independent partial sums is what reduces accumulated rounding error. That is
/// asserted below rather than only written down.

namespace {

using orrery::core::Index;
using orrery::core::kSinglePrecision;
using orrery::core::ParticleData;
using orrery::core::RandomSource;
using orrery::core::Real;
using orrery::core::Softening;
using orrery::core::Vec3;
using orrery::initial_conditions::make_plummer_sphere;
using orrery::initial_conditions::PlummerParameters;
using orrery::solvers::DirectSolver;
using orrery::solvers::kernel_available;
using orrery::solvers::KernelKind;
using orrery::solvers::reference_acceleration;
using orrery::solvers::ReferenceAcceleration;
using orrery::solvers::to_string;

constexpr std::uint64_t kSeed = 20260810;

/// Large enough that the summation error has room to accumulate, since that is
/// the quantity under test, and small enough that a reference costing a branch
/// and a double-precision division per pair runs in well under a second.
constexpr Index kParticles = 2048;

const Softening kSoftening{static_cast<Real>(0.05)};

/// How wrong a kernel was, relative to the compensated answer.
struct ErrorSummary {
    double worst{};
    double root_mean_square{};
};

[[nodiscard]] double magnitude(ReferenceAcceleration acceleration) {
    return std::sqrt((acceleration.x * acceleration.x) + (acceleration.y * acceleration.y) +
                     (acceleration.z * acceleration.z));
}

/// Evaluate a configuration with one kernel and compare every particle against
/// the reference.
///
/// The error is relative to the magnitude of that particle's own acceleration
/// rather than to the largest in the configuration, which is the harder of the
/// two statements: a particle in the outskirts of a Plummer sphere feels a
/// small acceleration built from terms that largely cancel, and it is where a
/// summation error shows up first.
[[nodiscard]] ErrorSummary measure_kernel(ParticleData& data, KernelKind kind) {
    DirectSolver solver{kSoftening};
    solver.select_kernel(kind);
    solver.evaluate(data.positions(), data.masses(), data.accelerations());

    ErrorSummary summary;
    double sum_of_squares = 0;

    for (Index i = 0; i < data.size(); ++i) {
        const ReferenceAcceleration expected =
            reference_acceleration(data.positions(), data.masses(), i, kSoftening);
        const Vec3 measured = data.accelerations().get(i);

        const double dx = static_cast<double>(measured.x) - expected.x;
        const double dy = static_cast<double>(measured.y) - expected.y;
        const double dz = static_cast<double>(measured.z) - expected.z;

        const double relative = std::sqrt((dx * dx) + (dy * dy) + (dz * dz)) / magnitude(expected);

        summary.worst = std::max(summary.worst, relative);
        sum_of_squares += relative * relative;
    }

    summary.root_mean_square = std::sqrt(sum_of_squares / static_cast<double>(data.size()));
    return summary;
}

[[nodiscard]] ParticleData sphere() {
    RandomSource random{kSeed};
    return make_plummer_sphere(PlummerParameters{.count = kParticles}, random);
}

} // namespace

TEST_CASE("the compensated reference reproduces the analytic two-body result",
          "[validation][solvers]") {
    // The reference is the instrument the rest of this file measures with, so
    // it is checked against a case whose answer is known exactly before it is
    // trusted to judge anything. Two masses of 2 and 3 at a separation of 2
    // give accelerations of 3/4 and 1/2, and every intermediate value is a
    // small exact binary fraction, so the comparison is for equality.
    ParticleData data;
    data.add(Vec3{-1, 0, 0}, Vec3{}, Real{2});
    data.add(Vec3{1, 0, 0}, Vec3{}, Real{3});

    const ReferenceAcceleration first =
        reference_acceleration(data.positions(), data.masses(), 0, Softening{});
    const ReferenceAcceleration second =
        reference_acceleration(data.positions(), data.masses(), 1, Softening{});

    REQUIRE(first.x == 0.75);
    REQUIRE(first.y == 0.0);
    REQUIRE(first.z == 0.0);
    REQUIRE(second.x == -0.5);

    // A lone particle has nothing to attract it, which is the case where a sum
    // over "every particle except this one" is a sum over nothing.
    ParticleData lone;
    lone.add(Vec3{2, -3, 5}, Vec3{}, Real{4});
    const ReferenceAcceleration none =
        reference_acceleration(lone.positions(), lone.masses(), 0, kSoftening);
    REQUIRE(none.x == 0.0);
    REQUIRE(none.y == 0.0);
    REQUIRE(none.z == 0.0);
}

TEST_CASE("every kernel stays within the rounding its precision allows", "[validation][solvers]") {
    // The absolute statement: whichever kernel a machine runs, its answer is
    // correct to the precision the build was configured for, over a
    // configuration with the wide spread of separations a sampled Plummer
    // sphere produces.
    //
    // The bound is stated in units of the machine epsilon and the square root
    // of the particle count, which is how a rounding error accumulated over N
    // independent terms grows. The factor in front is loose enough to survive a
    // change of compiler and tight enough that a kernel summing the wrong terms
    // fails by orders of magnitude rather than marginally.
    ParticleData data = sphere();

    // Two numbers rather than one expression, because the expression would need
    // the machine epsilon widened to double and the widening is a cast in one
    // build and a no-op in the other. Each is 200 * sqrt(N) * epsilon at
    // N = 2048, rounded up: 200 * 45.3 * 2.22e-16 for double and the same
    // against 1.19e-7 for float.
    const double bound = kSinglePrecision ? 1.1e-3 : 2.1e-12;

    for (const KernelKind kind : {KernelKind::kScalar, KernelKind::kAvx2}) {
        if (!kernel_available(kind)) {
            continue;
        }

        const ErrorSummary summary = measure_kernel(data, kind);

        CAPTURE(kSeed, kParticles, std::string{to_string(kind)}, summary.worst,
                summary.root_mean_square, bound);

        REQUIRE(summary.worst < bound);
    }
}

TEST_CASE("vectorising the sum improves its accuracy rather than costing it",
          "[validation][solvers]") {
    // The comparative statement, and the one worth making. A vector kernel
    // keeps one partial sum per lane and adds them at the end, so a sum of n
    // terms becomes four or eight sums of n/4 or n/8. Rounding error in a
    // running sum accumulates with its length, so shorter sums are more
    // accurate, and the reassociation the vector kernel is often apologised for
    // is in fact where some of the error goes.
    //
    // Deterministic: one seed, one configuration, two kernels. If this ever
    // fails it is a change in the arithmetic rather than noise, and it should be
    // investigated rather than loosened.
    if (!kernel_available(KernelKind::kAvx2)) {
        SUCCEED("no vector kernel on this machine, so there is nothing to compare");
        return;
    }

    ParticleData data = sphere();

    const ErrorSummary scalar = measure_kernel(data, KernelKind::kScalar);
    const ErrorSummary vector = measure_kernel(data, KernelKind::kAvx2);

    CAPTURE(kSeed, kParticles, kSinglePrecision, scalar.worst, scalar.root_mean_square,
            vector.worst, vector.root_mean_square);

    REQUIRE(vector.root_mean_square <= scalar.root_mean_square);
}
