# ADR-0042: Package the whole project from the repository root

- **Status:** Accepted
- **Date:** 2026-08-12

## Context

The extension module links every layer of the C++ library, so building it means
building the project. Python's packaging expects a `pyproject.toml`, and pip
builds a package from the directory that file sits in: everything the build
needs has to be at or below it, because that is what goes into a source
distribution.

That leaves one real question, which is where `pyproject.toml` goes, and one
that follows from it, which is which build backend drives CMake.

## Decision

`pyproject.toml` is at the repository root and the backend is scikit-build-core.
`pip install .` from the root configures this project's CMake with the bindings
switched on, the tests and benchmarks switched off, and warnings not treated as
errors, then packages what the install step produces.

The version has one source: scikit-build-core reads it out of the `project()`
call in `CMakeLists.txt`, which is the same declaration `core::version()`
reports at run time.

## Alternatives considered

**`pyproject.toml` under `python/`.** It would keep the Python packaging beside
the Python package, which is tidier to look at. It cannot work: an sdist built
there would contain `python/` and nothing else, and the C++ sources it has to
compile are all above it. The tidiness is available anyway, since every file
this decision touches other than `pyproject.toml` is under `python/`.

**setuptools with a custom build_ext.** The traditional arrangement, and it
means writing the CMake invocation, the build directory handling, the
cross-platform library placement and the wheel tagging by hand in `setup.py`.
scikit-build-core exists because everybody who did that wrote the same two
hundred lines.

**scikit-build, the original.** Superseded by scikit-build-core, which is the
same idea reimplemented as a PEP 517 backend without the setuptools
dependency.

**Meson-python.** A good backend, but the project builds with CMake and would
have to acquire a second build system to use it.

**No wheel at all: build with the CMake preset and set `PYTHONPATH`.** This is
what the `python` preset already provides, and it is what a developer working on
the bindings uses. It is not a distribution: it cannot be installed, cannot
declare that it needs NumPy, and cannot be given to somebody who wants to use
the simulator rather than work on it.

## Consequences

A wheel build compiles the whole C++ library, which takes a couple of minutes.
There is no way around that and no reason to want one; the extension is the
library.

Building a wheel needs no network access beyond the build requirements, because
the CMake dependency declarations prefer a package already present
(`FIND_PACKAGE_ARGS`) and the isolated build environment pip creates already
holds the pybind11 named in `build-system.requires`. Catch2 and GLFW are not
fetched at all, since the tests and the renderer are off.

Warnings are not errors in a wheel build. A wheel may be compiled by a version
of a compiler this project has never seen, and a person running `pip install`
wants a working installation rather than a report on our diagnostic set.
Continuous integration is where warnings are errors, and it builds the bindings
with the same settings as everything else.

The two build routes produce the same package by different means: the preset
writes the extension into a directory in the build tree that holds copies of the
package sources, and the wheel takes the sources from `python/orrery` and the
extension from the install rule. Both end with `_orrery` inside `orrery`, which
is the only arrangement the package's own import statement can read, and a
mistake in either shows up immediately as a failed import.
