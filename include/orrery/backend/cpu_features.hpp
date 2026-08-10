#pragma once

/// \file
/// Which vector extensions this processor has, asked at run time.
///
/// Phase 7 adds an AVX2 kernel beside the scalar one, and the two have to
/// coexist in one binary. The alternative, building the whole project for the
/// machine it was compiled on, is rejected in ADR-0018: it makes every binary
/// unshippable, it makes a crash on an older machine the first symptom, and it
/// silently vectorises the reference kernel that the fast one is supposed to be
/// measured against.
///
/// So the AVX2 kernel is compiled in a translation unit of its own with the
/// instruction set enabled, the rest of the project is compiled for the
/// baseline, and this file answers the question that decides which of them runs.
///
/// Two things have to be true before an AVX2 instruction may be executed, and
/// asking only the first is a well-known way to produce an illegal instruction
/// on a machine that reports the feature. The processor must implement it, and
/// the operating system must have enabled the register state it uses, since a
/// kernel that does not save the upper halves of the vector registers across a
/// context switch would otherwise corrupt them. Both are checked below.
///
/// Nothing here is on a hot path. The answer is computed once, on first use, and
/// the kernel dispatch reads it once per force evaluation.

#include <string_view>

namespace orrery::backend {

/// The instruction set extensions this project can make use of.
///
/// Deliberately short. This is not a general CPU identification library, and a
/// field is added when a kernel is written that needs it, so that every entry
/// here has something in the project that reads it. AVX-512 is absent because
/// the target part does not implement it, which section 2 of the implementation
/// plan records; Phase 7's kernel is 256-bit for that reason rather than by
/// preference.
struct CpuFeatures {
    /// 256-bit floating-point vectors, with the operating system's consent.
    ///
    /// True only when the processor reports AVX and the operating system has
    /// enabled the upper vector state, so a caller can act on this alone.
    bool avx{};

    /// 256-bit integer vectors and the wider shuffles, on the same terms.
    bool avx2{};

    /// Fused multiply-add. Separate from AVX2 because the two are separate
    /// feature bits, and because what the kernel gains from each is different:
    /// AVX2 supplies the width and FMA halves the number of rounding steps in
    /// the accumulation.
    bool fma{};
};

/// What this processor supports, computed once and cached.
///
/// The result cannot change while the process runs, and the detection is a few
/// serialising instructions, so it is worth not repeating. Thread safe: the
/// initialisation happens on first call under the guarantee C++ gives a
/// function-local static.
[[nodiscard]] const CpuFeatures& cpu_features() noexcept;

/// The processor's own name for itself, or an empty view where it does not
/// offer one.
///
/// Recorded beside every measurement rather than used to decide anything. A
/// performance figure filed without saying which processor produced it is not a
/// measurement of anything, and section 7 of the implementation plan asks Phase
/// 7's harness to record the machine state alongside the numbers. Dispatch uses
/// the feature bits above and never this string, because a name is a marketing
/// decision and a feature bit is an answer.
[[nodiscard]] std::string_view cpu_brand() noexcept;

} // namespace orrery::backend
