# The three interface libraries through which every build setting reaches a
# target. Their names say what a target is asking for:
#
#   orrery::options     the language level and the correctness fixes a build
#                       needs to behave the same on all three compilers
#   orrery::warnings    the diagnostic set, kept private to this project
#   orrery::sanitisers  instrumentation, which has to reach both compile and
#                       link lines
#
# The alternative, setting CMAKE_CXX_FLAGS at the top of the tree, would apply
# the project's warning set and sanitiser flags to every dependency built in
# the same tree as well, including Catch2. That produces failures in code the
# project does not own and cannot fix. ADR-0003.

add_library(orrery_options INTERFACE)
add_library(orrery::options ALIAS orrery_options)

# C++20 is a requirement of the project rather than a preference of a
# particular target, so it propagates to anything that links the library.
target_compile_features(orrery_options INTERFACE cxx_std_20)

if(ORRERY_SINGLE_PRECISION)
  # The precision switch is public: a consumer compiled against a different
  # setting than the library would disagree about the size of every scalar in
  # every interface. Phase 2 defines the Real alias this selects.
  target_compile_definitions(orrery_options INTERFACE ORRERY_SINGLE_PRECISION)
endif()

if(MSVC)
  # These four are corrections rather than preferences. Without them MSVC
  # reports C++98 in __cplusplus, reads sources in the system code page rather
  # than UTF-8, ships a non-conforming preprocessor, and accepts constructs the
  # other two compilers reject.
  target_compile_options(orrery_options INTERFACE /permissive- /Zc:__cplusplus
                                                  /Zc:preprocessor /utf-8)
endif()

add_library(orrery_warnings INTERFACE)
add_library(orrery::warnings ALIAS orrery_warnings)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(
    orrery_warnings
    INTERFACE
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wnon-virtual-dtor
      -Wold-style-cast
      -Wcast-align
      -Woverloaded-virtual
      -Wformat=2
      -Wimplicit-fallthrough
      -Wnull-dereference
      # Silent narrowing is a correctness problem in numerical code rather than
      # a style one, so conversions are diagnosed rather than tolerated.
      -Wconversion
      -Wsign-conversion
      # A float expression promoted to double is the specific bug the
      # single-precision build exists to avoid: it doubles the bandwidth the
      # kernel consumes while looking correct.
      -Wdouble-promotion)

  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(orrery_warnings INTERFACE -Wduplicated-cond
                                                     -Wduplicated-branches
                                                     -Wlogical-op)
  endif()

  if(ORRERY_WARNINGS_AS_ERRORS)
    target_compile_options(orrery_warnings INTERFACE -Werror)
  endif()
elseif(MSVC)
  target_compile_options(orrery_warnings INTERFACE /W4)

  if(ORRERY_WARNINGS_AS_ERRORS)
    target_compile_options(orrery_warnings INTERFACE /WX)
  endif()
else()
  message(
    WARNING
      "No warning set is defined for compiler ${CMAKE_CXX_COMPILER_ID}. "
      "The build will succeed with that compiler's defaults, which is weaker "
      "than the project requires.")
endif()

add_library(orrery_sanitisers INTERFACE)
add_library(orrery::sanitisers ALIAS orrery_sanitisers)

if(ORRERY_SANITISERS)
  if(MSVC)
    # MSVC has address but neither undefined nor thread, so a request it cannot
    # satisfy is an error rather than a silently weaker build.
    if(NOT ORRERY_SANITISERS STREQUAL "address")
      message(
        FATAL_ERROR
          "MSVC supports only the address sanitiser, but ORRERY_SANITISERS is "
          "'${ORRERY_SANITISERS}'. Use the sanitise preset with Clang or GCC.")
    endif()
    target_compile_options(orrery_sanitisers INTERFACE /fsanitize=address)
  else()
    list(JOIN ORRERY_SANITISERS "," orrery_sanitiser_flag)
    # Frame pointers are kept so that a report names the function that caused
    # it, and recovery is disabled so that an undefined-behaviour finding stops
    # the test run rather than printing and continuing to a green result.
    target_compile_options(
      orrery_sanitisers INTERFACE -fsanitize=${orrery_sanitiser_flag}
                                  -fno-omit-frame-pointer
                                  -fno-sanitize-recover=all)
    target_link_options(orrery_sanitisers INTERFACE
                        -fsanitize=${orrery_sanitiser_flag})
  endif()
endif()
