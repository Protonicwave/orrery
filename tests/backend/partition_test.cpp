#include "orrery/backend/partition.hpp"

#include <array>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/core/aligned_allocator.hpp"
#include "orrery/core/types.hpp"

namespace {

using orrery::backend::equal_share;
using orrery::backend::IndexRange;
using orrery::backend::kPartitionGrain;
using orrery::core::Index;
using orrery::core::kCacheLineBytes;
using orrery::core::Real;

/// The counts worth checking: around zero, around one cache line, around the
/// worker count, and a size with an awkward remainder in every direction.
constexpr auto kCounts =
    std::to_array<Index>({0, 1, 2, 7, 8, 9, 15, 16, 17, 63, 64, 65, 100, 1000, 4096, 4097});

constexpr auto kWorkerCounts = std::to_array<unsigned>({1, 2, 3, 4, 7, 8, 16, 64});

} // namespace

TEST_CASE("the shares tile the range exactly once", "[unit][backend]") {
    // The property everything else depends on. If two shares overlapped, two
    // workers would write the same acceleration and the answer would depend on
    // which finished last. If they left a gap, a particle would keep whatever
    // acceleration the previous step left in the buffer, which is worse than a
    // crash because the run would continue and look plausible.
    for (const Index count : kCounts) {
        for (const unsigned workers : kWorkerCounts) {
            std::vector<int> covered(count, 0);

            for (unsigned worker = 0; worker < workers; ++worker) {
                const IndexRange share = equal_share(count, workers, worker);

                CAPTURE(count, workers, worker, share.begin, share.end);
                REQUIRE(share.begin <= share.end);
                REQUIRE(share.end <= count);

                for (Index index = share.begin; index < share.end; ++index) {
                    covered[index] += 1;
                }
            }

            for (Index index = 0; index < count; ++index) {
                CAPTURE(count, workers, index);
                REQUIRE(covered[index] == 1);
            }
        }
    }
}

TEST_CASE("the shares are contiguous and in worker order", "[unit][backend]") {
    // Each worker's range starts where the previous one ended. Contiguity is
    // what makes a share a sequential walk of the component arrays rather than
    // a stride, which is the access pattern the hardware prefetcher follows and
    // the one ADR-0004's layout was chosen for.
    for (const Index count : kCounts) {
        for (const unsigned workers : kWorkerCounts) {
            Index expected_begin = 0;

            for (unsigned worker = 0; worker < workers; ++worker) {
                const IndexRange share = equal_share(count, workers, worker);

                CAPTURE(count, workers, worker, share.begin, share.end, expected_begin);
                REQUIRE(share.begin == expected_begin);

                expected_begin = share.end;
            }

            REQUIRE(expected_begin == count);
        }
    }
}

TEST_CASE("no two shares meet inside a cache line", "[unit][backend]") {
    // The half of false-sharing avoidance that `core/aligned_allocator.hpp`
    // left to the scheduler. Every boundary between two workers falls on a
    // cache line, so no line of a component array is written by more than one
    // worker. The one exception is the end of the last share, which is the end
    // of the array and has no neighbour to contend with.
    static_assert(kPartitionGrain * sizeof(Real) == kCacheLineBytes,
                  "The partition grain is meant to be exactly one cache line of scalars");

    for (const Index count : kCounts) {
        for (const unsigned workers : kWorkerCounts) {
            for (unsigned worker = 0; worker < workers; ++worker) {
                const IndexRange share = equal_share(count, workers, worker);

                // An empty share has no elements and so no line to contend
                // over. Where it sits is not a boundary between two workers,
                // and it is reported at the end of the range rather than on a
                // line, so it is not held to the alignment rule.
                if (share.empty()) {
                    continue;
                }

                CAPTURE(count, workers, worker, share.begin, share.end, kPartitionGrain);

                REQUIRE(share.begin % kPartitionGrain == 0);
                REQUIRE((share.end % kPartitionGrain == 0 || share.end == count));
            }
        }
    }
}

TEST_CASE("the imbalance built into the partition is one cache line", "[unit][backend]") {
    // How much work the static scheme has already given away before the machine
    // is involved at all. The division is over whole cache lines, so the
    // guarantee is stated in lines: no worker holds more than one line more than
    // any other worker that holds anything.
    //
    // It is worth stating in those terms rather than in particles. The last line
    // of the array is generally partial, so a worker holding one line may hold
    // as few as one particle while its neighbour holds a full line of eight, and
    // the imbalance counted in particles can reach two lines. That is a property
    // of the array not ending on a line rather than of the division, and it
    // shrinks to nothing as a fraction of the work as N grows, which is the only
    // regime the performance figures are taken in.
    for (const Index count : kCounts) {
        for (const unsigned workers : kWorkerCounts) {
            Index smallest = count;
            Index largest = 0;

            for (unsigned worker = 0; worker < workers; ++worker) {
                const IndexRange share = equal_share(count, workers, worker);

                // Only among the workers that got anything. A pool larger than
                // the problem leaves some with nothing, which is a different
                // situation and not an imbalance in the division.
                if (share.empty()) {
                    continue;
                }

                const Index lines = (share.size() + kPartitionGrain - 1) / kPartitionGrain;

                smallest = lines < smallest ? lines : smallest;
                largest = lines > largest ? lines : largest;
            }

            if (largest == 0) {
                continue;
            }

            CAPTURE(count, workers, smallest, largest);
            REQUIRE(largest - smallest <= 1);
        }
    }
}

TEST_CASE("a worker outside the pool gets nothing", "[unit][backend]") {
    // Guarding the arithmetic rather than the caller. Every caller in this
    // project asks only about workers it has, and a share computed from a
    // nonsense index should be empty rather than wrap around into somebody
    // else's range.
    REQUIRE(equal_share(100, 4, 4).empty());
    REQUIRE(equal_share(100, 4, 99).empty());
    REQUIRE(equal_share(100, 0, 0).empty());
    REQUIRE(equal_share(0, 8, 0).empty());
}
