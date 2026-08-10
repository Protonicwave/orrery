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
# Phase 7 replaces the timing methodology used here with a harness that treats
# repeats, dispersion and thermal throttling properly. The programs stay; what
# changes is how they measure.
add_executable(orrery_threading_scaling threading_scaling.cpp)

target_link_libraries(
  orrery_threading_scaling
  PRIVATE orrery::solvers orrery::backend orrery::initial_conditions
          orrery::core orrery::warnings)

orrery_enable_linting(orrery_threading_scaling)
