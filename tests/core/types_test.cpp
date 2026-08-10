#include "orrery/core/types.hpp"

#include <cstddef>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "orrery/core/build_info.hpp"

namespace {

using orrery::core::Index;
using orrery::core::kSinglePrecision;
using orrery::core::Real;

static_assert(std::is_floating_point_v<Real>, "Real must be a floating-point type");

static_assert(kSinglePrecision == std::is_same_v<Real, float>,
              "The precision constant must describe the alias it is derived from");

static_assert(sizeof(Real) == (kSinglePrecision ? sizeof(float) : sizeof(double)),
              "The precision switch must change the size of the scalar type");

static_assert(std::is_same_v<Index, std::size_t>,
              "Index must be the type the standard library reports lengths in");

static_assert(std::is_unsigned_v<Index>, "Index must be unsigned");

} // namespace

TEST_CASE("the scalar type this test sees is the one the library was built with", "[unit][core]") {
    // The static assertions above describe this translation unit. This one
    // compares it against the library, which was compiled separately, and the
    // failure it guards against is a precision setting that reached one and not
    // the other. Every value crossing that boundary would then be read at a
    // different size than it was written.
    REQUIRE(orrery::core::uses_single_precision() == kSinglePrecision);
}
