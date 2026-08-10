#pragma once

/// \file
/// The project's source of randomness, and the only one.
///
/// Every result Orrery reports about a sampled configuration is a claim about a
/// specific set of particles. If that set cannot be reconstructed, the claim
/// cannot be rechecked, and a regression test against a golden output is
/// testing the generator as much as the physics. So the requirement is stronger
/// than "random enough": the same seed must give the same particles, on every
/// compiler and every platform the project builds on.
///
/// The standard library provides half of that. `std::mt19937_64` is specified
/// bit for bit by the standard, so it produces the same stream everywhere. The
/// distributions are not: `std::uniform_real_distribution` is defined by its
/// output distribution and not by its algorithm, and three standard libraries
/// give three different sequences from one engine and one seed. Only the engine
/// is therefore used, and the mapping from bits to numbers is written here.
/// ADR-0009 records the alternatives.
///
/// One limit is worth stating rather than leaving to be discovered. The values
/// this class returns are bit-identical across platforms, because they are made
/// with shifts and an exact power-of-two multiplication. Quantities derived
/// from them through the standard maths library, a square root or a cosine, are
/// identical only as far as that library's last bit, which is not guaranteed to
/// agree between platforms. Reproducibility of a sampled configuration is
/// therefore exact within a platform and to a few units in the last place
/// across them, which is why the tests over sampled configurations compare
/// against tolerances rather than against stored values.

#include <cstdint>
#include <limits>
#include <random>

#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"

namespace orrery::core {

/// A seeded stream of random numbers.
///
/// Passed by reference to everything that needs randomness, so that one run
/// draws from one stream. Two generators seeded independently inside one setup
/// would make the configuration depend on the order in which they happened to
/// be constructed.
class RandomSource {
public:
    /// Start a stream from the given seed.
    ///
    /// There is no default seed and no seeding from a device. A generator that
    /// can be constructed without a seed will eventually be constructed without
    /// one, and the run that produced a result will not be repeatable.
    explicit RandomSource(std::uint64_t seed) noexcept;

    /// The seed this stream was started from.
    ///
    /// Kept so that whatever reports a result, a failing test or a run's
    /// metadata, can record the one number needed to reproduce it without the
    /// caller having to carry it alongside.
    [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

    /// A number in [0, 1).
    ///
    /// Half open at the top on purpose: several samplers below divide by the
    /// value or raise it to a negative power, and the closed form would make
    /// those unbounded once in every few billion draws.
    [[nodiscard]] Real uniform() noexcept;

    /// A number in [low, high).
    [[nodiscard]] Real uniform(Real low, Real high) noexcept;

    /// A direction drawn uniformly over the unit sphere.
    ///
    /// Every spherically symmetric configuration in the project needs one, and
    /// the obvious construction, an angle from each of two uniform draws, is
    /// wrong: it concentrates points at the poles. Uniformity in area needs the
    /// cosine of the polar angle to be the uniform variable, which is what this
    /// does.
    [[nodiscard]] Vec3 unit_vector() noexcept;

private:
    /// The number of bits of a draw that a `Real` can hold: 53 for `double`,
    /// 24 for `float`.
    ///
    /// Taking the top bits of the engine's output and scaling by an exact power
    /// of two gives a result that is exactly representable, so no rounding can
    /// push it up to one. Constructing a double first and converting would do
    /// exactly that in the single-precision build, where every draw above
    /// 1 - 2^-25 rounds to 1 and quietly breaks the half-open interval the
    /// samplers rely on.
    static constexpr int kMantissaBits = std::numeric_limits<Real>::digits;

    /// One draw of the engine per number returned, whatever the precision, so
    /// that the two builds stay at the same point in the stream and sample the
    /// same configuration to within their respective resolutions.
    static constexpr Real kScale = Real{1} / static_cast<Real>(std::uint64_t{1} << kMantissaBits);

    std::uint64_t seed_;
    std::mt19937_64 engine_;
};

} // namespace orrery::core
