#include "orrery/core/random.hpp"

#include <cmath>
#include <cstdint>
#include <numbers>

#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"

namespace orrery::core {

RandomSource::RandomSource(std::uint64_t seed) noexcept : seed_(seed), engine_(seed) {}

Real RandomSource::uniform() noexcept {
    // The top bits rather than the bottom ones. The Mersenne twister's low bits
    // are as good as its high bits, so this is not a correction for a weakness;
    // it is the shift that leaves exactly as many bits as the mantissa can
    // hold, which is what makes the scaling below exact.
    const std::uint64_t bits = engine_() >> (64 - kMantissaBits);
    return static_cast<Real>(bits) * kScale;
}

Real RandomSource::uniform(Real low, Real high) noexcept {
    // Written as an interpolation rather than as low + u * (high - low)
    // because the difference of two large bounds loses precision that this form
    // keeps, and because it returns exactly `low` at u = 0.
    const Real fraction = uniform();
    return (low * (Real{1} - fraction)) + (high * fraction);
}

Vec3 RandomSource::unit_vector() noexcept {
    // Archimedes' theorem: the projection of the unit sphere onto its axis
    // preserves area, so a uniform height and a uniform azimuth give a uniform
    // direction. Two draws, one square root and one sine and cosine pair, with
    // no rejection loop and so no dependence of the stream position on the
    // values drawn.
    const Real height = uniform(-1, 1);
    const Real azimuth = uniform(0, 2 * std::numbers::pi_v<Real>);

    // No clamp guards the square root. The interpolation above returns a
    // height in [-1, 1], and the square of a value in that range never rounds
    // above one, so the argument is non-negative for every draw rather than for
    // almost every draw.
    const Real radius = std::sqrt(Real{1} - (height * height));

    return {radius * std::cos(azimuth), radius * std::sin(azimuth), height};
}

} // namespace orrery::core
