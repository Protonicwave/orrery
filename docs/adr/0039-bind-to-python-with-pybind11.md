# ADR-0039: Bind to Python with pybind11

- **Status:** Accepted
- **Date:** 2026-08-12

## Context

Phase 13 exposes the simulator to Python. The interface it has to carry is
small and awkward in equal measure: a dozen aggregates of scalars, four
enumerations, a move-only `Simulation` holding three polymorphic objects by
unique pointer, and ten contiguous arrays that must reach NumPy without being
copied.

The last of those is the constraint that decides everything. At the sizes this
project runs at a copy of the state is tens of megabytes, so a binding that
cannot hand out a borrowed view is not a slower binding but a useless one. The
tool therefore has to understand both C++ object lifetimes and NumPy's buffer
protocol, and it has to keep the C++ object alive for as long as the array that
points into it.

The second constraint is the one section 5 of the implementation plan imposes:
the extension has to build clean under this project's warning set with Clang,
GCC and MSVC, and be buildable by CMake as one more target in the existing tree.

## Decision

The bindings are written with pybind11, pinned to an exact commit like every
other dependency (ADR-0002), and fetched at configure time or found from the
environment that is building a wheel.

## Alternatives considered

**nanobind.** By the same author, several times faster to compile, and it
produces smaller extensions. It was the closest call here. It was not taken for
two reasons that are both about this project rather than about the library: it
requires Python 3.8 and C++17 and drops support for older interpreters, which is
fine, but it is also a younger library with a smaller installed base, and the
value of a binding layer in a project whose point is that a reviewer can check
it is that the reviewer already knows how to read it. The compile time it would
save is real and is measured in seconds on five translation units. If the
binding surface grows to the point where compile time matters, this ADR is the
one to supersede.

**Cython.** It would mean a second language in the repository with its own build
step, its own syntax for declaring the C++ interface, and a generated C file
that nobody reads but everybody has to rebuild. Its advantage is speed at the
call boundary, which is worth nothing here: every call this interface makes is
either trivial or is a force evaluation costing billions of operations.

**ctypes or CFFI over a C shim.** No build-time dependency at all, which is
genuinely attractive. It would require writing and maintaining a C interface to
a C++ library whose types are `std::span`, `std::unique_ptr` and a class
hierarchy, which means hand-written wrappers for every function, hand-written
lifetime management for the arrays, and an error-reporting convention invented
here. That is the whole of what pybind11 does, written again and less carefully.

**SWIG.** Generates bindings for many languages from an interface file. This
project needs one language, and SWIG's output for modern C++ needs as much
hand-holding as it saves.

**Expose nothing, and drive the simulator from files.** The command-line program
already reads a configuration and writes a trajectory, so a Python user could
run it and parse the output. That is the status quo the phase exists to improve
on: it makes an interactive parameter sweep a matter of writing files and
spawning processes, and it makes reading a state a matter of parsing a binary
format rather than looking at an array.

## Consequences

pybind11 is a build-time dependency of the extension and of nothing else. The
C++ library, the command-line program and the viewer are unaffected, and a build
configured without `ORRERY_BUILD_PYTHON` never fetches it.

pybind11 headers are the slowest thing this project compiles. The bindings are
split into four translation units, one per group of types, which keeps a change
to the configuration bindings from rebuilding the array views.

The exception translation is pybind11's: `std::invalid_argument` becomes
`ValueError`, `std::runtime_error` becomes `RuntimeError`, and the
`ConfigurationError` the parser raises arrives as a `RuntimeError` carrying its
message. That is enough to tell a person what went wrong and not enough to
distinguish a parse failure from an I/O failure programmatically. If that
becomes a real need, registering the exception is three lines.
