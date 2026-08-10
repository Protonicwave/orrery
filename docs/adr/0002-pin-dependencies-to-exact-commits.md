# ADR-0002: Pin dependencies to exact commits fetched at configure time

- **Status:** Accepted
- **Date:** 2026-08-10

## Context

Orrery needs third-party code. Catch2 arrives in Phase 1, and pybind11, a
plotting path and a windowing library follow in later phases. The project's
central claim is that its results are reproducible, and a result is only
reproducible if the code that produced it can be reconstructed. That includes
the code the project did not write.

The machine this project is developed on is a Windows laptop, continuous
integration runs on Linux, macOS and Windows runners, and a reader who clones
the repository may be on any of the three. Whatever acquires dependencies has
to work on all of them without a separate setup document.

## Decision

Dependencies are acquired with CMake's `FetchContent` at configure time and
pinned to an exact commit hash, recorded with the release that hash belongs to
in `cmake/Dependencies.cmake`. Each declaration also passes `FIND_PACKAGE_ARGS`,
so a copy already present on the system is used in preference to fetching one.

## Alternatives considered

**Git submodules.** They pin to a commit, which is the property that matters,
but they place the pin in a file that is invisible in a normal diff and they
require a clone step that is easy to forget. A shallow clone without
`--recurse-submodules` produces a confusing configure failure rather than a
clear one. The pin belongs somewhere a reviewer reads, which is the CMake code.

**A tag rather than a commit.** A tag is a mutable reference. It can be deleted
and recreated pointing at different code, and then a checkout of a given Orrery
revision no longer builds what it built before, with nothing in this repository
having changed. That failure is rare and extremely difficult to diagnose, and
avoiding it costs one line.

**System packages only, through `find_package`.** This is what a distribution
packager wants and it is why the escape hatch exists, but as the only mechanism
it makes the version depend on the machine. Three continuous integration
platforms would then test three different Catch2 versions and a contributor
would test a fourth.

**A package manager such as vcpkg or Conan.** Both solve this problem well and
both add a tool that a reader has to install and understand before the project
builds at all. With a dependency list this short, the tool costs more than it
saves. This decision would be worth revisiting if the list grows past about
half a dozen entries, particularly once the renderer's dependencies arrive.

## Consequences

The first configure of a fresh build tree needs network access and takes as
long as cloning the dependency, and each new build tree pays that cost again
because the download is not shared between them. Continuous integration pays it
on every run until a cache is added, which is worth doing when the dependency
list is longer than one entry.

Updating a dependency is a deliberate commit that changes a hash and says why,
rather than something that happens silently when a tag moves or a runner image
changes. That is the intended effect.

An offline build is possible only through the `find_package` path, with a
system copy of the dependency installed beforehand.

Dependencies are built from source with their own build settings, not the
project's. ADR-0003 covers why that separation matters.
