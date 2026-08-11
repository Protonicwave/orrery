# The configuration format

A run of Orrery is decided entirely by a configuration file and the revision of
this repository that reads it. This document specifies the file. ADR-0031
records why the format is defined here rather than borrowed.

## Syntax

A file is a sequence of lines. Each is one of:

- **Blank**, or whitespace only. Ignored.
- **A comment**, whose first non-blank character is `#`. Ignored to the end of
  the line. A `#` anywhere else is an ordinary character, so a path may contain
  one.
- **A section heading**, `[name]`. It sets the section that following settings
  belong to.
- **A setting**, `key = value`. Whitespace around the key and the value is
  removed; whitespace inside the value is kept.

A key may name its own section, as `solver.softening = 0.05`. That form works
anywhere, including before any heading and inside a different section, and it
does not change the current section. It is what `--set` on the command line
uses.

Everything is case sensitive. Values run to the end of the line and are not
quoted, so there is no escaping rule and none is needed.

## Strictness

All of these are errors, reported with the file name and the line number:

- An unknown section or an unknown setting.
- A value that does not parse as the setting's type.
- A number with anything after it, as `0.5 and a bit`.
- A setting with no value.
- The same setting given twice, in either spelling.
- A setting before any section heading that does not name its own section.

Nothing is skipped and nothing is guessed. A run whose `softenning` was silently
dropped is not a run that failed; it is a run that produced a plausible answer to
a question nobody asked.

## Types

| Type | Accepted |
| --- | --- |
| number | Decimal, with an optional sign and an optional exponent: `0.001`, `-2`, `1.5e-3`. Read in the classic locale whatever the machine is configured for |
| whole number | Digits, not negative: `4096` |
| boolean | `true` or `false`, and nothing else |
| text | The rest of the line |

## Settings

Every setting may be left out, and what is left out keeps the default below. A
file states what it changes.

### `[run]`

| Setting | Type | Default | Meaning |
| --- | --- | --- | --- |
| `timestep` | number | none | The interval of simulated time one step covers. Must be positive. There is no default worth having: the right timestep is a property of the configuration, roughly the shortest orbital period in it divided by a few dozen |
| `steps` | whole number | 0 | How many steps to take. Must be at least one |
| `seed` | whole number | 0 | The seed the initial conditions are sampled from |

A run is a number of steps rather than an interval of time. A finishing time
would have to round somewhere, and the rounding would decide whether a resumed
run took the same number of steps as an uninterrupted one.

### `[initial_conditions]`

| Setting | Type | Default | Meaning |
| --- | --- | --- | --- |
| `kind` | text | `plummer` | One of `plummer`, `uniform-sphere`, `kepler` |
| `count` | whole number | 0 | Particles, for the two sampled models. At least two. Not used by `kepler`, which is two bodies by definition |
| `total_mass` | number | 1 | Shared equally among the particles of a sampled model |
| `scale_radius` | number | 0 | The Plummer scale radius. Zero means `3 pi / 16`, the value that puts a unit-mass sphere into standard N-body units |
| `radius` | number | 1 | The radius of the uniform sphere |
| `mass_fraction_cutoff` | number | 0.999 | The fraction of a Plummer model's mass the sample is drawn from. The model is infinite in extent, and truncating at 0.999 bounds the sample at 38.7 scale radii |
| `primary_mass` | number | 1 | The Kepler configuration's first body |
| `secondary_mass` | number | 1 | Its second |
| `semi_major_axis` | number | 1 | Of the relative orbit |
| `eccentricity` | number | 0 | In `[0, 1)`. One or more is an unbound encounter with no period |

A setting the chosen `kind` does not use is ignored, so one file may describe
several configurations and select between them.

### `[solver]`

| Setting | Type | Default | Meaning |
| --- | --- | --- | --- |
| `kind` | text | `barnes-hut` | One of `direct`, `barnes-hut`, `sycl-direct`, `sycl-tree` |
| `softening` | number | 0 | The Plummer softening length. Zero is exact point masses, which is what the analytic comparisons need |
| `opening_angle` | number | 0.5 | Barnes-Hut only, in `[0, 1]`. Above one a cell can be accepted while the particle being accelerated is inside it |
| `leaf_capacity` | whole number | 32 | The most particles a tree cell may hold and remain a leaf |
| `quadrupole` | boolean | `false` | Whether to carry the second moment of each cell. An accuracy option rather than an improvement (ADR-0024) |
| `executor` | text | `work-stealing` | One of `serial`, `static`, `work-stealing`. A run should use the default; the others exist so a measurement can reproduce a published figure |
| `threads` | whole number | 0 | Workers. Zero means one per core |
| `allow_cpu_fallback` | boolean | `true` | Whether a run may use the CPU when the GPU it asked for is absent |

The two `sycl-` values are accepted by the parser in every build, including one
compiled without the GPU backend. A configuration file is a document and should
mean the same thing whatever binary reads it; whether this machine can provide
what it asks for is decided when the run is assembled.

### `[integrator]`

| Setting | Type | Default | Meaning |
| --- | --- | --- | --- |
| `kind` | text | `velocity-verlet` | One of `velocity-verlet`, `yoshida4`, `rk4` |

Velocity Verlet is the default on ADR-0011's argument. RK4 is here as the
counterexample: of the same order as Yoshida and costing a third more per step,
its energy error grows without bound where the symplectic pair stay inside an
envelope.

### `[output]`

| Setting | Type | Default | Meaning |
| --- | --- | --- | --- |
| `trajectory_path` | text | none | Where the binary trajectory goes. Empty writes none |
| `trajectory_stride` | whole number | 0 | Steps between frames |
| `trajectory_velocities` | boolean | `false` | Whether frames carry velocities as well as positions |
| `diagnostics_path` | text | none | Where the CSV diagnostics go |
| `diagnostics_stride` | whole number | 0 | Steps between measurements. These cost an N^2 pass, so this should be a few hundred rather than one |
| `checkpoint_path` | text | none | Where checkpoints go. One file, overwritten |
| `checkpoint_stride` | whole number | 0 | Steps between checkpoints |

A stride of zero writes at the two ends of the run and nowhere in between. The
last step of a run is always written whatever the stride, so the state a run
finished in is never absent from its own output.

## An example

```
# A Plummer sphere of four thousand particles, integrated for a thousand steps
# with the tree solver. Softening is a twentieth of the scale radius, which is
# the smallest length this configuration is meant to resolve.

[run]
timestep = 0.001
steps    = 1000
seed     = 20260811

[initial_conditions]
kind  = plummer
count = 4096

[solver]
kind      = barnes-hut
softening = 0.05

[integrator]
kind = velocity-verlet

[output]
diagnostics_path   = cluster.csv
diagnostics_stride = 50
trajectory_path    = cluster.otj
trajectory_stride  = 100
checkpoint_path    = cluster.ock
checkpoint_stride  = 500
```

Run it, and see what the settings resolve to without taking a step:

```
orrery run cluster.orrery
orrery show cluster.orrery
```

Override anything from the command line, using the same names:

```
orrery run cluster.orrery --set solver.kind=direct --set run.steps=100
```
