# The checkpoint format

A run, stopped, in a form it can be picked up from exactly where it was put
down. Written by `sim/checkpoint.hpp`. ADR-0032 records what is in it and why,
and ADR-0033 why it is not the trajectory format.

## Conventions

The same as the trajectory format: unsigned little-endian integers whatever the
machine, IEEE 754 floating-point stored as bit patterns, `R` for the size of a
scalar, and 64-bit FNV-1a for the checksum.

## Layout

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | Magic, the ASCII bytes `ORRERYCK` |
| 8 | 4 | Format version. Currently 1 |
| 12 | 4 | Flags. Bit 0 means single precision; every other bit is reserved and must be zero |
| 16 | 8 | The step the run had reached |
| 24 | 8 | `N`, the number of particles |
| 32 | 4 | `L`, the length in bytes of the configuration text |
| 36 | L | The configuration, in the format of `docs/formats/configuration.md` |
| 36 + L | 10 × N × R | The state, as ten arrays |
| 36 + L + 10NR | 8 | Checksum of every byte from offset 0 to here |

The ten arrays are in this order, each `N` scalars long: masses, then `x`, `y`,
`z` positions, then `x`, `y`, `z` velocities, then `x`, `y`, `z` accelerations.

The simulated time is not stored. It is the step number times the timestep, and
the timestep is in the configuration; two fields that must agree are how they
come to disagree.

## Why the accelerations are in it

They could be recomputed from the positions, and for every solver in this project
that would give the same bits. Storing them makes a resumed state a copy of the
stored state rather than a reconstruction that happens to agree, which is a
statement about this file rather than about whichever solvers exist when it is
read. It also saves a force evaluation on every resume, which at two million
particles is most of a second. ADR-0032 sets out the argument.

## Why the configuration is in it

So that a checkpoint is sufficient on its own. `orrery resume state.ock` needs
the file and nothing else: it cannot be resumed under settings that differ from
the ones it was taken under, because there is nowhere else for the settings to
come from. The text is about a kilobyte against a state of eighty bytes a
particle.

## Why it is written atomically

A checkpoint exists to survive a run being killed, and the moment a run is most
likely to be killed is not chosen to avoid the moment its checkpoint is half
written. So the bytes go to `<path>.partial` and that file is renamed over the
target, which either happens or does not. Without the rename, a signal arriving
during the write would destroy the previous checkpoint as well as the current
one, and the feature would be a liability.

The temporary is a neighbour of the target rather than in a system temporary
directory, because a rename is only atomic within one filesystem and the
temporary directory is frequently on another.

## What a reader refuses

A checkpoint that fails any of these is refused rather than resumed from under a
warning. Continuing from a state that might be damaged would produce results
indistinguishable from correct ones, which is the worst failure this project can
have.

- The magic is not `ORRERYCK`, so the file is something else.
- The version is not one this build knows.
- A reserved flag bit is set.
- The precision does not match this build's.
- The file ends before the state is complete.
- The checksum does not match, so a byte changed or the write was interrupted.
- The configuration text does not parse.

## Using one

```
orrery run cluster.orrery              # writes cluster.ock at its stride
orrery inspect cluster.ock             # what step it is at, under what settings
orrery resume cluster.ock              # continue to the step count it was given
orrery resume cluster.ock --set run.steps=50000   # or further
```

Resuming rewrites the trajectory and diagnostics files from the resumed step
rather than appending to them, since a partial append cannot be made safe. A run
that will be resumed and wants to keep both segments should give the second one
different output paths.

## The guarantee

An interrupted run resumed from a checkpoint arrives at a state identical in
every bit of every position, velocity, acceleration and mass to one that was
never interrupted. This is asserted by `tests/sim/simulation_test.cpp` for all
three integrators and both CPU solvers, through a real file, and compared for
equality rather than against a tolerance.
