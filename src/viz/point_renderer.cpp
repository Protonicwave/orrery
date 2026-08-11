#include "orrery/viz/point_renderer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "orrery/core/types.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/viz/gl_api.hpp"
#include "orrery/viz/matrix4.hpp"
#include "orrery/viz/tone_map.hpp"

namespace orrery::viz {

namespace {

using core::Real;

/// The vertex shader for the particles.
///
/// The three position components arrive as three separate attributes, which is
/// what the simulation's component arrays already are. `gl_PointSize` falls with
/// the distance from the camera, which is the `w` the projection wrote, so a
/// sprite behaves like an object of fixed size rather than a mark of fixed size
/// on the screen.
constexpr std::string_view kPointVertexShader = R"(#version 330 core
layout(location = 0) in float position_x;
layout(location = 1) in float position_y;
layout(location = 2) in float position_z;
layout(location = 3) in float tint;

uniform mat4 view_projection;
uniform float point_size;
uniform float minimum_point_size;
uniform vec3 cool_colour;
uniform vec3 warm_colour;

out vec3 particle_colour;

void main() {
    vec4 clip = view_projection * vec4(position_x, position_y, position_z, 1.0);
    gl_Position = clip;

    // The guard on w keeps a particle that is level with or behind the eye from
    // producing a negative or enormous size. It is clipped away regardless, but
    // an invalid point size is undefined rather than merely invisible.
    gl_PointSize = max(point_size / max(clip.w, 0.001), minimum_point_size);

    particle_colour = mix(cool_colour, warm_colour, clamp(tint, 0.0, 1.0));
}
)";

/// The fragment shader for the particles.
///
/// A radial falloff rather than a flat disc, so that overlapping sprites sum
/// into something smooth instead of into a mosaic of hard-edged circles. The
/// profile is a Gaussian with its value at the rim subtracted, which brings the
/// edge to exactly zero: without that subtraction every sprite ends in a step of
/// about two per cent of its peak, and a hundred thousand such steps are clearly
/// visible as texture in the faint parts of the image.
constexpr std::string_view kPointFragmentShader = R"(#version 330 core
in vec3 particle_colour;

uniform float brightness;

out vec4 fragment;

void main() {
    vec2 offset = (gl_PointCoord * 2.0) - 1.0;
    float radius_squared = dot(offset, offset);
    if (radius_squared > 1.0) {
        discard;
    }

    const float sharpness = 4.0;
    float falloff = exp(-sharpness * radius_squared) - exp(-sharpness);

    fragment = vec4(particle_colour * brightness * falloff, 1.0);
}
)";

/// The vertex shader of the tone mapping pass.
///
/// Draws one triangle that covers the screen, built from the vertex index rather
/// than from a buffer, so the pass needs no vertex data at all. One oversized
/// triangle rather than two triangles making a quad: the diagonal seam of a quad
/// is a place where the rasteriser evaluates some pixels twice.
constexpr std::string_view kPostVertexShader = R"(#version 330 core
out vec2 texture_coordinate;

void main() {
    texture_coordinate = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4((texture_coordinate * 2.0) - 1.0, 0.0, 1.0);
}
)";

/// The fragment shader of the tone mapping pass, with the curve prepended.
///
/// The curve itself comes from `tone_map.hpp`, which is also where the host-side
/// copy of it lives, so the two can be read together.
[[nodiscard]] std::string post_fragment_shader() {
    return std::string{R"(#version 330 core
in vec2 texture_coordinate;

uniform sampler2D scene;
uniform float exposure;
uniform float white_point;

out vec4 fragment;

)"} + std::string{tone_curve_glsl()} +
           R"(
void main() {
    vec3 radiance = texture(scene, texture_coordinate).rgb;
    fragment = vec4(orrery_tone_curve(radiance, exposure, white_point), 1.0);
}
)";
}

[[nodiscard]] std::string shader_log(const GlFunctions& gl, GlUint shader) {
    GlInt length = 0;
    gl.get_shader_iv(shader, kGlInfoLogLength, &length);
    if (length <= 0) {
        return "the driver gave no reason";
    }

    std::string log(static_cast<std::size_t>(length), '\0');
    gl.get_shader_info_log(shader, length, nullptr, log.data());
    return log;
}

[[nodiscard]] std::string program_log(const GlFunctions& gl, GlUint program) {
    GlInt length = 0;
    gl.get_program_iv(program, kGlInfoLogLength, &length);
    if (length <= 0) {
        return "the driver gave no reason";
    }

    std::string log(static_cast<std::size_t>(length), '\0');
    gl.get_program_info_log(program, length, nullptr, log.data());
    return log;
}

[[nodiscard]] GlUint compile(const GlFunctions& gl, GlEnum stage, const std::string& source) {
    const GlUint shader = gl.create_shader(stage);
    const GlChar* const text = source.c_str();
    gl.shader_source(shader, 1, &text, nullptr);
    gl.compile_shader(shader);

    GlInt compiled = 0;
    gl.get_shader_iv(shader, kGlCompileStatus, &compiled);
    if (compiled == 0) {
        const std::string log = shader_log(gl, shader);
        gl.delete_shader(shader);
        throw std::runtime_error{"a shader did not compile: " + log};
    }
    return shader;
}

[[nodiscard]] GlUint link(const GlFunctions& gl, const std::string& vertex,
                          const std::string& fragment) {
    const GlUint vertex_shader = compile(gl, kGlVertexShader, vertex);
    const GlUint fragment_shader = compile(gl, kGlFragmentShader, fragment);

    const GlUint program = gl.create_program();
    gl.attach_shader(program, vertex_shader);
    gl.attach_shader(program, fragment_shader);
    gl.link_program(program);

    // Deleted immediately: a shader attached to a program is kept alive by it,
    // and marking them for deletion here means the program owns them for the
    // rest of its life and no separate bookkeeping is needed.
    gl.delete_shader(vertex_shader);
    gl.delete_shader(fragment_shader);

    GlInt linked = 0;
    gl.get_program_iv(program, kGlLinkStatus, &linked);
    if (linked == 0) {
        const std::string log = program_log(gl, program);
        gl.delete_program(program);
        throw std::runtime_error{"a shader program did not link: " + log};
    }
    return program;
}

} // namespace

/// Everything the renderer owns.
///
/// Behind a pointer so that `point_renderer.hpp` does not have to declare the
/// driver's object names, and so that a translation unit that merely mentions
/// the renderer does not acquire the OpenGL constants.
struct PointRenderer::State {
    GlFunctions gl;

    std::size_t width = 0;
    std::size_t height = 0;

    GlUint particle_array = 0;
    std::array<GlUint, 3> position_buffers{};
    GlUint tint_buffer = 0;

    /// The particles the buffers currently have room for.
    std::size_t capacity = 0;

    /// One component of the positions, converted to the precision the device
    /// works in. Reused between frames and between components, so a long run
    /// allocates once.
    std::vector<float> staging;

    GlUint point_program = 0;
    GlInt view_projection_location = -1;
    GlInt point_size_location = -1;
    GlInt minimum_point_size_location = -1;
    GlInt brightness_location = -1;
    GlInt cool_location = -1;
    GlInt warm_location = -1;

    GlUint post_program = 0;
    GlUint post_array = 0;
    GlInt scene_location = -1;
    GlInt exposure_location = -1;
    GlInt white_point_location = -1;

    GlUint framebuffer = 0;
    GlUint colour_texture = 0;

    /// Allocate the accumulation target at the current size.
    void allocate_target() {
        gl.bind_texture(kGlTexture2d, colour_texture);
        gl.tex_image_2d(kGlTexture2d, 0, static_cast<GlInt>(kGlRgba16f),
                        static_cast<GlSizei>(width), static_cast<GlSizei>(height), 0, kGlRgba,
                        kGlFloatType, nullptr);

        // Nearest filtering and clamped edges. The tone mapping pass samples the
        // target at exactly one texel per pixel, so there is nothing for a
        // linear filter to interpolate between and it would only cost a little
        // sharpness at the edges of sprites.
        gl.tex_parameter_i(kGlTexture2d, kGlTextureMinFilter, static_cast<GlInt>(kGlNearest));
        gl.tex_parameter_i(kGlTexture2d, kGlTextureMagFilter, static_cast<GlInt>(kGlNearest));
        gl.tex_parameter_i(kGlTexture2d, kGlTextureWrapS, static_cast<GlInt>(kGlClampToEdge));
        gl.tex_parameter_i(kGlTexture2d, kGlTextureWrapT, static_cast<GlInt>(kGlClampToEdge));

        gl.bind_framebuffer(kGlFramebuffer, framebuffer);
        gl.framebuffer_texture_2d(kGlFramebuffer, kGlColorAttachment0, kGlTexture2d, colour_texture,
                                  0);

        const GlEnum status = gl.check_framebuffer_status(kGlFramebuffer);
        gl.bind_framebuffer(kGlFramebuffer, 0);

        if (status != kGlFramebufferComplete) {
            throw std::runtime_error{
                "this driver would not give a floating-point render target, which the "
                "accumulation pass needs (framebuffer status " +
                std::to_string(status) + ")"};
        }
    }

    /// Make room for `count` particles in each of the four attribute buffers.
    ///
    /// Grows and never shrinks. A viewer that plays a trajectory back sees the
    /// same particle count on every frame, and one that follows a live run sees
    /// a count that does not change at all, so the only reallocation that ever
    /// happens is the first.
    void reserve(std::size_t count) {
        if (count <= capacity) {
            return;
        }

        const auto bytes = static_cast<GlSizeiptr>(count * sizeof(float));
        for (const GlUint buffer : position_buffers) {
            gl.bind_buffer(kGlArrayBuffer, buffer);
            gl.buffer_data(kGlArrayBuffer, bytes, nullptr, kGlStreamDraw);
        }
        gl.bind_buffer(kGlArrayBuffer, tint_buffer);
        gl.buffer_data(kGlArrayBuffer, bytes, nullptr, kGlStreamDraw);

        capacity = count;
        staging.resize(count);
    }

    /// Copy one component into its buffer, converting to the device's precision.
    void upload(GlUint buffer, std::span<const Real> component) {
        for (std::size_t index = 0; index < component.size(); ++index) {
            staging[index] = static_cast<float>(component[index]);
        }

        gl.bind_buffer(kGlArrayBuffer, buffer);
        gl.buffer_sub_data(kGlArrayBuffer, 0,
                           static_cast<GlSizeiptr>(component.size() * sizeof(float)),
                           staging.data());
    }
};

PointRenderer::PointRenderer(GlProcedureLoader loader, std::size_t width, std::size_t height)
    : state_(std::make_unique<State>()) {
    State& state = *state_;
    state.gl = load_gl_functions(loader);
    state.width = width;
    state.height = height;

    const GlFunctions& gl = state.gl;

    gl.gen_vertex_arrays(1, &state.particle_array);
    gl.gen_vertex_arrays(1, &state.post_array);
    gl.gen_buffers(3, state.position_buffers.data());
    gl.gen_buffers(1, &state.tint_buffer);
    gl.gen_textures(1, &state.colour_texture);
    gl.gen_framebuffers(1, &state.framebuffer);

    // The attribute layout is recorded once, into the vertex array object. Each
    // attribute is one float from a buffer of its own, which is what a component
    // array is, so the stride is the size of the element and there is no offset.
    gl.bind_vertex_array(state.particle_array);
    for (GlUint index = 0; index < 3; ++index) {
        gl.bind_buffer(kGlArrayBuffer, state.position_buffers[index]);
        gl.enable_vertex_attrib_array(index);
        gl.vertex_attrib_pointer(index, 1, kGlFloatType, kGlFalse, sizeof(float), nullptr);
    }
    gl.bind_buffer(kGlArrayBuffer, state.tint_buffer);
    gl.enable_vertex_attrib_array(3);
    gl.vertex_attrib_pointer(3, 1, kGlFloatType, kGlFalse, sizeof(float), nullptr);
    gl.bind_vertex_array(0);

    state.point_program =
        link(gl, std::string{kPointVertexShader}, std::string{kPointFragmentShader});
    state.view_projection_location =
        gl.get_uniform_location(state.point_program, "view_projection");
    state.point_size_location = gl.get_uniform_location(state.point_program, "point_size");
    state.minimum_point_size_location =
        gl.get_uniform_location(state.point_program, "minimum_point_size");
    state.brightness_location = gl.get_uniform_location(state.point_program, "brightness");
    state.cool_location = gl.get_uniform_location(state.point_program, "cool_colour");
    state.warm_location = gl.get_uniform_location(state.point_program, "warm_colour");

    state.post_program = link(gl, std::string{kPostVertexShader}, post_fragment_shader());
    state.scene_location = gl.get_uniform_location(state.post_program, "scene");
    state.exposure_location = gl.get_uniform_location(state.post_program, "exposure");
    state.white_point_location = gl.get_uniform_location(state.post_program, "white_point");

    state.allocate_target();

    const GlEnum error = gl.get_error();
    if (error != kGlNoError) {
        throw std::runtime_error{"OpenGL reported error " + std::to_string(error) +
                                 " while the renderer was being built"};
    }
}

PointRenderer::~PointRenderer() {
    const GlFunctions& gl = state_->gl;

    // Order does not matter to the driver, which reference-counts what is still
    // bound, but the framebuffer goes first so that nothing is deleted while it
    // is attached to something still alive.
    gl.delete_framebuffers(1, &state_->framebuffer);
    gl.delete_textures(1, &state_->colour_texture);
    gl.delete_program(state_->point_program);
    gl.delete_program(state_->post_program);
    gl.delete_buffers(3, state_->position_buffers.data());
    gl.delete_buffers(1, &state_->tint_buffer);
    gl.delete_vertex_arrays(1, &state_->particle_array);
    gl.delete_vertex_arrays(1, &state_->post_array);
}

void PointRenderer::resize(std::size_t width, std::size_t height) {
    if (width == state_->width && height == state_->height) {
        return;
    }
    if (width == 0 || height == 0) {
        // A minimised window reports a size of zero. Keeping the old target is
        // better than allocating an empty one, since the window will come back.
        return;
    }

    state_->width = width;
    state_->height = height;
    state_->allocate_target();
}

std::size_t PointRenderer::width() const noexcept {
    return state_->width;
}

std::size_t PointRenderer::height() const noexcept {
    return state_->height;
}

std::size_t PointRenderer::radiance_size() const noexcept {
    return state_->width * state_->height * 3;
}

void PointRenderer::accumulate(core::Vec3Span<const Real> positions, std::span<const float> tints,
                               const Mat4& view_projection, const RenderSettings& settings) {
    State& state = *state_;
    const GlFunctions& gl = state.gl;
    const std::size_t count = positions.size();

    if (!tints.empty() && tints.size() != count) {
        throw std::invalid_argument{"a tint per particle, or none at all"};
    }

    state.reserve(count);

    gl.bind_framebuffer(kGlFramebuffer, state.framebuffer);
    gl.viewport(0, 0, static_cast<GlSizei>(state.width), static_cast<GlSizei>(state.height));
    gl.clear_colour(0, 0, 0, 1);
    gl.clear(kGlColorBufferBit);

    // No depth test, because the picture is a sum along the line of sight rather
    // than a question of what is in front. One and one, because that sum is what
    // additive blending is.
    gl.disable(kGlDepthTest);
    gl.enable(kGlBlend);
    gl.blend_func(kGlOne, kGlOne);
    gl.enable(kGlProgramPointSize);

    if (count > 0) {
        state.upload(state.position_buffers[0], positions.x);
        state.upload(state.position_buffers[1], positions.y);
        state.upload(state.position_buffers[2], positions.z);

        gl.bind_buffer(kGlArrayBuffer, state.tint_buffer);
        if (tints.empty()) {
            // Every particle at the cool end of the ramp. Written rather than
            // left as whatever the buffer held, since a buffer that was filled
            // by a previous configuration would tint this one by its history.
            std::fill(state.staging.begin(),
                      state.staging.begin() + static_cast<std::ptrdiff_t>(count), 0.0F);
            gl.buffer_sub_data(kGlArrayBuffer, 0, static_cast<GlSizeiptr>(count * sizeof(float)),
                               state.staging.data());
        } else {
            gl.buffer_sub_data(kGlArrayBuffer, 0, static_cast<GlSizeiptr>(count * sizeof(float)),
                               tints.data());
        }

        gl.use_program(state.point_program);
        gl.uniform_matrix4fv(state.view_projection_location, 1, kGlFalse,
                             view_projection.values.data());
        gl.uniform1f(state.point_size_location, settings.point_size);
        gl.uniform1f(state.minimum_point_size_location, settings.minimum_point_size);
        gl.uniform1f(state.brightness_location, settings.brightness);
        gl.uniform3f(state.cool_location, settings.cool.red, settings.cool.green,
                     settings.cool.blue);
        gl.uniform3f(state.warm_location, settings.warm.red, settings.warm.green,
                     settings.warm.blue);

        gl.bind_vertex_array(state.particle_array);
        gl.draw_arrays(kGlPoints, 0, static_cast<GlSizei>(count));
        gl.bind_vertex_array(0);
    }

    gl.bind_framebuffer(kGlFramebuffer, 0);
}

void PointRenderer::present(const RenderSettings& settings) {
    const State& state = *state_;
    const GlFunctions& gl = state.gl;

    gl.bind_framebuffer(kGlFramebuffer, 0);
    gl.viewport(0, 0, static_cast<GlSizei>(state.width), static_cast<GlSizei>(state.height));

    // The pass writes a linear value and the hardware encodes it for the display,
    // which is the same sRGB transfer function `encode_srgb` applies on the host
    // for an exported frame. Blending is off: this pass replaces the whole
    // framebuffer rather than adding to it.
    gl.enable(kGlFramebufferSrgb);
    gl.disable(kGlBlend);

    gl.use_program(state.post_program);
    gl.active_texture(kGlTexture0);
    gl.bind_texture(kGlTexture2d, state.colour_texture);
    gl.uniform1i(state.scene_location, 0);
    gl.uniform1f(state.exposure_location, settings.tone_mapping.exposure);
    gl.uniform1f(state.white_point_location, settings.tone_mapping.white_point);

    gl.bind_vertex_array(state.post_array);
    gl.draw_arrays(kGlTriangles, 0, 3);
    gl.bind_vertex_array(0);

    gl.disable(kGlFramebufferSrgb);
}

void PointRenderer::read_radiance(std::span<float> radiance) const {
    if (radiance.size() < radiance_size()) {
        throw std::invalid_argument{"the buffer is smaller than the frame it is to receive"};
    }

    const GlFunctions& gl = state_->gl;
    gl.bind_framebuffer(kGlFramebuffer, state_->framebuffer);
    gl.read_pixels(0, 0, static_cast<GlSizei>(state_->width), static_cast<GlSizei>(state_->height),
                   kGlRgb, kGlFloatType, radiance.data());
    gl.bind_framebuffer(kGlFramebuffer, 0);
}

std::string PointRenderer::device_description() const {
    const GlFunctions& gl = state_->gl;

    const auto text = [&gl](GlEnum name) {
        const GlUbyte* const value = gl.get_string(name);
        if (value == nullptr) {
            return std::string{"unknown"};
        }

        // The driver returns a C string of unsigned bytes. Copied a character at
        // a time rather than cast, which keeps the one place in this file that
        // would need a reinterpret_cast down to none.
        std::string result;
        for (const GlUbyte* character = value; *character != 0; ++character) {
            result.push_back(static_cast<char>(*character));
        }
        return result;
    };

    return text(kGlRenderer) + ", " + text(kGlVendor) + ", OpenGL " + text(kGlVersion);
}

} // namespace orrery::viz
