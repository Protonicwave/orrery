# The SYCL toolchain, behind ORRERY_ENABLE_SYCL and off by default.
#
# Every other setting in this project reaches a target through an interface
# library, and SYCL is no exception (ADR-0003). What makes this one different
# from orrery::options and the rest is that it is conditional on a compiler the
# project does not otherwise require: section 5 of the implementation plan asks
# for clean builds with Clang, GCC and MSVC, and none of the three implements
# SYCL. ADR-0025 records why the backend is written in it regardless.
#
# So the target always exists and is empty unless the option is on. A source
# file that links orrery::sycl in a build without it compiles as ordinary C++
# with ORRERY_ENABLE_SYCL undefined, which is how the GPU sources stay out of
# the build without every CMakeLists.txt that mentions them growing a
# conditional.

add_library(orrery_sycl INTERFACE)
add_library(orrery::sycl ALIAS orrery_sycl)

if(NOT ORRERY_ENABLE_SYCL)
  return()
endif()

include(CheckCXXSourceCompiles)
include(CMakePushCheckState)

# IntelLLVM is what the oneAPI DPC++ compiler reports. Other implementations of
# the standard exist, AdaptiveCpp being the one most likely to appear here, and
# they take the same -fsycl spelling, so the check below is on whether the
# compiler can actually build a SYCL translation unit rather than on its name.
# The name is used only to make the diagnostic useful when it cannot.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "IntelLLVM")
  message(
    STATUS
      "ORRERY_ENABLE_SYCL is on with compiler ${CMAKE_CXX_COMPILER_ID}, which is "
      "not the oneAPI DPC++ compiler this project is tested against. Checking "
      "whether it accepts SYCL anyway.")
endif()

# -fsycl has to reach the link line as well as the compile line. It is what
# tells the driver to run the device compiler and to embed the resulting device
# image in the object, and a link performed without it produces undefined
# references to the SYCL runtime's registration symbols rather than a clear
# diagnostic.
set(ORRERY_SYCL_FLAGS -fsycl)

cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_FLAGS "-fsycl")
set(CMAKE_REQUIRED_LINK_OPTIONS "-fsycl")

# The check has to be compiled in the configuration the project will be built
# in, not in whichever one try_compile would otherwise pick. On Windows the two
# differ in the C runtime, and that difference is not cosmetic here: oneAPI
# 2025.1's SYCL headers define _CONTAINER_DEBUG_LEVEL, which the MSVC 14.44
# standard library removed in favour of _MSVC_STL_HARDENING and now rejects with
# a static assertion. A check compiled against the debug runtime therefore fails
# on a machine where the release build works perfectly, which is the least
# useful answer a capability check can give.
set(CMAKE_TRY_COMPILE_CONFIGURATION "${CMAKE_BUILD_TYPE}")
check_cxx_source_compiles(
  "
  #include <sycl/sycl.hpp>
  int main() {
      sycl::queue queue{sycl::default_selector_v};
      return static_cast<int>(queue.get_device().get_info<sycl::info::device::max_compute_units>());
  }
  "
  ORRERY_SYCL_COMPILES)
cmake_pop_check_state()

if(NOT ORRERY_SYCL_COMPILES)
  message(
    FATAL_ERROR
      "ORRERY_ENABLE_SYCL is on but ${CMAKE_CXX_COMPILER} cannot compile a SYCL "
      "translation unit. Run the oneAPI environment script and select the DPC++ "
      "compiler explicitly: on Windows that is icx-cl, the MSVC-style driver, "
      "since CMake drives a Windows IntelLLVM compiler with MSVC-style flags "
      "that icpx rejects; elsewhere it is icpx. Or leave the option off and "
      "build the CPU backends alone, which is a complete build.")
endif()

target_compile_options(orrery_sycl INTERFACE ${ORRERY_SYCL_FLAGS})
target_link_options(orrery_sycl INTERFACE ${ORRERY_SYCL_FLAGS})

# Public in effect, since it guards declarations in public headers exactly as
# the precision switch in core/types.hpp and the AVX2 guard in solvers do. A
# consumer compiled without it would not see the same interface the library was
# built against.
target_compile_definitions(orrery_sycl INTERFACE ORRERY_ENABLE_SYCL)

# Device code is compiled just in time by default: the object carries SPIR-V,
# and the Level Zero driver specialises it for whatever device is present the
# first time a kernel is submitted. The alternative, ahead-of-time compilation
# for a named device, removes that first-submission pause and turns a device
# that cannot run the kernel into a build error rather than a runtime one, at
# the cost of a much slower build and a binary that names one GPU.
#
# Just in time is the default here because the benchmark protocol in Phase 7
# already discards warm-up iterations, which is where the compilation lands, so
# the pause does not reach a reported figure. ORRERY_SYCL_TARGETS is offered for
# anyone who wants the other trade, for example
# -DORRERY_SYCL_TARGETS=spir64_gen with the device passed through -Xs.
set(ORRERY_SYCL_TARGETS
    ""
    CACHE STRING
          "Value for -fsycl-targets, for example spir64_gen. Empty means just-in-time compilation to SPIR-V.")

if(ORRERY_SYCL_TARGETS)
  target_compile_options(orrery_sycl
                         INTERFACE "-fsycl-targets=${ORRERY_SYCL_TARGETS}")
  target_link_options(orrery_sycl INTERFACE
                      "-fsycl-targets=${ORRERY_SYCL_TARGETS}")
endif()

message(STATUS "SYCL backend enabled, compiled by ${CMAKE_CXX_COMPILER_ID}")
