#pragma once

/// \file
/// An 8-bit colour image, and the one format this project writes it in.
///
/// The exported frames are PPM: a five-byte magic, the dimensions and the
/// maximum value in ASCII, then three bytes per pixel with no compression and no
/// metadata. It is the plainest raster format that exists.
///
/// That is a deliberate choice against PNG, which would produce files a fifth of
/// the size and would need either a compression library as a dependency or a
/// deflate implementation written here. ADR-0037 records it. The short version
/// is that these files exist to be consumed by an encoder within seconds of
/// being written and then deleted: the demonstration writes a few thousand of
/// them, hands the directory to ffmpeg, and what survives is the video. Paying
/// for a dependency, or for several hundred lines of bit manipulation, to make a
/// temporary file smaller is the wrong trade, and every image viewer and every
/// encoder reads PPM already.
///
/// The one real cost is disc traffic: a thousand frames at 1920 by 1080 is six
/// gigabytes rather than one. `docs/visualisation.md` gives the pipe that avoids
/// writing them at all when only the video is wanted.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "orrery/viz/tone_map.hpp"

namespace orrery::viz {

/// A rectangle of 8-bit RGB pixels, row-major from the top left.
class Image {
public:
    /// The number of bytes one pixel occupies. Three: red, green and blue, with
    /// no alpha, because a frame of a video has nothing to be transparent
    /// against.
    static constexpr std::size_t kChannels = 3;

    Image() = default;

    /// A black image of the given size.
    Image(std::size_t width, std::size_t height);

    [[nodiscard]] std::size_t width() const noexcept { return width_; }

    [[nodiscard]] std::size_t height() const noexcept { return height_; }

    /// The pixel bytes, `width * height * kChannels` of them.
    [[nodiscard]] std::span<const std::uint8_t> pixels() const noexcept { return pixels_; }

    [[nodiscard]] std::span<std::uint8_t> pixels() noexcept { return pixels_; }

    /// Write one pixel, with `y` counted from the top.
    ///
    /// Out-of-range coordinates are ignored rather than checked by assertion.
    /// The only callers are loops over the image's own dimensions, and a silent
    /// no-op is a better failure than a crash in a viewer that is midway through
    /// exporting four thousand frames.
    void set(std::size_t x, std::size_t y, std::uint8_t red, std::uint8_t green,
             std::uint8_t blue) noexcept;

    /// Write the image as binary PPM.
    ///
    /// Throws `std::runtime_error` if the file cannot be opened or the write
    /// fails. An export writes one of these per frame, and a disc that fills
    /// halfway through should stop the export rather than produce a video with a
    /// hole in it.
    void write_ppm(const std::filesystem::path& path) const;

private:
    std::size_t width_ = 0;
    std::size_t height_ = 0;
    std::vector<std::uint8_t> pixels_;
};

/// Fill `image` from a buffer of linear radiance, three floats per pixel.
///
/// `radiance` must hold `width * height * 3` values. Each is exposed, put
/// through the tone curve and encoded for display by `tone_map.hpp`.
///
/// `bottom_up` says the source's first row is the bottom of the picture, which
/// is how OpenGL reports a framebuffer: its origin is the lower left corner,
/// while every raster format in common use starts at the top. Getting this wrong
/// produces an image that is upside down and otherwise perfect, which is
/// mentioned here because it is the failure this parameter exists to prevent.
void tone_map_into(Image& image, std::span<const float> radiance, ToneMapping mapping,
                   bool bottom_up);

} // namespace orrery::viz
