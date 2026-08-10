#include "orrery/core/particle_data.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <random>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/core/aligned_allocator.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/core/vec3_span.hpp"

namespace {

using orrery::core::Index;
using orrery::core::kCacheLineBytes;
using orrery::core::ParticleData;
using orrery::core::Real;
using orrery::core::Vec3;
using orrery::core::Vec3Span;

/// The seed for the property test below. Fixed so that a failure can be
/// reproduced exactly, and reported in the failure message so that the reader
/// does not have to open this file to find it.
constexpr std::uint_fast32_t kSeed = 20260810;

/// The lengths of all ten component arrays, in one place.
///
/// The class invariant is that these are equal to each other and to `size()`.
/// Collecting them through the public views is how a test can see the invariant
/// at all, since the arrays themselves are private.
[[nodiscard]] std::array<Index, 10> component_lengths(const ParticleData& data) {
    const auto positions = data.positions();
    const auto velocities = data.velocities();
    const auto accelerations = data.accelerations();

    return {positions.x.size(),     positions.y.size(),     positions.z.size(),
            velocities.x.size(),    velocities.y.size(),    velocities.z.size(),
            accelerations.x.size(), accelerations.y.size(), accelerations.z.size(),
            data.masses().size()};
}

[[nodiscard]] bool is_cache_line_aligned(const void* pointer) {
    return std::bit_cast<std::uintptr_t>(pointer) % kCacheLineBytes == 0;
}

/// Every component array's first element, so that the alignment the container
/// inherits from its allocator can be checked in one place.
[[nodiscard]] std::array<const Real*, 10> component_data(const ParticleData& data) {
    const auto positions = data.positions();
    const auto velocities = data.velocities();
    const auto accelerations = data.accelerations();

    return {positions.x.data(),     positions.y.data(),     positions.z.data(),
            velocities.x.data(),    velocities.y.data(),    velocities.z.data(),
            accelerations.x.data(), accelerations.y.data(), accelerations.z.data(),
            data.masses().data()};
}

} // namespace

TEST_CASE("a default constructed container holds nothing", "[unit][core]") {
    const ParticleData data;

    REQUIRE(data.empty());

    for (const Index length : component_lengths(data)) {
        REQUIRE(length == 0);
    }
}

TEST_CASE("sizing the container zero-fills every component", "[unit][core]") {
    constexpr Index kCount = 5;
    const ParticleData data(kCount);

    REQUIRE(data.size() == kCount);
    REQUIRE_FALSE(data.empty());

    for (Index particle = 0; particle < kCount; ++particle) {
        CAPTURE(particle);
        REQUIRE(data.positions().get(particle) == Vec3{});
        REQUIRE(data.velocities().get(particle) == Vec3{});
        REQUIRE(data.accelerations().get(particle) == Vec3{});
        REQUIRE(data.masses()[particle] == Real{0});
    }
}

TEST_CASE("an added particle can be read back through the views", "[unit][core]") {
    ParticleData data;

    const Index first = data.add(Vec3{1, 2, 3}, Vec3{4, 5, 6}, Real{7});
    const Index second = data.add(Vec3{-1, -2, -3}, Vec3{-4, -5, -6}, Real{8});

    REQUIRE(first == 0);
    REQUIRE(second == 1);
    REQUIRE(data.size() == 2);

    REQUIRE(data.positions().get(first) == Vec3{1, 2, 3});
    REQUIRE(data.velocities().get(first) == Vec3{4, 5, 6});
    REQUIRE(data.masses()[first] == Real{7});

    REQUIRE(data.positions().get(second) == Vec3{-1, -2, -3});
    REQUIRE(data.velocities().get(second) == Vec3{-4, -5, -6});
    REQUIRE(data.masses()[second] == Real{8});

    // An appended particle starts with no acceleration, because acceleration is
    // an output of the force solver rather than something the caller supplies.
    REQUIRE(data.accelerations().get(first) == Vec3{});
    REQUIRE(data.accelerations().get(second) == Vec3{});
}

TEST_CASE("a mutable view writes through to the container", "[unit][core]") {
    ParticleData data(2);

    data.accelerations().set(1, Vec3{9, 8, 7});
    data.masses()[1] = Real{3};

    const ParticleData& read_only = data;
    REQUIRE(read_only.accelerations().get(1) == Vec3{9, 8, 7});
    REQUIRE(read_only.masses()[1] == Real{3});

    // A mutable view converts to a read-only one, which is what lets a caller
    // holding write access pass its data to a diagnostic that only reads.
    const Vec3Span<const Real> converted = data.accelerations();
    REQUIRE(converted.get(1) == Vec3{9, 8, 7});
    REQUIRE(converted.size() == data.size());
}

TEST_CASE("resizing preserves the particles it keeps", "[unit][core]") {
    ParticleData data;
    data.add(Vec3{1, 2, 3}, Vec3{4, 5, 6}, Real{7});
    data.add(Vec3{2, 3, 4}, Vec3{5, 6, 7}, Real{8});

    SECTION("growing leaves the existing particles alone and zeroes the new ones") {
        data.resize(4);

        REQUIRE(data.size() == 4);
        REQUIRE(data.positions().get(0) == Vec3{1, 2, 3});
        REQUIRE(data.positions().get(1) == Vec3{2, 3, 4});
        REQUIRE(data.positions().get(3) == Vec3{});
        REQUIRE(data.masses()[3] == Real{0});
    }

    SECTION("shrinking keeps the prefix") {
        data.resize(1);

        REQUIRE(data.size() == 1);
        REQUIRE(data.positions().get(0) == Vec3{1, 2, 3});
        REQUIRE(data.masses()[0] == Real{7});
    }

    for (const Index length : component_lengths(data)) {
        REQUIRE(length == data.size());
    }
}

TEST_CASE("clearing empties the container but keeps its storage", "[unit][core]") {
    ParticleData data;
    data.reserve(100);
    data.add(Vec3{1, 1, 1}, Vec3{}, Real{1});

    data.clear();

    REQUIRE(data.empty());
    REQUIRE(data.capacity() >= 100);

    for (const Index length : component_lengths(data)) {
        REQUIRE(length == 0);
    }
}

TEST_CASE("appending repeatedly does not reallocate on every call", "[unit][core]") {
    // The container grows all ten arrays together, so growing by one element at
    // a time would copy ten arrays on every append and make filling a container
    // quadratic. This checks the geometric growth that avoids it.
    ParticleData data;

    constexpr Index kCount = 1000;
    for (Index particle = 0; particle < kCount; ++particle) {
        data.add(Vec3{}, Vec3{}, Real{1});
    }

    REQUIRE(data.size() == kCount);
    REQUIRE(data.capacity() < 2 * kCount);
}

TEST_CASE("every component array is cache-line aligned as the container grows", "[unit][core]") {
    // The alignment argument in the header is about the arrays the kernels
    // read, not about a bare allocation, so it is checked here on the container
    // rather than only on the allocator.
    ParticleData data;

    for (Index particle = 0; particle < 300; ++particle) {
        data.add(Vec3{}, Vec3{}, Real{1});
        CAPTURE(particle);

        for (const Real* start : component_data(data)) {
            REQUIRE(is_cache_line_aligned(start));
        }
    }
}

TEST_CASE("the component arrays stay the same length under any sequence of operations",
          "[property][core]") {
    INFO("seed = " << kSeed);
    std::mt19937 generator{kSeed};
    std::uniform_int_distribution<int> choose_operation{0, 3};
    std::uniform_int_distribution<Index> choose_count{0, 64};

    ParticleData data;

    constexpr int kSteps = 500;
    for (int step = 0; step < kSteps; ++step) {
        CAPTURE(step);

        switch (choose_operation(generator)) {
        case 0:
            data.add(Vec3{1, 2, 3}, Vec3{4, 5, 6}, Real{1});
            break;
        case 1:
            data.resize(choose_count(generator));
            break;
        case 2:
            data.reserve(choose_count(generator));
            break;
        default:
            data.clear();
            break;
        }

        for (const Index length : component_lengths(data)) {
            REQUIRE(length == data.size());
        }

        REQUIRE(data.capacity() >= data.size());
    }
}
