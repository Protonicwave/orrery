# Dependencies are fetched at configure time and pinned to an exact commit. A
# tag is a moving reference: it can be deleted and recreated on a different
# commit, which would change what the project builds without changing anything
# in this repository. A commit hash cannot move, so a checkout of a given
# Orrery revision builds the same dependency source today and in a year.
# ADR-0002 records the alternatives.
#
# The version beside each hash is the release that hash belongs to. It exists
# so that a reader can tell at a glance how old the pin is, and it is what
# find_package matches against when a system copy is preferred.

include(FetchContent)

if(ORRERY_BUILD_TESTS)
  set(ORRERY_CATCH2_VERSION 3.15.3)
  set(ORRERY_CATCH2_COMMIT 8b08d4d79514f45f7e4ce2a607ac9c94e920d1bb)

  # SYSTEM marks Catch2's headers as system headers, so the project's warning
  # set is not applied to code the project does not own. FIND_PACKAGE_ARGS lets
  # a distribution or a package manager supply Catch2 instead, which is what a
  # packager needs and what an offline build depends on.
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG ${ORRERY_CATCH2_COMMIT}
    GIT_SHALLOW FALSE
    SYSTEM
    FIND_PACKAGE_ARGS ${ORRERY_CATCH2_VERSION} NAMES Catch2)

  FetchContent_MakeAvailable(Catch2)

  # catch_discover_tests lives in the extras directory of a fetched Catch2. A
  # Catch2 found through find_package adds its own module directory instead, so
  # this is guarded rather than unconditional.
  if(catch2_SOURCE_DIR)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
  endif()
endif()
