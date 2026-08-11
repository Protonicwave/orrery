# The force solvers: direct summation and Barnes-Hut.
#
# It sits above integrators and depends on it, which is the one direction that
# might look wrong at first glance. A solver is an acceleration field, and that
# interface is declared in the layer that consumes it rather than in the layer
# that implements it, so that an integrator needs to know nothing about solvers.
# ADR-0012 records that choice and ADR-0014 the interface added here on top of
# it.
add_library(
  orrery_solvers STATIC
  barnes_hut_solver.cpp direct_kernel.cpp direct_kernel_scalar.cpp
  direct_solver.cpp morton.cpp octree.cpp reference_kernel.cpp tree_walk.cpp)
add_library(orrery::solvers ALIAS orrery_solvers)

# All three are public: the solver interface derives from the integrators'
# acceleration field, every function here takes spans over core's component
# arrays, and a solver is constructed from a reference to a backend executor. A
# consumer of this library is a consumer of all three whether it says so or not.
target_link_libraries(
  orrery_solvers
  PUBLIC orrery::integrators orrery::backend orrery::core
  PRIVATE orrery::warnings)

# The AVX2 kernel, compiled for an instruction set the rest of the project is
# not compiled for. That separation is the whole mechanism behind ADR-0018: one
# translation unit may use AVX2 because the dispatch in direct_kernel.cpp will
# not call into it unless the processor reports the feature, and every other
# translation unit stays at the baseline so the binary starts on machines that
# do not.
#
# Guarded on the architecture rather than on the compiler, because the question
# is whether the target has these instructions at all. On anything else the
# kernel is not compiled, ORRERY_HAS_AVX2_KERNEL is not defined, and the scalar
# kernel is the only one there is. That is a working build rather than a
# degraded one: the project's correctness has never depended on vector width.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^([Xx]86_64|AMD64|amd64|i[3-6]86|[Xx]86)$")
  target_sources(orrery_solvers PRIVATE direct_kernel_avx2.cpp)

  # Public because it guards a declaration in a public header, so a consumer
  # compiled without it would not see the same interface the library was built
  # against. The precision switch in core/types.hpp is public for the same
  # reason.
  target_compile_definitions(orrery_solvers PUBLIC ORRERY_HAS_AVX2_KERNEL)

  if(MSVC)
    # MSVC has no way to enable FMA separately: /arch:AVX2 brings both, which is
    # correct for every part that implements either.
    set_source_files_properties(direct_kernel_avx2.cpp PROPERTIES COMPILE_OPTIONS
                                                                  "/arch:AVX2")
  else()
    # FMA asked for by name rather than left to -mavx2, which does not imply it.
    # The kernel accumulates with fused multiply-add and would fail to compile
    # without it, which is the right failure: a silent fallback to separate
    # multiply and add would change the arithmetic the accuracy tests measure.
    #
    # GCC warns that it is ignoring the alignment attributes on __m256d when the
    # type appears as a template argument, which it does here in the alias that
    # selects the register width from the build's scalar type. The attributes
    # are not in fact ignored, the warning is long-standing and known to be
    # spurious for the intrinsic vector types, and there is no way to write the
    # alias without it. Suppressed on this file alone, where the intrinsics are.
    set(orrery_avx2_options "-mavx2;-mfma")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      list(APPEND orrery_avx2_options "-Wno-ignored-attributes")
    endif()
    set_source_files_properties(direct_kernel_avx2.cpp
                                PROPERTIES COMPILE_OPTIONS "${orrery_avx2_options}")
  endif()
endif()

orrery_enable_linting(orrery_solvers)
