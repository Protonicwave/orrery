# Orrery: Implementation Plan

A GPU-accelerated N-body gravitational simulator in C++20, built and benchmarked
entirely on a Lunar Lake laptop.

This document is the single source of truth for how Orrery is built. Each phase
below is designed to be completed in one working session, ending in a pull
request against `main`. Read sections 1 to 6 before starting any phase.

---

## 1. Purpose

Simulate the gravitational interaction of large numbers of point masses, fast
enough and accurately enough to be worth taking seriously, on consumer laptop
hardware.

Three goals, in priority order:

1. **Correctness that can be demonstrated.** The code must be validated against
   problems with known analytic solutions, not merely against its own output.
2. **Performance that can be quantified.** Speed claims must be expressed as a
   fraction of measured hardware limits, not as raw timings in isolation.
3. **Engineering that survives inspection.** Any single file should be
   defensible on its own terms to a reviewer who did not write it.

Non-goals: general relativity, hydrodynamics, collisional stellar dynamics with
regularisation, and distributed multi-node execution. Each is a reasonable
extension and none is in scope.

---

## 2. Target hardware

Every performance decision in this plan follows from these figures. They are
recorded here so that later work can be judged against the machine it was
written for.

| Property | Value |
| --- | --- |
| CPU | Intel Core Ultra 5 238V (Lunar Lake) |
| Cores | 8 physical, 8 threads, no SMT |
| Core topology | 4 performance cores (Lion Cove) plus 4 efficiency cores (Skymont) |
| Vector width | AVX2, 256-bit. No AVX-512 on this part |
| L3 cache | 8 MB |
| Memory | 32 GB LPDDR5X-8533, on package |
| Peak memory bandwidth | Approximately 135 GB/s, shared between CPU and GPU |
| GPU | Intel Arc 130V, Xe2 architecture, 7 Xe-cores |
| GPU memory | Unified with system memory. No discrete VRAM, no PCIe transfer |

Three properties drive the architecture:

**Heterogeneous cores.** Performance and efficiency cores differ substantially in
throughput. Any scheme that divides work into equal fixed shares will leave the
performance cores idle while the efficiency cores finish. Load balancing must be
dynamic. This is measured and reported in Phase 6.

**Unified memory.** There is no host-to-device copy. With SYCL unified shared
memory the GPU can read the exact allocation the CPU wrote. The transfer-hiding
techniques that dominate discrete-GPU programming are unnecessary here, and the
design should exploit that rather than imitate a discrete architecture.

**Bandwidth limits before arithmetic limits.** With roughly 135 GB/s shared
across both processors, most kernels in this project are bound by data movement
rather than floating-point throughput. Memory layout is therefore a first-order
design concern, and the roofline model (Phase 7) is the correct lens for judging
every kernel.

All bandwidth and throughput figures above are nominal. Phase 7 measures them
directly and the measured values are what the project quotes thereafter.

---

## 3. Architecture

Layers depend downwards only. No layer may include a header from a layer above
it. This is enforced by directory structure and reviewed at every PR.

```
apps/          Command-line simulator, interactive renderer
sim/           Simulation driver: owns solver, integrator, output, checkpoints
solvers/       Direct O(N^2), Barnes-Hut O(N log N)
integrators/   Velocity Verlet, Yoshida 4th order, RK4
backend/       CPU (work-stealing, AVX2) and SYCL (Arc iGPU, unified memory)
core/          Particle storage, vector maths, diagnostics, memory
```

### Load-bearing decisions

**Structure-of-arrays particle storage.** Positions, velocities, accelerations
and masses are stored as separate contiguous arrays rather than as an array of
particle structs. The force kernel reads only positions and masses; with an
array-of-structs layout every cache line fetched would also carry velocity and
acceleration data the kernel never touches, wasting a large fraction of the
memory bandwidth that is already the binding constraint. Separate arrays also
allow contiguous vector loads rather than strided gathers.

**Virtual dispatch at boundaries, never inside loops.** Solver and backend
selection are runtime-polymorphic, so a benchmark or test can swap
implementations from a command-line flag. The cost is one indirect call per
timestep, ahead of billions of floating-point operations, and is unmeasurable.
No virtual call appears in any loop over particles.

**One solver, several backends.** A GPU implementation is a backend behind the
existing solver interface, not a parallel copy of the solver. Two divergent
implementations of the same physics is the standard way projects of this kind
decay.

**Precision selected at build time.** `Real` is `double` by default and `float`
under a build option. Templating every solver on scalar type would multiply
compile times and complicate the SYCL kernels for no practical benefit, since a
given run is either accuracy-oriented or throughput-oriented, never both.

**The direct solver is the reference.** Every approximation, tree opening angle,
multipole order, reduced precision, is measured against direct summation in
double precision. It is not deleted once faster methods exist.

### Architecture decision records

Non-obvious decisions are recorded in `docs/adr/` as short numbered documents:
context, decision, consequences. An ADR is written when a choice has a credible
alternative that a reviewer might reasonably expect instead. ADRs are never
edited after merge; they are superseded by later ADRs.

---

## 4. Conventions

### Language and tone

- UK English throughout: code, comments, documentation, commit messages, PRs.
- Plain English. Prefer short sentences and concrete nouns.
- No em-dashes anywhere.
- No AI attribution of any kind. No generated-by notices, no co-author trailers,
  no tool names in commits, PR bodies, comments or documentation.

### Code style

- C++20. Clang is the reference compiler; GCC and MSVC must also build cleanly.
- `.clang-format` is authoritative. Formatting is never discussed in review.
- `.clang-tidy` is authoritative for lint. Warnings are errors in CI.
- Naming: types `PascalCase`, functions and variables `snake_case`, private
  members trailing underscore, constants `kPascalCase`, macros avoided entirely.
- Headers in `include/orrery/<layer>/`, implementation in `src/<layer>/`.
- Include what you use. Include order: own header, C++ standard, third party,
  project, each block alphabetised.
- RAII everywhere. No owning raw pointers. No manual `new` or `delete` outside
  the allocator in `core/`.
- `[[nodiscard]]` on every function whose return value is its only effect.
- Pass ranges as `std::span`, not pointer and length pairs, except inside kernels
  where the raw pointer is deliberate and commented.
- Exceptions are permitted at configuration and setup boundaries. No exception
  may propagate from a kernel or a per-particle loop.

### Comments

Comments explain why, not what. A comment restating the code is noise and will be
rejected in review. The comments that matter in this project explain physical
reasoning, numerical trade-offs, and performance decisions that would otherwise
look arbitrary. Every non-obvious constant carries its justification.

### Git workflow

- `main` is protected and always green. No direct pushes.
- One branch per phase: `phase-NN-short-name`, for example `phase-02-core-types`.
- Conventional commits: `feat:`, `fix:`, `perf:`, `test:`, `docs:`, `build:`,
  `ci:`, `refactor:`, `chore:`. Subject in the imperative, under 72 characters.
- Commits are logical units, not end-of-session dumps. A commit that mixes a
  feature with unrelated formatting will be split.
- One PR per phase. The PR body states what changed, why, how it was verified,
  and any measured numbers. It links the ADRs it introduces.
- Squash on merge. The PR title becomes the commit subject on `main`.

### Testing

Four categories, all required as the project grows:

1. **Unit.** Individual components in isolation. Catch2.
2. **Property.** Invariants over randomised inputs with fixed seeds. For example,
   total linear momentum is conserved to round-off for any valid configuration.
3. **Validation.** Comparison against known analytic results. Kepler two-body
   orbits, the virial ratio of a sampled Plummer sphere, convergence order of
   each integrator. This category is the point of the project.
4. **Regression.** Golden outputs with fixed seeds, so that an optimisation which
   silently changes the physics is caught rather than celebrated.

Tests are deterministic. Any randomness is seeded explicitly and the seed is
recorded in the failure message.

---

## 5. Definition of done

A phase is complete when all of the following hold. This list is checked at
review and no phase merges without it.

- Builds clean with Clang, GCC and MSVC, with warnings as errors.
- `clang-format` and `clang-tidy` pass with no diagnostics.
- Address and undefined-behaviour sanitiser builds pass the full test suite.
- All new code is covered by tests in at least one of the four categories.
- Public headers carry documentation comments explaining purpose and rationale.
- Any new non-obvious decision has an ADR.
- Performance-affecting changes report measured before and after figures, taken
  by the benchmark methodology in Phase 7 once that exists.
- The README reflects reality. No claim appears there that is not reproducible by
  running a documented command.
- No AI attribution anywhere in the diff.

---

## 6. Running a phase in a fresh session

Each phase is a separate chat. Start it with this, substituting the phase number:

> Read `docs/IMPLEMENTATION_PLAN.md` in full, then implement Phase N.
> Follow sections 4 and 5 exactly. Work on the branch named in the phase.
> Commit in logical units and open a pull request when the definition of done is
> met. Do not start any later phase.

Set reasoning effort to **medium** for every phase. The phases below are sized on
that assumption: each is one coherent concern, roughly eight to fifteen files,
with a clear finish line. High effort is unnecessary and will tend to expand
scope beyond the phase boundary.

Two rules that keep phases independent:

- A phase may refactor code from earlier phases where the plan requires it, but
  may not implement anything scheduled for a later phase.
- If a phase turns out to be materially larger than described, stop, split it,
  and record the split in this document rather than pushing through.

---

## 7. Phases

### Phase 0: Repository foundations

**Branch** `phase-00-foundations` **PR** `chore: repository foundations`

Establish the conventions before any code exists, so nothing has to be retrofitted.

Deliverables: licence (MIT), `.gitignore`, `.editorconfig`, `.clang-format`,
`.clang-tidy`, `CONTRIBUTING.md`, pull request and issue templates, the
`docs/adr/` directory with a template and ADR-0001 recording the decision to keep
ADRs, and a README skeleton stating what the project is and what it is not.

Done when: the repository is navigable, the conventions are written down, and a
newcomer could infer the intended standard from the files alone.

### Phase 1: Build system and continuous integration

**Branch** `phase-01-build-ci` **PR** `build: CMake project and CI pipeline`

Deliverables: target-based CMake with no global flag mutation, `CMakePresets.json`
covering debug, release, sanitiser and single-precision configurations,
dependency acquisition pinned to exact commits, Catch2 wired to CTest, and a
GitHub Actions workflow running a matrix over operating system, compiler and
build type, plus format and lint checks and the sanitiser build.

A placeholder library target and one trivial test exist purely so CI has
something real to exercise.

Done when: CI is green on a pull request, and a clone plus one preset command
produces a working build on a machine with no prior setup.

### Phase 2: Core data structures

**Branch** `phase-02-core` **PR** `feat: core particle storage and vector maths`

Deliverables: scalar and index type definitions with the precision switch, a
constexpr 3-vector for interface use, a cache-line-aligned allocator with its
rationale documented, and the structure-of-arrays particle container with span
based views and the size invariant enforced through a single mutation path.

Done when: unit tests cover the container invariants and the allocator's
alignment guarantee, and the header documentation explains the layout choice in
terms of bandwidth rather than taste.

### Phase 3: Diagnostics and initial conditions

**Branch** `phase-03-diagnostics-ic` **PR** `feat: conserved quantities and initial conditions`

Deliverables: kinetic energy, potential energy, linear momentum, angular momentum
and virial ratio; a deterministic random number source; a Plummer sphere sampler;
an exact Kepler two-body configuration; and a uniform sphere for scaling tests.

The potential energy calculation must use the same softening as the force kernel,
otherwise the conservation tests measure an artefact of the diagnostic rather
than the physics.

Done when: the sampled Plummer sphere has a virial ratio near unity within a
stated tolerance, and the Kepler configuration reproduces its analytic orbital
period.

### Phase 4: Time integrators

**Branch** `phase-04-integrators` **PR** `feat: symplectic and reference integrators`

Deliverables: the integrator interface, velocity Verlet, Yoshida fourth-order
symplectic, and classical RK4. An ADR recording why a symplectic second-order
scheme is preferred over a non-symplectic fourth-order one.

RK4 is included deliberately as the counterexample. The validation suite uses it
to show energy drifting without bound while the symplectic schemes stay within a
bounded envelope. That comparison is among the most informative results the
project produces.

Done when: measured convergence order matches the stated order for each scheme,
and the bounded-versus-secular energy behaviour is demonstrated by test.

### Phase 5: Direct force solver

**Branch** `phase-05-direct-solver` **PR** `feat: direct O(N^2) gravitational solver`

Deliverables: the force solver interface, a single-threaded correct direct
summation kernel, Plummer softening with its physical justification documented,
and the interaction counter used later for cross-algorithm comparison.

Correctness only. No threading and no explicit vectorisation in this phase.

Done when: two-body acceleration matches the analytic result to machine
precision, momentum conservation holds to round-off under property test, and a
long Kepler integration closes its orbit.

### Phase 6: CPU threading for heterogeneous cores

**Branch** `phase-06-threading` **PR** `perf: work-stealing scheduler for hybrid cores`

The first phase where the target hardware shapes the design directly.

Deliverables: a work-stealing task scheduler, or a carefully justified use of
dynamic scheduling, with per-thread instrumentation recording idle time and work
completed. A comparison of static against dynamic partitioning on the 4 plus 4
core topology, with the performance core idle time quantified in both cases.

Done when: the speedup over single-threaded execution is reported alongside the
idle-time measurement that explains it, and the result is written up in
`docs/performance/`.

### Phase 7: Vectorisation and benchmark methodology

**Branch** `phase-07-simd-roofline` **PR** `perf: AVX2 kernels and roofline analysis`

Deliverables: an explicitly vectorised AVX2 direct kernel with a portable scalar
fallback; a benchmark harness with proper statistical treatment covering warm-up,
repeated trials, median and dispersion rather than best-of, and recorded machine
state; measured peak memory bandwidth and floating-point throughput for this
specific machine; and a roofline plot placing each kernel against those limits.

This phase establishes the measurement methodology every later performance claim
depends on, including the handling of thermal throttling on a laptop part.

Done when: each kernel's achieved fraction of the relevant hardware limit is
stated with a figure, and the benchmark harness produces reproducible numbers
across repeated runs.

### Phase 8: Barnes-Hut tree solver

**Branch** `phase-08-barnes-hut` **PR** `feat: Barnes-Hut hierarchical solver`

The main algorithmic contribution. Likely the largest phase; split it if it grows
beyond one session.

Deliverables: Morton code particle ordering for spatial locality, parallel octree
construction, centre of mass and monopole moments, quadrupole moments as a
documented accuracy option, the opening angle criterion, and a tree walk that
respects the memory hierarchy.

Accuracy is characterised against the direct solver across a range of opening
angles, producing the error against cost curve that justifies the default.

Done when: complexity is empirically shown to be N log N over at least two
decades of N, accuracy against direct summation is quantified as a function of
opening angle, and the crossover point where the tree overtakes direct summation
on this hardware is measured.

### Phase 9: SYCL backend, direct kernel

**Branch** `phase-09-sycl-direct` **PR** `feat: SYCL backend for Intel Arc`

Deliverables: oneAPI toolchain integration behind an optional build flag, device
discovery with graceful fallback when no device is present, unified shared memory
allocation exploiting the zero-copy property of this architecture, the direct
kernel in SYCL, and bit-comparable validation against the CPU backend within a
stated tolerance.

An ADR recording the choice of SYCL over CUDA, covering both the hardware reality
and the portability argument.

Done when: the GPU direct kernel matches CPU results within tolerance, the
speedup is reported against the roofline for the integrated GPU, and the absence
of any host-to-device copy is demonstrated rather than asserted.

### Phase 10: SYCL backend, tree traversal

**Branch** `phase-10-sycl-tree` **PR** `perf: GPU tree traversal`

The hardest phase technically. Irregular pointer-chasing on a wide SIMD device is
where naive GPU ports fail.

Deliverables: a GPU-suitable tree representation, warp-coherent or group-coherent
traversal to limit divergence, and the scaling study that establishes the largest
tractable particle count on this machine.

Done when: the headline figure is established and reproducible, with the
divergence mitigation measured rather than assumed.

### Phase 11: Simulation driver, configuration and I/O

**Branch** `phase-11-driver-io` **PR** `feat: simulation driver and file formats`

Deliverables: the simulation class owning solver, integrator and output; a
declarative configuration file format; checkpoint and restart; a compact binary
trajectory format with a documented specification; a CSV diagnostics stream; and
the command-line application.

Done when: a long run can be interrupted and resumed to bitwise-identical state,
and every earlier capability is reachable from the command line.

### Phase 12: Visualisation

**Branch** `phase-12-visualisation` **PR** `feat: real-time renderer and video export`

Deliverables: a real-time OpenGL point renderer with additive blending and
tone mapping suitable for the visual character of a star field, an interactive
camera, offline frame export, and a documented path to an encoded video.

The galaxy collision scenario is defined here and becomes the project's headline
demonstration.

Done when: the collision renders in real time at a stated particle count and the
exported video is reproducible from a single documented command.

### Phase 13: Python bindings

**Branch** `phase-13-python` **PR** `feat: Python bindings`

Deliverables: pybind11 bindings exposing the simulation and diagnostics, NumPy
views over particle arrays without copying, a packaging configuration, and
example notebooks reproducing the validation results.

Done when: the notebooks run from a clean environment and reproduce figures that
match the C++ test suite.

### Phase 14: Validation report, documentation and release

**Branch** `phase-14-release` **PR** `docs: validation report and v1.0 release`

Deliverables: the full validation report gathering every analytic comparison,
convergence study and conservation result; the performance report gathering the
roofline analyses and scaling studies; a generated API reference; a documentation
site; and the README rewritten to lead with the video, then the validation
evidence, then the performance figures.

Done when: every claim in the README is traceable to a command in the repository
that reproduces it.

---

## 8. Risks

**Phase 10 is the most likely to overrun.** Irregular traversal on a wide SIMD
device is genuinely hard. If it stalls, the project is still complete and
defensible with Phase 9, since a GPU direct kernel plus a CPU tree solver already
covers both the parallelism and the algorithms. Treat Phase 10 as the stretch
goal it is.

**Thermal throttling will corrupt benchmark results** on a laptop part unless
handled explicitly. Phase 7 must address it directly, not work around it
quietly.

**Scope creep towards a general framework.** Orrery is a gravitational N-body
simulator. The architecture is layered so that other physics could be added, and
that is where the matter rests until there is a genuine second solver. A
framework with one solver in it is a solver with extra indirection, and reviewers
notice.

**Precision loss under fast floating-point flags.** Any relaxation of IEEE
semantics for vectorisation is a measured decision validated against a
compensated-summation reference, not an assumption. This is checked in Phase 7
and re-checked whenever kernels change.

---

## 9. Status

| Phase | Title | State |
| --- | --- | --- |
| 0 | Repository foundations | Complete |
| 1 | Build system and CI | Complete |
| 2 | Core data structures | Not started |
| 3 | Diagnostics and initial conditions | Not started |
| 4 | Time integrators | Not started |
| 5 | Direct force solver | Not started |
| 6 | CPU threading for heterogeneous cores | Not started |
| 7 | Vectorisation and benchmark methodology | Not started |
| 8 | Barnes-Hut tree solver | Not started |
| 9 | SYCL backend, direct kernel | Not started |
| 10 | SYCL backend, tree traversal | Not started |
| 11 | Simulation driver, configuration and I/O | Not started |
| 12 | Visualisation | Not started |
| 13 | Python bindings | Not started |
| 14 | Validation report, documentation and release | Not started |

Update this table in the pull request that completes each phase.
