# One directory per layer of the architecture in section 3 of the
# implementation plan. The remaining layers arrive with the phases that need
# them, and each may depend only on the layers below it.
add_subdirectory(core)
add_subdirectory(backend)
add_subdirectory(initial_conditions)
add_subdirectory(integrators)
add_subdirectory(solvers)
