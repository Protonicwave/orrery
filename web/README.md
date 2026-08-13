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
| `npm run fonts` | Cut the three faces to the characters the design sets |

## Layout

```
src/components/   One component and one stylesheet each
src/config/       A reader for the repository's configuration format
src/data/         Measurements transcribed from the reports, with their sources
src/format/       How a number is set
src/state/        The store, and the two kinds of state it keeps apart
src/styles/       tokens.css, the fonts, and what every element starts from
public/fonts/     The subset faces, committed so a build needs no network
tools/            The font cutter, the budget, and the version reader
e2e/              What only a browser can answer
```

## Rules the code keeps

**One definition of every colour and every size.** `src/styles/tokens.css` holds
the palette, the type scale, the spacing scale, the hairline and the focus ring.
A component's stylesheet reads tokens and defines none of its own. A colour that
no token provides is a gap in the system, and it is filled there rather than
worked around here.

**Nothing is transcribed that can be read.** The configuration register is
parsed from `examples/collision.orrery` and the version from the `project()`
call in `CMakeLists.txt`. What genuinely cannot be parsed, the measured figures,
lives in `src/data/` with the document it came from named beside it and the
conditions it was measured under carried with it.

**Every number goes through one component.** `Numeric` sets real minus signs,
thin spaces between digit groups, raised exponents, units a weight lighter and
tabular figures throughout, and offers a spoken form of any value a screen
reader would otherwise read wrongly.

**Two kinds of state, kept apart.** What a person changes lives in a typed store
with subscriptions and may be rendered by React. What a run changes, sixty times
a second, lives in a mutable record that notifies nobody. `src/state/store.ts`
says why.

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
