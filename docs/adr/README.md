# Architecture decision records

Short numbered documents recording the decisions behind Orrery's design, each
with its context, the alternatives that were rejected, and what follows from the
choice.

An ADR is written when a decision has a credible alternative that a reviewer
might reasonably have expected instead. It is never edited after it is merged;
a decision that changes is recorded in a new ADR that supersedes the old one.

To add one, copy [`0000-template.md`](0000-template.md), take the next free
number, and link it from the table below and from the pull request that
introduces it.

| Number | Title | Status |
| --- | --- | --- |
| [0001](0001-record-architecture-decisions.md) | Record architecture decisions | Accepted |
| [0002](0002-pin-dependencies-to-exact-commits.md) | Pin dependencies to exact commits fetched at configure time | Accepted |
| [0003](0003-build-settings-through-interface-targets.md) | Carry build settings on interface targets rather than global flags | Accepted |
| [0004](0004-store-particles-as-component-arrays.md) | Store particles as one array per component | Accepted |
| [0005](0005-allocate-particle-arrays-on-cache-lines.md) | Allocate particle arrays on cache-line boundaries | Accepted |
| [0006](0006-select-precision-at-build-time.md) | Select the scalar precision when the project is configured | Accepted |
| [0007](0007-work-in-units-where-g-is-one.md) | Work in units where the gravitational constant is one | Accepted |
| [0008](0008-share-one-softening-definition.md) | Share one softening definition between the solver and the diagnostics | Accepted |
| [0009](0009-generate-the-random-distributions-here.md) | Generate the random distributions in the project rather than with the standard library | Accepted |
| [0010](0010-give-initial-conditions-their-own-layer.md) | Give initial conditions their own layer | Accepted |
| [0011](0011-prefer-symplectic-integration.md) | Prefer a symplectic second-order integrator to a non-symplectic fourth-order one | Accepted |
| [0012](0012-integrators-call-an-abstract-acceleration-field.md) | Give the integrators an abstract acceleration field of their own | Accepted |
| [0013](0013-carry-the-acceleration-between-steps.md) | Carry the acceleration between steps as an interface invariant | Accepted |
| [0014](0014-give-the-solvers-an-interface-of-their-own.md) | Give the force solvers an interface of their own above the acceleration field | Accepted |
| [0015](0015-compute-every-pair-from-both-ends.md) | Compute every pair from both ends rather than applying Newton's third law | Accepted |
| [0016](0016-balance-the-threads-dynamically.md) | Balance the threads dynamically by stealing ranges | Accepted |
| [0017](0017-thread-the-solver-through-an-executor.md) | Thread the solver through an executor it does not own | Accepted |
| [0018](0018-dispatch-to-the-vector-kernel-at-run-time.md) | Compile the vector kernel apart and choose it at run time | Accepted |
| [0019](0019-report-the-median-with-its-dispersion.md) | Report the median with its dispersion, and measure the throttling | Accepted |
| [0020](0020-keep-the-arithmetic-ieee.md) | Keep the arithmetic IEEE and measure what vectorising changed | Accepted |
| [0021](0021-sort-a-copy-rather-than-the-caller-s-particles.md) | Sort a copy of the configuration rather than the caller's particles | Accepted |
| [0022](0022-rebuild-the-tree-every-evaluation.md) | Rebuild the tree on every force evaluation | Accepted |
| [0023](0023-open-cells-on-a-corrected-distance.md) | Open a cell on the distance to its centre of mass, corrected for where that is | Accepted |
| [0024](0024-make-quadrupole-moments-an-option.md) | Make quadrupole moments an option that is off by default | Accepted |
| [0025](0025-write-the-gpu-backend-in-sycl.md) | Write the GPU backend in SYCL | Accepted |
| [0026](0026-put-the-gpu-behind-the-solver-interface.md) | Put the GPU behind the solver interface, not behind the executor | Accepted |
| [0027](0027-stage-particle-data-into-shared-allocations.md) | Stage particle data into the solver's own shared allocations | Accepted |
| [0028](0028-build-the-tree-on-the-host-and-walk-it-on-the-device.md) | Build the tree on the host and walk it on the device | Accepted |
| [0029](0029-mask-the-accepted-cells-rather-than-descending-together.md) | Mask the accepted cells rather than descending together | Accepted |
| [0030](0030-send-the-node-array-to-the-device-narrowed-rather-than-transposed.md) | Send the node array to the device narrowed rather than transposed | Accepted |
| [0031](0031-define-a-configuration-format-rather-than-adopting-one.md) | Define a configuration format rather than adopting one | Accepted |
| [0032](0032-store-the-whole-state-in-a-checkpoint.md) | Store the whole state in a checkpoint, accelerations included | Accepted |
| [0033](0033-keep-the-trajectory-and-the-checkpoint-apart.md) | Keep the trajectory and the checkpoint apart | Accepted |
| [0034](0034-draw-with-opengl-through-glfw.md) | Draw with OpenGL 3.3 through GLFW | Accepted |
| [0035](0035-resolve-the-opengl-entry-points-by-hand.md) | Resolve the OpenGL entry points by hand | Accepted |
| [0036](0036-tone-map-exported-frames-on-the-host.md) | Tone map exported frames on the host | Accepted |
| [0037](0037-write-frames-as-ppm-and-encode-elsewhere.md) | Write frames as PPM and leave the encoding to an external tool | Accepted |
| [0038](0038-sample-the-galaxy-as-a-scenario.md) | Sample the galaxy as a scenario rather than as an equilibrium model | Accepted |
| [0039](0039-bind-to-python-with-pybind11.md) | Bind to Python with pybind11 | Accepted |
| [0040](0040-expose-the-component-arrays-rather-than-triples.md) | Expose the component arrays rather than arrays of triples | Accepted |
| [0041](0041-give-a-running-simulation-a-read-only-state-type.md) | Give a running simulation's state a read-only type of its own | Accepted |
| [0042](0042-package-the-whole-project-from-the-repository-root.md) | Package the whole project from the repository root | Accepted |
| [0043](0043-generate-the-site-with-the-tool-that-generates-the-reference.md) | Generate the site with the tool that generates the reference | Accepted |
| [0044](0044-write-the-validation-report-rather-than-generating-it.md) | Write the validation report rather than generating it | Accepted |
| [0045](0045-keep-the-browser-client-in-this-repository.md) | Keep the browser client in this repository | Accepted |
| [0046](0046-put-the-renderer-s-device-behind-one-interface.md) | Put the renderer's device behind one interface | Accepted |
| [0047](0047-decode-trajectories-off-the-main-thread.md) | Decode trajectories off the main thread | Accepted |
| [0048](0048-read-the-diagnostics-the-run-wrote.md) | Read the diagnostics the run wrote | Accepted |
| [0049](0049-put-the-run-and-the-moment-in-the-address.md) | Put the run and the moment in the address | Accepted |
| [0050](0050-serve-the-reading-half-as-static-pages.md) | Serve the reading half as static pages | Accepted |
| [0051](0051-compile-the-solver-to-webassembly.md) | Compile the solver to WebAssembly rather than write a second one | Accepted |
| [0052](0052-build-webassembly-single-threaded.md) | Build the WebAssembly module single threaded, in a Worker | Accepted |
| [0053](0053-draw-the-initial-conditions-as-a-technical-drawing.md) | Draw the initial conditions as a technical drawing | Accepted |
| [0054](0054-degrade-to-the-gallery-when-compute-is-unavailable.md) | Degrade to the gallery when compute is unavailable | Accepted |
| [0055](0055-run-the-full-solver-server-side.md) | Run the full solver server side, over a document rather than a form | Accepted |
| [0056](0056-keep-the-job-queue-in-the-database.md) | Keep the job queue in the database that already holds the jobs | Accepted |
| [0057](0057-store-trajectories-outside-the-database.md) | Store trajectories outside the database, and serve them through the API | Accepted |
| [0058](0058-bound-the-service-with-the-jobs-it-already-stores.md) | Bound the service with the jobs it already stores | Accepted |
| [0059](0059-expire-submitted-results-after-a-week.md) | Expire submitted results after a week | Accepted |
| [0060](0060-write-the-cuda-backend-against-the-same-solver-interface.md) | Write the CUDA backend to test the solver interface, not because the project needs two GPUs | Accepted |
