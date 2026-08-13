# The WebAssembly module: the same solver, reached from a browser.
#
# This directory is not a layer of the simulator, for the same reason python/ is
# not. It depends on sim/ and on everything beneath it, nothing depends on it,
# and no C++ target links it. It is a second front door, and the layering rule
# is about the library rather than about its callers.
#
# What it produces is one JavaScript file and one .wasm file, written straight
# into the browser client's static assets so that the client's build has no step
# of its own to fetch them. Both are generated and neither is committed, exactly
# as the published gallery is.

add_executable(orrery_wasm orrery_wasm.cpp)

# The target name is decorated because CMake target names are global to the
# build tree; the file name is not, because it is what the client imports.
set_target_properties(orrery_wasm PROPERTIES OUTPUT_NAME orrery)

# orrery::sim brings the layers beneath it with it. The warning set is private,
# as everywhere else in the project.
target_link_libraries(orrery_wasm PRIVATE orrery::sim orrery::core orrery::options
                                          orrery::warnings)

# Every name the client calls, and nothing else. Emscripten removes anything not
# reachable from this list, which is most of what a C++ standard library
# contains, and it is the single largest thing keeping the module inside its
# size budget.
#
# malloc and free are on the list because the client owns the buffers the module
# copies positions and diagnostics into: the alternative, a buffer the module
# owns and hands out a pointer to, would be a pointer into a heap that can move
# under memory growth.
set(orrery_wasm_exports
    _orrery_abi_version
    _orrery_scalar_size
    _orrery_particle_limit
    _orrery_step_limit
    _orrery_last_error
    _orrery_create
    _orrery_destroy
    _orrery_report
    _orrery_solver_name
    _orrery_kernel_name
    _orrery_particle_count
    _orrery_step_index
    _orrery_timestep
    _orrery_time
    _orrery_advance
    _orrery_read_positions
    _orrery_read_masses
    _orrery_measure
    _malloc
    _free)
list(JOIN orrery_wasm_exports "," orrery_wasm_export_list)

target_link_options(
  orrery_wasm
  PRIVATE
  # An ES module with a factory function, because the client is an ES module and
  # the module is instantiated inside a Worker rather than on the page.
  "-sMODULARIZE=1"
  "-sEXPORT_ES6=1"
  "-sEXPORT_NAME=createOrrery"
  # A Worker, which is the only place the client instantiates it, and Node,
  # which is where the test that checks its answers against the native build
  # runs. Dropping the page and shell cases removes their detection and their
  # loaders from the JavaScript file. Testing the artefact the site serves
  # rather than a second build of it is worth the few hundred bytes Node costs.
  "-sENVIRONMENT=worker,node"
  "-sEXPORTED_FUNCTIONS=[${orrery_wasm_export_list}]"
  # The three views and the two string helpers the client reads results
  # through. Everything else Emscripten could attach to the module object is
  # left off it.
  "-sEXPORTED_RUNTIME_METHODS=[HEAPF32,HEAPF64,UTF8ToString,stringToUTF8,lengthBytesUTF8]"
  # The heap grows because the particle count is not known until a configuration
  # arrives, and a module sized for the largest run it permits would ask the
  # browser for that memory even to run two bodies.
  "-sALLOW_MEMORY_GROWTH=1"
  # Nothing here reads or writes a file. The library's file I/O is not linked,
  # and saying so removes Emscripten's filesystem shim as well.
  "-sFILESYSTEM=0"
  # A failure inside the module is a defect in this project rather than
  # something a person in a browser can act on, so the module aborts and the
  # Worker reports that it stopped, instead of carrying Emscripten's assertion
  # text into the download.
  "-sASSERTIONS=0"
  # Optimised for size as well as speed at the link step, where the choice is
  # about what to keep rather than about how to compile it.
  "-O3")

# The client's static assets. Written here rather than copied by a script in
# web/, so that the file the site serves is the file this build produced and
# there is no second place for a stale one to live.
set(ORRERY_WASM_OUTPUT_DIR "${PROJECT_SOURCE_DIR}/web/public/solver")

set_target_properties(orrery_wasm PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                             "${ORRERY_WASM_OUTPUT_DIR}")

foreach(config IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${config}" config_upper)
  set_target_properties(
    orrery_wasm PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config_upper}
                           "${ORRERY_WASM_OUTPUT_DIR}")
endforeach()

# The module's size budget is asserted by web/tools/budget.ts alongside the
# client's own, rather than here. It is a gzipped figure, which is what a
# browser downloads and what CMake has no way to measure, and a second check
# with a second number would be a second budget.
