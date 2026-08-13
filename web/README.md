# The browser client

The instrument that reads a run, and the reading half beside it. ADR-0045
records why it is in this repository rather than in one of its own.

```
npm ci
npm run dev
```

| Command | What it does |
| --- | --- |
| `npm run dev` | The development server |
| `npm run build` | Type check, then build into `dist/` |
| `npm test` | Unit tests, mirroring `src/` under `tests/` |
| `npm run e2e` | Browser tests against the built client, including an accessibility audit |
| `npm run lint` | Biome, linting and formatting in one pass |
| `npm run budget` | Assert the download sizes against `tools/budget.ts` |
| `npm run gallery` | Run the simulator to produce the published trajectories |
| `npm run measure` | A minute of playback, for the frame rate and the heap |
| `npm run fonts` | Cut the three faces to the characters the design sets |

The gallery is not committed. `npm run gallery -- <path to orrery>` runs the
three configurations in `src/gallery/runs.ts` and writes their trajectories and
diagnostics into `public/gallery/`, which is where the client fetches them from;
the site workflow does the same before it builds. Without it the instrument
draws nothing and says so, which is a useful state to have seen.

The WebAssembly module is not committed either, for the same reason: it is
compiler output. `cmake --preset wasm && cmake --build --preset wasm`, from the
repository root with emsdk on the environment, writes it into `public/solver/`.
Without it the console's browser-run control says the module could not be
fetched, and the one unit test that compares it against the native build skips
itself and says so. `docs/webassembly.md` describes what is in it.

A run and a moment in it are both in the address: `?run=cluster&t=12.5`. So is
the backend, as `?renderer=webgl2`, which is what makes the two comparable.

## Layout

```
method/           The reading half: four arguments and a contents, as HTML
src/components/   One component and one stylesheet each
src/config/       A reader for the repository's configuration format
src/data/         Measurements transcribed from the reports, with their sources
src/diagnostics/  The diagnostics reader, its plots, and what is derived here
src/format/       How a number is set
src/gallery/      Which runs are published, and what each one changes
src/method/       Where the reading half is, for the links the instrument makes
src/render/       The renderer interface, its two backends, the camera and loop
src/solver/       The WebAssembly module, its Worker, and what a browser run is
src/state/        The store, and the two kinds of state it keeps apart
src/styles/       tokens.css, the fonts, and what every element starts from
src/trajectory/   The format, the ranged reader, and the Worker that runs them
public/fonts/     The subset faces, committed so a build needs no network
public/gallery/   The published runs, generated rather than committed
public/solver/    The WebAssembly module, generated rather than committed
tools/            The font cutter, the budget, the gallery and the measurements
tests/fixtures/   Small files the C++ wrote, for the reader and for the module
e2e/              What only a browser can answer
```

## Rules the code keeps

**One definition of every colour and every size.** `src/styles/tokens.css` holds
the palette, the type scale, the spacing scale, the hairline and the focus ring.
A component's stylesheet reads tokens and defines none of its own. A colour that
no token provides is a gap in the system, and it is filled there rather than
worked around here.

**Nothing is transcribed that can be read.** The configuration register is
parsed from the run's own `.orrery` file, the plots and the drift figures come
from the diagnostics CSV that run wrote, and the version from the `project()`
call in `CMakeLists.txt`. What genuinely cannot be parsed, the step time and the
wall clock, lives in `src/data/` with the document it came from named beside it
and the conditions it was measured under carried with it. ADR-0048.

**What the run measured and what the client derives are kept apart.** The four
plots in the rail are columns of a file the simulator wrote. The radial profile
under them is worked out here from the frame on the plate, and it is labelled as
derived, because a plot in a rail of measurements reads as a measurement.

**A control that cannot act says what it would need.** It is drawn back rather
than removed and keeps its place in the tab order, with the reason at the foot
of its tier. Read together those notes say exactly what a trajectory carries.

**Every number goes through one component.** `Numeric` sets real minus signs,
thin spaces between digit groups, raised exponents, units a weight lighter and
tabular figures throughout, and offers a spoken form of any value a screen
reader would otherwise read wrongly.

**The device sits behind the interface, not in front of it.** `src/render/`
defines one renderer and implements it twice, for WebGPU and for WebGL2, and
nothing outside that directory knows which one started. ADR-0046 records why,
and what the two do differently. The same shape as ADR-0026 on the other side of
the repository.

**Nothing decodes on the thread that draws.** The trajectory reader runs in a
Worker and frames cross to the page by transfer rather than by copy. The render
loop reads them, and allocates nothing worth measuring doing it:
`docs/instrument.md` has the numbers and `tools/measure_render.ts` produces
them. ADR-0047.

**Two kinds of state, kept apart.** What a person changes lives in a typed store
with subscriptions and may be rendered by React. What a run changes, sixty times
a second, lives in a mutable record that notifies nobody. `src/state/store.ts`
says why.

**The reading half is a document and costs nothing to read.** The pages under
`method/` are static HTML with a stylesheet and no script, built as their own
entries of the same site. They share `tokens.css`, the three faces and the
masthead with the instrument, at the opposite value. Every figure on one names
the file it came from, and `tests/method/figures.test.ts` opens that file and
requires the figure to be in it. ADR-0050.

**The budgets and the greys are asserted rather than believed.**
`tools/budget.ts` fails the build when the download grows past its limit, and
`tests/styles/contrast.test.ts` reads `tokens.css` and checks every grey against
the ratio its use requires.

## The fonts

The three faces are self-hosted and cut to the 110 characters the design sets,
which is the difference between a preload of 47 kB and one of half a megabyte.
`tools/subset_fonts.py` does the cutting: it downloads the upstream releases,
checks them against pinned hashes, subsets and converts to WOFF2. The output is
committed, so a build needs neither the script nor the network. Run it when the
character set or a font version changes:

```
pip install "fonttools[woff]"
npm run fonts
```
