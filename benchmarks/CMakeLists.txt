# Measurement programs. These are not tests: they report numbers rather than
# assert them, and nothing in CI depends on what they print.
#
# They live outside tests/ for that reason. A benchmark registered with CTest
# would either fail the build on a machine that happened to be busy, or assert
# nothing and waste the time of every run. The figures they produce are recorded
# in docs/performance/ alongside the machine state they were taken on, which is
# where a performance claim can be checked against the conditions that produced
# it.
#
# The measurement methodology all of them share, added first because the
# programs below link it.
add_subdirectory(harness)

add_executable(orrery_threading_scaling threading_scaling.cpp)

target_link_libraries(
  orrery_threading_scaling
  PRIVATE orrery::benchmark_harness orrery::solvers orrery::backend
          orrery::initial_conditions orrery::core orrery::warnings)

orrery_enable_linting(orrery_threading_scaling)

# The measurements Phase 7 exists for: this machine's bandwidth and
# floating-point ceilings, each kernel's achieved fraction of them, and the
# roofline plot that puts the two on one pair of axes.
add_executable(orrery_roofline roofline.cpp)

target_link_libraries(
  orrery_roofline
  PRIVATE orrery::benchmark_harness orrery::solvers orrery::backend
          orrery::initial_conditions orrery::core orrery::warnings)

orrery_enable_linting(orrery_roofline)

# What Phase 8 exists to establish: that the tree is N log N over two decades,
# where it overtakes direct summation on this machine, and what its
# approximation costs in accuracy at each opening angle.
add_executable(orrery_tree_scaling tree_scaling.cpp)

target_link_libraries(
  orrery_tree_scaling
  PRIVATE orrery::benchmark_harness orrery::solvers orrery::backend
          orrery::initial_conditions orrery::core orrery::warnings)

orrery_enable_linting(orrery_tree_scaling)

# What Phase 9 exists to establish: what the integrated GPU is worth against the
# CPU it shares its memory with, where the staging cost of ADR-0027 goes, and
# what the kernel's answer is worth against the compensated reference.
#
# Built in every configuration rather than only when the backend is on, so that
# a reader who runs it in an ordinary build is told the backend is absent rather
# than left wondering why the program does not exist.
add_executable(orrery_sycl_direct sycl_direct.cpp)

target_link_libraries(
  orrery_sycl_direct
  PRIVATE orrery::benchmark_harness orrery::solvers orrery::backend
          orrery::initial_conditions orrery::core orrery::warnings)

orrery_enable_linting(orrery_sycl_direct)

# What Phase 10 exists to establish: that the coherent traversal is worth what
# it costs, at what sub-group width, and how far the tree scales on this device
# before something other than the traversal becomes the limit.
#
# Built in every configuration, on the same terms as the program above.
add_executable(orrery_sycl_tree sycl_tree.cpp)

target_link_libraries(
  orrery_sycl_tree
  PRIVATE orrery::benchmark_harness orrery::solvers orrery::backend
          orrery::initial_conditions orrery::core orrery::warnings)

orrery_enable_linting(orrery_sycl_tree)

# What Phase 10 exists to establish: what a discrete NVIDIA card is worth beside
# the integrated part this project measured first, what the transfer that
# hardware makes unavoidable costs, and whether the divergence mitigation of
# ADR-0029 is worth what it costs on a second vendor.
#
# Built in every configuration, on the same terms as the two programs above: a
# reader who runs it in an ordinary build is told the backend is absent rather
# than left wondering why the program does not exist.
add_executable(orrery_cuda_scaling cuda_scaling.cpp)

target_link_libraries(
  orrery_cuda_scaling
  PRIVATE orrery::benchmark_harness orrery::solvers orrery::backend
          orrery::initial_conditions orrery::core orrery::warnings)

orrery_enable_linting(orrery_cuda_scaling)

# The three ceiling probes, in a target of their own for the reason
# src/solvers/CMakeLists.txt gives about the kernels: this is a translation unit
# a different compiler reads, and the project's own C++ options must not reach
# it.
#
# They are ceilings rather than measurements of this project's code, so they
# would belong in harness/ if they were not device code. They are not there
# because the harness is linked by every benchmark, including the ones that run
# on machines with no toolkit, and a library that pulled in the CUDA runtime
# would make the whole of benchmarks/ conditional on it.
if(ORRERY_ENABLE_CUDA)
  add_library(orrery_cuda_probes STATIC cuda_probes.cu)

  target_include_directories(
    orrery_cuda_probes PRIVATE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/benchmarks>)

  target_link_libraries(orrery_cuda_probes PRIVATE orrery::cuda)

  set_target_properties(
    orrery_cuda_probes
    PROPERTIES CUDA_ARCHITECTURES "${ORRERY_CUDA_ARCHITECTURES}"
               CUDA_STANDARD 20
               CUDA_STANDARD_REQUIRED ON)

  if(ORRERY_WARNINGS_AS_ERRORS)
    target_compile_options(orrery_cuda_probes PRIVATE --Werror=all-warnings)
  endif()

  target_link_libraries(orrery_cuda_scaling PRIVATE orrery_cuda_probes)
endif()
