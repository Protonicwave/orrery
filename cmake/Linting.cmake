# clang-tidy is attached to individual targets rather than set through
# CMAKE_CXX_CLANG_TIDY, which would apply it to every target created after the
# variable is set. That includes the dependencies fetched into this tree, whose
# diagnostics the project cannot act on. Opting each target in keeps the lint
# job's output limited to code this repository owns.

set(ORRERY_CLANG_TIDY_EXECUTABLE
    "clang-tidy"
    CACHE STRING
          "Name or path of the clang-tidy binary, for example clang-tidy-22")

# Attach clang-tidy to a target, if the build was configured with
# ORRERY_CLANG_TIDY. Call this once for every target the project defines.
function(orrery_enable_linting target)
  if(NOT ORRERY_CLANG_TIDY)
    return()
  endif()

  find_program(ORRERY_CLANG_TIDY_PROGRAM NAMES "${ORRERY_CLANG_TIDY_EXECUTABLE}")

  if(NOT ORRERY_CLANG_TIDY_PROGRAM)
    message(
      FATAL_ERROR
        "ORRERY_CLANG_TIDY is on but '${ORRERY_CLANG_TIDY_EXECUTABLE}' was not "
        "found. Install clang-tidy or set ORRERY_CLANG_TIDY_EXECUTABLE to its "
        "name on this machine.")
  endif()

  # .clang-tidy at the root of the repository decides which checks run and
  # already treats every diagnostic as an error, so nothing is repeated here.
  set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY
                                             "${ORRERY_CLANG_TIDY_PROGRAM}")
endfunction()
