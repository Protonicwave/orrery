#include "orrery/viz/gl_api.hpp"

#include <stdexcept>
#include <string>

namespace orrery::viz {

namespace {

/// Fetch one entry point and refuse to continue without it.
///
/// The cast is between two function pointer types, which is what every OpenGL
/// loader in existence does and what the window system's own documentation
/// instructs: the loader returns one generic function pointer type and the
/// caller knows the signature. There is no other way to express it, and the
/// project's rule about reinterpret_cast is suspended for this one line rather
/// than for the file.
template<typename Function>
void resolve(Function& target, GlProcedureLoader loader, const char* name) {
    void (*const procedure)() = loader(name);
    if (procedure == nullptr) {
        throw std::runtime_error{std::string{"this OpenGL driver does not provide "} + name};
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    target = reinterpret_cast<Function>(procedure);
}

} // namespace

GlFunctions load_gl_functions(GlProcedureLoader loader) {
    GlFunctions gl;

    // In the order they are declared, which is the order they are used in: the
    // state that applies to the whole frame, then the objects a draw needs, then
    // the shaders, then the render target. A function added to the structure and
    // forgotten here is a null pointer that would crash inside the driver, so the
    // two lists are kept in step by being written in the same order and read
    // together.
    resolve(gl.viewport, loader, "glViewport");
    resolve(gl.clear_colour, loader, "glClearColor");
    resolve(gl.clear, loader, "glClear");
    resolve(gl.enable, loader, "glEnable");
    resolve(gl.disable, loader, "glDisable");
    resolve(gl.blend_func, loader, "glBlendFunc");
    resolve(gl.draw_arrays, loader, "glDrawArrays");
    resolve(gl.read_pixels, loader, "glReadPixels");
    resolve(gl.get_error, loader, "glGetError");
    resolve(gl.get_string, loader, "glGetString");

    resolve(gl.gen_vertex_arrays, loader, "glGenVertexArrays");
    resolve(gl.bind_vertex_array, loader, "glBindVertexArray");
    resolve(gl.delete_vertex_arrays, loader, "glDeleteVertexArrays");

    resolve(gl.gen_buffers, loader, "glGenBuffers");
    resolve(gl.bind_buffer, loader, "glBindBuffer");
    resolve(gl.buffer_data, loader, "glBufferData");
    resolve(gl.buffer_sub_data, loader, "glBufferSubData");
    resolve(gl.delete_buffers, loader, "glDeleteBuffers");

    resolve(gl.enable_vertex_attrib_array, loader, "glEnableVertexAttribArray");
    resolve(gl.vertex_attrib_pointer, loader, "glVertexAttribPointer");

    resolve(gl.create_shader, loader, "glCreateShader");
    resolve(gl.shader_source, loader, "glShaderSource");
    resolve(gl.compile_shader, loader, "glCompileShader");
    resolve(gl.get_shader_iv, loader, "glGetShaderiv");
    resolve(gl.get_shader_info_log, loader, "glGetShaderInfoLog");
    resolve(gl.delete_shader, loader, "glDeleteShader");

    resolve(gl.create_program, loader, "glCreateProgram");
    resolve(gl.attach_shader, loader, "glAttachShader");
    resolve(gl.link_program, loader, "glLinkProgram");
    resolve(gl.get_program_iv, loader, "glGetProgramiv");
    resolve(gl.get_program_info_log, loader, "glGetProgramInfoLog");
    resolve(gl.use_program, loader, "glUseProgram");
    resolve(gl.delete_program, loader, "glDeleteProgram");

    resolve(gl.get_uniform_location, loader, "glGetUniformLocation");
    resolve(gl.uniform_matrix4fv, loader, "glUniformMatrix4fv");
    resolve(gl.uniform1f, loader, "glUniform1f");
    resolve(gl.uniform1i, loader, "glUniform1i");
    resolve(gl.uniform3f, loader, "glUniform3f");

    resolve(gl.gen_textures, loader, "glGenTextures");
    resolve(gl.bind_texture, loader, "glBindTexture");
    resolve(gl.active_texture, loader, "glActiveTexture");
    resolve(gl.tex_image_2d, loader, "glTexImage2D");
    resolve(gl.tex_parameter_i, loader, "glTexParameteri");
    resolve(gl.delete_textures, loader, "glDeleteTextures");

    resolve(gl.gen_framebuffers, loader, "glGenFramebuffers");
    resolve(gl.bind_framebuffer, loader, "glBindFramebuffer");
    resolve(gl.framebuffer_texture_2d, loader, "glFramebufferTexture2D");
    resolve(gl.check_framebuffer_status, loader, "glCheckFramebufferStatus");
    resolve(gl.delete_framebuffers, loader, "glDeleteFramebuffers");

    return gl;
}

} // namespace orrery::viz
