# The lowest layer: particle storage, vector maths, the aligned allocator and
# the build information. Every layer above depends on this one and it depends on
# nothing, which is what keeps the dependency direction in the implementation
# plan enforceable rather than aspirational.
#
# The parts a kernel calls per particle are header-only, because a 3-vector
# operation or a span accessor that could not be inlined would cost more than it
# does work. The parts whose unit of work is a whole container, the particle
# store and the random source, have translation units instead, which also keeps
# <random> out of every file that includes a header from this layer.
add_library(orrery_core STATIC build_info.cpp particle_data.cpp random.cpp)
add_library(orrery::core ALIAS orrery_core)

target_include_directories(
  orrery_core PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

# The one place where the build system reaches into the source. Macros are
# avoided in this project's C++, but a version known only to CMake has to cross
# the boundary somehow, and a private definition on a single translation unit
# is the narrowest way to do it.
target_compile_definitions(orrery_core
                           PRIVATE ORRERY_VERSION_STRING="${PROJECT_VERSION}")

# Options and sanitisers are public because a consumer compiled without them
# would disagree with this library about the language level, the scalar type
# and the instrumentation. Warnings are private because they are this project's
# standard to meet, not a condition it imposes on anyone who links it.
target_link_libraries(
  orrery_core
  PUBLIC orrery::options orrery::sanitisers
  PRIVATE orrery::warnings)

orrery_enable_linting(orrery_core)
