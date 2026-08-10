#include "orrery/solvers/morton.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "orrery/backend/executor.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/core/vec3_span.hpp"

namespace orrery::solvers {

using core::Index;
using core::Real;
using core::Vec3;
using core::Vec3Span;

namespace {

/// The smallest number of sorted runs worth merging in parallel.
///
/// Below this the sort is a few microseconds and the cost of handing work to a
/// pool, waking eight threads and waiting for the slowest of them exceeds what
/// they save. The figure is not critical: it only decides which side of a
/// boundary a sort of a few thousand particles falls on, and both sides are
/// fast there.
constexpr Index kParallelSortThreshold = 1 << 14;

/// The boundary between run `run` and the one after it, out of `runs`.
///
/// Written as one expression used by every phase of the merge, because the
/// merging below depends on a property of this particular formula: with `runs`
/// a power of two, the boundaries of a round with half as many runs are exactly
/// the even-numbered boundaries of the round before it. That is what lets a
/// round merge pairs of adjacent runs in place of a bookkeeping structure
/// describing where the runs are.
[[nodiscard]] Index run_boundary(Index count, Index runs, Index run) noexcept {
    return count * run / runs;
}

/// The number of runs to start a parallel merge sort with.
///
/// A power of two so that every round halves it exactly, at least as large as
/// the worker count so that no worker is idle in the first round, and bounded
/// below by a run length worth sorting on its own.
[[nodiscard]] Index initial_run_count(Index count, unsigned workers) noexcept {
    Index runs = 1;
    while (runs * 2 <= workers && count / (runs * 2) >= kParallelSortThreshold / 2) {
        runs *= 2;
    }

    return runs;
}

} // namespace

BoundingCube bounding_cube(Vec3Span<const Real> positions) noexcept {
    const Index count = positions.size();

    if (count == 0) {
        return BoundingCube{};
    }

    Vec3 low = positions.get(0);
    Vec3 high = low;

    for (Index i = 1; i < count; ++i) {
        low.x = std::min(low.x, positions.x[i]);
        low.y = std::min(low.y, positions.y[i]);
        low.z = std::min(low.z, positions.z[i]);
        high.x = std::max(high.x, positions.x[i]);
        high.y = std::max(high.y, positions.y[i]);
        high.z = std::max(high.z, positions.z[i]);
    }

    // The longest of the three extents, so the cube contains the box on every
    // axis. A configuration that is flat in one axis, which two-body tests are,
    // still gets a cube rather than a slab.
    const Vec3 extent = high - low;
    Real size = std::max({extent.x, extent.y, extent.z});

    if (!(size > 0)) {
        // Every particle at one point. A cube of zero size is not merely
        // useless, it is dangerous: every cell of the tree built in it would
        // have an acceptance radius of zero, and a cell that is accepted at
        // zero distance is a cell that accelerates the particle inside it by
        // its own mass at its own position. One is as good as any other
        // positive number here, since the codes are all equal either way and
        // the tree becomes a single leaf.
        size = 1;
    }

    // Centred on the box rather than anchored at its low corner. The two differ
    // when the extents differ, and centring means a flat configuration sits in
    // the middle of its cube instead of against one face, which keeps the
    // subdivision balanced for the axes that do have extent.
    const Vec3 centre = (low + high) / 2;
    const Vec3 origin = centre - Vec3{size / 2, size / 2, size / 2};

    return BoundingCube{.origin = origin, .size = size};
}

MortonCode spread_bits(std::uint32_t value) noexcept {
    // Five doublings of the gap between bits, from adjacent to three apart. The
    // masks are the fixed point of the operation at each stage: after the shift
    // and the or, the bits that survive the mask are exactly those in their
    // final positions for that stage. They are written in hexadecimal because
    // the pattern, groups of ones and zeros of a length that halves each line,
    // is visible there and in no other base.
    MortonCode spread = value & 0x1fffff;

    spread = (spread | (spread << 32)) & 0x1f00000000ffff;
    spread = (spread | (spread << 16)) & 0x1f0000ff0000ff;
    spread = (spread | (spread << 8)) & 0x100f00f00f00f00f;
    spread = (spread | (spread << 4)) & 0x10c30c30c30c30c3;
    spread = (spread | (spread << 2)) & 0x1249249249249249;

    return spread;
}

MortonCode morton_code(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept {
    return (spread_bits(x) << 2) | (spread_bits(y) << 1) | spread_bits(z);
}

MortonCode morton_code(Vec3 position, const BoundingCube& cube) noexcept {
    if (!(cube.size > 0)) {
        // Every particle is at one point, or there are none. Every code is then
        // the same, which is the honest answer: the curve cannot order points
        // it cannot distinguish, and the tree builder turns a range of equal
        // codes into a leaf.
        //
        // Written as the negation so that a cube whose size came out NaN, which
        // takes a NaN coordinate to produce, lands here rather than in the
        // arithmetic below.
        return 0;
    }

    const Real scale = static_cast<Real>(kMortonGridSize) / cube.size;

    const auto to_grid = [scale](Real coordinate, Real origin) noexcept {
        const Real cell = (coordinate - origin) * scale;

        // Clamped rather than trusted. The upper end is reached by any particle
        // on the far face of the cube, which the particle that defined the
        // bounding box is, and one past the last cell would set a bit outside
        // the 21 the code has room for. The lower end cannot be reached from a
        // cube built by `bounding_cube` and is clamped for the caller that
        // builds one some other way.
        if (!(cell > 0)) {
            return std::uint32_t{0};
        }
        if (cell >= static_cast<Real>(kMortonGridSize)) {
            return kMortonGridSize - 1;
        }

        return static_cast<std::uint32_t>(cell);
    };

    return morton_code(to_grid(position.x, cube.origin.x), to_grid(position.y, cube.origin.y),
                       to_grid(position.z, cube.origin.z));
}

unsigned morton_octant(MortonCode code, unsigned level) noexcept {
    if (level == 0 || level > kMortonBitsPerAxis) {
        return 0;
    }

    // Level 1 occupies the highest triple of the 63 bits in use, so that
    // ordering by code is ordering by the coarsest subdivision first. That is
    // the property the whole scheme rests on: a sorted array has the particles
    // of every cell contiguous, at every level at once.
    const unsigned shift = 3 * (kMortonBitsPerAxis - level);

    return static_cast<unsigned>((code >> shift) & 0b111U);
}

void MortonOrdering::build(Vec3Span<const Real> positions, backend::Executor* executor) {
    const Index count = positions.size();

    cube_ = bounding_cube(positions);
    keys_.resize(count);

    // The codes themselves are not computed in parallel, although they trivially
    // could be. One code is five shifts and five masks per axis against a sort
    // that is O(N log N) with a comparison and a 16-byte move in its inner loop,
    // and the measurement in docs/performance/barnes_hut.md puts this loop at a
    // few per cent of a build. Handing it to the pool would add a scheduling
    // round trip to save a fraction of that.
    for (Index i = 0; i < count; ++i) {
        keys_[i] = MortonKey{.code = morton_code(positions.get(i), cube_), .index = i};
    }

    sort(executor);
}

void MortonOrdering::sort(backend::Executor* executor) {
    const Index count = keys_.size();
    const unsigned workers = executor == nullptr ? 1 : executor->worker_count();

    if (executor == nullptr || workers < 2 || count < kParallelSortThreshold) {
        std::ranges::sort(keys_);
        return;
    }

    // A merge sort in two phases, run through the same parallel-for every
    // kernel in this project uses. There is no parallel sort in the standard
    // library this project can rely on: the parallel algorithms need a TBB
    // runtime under libstdc++, they are absent from libc++, and depending on
    // one for a step of a force evaluation would put a second thread pool
    // beside the one Phase 6 built and tuned for this machine's asymmetric
    // cores.
    //
    // The first phase sorts each run on its own, which is where nearly all the
    // work is: sorting P runs of N/P costs N log(N/P) against N log N, so at
    // eight runs and a million particles the merges below account for about a
    // seventh of the total.
    scratch_.resize(count);

    Index runs = initial_run_count(count, workers);

    executor->run(runs, [&](Index begin, Index end) {
        for (Index run = begin; run < end; ++run) {
            const auto first =
                keys_.begin() + static_cast<std::ptrdiff_t>(run_boundary(count, runs, run));
            const auto last =
                keys_.begin() + static_cast<std::ptrdiff_t>(run_boundary(count, runs, run + 1));
            std::sort(first, last);
        }
    });

    // The second phase halves the number of runs each round by merging them in
    // pairs, and each round has half the parallelism of the one before it. That
    // is the well-known weakness of this shape and it is accepted here: with
    // eight runs there are three rounds, the last of which is a single serial
    // merge of the whole array, and the whole phase is a small fraction of a
    // build. A merge that split the work by rank to keep every worker busy in
    // the last round is the standard remedy and is not worth its complexity at
    // this size.
    while (runs > 1) {
        const Index pairs = runs / 2;

        executor->run(pairs, [&](Index begin, Index end) {
            for (Index pair = begin; pair < end; ++pair) {
                const Index first = run_boundary(count, runs, pair * 2);
                const Index middle = run_boundary(count, runs, (pair * 2) + 1);
                const Index last = run_boundary(count, runs, (pair * 2) + 2);

                std::merge(keys_.begin() + static_cast<std::ptrdiff_t>(first),
                           keys_.begin() + static_cast<std::ptrdiff_t>(middle),
                           keys_.begin() + static_cast<std::ptrdiff_t>(middle),
                           keys_.begin() + static_cast<std::ptrdiff_t>(last),
                           scratch_.begin() + static_cast<std::ptrdiff_t>(first));
            }
        });

        // Swapped rather than copied back, so a round costs one pass rather
        // than two. The result is in `keys_` at the end of every round, which
        // is what the next round and the caller both expect.
        keys_.swap(scratch_);
        runs = pairs;
    }
}

} // namespace orrery::solvers
