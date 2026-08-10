#include "orrery/backend/cpu_features.hpp"

#include <string>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

// A test for a hardware query cannot assert what the hardware is. It runs on
// continuous integration machines of several vintages and on developer laptops,
// and a test that required AVX2 would fail on a correct answer.
//
// What it can assert is that the answer is self-consistent, stable and not the
// result of an accident, which is where the failures in code of this kind
// actually live: a feature bit read from the wrong register, a result that
// changes between calls because the caching was not thread safe, or a brand
// string returned with the padding the architecture leaves in it.

namespace {

using orrery::backend::cpu_brand;
using orrery::backend::cpu_features;
using orrery::backend::CpuFeatures;

} // namespace

TEST_CASE("the feature query answers the same thing every time", "[unit][backend]") {
    // The result is cached in a function-local static, so this is a test that
    // the caching returns the same object rather than recomputing into a
    // temporary whose address is then returned. A dangling view of a stale
    // answer would show up here as a difference or as a crash.
    const CpuFeatures& first = cpu_features();
    const CpuFeatures& second = cpu_features();

    REQUIRE(&first == &second);
    REQUIRE(first.avx == second.avx);
    REQUIRE(first.avx2 == second.avx2);
    REQUIRE(first.fma == second.fma);
}

TEST_CASE("the reported features are consistent with each other", "[unit][backend]") {
    const CpuFeatures& features = cpu_features();

    CAPTURE(features.avx, features.avx2, features.fma);

    // AVX2 is an extension of AVX and no processor implements the second
    // without the first. Both are also gated behind the same check that the
    // operating system preserves the upper vector registers, so a build
    // reporting AVX2 without AVX would mean that gate had been applied to one
    // and not the other, which is the mistake this file is guarding against.
    if (features.avx2) {
        REQUIRE(features.avx);
    }
}

TEST_CASE("the brand string carries no padding", "[unit][backend]") {
    const std::string brand{cpu_brand()};

    CAPTURE(brand);

    // Empty is a legitimate answer, on a processor that offers no brand string
    // and on every architecture that has no CPUID at all. What is not
    // legitimate is the forty-eight byte field handed back as it was read, with
    // its trailing nulls and the leading spaces most parts pad with, because
    // that lands in the machine state recorded beside every measurement.
    if (!brand.empty()) {
        REQUIRE(brand.front() != ' ');
        REQUIRE(brand.back() != ' ');
        REQUIRE(brand.find('\0') == std::string::npos);
    }
}
