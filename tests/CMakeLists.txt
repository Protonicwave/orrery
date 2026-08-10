# One test executable per layer. A failure then names the layer it came from,
# and a layer's tests can be run on their own while working on it.
include(Catch)

add_executable(
  orrery_core_test
  core/aligned_allocator_test.cpp core/build_info_test.cpp
  core/particle_data_test.cpp core/types_test.cpp core/vec3_test.cpp)

target_link_libraries(
  orrery_core_test PRIVATE orrery::core orrery::warnings
                           Catch2::Catch2WithMain)

orrery_enable_linting(orrery_core_test)

# Registering each Catch2 test case with CTest individually, rather than the
# whole binary as one test, means a failure report names the case that failed
# and that cases can be selected by name from ctest.
catch_discover_tests(orrery_core_test)
