# The lowest layer. Phase 2 fills it with particle storage, vector maths and
# the aligned allocator. It holds the build information for now, which is
# enough to give continuous integration a real library to compile, link and
# test rather than a target that exists only on paper.
add_library(orrery_core STATIC build_info.cpp)
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
