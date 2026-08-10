# ADR-0003: Carry build settings on interface targets rather than global flags

- **Status:** Accepted
- **Date:** 2026-08-10

## Context

The definition of done requires that Orrery builds clean with Clang, GCC and
MSVC with warnings as errors, and that address and undefined-behaviour
sanitiser builds pass the full test suite. Both requirements are expressed as
compiler flags, and both must apply to the code this project owns.

They must not apply to anything else. Catch2 is built from source inside the
same CMake tree, and the later phases add more dependencies of the same kind.
Third-party code compiled with this project's warning set will produce
diagnostics, those diagnostics will be errors, and the project cannot fix them
in code it does not own. The usual response is to start disabling warnings
until the dependency compiles, which weakens the check for the code it was
meant to protect.

## Decision

No CMake code in this project assigns to `CMAKE_CXX_FLAGS`, calls
`add_compile_options` at directory scope, or otherwise sets a flag globally.
Build settings are carried on three interface libraries, defined in
`cmake/BuildSettings.cmake`, and every target states which it wants:

- `orrery::options`, the language level and the per-compiler conformance
  settings, linked publicly because a consumer that disagrees about them
  disagrees about the meaning of the headers.
- `orrery::warnings`, the diagnostic set, linked privately because it is a
  standard this project holds itself to rather than one it imposes on anything
  that links it.
- `orrery::sanitisers`, instrumentation, linked publicly because it has to
  reach both the compile line and the link line of everything in the binary.

clang-tidy follows the same rule. It is attached to individual targets through
the `CXX_CLANG_TIDY` property in `cmake/Linting.cmake`, not set through
`CMAKE_CXX_CLANG_TIDY`, which would lint every target created afterwards
including the fetched dependencies.

## Alternatives considered

**Set `CMAKE_CXX_FLAGS` at the top of the tree.** The shortest way to express
the requirement and the reason the problem above exists. It also makes the
project unusable as a subdirectory of anything else, since a consumer would
silently acquire this project's flags.

**`add_compile_options` in the project's own directories only.** Better, and it
does keep dependencies out of it, but the settings then follow directory
structure rather than intent. A test executable and a benchmark in the same
directory could not differ, and nothing in the build says which setting a
target has or why.

**One combined settings target.** Simpler to write and simpler to forget to
think about. Warnings and options differ in the property that matters here:
warnings must stay private to this project and options must be public. Merging
them forces one of the two to be wrong.

**A toolchain file.** Toolchain files describe the compiler and platform, not a
project's opinion about diagnostics. Using one this way would make the project
build differently depending on how it was configured, which is the opposite of
what is wanted, and it would still apply to dependencies.

## Consequences

Every target the project defines has to link the interface libraries
explicitly, and a target that forgets gets no warnings and no instrumentation.
The failure is silent. With the small number of targets this project will have
it is caught in review, but a helper function that wraps `add_library` becomes
worth writing if the count grows.

Dependencies build with their own settings, which is the intended outcome: the
project's warning set applies to the project, and Catch2's headers are marked
as system headers so that their contents do not produce diagnostics in
translation units that include them.

If Orrery is ever installed and exported for use through `find_package`, the
interface targets that are linked publicly have to be exported alongside the
libraries that use them. That is a known cost of the approach and is not
addressed here, because the project does not install anything yet.
