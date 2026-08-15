#pragma once

/// \file
/// The one annotation a second device compiler needs, and why the project
/// carries it.
///
/// SYCL is single source: a kernel written inside `parallel_for` is ordinary
/// C++, the device compiler sees the whole call graph through the lambda, and a
/// function the kernel calls is compiled for the device because it was called
/// from device code. Nothing has to be marked. That property is most of why
/// ADR-0025 chose SYCL, and it is why `core::softened_inverse_distance_cubed`
/// has one definition that the CPU kernel, the potential energy diagnostic and
/// the GPU kernel all call.
///
/// CUDA is single source too, and it draws the line in a different place. A
/// function is compiled for the device only if it says so, with `__device__` in
/// front of it, and calling an unmarked function from a kernel is a diagnostic
/// rather than an instruction to go and compile it. So the same property costs
/// an annotation here where it costs nothing there.
///
/// There are two ways to pay for it and only one of them is acceptable.
///
/// The first is to restate the force law inside the CUDA kernel. It is nine
/// lines and it would compile immediately. It would also put a second
/// definition of the softened potential in a project whose headline validation
/// result rests on there being exactly one: ADR-0008 records why the kernel and
/// the diagnostic share a definition, and a backend that quietly forked it
/// would make the energy drift a measurement of the fork. Section 3 of the
/// implementation plan calls this duplicated truth and this is exactly the
/// shape of it.
///
/// The second is this macro. It expands to `__host__ __device__` when a CUDA
/// device compiler is reading the header and to nothing at all otherwise, so
/// every other build in this project, and there are ten of them, sees the same
/// declarations it saw before. The arithmetic is written once and two device
/// compilers are allowed to look at it.
///
/// ## Where it is applied, and where it is not
///
/// On the small arithmetic in `core/vec3.hpp`, `core/softening.hpp` and the two
/// multipole terms in `solvers/tree_walk.hpp`. That set is not chosen for
/// convenience: it is precisely the functions a force kernel evaluates per
/// interaction, which is the same set the SYCL kernels already call and the
/// same set the CUDA kernels call. Nothing that allocates, throws, touches a
/// container or reads a file carries the annotation, because none of those can
/// run on a device and marking them would make the compiler say so at a less
/// useful moment.
///
/// It is not applied to a whole header at a time and it is not applied by
/// wrapping includes in a pragma. Both would compile the annotation onto
/// functions that have no business on a device, and the resulting errors name
/// the header rather than the call.
///
/// ## Why this is a macro when section 4 discourages them
///
/// Because there is no other spelling. The annotation is a compiler extension
/// that has to disappear entirely under every compiler that does not implement
/// it, and a language feature that disappears is what the preprocessor is for.
/// The same argument already justifies `ORRERY_SINGLE_PRECISION` and
/// `ORRERY_ENABLE_SYCL`, and this macro is smaller than either: it names no
/// configuration, it is set by no build option, and it is defined by the
/// compiler that needs it rather than by this project's build system.
///
/// ADR-0060 records the decision.

/// `__host__ __device__` under a CUDA device compiler, and nothing otherwise.
///
/// `__CUDACC__` is defined by nvcc and by the other compilers that consume CUDA
/// C++, and only while they are compiling a `.cu` translation unit. The same
/// header included by an ordinary C++ compiler in the same build, which is what
/// happens for every solver's host half, expands this to nothing and produces
/// the declaration the rest of the project has always had.
#ifdef __CUDACC__
#    define ORRERY_DEVICE __host__ __device__
#else
#    define ORRERY_DEVICE
#endif
