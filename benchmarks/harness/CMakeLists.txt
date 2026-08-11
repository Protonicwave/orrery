# The measurement infrastructure the programs in the directory above share.
#
# It is a library rather than a header or a copy in each program because Phase 7
# exists to make one measurement methodology apply to every performance claim
# this project makes. Two benchmarks that each rolled their own timing loop
# would produce two conventions, and the second one to be written would quietly
# be the one nobody checked.
#
# It lives here rather than under src/ because it is not a layer of the
# simulator. Nothing in src/ depends on it, no simulation links it, and the
# architecture in section 3 of the implementation plan does not have a place for
# it. What it does have is tests, which is why the top-level CMakeLists adds
# this directory before tests/ rather than after.
add_library(
  orrery_benchmark_harness STATIC
  arithmetic_probe_scalar.cpp machine_limits.cpp machine_state.cpp protocol.cpp
  roofline_plot.cpp statistics.cpp)
add_library(orrery::benchmark_harness ALIAS orrery_benchmark_harness)

# The headers are included as "harness/statistics.hpp", so the directory on the
# include path is benchmarks/ rather than this one. That keeps the prefix in
# every include and makes it obvious in a source file that the header came from
# the harness rather than from a layer of the project.
target_include_directories(orrery_benchmark_harness
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")

target_link_libraries(
  orrery_benchmark_harness
  PUBLIC orrery::backend orrery::core
  PRIVATE orrery::warnings)

# The optimisation level is the single most common reason a performance figure
# turns out to be worthless, and nothing inside a translation unit can discover
# it. A generator expression rather than CMAKE_BUILD_TYPE so that a multi-config
# generator, which leaves that variable empty, reports the configuration it
# actually built.
target_compile_definitions(orrery_benchmark_harness
                           PRIVATE "ORRERY_BUILD_TYPE=\"$<CONFIG>\"")

# The arithmetic probe in AVX2, on the same terms as the AVX2 direct kernel and
# for the same reason. See ADR-0018 and src/solvers/CMakeLists.txt.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^([Xx]86_64|AMD64|amd64|i[3-6]86|[Xx]86)$")
  target_sources(orrery_benchmark_harness PRIVATE arithmetic_probe_avx2.cpp)
  target_compile_definitions(orrery_benchmark_harness PRIVATE ORRERY_HAS_AVX2_PROBE)

  # IntelLLVM is excluded from the MSVC spelling for the reason set out at
  # length in src/solvers/CMakeLists.txt: it accepts /arch:AVX2 and then fails to
  # enable the FMA target feature its own intrinsics need. This probe is where
  # that showed up most starkly, since it measures nothing but fused
  # multiply-adds and reported an eighth of the machine's real ceiling.
  if(MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "IntelLLVM")
    set_source_files_properties(arithmetic_probe_avx2.cpp PROPERTIES COMPILE_OPTIONS
                                                              "/arch:AVX2")
  else()
    # FMA by name and, on GCC, the spurious warning about ignored attributes on
    # the intrinsic vector types when they appear as template arguments. Both
    # are explained in src/solvers/CMakeLists.txt, which does the same thing for
    # the direct kernel.
    set(orrery_avx2_options "-mavx2;-mfma")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      list(APPEND orrery_avx2_options "-Wno-ignored-attributes")
    endif()
    set_source_files_properties(arithmetic_probe_avx2.cpp
                                PROPERTIES COMPILE_OPTIONS "${orrery_avx2_options}")
  endif()
endif()

orrery_enable_linting(orrery_benchmark_harness)
