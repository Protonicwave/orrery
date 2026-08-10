#include "orrery/core/aligned_allocator.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/core/types.hpp"

namespace {

using orrery::core::AlignedAllocator;
using orrery::core::kCacheLineBytes;
using orrery::core::Real;

using Allocator = AlignedAllocator<Real>;

/// Whether a pointer sits on a cache-line boundary.
///
/// `std::bit_cast` rather than a reinterpret cast, because the question asked
/// is what the pointer's bits are rather than what they might be reinterpreted
/// as, and because the project's lint rules reject the cast usually reached for
/// here.
[[nodiscard]] bool is_cache_line_aligned(const void* pointer) {
    return std::bit_cast<std::uintptr_t>(pointer) % kCacheLineBytes == 0;
}

static_assert(
    std::is_same_v<std::allocator_traits<Allocator>::rebind_alloc<int>, AlignedAllocator<int>>,
    "Containers must be able to rebind the allocator to their own internal types");

static_assert(Allocator{} == AlignedAllocator<int>{},
              "The allocator is stateless, so all of its instances are interchangeable");

static_assert(std::allocator_traits<Allocator>::propagate_on_container_move_assignment::value,
              "Moving a container must move its storage rather than copy its elements");

} // namespace

TEST_CASE("allocated storage begins on a cache line", "[unit][core]") {
    // Counts that are neither multiples of the alignment nor larger than it,
    // because an allocator that is aligned only for round requests is the
    // failure this test exists to catch.
    for (const std::size_t count :
         {std::size_t{1}, std::size_t{3}, std::size_t{7}, std::size_t{100}, std::size_t{1023}}) {
        CAPTURE(count);
        Real* storage = Allocator::allocate(count);
        REQUIRE(storage != nullptr);
        REQUIRE(is_cache_line_aligned(storage));
        Allocator::deallocate(storage, count);
    }
}

TEST_CASE("a container using the allocator stays aligned as it grows", "[unit][core]") {
    // The guarantee that matters is not the one the allocator makes on its own
    // but the one the particle arrays inherit from it, and those arrays are
    // grown by a vector that reallocates on a schedule of its own.
    std::vector<Real, Allocator> values;

    constexpr std::size_t kElements = 4096;
    for (std::size_t element = 0; element < kElements; ++element) {
        values.push_back(static_cast<Real>(element));
        CAPTURE(element);
        REQUIRE(is_cache_line_aligned(values.data()));
    }

    REQUIRE(values.size() == kElements);
}

TEST_CASE("moving a container hands over the aligned storage itself", "[unit][core]") {
    // With a stateless allocator that propagates on move, the destination takes
    // the source's buffer rather than allocating one and copying into it. The
    // alignment therefore survives the move, and so does the address.
    std::vector<Real, Allocator> source(1000, Real{1});
    const Real* original = source.data();

    const std::vector<Real, Allocator> destination = std::move(source);

    REQUIRE(destination.data() == original);
    REQUIRE(is_cache_line_aligned(destination.data()));
}

TEST_CASE("a request too large to express in bytes fails rather than wraps", "[unit][core]") {
    // One element more than can be counted in bytes. Without the check in
    // allocate, the multiplication would wrap and return a small block
    // presented as an enormous one, which the caller would then write past the
    // end of.
    REQUIRE_THROWS_AS(static_cast<void>(Allocator::allocate(Allocator::max_size() + 1)),
                      std::bad_alloc);
}
