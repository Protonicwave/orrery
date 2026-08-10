#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "harness/arithmetic_probe.hpp"
#include "orrery/core/types.hpp"

namespace orrery::benchmark {

namespace {

using core::Real;

/// Distinct starting values, so that a compiler cannot notice the chains are
/// identical and compute one of them.
[[nodiscard]] std::array<Real, kChains> starting_values() noexcept {
    std::array<Real, kChains> chains{};
    for (int chain = 0; chain < kChains; ++chain) {
        chains[static_cast<std::size_t>(chain)] = static_cast<Real>(chain + 1);
    }
    return chains;
}

[[nodiscard]] Real total_of(const std::array<Real, kChains>& chains) noexcept {
    Real total = 0;
    for (const Real value : chains) {
        total += value;
    }
    return total;
}

} // namespace

double fused_multiply_add_block_scalar(std::uint64_t rounds, Real* sink) noexcept {
    // A fixed point at one, so that every chain converges rather than growing
    // without bound or decaying into the subnormal range.
    constexpr Real kDecay = static_cast<Real>(1) - static_cast<Real>(1e-7);
    constexpr Real kOffset = static_cast<Real>(1e-7);

    std::array<Real, kChains> accumulators = starting_values();

    for (std::uint64_t round = 0; round < rounds; ++round) {
        for (Real& accumulator : accumulators) {
            accumulator = (accumulator * kDecay) + kOffset;
        }
    }

    *sink = total_of(accumulators);

    // Two operations per chain per round, a multiply and an add. Counted the
    // same way as the fused form so that the two probes report comparable
    // numbers, which is the comparison a reader wants: a machine with fused
    // multiply-add performs the same two operations in one instruction.
    return static_cast<double>(rounds) * kChains * 2.0;
}

double divide_and_sqrt_block_scalar(std::uint64_t rounds, Real* sink) noexcept {
    std::array<Real, kChains> accumulators = starting_values();

    for (std::uint64_t round = 0; round < rounds; ++round) {
        for (Real& accumulator : accumulators) {
            accumulator = static_cast<Real>(1) / std::sqrt(accumulator + static_cast<Real>(1));
        }
    }

    *sink = total_of(accumulators);

    // One square root and one division per chain per round. The addition is not
    // counted: it is there to keep the recurrence bounded, it retires on a unit
    // that is idle throughout, and including it would flatter the result of a
    // probe whose whole purpose is to isolate the slow unit.
    return static_cast<double>(rounds) * kChains * 2.0;
}

} // namespace orrery::benchmark
