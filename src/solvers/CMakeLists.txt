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
  barnes_hut_solver.cpp cuda_direct_solver.cpp cuda_tree_solver.cpp
  direct_kernel.cpp direct_kernel_scalar.cpp direct_solver.cpp morton.cpp
  octree.cpp reference_kernel.cpp sycl_direct_solver.cpp sycl_tree_solver.cpp
  tree_walk.cpp)
add_library(orrery::solvers ALIAS orrery_solvers)

# All three are public: the solver interface derives from the integrators'
# acceleration field, every function here takes spans over core's component
# arrays, and a solver is constructed from a reference to a backend executor. A
# consumer of this library is a consumer of all three whether it says so or not.
#
# The GPU solver reaches SYCL through orrery::backend, which links orrery::sycl
# publicly, so sycl_direct_solver.cpp is compiled with -fsycl and guarded by the
# same definition without this file repeating either. It is in the source list
# unconditionally and compiles to nothing when the option is off, which is the
# arrangement the discovery layer beneath it uses.
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
#
# Emscripten is excluded by name rather than by processor, because it reports
# CMAKE_SYSTEM_PROCESSOR as x86 and is not: the target is wasm32, whose vector
# extension is SIMD128 and which has no AVX2 and no CPUID to ask about it.
# Without this the kernel is compiled with -mavx2 for a machine that will never
# run it, and the run-time dispatch then correctly refuses to call it, because
# cpu_features.cpp has no CPUID on this target and reports nothing. That is a
# module carrying a kernel it cannot execute, which is a size the download pays
# for and a claim the build would be making falsely.
if(NOT EMSCRIPTEN AND CMAKE_SYSTEM_PROCESSOR MATCHES
                      "^([Xx]86_64|AMD64|amd64|i[3-6]86|[Xx]86)$")
  target_sources(orrery_solvers PRIVATE direct_kernel_avx2.cpp)

  # Public because it guards a declaration in a public header, so a consumer
  # compiled without it would not see the same interface the library was built
  # against. The precision switch in core/types.hpp is public for the same
  # reason.
  target_compile_definitions(orrery_solvers PUBLIC ORRERY_HAS_AVX2_KERNEL)

  # The oneAPI compiler presents an MSVC-like command line, so it reaches the
  # MSVC branch below and accepts /arch:AVX2 without complaint. It then produces
  # AVX2 code an order of magnitude slower than Clang's, because underneath it is
  # Clang and /arch:AVX2 does not turn on the target feature its FMA intrinsics
  # need. Measured on this machine at 8192 particles in single precision: the
  # kernel took 251 ms against Clang's 24, which made it slower than the scalar
  # kernel it exists to replace, and the harness's own FMA ceiling probe
  # collapsed from 532 Gflop/s to 42.
  #
  # Nothing warns about this. The build succeeds, the CPUID check passes, the
  # solver reports that it is running the AVX2 kernel, and every figure taken
  # with it is wrong. So the compiler is asked in the spelling it actually
  # understands, which is the one the Clang branch already uses.
  if(MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "IntelLLVM")
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

# The CUDA kernels, and only the kernels.
#
# A target of their own because these are the two translation units in this
# repository that a different compiler reads, and everything that follows is
# about keeping that set small and keeping the project's own settings away from
# it.
#
# They link orrery::cuda and nothing else. In particular they do not link
# orrery::core, which would be the obvious thing to write since they include its
# headers: orrery::core carries orrery::options publicly, and those are C++
# compiler flags. Under MSVC that is /permissive- and three others, which nvcc
# would receive as its own options and reject. The include path and the precision
# switch are therefore set here by hand, which is a duplication of two lines
# rather than of a decision, and the alternative is a generator expression on
# every option in cmake/BuildSettings.cmake to exclude a language most builds do
# not have.
#
# Nothing here is compiled unless ORRERY_ENABLE_CUDA is on, because unlike the
# SYCL sources these files are not valid C++ without their compiler: a kernel
# launch has a syntax no other compiler parses. The host halves in
# cuda_direct_solver.cpp and backend/cuda_*.cpp are compiled in every build and
# guarded internally, which is what keeps the interface answerable.
if(ORRERY_ENABLE_CUDA)
  add_library(orrery_cuda_kernels STATIC cuda_direct_kernel.cu
                                         cuda_tree_kernel.cu)
  add_library(orrery::cuda_kernels ALIAS orrery_cuda_kernels)

  target_include_directories(
    orrery_cuda_kernels PRIVATE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

  target_link_libraries(orrery_cuda_kernels PRIVATE orrery::cuda)

  # The same switch orrery::options carries, applied by hand for the reason
  # above. A kernel compiled against a different scalar type from the solver that
  # launches it would disagree about the size of every array it reads, and the
  # link would succeed.
  if(ORRERY_SINGLE_PRECISION)
    target_compile_definitions(orrery_cuda_kernels PRIVATE ORRERY_SINGLE_PRECISION)
  endif()

  set_target_properties(
    orrery_cuda_kernels
    PROPERTIES CUDA_ARCHITECTURES "${ORRERY_CUDA_ARCHITECTURES}"
               CUDA_STANDARD 20
               CUDA_STANDARD_REQUIRED ON)

  # The project's warning policy in this compiler's spelling. nvcc has no
  # equivalent of the diagnostic set in cmake/BuildSettings.cmake, so what is
  # asked for is that whatever it does diagnose is fatal, which is the part of
  # the policy that matters: a warning nobody has to act on is a warning nobody
  # acts on.
  if(ORRERY_WARNINGS_AS_ERRORS)
    target_compile_options(orrery_cuda_kernels PRIVATE --Werror=all-warnings)
  endif()

  # One diagnostic from the device compiler's front end, suppressed by number
  # rather than by widening the policy above.
  #
  # 554 is "will not be called for implicit or explicit conversions", raised
  # against the conversion operator in core/vec3_span.hpp that turns a mutable
  # view into a read-only one. That operator carries a requires-clause excluding
  # the const specialisation, so for a read-only view it does not exist and
  # Clang, GCC and MSVC all say nothing. EDG, which is nvcc's front end,
  # evaluates the declaration before the constraint and reports a conversion from
  # a type to itself that it then never offers.
  #
  # The code is correct and the remark is a front-end limitation, so the remark
  # is silenced and the header is left alone. Suppressed by number so that this
  # is the only diagnostic it hides: --Werror=all-warnings above stays in force
  # for everything else.
  target_compile_options(orrery_cuda_kernels PRIVATE --diag-suppress=554)

  target_link_libraries(orrery_solvers PRIVATE orrery::cuda_kernels)
endif()

orrery_enable_linting(orrery_solvers)
