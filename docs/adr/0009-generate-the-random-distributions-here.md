# ADR-0009: Generate the random distributions in the project rather than with the standard library

- **Status:** Accepted
- **Date:** 2026-08-10

## Context

Sampled initial conditions are the input to almost every result this project
will report: the scaling studies, the conservation tests, the accuracy of the
tree solver against direct summation, and the regression tests with golden
outputs. Each of those is a claim about a specific set of particles, and a claim
that cannot be rechecked on the same particles is not evidence.

The requirement is therefore that a seed determines the configuration, on every
compiler and platform the project builds on. The `<random>` header meets half of
it and not the other half, and the split is easy to miss:

- The engines are specified exactly. `std::mt19937_64` is defined in the
  standard by its recurrence and its initialisation, so a given seed gives the
  same stream of 64-bit values in every implementation.
- The distributions are specified only by their output distribution.
  `std::uniform_real_distribution` and `std::normal_distribution` are free to
  consume the engine in whatever way they like, and the three major standard
  libraries do differ. A configuration sampled with them is reproducible on the
  machine that produced it and nowhere else.

The project builds with Clang, GCC and MSVC, which is all three
implementations, and continuous integration runs on Linux, macOS and Windows.

## Decision

`core/random.hpp` wraps `std::mt19937_64` and provides the project's own
mapping from its bits to the values the samplers need: a uniform number in
[0, 1), a uniform number in a range, and a direction drawn uniformly over the
sphere. No `std::` distribution is used in library code.

The uniform mapping takes the top bits of one engine draw, as many as the
mantissa of `Real` can hold, and scales by an exact power of two. That makes the
result exactly representable, keeps the interval half open in both precision
builds, and consumes exactly one engine draw per value in each, so the two
builds stay at the same point in the stream.

## Alternatives considered

**Use the standard distributions and accept per-platform reproducibility.** The
smallest amount of code, and adequate if results are only ever reproduced on the
machine that made them. It fails the case that matters: a reviewer, or the
project's own continuous integration, rerunning a reported figure on different
hardware and getting different particles. It would also make golden-output
regression tests impossible to write portably, and those are one of the four
test categories the plan requires.

**Fix the standard library by pinning one implementation.** Building everything
against libc++ everywhere would make the distributions consistent. It is a large
constraint on the project's build for a small piece of behaviour, it would not
survive a toolchain update that changed libc++'s algorithm, since nothing
promises it will not, and it conflicts with testing on all three compilers.

**Write the engine as well.** A counter-based generator, or one of the xoshiro
family, would be smaller and faster than the Mersenne twister and equally
reproducible. Speed is irrelevant here, since randomness is consumed once at
setup and never inside a kernel, and the twister has the advantage that it is
specified by a standard rather than by this project: a reader can check the
engine against the standard's definition instead of against this code. If a
later phase needs per-thread streams, that argument may be revisited in a new
ADR.

**Also provide a normal distribution.** The usual companion to a uniform one,
and the obvious way to sample velocities. Nothing in this project needs one:
Plummer speeds come from rejection sampling of the model's distribution
function, and directions come from Archimedes' construction, neither of which
uses a normal variate. An untested generator kept for a future caller is a
liability rather than an asset, so it is left out until something asks for it.

## Consequences

The uniform values are bit-identical across platforms. Quantities derived from
them through the standard maths library, a square root, a cube root, a sine, are
identical only as far as that library's last bit, which no standard guarantees.
Reproducibility of a sampled configuration is therefore exact within a platform
and correct to a few units in the last place across them. The tests over sampled
configurations compare against tolerances rather than against stored values, and
the header says so.

The rejection sampler in the Plummer generator consumes a number of draws that
depends on the values drawn. That is deterministic given the seed, but it means
a change in the rejection constant changes the whole sequence after it, not just
the speeds. It also means the single-precision build can diverge from the
double-precision one at a draw that falls on the acceptance boundary, and from
that point on the two builds sample different configurations. Both remain valid
samples of the same model, which is why the statistical tolerances in the tests
are set from the sampling scatter rather than from one observed run.

Anything the project later needs, a normal variate, a shuffle, a Poisson count,
has to be written and tested here rather than taken from the standard library.
