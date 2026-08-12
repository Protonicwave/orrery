#include "orrery/viz/viewer_window.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

#include <GLFW/glfw3.h>

#include "orrery/core/types.hpp"
#include "orrery/viz/camera.hpp"
#include "orrery/viz/gl_api.hpp"

namespace orrery::viz {

namespace {

using core::Real;

/// How far the camera turns for a drag across the whole window, in radians.
///
/// Two and a half turns of azimuth across the width. Enough that the far side of
/// the system is a short movement away and not so much that a small hand
/// movement loses the subject.
constexpr Real kOrbitPerScreen = static_cast<Real>(15.7);

/// How much one notch of a scroll wheel changes the distance.
///
/// A ninth, so nine notches roughly halve or double it. Applied multiplicatively
/// by the camera, so the feel is the same at every scale.
constexpr Real kZoomPerNotch = static_cast<Real>(0.11);

/// The keys in the order of the enumeration, so a lookup is an index.
[[nodiscard]] int glfw_key(Key key) noexcept {
    switch (key) {
    case Key::kQuit:
        return GLFW_KEY_ESCAPE;
    case Key::kPause:
        return GLFW_KEY_SPACE;
    case Key::kBrighter:
        return GLFW_KEY_EQUAL;
    case Key::kDarker:
        return GLFW_KEY_MINUS;
    case Key::kReset:
        return GLFW_KEY_R;
    case Key::kCapture:
        return GLFW_KEY_P;
    }
    return GLFW_KEY_UNKNOWN;
}

constexpr std::size_t kKeyCount = 6;

[[nodiscard]] std::size_t key_index(Key key) noexcept {
    return static_cast<std::size_t>(key);
}

/// Where the scroll wheel's movement is collected between polls.
///
/// A type of its own, rather than the window's whole state, because the window
/// library's callback is a free function and the window's state is private to
/// it. Pointing the library at this one field keeps it that way, and it is the
/// only thing a callback here needs to reach.
struct ScrollAccumulator {
    double amount = 0;
};

void on_scroll(GLFWwindow* window, double /*x_offset*/, double y_offset) {
    // Accumulated rather than acted on here, so that the camera is only ever
    // moved from `poll` and a frame sees one consistent state.
    static_cast<ScrollAccumulator*>(glfwGetWindowUserPointer(window))->amount += y_offset;
}

/// The window library is initialised once and terminated when the last window
/// goes.
///
/// A counter rather than a flag, because a program that opened a second window
/// after closing the first would otherwise find the library shut down. Nothing
/// in this project does that today; the counter costs one integer and removes
/// the question.
int live_windows = 0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

} // namespace

struct ViewerWindow::State {
    GLFWwindow* window = nullptr;

    std::size_t width = 0;
    std::size_t height = 0;

    OrbitCamera camera;

    /// Where the cursor was on the previous poll, and whether that is known.
    double last_cursor_x = 0;
    double last_cursor_y = 0;
    bool dragging = false;
    bool panning = false;

    /// Scroll accumulated by the callback since the last poll.
    ScrollAccumulator scroll;

    std::array<bool, kKeyCount> down{};
    std::array<bool, kKeyCount> went_down{};
};

ViewerWindow::ViewerWindow(const WindowOptions& options) : state_(std::make_unique<State>()) {
    if (live_windows == 0 && glfwInit() != GLFW_TRUE) {
        throw std::runtime_error{"the window system could not be initialised"};
    }
    ++live_windows;

    // OpenGL 3.3 core, which is what the shaders are written against and what
    // every desktop driver of the last decade provides. The forward-compatible
    // flag is required on macOS and harmless elsewhere.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, options.visible ? GLFW_TRUE : GLFW_FALSE);

    // No depth or stencil buffer. The renderer draws with blending and no depth
    // test, so asking for one would reserve memory for the width and height of
    // the window and never read it.
    glfwWindowHint(GLFW_DEPTH_BITS, 0);
    glfwWindowHint(GLFW_STENCIL_BITS, 0);

    // An sRGB default framebuffer, so the hardware applies the display transfer
    // function to what the tone mapping pass writes.
    glfwWindowHint(GLFW_SRGB_CAPABLE, GLFW_TRUE);

    state_->window =
        glfwCreateWindow(static_cast<int>(options.width), static_cast<int>(options.height),
                         options.title.c_str(), nullptr, nullptr);
    if (state_->window == nullptr) {
        --live_windows;
        if (live_windows == 0) {
            glfwTerminate();
        }
        throw std::runtime_error{"no OpenGL 3.3 context could be created on this machine"};
    }

    glfwSetWindowUserPointer(state_->window, &state_->scroll);
    glfwSetScrollCallback(state_->window, on_scroll);
    glfwMakeContextCurrent(state_->window);
    glfwSwapInterval(options.vertical_sync ? 1 : 0);

    int framebuffer_width = 0;
    int framebuffer_height = 0;
    glfwGetFramebufferSize(state_->window, &framebuffer_width, &framebuffer_height);
    state_->width = static_cast<std::size_t>(framebuffer_width);
    state_->height = static_cast<std::size_t>(framebuffer_height);
}

ViewerWindow::~ViewerWindow() {
    glfwDestroyWindow(state_->window);

    --live_windows;
    if (live_windows == 0) {
        glfwTerminate();
    }
}

GlProcedureLoader ViewerWindow::loader() noexcept {
    // glfwGetProcAddress already falls back to the platform's own library for
    // the entry points that predate the extension mechanism, which on Windows is
    // everything in OpenGL 1.1, so the renderer's loader can be this alone.
    return glfwGetProcAddress;
}

std::size_t ViewerWindow::width() const noexcept {
    return state_->width;
}

std::size_t ViewerWindow::height() const noexcept {
    return state_->height;
}

bool ViewerWindow::should_close() const noexcept {
    return glfwWindowShouldClose(state_->window) == GLFW_TRUE;
}

void ViewerWindow::request_close() noexcept {
    glfwSetWindowShouldClose(state_->window, GLFW_TRUE);
}

void ViewerWindow::poll() {
    State& state = *state_;
    state.scroll.amount = 0;
    glfwPollEvents();

    int framebuffer_width = 0;
    int framebuffer_height = 0;
    glfwGetFramebufferSize(state.window, &framebuffer_width, &framebuffer_height);
    state.width = static_cast<std::size_t>(framebuffer_width);
    state.height = static_cast<std::size_t>(framebuffer_height);

    for (std::size_t index = 0; index < kKeyCount; ++index) {
        const bool now = glfwGetKey(state.window, glfw_key(static_cast<Key>(index))) == GLFW_PRESS;
        state.went_down[index] = now && !state.down[index];
        state.down[index] = now;
    }

    double cursor_x = 0;
    double cursor_y = 0;
    glfwGetCursorPos(state.window, &cursor_x, &cursor_y);

    const bool left = glfwGetMouseButton(state.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    const bool right = glfwGetMouseButton(state.window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;

    if ((left || right) && (state.dragging || state.panning)) {
        const auto span = static_cast<Real>(state.width > 0 ? state.width : 1);
        const Real dx = static_cast<Real>(cursor_x - state.last_cursor_x) / span;
        const Real dy = static_cast<Real>(cursor_y - state.last_cursor_y) / span;

        if (left) {
            // Dragging right turns the camera to the left around the subject,
            // which is what makes the subject appear to follow the cursor.
            state.camera.orbit(-dx * kOrbitPerScreen, dy * kOrbitPerScreen);
        } else {
            state.camera.pan(-dx, dy);
        }
    }

    state.dragging = left;
    state.panning = right;
    state.last_cursor_x = cursor_x;
    state.last_cursor_y = cursor_y;

    if (state.scroll.amount != 0) {
        // A positive scroll is towards the subject, so the distance shrinks.
        state.camera.zoom(static_cast<Real>(1) -
                          (static_cast<Real>(state.scroll.amount) * kZoomPerNotch));
    }
}

bool ViewerWindow::pressed(Key key) const noexcept {
    return state_->went_down[key_index(key)];
}

bool ViewerWindow::held(Key key) const noexcept {
    return state_->down[key_index(key)];
}

void ViewerWindow::present() {
    glfwSwapBuffers(state_->window);
}

OrbitCamera& ViewerWindow::camera() noexcept {
    return state_->camera;
}

const OrbitCamera& ViewerWindow::camera() const noexcept {
    return state_->camera;
}

} // namespace orrery::viz
