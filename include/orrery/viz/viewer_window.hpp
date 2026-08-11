#pragma once

/// \file
/// A window with an OpenGL context, and the controls that drive the camera.
///
/// This is the only file in the project that knows a window system exists, and
/// it exposes none of it. No GLFW type appears here: the window hands out a
/// function that resolves OpenGL entry points, its own size, a camera the mouse
/// has already moved, and the answer to a short list of questions about keys.
/// Everything else stays in the implementation.
///
/// That boundary is worth the small amount of translation it costs. The renderer
/// beneath it is a class that draws into whatever context is current, and the
/// viewer above it is a loop; neither has any reason to see a `GLFWwindow*`, and
/// once one of them does, the choice of window library has reached into the rest
/// of the program.
///
/// ## Hidden windows
///
/// A window can be created invisible, which is how the offline export gets a
/// context without putting anything on the screen. It is not a headless context:
/// there is a window, the compositor simply never shows it. A genuinely headless
/// context needs a platform-specific path on each of the three platforms, and
/// this project's export is something a person runs on a machine they are
/// sitting at.

#include <cstddef>
#include <memory>
#include <string>

#include "orrery/viz/camera.hpp"
#include "orrery/viz/gl_api.hpp"

namespace orrery::viz {

/// How the window is created.
struct WindowOptions {
    std::size_t width = 1280;
    std::size_t height = 720;
    std::string title = "Orrery";

    /// Whether the window appears on screen.
    ///
    /// False for the offline export, which needs a context and not a picture.
    bool visible = true;

    /// Whether presenting waits for the display's refresh.
    ///
    /// On for the interactive viewer, where drawing faster than the screen
    /// refreshes wastes power to produce frames nobody sees, and off for the
    /// export, where the point is to finish.
    bool vertical_sync = true;
};

/// The keys the viewer asks about.
///
/// An enumeration rather than character codes, so that the mapping to whatever
/// the window library calls them lives in one function in the implementation.
enum class Key : std::uint8_t {
    kQuit,
    kPause,
    kBrighter,
    kDarker,
    kReset,
    kCapture,
};

/// A window, its context, and an orbit camera the mouse drives.
class ViewerWindow {
public:
    /// Create the window and make its context current on this thread.
    ///
    /// Throws `std::runtime_error` if the window system cannot be initialised or
    /// a context of the version the renderer needs cannot be created. The second
    /// is the case worth a clear message: a machine with no OpenGL 3.3 driver is
    /// a machine this renderer cannot run on, and saying so is more use than a
    /// black window.
    explicit ViewerWindow(const WindowOptions& options);

    ~ViewerWindow();

    ViewerWindow(const ViewerWindow&) = delete;
    ViewerWindow& operator=(const ViewerWindow&) = delete;
    ViewerWindow(ViewerWindow&&) = delete;
    ViewerWindow& operator=(ViewerWindow&&) = delete;

    /// How the renderer resolves OpenGL entry points.
    [[nodiscard]] static GlProcedureLoader loader() noexcept;

    /// The size of the drawable surface in pixels.
    ///
    /// Not the size the window was asked for: on a display with a scale factor
    /// the two differ, and it is this one the viewport and the render target
    /// have to match.
    [[nodiscard]] std::size_t width() const noexcept;
    [[nodiscard]] std::size_t height() const noexcept;

    [[nodiscard]] bool should_close() const noexcept;

    /// Ask the window to close, which the loop notices on its next turn.
    void request_close() noexcept;

    /// Handle everything that has happened since the last call.
    ///
    /// Moves the camera for any dragging or scrolling, refreshes the drawable
    /// size, and records which of the keys above were pressed. The key state this
    /// leaves behind is edge triggered: `pressed` answers whether a key went down
    /// during this call, not whether it is down now, so holding one does not
    /// repeat.
    void poll();

    /// Whether `key` went down during the most recent `poll`.
    [[nodiscard]] bool pressed(Key key) const noexcept;

    /// Whether `key` is held down now, for the controls that should repeat.
    [[nodiscard]] bool held(Key key) const noexcept;

    void present();

    [[nodiscard]] OrbitCamera& camera() noexcept;
    [[nodiscard]] const OrbitCamera& camera() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace orrery::viz
