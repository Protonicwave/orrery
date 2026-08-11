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
  # These are corrections rather than preferences. Without them MSVC reports
  # C++98 in __cplusplus, reads sources in the system code page rather than
  # UTF-8, and accepts constructs the other two compilers reject.
  target_compile_options(orrery_options INTERFACE /permissive- /Zc:__cplusplus
                                                  /utf-8)

  # The fourth correction, a conforming preprocessor, is needed by MSVC alone.
  # The oneAPI compiler of Phase 9 presents an MSVC-like command line, so it
  # reaches this branch, but its preprocessor is Clang's and already conforms.
  # It reports the flag as unused, and this project promotes that remark to an
  # error, so asking for something already true would fail every translation
  # unit in the build.
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "IntelLLVM")
    target_compile_options(orrery_options INTERFACE /Zc:preprocessor)
  endif()

  # Let the optimiser inline. CMake's MSVC-style RelWithDebInfo is /O2 /Ob1, and
  # /Ob1 inlines only what is marked inline, which is not what /O2 means on any
  # other compiler this project builds with.
  #
  # The cost is not the few per cent that flag usually suggests. The AVX2 kernel
  # is built from small helper functions wrapping the intrinsics, exactly as
  # ADR-0018 describes, and under /Ob1 each becomes a call. Measured on this
  # machine at 8192 particles in single precision, the vector kernel went from
  # 18.5 ms to 222 ms, which is thirteen times slower and slower than the scalar
  # kernel it exists to replace. The harness's own FMA ceiling probe measured 47
  # Gflop/s against the machine's real 571.
  #
  # Nothing warned. The build succeeded, the CPUID check passed, the solver
  # reported that it was running the vector kernel, and every figure taken that
  # way was wrong by an order of magnitude. It was found in Phase 9 only because
  # a GPU speedup came out implausibly large and the CPU baseline was checked
  # against Phase 7's published figures.
  #
  # Applied to every MSVC-style compiler rather than to the oneAPI one alone,
  # because MSVC proper has the same default and the same helpers. Set as a
  # target option rather than by editing CMAKE_CXX_FLAGS_RELWITHDEBINFO, so it
  # reaches this project's targets and not the dependencies built beside them
  # (ADR-0003). It comes after the /Ob1 CMake supplies, and the last of the two
  # wins.
  target_compile_options(orrery_options
                         INTERFACE $<$<CONFIG:RelWithDebInfo>:/Ob2>)
endif()

# The oneAPI compiler is the one compiler here that is not IEEE by default.
#
# Clang, GCC and MSVC all keep strict floating-point semantics unless asked
# otherwise, and ADR-0020 records this project's decision to leave them that
# way: no fast-math flag anywhere, because reassociation applied silently to
# every loop is not a trade-off anybody measured. icx and icpx default to
# -fp-model=fast, which is precisely that relaxation, so the setting has to be
# turned back off rather than merely not turned on.
#
# It is not a theoretical concern. Configured without this, the compensated
# summation in core/diagnostics.cpp and solvers/reference_kernel.cpp is
# algebraically a no-op, and the compiler is entitled to delete the compensation
# term. Eight tests fail under the default, among them the two that exist to
# assert that a compensated total keeps digits an ordinary one loses, and the
# reference the whole project measures its approximations against stops being a
# reference. The phase that introduced this compiler is the phase that has to
# say so.
if(CMAKE_CXX_COMPILER_ID STREQUAL "IntelLLVM")
  if(MSVC)
    target_compile_options(orrery_options INTERFACE /fp:precise)
  else()
    target_compile_options(orrery_options INTERFACE -fp-model=precise)
  endif()
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
  # C4324 reports that a structure was padded to satisfy an alignas, which is
  # what alignas is for. The project uses it to give a per-thread counter and a
  # per-thread work range a cache line each, so that two cores updating their
  # own do not contend for the same line. The padding the warning objects to is
  # the entire mechanism, and there is no way to ask for the alignment without
  # it. GCC and Clang do not warn here at all.
  target_compile_options(orrery_warnings INTERFACE /W4 /wd4324)

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
