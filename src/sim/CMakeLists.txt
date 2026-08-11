# The simulation driver: the layer that owns a run rather than a kernel.
#
# It sits above solvers and below apps, and it is where the pieces the earlier
# phases built in isolation are finally assembled into something that can be
# pointed at a configuration file. Nothing below this layer knows that files
# exist, which is why the trajectory writer and the checkpoint reader are here
# and not in core: a particle store that could serialise itself would put
# <fstream> on the include path of the force kernels.
add_library(orrery_sim STATIC config_file.cpp configuration.cpp)
add_library(orrery::sim ALIAS orrery_sim)

# All four are public. A Simulation holds a solver and an integrator by
# reference to their interfaces, its state is core's particle store, and
# assembling one from a configuration means sampling an initial condition, so a
# consumer of this library is a consumer of every layer beneath it.
target_link_libraries(
  orrery_sim
  PUBLIC orrery::solvers orrery::integrators orrery::initial_conditions
         orrery::backend orrery::core
  PRIVATE orrery::warnings)

orrery_enable_linting(orrery_sim)
