#include "orrery/viz/tone_map.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

namespace {

using orrery::viz::encode_srgb;
using orrery::viz::to_display;
using orrery::viz::tone_curve;
using orrery::viz::tone_curve_glsl;
using orrery::viz::ToneMapping;

constexpr ToneMapping kMapping{.exposure = 1, .white_point = 4};

} // namespace

TEST_CASE("the tone curve maps black to black and the white point to white", "[unit][viz]") {
    // The two ends are the whole reason for choosing this curve over a plain
    // Reinhard. Black stays black, so an empty region of sky is black rather
    // than a dark grey, and the stated white point reaches exactly one, so there
    // is a brightness at which the image saturates rather than an asymptote it
    // never quite reaches.
    CHECK(tone_curve(0, kMapping) == 0.0F);
    CHECK(std::abs(tone_curve(kMapping.white_point, kMapping) - 1.0F) < 1e-6F);
}

TEST_CASE("the tone curve rises and never leaves the unit interval", "[unit][viz]") {
    float previous = -1;
    for (int step = 0; step <= 1000; ++step) {
        const float radiance = static_cast<float>(step) / 50.0F;
        const float mapped = tone_curve(radiance, kMapping);

        // Rising, because a brighter part of the image has to come out brighter.
        // A curve that turned over would make the very centre of a galaxy darker
        // than its surroundings, which is the kind of thing that looks like a
        // bug in the physics.
        CHECK(mapped >= previous);
        CHECK(mapped >= 0.0F);

        // The extended Reinhard curve continues past one beyond the white point
        // rather than saturating at it, and the clamp is what keeps a value of
        // 1.4 from wrapping to a dark byte.
        CHECK(mapped <= 1.0F);
        previous = mapped;
    }
}

TEST_CASE("the tone curve is nearly linear where the image is faint", "[unit][viz]") {
    // The property that makes this curve right for a star field. Faint structure,
    // a tidal tail against the background, keeps its relative brightness, so what
    // is twice as bright looks twice as bright. A filmic curve with a toe would
    // crush exactly this range.
    const float faint = tone_curve(0.001F, kMapping);
    const float twice = tone_curve(0.002F, kMapping);

    CHECK(twice / faint > 1.99F);
    CHECK(twice / faint < 2.01F);
}

TEST_CASE("exposure scales what counts as bright", "[unit][viz]") {
    const ToneMapping dim{.exposure = 0.5F, .white_point = 4};
    const ToneMapping bright{.exposure = 2, .white_point = 4};

    // The same radiance through a larger exposure comes out brighter, which is
    // the one thing the control has to do. Checked at a value well below the
    // white point, where the curve has not started to compress.
    CHECK(tone_curve(0.2F, dim) < tone_curve(0.2F, kMapping));
    CHECK(tone_curve(0.2F, kMapping) < tone_curve(0.2F, bright));

    // Negative radiance cannot arise from an additive pass, but a buffer read
    // back from a device is whatever it is, and a negative value taken through
    // the curve would come out positive without the clamp inside it.
    CHECK(tone_curve(-1, kMapping) == 0.0F);
}

TEST_CASE("the sRGB encoding covers the whole range of a byte", "[unit][viz]") {
    CHECK(encode_srgb(0) == 0);
    CHECK(encode_srgb(1) == 255);

    // Rounded rather than truncated: a linear value of one has to encode to 255
    // and not to 254, or a saturated pixel is not white.
    CHECK(encode_srgb(2) == 255);
    CHECK(encode_srgb(-1) == 0);

    // Mid grey. The sRGB transfer function is deliberately not a plain power
    // law, and a linear value of a half encodes to 188 rather than to the 128 a
    // linear encoding would give. Getting this wrong makes every rendered frame
    // too dark, and it is the sort of error that is only visible beside a
    // correct image.
    CHECK(encode_srgb(0.5F) == 188);
}

TEST_CASE("a bright pixel desaturates towards white", "[unit][viz]") {
    // The curve is applied to each channel, which is what makes an overexposed
    // region turn white rather than turn into an intense version of its own
    // colour. It is how a camera behaves and it is what makes the core of a
    // galaxy read as bright.
    const std::array<std::uint8_t, 3> dim = to_display(0.02F, 0.01F, 0.005F, kMapping);
    const std::array<std::uint8_t, 3> blazing = to_display(40, 20, 10, kMapping);

    CHECK(dim[0] > dim[1]);
    CHECK(dim[1] > dim[2]);

    CHECK(blazing[0] == 255);
    CHECK(blazing[1] == 255);
    CHECK(blazing[2] == 255);
}

TEST_CASE("the shader copy of the curve is a function of the expected name", "[unit][viz]") {
    // The interactive path applies the curve in GLSL and the export path applies
    // it here, for the reason ADR-0036 gives. The two spellings sit in one file
    // so that they can be read together, and this checks the only part of the
    // agreement a test can reach: that the shader source defines the function
    // the renderer's fragment shader calls, and that it names the same two
    // parameters this header does.
    const std::string_view glsl = tone_curve_glsl();

    CHECK(glsl.find("vec3 orrery_tone_curve(vec3 radiance, float exposure, float white_point)") !=
          std::string_view::npos);
    CHECK(glsl.find("clamp") != std::string_view::npos);
}
