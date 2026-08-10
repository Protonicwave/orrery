# ADR-0006: Select the scalar precision when the project is configured

- **Status:** Accepted
- **Date:** 2026-08-10

## Context

The project has two uses for floating-point numbers that pull in opposite
directions.

Validation needs accuracy. A Kepler orbit integrated for many periods, the
energy conservation of a symplectic scheme against the secular drift of RK4, and
the comparison of an approximate solver against direct summation are all
measurements where single precision would put the rounding error above the
effect being measured.

Throughput needs bytes. Every kernel of interest is limited by memory bandwidth
on this hardware, and `float` halves the bytes each particle costs. It also
doubles the number of lanes in a 256-bit vector register and is the precision
the integrated GPU is fastest at. A single-precision run is not a degraded
double-precision run; it is the configuration in which the largest particle
counts are reachable at all.

Both are wanted, and no single run needs both.

## Decision

`orrery::core::Real` is an alias for `double`, or for `float` when the project
is configured with `ORRERY_SINGLE_PRECISION`. The option is carried publicly on
the `orrery::options` interface target, so a consumer cannot disagree with the
library about it, and `CMakePresets.json` provides the single-precision
configuration as a preset that continuous integration builds and tests on every
pull request.

Code that needs to know which build it is in reads
`orrery::core::kSinglePrecision`, which is how a test tolerance can follow the
precision it is being applied at.

## Alternatives considered

**Template every solver, integrator and container on the scalar type.** The
answer a C++ library would usually give, and the one that allows both precisions
in one binary. It costs more than it returns here. Every solver, integrator and
kernel becomes a template, which means the implementations move into headers or
into explicit instantiations, compile times multiply, and the SYCL kernels of
Phase 9 acquire a template parameter that the toolchain has to instantiate for
each device. The return would be the ability to mix precisions within a run,
which nothing in the project's plan asks for.

**Choose the precision at run time from a flag.** This means both instantiations
exist in the binary and a virtual boundary selects between them, which is the
templated design plus the double compile time plus a dispatch. It also makes
every reported measurement ambiguous about which path produced it unless the
run records it, whereas a build that can only do one thing cannot be confused
about which it did.

**Use `double` everywhere and never offer `float`.** Simple, always accurate,
and it discards the configuration in which the hardware is fastest. It would
also make the performance chapters of the project less interesting, since the
difference between the two is one of the clearer demonstrations that these
kernels are bound by bandwidth rather than by arithmetic.

**Use `float` everywhere.** Fastest and unable to demonstrate correctness. The
validation results are the point of the project, and several of them cannot be
shown at all in single precision.

## Consequences

Every configuration has to be built and tested twice, once per precision. That
is a continuous integration cost rather than a development cost, and the
single-precision preset exists so it is one command either way.

Tests cannot hard-code tolerances. A tolerance written as an absolute number is
an assertion about double precision that silently passes in single precision or
fails there for no physical reason, so tolerances are expressed in terms of
`std::numeric_limits<Real>::epsilon()` and the magnitudes involved.

Binaries of the two configurations are not interchangeable. Every interface in
the project changes size with the setting, so a library built one way and a
consumer built the other would disagree about the size of everything they
exchange. `orrery::core::uses_single_precision()` reports the library's own
setting at run time so that the disagreement can be detected rather than
inferred from a crash, and a test compares it against the constant this
translation unit was compiled with.

Where an algorithm needs a specific precision regardless of the build, it must
say so with an explicit type rather than with `Real`. The compensated-summation
reference that Phase 7 checks the vectorised kernels against is the case already
foreseen: it is a statement about accumulated rounding error and it means
nothing if it is compiled at the same precision as the thing it is checking.
