# The solver in a browser

The instrument plays trajectories this repository produced. It can also compute
one, because the same C++ that the native binary runs is compiled to WebAssembly
and stepped in a Worker in the page. This describes what that build is, what it
is not, and what it costs.

The short version: it is the physics, not the performance. Everything the
performance report quotes was measured on the machine that report names, with
eight threads and an AVX2 kernel. The browser build has one thread and no vector
kernel, and it says so on the plate for as long as it is running.

## Building it

```
cmake --preset wasm
cmake --build --preset wasm
```

The preset needs the Emscripten toolchain on the environment, which means
running emsdk's `emsdk_env` script first so that `EMSDK` is set. The module is
written into `web/public/solver`, where the client serves it from as a static
asset, and it is not committed: it is compiler output, like the published
gallery beside it.

Continuous integration builds it through `.github/actions/solver`, pinned to
Emscripten 6.0.6, before the suites that need it. A checkout without the
toolchain builds and tests everything else; the one test that needs the module
skips itself and says so.

## What is in it

`core`, `initial_conditions`, `integrators` and the CPU solvers. Not the
command-line programs, not the renderer, not the SYCL backend, and not the
library's file I/O.

The whole of what crosses the boundary is in `wasm/orrery_wasm.h`: about a dozen
C functions that take numbers and pointers. Make a simulation from a
configuration, step it, read the positions, read the conserved quantities,
destroy it. Nothing else. ADR-0051 records why that is a C interface written by
hand rather than a binding generated over the library.

A configuration crosses as the text of an `.orrery` file. The browser and the
command line therefore read the same document with the same parser, which is what
will let the initial-conditions editor produce a file the native binary runs
unchanged.

The build is double precision, as the default native build is, and reports so
through `orrery_scalar_size`. Positions are narrowed to `float` on the way out
because what reads them is a renderer; the conserved quantities cross as doubles,
because that is where the numerical claim is made.

## What it refuses

**More than 4096 particles.** The limit is in the module, reported by
`orrery_particle_limit`, and a configuration asking for more is refused with a
sentence saying by how much. The client asks the module what the limit is rather
than keeping its own copy, and reduces the published configuration to fit.

**More than 1024 steps in one call.** WebAssembly has no signals and a Worker
does not read its message queue while a call is on its stack, so a call that ran
for a minute would be a run that could not be stopped.

**Threads.** The module is built without them, and a configuration naming the
work-stealing scheduler is run serially and told so. Shared memory in a browser
needs two response headers that GitHub Pages will not send; ADR-0052 sets out
what that costs and what the alternatives were.

**Output paths.** A browser tab has nowhere to write a trajectory, so the output
section is ignored, and `orrery_report` says that it was rather than leaving a
run that quietly wrote nothing.

## What it costs

The module is 103 kB gzipped and its loader 4 kB, against a budget of 400 kB
that `web/tools/budget.ts` asserts on every build. Most of that is the
configuration reader and the standard library's streams that come with it.

Step times, measured in Chromium on the development machine at a softening of
0.05, as the median of nine steps after three warm-up steps:

| Particles | Direct | Barnes-Hut |
| --- | --- | --- |
| 256 | 0.2 ms | 0.3 ms |
| 512 | 0.8 ms | 0.8 ms |
| 1024 | 2.9 ms | 2.1 ms |
| 2048 | 12.1 ms | 7.5 ms |
| 4096 | 44.5 ms | 27.7 ms |

The quadratic term in the direct solver is plain in that column, and it is what
sets the particle limit: the next power of two would be a Worker occupied for
three tenths of a second per step.

For comparison, [the performance report](performance.md) has the native direct
solver at 8192 particles taking 13.8 ms a force evaluation in double precision,
with the AVX2 kernel on eight threads, on the same machine. Halving the particle
count quarters that, so the native figure at 4096 is about 3.5 ms against this
build's 44.5: roughly thirteen times, which is about what one thread instead of
eight and no vector kernel instead of a 256-bit one should cost. The browser
build is not the native one and is not meant to be.

## Whether it agrees with the native build

Compiling the solver rather than reimplementing it is only worth anything if the
compiled one gives the same answers, so that is a test.
`web/tests/solver/agreement.test.ts` runs
`web/tests/fixtures/agreement.orrery` in the module and compares what it
computes against a trajectory and a diagnostics file the native build wrote from
the same configuration:

```
orrery run web/tests/fixtures/agreement.orrery
```

Sixty-four particles, direct summation, two hundred steps. The two are not
required to agree bit for bit and do not: the native build accumulates with the
AVX2 kernel, which reassociates the sum and rounds once where the scalar kernel
rounds twice, and `solvers/direct_kernel.hpp` sets out exactly what that is
allowed to change. On the development machine they agree to the last bit or two
in the conserved quantities and to what a `float` can hold in the positions.

The configuration is a uniform sphere rather than a Plummer sphere, and the
reason is worth stating because it is a limit of the project rather than of this
build. `core/random.hpp` promises that the random stream is bit-identical on
every platform, and that quantities derived from it through the standard maths
library agree only to that library's last bit. The Plummer sampler draws its
speeds by rejection, so one unit in the last place in a square root can flip an
acceptance, move every later draw one place along the stream, and produce a
configuration that is not a perturbation of the other but a different one. The
uniform sphere draws a fixed number of times per particle and has no such loop,
so the two builds start from the same particles and a comparison of where they
end up is a comparison of the solver.

## Using it from the instrument

The solver tier of the console carries a control that runs the scenario on the
plate in the browser. It reduces the published configuration to what the module
will take, which for the collision is 4096 particles, and to the first two
thousand steps, and the note beside the control states both.

While it runs, the plate's catalogue carries what the module reported: the
solver, the kernel, the thread count, the measured step time and the relative
energy error. That is the same information a native run prints, in the place a
plate records the conditions an exposure was taken under, and it is there so
that a picture computed in a tab cannot be mistaken for one of the figures
above.
