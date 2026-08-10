# The force solvers: direct summation now, Barnes-Hut in Phase 8.
#
# It sits above integrators and depends on it, which is the one direction that
# might look wrong at first glance. A solver is an acceleration field, and that
# interface is declared in the layer that consumes it rather than in the layer
# that implements it, so that an integrator needs to know nothing about solvers.
# ADR-0012 records that choice and ADR-0014 the interface added here on top of
# it.
add_library(orrery_solvers STATIC direct_solver.cpp)
add_library(orrery::solvers ALIAS orrery_solvers)

# Both are public: the solver interface derives from the integrators'
# acceleration field and every function here takes spans over core's component
# arrays, so a consumer of this library is a consumer of both whether it says so
# or not.
target_link_libraries(
  orrery_solvers
  PUBLIC orrery::integrators orrery::core
  PRIVATE orrery::warnings)

orrery_enable_linting(orrery_solvers)
