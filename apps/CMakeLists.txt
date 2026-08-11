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
