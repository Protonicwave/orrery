# The Python extension module and the package that holds it.
#
# This directory is not a layer of the simulator. It depends on sim/ and on
# everything beneath it, it is depended on by nothing, and no C++ target links
# it. That is why it sits beside apps/ rather than under src/: a binding is a
# second front door onto the library, and the layering rule in section 3 of the
# implementation plan is about the library rather than about its callers.
#
# The compiled module is named _orrery and the package the user imports is
# orrery. The leading underscore is the convention that says the extension is an
# implementation detail: the package's __init__.py decides what the public
# surface is, and it can add the things that are better written in Python than
# bound from C++ without those being distinguishable from the rest.

pybind11_add_module(
  orrery_python
  MODULE
  src/module.cpp
  src/array_view.cpp
  src/core_bindings.cpp
  src/initial_conditions_bindings.cpp
  src/sim_bindings.cpp)

# The target name and the module name differ because CMake target names are
# global to the build tree and _orrery is too plain a name to claim, while the
# name Python imports is fixed by the package layout and cannot be decorated.
set_target_properties(orrery_python PROPERTIES OUTPUT_NAME _orrery)

target_include_directories(orrery_python PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# orrery::sim brings the layers beneath it with it, since it links them
# publicly. The warning set is private, as everywhere else in the project.
target_link_libraries(
  orrery_python PRIVATE orrery::sim orrery::initial_conditions orrery::core
                        orrery::options orrery::warnings)

orrery_enable_linting(orrery_python)

# In a build tree, the extension is written into a directory named orrery
# holding the package sources, so that adding this directory to PYTHONPATH
# gives a working import without installing anything. A wheel gets the same
# arrangement by a different route: scikit-build-core takes the package sources
# from the source tree and the install rule below places the extension beside
# them. Both end with _orrery inside orrery, which is the only arrangement the
# package's own import statement can read.
set(ORRERY_PYTHON_PACKAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/orrery")

set_target_properties(orrery_python PROPERTIES LIBRARY_OUTPUT_DIRECTORY
                                               "${ORRERY_PYTHON_PACKAGE_DIR}")

# Multi-configuration generators append the configuration name to the output
# directory unless told otherwise for each configuration by name, which would
# put the extension somewhere the package is not.
foreach(config IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${config}" config_upper)
  set_target_properties(
    orrery_python PROPERTIES LIBRARY_OUTPUT_DIRECTORY_${config_upper}
                             "${ORRERY_PYTHON_PACKAGE_DIR}")
endforeach()

set(ORRERY_PYTHON_PACKAGE_SOURCES orrery/__init__.py)

foreach(source IN LISTS ORRERY_PYTHON_PACKAGE_SOURCES)
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${source}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
            "${CMAKE_CURRENT_BINARY_DIR}/${source}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
    COMMENT "Copying ${source} into the build tree's package")
  list(APPEND orrery_python_package_outputs
       "${CMAKE_CURRENT_BINARY_DIR}/${source}")
endforeach()

add_custom_target(orrery_python_package ALL
                  DEPENDS ${orrery_python_package_outputs})
add_dependencies(orrery_python_package orrery_python)

# The wheel. scikit-build-core installs into the wheel root, so a destination of
# orrery puts the extension inside the package it belongs to.
install(TARGETS orrery_python DESTINATION orrery COMPONENT orrery_python)

if(ORRERY_BUILD_TESTS)
  # The bindings are tested from Python, because what they are is a Python
  # interface and a C++ test of them would be a test of the C++ they call. The
  # suite runs under CTest like everything else, so a single ctest invocation is
  # still the whole definition of done rather than most of it.
  #
  # Skipped when the sanitisers are on. An instrumented extension loaded into an
  # uninstrumented interpreter reports the interpreter's own allocations rather
  # than this project's, and the address sanitiser has to be the first library
  # loaded into a process to work at all, which it cannot be when the process is
  # python.
  if(ORRERY_SANITISERS)
    message(
      STATUS
        "Python bindings: the pytest suite is not registered in a sanitiser "
        "build, since the extension would be instrumented and the interpreter "
        "loading it would not.")
  else()
    # NumPy and pytest are run-time requirements of the suite rather than of the
    # module, so the check is here and not beside find_package. A registered
    # test that fails because a test runner is absent reports a missing package
    # as a broken build; an unregistered one with a reason on the configure log
    # reports it as what it is.
    execute_process(
      COMMAND ${Python_EXECUTABLE} -c "import numpy, pytest"
      RESULT_VARIABLE orrery_python_test_requirements
      OUTPUT_QUIET ERROR_QUIET)

    if(orrery_python_test_requirements EQUAL 0)
      add_test(NAME python_bindings
               COMMAND ${Python_EXECUTABLE} -m pytest -q
                       "${CMAKE_CURRENT_SOURCE_DIR}/tests")

      # The build tree's package directory is what the test imports, so the
      # tests exercise the extension that was just built rather than one that
      # happens to be installed in the interpreter's site-packages.
      set_tests_properties(
        python_bindings PROPERTIES ENVIRONMENT
                                   "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR}")
    else()
      message(
        STATUS
          "Python bindings: the pytest suite is not registered, because "
          "${Python_EXECUTABLE} cannot import numpy and pytest. Install them "
          "to run it.")
    endif()
  endif()
endif()
