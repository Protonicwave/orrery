# The CUDA toolchain, behind ORRERY_ENABLE_CUDA and off by default.
#
# The same shape as cmake/Sycl.cmake, and for the same reason: every setting
# reaches a target through an interface library rather than through a global
# (ADR-0003), and the target always exists so that a CMakeLists.txt mentioning
# the CUDA sources does not have to grow a conditional. What differs is the
# mechanism underneath, and the difference is worth stating because it decides
# how much of this backend a machine without a device can check.
#
# SYCL is one compiler for both halves: -fsycl makes every translation unit a
# device translation unit as well, which is why the SYCL sources have to be
# compiled by icpx and why the address sanitiser cannot be applied to any of
# them (docs/performance/sycl_direct.md records that limit). CUDA separates the
# two. Only a .cu file needs nvcc; a .cpp file that calls cudaMalloc needs the
# runtime's header and its library and nothing else. So this project keeps the
# device compiler's surface as small as it can be: the kernels are two .cu
# files, everything else about the backend is ordinary C++ compiled by the
# project's own compiler under the project's own warning set and clang-tidy.
#
# ADR-0060 records why the backend exists at all.

add_library(orrery_cuda INTERFACE)
add_library(orrery::cuda ALIAS orrery_cuda)

if(NOT ORRERY_ENABLE_CUDA)
  return()
endif()

# The architectures the kernels are compiled for.
#
# Named rather than discovered, because CMake's `native` asks the machine that
# is configuring the build which device it has, and the two machines that matter
# here have none: continuous integration compiles this backend without a device
# on purpose, and the figures are taken on a hosted notebook that is a different
# machine again.
#
# 75 is Turing, which is what a free-tier T4 is and therefore what
# docs/performance/cuda.md is measured on. A second architecture is one more
# device image in the binary and one more pass of the device compiler, so the
# default carries one and anyone with different hardware passes their own.
set(ORRERY_CUDA_ARCHITECTURES
    "75"
    CACHE STRING
          "Semicolon separated CUDA architectures to compile the kernels for, for example 75;86")

# The runtime API, which the host halves of the backend call directly. This is
# what lets discovery, allocation and the solver bodies stay ordinary C++: they
# include <cuda_runtime.h> and link CUDA::cudart, and nvcc never sees them.
#
# Found before the language is enabled, so that a machine with the header and
# the library but no working device compiler is told which of the two is
# missing.
find_package(CUDAToolkit REQUIRED)

# The device compiler, for the two .cu files and nothing else.
#
# enable_language rather than a project() language, because the project has to
# configure on the ten machines that do not have a toolkit at all. A language
# enabled here is enabled for the whole tree from this point down, which is
# where src/ is added from.
enable_language(CUDA)

target_link_libraries(orrery_cuda INTERFACE CUDA::cudart)

# Public in effect, since it guards declarations in public headers exactly as
# ORRERY_ENABLE_SYCL and the precision switch in core/types.hpp do. A consumer
# compiled without it would not see the same interface the library was built
# against.
target_compile_definitions(orrery_cuda INTERFACE ORRERY_ENABLE_CUDA)

# C++20 on the device side too, because the kernels include this project's
# headers and those are C++20. CUDA 12 is the first release that accepts it,
# which is therefore the floor this option carries.
if(CMAKE_CUDA_COMPILER_VERSION VERSION_LESS 12.0)
  message(
    FATAL_ERROR
      "ORRERY_ENABLE_CUDA needs CUDA 12.0 or later, and ${CMAKE_CUDA_COMPILER} "
      "reports ${CMAKE_CUDA_COMPILER_VERSION}. Earlier releases do not accept "
      "-std=c++20, which the headers the kernels include require. Or leave the "
      "option off and build the CPU backends alone, which is a complete build.")
endif()

message(
  STATUS
    "CUDA backend enabled, ${CMAKE_CUDA_COMPILER_ID} ${CMAKE_CUDA_COMPILER_VERSION} "
    "for architectures ${ORRERY_CUDA_ARCHITECTURES}")
