#include "orrery/solvers/direct_kernel.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/initial_conditions/plummer.hpp"
#include "orrery/solvers/direct_solver.hpp"

namespace {

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
using orrery::solvers::accumulate_range_for;
using orrery::solvers::accumulate_range_scalar;
using orrery::solvers::DirectSolver;
using orrery::solvers::fastest_available_kernel;
using orrery::solvers::kernel_available;
using orrery::solvers::kernel_lane_count;
using orrery::solvers::KernelKind;
using orrery::solvers::to_string;

constexpr std::uint64_t kSeed = 20260810;

constexpr Real kEpsilon = std::numeric_limits<Real>::epsilon();

const Softening kSoftening{static_cast<Real>(0.05)};

/// Every kernel this build can run here, scalar always first.
///
/// The list is short by construction and will stay short. Its point is that
/// every structural test below runs against all of them rather than against
/// whichever one the machine happens to default to, so a vector kernel with a
/// broken epilogue fails on the machine that has it rather than passing
/// everywhere it is not executed.
[[nodiscard]] std::vector<KernelKind> available_kernels() {
    std::vector<KernelKind> kernels;

    for (const KernelKind kind : {KernelKind::kScalar, KernelKind::kAvx2}) {
        if (kernel_available(kind)) {
            kernels.push_back(kind);
        }
    }

    return kernels;
}

} // namespace

TEST_CASE("the scalar kernel is always available", "[unit][solvers]") {
    // The property the whole dispatch rests on. Every other kernel is optional,
    // this one is the fallback for all of them, and a machine on which it were
    // absent would have no way to compute anything at all.
    REQUIRE(kernel_available(KernelKind::kScalar));
    REQUIRE(kernel_available(fastest_available_kernel()));

    REQUIRE(std::string{to_string(KernelKind::kScalar)} == "scalar");
    REQUIRE(std::string{to_string(KernelKind::kAvx2)} == "avx2");
}

TEST_CASE("an unavailable kernel resolves to the scalar one", "[unit][solvers]") {
    // Asking for something the machine cannot run has to produce correct
    // physics rather than a null pointer or a fault. The caller that needs to
    // know whether it got what it asked for is a benchmark, and it asks
    // separately.
    for (const KernelKind kind : {KernelKind::kScalar, KernelKind::kAvx2}) {
        CAPTURE(std::string{to_string(kind)}, kernel_available(kind));

        const auto resolved = accumulate_range_for(kind);
        REQUIRE(resolved != nullptr);

        if (!kernel_available(kind)) {
            REQUIRE(resolved == &accumulate_range_scalar);
        }
    }
}

TEST_CASE("the lane count describes the register rather than the build", "[unit][solvers]") {
    REQUIRE(kernel_lane_count(KernelKind::kScalar) == 1);

    // 256 bits either way, which is four doubles or eight floats. This is the
    // arithmetic half of what the single-precision build buys, and it is
    // asserted because a benchmark quotes it beside every vector row.
    REQUIRE(kernel_lane_count(KernelKind::kAvx2) == (kSinglePrecision ? 8 : 4));
    REQUIRE(kernel_lane_count(KernelKind::kAvx2) * sizeof(Real) == 32);
}

TEST_CASE("an empty source range contributes nothing", "[unit][solvers]") {
    // The case every target particle hits twice: the range before particle
    // zero and the range after the last one are both empty, and a kernel whose
    // loop bounds were computed by subtraction on unsigned indices would run
    // for four billion iterations rather than none.
    RandomSource random{kSeed};
    const ParticleData data = make_plummer_sphere(PlummerParameters{.count = 16}, random);

    for (const KernelKind kind : available_kernels()) {
        CAPTURE(std::string{to_string(kind)});

        const auto accumulate = accumulate_range_for(kind);
        const Vec3 target = data.positions().get(0);

        REQUIRE(accumulate(data.positions(), data.masses(), target, 0, 0, kSoftening) == Vec3{});
        REQUIRE(accumulate(data.positions(), data.masses(), target, 7, 7, kSoftening) == Vec3{});
    }
}

TEST_CASE("every kernel agrees with the scalar one at every remainder", "[unit][solvers]") {
    // This is the test the vector kernel exists to be caught by. It processes
    // whole registers of pairs and hands the leftover to the scalar kernel, so
    // there is a distinct code path for each range length modulo the lane
    // count, and a wrong boundary between the two would drop or double-count
    // pairs at some lengths and not at others.
    //
    // Rather than guessing which lengths matter, every length from nothing to
    // rather more than two full registers is checked, at two different starting
    // offsets so that a kernel which assumed its range began at zero is caught
    // as well.
    RandomSource random{kSeed};
    const ParticleData data = make_plummer_sphere(PlummerParameters{.count = 48}, random);

    const Vec3 target = data.positions().get(0);

    // The two kernels round differently, deliberately and by an amount that
    // grows with the number of terms. Over the very short ranges here that is a
    // handful of units in the last place, and anything structural is many
    // orders of magnitude larger, so the tolerance does not have to be tight to
    // catch what this test is for.
    const Real tolerance = 32 * kEpsilon;

    for (const KernelKind kind : available_kernels()) {
        const auto accumulate = accumulate_range_for(kind);

        for (const Index begin : {Index{1}, Index{5}}) {
            for (Index length = 0; length <= 20; ++length) {
                const Index end = begin + length;

                const Vec3 expected = accumulate_range_scalar(data.positions(), data.masses(),
                                                              target, begin, end, kSoftening);
                const Vec3 measured =
                    accumulate(data.positions(), data.masses(), target, begin, end, kSoftening);

                const Real magnitude = norm(expected);
                const Real error = norm(measured - expected);

                CAPTURE(std::string{to_string(kind)}, begin, length, error, magnitude);

                REQUIRE(error <= tolerance * (magnitude + kEpsilon));
            }
        }
    }
}

TEST_CASE("a kernel gives the same answer every time it is asked", "[regression][solvers]") {
    // Reproducibility is a property this project claims and therefore has to
    // assert. Nothing in either kernel is order-dependent across calls, so the
    // comparison is for equality rather than against a tolerance: a difference
    // of one bit between two evaluations of the same configuration would mean
    // the answer depended on something other than the inputs.
    RandomSource random{kSeed};
    ParticleData data = make_plummer_sphere(PlummerParameters{.count = 200}, random);

    for (const KernelKind kind : available_kernels()) {
        CAPTURE(std::string{to_string(kind)});

        DirectSolver solver{kSoftening};
        solver.select_kernel(kind);
        REQUIRE(solver.kernel() == kind);

        solver.evaluate(data.positions(), data.masses(), data.accelerations());

        std::vector<Vec3> first;
        first.reserve(data.size());
        for (Index i = 0; i < data.size(); ++i) {
            first.push_back(data.accelerations().get(i));
        }

        solver.evaluate(data.positions(), data.masses(), data.accelerations());

        for (Index i = 0; i < data.size(); ++i) {
            CAPTURE(i);
            REQUIRE(data.accelerations().get(i) == first[i]);
        }
    }
}

TEST_CASE("selecting a kernel the machine lacks falls back rather than fails", "[unit][solvers]") {
    DirectSolver solver;

    // The default is whatever this machine can run fastest, which is what a
    // simulation should get without asking.
    REQUIRE(solver.kernel() == fastest_available_kernel());
    REQUIRE(kernel_available(solver.kernel()));

    solver.select_kernel(KernelKind::kAvx2);
    REQUIRE(kernel_available(solver.kernel()));
    REQUIRE(solver.kernel() ==
            (kernel_available(KernelKind::kAvx2) ? KernelKind::kAvx2 : KernelKind::kScalar));

    // The scalar kernel can always be asked for, which is what lets the
    // accuracy tests and the benchmark measure it on a machine that would
    // otherwise never run it.
    solver.select_kernel(KernelKind::kScalar);
    REQUIRE(solver.kernel() == KernelKind::kScalar);
}
