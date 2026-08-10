#include "orrery/core/build_info.hpp"

#include <string_view>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("the reported version is the one the build system set", "[unit][core]") {
    const std::string_view version = orrery::core::version();

    REQUIRE_FALSE(version.empty());
    REQUIRE(version.find('.') != std::string_view::npos);
}

TEST_CASE("the precision switch reaches the library and its consumers alike", "[unit][core]") {
    // The subject of this test is the build system rather than the code. The
    // failure it guards against is the precision setting reaching one
    // translation unit but not another, which would produce a binary whose
    // halves disagree about the size of every scalar. Comparing what this test
    // was compiled with against what the library was compiled with is the only
    // way to see that from inside the test.
#ifdef ORRERY_SINGLE_PRECISION
    REQUIRE(orrery::core::uses_single_precision());
#else
    REQUIRE_FALSE(orrery::core::uses_single_precision());
#endif
}
