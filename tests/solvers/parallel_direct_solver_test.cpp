#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/backend/executor.hpp"
#include "orrery/backend/serial_executor.hpp"
#include "orrery/backend/static_executor.hpp"
#include "orrery/backend/work_stealing_executor.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/initial_conditions/plummer.hpp"
#include "orrery/solvers/direct_solver.hpp"
#include "orrery/solvers/interaction_count.hpp"

namespace {

using orrery::backend::Executor;
using orrery::backend::SerialExecutor;
using orrery::backend::StaticExecutor;
using orrery::backend::WorkStealingExecutor;
using orrery::core::Index;
using orrery::core::norm;
using orrery::core::ParticleData;
using orrery::core::RandomSource;
using orrery::core::Real;
using orrery::core::Softening;
using orrery::core::Vec3;
using orrery::initial_conditions::make_plummer_sphere;
using orrery::initial_conditions::PlummerParameters;
using orrery::solvers::DirectSolver;
using orrery::solvers::InteractionCount;

constexpr std::uint64_t kSeed = 20260810;

/// Deliberately not a multiple of the cache line grain, and not divisible by any
/// of the worker counts below.
///
/// The partition rounds shares out to whole cache lines, so a count that divided
/// evenly would exercise only the case where every worker's range ends where the
/// arithmetic says it should. The remainder is where an off-by-one in the
/// division would show up, and it is the last worker's range that carries it.
constexpr Index kParticles = 777;

const Softening kSoftening{static_cast<Real>(0.03)};

/// A value no acceleration in this configuration could take.
///
/// The buffer is filled with it before every evaluation, so that an index no
/// worker wrote to is caught here rather than surviving as whatever the previous
/// evaluation left behind. A gap in the partition is otherwise invisible: the
/// answer for that particle would simply be one step out of date, which is a
/// difference no tolerance-based comparison would notice.
constexpr Real kPoison = -12345;

/// The accelerations of `data`, as one flat array of components.
[[nodiscard]] std::vector<Real> accelerations_from(DirectSolver& solver, ParticleData& data) {
    const auto accelerations = data.accelerations();

    for (Index i = 0; i < data.size(); ++i) {
        accelerations.set(i, Vec3{kPoison, kPoison, kPoison});
    }

    solver.evaluate(data.positions(), data.masses(), accelerations);

    std::vector<Real> flattened;
    flattened.reserve(3 * data.size());

    for (Index i = 0; i < data.size(); ++i) {
        flattened.push_back(accelerations.x[i]);
        flattened.push_back(accelerations.y[i]);
        flattened.push_back(accelerations.z[i]);
    }

    return flattened;
}

/// One executor of each shape worth distinguishing.
///
/// The worker counts are chosen against the target machine's eight logical
/// processors: one, so that a pool with nobody to steal from is covered; two and
/// three, which do not divide the particle count; eight, which is the
/// configuration the project actually runs; and sixteen, which oversubscribes
/// the machine and puts two workers on every core.
[[nodiscard]] std::vector<std::unique_ptr<Executor>> every_executor() {
    std::vector<std::unique_ptr<Executor>> executors;
    executors.push_back(std::make_unique<SerialExecutor>());

    for (const unsigned workers : {1U, 2U, 3U, 8U, 16U}) {
        executors.push_back(std::make_unique<StaticExecutor>(workers));
        executors.push_back(std::make_unique<WorkStealingExecutor>(workers));
    }

    return executors;
}

[[nodiscard]] std::string describe(const Executor& executor) {
    return std::string{executor.name()} + " x" + std::to_string(executor.worker_count());
}

} // namespace

TEST_CASE("a threaded evaluation is bit for bit the unthreaded one", "[regression][solvers]") {
    // The claim that lets the direct solver stay the project's reference after
    // being parallelised, and the reason it is asserted for equality rather than
    // against a tolerance.
    //
    // Particle i's acceleration is summed over j in index order, and which
    // worker happens to run iteration i changes nothing about that order. So the
    // answer does not depend on the thread count, on the chunk size, or on which
    // worker stole which chunk. A scheme that had introduced a dependence, by
    // splitting the inner sum across threads and combining the parts, would
    // still produce a correct acceleration and would fail here, which is the
    // point: reassociating the sum is a change to the reference and has to be a
    // decision rather than an accident.
    RandomSource random{kSeed};
    ParticleData data = make_plummer_sphere(PlummerParameters{.count = kParticles}, random);

    DirectSolver unthreaded{kSoftening};
    const std::vector<Real> expected = accelerations_from(unthreaded, data);

    // Nothing was left unwritten, which is what makes the comparisons below
    // comparisons rather than an agreement between two stale buffers.
    for (const Real component : expected) {
        REQUIRE(component != kPoison);
    }

    for (const std::unique_ptr<Executor>& executor : every_executor()) {
        DirectSolver threaded{kSoftening, *executor};
        const std::vector<Real> measured = accelerations_from(threaded, data);

        REQUIRE(measured.size() == expected.size());

        for (std::size_t component = 0; component < expected.size(); ++component) {
            CAPTURE(describe(*executor), component, component / 3, measured[component],
                    expected[component]);
            REQUIRE(measured[component] == expected[component]);
        }
    }
}

TEST_CASE("the answer does not change when the same solver is reused", "[regression][solvers]") {
    // A pool is built once and evaluated through many times, and the second
    // region reuses ranges and counters the first one left behind. If any of
    // that state were carried over, the second evaluation would differ from the
    // first, which is the shape a stale range or an unreset cursor would take.
    RandomSource random{kSeed};
    ParticleData data = make_plummer_sphere(PlummerParameters{.count = kParticles}, random);

    WorkStealingExecutor executor{8};
    DirectSolver solver{kSoftening, executor};

    const std::vector<Real> first = accelerations_from(solver, data);

    for (int repeat = 0; repeat < 20; ++repeat) {
        const std::vector<Real> again = accelerations_from(solver, data);

        for (std::size_t component = 0; component < first.size(); ++component) {
            CAPTURE(repeat, component, again[component], first[component]);
            REQUIRE(again[component] == first[component]);
        }
    }

    REQUIRE(executor.statistics().regions == 21);
}

TEST_CASE("threading does not change the work the algorithm did", "[unit][solvers]") {
    // The interaction count is the unit every cross-algorithm comparison in this
    // project is expressed in, and it is a property of the algorithm rather than
    // of the machine it ran on. Dividing the loop between eight threads must
    // therefore leave it exactly where it was.
    RandomSource random{kSeed};
    ParticleData data = make_plummer_sphere(PlummerParameters{.count = kParticles}, random);

    WorkStealingExecutor executor{8};

    DirectSolver unthreaded;
    DirectSolver threaded{executor};

    unthreaded.evaluate(data.positions(), data.masses(), data.accelerations());
    threaded.evaluate(data.positions(), data.masses(), data.accelerations());

    const InteractionCount serial_count = unthreaded.interaction_count();
    const InteractionCount parallel_count = threaded.interaction_count();

    REQUIRE(serial_count.evaluations == parallel_count.evaluations);
    REQUIRE(serial_count.particle_particle == parallel_count.particle_particle);
    REQUIRE(parallel_count.particle_particle == kParticles * (kParticles - 1));
}

TEST_CASE("momentum still cancels when the loop is threaded", "[property][solvers]") {
    // Implied by the bitwise test above, and asserted separately because it is
    // the physical statement rather than the numerical one. If the threading
    // were ever relaxed to a form that did not reproduce the serial answer
    // exactly, this is the property that would have to survive, and it should
    // fail on its own terms rather than only as a difference from a reference.
    RandomSource random{kSeed};
    ParticleData data = make_plummer_sphere(PlummerParameters{.count = kParticles}, random);

    WorkStealingExecutor executor{8};
    DirectSolver solver{kSoftening, executor};

    solver.evaluate(data.positions(), data.masses(), data.accelerations());

    const auto accelerations = data.accelerations();
    const auto masses = data.masses();

    Vec3 sum{};
    Real scale = 0;

    for (Index i = 0; i < data.size(); ++i) {
        const Vec3 term = masses[i] * accelerations.get(i);
        sum += term;
        scale += norm(term);
    }

    const Real residual = norm(sum) / scale;
    const Real tolerance =
        orrery::core::kSinglePrecision ? static_cast<Real>(1e-5) : static_cast<Real>(1e-13);

    CAPTURE(kSeed, residual, sum.x, sum.y, sum.z, scale);
    REQUIRE(residual <= tolerance);
}

TEST_CASE("a configuration smaller than the pool is not an error", "[unit][solvers]") {
    // Eight workers and two particles. Most of the pool gets an empty share,
    // which has to be a share of nothing rather than a range that wraps, and the
    // physics has to come out unchanged.
    ParticleData data;
    data.add(Vec3{-1, 0, 0}, Vec3{}, Real{2});
    data.add(Vec3{1, 0, 0}, Vec3{}, Real{3});

    WorkStealingExecutor executor{8};
    DirectSolver solver{executor};

    solver.evaluate(data.positions(), data.masses(), data.accelerations());

    REQUIRE(data.accelerations().get(0) == Vec3{Real{0.75}, 0, 0});
    REQUIRE(data.accelerations().get(1) == Vec3{Real{-0.5}, 0, 0});
}

TEST_CASE("an empty configuration reaches the executor harmlessly", "[unit][solvers]") {
    // An empty container is a configuration like any other and arrives by the
    // same path. The executor is asked for nothing and must not dispatch a
    // region for it, since a region over an empty range would have every worker
    // sweep every other looking for work that was never there.
    ParticleData data;

    WorkStealingExecutor executor{4};
    DirectSolver solver{executor};

    solver.evaluate(data.positions(), data.masses(), data.accelerations());

    REQUIRE(solver.interaction_count().evaluations == 1);
    REQUIRE(solver.interaction_count().particle_particle == 0);
    REQUIRE(executor.statistics().regions == 0);
}

TEST_CASE("the solver reports the executor it was given", "[unit][solvers]") {
    // A threading figure with no record of which scheme produced it is not a
    // measurement, so the benchmark has to be able to ask.
    WorkStealingExecutor executor{2};

    DirectSolver threaded{executor};
    DirectSolver unthreaded;

    REQUIRE(threaded.executor() == &executor);
    REQUIRE(unthreaded.executor() == nullptr);
}
