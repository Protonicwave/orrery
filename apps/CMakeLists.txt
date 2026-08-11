# The command-line simulator.
#
# The top layer of the architecture in section 3 of the implementation plan, and
# the only one that is a program rather than a library. It links orrery::sim and
# nothing else directly: everything below the driver reaches it transitively,
# which is the check that the layering holds. A day when this file has to name
# orrery::solvers is a day the application has started deciding something the
# driver should have.
add_executable(orrery orrery.cpp)

target_link_libraries(orrery PRIVATE orrery::sim orrery::warnings)

orrery_enable_linting(orrery)

# The viewer, which exists only in a build with the renderer. It is a second
# program rather than a subcommand of the first for the same reason the renderer
# is a separate option: a machine with no display should still get a simulator,
# and a simulator that fails to link because there is no window library would
# not be one.
#
# This is the one application that names two libraries. That is not a breach of
# the layering: viz and sim are siblings, neither depends on the other, and
# putting a simulation and a picture of one together is exactly the job of the
# layer above them both.
if(ORRERY_ENABLE_RENDERER)
  add_executable(orrery_view orrery_view.cpp)
  set_target_properties(orrery_view PROPERTIES OUTPUT_NAME orrery-view)

  target_link_libraries(orrery_view PRIVATE orrery::sim orrery::viz
                                            orrery::warnings)

  orrery_enable_linting(orrery_view)
endif()
