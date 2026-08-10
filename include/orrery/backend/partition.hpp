#pragma once

/// \file
/// How a range of indices is divided between workers.
///
/// Both partitioning schemes start from the same equal division and differ in
/// what happens afterwards, so the division itself is written once here. It is
/// also the place where a decision deferred in Phase 2 is finally spent.
///
/// `core/aligned_allocator.hpp` explains that aligning a component array to a
/// cache line is only half of avoiding false sharing between threads, and that
/// the other half, choosing partition boundaries on line multiples, belongs to
/// the scheduler. This is that scheduler. If two workers were given ranges that
/// met in the middle of a cache line, both would write to that line, and each
/// write would take exclusive ownership of it away from the other core. The two
/// threads would ping the line between their caches for the whole region while
/// touching none of the same elements. Rounding every boundary out to a line
/// multiple costs at most 63 bytes of imbalance per worker and removes the
/// effect entirely.

#include <cstddef>

#include "orrery/core/aligned_allocator.hpp"
#include "orrery/core/types.hpp"

namespace orrery::backend {

/// How many scalars fit in a cache line, and so the granularity every partition
/// boundary is a multiple of.
///
/// Eight in the default double-precision build and sixteen under the
/// single-precision one, which is correct in both cases: what matters is the
/// line, not the number of elements that happen to fill it.
inline constexpr core::Index kPartitionGrain = core::kCacheLineBytes / sizeof(core::Real);

/// A half-open range of indices.
struct IndexRange {
    core::Index begin{};
    core::Index end{};

    [[nodiscard]] constexpr bool empty() const noexcept { return begin >= end; }

    [[nodiscard]] constexpr core::Index size() const noexcept { return empty() ? 0 : end - begin; }
};

/// The share of `[0, count)` belonging to `worker` out of `workers`.
///
/// The division is over whole cache lines rather than over indices, and the
/// remainder is spread one line at a time across the first few workers rather
/// than dropped on the last one. Both matter for the same reason: this is the
/// partition the static scheme lives with for the whole region, so any
/// imbalance built into it is imbalance the measurement will attribute to the
/// hardware.
///
/// An out-of-range worker gets an empty range, which is what a pool with more
/// workers than there is work should hand the surplus.
[[nodiscard]] constexpr IndexRange equal_share(core::Index count, unsigned workers,
                                               unsigned worker) noexcept {
    if (workers == 0 || worker >= workers) {
        return IndexRange{count, count};
    }

    const core::Index lines = (count + kPartitionGrain - 1) / kPartitionGrain;
    const core::Index share = lines / workers;
    const core::Index remainder = lines % workers;
    const core::Index index = worker;

    const core::Index first_line = (index * share) + (index < remainder ? index : remainder);
    const core::Index line_count = share + (index < remainder ? 1 : 0);

    const core::Index begin = first_line * kPartitionGrain;
    const core::Index end = (first_line + line_count) * kPartitionGrain;

    // The last worker's range runs off the end of the array, because the count
    // is not generally a whole number of cache lines. Clamping here rather than
    // in every caller keeps the boundary arithmetic above in one piece.
    return IndexRange{begin < count ? begin : count, end < count ? end : count};
}

} // namespace orrery::backend
