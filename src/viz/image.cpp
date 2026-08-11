#include "orrery/viz/image.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <stdexcept>
#include <string>

#include "orrery/viz/tone_map.hpp"

namespace orrery::viz {

Image::Image(std::size_t width, std::size_t height)
    : width_(width), height_(height), pixels_(width * height * kChannels, 0) {}

void Image::set(std::size_t x, std::size_t y, std::uint8_t red, std::uint8_t green,
                std::uint8_t blue) noexcept {
    if (x >= width_ || y >= height_) {
        return;
    }

    const std::size_t offset = ((y * width_) + x) * kChannels;
    pixels_[offset] = red;
    pixels_[offset + 1] = green;
    pixels_[offset + 2] = blue;
}

void Image::write_ppm(const std::filesystem::path& path) const {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error{"could not open " + path.string() + " to write a frame"};
    }

    // P6 is the binary form of the format. The ASCII form, P3, writes each
    // channel as decimal digits and produces a file four times the size for the
    // same picture.
    file << "P6\n" << width_ << ' ' << height_ << "\n255\n";

    // The pixels go out in one call rather than a byte at a time, which for a
    // 1920 by 1080 frame is one write of six megabytes instead of six million
    // calls through the stream. The cast is the one the standard permits and
    // requires for this: a stream writes `char`, an object's representation is
    // addressable as `char`, and there is no other spelling that hands a block
    // of bytes to `write`.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    file.write(reinterpret_cast<const char*>(pixels_.data()),
               static_cast<std::streamsize>(pixels_.size()));

    if (!file) {
        throw std::runtime_error{"could not write " + path.string()};
    }
}

void tone_map_into(Image& image, std::span<const float> radiance, ToneMapping mapping,
                   bool bottom_up) {
    const std::size_t width = image.width();
    const std::size_t height = image.height();
    if (radiance.size() < width * height * Image::kChannels) {
        throw std::invalid_argument{"the radiance buffer is smaller than the image it fills"};
    }

    for (std::size_t y = 0; y < height; ++y) {
        const std::size_t source_row = bottom_up ? height - 1 - y : y;
        for (std::size_t x = 0; x < width; ++x) {
            const std::size_t offset = ((source_row * width) + x) * Image::kChannels;
            const std::array<std::uint8_t, 3> pixel =
                to_display(radiance[offset], radiance[offset + 1], radiance[offset + 2], mapping);
            image.set(x, y, pixel[0], pixel[1], pixel[2]);
        }
    }
}

} // namespace orrery::viz
