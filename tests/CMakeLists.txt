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
  orrery_backend_test backend/cpu_features_test.cpp backend/cpu_topology_test.cpp
                      backend/executor_test.cpp backend/partition_test.cpp)

target_link_libraries(
  orrery_backend_test PRIVATE orrery::backend orrery::core orrery::warnings
                              Catch2::Catch2WithMain)

orrery_enable_linting(orrery_backend_test)

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
  solvers/conservation_test.cpp
  solvers/direct_kernel_test.cpp
  solvers/direct_solver_test.cpp
  solvers/kepler_orbit_test.cpp
  solvers/kernel_accuracy_test.cpp
  solvers/morton_test.cpp
  solvers/octree_test.cpp
  solvers/parallel_direct_solver_test.cpp)

# The solver is validated by integrating configurations it did not create, so
# these tests reach across four layers: the solver under test, the backend that
# schedules its loop, an integrator to move the particles with, and the analytic
# orbits and sampled spheres to start from. That is the shape the validation
# category asks for, since the comparison is against closed-form results rather
# than against fixtures written by hand.
target_link_libraries(
  orrery_solvers_test
  PRIVATE orrery::solvers orrery::integrators orrery::initial_conditions
          orrery::backend orrery::core orrery::warnings Catch2::Catch2WithMain)

orrery_enable_linting(orrery_solvers_test)

# The measurement harness, which lives under benchmarks/ because it is not a
# layer of the simulator. It is still code this project owns and section 5 of
# the implementation plan asks for it to be covered, so it is tested like
# anything else. Guarded on the target rather than on ORRERY_BUILD_BENCHMARKS so
# that a build without the benchmarks still produces a complete suite for what
# it did build.
if(TARGET orrery::benchmark_harness)
  add_executable(
    orrery_benchmark_test
    benchmark/machine_limits_test.cpp benchmark/protocol_test.cpp
    benchmark/roofline_plot_test.cpp benchmark/statistics_test.cpp)

  target_link_libraries(
    orrery_benchmark_test PRIVATE orrery::benchmark_harness orrery::backend
                                  orrery::core orrery::warnings
                                  Catch2::Catch2WithMain)

  orrery_enable_linting(orrery_benchmark_test)

  catch_discover_tests(orrery_benchmark_test)
endif()

# Registering each Catch2 test case with CTest individually, rather than the
# whole binary as one test, means a failure report names the case that failed
# and that cases can be selected by name from ctest.
catch_discover_tests(orrery_backend_test)
catch_discover_tests(orrery_core_test)
catch_discover_tests(orrery_initial_conditions_test)
catch_discover_tests(orrery_integrators_test)
catch_discover_tests(orrery_solvers_test)
