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
