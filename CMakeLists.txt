# Orrery: a GPU-accelerated N-body gravitational simulator.
#
# Nothing in this project mutates CMAKE_CXX_FLAGS or any other global. Every
# compiler and linker setting reaches a target through one of the interface
# libraries defined in cmake/BuildSettings.cmake, so a dependency built inside
# this tree keeps its own flags and a consumer that adds Orrery as a
# subdirectory inherits only what it asks for. ADR-0003 records why.

cmake_minimum_required(VERSION 3.25)

project(
  Orrery
  VERSION 0.1.0
  DESCRIPTION "GPU-accelerated N-body gravitational simulator"
  HOMEPAGE_URL "https://github.com/Protonicwave/orrery"
  LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(
    FATAL_ERROR
      "In-source builds are not supported. Configure with a preset instead, "
      "for example: cmake --preset debug")
endif()

option(ORRERY_BUILD_TESTS "Build the test suite" ${PROJECT_IS_TOP_LEVEL})
option(ORRERY_SINGLE_PRECISION
       "Use float rather than double as the scalar type" OFF)
option(ORRERY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"
       ${PROJECT_IS_TOP_LEVEL})
option(ORRERY_CLANG_TIDY "Run clang-tidy on this project's targets" OFF)

# Sanitisers are a list rather than a boolean because address and thread cannot
# be combined, and later phases will want thread on its own once the scheduler
# in Phase 6 exists.
set(ORRERY_SANITISERS
    ""
    CACHE STRING
          "Semicolon separated sanitisers to enable, for example address;undefined")

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(BuildSettings)
include(Linting)
include(Dependencies)

add_subdirectory(src)

if(ORRERY_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
