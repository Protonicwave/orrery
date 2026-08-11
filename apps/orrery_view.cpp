/// \file
/// The viewer: a simulation, or a recording of one, as a picture.
///
/// Two ways in. `run` takes a configuration file, assembles the same simulation
/// `orrery run` would, and draws it as it advances, which is what makes the
/// galaxy collision something a person can watch rather than a file. `play`
/// takes a trajectory written earlier and shows the frames in it, which is what
/// a run too large to integrate at interactive speed is for.
///
/// Either can be exported instead of shown. With `--export`, the window is
/// created but never made visible, each frame is rendered, read back and written
/// as a PPM, and `docs/visualisation.md` gives the one command that turns the
/// directory into a video. Nothing else about the two paths differs: the same
/// camera, the same renderer, the same tone mapping, so what is exported is what
/// would have been on screen.
///
/// This file is thin on purpose, in the same way `orrery.cpp` is. Assembling a
/// run is `sim/assembly.hpp`, drawing is `viz/point_renderer.hpp`, and what is
/// left here is arguments, a loop and the report at the end.

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <locale>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "orrery/core/build_info.hpp"
#include "orrery/core/diagnostics.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/sim/assembly.hpp"
#include "orrery/sim/config_file.hpp"
#include "orrery/sim/configuration.hpp"
#include "orrery/sim/simulation.hpp"
#include "orrery/sim/trajectory.hpp"
#include "orrery/viz/camera.hpp"
#include "orrery/viz/image.hpp"
#include "orrery/viz/point_renderer.hpp"
#include "orrery/viz/tone_map.hpp"
#include "orrery/viz/viewer_window.hpp"

namespace {

using orrery::core::Index;
using orrery::core::Real;
using orrery::core::squared_norm;
using orrery::core::Vec3;
using orrery::core::Vec3Span;
using orrery::sim::Configuration;
using orrery::viz::Image;
using orrery::viz::Key;
using orrery::viz::OrbitCamera;
using orrery::viz::PointRenderer;
using orrery::viz::RenderSettings;
using orrery::viz::tone_map_into;
using orrery::viz::ViewerWindow;
using orrery::viz::WindowOptions;

constexpr int kSuccess = 0;
constexpr int kFailure = 1;

/// Everything the command line can set.
struct Options {
    std::size_t width = 1280;
    std::size_t height = 720;

    RenderSettings render;
    OrbitCamera camera;

    /// How many steps the simulation takes between frames, for `run`.
    ///
    /// One by default, which shows every step. A configuration with a timestep
    /// short enough for the physics is usually far shorter than a frame needs,
    /// so a demonstration raises this until the motion is at a watchable speed.
    Index steps_per_frame = 1;

    /// Stop after this many frames. Zero runs until the run ends or the window
    /// is closed.
    Index frames = 0;

    /// Where exported frames go. Empty shows a window instead.
    std::string export_directory;

    /// Radians of azimuth added to the camera between frames.
    ///
    /// For a turntable, which is the honest way to show the three-dimensional
    /// shape of a merger remnant in a video nobody can rotate themselves.
    Real spin = 0;

    std::vector<std::string> settings;
};

void print_usage(std::ostream& out) {
    out << "orrery-view " << orrery::core::version() << ", the Orrery viewer\n\n"
        << "Usage:\n"
        << "  orrery-view run <configuration> [options] [--set key=value]...\n"
        << "      Integrate the configuration and draw it as it goes.\n\n"
        << "  orrery-view play <trajectory> [options]\n"
        << "      Show the frames of a trajectory written by an earlier run.\n\n"
        << "Options:\n"
        << "  --width N, --height N      The frame size in pixels. 1280 by 720.\n"
        << "  --export <directory>       Write PPM frames there instead of\n"
        << "                             opening a window.\n"
        << "  --frames N                 Stop after N frames.\n"
        << "  --steps-per-frame N        Steps between frames, for run.\n"
        << "  --exposure X               Brightness of the tone mapping. 1.\n"
        << "  --white-point X            The radiance that maps to white. 4.\n"
        << "  --brightness X             Light per particle. 1.\n"
        << "  --point-size X             Sprite diameter one unit away. 40.\n"
        << "  --distance X               Camera distance. Framed automatically\n"
        << "                             when not given.\n"
        << "  --azimuth X, --elevation X Camera angles, in radians.\n"
        << "  --spin X                   Radians of azimuth added per frame.\n\n"
        << "While a window is open: drag to turn, right-drag to pan, scroll to\n"
        << "zoom, - and = to change the exposure, space to pause, r to reframe,\n"
        << "p to write a frame to the current directory, escape to leave.\n";
}

/// Read one option's value, or say what was expected.
class ArgumentReader {
public:
    ArgumentReader(std::span<const std::string_view> arguments, std::ostream& error)
        : arguments_(arguments), error_(&error) {}

    /// The next argument, or empty with `ok` cleared.
    [[nodiscard]] std::string_view value(std::string_view option) {
        if (index_ + 1 >= arguments_.size()) {
            *error_ << "orrery-view: " << option << " needs a value\n";
            ok_ = false;
            return {};
        }
        return arguments_[++index_];
    }

    [[nodiscard]] Real number(std::string_view option) {
        const std::string_view text = value(option);
        if (!ok_) {
            return 0;
        }

        // The same istringstream in the classic locale the configuration parser
        // uses, and for the same reason: a command line should mean the same
        // thing on a machine whose locale writes a comma for a decimal point.
        std::istringstream stream{std::string{text}};
        stream.imbue(std::locale::classic());

        Real parsed = 0;
        stream >> parsed;
        if (stream.fail() || !(stream >> std::ws).eof()) {
            *error_ << "orrery-view: " << option << " expected a number, found '" << text << "'\n";
            ok_ = false;
        }
        return parsed;
    }

    [[nodiscard]] Index count(std::string_view option) {
        const Real parsed = number(option);
        if (ok_ && !(parsed >= 0)) {
            *error_ << "orrery-view: " << option << " expected a count that is not negative\n";
            ok_ = false;
        }
        return static_cast<Index>(parsed);
    }

    [[nodiscard]] bool ok() const noexcept { return ok_; }

    void advance() noexcept { ++index_; }

    [[nodiscard]] bool done() const noexcept { return index_ >= arguments_.size(); }

    [[nodiscard]] std::string_view current() const { return arguments_[index_]; }

private:
    std::span<const std::string_view> arguments_;
    std::ostream* error_;
    std::size_t index_ = 0;
    bool ok_ = true;
};

/// Parse the options that follow the file, leaving `distance_given` set if the
/// camera was placed explicitly.
[[nodiscard]] Options read_options(std::span<const std::string_view> arguments, std::ostream& error,
                                   bool& ok, bool& distance_given) {
    Options options;
    ArgumentReader reader(arguments, error);
    distance_given = false;

    for (; !reader.done(); reader.advance()) {
        const std::string_view argument = reader.current();

        if (argument == "--width") {
            options.width = reader.count(argument);
        } else if (argument == "--height") {
            options.height = reader.count(argument);
        } else if (argument == "--export") {
            options.export_directory = reader.value(argument);
        } else if (argument == "--frames") {
            options.frames = reader.count(argument);
        } else if (argument == "--steps-per-frame") {
            options.steps_per_frame = reader.count(argument);
        } else if (argument == "--exposure") {
            options.render.tone_mapping.exposure = static_cast<float>(reader.number(argument));
        } else if (argument == "--white-point") {
            options.render.tone_mapping.white_point = static_cast<float>(reader.number(argument));
        } else if (argument == "--brightness") {
            options.render.brightness = static_cast<float>(reader.number(argument));
        } else if (argument == "--point-size") {
            options.render.point_size = static_cast<float>(reader.number(argument));
        } else if (argument == "--distance") {
            options.camera.distance = reader.number(argument);
            distance_given = true;
        } else if (argument == "--azimuth") {
            options.camera.azimuth = reader.number(argument);
        } else if (argument == "--elevation") {
            options.camera.elevation = reader.number(argument);
        } else if (argument == "--spin") {
            options.spin = reader.number(argument);
        } else if (argument == "--set") {
            options.settings.emplace_back(reader.value(argument));
        } else {
            error << "orrery-view: unexpected argument '" << argument << "'\n";
            ok = false;
            return options;
        }

        if (!reader.ok()) {
            ok = false;
            return options;
        }
    }

    if (options.width == 0 || options.height == 0) {
        error << "orrery-view: a frame of no pixels cannot be rendered\n";
        ok = false;
    }
    if (options.steps_per_frame == 0) {
        error << "orrery-view: --steps-per-frame must be at least one\n";
        ok = false;
    }
    return options;
}

/// Point the camera at the particles and stand back far enough to see them.
///
/// The distance is three times the root-mean-square radius about the centre of
/// mass. That is a compromise rather than a bound: a bounding sphere would be
/// set by the one particle flung furthest by the encounter and would show a
/// galaxy the size of a full stop, and the root-mean-square radius is dominated
/// by where the mass actually is.
void frame_particles(OrbitCamera& camera, Vec3Span<const Real> positions) {
    if (positions.empty()) {
        return;
    }

    Vec3 centre;
    for (Index particle = 0; particle < positions.size(); ++particle) {
        centre += positions.get(particle);
    }
    centre /= static_cast<Real>(positions.size());

    Real squared = 0;
    for (Index particle = 0; particle < positions.size(); ++particle) {
        squared += squared_norm(positions.get(particle) - centre);
    }

    camera.target = centre;
    camera.distance = 3 * std::sqrt(squared / static_cast<Real>(positions.size()));
}

/// One value per particle, saying which galaxy of a collision it came from.
///
/// Empty when the configuration has only one group, which draws everything in
/// the cool colour.
[[nodiscard]] std::vector<float> tints_for(const Configuration& configuration, Index count) {
    const Index primary = orrery::sim::primary_galaxy_count(configuration);
    if (primary == 0 || primary >= count) {
        return {};
    }

    std::vector<float> tints(count, 0.0F);
    for (Index particle = primary; particle < count; ++particle) {
        tints[particle] = 1.0F;
    }
    return tints;
}

/// Where a frame of an export goes.
[[nodiscard]] std::filesystem::path frame_path(const std::string& directory, Index frame) {
    std::ostringstream name;
    // Five digits, zero filled, so that the files sort in the order they were
    // written by every tool that sorts by name, which is what lets an encoder be
    // pointed at the pattern rather than at a list.
    name << "frame_" << std::setfill('0') << std::setw(5) << frame << ".ppm";
    return std::filesystem::path{directory} / name.str();
}

/// The loop both commands share.
///
/// `advance` produces the next set of positions, or nothing when there are no
/// more. Everything about windows, exports, timing and the report is here; what
/// differs between playing a file and integrating a configuration is the one
/// function passed in.
class Viewer {
public:
    Viewer(const Options& options, std::span<const float> tints, std::ostream& out)
        : options_(options),
          tints_(tints),
          out_(&out),
          window_({.width = options.width,
                   .height = options.height,
                   .title = "Orrery",
                   .visible = options.export_directory.empty(),
                   // An export should finish as fast as the machine allows. An
                   // interactive window should not draw frames the display will
                   // never show.
                   .vertical_sync = options.export_directory.empty()}),
          renderer_(ViewerWindow::loader(), window_.width(), window_.height()),
          render_(options.render) {
        window_.camera() = options.camera;

        if (!options.export_directory.empty()) {
            std::filesystem::create_directories(options.export_directory);
            radiance_.resize(renderer_.radiance_size());
            image_ = Image(renderer_.width(), renderer_.height());
        }

        *out_ << "renderer:    " << renderer_.device_description() << '\n'
              << "frame:       " << renderer_.width() << " by " << renderer_.height() << '\n';
    }

    /// Draw one set of positions. Returns false when the viewer should stop.
    [[nodiscard]] bool show(Vec3Span<const Real> positions) {
        window_.poll();
        if (window_.should_close() || window_.pressed(Key::kQuit)) {
            return false;
        }

        handle_keys(positions);

        renderer_.resize(window_.width(), window_.height());
        const Real aspect = static_cast<Real>(renderer_.width()) /
                            static_cast<Real>(renderer_.height() > 0 ? renderer_.height() : 1);

        window_.camera().azimuth += options_.spin;
        renderer_.accumulate(positions, tints_, window_.camera().view_projection(aspect), render_);

        if (options_.export_directory.empty()) {
            renderer_.present(render_);
            window_.present();
        } else {
            write_frame();
        }

        ++frames_;
        return options_.frames == 0 || frames_ < options_.frames;
    }

    /// Whether the run is paused, which stops the simulation advancing while
    /// leaving the camera live.
    [[nodiscard]] bool paused() const noexcept { return paused_; }

    [[nodiscard]] Index frames() const noexcept { return frames_; }

private:
    void handle_keys(Vec3Span<const Real> positions) {
        if (window_.pressed(Key::kPause)) {
            paused_ = !paused_;
        }
        if (window_.pressed(Key::kReset)) {
            frame_particles(window_.camera(), positions);
        }

        // Held rather than pressed, and multiplicative, so that holding the key
        // sweeps the exposure smoothly rather than stepping it once per press.
        if (window_.held(Key::kBrighter)) {
            render_.tone_mapping.exposure *= 1.03F;
        }
        if (window_.held(Key::kDarker)) {
            render_.tone_mapping.exposure /= 1.03F;
        }

        if (window_.pressed(Key::kCapture)) {
            capture();
        }
    }

    void write_frame() {
        radiance_.resize(renderer_.radiance_size());
        if (image_.width() != renderer_.width() || image_.height() != renderer_.height()) {
            image_ = Image(renderer_.width(), renderer_.height());
        }

        renderer_.read_radiance(radiance_);
        tone_map_into(image_, radiance_, render_.tone_mapping, true);
        image_.write_ppm(frame_path(options_.export_directory, frames_));
    }

    /// Write the frame on screen to the current directory, for the key that
    /// takes a still without setting up an export.
    void capture() {
        radiance_.resize(renderer_.radiance_size());
        Image still(renderer_.width(), renderer_.height());
        renderer_.read_radiance(radiance_);
        tone_map_into(still, radiance_, render_.tone_mapping, true);

        const std::filesystem::path path = frame_path(".", captures_++);
        still.write_ppm(path);
        *out_ << "wrote " << path.string() << '\n';
    }

    Options options_;
    std::span<const float> tints_;
    std::ostream* out_;

    // The window is declared before the renderer, so the context exists when the
    // renderer's constructor asks for entry points and is still alive when its
    // destructor deletes what it made.
    ViewerWindow window_;
    PointRenderer renderer_;

    RenderSettings render_;
    Index frames_ = 0;
    Index captures_ = 0;
    bool paused_ = false;

    std::vector<float> radiance_;
    Image image_;
};

void report(const Viewer& viewer, double seconds, std::ostream& out) {
    out << "frames:      " << viewer.frames() << " in " << seconds << " s";
    if (viewer.frames() > 0 && seconds > 0) {
        out << ", " << static_cast<double>(viewer.frames()) / seconds << " per second";
    }
    out << '\n';
}

int command_run(std::span<const std::string_view> arguments, std::ostream& out,
                std::ostream& error) {
    if (arguments.empty()) {
        error << "orrery-view run: expected a configuration file\n";
        return kFailure;
    }

    bool ok = true;
    bool distance_given = false;
    Options options = read_options(arguments.subspan(1), error, ok, distance_given);
    if (!ok) {
        return kFailure;
    }

    Configuration configuration = orrery::sim::read_configuration_file(arguments[0]);
    orrery::sim::apply_settings(configuration, options.settings, "--set");

    const std::vector<std::string> problems = orrery::sim::problems_with(configuration);
    for (const std::string& problem : problems) {
        error << "orrery-view: " << problem << '\n';
    }
    if (!problems.empty()) {
        return kFailure;
    }

    orrery::sim::Simulation simulation = orrery::sim::assemble(configuration, nullptr, out);
    const std::vector<float> tints = tints_for(configuration, simulation.particles().size());

    if (!distance_given) {
        frame_particles(options.camera, simulation.particles().positions());
    }

    out << "particles:   " << simulation.particles().size() << '\n'
        << "solver:      " << simulation.solver().name() << '\n';

    Viewer viewer(options, tints, out);
    const auto started = std::chrono::steady_clock::now();

    while (viewer.show(simulation.particles().positions())) {
        if (viewer.paused()) {
            continue;
        }
        if (simulation.step_index() >= configuration.run.steps) {
            break;
        }
        for (Index step = 0; step < options.steps_per_frame; ++step) {
            simulation.step();
        }
    }

    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started);
    report(viewer, elapsed.count(), out);
    out << "steps taken: " << simulation.step_index() << '\n';
    return kSuccess;
}

int command_play(std::span<const std::string_view> arguments, std::ostream& out,
                 std::ostream& error) {
    if (arguments.empty()) {
        error << "orrery-view play: expected a trajectory file\n";
        return kFailure;
    }

    bool ok = true;
    bool distance_given = false;
    Options options = read_options(arguments.subspan(1), error, ok, distance_given);
    if (!ok) {
        return kFailure;
    }
    if (!options.settings.empty()) {
        error << "orrery-view play: --set changes a configuration, and a trajectory has none\n";
        return kFailure;
    }

    orrery::sim::TrajectoryReader reader(arguments[0]);
    orrery::sim::TrajectoryFrame frame;

    out << "particles:   " << reader.info().particle_count << '\n';

    // The camera is framed from the first frame and then left alone, so the
    // system moves through a fixed view rather than the view chasing it.
    if (!reader.read_frame(frame)) {
        error << "orrery-view: " << arguments[0] << " holds no frames\n";
        return kFailure;
    }
    if (!distance_given) {
        frame_particles(options.camera, frame.positions.view());
    }

    // A trajectory does not record which galaxy a particle came from, so there
    // is nothing to tint by. The two ends of the colour ramp are a property of
    // the configuration, and a trajectory is not one.
    Viewer viewer(options, {}, out);
    const auto started = std::chrono::steady_clock::now();

    bool more = true;
    while (more && viewer.show(frame.positions.view())) {
        if (!viewer.paused()) {
            more = reader.read_frame(frame);
        }
    }

    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started);
    report(viewer, elapsed.count(), out);
    return kSuccess;
}

int dispatch(std::span<const std::string_view> arguments, std::ostream& out, std::ostream& error) {
    if (arguments.empty() || arguments[0] == "--help" || arguments[0] == "-h") {
        print_usage(out);
        return kSuccess;
    }
    if (arguments[0] == "--version") {
        out << "orrery-view " << orrery::core::version() << '\n';
        return kSuccess;
    }

    const std::span<const std::string_view> rest = arguments.subspan(1);
    if (arguments[0] == "run") {
        return command_run(rest, out, error);
    }
    if (arguments[0] == "play") {
        return command_play(rest, out, error);
    }

    error << "orrery-view: unknown command '" << arguments[0] << "'\n\n";
    print_usage(error);
    return kFailure;
}

/// Report a failure with C output, for the same reason `orrery.cpp` does: this
/// is called from the handlers in `main`, where a stream inserter that threw
/// would leave by a path no handler covers.
void report_failure(const char* message) noexcept {
    static_cast<void>(std::fputs("orrery-view: ", stderr));
    static_cast<void>(std::fputs(message, stderr));
    static_cast<void>(std::fputs("\n", stderr));
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::span<char*> raw(argv, static_cast<std::size_t>(argc));
        std::vector<std::string_view> arguments;
        arguments.reserve(raw.size());
        for (std::size_t index = 1; index < raw.size(); ++index) {
            arguments.emplace_back(raw[index]);
        }

        return dispatch(arguments, std::cout, std::cerr);
    } catch (const std::exception& error) {
        report_failure(error.what());
        return kFailure;
    } catch (...) {
        report_failure("an unrecognised error stopped the viewer");
        return kFailure;
    }
}
