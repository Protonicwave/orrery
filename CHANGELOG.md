# Changelog

What each released version of Orrery can do that the one before it could not.
Newest first. Every figure quoted here is one the repository reproduces, and the
document that carries the working is named beside it.

## 1.1.0

At 1.0.0 Orrery was a simulator: two solvers, three integrators, a CPU backend
and a SYCL one, a real-time viewer and a Python interface, all driven from a
command line by a configuration file. Everything it could demonstrate had to be
built first.

It can now be read without being built. The same physics is published as an
instrument in a browser, compiled to WebAssembly so that a small run can be
computed there, wrapped in an editor that writes a configuration file back out,
and put behind a service that runs a full-size one on hardware that is not the
reader's. Nothing in that path reimplements anything: the browser plays the
binary trajectory format the native run writes, the module in the page is the
library compiled by a second toolchain, and the service runs the same binary
this repository builds.

**The instrument.** A client at
[protonicwave.github.io/orrery/instrument](https://protonicwave.github.io/orrery/instrument/)
that plays three published runs, the Kepler two-body problem, a Plummer sphere
and the galaxy collision, each one of the repository's own configurations. It
draws with the optics the native renderer uses, additive sprites into a
half-float target with a tone-mapping pass, on WebGPU where a browser has it and
WebGL2 where it does not, both behind one renderer interface. Trajectories are
fetched in ranges and decoded in a Worker, so playback starts before the file
has landed and the main thread never decodes. A run and a moment inside it are
in the address, so either can be linked to. Beside it is the reading half, the
same argument as the two reports at the length a page should be, served as
static HTML that runs no script, with every figure on it traced back to the file
in `docs/` it came from by a test. [`docs/instrument.md`](docs/instrument.md).

**The solver in a browser.** A `wasm` preset builds `core`,
`initial_conditions`, `integrators` and the CPU solvers through the Emscripten
toolchain and nothing else: no command-line programs, no renderer, no file I/O.
About a dozen C functions in [`wasm/orrery_wasm.h`](wasm/orrery_wasm.h) are the
whole boundary. The module is 103 kB gzipped with a 4 kB loader, against a
budget of 400 kB the build asserts, and it runs in a Worker with one thread and
a scalar kernel, which the plate states for as long as it is stepping. A test
runs a configuration in the module and compares it against a trajectory the
native build wrote from the same file, so the two are known to agree rather than
assumed to. [`docs/webassembly.md`](docs/webassembly.md).

**The editor.** A page that designs a run as a technical drawing rather than a
scene: construction lines for the predicted orbit, dimension lines with
measurements, velocity arrows with magnitudes, and an orbital-element readout
worked out from the settings as they move. Four scenarios arranged as a path,
the two-body problem into a Plummer sphere into a single disc into the
collision, each editable into the next. What leaves it is an `.orrery` file with
the comment header the examples carry, and `orrery run` is all that file needs.
[`docs/editor.md`](docs/editor.md).

**The compute service.** A FastAPI application and a worker that take a
configuration file, queue it, run the native binary over it and give back a
trajectory the instrument plays through the same reader it plays the gallery
with. The queue is in PostgreSQL, claimed with
`SELECT ... FOR UPDATE SKIP LOCKED`, and a job is identified by the hash of the
configuration it runs, so the same document submitted twice returns the first
result rather than running again. Progress arrives over a WebSocket and falls back to polling. Trajectories
go to object storage before the job is marked done, so a job that says it has
finished is one whose result is there. Submissions are bound at both ends, one
run at a time and the sequence of them together: 20,000 particles, 20,000 steps,
the work of the published demonstration, a queue of 32, six runs an hour from
one address and twenty-four full-size runs a day. A submission outside any of
them is refused with every objection at once, each naming the setting in the
spelling the file used. When no service is configured or none answers, the
client says so and offers the gallery instead of an error page.
[`docs/service.md`](docs/service.md) and [`deploy/`](deploy/), which holds the
two images, a compose file for local use and a documented database restore.

**A second GPU vendor.** A CUDA backend written against the existing solver
interface, mirroring the SYCL one decision for decision: staged allocations, a
host-built tree walked on the device, masked cell acceptance, a narrowed node
array. It is off by default and continuous integration compiles it with the
toolkit and no device attached. It exists to test that the interface holds for a
second vendor rather than because the project needs two GPUs, which is what
ADR-0060 records. [`docs/performance/cuda.md`](docs/performance/cuda.md) carries
no timings: no machine this project is developed on has an NVIDIA device, and a
figure that was not measured is not written down.

**Packaging.** A release workflow builds wheels for Linux, macOS and Windows on
a tag and publishes them to PyPI through trusted publishing, with no token
anywhere in the repository. The wheels are built on runners with no oneAPI
toolchain and no GPU, which is where the claim that both GPU backends are
optional stops being an assertion.

Nothing in the C++ or Python interfaces changed shape, so a program written
against 1.0.0 compiles and runs unchanged.

## 1.0.0

The simulator: a direct O(N^2) solver and a Barnes-Hut O(N log N) solver,
velocity Verlet, Yoshida 4 and RK4, a threaded and vectorised CPU backend, a
SYCL backend for an integrated GPU, a real-time renderer, the configuration,
trajectory and checkpoint formats, and Python bindings whose arrays share memory
with the running solver. The version was set in `CMakeLists.txt` and the work
was merged, but no tag was cut and nothing was published from it, so 1.1.0 is
the first release an installer can reach.
