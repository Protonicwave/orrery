/// \file
/// What the GPU traversal gets right, and the sense in which it is the same
/// traversal as the CPU one.
///
/// Phase 9 validated its kernel by comparing accelerations against the
/// compensated reference within a tolerance, which is the right test for direct
/// summation because direct summation has one right answer. A tree walk does
/// not. Its answer depends on the opening angle, on the leaf capacity, on
/// whether quadrupole moments were computed and on the order the tree happened
/// to put the particles in, so a tolerance against the reference measures the
/// approximation rather than the implementation and would pass just as happily
/// for a traversal that visited the wrong cells.
///
/// So the substance of this file is a stronger claim, made three ways.
///
/// The GPU traversal computes the same sum as the CPU traversal. Not a sum
/// within tolerance of it: the same terms, over the same cells, in the same
/// order, differing only in how the device rounds. The evidence is that the
/// interaction counters agree exactly, which is a statement about which cells
/// were opened rather than about where the answer landed, and that the
/// accelerations then agree to the precision the build was configured with
/// rather than to the size of the tree's error.
///
/// The two device traversals compute the same sum as each other. That is the
/// property ADR-0029 exists to preserve and the reason the coherent walk masks
/// rather than voting to descend, so it is asserted here rather than described.
///
/// And the whole thing degenerates to direct summation when the opening angle
/// is zero, which is the instrument `tests/solvers/barnes_hut_test.cpp` uses on
/// the CPU walk: no cell is ever far enough away, every walk reaches the leaves,
/// and the tree computes exact direct summation by a slower route. A traversal
/// with a defect in its acceptance test passes none of the three.

// Everything is inside the guard, including the includes, for the reason
// `tests/solvers/sycl_direct_solver_test.cpp` gives: without the backend this
// file has no cases, and headers included ahead of a block that is compiled out
// are unused by construction, which the lint job reports as errors.

#ifdef ORRERY_ENABLE_SYCL

#    include "orrery/solvers/sycl_tree_solver.hpp"

#    include <algorithm>
#    include <cmath>
#    include <cstdint>
#    include <memory>
#    include <span>
#    include <vector>

#    include <catch2/catch_message.hpp>
#    include <catch2/catch_test_macros.hpp>

#    include "orrery/backend/sycl_device.hpp"
#    include "orrery/core/particle_data.hpp"
#    include "orrery/core/random.hpp"
#    include "orrery/core/softening.hpp"
#    include "orrery/core/types.hpp"
#    include "orrery/core/vec3.hpp"
#    include "orrery/initial_conditions/plummer.hpp"
#    include "orrery/solvers/barnes_hut_solver.hpp"
#    include "orrery/solvers/direct_kernel.hpp"
#    include "orrery/solvers/direct_solver.hpp"
#    include "orrery/solvers/interaction_count.hpp"
#    include "orrery/solvers/octree.hpp"
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
using orrery::solvers::BarnesHutSolver;
using orrery::solvers::DirectSolver;
using orrery::solvers::InteractionCount;
using orrery::solvers::KernelKind;
using orrery::solvers::reference_acceleration;
using orrery::solvers::ReferenceAcceleration;
using orrery::solvers::SyclTreeSolver;
using orrery::solvers::TreeParameters;
using orrery::solvers::TreeTraversal;

constexpr std::uint64_t kSeed = 20260811;

/// Softened throughout, for the reason the tree tests on the CPU give: a close
/// pair in a sampled sphere would otherwise make every comparison below a
/// measurement of how two summations rounded a near-singular term.
const Softening kSoftening{static_cast<Real>(0.05)};

[[nodiscard]] ParticleData sampled_sphere(Index count) {
    RandomSource random{kSeed};
    return make_plummer_sphere(PlummerParameters{.count = count}, random);
}

/// The largest relative difference between two sets of accelerations.
///
/// Relative to the size of the second, which is the one being treated as the
/// answer in every use below.
[[nodiscard]] double worst_difference(const ParticleData& measured, const ParticleData& against) {
    double worst = 0;

    for (Index i = 0; i < against.size(); ++i) {
        const Vec3 a = measured.accelerations().get(i);
        const Vec3 b = against.accelerations().get(i);

        const double magnitude = static_cast<double>(norm(b));
        if (magnitude == 0) {
            continue;
        }

        const Vec3 difference{a.x - b.x, a.y - b.y, a.z - b.z};
        worst = std::max(worst, static_cast<double>(norm(difference)) / magnitude);
    }

    return worst;
}

/// How wrong an answer is against the compensated reference, summarised.
struct ErrorSummary {
    double worst{};
    double root_mean_square{};
};

[[nodiscard]] ErrorSummary measure_error(const ParticleData& data) {
    ErrorSummary summary;
    double total = 0;
    Index samples = 0;

    for (Index i = 0; i < data.size(); ++i) {
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

        const double relative = std::sqrt((dx * dx) + (dy * dy) + (dz * dz)) / magnitude;
        summary.worst = std::max(summary.worst, relative);
        total += relative * relative;
        ++samples;
    }

    if (samples > 0) {
        summary.root_mean_square = std::sqrt(total / static_cast<double>(samples));
    }
    return summary;
}

/// The bound on two summations of the same terms disagreeing.
///
/// This is a rounding tolerance and not an approximation tolerance, which is the
/// distinction the whole file rests on. The two walks sum identical terms in
/// identical order, so what separates them is that the device is entitled to
/// contract a multiply and an add into a fused multiply-add where the host did
/// not. That perturbs each term by at most one rounding, and a walk accumulates
/// a few hundred of them.
///
/// Set an order of magnitude above the scale that argument predicts, and the
/// measured value is reported on every run so that a regression shows up as a
/// number moving rather than as a bound being crossed.
[[nodiscard]] constexpr double rounding_bound() noexcept {
    return kSinglePrecision ? 1.0e-5 : 1.0e-12;
}

} // namespace

TEST_CASE("The GPU tree solver computes the same sum as the CPU tree solver",
          "[solvers][sycl][tree][validation]") {
    const std::unique_ptr<SyclTreeSolver> gpu =
        SyclTreeSolver::try_create(TreeParameters{}, kSoftening);
    if (gpu == nullptr) {
        SKIP("no usable SYCL GPU on this machine");
    }

    INFO("device: " << to_string(gpu->device()));
    INFO("work-group: " << gpu->work_group_size());

    ParticleData on_gpu = sampled_sphere(4096);
    ParticleData on_cpu = sampled_sphere(4096);

    gpu->evaluate(on_gpu.positions(), on_gpu.masses(), on_gpu.accelerations());

    // The scalar kernel at the leaves rather than the vectorised one, so that
    // the CPU sums the pairs of a leaf in the same order the device does. With
    // the AVX2 kernel the comparison would still pass, and it would be measuring
    // Phase 7's reassociation as well as this phase's arithmetic. One difference
    // at a time.
    BarnesHutSolver cpu{TreeParameters{}, kSoftening};
    cpu.select_kernel(KernelKind::kScalar);
    cpu.evaluate(on_cpu.positions(), on_cpu.masses(), on_cpu.accelerations());

    // The tree first. Both solvers order the particles and build over them with
    // the same code, so anything but equality here means the GPU solver walked a
    // different tree and the comparison below would be meaningless.
    REQUIRE(gpu->tree().nodes().size() == cpu.tree().nodes().size());
    REQUIRE(gpu->tree().leaf_count() == cpu.tree().leaf_count());
    REQUIRE(gpu->tree().depth() == cpu.tree().depth());

    // Then which cells were opened, which is the claim a tolerance cannot make.
    // Two traversals that agree on these counters visited the same cells and
    // summed the same pairs; two that do not have taken different routes through
    // the tree whatever their accelerations look like.
    const InteractionCount from_gpu = gpu->interaction_count();
    const InteractionCount from_cpu = cpu.interaction_count();

    INFO("gpu pairs " << from_gpu.particle_particle << ", cells " << from_gpu.particle_cell);
    INFO("cpu pairs " << from_cpu.particle_particle << ", cells " << from_cpu.particle_cell);

    REQUIRE(from_gpu.particle_particle == from_cpu.particle_particle);
    REQUIRE(from_gpu.particle_cell == from_cpu.particle_cell);

    // And only then the numbers, which now have to agree to rounding rather than
    // to the size of the tree's approximation.
    const double worst = worst_difference(on_gpu, on_cpu);
    INFO("worst relative difference: " << worst);
    REQUIRE(worst < rounding_bound());
}

TEST_CASE("The two device traversals compute the same sum", "[solvers][sycl][tree][validation]") {
    const std::unique_ptr<SyclTreeSolver> solver =
        SyclTreeSolver::try_create(TreeParameters{}, kSoftening);
    if (solver == nullptr) {
        SKIP("no usable SYCL GPU on this machine");
    }

    ParticleData coherent = sampled_sphere(4096);
    ParticleData independent = sampled_sphere(4096);

    solver->select_traversal(TreeTraversal::kCoherent);
    solver->evaluate(coherent.positions(), coherent.masses(), coherent.accelerations());
    const InteractionCount after_coherent = solver->interaction_count();
    const std::uint64_t coherent_visits = solver->node_visits();

    solver->reset_interaction_count();

    solver->select_traversal(TreeTraversal::kIndependent);
    solver->evaluate(independent.positions(), independent.masses(), independent.accelerations());
    const InteractionCount after_independent = solver->interaction_count();
    const std::uint64_t independent_visits = solver->node_visits();

    // The point of ADR-0029. The published warp-coherent traversals let a lane
    // that would have accepted a cell descend with the rest of its warp, which
    // computes a more accurate sum that depends on which particles shared a
    // warp. This one masks instead, so the two traversals are the same function
    // of the input and these counters have to be equal.
    REQUIRE(after_coherent.particle_particle == after_independent.particle_particle);
    REQUIRE(after_coherent.particle_cell == after_independent.particle_cell);

    const double worst = worst_difference(coherent, independent);
    INFO("worst relative difference between traversals: " << worst);
    REQUIRE(worst < rounding_bound());

    // What the two do not share is how many nodes they step through, which is
    // the cost the coherence pays and the quantity `docs/performance/sycl_tree.md`
    // reports beside the time it buys. The coherent walk visits the union of its
    // sub-group's walks, counted once per lane, so it can only be the larger.
    INFO("coherent visits " << coherent_visits << ", independent visits " << independent_visits);
    REQUIRE(independent_visits > 0);
    REQUIRE(coherent_visits >= independent_visits);
}

TEST_CASE("Every sub-group width the device offers computes the same sum",
          "[solvers][sycl][tree][validation]") {
    const std::unique_ptr<SyclTreeSolver> solver =
        SyclTreeSolver::try_create(TreeParameters{}, kSoftening);
    if (solver == nullptr) {
        SKIP("no usable SYCL GPU on this machine");
    }

    ParticleData reference = sampled_sphere(2048);
    solver->select_traversal(TreeTraversal::kIndependent);
    solver->evaluate(reference.positions(), reference.masses(), reference.accelerations());

    solver->select_traversal(TreeTraversal::kCoherent);

    // The widths are a sweep in the benchmark, so they have to be a correctness
    // case here: the coherence granularity changes which lanes walk together and
    // therefore which nodes the group visits, and it must change nothing else.
    const std::vector<unsigned> widths{solver->supported_sub_group_widths().begin(),
                                       solver->supported_sub_group_widths().end()};
    INFO("widths offered: " << widths.size());

    for (const unsigned width : widths) {
        solver->select_sub_group_width(width);
        REQUIRE(solver->sub_group_width() == width);

        ParticleData data = sampled_sphere(2048);
        solver->evaluate(data.positions(), data.masses(), data.accelerations());

        const double worst = worst_difference(data, reference);
        INFO("width " << width << ", worst relative difference " << worst);
        REQUIRE(worst < rounding_bound());
    }

    // A width the hardware does not implement is answered with the compiler's
    // own choice rather than with a submission that throws, on the same terms as
    // asking the direct solver for a kernel this processor cannot run.
    solver->select_sub_group_width(3);
    REQUIRE(solver->sub_group_width() == 0);
}

TEST_CASE("An opening angle of zero reduces the GPU tree to direct summation",
          "[solvers][sycl][tree][validation]") {
    const std::unique_ptr<SyclTreeSolver> gpu =
        SyclTreeSolver::try_create(TreeParameters{.opening_angle = 0}, kSoftening);
    if (gpu == nullptr) {
        SKIP("no usable SYCL GPU on this machine");
    }

    // No cell is ever far enough away, so every walk descends to the leaves and
    // the tree computes exact direct summation by a slower route. A defect in
    // the acceptance test, in the escape index or in the skip mask shows up here
    // as an answer that is merely close rather than as one that rounds
    // differently.
    ParticleData on_gpu = sampled_sphere(1024);
    ParticleData on_cpu = sampled_sphere(1024);

    gpu->evaluate(on_gpu.positions(), on_gpu.masses(), on_gpu.accelerations());

    DirectSolver direct{kSoftening};
    direct.select_kernel(KernelKind::kScalar);
    direct.evaluate(on_cpu.positions(), on_cpu.masses(), on_cpu.accelerations());

    // N(N-1) pairs and not a single cell, which is what an opening angle of zero
    // means and is checkable exactly.
    REQUIRE(gpu->interaction_count().particle_particle == 1024ULL * 1023ULL);
    REQUIRE(gpu->interaction_count().particle_cell == 0);

    const double worst = worst_difference(on_gpu, on_cpu);
    INFO("worst relative difference against direct summation: " << worst);
    REQUIRE(worst < rounding_bound());
}

TEST_CASE("The GPU tree solver carries the quadrupole moments",
          "[solvers][sycl][tree][validation]") {
    const std::unique_ptr<SyclTreeSolver> monopole =
        SyclTreeSolver::try_create(TreeParameters{}, kSoftening);
    if (monopole == nullptr) {
        SKIP("no usable SYCL GPU on this machine");
    }

    const std::unique_ptr<SyclTreeSolver> quadrupole =
        SyclTreeSolver::try_create(TreeParameters{.quadrupole = true}, kSoftening);
    REQUIRE(quadrupole != nullptr);

    ParticleData without = sampled_sphere(4096);
    ParticleData with = sampled_sphere(4096);

    monopole->evaluate(without.positions(), without.masses(), without.accelerations());
    quadrupole->evaluate(with.positions(), with.masses(), with.accelerations());

    const ErrorSummary coarse = measure_error(without);
    const ErrorSummary fine = measure_error(with);

    INFO("monopole rms " << coarse.root_mean_square << ", quadrupole rms "
                         << fine.root_mean_square);

    // The next term of the same expansion, so it has to make the answer better
    // at a fixed opening angle. It is an accuracy option rather than an
    // improvement, which ADR-0024 records, and this asserts the accuracy half of
    // that: a device walk that read the moments from the wrong node, or ignored
    // them, would fail here while passing every comparison that does not switch
    // them on.
    REQUIRE(fine.root_mean_square < coarse.root_mean_square);

    // The counters are unaffected: the same cells are accepted either way, and
    // what changes is how much arithmetic each accepted cell costs.
    REQUIRE(quadrupole->interaction_count().particle_cell ==
            monopole->interaction_count().particle_cell);
}

TEST_CASE("The GPU tree solver approximates the true accelerations",
          "[solvers][sycl][tree][validation]") {
    const std::unique_ptr<SyclTreeSolver> solver =
        SyclTreeSolver::try_create(TreeParameters{}, kSoftening);
    if (solver == nullptr) {
        SKIP("no usable SYCL GPU on this machine");
    }

    ParticleData data = sampled_sphere(4096);
    solver->evaluate(data.positions(), data.masses(), data.accelerations());

    const ErrorSummary error = measure_error(data);
    INFO("worst " << error.worst << ", rms " << error.root_mean_square);

    // The bound this case asserts is the approximation's, not the arithmetic's,
    // and it is loose on purpose: what it exists to catch is a traversal that
    // has stopped computing gravity, not one whose error has moved in the third
    // figure. `docs/performance/sycl_tree.md` reports where the error actually
    // sits, and the case above is where a small regression would surface.
    REQUIRE(error.root_mean_square < 1.0e-2);
    REQUIRE(error.worst < 1.0e-1);
}

TEST_CASE("The GPU tree solver reports what it did", "[solvers][sycl][tree]") {
    const std::unique_ptr<SyclTreeSolver> solver =
        SyclTreeSolver::try_create(TreeParameters{}, kSoftening);
    if (solver == nullptr) {
        SKIP("no usable SYCL GPU on this machine");
    }

    REQUIRE(solver->name() == "sycl-tree");
    REQUIRE(solver->softening().squared() == kSoftening.squared());
    REQUIRE(solver->work_group_size() > 0);
    REQUIRE(solver->traversal() == TreeTraversal::kCoherent);

    // An opening angle above one is reduced to one rather than rejected, on the
    // same terms the CPU tree solver applies it, and the solver reports what it
    // settled on.
    const std::unique_ptr<SyclTreeSolver> clamped =
        SyclTreeSolver::try_create(TreeParameters{.opening_angle = 2});
    REQUIRE(clamped != nullptr);
    REQUIRE(clamped->parameters().opening_angle == Real{1});

    ParticleData data = sampled_sphere(512);
    solver->evaluate(data.positions(), data.masses(), data.accelerations());

    REQUIRE(solver->interaction_count().evaluations == 1);
    REQUIRE(solver->interaction_count().particle_particle > 0);
    REQUIRE(solver->interaction_count().particle_cell > 0);
    REQUIRE(solver->node_visits() > 0);

    // The zero-copy property, asked of the node array as well as of the
    // particles, because the node array is the allocation this phase adds.
    REQUIRE(solver->uses_shared_memory());

    solver->reset_interaction_count();
    REQUIRE(solver->interaction_count().evaluations == 0);
    REQUIRE(solver->interaction_count().particle_particle == 0);
    REQUIRE(solver->node_visits() == 0);
}

TEST_CASE("The GPU tree solver handles configurations with nothing in them",
          "[solvers][sycl][tree][property]") {
    const std::unique_ptr<SyclTreeSolver> solver = SyclTreeSolver::try_create();
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

    // One particle is a tree of one leaf holding one particle, which the walk
    // opens and then sums over the empty range either side of the target. The
    // self-mask is the only thing standing between that and a division by zero,
    // and this solver was constructed without softening so nothing else is.
    ParticleData single;
    single.add(Vec3{1, 2, 3}, Vec3{}, Real{1});
    solver->evaluate(single.positions(), single.masses(), single.accelerations());

    const Vec3 acceleration = single.accelerations().get(0);
    REQUIRE(acceleration.x == Real{0});
    REQUIRE(acceleration.y == Real{0});
    REQUIRE(acceleration.z == Real{0});
}

TEST_CASE("A particle at the origin survives the padded launch",
          "[solvers][sycl][tree][regression]") {
    const std::unique_ptr<SyclTreeSolver> solver = SyclTreeSolver::try_create();
    if (solver == nullptr) {
        SKIP("no usable SYCL GPU on this machine");
    }

    // The case that caught a real defect in the Phase 9 kernel, repeated here
    // because this traversal pads its launch for the same reason and would fail
    // the same way. A particle count that is not a multiple of the work-group
    // leaves lanes with no particle of their own, and the coherent walk reads
    // their positions unconditionally so that they can take part in the
    // collectives. Those positions are zero, and a real particle at the origin
    // is at zero separation from them.
    //
    // It is not a contrived configuration. The central body of the Kepler
    // two-body problem sits at the origin and no softening is what the analytic
    // comparisons use, so this is the shape of the project's primary validation
    // instrument.
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

TEST_CASE("The GPU tree solver drifts in momentum only as far as the tree does",
          "[solvers][sycl][tree][property]") {
    const std::unique_ptr<SyclTreeSolver> solver =
        SyclTreeSolver::try_create(TreeParameters{}, kSoftening);
    if (solver == nullptr) {
        SKIP("no usable SYCL GPU on this machine");
    }

    ParticleData data = sampled_sphere(4096);
    solver->evaluate(data.positions(), data.masses(), data.accelerations());

    // Direct summation computes each pair from both ends, so its mass-weighted
    // sum of accelerations is zero to round-off. A tree evaluation has no such
    // symmetry: particle i may see j through a cell while j sees i directly.
    // `solvers/barnes_hut_solver.hpp` sets that out as a property of the method,
    // and asserting the residual were zero would be asserting something false.
    //
    // What is asserted is that it stays at the size the approximation implies.
    // A device walk that dropped or double-counted terms would push it well
    // past this, which makes the case a sensitive check on the skip mask.
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
    REQUIRE(residual < 1.0e-3);
}

#endif // ORRERY_ENABLE_SYCL
