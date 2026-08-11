# The visualisation layer: what a configuration of point masses looks like.
#
# It sits beside the driver rather than under it. It depends on core, because a
# renderer draws positions, and on nothing else: it does not know what a solver
# is, it cannot read a trajectory file, and it has no opinion about what the
# particles are doing. The viewer application above is what puts a simulation
# and a picture of one together, which is the same division the command-line
# simulator already follows.
#
# The library is built in every configuration and grows three translation units
# when the renderer is switched on. That split is the point of the layer's
# design: the camera, the projection and the tone mapping are arithmetic, they
# are tested like any other arithmetic in this project, and they are where a
# sign error hides. Only the three files that call OpenGL need a device.
set(ORRERY_VIZ_SOURCES camera.cpp image.cpp matrix4.cpp tone_map.cpp)

if(ORRERY_ENABLE_RENDERER)
  list(APPEND ORRERY_VIZ_SOURCES gl_api.cpp point_renderer.cpp viewer_window.cpp)
endif()

add_library(orrery_viz STATIC ${ORRERY_VIZ_SOURCES})
add_library(orrery::viz ALIAS orrery_viz)

target_link_libraries(
  orrery_viz
  PUBLIC orrery::core
  PRIVATE orrery::warnings)

if(ORRERY_ENABLE_RENDERER)
  # Private, and it can be: no header in this layer mentions a GLFW type, and
  # viewer_window.hpp exists to keep it that way. A consumer links the viewer
  # without acquiring the window library's headers or its include path.
  target_link_libraries(orrery_viz PRIVATE glfw)

  # Public, because whether the renderer exists is a question the application
  # above has to be able to ask, and it asks it with #ifdef. The alternative, a
  # run-time function returning false, cannot be right here: the class it would
  # be asked about does not exist to be named in a build without this.
  target_compile_definitions(orrery_viz PUBLIC ORRERY_ENABLE_RENDERER)
endif()

orrery_enable_linting(orrery_viz)
