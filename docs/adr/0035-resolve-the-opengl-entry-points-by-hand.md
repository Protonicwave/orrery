# ADR-0035: Resolve the OpenGL entry points by hand

- **Status:** Accepted
- **Date:** 2026-08-11

## Context

An OpenGL library exports the functions of version 1.1 and nothing later.
Everything a renderer written against version 3.3 calls, buffers, shaders,
vertex arrays, framebuffers, has to be fetched from the driver at run time by
name.

The universal answer is a loader: glad, GLEW or one of their relatives. A
generated loader declares the entire API, resolves every function the driver
offers, and is between five and forty thousand lines of generated C.

## Decision

`include/orrery/viz/gl_api.hpp` declares the thirty-nine entry points this
renderer calls, as a structure of function pointers, and
`src/viz/gl_api.cpp` resolves them from a loader function the window library
supplies. Nothing else in the project sees either file.

## Alternatives considered

**glad.** The usual choice, and it is a generator rather than a library: the
normal way to use it is to run a Python script or a web form and commit the
result. A generated artefact in the source tree is a thing nobody reads, nobody
reviews and nobody can regenerate identically without recording the exact
generator options somewhere. Fetching it at configure time as this project
fetches Catch2 and GLFW is not available, because what is on the repository is
the generator and not the loader.

**GLEW.** A library rather than a generator, so it can be pinned like anything
else. It is also a dependency with its own build system quirks, it resolves the
whole API whether or not it is used, and on a core profile it needs a flag set
before initialisation to avoid an error that appears as a mysterious failure at
start-up. For thirty-nine functions this is a large amount of machinery.

**Linking the entry points directly.** Not possible for anything above OpenGL
1.1 on Windows, and on Linux it works only by accident of which symbols the
loader library happens to export. The run-time fetch is the specified way.

## Consequences

The list is the cost. Calling a function that is not in it means adding a
declaration and a line in the resolver, which is two lines of mechanical edit
that a generated loader would not need. That is a real friction and it is
proportionate: the whole renderer is one pipeline drawing one kind of primitive,
and the set of calls it makes is nearly closed.

The declarations have to match the API exactly, because nothing checks them. A
wrong signature is undefined behaviour and a wrong constant is a silently wrong
render. They are therefore written from the registry, each constant as the
hexadecimal value the registry gives it rather than derived from anything, and
the two lists in the header and the resolver are kept in the same order so that
they read together.

A missing entry point is reported by name at construction and stops the viewer
there. A loader that left a null pointer behind would crash inside the driver
with no indication of which call it was.

The structure has no global state, so a second context would get its own set of
pointers rather than overwriting the first. Nothing needs that today; it is a
consequence of the shape rather than a feature.
