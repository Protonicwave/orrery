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
  VERSION 1.1.0
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
option(ORRERY_BUILD_BENCHMARKS "Build the measurement programs"
       ${PROJECT_IS_TOP_LEVEL})
option(ORRERY_SINGLE_PRECISION
       "Use float rather than double as the scalar type" OFF)
option(ORRERY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"
       ${PROJECT_IS_TOP_LEVEL})
option(ORRERY_CLANG_TIDY "Run clang-tidy on this project's targets" OFF)

# Off by default because it requires a compiler none of the three the project is
# tested with provides. A build without it is complete rather than degraded: the
# GPU backend is one implementation of an interface that already has others, and
# everything above it selects between them at run time. ADR-0025.
option(ORRERY_ENABLE_SYCL "Build the SYCL backend for the integrated GPU" OFF)

# Off by default for a related but not identical reason. It needs the CUDA
# toolkit, which no machine this project is developed or tested on carries, and
# it produces a second implementation of two solvers that already exist. ADR-0060
# records why a second vendor's backend is worth writing anyway: it is the only
# way to find out whether the solver interface of ADR-0026 describes gravity on a
# device or describes one device.
option(ORRERY_ENABLE_CUDA "Build the CUDA backend for an NVIDIA GPU" OFF)

# Off by default for the same reason, and a different one. It fetches GLFW, and
# it needs a machine with a display and an OpenGL 3.3 driver, which a build
# server generally is not. A build without it still produces the whole
# simulator; what it does not produce is the viewer. The parts of the viz layer
# that are arithmetic rather than graphics, the camera and the tone mapping, are
# built and tested either way, which is deliberate: they are where the mistakes
# that show up as a black window live.
option(ORRERY_ENABLE_RENDERER "Build the real-time renderer and the viewer" OFF)

# Off by default for a third reason again: it needs a Python interpreter with
# its development headers, and it produces an extension module rather than
# anything the C++ side of the project links. It is switched on by the python
# preset, by continuous integration, and by the wheel build, which configures
# this project through scikit-build-core and needs nothing else from it.
option(ORRERY_BUILD_PYTHON "Build the Python extension module" OFF)

# The WebAssembly module, which is the same library reached through the C
# interface in wasm/ rather than a second implementation of it. Off by default
# because it needs the Emscripten toolchain, and meaningless without it: the
# wasm preset selects that toolchain and turns this on together.
option(ORRERY_BUILD_WASM "Build the WebAssembly module" OFF)

if(ORRERY_BUILD_WASM AND NOT EMSCRIPTEN)
  message(
    FATAL_ERROR
      "ORRERY_BUILD_WASM needs the Emscripten toolchain. Configure with "
      "--preset wasm, having run emsdk_env first.")
endif()

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
include(Sycl)
include(Cuda)
include(Dependencies)

# Before any directory that registers a test. add_test only records a test in a
# directory added after testing was enabled, so a call further down would leave
# the bindings' suite silently unregistered: it would build, it would pass when
# run by hand, and ctest would report that it did not exist.
if(ORRERY_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(src)

# The command-line programs are native tools: they read files, write files and
# take arguments from a shell. A WebAssembly build has none of those and
# produces one module instead, so apps/ is not added there.
if(NOT ORRERY_BUILD_WASM)
  add_subdirectory(apps)
endif()

# The WebAssembly module is a consumer of the library rather than a layer of it,
# so it sits beside apps/ and python/ for the reason each of those gives.
if(ORRERY_BUILD_WASM)
  add_subdirectory(wasm)
endif()

# The bindings are a consumer of the library rather than a layer of it, so they
# sit beside apps/ and are added after the layers they bind.
if(ORRERY_BUILD_PYTHON)
  add_subdirectory(python)
endif()

# Benchmarks before tests, which is the one place in this file where the order
# matters. Phase 7's measurement harness is a library under benchmarks/, and
# section 5 of the implementation plan requires every new component to be
# covered by tests, so tests/ has to be able to see the target. The tests that
# need it are guarded on its existence rather than on the option, so that a
# build configured without benchmarks still produces a complete test suite for
# everything it did build.
if(ORRERY_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(ORRERY_BUILD_TESTS)
  add_subdirectory(tests)
endif()
