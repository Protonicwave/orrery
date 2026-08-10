# ADR-0018: Compile the vector kernel apart and choose it at run time

- **Status:** Accepted
- **Date:** 2026-08-10

## Context

Phase 7 adds an explicitly vectorised AVX2 kernel to the direct solver. Section
2 of the implementation plan settles the width: the target part has AVX2 and no
AVX-512, so the kernel is 256 bits wide because that is what the machine has.

What it does not settle is how the instructions get into the binary. Using an
AVX2 intrinsic requires the translation unit to be compiled with AVX2 enabled,
and a processor that does not implement AVX2 faults on the first such
instruction it is handed. So the question is which translation units are
compiled for which instruction set, and how the program decides what to execute.

The obvious answer is to compile everything for the machine in front of you,
with `-march=native`, and it is the answer most single-machine numerical
projects take. It is wrong here for three reasons that are worth stating
separately.

A binary built that way runs on one machine. Section 5 of the implementation
plan requires the project to build and test on Linux, macOS and Windows across
three compilers, and continuous integration runners are not the development
laptop. The failure mode is also the worst available: not a diagnostic at build
time but an illegal instruction at run time, in the middle of the innermost
loop, on somebody else's computer.

It also makes the phase's own measurement impossible. Phase 7 reports what
vectorisation was worth, which is a comparison between a scalar kernel and a
vector one. Under `-march=native` the compiler auto-vectorises the scalar
kernel too, so the comparison becomes one between hand-written AVX2 and
whatever the compiler decided that day, and the baseline changes with the
compiler version.

And it would quietly vectorise the reference. The direct solver in double
precision is what every approximation in this project is measured against, and
the value of that comparison depends on the reference being the plain,
inspectable summation it says it is.

## Decision

The AVX2 kernel lives in one translation unit, `src/solvers/direct_kernel_avx2.cpp`,
compiled with `-mavx2 -mfma` or `/arch:AVX2` through per-source properties. Every
other translation unit in the project is compiled for the baseline instruction
set. CMake compiles that file only on x86 targets, and defines
`ORRERY_HAS_AVX2_KERNEL` on the interface of `orrery::solvers` when it does.

Which kernel runs is decided at run time. `backend/cpu_features.hpp` asks CPUID
for AVX2 and FMA, and asks the extended control register whether the operating
system has enabled the vector state that comes with them.
`solvers::accumulate_range_for` returns the vector kernel only when the build
compiled it and the machine can execute it, and the scalar kernel otherwise. The
choice is resolved once per force evaluation and read from a local, so it costs
one indirect call ahead of N^2 interactions.

## Alternatives considered

**Build the whole project for the host, with `-march=native`.** The three
reasons above. The one thing it has going for it, that the compiler may
vectorise code nobody thought to vectorise by hand, is worth less here than it
looks: the direct kernel is the only loop in the project hot enough to matter
so far, and it is now vectorised deliberately.

**Compile-time dispatch on a build option, with no run-time check.** Two
binaries, one for machines with AVX2 and one without, chosen by whoever runs
them. Simpler in the source and it moves the decision to somebody who may not
know the answer. It also doubles the build matrix for a check that costs one
CPUID instruction per process.

**Function multiversioning.** GCC and Clang can emit several versions of a
function and resolve between them through the dynamic loader, which would remove
the explicit dispatch entirely. MSVC has no equivalent, so the project would
carry the explicit path anyway for one of its three required compilers, and it
would carry it in a form that only ever ran on Windows and would therefore be
the least tested of the three.

**A portable SIMD library, or `std::experimental::simd`.** A real option and the
one most likely to be right in a few years. Rejected now on availability: the
standard version is not in a released standard this project can require, and the
third-party libraries that implement the idea would each be a dependency
carrying its own portability surface, for a kernel that is forty lines long. The
helpers at the top of the kernel file are the twenty lines of that idea this
project actually needs. If a second vector kernel appears, for the tree walk of
Phase 8 or for a different instruction set, this is the decision to revisit.

**Leave it to the compiler's auto-vectoriser.** Tempting, because the scalar
loop has no control flow in it and was deliberately written that way in Phase 5.
Rejected because the result is not reproducible across the three compilers the
project supports, and because a compiler will not reassociate the accumulation
without a fast-math flag that ADR-0020 declines to set. Without reassociation
the sum is a serial dependency chain and the loop does not vectorise
profitably, which is exactly the situation an explicit kernel is for: the
reassociation becomes a documented, tested decision about one loop instead of a
flag that silently changes the arithmetic of the whole project.

## Consequences

The project carries two implementations of one summation, which is the thing
section 3 of the implementation plan warns about. The mitigation is structural:
they are two functions behind one signature, called from one outer loop written
once, and `tests/solvers/direct_kernel_test.cpp` checks them against each other
at every range length modulo the lane count. The scalar one is not an
abandoned first draft; it is the reference, it runs in the test suite on every
machine, and it is timed in every benchmark row.

The two kernels do not produce bit-identical answers, for the reasons
`solvers/direct_kernel.hpp` sets out, and ADR-0020 covers what that is allowed
to mean. Each kernel remains bit-for-bit reproducible on its own, including
across thread counts, which is the property the direct solver's standing as a
reference actually rests on.

An accuracy or performance result now has to say which kernel produced it. The
solver reports the kernel it settled on for that reason, and every table in
`docs/performance/` carries the column.

A machine without AVX2 runs the scalar kernel and is slower, correctly and
without being told. That is the intended behaviour and it is why
`accumulate_range_for` falls back rather than refusing, but it does mean a
benchmark that quoted a vector row without checking `kernel_available` would be
reporting scalar timings under a vector heading. The solver's `kernel()`
accessor exists so that no benchmark in this project has to.
