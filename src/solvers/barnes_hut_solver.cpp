#include "orrery/solvers/barnes_hut_solver.hpp"

#include <atomic>
#include <cstdint>
#include <span>

#include "orrery/backend/executor.hpp"
#include "orrery/backend/worker_statistics.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/units.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/solvers/direct_kernel.hpp"
#include "orrery/solvers/morton.hpp"
#include "orrery/solvers/tree_walk.hpp"

namespace orrery::solvers {

using core::Index;
using core::Real;
using core::Vec3;
using core::Vec3Span;

namespace {

using backend::Clock;

/// A phase boundary, so that the four timings below read as one sequence rather
/// than as four pairs of calls.
[[nodiscard]] backend::Duration since(Clock::time_point& mark) noexcept {
    const Clock::time_point now = Clock::now();
    const backend::Duration elapsed = now - mark;
    mark = now;

    return elapsed;
}

} // namespace

void BarnesHutSolver::gather(Vec3Span<const Real> positions, std::span<const Real> masses) {
    const std::span<const MortonKey> keys = ordering_.keys();
    const Index count = keys.size();

    sorted_x_.resize(count);
    sorted_y_.resize(count);
    sorted_z_.resize(count);
    sorted_mass_.resize(count);

    // Four sequential writes and four scattered reads per particle. The reads
    // are scattered because the permutation is arbitrary, and that is the whole
    // cost of the gather; it is paid once per evaluation against a traversal
    // that reads these arrays a few hundred times per particle, and if it were
    // not paid here every one of those reads would be scattered instead.
    for (Index i = 0; i < count; ++i) {
        const Index source = keys[i].index;
        sorted_x_[i] = positions.x[source];
        sorted_y_[i] = positions.y[source];
        sorted_z_[i] = positions.z[source];
        sorted_mass_[i] = masses[source];
    }
}

void BarnesHutSolver::evaluate(Vec3Span<const Real> positions, std::span<const Real> masses,
                               Vec3Span<Real> accelerations) {
    Clock::time_point mark = Clock::now();

    ordering_.build(positions, executor_);
    timings_.ordering = since(mark);

    gather(positions, masses);
    timings_.gathering = since(mark);

    const Vec3Span<const Real> sorted{sorted_x_, sorted_y_, sorted_z_};

    tree_.build(sorted, sorted_mass_, ordering_.keys(), ordering_.cube(), parameters_, executor_);
    timings_.construction = since(mark);

    // Resolved once per evaluation and read from a local afterwards, exactly as
    // in the direct solver: the cost of choosing an instruction set is paid
    // once against every pair at every leaf.
    const AccumulateRange accumulate = accumulate_range_for(kernel_);

    const std::span<const MortonKey> keys = ordering_.keys();

    // The two counters, summed once per chunk rather than once per interaction.
    // A counter incremented inside the walk would be a contended cache line
    // written billions of times per evaluation, and making it atomic would make
    // that worse rather than better. Relaxed ordering because these are
    // statistics: nothing reads them until the region has finished, and the
    // region's own completion is what publishes the writes.
    std::atomic<std::uint64_t> particle_particle{0};
    std::atomic<std::uint64_t> particle_cell{0};

    auto compute_targets = [&](Index begin, Index end) {
        WalkCounts counts;

        for (Index i = begin; i < end; ++i) {
            const Vec3 acceleration =
                walk_tree(tree_, sorted, sorted_mass_, i, softening_, accumulate, counts);

            // Scattered back to the caller's order as it is computed, rather
            // than into a sorted buffer that a fifth pass would then permute.
            // The write is three scattered stores per particle and the pass
            // saved would be three more of each.
            //
            // G is one in this project's units (ADR-0007). It is written for
            // the reason the direct solver writes it: an acceleration with no G
            // in it reads as though the physics had been forgotten.
            accelerations.set(keys[i].index, core::kGravitationalConstant * acceleration);
        }

        particle_particle.fetch_add(counts.particle_particle, std::memory_order_relaxed);
        particle_cell.fetch_add(counts.particle_cell, std::memory_order_relaxed);
    };

    const Index count = keys.size();

    if (executor_ == nullptr) {
        compute_targets(0, count);
    } else {
        executor_->run(count, compute_targets);
    }

    timings_.traversal = since(mark);

    ++count_.evaluations;
    count_.particle_particle += particle_particle.load(std::memory_order_relaxed);
    count_.particle_cell += particle_cell.load(std::memory_order_relaxed);
}

} // namespace orrery::solvers
