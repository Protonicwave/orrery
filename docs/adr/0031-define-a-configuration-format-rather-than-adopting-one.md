# ADR-0031: Define a configuration format rather than adopting one

- **Status:** Accepted
- **Date:** 2026-08-11

## Context

Phase 11 asks for a declarative configuration file. A run is decided by about
twenty scalars, a handful of enumerated choices and three output paths, and the
file has to carry those in a form a person writes by hand and a machine reads
without ambiguity.

The obvious move is to take a format that already exists. TOML, YAML and JSON
are the candidates, and each has good C++ libraries available.

## Decision

Orrery defines its own format: sections in square brackets holding `key = value`
lines, with whole-line comments introduced by `#`, and a fully qualified key
form `section.key = value` that may appear anywhere. It is specified in
`docs/formats/configuration.md` and parsed by `src/sim/config_file.cpp`, which
is about three hundred lines including its diagnostics.

## Alternatives considered

**TOML.** The closest fit by intent: it exists to be a configuration format, its
syntax for what this project needs is nearly identical to what was written here,
and toml++ is a good library. The objection is proportion. The subset used would
be flat tables of scalars, which is what the file above parses, and the rest of
TOML, arrays of tables, inline tables, dates, times, offsets, multi-line
strings, would arrive as syntax this project must accept, document as accepted,
and then reject at a higher level when someone writes a date where a timestep
belongs. A dependency is also a pin to maintain, a fetch at configure time and a
licence to track, and ADR-0002 already records the care this project takes over
that.

**JSON.** Machine-friendly and hostile to hand editing in exactly the ways that
matter here: no comments, which means a configuration file cannot say why its
timestep is what it is, and strict commas, which is the most common way a person
breaks a file they are editing at midnight. A configuration nobody can annotate
is a configuration whose reasoning lives somewhere else or nowhere.

**YAML.** Comments and a light syntax, and the largest specification of the
three. Significant indentation makes a file's meaning depend on whitespace that
is invisible on screen, and the implicit type rules are a well-known source of
surprise. It is the wrong risk for a file that decides what a numerical
experiment computes.

**Command-line flags alone, with no file.** Simplest, and Phase 7's benchmark
programs already work this way. It fails the requirement rather than answering
it: a run is meant to be reproducible from a document, and a shell history is
not a document. It also has nowhere to put a comment.

## Consequences

The format is small enough that the specification is one page and the parser is
readable in one sitting, which is what section 1 of the implementation plan asks
of any file. It is strict: unknown sections and settings, unparseable values and
repeated keys are all errors naming the line they are on. That strictness is the
main thing this decision buys, because a forgiving parser lets a run compute
something other than what its author asked for and says nothing.

The qualified key form exists so that the command line's `--set` and the file
speak the same language. An override arrives with nowhere to put a section
heading, and inventing a second syntax for it would mean two spellings of every
setting.

The format is not a general one and should not grow into one. It has no arrays,
no nesting beyond one level and no types other than number, boolean and string,
because the configuration it describes needs none of those. A future phase that
genuinely needs a list, several initial conditions superposed into a galaxy
collision being the likely case, should reconsider this decision in a new ADR
rather than adding brackets to this parser.

Numbers are read and written in the classic locale explicitly. This is the kind
of defect that never appears on the machine that wrote the code: on a system
configured for a language whose decimal separator is a comma, a parser using the
environment's locale reads `timestep = 0.001` as one rather than as a
thousandth, and every run on that machine is wrong by a factor of a thousand
with nothing to indicate it.
