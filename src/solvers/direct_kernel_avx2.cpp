/// \file
/// The direct kernel in AVX2, four pairs at a time in double precision and
/// eight in single.
///
/// This translation unit is compiled with AVX2 and FMA enabled and nothing else
/// in the project is. That is what makes one binary run on a machine with these
/// instructions and on a machine without them, and it is why the file contains
/// the kernel and no support code: anything else that lived here would be
/// compiled for a processor the caller may not have.
///
/// The intrinsics are wrapped in a handful of named helpers below rather than
/// spelled out at each use. Two reasons, and neither is decoration. The
/// project's scalar type is selected at build time (ADR-0006), so the same
/// kernel has to compile against `__m256d` and `__m256`, and the helpers are
/// where that fork lives instead of two copies of the loop. And a line reading
/// `fused_multiply_add(factor, dx, acceleration_x)` says what the arithmetic is
/// where `_mm256_fmadd_pd` says which instruction encodes it, which is the
/// wrong half of the information for a reader checking the physics.

#include <span>
#include <type_traits>

#include <immintrin.h>

#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/solvers/direct_kernel.hpp"

namespace orrery::solvers {

namespace {

using core::Index;
using core::Real;
using core::Softening;
using core::Vec3;
using core::Vec3Span;

/// A full 256-bit register of whichever scalar this build uses.
using Wide = std::conditional_t<core::kSinglePrecision, __m256, __m256d>;

/// How many pairs one iteration of the loop below handles.
///
/// Four in double precision and eight in single, because the register is 256
/// bits wide either way. This is the whole of the arithmetic advantage the
/// single-precision build has over the double-precision one, on top of halving
/// the bytes each pair costs to fetch.
constexpr Index kLanes = 32 / sizeof(Real);

// Every helper below is a template, and that is load bearing rather than
// stylistic. Written as a plain function containing `if constexpr`, both arms
// would be type checked whichever precision the build selected, and the arm for
// the other precision would be compiled against the wrong element type and
// rejected. Written as an overloaded pair, the half this build does not use
// would be an unused static function, which this project's warning set treats
// as an error. A template has neither problem: the discarded arm is never
// instantiated, and neither is the specialisation that is never called.

template<typename Scalar> [[nodiscard]] auto broadcast(Scalar value) noexcept {
    if constexpr (std::is_same_v<Scalar, float>) {
        return _mm256_set1_ps(value);
    } else {
        return _mm256_set1_pd(value);
    }
}

/// An unaligned load, deliberately.
///
/// `core/aligned_allocator.hpp` starts every component array on a cache line,
/// but the ranges this kernel is given start at an arbitrary particle index,
/// because the range either side of the target is what excludes its
/// self-interaction. So the first load of a range is aligned only by accident.
/// On every processor this project targets an unaligned load costs nothing
/// extra when it does not straddle a cache line, and the alignment of the array
/// is what keeps most of them from straddling one.
template<typename Scalar> [[nodiscard]] auto load(const Scalar* source) noexcept {
    if constexpr (std::is_same_v<Scalar, float>) {
        return _mm256_loadu_ps(source);
    } else {
        return _mm256_loadu_pd(source);
    }
}

template<typename Vector> [[nodiscard]] Vector subtract(Vector left, Vector right) noexcept {
    if constexpr (std::is_same_v<Vector, __m256>) {
        return _mm256_sub_ps(left, right);
    } else {
        return _mm256_sub_pd(left, right);
    }
}

template<typename Vector> [[nodiscard]] Vector add(Vector left, Vector right) noexcept {
    if constexpr (std::is_same_v<Vector, __m256>) {
        return _mm256_add_ps(left, right);
    } else {
        return _mm256_add_pd(left, right);
    }
}

template<typename Vector> [[nodiscard]] Vector multiply(Vector left, Vector right) noexcept {
    if constexpr (std::is_same_v<Vector, __m256>) {
        return _mm256_mul_ps(left, right);
    } else {
        return _mm256_mul_pd(left, right);
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

/// `left * right + addend`, with one rounding rather than two.
///
/// The reciprocal square root is not approximated anywhere in this kernel, and
/// this is the only place where its arithmetic departs from the scalar
/// kernel's rounding. FMA is an IEEE-754 operation with its own correct
/// rounding, so the departure is towards the exact answer rather than away from
/// it. ADR-0020 sets out the distinction between using it and letting a
/// compiler introduce it under a fast-math flag.
template<typename Vector>
[[nodiscard]] Vector fused_multiply_add(Vector left, Vector right, Vector addend) noexcept {
    if constexpr (std::is_same_v<Vector, __m256>) {
        return _mm256_fmadd_ps(left, right, addend);
    } else {
        return _mm256_fmadd_pd(left, right, addend);
    }
}

/// Add the lanes of one accumulator together.
///
/// The order is fixed and documented rather than left to whatever the shuffles
/// happen to do, because it is part of what makes this kernel's answer
/// reproducible: lanes are folded in halves, so four lanes sum as
/// `(v0 + v2) + (v1 + v3)` and eight as the same pattern applied twice. Any
/// order would be as accurate; only a fixed one is repeatable.
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

[[nodiscard]] Wide zero() noexcept {
    return broadcast(Real{0});
}

} // namespace

Vec3 accumulate_range_avx2(Vec3Span<const Real> positions, std::span<const Real> masses,
                           Vec3 target, Index begin, Index end, Softening softening) noexcept {
    // Raw pointers rather than the spans themselves, which section 4 of the
    // implementation plan permits inside a kernel and asks to have said out
    // loud. The bound is the loop's own and was established by the caller; a
    // subscript through `std::span` here would re-check it on every one of the
    // N^2 loads.
    const Real* position_x = positions.x.data();
    const Real* position_y = positions.y.data();
    const Real* position_z = positions.z.data();
    const Real* mass = masses.data();

    const Wide target_x = broadcast(target.x);
    const Wide target_y = broadcast(target.y);
    const Wide target_z = broadcast(target.z);
    const Wide softening_squared = broadcast(softening.squared());
    const Wide one = broadcast(Real{1});

    Wide acceleration_x = zero();
    Wide acceleration_y = zero();
    Wide acceleration_z = zero();

    // Whole vectors first, then whatever is left over. The remainder is at most
    // three pairs in double precision, and it is handed to the scalar kernel
    // rather than computed here under a mask: a masked epilogue is a second
    // copy of the arithmetic that runs on at most seven of every N pairs, and
    // getting it subtly wrong is a classic way to make a fast kernel disagree
    // with the reference on some sizes and not others.
    const Index vector_end = begin + (((end - begin) / kLanes) * kLanes);

    for (Index j = begin; j < vector_end; j += kLanes) {
        const Wide dx = subtract(load(position_x + j), target_x);
        const Wide dy = subtract(load(position_y + j), target_y);
        const Wide dz = subtract(load(position_z + j), target_z);

        // Associated exactly as the scalar kernel associates it,
        // ((dx^2 + dy^2) + dz^2) + eps^2, so that the only differences between
        // the two are the ones the header lists.
        Wide separation_squared = multiply(dx, dx);
        separation_squared = fused_multiply_add(dy, dy, separation_squared);
        separation_squared = fused_multiply_add(dz, dz, separation_squared);
        separation_squared = add(separation_squared, softening_squared);

        // The softened inverse cube, written as the cube of the reciprocal for
        // the reason `core/softening.hpp` gives: the acceleration and the
        // potential energy diagnostic have to agree in the last bits, and they
        // do so by sharing this form rather than by both being close to it.
        const Wide inverse = divide(one, square_root(separation_squared));
        const Wide inverse_cubed = multiply(multiply(inverse, inverse), inverse);
        const Wide factor = multiply(load(mass + j), inverse_cubed);

        acceleration_x = fused_multiply_add(factor, dx, acceleration_x);
        acceleration_y = fused_multiply_add(factor, dy, acceleration_y);
        acceleration_z = fused_multiply_add(factor, dz, acceleration_z);
    }

    const Vec3 remainder =
        accumulate_range_scalar(positions, masses, target, vector_end, end, softening);

    return {reduce(acceleration_x) + remainder.x, reduce(acceleration_y) + remainder.y,
            reduce(acceleration_z) + remainder.z};
}

} // namespace orrery::solvers
