#pragma once

/// \file
/// The innermost loop of the direct solver, in more than one instruction set.
///
/// Phase 5 wrote the direct kernel once, scalar and obviously correct, and
/// Phase 6 divided its outer loop between threads without touching the
/// arithmetic. This file is where the arithmetic itself gains a second
/// implementation: the same sum, over the same terms, computed four or eight
/// pairs at a time with AVX2.
///
/// ## What a kernel is here
///
/// One function, computing the acceleration on a single target particle from
/// the sources in a half-open range of indices, without the factor of G.
/// Everything above it, which targets to compute, how they are divided between
/// threads, where the ranges come from, stays in `direct_solver.cpp` and is
/// written once. So the two kernels are two implementations of one summation
/// rather than two solvers, which is the distinction section 3 of the
/// implementation plan draws between a backend and a copy of the physics.
///
/// The range is a parameter for the reason Phase 5 gave: the particle a body
/// does not attract is itself, and the two ranges either side of its index are
/// how that is arranged without a branch in the innermost loop. The vector
/// kernel inherits the benefit directly. A masked self-interaction would need a
/// comparison and a blend on every iteration of a loop that runs N^2 times, to
/// suppress one term in N.
///
/// ## Which one runs
///
/// Chosen at run time, from what the processor reports. The AVX2 kernel is
/// compiled in a translation unit of its own with the instruction set enabled,
/// and the rest of the project stays at the baseline, so one binary runs on
/// machines with and without it. ADR-0018 records why that is preferred to
/// building the whole project for the machine that compiled it.
///
/// A caller that wants a particular kernel asks for it and then reads back what
/// it got. `accumulate_range_for` never returns something the machine cannot
/// execute, and a benchmark that quoted an AVX2 row while running the scalar
/// kernel would be worse than one that refused, so the effective choice is
/// reported rather than assumed.
///
/// ## How the two differ, exactly
///
/// The vector kernel is not bit-for-bit identical to the scalar one, and the
/// two reasons are worth stating precisely because everything this project
/// claims about accuracy is a claim about the difference between a fast answer
/// and the reference answer.
///
/// The first is summation order. The scalar kernel adds the terms in index
/// order into one accumulator per component. The vector kernel keeps one
/// accumulator per lane per component and adds them together at the end, so the
/// sum is reassociated. Floating-point addition is not associative, so the
/// results differ in the last bits. They differ in the vector kernel's favour:
/// splitting a sum of n terms into w independent partial sums reduces the
/// accumulated rounding error, which `tests/solvers/kernel_accuracy_test.cpp`
/// measures against a compensated sum rather than asserting.
///
/// The second is fused multiply-add. The vector kernel accumulates with FMA,
/// which rounds once where the scalar kernel rounds twice. That is an IEEE-754
/// operation with its own correct rounding, not a relaxation of the standard.
/// ADR-0020 records that this project enables no fast-math flag anywhere, and
/// why the two deviations above are acceptable when reassociation by the
/// compiler would not be.
///
/// Both kernels remain deterministic and independent of the thread count. The
/// lanes a term lands in depend only on the range it was given, and the ranges
/// depend only on the target index, so a threaded evaluation is still bit for
/// bit identical to a serial one for whichever kernel is in use. That property
/// is what lets the direct solver remain the project's reference, and
/// `tests/solvers/parallel_direct_solver_test.cpp` asserts it.

#include <cstdint>
#include <span>
#include <string_view>

#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/core/vec3_span.hpp"

namespace orrery::solvers {

/// Which implementation of the summation to use.
enum class KernelKind : std::uint8_t {
    /// One pair at a time, in index order. The reference.
    ///
    /// Kept and exercised on every machine, including those where the vector
    /// kernel is available and faster, because a reference that only runs on
    /// hardware without AVX2 is a reference nobody runs.
    kScalar,

    /// Four pairs at a time in double precision, eight in single, with AVX2 and
    /// FMA.
    kAvx2,
};

/// The signature both kernels have.
///
/// A plain function pointer rather than a virtual call or a `std::function`.
/// The choice is made once per parallel chunk and read from a local, so the
/// indirection is paid a few tens of times per force evaluation against the N^2
/// interactions inside it, which is the boundary section 3 of the
/// implementation plan permits dispatch at. It is emphatically not paid per
/// pair.
using AccumulateRange = core::Vec3 (*)(core::Vec3Span<const core::Real> positions,
                                       std::span<const core::Real> masses, core::Vec3 target,
                                       core::Index begin, core::Index end,
                                       core::Softening softening) noexcept;

/// The acceleration on a particle at `target` from the sources in
/// `[begin, end)`, without the factor of G, one pair at a time.
///
/// Declared here as well as reachable through `accumulate_range_for` because it
/// is the reference the other kernels are measured against, and a test that
/// wants the reference should not have to ask a dispatcher for it.
[[nodiscard]] core::Vec3 accumulate_range_scalar(core::Vec3Span<const core::Real> positions,
                                                 std::span<const core::Real> masses,
                                                 core::Vec3 target, core::Index begin,
                                                 core::Index end,
                                                 core::Softening softening) noexcept;

#ifdef ORRERY_HAS_AVX2_KERNEL
/// The same sum, four pairs at a time in double precision and eight in single.
///
/// Declared only where it is compiled, which is on x86-64 targets, and defined
/// in a translation unit built with the instruction set enabled. Whether it may
/// then be *executed* is a separate question about the machine rather than the
/// build, and `kernel_available` is the one that answers it. A caller that
/// reaches this symbol directly has taken responsibility for asking; everything
/// in the project goes through `accumulate_range_for` instead.
///
/// The build option is a preprocessor definition rather than a C++ constant
/// because it decides whether a declaration exists at all, which is the one
/// question C++ cannot ask. It follows the precedent of the precision switch in
/// `core/types.hpp` and is set on this library's interface by CMake.
[[nodiscard]] core::Vec3 accumulate_range_avx2(core::Vec3Span<const core::Real> positions,
                                               std::span<const core::Real> masses,
                                               core::Vec3 target, core::Index begin,
                                               core::Index end, core::Softening softening) noexcept;
#endif

/// Whether this build, on this machine, can run `kind`.
///
/// Two conditions, and both have to hold. The kernel has to have been compiled,
/// which is a property of the build and false on a target this project has no
/// vector kernel for, and the processor has to implement the instructions,
/// which is a property of the machine and asked through
/// `backend/cpu_features.hpp`.
///
/// `kScalar` is always available. That is the point of it.
[[nodiscard]] bool kernel_available(KernelKind kind) noexcept;

/// The fastest kernel this machine can run.
///
/// The default for a solver that was not told otherwise, and the one figure a
/// caller has to override to measure the scalar kernel on a machine that has
/// AVX2.
[[nodiscard]] KernelKind fastest_available_kernel() noexcept;

/// The function implementing `kind`, or the scalar kernel where `kind` is not
/// available here.
///
/// Never returns null and never returns something the machine cannot execute.
/// A caller that needs to know whether it got what it asked for calls
/// `kernel_available` first; `DirectSolver` does exactly that and reports the
/// kernel it settled on.
[[nodiscard]] AccumulateRange accumulate_range_for(KernelKind kind) noexcept;

/// How many pairs `kind` computes at once, in the precision this build uses.
///
/// One for the scalar kernel, and 32 / sizeof(Real) for AVX2, since the vector
/// is 256 bits wide either way. Reported so that a benchmark can state the
/// width it measured rather than leaving a reader to infer it from the build
/// configuration.
[[nodiscard]] core::Index kernel_lane_count(KernelKind kind) noexcept;

/// A short name, for benchmark tables and reports.
[[nodiscard]] std::string_view to_string(KernelKind kind) noexcept;

} // namespace orrery::solvers
