# ADR-0051: Compile the solver to WebAssembly rather than write a second one

- **Status:** Accepted
- **Date:** 2026-08-13

## Context

The instrument plays trajectories this repository produced. It draws them
faithfully and it cannot compute anything, so every control that would change
the physics is drawn back with a note saying it needs a run. Making one of those
controls work means having a solver in the browser.

There are two ways to get one. Write it again in TypeScript, which is a few
hundred lines for direct summation and a few hundred more for a tree; or compile
the C++ that is already here and already measured.

The second is only obviously better if it works. WebAssembly has no threads
without response headers this project's host will not send, no AVX2, and a
download budget measured in hundreds of kilobytes, and the library it would be
compiling reaches for all three of the first and knows nothing about the last.

## Decision

Compile the existing solver. A `wasm` preset drives the Emscripten toolchain
over `core`, `initial_conditions`, `integrators` and the CPU solvers, and the
result is reached through a narrow C interface in `wasm/orrery_wasm.h`.

The reason is not that reimplementing it would be hard. It is that a second
implementation of the physics is the thing this project has refused at every
other opportunity, and refused for a reason that applies here more strongly than
anywhere else. ADR-0026 put the GPU behind the solver interface rather than in
front of it, so that there would not be two versions of the force calculation to
keep in step. A browser version written by hand would be a third, in a different
language, tested by different tools, and the one most people would actually see.
When it drifted, and it would, the drift would be invisible: a plausible picture
of a galaxy is not a thing anybody can check by looking at it.

Compiling instead makes the claim testable, and `web/tests/solver/agreement.test.ts`
is the test. It runs one configuration in the module and compares the result
against a trajectory and a diagnostics file the native build wrote. That
comparison is only meaningful because it is the same code: what it measures is
the difference between two compilers and two instruction sets, which is a few
units in the last place, rather than the difference between two people's ideas of
what the physics is.

## What is not compiled

The command-line programs, the renderer, the SYCL backend and the library's file
I/O. A browser tab has no argument vector, no OpenGL context this project would
use, no device the SYCL runtime can see and nowhere to write a trajectory. None
of that needed a change to the library: each of those is already a layer or a
build option, so leaving them out is a matter of which targets the preset builds
and which sources the module links.

One thing did need a change, in `src/solvers/CMakeLists.txt`. Emscripten reports
`CMAKE_SYSTEM_PROCESSOR` as x86, so the guard that decides whether to compile the
AVX2 kernel admitted a target that is wasm32. The kernel was compiled, the
run-time dispatch of ADR-0018 correctly declined to call it because there is no
CPUID to ask on that target, and the module carried a kernel it could never
execute. The guard now excludes Emscripten by name. That is the whole of what the
existing dispatch needed: on this target it has one kernel to choose from and
chooses it, which is the same mechanism doing the same thing.

## The boundary is a C interface, not a binding

Emscripten will generate bindings over C++ classes. It is not used here. The
whole of what a browser needs from this library is: make a simulation from a
configuration, step it, read the positions, read the conserved quantities,
destroy it. That is about a dozen functions taking numbers and pointers, and
writing them down is cheaper than carrying a binding generator's view of a class
hierarchy into a download that has a size budget.

A configuration crosses as the text of an `.orrery` file rather than as a
structure of numbers. That is the decision in this ADR with the longest reach.
It means the browser and the command line read the same document with the same
parser, so a configuration edited in a browser is a configuration the native
binary runs, and there is no second definition of what a configuration is to
keep in step. It costs the module the configuration reader and, with it, the
standard library's streams: about forty kilobytes of the download, which is a
tenth of the budget for the only thing on the boundary that would otherwise have
been written twice.

The interface reports failure as a value. Every entry point catches, returns a
null or a zero, and leaves a sentence in `orrery_last_error`. The library beneath
throws, as this project's rules ask of a configuration boundary, and a browser
cannot catch a C++ exception: this is where the two meet.

## Alternatives considered

**Write the solver again in TypeScript.** No toolchain, no download, no build
step, and the numbers would be the browser's own. Rejected on the argument above:
it is a second implementation of the only thing this project is about, and the
one that most readers would be looking at.

**Run everything server-side and stream the result.** That is Phase 8 and it is
being built. It is not a substitute for this: a preview that has to make a
network round trip per edit is not a preview, and a service that is down leaves
the instrument with nothing to show, which is what ADR-0054 exists to avoid.

**Ship the whole library, including the renderer and the file formats.** Every
one of those has a browser-native counterpart that is better than a compiled one:
the renderer's counterpart is WebGPU, and the trajectory reader's is
`web/src/trajectory/`, which already exists and is already tested against files
the C++ wrote.

**Use embind, or `WebIDL`.** Both would remove hand-written glue and neither
removes the question of what should cross. Since the answer to that is twelve
functions, the glue they would remove is the twelve declarations that document
the boundary.

## Consequences

The module is 103 kB gzipped and its loader 4 kB, against a budget of 400 kB
asserted by `web/tools/budget.ts`. Most of what is in it is the configuration
reader and the standard library's streams; the solvers, the integrators and the
samplers together are a small part of it.

The browser build must never be mistaken for the machine the performance report
was taken on, so the plate states what produced the picture: the solver, the
kernel, the thread count, the measured step time and the energy drift, all
reported by the module rather than decided by the page.

There is now a toolchain in continuous integration that most contributors will
not have. The agreement test skips itself, loudly, when the module is absent, and
`.github/actions/solver` builds it before the suites that need it, so a
contributor without Emscripten can still run everything else.

A run sampled in the browser and the same run sampled natively are not the same
run for every scenario, and that is a property of the samplers rather than of
this decision. `core/random.hpp` already says why: the random stream is
bit-identical everywhere, but the Plummer sampler draws its speeds by rejection,
so one unit in the last place of a square root can flip an acceptance and move
every later draw one place along the stream. The agreement test uses the uniform
sphere, which draws a fixed number of times per particle and has no such loop.
