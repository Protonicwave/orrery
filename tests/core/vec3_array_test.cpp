#include "orrery/core/vec3_array.hpp"

#include <bit>
#include <cstdint>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/core/aligned_allocator.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"

namespace {

using orrery::core::Index;
using orrery::core::kCacheLineBytes;
using orrery::core::Real;
using orrery::core::Vec3;
using orrery::core::Vec3Array;

} // namespace

TEST_CASE("a new array is empty and a sized one is zero", "[unit][core]") {
    const Vec3Array empty;
    REQUIRE(empty.empty());
    REQUIRE(empty.view().empty());

    const Vec3Array sized{4};
    REQUIRE_FALSE(sized.empty());
    REQUIRE(sized.size() == 4);

    for (Index index = 0; index < sized.size(); ++index) {
        CAPTURE(index);
        REQUIRE(sized.view().get(index) == Vec3{});
    }
}

TEST_CASE("resizing keeps what is there and zeroes what is new", "[unit][core]") {
    // The scratch buffer of an integrator is resized on its first step and found
    // at the right length on every step after, so the operation that matters is
    // the one that changes nothing. Growth is checked here because a buffer whose
    // new elements held whatever was in the allocation would make a run depend on
    // what the allocator handed back.
    Vec3Array array{2};
    array.view().set(0, Vec3{1, 2, 3});
    array.view().set(1, Vec3{4, 5, 6});

    array.resize(4);
    REQUIRE(array.size() == 4);
    REQUIRE(array.view().get(0) == Vec3{1, 2, 3});
    REQUIRE(array.view().get(1) == Vec3{4, 5, 6});
    REQUIRE(array.view().get(2) == Vec3{});
    REQUIRE(array.view().get(3) == Vec3{});

    array.resize(4);
    REQUIRE(array.size() == 4);
    REQUIRE(array.view().get(0) == Vec3{1, 2, 3});

    array.resize(1);
    REQUIRE(array.size() == 1);
    REQUIRE(array.view().get(0) == Vec3{1, 2, 3});
}

TEST_CASE("the three components stay the same length and start on a cache line", "[unit][core]") {
    Vec3Array array;
    array.resize(37);

    const auto view = array.view();
    REQUIRE(view.x.size() == 37);
    REQUIRE(view.y.size() == 37);
    REQUIRE(view.z.size() == 37);

    // The reason this class exists rather than three loose vectors: the
    // allocation decision of ADR-0005 applies to every array a kernel reads, not
    // only to the ones inside `ParticleData`.
    REQUIRE(std::bit_cast<std::uintptr_t>(view.x.data()) % kCacheLineBytes == 0);
    REQUIRE(std::bit_cast<std::uintptr_t>(view.y.data()) % kCacheLineBytes == 0);
    REQUIRE(std::bit_cast<std::uintptr_t>(view.z.data()) % kCacheLineBytes == 0);
}
