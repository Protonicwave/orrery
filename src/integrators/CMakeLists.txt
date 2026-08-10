# The time integrators, and the acceleration field interface they advance a
# state under.
#
# It depends on core and on nothing else. In particular it does not depend on
# solvers, which sits above it: an integrator calls an abstract acceleration
# field, and the solvers of later phases implement that interface. ADR-0012
# records why the interface is declared here rather than where its
# implementations will live.
add_library(orrery_integrators STATIC runge_kutta4.cpp velocity_verlet.cpp
                                      yoshida4.cpp)
add_library(orrery::integrators ALIAS orrery_integrators)

# core is public because every function here takes a ParticleData or a span over
# its component arrays.
target_link_libraries(
  orrery_integrators
  PUBLIC orrery::core
  PRIVATE orrery::warnings)

orrery_enable_linting(orrery_integrators)
