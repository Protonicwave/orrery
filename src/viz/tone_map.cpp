#include "orrery/viz/tone_map.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace orrery::viz {

namespace {

/// The break between the linear and the exponential parts of the sRGB transfer
/// function, and the constants either side of it.
///
/// From IEC 61966-2-1. They are named rather than written into the expression
/// because a reader has to be able to check them against the standard, and
/// because 1.055 and 0.0031308 look like arbitrary tuning otherwise.
constexpr float kSrgbLinearThreshold = 0.0031308F;
constexpr float kSrgbLinearSlope = 12.92F;
constexpr float kSrgbScale = 1.055F;
constexpr float kSrgbOffset = 0.055F;
constexpr float kSrgbExponent = 1.0F / 2.4F;

} // namespace

float tone_curve(float radiance, ToneMapping mapping) noexcept {
    const float exposed = std::max(radiance * mapping.exposure, 0.0F);
    const float white = mapping.white_point * mapping.white_point;
    const float mapped = exposed * (1.0F + (exposed / white)) / (1.0F + exposed);

    return std::clamp(mapped, 0.0F, 1.0F);
}

std::uint8_t encode_srgb(float linear) noexcept {
    const float clamped = std::clamp(linear, 0.0F, 1.0F);
    const float encoded = clamped <= kSrgbLinearThreshold
                              ? clamped * kSrgbLinearSlope
                              : (kSrgbScale * std::pow(clamped, kSrgbExponent)) - kSrgbOffset;

    // Rounded rather than truncated. Truncating would make the whole scale one
    // half of a level dark and, more visibly, would make a value of exactly one
    // encode to 254 rather than to 255, so a saturated pixel would not be white.
    return static_cast<std::uint8_t>(std::lround(encoded * 255.0F));
}

std::array<std::uint8_t, 3> to_display(float red, float green, float blue,
                                       ToneMapping mapping) noexcept {
    return {encode_srgb(tone_curve(red, mapping)), encode_srgb(tone_curve(green, mapping)),
            encode_srgb(tone_curve(blue, mapping))};
}

} // namespace orrery::viz
