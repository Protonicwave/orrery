# The execution backends: CPU threading now, SYCL for the integrated GPU in
# Phase 9.
#
# It depends on core and on nothing else. A backend divides a range of indices
# between threads and knows nothing about gravity, which is what lets the same
# scheduler serve the direct solver here and the tree walk of Phase 8 without
# either of them appearing in this layer.
add_library(
  orrery_backend STATIC
  cpu_features.cpp cpu_topology.cpp serial_executor.cpp static_executor.cpp
  sycl_device.cpp sycl_usm.cpp thread_pool.cpp work_stealing_executor.cpp)
add_library(orrery::backend ALIAS orrery_backend)

# The standard threading library is a public dependency because the pool holds
# std::thread and std::mutex members in a header, so anything that includes it
# needs whatever the platform requires to use them, which on Linux is a link
# flag rather than only a compile one.
find_package(Threads REQUIRED)

# core is public because every interface here is expressed in its index type.
#
# orrery::sycl is public and is empty unless ORRERY_ENABLE_SYCL is on. When it
# is, it carries -fsycl, which has to reach the link line of anything that ends
# up containing device code as well as the compile line, and it defines the
# macro that guards the declarations in sycl_device.hpp and sycl_usm.hpp. A
# consumer compiled without that definition would not see the same interface
# this library was built against, which is the same reason the precision switch
# and the AVX2 guard are public.
#
# The two SYCL sources are compiled in every build rather than added
# conditionally. Both are guarded internally and reduce to a few lines reporting
# that there is no device, which is what makes the discovery interface
# answerable on a machine that has no runtime at all.
target_link_libraries(
  orrery_backend
  PUBLIC orrery::core orrery::sycl Threads::Threads
  PRIVATE orrery::warnings)

if(WIN32)
  # windows.h defines min and max as macros and pulls in a large amount of the
  # platform surface unless told not to. Both are corrections to a header this
  # project does not own, and they are set here rather than in the source so
  # that the convention against macros in section 4 of the implementation plan
  # holds in the C++ itself. Private because no consumer of this library
  # includes windows.h through it.
  target_compile_definitions(orrery_backend PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

orrery_enable_linting(orrery_backend)
