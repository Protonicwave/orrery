#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <immintrin.h>

#include "harness/arithmetic_probe.hpp"
#include "orrery/core/types.hpp"

namespace orrery::benchmark {

namespace {

using core::Real;

/// A full 256-bit register of whichever scalar this build uses.
using Wide = std::conditional_t<core::kSinglePrecision, __m256, __m256d>;

/// How many of them fit: four doubles or eight floats.
constexpr double kLanes = 32.0 / sizeof(Real);

// Templates rather than overloads or `if constexpr` in a plain function, for
// the reason `src/solvers/direct_kernel_avx2.cpp` gives at the same place: only
// the specialisation this build calls is instantiated, so the arm for the other
// precision is neither type checked against the wrong element type nor left
// behind as an unused function.

template<typename Scalar> [[nodiscard]] auto broadcast(Scalar value) noexcept {
    if constexpr (std::is_same_v<Scalar, float>) {
        return _mm256_set1_ps(value);
    } else {
        return _mm256_set1_pd(value);
    }
}

template<typename Vector>
[[nodiscard]] Vector fused_multiply_add(Vector left, Vector right, Vector addend) noexcept {
    if constexpr (std::is_same_v<Vector, __m256>) {
        return _mm256_fmadd_ps(left, right, addend);
    } else {
        return _mm256_fmadd_pd(left, right, addend);
    }
}

template<typename Vector> [[nodiscard]] Vector add(Vector left, Vector right) noexcept {
    if constexpr (std::is_same_v<Vector, __m256>) {
        return _mm256_add_ps(left, right);
    } else {
        return _mm256_add_pd(left, right);
    }
}

template<typename Vector> [[nodiscard]] Vector divide(Vector left, Vector right) noexcept {
    if constexpr (std::is_same_v<Vector, __m256>) {
        return _mm256_div_ps(left, right);
    } else {
        return _mm256_div_pd(left, right);
    }
}

template<typename Vector> [[nodiscard]] Vector square_root(Vector value) noexcept {
    if constexpr (std::is_same_v<Vector, __m256>) {
        return _mm256_sqrt_ps(value);
    } else {
        return _mm256_sqrt_pd(value);
    }
}

template<typename Vector> [[nodiscard]] auto reduce(Vector value) noexcept {
    if constexpr (std::is_same_v<Vector, __m256>) {
        __m128 folded = _mm_add_ps(_mm256_castps256_ps128(value), _mm256_extractf128_ps(value, 1));
        folded = _mm_add_ps(folded, _mm_movehl_ps(folded, folded));
        return _mm_cvtss_f32(
            _mm_add_ss(folded, _mm_shuffle_ps(folded, folded, _MM_SHUFFLE(1, 1, 1, 1))));
    } else {
        const __m128d folded =
            _mm_add_pd(_mm256_castpd256_pd128(value), _mm256_extractf128_pd(value, 1));
        return _mm_cvtsd_f64(_mm_add_pd(folded, _mm_unpackhi_pd(folded, folded)));
    }
}

/// The chains a probe accumulates into.
///
/// A plain array rather than a `std::array`, because the two do not agree about
/// alignment under the Microsoft ABI: `alignof(Wide)` and `alignof(Wide[12])`
/// are both 32, but `alignof(std::array<Wide, 12>)` is 8, since Clang does not
/// propagate a vector member's over-alignment through a class there. The
/// compiler is then free to place the object on any 8-byte boundary, it lands
/// on a 16-byte one in practice, and every access to an element binds a
/// reference to a misaligned address. That is undefined behaviour, the
/// sanitiser reports it as such, and a plain array simply does not have the
/// problem: the alignment is part of the array type.
///
/// This is the one place in the project where the usual advice to prefer
/// `std::array` is wrong, so the check that gives it is silenced here alone.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
using Chains = Wide[kChains];

/// Filled in place rather than returned, since a plain array is not copyable.
void set_starting_values(Chains& chains) noexcept {
    for (int chain = 0; chain < kChains; ++chain) {
        chains[static_cast<std::size_t>(chain)] = broadcast(static_cast<Real>(chain + 1));
    }
}

[[nodiscard]] Real total_of(const Chains& chains) noexcept {
    Wide total = chains[0];
    for (std::size_t chain = 1; chain < kChains; ++chain) {
        total = add(total, chains[chain]);
    }
    return reduce(total);
}

} // namespace

double fused_multiply_add_block_avx2(std::uint64_t rounds, Real* sink) noexcept {
    const Wide decay = broadcast(static_cast<Real>(1) - static_cast<Real>(1e-7));
    const Wide offset = broadcast(static_cast<Real>(1e-7));

    Chains accumulators{};
    set_starting_values(accumulators);

    // The inner loop has a constant trip count, so the compiler unrolls it and
    // what reaches the processor is kChains independent fused multiply-adds per
    // round with nothing between them. That is the shape the measurement wants:
    // twelve chains is more than enough to keep both of the part's multiply-add
    // units busy through the four-cycle latency of each instruction.
    for (std::uint64_t round = 0; round < rounds; ++round) {
        for (Wide& accumulator : accumulators) {
            accumulator = fused_multiply_add(accumulator, decay, offset);
        }
    }

    *sink = total_of(accumulators);

    // Two operations per lane per chain per round, which is the convention
    // every published peak floating-point figure uses for a fused multiply-add.
    return static_cast<double>(rounds) * kChains * kLanes * 2.0;
}

double divide_and_sqrt_block_avx2(std::uint64_t rounds, Real* sink) noexcept {
    const Wide one = broadcast(static_cast<Real>(1));

    Chains accumulators{};
    set_starting_values(accumulators);

    for (std::uint64_t round = 0; round < rounds; ++round) {
        for (Wide& accumulator : accumulators) {
            accumulator = divide(one, square_root(add(accumulator, one)));
        }
    }

    *sink = total_of(accumulators);

    // One square root and one division per lane per chain per round, which is
    // exactly what one pairwise interaction of the direct kernel costs. The
    // addition is not counted, for the reason the portable version gives.
    return static_cast<double>(rounds) * kChains * kLanes * 2.0;
}

} // namespace orrery::benchmark
