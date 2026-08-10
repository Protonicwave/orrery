# One test executable per layer. A failure then names the layer it came from,
# and a layer's tests can be run on their own while working on it.
include(Catch)

add_executable(
  orrery_core_test
  core/aligned_allocator_test.cpp
  core/build_info_test.cpp
  core/diagnostics_test.cpp
  core/particle_data_test.cpp
  core/random_test.cpp
  core/softening_test.cpp
  core/types_test.cpp
  core/vec3_array_test.cpp
  core/vec3_test.cpp)

target_link_libraries(
  orrery_core_test PRIVATE orrery::core orrery::warnings
                           Catch2::Catch2WithMain)

orrery_enable_linting(orrery_core_test)

add_executable(
  orrery_initial_conditions_test
  initial_conditions/centre_of_mass_frame_test.cpp
  initial_conditions/kepler_test.cpp
  initial_conditions/plummer_test.cpp
  initial_conditions/uniform_sphere_test.cpp)

target_link_libraries(
  orrery_initial_conditions_test
  PRIVATE orrery::initial_conditions orrery::core orrery::warnings
          Catch2::Catch2WithMain)

orrery_enable_linting(orrery_initial_conditions_test)

add_executable(
  orrery_integrators_test
  integrators/conservation_test.cpp integrators/convergence_test.cpp
  integrators/energy_behaviour_test.cpp integrators/integrator_test.cpp)

# The integrators depend on core alone, but their tests also need somewhere to
# start from and something to measure: a Kepler orbit with a known period and a
# sampled Plummer sphere. A test linking a layer the code under test does not is
# the normal shape here, since the validation is against analytic configurations
# rather than against fixtures written by hand.
target_link_libraries(
  orrery_integrators_test
  PRIVATE orrery::integrators orrery::initial_conditions orrery::core
          orrery::warnings Catch2::Catch2WithMain)

orrery_enable_linting(orrery_integrators_test)

add_executable(
  orrery_solvers_test
  solvers/conservation_test.cpp solvers/direct_solver_test.cpp
  solvers/kepler_orbit_test.cpp)

# The solver is validated by integrating configurations it did not create, so
# these tests reach across three layers: the solver under test, an integrator to
# move the particles with, and the analytic orbits and sampled spheres to start
# from. That is the shape the validation category asks for, since the comparison
# is against closed-form results rather than against fixtures written by hand.
target_link_libraries(
  orrery_solvers_test
  PRIVATE orrery::solvers orrery::integrators orrery::initial_conditions
          orrery::core orrery::warnings Catch2::Catch2WithMain)

orrery_enable_linting(orrery_solvers_test)

# Registering each Catch2 test case with CTest individually, rather than the
# whole binary as one test, means a failure report names the case that failed
# and that cases can be selected by name from ctest.
catch_discover_tests(orrery_core_test)
catch_discover_tests(orrery_initial_conditions_test)
catch_discover_tests(orrery_integrators_test)
catch_discover_tests(orrery_solvers_test)
