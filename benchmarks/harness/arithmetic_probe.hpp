#pragma once

/// \file
/// The two arithmetic probes, in the instruction sets this project has kernels
/// for.
///
/// Separated from `machine_limits.hpp` because the vector versions are compiled
/// for an instruction set the rest of the harness is not compiled for, exactly
/// as the direct kernel is in ADR-0018 and for the same reason, and because
/// nothing outside the harness has any use for them.
///
/// ## Why there are two
///
/// A part's advertised floating-point peak is the rate of its fused
/// multiply-add units, because that is the largest number available. Almost no
/// real kernel issues only fused multiply-adds, and this project's does not:
/// each pairwise interaction in the direct solver contains one square root and
/// one division, which on every x86 part are handled by a unit with a fraction
/// of the throughput of the multiply-add pipelines.
///
/// Comparing the direct kernel against the multiply-add peak alone is therefore
/// a comparison against a ceiling it could not reach with a perfect
/// implementation. It is still worth making, because it is the number everybody
/// else quotes and because the gap is informative. But the useful question,
/// which section 7 of the implementation plan phrases as the achieved fraction
/// of the *relevant* hardware limit, needs the second probe: how many square
/// roots and divisions per second can this machine actually retire.
///
/// ## What a block does, and why that shape
///
/// Both probes run `kChains` independent chains for `rounds` iterations. Three
/// properties matter and each is deliberate.
///
/// The chains are independent, so no instruction waits for the one before it. A
/// single chain would measure latency rather than throughput, and on this part
/// those differ by a factor of four for a multiply-add and considerably more
/// for a division. Twelve chains is enough to cover both latencies and still
/// keeps every accumulator in a register: of the sixteen vector registers,
/// twelve hold accumulators and two hold the constants. A probe that spilled to
/// the stack would be measuring memory again, and one with too few chains would
/// report a ceiling lower than a real kernel reaches, which is worse than
/// useless because it would make the kernel look finished.
///
/// The recurrences converge rather than diverge. Nothing overflows however long
/// a probe runs and no chain decays into the subnormal range, which on some
/// parts is enormously slower and would make the probe measure the wrong thing
/// entirely.
///
/// The result has to go somewhere. A loop whose output is never read is dead
/// code, and at the optimisation level every figure in this project is measured
/// at, a compiler will delete it and report an infinite rate. Writing the
/// answer through a caller-supplied pointer is what keeps it alive.

#include <cstdint>

#include "orrery/core/types.hpp"

namespace orrery::benchmark {

/// How many independent chains one block runs. See above.
inline constexpr int kChains = 12;

/// Multiply-add throughput, portably.
///
/// The baseline instruction set has no fused multiply-add, and this project
/// sets no flag that would let a compiler contract a multiply and an add into
/// one (ADR-0020), so this measures separate multiply and add throughput. That
/// is the correct ceiling for a machine that has nothing better.
[[nodiscard]] double fused_multiply_add_block_scalar(std::uint64_t rounds,
                                                     core::Real* sink) noexcept;

/// Square root and division throughput, portably.
///
/// The recurrence is `acc = 1 / sqrt(acc + 1)`, whose fixed point is near 0.68,
/// so every chain settles rather than drifting. One square root and one
/// division per round per chain, which is exactly what one pairwise interaction
/// of the direct kernel costs.
[[nodiscard]] double divide_and_sqrt_block_scalar(std::uint64_t rounds, core::Real* sink) noexcept;

#ifdef ORRERY_HAS_AVX2_PROBE
/// The same two, four lanes at a time in double precision and eight in single,
/// with real fused multiply-adds.
[[nodiscard]] double fused_multiply_add_block_avx2(std::uint64_t rounds, core::Real* sink) noexcept;

[[nodiscard]] double divide_and_sqrt_block_avx2(std::uint64_t rounds, core::Real* sink) noexcept;
#endif

} // namespace orrery::benchmark
