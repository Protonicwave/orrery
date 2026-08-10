# The configurations a run starts from: sampled models and analytic orbits.
#
# A layer of its own rather than a corner of core. It depends on core and on
# nothing else, and nothing in core depends on it, so the dependency direction
# in the implementation plan is preserved by construction. ADR-0010 records why
# it is not simply part of core.
add_library(orrery_initial_conditions STATIC centre_of_mass_frame.cpp kepler.cpp
                                             plummer.cpp uniform_sphere.cpp)
add_library(orrery::initial_conditions ALIAS orrery_initial_conditions)

# core is public because every one of these headers hands back a ParticleData,
# so a consumer of this library is a consumer of that one whether it says so or
# not.
target_link_libraries(
  orrery_initial_conditions
  PUBLIC orrery::core
  PRIVATE orrery::warnings)

orrery_enable_linting(orrery_initial_conditions)
