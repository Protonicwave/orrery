# ADR-0005: Allocate particle arrays on cache-line boundaries

- **Status:** Accepted
- **Date:** 2026-08-10

## Context

ADR-0004 stores particle state as contiguous component arrays so that a vector
load reads consecutive coordinates. Where those arrays begin decides whether
that works.

Two costs follow from an array that starts at an arbitrary address. A 256-bit
AVX2 load reads 32 bytes; from a misaligned array some of those loads span two
64-byte cache lines, and a split load costs two line fills rather than one on a
kernel that is already limited by how fast lines can be filled. Separately, the
threading in Phase 6 divides each array into per-thread ranges, and two threads
whose ranges meet inside a cache line contend for exclusive ownership of that
line on every write even though neither touches the other's elements. That is
false sharing, and it is invisible in the source: the code looks perfectly
partitioned.

The default allocator provides alignment suitable for any fundamental type,
which is 16 bytes on the platforms this project targets. That is enough for
correctness and not enough for either of the above.

## Decision

Particle component arrays are allocated through `orrery::core::AlignedAllocator`,
a stateless standard-conforming allocator that returns storage aligned to
`kCacheLineBytes`, which is 64. It is implemented with the aligned overloads of
`::operator new` and `::operator delete`. `ParticleData` uses
`std::vector<Real, AlignedAllocator<Real>>` for each of its ten arrays, so the
guarantee survives every reallocation the container performs as it grows.

The alignment is a constant rather than a template parameter. It is one fact
about one machine, decided here once, rather than a value each call site could
choose differently and get wrong unobserved.

## Alternatives considered

**Use the default allocator and `std::assume_aligned` in the kernels.** This is
the cheapest change and the most dangerous one. `std::assume_aligned` does not
align anything; it promises the compiler that something already is, and the
compiler then emits aligned instructions on that promise. If the promise is
false the program has undefined behaviour, which on x86-64 usually means it
works until a vector store faults. A promise that the allocator makes true is
sound; a promise made about the default allocator is not.

**Over-allocate and align the pointer by hand.** The traditional answer, and it
works, but it needs the original pointer kept somewhere to free it later, which
means either a header before the data, which reintroduces the offset that was
being removed, or a side table. The standard has provided aligned `operator new`
since C++17 and it is available on all three supported compilers.

**`posix_memalign` or `_aligned_malloc`.** Platform-specific, spelled
differently on each of the three platforms, and no better than the standard
facility that replaced them.

**Over-align `Real` itself with `alignas`.** This would align every scalar,
including every single one in every array, which does not mean anything useful
and would inflate a 4-byte float to a 64-byte object. It confuses aligning the
array with aligning its elements.

**Make the alignment a template parameter.** More flexible and the flexibility
has no customer. A per-container choice would also mean the value could differ
between the arrays a kernel reads together, and a fixed alignment additionally
keeps the allocator in the `Alloc<T>` shape that `std::allocator_traits` can
rebind on its own, so the class needs no rebind member.

## Consequences

Every allocation is rounded up to a multiple of the alignment, so a container of
a handful of particles wastes up to 63 bytes per array. At the sizes this
project runs at, hundreds of thousands of particles upwards, that is not
measurable. At the sizes the tests run at it is not measurable either, because
the tests measure correctness.

The allocator becomes part of the container's type, so `std::vector<Real>` and
the component arrays are different types and do not interconvert. Interfaces
therefore pass `std::span`, which is what the conventions require anyway and
which hides the distinction entirely.

Alignment is necessary for the vector loads of Phase 7 but not sufficient: the
loads also have to be issued from indices that are multiples of the vector
width, which is a question about how the kernel walks the array and about
whether the arrays are padded. That is left to the phase that writes the kernel.

The same alignment is a requirement rather than a preference for the SYCL
backend in Phase 9, where an allocation shared with the GPU without a copy has
its own alignment expectations. Meeting the stricter requirement now costs
nothing and removes a reason to reallocate later.
