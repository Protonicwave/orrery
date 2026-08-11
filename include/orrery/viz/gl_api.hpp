#pragma once

/// \file
/// The OpenGL entry points this renderer uses, and only those.
///
/// On every desktop platform the OpenGL library exports the functions of version
/// 1.1 and nothing later. Everything a modern renderer needs, buffers, shaders,
/// vertex arrays, framebuffers, is fetched at run time from the driver by name.
/// Something has to do that fetching.
///
/// The usual something is a generated loader, glad or GLEW, which declares every
/// function in every version and extension of the API and resolves them all.
/// This file does it by hand for the thirty-nine entry points the renderer
/// actually calls. The reason is proportion: a generated loader is several
/// thousand lines of code that nobody in this repository will read, added as a
/// dependency or vendored as a generated artefact, to save writing the list
/// below. ADR-0035 records the choice and what it costs, which is that adding a
/// call to a function not in this list is a two-line edit rather than none.
///
/// The declarations are the API's own, taken from the OpenGL registry. The
/// types and the constants have to match the driver's exactly, because nothing
/// checks them: a wrong constant is a silently wrong render and a wrong
/// signature is undefined behaviour. Each constant is therefore written as the
/// hexadecimal value the registry gives it rather than derived from anything.
///
/// Nothing here is included by anything above the renderer.

#include <cstddef>
#include <cstdint>

namespace orrery::viz {

/// The subset of the OpenGL type system these entry points need.
///
/// Fixed-width where the API fixes the width, which it does for all of these:
/// `GLint` is 32 bits on every platform OpenGL runs on, whatever `int` happens
/// to be, and spelling it as `int` would be right by accident.
using GlEnum = std::uint32_t;
using GlBitfield = std::uint32_t;
using GlUint = std::uint32_t;
using GlInt = std::int32_t;
using GlSizei = std::int32_t;
using GlBoolean = std::uint8_t;
using GlUbyte = std::uint8_t;
using GlFloat = float;
using GlChar = char;
using GlSizeiptr = std::ptrdiff_t;
using GlIntptr = std::ptrdiff_t;

inline constexpr GlEnum kGlFalse = 0;
inline constexpr GlEnum kGlOne = 1;

inline constexpr GlEnum kGlPoints = 0x0000;
inline constexpr GlEnum kGlTriangles = 0x0004;
inline constexpr GlEnum kGlNoError = 0;
inline constexpr GlEnum kGlVendor = 0x1F00;
inline constexpr GlEnum kGlRenderer = 0x1F01;
inline constexpr GlEnum kGlVersion = 0x1F02;
inline constexpr GlEnum kGlFloatType = 0x1406;

inline constexpr GlBitfield kGlColorBufferBit = 0x00004000;
inline constexpr GlEnum kGlDepthTest = 0x0B71;
inline constexpr GlEnum kGlBlend = 0x0BE2;
inline constexpr GlEnum kGlProgramPointSize = 0x8642;
inline constexpr GlEnum kGlFramebufferSrgb = 0x8DB9;

inline constexpr GlEnum kGlArrayBuffer = 0x8892;
inline constexpr GlEnum kGlStreamDraw = 0x88E0;

inline constexpr GlEnum kGlVertexShader = 0x8B31;
inline constexpr GlEnum kGlFragmentShader = 0x8B30;
inline constexpr GlEnum kGlCompileStatus = 0x8B81;
inline constexpr GlEnum kGlLinkStatus = 0x8B82;
inline constexpr GlEnum kGlInfoLogLength = 0x8B84;

inline constexpr GlEnum kGlTexture2d = 0x0DE1;
inline constexpr GlEnum kGlTexture0 = 0x84C0;
inline constexpr GlEnum kGlRgb = 0x1907;
inline constexpr GlEnum kGlRgba = 0x1908;
inline constexpr GlEnum kGlRgba16f = 0x881A;
inline constexpr GlEnum kGlTextureMinFilter = 0x2801;
inline constexpr GlEnum kGlTextureMagFilter = 0x2800;
inline constexpr GlEnum kGlTextureWrapS = 0x2802;
inline constexpr GlEnum kGlTextureWrapT = 0x2803;
inline constexpr GlEnum kGlNearest = 0x2600;
inline constexpr GlEnum kGlClampToEdge = 0x812F;

inline constexpr GlEnum kGlFramebuffer = 0x8D40;
inline constexpr GlEnum kGlColorAttachment0 = 0x8CE0;
inline constexpr GlEnum kGlFramebufferComplete = 0x8CD5;

/// How a function is fetched from the driver.
///
/// Taken as a parameter rather than called directly, because the answer comes
/// from the window system library and this file has no business knowing which
/// one is in use.
using GlProcedureLoader = void (*(*)(const char*))();

/// The entry points, as the pointers the driver returns.
///
/// A structure of members rather than a set of namespace-scope pointers, so that
/// there is no mutable global state and so that a second context, should one ever
/// be wanted, gets its own. The names drop the `gl` prefix and the camel case:
/// `gen_buffers` rather than `glGenBuffers`, since the prefix exists to keep C
/// identifiers apart in one flat namespace and this is neither C nor flat.
struct GlFunctions {
    void (*viewport)(GlInt, GlInt, GlSizei, GlSizei) = nullptr;
    void (*clear_colour)(GlFloat, GlFloat, GlFloat, GlFloat) = nullptr;
    void (*clear)(GlBitfield) = nullptr;
    void (*enable)(GlEnum) = nullptr;
    void (*disable)(GlEnum) = nullptr;
    void (*blend_func)(GlEnum, GlEnum) = nullptr;
    void (*draw_arrays)(GlEnum, GlInt, GlSizei) = nullptr;
    void (*read_pixels)(GlInt, GlInt, GlSizei, GlSizei, GlEnum, GlEnum, void*) = nullptr;
    GlEnum (*get_error)() = nullptr;
    const GlUbyte* (*get_string)(GlEnum) = nullptr;

    void (*gen_vertex_arrays)(GlSizei, GlUint*) = nullptr;
    void (*bind_vertex_array)(GlUint) = nullptr;
    void (*delete_vertex_arrays)(GlSizei, const GlUint*) = nullptr;

    void (*gen_buffers)(GlSizei, GlUint*) = nullptr;
    void (*bind_buffer)(GlEnum, GlUint) = nullptr;
    void (*buffer_data)(GlEnum, GlSizeiptr, const void*, GlEnum) = nullptr;
    void (*buffer_sub_data)(GlEnum, GlIntptr, GlSizeiptr, const void*) = nullptr;
    void (*delete_buffers)(GlSizei, const GlUint*) = nullptr;

    void (*enable_vertex_attrib_array)(GlUint) = nullptr;
    void (*vertex_attrib_pointer)(GlUint, GlInt, GlEnum, GlBoolean, GlSizei, const void*) = nullptr;

    GlUint (*create_shader)(GlEnum) = nullptr;
    void (*shader_source)(GlUint, GlSizei, const GlChar* const*, const GlInt*) = nullptr;
    void (*compile_shader)(GlUint) = nullptr;
    void (*get_shader_iv)(GlUint, GlEnum, GlInt*) = nullptr;
    void (*get_shader_info_log)(GlUint, GlSizei, GlSizei*, GlChar*) = nullptr;
    void (*delete_shader)(GlUint) = nullptr;

    GlUint (*create_program)() = nullptr;
    void (*attach_shader)(GlUint, GlUint) = nullptr;
    void (*link_program)(GlUint) = nullptr;
    void (*get_program_iv)(GlUint, GlEnum, GlInt*) = nullptr;
    void (*get_program_info_log)(GlUint, GlSizei, GlSizei*, GlChar*) = nullptr;
    void (*use_program)(GlUint) = nullptr;
    void (*delete_program)(GlUint) = nullptr;

    GlInt (*get_uniform_location)(GlUint, const GlChar*) = nullptr;
    void (*uniform_matrix4fv)(GlInt, GlSizei, GlBoolean, const GlFloat*) = nullptr;
    void (*uniform1f)(GlInt, GlFloat) = nullptr;
    void (*uniform1i)(GlInt, GlInt) = nullptr;
    void (*uniform3f)(GlInt, GlFloat, GlFloat, GlFloat) = nullptr;

    void (*gen_textures)(GlSizei, GlUint*) = nullptr;
    void (*bind_texture)(GlEnum, GlUint) = nullptr;
    void (*active_texture)(GlEnum) = nullptr;
    void (*tex_image_2d)(GlEnum, GlInt, GlInt, GlSizei, GlSizei, GlInt, GlEnum, GlEnum,
                         const void*) = nullptr;
    void (*tex_parameter_i)(GlEnum, GlEnum, GlInt) = nullptr;
    void (*delete_textures)(GlSizei, const GlUint*) = nullptr;

    void (*gen_framebuffers)(GlSizei, GlUint*) = nullptr;
    void (*bind_framebuffer)(GlEnum, GlUint) = nullptr;
    void (*framebuffer_texture_2d)(GlEnum, GlEnum, GlEnum, GlUint, GlInt) = nullptr;
    GlEnum (*check_framebuffer_status)(GlEnum) = nullptr;
    void (*delete_framebuffers)(GlSizei, const GlUint*) = nullptr;
};

/// Resolve every entry point above, or say which one was missing.
///
/// Throws `std::runtime_error` naming the first function the driver did not
/// provide. That is a setup boundary and the only useful response to it: a
/// renderer that carried on with a null pointer would crash inside the driver
/// with no indication of which call it was.
[[nodiscard]] GlFunctions load_gl_functions(GlProcedureLoader loader);

} // namespace orrery::viz
