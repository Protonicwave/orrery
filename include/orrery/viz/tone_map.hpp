#pragma once

/// \file
/// Turning accumulated light into pixels.
///
/// A star field rendered with additive blending has no upper bound on
/// brightness. The centre of a galactic bulge may hold a thousand times the
/// light of its outskirts, and a display holds two hundred and fifty-six values
/// per channel. Writing the accumulated value straight to the screen gives a
/// picture that is a solid white disc surrounded by black, which is not a
/// failure of the renderer but of the last step in it.
///
/// So the accumulated radiance is compressed by a curve that is close to linear
/// where the image is dark and flattens where it is bright, and only then
/// encoded for display. That is what a camera's response curve does and why a
/// photograph of a night sky shows both the bright core and the faint arms.
///
/// ## Which curve
///
/// The extended Reinhard operator: `L (1 + L / W^2) / (1 + L)`, where `L` is the
/// exposed radiance and `W` the value chosen to map exactly to white. It is the
/// simplest curve with the two properties this needs. It is linear at the dark
/// end, so faint structure keeps its relative brightness and the outer disc does
/// not turn grey. And it reaches exactly one at `W` rather than approaching it,
/// so there is a stated brightness at which the image saturates rather than an
/// asymptote that never quite gets there and leaves the brightest pixel at 254.
///
/// The filmic curves used in games are better at making an image look
/// photographic and worse here for exactly that reason: they apply a toe that
/// crushes the darkest values, and the faintest parts of a tidal tail are the
/// thing the picture exists to show.
///
/// The curve is applied to each channel rather than to a luminance. That is what
/// makes a bright region desaturate towards white as it saturates, which is how
/// an overexposed light behaves and what makes the core of a galaxy read as
/// bright rather than as intensely coloured.
///
/// ## Two spellings of one curve
///
/// The interactive renderer applies this in a fragment shader and the offline
/// export applies it here, on the host. The reason is in ADR-0036: what is
/// exported has to be reproducible from the accumulated buffer alone, and a
/// fragment shader's arithmetic is the driver's business. So the curve is
/// written twice, once in C++ below and once in the GLSL beside it. They are
/// kept in the same file, adjacent, because that is the only real guard against
/// them drifting apart; the properties both must have are what
/// `tests/viz/tone_map_test.cpp` checks of the one it can reach.

#include <array>
#include <cstdint>
#include <string_view>

namespace orrery::viz {

/// The two numbers that decide how bright the picture is.
struct ToneMapping {
    /// What the accumulated radiance is multiplied by before the curve.
    ///
    /// This is the exposure control, and it is the setting a viewer actually
    /// reaches for. A run at ten thousand particles and one at a million deposit
    /// very different amounts of light per pixel for the same physical
    /// brightness, so there is no default that suits both and the viewer offers
    /// a key for it.
    float exposure = 1;

    /// The exposed radiance that maps exactly to white.
    ///
    /// Above four the curve is nearly a plain Reinhard and the image never quite
    /// saturates; below about two it clips early and the bulge becomes a flat
    /// white disc again. Four is what the demonstration uses.
    float white_point = 4;
};

/// One channel through the curve, clamped to [0, 1].
///
/// The clamp matters above `white_point`: the extended Reinhard curve continues
/// past one rather than saturating at it, and a pixel that came out at 1.4 would
/// wrap to a dark value when it was converted to a byte.
[[nodiscard]] float tone_curve(float radiance, ToneMapping mapping) noexcept;

/// The sRGB transfer function, from a linear value in [0, 1] to a byte.
///
/// Separate from the curve because it is not a choice. Displays are sRGB, the
/// encoding is defined by a standard, and the interactive path does not call
/// this at all: it writes linear values into an sRGB framebuffer and lets the
/// hardware apply exactly this function. Doing it here by hand for the export
/// path is therefore not a second implementation of a decision but the same
/// standard function, applied where no hardware is doing it.
[[nodiscard]] std::uint8_t encode_srgb(float linear) noexcept;

/// A linear radiance triple through the curve and the encoding, ready to store.
[[nodiscard]] std::array<std::uint8_t, 3> to_display(float red, float green, float blue,
                                                     ToneMapping mapping) noexcept;

/// The same curve as GLSL, for the shader that applies it on the device.
///
/// A function `vec3 orrery_tone_curve(vec3, float, float)` and nothing else, so
/// that the shader that includes it can be read beside this file and compared
/// line for line. It does not encode for display: the interactive path renders
/// into an sRGB framebuffer and the hardware does that.
[[nodiscard]] constexpr std::string_view tone_curve_glsl() noexcept {
    return R"(vec3 orrery_tone_curve(vec3 radiance, float exposure, float white_point) {
    vec3 exposed = max(radiance * exposure, vec3(0.0));
    vec3 mapped = exposed * (1.0 + exposed / (white_point * white_point)) / (1.0 + exposed);
    return clamp(mapped, 0.0, 1.0);
}
)";
}

} // namespace orrery::viz
